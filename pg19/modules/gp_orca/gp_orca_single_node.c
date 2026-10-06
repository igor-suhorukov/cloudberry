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
 * gp_orca_single_node.c
 *	  NOT IN CLOUDBERRY.  What gp_core answers ORCA, answered by gp_orca
 *	  itself, for one node of a PostgreSQL 19 without gp_core: gp_orca's
 *	  single-node build, -Dorca_single_node (pg_vector_executor.md §3.3.5).
 *
 * gp_orca needs gp_core on the port.  It refuses to load without it; it asks
 * gp_core's API (gp_core_api.h) what node this is, and makes its Motions
 * through it; and it calls gp_core's policy, label and hash functions
 * directly, which a module loaded after gp_core reaches because PostgreSQL
 * opens every library RTLD_GLOBAL.  On one node ORCA's plans have no
 * Motions: every relation is the coordinator's (gpdb::IsSingleNode(), in the
 * relcache translator).  So what it asks of gp_core there is what node this
 * is, and what a relation's policy, an object's label, a type's distribution
 * class and a hash family's function are.
 *
 * This file answers those in gp_core's stead, as gp_core answers on a server
 * with no segments, its settings at their defaults and its extension not in
 * the database:
 *
 *	  a utility session of the only node: content -1, dbid 1, one segment to
 *	  compute with, which ORCA divides by;
 *	  no relation with a policy and no object with a "gp" label -- nothing
 *	  makes either without gp_core -- so every relation is the
 *	  coordinator's, and every function runs where it is called;
 *	  no legacy hash class, which gp_core's extension makes;
 *	  a type's distribution class, and a family's hash function, from the
 *	  catalog, as gp_core finds them (gp_policy.c, gp_hash.c);
 *	  the statement as PostgreSQL's planner has it: gp_core's rewrites of it
 *	  are a cluster's, and of functions its extension makes (gp_segment.c).
 *
 * What only a cluster asks -- a Motion, a split update, the explicit write,
 * a parallel retrieve cursor's endpoints -- raises: it is not reached on one
 * node, where the translator checks gp_core can dispatch before it makes a
 * Motion, and a statement that would need one falls back to PostgreSQL's
 * planner, as it does in a database without gp_core's extension
 * (CheckCanDispatchPlans(), CTranslatorDXLToPlStmt.cpp).
 *
 * The build compiles this file in, and gp_core's headers declare what it
 * defines; cb_compat.h's cb_core_api() returns the API here rather than
 * look gp_core up.  Its functions are hidden, so that a library loaded after
 * gp_orca finds none of gp_core's names in it.  gp_core and this build
 * cannot run in one server -- this ORCA would plan a cluster's tables as the
 * coordinator's -- so gp_orca refuses to start beside it, whichever of the
 * two is loaded first.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_amproc.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "cb_compat.h"
#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_foreign.h"
#include "gp_hash.h"
#include "gp_label.h"
#include "gp_orca_single_node.h"
#include "gp_policy.h"

#ifndef GP_ORCA_SINGLE_NODE
#error "gp_orca_single_node.c is built only by -Dorca_single_node"
#endif

/* gp_core's names, defined here for gp_orca alone */
#ifdef HAVE_VISIBILITY_ATTRIBUTE
#define SINGLE_NODE_HIDDEN __attribute__((visibility("hidden")))
#else
#define SINGLE_NODE_HIDDEN
#endif

static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* ------------------------------------------------------------------------- */
/* gp_core, absent                                                            */
/* ------------------------------------------------------------------------- */

static bool
gp_core_loaded(void)
{
	return *find_rendezvous_variable(CB_CORE_RENDEZVOUS) != NULL;
}

static void
refuse_beside_gp_core(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("this gp_orca is built for a server without \"gp_core\", which is loaded"),
			 errdetail("gp_orca built with -Dorca_single_node plans every relation as one node's."),
			 errhint("Load the port's own gp_orca beside \"gp_core\", or remove \"gp_core\" from \"shared_preload_libraries\".")));
}

/*
 * Once every library of shared_preload_libraries is loaded, in the
 * postmaster: gp_core listed after gp_orca is loaded by now.
 */
static void
single_node_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	if (gp_core_loaded())
		refuse_beside_gp_core();
}

