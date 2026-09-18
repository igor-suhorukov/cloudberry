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
 * gp_exttable.c
 *	  External tables, gpfdist and the loading path.
 *
 * External tables stay a foreign data wrapper, and single-row error handling
 * is rebuilt on PG19's exported COPY API.  Writable external tables format
 * their own rows, because CopyOneRowTo is static in PG19.
 *
 * Cloudberry sources this module is made of:
 *	  gpcontrib/gp_exttable_fdw/, src/backend/access/external/,
 *	  cdb/cdbsreh.c, cdb/cdbcopy.c, contrib/extprotocol/, contrib/formatter*
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
					.name = "gp_exttable",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_exttable");
}
