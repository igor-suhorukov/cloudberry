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
 * udp2.c
 *	  The UDP2 motion transport.
 *
 * UDP2's core is C++ that includes no PostgreSQL header, so only its adapter
 * needs the shim that the other transports need.  Decision 6 builds it once
 * it has stabilised, which is why it is off by default.
 *
 * Cloudberry sources this module is made of:
 *	  contrib/udp2/
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
					.name = "udp2",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("udp2");
	CB_REQUIRE_CORE("udp2");
}
