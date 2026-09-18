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
 * gp_ao.c
 *	  Append-optimized row and column tables, and the bitmap index.
 *
 * AO and AOCO become table access methods, the bitmap index an index access
 * method, and both log through a custom WAL resource manager, which is why
 * this module has to be preloaded.  UPDATE needs the old row, which PG19
 * fetches by TID and AO cannot supply: decision 7a takes O20 if the table AM
 * registry O13 is taken, and otherwise puts a block directory on every AO
 * table.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/access/appendonly/, access/aocs/, access/bitmap/,
 *	  utils/datumstream/, cdb/cdbappendonly*, commands/vacuum_ao.c
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
					.name = "gp_ao",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("gp_ao");
	CB_REQUIRE_CORE("gp_ao");
}
