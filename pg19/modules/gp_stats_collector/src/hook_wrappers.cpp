/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * hook_wrappers.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/hook_wrappers.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - Cloudberry's core hook query_info_collect_hook is the rendezvous of
 *     gp_query_info.h, which gp_resource calls as a query waits in its
 *     queue; the rest of what Cloudberry's core told it the executor's hooks
 *     tell: a query done after ExecutorEnd(), where standard_ExecutorEnd()
 *     told it, and one failed or cancelled from the executor's hooks'
 *     PG_CATCH, where mppExecutorCleanup() told it;
 *   - what fails outside them ends at the abort of its transaction or
 *     subtransaction (the xact callbacks), with the error the log saw
 *     (emit_log_hook), where Cloudberry's PortalCleanup() told it;
 *   - a node's events (METRICS_PLAN_NODE_*), which Cloudberry's collector
 *     passes over, are not made: PostgreSQL 19 calls no hook as a node runs;
 *   - ExecutorRun() takes no execute_once;
 *   - EXPLAIN ANALYZE's text is made as the query ends (ExecutorEnd()),
 *     where Cloudberry's analyze_stats_collect_hook was to make it; that
 *     hook and ic_teardown_hook, which Cloudberry's core never defines, are
 *     gone;
 *   - on a segment, the statements that carry the coordinator's -- the
 *     transaction's and the settings' commands, a gather's cursor -- are
 *     neither reported nor counted as nesting (gpsc_segment_transport());
 *   - a query's key is forgotten as the query ends;
 *   - GpIdentity.segindex is gp_core's content id, and there is no
 *     hooks_deinit(): PostgreSQL 19 calls no _PG_fini();
 *   - a tuple descriptor made column by column is finalized, as PostgreSQL
 *     19 asks before it is blessed.
 *-------------------------------------------------------------------------
 */

#define typeid __typeid
extern "C" {
#include "postgres.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "stat_statements_parser/pg_stat_statements_parser.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/elog.h"

#include "gp_query_info.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
}
#undef typeid

#include "Config.h"
#include "EventSender.h"
#include "GpscStat.h"
#include "PgUtils.h"
#include "hook_wrappers.h"
#include "memory/gpdbwrappers.h"

static ExecutorStart_hook_type previous_ExecutorStart_hook = nullptr;
static ExecutorRun_hook_type previous_ExecutorRun_hook = nullptr;
static ExecutorFinish_hook_type previous_ExecutorFinish_hook = nullptr;
static ExecutorEnd_hook_type previous_ExecutorEnd_hook = nullptr;
static query_info_collect_hook_type previous_query_info_collect_hook = nullptr;
static emit_log_hook_type previous_emit_log_hook = nullptr;
static ProcessUtility_hook_type previous_ProcessUtility_hook = nullptr;

static void gpsc_ExecutorStart_hook(QueryDesc *query_desc, int eflags);
static void gpsc_ExecutorRun_hook(QueryDesc *query_desc,
								  ScanDirection direction, uint64 count);
static void gpsc_ExecutorFinish_hook(QueryDesc *query_desc);
static void gpsc_ExecutorEnd_hook(QueryDesc *query_desc);
static void gpsc_query_info_collect_hook(QueryMetricsStatus status, void *arg);
static void gpsc_emit_log_hook(ErrorData *edata);
static void gpsc_xact_callback(XactEvent event, void *arg);
static void gpsc_subxact_callback(SubXactEvent event,
								  SubTransactionId mySubid,
								  SubTransactionId parentSubid, void *arg);
static void report_executor_error(QueryDesc *query_desc);
static void gpsc_process_utility_hook(
	PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
	ProcessUtilityContext context, ParamListInfo params,
	QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc);

#define TEST_MAX_CONNECTIONS 4
#define TEST_RCV_BUF_SIZE 8192
#define TEST_POLL_TIMEOUT_MS 200

static int test_server_fd = -1;
static char *test_sock_path = NULL;

static EventSender *sender = nullptr;

static inline EventSender *
get_sender()
{
	if (!sender)
	{
		sender = new EventSender();
	}
	return sender;
}

template <typename T, typename R, typename... Args>
R
cpp_call(T *obj, R (T::*func)(Args...), Args... args)
{
	try
	{
		return (obj->*func)(args...);
	}
	catch (const std::exception &e)
	{
		ereport(ERROR, (errmsg("Unexpected exception in gpsc %s", e.what())));
		pg_unreachable();
	}
}

