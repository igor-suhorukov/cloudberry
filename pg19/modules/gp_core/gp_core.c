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
 * gp_core.c
 *	  The module every other module of the port needs.
 *
 * gp_core owns what the rest of Cloudberry is built on: the node's role in the
 * cluster, the settings that describe it, and -- as the milestones fill this
 * in -- the dispatcher, the motion layer, the distributed transaction manager
 * and the slice table.  It is preloaded first, so that the modules listed
 * after it resolve its symbols when PostgreSQL opens them with RTLD_GLOBAL.
 *
 * At this milestone it carries the settings and the rendezvous variable, and
 * refuses to load outside preload.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "cb_module.h"
#include "gp_core_api.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_core",
					.version = GP_VERSION
);

/*
 * What gp.version() reports.  Cloudberry puts this in version() itself, which
 * the port cannot do: version() is PostgreSQL's own output and has to stay as
 * it is on an unmodified server.
 */
#define GP_VERSION_STR	PG_VERSION_STR " (Apache Cloudberry " GP_VERSION ")"

/* Settings.  Every name a file may hold is dotted; see the note below. */
static int	gp_role = GP_ROLE_UTILITY;
static char *gp_qe_identity = NULL;

static const struct config_enum_entry gp_role_options[] = {
	{"utility", GP_ROLE_UTILITY, false},
	{"dispatch", GP_ROLE_DISPATCH, false},
	{"execute", GP_ROLE_EXECUTE, false},
	{NULL, 0, false}
};

static int	gp_api_get_role(void);
static int	gp_api_get_segment_count(void);
static int	gp_api_get_content_id(void);

/*
 * What the other modules see of us.  It is static storage, so the pointer we
 * publish stays valid for the life of the process.
 */
static const GpCoreApi gp_core_api = {
	.version_major = GP_CORE_API_VERSION_MAJOR,
	.version_minor = GP_CORE_API_VERSION_MINOR,
	.get_role = gp_api_get_role,
	.get_segment_count = gp_api_get_segment_count,
	.get_content_id = gp_api_get_content_id,
};

static int
gp_api_get_role(void)
{
	return gp_role;
}

static int
gp_api_get_segment_count(void)
{
	/*
	 * Until the cluster configuration is read (M2), this node knows of no
	 * segments, which is what single-node mode means: the extension is
	 * loaded and there are no segments to dispatch to.
	 */
	return 0;
}

static int
gp_api_get_content_id(void)
{
	return -1;					/* coordinator */
}

/*
 * GpCoreApiLookup
 *		Find gp_core from another module, without linking to it.
 */
const GpCoreApi *
GpCoreApiLookup(void)
{
	void	  **rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS);

	return (const GpCoreApi *) *rv;
}

void
_PG_init(void)
{
	void	  **rv;

	/*
	 * gp_core installs hooks and, from M2 on, requests shared memory and
	 * registers background workers and a custom WAL resource manager.  None
	 * of that can be done after the postmaster has started, so loading this
	 * library any other way is an error rather than a half-initialised
	 * server.
	 */
	CB_REQUIRE_PRELOAD("gp_core");

	/*
	 * Settings are named "gp.*".  An undotted name that PostgreSQL does not
	 * know is an error when it reads postgresql.conf, and that file is read
	 * before shared_preload_libraries is loaded; a dotted name becomes a
	 * placeholder instead and is picked up when we define it here.  So every
	 * setting that may end up in a file has to be dotted, whatever its
	 * context.
	 */
	DefineCustomEnumVariable("gp.role",
							 "Role this node plays in the cluster.",
							 "\"dispatch\" is the coordinator, \"execute\" a segment, "
							 "\"utility\" a node used on its own.",
							 &gp_role,
							 GP_ROLE_UTILITY,
							 gp_role_options,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gp.qe_identity",
							   "Identity the dispatcher gave this segment process.",
							   "Set by the coordinator on the connection that starts a "
							   "segment process; empty in every other backend.",
							   &gp_qe_identity,
							   "",
							   PGC_BACKEND,
							   0,
							   NULL, NULL, NULL);

	/*
	 * Deliberately no MarkGUCPrefixReserved("gp") here.  It drops every
	 * "gp.*" placeholder that is not defined yet, with a warning, and the
	 * modules that load on demand define their own "gp.*" settings long after
	 * this runs -- their values from postgresql.conf would be thrown away
	 * before they ever saw them.  A module whose settings may appear in a
	 * file has to be preloaded; reserving the prefix would hide that mistake
	 * rather than prevent it.
	 */

	/*
	 * Publish ourselves last, so that a module which finds us also finds the
	 * settings above already defined.
	 */
	rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS);
	*rv = unconstify(GpCoreApi *, &gp_core_api);
}

PG_FUNCTION_INFO_V1(gp_version);

/*
 * gp.version()
 *		What Cloudberry's own version() reports.
 */
Datum
gp_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(GP_VERSION_STR));
}
