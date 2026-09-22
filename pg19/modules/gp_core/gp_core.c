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

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "cb_module.h"
#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_policy.h"

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

/*
 * What the other modules see of us.  It is static storage, so the pointer we
 * publish stays valid for the life of the process.
 *
 * Everything here is the cluster's, and gp_cluster.c answers it: the role this
 * *backend* plays, which is not always the node's, how many segments to
 * compute with, which never falls to zero because consumers divide by it, and
 * whether there are any segments at all, which is the question the count
 * cannot carry.
 */
static const GpCoreApi gp_core_api = {
	.version_major = GP_CORE_API_VERSION_MAJOR,
	.version_minor = GP_CORE_API_VERSION_MINOR,
	.get_role = GpClusterBackendRole,
	.get_segment_count = GpClusterSegmentCount,
	.get_content_id = GpClusterContentId,
	.is_single_node = GpClusterIsSingleNode,
	.get_dbid = GpClusterDbid,
};

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
	 * The cluster first: it defines "gp.role", "gp.qe_identity",
	 * "gp.cluster_config" and "gp.dbid", and reads the file that says which
	 * nodes there are.  A cluster described wrongly is a server that does not
	 * start, which is why this runs here rather than at the first query.
	 */
	GpClusterInit();

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
	 * The "gp" security label, which is where the port keeps what Cloudberry
	 * keeps in catalog columns of its own.  It is registered here, in the
	 * module every other one needs, because several of them use it.
	 */
	GpLabelRegisterProvider();

	/*
	 * Publish ourselves last, so that a module which finds us also finds the
	 * settings above already defined.
	 */

	rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS);
	*rv = unconstify(GpCoreApi *, &gp_core_api);
}

PG_FUNCTION_INFO_V1(gp_version);
PG_FUNCTION_INFO_V1(gp_node);
PG_FUNCTION_INFO_V1(gp_policy);

/*
 * gp.version()
 *		What Cloudberry's own version() reports.
 */
Datum
gp_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(GP_VERSION_STR));
}

/*
 * gp.node()
 *		What this node thinks it is.
 *
 * Two of these are easy to confuse, and confusing them cost something once:
 * "segments" is how many segments to compute with and is never zero, because
 * consumers divide by it -- ORCA asserts 0 < segments and its skew model
 * computes 1.0 / segments.  Whether this server has segments configured at
 * all is "single_node", a flag.  On a single node the two read 1 and true.
 *
 * "role" is this *backend's* role, which on a segment is not the node's: a
 * connection the dispatcher opened executes, and one somebody opened with
 * psql is a utility session, as it is in Cloudberry.  "dbid" says which node
 * of the cluster this is, so that a dispatched session can be told apart from
 * the coordinator's in a log or a test.
 */
Datum
gp_node(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	HeapTuple	tuple;
	const char *role;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	switch (gp_core_api.get_role())
	{
		case GP_ROLE_DISPATCH:
			role = "dispatch";
			break;
		case GP_ROLE_EXECUTE:
			role = "execute";
			break;
		default:
			role = "utility";
			break;
	}

	values[0] = CStringGetTextDatum(role);
	values[1] = Int32GetDatum(gp_core_api.get_segment_count());
	values[2] = Int32GetDatum(gp_core_api.get_content_id());
	values[3] = BoolGetDatum(gp_core_api.is_single_node());
	values[4] = Int32GetDatum(gp_core_api.get_dbid());

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp.policy(regclass)
 *		How this relation's rows are spread, as the readers see it.
 *
 * The label holds text; this is the policy that text becomes -- the kind, the
 * distribution key by column name, and the opclass family each column is
 * hashed with, which is what ORCA carries into DXL.  NULL for a relation with
 * no policy, which is what an unlabelled relation means and what every table
 * means on one node.
 *
 * It reports names rather than attribute numbers because a number says
 * nothing about whether the right column was found, which is the question a
 * reader of this has.
 */
Datum
gp_policy(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	GpPolicy   *policy = GpPolicyGet(relid);
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tuple;
	const char *kind;
	Datum	   *cols;
	Datum	   *families;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	if (policy == NULL)
		PG_RETURN_NULL();

	if (GpPolicyIsReplicated(policy))
		kind = "replicated";
	else if (GpPolicyIsHashPartitioned(policy))
		kind = "hash";
	else if (GpPolicyIsRandomPartitioned(policy))
		kind = "random";
	else
		kind = "entry";

	cols = (Datum *) palloc(sizeof(Datum) * Max(policy->nattrs, 1));
	families = (Datum *) palloc(sizeof(Datum) * Max(policy->nattrs, 1));

	for (int i = 0; i < policy->nattrs; i++)
	{
		cols[i] = CStringGetTextDatum(get_attname(relid, policy->attrs[i],
												  false));
		families[i] = ObjectIdGetDatum(get_opclass_family(policy->opclasses[i]));
	}

	values[0] = CStringGetTextDatum(kind);
	values[1] = PointerGetDatum(construct_array_builtin(cols, policy->nattrs,
														TEXTOID));
	values[2] = PointerGetDatum(construct_array_builtin(families,
														policy->nattrs, OIDOID));
	values[3] = Int32GetDatum(policy->numsegments);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
