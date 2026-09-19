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
 * dirtable.c
 *	  Directory tables: files on disk with a row each.
 *
 * Cloudberry gives a directory table a relkind of its own ('d'), a fixed
 * schema, a row in the pg_directory_table catalog saying where its files are,
 * and a check in the executor that refuses DML on it.  None of the first
 * three is open to an extension, so a directory table here is an ordinary
 * table with the same five columns and a "gp" label holding the location --
 * the label being both the flag and the value, as a dynamic table's schedule
 * is.  The executor check becomes an ExecutorStart_hook.
 *
 * The files live where Cloudberry puts them, in a directory per table inside
 * the database directory, so that a base backup and pg_rewind carry them: both
 * treat anything that is not a relation file as a file to copy whole.  Offline
 * pg_checksums does not -- it walks every file under base/ and fails on one
 * whose size is not a multiple of a page.  That hazard is Cloudberry's too,
 * and what removes it is O23, which is not taken.
 *
 * Removing a file has to wait for the transaction that removed the row, and
 * writing one has to be undone if that transaction rolls back, so both go
 * through a list this module keeps and a transaction callback drains.  A
 * crash between the two leaves a file nothing points at; the queue that
 * removes those is Track D's work, with the rest of the storage side.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/commands/dirtablecmds.c, catalog/pg_directory_table.c,
 *	  storage/file/ufile.c (the local file handler)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/relpath.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_label.h"
#include "gp_sql.h"

/* Cloudberry's DIRECTORY_TABLE_TAG_COLUMN_ATTNUM: the only column DML may touch. */
#define GP_DIRTABLE_TAG_ATTNUM	5

/* Cloudberry's allow_dml_directory_table, under the port's naming. */
bool		gp_allow_dml_directory_table = false;

/*
 * Raised while this module writes a directory table's own rows, so that the
 * guard lets its own work through.  A counter rather than a flag, because
 * put() is allowed to be reached from a function that is already inside one.
 */
static int	dirtable_maintenance_depth = 0;

/* ------------------------------------------------------------------------- */
/* Files to forget at the end of the transaction                             */
/* ------------------------------------------------------------------------- */

typedef struct DirTableFileAction
{
	char	   *path;			/* relative to the data directory */
	bool		on_commit;		/* remove it if we commit, else if we abort */
	bool		is_dir;			/* a whole directory, from DROP TABLE */
} DirTableFileAction;

static List *dirtable_actions = NIL;
static bool dirtable_xact_callback_set = false;

static void
dirtable_remember(const char *path, bool on_commit, bool is_dir)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	DirTableFileAction *act = palloc(sizeof(DirTableFileAction));

	act->path = pstrdup(path);
	act->on_commit = on_commit;
	act->is_dir = is_dir;
	dirtable_actions = lappend(dirtable_actions, act);

	MemoryContextSwitchTo(old);
}

/* Remove a directory and everything in it, complaining rather than failing. */
static void
dirtable_rmtree(const char *dir)
{
	if (!rmtree(dir, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\" of a dropped directory table",
						dir),
				 errdetail("Its files are left behind and have to be removed by hand.")));
}

/*
 * The transaction is over: the files it created or removed follow it.
 *
 * Nothing here may throw.  By the time a commit reaches this point the
 * transaction is already durable, and a file that is left behind is a far
 * smaller problem than a PANIC.
 */
