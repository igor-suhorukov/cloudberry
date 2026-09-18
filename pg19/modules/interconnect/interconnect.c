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
 * interconnect.c
 *	  Motion transports: tcp, udpifc and proxy.
 *
 * The transports are already loadable modules behind a function table in
 * Cloudberry, so what changes here is the boundary: gp_core owns the motion
 * layer, the settings and the per-query state, and exports them, while this
 * module registers a transport and calls back into them.  Decision 6 builds
 * tcp and udpifc first, which is today's default, and proxy on request.
 *
 * Cloudberry sources this module is made of:
 *	  contrib/interconnect/
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
					.name = "interconnect",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("interconnect");
	CB_REQUIRE_CORE("interconnect");
}
