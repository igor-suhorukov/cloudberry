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
 * compat/cdb/cdb_plan_nodes.h
 *	  Cloudberry's MPP plan nodes, so that the translator compiles whole.
 *
 * Cloudberry adds these to nodes/plannodes.h.  PostgreSQL 19 has none of them
 * and the port may not add one: rule 2 of "Core patches that keep vanilla
 * behaviour" forbids a new node type, and PG19 generates its NodeTag enum from
 * its own sources, so an extension could not add to it even if the rule
 * allowed it.
 *
 * WHY THEY ARE HERE AT ALL, GIVEN M1 EMITS NONE OF THEM.
 *
 * The translator is one body of code and it is Cloudberry's.  Most of what its
 * Motion and slice paths do is not about the node type -- assigning slices,
 * building hash expressions, working out whether a query can be dispatched to
 * one segment -- and that logic is what M2 needs.  Deleting it to bring it back
 * later would be deleting the part that is worth having.  So the types exist,
 * the logic compiles, and at M1 nothing reaches it, because gp_core reports one
 * segment and ORCA plans no Motion for one segment.
 *
 * This is the opposite of what the plan-tree walker did with the same 24 node
 * tags, and the difference is worth stating: the walker *consumes* nodes, so a
 * case for a node that cannot arrive is a case nobody can test.  The translator
 * *produces* them, and the code that produces one carries the reasoning that
 * decides when to.
 *
 * WHAT THESE BECOME AT M2, WHICH IS NOT A Motion.
 *
 * An MPP node reaches the executor as a CustomScan with an ExtensibleNode
 * behind it -- that is Track B's design and the only shape available to an
 * extension.  So every makeNode() of a type below is a call site that changes
 * at M2, not code that starts working then.  The tags exist so the file
 * compiles; they are not a plan the executor will ever be handed.
 *
 * THE TAG VALUES, AND THE ONE HAZARD THEY CARRY.
 *
 * PostgreSQL 19's highest NodeTag is 487 (nodes/nodetags.h, generated).  These
 * start at 5001, far outside it, so a node that escaped into a real plan would
 * stop the executor with "unrecognized node type: 5001" -- a number that can
 * be grepped for and that names the node it came from.  That is the hazard and
 * the whole of it: it is loud, it is at run time, and it cannot be silent,
 * because no core switch has a case for 5001.  pgorca reasons the same way and
 * picks the same range (pgorca/compat/cdb/cdb_plan_nodes.h); the gaps in the
 * numbering below are its, kept so that the two agree.
 *
 * WHAT IS NOT HERE.
 *
 * Nodes ORCA emits that PostgreSQL 19 can express directly do not get a stub,
 * because a stub would hide the translation the port has to write:
 *
 *	 Sequence			   folded away; its children become the plan.
 *	 DynamicSeqScan and    a CustomScan over PG19's own run-time pruning,
 *	 PartitionSelector	   whose API PG19 exports (executor/execPartition.h).
 *	 ShareInputScan		   a subplan, or a CTE scan.
 *
 * Sequence, PartitionSelector and DynamicSeqScan are named in the tag list all
 * the same, because the translator switches on them in places it does not
 * build them.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CDB_PLAN_NODES_H
#define GP_ORCA_COMPAT_CDB_PLAN_NODES_H

#include "postgres.h"

#include "nodes/plannodes.h"

/*
 * Tags.  See the note above on the range and on what happens if one is ever
 * reached.  The gaps are pgorca's and are left unused so the live tags keep
 * the same numbers in both projects.
 */
#define T_Motion				((NodeTag) 5001)
#define T_SplitUpdate			((NodeTag) 5003)
#define T_AssertOp				((NodeTag) 5006)
#define T_DynamicIndexScan		((NodeTag) 5008)
#define T_DynamicIndexOnlyScan	((NodeTag) 5009)
#define T_DynamicBitmapHeapScan ((NodeTag) 5010)
#define T_DynamicBitmapIndexScan ((NodeTag) 5011)
#define T_DynamicForeignScan	((NodeTag) 5012)
#define T_DynamicSeqScan		((NodeTag) 5013)
#define T_PartitionSelector		((NodeTag) 5014)
#define T_ShareInputScan		((NodeTag) 5015)
#define T_Sequence				((NodeTag) 5016)

/* The coordinator's content id.  gp_core reports it through gp.node(). */
#define MASTER_CONTENT_ID		(-1)

/*
 * How a slice's processes are arranged.  At M1 there is one slice and it is a
 * GANGTYPE_UNALLOCATED one: the coordinator, alone, running the whole plan.
 */
typedef enum GangType
{
	GANGTYPE_UNALLOCATED,		/* no gang; runs on the coordinator */
	GANGTYPE_ENTRYDB_READER,	/* one process on the coordinator */
	GANGTYPE_SINGLETON_READER,	/* one process on one segment */
	GANGTYPE_PRIMARY_READER,	/* one process per segment, reading */
	GANGTYPE_PRIMARY_WRITER,	/* one process per segment, writing */
} GangType;

/*
 * Which segments a slice has to be sent to, when it is fewer than all of them.
 *
 * This is the piece of the Motion machinery that is worth carrying even while
 * nothing dispatches: deciding that a query touches one segment is a property
 * of the query, and ORCA works it out.  gp.optimizer_enable_direct_dispatch
 * switches it.
 */
