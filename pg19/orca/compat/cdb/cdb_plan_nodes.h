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
 *	  The slice table's types, which Cloudberry adds to nodes/plannodes.h.
 *
 * This is the one part of Cloudberry's plannodes.h additions that the
 * single-node translator needs, and it is not MPP.  Every plan ORCA's
 * translator builds starts with a slice -- GetPlannedStmtFromDXL makes one,
 * and CContextDXLToPlStmt keeps a current slice throughout -- and on one node
 * that slice is the only one: GANGTYPE_UNALLOCATED, one segment, the
 * coordinator running the whole plan.  So these types say "the plan runs
 * here", and the code that uses them is the same code at M1 and at M2.
 *
 * PostgreSQL 19's PlannedStmt has no field to hold a slice table and the port
 * may not add one (rule 2 of "Core patches that keep vanilla behaviour").  At
 * M2 it goes in PlannedStmt.extension_state, which PostgreSQL 19 provides for
 * this.  At M1 there is one slice and nothing dispatches, so nothing reads it.
 *
 * WHAT IS NOT HERE: THE DISTRIBUTED LAYER.
 *
 * Motion, SplitUpdate, and the slice-to-gang assignment and direct dispatch
 * that go with them, are M2.  ORCA plans none of them at M1, and not because
 * gp_core reports one segment -- one segment does not stop ORCA adding a
 * Motion.  What stops it is COptTasks::OptimizeTask, which sets
 * EopttraceDisableMotions for any query that touches no distributed table;
 * with that flag set ORCA adds no Motion enforcer, and every distribution spec
 * in its core asserts that it did not.  On one node no table is distributed,
 * because the relcache translator reports every relation as coordinator-only
 * there, whatever policy its label records.
 *
 * So the translator methods that would build one raise at M1, as the M2
 * wrappers in gpdbwrappers.cpp do, and a query that reached one would become
 * a counted fallback with a reason rather than a plan the executor rejects.
 * At M2 they are written against Track B's Motion CustomScan -- a Motion
 * reaches the executor as a CustomScan with an ExtensibleNode behind it, the
 * only shape an extension has -- from Cloudberry's originals, which stay
 * untouched at their paths under src/backend/gpopt/ for exactly that.
 *
 * AND WHAT IS NOT HERE BECAUSE IT HAS A TRANSLATION OF ITS OWN.
 *
 * ORCA also emits nodes that are not MPP at all and that PostgreSQL 19 can
 * express another way.  Each is translated when the operators it belongs to
 * are, and none gets a stub here, because a stub would hide the translation
 * that has to be written:
 *
 *	 ShareInputScan		   ORCA's CTE producer and consumer; a CTE scan or a
 *						   subplan.
 *	 Sequence			   folded away; its children become the plan.
 *	 DynamicSeqScan and    a CustomScan over PostgreSQL 19's run-time pruning,
 *	 the other dynamic	   whose API it exports (executor/execPartition.h),
 *	 scans, and			   as pgorca already does.
 *	 PartitionSelector
 *	 AssertOp			   a check on the rows, with the DML operators.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CDB_PLAN_NODES_H
#define GP_ORCA_COMPAT_CDB_PLAN_NODES_H

#include "postgres.h"

#include "nodes/pg_list.h"

/*
 * How a slice's processes are arranged.  On one node every slice is
 * GANGTYPE_UNALLOCATED; the other four arrive with M2.  The enum is carried
 * whole all the same, because TranslateDXLDml writes GANGTYPE_PRIMARY_WRITER
 * for a distributed target table -- a branch that is dead on one node for the
 * same reason Motion is.
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
 * Which segments a slice has to be sent to, when it is fewer than all of
 * them.  Deciding that is M2 work; the struct is here because it is a field
 * of PlanSlice, and a PlanSlice is built for every plan.
 */
typedef struct DirectDispatchInfo
{
	bool		isDirectDispatch;
	List	   *contentIds;
	bool		haveProcessedAnyCalculations;	/* planner-only bookkeeping */
} DirectDispatchInfo;

/* One slice of a plan. */
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

#endif							/* GP_ORCA_COMPAT_CDB_PLAN_NODES_H */
