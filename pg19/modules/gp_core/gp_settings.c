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
 * gp_settings.c
 *	  Cloudberry's settings of the dispatcher and the planner that are not
 *	  ORCA's.
 *
 * Every name gains the "gp." prefix, as ORCA's do (config/gp_orca_guc.c):
 * PostgreSQL 19 defines no custom setting without a dot.  A Cloudberry name
 * that starts gp_ loses it -- gp_autostats_mode is gp.autostats_mode -- and
 * one that does not keeps its name -- statement_mem is gp.statement_mem.  The
 * defaults, ranges and descriptions are Cloudberry's
 * (github/cloudberry/src/backend/utils/misc/guc_gp.c).
 *
 * Two kinds.  Those the port carries out:
 *
 *	 gp.test_print_direct_dispatch_info  the INFO line per dispatched slice
 *	 gp.enable_direct_dispatch			 asking one segment when one holds
 *										 every row a query can touch
 *	 gp.autostats_mode, and the rest	 ANALYZE after a write, as auto_stats()
 *	 of autostats						 decides (postmaster/autostats.c)
 *	 gp.motion_cost_per_row				 the planner's cost of a gathered row
 *	 gp.enable_groupagg					 the planner's sorted grouping
 *
 * And those it accepts and has nothing to apply to yet, each for a reason
 * that says when it will: the planner's own MPP plans (Route B, decided at
 * M7), memory accounting (M6), the UDP interconnect, intra-segment
 * parallelism (after M7, decision 2) -- or that it will not: the executor's
 * prefetch of a join's quals, which PostgreSQL's joins do not do.  They are
 * defined so that a script written for Cloudberry runs; their descriptions
 * say what they do here, which is nothing until then.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <limits.h>

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/planner.h"
#include "parser/parse_node.h"
#include "parser/parsetree.h"
#include "tcop/dest.h"
#include "tcop/utility.h"
#include "catalog/namespace.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_settings.h"

/* ------------------------------------------------------------------------- */
/* The settings the port carries out                                         */
/* ------------------------------------------------------------------------- */

bool		gp_test_print_direct_dispatch_info = false;
bool		gp_enable_direct_dispatch = true;
double		gp_motion_cost_per_row = 0;

static bool gp_enable_groupagg = true;

typedef enum GpAutoStatsMode
{
	GP_AUTOSTATS_NONE = 0,
	GP_AUTOSTATS_ON_CHANGE,
	GP_AUTOSTATS_ON_NO_STATS,
} GpAutoStatsMode;

static const struct config_enum_entry autostats_modes[] = {
	{"none", GP_AUTOSTATS_NONE, false},
	{"on_change", GP_AUTOSTATS_ON_CHANGE, false},
	{"onchange", GP_AUTOSTATS_ON_CHANGE, true},
	{"on_no_stats", GP_AUTOSTATS_ON_NO_STATS, false},
	{NULL, 0, false}
};

static int	gp_autostats_mode = GP_AUTOSTATS_NONE;
static int	gp_autostats_mode_in_functions = GP_AUTOSTATS_NONE;
static int	gp_autostats_on_change_threshold = INT_MAX;
static bool gp_autostats_allow_nonowner = false;

/* ------------------------------------------------------------------------- */
/* The settings it accepts, with nothing to apply them to yet                */
/* ------------------------------------------------------------------------- */

static int	statement_mem = 128000;
static bool enable_parallel = false;
static int	gp_interconnect_queue_depth = 4;
static int	gp_vmem_idle_resource_timeout = 18000;
static int	gp_segments_for_planner = 0;
static bool gp_workfile_compression = false;
static bool gp_enable_multiphase_agg = true;
static bool gp_cte_sharing = false;
static bool test_print_prefetch_joinqual = false;
static bool gp_enable_preunique = true;
static bool gp_enable_agg_distinct_pruning = true;
static bool gp_eager_distinct_dedup = false;
static bool gp_enable_agg_pushdown = false;
static bool gp_enable_fast_sri = true;
static bool gp_force_random_redistribution = false;

/* The planner's MPP knobs: Cloudberry's planner makes plans the port's does not. */
#define ROUTE_B		" Accepted for Cloudberry's scripts: the planner here makes none of Cloudberry's multi-phase or motion plans, so it has nothing to apply this to until Route B (M7)."

static create_upper_paths_hook_type prev_create_upper_paths = NULL;
static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* the planner's gathers of the statement being started, so far */
static int	gather_slices = 0;

/* ------------------------------------------------------------------------- */
/* The INFO line of a dispatched slice                                       */
/* ------------------------------------------------------------------------- */

