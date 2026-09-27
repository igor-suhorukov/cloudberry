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
 * gp_orca_parallel.h
 *	  Parallelism within a segment, under ORCA.
 *
 * See parallel.c.  Called from orca.c, on the plan ORCA made.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_PARALLEL_H
#define GP_ORCA_PARALLEL_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/*
 * Put PostgreSQL's Gathers into the fragments a segment's writer runs, where
 * parallelism within a segment is on and they pay: "query" is the query
 * ORCA was handed, "cursorOptions" the planner's.
 */
extern void GpOrcaParallelize(PlannedStmt *stmt, Query *query,
							  int cursorOptions);

#endif							/* GP_ORCA_PARALLEL_H */
