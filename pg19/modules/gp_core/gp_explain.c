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
 * gp_explain.c
 *	  Cloudberry's options of EXPLAIN: SLICETABLE, the plan's slices, and
 *	  LOCUS, where each node's rows are.
 *
 * PostgreSQL 19 lets a module register an option of EXPLAIN and print by it
 * (explain_state.c, as pg_overexplain does); Cloudberry's are its own
 * grammar's.  What they print is Cloudberry's, in its words:
 *
 *	 SLICETABLE		after the plan, one line a slice -- "Slice 1: Reader;
 *					root 0; parent 0; gang size 3" -- or a "Slice Table"
 *					group in the other formats (ExplainPrintSliceTable()).
 *					ORCA's plan carries its slice table (GP_SLICE_TABLE,
 *					orca/compat/cb_motion.h).  The planner's route has slice
 *					0, the coordinator's -- a Primary Writer where one of
 *					gp_core's writes sends its statement to the segments, as
 *					Cloudberry's root slice writes -- and a Reader a gather,
 *					each numbered as the executor met it (gp_scan.c).
 *
 *	 LOCUS			under each node, "Locus: Entry" where its rows are on
 *					the coordinator, "General" where they could be computed
 *					anywhere (Explainlocus()), with gp.optimizer off only, as
 *					Cloudberry prints none under ORCA.  The planner's route
 *					runs every node the coordinator shows on the
 *					coordinator: above a gather, over a table of its own, or
 *					anywhere at all -- a VALUES list, a function of no
 *					volatility -- so that Entry and General are all it has of
 *					Cloudberry's loci, which the segments' Motions make.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/commands/explain.c (the options, ExplainPrintSliceTable()
 *	  and Explainlocus())
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/guc.h"

#include "gp_cluster.h"
#include "gp_explain.h"
#include "gp_policy.h"
#include "gp_scan.h"

/* What the options asked for, on an ExplainState. */
typedef struct GpExplainOptions
{
	bool		slicetable;
	bool		locus;
	PlanState  *root;			/* the plan's top node, as the node hook met it */
} GpExplainOptions;

/*
 * Cloudberry's gp_enable_explain_allstat: EXPLAIN ANALYZE's statistics of
 * each segment's run of a node, where they come back.
 */
bool		gp_enable_explain_allstat = false;

static int	gp_explain_id = -1;
static explain_per_node_hook_type prev_explain_per_node = NULL;
static explain_per_plan_hook_type prev_explain_per_plan = NULL;

/* The slice table ORCA's translator keeps in the plan: orca/compat/cb_motion.h. */
#define GP_SLICE_TABLE	"gp_slice_table"

/* Cloudberry's gang types, as a slice table's entries number them. */
#define GANG_UNALLOCATED		0
#define GANG_ENTRYDB_READER		1
#define GANG_SINGLETON_READER	2
#define GANG_PRIMARY_READER		3
#define GANG_PRIMARY_WRITER		4

static GpExplainOptions *
explain_options(ExplainState *es)
{
	GpExplainOptions *o = GetExplainExtensionState(es, gp_explain_id);

	if (o == NULL)
	{
		o = palloc0_object(GpExplainOptions);
		SetExplainExtensionState(es, gp_explain_id, o);
	}
	return o;
}

static void
slicetable_handler(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	explain_options(es)->slicetable = defGetBoolean(opt);
}

static void
locus_handler(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	explain_options(es)->locus = defGetBoolean(opt);
}

/* ------------------------------------------------------------------------- */
/* LOCUS                                                                     */
/* ------------------------------------------------------------------------- */

/* Is this one of gp_core's writes, which send their statements to the segments? */
static bool
is_write_node(PlanState *ps)
{
	const char *name;

	if (!IsA(ps, CustomScanState))
		return false;
	name = ((CustomScanState *) ps)->methods->CustomName;
	return strcmp(name, "Redistribute Motion") == 0 ||	/* gp_modify.c */
		strcmp(name, "Dispatch") == 0 ||
		strcmp(name, "Explicit Redistribute Motion") == 0 ||	/* gp_explicit.c */
		strcmp(name, "GpSplitUpdate") == 0 ||	/* gp_split.c */
		strcmp(name, "GpSplitModify") == 0;
}

static bool locus_is_general(PlanState *ps);

static bool
all_general(PlanState **nodes, int n)
{
	for (int i = 0; i < n; i++)
		if (!locus_is_general(nodes[i]))
			return false;
	return true;
}

