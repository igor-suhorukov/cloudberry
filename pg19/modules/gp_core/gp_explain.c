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
 *	  LOCUS, where each node's rows are; and what EXPLAIN ANALYZE says of
 *	  the parts of a plan the segments ran.
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
 * EXPLAIN ANALYZE of a plan the segments run parts of, as Cloudberry's
 * cdbexplain does it: each segment measures its part, says what it measured
 * as its part ends, and the coordinator prints it with its own figures.
 *
 *	 asking			an explained statement's fragments carry its options
 *					and its number (GP_EXPLAIN_MARK, gp_motion.c); what the
 *					planner's route sends -- a gather's cursor, a write's
 *					statement or its COPY -- is measured while
 *					gp.explain_instrument, which the coordinator sets for the
 *					statement and sends with the other settings, says so.
 *
 *	 measuring		the segment instruments its part as EXPLAIN ANALYZE
 *					would, each node's memory in a context of its own under
 *					explain_memory_verbosity = detail and its first start
 *					under gp.enable_explain_allstat -- a wrapper of the node's
 *					ExecProcNode -- and as its executor ends sends each node's
 *					figures and its process's, base64 of them in an INFO with
 *					a SQLSTATE of gp_core's.  Cloudberry's segment sends a
 *					message of its own ('Y'), which its patched libpq attaches
 *					to the result; an INFO reaches the coordinator whatever
 *					client_min_messages says, in the order it was sent, one
 *					for each part, inside the answer to the command whose end
 *					ended the part.
 *
 *	 hearing		the coordinator's notice filter takes it (gp_dispatch.c):
 *					a fragment's by its statement's number and each node's
 *					plan_node_id; a statement's as the node's that is waiting
 *					for it -- the gather closing its cursors, the write
 *					sending its rows.
 *
 *	 printing		once the plan has run (ExecutorFinish), and every gather
 *					and Motion a LIMIT left open has been ended so that all
 *					have spoken, the nodes of a fragment, which the
 *					coordinator only describes, take the figures of the
 *					segment that returned the most rows -- Cloudberry's
 *					"winner" -- and the WAL of all, so that "never executed"
 *					becomes the segments' "actual"; a write takes its
 *					statements' WAL.  Under each node, Cloudberry's "Executor
 *					Memory", "work_mem ... Workfile: (N spilling)" and
 *					"allstat" lines; after the plan, each slice's memory:
 *					"(slice1)    Executor memory: ...  Vmem reserved: ...".
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/commands/explain.c (the options, ExplainPrintSliceTable()
 *	  and Explainlocus()) and src/backend/commands/explain_gp.c
 *	  (cdbexplain_sendExecStats(), cdbexplain_depositStatsToNode(),
 *	  cdbexplain_showExecStats() and gpexplain_formatSlicesOutput())
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/xact.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/base64.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/instrument.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_explain.h"
#include "gp_metrics.h"
#include "gp_motion.h"
#include "gp_policy.h"
#include "gp_rtfilter.h"
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

