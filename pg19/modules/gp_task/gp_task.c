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
 * gp_task.c
 *	  The task scheduler.
 *
 * Cloudberry's scheduler is pg_cron moved into the server, so it goes back to
 * pg_cron's own shape: a background worker on the coordinator, and job tables
 * in one maintenance database instead of shared catalogs.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/task/, commands/taskcmds.c
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
					.name = "gp_task",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_task");
}
