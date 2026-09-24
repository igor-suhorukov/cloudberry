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
 * storage.c
 *	  Storage servers, and the tablespaces that reach them.
 *
 * Cloudberry keeps storage servers and their user mappings in two shared
 * catalogs of their own, gp_storage_server and gp_storage_user_mapping, whose
 * columns are a foreign server's columns and whose rules about who may change
 * a mapping are a foreign server's rules.  PostgreSQL already has both, so a
 * storage server here is an ordinary SERVER of a data-less foreign data
 * wrapper, "gp_storage", and a storage user mapping is an ordinary USER
 * MAPPING.  Nothing is reimplemented: the option bookkeeping, the ownership
 * rules, pg_dump and the pg_user_mappings view that hides another user's
 * options all come with them.
 *
 * Cloudberry's are shared; these live in one database, gp.maintenance_database
 * -- their mappings hold credentials, so they cannot be labels, which is what
 * the port's shared metadata otherwise is.  From any other database the SQL
 * functions that make and change them are called there, as the transaction
 * commits, through gp_core's loopback, and the views read them there, as the
 * calling user, so that pg_user_mappings applies its own rule.  A tablespace
 * is the cluster's, and so, now, is the server its label names.
 *
 * A tablespace reaches a storage server through a "gp" label rather than
 * through the two pg_tablespace columns Cloudberry adds, and it names the
 * server only -- not a library and function, as Cloudberry's
 * spcfilehandlersrc and spcfilehandlerbin do.  A handler registers itself
 * with the extension instead, which is what "Directory tables, storage side"
 * decides.  No handler is registered yet, so a directory table asked to live
 * in such a tablespace is refused rather than quietly given local files.
 *
 * Cloudberry source this file is made of:
 *	  src/backend/commands/storagecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_tablespace.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "gp_label.h"
#include "gp_loopback.h"
#include "gp_sql.h"

/*
 * Take WITH (gp.server = '...') out of a tablespace's option list, so that
 * what is left is something PostgreSQL will accept.
 */
List *
GpStorageTakeTablespaceOptions(List **options)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace == NULL ||
			strcmp(def->defnamespace, GP_OPTION_NS) != 0)
			continue;

		if (strcmp(def->defname, "server") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized tablespace option \"%s.%s\"",
							GP_OPTION_NS, def->defname),
					 errhint("The only one is %s.server.", GP_OPTION_NS)));

		taken = lappend(taken, makeDefElem(pstrdup(def->defname), def->arg, -1));
		*options = foreach_delete_current(*options, lc);
	}

	return taken;
}

/*
 * Record on the tablespace which storage server its files go through.  The
 * label is shared, as the tablespace is, so every database sees it.
 */
void
GpStorageApplyToTablespace(const char *spcname, List *opts)
{
	Oid			spcId;
	ObjectAddress addr;
	ListCell   *lc;

	if (opts == NIL)
		return;

	spcId = get_tablespace_oid(spcname, false);
	ObjectAddressSet(addr, TableSpaceRelationId, spcId);

	foreach(lc, opts)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		/* No value takes the setting away, as RESET does elsewhere. */
		if (def->arg == NULL)
		{
			GpLabelSet(&addr, GP_LABEL_storage_server, NULL);
			continue;
		}

		GpLabelSet(&addr, GP_LABEL_storage_server, defGetString(def));
	}
}

/*
 * Which storage server this tablespace's files go through, or NULL for an
 * ordinary local tablespace.
 */
char *
GpStorageTablespaceServer(Oid spcId)
{
	ObjectAddress addr;

	if (!OidIsValid(spcId))
		return NULL;

	ObjectAddressSet(addr, TableSpaceRelationId, spcId);
	return GpLabelGet(&addr, GP_LABEL_storage_server);
}

PG_FUNCTION_INFO_V1(gp_sql_tablespace_storage_server);

/*
 * gp_sql.tablespace_storage_server(name) -> text
 *
 * What Cloudberry answers from pg_tablespace.spcfilehandlersrc.
 */
Datum
gp_sql_tablespace_storage_server(PG_FUNCTION_ARGS)
{
	Name		spcname = PG_GETARG_NAME(0);
	Oid			spcId = get_tablespace_oid(NameStr(*spcname), true);
	char	   *server;

	if (!OidIsValid(spcId))
		PG_RETURN_NULL();

	server = GpStorageTablespaceServer(spcId);
	if (server == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(server));
}

PG_FUNCTION_INFO_V1(gp_sql_forward_storage);
PG_FUNCTION_INFO_V1(gp_sql_storage_server_rows);
PG_FUNCTION_INFO_V1(gp_sql_storage_user_mapping_rows);

/*
 * gp_sql.forward_storage(function text, args text[])
 *
 * One of the functions that make and change storage servers and their
 * mappings, called in gp.maintenance_database with the same arguments, as
 * this transaction commits.  Only those five, each argument quoted here, so
 * that what reaches the other database is one of them and nothing else.
 */
Datum
gp_sql_forward_storage(PG_FUNCTION_ARGS)
{
	static const char *const functions[] = {
		"create_storage_server", "alter_storage_server", "drop_storage_server",
		"create_storage_user_mapping", "drop_storage_user_mapping",
	};
	char	   *function = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	bool		known = false;
	StringInfoData sql;

	for (int i = 0; i < lengthof(functions); i++)
		known |= strcmp(function, functions[i]) == 0;
	if (!known)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a storage server function of gp_sql's", function)));

	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(1), TEXTOID, &elems, &nulls, &n);
	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT " GP_SQL_SCHEMA ".%s(", function);
	for (int i = 0; i < n; i++)
		appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "",
						 nulls[i] ? "NULL"
						 : quote_literal_cstr(TextDatumGetCString(elems[i])));
	appendStringInfoChar(&sql, ')');

	GpLoopbackDefer(GpLoopbackMaintenanceDatabase(), sql.data);
	PG_RETURN_VOID();
}

/* gp_sql.storage_server_rows(): the servers, from the maintenance database. */
Datum
gp_sql_storage_server_rows(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	GpLoopbackQueryInto(GpLoopbackMaintenanceDatabase(),
						"SELECT s.srvname, pg_catalog.pg_get_userbyid(s.srvowner),"
						"       s.srvoptions"
						"  FROM pg_catalog.pg_foreign_server s"
						"  JOIN pg_catalog.pg_foreign_data_wrapper w ON w.oid = s.srvfdw"
						" WHERE w.fdwname = 'gp_storage'",
						(ReturnSetInfo *) fcinfo->resultinfo);
	return (Datum) 0;
}

/*
 * gp_sql.storage_user_mapping_rows(): the mappings, with their options as
 * pg_user_mappings shows them there to this user.
 */
Datum
gp_sql_storage_user_mapping_rows(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	GpLoopbackQueryInto(GpLoopbackMaintenanceDatabase(),
						"SELECT m.srvname, m.usename, m.umoptions"
						"  FROM pg_catalog.pg_user_mappings m"
						"  JOIN pg_catalog.pg_foreign_server s ON s.srvname = m.srvname"
						"  JOIN pg_catalog.pg_foreign_data_wrapper w ON w.oid = s.srvfdw"
						" WHERE w.fdwname = 'gp_storage'",
						(ReturnSetInfo *) fcinfo->resultinfo);
	return (Datum) 0;
}
