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
 * The fragment below each Motion runs on a segment with the statement's
 * parameter slots empty, so every PARAM_EXEC it reads has to be set inside
 * it.  ORCA's translator is the only thing that makes these plans, and what
 * sets a parameter in them is a short list: a NestLoop's nestParams, a
 * SubPlan -- its setParam as an initplan, its parParam from its arguments,
 * its paramIds from its own output -- a RecursiveUnion's work table, and a
 * Partition Selector.  What reads one is a Param, a CteScan's cteParam, a
 * WorkTableScan's wtParam, and a Dynamic Scan's selectors.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"

#include "optimizer/walkers.h"

#include "cb_compat.h"
#include "cb_dynamicscan.h"
#include "cb_motion.h"
#include "gp_motion.h"

typedef struct motion_check_context
{
	plan_tree_base_prefix base;	/* plan_tree_walker's, first */
	bool		in_fragment;
	Bitmapset  *referenced;
	Bitmapset  *produced;
	int			problem;
	List	  **order;			/* the enclosing Gather's Motions, senders first */
	bool		may_write;		/* the fragment a write is dispatched as */
	int			slice;			/* the fragment's slice, where in one */
} motion_check_context;

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
		bool		gather = type == GP_MOTION_GATHER || type == GP_MOTION_DML;
		List	   *order = NIL;
		motion_check_context sub;

		/*
		 * A Gather's rows go to the coordinator, from where it runs; so do a
		 * dispatched write's counts.
		 */
		if (gather && ctx->in_fragment)
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
		sub.produced = NULL;
		sub.problem = GP_ORCA_MOTION_OK;
		sub.order = gather ? &order : ctx->order;
		sub.may_write = type == GP_MOTION_DML;
		sub.slice = api->motion_slice(plan);
		if (motion_check_walker((Node *) plan->lefttree, &sub))
		{
			ctx->problem = sub.problem;
			return true;
		}
		if (!bms_is_subset(sub.referenced, sub.produced))
		{
			ctx->problem = GP_ORCA_MOTION_PARAM;
			return true;
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

	if (ctx->in_fragment)
	{
		switch (nodeTag(node))
		{
			case T_Param:
				{
					Param	   *param = (Param *) node;

					if (param->paramkind == PARAM_EXTERN)
					{
						ctx->problem = GP_ORCA_MOTION_EXTERN;
						return true;
					}
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
				ctx->referenced = bms_add_member(ctx->referenced,
												 ((CteScan *) node)->cteParam);
				break;
			case T_WorkTableScan:
				ctx->referenced = bms_add_member(ctx->referenced,
												 ((WorkTableScan *) node)->wtParam);
				break;
			case T_ModifyTable:
				if (!ctx->may_write)
				{
					ctx->problem = GP_ORCA_MOTION_WRITE;
					return true;
				}
				break;
			case T_CustomScan:
				{
					CustomScan *cscan = (CustomScan *) node;

					if (cscan->methods == &gp_orca_partition_selector_methods)
						ctx->produced =
							bms_add_member(ctx->produced,
										   intVal(linitial(cscan->custom_private)));
					else if (cscan->methods == &gp_orca_dynamic_scan_methods)
						ctx->referenced =
							add_int_list(ctx->referenced,
										 (List *) lsecond(cscan->custom_private));
					break;
				}
			default:
				break;
		}
	}

	return plan_tree_walker(node, motion_check_walker, ctx, true);
}

int
gp_orca_check_motions(PlannedStmt *stmt)
{
	motion_check_context ctx;

	exec_init_plan_tree_base(&ctx.base, stmt);
	ctx.in_fragment = false;
	ctx.referenced = NULL;
	ctx.produced = NULL;
	ctx.problem = GP_ORCA_MOTION_OK;
	ctx.order = NULL;
	ctx.may_write = false;
	ctx.slice = -1;

	(void) motion_check_walker((Node *) stmt->planTree, &ctx);
	return ctx.problem;
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
		ListCell   *lm;

		/* a Gather or a write that direct dispatch sent to one segment says which */
		foreach(lm, motions)
		{
			Plan	   *motion = (Plan *) lfirst(lm);

			if (api->motion_slice(motion) == slice->sliceIndex &&
				((api->motion_type(motion) == GP_MOTION_GATHER &&
				  slice->gangType == GANGTYPE_PRIMARY_READER) ||
				 (api->motion_type(motion) == GP_MOTION_DML &&
				  slice->gangType == GANGTYPE_PRIMARY_WRITER)) &&
				api->motion_segment(motion) >= 0)
				direct = api->motion_segment(motion);
		}

		table = lappend(table,
						list_make5(makeInteger(slice->sliceIndex),
								   makeInteger(slice->parentIndex),
								   makeInteger((int) slice->gangType),
								   makeInteger(slice->numsegments),
								   makeInteger(slice->segindex)));
		llast(table) = lappend((List *) llast(table), makeInteger(direct));
	}

	return (Node *) makeDefElem(pstrdup(GP_SLICE_TABLE), (Node *) table, -1);
}
