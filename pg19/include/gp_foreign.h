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
 * gp_foreign.h
 *	  Where a foreign table is read: Cloudberry's mpp_execute and
 *	  num_segments (gp_foreign.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_FOREIGN_H
#define GP_FOREIGN_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

#include "gp_policy.h"

/*
 * Where a foreign table is read, as its mpp_execute says -- Cloudberry's
 * FTEXECLOCATION_*, by the same letters.
 */
#define GP_FOREIGN_COORDINATOR		'c'
#define GP_FOREIGN_ANY				'a'
#define GP_FOREIGN_ALL_SEGMENTS		's'

/*
 * The table's place: its own mpp_execute, its server's or its wrapper's, the
 * coordinator where none says; and in *numsegments, where it is read on the
 * segments, how many of them.  An external table's is gp_exttable's, and not
 * asked here.
 */
extern char GpForeignExecLocation(Oid relid, int *numsegments);

/* Does this statement set the options of a wrapper, a server or a table? */
extern bool GpForeignSetsOptions(Node *parsetree);

/*
 * Run such a statement here, through run(pstmt, arg), with mpp_execute and
 * num_segments out of what the wrapper's validator is given, and in the
 * object's options after it, as Cloudberry keeps them.
 */
typedef void (*GpForeignRunFn) (PlannedStmt *pstmt, bool readOnlyTree,
								void *arg);
extern void GpForeignRunStatement(PlannedStmt *pstmt, bool readOnlyTree,
								  GpForeignRunFn run, void *arg);

/* On one node, the hook that runs them so; gp_ddl.c does on a cluster. */
extern void GpForeignInit(void);

#endif							/* GP_FOREIGN_H */
