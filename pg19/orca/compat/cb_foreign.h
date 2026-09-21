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
 * compat/cb_foreign.h
 *	  A ForeignScan for a plan the planner did not make.
 *
 * Cloudberry's BuildForeignScan() is in its foreign/foreign.c, a PostgreSQL
 * file it patched, and relies on a second patch, to clausesel.c, which
 * skips the selectivity estimate for the half-built PlannerInfo it hands the
 * foreign-data wrapper.  PostgreSQL 19 has neither, so the port's builds a
 * PlannerInfo the wrapper's planning callbacks can use as they would the
 * planner's.  See foreign.c.
 *
 * Called from gpdb::CreateForeignScan, for DXL to PlannedStmt.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_FOREIGN_H
#define CB_FOREIGN_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/*
 * The ForeignScan of relation `relid`, whose range table entry `rte` is at
 * `scanrelid` in the plan's range table, returning `targetlist` and
 * applying `qual` -- both in terms of that entry -- as its wrapper plans it.
 * `query` is the statement being planned, with the plan's permission entries
 * in place of its own, so that `rte` finds its entry.
 */
extern ForeignScan *BuildForeignScan(Oid relid, Index scanrelid, List *qual,
									 List *targetlist, Query *query,
									 RangeTblEntry *rte);

#endif							/* CB_FOREIGN_H */
