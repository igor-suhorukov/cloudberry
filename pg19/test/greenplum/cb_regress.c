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
 * cb_regress.c
 *	  The functions of Cloudberry's regress.so that its tests load, served by
 *	  the port.
 *
 * Cloudberry's tests make functions from the regress.so of their build
 * directory, whose regress_gp.c has test functions of Cloudberry's own
 * beside PostgreSQL's.  The greenplum suite links this module into the
 * directory the tests load it from, under that name, and each function here
 * asks the module that does the work, found by name when the function is
 * called, so that this one loads whatever else is loaded.
 *
 * It is a test module: built and installed only where the tests run.
 *
 * Cloudberry source this file stands in for:
 *	  src/test/regress/regress_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
					.name = "cb_regress",
					.version = "1.0"
);

typedef bool (*deny_allows_fn) (const char *rolename, TimestampTz when);

PG_FUNCTION_INFO_V1(check_auth_time_constraints);

/*
 * check_auth_time_constraints(cstring, timestamptz) -> bool: whether the role
 * may log in at that time, by its DENY windows, which are gp_security's.
 */
Datum
check_auth_time_constraints(PG_FUNCTION_ARGS)
{
	static deny_allows_fn allows = NULL;

	if (allows == NULL)
		allows = (deny_allows_fn)
			load_external_function("$libdir/gp_security", "GpDenyRoleAllowed",
								   true, NULL);

	PG_RETURN_BOOL(allows(PG_GETARG_CSTRING(0), PG_GETARG_TIMESTAMPTZ(1)));
}
