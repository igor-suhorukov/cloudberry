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

/*
 * gp.test_print_direct_dispatch_info: an INFO line per slice dispatched, and
 * per command of a two-phase commit
 */
extern bool gp_test_print_direct_dispatch_info;

/* gp.enable_direct_dispatch: send to the one segment that holds the rows */
extern bool gp_enable_direct_dispatch;

/* gp.motion_cost_per_row: the planner's cost of moving a row; 0 is 2 * cpu_tuple_cost */
extern double gp_motion_cost_per_row;

/* gp.use_legacy_hashops: a new key's legacy operator classes (GpPolicyColumnOpclass) */
extern bool gp_use_legacy_hashops;

/* gp.statement_mem, in kB: what gp_resource budgets a query by */
extern PGDLLIMPORT int gp_statement_mem;

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
 * The same, for a slice direct dispatch sends to the n segments "contents"
 * lists, in the order it computed them: "SINGLE content" for one, "PARTIAL
 * contents: 2 0" for a few, "ALL contents: ..." for every one.
 */
extern void GpReportDispatchContents(int slice, const int *contents, int n);

/*
 * The INFO Cloudberry prints for a command of its two-phase commit, when
 * gp.test_print_direct_dispatch_info is on: "Distributed transaction
 * command 'Distributed Prepare' to ALL contents: 0 1 2", or to a SINGLE
 * content, or to PARTIAL contents -- the n segments whose content ids
 * contents holds, in order.
 */
extern void GpReportDtxCommand(const char *command, const int *contents, int n);

/*
 * A statement's dispatch to segments, for the INFO lines of its
 * transaction's commit, which name the segments the transaction reached as
 * Cloudberry's do: the n segments "contents" lists, in the order it sends to
 * them, or, when it is NULL, the first n -- every one where n is 0.  "stmt"
 * is the plan being run, which says whether the statement writes; NULL is
 * DDL's, COPY's or a savepoint's, which count as writes.  Kept only while
 * gp.test_print_direct_dispatch_info is on.
 */
struct PlannedStmt;
extern void GpReportDtxReached(struct PlannedStmt *stmt, const int *contents,
							   int n);

/*
 * The segments a command of the commit names, palloc'd, in the order the
 * transaction first reached them: every one it reached, when "reached" says
 * so, and those of the nset "set" lists -- and after them any of "set" no
 * dispatch noted, in the order given.  How many is returned.
 */
extern int	GpReportDtxContents(const int *set, int nset, bool reached,
								int **contents);

/* The transaction is over: no segment is reached any more. */
extern void GpReportDtxForget(void);

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
