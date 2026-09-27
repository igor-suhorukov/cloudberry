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

#include "nodes/pg_list.h"

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

extern void GpPartAnalyzeInit(void);

#endif							/* GP_PARTANALYZE_H */