static void
dirtable_xact_callback(XactEvent event, void *arg)
{
	ListCell   *lc;
	bool		committed;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
	{
		return;
	}

	if (event == XACT_EVENT_PREPARE)
	{
		/*
		 * A prepared transaction outlives this backend, so nothing here can
		 * be carried to its commit.  Two-phase commit over directory tables
		 * is Track C's, with the rest of the distributed transaction.
		 */
		dirtable_actions = NIL;
		return;
	}

	committed = (event == XACT_EVENT_COMMIT);

	foreach(lc, dirtable_actions)
	{
		DirTableFileAction *act = (DirTableFileAction *) lfirst(lc);

		if (act->on_commit != committed)
			continue;

		if (act->is_dir)
			dirtable_rmtree(act->path);
		else if (unlink(act->path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", act->path)));
	}

	dirtable_actions = NIL;
}

void
GpDirTableRegisterXactCallback(void)
{
	if (!dirtable_xact_callback_set)
	{
		RegisterXactCallback(dirtable_xact_callback, NULL);
		dirtable_xact_callback_set = true;
	}
}

/* ------------------------------------------------------------------------- */
/* Where a directory table keeps its files                                   */
/* ------------------------------------------------------------------------- */

/*
 * The directory of a table that does not have one yet.  It is Cloudberry's
 * localFormatPathName: the database's own directory, in the table's
 * tablespace, plus a name made from the relation's OID.
 */
static char *
dirtable_compute_location(Oid relid)
{
	Oid			reltablespace = get_rel_tablespace(relid);
	char	   *dbpath = GetDatabasePath(MyDatabaseId,
										 OidIsValid(reltablespace)
										 ? reltablespace : MyDatabaseTableSpace);

	return psprintf("%s/%u_dirtable", dbpath, relid);
}

/*
 * Where this relation keeps its files, or NULL if it is not a directory
 * table.  The label is what says it is one.
 */
char *
GpDirTableLocation(Oid relid)
{
	ObjectAddress addr;

	if (get_rel_relkind(relid) != RELKIND_RELATION)
		return NULL;

	ObjectAddressSet(addr, RelationRelationId, relid);
	return GpLabelGet(&addr, GP_LABEL_directory_location);
}

/*
 * A path a user gave, joined to the table's directory.
 *
 * A relative path must stay inside that directory: it names a file of this
 * table, not a place on the server's disk.  So no absolute path and no ".."
 * component, which is the check Cloudberry does not make.
 */
static char *
dirtable_file_path(Oid relid, const char *relative_path, char **location_out)
{
	char	   *location = GpDirTableLocation(relid);
	const char *p;

	if (location == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a directory table", get_rel_name(relid))));

	if (relative_path[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a file path must not be empty")));

	if (is_absolute_path(relative_path))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a directory table file path must be relative: \"%s\"",
						relative_path)));

	for (p = relative_path; *p != '\0'; p++)
	{
		if (p[0] == '.' && p[1] == '.' &&
			(p[2] == '\0' || p[2] == '/') &&
			(p == relative_path || p[-1] == '/'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a directory table file path must not leave the table's directory: \"%s\"",
							relative_path)));
	}

	if (location_out != NULL)
		*location_out = location;

	return psprintf("%s/%s", location, relative_path);
}

/* Create the directories a path needs, as Cloudberry's localEnsurePath does. */
static void
dirtable_ensure_dir(const char *path)
{
	char	   *dir = pstrdup(path);
	char	   *slash = strrchr(dir, '/');

	if (slash == NULL)
		return;
	*slash = '\0';

	if (pg_mkdir_p(dir, pg_dir_create_mode) != 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));

	pfree(dir);
}

/* ------------------------------------------------------------------------- */
/* The DML guard                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry refuses INSERT and DELETE on a directory table, and allows
 * UPDATE only of the tag column, in ExecMain.  PG19 has no such check to
 * change, so the same question is asked of the finished plan.
 *
 * Only a statement that writes pays for this: reads never reach it.
 */
void
GpDirTableCheckDML(QueryDesc *queryDesc)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	ModifyTable *node;
	ListCell   *lc;
	int			which = 0;

	if (dirtable_maintenance_depth > 0 || gp_allow_dml_directory_table)
		return;

	if (stmt == NULL || stmt->commandType == CMD_SELECT ||
		bms_is_empty(stmt->resultRelationRelids))
		return;

	if (stmt->planTree == NULL || !IsA(stmt->planTree, ModifyTable))
		return;

	node = (ModifyTable *) stmt->planTree;

	foreach(lc, node->resultRelations)
	{
		Index		rti = lfirst_int(lc);
		RangeTblEntry *rte = rt_fetch(rti, stmt->rtable);
		char	   *location;

		which++;

		if (rte->rtekind != RTE_RELATION)
			continue;

		location = GpDirTableLocation(rte->relid);
		if (location == NULL)
			continue;

		if (node->operation != CMD_UPDATE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change directory table \"%s\"",
							get_rel_name(rte->relid)),
					 errhint("Use %s.directory_table_put() and %s.remove_file().",
							 GP_SQL_SCHEMA, GP_SQL_SCHEMA)));

		/*
		 * An UPDATE may set the tag and nothing else, which is what
		 * Cloudberry allows.
		 */
		if (list_length(node->updateColnosLists) >= which)
		{
			List	   *colnos = (List *) list_nth(node->updateColnosLists, which - 1);
			ListCell   *cl;

			foreach(cl, colnos)
			{
				if (lfirst_int(cl) != GP_DIRTABLE_TAG_ATTNUM)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("only the \"tag\" column of directory table \"%s\" may be updated",
									get_rel_name(rte->relid))));
			}
		}
	}
}

