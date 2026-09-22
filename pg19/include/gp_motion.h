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
 * gp_motion.h
 *	  ORCA's Gather Motion, which gp_core carries out; see gp_motion.c.
 *
 * ORCA's translator reaches these through gp_core_api.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_MOTION_H
#define GP_MOTION_H

#include "postgres.h"

#include "nodes/plannodes.h"

/* The CustomScan provider's name, which a serialized plan carries. */
#define GP_MOTION_NAME		"GpMotion"

/* What a Motion does with the rows its senders send: Cloudberry's kinds. */
#define GP_MOTION_GATHER		0	/* to the coordinator */
#define GP_MOTION_HASH			1	/* each to the segment its keys hash to */
#define GP_MOTION_BROADCAST		2	/* each to every segment */
#define GP_MOTION_RANDOM		3	/* each to the next segment in turn */

/* A Motion whose sender is the coordinator, where "content" names a segment. */
#define GP_MOTION_FROM_COORDINATOR	(-2)

/*
 * Can ORCA's plans with a Motion be carried out from this backend?  The
 * coordinator, with a cluster secret, in a database gp_core is installed in.
 */
extern bool GpMotionCanDispatchPlans(void);

/*
 * A Gather Motion over a plan fragment.  "targetlist" and "qual" are the
 * Motion's own, reading the fragment's output as OUTER_VAR, as ORCA's
 * translator builds them.  "content" is the one segment it reads from, or -1
 * for every one; "slice" the slice it receives, for EXPLAIN.  With nkeys > 0
 * it merges the segments' sorted streams, the keys being columns of the
 * Motion's target list.  NULL when a key is not a column passed through.
 */
extern Plan *GpMotionMakeGather(Plan *fragment, List *targetlist, List *qual,
								int content, int slice, int nkeys,
								const AttrNumber *keys, const Oid *sortops,
								const Oid *collations, const bool *nullsfirst);

/*
 * A Motion between segments over a plan fragment: GP_MOTION_HASH, whose
 * "hashexprs" read the fragment's output as OUTER_VAR and are hashed with
 * "hashfuncs" as cdbhash hashes a table's key; GP_MOTION_BROADCAST or
 * GP_MOTION_RANDOM, with neither.  "content" is the one segment that sends,
 * -1 for every one, or GP_MOTION_FROM_COORDINATOR.  Every segment receives.
 */
extern Plan *GpMotionMakeSend(int type, Plan *fragment, List *targetlist,
							  List *qual, int content, int slice,
							  List *hashexprs, List *hashfuncs);

/*
 * Cloudberry's Result with hash filters: "child"'s rows, projected by
 * "targetlist" and filtered by "qual" (both reading the child as OUTER_VAR),
 * kept on the segment their output columns "cols" hash to with "hashfuncs";
 * with nkeys 0, kept on "segment" only.  Not a Motion: nothing moves.
 */
extern Plan *GpMotionMakeHashFilter(Plan *child, List *targetlist, List *qual,
									int nkeys, const AttrNumber *cols,
									const Oid *hashfuncs, int segment);

/* Its kind, and the slice that sends. */
extern int	GpMotionType(Plan *plan);
extern int	GpMotionSlice(Plan *plan);

/*
 * A Gather's Motions between segments, by the slices that send them, in the
 * order they are to be carried out before the Gather sends its fragment.
 */
extern void GpMotionSetPrepare(Plan *plan, List *slices);

/* Is this plan node one, and which segment it reads from; and set that. */
extern bool GpMotionIs(Plan *plan);
extern int	GpMotionSegment(Plan *plan);
extern void GpMotionSetSegment(Plan *plan, int content);

/*
 * The segment holding every row of relation "relid" whose distribution key
 * is these values, in the key's order; -1 when that is not one segment or
 * cannot be said.
 */
extern int	GpMotionDirectDispatchSegment(Oid relid, int nvalues,
										  const Oid *types,
										  const Datum *values,
										  const bool *isnull);

/* The CustomScan, and the segments' planner hook; from gp_core's _PG_init. */
extern void GpMotionInit(void);

#endif							/* GP_MOTION_H */
