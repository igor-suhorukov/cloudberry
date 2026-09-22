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
 * gp_scan.c
 *	  Reading a distributed table: the gather-all shape.
 *
 * A distributed table's rows are on the segments, and its copy on the
 * coordinator is empty.  When PostgreSQL's planner plans a query -- ORCA
 * declined it, or is off -- every scan of such a table becomes a Gather
 * Motion of the table from the segments: the plan the planner made runs on
 * the coordinator, over the rows gathered.  That is the gather-all fallback of
 * Route A in the plan (Track A §2.2, gpdb_hook.md): correct for every query
 * PostgreSQL can plan, and as slow as moving every row to one node is.  ORCA's
 * own plans, with Motions where it chooses, are the fast path, and this is
 * what a query gets when it is not on it.
 *
 * What it does not move: the columns the plan does not use, which are
 * fetched as NULLs; and the rows the plan would throw away, when the
 * condition that throws them away can be evaluated on a segment -- one of
 * this table's columns, constants, and nothing whose answer could differ
 * there: no parameter, no subquery, no function that is not immutable.  Such
 * a condition is sent as SQL, deparsed by PostgreSQL's own ruleutils.
 *
 * And the segments it does not ask: a hash-distributed table whose
 * conditions fix every column of its key to a constant has all the rows they
 * can match on one segment, which is the only one asked -- Cloudberry's
 * direct dispatch.  A replicated table's rows are all on every segment, so
 * one is asked, a different one per session.
 *
 * Cloudberry sources this file stands in for:
 *	  the Gather Motion over a scan that cdbllize.c and cdbpath.c put above a
 *	  distributed table, and cdbtargeteddispatch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_policy.h"
#include "gp_scan.h"

/* What starting a gather costs before its first row: a round trip per segment. */
#define GATHER_STARTUP_COST		1000.0
/* And per row, moving it: a send, a receive, and the conversion between. */
#define GATHER_ROW_COST			(10.0 * DEFAULT_CPU_TUPLE_COST)

static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static build_simple_rel_hook_type prev_build_simple_rel = NULL;

static Plan *gather_plan(PlannerInfo *root, RelOptInfo *rel,
						 CustomPath *best_path, List *tlist,
						 List *clauses, List *custom_plans);
static Node *gather_create_state(CustomScan *cscan);
static void gather_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *gather_exec(CustomScanState *node);
static void gather_end(CustomScanState *node);
static void gather_rescan(CustomScanState *node);
static void gather_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomPathMethods gather_path_methods = {
	.CustomName = "Gather Motion",
	.PlanCustomPath = gather_plan,
};

static const CustomScanMethods gather_scan_methods = {
	.CustomName = "Gather Motion",
	.CreateCustomScanState = gather_create_state,
};

static const CustomExecMethods gather_exec_methods = {
	.CustomName = "Gather Motion",
	.BeginCustomScan = gather_begin,
	.ExecCustomScan = gather_exec,
	.EndCustomScan = gather_end,
	.ReScanCustomScan = gather_rescan,
	.ExplainCustomScan = gather_explain,
};

typedef struct GatherScanState
{
	CustomScanState css;
	char	   *sql;
	int			content;		/* -1 every segment, else just this one */
	int			nsegments;
	GpGatherState *gather;
	bool		done;
} GatherScanState;

/* ------------------------------------------------------------------------- */
/* Which relations                                                           */
/* ------------------------------------------------------------------------- */

GpPolicy *
GpScanDistributedPolicy(Oid relid)
{
	GpPolicy   *policy;

	if (GpClusterIsSingleNode())
		return NULL;
	if (get_rel_relkind(relid) != RELKIND_RELATION &&
		get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE)
		return NULL;

	policy = GpPolicyGet(relid);
	if (policy == NULL || GpPolicyIsEntry(policy))
		return NULL;
	return policy;
}

/* ------------------------------------------------------------------------- */
/* What can be sent                                                          */
/* ------------------------------------------------------------------------- */

typedef struct ShippableContext
{
	Index		relid;
} ShippableContext;

static bool
shippable_walker(Node *node, ShippableContext *cxt)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				/* this table's own columns, and no system column */
				if (var->varno != cxt->relid || var->varlevelsup != 0 ||
					var->varattno <= 0)
					return true;
				return false;
			}
		case T_Param:
		case T_SubLink:
		case T_SubPlan:
		case T_AlternativeSubPlan:
		case T_Aggref:
		case T_WindowFunc:
		case T_GroupingFunc:
		case T_PlaceHolderVar:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
			return true;
		default:
			break;
	}

	return expression_tree_walker(node, shippable_walker, cxt);
}

/*
 * Can this condition be evaluated on a segment and mean the same there?
 * Nothing whose answer could differ between nodes: only this table's columns,
 * no parameter or subquery, and no function that is not immutable -- now()
 * is a different instant on each node.
 */
static bool
is_shippable(Expr *expr, Index relid)
{
	ShippableContext cxt = {.relid = relid};

	if (contain_mutable_functions((Node *) expr))
		return false;
	return !shippable_walker((Node *) expr, &cxt);
}