static void print_node_statistics(PlanState *ps, ExplainState *es);

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

	if (o != NULL)
	{
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

	print_node_statistics(planstate, es);
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

/* ------------------------------------------------------------------------- */
/* EXPLAIN ANALYZE: what the segments did                                    */
/* ------------------------------------------------------------------------- */

/* A segment's figures for the coordinator, in an INFO of this SQLSTATE. */
#define ERRCODE_GP_EXPLAIN_STATS	MAKE_SQLSTATE('X','X','G','E','X')
#define GP_EXPLAIN_SQLSTATE		"XXGEX"

/* On an explained statement's fragment: (options, the statement's number). */
#define GP_EXPLAIN_MARK			"gp_explain"

/*
 * What the coordinator asks besides PostgreSQL's instrumentation options:
 * each node's memory, under explain_memory_verbosity = detail, and each
 * node's first start, for gp.enable_explain_allstat.
 */
#define GP_EXPLAIN_MEMORY		(1 << 20)
#define GP_EXPLAIN_ALLSTAT		(1 << 21)
#define GP_EXPLAIN_OWN_OPTIONS	(GP_EXPLAIN_MEMORY | GP_EXPLAIN_ALLSTAT)

/* explain_memory_verbosity, gp_resource's setting */
#define VERBOSITY_SUPPRESS		0
#define VERBOSITY_SUMMARY		1
#define VERBOSITY_DETAIL		2

/*
 * What a segment's figures are of: the nodes of a fragment, by the
 * plan_node_id the coordinator's plan has them by, or a statement the
 * planner's route sent, whose plan is the segment's own.
 */
#define GP_REPORT_FRAGMENT		1
#define GP_REPORT_STATEMENT		2

/* A node's, as a segment sends them: its runs, summed; its memory, at most. */
typedef struct GpReportNode
{
	int32		plan_node_id;
	bool		spilled;		/* its work_mem ran out, and it wrote to disk */
	bool		rtf_worked;		/* a runtime filter worked in it: Cloudberry's
								 * prf_work (gp_rtfilter.c) */
	double		ntuples;
	double		ntuples2;
	double		nloops;
	double		nfiltered1;
	double		nfiltered2;
	int64		startup_ns;
	int64		total_ns;
	TimestampTz firststart;		/* 0 where not asked for */
	int64		execmem;		/* its own context's bytes, 0 where not asked */
	int64		workmem;		/* bytes of work_mem a Sort, hash or Material used */
	int64		nsearches;		/* an index scan's searches */
	int32		nworkers;		/* the workers a Gather launched, at most */
	WalUsage	wal;
} GpReportNode;

typedef struct GpReportHeader
{
	int32		kind;			/* GP_REPORT_* */
	int32		serial;			/* a fragment's: the coordinator's statement */
	int32		content;
	int32		nnodes;			/* the GpReportNodes that follow */
	int64		execmem;		/* the executor's memory at the end */
	int64		vmem;			/* the most vmem the process reserved */
	WalUsage	wal;			/* all the part wrote */
} GpReportHeader;

/* A report as the notice filter keeps it, untouched, until it is read. */
typedef struct RawReport
{
	struct RawReport *next;
	PlanState  *node;			/* a statement's: the node it answers */
	int			len;
	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} RawReport;

/* One segment's figures for a node, all its reports taken together. */
typedef struct SegFigures
{
	bool		seen;
	GpReportNode node;
	int64		execmem;		/* a statement's: its executor's memory */
} SegFigures;

typedef struct NodeStats
{
	SegFigures *frag;			/* by content: the node, as the segments ran it */
	SegFigures *stmt;			/* by content: the statements the node sent */
} NodeStats;

typedef struct SliceStats
{
	int			slice;
	bool	   *seen;			/* by content */
	int64	   *execmem;
	int64	   *vmem;
	int64		workmem;		/* the most any of its nodes used */
} SliceStats;

/*
 * A statement being measured: on the coordinator one being explained, and
 * on a segment its part of one.
 */
typedef struct GpExplainQuery
{
	QueryDesc  *queryDesc;
	EState	   *estate;
	bool		coordinator;
	int			options;		/* instrumentation's, and GP_EXPLAIN_* */
	int			serial;
	int			kind;			/* a segment's: GP_REPORT_* */
	WalUsage	wal_start;		/* a segment's: pgWalUsage as it began */
	TimestampTz start;			/* the coordinator's: as it began */
	char	   *saved_instrument;	/* gp.explain_instrument as it was */

	/* by plan_node_id */
	int			nnodes;
	ExecProcNodeMtd *real;		/* each node's own ExecProcNode, wrapped */
	bool	   *answers;		/* the coordinator's: its statements report */
	MemoryContext *contexts;	/* each node's memory, where measured */
	TimestampTz *firststart;	/* each node's first start, where asked */

	/* the coordinator's: what the segments said, and what it made of it */
	RawReport  *raw;
	RawReport **raw_tail;
	bool		collected;
	NodeStats  *stats;
	int		   *slice_of;
	List	   *slices;			/* SliceStats */
	int64		execmem;
	int64		vmem;
} GpExplainQuery;

/* gp.explain_instrument: what the segments are asked to measure, or 0 */
static int	gp_explain_instrument = 0;

static List *queries = NIL;		/* GpExplainQuery, innermost first */
static int	explain_serial = 0;
static int	executor_depth = 0;	/* in an ExecutorRun or ExecutorFinish */
static GpExplainVmemReserved vmem_reserved = NULL;

/*
 * The coordinator's node whose segments' statements answer now, and its
 * statement: the node running -- a write sending its rows -- or a gather
 * closing its cursors.  Never dereferenced here but through the statement,
 * which is looked for among the live ones first.
 */
static GpExplainQuery *answer_query = NULL;
static PlanState *answer_node = NULL;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static int
memory_verbosity(void)
{
	const char *v = GetConfigOption("gp.explain_memory_verbosity", true, false);

	if (v == NULL || strcmp(v, "suppress") == 0)
		return VERBOSITY_SUPPRESS;
	if (strcmp(v, "summary") == 0)
		return VERBOSITY_SUMMARY;
	return VERBOSITY_DETAIL;
}

static GpExplainQuery *
query_of(EState *estate)
{
	foreach_ptr(GpExplainQuery, q, queries)
		if (q->estate == estate)
			return q;
	return NULL;
}

static bool
query_live(GpExplainQuery *query)
{
	foreach_ptr(GpExplainQuery, q, queries)
		if (q == query)
			return true;
	return false;
}

/* As the statement's executor memory goes, at its end or after an error. */
static void
query_forget(void *arg)
{
	GpExplainQuery *q = (GpExplainQuery *) arg;

	queries = list_delete_ptr(queries, q);
	while (q->raw != NULL)
	{
		RawReport  *r = q->raw;

		q->raw = r->next;
		free(r);
	}
	if (answer_query == q)
	{
		answer_query = NULL;
		answer_node = NULL;
	}
}

/* The largest plan_node_id below ps, subplans' included. */
static bool
max_node_id_walker(PlanState *ps, int *max)
{
	*max = Max(*max, ps->plan->plan_node_id);
	return planstate_tree_walker(ps, max_node_id_walker, max);
}

static GpExplainQuery *
query_begin(QueryDesc *queryDesc, bool coordinator, int options)
{
	EState	   *estate = queryDesc->estate;
	GpExplainQuery *q = palloc0_object(GpExplainQuery);
	MemoryContextCallback *cb = palloc0_object(MemoryContextCallback);
	int			max = 0;

	q->queryDesc = queryDesc;
	q->estate = estate;
	q->coordinator = coordinator;
	q->options = options;
	q->raw_tail = &q->raw;

	(void) max_node_id_walker(queryDesc->planstate, &max);
	foreach_ptr(PlanState, sub, estate->es_subplanstates)
		if (sub != NULL)
			(void) max_node_id_walker(sub, &max);
	q->nnodes = max + 1;

	cb->func = query_forget;
	cb->arg = q;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		queries = lcons(q, queries);
		MemoryContextSwitchTo(oldcxt);
	}
	return q;
}

/* ------------------------------------------------------------------------- */
/* Each node's run: its memory, its first start, and who is answering       */
/* ------------------------------------------------------------------------- */

/*
 * A node's ExecProcNode, wrapped: on the coordinator the node the segments'
 * statements answer while it runs; a context of its own that what it
 * allocates as it runs goes to, as Cloudberry's node_context -- a child of
 * the executor's, gone with it -- where memory is measured; and the time it
 * first ran, for allstat.  What a node allocates as it is initialized is
 * not counted: no hook reaches ExecInitNode().
 */
