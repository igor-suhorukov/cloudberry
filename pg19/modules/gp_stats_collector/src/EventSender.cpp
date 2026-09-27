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
 * EventSender.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/EventSender.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - Cloudberry's dispatcher is the coordinator or a node on its own, and
 *     its executor a segment's dispatched backend; a session of one node of
 *     a cluster, Cloudberry's utility mode, reports nothing, as there
 *     (PgUtils.cpp);
 *   - a segment reports what the coordinator's statement has it run -- a
 *     gather's query and a slice (gpsc_segment_slice()) -- and not the
 *     queries that carry it or set it up, which Cloudberry's segment never
 *     runs;
 *   - a query's key is the module's table's (gpsc_query_key()), which
 *     Cloudberry keeps in QueryDesc.gpsc_query_key, and "a query of no
 *     client command", Cloudberry's gp_command_count of 0, is one that is
 *     not a regular backend's;
 *   - a query already submitted -- gp_resource tells of one before its
 *     queue's wait, the executor's hook after -- is not submitted again;
 *   - an error's message is the one copied where it was caught, or the one
 *     the log saw last (error_seen()), where Cloudberry's elog_message()
 *     read the error being handled; and an abort ends what it leaves in
 *     flight (abort_queries()), which Cloudberry's PortalCleanup() and
 *     mppExecutorCleanup() report;
 *   - EXPLAIN ANALYZE's time is QueryDesc.query_instr's, which the query
 *     asks for before it starts, where Cloudberry's allocated totaltime
 *     after; and the plan's text has no Cloudberry statistics of its
 *     slices (INSTRUMENT_CDB, cdbexplain);
 *   - the interconnect's statistics are gone with Cloudberry's
 *     IC_TEARDOWN_HOOK, which Cloudberry's core never defines.
 *-------------------------------------------------------------------------
 */

#include "UDSConnector.h"
#include "log/LogOps.h"
#include "memory/gpdbwrappers.h"

#define typeid __typeid
extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/guc.h"
}
#undef typeid

#include <unordered_map>

#include "EventSender.h"
#include "PgUtils.h"
#include "ProtoUtils.h"

#define need_collect_analyze()                                        \
	(gpsc_coordinator() && config.min_analyze_time() >= 0 && \
	 config.enable_analyze())

/*
 * The queries' keys, by their QueryDesc's address, and the number of the
 * top-level transaction, which each transaction's end makes a new one.
 */
static std::unordered_map<uintptr_t, GpscQueryKey> query_keys;
static uint64_t transaction_number = 0;

GpscQueryKey *
gpsc_query_key(const QueryDesc *query_desc)
{
	auto it = query_keys.find((uintptr_t) query_desc);

	return it == query_keys.end() ? nullptr : &it->second;
}

GpscQueryKey *
gpsc_query_key_register(const QueryDesc *query_desc)
{
	GpscQueryKey &key = query_keys[(uintptr_t) query_desc];

	key = GpscQueryKey();
	key.xact = transaction_number;
	key.subid = GetCurrentSubTransactionId();
	return &key;
}

void
gpsc_query_key_forget(const QueryDesc *query_desc)
{
	query_keys.erase((uintptr_t) query_desc);
}

void
gpsc_query_enter(const QueryDesc *query_desc)
{
	GpscQueryKey *key = gpsc_query_key(query_desc);

	if (key != nullptr)
	{
		key->active++;
	}
}

void
gpsc_query_leave(const QueryDesc *query_desc)
{
	GpscQueryKey *key = gpsc_query_key(query_desc);

	if (key != nullptr)
	{
		key->active--;
	}
}

uint64_t
gpsc_transaction()
{
	return transaction_number;
}

void
gpsc_transaction_end()
{
	transaction_number++;
}

