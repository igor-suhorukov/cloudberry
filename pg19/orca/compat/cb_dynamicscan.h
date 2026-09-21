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
 * compat/cb_dynamicscan.h
 *	  The scan of a partitioned table, and the nodes that choose its
 *	  partitions while the query runs.
 *
 * Cloudberry's executor has DynamicSeqScan, DynamicIndexScan and the rest,
 * which scan the partitions ORCA chose one after another, and
 * PartitionSelector, which narrows that choice while the query runs
 * (github/cloudberry/src/backend/executor/nodeDynamic*.c,
 * nodePartitionSelector.c).  They lean on Cloudberry's own changes to
 * PostgreSQL -- a scan node initialised for a partition, pruning steps that
 * read a row -- which PostgreSQL 19 does not have.  The port's are two
 * CustomScans:
 *
 *	Dynamic Scan		scanrelid 0; custom_plans holds one ordinary scan of
 *						each partition ORCA's static pruning left, each built
 *						from the one ORCA planned for the partitioned table
 *						(gp_orca_plan_for_partition); custom_scan_tlist is
 *						that scan's target list, in the partitioned table's
 *						terms, and the node's own target list reads it through
 *						INDEX_VAR.  custom_private: an IntList with the index,
 *						in the table's partition descriptor, of the partition
 *						each child scans or is under, and an IntList of the
 *						parameters of the Partition Selectors that choose among
 *						them.  It runs the children a selector chose, or all
 *						of them if none has.
 *
 *	Partition Selector	its child is the outer plan, whose rows it passes on;
 *						for each, it finds the partitions of the table that
 *						can hold a matching row, with PostgreSQL 19's
 *						get_matching_partitions() over pruning steps that read
 *						the row (OUTER_VAR), and when its child is done it
 *						hands the set to the Dynamic Scan through a PARAM_EXEC
 *						parameter.  custom_private: the parameter (Integer),
 *						the table (Integer, an Oid) and the pruning steps;
 *						custom_exprs: the steps' expressions, for walkers and
 *						EXPLAIN.
 *
 * Built by CTranslatorDXLToPlStmt, and registered once per process from
 * _PG_init.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_DYNAMICSCAN_H
#define CB_DYNAMICSCAN_H

#include "nodes/extensible.h"
#include "nodes/plannodes.h"
#include "utils/relcache.h"

extern const CustomScanMethods gp_orca_dynamic_scan_methods;
extern const CustomScanMethods gp_orca_partition_selector_methods;

extern void gp_orca_register_dynamic_scans(void);

/* Why gp_orca_plan_for_partition() made no plan. */
#define GP_ORCA_PARTITION_OK			0
#define GP_ORCA_PARTITION_NO_INDEX		1	/* the table's index is not on it */
#define GP_ORCA_PARTITION_INDEX_TOO_NEW	2	/* its index is too new to use */

/*
 * The scan of partition `part_relid`, whose range table entry is at
 * `part_rti`, made from `scan`, a scan of the partitioned table `root_relid`
 * at `root_rti`: a copy, with every Var of the table read as the partition's
 * column of the same name, and every index the partition's index of the
 * table's.  NULL, with *failure set, for what it cannot make.
 */
extern Plan *gp_orca_plan_for_partition(Plan *scan, Index root_rti,
										Index part_rti, Oid root_relid,
										Oid part_relid, int *failure);

/*
 * The range table entry of a partition the scan of `root` reads, as the
 * planner makes one (inherit.c, expand_single_inheritance_child): the
 * partition's relation under the table's alias, with the table's column
 * aliases where the names match, and no permission entry of its own -- the
 * table's is what the executor checks.
 */
extern RangeTblEntry *gp_orca_partition_rte(const RangeTblEntry *root,
											Oid part_relid);

/*
 * What a scan of the partitioned table returns, in the table's own columns:
 * its target list, with an index-only scan's references to the index's
 * columns (INDEX_VAR) read through its indextlist.  The Dynamic Scan's
 * custom_scan_tlist, which its own target list reads by INDEX_VAR -- so it
 * must not read by INDEX_VAR itself, or EXPLAIN resolves one into the other
 * for ever.
 */
extern List *gp_orca_dynamic_scan_tlist(Plan *scan);

/*
 * The index, in the partition descriptor of `root_relid`, of the partition
 * that is `leaf_relid` or has it below it; -1 if there is none.
 */
extern int	gp_orca_top_partition_index(Oid root_relid, Oid leaf_relid);

#endif							/* CB_DYNAMICSCAN_H */