/*
 * Could this node's rows be computed anywhere -- Cloudberry's General locus
 * -- or are they the coordinator's, Entry?  A gather and a write are the
 * coordinator's, and so is a scan of a relation, which on this route is one
 * the coordinator keeps; a VALUES list, a function's rows and a Result over
 * nothing are General unless something in them is volatile or runs a
 * subplan; anything else is General if every node below it is.
 */
static bool
locus_is_general(PlanState *ps)
{
	Plan	   *plan = ps->plan;
	int			dummy;

	if (GpGatherScanSlice(ps, &dummy, &dummy) || is_write_node(ps))
		return false;
	if (contain_volatile_functions((Node *) plan->targetlist) ||
		contain_volatile_functions((Node *) plan->qual) ||
		ps->subPlan != NIL)
		return false;

	switch (nodeTag(plan))
	{
		case T_ValuesScan:
			return !contain_volatile_functions((Node *) ((ValuesScan *) plan)->values_lists) &&
				!contain_subplans((Node *) ((ValuesScan *) plan)->values_lists);
		case T_FunctionScan:
			return !contain_volatile_functions((Node *) ((FunctionScan *) plan)->functions) &&
				!contain_subplans((Node *) ((FunctionScan *) plan)->functions);
		case T_TableFuncScan:
			return !contain_volatile_functions((Node *) ((TableFuncScan *) plan)->tablefunc);
		case T_Result:
			return outerPlanState(ps) == NULL ||
				locus_is_general(outerPlanState(ps));
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_BitmapIndexScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_ForeignScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_NamedTuplestoreScan:
		case T_CustomScan:
		case T_ModifyTable:
			return false;
		case T_Append:
			return all_general(((AppendState *) ps)->appendplans,
							   ((AppendState *) ps)->as_nplans);
		case T_MergeAppend:
			return all_general(((MergeAppendState *) ps)->mergeplans,
							   ((MergeAppendState *) ps)->ms_nplans);
		case T_SubqueryScan:
			return locus_is_general(((SubqueryScanState *) ps)->subplan);
		default:
			return (outerPlanState(ps) == NULL ||
					locus_is_general(outerPlanState(ps))) &&
				(innerPlanState(ps) == NULL ||
				 locus_is_general(innerPlanState(ps)));
	}
}

static void
gp_explain_per_node(PlanState *planstate, List *ancestors,
					const char *relationship, const char *plan_name,
					ExplainState *es)
{
	GpExplainOptions *o = GetExplainExtensionState(es, gp_explain_id);

	if (prev_explain_per_node)
		prev_explain_per_node(planstate, ancestors, relationship, plan_name, es);
	if (o == NULL)
		return;

	/* the plan's top node: the first the hook meets, with no ancestors */
	if (ancestors == NIL && relationship == NULL && o->root == NULL)
		o->root = planstate;

	/* Cloudberry prints no locus under ORCA ("doesn't support Orca yet") */
	if (o->locus)
	{
		const char *optimizer = GetConfigOption("gp.optimizer", true, false);

		if (optimizer == NULL || strcmp(optimizer, "on") != 0)
			ExplainPropertyText("Locus",
								locus_is_general(planstate) ? "General" : "Entry",
								es);
	}
}

/* ------------------------------------------------------------------------- */
/* SLICETABLE                                                                */
/* ------------------------------------------------------------------------- */

typedef struct GpExplainSlice
{
	int			index;
	int			parent;
	int			gang;
	int			size;
	int			segment;		/* a singleton reader's */
} GpExplainSlice;

static const char *
gang_type_name(int gang)
{
	switch (gang)
	{
		case GANG_UNALLOCATED:
			return "Dispatcher";
		case GANG_ENTRYDB_READER:
			return "Entry DB Reader";
		case GANG_SINGLETON_READER:
			return "Singleton Reader";
		case GANG_PRIMARY_READER:
			return "Reader";
		case GANG_PRIMARY_WRITER:
			return "Primary Writer";
	}
	return "???";
}

/* A slice's root: its ancestor whose parent is none (Cloudberry's rootIndex). */
static int
slice_root(GpExplainSlice *slices, int n, int index)
{
	int			steps = 0;

	while (steps++ <= n)
	{
		int			parent = -1;

		for (int i = 0; i < n; i++)
			if (slices[i].index == index)
				parent = slices[i].parent;
		if (parent < 0)
			return index;
		index = parent;
	}
	return index;
}

