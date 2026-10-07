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
 * nodes one for one, and a Motion only where it stands -- plan node ids
 * stay the translator's, and a fragment's target list keeps its resnos and
 * types.
 *
 * A Motion the engine is offered it may rebuild where it stands, through
 * gp_core's API, with nodes of its own above and below it -- vexec's
 * frames, a vector node's batches across the Motion as Arrow IPC frames
 * (pg_vector_executor.md §3.10, V7): the Motion's rows become its
 * fragment's columns, all NULL, a frame's segment and the frame.  It is
 * the same node, which the translator's own list of the plan's Motions
 * still names for direct dispatch and the slice table; the check of the
 * Motions, which walks the tree, sees the new fragment below it and the
 * engine's node above it, and the engine numbers the nodes it added once
 * the plan is made.
 *
 * The engine decides each node from the node alone, children first, so
 * that a node over a vector child can take its batches.  It is offered the
 * whole tree after it is built rather than each node as it is built, since
 * the translator looks at children it has built: PlaceResultFilter moves a
 * Result's filter into a SeqScan below it (CTranslatorDXLToPlStmt.cpp),
 * which a node replaced too early would change.
 *
 * From the API's minor version 2, the engine also prices ORCA's search
 * and builds what only it runs.  Before ORCA is asked, the engine sets the
 * statement's options (gp_orca_vector_options(), gp_orca_planner.c);
 * during ORCA's search, CCostModelVec (cost/CCostModelVec.cpp) asks it for
 * its prices and its oracle through the functions below, which the
 * translator's wrappers call (gpdbwrappers.cpp); and a hashed window the
 * translator lowered -- a WindowAgg over the Sort that brings its
 * partitions together -- is offered to it whole, once its input's nodes
 * have been offered, before its Sort and its WindowAgg would be (walk()).
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

/*
 * ORCA's options for the statement: what gp_orca uses without the engine
 * in *options, which the engine changes for its plans (set_options).
 */
void
gp_orca_vector_options(GpOrcaVecOptions *options)
{
	if (engine_state != NULL && GP_ORCA_VEC_HAS(engine, set_options))
		engine->set_options(engine_state, options);
}

/* The engine's prices for the statement, for CCostModelVec; false: none. */
bool
gp_orca_vector_costs(GpOrcaVecCosts *costs)
{
	memset(costs, 0, sizeof(GpOrcaVecCosts));
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, cost_factors) ||
		!GP_ORCA_VEC_HAS(engine, cost_call) ||
		!GP_ORCA_VEC_HAS(engine, cost_aggregate) ||
		!GP_ORCA_VEC_HAS(engine, cost_relation) ||
		!GP_ORCA_VEC_HAS(engine, cost_hash_key))
		return false;
	return engine->cost_factors(engine_state, costs);
}

/* The engine's oracle, which CCostModelVec asks during ORCA's search. */
int
gp_orca_vector_cost_call(Oid funcid, Oid opno, int nargs, const Oid *argtypes,
						 Oid collation)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, cost_call))
		return GP_ORCA_VEC_STEP_REFUSED;
	return engine->cost_call(engine_state, funcid, opno, nargs, argtypes, collation);
}

int
gp_orca_vector_cost_aggregate(Oid aggfnoid, int nargs, const Oid *argtypes,
							  bool distinct, bool ordered)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, cost_aggregate))
		return GP_ORCA_VEC_STEP_REFUSED;
	return engine->cost_aggregate(engine_state, aggfnoid, nargs, argtypes,
								  distinct, ordered);
}

int
gp_orca_vector_cost_relation(Oid relid)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, cost_relation))
		return GP_ORCA_VEC_REL_NONE;
	return engine->cost_relation(engine_state, relid);
}

bool
gp_orca_vector_cost_hash_key(Oid eqop, Oid collation)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, cost_hash_key))
		return false;
	return engine->cost_hash_key(engine_state, eqop, collation);
}

static Plan *walk(Plan *plan, List *rtable, List *windows);

/* Each plan of a list, walked in place. */
static void
walk_list(List *plans, List *rtable, List *windows)
{
	ListCell   *lc;

	foreach(lc, plans)
		lfirst(lc) = walk((Plan *) lfirst(lc), rtable, windows);
}

/* A node's children, each walked in place. */
static void
walk_children(Plan *plan, List *rtable, List *windows)
{
	switch (nodeTag(plan))
	{
		case T_Append:
			walk_list(((Append *) plan)->appendplans, rtable, windows);
			break;
		case T_MergeAppend:
			walk_list(((MergeAppend *) plan)->mergeplans, rtable, windows);
			break;
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan =
				walk(((SubqueryScan *) plan)->subplan, rtable, windows);
			break;
		case T_CustomScan:
			walk_list(((CustomScan *) plan)->custom_plans, rtable, windows);
			break;
		default:
			break;
	}
	plan->lefttree = walk(plan->lefttree, rtable, windows);
	plan->righttree = walk(plan->righttree, rtable, windows);
}

/* A node, its children walked, offered to the engine. */
static Plan *
offer(Plan *plan, List *rtable)
{
	Plan	   *built = engine->build_node(engine_state, plan, rtable);

	return built != NULL ? built : plan;
}

/*
 * A node's children first, then the node itself, offered to the engine.  A
 * hashed window the translator lowered (windows) is offered whole once its
 * input's nodes are, and where the engine keeps the lowering, its Sort and
 * its WindowAgg are offered in turn, as other nodes are.
 */
static Plan *
walk(Plan *plan, List *rtable, List *windows)
{
	if (plan == NULL)
		return NULL;
	check_stack_depth();

	if (IsA(plan, WindowAgg) && list_member_ptr(windows, plan) &&
		plan->lefttree != NULL && IsA(plan->lefttree, Sort) &&
		GP_ORCA_VEC_HAS(engine, build_window))
	{
		Plan	   *sort = plan->lefttree;
		Plan	   *built;

		walk_children(sort, rtable, windows);
		built = engine->build_window(engine_state, (WindowAgg *) plan, rtable);
		if (built != NULL)
			return built;
		plan->lefttree = offer(sort, rtable);
		return offer(plan, rtable);
	}

	walk_children(plan, rtable, windows);
	return offer(plan, rtable);
}

Plan *
gp_orca_vector_plan(Plan *plan, List *subplans, List *rtable, List *windows)
{
	if (engine_state == NULL || !GP_ORCA_VEC_HAS(engine, build_node))
		return plan;
	walk_list(subplans, rtable, windows);
	return walk(plan, rtable, windows);
}