/*
 * What Cloudberry's segmentsToContentStr() says of a slice's segments
 * (cdb/dispatcher/cdbdisp.c): one is a SINGLE content, and every one is ALL
 * contents with their ids.
 */
void
GpReportDispatch(int slice, bool single)
{
	StringInfoData buf;
	int			nsegs;

	if (!gp_test_print_direct_dispatch_info)
		return;

	initStringInfo(&buf);
	GpClusterSegments(&nsegs);
	if (single || nsegs == 1)
		appendStringInfoString(&buf, "SINGLE content");
	else
	{
		appendStringInfoString(&buf, "ALL contents:");
		for (int i = 0; i < nsegs; i++)
			appendStringInfo(&buf, " %d", i);
	}
	elog(INFO, "(slice %d) Dispatch command to %s", slice, buf.data);
	pfree(buf.data);
}

int
GpNextGatherSlice(void)
{
	return ++gather_slices;
}

static void
settings_executor_start(QueryDesc *queryDesc, int eflags)
{
	gather_slices = 0;

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/* ------------------------------------------------------------------------- */
/* Autostats                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Has the table no statistics?  Cloudberry's test: relpages 0 and fewer than
 * one tuple counted, which is what a table never analyzed has.
 */
static bool
has_no_stats(Oid relid)
{
	HeapTuple	tuple;
	Form_pg_class classForm;
	bool		result;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return false;
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	result = classForm->relpages == 0 && classForm->reltuples < 1;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * ANALYZE the table, as Cloudberry's autostats_issue_analyze() does, and as
 * the owner of the table or the database could.  A distributed table is
 * sampled on the segments (O3, gp_analyze.c).
 */
static void
issue_analyze(Oid relid)
{
	VacuumStmt *stmt;
	ParseState *pstate;
	bool		pushed = false;

	if (!gp_autostats_allow_nonowner &&
		!object_ownercheck(RelationRelationId, relid, GetUserId()) &&
		!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		return;

	stmt = makeNode(VacuumStmt);
	stmt->options = NIL;
	stmt->rels = list_make1(makeVacuumRelation(NULL, relid, NIL));
	stmt->is_vacuumcmd = false;

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = NULL;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	ExecVacuum(pstate, stmt, false);
	if (pushed)
		PopActiveSnapshot();
	free_parsestate(pstate);
}

/*
 * Cloudberry's auto_stats(): on_change analyzes after any write of more rows
 * than the threshold; on_no_stats after a CTAS, and after an INSERT or COPY
 * into a table that has no statistics.  Only the coordinator does it, and a
 * statement run by a function follows gp.autostats_mode_in_functions.
 */
void
GpAutoStats(CmdType cmd, Oid relid, uint64 ntuples, bool in_function)
{
	int			mode = in_function ? gp_autostats_mode_in_functions
		: gp_autostats_mode;
	char		relkind;
	bool		analyze = false;

	if (mode == GP_AUTOSTATS_NONE || !OidIsValid(relid))
		return;
	if (GpClusterBackendRole() == GP_ROLE_EXECUTE)
		return;

	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
		return;

	switch (mode)
	{
		case GP_AUTOSTATS_ON_CHANGE:
			analyze = (cmd == CMD_INSERT || cmd == CMD_UPDATE ||
					   cmd == CMD_DELETE || cmd == CMD_MERGE) &&
				ntuples > (uint64) gp_autostats_on_change_threshold;
			break;
		case GP_AUTOSTATS_ON_NO_STATS:
			analyze = cmd == CMD_INSERT && has_no_stats(relid);
			break;
		default:
			break;
	}

	if (analyze)
		issue_analyze(relid);
}

/*
 * After a statement: the table it wrote, and how many rows.  A statement a
 * function runs reaches the executor through SPI or a SQL function's own
 * receiver, which is how "in a function" is told here.
 */
static void
settings_executor_end(QueryDesc *queryDesc)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	CmdType		cmd = stmt->commandType;
	Oid			relid = InvalidOid;
	uint64		ntuples = 0;
	bool		in_function;

	if (queryDesc->estate != NULL &&
		(cmd == CMD_INSERT || cmd == CMD_UPDATE || cmd == CMD_DELETE ||
		 cmd == CMD_MERGE) && !bms_is_empty(stmt->resultRelationRelids) &&
		!(queryDesc->estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		/* the first result relation, as Cloudberry's autostats takes */
		relid = rt_fetch(bms_next_member(stmt->resultRelationRelids, -1),
						 stmt->rtable)->relid;
		ntuples = queryDesc->estate->es_processed;
	}
	in_function = queryDesc->dest != NULL &&
		(queryDesc->dest->mydest == DestSPI ||
		 queryDesc->dest->mydest == DestSQLFunction);

	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (OidIsValid(relid))
		GpAutoStats(cmd, relid, ntuples, in_function);
}

/*
 * COPY FROM, and CREATE TABLE AS, which are utility statements: the rows they
 * wrote are in the completion tag.  On a cluster gp_sql turns CREATE TABLE
 * AS into the table WITH NO DATA and an INSERT, and calls GpAutoStats() for
 * it itself; what arrives here then says it wrote nothing.  A distributed
 * table's COPY is gp_modify.c's, which does the same.
 */
static void
settings_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						bool readOnlyTree, ProcessUtilityContext context,
						ParamListInfo params, QueryEnvironment *queryEnv,
						DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	RangeVar   *target = NULL;
	QueryCompletion local;

	if (IsA(parsetree, CopyStmt) && ((CopyStmt *) parsetree)->is_from)
		target = ((CopyStmt *) parsetree)->relation;
	else if (IsA(parsetree, CreateTableAsStmt) &&
			 ((CreateTableAsStmt *) parsetree)->objtype == OBJECT_TABLE &&
			 !((CreateTableAsStmt *) parsetree)->into->skipData)
		target = ((CreateTableAsStmt *) parsetree)->into->rel;

	if (target != NULL && qc == NULL)
	{
		InitializeQueryCompletion(&local);
		qc = &local;
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (target != NULL && qc->nprocessed > 0)
		GpAutoStats(CMD_INSERT, RangeVarGetRelid(target, NoLock, true),
					qc->nprocessed, context != PROCESS_UTILITY_TOPLEVEL);
}

/* ------------------------------------------------------------------------- */
/* enable_groupagg                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's enable_groupagg turns off the planner's grouping over sorted
 * input, as enable_hashagg turns off the hashed kind: a sorted Agg, or a
 * Group, is disabled -- chosen only where nothing else can do it, which is
 * how PostgreSQL's own enable_* settings work.  Only the paths that reach
 * this hook are seen, so a hashed path the planner already discarded as
 * dearer is not brought back.
 */
static void
settings_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					 RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	if (prev_create_upper_paths)
		prev_create_upper_paths(root, stage, input_rel, output_rel, extra);

	if (stage == UPPERREL_GROUP_AGG && !gp_enable_groupagg)
	{
		ListCell   *lc;

		foreach(lc, output_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			if ((IsA(path, AggPath) &&
				 ((AggPath *) path)->aggstrategy == AGG_SORTED) ||
				IsA(path, GroupPath))
				path->disabled_nodes++;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Definitions                                                               */
/* ------------------------------------------------------------------------- */

static void
define_accepted_bool(const char *name, const char *desc, bool *var, bool dflt)
{
	DefineCustomBoolVariable(name, desc, NULL, var, dflt, PGC_USERSET, 0,
							 NULL, NULL, NULL);
}

void
GpSettingsInit(void)
{
	/*
	 * Cloudberry's is also GUC_NO_SHOW_ALL, which here would hide it from
	 * pg_settings, where the test harnesses find the names they respell.
	 */
	DefineCustomBoolVariable("gp.test_print_direct_dispatch_info",
							 "For testing purposes, print information about direct dispatch decisions.",
							 "An INFO line for each slice a statement dispatches, and the segments it goes to.",
							 &gp_test_print_direct_dispatch_info,
							 false, PGC_SUSET,
							 GUC_SUPERUSER_ONLY | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.enable_direct_dispatch",
							 "Enable dispatch for single-row-insert targeted mirror-pairs.",
							 "Don't involve the whole cluster if it isn't needed.",
							 &gp_enable_direct_dispatch,
							 true, PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gp.motion_cost_per_row",
							 "Sets the planner's estimate of the cost of moving a row between worker processes.",
							 "If >0, the planner uses this value -- instead of double the cpu_tuple_cost -- for the cost of a row a gather moves.",
							 &gp_motion_cost_per_row,
							 0, 0, DBL_MAX, PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.enable_groupagg",
							 "Enables the planner's use of grouping aggregation plans.",
							 NULL,
							 &gp_enable_groupagg,
							 true, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.autostats_mode",
							 "Sets the autostats mode.",
							 "Valid values are NONE, ON_CHANGE, ON_NO_STATS. ON_CHANGE requires setting gp.autostats_on_change_threshold.",
							 &gp_autostats_mode,
							 GP_AUTOSTATS_NONE, autostats_modes,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.autostats_mode_in_functions",
							 "Sets the autostats mode for statements in procedural language functions.",
							 "Valid values are NONE, ON_CHANGE, ON_NO_STATS. ON_CHANGE requires setting gp.autostats_on_change_threshold.",
							 &gp_autostats_mode_in_functions,
							 GP_AUTOSTATS_NONE, autostats_modes,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.autostats_on_change_threshold",
							"Threshold for number of tuples added to table by CTAS or Insert-to to trigger autostats in on_change mode.",
							NULL,
							&gp_autostats_on_change_threshold,
							INT_MAX, 0, INT_MAX, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.autostats_allow_nonowner",
							 "Allow automatic stats collection on tables even for users who are not the owner of the relation.",
							 "If disabled, table statistics will be updated only when tables are modified by the owners of the relations.",
							 &gp_autostats_allow_nonowner,
							 false, PGC_SUSET, 0,
							 NULL, NULL, NULL);

	/* Accepted, with nothing to apply them to yet; see the file header. */
	DefineCustomIntVariable("gp.statement_mem",
							"Sets the memory to be reserved for a statement.",
							"Accepted for Cloudberry's scripts: memory is not accounted per statement until M6's resource management.",
							&statement_mem,
							128000, 50, INT_MAX, PGC_USERSET, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.enable_parallel",
							 "allow to use of parallel query facilities or not.",
							 "Accepted for Cloudberry's scripts: a segment runs each slice in one process until intra-segment parallelism (after M7, decision 2).",
							 &enable_parallel,
							 false, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_queue_depth",
							"Sets the maximum size of the receive queue for each connection in the UDP interconnect",
							"Accepted for Cloudberry's scripts: the interconnect here is TCP, which Cloudberry's own TCP interconnect also ignores this for.",
							&gp_interconnect_queue_depth,
							4, 1, 4096, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.vmem_idle_resource_timeout",
							"Sets the time a session can be idle (in milliseconds) before we release gangs on the segment DBs to free resources.",
							"Accepted for Cloudberry's scripts: a session keeps its segment connections until it ends.",
							&gp_vmem_idle_resource_timeout,
							18000, 0, INT_MAX, PGC_USERSET, GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.workfile_compression",
							 "Enables compression of temporary files.",
							 "Accepted for Cloudberry's scripts: temporary files are PostgreSQL's own, which are not compressed.",
							 &gp_workfile_compression,
							 false, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.test_print_prefetch_joinqual",
							 "For testing purposes, print information about if we prefetch join qual.",
							 "Accepted for Cloudberry's scripts: the joins here are PostgreSQL's, which never prefetch a join's quals, so there is nothing to print.",
							 &test_print_prefetch_joinqual,
							 false, PGC_SUSET,
							 GUC_SUPERUSER_ONLY | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.segments_for_planner",
							"If >0, number of segment dbs for the planner to assume in its cost and size estimates." ROUTE_B,
							NULL,
							&gp_segments_for_planner,
							0, 0, INT_MAX, PGC_USERSET, 0,
							NULL, NULL, NULL);
	define_accepted_bool("gp.enable_multiphase_agg",
						 "Enables the planner's use of two- or three-stage parallel aggregation plans." ROUTE_B,
						 &gp_enable_multiphase_agg, true);
	define_accepted_bool("gp.cte_sharing",
						 "This guc enables sharing of plan fragments for common table expressions." ROUTE_B,
						 &gp_cte_sharing, false);
	define_accepted_bool("gp.enable_preunique",
						 "Enable 2-phase duplicate removal." ROUTE_B,
						 &gp_enable_preunique, true);
	define_accepted_bool("gp.enable_agg_distinct_pruning",
						 "Enable 3-phase aggregation and join to compute distinct-qualified aggregates." ROUTE_B,
						 &gp_enable_agg_distinct_pruning, true);
	define_accepted_bool("gp.eager_distinct_dedup",
						 "Eager a 3-phase agg with deduplication for DISTINCT aggregations." ROUTE_B,
						 &gp_eager_distinct_dedup, false);
	define_accepted_bool("gp.enable_agg_pushdown",
						 "Enables aggregate push-down." ROUTE_B " PostgreSQL 19's enable_eager_aggregate is the nearest thing it has.",
						 &gp_enable_agg_pushdown, false);
	define_accepted_bool("gp.enable_fast_sri",
						 "Enable single-slice single-row inserts." ROUTE_B,
						 &gp_enable_fast_sri, true);
	define_accepted_bool("gp.force_random_redistribution",
						 "Force redistribution of insert for randomly-distributed." ROUTE_B,
						 &gp_force_random_redistribution, false);

	prev_create_upper_paths = create_upper_paths_hook;
	create_upper_paths_hook = settings_upper_paths;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = settings_ProcessUtility;

	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = settings_executor_start;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = settings_executor_end;
}