/* ORCA's slice table, as its plan carries it. */
static int
orca_slices(PlannedStmt *stmt, GpExplainSlice **out)
{
	List	   *table = NIL;
	GpExplainSlice *slices;
	int			n = 0;

	foreach_node(DefElem, def, stmt->extension_state)
		if (strcmp(def->defname, GP_SLICE_TABLE) == 0)
			table = (List *) def->arg;
	if (table == NIL)
		return 0;

	slices = palloc0_array(GpExplainSlice, list_length(table));
	foreach_node(List, slice, table)
	{
		GpExplainSlice *s = &slices[n++];
		int			nsegs = intVal(list_nth(slice, 3));
		int			segindex = intVal(list_nth(slice, 4));
		int			direct = intVal(list_nth(slice, 5));
		List	   *several = list_length(slice) > 6 ? (List *) list_nth(slice, 6) : NIL;

		s->index = intVal(linitial(slice));
		s->parent = intVal(lsecond(slice));
		s->gang = intVal(list_nth(slice, 2));
		s->segment = s->gang == GANG_SINGLETON_READER ?
			(direct >= 0 ? direct : Max(segindex, 0)) : -1;
		/* Cloudberry's gang size is list_length(slice->segments) */
		if (s->gang == GANG_UNALLOCATED)
			s->size = 0;
		else if (s->gang == GANG_ENTRYDB_READER || s->gang == GANG_SINGLETON_READER ||
				 direct >= 0)
			s->size = 1;
		else if (several != NIL)
			s->size = list_length(several);
		else
			s->size = nsegs;
	}
	*out = slices;
	return n;
}

/* Every gather under ps, and whether a write of gp_core's is among its nodes. */
static void
collect_gathers(PlanState *ps, List **gathers, int *writer_nsegs)
{
	int			slice,
				nsegs;

	if (ps == NULL)
		return;
	if (GpGatherScanSlice(ps, &slice, &nsegs))
	{
		GpExplainSlice *s = palloc0_object(GpExplainSlice);

		s->index = slice;
		s->parent = 0;
		s->gang = GANG_PRIMARY_READER;
		s->size = nsegs;
		s->segment = -1;
		*gathers = lappend(*gathers, s);
	}
	else if (is_write_node(ps) && *writer_nsegs < 0)
	{
		EState	   *estate = ps->state;
		GpPolicy   *policy = NULL;

		int			rti = bms_next_member(estate->es_plannedstmt->resultRelationRelids, -1);

		if (rti > 0)
			policy = GpPolicyGet(rt_fetch(rti, estate->es_plannedstmt->rtable)->relid);
		*writer_nsegs = policy != NULL && !GpPolicyIsEntry(policy) ?
			policy->numsegments : GpClusterSegmentCount();
	}

	foreach_node(SubPlanState, sps, ps->initPlan)
		collect_gathers(sps->planstate, gathers, writer_nsegs);
	foreach_node(SubPlanState, sps, ps->subPlan)
		collect_gathers(sps->planstate, gathers, writer_nsegs);
	switch (nodeTag(ps))
	{
		case T_AppendState:
			for (int i = 0; i < ((AppendState *) ps)->as_nplans; i++)
				collect_gathers(((AppendState *) ps)->appendplans[i], gathers, writer_nsegs);
			break;
		case T_MergeAppendState:
			for (int i = 0; i < ((MergeAppendState *) ps)->ms_nplans; i++)
				collect_gathers(((MergeAppendState *) ps)->mergeplans[i], gathers, writer_nsegs);
			break;
		case T_SubqueryScanState:
			collect_gathers(((SubqueryScanState *) ps)->subplan, gathers, writer_nsegs);
			break;
		case T_CustomScanState:
			foreach_ptr(PlanState, child, ((CustomScanState *) ps)->custom_ps)
				collect_gathers(child, gathers, writer_nsegs);
			break;
		default:
			break;
	}
	collect_gathers(outerPlanState(ps), gathers, writer_nsegs);
	collect_gathers(innerPlanState(ps), gathers, writer_nsegs);
}

static int
slice_index_cmp(const ListCell *a, const ListCell *b)
{
	return ((GpExplainSlice *) lfirst(a))->index -
		((GpExplainSlice *) lfirst(b))->index;
}

