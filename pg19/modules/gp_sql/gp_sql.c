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
 * gp_sql.c
 *	  Cloudberry-only SQL surface: tags, directory tables, storage servers.
 *
 * Cloudberry's DDL keeps working through O26, whose grammar emits only PG19
 * parse nodes: namespaced options, security labels and function calls
 * (decision 11).  This module handles those forms -- it strips the options in
 * ProcessUtility_hook, registers the label providers, and offers the
 * functions that the grammar desugars to.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/commands/tag.c, dirtablecmds.c, storagecmds.c,
 *	  storage/file/ufile.c, parser/parse_partition_gp.c
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
					.name = "gp_sql",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_CORE("gp_sql");
}