static TupleTableSlot *
explain_exec_node(PlanState *ps)
{
	GpExplainQuery *q = query_of(ps->state);
	int			id = ps->plan->plan_node_id;
	GpExplainQuery *save_query = answer_query;
	PlanState  *save_node = answer_node;
	MemoryContext oldcxt = NULL;
	TupleTableSlot *slot;

	Assert(q != NULL && id >= 0 && id < q->nnodes && q->real[id] != NULL);

	if (q->firststart != NULL && q->firststart[id] == 0)
		q->firststart[id] = GetCurrentTimestamp();
	if (q->contexts != NULL)
	{
		if (q->contexts[id] == NULL)
			q->contexts[id] = AllocSetContextCreate(q->estate->es_query_cxt,
													"gp_explain node",
													ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(q->contexts[id]);
	}
	if (q->coordinator)
	{
		answer_query = q->answers[id] ? q : NULL;
		answer_node = q->answers[id] ? ps : NULL;
	}

	slot = q->real[id] (ps);

	answer_query = save_query;
	answer_node = save_node;
	if (oldcxt != NULL)
		MemoryContextSwitchTo(oldcxt);
	return slot;
}

/*
 * Every node wrapped; on the coordinator, the gathers and gp_core's writes
 * are the nodes whose statements' reports are taken while they run.
 */
static bool
wrap_walker(PlanState *ps, GpExplainQuery *q)
{
	int			id = ps->plan->plan_node_id;
	int			slice;
	int			nsegs;

	if (id >= 0 && id < q->nnodes && q->real[id] == NULL &&
		ps->ExecProcNodeReal != NULL)
	{
		q->real[id] = ps->ExecProcNodeReal;
		ExecSetExecProcNode(ps, explain_exec_node);
		if (q->coordinator)
			q->answers[id] = is_write_node(ps) ||
				GpGatherScanSlice(ps, &slice, &nsegs);
	}
	return planstate_tree_walker(ps, wrap_walker, q);
}

static void
wrap_nodes(GpExplainQuery *q)
{
	q->real = palloc0_array(ExecProcNodeMtd, q->nnodes);
	q->answers = palloc0_array(bool, q->nnodes);
	if (q->options & GP_EXPLAIN_MEMORY)
		q->contexts = palloc0_array(MemoryContext, q->nnodes);
	if (q->options & GP_EXPLAIN_ALLSTAT)
		q->firststart = palloc0_array(TimestampTz, q->nnodes);

	(void) wrap_walker(q->queryDesc->planstate, q);
	foreach_ptr(PlanState, sub, q->estate->es_subplanstates)
		if (sub != NULL)
			(void) wrap_walker(sub, q);
}

PlanState *
GpExplainAnswerFor(PlanState *node)
{
	PlanState  *prev = answer_node;

	answer_query = node != NULL ? query_of(node->state) : NULL;
	answer_node = answer_query != NULL ? node : NULL;
	return prev;
}

/* ------------------------------------------------------------------------- */
/* On a segment: measuring, and saying what was measured                     */
/* ------------------------------------------------------------------------- */

/*
 * What the coordinator asks this statement to be measured with: an
 * explained statement's fragment by its mark, and any other statement it
 * sends by the setting -- a gather's cursor, a write's statement, whose
 * reports the coordinator takes, and whatever else it sends meanwhile,
 * whose it drops.  A fragment without the mark is of a statement nobody
 * explains, whatever the setting says.
 */
static int
asked_options(QueryDesc *queryDesc, int *kind, int *serial)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;

	if (GpMotionIsFragment(stmt))
	{
		foreach_node(DefElem, def, stmt->extension_state)
		{
			if (strcmp(def->defname, GP_EXPLAIN_MARK) == 0)
			{
				*kind = GP_REPORT_FRAGMENT;
				*serial = intVal(lsecond((List *) def->arg));
				return intVal(linitial((List *) def->arg));
			}
		}
		return 0;
	}
	if (gp_explain_instrument != 0)
	{
		*kind = GP_REPORT_STATEMENT;
		*serial = 0;
		return gp_explain_instrument;
	}
	return 0;
}

/*
 * The work_mem a Sort, a hash join, a hashed Agg or a Material used, and
 * whether it spilled -- the four Cloudberry says so of
 * (nodeSupportWorkfileCaching()) -- as PostgreSQL 19's own EXPLAIN reads
 * them.  A sort that spilled says only the disk it wrote, which is what it
 * gives, where Cloudberry's gives its memory's peak.
 */
static void
node_work_mem(PlanState *ps, GpReportNode *n)
{
	switch (nodeTag(ps))
	{
		case T_SortState:
			{
				SortState  *sort = (SortState *) ps;
				TuplesortInstrumentation stats;

				if (!sort->sort_Done || sort->tuplesortstate == NULL)
					break;
				tuplesort_get_stats((Tuplesortstate *) sort->tuplesortstate, &stats);
				n->workmem = stats.spaceUsed * 1024;
				n->spilled = stats.spaceType == SORT_SPACE_TYPE_DISK;
				break;
			}
		case T_MaterialState:
			{
				MaterialState *mat = (MaterialState *) ps;
				char	   *type;
				int64		space;

				if (mat->tuplestorestate == NULL)
					break;
				tuplestore_get_stats(mat->tuplestorestate, &type, &space);
				n->workmem = space;
				n->spilled = strcmp(type, "Disk") == 0;
				break;
			}
		case T_HashJoinState:
			{
				HashJoinTable table = ((HashJoinState *) ps)->hj_HashTable;
				HashState  *hash = (HashState *) innerPlanState(ps);

				if (table != NULL)
				{
					n->workmem = table->spacePeak;
					n->spilled = table->nbatch > 1;
				}
				else if (hash != NULL && hash->hinstrument != NULL)
				{
					n->workmem = hash->hinstrument->space_peak;
					n->spilled = hash->hinstrument->nbatch > 1;
				}
				break;
			}
		case T_AggState:
			if (((Agg *) ps->plan)->aggstrategy == AGG_HASHED)
			{
				n->workmem = ((AggState *) ps)->hash_mem_peak;
				n->spilled = ((AggState *) ps)->hash_disk_used > 0;
			}
			break;
		default:
			break;
	}
}

static uint64
node_searches(PlanState *ps)
{
	IndexScanInstrumentation *instr = NULL;

	switch (nodeTag(ps))
	{
		case T_IndexScanState:
			instr = ((IndexScanState *) ps)->iss_Instrument;
			break;
		case T_IndexOnlyScanState:
			instr = ((IndexOnlyScanState *) ps)->ioss_Instrument;
			break;
		case T_BitmapIndexScanState:
			instr = ((BitmapIndexScanState *) ps)->biss_Instrument;
			break;
		default:
			break;
	}
	return instr != NULL ? instr->nsearches : 0;
}

/* A node's figures, its executor not ended yet: InstrEndLoop() first. */
static void
node_figures(PlanState *ps, GpExplainQuery *q, GpReportNode *n)
{
	NodeInstrumentation *instr = ps->instrument;
	int			id = ps->plan->plan_node_id;

	InstrEndLoop(instr);
	memset(n, 0, sizeof(GpReportNode));
	n->plan_node_id = id;
	n->ntuples = instr->ntuples;
	n->ntuples2 = instr->ntuples2;
	n->nloops = instr->nloops;
	n->nfiltered1 = instr->nfiltered1;
	n->nfiltered2 = instr->nfiltered2;
	n->rtf_worked = GpRtFilterWorked(ps);
	n->startup_ns = INSTR_TIME_GET_NANOSEC(instr->startup);
	n->total_ns = INSTR_TIME_GET_NANOSEC(instr->instr.total);
	n->wal = instr->instr.walusage;
	n->nsearches = node_searches(ps);
	if (IsA(ps, GatherState))
		n->nworkers = ((GatherState *) ps)->nworkers_launched;
	else if (IsA(ps, GatherMergeState))
		n->nworkers = ((GatherMergeState *) ps)->nworkers_launched;
	if (q->firststart != NULL && id >= 0 && id < q->nnodes)
		n->firststart = q->firststart[id];
	if (q->contexts != NULL && id >= 0 && id < q->nnodes && q->contexts[id] != NULL)
		n->execmem = MemoryContextMemAllocated(q->contexts[id], true);
	node_work_mem(ps, n);
}

typedef struct ReportWalk
{
	GpExplainQuery *q;
	StringInfo	buf;
	int			nnodes;
} ReportWalk;

