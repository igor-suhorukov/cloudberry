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
 * compat/sharedscan.c
 *	  ORCA's CTE in a slice the segments run: Cloudberry's ShareInputScan,
 *	  as a producer that writes the CTE's rows to files each segment keeps,
 *	  and consumers that read them, in its slice or in others.
 *
 * ORCA plans a CTE once -- a producer under a Sequence, which runs it before
 * the plan that reads it -- and reads it where the query does, a consumer
 * for each reference.  In the coordinator's slice the translator makes the
 * CTE PostgreSQL's own: a subplan whose CteScans share one tuplestore
 * (TranslateDXLCTEProducerToSharedScan).  A fragment the segments run
 * carries no such subplan -- its initplans are the coordinator's
 * (compat/motion.c) -- and a consumer below a Motion is in another slice,
 * run by another process on each segment, which could not read that
 * tuplestore anyway.  Cloudberry's ShareInputScan writes the producer's rows
 * to a file each segment keeps, which the consumers in the other slices
 * read once the producer has written them all (shareinput_Xslice ...);
 * these do the same, in its slice too, in the FileSet gp_core names after
 * the statement on each segment (GpCoreApi.share_fileset):
 *
 *	 Sequence			its outer plan the plan that reads the CTEs, its
 *						custom plans a producer for each; the producers run
 *						first, once, and the rows are the outer plan's
 *	 Shared Scan		a producer: its child's rows written to "share<id>",
 *						then "share<id>.ready" made
 *	 Shared Scan		a consumer: waits for "share<id>.ready", then reads
 *						"share<id>" -- again from the start for a rescan
 *
 * A consumer in another slice than its producer's runs, as the producer
 * does, on all the segments, each segment's consumers reading the rows its
 * producer wrote, as the plan's distribution of the CTE has them; and the
 * slices run at once.  A consumer's slice sends, through Motions, to the
 * producer's, whose Sequence runs the producer before it reads them: no
 * slice waits for one that waits for it.  The translator plans nothing
 * else, and gp_core refuses to relay the slices of such a plan one at a
 * time (stream_plan()), which would run a consumer before its producer.
 *
 * But a slice may be done without ever asking its Sequence for a row: a
 * hash join whose outer side is empty on a segment -- every row of it
 * redistributed to another -- builds no hash table there, and the Sequence
 * under it never runs.  The consumers of that segment, in the other
 * slices, would wait for the producer for ever, and the processes of every
 * segment that wait for their rows with them: the statement hung.  So a
 * producer the translator marks as read in other slices (the fourth of its
 * custom_private, gp_orca_set_share_across()) is run when its slice's plan
 * is done, if it has not run -- the Sequence's ShutdownCustomScan, which the
 * executor calls when a run of the plan ends, before gp_core ends the
 * fragment's streams -- as Cloudberry's squelched ShareInputScan writes its
 * rows for the other slices (ExecSquelchShareInputScan()).
 * The files go when the Gather above ends, with its Motions' rows
 * (gp_internal.motion_drop()), or with the aborted transaction of the
 * process that wrote them.
 *
 * A row is a MinimalTuple as it is, its length first.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "storage/buffile.h"
#include "storage/fileset.h"
#include "storage/latch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "cb_compat.h"
#include "cb_sharedscan.h"

/* custom_private of a Shared Scan */
#define SHARE_PRIVATE_ID		0
#define SHARE_PRIVATE_SLICE		1
#define SHARE_PRIVATE_PRODUCER	2
#define SHARE_PRIVATE_ACROSS	3	/* a producer: read in other slices */

/* How long a consumer sleeps between looks for its rows, at most, in ms. */
#define SHARE_WAIT_MAX_MS		20

typedef struct SequenceState
{
	CustomScanState css;
	bool		produced;
	bool		across;			/* a producer's CTE is read in other slices */
	bool		described;		/* initialised only to be described: the
								 * coordinator's copy of a fragment */
	bool		explained;		/* its children in EXPLAIN's order */
} SequenceState;

typedef struct SharedScanState
{
	CustomScanState css;
	int			share_id;
	bool		producer;
	FileSet		fileset;
	BufFile    *file;			/* a consumer's, once the rows are there */
} SharedScanState;

