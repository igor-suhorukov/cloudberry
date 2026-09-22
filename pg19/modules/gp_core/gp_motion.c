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
 * gp_motion.c
 *	  ORCA's Gather Motion: a plan fragment carried out on the segments.
 *
 * Cloudberry's Motion is a node of its executor, and a slice below one is a
 * plan the dispatcher serializes and every segment runs, sending its rows up
 * through the interconnect.  Here the Motion is a CustomScan, and the slice
 * below it -- its outer plan, as ORCA built it -- travels as a PlannedStmt of
 * its own: the whole statement's range table, subplans and parameter types,
 * with the fragment as its tree.  Each segment receives it over the
 * dispatcher's ordinary libpq connection as
 *
 *     SELECT gp_internal.exec_fragment('<the PlannedStmt, as nodeToString>')
 *
 * behind a binary cursor, as every gather is (gp_dispatch.c); the segment's
 * planner_hook recognises the call, and instead of planning a function call
 * returns the fragment, which the cursor then runs.  The rows come back as
 * any gather's do.  That is stage A of the plan's distributed layer: plans
 * whose Motions all gather to the coordinator.  A Motion between segments
 * needs the interconnect, and is stage B.
 *
 * A plan is carried out as it stands -- its permission checks are part of it
 * -- so a segment takes one only from the coordinator: a connection that is
 * dispatched and carries the cluster secret (gp_cluster.c).  Without a
 * secret, ORCA is told plans cannot be dispatched, and its plans with a
 * Motion fall back to the planner, whose gathers send SQL.
 *
 * On the coordinator the fragment is never run.  It is initialised only for
 * EXPLAIN, so that EXPLAIN prints it below the Motion as Cloudberry prints a
 * slice; its Vars are read through the Motion's custom_scan_tlist, which
 * names the fragment's columns as OUTER_VAR, so ruleutils finds them in the
 * outer plan as it would under a Motion.
 *
 * A sorted Motion -- Cloudberry's merge-receive -- merges the segments'
 * streams, each already in order, with a binary heap as MergeAppend does.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_motion.h"
#include "gp_policy.h"

/*
 * custom_private, in order: the segment it reads from (-1 every one), the
 * slice it receives, and for a sorted Motion the sort keys -- columns of the
 * fragment's output, their ordering operators, collations and NULLS FIRST.
 */
#define MOTION_PRIVATE_CONTENT		0
#define MOTION_PRIVATE_SLICE		1
#define MOTION_PRIVATE_KEYS			2
#define MOTION_PRIVATE_SORTOPS		3
#define MOTION_PRIVATE_COLLATIONS	4
#define MOTION_PRIVATE_NULLSFIRST	5

typedef struct MotionState
{
	CustomScanState css;

	int			content;		/* -1: every segment */
	int			slice;
	int			nkeys;			/* 0: not sorted */
	AttrNumber *keys;
	SortSupport sortkeys;

	GpGatherState *gather;
	bool		done;

	/* A merge: each segment's next row, and which of them is least. */
	int			nsegs;
	TupleTableSlot **segslots;
	TupleTableSlot *receive;	/* what a row is received into first */
	binaryheap *heap;
	bool		merging;
	int			last;			/* the segment whose row went out last */
} MotionState;

static Node *motion_create_state(CustomScan *cscan);
static void motion_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *motion_exec(CustomScanState *node);
static void motion_end(CustomScanState *node);
static void motion_rescan(CustomScanState *node);
static void motion_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomScanMethods motion_scan_methods = {
	.CustomName = GP_MOTION_NAME,
	.CreateCustomScanState = motion_create_state,
};

static const CustomExecMethods motion_exec_methods = {
	.CustomName = GP_MOTION_NAME,
	.BeginCustomScan = motion_begin,
	.ExecCustomScan = motion_exec,
	.EndCustomScan = motion_end,
	.ReScanCustomScan = motion_rescan,
	.ExplainCustomScan = motion_explain,
};

static planner_hook_type prev_planner = NULL;
static explain_node_label_hook_type prev_explain_node_label = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;

