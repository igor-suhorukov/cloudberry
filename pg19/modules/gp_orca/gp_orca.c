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
 * gp_orca.c
 *	  ORCA, the cost-based optimizer, behind planner_hook.
 *
 * ORCA runs from planner_hook on the coordinator and hands back a plan whose
 * Motion nodes are CustomScans.  Before it is called, a rewrite adds explicit
 * bounding-box conditions for PostGIS's indexable functions, so those queries
 * stay on ORCA instead of the gather-everything fallback, and a counter
 * records every plan that falls back and why (decision 1).  O4 makes the
 * Motion lines read as they do on Cloudberry.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/gpopt/, src/backend/gporca/, src/include/gpopt/
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
					.name = "gp_orca",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("gp_orca");
	CB_REQUIRE_CORE("gp_orca");
}