/*
 * The one segment that holds every row the conditions can match, or -1.
 *
 * A hash-distributed table whose key columns are each compared by equality
 * to a constant: the constants hash to one segment, and nothing on the others
 * can match.
 */
static int
direct_dispatch_segment(GpPolicy *policy, Relation rel, List *quals,
						Index relid)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum	   *values;
	bool	   *isnull;
	bool	   *found;
	GpHash	   *h;
	ListCell   *lc;

	if (!GpPolicyIsHashPartitioned(policy))
		return -1;

	values = palloc0_array(Datum, tupdesc->natts);
	isnull = palloc_array(bool, tupdesc->natts);
	found = palloc0_array(bool, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
		isnull[i] = true;

	foreach(lc, quals)
	{
		Expr	   *qual = (Expr *) lfirst(lc);
		OpExpr	   *op;
		Node	   *left;
		Node	   *right;
		Var		   *var;
		Const	   *con;

		if (!IsA(qual, OpExpr) || list_length(((OpExpr *) qual)->args) != 2)
			continue;
		op = (OpExpr *) qual;
		left = strip_implicit_coercions(linitial(op->args));
		right = strip_implicit_coercions(lsecond(op->args));

		if (IsA(left, Var) && IsA(right, Const))
		{
			var = (Var *) left;
			con = (Const *) right;
		}
		else if (IsA(right, Var) && IsA(left, Const))
		{
			var = (Var *) right;
			con = (Const *) left;
		}
		else
			continue;

		if (var->varno != relid || var->varattno <= 0 || con->constisnull)
			continue;

		for (int k = 0; k < policy->nattrs; k++)
		{
			Oid			opfamily = get_opclass_family(policy->opclasses[k]);

			if (policy->attrs[k] != var->varattno)
				continue;

			/*
			 * The comparison has to be the key's own equality, and the
			 * constant of the column's own type: the hash of a value is a
			 * property of its type, and 1::int8 and 1::int4 need not hash
			 * alike.
			 */
			if (get_op_opfamily_strategy(op->opno, opfamily) != HTEqualStrategyNumber)
				continue;
			if (con->consttype != TupleDescAttr(tupdesc, var->varattno - 1)->atttypid)
				continue;

			values[var->varattno - 1] = con->constvalue;
			isnull[var->varattno - 1] = false;
			found[var->varattno - 1] = true;
		}
	}

	for (int k = 0; k < policy->nattrs; k++)
		if (!found[policy->attrs[k] - 1])
			return -1;

	h = GpHashMake(policy, tupdesc);
	return GpHashSegment(h, values, isnull);
}

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The size of a distributed table.  The planner scales pg_class's reltuples
 * by the pages the table has now, and the coordinator's copy has none, so a
 * table ANALYZE has counted would be estimated at no rows at all.  What
 * ANALYZE wrote is the size across the segments (gp_analyze.c), and is taken
 * as it stands.  A table never analyzed keeps the planner's own guess.
 */
static void
gp_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	HeapTuple	tuple;
	Form_pg_class classForm;

	if (prev_build_simple_rel)
		prev_build_simple_rel(root, rel, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	if (!HeapTupleIsValid(tuple))
		return;
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	if (classForm->relpages > 0 && classForm->reltuples >= 0)
	{
		rel->pages = (BlockNumber) classForm->relpages;
		rel->tuples = classForm->reltuples;
		rel->allvisfrac = Min(1.0, (double) classForm->relallvisible /
							  classForm->relpages);
	}
	ReleaseSysCache(tuple);
}

static void
gp_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					RangeTblEntry *rte)
{
	CustomPath *cp;
	int			nsegs;
	GpPolicy   *policy;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	/* A parent whose children are scanned instead has no scan of its own. */
	if (rte->rtekind != RTE_RELATION || rte->inh)
		return;
	if (IS_DUMMY_REL(rel))
		return;
	if (get_rel_relkind(rte->relid) != RELKIND_RELATION)
		return;

	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL)
		return;

	/*
	 * Every other way of reading it reads the coordinator's empty copy, so
	 * this is the only path left: not the cheapest of several, the one that
	 * gives the right answer.
	 */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	GpClusterSegments(&nsegs);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rel->rows;
	cp->path.startup_cost = GATHER_STARTUP_COST;
	cp->path.total_cost = GATHER_STARTUP_COST +
		rel->rows * (GATHER_ROW_COST + cpu_tuple_cost);
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &gather_path_methods;

	add_path(rel, &cp->path);
}