/*
 * TRUNCATE empties the table without touching the files, so every file it
 * describes would be left with nothing pointing at it.
 */
void
GpDirTableCheckTruncate(TruncateStmt *stmt)
{
	ListCell   *lc;

	if (gp_allow_dml_directory_table)
		return;

	foreach(lc, stmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(lc);
		Oid			relid = RangeVarGetRelid(rv, NoLock, true);

		if (OidIsValid(relid) && GpDirTableLocation(relid) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot truncate directory table \"%s\"",
							get_rel_name(relid)),
					 errhint("Remove its files with %s.remove_file().",
							 GP_SQL_SCHEMA)));
	}
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_sql_dirtable_claim);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_location);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_put);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_get);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_remove);

static void
dirtable_require_owner(Oid relid)
{
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));
}

/*
 * gp_sql.dirtable_claim(regclass) -> text
 *
 * Make an ordinary table into a directory table: work out where its files go,
 * record that in its label, and create the directory.  What Cloudberry does
 * inside CREATE DIRECTORY TABLE.
 */
Datum
gp_sql_dirtable_claim(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ObjectAddress addr;
	char	   *location;
	struct stat st;

	dirtable_require_owner(relid);

	if (GpDirTableLocation(relid) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is already a directory table", get_rel_name(relid))));

	{
		Oid			reltablespace = get_rel_tablespace(relid);
		char	   *server = GpStorageTablespaceServer(OidIsValid(reltablespace)
													   ? reltablespace
													   : MyDatabaseTableSpace);

		/*
		 * The tablespace says its files go through a storage server, and
		 * nothing has registered a handler for one.  Writing local files
		 * where the user asked for remote ones would be worse than refusing.
		 */
		if (server != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create a directory table in a tablespace that reaches storage server \"%s\"",
							server),
					 errdetail("No module has registered a handler for a storage server, so its files could only be written locally."),
					 errhint("Use a tablespace without %s.server, or load a module that provides the handler.",
							 GP_OPTION_NS)));
	}

	location = dirtable_compute_location(relid);

	if (stat(location, &st) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
				 errmsg("directory \"%s\" already exists", location),
				 errdetail("A directory table dropped by a transaction that has not ended yet still owns it.")));

	if (pg_mkdir_p(location, pg_dir_create_mode) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", location)));

	/* If this transaction rolls back, the directory goes with it. */
	dirtable_remember(location, false, true);

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_directory_location, location);

	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * gp_sql.directory_table_location(regclass) -> text
 *
 * NULL for a table that is not a directory table, which is what makes the
 * label the flag as well.
 */
