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
 * gp_settings.h
 *	  Cloudberry's settings of the dispatcher and the planner that are not
 *	  ORCA's; see gp_settings.c for what each does here.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SETTINGS_H
#define GP_SETTINGS_H

#include "postgres.h"

/* gp.test_print_direct_dispatch_info: an INFO line per slice dispatched */
extern bool gp_test_print_direct_dispatch_info;

/* gp.enable_direct_dispatch: send to the one segment that holds the rows */
extern bool gp_enable_direct_dispatch;

/* gp.motion_cost_per_row: the planner's cost of moving a row; 0 is 2 * cpu_tuple_cost */
extern double gp_motion_cost_per_row;

/* gp.use_legacy_hashops: a new key's legacy operator classes (GpPolicyColumnOpclass) */
extern bool gp_use_legacy_hashops;

/*
 * The INFO Cloudberry prints for a slice it dispatches, when
 * gp.test_print_direct_dispatch_info is on: "(slice 1) Dispatch command to
 * ALL contents: 0 1 2", or "SINGLE content" when it goes to one process --
 * a segment, or the coordinator's own for an entry slice -- or "PARTIAL
 * contents: 0 1" when to the first nsegments of a partial table's; 0 is
 * every segment.
 */
extern void GpReportDispatch(int slice, bool single, int nsegments);

/*
 * The planner's gathers have no slice table; each is a slice of its own,
 * numbered from 1 in the order the executor starts them.  The count starts
 * again with every statement.
 */
extern int	GpNextGatherSlice(void);

/*
 * Autostats (gp.autostats_mode): ANALYZE after a statement that wrote rows,
 * as Cloudberry's auto_stats() decides.  cmd is CMD_INSERT, CMD_UPDATE,
 * CMD_DELETE or CMD_MERGE; COPY FROM counts as CMD_INSERT, as it does there.
 */
extern void GpAutoStats(CmdType cmd, Oid relid, uint64 ntuples,
						bool in_function);

extern void GpSettingsInit(void);

#endif							/* GP_SETTINGS_H */