/*
 * Each node of the fragment that ran, subplans it ran included; not a
 * reader's Motion that sends, whose rows the Motion that receives them
 * counts, in the fragment of the slice above, as its own.
 */
static bool
report_walker(PlanState *ps, ReportWalk *w)
{
	if (ps->instrument != NULL && !GpMotionIsSender(ps))
	{
		GpReportNode n;

		node_figures(ps, w->q, &n);
		if (n.nloops > 0)
		{
			appendBinaryStringInfo(w->buf, &n, sizeof(n));
			w->nnodes++;
		}
	}
	return planstate_tree_walker(ps, report_walker, w);
}

static void
send_report(GpReportHeader *hdr, StringInfo buf)
{
	int			len;
	char	   *text;

	hdr->content = GpClusterContentId();
	hdr->vmem = vmem_reserved != NULL ? vmem_reserved() : 0;
	memcpy(buf->data, hdr, sizeof(GpReportHeader));

	len = pg_b64_enc_len(buf->len);
	text = palloc(len + 1);
	len = pg_b64_encode((uint8 *) buf->data, buf->len, text, len);
	if (len < 0)
		elog(ERROR, "could not encode EXPLAIN ANALYZE's statistics");
	text[len] = '\0';

	ereport(INFO,
			(errcode(ERRCODE_GP_EXPLAIN_STATS),
			 errmsg_internal("%s", text),
			 errhidestmt(true),
			 errhidecontext(true)));
	pfree(text);
}

/*
 * As a segment's part ends, before its executor does: a fragment's every
 * node that ran, or a statement's top node alone, whose plan is the
 * segment's own; its executor's memory, its process's vmem, and all it
 * wrote to the WAL.
 */
static void
segment_report(GpExplainQuery *q)
{
	GpReportHeader hdr;
	StringInfoData buf;
	ReportWalk	w;

	memset(&hdr, 0, sizeof(hdr));
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, &hdr, sizeof(hdr));

	w.q = q;
	w.buf = &buf;
	w.nnodes = 0;
	if (q->kind == GP_REPORT_FRAGMENT)
		(void) report_walker(q->queryDesc->planstate, &w);
	else if (q->queryDesc->planstate->instrument != NULL)
	{
		GpReportNode n;

		node_figures(q->queryDesc->planstate, q, &n);
		appendBinaryStringInfo(&buf, &n, sizeof(n));
		w.nnodes = 1;
	}

	hdr.kind = q->kind;
	hdr.serial = q->serial;
	hdr.nnodes = w.nnodes;
	hdr.execmem = MemoryContextMemAllocated(q->estate->es_query_cxt, true);
	WalUsageAccumDiff(&hdr.wal, &pgWalUsage, &q->wal_start);
	send_report(&hdr, &buf);
	pfree(buf.data);
}

/* ------------------------------------------------------------------------- */
/* On the coordinator: hearing, and taking what was heard                    */
/* ------------------------------------------------------------------------- */

/*
 * gp_dispatch.c's filter of what the segments say, in libpq's callback: a
 * report is taken, and kept as it came -- nothing may be raised or palloc'd
 * here -- for the statement it is of, or dropped where that is none being
 * explained.  It is never the client's.
 */
static bool
explain_notice_filter(const char *sqlstate, const char *message)
{
	int			len;
	int			declen;
	RawReport  *r;
	GpReportHeader hdr;
	GpExplainQuery *target = NULL;

	if (sqlstate == NULL || strcmp(sqlstate, GP_EXPLAIN_SQLSTATE) != 0)
		return false;

	len = strlen(message);
	declen = pg_b64_dec_len(len);
	r = malloc(offsetof(RawReport, data) + declen);
	if (r == NULL)
		return true;
	r->next = NULL;
	r->node = NULL;
	r->len = pg_b64_decode(message, len, r->data, declen);
	if (r->len < (int) sizeof(GpReportHeader))
	{
		free(r);
		return true;
	}
	memcpy(&hdr, r->data, sizeof(hdr));

	if (hdr.kind == GP_REPORT_FRAGMENT)
	{
		foreach_ptr(GpExplainQuery, q, queries)
			if (q->coordinator && q->serial == hdr.serial)
				target = q;
	}
	else if (hdr.kind == GP_REPORT_STATEMENT && answer_query != NULL &&
			 query_live(answer_query))
	{
		target = answer_query;
		r->node = answer_node;
	}

	if (target == NULL)
	{
		free(r);
		return true;
	}
	*target->raw_tail = r;
	target->raw_tail = &r->next;
	return true;
}

/*
 * The gathers and Motions a LIMIT left open, ended now rather than as the
 * executor ends: their segments' parts end, and say what they did, before
 * the plan is printed.  Nothing reads them after ExecutorFinish.
 */
static bool
finish_walker(PlanState *ps, void *context)
{
	if (GpGatherScanFinish(ps))
		return false;
	if (GpMotionFinish(ps))
		return false;			/* below it is the segments' */
	return planstate_tree_walker(ps, finish_walker, context);
}

static SegFigures *
figures_of(SegFigures **array)
{
	if (*array == NULL)
		*array = palloc0_array(SegFigures, GpClusterSegmentCount());
	return *array;
}

static SliceStats *
slice_stats(GpExplainQuery *q, int slice)
{
	int			nsegs = GpClusterSegmentCount();
	SliceStats *s;

	foreach_ptr(SliceStats, found, q->slices)
		if (found->slice == slice)
			return found;
	s = palloc0_object(SliceStats);
	s->slice = slice;
	s->seen = palloc0_array(bool, nsegs);
	s->execmem = palloc0_array(int64, nsegs);
	s->vmem = palloc0_array(int64, nsegs);
	q->slices = lappend(q->slices, s);
	return s;
}

/* dst += add */
static void
wal_add(WalUsage *dst, const WalUsage *add)
{
	WalUsage	none = {0};

	WalUsageAccumDiff(dst, add, &none);
}

/* a node's runs, one after another: summed, their memory at most */
static void
add_figures(SegFigures *to, const GpReportNode *n)
{
	GpReportNode *t = &to->node;

	if (!to->seen)
	{
		*t = *n;
		to->seen = true;
		return;
	}
	t->spilled |= n->spilled;
	t->rtf_worked |= n->rtf_worked;
	t->ntuples += n->ntuples;
	t->ntuples2 += n->ntuples2;
	t->nloops += n->nloops;
	t->nfiltered1 += n->nfiltered1;
	t->nfiltered2 += n->nfiltered2;
	t->startup_ns += n->startup_ns;
	t->total_ns += n->total_ns;
	if (n->firststart != 0 && (t->firststart == 0 || n->firststart < t->firststart))
		t->firststart = n->firststart;
	t->execmem = Max(t->execmem, n->execmem);
	t->workmem = Max(t->workmem, n->workmem);
	t->nsearches += n->nsearches;
	t->nworkers = Max(t->nworkers, n->nworkers);
	wal_add(&t->wal, &n->wal);
}

