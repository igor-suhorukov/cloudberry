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
 * gp_orca_lockrows.h
 *	  SELECT ... FOR UPDATE, FOR SHARE and the rest, under ORCA.
 *
 * See lockrows.c.  Called from orca.c: the query before ORCA is handed it,
 * the plan after.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_LOCKROWS_H
#define GP_ORCA_LOCKROWS_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/*
 * Make `query` ready for ORCA, in place: each row it is to lock gets its
 * ctid as an output column, and on a cluster where rows are not locked the
 * tables are, as Cloudberry locks them.  *marks is what the plan is to lock,
 * for GpOrcaAddLockRows.  False, with *why, for a query whose locks ORCA's
 * plan cannot take; it goes to the planner.
 */
extern bool GpOrcaPrepareRowMarks(Query *query, List **marks, const char **why);

/*
 * Put the LockRows node that takes those locks into ORCA's plan: at the top
 * on one node, below the Gather on the segments on a cluster.  False, with
 * *why, when the plan is not one it can go into.
 */
extern bool GpOrcaAddLockRows(PlannedStmt *stmt, List *marks, const char **why);

#endif							/* GP_ORCA_LOCKROWS_H */
