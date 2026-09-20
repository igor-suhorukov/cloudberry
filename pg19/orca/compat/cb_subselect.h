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
 * compat/cb_subselect.h
 *	  May an ANY SubLink's test expression be hashed?
 *
 * ORCA decides this for itself, when it turns a DXL subplan back into a
 * PostgreSQL SubPlan: a hashed subplan builds the subquery's output into a
 * hash table once instead of re-running the comparison per outer row, and
 * whether that is allowed depends on the operator being hashable and strict
 * and on which side of it the subquery's Params appear.
 *
 * PostgreSQL asks the same question, in subselect.c, and keeps the answer
 * static.  As with compat/selfuncs.c, Cloudberry's change to the file is to
 * export it -- see cb_selfuncs.h for why cloudberry.md says neither file is
 * needed, and why that is wrong.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_SUBSELECT_H
#define GP_ORCA_COMPAT_CB_SUBSELECT_H

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

/*
 * param_ids is the subquery's list of output Param ids, which is how the
 * left and right sides are told apart: a Param the subquery supplies may not
 * appear on the left, and a Var of the outer query may not appear on the
 * right.
 */
extern bool testexpr_is_hashable(Node *testexpr, List *param_ids);

#endif							/* GP_ORCA_COMPAT_CB_SUBSELECT_H */
