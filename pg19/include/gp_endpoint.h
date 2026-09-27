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
 * gp_endpoint.h
 *	  Parallel retrieve cursors: DECLARE ... PARALLEL RETRIEVE CURSOR, its
 *	  endpoints, and RETRIEVE ... FROM ENDPOINT in a retrieve session.
 *
 * What gp_core's own files share of it; gp_endpoint.c says how it works.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ENDPOINT_H
#define GP_ENDPOINT_H

#include "postgres.h"

#include "executor/execdesc.h"
#include "nodes/plannodes.h"
#include "tcop/dest.h"

/*
 * Where a cursor's endpoints are, as the plan says it (Cloudberry's
 * EndPointExecPosition): the coordinator, the one segment a replicated
 * table's rows are read on, the segments direct dispatch sends it to, or
 * every segment.
 */
typedef enum GpEndpointKind
{
	GP_ENDPOINT_COORDINATOR,
	GP_ENDPOINT_SINGLE,
	GP_ENDPOINT_SOME,
	GP_ENDPOINT_ALL,
} GpEndpointKind;

/*
 * The mark a parallel retrieve cursor's plan carries in extension_state:
 * (kind, the segments' content ids, the Gather its top slice was below) --
 * the Gather only where the endpoints are on the segments.  And the mark on
 * the fragment an endpoint's reader runs: the endpoint's name.
 */
#define GP_ENDPOINT_MARK		"gp_endpoint"
#define GP_ENDPOINT_RUN_MARK	"gp_endpoint_run"

/*
 * The planner's part, from gp_orca through the API and from gp_core's own
 * planner hook: decide where the endpoints are, take the Gather above the
 * top slice off where they are on the segments, and mark the plan.
 */
extern void GpEndpointPlan(PlannedStmt *stmt);

/* The mark of a plan, or NULL where it is no parallel retrieve cursor's. */
extern List *GpEndpointPlanMark(PlannedStmt *stmt);

/*
 * gp_motion.c, on the coordinator: start the segments' part of a parallel
 * retrieve cursor, the top slice the recorded Gather sent, and answer the
 * key its Motions' rows are kept under, or NULL.  A reader of the held
 * stream on each endpoint's segment ("contents", "readers" set to them) opens
 * the endpoint with "open", which is waited for, and then runs the slice
 * into the endpoint "name", beside the readers of the slices below.
 */
struct GpStream;
extern char *GpEndpointDispatch(QueryDesc *queryDesc, CustomScan *gather,
								struct GpStream *stream, const int *contents,
								int *readers, int nendpoints,
								const char *name, const char *open);

/*
 * On a segment's reader that runs an endpoint's slice (gp_motion.c's
 * ExecutorRun hook): the DestReceiver its rows go into, NULL for any other
 * fragment; and, the slice run, the wait for the retrieve session to read
 * them all.
 */
extern DestReceiver *GpEndpointRunDest(QueryDesc *queryDesc);
extern void GpEndpointRunDone(void);

/* Is this backend a retrieve session?  Its role is utility, as in Cloudberry. */
extern bool GpEndpointIsRetrieveSession(void);

/*
 * What "canceling MPP operation" says after it on the segment an endpoint's
 * sender was cancelled on by its retrieve session, or NULL (gp_workfile.c).
 */
extern const char *GpEndpointCancelMessage(void);

/*
 * O26: the statement RETRIEVE { ALL | count } FROM ENDPOINT name is, in a
 * retrieve session -- a SELECT of gp_internal.retrieve() with the endpoint's
 * columns -- through the API, for gp_sql's rewrite.
 */
extern char *GpEndpointRetrieveSql(const char *endpoint, bool all,
								   int64 count);

extern void GpEndpointInit(void);

#endif							/* GP_ENDPOINT_H */