/* One report, into the figures of the nodes and the slice it is of. */
static void
take_report(GpExplainQuery *q, RawReport *r)
{
	GpReportHeader hdr;
	GpReportNode *nodes;
	int			nsegs = GpClusterSegmentCount();
	int			slice = 0;
	SliceStats *s;

	memcpy(&hdr, r->data, sizeof(hdr));
	if (hdr.content < 0 || hdr.content >= nsegs || hdr.nnodes < 0 ||
		r->len != (int) (sizeof(GpReportHeader) + hdr.nnodes * sizeof(GpReportNode)) ||
		(hdr.kind == GP_REPORT_FRAGMENT && hdr.nnodes == 0))
		return;
	nodes = palloc_array(GpReportNode, Max(hdr.nnodes, 1));
	memcpy(nodes, r->data + sizeof(GpReportHeader), hdr.nnodes * sizeof(GpReportNode));

	if (hdr.kind == GP_REPORT_FRAGMENT)
	{
		for (int i = 0; i < hdr.nnodes; i++)
		{
			int			id = nodes[i].plan_node_id;

			if (id < 0 || id >= q->nnodes)
				continue;
			add_figures(&figures_of(&q->stats[id].frag)[hdr.content], &nodes[i]);
		}
		if (nodes[0].plan_node_id >= 0 && nodes[0].plan_node_id < q->nnodes)
			slice = q->slice_of[nodes[0].plan_node_id];
	}
	else
	{
		PlanState  *node = r->node;
		int			id = node->plan->plan_node_id;
		int			nsegs_read;
		GpReportNode top;
		SegFigures *f;

		if (id < 0 || id >= q->nnodes)
		{
			pfree(nodes);
			return;
		}

		/*
		 * The statement's top node, but for what it wrote to the WAL, which
		 * is the whole statement's -- a COPY has no node at all.
		 */
		if (hdr.nnodes > 0)
			top = nodes[0];
		else
			memset(&top, 0, sizeof(top));
		top.wal = hdr.wal;
		f = &figures_of(&q->stats[id].stmt)[hdr.content];
		add_figures(f, &top);
		f->execmem = Max(f->execmem, hdr.execmem);

		/* a gather's statements are its slice's; a write's, slice 0's */
		if (!GpGatherScanSlice(node, &slice, &nsegs_read))
			slice = 0;
	}

	s = slice_stats(q, slice);
	s->seen[hdr.content] = true;
	s->execmem[hdr.content] = Max(s->execmem[hdr.content], hdr.execmem);
	s->vmem[hdr.content] = Max(s->vmem[hdr.content], hdr.vmem);
	for (int i = 0; i < hdr.nnodes; i++)
		s->workmem = Max(s->workmem, nodes[i].workmem);
	pfree(nodes);
}

typedef struct SliceWalk
{
	GpExplainQuery *q;
	int			slice;
	bool	   *seen;
} SliceWalk;

/*
 * The slice each node runs in: the root's, 0, down to a Motion, whose
 * fragment is the slice it sends from (its "sliceN").  A subplan a fragment
 * runs is that fragment's.
 */
static bool
slice_walker(PlanState *ps, SliceWalk *w)
{
	SliceWalk	below = *w;
	int			id = ps->plan->plan_node_id;

	if (id >= 0 && id < w->q->nnodes)
	{
		if (w->seen[id])
			return false;
		w->seen[id] = true;
		w->q->slice_of[id] = w->slice;
	}
	if (GpMotionIs(ps->plan))
		below.slice = GpMotionSlice(ps->plan);
	return planstate_tree_walker(ps, slice_walker, &below);
}

/*
 * The fragment node's own instrumentation, which the coordinator never ran,
 * given the figures of the segment that returned the most rows, or ran it
 * the most times where none returned any -- Cloudberry's winner
 * (cdbexplain_depositStatsToNode()) -- and the WAL and index searches of
 * all of them.  A Gather's workers are the winner's, and so is whether a
 * runtime filter worked in the node, as Cloudberry's prf_work is.
 */
static void
deposit_fragment(PlanState *ps, SegFigures *segs)
{
	NodeInstrumentation *instr = ps->instrument;
	int			nsegs = GpClusterSegmentCount();
	int			w = -1;
	GpReportNode *n;
	uint64		nsearches = 0;

	for (int c = 0; c < nsegs; c++)
		if (segs[c].seen && segs[c].node.ntuples > 0 &&
			(w < 0 || segs[c].node.ntuples > segs[w].node.ntuples))
			w = c;
	for (int c = 0; w < 0 && c < nsegs; c++)
		if (segs[c].seen && segs[c].node.nloops > 0)
			w = c;
	if (w < 0)
		return;

	n = &segs[w].node;
	instr->running = false;
	instr->nloops = n->nloops;
	instr->ntuples = n->ntuples;
	instr->ntuples2 = n->ntuples2;
	instr->nfiltered1 = n->nfiltered1;
	instr->nfiltered2 = n->nfiltered2;
	if (n->rtf_worked)
		GpRtFilterSetWorked(ps);
	INSTR_TIME_SET_ZERO(instr->startup);
	INSTR_TIME_ADD_NANOSEC(instr->startup, n->startup_ns);
	INSTR_TIME_SET_ZERO(instr->instr.total);
	INSTR_TIME_ADD_NANOSEC(instr->instr.total, n->total_ns);

	for (int c = 0; c < nsegs; c++)
	{
		if (!segs[c].seen)
			continue;
		if (instr->instr.need_walusage)
			wal_add(&instr->instr.walusage, &segs[c].node.wal);
		nsearches += segs[c].node.nsearches;
	}

	/*
	 * the counter gp_motion.c gives a described index scan, and the workers
	 * the winner's Gather launched (gp_parallel.c), which EXPLAIN prints as
	 * "Workers Launched"
	 */
	switch (nodeTag(ps))
	{
		case T_GatherState:
			((GatherState *) ps)->nworkers_launched = n->nworkers;
			break;
		case T_GatherMergeState:
			((GatherMergeState *) ps)->nworkers_launched = n->nworkers;
			break;
		case T_IndexScanState:
			if (((IndexScanState *) ps)->iss_Instrument != NULL)
				((IndexScanState *) ps)->iss_Instrument->nsearches += nsearches;
			break;
		case T_IndexOnlyScanState:
			if (((IndexOnlyScanState *) ps)->ioss_Instrument != NULL)
				((IndexOnlyScanState *) ps)->ioss_Instrument->nsearches += nsearches;
			break;
		case T_BitmapIndexScanState:
			if (((BitmapIndexScanState *) ps)->biss_Instrument != NULL)
				((BitmapIndexScanState *) ps)->biss_Instrument->nsearches += nsearches;
			break;
		default:
			break;
	}
}

