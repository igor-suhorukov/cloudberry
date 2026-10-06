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
 *	  planning of a statement, and from the translator, once it has built
 *	  the plan.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_VECTOR_H
#define GP_ORCA_VECTOR_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

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

/*
 * The translated plan and its subplans, each node offered to the engine,
 * children first, in place; the plan's new top.
 */
extern Plan *gp_orca_vector_plan(Plan *plan, List *subplans, List *rtable);

#ifdef __cplusplus
}
#endif

#endif							/* GP_ORCA_VECTOR_H */
