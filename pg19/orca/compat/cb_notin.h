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
 * compat/cb_notin.h
 *	  NOT IN as PostgreSQL's planner makes it: a hashed SubPlan.
 *
 * See compat/notin.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_NOTIN_H
#define CB_NOTIN_H

#include "nodes/plannodes.h"

/*
 * The plan of the SubPlan that stands for ORCA's NOT IN anti-join over the
 * inner side "inner": a Result giving, one column each, the inner
 * expressions of the join's conditions "clauses" -- each an equality of an
 * outer expression and an inner one, as a hash join has them -- and in
 * *testexpr the SubPlan's test, each outer expression against the PARAM_EXEC
 * of "paramids" that stands for its inner one.  *hashable says whether the
 * SubPlan may hash the inner rows, as the planner's would.  NULL where a
 * condition is not such an equality.
 */
extern Plan *gp_orca_not_in_subplan(List *clauses, Plan *inner,
									List *paramids, Expr **testexpr,
									bool *hashable);

#endif							/* CB_NOTIN_H */
