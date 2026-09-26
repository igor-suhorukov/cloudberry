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
 * compat/motion.c
 *	  Whether a plan's Motions can be carried out as stage A carries them; see
 *	  cb_motion.h.
 *
 * The fragment below each Motion runs on a segment, and a parameter it reads
 * is set there or sent there.  ORCA's translator is the only thing that makes
 * these plans, and what sets a parameter in them is a short list: a
 * NestLoop's nestParams, a SubPlan -- its setParam as an initplan, its
 * parParam from its arguments, its paramIds from its own output -- a
 * RecursiveUnion's work table, and a Partition Selector.  What reads one is
 * a Param, a CteScan's cteParam, a WorkTableScan's wtParam, and a Dynamic
 * Scan's selectors.
 *
 * A Param's is a value, and one the coordinator sets -- an initplan's, a
 * nested loop's outer column above the Gather -- is sent with the fragment,
 * as Cloudberry's dispatcher sends a slice its parameters; so is a statement
 * parameter.  The other three hold a pointer into the executor that set
 * them, and have to be set in the fragment itself.  A value another fragment
 * sets is on a segment, where the coordinator cannot read it: that plan is
 * still refused.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "catalog/pg_class.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "optimizer/walkers.h"

#include "cb_compat.h"
#include "cb_dynamicscan.h"
#include "cb_motion.h"
#include "cb_sharedscan.h"
#include "gp_motion.h"

/* A Motion, and what its fragment is sent with. */
typedef struct motion_params
{
	Plan	   *motion;
	Bitmapset  *exec_params;
	Bitmapset  *extern_params;
} motion_params;

typedef struct motion_check_context
{
	plan_tree_base_prefix base;	/* plan_tree_walker's, first */
	bool		in_fragment;
	Bitmapset  *referenced;		/* values: Params */
	Bitmapset  *referenced_ptr; /* pointers: CTEs, work tables, selectors */
	Bitmapset  *externs;		/* statement parameters */
	Bitmapset  *produced;
	Bitmapset **fragment_produced;	/* by any fragment of the plan */
	List	  **params;			/* of motion_params */
	Plan	   *top;			/* the Motion the coordinator runs it from */
	int			problem;
	List	  **order;			/* the enclosing Gather's Motions, senders first */
	bool		may_write;		/* the fragment a write is dispatched as */
	int			slice;			/* the fragment's slice, where in one */
	bool		on_coordinator; /* a fragment the coordinator sends from */
	List	  **callers;		/* each subplan's calling slice; see gp_motion.h */
	List	  **shares;			/* each shared CTE's id and slices, IntLists */
	bool	   *from_coordinator;	/* a fragment the coordinator sends */
} motion_check_context;

/* A subplan no SubPlan the walk met calls. */
#define SUBPLAN_UNSEEN		(-3)

/* Subplan "plan_id" is called from "slice": the one, or several. */
static void
note_subplan_caller(motion_check_context *ctx, int plan_id, int slice)
{
	List	   *callers = *ctx->callers;
	ListCell   *cell;

	while (list_length(callers) < plan_id)
		callers = lappend_int(callers, SUBPLAN_UNSEEN);
	cell = list_nth_cell(callers, plan_id - 1);
	if (lfirst_int(cell) == SUBPLAN_UNSEEN)
		lfirst_int(cell) = slice;
	else if (lfirst_int(cell) != slice)
		lfirst_int(cell) = GP_SUBPLAN_UNKNOWN;
	*ctx->callers = callers;
}

/* Shared CTE "share_id" is read or written in "slice". */
static void
note_share(motion_check_context *ctx, int share_id, int slice)
{
	ListCell   *lc;

	foreach(lc, *ctx->shares)
	{
		List	   *share = (List *) lfirst(lc);

		if (linitial_int(share) == share_id)
		{
			if (!list_member_int(list_delete_first(list_copy(share)), slice))
				lfirst(lc) = lappend_int(share, slice);
			return;
		}
	}
	*ctx->shares = lappend(*ctx->shares, list_make2_int(share_id, slice));
}

static bool
is_motion(Node *node)
{
	return IsA(node, CustomScan) &&
		strcmp(((CustomScan *) node)->methods->CustomName, GP_MOTION_NAME) == 0;
}

static Bitmapset *
add_int_list(Bitmapset *set, List *ints)
{
	ListCell   *lc;

	foreach(lc, ints)
		set = bms_add_member(set, lfirst_int(lc));
	return set;
}

