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
 * gp_security.c
 *	  Password profiles, account locking and login windows.
 *
 * ClientAuthentication_hook sees a failed login before the backend exits, but
 * cannot record it, because that backend never commits; so it queues the
 * event and a background worker counts failures and sets NOLOGIN at the
 * limit.  check_password_hook enforces reuse rules and verify functions.
 * Profile state lives in shared security labels on roles, and password
 * history in a maintenance-database table that is revoked from PUBLIC,
 * because labels are readable by everyone (decision 9).
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/commands/pg_profile.c, postmaster/loginmonitor.c,
 *	  and the profile code of user.c, auth.c and postinit.c
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
					.name = "gp_security",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_security");
}