/* How a fragment's PlannedStmt says it is one, on the segment that runs it. */
#define GP_FRAGMENT_MARK	"gp_fragment"

/* How many fragments this segment process is running, one inside another. */
static int	fragment_depth = 0;

/* ------------------------------------------------------------------------- */
/* Building one, for ORCA's translator                                       */
/* ------------------------------------------------------------------------- */

/* The Motion's own expressions read its scan tuple, not an outer plan. */
static Node *
outer_to_index_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *var = (Var *) copyObject(node);

		var->varno = INDEX_VAR;
		return (Node *) var;
	}
	return expression_tree_mutator(node, outer_to_index_mutator, context);
}

/* The oid of gp_internal.exec_fragment(text), or InvalidOid. */
static Oid
exec_fragment_oid(void)
{
	Oid			argtype = TEXTOID;

	return LookupFuncName(list_make2(makeString("gp_internal"),
									 makeString("exec_fragment")),
						  1, &argtype, true);
}

bool
GpMotionCanDispatchPlans(void)
{
	return GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		GpClusterHasSecret() &&
		OidIsValid(exec_fragment_oid());
}

Plan *
GpMotionMakeGather(Plan *fragment, List *targetlist, List *qual,
				   int content, int slice, int nkeys,
				   const AttrNumber *keys, const Oid *sortops,
				   const Oid *collations, const bool *nullsfirst)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *scan_tlist = NIL;
	List	   *keylist = NIL;
	List	   *oplist = NIL;
	List	   *colllist = NIL;
	List	   *nflist = NIL;
	List	   *tlist;
	ListCell   *lc;

	foreach(lc, fragment->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *var = makeVar(OUTER_VAR, tle->resno,
								  exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) var,
											 list_length(scan_tlist) + 1,
											 tle->resname, false));
	}

	tlist = (List *) outer_to_index_mutator((Node *) targetlist, NULL);

	/*
	 * A sort key is a column of the Motion's output; the merge compares the
	 * fragment's rows, so each has to be a column of those, passed through.
	 */
	for (int i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = get_tle_by_resno(tlist, keys[i]);

		if (tle == NULL || !IsA(tle->expr, Var) ||
			((Var *) tle->expr)->varno != INDEX_VAR)
			return NULL;
		keylist = lappend_int(keylist, ((Var *) tle->expr)->varattno);
		oplist = lappend_oid(oplist, sortops[i]);
		colllist = lappend_oid(colllist, collations[i]);
		nflist = lappend_int(nflist, nullsfirst[i] ? 1 : 0);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = (List *) outer_to_index_mutator((Node *) qual, NULL);
	cscan->scan.plan.lefttree = fragment;
	cscan->scan.scanrelid = 0;
	cscan->flags = 0;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_relids = NULL;
	cscan->custom_private = list_make4(makeInteger(content),
									   makeInteger(slice),
									   keylist, oplist);
	cscan->custom_private = lappend(cscan->custom_private, colllist);
	cscan->custom_private = lappend(cscan->custom_private, nflist);
	cscan->methods = &motion_scan_methods;

	return (Plan *) cscan;
}

bool
GpMotionIs(Plan *plan)
{
	return plan != NULL && IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &motion_scan_methods;
}

void
GpMotionSetSegment(Plan *plan, int content)
{
	CustomScan *cscan = (CustomScan *) plan;

	Assert(GpMotionIs(plan));
	intVal(list_nth(cscan->custom_private, MOTION_PRIVATE_CONTENT)) = content;
}

int
GpMotionSegment(Plan *plan)
{
	Assert(GpMotionIs(plan));
	return intVal(list_nth(((CustomScan *) plan)->custom_private,
						   MOTION_PRIVATE_CONTENT));
}

/*
 * Direct dispatch, for ORCA: the segment that holds every row whose
 * distribution key is these values, or -1.  The values are the key's, in the
 * key's order, and each has to be of its column's own type -- a hash is a
 * property of the type, and 1::int8 and 1::int4 need not hash alike.
 */