/*
 * The FileSets this process wrote a share in, in this transaction: removed
 * if it aborts, and otherwise left to the Gather's end.  In
 * TopMemoryContext.
 */
static List *written_filesets = NIL;

static Node *create_sequence_state(CustomScan *cscan);
static void begin_sequence(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *exec_sequence(CustomScanState *node);
static void end_sequence(CustomScanState *node);
static void rescan_sequence(CustomScanState *node);

static Node *create_shared_scan_state(CustomScan *cscan);
static void shutdown_sequence(CustomScanState *node);
static void explain_sequence(CustomScanState *node, List *ancestors,
							 ExplainState *es);
static void begin_shared_scan(CustomScanState *node, EState *estate,
							  int eflags);
static TupleTableSlot *exec_shared_scan(CustomScanState *node);
static void end_shared_scan(CustomScanState *node);
static void rescan_shared_scan(CustomScanState *node);
static void explain_shared_scan(CustomScanState *node, List *ancestors,
								ExplainState *es);

const CustomScanMethods gp_orca_sequence_methods = {
	.CustomName = "Sequence",
	.CreateCustomScanState = create_sequence_state,
};

static const CustomExecMethods sequence_exec_methods = {
	.CustomName = "Sequence",
	.BeginCustomScan = begin_sequence,
	.ExecCustomScan = exec_sequence,
	.EndCustomScan = end_sequence,
	.ReScanCustomScan = rescan_sequence,
	.ShutdownCustomScan = shutdown_sequence,
	.ExplainCustomScan = explain_sequence,
};

const CustomScanMethods gp_orca_shared_scan_methods = {
	.CustomName = "Shared Scan",
	.CreateCustomScanState = create_shared_scan_state,
};

static const CustomExecMethods shared_scan_exec_methods = {
	.CustomName = "Shared Scan",
	.BeginCustomScan = begin_shared_scan,
	.ExecCustomScan = exec_shared_scan,
	.EndCustomScan = end_shared_scan,
	.ReScanCustomScan = rescan_shared_scan,
	.ExplainCustomScan = explain_shared_scan,
};

bool
gp_orca_can_share_across_slices(void)
{
	const GpCoreApi *api = cb_core_api();

	return api != NULL && api->version_major == GP_CORE_API_VERSION_MAJOR &&
		api->version_minor >= 10;
}

/*
 * The target list of a node that passes on its outer plan's rows, and the
 * scan target list it reads them through, as a Motion's are: each column of
 * the outer plan as OUTER_VAR, so that EXPLAIN finds it there.
 */
static void
pass_through_tlists(CustomScan *cscan, Plan *outer)
{
	ListCell   *lc;

	foreach(lc, outer->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *var = makeVarFromTargetEntry(OUTER_VAR, tle);
		TargetEntry *scan_tle = makeTargetEntry((Expr *) var, tle->resno,
												tle->resname, tle->resjunk);

		cscan->custom_scan_tlist = lappend(cscan->custom_scan_tlist, scan_tle);
		var = makeVarFromTargetEntry(INDEX_VAR, scan_tle);
		cscan->scan.plan.targetlist =
			lappend(cscan->scan.plan.targetlist,
					makeTargetEntry((Expr *) var, tle->resno, tle->resname,
									tle->resjunk));
	}
}

static void
copy_costs(Plan *to, Plan *from)
{
	to->startup_cost = from->startup_cost;
	to->total_cost = from->total_cost;
	to->plan_rows = from->plan_rows;
	to->plan_width = from->plan_width;
}

Plan *
gp_orca_make_sequence(Plan *plan, List *producers)
{
	CustomScan *cscan = makeNode(CustomScan);
	ListCell   *lc;

	cscan->methods = &gp_orca_sequence_methods;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.lefttree = plan;
	cscan->custom_plans = producers;
	pass_through_tlists(cscan, plan);
	copy_costs(&cscan->scan.plan, plan);

	/* the producers run first, and all of their rows are written */
	foreach(lc, producers)
	{
		Plan	   *producer = (Plan *) lfirst(lc);

		cscan->scan.plan.startup_cost += producer->total_cost;
		cscan->scan.plan.total_cost += producer->total_cost;
	}
	return (Plan *) cscan;
}

static CustomScan *
make_shared_scan(int share_id, int slice, bool producer)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->methods = &gp_orca_shared_scan_methods;
	cscan->scan.scanrelid = 0;
	cscan->custom_private = list_make4(makeInteger(share_id),
									   makeInteger(slice),
									   makeBoolean(producer),
									   makeBoolean(false));
	return cscan;
}

