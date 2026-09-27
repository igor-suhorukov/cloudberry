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

#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

/* The CustomScan provider's name, which a serialized plan carries. */
#define GP_MOTION_NAME		"GpMotion"

/* What a Motion does with the rows its senders send: Cloudberry's kinds. */
#define GP_MOTION_GATHER		0	/* to the coordinator, or one segment */
#define GP_MOTION_HASH			1	/* each to the segment its keys hash to */
#define GP_MOTION_BROADCAST		2	/* each to every segment */
#define GP_MOTION_RANDOM		3	/* each to the next segment in turn */
#define GP_MOTION_DML			4	/* a write the segments carry out */
#define GP_MOTION_EXPLICIT		5	/* each to the segment a column of it names:
									 * Cloudberry's Explicit Redistribute */

/* A Motion whose sender is the coordinator, where "content" names a segment. */
#define GP_MOTION_FROM_COORDINATOR	(-2)

/*
 * How many readers a segment takes for one session (gp_dispatch.c): the
 * slices of a statement it runs at once, less the one its writer runs.
 */
#define GP_MAX_READERS_PER_SEGMENT	64

/*
 * On a statement's PlannedStmt, a DefElem of its extension_state that ORCA's
 * translator adds: for each of the statement's subplans, in order, the slice
 * that calls it -- where its own part, above any Motion in it, runs -- in an
 * IntList; GP_SUBPLAN_COORDINATOR for the coordinator's own part, and
 * GP_SUBPLAN_UNKNOWN where no one slice does.  A Gather relays only the
 * slice that calls a subplan whose own part scans a temporary table, which
 * the writer alone can read, rather than every slice below it.
 */
#define GP_SUBPLAN_SLICES			"gp_subplan_slices"
#define GP_SUBPLAN_COORDINATOR		(-1)
#define GP_SUBPLAN_UNKNOWN			(-2)

/*
 * Another the translator adds where ORCA shares a CTE in a slice the
 * segments run (gp_orca's compat/sharedscan.c): for each, the slices its
 * producer and consumers are in, an IntList, in a List.  Its rows are kept
 * in files named after the key of the Gather that sends the slices, which
 * so has one; and where they are more than one slice, each has to run at
 * the same time as the others -- a consumer waits for its producer -- so a
 * Gather that would relay one of them, a slice at a time, refuses the
 * statement instead of waiting for ever.
 */
#define GP_SHARE_SLICES				"gp_share_slices"

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
 * In a fragment a segment runs, it gathers to that segment, the one its
 * slice runs on, which receives it as a Motion between segments -- and
 * sorts what came, rather than merge it.
 */
extern Plan *GpMotionMakeGather(Plan *fragment, List *targetlist, List *qual,
								int content, int slice, int nkeys,
								const AttrNumber *keys, const Oid *sortops,
								const Oid *collations, const bool *nullsfirst);

/*
 * A Motion between segments over a plan fragment: GP_MOTION_HASH, whose
 * "hashexprs" read the fragment's output as OUTER_VAR and are hashed with
 * "hashfuncs" as cdbhash hashes a table's key; GP_MOTION_EXPLICIT, whose one
 * "hashexprs" is the segment each row goes to -- its gp_segment_id -- and
 * "hashfuncs" one InvalidOid; GP_MOTION_BROADCAST or GP_MOTION_RANDOM, with
 * neither.  "content" is the one segment that sends,
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

/*
 * A write of a distributed table, where its rows are: "modify", a
 * ModifyTable, runs on the segments -- every one, or "content" -- in the
 * slice Cloudberry calls its writer gang, and the coordinator counts the
 * rows they changed; with RETURNING, the rows it gives come back through
 * it, as a Gather's do.  Carries out the Motions below it first, as a
 * Gather.
 */
extern Plan *GpMotionMakeDml(Plan *modify, int content, int slice);

/*
 * ORCA's Split, for an UPDATE of a distribution key (gp_split.c): each row of
 * "child" as a DELETE of its old values and an INSERT of its new ones, the
 * columns of the output being "deletecols" and "insertcols" of the child's,
 * and "actioncol" the action.  And what applies them on a segment: the rows
 * of "child", the table's "natts" attributes first, then the action and the
 * ctid, against range table entry "rti".
 */
extern Plan *GpSplitMake(Plan *child, List *targetlist, List *deletecols,
						 List *insertcols, AttrNumber actioncol);
extern Plan *GpSplitModifyMake(Plan *child, Index rti, int natts,
							   AttrNumber actioncol, AttrNumber ctidcol);

/*
 * A partitioned table's split update: its rows' tableoid column, the
 * partition each DELETE's row is in; each INSERT is routed as an INSERT into
 * the table is.
 */
extern void GpSplitModifySetTableOid(Plan *plan, AttrNumber tableoidcol);
extern bool GpSplitModifyIs(Plan *plan, Index *rti);
extern void GpMotionRefuseRecheck(void);
struct ExplainState;
extern bool GpSplitExplainLabel(PlanState *planstate, struct ExplainState *es,
								const char **pname, const char **suffix);
extern void GpSplitInit(void);

/* Its kind, and the slice that sends. */
extern int	GpMotionType(Plan *plan);
extern int	GpMotionSlice(Plan *plan);

/*
 * The slice that receives a Motion between segments, or a Gather into one
 * -- the slice of the fragment it is in, which the translator knows -- so
 * that its senders can stream to the processes running that slice.
 */
extern void GpMotionSetParent(Plan *plan, int parent);
extern int	GpMotionParent(Plan *plan);

/*
 * The parameters a Motion's fragment reads that nothing in it sets, by id:
 * PARAM_EXEC ones the coordinator sets -- an initplan's value, a nested
 * loop's outer column -- and the statement's own.  Their values travel with
 * the fragment each time it is sent.
 */
extern void GpMotionSetParams(Plan *plan, List *exec_params,
							  List *extern_params);

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
 * A Gather's or a write's segments, where direct dispatch sends it to
 * several: their content ids, in the order its INFO line names them.
 */
extern void GpMotionSetSegments(Plan *plan, List *contents);
extern List *GpMotionSegments(Plan *plan);

/*
 * The segment holding every row of relation "relid" whose distribution key
 * is these values, in the key's order; -1 when that is not one segment or
 * cannot be said.
 */
extern int	GpMotionDirectDispatchSegment(Oid relid, int nvalues,
										  const Oid *types,
										  const Datum *values,
										  const bool *isnull);

/*
 * The FileSet this segment keeps the rows of a statement's CTEs in, where
 * ORCA reads one in more than one slice (GpCoreApi.share_fileset); false
 * where "stmt" is no fragment.
 */
struct FileSet;
extern bool GpMotionShareFileSet(PlannedStmt *stmt, struct FileSet *fileset);

/* The CustomScan, and the segments' planner hook; from gp_core's _PG_init. */
extern void GpMotionInit(void);

#endif							/* GP_MOTION_H */