typedef struct DepositWalk
{
	GpExplainQuery *q;
	bool	   *seen;
} DepositWalk;

static bool
deposit_walker(PlanState *ps, DepositWalk *w)
{
	int			id = ps->plan->plan_node_id;
	NodeInstrumentation *instr = ps->instrument;

	if (id >= 0 && id < w->q->nnodes)
	{
		NodeStats  *ns = &w->q->stats[id];

		if (w->seen[id])
			return false;
		w->seen[id] = true;

		/* a node the coordinator never ran, which the segments did */
		if (instr != NULL && ns->frag != NULL && instr->nloops == 0 &&
			!instr->running)
			deposit_fragment(ps, ns->frag);

		/* a node whose statements the segments ran: their WAL */
		if (instr != NULL && ns->stmt != NULL && instr->instr.need_walusage)
		{
			for (int c = 0; c < GpClusterSegmentCount(); c++)
				if (ns->stmt[c].seen)
					wal_add(&instr->instr.walusage, &ns->stmt[c].node.wal);
		}
	}
	return planstate_tree_walker(ps, deposit_walker, w);
}

/*
 * The plan has run: every part the segments ran has ended and spoken, and
 * what they said is taken into the plan's nodes and slices, before EXPLAIN
 * prints them.
 */
static void
coordinator_collect(GpExplainQuery *q)
{
	EState	   *estate = q->estate;
	MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	PlanState  *root = q->queryDesc->planstate;
	SliceWalk	sw;
	DepositWalk dw;

	(void) finish_walker(root, NULL);
	foreach_ptr(PlanState, sub, estate->es_subplanstates)
		if (sub != NULL)
			(void) finish_walker(sub, NULL);

	q->execmem = MemoryContextMemAllocated(estate->es_query_cxt, true);
	q->vmem = vmem_reserved != NULL ? vmem_reserved() : 0;

	q->stats = palloc0_array(NodeStats, q->nnodes);
	q->slice_of = palloc0_array(int, q->nnodes);
	(void) slice_stats(q, 0);
	sw.q = q;
	sw.slice = 0;
	sw.seen = palloc0_array(bool, q->nnodes);
	(void) slice_walker(root, &sw);
	foreach_ptr(PlanState, sub, estate->es_subplanstates)
		if (sub != NULL)
			(void) slice_walker(sub, &sw);

	while (q->raw != NULL)
	{
		RawReport  *r = q->raw;

		q->raw = r->next;
		take_report(q, r);
		free(r);
	}
	q->raw_tail = &q->raw;

	dw.q = q;
	dw.seen = palloc0_array(bool, q->nnodes);
	(void) deposit_walker(root, &dw);
	foreach_ptr(PlanState, sub, estate->es_subplanstates)
		if (sub != NULL)
			(void) deposit_walker(sub, &dw);

	q->collected = true;
	MemoryContextSwitchTo(oldcxt);
}

/* ------------------------------------------------------------------------- */
/* Printing, in Cloudberry's words                                           */
/* ------------------------------------------------------------------------- */

/* Cloudberry's CdbExplain_Agg: the values above 0, and the first largest's id. */
typedef struct StatAgg
{
	double		vmax;
	double		vsum;
	int			vcnt;
	int			imax;
} StatAgg;

static void
agg_add(StatAgg *agg, double v, int id)
{
	if (v <= 0)
		return;
	agg->vsum += v;
	agg->vcnt++;
	if (v > agg->vmax || agg->vcnt == 1)
	{
		agg->vmax = v;
		agg->imax = id;
	}
}

#define KB(bytes)	((long) floor(((double) (bytes) + 1023.0) / 1024.0))

/* "Executor Memory: 10kB  Segments: 3  Max: 4kB (segment 1)" */
static void
print_memory(StatAgg *agg, ExplainState *es)
{
	if (agg->vcnt == 0)
		return;
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "Executor Memory: %ldkB  Segments: %d  Max: %ldkB (segment %d)\n",
						 KB(agg->vsum), agg->vcnt, KB(agg->vmax), agg->imax);
	}
	else
	{
		ExplainPropertyInteger("Executor Memory", "kB", KB(agg->vsum), es);
		ExplainPropertyInteger("Executor Memory Segments", NULL, agg->vcnt, es);
		ExplainPropertyInteger("Executor Max Memory", "kB", KB(agg->vmax), es);
		ExplainPropertyInteger("Executor Max Memory Segment", NULL, agg->imax, es);
	}
}

/* Cloudberry's cdbexplain_formatSeconds(): three decimals below 10 ms */
static char *
format_ms(double ms, bool unit)
{
	return psprintf("%.*f%s", (ms < 10.0 && ms != 0.0 && ms > -10.0) ? 3 : 0,
					ms, unit ? " ms" : "");
}

/*
 * What the segments did of a node (cdbexplain_showExecStats()): its memory
 * and the coordinator's, under explain_memory_verbosity = detail -- a
 * gather of the planner's route has both, Cloudberry's Gather Motion and
 * the scan below it -- its work_mem and how many segments spilled, with
 * VERBOSE, and each segment's run, with gp.enable_explain_allstat.  Never
 * a node's the coordinator alone ran but its memory: "(segment -1)" of a
 * work_mem line would not be Cloudberry's, whose nodes that spill run on
 * the segments.
 */