Plan *
gp_orca_make_share_producer(Plan *child, int share_id, int slice)
{
	CustomScan *cscan = make_shared_scan(share_id, slice, true);

	cscan->scan.plan.lefttree = child;
	pass_through_tlists(cscan, child);
	copy_costs(&cscan->scan.plan, child);
	return (Plan *) cscan;
}

Plan *
gp_orca_make_share_consumer(int share_id, int slice, List *scan_tlist,
							List *targetlist)
{
	CustomScan *cscan = make_shared_scan(share_id, slice, false);

	cscan->custom_scan_tlist = scan_tlist;
	cscan->scan.plan.targetlist = targetlist;
	return (Plan *) cscan;
}

void
gp_orca_set_share_across(Plan *plan)
{
	CustomScan *cscan = (CustomScan *) plan;

	Assert(IsA(plan, CustomScan) && cscan->methods == &gp_orca_shared_scan_methods);
	list_nth_cell(cscan->custom_private, SHARE_PRIVATE_ACROSS)->ptr_value =
		makeBoolean(true);
}

/* Is "plan" a producer whose CTE other slices read? */
static bool
share_read_across(Plan *plan)
{
	CustomScan *cscan = (CustomScan *) plan;

	return IsA(plan, CustomScan) &&
		cscan->methods == &gp_orca_shared_scan_methods &&
		list_length(cscan->custom_private) > SHARE_PRIVATE_ACROSS &&
		boolVal(list_nth(cscan->custom_private, SHARE_PRIVATE_ACROSS));
}

bool
gp_orca_is_shared_scan(Plan *plan, int *share_id, int *slice, bool *producer)
{
	CustomScan *cscan = (CustomScan *) plan;

	if (plan == NULL || !IsA(plan, CustomScan) ||
		cscan->methods != &gp_orca_shared_scan_methods)
		return false;
	*share_id = intVal(list_nth(cscan->custom_private, SHARE_PRIVATE_ID));
	*slice = intVal(list_nth(cscan->custom_private, SHARE_PRIVATE_SLICE));
	*producer = boolVal(list_nth(cscan->custom_private,
								 SHARE_PRIVATE_PRODUCER));
	return true;
}

/* ------------------------------------------------------------------------- */

static Node *
create_sequence_state(CustomScan *cscan)
{
	SequenceState *state = palloc0_object(SequenceState);

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &sequence_exec_methods;
	return (Node *) state;
}

