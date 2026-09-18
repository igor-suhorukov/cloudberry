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
 * pax.c
 *	  PAX, the partition-attributes-across storage format.
 *
 * PAX is a table access method that keeps its own files in a directory beside
 * the relation and logs them through a custom WAL resource manager.  On PG19
 * it is packaged as an extension instead of being written into the catalog by
 * initdb, its TIDs are remapped to fit PostgreSQL's TID bitmap, projection
 * and filters move into a CustomScan, and its directories are removed through
 * O22 (decision 7b).
 *
 * Cloudberry sources this module is made of:
 *	  contrib/pax_storage/
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
					.name = "pax",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("pax");
	CB_REQUIRE_CORE("pax");
}
