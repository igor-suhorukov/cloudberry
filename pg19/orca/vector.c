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
 * vector.c
 *	  NOT IN CLOUDBERRY.  A vectorized executor's nodes in ORCA's plans,
 *	  through gp_orca's API (gp_orca_vec.h; pg_vector_executor.md §3.3.4).
 *
 * Cloudberry's closed engine replaced finished plans node for node, after
 * planning.  An engine that registers here is offered each node of ORCA's
 * plan inside translation instead: once the translator has built the tree
 * from ORCA's DXL, and before anything after it runs -- the check of its
 * Motions and the slice table (CTranslatorDXLToPlStmt.cpp), the port's
 * passes over the finished plan, lockrows.c, merge.c and parallel.c, which
 * never descends into a CustomScan, and the dependency walk (orca.c).  So
 * every later step sees the final tree, and the rules those passes keep
 * hold of it: no Motion is crossed, added or removed -- the engine replaces
 * nodes one for one and never a Motion -- plan node ids stay the
 * translator's, and a fragment's target list keeps its resnos and types.
 *
 * The engine decides each node from the node alone, children first, so
 * that a node over a vector child can take its batches.  It is offered the
 * whole tree after it is built rather than each node as it is built, since
 * the translator looks at children it has built: PlaceResultFilter moves a
 * Result's filter into a SeqScan below it (CTranslatorDXLToPlStmt.cpp),
 * which a node replaced too early would change.
 *
 * The statement's engine state lives from gp_orca's planner_hook to the
 * plan it returns; ORCA plans one statement at a time in a backend
 * (gp_orca_planner.c, orca_depth).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/plannodes.h"

#include "gp_orca_vec.h"
#include "gp_orca_vector.h"

static const GpOrcaVecRoutine *engine = NULL;
static void *engine_state = NULL;

void
gp_orca_vector_begin(Query *parse, int cursorOptions, struct ExplainState *es)
{
	engine = gp_orca_vec_find();
	engine_state = NULL;
	if (engine != NULL && GP_ORCA_VEC_HAS(engine, begin_statement))
		engine_state = engine->begin_statement(parse, cursorOptions, es);
}

void
gp_orca_vector_end(PlannedStmt *stmt)
{
	void	   *state = engine_state;

	engine_state = NULL;
	if (stmt != NULL && state != NULL && GP_ORCA_VEC_HAS(engine, end_statement))
		engine->end_statement(state, stmt);
}

static Plan *walk(Plan *plan, List *rtable);

/* Each plan of a list, walked in place. */
static void
walk_list(List *plans, List *rtable)
{
	ListCell   *lc;

	foreach(lc, plans)
		lfirst(lc) = walk((Plan *) lfirst(lc), rtable);
}

/* A node's children first, then the node itself, offered to the engine. */
static Plan *
walk(Plan *plan, List *rtable)
{
	Plan	   *built;

	if (plan == NULL)
		return NULL;
	check_stack_depth();

	switch (nodeTag(plan))
	{
		case T_Append:
			walk_list(((Append *) plan)->appendplans, rtable);
			break;
		case T_MergeAppend:
			walk_list(((MergeAppend *) plan)->mergeplans, rtable);
			break;
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan = walk(((SubqueryScan *) plan)->subplan, rtable);
			break;
		case T_CustomScan:
			walk_list(((CustomScan *) plan)->custom_plans, rtable);
			break;
		default:
			break;
	}
	plan->lefttree = walk(plan->lefttree, rtable);
	plan->righttree = walk(plan->righttree, rtable);

	built = engine->build_node(engine_state, plan, rtable);
	return built != NULL ? built : plan;
}

Plan *
gp_orca_vector_plan(Plan *plan, List *subplans, List *rtable)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, build_node))
		return plan;
	walk_list(subplans, rtable);
	return walk(plan, rtable);
}
