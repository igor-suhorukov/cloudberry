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
 * gp_tde.c
 *	  Transparent data encryption for relations of extension access methods.
 *
 * Decision 8 uses volume or filesystem encryption, so this module is built
 * only where a formal requirement asks for encryption inside the database.
 * It then encrypts through O24, and only pages of relations whose access
 * method this port registers, so that a server without the key fails at
 * relation open rather than reading ciphertext as line pointers.  Cloudberry's
 * own IV repeats across versions of a page; the port uses a tweakable mode.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/crypto/, src/common/kmgr_utils.c, src/bin/pg_alterckey/
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
					.name = "gp_tde",
					.version = GP_VERSION
);

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("gp_tde");
	CB_REQUIRE_CORE("gp_tde");
}