void
GpOrcaSingleNodeInit(void)
{
	if (gp_core_loaded())
		refuse_beside_gp_core();
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = single_node_shmem_request;
}

/* ------------------------------------------------------------------------- */
/* The API                                                                    */
/* ------------------------------------------------------------------------- */

#define NOT_ON_ONE_NODE(what) \
	elog(ERROR, "%s is gp_core's, which gp_orca's single-node build runs without", (what))

static int
single_node_role(void)
{
	return GP_ROLE_UTILITY;
}

static int
single_node_segment_count(void)
{
	return 1;
}

static int
single_node_content_id(void)
{
	return -1;
}

static bool
single_node_is_single_node(void)
{
	return true;
}

static int
single_node_dbid(void)
{
	return 1;
}

static bool
single_node_motion_can_dispatch(void)
{
	return false;
}

static Plan *
single_node_motion_make_gather(Plan *fragment, List *targetlist, List *qual,
							   int content, int slice, int nkeys,
							   const AttrNumber *keys, const Oid *sortops,
							   const Oid *collations, const bool *nullsfirst)
{
	NOT_ON_ONE_NODE("a Gather Motion");
}

static bool
single_node_motion_is(Plan *plan)
{
	return false;
}

static int
single_node_motion_segment(Plan *plan)
{
	NOT_ON_ONE_NODE("a Motion");
}

static void
single_node_motion_set_segment(Plan *plan, int content)
{
	NOT_ON_ONE_NODE("a Motion");
}

static int
single_node_direct_dispatch_segment(Oid relid, int nvalues, const Oid *types,
									const Datum *values, const bool *isnull)
{
	NOT_ON_ONE_NODE("direct dispatch");
}

static Plan *
single_node_motion_make_send(int type, Plan *fragment, List *targetlist,
							 List *qual, int content, int slice,
							 List *hashexprs, List *hashfuncs)
{
	NOT_ON_ONE_NODE("a Motion between segments");
}

static int
single_node_motion_type(Plan *plan)
{
	NOT_ON_ONE_NODE("a Motion");
}

static int
single_node_motion_slice(Plan *plan)
{
	NOT_ON_ONE_NODE("a Motion");
}

static void
single_node_motion_set_prepare(Plan *plan, List *slices)
{
	NOT_ON_ONE_NODE("a Motion");
}

static Plan *
single_node_motion_make_hash_filter(Plan *child, List *targetlist, List *qual,
									int nkeys, const AttrNumber *cols,
									const Oid *hashfuncs, int segment)
{
	NOT_ON_ONE_NODE("a hash filter");
}

static Plan *
single_node_motion_make_dml(Plan *modify, int content, int slice)
{
	NOT_ON_ONE_NODE("a write the segments carry out");
}

static Plan *
single_node_split_make(Plan *child, List *targetlist, List *deletecols,
					   List *insertcols, AttrNumber actioncol)
{
	NOT_ON_ONE_NODE("a split update");
}

static Plan *
single_node_split_modify_make(Plan *child, Index rti, int natts,
							  AttrNumber actioncol, AttrNumber ctidcol)
{
	NOT_ON_ONE_NODE("a split update");
}

static void
single_node_motion_set_parent(Plan *plan, int parent)
{
	NOT_ON_ONE_NODE("a Motion");
}

static void
single_node_motion_set_params(Plan *plan, List *exec_params,
							  List *extern_params)
{
	NOT_ON_ONE_NODE("a Motion");
}

static void
single_node_motion_set_segments(Plan *plan, List *contents)
{
	NOT_ON_ONE_NODE("a Motion");
}

static List *
single_node_motion_segments(Plan *plan)
{
	NOT_ON_ONE_NODE("a Motion");
}

static List *
single_node_direct_dispatch_contents(Oid relid, Node *quals, Index varno)
{
	NOT_ON_ONE_NODE("direct dispatch");
}

/* gp_segment_id is gp_core's extension's function, which is not here */
static Oid
single_node_segment_of_function(void)
{
	return InvalidOid;
}

/*
 * gp_core's rewrites (GpPrepareQuery()): gp_dist_random()'s query, and the
 * size functions, on a cluster's coordinator, and pg_tablespace_location()
 * through a hook of the core series -- none of them here.
 */
static void
single_node_prepare_query(Query *parse)
{
}

/* no fragment: a CTE is not shared through files */
static bool
single_node_share_fileset(PlannedStmt *stmt, struct FileSet *fileset)
{
	return false;
}

