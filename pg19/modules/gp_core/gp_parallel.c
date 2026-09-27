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
 * gp_parallel.c
 *	  Parallelism within a segment: PostgreSQL's Gather in the segment's
 *	  writer.
 *
 * Cloudberry runs a slice in parallel by widening its gang: a slice whose
 * path has parallel_workers has that many processes on each segment,
 * sibling QEs sharing their scans through a DSM they find by the session,
 * the command and the slice (README.cbdb.parallel), under enable_parallel.
 * The port takes PostgreSQL's own design first, as decision 2 has it: a
 * segment's process runs its part of a statement as it does, and a Gather
 * in that part starts PostgreSQL's parallel workers, which read the scan
 * below it with the process.
 *
 * Only the writer starts them, the session's first backend on the segment,
 * which runs what the coordinator gathers.  Starting workers makes a backend
 * the leader of a lock group (BecomeLockGroupLeader()), and the writer is
 * one already, of its readers (gp_share.c): its workers join the same
 * group.  A reader, a member of that group, can lead none, and plans none.
 *
 * What the writer runs for the coordinator runs in a cursor, fetched in
 * batches (gp_dispatch.c), and PostgreSQL runs a plan in parallel mode only
 * when it runs it whole (ExecutePlan()): so a statement with a Gather is
 * run whole at its first FETCH, into a tuplestore, and each FETCH is handed
 * its rows from there.  It is marked to be (GP_RUN_WHOLE_MARK) where it
 * will be read whole anyway:
 *
 *	 the planner's route	a gather's query, which the coordinator marked as
 *							read to its end (GP_WHOLE_MARKER, gp_scan.c), is
 *							planned here as a query run whole is, parallel
 *							plans allowed, and PostgreSQL's costs decide
 *							whether a Gather pays.
 *
 *	 ORCA's					the fragment of a Gather Motion, which the ORCA
 *							module gave Gathers on the coordinator, where the
 *							coordinator reads the Motion to its end -- over
 *							its large scans, the hash joins above them and
 *							the aggregates it splits in three stages
 *							(orca/parallel.c): it runs in parallel mode in
 *							the writer, and in a reader, or for a statement
 *							that cannot, without its Gathers.
 *
 * The settings are Cloudberry's: gp.enable_parallel, off by default as
 * Cloudberry's is, says the segments may start workers for this session's
 * statements, and PostgreSQL's own -- max_parallel_workers_per_gather and
 * the costs of a parallel plan -- say how many and where they pay, sent to
 * the segments with the statement.  Cloudberry's settings of its parallel
 * joins are accepted for its scripts: the port makes no parallel join
 * across segments.
 *
 * Cloudberry sources this file stands in for:
 *	  the settings of src/backend/utils/misc/guc_gp.c that parallel plans
 *	  read (enable_parallel and its kin); its widened gangs
 *	  (README.cbdb.parallel) as far as a Gather within a segment's process
 *	  does their work
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/planner.h"
#include "storage/proc.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_motion.h"
#include "gp_parallel.h"
#include "gp_scan.h"
#include "gp_share.h"

/* Cloudberry's enable_parallel */
static bool enable_parallel = false;

/* Cloudberry's settings of its parallel joins, accepted */
static bool enable_parallel_semi_join = true;
static bool enable_parallel_dedup_semi_join = true;
static bool enable_parallel_dedup_semi_reverse_join = true;
static bool parallel_query_use_streaming_hashagg = true;

/* What the parallel joins of Cloudberry's planner are, here. */
#define PARALLEL_JOINS	" Accepted for Cloudberry's scripts: the port's parallelism is PostgreSQL's Gather in a segment's process, which makes no parallel join across segments."

/*
 * On a PlannedStmt, in its extension_state: run it whole at its first FETCH
 * (above).
 */
#define GP_RUN_WHOLE_MARK	"gp_run_whole"

static planner_hook_type prev_planner = NULL;
static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;

bool
GpParallelEnabled(void)
{
	return enable_parallel && max_parallel_workers_per_gather > 0;
}

/* ------------------------------------------------------------------------- */
/* A statement with a Gather, run whole at its first FETCH                   */
/* ------------------------------------------------------------------------- */