bool
EventSender::verify_query(QueryDesc *query_desc, QueryState state, bool utility)
{
	if (!proto_verified)
	{
		return false;
	}
	if (gpsc_utility_mode())
	{
		return false;
	}

	switch (state)
	{
		case QueryState::SUBMIT:
			// Cache GUCs once at SUBMIT. Synced GUCs are visible to all subsequent
			// states. Without caching, a query that unsets/sets filtering GUCs would
			// see different filter criteria at DONE, because at SUBMIT the query was
			// not executed yet, causing DONE to be skipped/added.
			config.sync();

			if (!config.enable_collector())
			{
				return false;
			}

			if (utility && !config.enable_utility())
			{
				return false;
			}

			// Register qkey for a nested query we won't report,
			// so we can detect nesting_level > 0 and skip reporting at end/done.
			if (!need_report_nested_query() && nesting_level > 0)
			{
				QueryKey::register_qkey(query_desc, nesting_level);
				return false;
			}
			if (is_top_level_query(query_desc, nesting_level))
			{
				nested_timing = 0;
				nested_calls = 0;
			}
			break;
		case QueryState::START:
			if (!qdesc_submitted(query_desc))
			{
				collect_query_submit(query_desc, false /* utility */);
			}
			break;
		case QueryState::DONE:
			if (utility && !config.enable_utility())
			{
				return false;
			}
		default:
			break;
	}

	if (filter_query(query_desc))
	{
		return false;
	}
	if (!nesting_is_valid(query_desc, nesting_level))
	{
		return false;
	}
	if (!segment_statement(query_desc, utility))
	{
		return false;
	}

	return true;
}

bool
EventSender::log_query_req(const gpsc::SetQueryReq &req,
						   const std::string &event, bool utility)
{
	bool clear_big_fields = false;
	switch (config.logging_mode())
	{
		case LOG_MODE_UDS:
			clear_big_fields = UDSConnector::report_query(req, event, config);
			break;
		case LOG_MODE_TBL:
			gpdb::insert_log(req, utility);
			clear_big_fields = false;
			break;
		default:
			Assert(false);
	}
	return clear_big_fields;
}

void
EventSender::query_metrics_collect(QueryMetricsStatus status, void *arg,
								   bool utility, ErrorData *edata)
{
	auto *query_desc = reinterpret_cast<QueryDesc *>(arg);
	switch (status)
	{
		case METRICS_PLAN_NODE_INITIALIZE:
		case METRICS_PLAN_NODE_EXECUTING:
		case METRICS_PLAN_NODE_FINISHED:
			// TODO
			break;
		case METRICS_QUERY_SUBMIT:
			if (!qdesc_submitted(query_desc))
				collect_query_submit(query_desc, utility);
			break;
		case METRICS_QUERY_START:
			// no-op: executor_after_start is enough
			break;
		case METRICS_QUERY_CANCELING:
			// it appears we're only interested in the actual CANCELED event.
			// for now we will ignore CANCELING state unless otherwise requested from
			// end users
			break;
		case METRICS_QUERY_DONE:
		case METRICS_QUERY_ERROR:
		case METRICS_QUERY_CANCELED:
		case METRICS_INNER_QUERY_DONE:
			collect_query_done(query_desc, utility, status, edata);
			break;
		default:
			ereport(ERROR, (errmsg("Unknown query status: %d", status)));
	}
}

