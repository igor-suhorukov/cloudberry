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
 * gp_resource.c
 *	  Resource groups, resource queues and memory protection.
 *
 * The cgroup code ports as it is.  A group is taken at the first executor or
 * utility hook of a transaction and released by a transaction callback, and
 * queue waits become shared-memory counters with condition variables.  What
 * Cloudberry adds to CHECK_FOR_INTERRUPTS cannot be carried, so runaway
 * detection moves into O25 or a sampling worker.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/utils/resgroup/, utils/resscheduler/, utils/resource_manager/,
 *	  commands/resgroupcmds.c, commands/queue.c
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
					.name = "gp_resource",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_resource");
}