/* A statement run whole, and the rows it has not handed out yet. */
typedef struct RunWhole
{
	EState	   *estate;			/* the statement's, whose rows these are */
	Tuplestorestate *store;
	TupleTableSlot *slot;		/* a row of it, read back */
	MemoryContextCallback forget;	/* as the statement's memory goes */
} RunWhole;

/* The statements this backend is running whole, in TopMemoryContext. */
static List *run_wholes = NIL;

static bool
run_whole_marked(PlannedStmt *stmt)
{
	foreach_node(DefElem, def, stmt->extension_state)
		if (strcmp(def->defname, GP_RUN_WHOLE_MARK) == 0)
			return true;
	return false;
}

static void
mark_run_whole(PlannedStmt *stmt)
{
	if (!run_whole_marked(stmt))
		stmt->extension_state =
			lappend(stmt->extension_state,
					makeDefElem(pstrdup(GP_RUN_WHOLE_MARK),
								(Node *) makeBoolean(true), -1));
}

static RunWhole *
run_whole_find(EState *estate)
{
	foreach_ptr(RunWhole, rw, run_wholes)
		if (rw->estate == estate)
			return rw;
	return NULL;
}

/*
 * The statement's memory going, as its executor ends or its portal is
 * dropped: an abort's too, whose resource owners have closed the store's
 * files already, so it is only forgotten here (parallel_executor_end()).
 */
static void
run_whole_forget(void *arg)
{
	run_wholes = list_delete_ptr(run_wholes, arg);
}

/* Run it whole, now, in parallel mode, its rows into a tuplestore. */
static RunWhole *
run_whole(QueryDesc *queryDesc)
{
	EState	   *estate = queryDesc->estate;
	DestReceiver *orig = queryDesc->dest;
	DestReceiver *dest;
	RunWhole   *rw;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	rw = palloc0_object(RunWhole);
	rw->estate = estate;
	rw->store = tuplestore_begin_heap(false, false, work_mem);
	rw->slot = MakeSingleTupleTableSlot(queryDesc->tupDesc, &TTSOpsMinimalTuple);
	rw->forget.func = run_whole_forget;
	rw->forget.arg = rw;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &rw->forget);
	MemoryContextSwitchTo(TopMemoryContext);
	run_wholes = lappend(run_wholes, rw);
	MemoryContextSwitchTo(oldcxt);

	dest = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(dest, rw->store, estate->es_query_cxt,
									false, NULL, NULL);
	queryDesc->dest = dest;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, ForwardScanDirection, 0);
		else
			standard_ExecutorRun(queryDesc, ForwardScanDirection, 0);
	}
	PG_FINALLY();
	{
		queryDesc->dest = orig;
	}
	PG_END_TRY();
	dest->rDestroy(dest);
	return rw;
}

/*
 * What a FETCH of a statement run whole returns: its next rows, as many as
 * it asks for, as standard_ExecutorRun() would have sent them.
 */
static void
run_whole_fetch(QueryDesc *queryDesc, RunWhole *rw, ScanDirection direction,
				uint64 count)
{
	EState	   *estate = queryDesc->estate;
	DestReceiver *dest = queryDesc->dest;
	MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	uint64		n = 0;

	dest->rStartup(dest, queryDesc->operation, queryDesc->tupDesc);
	if (ScanDirectionIsForward(direction))
	{
		while ((count == 0 || n < count) &&
			   tuplestore_gettupleslot(rw->store, true, false, rw->slot))
		{
			if (!dest->receiveSlot(rw->slot, dest))
				break;
			n++;
		}
	}
	dest->rShutdown(dest);
	estate->es_processed = n;
	MemoryContextSwitchTo(oldcxt);
}

/*
 * A marked statement's FETCH on the writer: the first runs it whole, and
 * each is handed its rows from what it made.  One run with no count -- a
 * statement not fetched in batches -- is PostgreSQL's own, in parallel mode
 * already; so is one a FETCH has begun some other way.  A parallel worker
 * runs its share of the plan as PostgreSQL has it run.
 */