static void
begin_sequence(CustomScanState *node, EState *estate, int eflags)
{
	SequenceState *state = (SequenceState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	ListCell   *lc;

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
	foreach(lc, cscan->custom_plans)
	{
		node->custom_ps = lappend(node->custom_ps,
								  ExecInitNode((Plan *) lfirst(lc), estate,
											   eflags));
		if (share_read_across((Plan *) lfirst(lc)))
			state->across = true;
	}
	state->described = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;

	/* the rows are the outer plan's, in whatever slots they come in */
	node->ss.ps.resultopsset = true;
	node->ss.ps.resultopsfixed = false;
}

static TupleTableSlot *
exec_sequence(CustomScanState *node)
{
	SequenceState *state = (SequenceState *) node;

	if (!state->produced)
	{
		ListCell   *lc;

		/* each producer writes every row of its child, and returns none */
		foreach(lc, node->custom_ps)
			(void) ExecProcNode((PlanState *) lfirst(lc));
		state->produced = true;
	}
	return ExecProcNode(outerPlanState(node));
}

/*
 * A run of the plan is over, and it never asked for a row: a producer whose
 * CTE other slices read runs all the same, so that their consumers do not
 * wait for it for ever -- see the file's header.  A run that ends with rows
 * still to fetch -- a FETCH's -- may produce before the plan would have: the
 * CTE reads nothing the plan sets.
 */
static void
shutdown_sequence(CustomScanState *node)
{
	SequenceState *state = (SequenceState *) node;
	ListCell   *lc;

	if (state->produced || !state->across || state->described)
		return;
	foreach(lc, node->custom_ps)
		(void) ExecProcNode((PlanState *) lfirst(lc));
	state->produced = true;
}

/*
 * EXPLAIN: the producers first and then the plan that reads them, as
 * Cloudberry's Sequence prints its subplans, in the order they run.  The
 * node reads its rows through its outer plan -- the plan that reads the
 * CTEs -- which EXPLAIN prints before a node's custom children; so once the
 * node's own lines are printed, which read through it, the first producer
 * takes the outer plan's place, and the outer plan goes after the others.
 * Only for the rest of the statement's EXPLAIN, which runs it no more; the
 * end of the node ends each of them as before.
 */
static void
explain_sequence(CustomScanState *node, List *ancestors, ExplainState *es)
{
	SequenceState *state = (SequenceState *) node;
	PlanState  *outer = outerPlanState(node);

	if (state->explained || outer == NULL || node->custom_ps == NIL)
		return;
	outerPlanState(node) = (PlanState *) linitial(node->custom_ps);
	node->custom_ps = lappend(list_delete_first(node->custom_ps), outer);
	state->explained = true;
}

static void
end_sequence(CustomScanState *node)
{
	ListCell   *lc;

	ExecEndNode(outerPlanState(node));
	foreach(lc, node->custom_ps)
		ExecEndNode((PlanState *) lfirst(lc));
}

/*
 * Again: the outer plan's rows.  The producers' were written once, and a
 * CTE depends on nothing a rescan changes -- ORCA refuses one with outer
 * references.
 */
static void
rescan_sequence(CustomScanState *node)
{
	ExecReScan(outerPlanState(node));
}

/* ------------------------------------------------------------------------- */

static void
share_names(int share_id, char *rows, char *ready)
{
	snprintf(rows, 32, "share%d", share_id);
	snprintf(ready, 32, "share%d.ready", share_id);
}

static bool
share_exists(FileSet *fileset, const char *name)
{
	BufFile    *file = BufFileOpenFileSet(fileset, name, O_RDONLY, true);

	if (file == NULL)
		return false;
	BufFileClose(file);
	return true;
}

static void
share_xact_callback(XactEvent event, void *arg)
{
	ListCell   *lc;

	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			foreach(lc, written_filesets)
				FileSetDeleteAll((FileSet *) lfirst(lc));
			/* FALLTHROUGH */
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			list_free_deep(written_filesets);
			written_filesets = NIL;
			break;
		default:
			break;
	}
}

static Node *
create_shared_scan_state(CustomScan *cscan)
{
	SharedScanState *state = palloc0_object(SharedScanState);

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &shared_scan_exec_methods;
	/* a consumer's rows are the producer's tuples, as they were written */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

static void
begin_shared_scan(CustomScanState *node, EState *estate, int eflags)
{
	SharedScanState *state = (SharedScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->share_id = intVal(list_nth(cscan->custom_private, SHARE_PRIVATE_ID));
	state->producer = boolVal(list_nth(cscan->custom_private,
									   SHARE_PRIVATE_PRODUCER));
	if (state->producer)
		outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);

	/*
	 * Only a segment runs one; the coordinator initialises it only for
	 * EXPLAIN, inside a Motion's fragment.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	if (!cb_core_api()->share_fileset(estate->es_plannedstmt, &state->fileset))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a CTE shared through files is read only in a slice a segment runs")));
}

/*
 * The producer: every row of its child written, then the mark that they are
 * all there.  Where the mark is there already, the same statement's slice
 * is being run again -- its Gather rescanned -- and the rows are the same.
 */
static void
share_produce(SharedScanState *state)
{
	PlanState  *child = outerPlanState(&state->css);
	EState	   *estate = state->css.ss.ps.state;
	char		rows[32];
	char		ready[32];
	BufFile    *file;
	MemoryContext oldcxt;

	share_names(state->share_id, rows, ready);
	if (share_exists(&state->fileset, ready))
		return;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	written_filesets = lappend(written_filesets,
							   memcpy(palloc(sizeof(FileSet)), &state->fileset,
									  sizeof(FileSet)));
	MemoryContextSwitchTo(estate->es_query_cxt);
	file = BufFileCreateFileSet(&state->fileset, rows);
	MemoryContextSwitchTo(oldcxt);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		MinimalTuple tuple;
		bool		shouldFree;

		if (TupIsNull(slot))
			break;
		tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);
		BufFileWrite(file, tuple, tuple->t_len);
		if (shouldFree)
			pfree(tuple);
	}
	BufFileClose(file);

	/* whole on disk before the mark says so */
	BufFileClose(BufFileCreateFileSet(&state->fileset, ready));
}