static bool
motion_check_walker(Node *node, void *arg)
{
	motion_check_context *ctx = (motion_check_context *) arg;

	if (node == NULL)
		return false;

	if (is_motion(node))
	{
		Plan	   *plan = (Plan *) node;
		const GpCoreApi *api = cb_core_api();
		int			type = api->motion_type(plan);
		bool		to_segment = type == GP_MOTION_GATHER &&
			ctx->in_fragment && !ctx->on_coordinator;
		bool		gather = (type == GP_MOTION_GATHER && !to_segment) ||
			type == GP_MOTION_DML;
		List	   *order = NIL;
		motion_check_context sub;

		/*
		 * A Gather's rows go to the coordinator, from where it runs; so do a
		 * dispatched write's counts, and no write is in a fragment.  The
		 * fragment a Motion from the coordinator sends is the coordinator's
		 * own, which runs a Gather in it as it runs one above every fragment
		 * -- Cloudberry's entry DB slice, a LIMIT or an aggregate over a
		 * Gather broadcast back.  A Gather in a fragment a segment runs goes
		 * to that segment -- the slice it is in runs on one -- which receives
		 * it as it receives a Motion between segments, from gp_core 1.9 on;
		 * so it is one here too.
		 */
		if ((type == GP_MOTION_DML && ctx->in_fragment) ||
			(to_segment && api->version_minor < 9))
		{
			ctx->problem = GP_ORCA_MOTION_NESTED;
			return true;
		}

		/*
		 * Below any Motion is a fragment of its own, which a segment runs --
		 * or the coordinator, for a Motion it sends -- apart from the rest.
		 */
		sub = *ctx;
		sub.in_fragment = true;
		sub.referenced = NULL;
		sub.referenced_ptr = NULL;
		sub.externs = NULL;
		sub.produced = NULL;
		sub.problem = GP_ORCA_MOTION_OK;
		sub.order = gather ? &order : ctx->order;
		sub.may_write = type == GP_MOTION_DML;
		sub.slice = api->motion_slice(plan);
		sub.on_coordinator = !gather &&
			api->motion_segment(plan) == GP_MOTION_FROM_COORDINATOR;
		if (sub.on_coordinator)
			*ctx->from_coordinator = true;
		if (!ctx->in_fragment || gather)
			sub.top = plan;
		if (motion_check_walker((Node *) plan->lefttree, &sub))
		{
			ctx->problem = sub.problem;
			return true;
		}
		if (!bms_is_subset(sub.referenced_ptr, sub.produced))
		{
			ctx->problem = GP_ORCA_MOTION_PARAM;
			return true;
		}

		/* a value the coordinator's own fragment sets is the coordinator's */
		if (!sub.on_coordinator)
			*ctx->fragment_produced = bms_add_members(*ctx->fragment_produced,
													  sub.produced);
		{
			Bitmapset  *needed = bms_difference(sub.referenced, sub.produced);

			if (!bms_is_empty(needed) || !bms_is_empty(sub.externs))
			{
				motion_params *mp;

				/* a gp_core that cannot send them */
				if (api->version_minor < 6)
				{
					ctx->problem = !bms_is_empty(needed) ?
						GP_ORCA_MOTION_PARAM : GP_ORCA_MOTION_EXTERN;
					return true;
				}
				mp = palloc(sizeof(motion_params));
				mp->motion = plan;
				mp->exec_params = needed;
				mp->extern_params = sub.externs;
				*ctx->params = lappend(*ctx->params, mp);
			}
		}

		/*
		 * A Motion between segments is carried out after the ones its own
		 * fragment receives from, and before the Gather above it sends.
		 */
		if (gather)
			api->motion_set_prepare(plan, order);
		else if (ctx->order != NULL)
			*ctx->order = lappend_int(*ctx->order, api->motion_slice(plan));

		/*
		 * The fragment it is in receives it -- in a SubPlan too, which is
		 * run where the expression that calls it is -- and its senders
		 * stream to whichever processes run that fragment.
		 */
		if (!gather && ctx->in_fragment && api->version_minor >= 5)
			api->motion_set_parent(plan, ctx->slice);

		/* Its own expressions are evaluated where it receives. */
		return motion_check_walker((Node *) plan->targetlist, ctx) ||
			motion_check_walker((Node *) plan->qual, ctx) ||
			motion_check_walker((Node *) plan->initPlan, ctx);
	}

	/*
	 * An initplan of a node in a fragment is the coordinator's, as
	 * Cloudberry's are -- its dispatcher runs every one before it dispatches
	 * (preprocess_initplans(), cdbsubplan.c) -- and its value is sent with
	 * the fragment.  So it moves to the Motion the coordinator runs the
	 * fragment from, which walks it after the fragment, where the
	 * coordinator's Gathers are, and which the executor initialises it on.
	 * The plan node tags are one run, T_Result to T_Limit, with one tag that
	 * is not a plan node in the middle of it.
	 */
	if (ctx->in_fragment && ctx->top != NULL &&
		nodeTag(node) >= T_Result && nodeTag(node) <= T_Limit &&
		!IsA(node, NestLoopParam) &&
		((Plan *) node)->initPlan != NIL)
	{
		ctx->top->initPlan = list_concat(ctx->top->initPlan,
										 ((Plan *) node)->initPlan);
		((Plan *) node)->initPlan = NIL;
	}

	/*
	 * A subplan's own part runs where the expression that calls it is: in
	 * the fragment's slice, or on the coordinator.
	 */
	if (IsA(node, SubPlan))
		note_subplan_caller(ctx, ((SubPlan *) node)->plan_id,
							ctx->in_fragment ? ctx->slice : GP_SUBPLAN_COORDINATOR);

	if (ctx->in_fragment)
	{
		switch (nodeTag(node))
		{
			case T_Param:
				{
					Param	   *param = (Param *) node;

					if (param->paramkind == PARAM_EXTERN)
						ctx->externs = bms_add_member(ctx->externs,
													  param->paramid);
					if (param->paramkind == PARAM_EXEC)
						ctx->referenced = bms_add_member(ctx->referenced,
														 param->paramid);
					break;
				}
			case T_NestLoop:
				{
					ListCell   *lc;

					foreach(lc, ((NestLoop *) node)->nestParams)
						ctx->produced =
							bms_add_member(ctx->produced,
										   lfirst_node(NestLoopParam, lc)->paramno);
					break;
				}
			case T_SubPlan:
				{
					SubPlan    *subplan = (SubPlan *) node;

					ctx->produced = add_int_list(ctx->produced, subplan->setParam);
					ctx->produced = add_int_list(ctx->produced, subplan->parParam);
					ctx->produced = add_int_list(ctx->produced, subplan->paramIds);
					break;
				}
			case T_RecursiveUnion:
				ctx->produced = bms_add_member(ctx->produced,
											   ((RecursiveUnion *) node)->wtParam);
				break;
			case T_CteScan:
				ctx->referenced_ptr = bms_add_member(ctx->referenced_ptr,
													 ((CteScan *) node)->cteParam);
				break;
			case T_WorkTableScan:
				ctx->referenced_ptr = bms_add_member(ctx->referenced_ptr,
													 ((WorkTableScan *) node)->wtParam);
				break;
			case T_ModifyTable:
				if (!ctx->may_write)
				{
					ctx->problem = GP_ORCA_MOTION_WRITE;
					return true;
				}
				break;

				/*
				 * A sequence is the coordinator's: a segment's copy of it is
				 * not the one the statement's values come from, and a reader
				 * may not advance it.  Cloudberry's segments ask the
				 * coordinator's sequence server; the port's have none.
				 */
			case T_NextValueExpr:
				ctx->problem = GP_ORCA_MOTION_SEQUENCE;
				return true;
			case T_FuncExpr:
				{
					Oid			f = ((FuncExpr *) node)->funcid;

					if (f == F_NEXTVAL || f == F_CURRVAL || f == F_LASTVAL ||
						f == F_SETVAL_REGCLASS_INT8 ||
						f == F_SETVAL_REGCLASS_INT8_BOOL)
					{
						ctx->problem = GP_ORCA_MOTION_SEQUENCE;
						return true;
					}
					break;
				}
			case T_CustomScan:
				{
					CustomScan *cscan = (CustomScan *) node;
					int			share_id;
					int			slice;
					bool		producer;

					if (gp_orca_is_shared_scan((Plan *) node, &share_id, &slice,
											   &producer))
						note_share(ctx, share_id, slice);
					if (cscan->methods == &gp_orca_partition_selector_methods)
						ctx->produced =
							bms_add_member(ctx->produced,
										   intVal(linitial(cscan->custom_private)));
					else if (cscan->methods == &gp_orca_dynamic_scan_methods)
						ctx->referenced_ptr =
							add_int_list(ctx->referenced_ptr,
										 (List *) lsecond(cscan->custom_private));
					break;
				}
			default:
				break;
		}
	}

	return plan_tree_walker(node, motion_check_walker, ctx, true);
}