static void
single_node_split_modify_set_tableoid(Plan *plan, AttrNumber tableoidcol)
{
	NOT_ON_ONE_NODE("a split update");
}

static Plan *
single_node_row_identity_make(Plan *child, AttrNumber contentcol,
							  AttrNumber ctidcol)
{
	NOT_ON_ONE_NODE("a row's identity on a segment");
}

static Plan *
single_node_explicit_write(PlannedStmt *stmt, Plan *modify)
{
	NOT_ON_ONE_NODE("the explicit write");
}

static void
single_node_metatrack_partition(Oid relid, const char *subtype)
{
	NOT_ON_ONE_NODE("pg_stat_last_operation");
}

static void
single_node_endpoint_plan(PlannedStmt *stmt)
{
	NOT_ON_ONE_NODE("a parallel retrieve cursor");
}

static char *
single_node_retrieve_sql(const char *endpoint, bool all, int64 count)
{
	NOT_ON_ONE_NODE("RETRIEVE");
}

static void
single_node_extension_mark_add(const char *suffix)
{
	NOT_ON_ONE_NODE("a module's mark on a database's files");
}

static void
single_node_size_from_am_register(const struct TableAmRoutine *am)
{
	NOT_ON_ONE_NODE("the size functions' access methods");
}

static const GpCoreApi single_node_api = {
	.version_major = GP_CORE_API_VERSION_MAJOR,
	.version_minor = GP_CORE_API_VERSION_MINOR,
	.get_role = single_node_role,
	.get_segment_count = single_node_segment_count,
	.get_content_id = single_node_content_id,
	.is_single_node = single_node_is_single_node,
	.get_dbid = single_node_dbid,
	.motion_can_dispatch = single_node_motion_can_dispatch,
	.motion_make_gather = single_node_motion_make_gather,
	.motion_is = single_node_motion_is,
	.motion_segment = single_node_motion_segment,
	.motion_set_segment = single_node_motion_set_segment,
	.direct_dispatch_segment = single_node_direct_dispatch_segment,
	.motion_make_send = single_node_motion_make_send,
	.motion_type = single_node_motion_type,
	.motion_slice = single_node_motion_slice,
	.motion_set_prepare = single_node_motion_set_prepare,
	.motion_make_hash_filter = single_node_motion_make_hash_filter,
	.motion_make_dml = single_node_motion_make_dml,
	.split_make = single_node_split_make,
	.split_modify_make = single_node_split_modify_make,
	.motion_set_parent = single_node_motion_set_parent,
	.motion_set_params = single_node_motion_set_params,
	.motion_set_segments = single_node_motion_set_segments,
	.motion_segments = single_node_motion_segments,
	.direct_dispatch_contents = single_node_direct_dispatch_contents,
	.segment_of_function = single_node_segment_of_function,
	.prepare_query = single_node_prepare_query,
	.share_fileset = single_node_share_fileset,
	.split_modify_set_tableoid = single_node_split_modify_set_tableoid,
	.row_identity_make = single_node_row_identity_make,
	.explicit_write = single_node_explicit_write,
	.metatrack_partition = single_node_metatrack_partition,
	.endpoint_plan = single_node_endpoint_plan,
	.retrieve_sql = single_node_retrieve_sql,
	.extension_mark_add = single_node_extension_mark_add,
	.size_from_am_register = single_node_size_from_am_register,
};

SINGLE_NODE_HIDDEN const GpCoreApi *
GpOrcaSingleNodeCoreApi(void)
{
	return &single_node_api;
}

/* ------------------------------------------------------------------------- */
/* gp_core's functions                                                        */
/* ------------------------------------------------------------------------- */

/* Nothing labels a relation with a policy, so every one is the node's. */
SINGLE_NODE_HIDDEN GpPolicy *
GpPolicyGet(Oid relid)
{
	return NULL;
}

/* An external table is gp_exttable's, which needs gp_core. */
SINGLE_NODE_HIDDEN bool
GpPolicyIsExternalTable(Oid relid)
{
	return false;
}

/* gp_policy.c's predicates, a NULL policy the coordinator's */
SINGLE_NODE_HIDDEN bool
GpPolicyIsEntry(const GpPolicy *policy)
{
	return policy == NULL || policy->ptype == POLICYTYPE_ENTRY;
}