void
hooks_init()
{
	Config::init_gucs();
	GpscStat::init();
	previous_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = gpsc_ExecutorStart_hook;
	previous_ExecutorRun_hook = ExecutorRun_hook;
	ExecutorRun_hook = gpsc_ExecutorRun_hook;
	previous_ExecutorFinish_hook = ExecutorFinish_hook;
	ExecutorFinish_hook = gpsc_ExecutorFinish_hook;
	previous_ExecutorEnd_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = gpsc_ExecutorEnd_hook;
	previous_query_info_collect_hook = *gp_query_info_collect_hook();
	*gp_query_info_collect_hook() = gpsc_query_info_collect_hook;
	previous_emit_log_hook = emit_log_hook;
	emit_log_hook = gpsc_emit_log_hook;
	RegisterXactCallback(gpsc_xact_callback, NULL);
	RegisterSubXactCallback(gpsc_subxact_callback, NULL);
	stat_statements_parser_init();
	previous_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = gpsc_process_utility_hook;
}

/*
 * A query's error or cancel, as it leaves one of the executor's calls: the
 * error copied from the handler it is in, as Cloudberry's
 * mppExecutorCleanup() tells its hook of it.
 */
static void
report_executor_error(QueryDesc *query_desc)
{
	MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);
	ErrorData *edata = CopyErrorData();

	MemoryContextSwitchTo(oldctx);
	cpp_call(get_sender(), &EventSender::query_metrics_collect,
			 edata->sqlerrcode == ERRCODE_QUERY_CANCELED ? METRICS_QUERY_CANCELED
														 : METRICS_QUERY_ERROR,
			 (void *) query_desc, false /* utility */, edata);
	FreeErrorData(edata);
}

void
gpsc_ExecutorStart_hook(QueryDesc *query_desc, int eflags)
{
	cpp_call(get_sender(), &EventSender::executor_before_start, query_desc,
			 eflags);
	gpsc_query_enter(query_desc);
	PG_TRY();
	{
		if (previous_ExecutorStart_hook)
		{
			(*previous_ExecutorStart_hook)(query_desc, eflags);
		}
		else
		{
			standard_ExecutorStart(query_desc, eflags);
		}
	}
	PG_CATCH();
	{
		gpsc_query_leave(query_desc);
		report_executor_error(query_desc);
		PG_RE_THROW();
	}
	PG_END_TRY();
	gpsc_query_leave(query_desc);
	cpp_call(get_sender(), &EventSender::executor_after_start, query_desc,
			 eflags);
}

void
gpsc_ExecutorRun_hook(QueryDesc *query_desc, ScanDirection direction,
					  uint64 count)
{
	get_sender()->incr_depth();
	gpsc_query_enter(query_desc);
	PG_TRY();
	{
		if (previous_ExecutorRun_hook)
			previous_ExecutorRun_hook(query_desc, direction, count);
		else
			standard_ExecutorRun(query_desc, direction, count);
		get_sender()->decr_depth();
	}
	PG_CATCH();
	{
		get_sender()->decr_depth();
		gpsc_query_leave(query_desc);
		report_executor_error(query_desc);
		PG_RE_THROW();
	}
	PG_END_TRY();
	gpsc_query_leave(query_desc);
}

void
gpsc_ExecutorFinish_hook(QueryDesc *query_desc)
{
	get_sender()->incr_depth();
	gpsc_query_enter(query_desc);
	PG_TRY();
	{
		if (previous_ExecutorFinish_hook)
			previous_ExecutorFinish_hook(query_desc);
		else
			standard_ExecutorFinish(query_desc);
		get_sender()->decr_depth();
	}
	PG_CATCH();
	{
		get_sender()->decr_depth();
		gpsc_query_leave(query_desc);
		report_executor_error(query_desc);
		PG_RE_THROW();
	}
	PG_END_TRY();
	gpsc_query_leave(query_desc);
}

/*
 * The query's end: EXPLAIN ANALYZE's text while its plan can still be read,
 * its END, and once the executor is shut down its DONE, as Cloudberry's
 * standard_ExecutorEnd() tells its hook -- and its key forgotten, which one
 * the collector passed over has too.
 */
