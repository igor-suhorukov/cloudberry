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
 * gp_size.c
 *	  The size functions, the cluster's: the coordinator's size of a
 *	  relation, a database or a tablespace and every segment's.
 *
 * Cloudberry's pg_relation_size(), pg_table_size(), pg_indexes_size(),
 * pg_total_relation_size(), pg_database_size() and pg_tablespace_size(),
 * called on the coordinator, add to its own answer each segment's to the
 * same call (dbsize.c, get_size_from_segDBs()): a distributed table's size
 * is the size of all its rows.  PostgreSQL's answer only for the node they
 * run on, and an extension cannot change a built-in function.  What it can
 * change is the query: before the planner, PostgreSQL's or ORCA, plans a
 * statement on a cluster's coordinator, each call of one of them becomes a
 * call of the function of the same name and arguments in gp_internal,
 * which is PostgreSQL's here plus the segments'.  The query is changed and
 * not what it was made of, so a view or a rule still says pg_relation_size.
 * On a segment, and on one node, the calls are PostgreSQL's.
 *
 * And pg_tablespace_location(), which is no size, but is answered the same
 * way, on every node of a cluster: a node's directory of a tablespace is
 * the directory of its dbid under the location CREATE TABLESPACE was given
 * (gp_ddl.c), and Cloudberry's pg_tablespace_location() says the location,
 * which is what pg_dump has to write for a restore to put each node's
 * directory under it again.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_size.h"

/*
 * Each of PostgreSQL's size functions, and gp_internal's of its name; and
 * pg_tablespace_location(), the one called on every node of a cluster, not
 * on its coordinator alone.
 */
static const struct
{
	Oid			builtin;
	const char *name;
	int			nargs;
	Oid			argtypes[2];
	bool		every_node;
}			size_functions[] = {
	{F_PG_RELATION_SIZE_REGCLASS, "relation_size", 1, {REGCLASSOID}},
	{F_PG_RELATION_SIZE_REGCLASS_TEXT, "relation_size", 2, {REGCLASSOID, TEXTOID}},
	{F_PG_TABLE_SIZE, "table_size", 1, {REGCLASSOID}},
	{F_PG_INDEXES_SIZE, "indexes_size", 1, {REGCLASSOID}},
	{F_PG_TOTAL_RELATION_SIZE, "total_relation_size", 1, {REGCLASSOID}},
	{F_PG_DATABASE_SIZE_NAME, "database_size", 1, {NAMEOID}},
	{F_PG_DATABASE_SIZE_OID, "database_size", 1, {OIDOID}},
	{F_PG_TABLESPACE_SIZE_NAME, "tablespace_size", 1, {NAMEOID}},
	{F_PG_TABLESPACE_SIZE_OID, "tablespace_size", 1, {OIDOID}},
	{F_PG_TABLESPACE_LOCATION, "tablespace_location", 1, {OIDOID}, true},
};

/* Looked up on first use and again whenever pg_proc changes. */
static Oid	wrappers[lengthof(size_functions)];
static bool wrappers_valid = false;
static bool callback_registered = false;

static void
invalidate_wrappers(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	wrappers_valid = false;
}

static void
lookup_wrappers(void)
{
	Oid			nsp;

	if (wrappers_valid)
		return;
	if (!callback_registered)
	{
		CacheRegisterSyscacheCallback(PROCOID, invalidate_wrappers, (Datum) 0);
		callback_registered = true;
	}
	nsp = get_namespace_oid("gp_internal", true);
	for (int i = 0; i < lengthof(size_functions); i++)
		wrappers[i] = !OidIsValid(nsp) ? InvalidOid :
			GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
							CStringGetDatum(size_functions[i].name),
							PointerGetDatum(buildoidvector(size_functions[i].argtypes,
														   size_functions[i].nargs)),
							ObjectIdGetDatum(nsp));
	wrappers_valid = true;
}

/* context: whether this is the coordinator, whose sizes are the cluster's */
static bool
size_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, size_walker, context, 0);
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *fexpr = (FuncExpr *) node;

		for (int i = 0; i < lengthof(size_functions); i++)
			if (fexpr->funcid == size_functions[i].builtin &&
				OidIsValid(wrappers[i]) &&
				(size_functions[i].every_node || *(bool *) context))
				fexpr->funcid = wrappers[i];
	}
	return expression_tree_walker(node, size_walker, context);
}

/*
 * GpSizeRewrite
 *		The statement's calls of the size functions, made the cluster's on
 *		its coordinator, and of pg_tablespace_location(), made the
 *		location's on each of its nodes.
 */
void
GpSizeRewrite(Query *parse)
{
	bool		coordinator;

	if (GpClusterIsSingleNode())
		return;
	coordinator = GpClusterBackendRole() == GP_ROLE_DISPATCH;
	lookup_wrappers();
	(void) size_walker((Node *) parse, &coordinator);
}

/* ------------------------------------------------------------------------- */
/* gp_internal's size functions                                              */
/* ------------------------------------------------------------------------- */

/*
 * PostgreSQL's function here, with these arguments, and on a cluster's
 * coordinator each segment's answer to "sql" added: a null there is none, as
 * Cloudberry takes it; a null here, a relation or a database dropped, is
 * the answer.
 */