static Plan *
gather_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation;
	TupleDesc	tupdesc;
	GpPolicy   *policy;
	List	   *pushed = NIL;
	List	   *local = NIL;
	List	   *dpcontext;
	Bitmapset  *needed = NULL;
	StringInfoData sql;
	ListCell   *lc;
	int			content = -1;
	bool		whole_row;

	relation = table_open(rte->relid, NoLock);
	tupdesc = RelationGetDescr(relation);
	policy = GpScanDistributedPolicy(rte->relid);

	/* Which conditions go to the segments, and which stay here. */
	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (!ri->pseudoconstant && is_shippable(ri->clause, rel->relid))
			pushed = lappend(pushed, ri->clause);
		else
			local = lappend(local, ri);
	}

	/* Which columns anything here reads. */
	pull_varattnos((Node *) tlist, rel->relid, &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, false), rel->relid,
				   &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, true), rel->relid,
				   &needed);
	whole_row = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, needed);

	dpcontext = deparse_context_for(RelationGetRelationName(relation),
									rte->relid);

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (i > 0)
			appendStringInfoString(&sql, ", ");
		if (att->attisdropped ||
			(!whole_row &&
			 !bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
							needed)))
			appendStringInfoString(&sql, "NULL");
		else
			appendStringInfoString(&sql, quote_identifier(NameStr(att->attname)));
	}
	appendStringInfo(&sql, " FROM ONLY %s",
					 GpDispatchRelationName(RelationGetRelid(relation)));

	foreach(lc, pushed)
	{
		Node	   *qual = copyObject(lfirst(lc));

		/* deparse_context_for() knows this relation as range table entry 1 */
		ChangeVarNodes(qual, rel->relid, 1, 0);
		/* ruleutils brackets an operator's operands itself */
		appendStringInfo(&sql, "%s%s",
						 lc == list_head(pushed) ? " WHERE " : " AND ",
						 deparse_expression(qual, dpcontext, false, true));
	}

	if (policy != NULL && GpPolicyIsReplicated(policy))
	{
		int			nsegs;

		/* Every segment has every row; spread the sessions over them. */
		GpClusterSegments(&nsegs);
		content = MyProcPid % nsegs;
	}
	else if (policy != NULL)
		content = direct_dispatch_segment(policy, relation, pushed, rel->relid);

	table_close(relation, NoLock);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(local, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_private = list_make2(makeString(sql.data),
									   makeInteger(content));
	cscan->methods = &gather_scan_methods;

	return &cscan->scan.plan;
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

static Node *
gather_create_state(CustomScan *cscan)
{
	GatherScanState *state = (GatherScanState *) newNode(sizeof(GatherScanState),
														 T_CustomScanState);

	state->css.methods = &gather_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	state->sql = strVal(linitial(cscan->custom_private));
	state->content = intVal(lsecond(cscan->custom_private));
	return (Node *) state;
}

static void
gather_begin(CustomScanState *node, EState *estate, int eflags)
{
	GatherScanState *state = (GatherScanState *) node;

	GpClusterSegments(&state->nsegments);
}

static TupleTableSlot *
gather_next(ScanState *ss)
{
	GatherScanState *state = (GatherScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	MemoryContext oldcxt;
	bool		got;

	if (state->done)
		return ExecClearTuple(slot);

	/*
	 * The row's values live in the per-tuple context, which ExecScan resets
	 * before asking for the next one; the query's own context would keep every
	 * row the scan ever read.
	 */
	oldcxt = MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);

	if (state->gather == NULL)
	{
		MemoryContextSwitchTo(oldcxt);
		state->gather = GpGatherStartOn(state->sql, slot->tts_tupleDescriptor,
										state->content);
		MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	}

	got = GpGatherNext(state->gather, slot, NULL);
	MemoryContextSwitchTo(oldcxt);

	if (got)
	{
		slot->tts_tableOid = RelationGetRelid(ss->ss_currentRelation);
		return slot;
	}

	GpGatherEnd(state->gather);
	state->gather = NULL;
	state->done = true;
	return ExecClearTuple(slot);
}

static bool
gather_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gather_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gather_next, gather_recheck);
}

static void
gather_end(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
}

static void
gather_rescan(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
	state->done = false;
}

static void
gather_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->content >= 0)
		ExplainPropertyInteger("Segment", NULL, state->content, es);
	else
		ExplainPropertyInteger("Segments", NULL, state->nsegments, es);
	if (es->verbose)
		ExplainPropertyText("Remote SQL", state->sql, es);
}

/*
 * What EXPLAIN calls the node, through O4.  Cloudberry prints its Gather
 * Motion over the scan as two lines, "Gather Motion 2:1  (slice1; segments:
 * 2)" and the scan beneath; the port's node is both, so it is one line that
 * says both -- the motion, the table it reads, and how many segments.
 */
static explain_node_label_hook_type prev_explain_node_label = NULL;

static void
gp_scan_explain_label(PlanState *planstate, ExplainState *es,
					  const char **pname, const char **suffix)
{
	CustomScanState *node = (CustomScanState *) planstate;

	if (IsA(planstate, CustomScanState) && node->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) node;
		int			nsegs = state->content >= 0 ? 1 : state->nsegments;

		*pname = psprintf("Gather Motion %d:1", nsegs);
		*suffix = psprintf("  (slice1; segments: %d)", nsegs);
		return;
	}

	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

void
GpScanInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&gather_scan_methods);

	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = gp_scan_explain_label;

	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gp_set_rel_pathlist;

	prev_build_simple_rel = build_simple_rel_hook;
	build_simple_rel_hook = gp_build_simple_rel;
}