void
gpsc_ExecutorEnd_hook(QueryDesc *query_desc)
{
	cpp_call(get_sender(), &EventSender::analyze_stats_collect, query_desc);
	cpp_call(get_sender(), &EventSender::executor_end, query_desc);
	gpsc_query_enter(query_desc);
	PG_TRY();
	{
		if (previous_ExecutorEnd_hook)
		{
			(*previous_ExecutorEnd_hook)(query_desc);
		}
		else
		{
			standard_ExecutorEnd(query_desc);
		}
	}
	PG_CATCH();
	{
		report_executor_error(query_desc);
		gpsc_query_key_forget(query_desc);
		PG_RE_THROW();
	}
	PG_END_TRY();
	cpp_call(get_sender(), &EventSender::query_metrics_collect,
			 METRICS_QUERY_DONE, (void *) query_desc, false /* utility */,
			 (ErrorData *) NULL);
	gpsc_query_key_forget(query_desc);
}

/*
 * The rendezvous of Cloudberry's query_info_collect_hook: gp_resource's
 * calls, as a query waits in its queue.  An error or cancel it tells of is
 * the one being handled, which is copied here.
 */
void
gpsc_query_info_collect_hook(QueryMetricsStatus status, void *arg)
{
	ErrorData *edata = NULL;

	if (status == METRICS_QUERY_ERROR || status == METRICS_QUERY_CANCELED)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

		edata = CopyErrorData();
		MemoryContextSwitchTo(oldctx);
	}
	cpp_call(get_sender(), &EventSender::query_metrics_collect, status,
			 arg /* queryDesc */, false /* utility */, edata);
	if (edata != NULL)
	{
		FreeErrorData(edata);
	}
	if (previous_query_info_collect_hook)
	{
		(*previous_query_info_collect_hook)(status, arg);
	}
}

/*
 * An error on its way to the log, which the top-level handler sends it
 * before the transaction is aborted: kept for what the abort ends.
 */
void
gpsc_emit_log_hook(ErrorData *edata)
{
	if (edata->elevel >= ERROR && sender != nullptr)
	{
		try
		{
			sender->error_seen(edata);
		}
		catch (...)
		{
		}
	}
	if (previous_emit_log_hook)
	{
		(*previous_emit_log_hook)(edata);
	}
}

/*
 * A transaction's abort ends the queries it left in flight, and every
 * transaction's end makes the next one's number.  Nothing may be raised
 * here, while the transaction ends.
 */
void
gpsc_xact_callback(XactEvent event, void * /* arg */)
{
	if (event == XACT_EVENT_ABORT && sender != nullptr)
	{
		try
		{
			sender->abort_queries(InvalidSubTransactionId);
		}
		catch (...)
		{
		}
	}
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		gpsc_transaction_end();
	}
}

void
gpsc_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId /* parentSubid */, void * /* arg */)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && sender != nullptr)
	{
		try
		{
			sender->abort_queries(mySubid);
		}
		catch (...)
		{
		}
	}
}

static void
gpsc_process_utility_hook(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	/*
	 * On a segment, a statement that carries the coordinator's: neither a
	 * statement of its own nor a level of nesting of the one it carries.
	 */
	if (gpsc_segment() && gpsc_segment_transport(queryString))
	{
		if (previous_ProcessUtility_hook)
		{
			(*previous_ProcessUtility_hook)(pstmt, queryString, readOnlyTree,
											context, params, queryEnv, dest,
											qc);
		}
		else
		{
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		}
		return;
	}

	/* Project utility data on QueryDesc to use existing logic */
	QueryDesc *query_desc = (QueryDesc *) palloc0(sizeof(QueryDesc));
	query_desc->sourceText = queryString;

	cpp_call(get_sender(), &EventSender::query_metrics_collect,
			 METRICS_QUERY_SUBMIT, (void *) query_desc, true /* utility */,
			 (ErrorData *) NULL);

	gpsc_query_enter(query_desc);
	get_sender()->incr_depth();
	PG_TRY();
	{
		if (previous_ProcessUtility_hook)
		{
			(*previous_ProcessUtility_hook)(pstmt, queryString, readOnlyTree,
											context, params, queryEnv, dest,
											qc);
		}
		else
		{
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		}

		get_sender()->decr_depth();
		cpp_call(get_sender(), &EventSender::query_metrics_collect,
				 METRICS_QUERY_DONE, (void *) query_desc, true /* utility */,
				 (ErrorData *) NULL);

		gpsc_query_key_forget(query_desc);
		pfree(query_desc);
	}
	PG_CATCH();
	{
		ErrorData *edata;
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		edata = CopyErrorData();
		FlushErrorState();
		MemoryContextSwitchTo(oldctx);

		get_sender()->decr_depth();
		cpp_call(get_sender(), &EventSender::query_metrics_collect,
				 METRICS_QUERY_ERROR, (void *) query_desc, true /* utility */,
				 edata);

		gpsc_query_key_forget(query_desc);
		pfree(query_desc);
		ReThrowError(edata);
	}
	PG_END_TRY();
}