static List *
bms_to_int_list(Bitmapset *set)
{
	List	   *result = NIL;
	int			x = -1;

	while ((x = bms_next_member(set, x)) >= 0)
		result = lappend_int(result, x);
	return result;
}

int
gp_orca_check_motions(PlannedStmt *stmt)
{
	motion_check_context ctx;
	Bitmapset  *fragment_produced = NULL;
	List	   *params = NIL;
	List	   *callers = NIL;
	List	   *shares = NIL;
	bool		from_coordinator = false;
	ListCell   *lc;

	exec_init_plan_tree_base(&ctx.base, stmt);
	ctx.in_fragment = false;
	ctx.referenced = NULL;
	ctx.referenced_ptr = NULL;
	ctx.externs = NULL;
	ctx.produced = NULL;
	ctx.fragment_produced = &fragment_produced;
	ctx.params = &params;
	ctx.top = NULL;
	ctx.problem = GP_ORCA_MOTION_OK;
	ctx.order = NULL;
	ctx.may_write = false;
	ctx.slice = -1;
	ctx.on_coordinator = false;
	ctx.callers = &callers;
	ctx.shares = &shares;
	ctx.from_coordinator = &from_coordinator;

	(void) motion_check_walker((Node *) stmt->planTree, &ctx);
	if (ctx.problem != GP_ORCA_MOTION_OK)
		return ctx.problem;

	/*
	 * A CTE shared in a slice the segments run keeps its rows in files named
	 * after the key of the Gather that sends the slice (compat/sharedscan.c),
	 * which the Gathers are told the slices of.  One read in more than one
	 * slice has them run at once, a consumer waiting for its producer; gp_core
	 * relays slices one at a time -- every one with gp.interconnect_type =
	 * relay, and otherwise one that scans a temporary table, one the
	 * coordinator sends and those below them (gp_motion.c, stream_plan()) --
	 * so such a plan is left to the planner, and a Gather that would relay
	 * one all the same refuses rather than wait.
	 */
	if (shares != NIL)
	{
		const char *ic = GetConfigOption("gp.interconnect_type", true, false);
		bool		across = false;
		bool		temp = false;
		List	   *slices = NIL;

		foreach(lc, shares)
		{
			List	   *share = list_delete_first(list_copy((List *) lfirst(lc)));

			across |= list_length(share) > 1;
			slices = lappend(slices, share);
		}
		foreach(lc, stmt->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			if (rte->rtekind == RTE_RELATION &&
				get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
				temp = true;
		}
		if (across &&
			(temp || from_coordinator || (ic != NULL && strcmp(ic, "relay") == 0)))
			return GP_ORCA_MOTION_SHARE;
		stmt->extension_state = lappend(stmt->extension_state,
										makeDefElem(pstrdup(GP_SHARE_SLICES),
													(Node *) slices, -1));
	}

	/*
	 * What a fragment is sent has to be the coordinator's to send: a value
	 * that some fragment sets is on a segment.
	 */
	foreach(lc, params)
	{
		motion_params *mp = (motion_params *) lfirst(lc);

		if (bms_overlap(mp->exec_params, fragment_produced))
			return GP_ORCA_MOTION_PARAM;
	}
	foreach(lc, params)
	{
		motion_params *mp = (motion_params *) lfirst(lc);

		cb_core_api()->motion_set_params(mp->motion,
										 bms_to_int_list(mp->exec_params),
										 bms_to_int_list(mp->extern_params));
	}

	/* where each subplan's own part runs, for the Gathers that stream */
	if (stmt->subplans != NIL)
	{
		while (list_length(callers) < list_length(stmt->subplans))
			callers = lappend_int(callers, SUBPLAN_UNSEEN);
		foreach(lc, callers)
			if (lfirst_int(lc) == SUBPLAN_UNSEEN)
				lfirst_int(lc) = GP_SUBPLAN_UNKNOWN;
		stmt->extension_state = lappend(stmt->extension_state,
										makeDefElem(pstrdup(GP_SUBPLAN_SLICES),
													(Node *) callers, -1));
	}
	return GP_ORCA_MOTION_OK;
}

Node *
gp_orca_slice_table(List *slices, List *motions)
{
	const GpCoreApi *api = cb_core_api();
	List	   *table = NIL;
	ListCell   *lc;

	foreach(lc, slices)
	{
		PlanSlice  *slice = (PlanSlice *) lfirst(lc);
		int			direct = -1;
		List	   *several = NIL;
		ListCell   *lm;

		/*
		 * a Gather or a write that direct dispatch sent to one segment says
		 * which, and to several, which those are
		 */
		foreach(lm, motions)
		{
			Plan	   *motion = (Plan *) lfirst(lm);

			if (api->motion_slice(motion) != slice->sliceIndex ||
				!((api->motion_type(motion) == GP_MOTION_GATHER &&
				   slice->gangType == GANGTYPE_PRIMARY_READER) ||
				  (api->motion_type(motion) == GP_MOTION_DML &&
				   slice->gangType == GANGTYPE_PRIMARY_WRITER)))
				continue;
			if (api->motion_segment(motion) >= 0)
				direct = api->motion_segment(motion);
			else if (api->version_minor >= 7)
				several = api->motion_segments(motion);
		}

		table = lappend(table,
						list_make5(makeInteger(slice->sliceIndex),
								   makeInteger(slice->parentIndex),
								   makeInteger((int) slice->gangType),
								   makeInteger(slice->numsegments),
								   makeInteger(slice->segindex)));
		llast(table) = lappend((List *) llast(table), makeInteger(direct));
		llast(table) = lappend((List *) llast(table), list_copy(several));
	}

	return (Node *) makeDefElem(pstrdup(GP_SLICE_TABLE), (Node *) table, -1);
}