SINGLE_NODE_HIDDEN bool
GpPolicyIsRandomPartitioned(const GpPolicy *policy)
{
	return policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED &&
		policy->nattrs == 0;
}

SINGLE_NODE_HIDDEN bool
GpPolicyIsHashPartitioned(const GpPolicy *policy)
{
	return policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED &&
		policy->nattrs > 0;
}

SINGLE_NODE_HIDDEN bool
GpPolicyIsReplicated(const GpPolicy *policy)
{
	return policy != NULL && policy->ptype == POLICYTYPE_REPLICATED;
}

/*
 * The class a column is hashed with: the one named, or the type's default
 * hash class where the type can be a key at all, as gp_core's
 * GpPolicyColumnOpclass() finds it with gp.use_legacy_hashops off -- the
 * legacy classes are its extension's.
 */
SINGLE_NODE_HIDDEN Oid
GpPolicyColumnOpclass(List *opclassName, Oid typeoid)
{
	TypeCacheEntry *tcache;
	Oid			opclass = InvalidOid;

	if (opclassName != NIL)
		return ResolveOpClass(opclassName, typeoid, "hash", HASH_AM_OID);

	tcache = lookup_type_cache(typeoid,
							   TYPECACHE_HASH_OPFAMILY |
							   TYPECACHE_HASH_PROC |
							   TYPECACHE_EQ_OPR);
	if (OidIsValid(tcache->hash_opf) && OidIsValid(tcache->hash_proc) &&
		OidIsValid(tcache->eq_opr))
		opclass = GetDefaultOpClass(typeoid, HASH_AM_OID);
	if (!OidIsValid(opclass))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("data type %s has no default operator class for access method \"%s\"",
						format_type_be(typeoid), "hash"),
				 errhint("You must specify an operator class or define a default operator class for the data type.")));
	return opclass;
}

/*
 * The hash support function of a family for a type, or for a type it is
 * binary coercible to, as gp_core's GpHashProcInOpfamily() finds it.
 */
SINGLE_NODE_HIDDEN Oid
GpHashProcInOpfamily(Oid opfamily, Oid typeoid, bool missing_ok)
{
	Oid			hashfunc;
	CatCList   *catlist;

	hashfunc = get_opfamily_proc(opfamily, typeoid, typeoid, HASHSTANDARD_PROC);
	if (OidIsValid(hashfunc))
		return hashfunc;

	catlist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamily));
	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amproc amproc = (Form_pg_amproc) GETSTRUCT(tuple);

		if (amproc->amprocnum != HASHSTANDARD_PROC)
			continue;
		if (amproc->amproclefttype != amproc->amprocrighttype)
			continue;
		if (IsBinaryCoercible(typeoid, amproc->amproclefttype))
		{
			hashfunc = amproc->amproc;
			break;
		}
	}
	ReleaseSysCacheList(catlist);

	if (!OidIsValid(hashfunc) && !missing_ok)
		elog(ERROR, "could not find hash function for type %u in operator family %u",
			 typeoid, opfamily);

	return hashfunc;
}

/* The legacy cdbhash classes and their functions are gp_core's extension's. */
SINGLE_NODE_HIDDEN bool
GpHashIsLegacyFunction(Oid funcid)
{
	return false;
}

SINGLE_NODE_HIDDEN Oid
GpLegacyHashOpclassForType(Oid typid)
{
	return InvalidOid;
}

SINGLE_NODE_HIDDEN Oid
GpLegacyHashOpfamilyOfOperator(Oid opno)
{
	return InvalidOid;
}

/* No object has a "gp" label: only gp_core's provider makes one. */
SINGLE_NODE_HIDDEN char *
GpLabelGet(const ObjectAddress *object, GpLabelKey key)
{
	return NULL;
}

SINGLE_NODE_HIDDEN bool
GpLabelHas(const ObjectAddress *object, GpLabelKey key)
{
	return false;
}

/*
 * Where a foreign table is read: the coordinator, the only node.  On one
 * node the relcache translator does not ask (GetDistributionFromForeign-
 * RelExecLocation()).
 */
SINGLE_NODE_HIDDEN char
GpForeignExecLocation(Oid relid, int *numsegments)
{
	*numsegments = 1;
	return GP_FOREIGN_COORDINATOR;
}