void
EventSender::executor_before_start(QueryDesc *query_desc, int eflags)
{
	if (!verify_query(query_desc, QueryState::START, false /* utility*/))
	{
		return;
	}

	if (gpsc_coordinator() && config.enable_analyze() &&
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
	{
		query_desc->instrument_options |= INSTRUMENT_BUFFERS;
		query_desc->instrument_options |= INSTRUMENT_ROWS;
		query_desc->instrument_options |= INSTRUMENT_TIMER;
		// The query's time, for EXPLAIN ANALYZE's threshold.
		if (need_collect_analyze())
		{
			query_desc->query_instr_options |= INSTRUMENT_TIMER;
		}
	}
}

void
EventSender::executor_after_start(QueryDesc *query_desc, int /* eflags*/)
{
	if (!verify_query(query_desc, QueryState::START, false /* utility */))
	{
		return;
	}

	auto &query = get_query(query_desc);
	auto query_msg = query.message.get();
	*query_msg->mutable_start_time() = current_ts();
	update_query_state(query, QueryState::START, false /* utility */);
	set_query_plan(query_msg, query_desc, config);
	gpsc::GPMetrics stats;
	std::swap(stats, *query_msg->mutable_query_metrics());
	if (log_query_req(*query_msg, "started", false /* utility */))
	{
		clear_big_fields(query_msg);
	}
	std::swap(stats, *query_msg->mutable_query_metrics());
}

void
EventSender::executor_end(QueryDesc *query_desc)
{
	if (!verify_query(query_desc, QueryState::END, false /* utility */))
	{
		return;
	}

	auto &query = get_query(query_desc);
	auto *query_msg = query.message.get();
	*query_msg->mutable_end_time() = current_ts();
	update_query_state(query, QueryState::END, false /* utility */);
	if (is_top_level_query(query_desc, nesting_level))
	{
		set_gp_metrics(query_msg->mutable_query_metrics(), query_desc,
					   nested_calls, nested_timing);
	}
	else
	{
		set_gp_metrics(query_msg->mutable_query_metrics(), query_desc, 0, 0);
	}
	if (log_query_req(*query_msg, "ended", false /* utility */))
	{
		clear_big_fields(query_msg);
	}
}

void
EventSender::collect_query_submit(QueryDesc *query_desc, bool utility)
{
	if (!verify_query(query_desc, QueryState::SUBMIT, utility))
	{
		return;
	}

	gpsc_count_statement();
	submit_query(query_desc);
	auto &query = get_query(query_desc);
	auto *query_msg = query.message.get();
	query.errors_before = errors_seen;
	*query_msg = create_query_req(gpsc::QueryStatus::QUERY_STATUS_SUBMIT);
	*query_msg->mutable_submit_time() = current_ts();
	set_query_info(query_msg);
	set_qi_nesting_level(query_msg, nesting_level);
	set_qi_slice_id(query_msg);
	set_query_text(query_msg, query_desc, config);
	if (log_query_req(*query_msg, "submit", utility))
	{
		clear_big_fields(query_msg);
	}
	// take initial metrics snapshot so that we can safely take diff afterwards
	// in END or DONE events.
	set_gp_metrics(query_msg->mutable_query_metrics(), query_desc, 0, 0);
}

void
EventSender::report_query_done(QueryDesc *query_desc, QueryItem &query,
							   QueryMetricsStatus status, bool utility,
							   ErrorData *edata)
{
	gpsc::QueryStatus query_status;
	std::string msg;
	switch (status)
	{
		case METRICS_QUERY_DONE:
		case METRICS_INNER_QUERY_DONE:
			query_status = gpsc::QueryStatus::QUERY_STATUS_DONE;
			msg = "done";
			break;
		case METRICS_QUERY_ERROR:
			query_status = gpsc::QueryStatus::QUERY_STATUS_ERROR;
			msg = "error";
			break;
		case METRICS_QUERY_CANCELING:
			// at the moment we don't track this event, but I`ll leave this code
			// here just in case
			Assert(false);
			query_status = gpsc::QueryStatus::QUERY_STATUS_CANCELLING;
			msg = "cancelling";
			break;
		case METRICS_QUERY_CANCELED:
			query_status = gpsc::QueryStatus::QUERY_STATUS_CANCELED;
			msg = "cancelled";
			break;
		default:
			ereport(ERROR,
					(errmsg("Unexpected query status in query_done hook: %d",
							status)));
	}
	auto prev_state = query.state;
	update_query_state(query, QueryState::DONE, utility,
					   query_status == gpsc::QueryStatus::QUERY_STATUS_DONE);
	auto query_msg = query.message.get();
	query_msg->set_query_status(query_status);
	if (status == METRICS_QUERY_ERROR)
	{
		if (edata != NULL && edata->message != NULL)
		{
			set_qi_error_message(query_msg, edata->message, config);
		}
		else if (errors_seen > query.errors_before)
		{
			set_qi_error_message(query_msg, last_error.c_str(), config);
		}
		else
		{
			ereport(WARNING, (errmsg("GPSC missing error message")));
			ereport(DEBUG3, (errmsg("GPSC query sourceText: %s",
									query_desc->sourceText)));
		}
	}
	if (prev_state == START)
	{
		// We've missed ExecutorEnd call due to query cancel or error. It's
		// fine, but now we need to collect and report execution stats
		*query_msg->mutable_end_time() = current_ts();
		set_gp_metrics(query_msg->mutable_query_metrics(), query_desc,
					   nested_calls, nested_timing);
	}
	(void) log_query_req(*query_msg, msg, utility);
}

/*
 * A query its (sub)transaction's abort left in flight, which no hook saw end:
 * a portal that failed between the executor's calls, a cursor cancelled
 * between FETCHes.  Its QueryDesc may be gone by now, so it is reported
 * from what was kept of it alone -- its message, and the error the log saw
 * last, if one was seen since it was submitted -- as Cloudberry's
 * PortalCleanup() reports a failed portal's.
 */
void
EventSender::report_aborted(QueryItem &query, bool utility)
{
	bool canceled =
		errors_seen > query.errors_before && last_error_canceled;
	auto prev_state = query.state;
	auto query_msg = query.message.get();

	update_query_state(query, QueryState::DONE, utility, false);
	query_msg->set_query_status(canceled
									? gpsc::QueryStatus::QUERY_STATUS_CANCELED
									: gpsc::QueryStatus::QUERY_STATUS_ERROR);
	if (errors_seen > query.errors_before)
	{
		set_qi_error_message(query_msg, last_error.c_str(), config);
	}
	if (prev_state == START)
	{
		*query_msg->mutable_end_time() = current_ts();
	}
	(void) log_query_req(*query_msg, canceled ? "cancelled" : "error",
						 utility);
}

void
EventSender::collect_query_done(QueryDesc *query_desc, bool utility,
								QueryMetricsStatus status, ErrorData *edata)
{
	if (!verify_query(query_desc, QueryState::DONE, utility))
	{
		return;
	}

	// Skip sending done message if query errored before submit.
	if (!qdesc_submitted(query_desc))
	{
		if (status != METRICS_QUERY_ERROR)
		{
			ereport(WARNING, (errmsg("GPSC trying to process DONE hook for "
									 "unsubmitted and unerrored query")));
			ereport(DEBUG3, (errmsg("GPSC query sourceText: %s",
									query_desc->sourceText)));
		}
		return;
	}

	if (queries.empty())
	{
		ereport(WARNING,
				(errmsg("GPSC cannot find query to process DONE hook")));
		ereport(DEBUG3,
				(errmsg("GPSC query sourceText: %s", query_desc->sourceText)));
		return;
	}
	auto &query = get_query(query_desc);

	report_query_done(query_desc, query, status, utility, edata);

	if (need_report_nested_query())
		update_nested_counters(query_desc);

	queries.erase(QueryKey::from_qdesc(query_desc));
	gpsc_query_key_forget(query_desc);
}

/*
 * An error the log saw: kept for the queries an abort ends with it
 * (abort_queries()), where Cloudberry's elog_message() read it.
 */
void
EventSender::error_seen(const ErrorData *edata)
{
	last_error = edata->message != NULL ? edata->message : "";
	last_error_canceled = edata->sqlerrcode == ERRCODE_QUERY_CANCELED;
	errors_seen++;
}

/*
 * The queries an abort leaves in flight -- the top-level transaction's with
 * subid InvalidSubTransactionId, else those of the subtransaction subid and
 * the ones inside it -- ended as failed, and every key of theirs forgotten;
 * but for the ones a hook of the collector's is running, which that hook
 * sees end, failed or not.  A utility statement's is always its hook's.
 */
void
EventSender::abort_queries(SubTransactionId subid)
{
	for (auto it = query_keys.begin(); it != query_keys.end();)
	{
		const GpscQueryKey &key = it->second;

		if (key.xact != gpsc_transaction() || key.active > 0 ||
			(subid != InvalidSubTransactionId && key.subid < subid))
		{
			++it;
			continue;
		}
		auto query = queries.find(QueryKey::from_key(&key));
		if (query != queries.end())
		{
			report_aborted(query->second, false /* utility */);
			queries.erase(query);
		}
		it = query_keys.erase(it);
	}
}

void
EventSender::analyze_stats_collect(QueryDesc *query_desc)
{
	if (!verify_query(query_desc, QueryState::END, false /* utility */))
	{
		return;
	}
	if (!gpsc_coordinator())
	{
		return;
	}
	if (!query_desc->query_instr || !need_collect_analyze())
	{
		return;
	}

	double ms = INSTR_TIME_GET_MILLISEC(query_desc->query_instr->total);
	if (ms >= config.min_analyze_time())
	{
		auto &query = get_query(query_desc);
		auto *query_msg = query.message.get();
		set_analyze_plan_text(query_desc, query_msg, config);
	}
}

EventSender::EventSender()
{
	// Perform initial sync to get default GUC values
	config.sync();

	try
	{
		GOOGLE_PROTOBUF_VERIFY_VERSION;
		proto_verified = true;
	}
	catch (const std::exception &e)
	{
		ereport(INFO, (errmsg("GPSC protobuf version mismatch is detected %s",
							  e.what())));
	}
}

EventSender::~EventSender()
{
	for (const auto &[qkey, _] : queries)
	{
		ereport(LOG,
				(errmsg("GPSC query with missing done event: "
						"tmid=%d ssid=%d ccnt=%d nlvl=%d",
						qkey.tmid, qkey.ssid, qkey.ccnt, qkey.nesting_level)));
	}
}

// That's basically a very simplistic state machine to fix or highlight any bugs
// coming from GP
void
EventSender::update_query_state(QueryItem &query, QueryState new_state,
								bool utility, bool success)
{
	switch (new_state)
	{
		case QueryState::SUBMIT:
			Assert(false);
			break;
		case QueryState::START:
			if (query.state == QueryState::SUBMIT)
			{
				query.message->set_query_status(
					gpsc::QueryStatus::QUERY_STATUS_START);
			}
			else
			{
				Assert(false);
			}
			break;
		case QueryState::END:
			// Example of below assert triggering: CURSOR closes before ever being
			// executed Assert(query->state == QueryState::START ||
			// IsAbortInProgress());
			query.message->set_query_status(
				gpsc::QueryStatus::QUERY_STATUS_END);
			break;
		case QueryState::DONE:
			Assert(query.state == QueryState::END || !success || utility);
			query.message->set_query_status(
				gpsc::QueryStatus::QUERY_STATUS_DONE);
			break;
		default:
			Assert(false);
	}
	query.state = new_state;
}

EventSender::QueryItem &
EventSender::get_query(QueryDesc *query_desc)
{
	if (!qdesc_submitted(query_desc))
	{
		ereport(
			WARNING,
			(errmsg("GPSC attempting to get query that was not submitted")));
		ereport(DEBUG3,
				(errmsg("GPSC query sourceText: %s", query_desc->sourceText)));
		throw std::runtime_error(
			"Attempting to get query that was not submitted");
	}
	return queries.find(QueryKey::from_qdesc(query_desc))->second;
}

void
EventSender::submit_query(QueryDesc *query_desc)
{
	if (gpsc_query_key(query_desc))
	{
		ereport(WARNING,
				(errmsg("GPSC trying to submit already submitted query")));
		ereport(DEBUG3,
				(errmsg("GPSC query sourceText: %s", query_desc->sourceText)));
	}
	QueryKey::register_qkey(query_desc, nesting_level);
	auto key = QueryKey::from_qdesc(query_desc);
	auto [_, inserted] = queries.emplace(key, QueryItem(QueryState::SUBMIT));
	if (!inserted)
	{
		ereport(WARNING, (errmsg("GPSC duplicate query submit detected")));
		ereport(DEBUG3,
				(errmsg("GPSC query sourceText: %s", query_desc->sourceText)));
	}
}

void
EventSender::update_nested_counters(QueryDesc *query_desc)
{
	if (!is_top_level_query(query_desc, nesting_level))
	{
		auto &query = get_query(query_desc);
		nested_calls++;
		double end_time = protots_to_double(query.message->end_time());
		double start_time = protots_to_double(query.message->start_time());
		if (end_time >= start_time)
		{
			nested_timing += end_time - start_time;
		}
		else
		{
			ereport(WARNING,
					(errmsg("GPSC query start_time > end_time (%f > %f)",
							start_time, end_time)));
			ereport(DEBUG3, (errmsg("GPSC nested query text %s",
									query_desc->sourceText)));
		}
	}
}

bool
EventSender::qdesc_submitted(QueryDesc *query_desc)
{
	if (gpsc_query_key(query_desc) == NULL)
	{
		return false;
	}
	return queries.find(QueryKey::from_qdesc(query_desc)) != queries.end();
}

bool
EventSender::nesting_is_valid(QueryDesc *query_desc, int nesting_level)
{
	return need_report_nested_query() ||
		   is_top_level_query(query_desc, nesting_level);
}

bool
EventSender::need_report_nested_query()
{
	return config.report_nested_queries() && gpsc_coordinator();
}

bool
EventSender::filter_query(QueryDesc *query_desc)
{
	return !AmRegularBackendProcess() || query_desc->sourceText == nullptr ||
		   !config.enable_collector() || config.filter_user(get_user_name());
}

/*
 * Is this, on a segment, a statement of the coordinator's the segment runs:
 * a query of a gather or a slice, or a utility statement the coordinator
 * dispatched?  Not a query of what carries them -- the cursors' statements,
 * which the utility hook passes over, and the port's own queries of
 * gp_internal -- which Cloudberry's segment never runs.  Anywhere else,
 * every statement is.
 */
bool
EventSender::segment_statement(QueryDesc *query_desc, bool utility)
{
	if (!gpsc_segment())
	{
		return true;
	}
	return utility ? !gpsc_segment_transport(query_desc->sourceText)
				   : gpsc_segment_slice(query_desc->sourceText);
}

EventSender::QueryItem::QueryItem(QueryState st)
	: message(std::make_unique<gpsc::SetQueryReq>()), state(st)
{
}
