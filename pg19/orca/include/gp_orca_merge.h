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
 * gp_orca_merge.h
 *	  MERGE under ORCA: the SELECT of its join ORCA plans, and the
 *	  ModifyTable put over ORCA's plan of it (merge.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_MERGE_H
#define GP_ORCA_MERGE_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

typedef struct OrcaMerge OrcaMerge;

/*
 * For a MERGE, the SELECT ORCA plans in its place, into *select, and what
 * GpOrcaFinishMerge() needs, into *state; false, and why, where it cannot.
 * Anything else is left alone: *select NULL.
 */
extern bool GpOrcaPrepareMerge(Query *query, Query **select, OrcaMerge **state,
							   const char **why);

/* The MERGE's ModifyTable over ORCA's plan of its SELECT, in "stmt". */
extern bool GpOrcaFinishMerge(PlannedStmt *stmt, OrcaMerge *state,
							  const char **why);

#endif							/* GP_ORCA_MERGE_H */