/* The planner's route: slice 0, the coordinator's, and a slice a gather. */
static int
planner_slices(GpExplainOptions *o, GpExplainSlice **out)
{
	List	   *gathers = NIL;
	int			writer_nsegs = -1;
	GpExplainSlice *slices;
	int			n = 0;

	if (o->root != NULL)
	{
		collect_gathers(o->root, &gathers, &writer_nsegs);
		/* a CTE's plan, and a subplan the tree reaches only by its id */
		foreach_ptr(PlanState, sub, o->root->state->es_subplanstates)
		{
			List	   *more = NIL;

			collect_gathers(sub, &more, &writer_nsegs);
			foreach_ptr(GpExplainSlice, s, more)
			{
				bool		seen = false;

				foreach_ptr(GpExplainSlice, g, gathers)
					if (g->index == s->index)
						seen = true;
				if (!seen)
					gathers = lappend(gathers, s);
			}
		}
	}
	list_sort(gathers, slice_index_cmp);

	slices = palloc0_array(GpExplainSlice, list_length(gathers) + 1);
	slices[n].index = 0;
	slices[n].parent = -1;
	slices[n].gang = writer_nsegs >= 0 ? GANG_PRIMARY_WRITER : GANG_UNALLOCATED;
	slices[n].size = writer_nsegs >= 0 ? writer_nsegs : 0;
	slices[n].segment = -1;
	n++;
	foreach_ptr(GpExplainSlice, s, gathers)
		slices[n++] = *s;
	*out = slices;
	return n;
}

static void
print_slice_table(GpExplainSlice *slices, int n, ExplainState *es)
{
	ExplainOpenGroup("Slice Table", "Slice Table", false, es);
	for (int i = 0; i < n; i++)
	{
		GpExplainSlice *s = &slices[i];
		const char *gang = gang_type_name(s->gang);
		int			root = slice_root(slices, n, s->index);

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "Slice %d: %s; root %d; parent %d; gang size %d",
							 s->index, gang, root, s->parent, s->size);
			if (s->gang == GANG_SINGLETON_READER)
				appendStringInfo(es->str, "; segment %d", s->segment);
			appendStringInfoChar(es->str, '\n');
		}
		else
		{
			ExplainOpenGroup("Slice", NULL, true, es);
			ExplainPropertyInteger("Slice ID", NULL, s->index, es);
			ExplainPropertyText("Gang Type", gang, es);
			ExplainPropertyInteger("Root", NULL, root, es);
			ExplainPropertyInteger("Parent", NULL, s->parent, es);
			ExplainPropertyInteger("Gang Size", NULL, s->size, es);
			if (s->gang == GANG_SINGLETON_READER)
				ExplainPropertyInteger("Segment", NULL, s->segment, es);
			ExplainCloseGroup("Slice", NULL, true, es);
		}
	}
	ExplainCloseGroup("Slice Table", "Slice Table", false, es);
}

static void
gp_explain_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
					ExplainState *es, const char *queryString,
					ParamListInfo params, QueryEnvironment *queryEnv)
{
	GpExplainOptions *o = GetExplainExtensionState(es, gp_explain_id);

	if (o != NULL && o->slicetable)
	{
		GpExplainSlice *slices = NULL;
		int			n = orca_slices(plannedstmt, &slices);

		if (n == 0)
			n = planner_slices(o, &slices);
		print_slice_table(slices, n, es);
	}
	/* the next plan's -- a multi-statement EXPLAIN's -- root is its own */
	if (o != NULL)
		o->root = NULL;

	if (prev_explain_per_plan)
		prev_explain_per_plan(plannedstmt, into, es, queryString, params,
							  queryEnv);
}

void
GpExplainInit(void)
{
	gp_explain_id = GetExplainExtensionId("gp_core");
	RegisterExtensionExplainOption("slicetable", slicetable_handler,
								   GUCCheckBooleanExplainOption);
	RegisterExtensionExplainOption("locus", locus_handler,
								   GUCCheckBooleanExplainOption);

	/*
	 * Cloudberry's is GUC_NO_SHOW_ALL too, which here would hide it from
	 * pg_settings, where the test harnesses find the names they respell.
	 */
	DefineCustomBoolVariable("gp.enable_explain_allstat",
							 "Experimental feature: dump stats for all segments in EXPLAIN ANALYZE.",
							 "Cloudberry calls this gp_enable_explain_allstat.",
							 &gp_enable_explain_allstat,
							 false, PGC_USERSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	prev_explain_per_node = explain_per_node_hook;
	explain_per_node_hook = gp_explain_per_node;
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = gp_explain_per_plan;
}
