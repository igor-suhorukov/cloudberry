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
 * gp_orca_vector.h
 *	  See vector.c.  Called from gp_orca's planner_hook, around ORCA's
 *	  planning of a statement; from ORCA's cost model, CCostModelVec, during
 *	  its search; and from the translator, once it has built the plan.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_VECTOR_H
#define GP_ORCA_VECTOR_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

#include "gp_orca_vec.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ExplainState;

/*
 * Around ORCA's planning of one statement: whether a vector engine wants
 * it, and its state while it is translated.  gp_orca_vector_end() is
 * called whether ORCA made a plan (stmt) or not (NULL).
 */
extern void gp_orca_vector_begin(Query *parse, int cursorOptions,
								 struct ExplainState *es);
extern void gp_orca_vector_end(PlannedStmt *stmt);

/* ORCA's options for the statement, as the engine wants them. */
extern void gp_orca_vector_options(GpOrcaVecOptions *options);

/*
 * During ORCA's search: the engine's prices for the statement, false for
 * none, and its oracle (gp_orca_vec.h).
 */
extern bool gp_orca_vector_costs(GpOrcaVecCosts *costs);
extern int	gp_orca_vector_cost_call(Oid funcid, Oid opno, int nargs,
									 const Oid *argtypes, Oid collation);
extern int	gp_orca_vector_cost_aggregate(Oid aggfnoid, int nargs,
										  const Oid *argtypes, bool distinct,
										  bool ordered);
extern int	gp_orca_vector_cost_relation(Oid relid);
extern bool gp_orca_vector_cost_hash_key(Oid eqop, Oid collation);

/*
 * The translated plan and its subplans, each node offered to the engine,
 * children first, in place, and each hashed window the translator lowered
 * (windows, its WindowAggs) offered whole; the plan's new top.
 */
extern Plan *gp_orca_vector_plan(Plan *plan, List *subplans, List *rtable,
								 List *windows);

#ifdef __cplusplus
}
#endif

#endif							/* GP_ORCA_VECTOR_H */