static void
print_node_statistics(PlanState *ps, ExplainState *es)
{
	GpExplainQuery *q = query_of(ps->state);
	int			id = ps->plan->plan_node_id;
	int			nsegs = GpClusterSegmentCount();
	SegFigures *segs;
	NodeStats  *ns;

	if (q == NULL || !q->coordinator || !q->collected || !es->analyze ||
		id < 0 || id >= q->nnodes)
		return;
	ns = &q->stats[id];
	segs = ns->frag != NULL ? ns->frag : ns->stmt;

	if (q->options & GP_EXPLAIN_MEMORY)
	{
		StatAgg		own = {0};
		StatAgg		theirs = {0};

		if (q->contexts[id] != NULL)
			agg_add(&own, MemoryContextMemAllocated(q->contexts[id], true), -1);
		for (int c = 0; segs != NULL && c < nsegs; c++)
			if (segs[c].seen)
				agg_add(&theirs, ns->frag != NULL ? segs[c].node.execmem
						: segs[c].execmem, c);

		/* other formats name a figure once: the segments', where there are */
		if (es->format == EXPLAIN_FORMAT_TEXT || theirs.vcnt == 0)
			print_memory(&own, es);
		print_memory(&theirs, es);
	}

	if (es->verbose && ns->frag != NULL)
	{
		StatAgg		used = {0};
		int			spilling = 0;

		for (int c = 0; c < nsegs; c++)
		{
			if (!segs[c].seen)
				continue;
			agg_add(&used, segs[c].node.workmem, c);
			if (segs[c].node.spilled)
				spilling++;
		}
		if (used.vcnt > 0 && es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "work_mem: %ldkB  Segments: %d  Max: %ldkB (segment %d)  Workfile: (%d spilling)\n",
							 KB(used.vsum), used.vcnt, KB(used.vmax),
							 used.imax, spilling);
		}
		else if (used.vcnt > 0)
		{
			ExplainOpenGroup("work_mem", "work_mem", true, es);
			ExplainPropertyInteger("Used", "kB", KB(used.vsum), es);
			ExplainPropertyInteger("Segments", NULL, used.vcnt, es);
			ExplainPropertyInteger("Max Memory", "kB", KB(used.vmax), es);
			ExplainPropertyInteger("Max Memory Segment", NULL, used.imax, es);
			ExplainPropertyInteger("Workfile Spilling", NULL, spilling, es);
			ExplainCloseGroup("work_mem", "work_mem", true, es);
		}
	}

	if (gp_enable_explain_allstat && (q->options & GP_EXPLAIN_ALLSTAT) &&
		segs != NULL)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "allstat: seg_firststart_total_ntuples");
		}
		else
			ExplainOpenGroup("Allstat", "Allstat", false, es);
		for (int c = 0; c < nsegs; c++)
		{
			GpReportNode *n = &segs[c].node;
			double		start;

			if (!segs[c].seen || n->firststart == 0)
				continue;
			start = (double) (n->firststart - q->start) / 1000.0;
			if (es->format == EXPLAIN_FORMAT_TEXT)
				appendStringInfo(es->str, "/seg%d_%s_%s_%.0f", c,
								 format_ms(start, true),
								 format_ms(n->total_ns / 1000000.0, true),
								 n->ntuples);
			else
			{
				ExplainOpenGroup("Segment", NULL, true, es);
				ExplainPropertyInteger("Segment index", NULL, c, es);
				ExplainPropertyText("Time To First Result", format_ms(start, false), es);
				ExplainPropertyText("Time To Total Result",
									format_ms(n->total_ns / 1000000.0, false), es);
				ExplainPropertyFloat("Tuples", NULL, n->ntuples, 1, es);
				ExplainCloseGroup("Segment", NULL, true, es);
			}
		}
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(es->str, "//end\n");
		else
			ExplainCloseGroup("Allstat", "Allstat", false, es);
	}
}

static int
slice_cmp(const ListCell *a, const ListCell *b)
{
	return ((SliceStats *) lfirst(a))->slice - ((SliceStats *) lfirst(b))->slice;
}

/* "50K bytes avg x 3 workers, 50K bytes max (seg0)" */
static void
print_slice_memory(const char *label, const char *text_prefix, StatAgg *agg,
				   ExplainState *es)
{
	if (agg->vcnt == 0)
		return;
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		const char *seg = agg->imax >= 0 ? psprintf(" (seg%d)", agg->imax) : "";

		if (agg->vcnt == 1)
			appendStringInfo(es->str, "%s%.0fK bytes%s.", text_prefix,
							 (double) KB(agg->vmax), seg);
		else
			appendStringInfo(es->str, "%s%.0fK bytes avg x %d workers, %.0fK bytes max%s.",
							 text_prefix, (double) KB(agg->vsum / agg->vcnt),
							 agg->vcnt, (double) KB(agg->vmax), seg);
	}
	else if (agg->vcnt == 1)
		ExplainPropertyInteger(label, "kB", KB(agg->vmax), es);
	else
	{
		ExplainOpenGroup(label, label, true, es);
		ExplainPropertyInteger("Average", "kB", KB(agg->vsum / agg->vcnt), es);
		ExplainPropertyInteger("Workers", NULL, agg->vcnt, es);
		ExplainPropertyInteger("Maximum Memory Used", "kB", KB(agg->vmax), es);
		ExplainCloseGroup(label, label, true, es);
	}
}

/*
 * Each slice's memory, after the plan (gpexplain_formatSlicesOutput()):
 * slice 0 the coordinator's -- or its segments', where a write's statements
 * are its Primary Writer -- and each other slice its segments', "Vmem
 * reserved" too above explain_memory_verbosity = suppress.  With SUMMARY,
 * as Cloudberry's.
 */
