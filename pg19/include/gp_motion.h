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