/*
 * A consumer: its producer's rows, once they are all there.  The producer
 * runs at the same time, in its own slice; this waits for it as a reader
 * waits for its writer's snapshot, and a cancel ends the wait.
 */
static void
share_open(SharedScanState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	char		rows[32];
	char		ready[32];
	long		delay = 1;
	MemoryContext oldcxt;

	share_names(state->share_id, rows, ready);
	while (!share_exists(&state->fileset, ready))
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		delay = Min(delay * 2, SHARE_WAIT_MAX_MS);
	}

	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	state->file = BufFileOpenFileSet(&state->fileset, rows, O_RDONLY, false);
	MemoryContextSwitchTo(oldcxt);
}

static TupleTableSlot *
shared_scan_next(ScanState *ss)
{
	SharedScanState *state = (SharedScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	uint32		len;
	MinimalTuple tuple;

	if (state->file == NULL)
		share_open(state);
	if (BufFileReadMaybeEOF(state->file, &len, sizeof(len), true) == 0)
		return ExecClearTuple(slot);
	tuple = (MinimalTuple) palloc(len);
	tuple->t_len = len;
	BufFileReadExact(state->file, (char *) tuple + sizeof(len),
					 len - sizeof(len));
	return ExecStoreMinimalTuple(tuple, slot, true);
}

static bool
shared_scan_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
exec_shared_scan(CustomScanState *node)
{
	SharedScanState *state = (SharedScanState *) node;

	if (state->producer)
	{
		share_produce(state);
		return NULL;
	}
	return ExecScan(&node->ss, shared_scan_next, shared_scan_recheck);
}

static void
end_shared_scan(CustomScanState *node)
{
	SharedScanState *state = (SharedScanState *) node;

	if (state->file != NULL)
		BufFileClose(state->file);
	state->file = NULL;
	if (outerPlanState(node) != NULL)
		ExecEndNode(outerPlanState(node));
}

static void
rescan_shared_scan(CustomScanState *node)
{
	SharedScanState *state = (SharedScanState *) node;

	if (state->file != NULL &&
		BufFileSeek(state->file, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to the start of a shared CTE's rows: %m")));
}

/* In text, the node's name says this, through gp_orca_label_shared_scans. */
static void
explain_shared_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	if (es->format == EXPLAIN_FORMAT_TEXT)
		return;
	ExplainPropertyInteger("Share ID", NULL,
						   intVal(list_nth(cscan->custom_private,
										   SHARE_PRIVATE_ID)), es);
	ExplainPropertyInteger("Slice", NULL,
						   intVal(list_nth(cscan->custom_private,
										   SHARE_PRIVATE_SLICE)), es);
	ExplainPropertyBool("Producer",
						boolVal(list_nth(cscan->custom_private,
										 SHARE_PRIVATE_PRODUCER)), es);
}

/* ------------------------------------------------------------------------- */

void
gp_orca_register_shared_scans(void)
{
	RegisterCustomScanMethods(&gp_orca_sequence_methods);
	RegisterCustomScanMethods(&gp_orca_shared_scan_methods);
	RegisterXactCallback(share_xact_callback, NULL);
}

bool
gp_orca_label_shared_scans(PlanState *planstate, ExplainState *es,
						   const char **pname, const char **suffix)
{
	CustomScan *cscan = (CustomScan *) planstate->plan;

	if (cscan->methods == &gp_orca_sequence_methods)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			*pname = "Sequence";
		return true;
	}
	if (cscan->methods == &gp_orca_shared_scan_methods)
	{
		/* github/cloudberry/src/backend/commands/explain.c, "share slice:id" */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			*pname = "Shared Scan";
			*suffix = psprintf(" (share slice:id %d:%d)",
							   intVal(list_nth(cscan->custom_private,
											   SHARE_PRIVATE_SLICE)),
							   intVal(list_nth(cscan->custom_private,
											   SHARE_PRIVATE_ID)));
		}
		return true;
	}
	return false;
}
