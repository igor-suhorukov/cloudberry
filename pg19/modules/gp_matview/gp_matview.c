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
 * gp_matview.c
 *	  Incremental materialized views, dynamic tables and AQUMV bookkeeping.
 *
 * An incremental view stays a real materialized view: O27 opens maintenance
 * mode so the deltas can be applied with ordinary DML, and O28 keeps the
 * counter columns out of SELECT * (decision 12).  A dynamic table is a
 * materialized view with a schedule, refreshed by a gp_task job.  AQUMV
 * becomes a planner_hook wrapper that plans twice and keeps the cheaper plan,
 * which puts it above ORCA as well.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/optimizer/plan/aqumv.c, catalog/gp_matview_aux.c,
 *	  and the incremental-view code of matview.c and createas.c
 *
 * At this milestone the module only loads.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"

#include "cb_module.h"
#include "gp_core_api.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_matview",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_matview");
}
