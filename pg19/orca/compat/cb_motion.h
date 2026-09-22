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
 * compat/cb_motion.h
 *	  Whether a plan's Motions can be carried out as stage A carries them.
 *
 * gp_core dispatches each Motion's fragment to the segments as a plan of its
 * own (gp_motion.c), and a fragment so dispatched has nothing of the
 * coordinator's with it but the statement it was cut from: no value the
 * coordinator computed, no parameter the client bound.  In Cloudberry the
 * dispatcher sends the values of the parameters a slice uses; the port does
 * not yet, so a plan that needs them is refused, with the reason, and
 * planned by PostgreSQL.  The walk that checks this also tells each Gather
 * which Motions between segments below it are carried out first, and in
 * what order.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_MOTION_H
#define CB_MOTION_H

#include "nodes/plannodes.h"

#include "cdb/cdb_plan_nodes.h"

#define GP_ORCA_MOTION_OK			0
#define GP_ORCA_MOTION_NESTED		1	/* a Gather in a slice the segments run */
#define GP_ORCA_MOTION_PARAM		2	/* a value computed outside a fragment */
#define GP_ORCA_MOTION_EXTERN		3	/* a statement parameter on a segment */
#define GP_ORCA_MOTION_WRITE		4	/* a write in a fragment */

/*
 * The first of the reasons above that the plan has; GP_ORCA_MOTION_OK if
 * none, in which case each Gather has been given its Motions to carry out.
 */
extern int	gp_orca_check_motions(PlannedStmt *stmt);

/*
 * The plan's slice table, as PlannedStmt.extension_state keeps it: a DefElem
 * named GP_SLICE_TABLE whose argument is one list per slice of Integers --
 * its index, its parent's, its gang type, how many segments run it, which
 * one if it is one, and the segment direct dispatch sends it to, or -1.
 * Cloudberry keeps it in PlannedStmt.slices, which PostgreSQL 19 does not
 * have; "slices" is a List of the translator's PlanSlice, "motions" its
 * Motions.
 */
#define GP_SLICE_TABLE	"gp_slice_table"

extern Node *gp_orca_slice_table(List *slices, List *motions);

#endif							/* CB_MOTION_H */