int
GpMotionDirectDispatchSegment(Oid relid, int nvalues, const Oid *types,
							  const Datum *values, const bool *isnull)
{
	GpPolicy   *policy;
	Relation	rel;
	TupleDesc	tupdesc;
	Datum	   *rowvalues;
	bool	   *rownulls;
	int			segment = -1;

	policy = GpPolicyGet(relid);
	if (policy == NULL || !GpPolicyIsHashPartitioned(policy) ||
		policy->nattrs != nvalues)
		return -1;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);
	rowvalues = palloc0_array(Datum, tupdesc->natts);
	rownulls = palloc_array(bool, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
		rownulls[i] = true;

	for (int k = 0; k < nvalues; k++)
	{
		AttrNumber	attno = policy->attrs[k];

		if (TupleDescAttr(tupdesc, attno - 1)->atttypid != types[k])
			goto done;
		rowvalues[attno - 1] = values[k];
		rownulls[attno - 1] = isnull[k];
	}
	segment = GpHashSegment(GpHashMake(policy, tupdesc), rowvalues, rownulls);

done:
	relation_close(rel, AccessShareLock);
	return segment;
}

/* ------------------------------------------------------------------------- */
/* Carrying it out, on the coordinator                                       */
/* ------------------------------------------------------------------------- */

static Node *
motion_create_state(CustomScan *cscan)
{
	MotionState *state = (MotionState *) newNode(sizeof(MotionState),
												 T_CustomScanState);

	state->css.methods = &motion_exec_methods;
	return (Node *) state;
}

static void
motion_begin(CustomScanState *node, EState *estate, int eflags)
{
	MotionState *state = (MotionState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	List	   *keys = (List *) list_nth(priv, MOTION_PRIVATE_KEYS);
	List	   *sortops = (List *) list_nth(priv, MOTION_PRIVATE_SORTOPS);
	List	   *colls = (List *) list_nth(priv, MOTION_PRIVATE_COLLATIONS);
	List	   *nfs = (List *) list_nth(priv, MOTION_PRIVATE_NULLSFIRST);

	state->content = intVal(list_nth(priv, MOTION_PRIVATE_CONTENT));
	state->slice = intVal(list_nth(priv, MOTION_PRIVATE_SLICE));
	state->nkeys = list_length(keys);

	if (state->nkeys > 0)
	{
		state->keys = palloc_array(AttrNumber, state->nkeys);
		state->sortkeys = palloc0_array(SortSupportData, state->nkeys);
		for (int i = 0; i < state->nkeys; i++)
		{
			SortSupport sk = &state->sortkeys[i];

			state->keys[i] = (AttrNumber) list_nth_int(keys, i);
			sk->ssup_cxt = CurrentMemoryContext;
			sk->ssup_collation = list_nth_oid(colls, i);
			sk->ssup_nulls_first = list_nth_int(nfs, i) != 0;
			sk->ssup_attno = state->keys[i];
			sk->abbreviate = false;
			PrepareSortSupportFromOrderingOp(list_nth_oid(sortops, i), sk);
		}
	}

	/*
	 * The fragment is the segments' to run.  Here it is only described: for
	 * EXPLAIN, and for EXPLAIN ANALYZE, where it shows as never executed.
	 */
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) || estate->es_instrument)
		outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate,
											eflags | EXEC_FLAG_EXPLAIN_ONLY);
}

/* The fragment, as the PlannedStmt a segment is sent. */
static char *
fragment_statement(MotionState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	PlannedStmt *whole = estate->es_plannedstmt;
	PlannedStmt *frag = makeNode(PlannedStmt);

	memcpy(frag, whole, sizeof(PlannedStmt));
	frag->commandType = CMD_SELECT;
	frag->hasReturning = false;
	frag->hasModifyingCTE = false;
	frag->canSetTag = true;
	frag->planTree = outerPlan(state->css.ss.ps.plan);
	frag->resultRelationRelids = NULL;
	frag->rowMarks = NIL;
	frag->extension_state = NIL;
	frag->utilityStmt = NULL;

	return psprintf("SELECT gp_internal.exec_fragment(%s)",
					quote_literal_cstr(nodeToString(frag)));
}

