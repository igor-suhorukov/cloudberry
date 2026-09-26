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
 * gp_refresh.h
 *	  A materialized view on a cluster, its rows on the segments.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_REFRESH_H
#define GP_REFRESH_H

#include "nodes/parsenodes.h"
#include "tcop/utility.h"

/* Is this relation a materialized view whose rows are on the segments? */
extern bool GpRefreshIsDistributed(Oid relid);

/*
 * The coordinator: does REFRESH of this view need more than the statement
 * every node runs -- its rows, or the concurrent kind's lock?  Then
 * GpRefreshMatView() carries it out.
 */
extern bool GpRefreshNeedsFill(RefreshMatViewStmt *stmt);
extern void GpRefreshMatView(PlannedStmt *pstmt, const char *queryString,
							 ProcessUtilityContext context,
							 ParamListInfo params, QueryEnvironment *queryEnv,
							 QueryCompletion *qc);

/*
 * A segment: is this the COPY that brings the view being filled its rows?
 * Then where they go instead -- a temporary table, from which they are moved
 * into the view -- and GpRefreshRunCopy() runs it, pointed there, as the
 * view's owner.
 */
extern RangeVar *GpRefreshFillTarget(CopyStmt *stmt);
extern void GpRefreshRunCopy(void (*run) (void *arg), void *arg);

/*
 * The coordinator, as a plan starts: a materialized view it reads from the
 * segments that has not been populated is refused here, in PostgreSQL's
 * words, as a scan of this node's copy would refuse it.
 */
extern void GpRefreshCheckScannable(struct PlannedStmt *stmt, int eflags);

#endif							/* GP_REFRESH_H */