typedef struct DirectDispatchInfo
{
	bool		isDirectDispatch;
	List	   *contentIds;
	bool		haveProcessedAnyCalculations;	/* planner-only bookkeeping */
} DirectDispatchInfo;

/*
 * One slice of a dispatched plan.
 *
 * Cloudberry hangs an array of these off PlannedStmt, in fields the port may
 * not add.  At M2 it lives in PlannedStmt.extension_state, which PostgreSQL 19
 * provides for exactly this (nodes/plannodes.h).  Nothing reads it at M1.
 */
typedef struct PlanSlice
{
	int			sliceIndex;
	int			parentIndex;

	GangType	gangType;

	/* how many segments in the gang, for PRIMARY_READER/WRITER */
	int			numsegments;
	int			parallel_workers;
	/* which segment, for SINGLETON_READER */
	int			segindex;

	DirectDispatchInfo directDispatch;
} PlanSlice;

typedef enum MotionType
{
	MOTIONTYPE_GATHER,			/* every segment to one process */
	MOTIONTYPE_GATHER_SINGLE,	/* as above, but from one segment only */
	MOTIONTYPE_HASH,			/* redistribute on a hash of the key */
	MOTIONTYPE_BROADCAST,		/* every row to every segment */
	MOTIONTYPE_EXPLICIT,		/* to the segment a column names */
	MOTIONTYPE_OUTER_QUERY,		/* to wherever the outer query is */
} MotionType;

/*
 * Moving rows between segments.
 *
 * pg_node_attr() is left off every field: it is Cloudberry's annotation for
 * its own node-support generator, and the port's nodes are not generated --
 * at M2 this becomes an ExtensibleNode with copy/equal/out/read callbacks the
 * port writes.
 */
typedef struct Motion
{
	Plan		plan;

	MotionType	motionType;
	bool		sendSorted;
	int			motionID;

	/* MOTIONTYPE_HASH */
	List	   *hashExprs;
	Oid		   *hashFuncs;
	int			numHashSegments;

	/* MOTIONTYPE_EXPLICIT */
	AttrNumber	segidColIdx;

	/* only when sendSorted */
	int			numSortCols;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	PlanSlice  *senderSliceInfo;
} Motion;

/*
 * A scan of a tuplestore another part of the plan filled.
 *
 * ORCA emits this for a CTE it plans once and reads several times.  It is not
 * an MPP node -- a single-node plan can want one -- so unlike Motion it has a
 * translation at M1 rather than a stub: it becomes a subplan or a CTE scan.
 * The type is here because the translator names it in places that decide
 * whether to produce one.
 */
typedef struct ShareInputScan
{
	Scan		scan;

	bool		cross_slice;
	int			share_id;

	/*
	 * Which slice fills the tuplestore, and which slice this node runs in.
	 * Both may be -1 when the plan has one slice, which at M1 is always.
	 */
	int			producer_slice_id;
	int			this_slice_id;

	int			nconsumers;		/* consumer slices, not counting the producer */
	bool		discard_output; /* true for ORCA's CTE producer */
	bool		ref_set;
} ShareInputScan;

/*
 * Turning an UPDATE of a distribution key into a DELETE and an INSERT, so that
 * the row can move to another segment.  M2; nothing on one node needs it,
 * because a row that stays where it is needs no split.
 */
typedef struct SplitUpdate
{
	Plan		plan;
	AttrNumber	actionColIdx;
	List	   *insertColIdx;
	List	   *deleteColIdx;

	/* how the target segment of an INSERT-action row is computed */
	int			numHashAttrs;
	AttrNumber *hashAttnos;
	Oid		   *hashFuncs;
	int			numHashSegments;
} SplitUpdate;

/*
 * Raising an error when a row reaches this node.  ORCA uses it for constraints
 * it cannot check any other way -- a scalar subquery that returned more than
 * one row, a NOT NULL that a DML plan has to enforce itself.
 */
typedef struct AssertOp
{
	Plan		plan;
	int			errcode;
	List	   *errmessage;
} AssertOp;

/*
 * Running a list of plans one after another.  The port folds it away: its
 * children become the plan, because on one node there is nothing to sequence.
 */
typedef struct Sequence
{
	Plan		plan;
	List	   *subplans;
} Sequence;

/*
 * Deciding at run time which partitions a scan below a join will need.
 *
 * PostgreSQL 19 has its own run-time pruning and exports the API
 * (executor/execPartition.h), so the port's answer is a CustomScan over that
 * rather than this node.  The type is here for the switches that name it.
 */
typedef struct PartitionSelector
{
	Plan		plan;

	struct PartitionPruneInfo *part_prune_info;
	int32		paramid;
} PartitionSelector;

/*
 * A scan whose set of partitions is decided at run time.  Same answer as
 * PartitionSelector: a CustomScan over PG19's pruning.
 */
typedef struct DynamicSeqScan
{
	SeqScan		seqscan;		/* must be first */

	List	   *partOids;
	struct PartitionPruneInfo *part_prune_info;
	List	   *join_prune_paramids;
} DynamicSeqScan;

#endif							/* GP_ORCA_COMPAT_CDB_PLAN_NODES_H */
