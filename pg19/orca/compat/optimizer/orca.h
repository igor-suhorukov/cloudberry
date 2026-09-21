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
 * compat/optimizer/orca.h
 *	  The way in to ORCA, and the way hints reach it.
 *
 * Ported from github/cloudberry/src/include/optimizer/orca.h.  Cloudberry puts
 * these three declarations in a PostgreSQL file it patched, so the port has to
 * carry them itself.
 *
 * Three differences from Cloudberry's:
 *
 *	 * No USE_ORCA guard.  Cloudberry can be configured without ORCA and this
 *	   header is read by files that are built either way.  In the port ORCA is
 *	   a module of its own: if this header is being read, ORCA is there.
 *
 *	 * plan_hint_hook is a variable this module defines rather than one the
 *	   core does.  Rule 4 of "Core patches that keep vanilla behaviour" forbids
 *	   new core surface, and a hint extension can set a variable in gp_orca
 *	   just as well -- it has to load gp_orca to be of any use in any case.
 *	   Nothing in the port sets it yet; pg_hint_plan is Track A's.
 *
 *	 * optimize_query() says why it made no plan.  Cloudberry's reports a
 *	   fallback itself and returns NULL; gp_orca's planner hook counts every
 *	   fallback by its reason, so the reason comes back to it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_ORCA_H
#define GP_ORCA_COMPAT_ORCA_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/params.h"

#include "optimizer/orcaopt.h"

#include "gp_orca_api.h"

/*
 * Plan a query with ORCA, or return NULL to say it would not, and why in
 * *failure.
 *
 * NULL is the fallback signal and is not an error: gp_orca's planner_hook
 * counts the reason and hands the query to the PostgreSQL planner.  See
 * gp_orca_planner.c.  A PostgreSQL error that stopped ORCA is not a reason;
 * it is re-thrown, as the statement's error.
 */
extern PlannedStmt *optimize_query(Query *parse, int cursorOptions,
								   ParamListInfo boundParams,
								   OptimizerOptions *options,
								   GpOrcaFailure *failure);

/*
 * Not transformGroupedWindows(), which Cloudberry's orca.h declares: only
 * orca.c calls it, so it is static there, and not one more name in the
 * namespace every library shares.
 */

/*
 * How a hint extension tells ORCA what it parsed out of a query.
 *
 * It returns an opaque HintState (optimizer/hints.h) which COptTasks passes
 * to ORCA's own hint machinery.  The pointer is void * on purpose: the hint
 * structures are the extension's, and ORCA only carries them.
 */
typedef void *(*plan_hint_hook_type) (Query *parse);
extern PGDLLIMPORT plan_hint_hook_type plan_hint_hook;

#endif							/* GP_ORCA_COMPAT_ORCA_H */