static void
motion_start(MotionState *state)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
	state->gather = GpGatherStartOn(fragment_statement(state),
									slot->tts_tupleDescriptor,
									state->content);
	MemoryContextSwitchTo(oldcxt);
}

static void
motion_finish(MotionState *state)
{
	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
	state->done = true;
}

/* binaryheap is a max-heap; the least row has to come out first. */
static int32
motion_heap_compare(Datum a, Datum b, void *arg)
{
	MotionState *state = (MotionState *) arg;
	TupleTableSlot *sa = state->segslots[DatumGetInt32(a)];
	TupleTableSlot *sb = state->segslots[DatumGetInt32(b)];

	for (int i = 0; i < state->nkeys; i++)
	{
		SortSupport sk = &state->sortkeys[i];
		Datum		va,
					vb;
		bool		na,
					nb;
		int			cmp;

		va = slot_getattr(sa, sk->ssup_attno, &na);
		vb = slot_getattr(sb, sk->ssup_attno, &nb);
		cmp = ApplySortComparator(va, na, vb, nb, sk);
		if (cmp != 0)
			return -cmp;
	}
	return 0;
}

/* A segment's next row into its slot; false when it has none. */
static bool
merge_read(MotionState *state, int seg)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	MemoryContext oldcxt;
	bool		got;

	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	got = GpGatherNextFrom(state->gather, seg, state->receive);
	MemoryContextSwitchTo(oldcxt);

	if (got)
		ExecCopySlot(state->segslots[seg], state->receive);
	else
		ExecClearTuple(state->segslots[seg]);
	return got;
}

static TupleTableSlot *
motion_merge_next(MotionState *state)
{
	if (!state->merging)
	{
		TupleDesc	tupdesc = state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

		state->nsegs = GpGatherSegmentCount(state->gather);
		if (state->segslots == NULL)
		{
			state->segslots = palloc_array(TupleTableSlot *, state->nsegs);
			for (int i = 0; i < state->nsegs; i++)
				state->segslots[i] = MakeSingleTupleTableSlot(tupdesc,
															  &TTSOpsMinimalTuple);
			state->receive = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
			state->heap = binaryheap_allocate(state->nsegs, motion_heap_compare,
											  state);
		}
		binaryheap_reset(state->heap);
		for (int i = 0; i < state->nsegs; i++)
			if (merge_read(state, i))
				binaryheap_add_unordered(state->heap, Int32GetDatum(i));
		binaryheap_build(state->heap);
		state->merging = true;
		state->last = -1;
	}
	else if (state->last >= 0)
	{
		/* the row that went out last is gone; its segment's next takes its place */
		if (merge_read(state, state->last))
			binaryheap_replace_first(state->heap, Int32GetDatum(state->last));
		else
			(void) binaryheap_remove_first(state->heap);
	}

	if (binaryheap_empty(state->heap))
		return NULL;

	state->last = DatumGetInt32(binaryheap_first(state->heap));
	return state->segslots[state->last];
}

static TupleTableSlot *
motion_next(ScanState *ss)
{
	MotionState *state = (MotionState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	MemoryContext oldcxt;
	bool		got;

	if (state->done)
		return ExecClearTuple(slot);

	if (state->gather == NULL)
		motion_start(state);

	if (state->nkeys > 0)
	{
		TupleTableSlot *next = motion_merge_next(state);

		if (next != NULL)
			return next;
		motion_finish(state);
		return ExecClearTuple(slot);
	}

	/* The row's values live until ExecScan resets the per-tuple context. */
	oldcxt = MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	got = GpGatherNext(state->gather, slot, NULL);
	MemoryContextSwitchTo(oldcxt);

	if (got)
		return slot;

	motion_finish(state);
	return ExecClearTuple(slot);
}

static bool
motion_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
motion_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, motion_next, motion_recheck);
}

static void
motion_end(CustomScanState *node)
{
	MotionState *state = (MotionState *) node;

	motion_finish(state);
	if (state->segslots != NULL)
	{
		for (int i = 0; i < state->nsegs; i++)
			ExecDropSingleTupleTableSlot(state->segslots[i]);
		ExecDropSingleTupleTableSlot(state->receive);
	}
	if (outerPlanState(node) != NULL)
		ExecEndNode(outerPlanState(node));
}

