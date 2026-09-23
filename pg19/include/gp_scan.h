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

#include "gp_policy.h"

/*
 * The policy of a table whose rows are on the segments, or NULL for one whose
 * rows are here: on a single node every table, on a cluster a table with no
 * policy or an entry one.
 */
extern GpPolicy *GpScanDistributedPolicy(Oid relid);

/*
 * The segment this session reads a replicated table from.  Every segment has
 * every row, so sessions are spread over them; it is also the answer to
 * gp_segment_id of such a row.
 */
extern int	GpScanReplicatedContent(void);

/*
 * The one segment that holds every row conditions on a hash-distributed
 * table can match -- they fix every column of its key to a constant -- or
 * -1.  varno is the table's range table index in the conditions.
 */
extern int	GpScanDirectDispatchSegment(Oid relid, Node *quals, Index varno);

/* The scan hooks, where there is a cluster; see gp_scan.c. */
extern void GpScanInit(void);

/* The write path, where there is a cluster; see gp_modify.c. */
extern void GpModifyInit(void);

/* ANALYZE of a distributed table through O3, where there is a cluster. */
extern void GpAnalyzeInit(void);

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
extern const char *GpExplicitCannot(struct PlannedStmt *stmt,
									struct ModifyTable *mt);
extern struct Plan *GpExplicitMake(struct ModifyTable *mt);
extern void GpExplicitInit(void);

#endif							/* GP_SCAN_H */