static Datum
cluster_size(FunctionCallInfo fcinfo, PGFunction builtin, int nargs,
			 const NullableDatum *args, char *sql)
{
	LOCAL_FCINFO(local, 2);
	int64		size;

	InitFunctionCallInfoData(*local, NULL, nargs, InvalidOid, NULL, NULL);
	for (int i = 0; i < nargs; i++)
		local->args[i] = args[i];
	size = DatumGetInt64(builtin(local));
	if (local->isnull)
		PG_RETURN_NULL();

	if (!GpClusterIsSingleNode() && GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		char	  **values = palloc0_array(char *, GpClusterSegmentCount());

		GpDispatchQueryFirstValues(sql, -1, values);
		for (int i = 0; i < GpClusterSegmentCount(); i++)
			if (values[i] != NULL)
				size += DatumGetInt64(DirectFunctionCall1(int8in,
														  CStringGetDatum(values[i])));
	}
	PG_RETURN_INT64(size);
}

/* This call's arguments, as PostgreSQL's function of the same takes them. */
#define SAME_ARGS	PG_NARGS(), fcinfo->args

#define REGCLASS_ARG(n) \
	psprintf("%u::pg_catalog.regclass", PG_GETARG_OID(n))

PG_FUNCTION_INFO_V1(gp_relation_size);

/* gp_internal.relation_size(regclass [, text]): pg_relation_size() */
Datum
gp_relation_size(PG_FUNCTION_ARGS)
{
	NullableDatum args[2];

	/* pg_relation_size(regclass) is pg_relation_size($1, 'main') */
	args[0] = fcinfo->args[0];
	if (PG_NARGS() > 1)
		args[1] = fcinfo->args[1];
	else
	{
		args[1].value = CStringGetTextDatum("main");
		args[1].isnull = false;
	}
	return cluster_size(fcinfo, pg_relation_size, 2, args,
						psprintf("SELECT pg_catalog.pg_relation_size(%s, %s)",
								 REGCLASS_ARG(0),
								 quote_literal_cstr(TextDatumGetCString(args[1].value))));
}

PG_FUNCTION_INFO_V1(gp_table_size);

/* gp_internal.table_size(regclass): pg_table_size() */
Datum
gp_table_size(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_table_size, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_table_size(%s)", REGCLASS_ARG(0)));
}

PG_FUNCTION_INFO_V1(gp_indexes_size);

/* gp_internal.indexes_size(regclass): pg_indexes_size() */
Datum
gp_indexes_size(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_indexes_size, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_indexes_size(%s)", REGCLASS_ARG(0)));
}

PG_FUNCTION_INFO_V1(gp_total_relation_size);

/* gp_internal.total_relation_size(regclass): pg_total_relation_size() */
Datum
gp_total_relation_size(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_total_relation_size, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_total_relation_size(%s)",
								 REGCLASS_ARG(0)));
}

PG_FUNCTION_INFO_V1(gp_database_size_name);

/* gp_internal.database_size(name): pg_database_size() */
Datum
gp_database_size_name(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_database_size_name, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_database_size(%s::pg_catalog.name)",
								 quote_literal_cstr(NameStr(*PG_GETARG_NAME(0)))));
}

PG_FUNCTION_INFO_V1(gp_database_size_oid);

/* gp_internal.database_size(oid): pg_database_size() */
Datum
gp_database_size_oid(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_database_size_oid, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_database_size(%u::pg_catalog.oid)",
								 PG_GETARG_OID(0)));
}

PG_FUNCTION_INFO_V1(gp_tablespace_size_name);

/* gp_internal.tablespace_size(name): pg_tablespace_size() */
Datum
gp_tablespace_size_name(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_tablespace_size_name, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_tablespace_size(%s::pg_catalog.name)",
								 quote_literal_cstr(NameStr(*PG_GETARG_NAME(0)))));
}

PG_FUNCTION_INFO_V1(gp_tablespace_size_oid);

/* gp_internal.tablespace_size(oid): pg_tablespace_size() */
Datum
gp_tablespace_size_oid(PG_FUNCTION_ARGS)
{
	return cluster_size(fcinfo, pg_tablespace_size_oid, SAME_ARGS,
						psprintf("SELECT pg_catalog.pg_tablespace_size(%u::pg_catalog.oid)",
								 PG_GETARG_OID(0)));
}

PG_FUNCTION_INFO_V1(gp_tablespace_location);

/*
 * gp_internal.tablespace_location(oid): pg_tablespace_location(), which says
 * where pg_tblspc links, less the directory of this node's dbid that
 * gp_ddl.c's node_tablespace_location() made under the location: the
 * location CREATE TABLESPACE was given, as Cloudberry's says it.  An
 * in-place tablespace, or one of another making, is PostgreSQL's answer.
 */
Datum
gp_tablespace_location(PG_FUNCTION_ARGS)
{
	Datum		answer = DirectFunctionCall1(pg_tablespace_location,
											 PG_GETARG_DATUM(0));
	char	   *path = TextDatumGetCString(answer);
	char	   *suffix = psprintf("/%d", GpClusterDbid());
	size_t		len = strlen(path);
	size_t		slen = strlen(suffix);

	if (is_absolute_path(path) && len > slen &&
		strcmp(path + len - slen, suffix) == 0)
	{
		path[len - slen] = '\0';
		PG_RETURN_TEXT_P(cstring_to_text(path));
	}
	PG_RETURN_DATUM(answer);
}