/*
 * Read again: the segments run the fragment again.  ORCA puts a Materialize
 * above a Motion that would be rescanned, as Cloudberry's executor cannot
 * rescan one; this one can, at the price of the round trip.
 */
static void
motion_rescan(CustomScanState *node)
{
	MotionState *state = (MotionState *) node;

	motion_finish(state);
	state->done = false;
	state->merging = false;
}

static int
motion_segments(MotionState *state)
{
	return state->content >= 0 ? 1 : GpClusterSegmentCount();
}

static void
motion_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	MotionState *state = (MotionState *) node;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	TupleDesc	tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/* In text the node's name says what it is; other formats need saying. */
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyText("Motion Type", "Gather", es);
		ExplainPropertyInteger("Slice", NULL, state->slice, es);
		ExplainPropertyInteger("Senders", NULL, motion_segments(state), es);
	}

	if (state->nkeys == 0)
		return;

	/* Cloudberry's words for a sorted Motion's keys. */
	context = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1 || es->verbose;
	for (int i = 0; i < state->nkeys; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, state->keys[i] - 1);
		Var		   *var = makeVar(INDEX_VAR, state->keys[i], att->atttypid,
								  att->atttypmod, att->attcollation, 0);

		result = lappend(result,
						 deparse_expression((Node *) var, context,
											useprefix, true));
	}
	ExplainPropertyList("Merge Key", result, es);
}

/*
 * What EXPLAIN calls it, through O4: "Gather Motion 2:1  (slice1; segments:
 * 2)", as Cloudberry does -- how many segments send, to the one that
 * receives, which slice they are, and how many run it.
 */
static void
motion_explain_label(PlanState *planstate, ExplainState *es,
					 const char **pname, const char **suffix)
{
	if (IsA(planstate, CustomScanState) &&
		((CustomScanState *) planstate)->methods == &motion_exec_methods)
	{
		MotionState *state = (MotionState *) planstate;
		int			nsegs = motion_segments(state);

		*pname = psprintf("Gather Motion %d:1", nsegs);
		*suffix = psprintf("  (slice%d; segments: %d)", state->slice, nsegs);
		return;
	}

	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

/* ------------------------------------------------------------------------- */
/* Carrying it out, on a segment                                             */
/* ------------------------------------------------------------------------- */

/*
 * The fragment a query is, if it is one: SELECT gp_internal.exec_fragment()
 * of a string, and nothing else.
 */
static const char *
fragment_payload(Query *parse)
{
	TargetEntry *tle;
	FuncExpr   *func;
	Const	   *arg;
	char	   *name;

	if (parse->commandType != CMD_SELECT || parse->rtable != NIL ||
		list_length(parse->targetList) != 1 || parse->jointree == NULL ||
		parse->jointree->quals != NULL || parse->hasSubLinks ||
		parse->cteList != NIL || parse->setOperations != NULL)
		return NULL;

	tle = linitial_node(TargetEntry, parse->targetList);
	if (!IsA(tle->expr, FuncExpr))
		return NULL;
	func = (FuncExpr *) tle->expr;
	if (list_length(func->args) != 1 || !IsA(linitial(func->args), Const))
		return NULL;

	/* By name rather than a remembered oid: the extension may be made again. */
	name = get_func_name(func->funcid);
	if (name == NULL || strcmp(name, "exec_fragment") != 0 ||
		get_func_namespace(func->funcid) != get_namespace_oid("gp_internal", true))
		return NULL;

	arg = (Const *) linitial(func->args);
	if (arg->constisnull || arg->consttype != TEXTOID)
		return NULL;
	return TextDatumGetCString(arg->constvalue);
}

static PlannedStmt *
fragment_plan(const char *payload)
{
	PlannedStmt *stmt;
	Node	   *node;
	ListCell   *lc;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a plan is carried out only for the coordinator"),
				 errdetail("The connection does not carry this cluster's secret."),
				 errhint("Set \"gp.cluster_secret\" to the same value on every node.")));

	node = stringToNode(payload);
	if (!IsA(node, PlannedStmt))
		elog(ERROR, "a dispatched plan fragment is not a PlannedStmt");
	stmt = (PlannedStmt *) node;

	/*
	 * The coordinator's parser locked the relations the statement reads; here
	 * nothing has parsed them, and the executor expects them locked.
	 */
	foreach(lc, stmt->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_RELATION)
			LockRelationOid(rte->relid,
							rte->rellockmode != NoLock ? rte->rellockmode
							: AccessShareLock);
	}

	/*
	 * Every column the fragment produces is one the Motion receives: a
	 * resjunk column would be dropped by the portal's junk filter, and the
	 * rows would arrive a column short.
	 */
	foreach(lc, stmt->planTree->targetlist)
		lfirst_node(TargetEntry, lc)->resjunk = false;

	stmt->extension_state = list_make1(makeDefElem(pstrdup(GP_FRAGMENT_MARK),
												   NULL, -1));
	return stmt;
}

