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
 * gp_exttable.c
 *	  External tables: the module, its settings, and what it does as a
 *	  statement starts and ends.
 *
 * External tables stay a foreign data wrapper, gp_exttable_fdw, as
 * Cloudberry made them (exttable_fdw.c).  CREATE EXTERNAL TABLE is its
 * foreign table (exttable_ddl.c), reading and writing go through COPY's
 * parser and a writer of the module's own (extaccess.c, copyout.c), the data
 * comes from a file, a command, gpfdist or a protocol of the user's (url*.c),
 * and single row error handling logs what it rejects (sreh.c).
 *
 * The module is the one of the port's a database gets with CREATE EXTENSION
 * alone, not preloaded: what has to happen in every backend -- a CREATE
 * EXTERNAL TABLE's clauses made options -- gp_sql, which is preloaded, asks
 * of it, loading it.  Loaded, it gives each statement that reads an external
 * table a name its segments share (url.c), and reports once, as a
 * statement ends, how many rows the segments rejected (sreh.c).
 *
 * Cloudberry sources this module is made of:
 *	  gpcontrib/gp_exttable_fdw/, src/backend/access/external/,
 *	  cdb/cdbsreh.c, commands/exttablecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"

#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_exttable.h"
#include "gp_policy.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_exttable",
					.version = GP_VERSION
);

bool		gp_external_enable_exec = true;
int			gp_external_max_segs = 64;
bool		gp_external_enable_filter_pushdown = true;
int			writable_external_table_bufsize = 64;
bool		verify_gpfdists_cert = true;

extern char *gp_exttable_statement_id;
extern char *gp_exttable_query_string;
extern bool SrehRelayNotice(const char *sqlstate, const char *message);

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static int	statement_depth = 0;
static uint64 statement_count = 0;

/*
 * A statement that starts on the coordinator is named for the segments'
 * gpfdist sessions (url.c): the coordinator's session and a count of its
 * statements.  gp_core sends the setting with the statement.
 */
static void
gp_exttable_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (statement_depth == 0 && !GpClusterIsSingleNode() &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		char		id[64];

		bool		external = false;

		snprintf(id, sizeof(id), "%d-%d-" UINT64_FORMAT,
				 GpClusterSessionId(), MyProcPid, ++statement_count);
		(void) set_config_option("gp_exttable.statement_id", id, PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SET, true, 0, false);

		/* and its text, where it has an external table to hand it to */
		foreach_node(RangeTblEntry, rte, queryDesc->plannedstmt->rtable)
			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_FOREIGN_TABLE &&
				GpPolicyIsExternalTable(rte->relid))
				external = true;
		if (external || (gp_exttable_query_string != NULL &&
						 gp_exttable_query_string[0] != '\0'))
			(void) set_config_option("gp_exttable.query_string",
									 external && debug_query_string ? debug_query_string : "",
									 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET,
									 true, 0, false);
	}

	statement_depth++;
	PG_TRY();
	{
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		statement_depth--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * As a statement ends: how many rows its scans rejected, on every node, said
 * once (Cloudberry's cdbdisp_sumRejectedRows()) -- for each statement, a
 * function's too, as each of Cloudberry's is dispatched and summed.
 */
static void
gp_exttable_ExecutorEnd(QueryDesc *queryDesc)
{
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (statement_depth > 0)
		statement_depth--;
	SrehReportRejected();
}

/*
 * A statement that fails never reaches ExecutorEnd, so the count of the
 * statements running is put back as the transaction, or the subtransaction
 * the failure is caught in, gives up: to none, or to what it was as the
 * subtransaction began.  What was rejected under it is never said.
 */
static List *subxact_depths = NIL;

static void
gp_exttable_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			statement_depth = 0;
			SrehForgetRejected();
			subxact_depths = NIL;	/* TopTransactionContext's, which goes */
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			subxact_depths = NIL;
			break;
		default:
			break;
	}
}

static void
gp_exttable_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
							 SubTransactionId parentSubid, void *arg)
{
	MemoryContext old;

	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
			old = MemoryContextSwitchTo(TopTransactionContext);
			subxact_depths = lcons_int(statement_depth, subxact_depths);
			MemoryContextSwitchTo(old);
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			if (subxact_depths != NIL)
			{
				statement_depth = linitial_int(subxact_depths);
				subxact_depths = list_delete_first(subxact_depths);
			}
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			if (subxact_depths != NIL)
				subxact_depths = list_delete_first(subxact_depths);
			break;
		default:
			break;
	}
}

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_exttable");

	DefineCustomBoolVariable("gp.external_enable_exec",
							 "Enable selecting from an external table with an EXECUTE clause.",
							 NULL, &gp_external_enable_exec, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.external_max_segs",
							"Maximum number of segments that connect to a single gpfdist URL.",
							NULL, &gp_external_max_segs, 64, 1, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.external_enable_filter_pushdown",
							 "Enable passing of query constraints to external table providers.",
							 NULL, &gp_external_enable_filter_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.initial_bad_row_limit",
							"Stops processing when the number of the first bad rows exceeds this value.",
							NULL, &gp_initial_bad_row_limit, 1000, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.reject_percent_threshold",
							"Reject limit in percent starts calculating after this number of rows processed.",
							NULL, &gp_reject_percent_threshold, 300, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.readable_external_table_timeout",
							"Cancel the query if no data read within N seconds.",
							NULL, &readable_external_table_timeout, 0, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_S, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.gpfdist_retry_timeout",
							"Timeout (in seconds) for writing data to gpfdist server.",
							NULL, &gpfdist_retry_timeout, 300, 1, INT_MAX,
							PGC_USERSET, GUC_UNIT_S, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.writable_external_table_bufsize",
							"Buffer size in kilobytes for a writable external table's data sent to gpfdist.",
							NULL, &writable_external_table_bufsize, 64, 32, 131072,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.verify_gpfdists_cert",
							 "Verifies the authenticity of the gpfdist's certificate.",
							 NULL, &verify_gpfdists_cert, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("gp_exttable.statement_id",
							   "The name of the statement a segment's gpfdist session is of.",
							   NULL, &gp_exttable_statement_id, "",
							   PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("gp_exttable.query_string",
							   "The text of the statement a segment's external table is read for.",
							   NULL, &gp_exttable_query_string, "",
							   PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
	MarkGUCPrefixReserved("gp_exttable");

	GpDispatchAddNoticeFilter(SrehRelayNotice);
	ExtRegisterLabelProvider();
	ExtProtocolRegisterLabelProvider();
	RegisterXactCallback(gp_exttable_xact_callback, NULL);
	RegisterSubXactCallback(gp_exttable_subxact_callback, NULL);

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = gp_exttable_ExecutorStart;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = gp_exttable_ExecutorEnd;
}