static void
parallel_executor_run(QueryDesc *queryDesc, ScanDirection direction,
					  uint64 count)
{
	RunWhole   *rw = NULL;

	if (!IsParallelWorker() && run_whole_marked(queryDesc->plannedstmt))
	{
		rw = run_whole_find(queryDesc->estate);
		if (rw == NULL && count != 0 && !queryDesc->already_executed &&
			ScanDirectionIsForward(direction))
			rw = run_whole(queryDesc);
	}
	if (rw != NULL)
	{
		run_whole_fetch(queryDesc, rw, direction, count);
		return;
	}

	if (prev_executor_run)
		prev_executor_run(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
}

/*
 * Its executor ending: the store closed, and its files with it, before the
 * transaction's resource owners find them open.
 */
static void
parallel_executor_end(QueryDesc *queryDesc)
{
	RunWhole   *rw = run_whole_find(queryDesc->estate);

	if (rw != NULL)
	{
		tuplestore_end(rw->store);
		ExecDropSingleTupleTableSlot(rw->slot);
		run_whole_forget(rw);
	}

	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/* ------------------------------------------------------------------------- */
/* The planner's route                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The coordinator: the gathers the statement starting here reads to their
 * end, marked (GpGatherScanMarkWhole()).  Its top reads its rows to the end
 * where it is the client's statement, in the unnamed portal -- not a cursor,
 * which is fetched as far as its client asks, nor a query a function runs,
 * which may take one row of it.
 */
static void
parallel_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && GpParallelEnabled())
		GpGatherScanMarkWhole(queryDesc->planstate,
							  queryDesc->sourceText == debug_query_string &&
							  (ActivePortal == NULL ||
							   ActivePortal->name == NULL ||
							   ActivePortal->name[0] == '\0'));
}

/* Does a plan have a Gather of PostgreSQL's? */
static bool
plan_has_gather(Plan *plan)
{
	if (plan == NULL)
		return false;
	check_stack_depth();
	if (IsA(plan, Gather) || IsA(plan, GatherMerge))
		return true;
	if (plan_has_gather(plan->lefttree) || plan_has_gather(plan->righttree))
		return true;
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				if (plan_has_gather(child))
					return true;
			break;
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				if (plan_has_gather(child))
					return true;
			break;
		case T_SubqueryScan:
			return plan_has_gather(((SubqueryScan *) plan)->subplan);
		case T_CustomScan:
			foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
				if (plan_has_gather(child))
					return true;
			break;
		default:
			break;
	}
	return false;
}

/*
 * A segment: is this a gather's query that the coordinator marked as read
 * to its end -- "DECLARE gp_gather_N ... FOR <query>", GP_WHOLE_MARKER,
 * "; FETCH ...", on the coordinator's own connection (gp_scan.c's
 * gather_start(), gp_dispatch.c's)?  Only a permission to plan with
 * workers: what the query reads, the coordinator checked (gp_motion.c).
 */
static bool
gather_read_whole(const char *query_string)
{
	return query_string != NULL &&
		strncmp(query_string, "DECLARE gp_gather_", 18) == 0 &&
		strstr(query_string, GP_WHOLE_MARKER "; FETCH ") != NULL &&
		GpClusterDispatchTrusted();
}

/*
 * A fragment's Gathers taken out: each replaced by what is below it, whose
 * columns a Gather passes on as they are (orca/parallel.c), run whole by its
 * one process -- the scan that drove it no longer parallel-aware, and an
 * aggregate's middle stage combining the one state below it.
 */
static Plan *
strip_gathers(Plan *plan)
{
	if (plan == NULL)
		return NULL;
	check_stack_depth();
	if (IsA(plan, Gather))
	{
		for (Plan *below = plan->lefttree; below != NULL; below = below->lefttree)
			below->parallel_aware = false;
		return strip_gathers(plan->lefttree);
	}
	plan->lefttree = strip_gathers(plan->lefttree);
	plan->righttree = strip_gathers(plan->righttree);
	switch (nodeTag(plan))
	{
		case T_Append:
			{
				ListCell   *lc;

				foreach(lc, ((Append *) plan)->appendplans)
					lfirst(lc) = strip_gathers((Plan *) lfirst(lc));
				break;
			}
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan =
				strip_gathers(((SubqueryScan *) plan)->subplan);
			break;
		default:
			break;
	}
	return plan;
}