static bool
is_fragment(PlannedStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->extension_state)
		if (strcmp(lfirst_node(DefElem, lc)->defname, GP_FRAGMENT_MARK) == 0)
			return true;
	return false;
}

static void
motion_executor_run(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count)
{
	bool		fragment = is_fragment(queryDesc->plannedstmt);

	if (fragment)
		fragment_depth++;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		if (fragment)
			fragment_depth--;
	}
	PG_END_TRY();
}

/*
 * A query a function runs while a fragment is carried out on a segment.
 *
 * The fragment is one segment's share of the statement, and a query that a
 * function in it plans here would read this segment's share of a table as
 * if it were the table.  Cloudberry refuses such a query on a QE unless it
 * reads only catalogs and replicated tables, and only reads
 * (querytree_safe_for_qe(), executor/functions.c); so does the port, with
 * Cloudberry's words -- and replicated tables refused too, because a
 * segment does not know a table's distribution: the "gp" label that says it
 * is kept on the coordinator.  A statement the coordinator dispatched itself
 * is planned before any fragment runs, and is not affected.
 */
static bool
fragment_safe_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		if (query->commandType != CMD_SELECT || query->resultRelation > 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function cannot execute on a QE slice because it issues a non-SELECT statement")));

		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			Oid			nsp;

			if (rte->rtekind != RTE_RELATION)
				continue;
			nsp = get_rel_namespace(rte->relid);
			if (!IsCatalogNamespace(nsp) && !IsToastNamespace(nsp))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function cannot execute on a QE slice because it accesses relation \"%s.%s\"",
								quote_identifier(get_namespace_name(nsp)),
								quote_identifier(get_rel_name(rte->relid)))));
		}
		return query_tree_walker(query, fragment_safe_walker, context, 0);
	}
	return expression_tree_walker(node, fragment_safe_walker, context);
}

static PlannedStmt *
motion_planner(Query *parse, const char *query_string, int cursorOptions,
			   ParamListInfo boundParams, ExplainState *es)
{
	if (GpClusterIsDispatched())
	{
		const char *payload = fragment_payload(parse);

		if (payload != NULL)
			return fragment_plan(payload);
		if (fragment_depth > 0)
			(void) fragment_safe_walker((Node *) parse, NULL);
	}

	if (prev_planner)
		return prev_planner(parse, query_string, cursorOptions, boundParams,
							es);
	return standard_planner(parse, query_string, cursorOptions, boundParams,
							es);
}

PG_FUNCTION_INFO_V1(gp_exec_fragment);

/*
 * gp_internal.exec_fragment(text)
 *
 * Never called: on a segment, the planner hook above puts the fragment in
 * its place.  Reached, it is somebody calling it by hand.
 */
Datum
gp_exec_fragment(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("gp_internal.exec_fragment() is not a function to call"),
			 errdetail("It marks a plan fragment the coordinator sends a segment.")));
	PG_RETURN_VOID();
}

void
GpMotionInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&motion_scan_methods);

	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = motion_explain_label;

	prev_planner = planner_hook;
	planner_hook = motion_planner;

	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = motion_executor_run;
}