static void
check_stats_loaded()
{
	if (!GpscStat::loaded())
	{
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("gp_stats_collector must be loaded via "
							   "shared_preload_libraries")));
	}
}

void
gpsc_functions_reset()
{
	check_stats_loaded();
	GpscStat::reset();
}

Datum
gpsc_functions_get(FunctionCallInfo fcinfo)
{
	const int ATTNUM = 6;
	check_stats_loaded();
	auto stats = GpscStat::get_stats();
	TupleDesc tupdesc = CreateTemplateTupleDesc(ATTNUM);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segid", INT4OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "total_messages", INT8OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "send_failures", INT8OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "connection_failures", INT8OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "other_errors", INT8OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "max_message_size", INT4OID,
					   -1 /* typmod */, 0 /* attdim */);
	TupleDescFinalize(tupdesc);
	tupdesc = BlessTupleDesc(tupdesc);
	Datum values[ATTNUM];
	bool nulls[ATTNUM];
	MemSet(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(gpsc_content_id());
	values[1] = Int64GetDatum(stats.total);
	values[2] = Int64GetDatum(stats.failed_sends);
	values[3] = Int64GetDatum(stats.failed_connects);
	values[4] = Int64GetDatum(stats.failed_other);
	values[5] = Int32GetDatum(stats.max_message_size);
	HeapTuple tuple = gpdb::heap_form_tuple(tupdesc, values, nulls);
	Datum result = HeapTupleGetDatum(tuple);
	PG_RETURN_DATUM(result);
}

void
test_uds_stop_server()
{
	if (test_server_fd >= 0)
	{
		close(test_server_fd);
		test_server_fd = -1;
	}
	if (test_sock_path)
	{
		unlink(test_sock_path);
		pfree(test_sock_path);
		test_sock_path = NULL;
	}
}

void
test_uds_start_server(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};

	if (strlen(path) >= sizeof(addr.sun_path))
		ereport(ERROR, (errmsg("path too long")));

	test_uds_stop_server();

	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	test_sock_path = MemoryContextStrdup(TopMemoryContext, path);
	unlink(path);

	if ((test_server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
		bind(test_server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
		listen(test_server_fd, TEST_MAX_CONNECTIONS) < 0)
	{
		test_uds_stop_server();
		ereport(ERROR, (errmsg("socket setup failed: %m")));
	}
}

int64
test_uds_receive(int timeout_ms)
{
	char buf[TEST_RCV_BUF_SIZE];
	int rc;
	struct pollfd pfd = {.fd = test_server_fd, .events = POLLIN};
	int64 total = 0;

	if (test_server_fd < 0)
		ereport(ERROR, (errmsg("server not started")));

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		rc = poll(&pfd, 1, Min(timeout_ms, TEST_POLL_TIMEOUT_MS));
		if (rc > 0)
			break;
		if (rc < 0 && errno != EINTR)
			ereport(ERROR, (errmsg("poll: %m")));
		timeout_ms -= TEST_POLL_TIMEOUT_MS;
		if (timeout_ms <= 0)
			return total;
	}

	if (pfd.revents & POLLIN)
	{
		int client = accept(test_server_fd, NULL, NULL);
		ssize_t n;

		if (client < 0)
			ereport(ERROR, (errmsg("accept: %m")));

		while ((n = recv(client, buf, sizeof(buf), 0)) != 0)
		{
			if (n > 0)
				total += n;
			else if (errno != EINTR)
				break;
		}

		close(client);
	}

	return total;
}