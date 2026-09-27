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
 * gp_partanalyze.h
 *	  ANALYZE of a partitioned table as Cloudberry does it: which of its
 *	  relations a statement takes, and in what order (gp_partanalyze.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_PARTANALYZE_H
#define GP_PARTANALYZE_H

#include "access/htup.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

/* Cloudberry's optimizer_analyze_root_partition and _midlevel_partition */
extern bool gp_optimizer_analyze_root_partition;
extern bool gp_optimizer_analyze_midlevel_partition;

/*
 * Have all of a partitioned table's leaves, but "exclude", statistics for
 * these columns (NIL: every column)?  Cloudberry's leaf_parts_analyzed(),
 * which says at elevel why not.
 */
extern bool GpLeafPartsAnalyzed(Oid parent, Oid exclude, List *va_cols,
								int elevel);

/*
 * Does gp_core apply Cloudberry's rules to this database's ANALYZE: is its
 * extension made here, and is this backend not a segment's running what
 * the coordinator sent?
 */
extern bool GpPartAnalyzeActive(void);

/*
 * What the ANALYZE this backend runs asks of a relation it takes, where
 * gp_partanalyze.c made its list: false for a relation it does not take;
 * the columns it names of it, NIL for every one.  And whether it is
 * VERBOSE, and FULLSCAN.
 */
extern bool GpPartAnalyzeTarget(Oid relid, List **va_cols);
extern bool GpPartAnalyzeVerbose(void);
extern bool GpPartAnalyzeFullscan(void);

extern void GpPartAnalyzeInit(void);

/*
 * A partitioned table's statistics merged from its leaves' (gp_partmerge.c).
 *
 * A leaf's counters, of the sample its ANALYZE took, of the columns the
 * statement names of it; with FULLSCAN, of a full scan, which say what they
 * run at elevel and whose numbers of distinct values are the leaf's own too.
 */
extern void GpLeafSampleCounters(Relation leaf, HeapTuple *rows, int numrows,
								 List *va_cols);
extern List *GpLeafFullScan(Relation leaf, List *va_cols, int elevel);
extern void GpLeafFullScanNdistinct(Relation leaf, List *counters,
									HeapTuple *rows, int numrows,
									double totalrows);

/*
 * A root's columns that can be merged, in *all whether every one can and
 * nothing needs a sample; the root's statistics of such columns merged and
 * written, with the rows of its leaves; or merged over PostgreSQL's once
 * PostgreSQL has written them.
 */
extern List *GpRootMergeableColumns(Relation root, List *va_cols, int elevel,
									bool *all);
extern double GpRootMerge(Relation root, List *attnums);
extern void GpRootMergeLater(Oid root, List *attnums);

/* What is to be written over PostgreSQL's statistics, written now. */
extern void GpPartMergeFinish(void);

extern void GpPartMergeInit(void);

#endif							/* GP_PARTANALYZE_H */
