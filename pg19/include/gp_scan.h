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
 * gp_scan.h
 *	  Reading and writing distributed tables when PostgreSQL's planner plans.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SCAN_H
#define GP_SCAN_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "storage/lockdefs.h"

#include "gp_policy.h"

/*
 * The policy of a table whose rows are on the segments, or NULL for one whose
 * rows are here: on a single node every table, on a cluster a table with no
 * policy or an entry one.
 */
extern GpPolicy *GpScanDistributedPolicy(Oid relid);

/*
 * The segment this session reads a replicated table from.  Every segment of
 * its policy has every row, so sessions are spread over them; it is also the
 * answer to gp_segment_id of such a row.
 */
extern int	GpScanReplicatedContent(const GpPolicy *policy);

/*
 * The segments that hold every row conditions on a distributed table can
 * match -- they fix its key, or its gp_segment_id, to constants -- in the
 * order Cloudberry's direct dispatch names them; NIL for every segment of
 * the table.  varno is the table's range table index in the conditions.
 */
extern List *GpScanDirectDispatchContents(Oid relid, Node *quals, Index varno);

/*
 * SELECT ... FOR UPDATE whose rows the segments lock, with the global
 * deadlock detector on: the locking clause the gather of that relation
 * sends them, for the one planning it is set for (gp_modify.c).
 */
extern void GpScanSetLocking(Oid relid, LockClauseStrength strength,
							 LockWaitPolicy waitPolicy);
extern void GpScanClearLocking(void);

/*
 * The planning in progress is a cursor's: its gathers bring each row's ctid,
 * which WHERE CURRENT OF finds the row by.  Answers what it was.
 */
extern bool GpScanSetCursor(bool cursor);

/*
 * A cursor's gather, its segments' cursors opened as the cursor is declared
 * (gp_motion.c, start_early_walker()): false if ps is not a gather of
 * gp_scan.c's.
 */
extern bool GpGatherScanStartEarly(struct PlanState *ps);

/*
 * As a statement starts: the gathers that the plan may read again keep what
 * they read, and read that again (gp_motion.c) -- but for those that bring
 * each row's ctid.
 */
extern void GpGatherScanMarkRescans(struct PlanState *root);

/*
 * Is this node a gather: its slice, as the executor met it, and how many
 * segments it reads (EXPLAIN's slice table).
 */
extern bool GpGatherScanSlice(struct PlanState *ps, int *slice, int *nsegs);

/*
 * EXPLAIN ANALYZE's end of a gather a LIMIT left open, before the plan is
 * printed; false if ps is not a gather.
 */
extern bool GpGatherScanFinish(struct PlanState *ps);

/*
 * After planning: a gather a LIMIT reads sends the segments the LIMIT
 * (gp_modify.c).
 */
struct PlannedStmt;
extern void GpScanBoundGathers(struct PlannedStmt *stmt);

/*
 * Before a statement is planned: Cloudberry's NOTICE for a NOT IN whose
 * subquery reads a distributed table's ctid without its gp_segment_id,
 * which its planner finds in an anti-join and PostgreSQL's keeps a subplan.
 */
struct Query;
extern void GpScanNoticeSublinkCtid(struct Query *parse);

/* The scan hooks, where there is a cluster; see gp_scan.c. */
extern void GpScanInit(void);

/* The write path, where there is a cluster; see gp_modify.c. */
extern void GpModifyInit(void);

/*
 * Without the global deadlock detector, a write of a partitioned table
 * locks every partition in that mode, as Cloudberry's does (gp_modify.c).
 */
extern void GpModifyLockPartitions(Oid relid, LOCKMODE lockmode);

/*
 * ANALYZE of a distributed table through O3, where there is a cluster, and
 * of a partitioned table's leaves, on one node too (gp_analyze.c).
 */
extern void GpAnalyzeInit(void);

/*
 * After a VACUUM or ANALYZE of distributed tables on the coordinator: the
 * pages, rows and all-visible pages the segments count of them, in the
 * coordinator's pg_class (gp_analyze.c).
 */
struct VacuumStmt;
extern void GpAnalyzeSegmentCounts(struct VacuumStmt *stmt);

/*
 * Around a statement that builds an index on the coordinator -- CREATE
 * INDEX, REINDEX, ALTER TABLE adding a key or an index: the pages, rows and
 * all-visible pages of the distributed tables it builds them on, kept, and
 * put back after it, as Cloudberry's coordinator never writes its own
 * (gp_analyze.c).
 */
extern List *GpAnalyzeKeepCounts(Node *parsetree);
extern void GpAnalyzeRestoreCounts(List *kept);

/*
 * The ctid the coordinator's plan knows a segment's row by -- the row at
 * "tid" on segment "content" -- in the statement "estate" runs: what a
 * gather of a table an UPDATE or DELETE changes gives each row it reads.
 * See gp_explicit.c.
 */
struct EState;
extern void GpRowIdentityMake(struct EState *estate, int content,
							  ItemPointer tid, ItemPointer result);

/*
 * Cloudberry's Explicit Redistribute Motion, in a ModifyTable's place: why
 * one that writes a distributed table cannot be written that way, or NULL;
 * and the node that writes it (gp_explicit.c).
 */
struct PlannedStmt;
struct ModifyTable;
struct Plan;
struct Query;
extern const char *GpExplicitCannot(struct PlannedStmt *stmt,
									struct ModifyTable *mt,
									const char *on_conflict);
extern struct Plan *GpExplicitMake(struct ModifyTable *mt,
								   const char *on_conflict);

/*
 * Over a plan ORCA made, for the explicit write: its column "ctidcol", a
 * row's ctid on the segment in column "contentcol", made the ctid the
 * statement's map knows the row by, the other columns as they come
 * (gp_explicit.c).  And the explicit write in a MERGE's ModifyTable's place,
 * refused as the planner's route refuses it (gp_modify.c).
 */
extern struct Plan *GpRowIdentityNodeMake(struct Plan *child,
										  AttrNumber contentcol,
										  AttrNumber ctidcol);
extern struct Plan *GpModifyWriteExplicitly(struct PlannedStmt *stmt,
											struct Plan *modify);

/*
 * The junk column a MERGE into a distributed table carries the target's row
 * in, for the explicit write's actions to read (gp_modify.c).
 */
#define GP_MERGE_TARGET_JUNK	"gp_target"

/*
 * An INSERT's ON CONFLICT clause as text for the segments, printed before
 * the statement is planned; refuses what Cloudberry refuses of it.
 */
extern char *GpExplicitOnConflict(struct Query *parse, GpPolicy *policy);
extern void GpExplicitInit(void);

#endif							/* GP_SCAN_H */
