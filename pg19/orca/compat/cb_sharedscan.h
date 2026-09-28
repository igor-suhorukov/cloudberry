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
 * compat/cb_sharedscan.h
 *	  ORCA's CTE in a slice the segments run: Cloudberry's ShareInputScan,
 *	  as a Sequence, its producers and their consumers.
 *
 * See compat/sharedscan.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_SHAREDSCAN_H
#define CB_SHAREDSCAN_H

#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"

struct ExplainState;

extern PGDLLIMPORT const CustomScanMethods gp_orca_sequence_methods;
extern PGDLLIMPORT const CustomScanMethods gp_orca_shared_scan_methods;

/*
 * Can this gp_core name the files a segment keeps a shared CTE's rows in
 * (GpCoreApi.share_fileset, 1.10)?
 */
extern bool gp_orca_can_share_across_slices(void);

/*
 * A Sequence: the producers "producers" run first, once, and then the rows
 * of "plan" are its own.
 */
extern Plan *gp_orca_make_sequence(Plan *plan, List *producers);

/*
 * A Shared Scan that writes the rows of "child" as share "share_id" of the
 * statement, in slice "slice".
 */
extern Plan *gp_orca_make_share_producer(Plan *child, int share_id, int slice);

/*
 * A Shared Scan that reads share "share_id", in slice "slice": each row a
 * row of "scan_tlist", the producer's columns, and "targetlist" of its
 * columns as INDEX_VAR Vars.
 */
extern Plan *gp_orca_make_share_consumer(int share_id, int slice,
										 List *scan_tlist, List *targetlist);

/*
 * A producer whose CTE is read in other slices than its own: run when its
 * slice is done, if its Sequence never ran it (compat/sharedscan.c).
 */
extern void gp_orca_set_share_across(Plan *plan);

/*
 * Is "plan" a Shared Scan, and if so of which share, in which slice, and
 * does it produce the rows?
 */
extern bool gp_orca_is_shared_scan(Plan *plan, int *share_id, int *slice,
								   bool *producer);

/* The nodes, for a fragment's plan; from gp_orca's _PG_init. */
extern void gp_orca_register_shared_scans(void);

/* Their names in EXPLAIN, Cloudberry's (cb_explain.h). */
extern bool gp_orca_label_shared_scans(PlanState *planstate,
									   struct ExplainState *es,
									   const char **pname,
									   const char **suffix);

#endif							/* CB_SHAREDSCAN_H */