static void
print_slice_statistics(PlannedStmt *plannedstmt, ExplainState *es)
{
	GpExplainQuery *q = NULL;
	int			verbosity = memory_verbosity();
	int			nsegs = GpClusterSegmentCount();
	List	   *slices;

	foreach_ptr(GpExplainQuery, found, queries)
	{
		if (found->coordinator && found->queryDesc->plannedstmt == plannedstmt)
		{
			q = found;
			break;
		}
	}
	if (q == NULL || !q->collected || !es->analyze || !es->summary)
		return;

	slices = list_copy(q->slices);
	list_sort(slices, slice_cmp);

	ExplainOpenGroup("Slice statistics", "Slice statistics", false, es);
	foreach_ptr(SliceStats, s, slices)
	{
		StatAgg		mem = {0};
		StatAgg		vmem = {0};

		for (int c = 0; c < nsegs; c++)
		{
			if (!s->seen[c])
				continue;
			agg_add(&mem, s->execmem[c], c);
			agg_add(&vmem, s->vmem[c], c);
		}

		/* a COPY's executor is none: the coordinator's figures, then */
		if (s->slice == 0 && mem.vcnt == 0)
			agg_add(&mem, q->execmem, -1);
		if (s->slice == 0 && vmem.vcnt == 0)
			agg_add(&vmem, q->vmem, -1);

		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(es->str, "  (slice%d) %s  ", s->slice,
							 s->slice < 10 ? " " : "");
		else
		{
			ExplainOpenGroup("Slice", NULL, true, es);
			ExplainPropertyInteger("Slice", NULL, s->slice, es);
		}
		print_slice_memory("Executor Memory", "Executor memory: ", &mem, es);
		if (verbosity > VERBOSITY_SUPPRESS)
			print_slice_memory("Virtual Memory", "  Vmem reserved: ", &vmem, es);
		if (s->workmem > 0 && es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(es->str, "  Work_mem: %.0fK bytes max.",
							 (double) KB(s->workmem));
		else if (s->workmem > 0)
			ExplainPropertyInteger("Work Maximum Memory", "kB", KB(s->workmem), es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoChar(es->str, '\n');
		else
			ExplainCloseGroup("Slice", NULL, true, es);
	}
	ExplainCloseGroup("Slice statistics", "Slice statistics", false, es);
}

/* ------------------------------------------------------------------------- */
/* The executor's hooks                                                      */
/* ------------------------------------------------------------------------- */

/* An explained statement: the segments are asked, and their answers heard. */
static void
coordinator_begin(QueryDesc *queryDesc)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
	int			options = queryDesc->instrument_options;
	GpExplainQuery *q;

	if (memory_verbosity() >= VERBOSITY_DETAIL)
		options |= GP_EXPLAIN_MEMORY;
	if (gp_enable_explain_allstat)
		options |= GP_EXPLAIN_ALLSTAT;
	q = query_begin(queryDesc, true, options);
	q->serial = ++explain_serial;
	q->start = GetCurrentTimestamp();
	wrap_nodes(q);

	q->saved_instrument = pstrdup(GetConfigOption("gp.explain_instrument", false, false));
	(void) set_config_option("gp.explain_instrument", psprintf("%d", options),
							 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true,
							 0, false);
	MemoryContextSwitchTo(oldcxt);
}

/* A segment's part of one: measured, and to be reported as it ends. */
static void
segment_begin(QueryDesc *queryDesc, int options, int kind, int serial)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
	GpExplainQuery *q = query_begin(queryDesc, false, options);

	q->kind = kind;
	q->serial = serial;
	q->wal_start = pgWalUsage;
	if ((kind == GP_REPORT_FRAGMENT && (options & GP_EXPLAIN_OWN_OPTIONS)) ||
		(options & GP_EXPLAIN_ALLSTAT))
		wrap_nodes(q);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * The statement is explained where the coordinator's instrumentation is
 * EXPLAIN ANALYZE's -- not query metrics' alone, which asks the segments
 * nothing -- and it runs; a segment's part is measured where the
 * coordinator asked, at the top of what it sent -- not a query a function
 * in it runs.
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		explained = false;
	int			asked = 0;
	int			kind = 0;
	int			serial = 0;

	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && !IsInParallelMode())
	{
		if (GpClusterBackendRole() == GP_ROLE_DISPATCH)
			explained = queryDesc->instrument_options != 0 &&
				!(queryDesc->instrument_options & GP_INSTR_METRICS_ONLY);
		else if (GpClusterIsDispatched() && executor_depth == 0)
			asked = asked_options(queryDesc, &kind, &serial);
	}
	if (asked != 0)
		queryDesc->instrument_options =
			(queryDesc->instrument_options & ~GP_INSTR_METRICS_ONLY) |
			(asked & ~GP_EXPLAIN_OWN_OPTIONS);

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (explained)
		coordinator_begin(queryDesc);
	else if (asked != 0)
		segment_begin(queryDesc, asked, kind, serial);
}

/*
 * A statement that runs inside another's node -- a function's query --
 * answers for none of the node's segments' statements: its own run, or
 * finish, sets none.
 */
static void
explain_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count)
{
	GpExplainQuery *save_query = answer_query;
	PlanState  *save_node = answer_node;

	answer_query = NULL;
	answer_node = NULL;
	executor_depth++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		executor_depth--;
		answer_query = save_query;
		answer_node = save_node;
	}
	PG_END_TRY();
}

static void
explain_ExecutorFinish(QueryDesc *queryDesc)
{
	GpExplainQuery *save_query = answer_query;
	PlanState  *save_node = answer_node;
	GpExplainQuery *q;

	answer_query = NULL;
	answer_node = NULL;
	executor_depth++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		executor_depth--;
		answer_query = save_query;
		answer_node = save_node;
	}
	PG_END_TRY();

	q = query_of(queryDesc->estate);
	if (q != NULL && q->coordinator && !q->collected)
		coordinator_collect(q);
}

static void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
	GpExplainQuery *q = query_of(queryDesc->estate);

	if (q != NULL && !q->coordinator)
		segment_report(q);
	else if (q != NULL)
		(void) set_config_option("gp.explain_instrument", q->saved_instrument,
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET,
								 true, 0, false);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * A COPY a write of the planner's route sends a segment its rows by has no
 * executor: what it wrote to the WAL is reported as it ends.  Neither it
 * nor a utility statement a node runs on the coordinator answers for the
 * node.
 */
static void
explain_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	GpExplainQuery *save_query = answer_query;
	PlanState  *save_node = answer_node;
	bool		copy = gp_explain_instrument != 0 && GpClusterIsDispatched() &&
		executor_depth == 0 && IsA(pstmt->utilityStmt, CopyStmt) &&
		((CopyStmt *) pstmt->utilityStmt)->is_from;
	WalUsage	start = pgWalUsage;

	answer_query = NULL;
	answer_node = NULL;
	if (copy)
		executor_depth++;
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		if (copy)
			executor_depth--;
		answer_query = save_query;
		answer_node = save_node;
	}
	PG_END_TRY();

	if (copy)
	{
		GpReportHeader hdr;
		StringInfoData buf;

		memset(&hdr, 0, sizeof(hdr));
		initStringInfo(&buf);
		appendBinaryStringInfo(&buf, &hdr, sizeof(hdr));
		hdr.kind = GP_REPORT_STATEMENT;
		WalUsageAccumDiff(&hdr.wal, &pgWalUsage, &start);
		send_report(&hdr, &buf);
		pfree(buf.data);
	}
}

DefElem *
GpExplainFragmentMark(EState *estate)
{
	GpExplainQuery *q = query_of(estate);

	if (q == NULL || !q->coordinator)
		return NULL;
	return makeDefElem(pstrdup(GP_EXPLAIN_MARK),
					   (Node *) list_make2(makeInteger(q->options),
										   makeInteger(q->serial)),
					   -1);
}

void
GpExplainSetVmemReserved(GpExplainVmemReserved reserved)
{
	vmem_reserved = reserved;
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
	print_slice_statistics(plannedstmt, es);

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

	/*
	 * What EXPLAIN ANALYZE asks the segments to measure, which they are sent
	 * with the other settings (gp_dispatch.c).  Set by the coordinator for
	 * the statement it explains; set by hand, it only makes a segment send
	 * what it measured, which a coordinator explaining nothing drops.
	 */
	DefineCustomIntVariable("gp.explain_instrument",
							"What EXPLAIN ANALYZE asks the segments to measure of the statement it explains.",
							"Zero asks nothing.",
							&gp_explain_instrument,
							0, 0, INT_MAX,
							PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							NULL, NULL, NULL);

	prev_explain_per_node = explain_per_node_hook;
	explain_per_node_hook = gp_explain_per_node;
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = gp_explain_per_plan;

	/* the segments' statistics: a cluster's only */
	if (GpClusterIsSingleNode())
		return;
	GpDispatchAddNoticeFilter(explain_notice_filter);
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = explain_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = explain_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = explain_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = explain_ProcessUtility;
}
