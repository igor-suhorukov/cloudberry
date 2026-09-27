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
 * And Cloudberry's own: cbdb_relation_size(), the sizes of many relations
 * at once, the segments asked once for them all; gp_tablespace_location(),
 * each node's location of a tablespace; and
 * get_tablespace_version_directory_name(), the directory each node makes
 * under a tablespace's, PostgreSQL 19's here.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/catversion.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/relpath.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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

PG_FUNCTION_INFO_V1(gp_tablespace_segment_location);

/*
 * gp_internal.tablespace_segment_location(oid): each segment's location of
 * the tablespace, as gp_internal.tablespace_location() says it there, with
 * the segment's content id -- Cloudberry's gp_tablespace_segment_location(),
 * EXECUTE ON ALL SEGMENTS.  None from the coordinator, which
 * gp_tablespace_location() asks for its own.
 */
Datum
gp_tablespace_segment_location(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	if (GpClusterContentId() < 0)
		return (Datum) 0;
	values[0] = Int32GetDatum(GpClusterContentId());
	values[1] = DirectFunctionCall1(gp_tablespace_location, PG_GETARG_DATUM(0));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_tablespace_version_directory_name);

/*
 * get_tablespace_version_directory_name(): the directory of this release a
 * node makes under its directory of a tablespace -- PostgreSQL 19's
 * TABLESPACE_VERSION_DIRECTORY, where Cloudberry's is its own
 * GP_TABLESPACE_VERSION_DIRECTORY, "GPDB_<major>_<catalog version>".
 */
Datum
gp_tablespace_version_directory_name(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(TABLESPACE_VERSION_DIRECTORY));
}

PG_FUNCTION_INFO_V1(gp_cbdb_relation_size);

/*
 * cbdb_relation_size(oid[], text): the size of a fork of each relation, the
 * cluster's, in the array's order -- Cloudberry's (dbsize.c), which asks
 * every segment once for them all where pg_relation_size() asks for each.
 * A relation that is gone is 0.  A foreign table, an external table among
 * them, has no files and its wrapper no size to give: 0, with Cloudberry's
 * WARNING, and the segments are not asked.
 */
Datum
gp_cbdb_relation_size(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum	   *oids;
	int			n;
	int64	   *sizes;
	StringInfoData asked;
	int			nasked = 0;

	if (array_contains_nulls(array))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("cannot work with arrays containing NULLs")));
	/* refuse a fork of no name before anything */
	(void) forkname_to_number(text_to_cstring(forkname));

	deconstruct_array_builtin(array, OIDOID, &oids, NULL, &n);
	sizes = palloc0_array(int64, n);
	initStringInfo(&asked);

	for (int i = 0; i < n; i++)
	{
		Oid			relid = DatumGetObjectId(oids[i]);
		Relation	rel = try_relation_open(relid, AccessShareLock);

		if (rel == NULL)
			continue;
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot calculate this foreign table size",
							RelationGetRelationName(rel))));
			relation_close(rel, AccessShareLock);
			continue;
		}
		/* this node's, the relation held open so that it is still there */
		sizes[i] = DatumGetInt64(DirectFunctionCall2(pg_relation_size,
													 ObjectIdGetDatum(relid),
													 PointerGetDatum(forkname)));
		relation_close(rel, AccessShareLock);

		/* the segments are asked of each relation once */
		for (int j = 0; j < i; j++)
			if (DatumGetObjectId(oids[j]) == relid)
				goto asked_already;
		appendStringInfo(&asked, "%s%u", nasked++ > 0 ? "," : "", relid);
asked_already:
		;
	}

	/* and every segment's, each segment's as one line of "oid size" pairs */
	if (nasked > 0 && !GpClusterIsSingleNode() &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		int			nsegments = GpClusterSegmentCount();
		char	  **values = palloc0_array(char *, nsegments);

		GpDispatchQueryFirstValues(psprintf("SELECT pg_catalog.string_agg(reloid || ' ' || size, ' ')"
											" FROM pg_catalog.cbdb_relation_size('{%s}'::pg_catalog.oid[], %s)",
											asked.data,
											quote_literal_cstr(text_to_cstring(forkname))),
								   -1, values);
		for (int s = 0; s < nsegments; s++)
		{
			char	   *p = values[s];

			while (p != NULL && *p != '\0')
			{
				char	   *end;
				Oid			relid = (Oid) strtoul(p, &end, 10);
				int64		size = strtoi64(end, &end, 10);

				for (int i = 0; i < n; i++)
					if (DatumGetObjectId(oids[i]) == relid)
						sizes[i] += size;
				p = end;
				while (*p == ' ')
					p++;
			}
		}
	}

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < n; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = oids[i];
		values[1] = Int64GetDatum(sizes[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}