Datum
gp_sql_dirtable_location(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *location = GpDirTableLocation(relid);

	if (location == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * gp_sql.directory_table_put(regclass, path, content, tag) -> bigint
 *
 * Write a file and the row that describes it.  Cloudberry writes both with
 * COPY into a directory table; PG19's COPY has no place to put the file, so
 * this is the form the O26 grammar desugars that COPY to.
 */
Datum
gp_sql_dirtable_put(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *relpath_txt = PG_GETARG_TEXT_PP(1);
	bytea	   *content = PG_GETARG_BYTEA_PP(2);
	char	   *tag = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *relative_path = text_to_cstring(relpath_txt);
	char	   *path;
	int			fd;
	int			len = VARSIZE_ANY_EXHDR(content);
	char	   *data = VARDATA_ANY(content);
	Oid			argtypes[5] = {TEXTOID, INT8OID, TEXTOID, TEXTOID, TEXTOID};
	Datum		values[5];
	char		nulls[5] = {' ', ' ', ' ', ' ', ' '};
	StringInfoData sql;
	bool		existed;

	dirtable_require_owner(relid);
	path = dirtable_file_path(relid, relative_path, NULL);

	existed = (access(path, F_OK) == 0);
	if (existed)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
				 errmsg("file \"%s\" already exists in directory table \"%s\"",
						relative_path, get_rel_name(relid)),
				 errhint("Remove it with %s.remove_file() first: rolling back cannot put the bytes it held back.",
						 GP_SQL_SCHEMA)));

	dirtable_ensure_dir(path);

	fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	if (len > 0 && write(fd, data, len) != len)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}

	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/* If this transaction rolls back, the file it wrote goes with it. */
	dirtable_remember(path, false, false);

	values[0] = CStringGetTextDatum(relative_path);
	values[1] = Int64GetDatum((int64) len);
	values[2] = DirectFunctionCall1(md5_bytea, PointerGetDatum(content));
	values[3] = tag ? CStringGetTextDatum(tag) : (Datum) 0;
	if (tag == NULL)
		nulls[3] = 'n';

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO %s (relative_path, size, last_modified, md5, tag)"
					 " VALUES ($1, $2, now(), $3, $4)",
					 quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
												get_rel_name(relid)));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	dirtable_maintenance_depth++;
	PG_TRY();
	{
		int			ret = SPI_execute_with_args(sql.data, 4, argtypes, values,
												nulls, false, 0);

		if (ret != SPI_OK_INSERT)
			elog(ERROR, "gp_sql: could not record a directory table file");
	}
	PG_FINALLY();
	{
		dirtable_maintenance_depth--;
	}
	PG_END_TRY();

	SPI_finish();

	PG_RETURN_INT64((int64) len);
}

/*
 * gp_sql.directory_table_get(regclass, path) -> bytea
 */
Datum
gp_sql_dirtable_get(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *relative_path = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *path = dirtable_file_path(relid, relative_path, NULL);
	int			fd;
	struct stat st;
	bytea	   *result;
	int			nbytes;

	if (stat(path, &st) != 0)
	{
		if (errno == ENOENT)
			PG_RETURN_NULL();
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}

	if (st.st_size > (off_t) (MaxAllocSize - VARHDRSZ))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file \"%s\" is too large to return as a value", path)));

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	result = (bytea *) palloc(VARHDRSZ + st.st_size);
	SET_VARSIZE(result, VARHDRSZ + st.st_size);

	nbytes = (st.st_size > 0) ? (int) read(fd, VARDATA(result), st.st_size) : 0;
	if (nbytes != (int) st.st_size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	}

	CloseTransientFile(fd);

	PG_RETURN_BYTEA_P(result);
}

/*
 * gp_sql.remove_file(regclass, path) -> boolean
 *
 * The row goes now and the file goes when the transaction commits, so a
 * rollback leaves both where they were.  Cloudberry's remove_file() dispatches
 * remove_file_segment() to the segments; on one node there is nothing to
 * dispatch.
 */
Datum
gp_sql_dirtable_remove(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *relative_path = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *path;
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];
	StringInfoData sql;
	uint64		removed;

	dirtable_require_owner(relid);
	path = dirtable_file_path(relid, relative_path, NULL);

	initStringInfo(&sql);
	appendStringInfo(&sql, "DELETE FROM %s WHERE relative_path = $1",
					 quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
												get_rel_name(relid)));

	values[0] = CStringGetTextDatum(relative_path);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	dirtable_maintenance_depth++;
	PG_TRY();
	{
		if (SPI_execute_with_args(sql.data, 1, argtypes, values, NULL, false, 0)
			!= SPI_OK_DELETE)
			elog(ERROR, "gp_sql: could not remove a directory table row");
	}
	PG_FINALLY();
	{
		dirtable_maintenance_depth--;
	}
	PG_END_TRY();

	removed = SPI_processed;
	SPI_finish();

	/*
	 * Also when there was no row: a crash between writing a file and
	 * committing its row leaves one behind, and this is what removes it.
	 */
	if (removed > 0 || access(path, F_OK) == 0)
		dirtable_remember(path, true, false);

	PG_RETURN_BOOL(removed > 0);
}

/*
 * A directory table is being dropped: its files follow it, once the drop
 * commits.
 */
void
GpDirTableDropped(Oid relid)
{
	char	   *location = GpDirTableLocation(relid);

	if (location != NULL)
		dirtable_remember(location, true, true);
}