/*
 * A fragment of ORCA's with Gathers in it (orca/parallel.c).  The writer
 * runs it in parallel mode, whole at its first FETCH, where the session
 * still asks for workers: not a lock group's member, which cannot lead a
 * group of its own, and a SELECT with no data-modifying CTE.  A worker
 * opens each relation with its range table's lock mode, and asserts that
 * it holds that lock (execUtils.c): a relation ORCA left without one is
 * read in AccessShareLock, as the writer locks it (gp_motion.c).  Anywhere
 * else the Gathers go.
 */
static void
fragment_gathers(PlannedStmt *stmt)
{
	if (GpParallelEnabled() && !GpShareIsReader() &&
		(MyProc->lockGroupLeader == NULL || MyProc->lockGroupLeader == MyProc) &&
		stmt->commandType == CMD_SELECT && !stmt->hasModifyingCTE)
	{
		foreach_node(RangeTblEntry, rte, stmt->rtable)
			if (rte->rtekind == RTE_RELATION && rte->rellockmode == NoLock)
				rte->rellockmode = AccessShareLock;
		stmt->parallelModeNeeded = true;
		mark_run_whole(stmt);
	}
	else
		stmt->planTree = strip_gathers(stmt->planTree);
}

/*
 * A gather's query read whole is planned as a statement run whole is:
 * parallel plans allowed, and for all of its rows, not the first ones a
 * cursor is planned for (CURSOR_OPT_FAST_PLAN).  A plan with a Gather runs
 * whole at its first FETCH.  A reader, which plans none (gp_share.c), is
 * never sent one.  And a fragment the coordinator sends (gp_motion.c's
 * fragment_plan()) keeps its Gathers where it may.
 */
static PlannedStmt *
parallel_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;
	bool		whole = false;

	if (GpClusterIsDispatched() && parse->commandType == CMD_SELECT &&
		gather_read_whole(query_string))
	{
		whole = true;
		cursorOptions |= CURSOR_OPT_PARALLEL_OK;
		cursorOptions &= ~CURSOR_OPT_FAST_PLAN;
	}

	if (prev_planner)
		stmt = prev_planner(parse, query_string, cursorOptions, boundParams,
							es);
	else
		stmt = standard_planner(parse, query_string, cursorOptions,
								boundParams, es);

	if (whole && stmt->parallelModeNeeded && plan_has_gather(stmt->planTree))
		mark_run_whole(stmt);
	else if (GpClusterIsDispatched() && GpMotionIsFragment(stmt) &&
			 plan_has_gather(stmt->planTree))
		fragment_gathers(stmt);
	return stmt;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpParallelInit(void)
{
	DefineCustomBoolVariable("gp.enable_parallel",
							 "allow to use of parallel query facilities or not.",
							 "A segment's process may start PostgreSQL's parallel workers for a part of this session's statements where PostgreSQL's settings say they pay.  Cloudberry calls this enable_parallel.",
							 &enable_parallel,
							 false, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.enable_parallel_semi_join",
							 "allow to use of parallel semi join.",
							 "Cloudberry calls this enable_parallel_semi_join." PARALLEL_JOINS,
							 &enable_parallel_semi_join,
							 true, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.enable_parallel_dedup_semi_join",
							 "allow to use of parallel dedup semi join.",
							 "Cloudberry calls this enable_parallel_dedup_semi_join." PARALLEL_JOINS,
							 &enable_parallel_dedup_semi_join,
							 true, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.enable_parallel_dedup_semi_reverse_join",
							 "allow to use of parallel dedup semi reverse join.",
							 "Cloudberry calls this enable_parallel_dedup_semi_reverse_join." PARALLEL_JOINS,
							 &enable_parallel_dedup_semi_reverse_join,
							 true, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.parallel_query_use_streaming_hashagg",
							 "allow to use of streaming hashagg in parallel query for DISTINCT.",
							 "Cloudberry calls this parallel_query_use_streaming_hashagg." PARALLEL_JOINS,
							 &parallel_query_use_streaming_hashagg,
							 true, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);

	/* the rest is a cluster's */
	if (GpClusterIsSingleNode())
		return;

	prev_planner = planner_hook;
	planner_hook = parallel_planner;
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = parallel_executor_start;
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = parallel_executor_run;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = parallel_executor_end;
}
