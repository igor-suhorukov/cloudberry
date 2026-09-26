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
 * WAL.  Cloudberry logs nothing of a directory table's files, so a mirror
 * has none of them.  The port logs them (M4, as the plan decided for Track
 * D, section 6, question 4), through a WAL resource manager of this
 * module's own, gp_dirtable: a directory as it is made, a file's bytes as it
 * is written, a record to each megabyte, and a file or a directory as it is
 * removed -- for a removal the transaction callback makes, after the commit
 * or abort record.  A mirror or a standby replays them, and has every local
 * file its primary has.  A server that replays them must preload gp_sql, as
 * a server that reads the rest of what the port writes must: without it,
 * recovery stops at the first such record, with PostgreSQL's FATAL.  A file
 * a storage server keeps is not on the node, and nothing is logged of it.
 * A database's directory tables are in its directory, so the replay of DROP
 * DATABASE takes their files with it.
 *
 * A directory table in a tablespace that reaches a storage server keeps its
 * files there instead, through the handler a module registered for the
 * server's protocol (gp_storage.h), which is given the server's options and
 * the calling user's credentials with each file.  Its location is then a
 * path within what the server reaches, and its label names the server.
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
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "common/extmarkfile.h"
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
#include "gp_storage.h"

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

	/* A storage server's file: its handler, and what reaching it takes. */
	const GpStorageHandler *handler;
	GpStorageFile file;
} DirTableFileAction;

static List *dirtable_actions = NIL;
static bool dirtable_xact_callback_set = false;

/* ------------------------------------------------------------------------- */
/* WAL                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * gp_dirtable's ID, among the custom ones (128-255): not one PostgreSQL's
 * wiki lists as taken (CustomWALResourceManagers), and not the 199
 * Cloudberry's PAX uses.
 */
#define GP_DIRTABLE_RMGR_ID		198

/*
 * What a directory table's directory is called after its relation's OID,
 * in its database's directory, and what O23's mark names (see
 * GpDirTableMarkFiles()).
 */
#define GP_DIRTABLE_SUFFIX		"_dirtable"

#define XLOG_GP_DIRTABLE_MKDIR	0x00	/* a table's directory, made */
#define XLOG_GP_DIRTABLE_WRITE	0x10	/* bytes of a file, at an offset */
#define XLOG_GP_DIRTABLE_UNLINK 0x20	/* a file, removed */
#define XLOG_GP_DIRTABLE_RMTREE 0x30	/* a directory and all in it, removed */

/*
 * A record: the path, relative to the data directory and with its NUL,
 * follows this, and a WRITE's bytes follow the path.
 */
typedef struct xl_gp_dirtable
{
	uint64		offset;			/* WRITE: where in the file its bytes go */
	uint32		pathlen;		/* strlen(path) + 1 */
} xl_gp_dirtable;

#define SizeOfGpDirtable	(offsetof(xl_gp_dirtable, pathlen) + sizeof(uint32))

/* The most of a file one WRITE record carries. */
#define GP_DIRTABLE_WAL_CHUNK	(1024 * 1024)

static void dirtable_ensure_dir(const char *path);

/*
 * Log what was done to a file or directory here, for a replica to do too --
 * only where there may be one, as PostgreSQL logs a relation's contents
 * where wal_level is above minimal.
 */
static void
dirtable_wal(uint8 info, const char *path, uint64 offset,
			 const char *data, uint32 len)
{
	xl_gp_dirtable rec;

	rec.offset = offset;
	rec.pathlen = strlen(path) + 1;
	XLogBeginInsert();
	XLogRegisterData(&rec, SizeOfGpDirtable);
	XLogRegisterData(path, rec.pathlen);
	if (len > 0)
		XLogRegisterData(data, len);
	(void) XLogInsert(GP_DIRTABLE_RMGR_ID, info);
}

static void
dirtable_wal_write(const char *path, const char *data, int len)
{
	int			offset = 0;

	if (!XLogIsNeeded())
		return;
	do
	{
		uint32		n = Min(len - offset, GP_DIRTABLE_WAL_CHUNK);

		dirtable_wal(XLOG_GP_DIRTABLE_WRITE, path, offset, data + offset, n);
		offset += n;
	} while (offset < len);
}

/*
 * A removal, after the transaction's commit or abort record, where nothing
 * may be raised: a record that could not be written leaves a file on the
 * replicas, as a crash between the record and the removal would, and says
 * so.
 */
static void
dirtable_wal_remove(const char *path, bool is_dir)
{
	MemoryContext cxt = CurrentMemoryContext;

	if (!XLogIsNeeded() || RecoveryInProgress())
		return;
	PG_TRY();
	{
		dirtable_wal(is_dir ? XLOG_GP_DIRTABLE_RMTREE : XLOG_GP_DIRTABLE_UNLINK,
					 path, 0, NULL, 0);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not log the removal of \"%s\": %s", path,
						edata->message),
				 errdetail("A mirror or a standby keeps it.")));
		FreeErrorData(edata);
	}
	PG_END_TRY();
}

/* The replay, on a mirror, a standby, or this server after a crash. */
static void
dirtable_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);
	xl_gp_dirtable rec;
	const char *path;

	memcpy(&rec, data, SizeOfGpDirtable);
	path = data + SizeOfGpDirtable;

	switch (info)
	{
		case XLOG_GP_DIRTABLE_MKDIR:
			{
				char	   *dir = pstrdup(path);	/* pg_mkdir_p() writes on it */

				if (pg_mkdir_p(dir, pg_dir_create_mode) != 0 && errno != EEXIST)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not create directory \"%s\": %m", path)));
				pfree(dir);
				break;
			}

		case XLOG_GP_DIRTABLE_WRITE:
			{
				const char *bytes = path + rec.pathlen;
				uint32		len = XLogRecGetDataLen(record) - SizeOfGpDirtable -
					rec.pathlen;
				int			fd;

				dirtable_ensure_dir(path);
				fd = BasicOpenFile(path, O_WRONLY | O_CREAT | PG_BINARY |
								   (rec.offset == 0 ? O_TRUNC : 0));
				if (fd < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open file \"%s\": %m", path)));
				if (len > 0 &&
					pg_pwrite(fd, bytes, len, (off_t) rec.offset) != (ssize_t) len)
				{
					int			save_errno = errno;

					close(fd);
					errno = save_errno ? save_errno : ENOSPC;
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write file \"%s\": %m", path)));
				}
				if (pg_fsync(fd) != 0)
				{
					int			save_errno = errno;

					close(fd);
					errno = save_errno;
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not fsync file \"%s\": %m", path)));
				}
				close(fd);
				break;
			}

		case XLOG_GP_DIRTABLE_UNLINK:
			if (unlink(path) != 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			break;

		case XLOG_GP_DIRTABLE_RMTREE:
			(void) rmtree(path, true);
			break;

		default:
			elog(PANIC, "gp_dirtable_redo: unknown op code %u", info);
	}
}

static void
dirtable_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);
	xl_gp_dirtable rec;

	memcpy(&rec, data, SizeOfGpDirtable);
	appendStringInfoString(buf, data + SizeOfGpDirtable);
	if (info == XLOG_GP_DIRTABLE_WRITE)
		appendStringInfo(buf, "; offset " UINT64_FORMAT ", %u bytes", rec.offset,
						 (uint32) (XLogRecGetDataLen(record) - SizeOfGpDirtable -
								   rec.pathlen));
}

static const char *
dirtable_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_GP_DIRTABLE_MKDIR:
			return "MKDIR";
		case XLOG_GP_DIRTABLE_WRITE:
			return "WRITE";
		case XLOG_GP_DIRTABLE_UNLINK:
			return "UNLINK";
		case XLOG_GP_DIRTABLE_RMTREE:
			return "RMTREE";
	}
	return NULL;
}

static const RmgrData dirtable_rmgr = {
	.rm_name = "gp_dirtable",
	.rm_redo = dirtable_redo,
	.rm_desc = dirtable_desc,
	.rm_identify = dirtable_identify,
};

/* From gp_sql's _PG_init, which only preload runs. */
void
GpDirTableRegisterRmgr(void)
{
	RegisterCustomRmgr(GP_DIRTABLE_RMGR_ID, &dirtable_rmgr);
}

/*
 * A directory table's files are no relation's pages, but they live in a
 * database directory, in <relid>_dirtable (dirtable_compute_location()), and
 * pg_checksums would read each of them as one: stop at the first whose name
 * is no segment number, or, with --enable, write a checksum into every 8K of
 * one that is whole blocks.  And pg_upgrade would leave them behind.  O23's
 * mark names the directory for both, from the postmaster of every node, so a
 * mirror's data directory and a standby's say so too.
 */
void
GpDirTableMarkFiles(void)
{
	ExtensionMarkAdd(GP_DIRTABLE_SUFFIX);
}

/*
 * A replica has what its primary logs of these files, and nothing may be
 * written on it: what one wrote would not be on the primary it follows.
 */
static void
dirtable_refuse_in_recovery(const char *what)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot %s during recovery", what)));
}

static void
dirtable_remember(const char *path, bool on_commit, bool is_dir)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	DirTableFileAction *act = palloc0(sizeof(DirTableFileAction));

	act->path = pstrdup(path);
	act->on_commit = on_commit;
	act->is_dir = is_dir;
	dirtable_actions = lappend(dirtable_actions, act);

	MemoryContextSwitchTo(old);
}

/*
 * The same for a storage server's file, with what reaching it takes: read
 * now, since the transaction's end can read nothing.
 */
static void
dirtable_remember_remote(const GpStorageHandler *handler,
						 const GpStorageFile *file, bool on_commit, bool is_dir)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	DirTableFileAction *act = palloc0(sizeof(DirTableFileAction));

	act->path = pstrdup(file->path);
	act->on_commit = on_commit;
	act->is_dir = is_dir;
	act->handler = handler;
	act->file.server = pstrdup(file->server);
	act->file.server_options = copyObject(file->server_options);
	act->file.user_options = copyObject(file->user_options);
	act->file.path = act->path;
	dirtable_actions = lappend(dirtable_actions, act);

	MemoryContextSwitchTo(old);
}

/* A storage server's file removed as a transaction ends: a warning, never an error. */
static void
dirtable_remove_remote(DirTableFileAction *act)
{
	MemoryContext cxt = CurrentMemoryContext;

	PG_TRY();
	{
		if (act->is_dir)
			act->handler->remove_directory(&act->file);
		else
			(void) act->handler->remove_file(&act->file);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not remove \"%s\" from storage server \"%s\": %s",
						act->path, act->file.server, edata->message),
				 errdetail("It is left behind and has to be removed by hand.")));
		FreeErrorData(edata);
	}
	PG_END_TRY();
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

		if (act->handler != NULL)
			dirtable_remove_remote(act);
		else
		{
			if (act->is_dir)
				dirtable_rmtree(act->path);
			else if (unlink(act->path) != 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", act->path)));
			dirtable_wal_remove(act->path, act->is_dir);
		}
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

	return psprintf("%s/%u%s", dbpath, relid, GP_DIRTABLE_SUFFIX);
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

/*
 * The handler that reaches a directory table's files, and the file at this
 * path with what reaching it takes; NULL for a table whose files are local.
 * The table's label names the server it was made on, whatever its
 * tablespace names since.
 */
static const GpStorageHandler *
dirtable_handler(Oid relid, const char *path, GpStorageFile *file)
{
	ObjectAddress addr;
	char	   *server;

	ObjectAddressSet(addr, RelationRelationId, relid);
	server = GpLabelGet(&addr, GP_LABEL_storage_server);
	if (server == NULL)
		return NULL;
	return GpStorageFileOf(server, path, file);
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
	PG_RETURN_TEXT_P(cstring_to_text(GpDirTableClaim(PG_GETARG_OID(0))));
}

/*
 * Make a table a directory table: its directory, and the "gp" label that says
 * where it is.  What CREATE DIRECTORY TABLE does once the table it becomes
 * exists (gp_sql.c), and what gp_sql.claim_directory_table() does for one
 * made by hand.
 */
char *
GpDirTableClaim(Oid relid)
{
	ObjectAddress addr;
	char	   *location;
	struct stat st;

	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("make a directory table");

	if (GpDirTableLocation(relid) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is already a directory table", get_rel_name(relid))));

	{
		Oid			reltablespace = get_rel_tablespace(relid);
		char	   *server = GpStorageTablespaceServer(OidIsValid(reltablespace)
													   ? reltablespace
													   : MyDatabaseTableSpace);
		List	   *options;

		/*
		 * The tablespace says its files go through a storage server: through
		 * the handler for the server's protocol, under a path of this
		 * database's and this table's there, which the handler makes as it
		 * writes a file.  When no module serves the protocol, writing local
		 * files where the user asked for remote ones would be worse than
		 * refusing.
		 */
		if (server != NULL)
		{
			if (GpStorageServerHandler(server, &options) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot create a directory table in a tablespace that reaches storage server \"%s\"",
								server),
						 GpStorageOption(options, "protocol") != NULL
						 ? errdetail("No module has registered a handler for its protocol, \"%s\", so its files could only be written locally.",
									 GpStorageOption(options, "protocol"))
						 : errdetail("It has no \"protocol\" option to say which handler reaches it, so its files could only be written locally."),
						 errhint("Use a tablespace without %s.server, or load a module that provides the handler.",
								 GP_OPTION_NS)));
			location = psprintf("%u/%u%s", MyDatabaseId, relid,
								GP_DIRTABLE_SUFFIX);
			ObjectAddressSet(addr, RelationRelationId, relid);
			GpLabelSet(&addr, GP_LABEL_directory_location, location);
			GpLabelSet(&addr, GP_LABEL_storage_server, server);
			return location;
		}
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
	if (XLogIsNeeded())
		dirtable_wal(XLOG_GP_DIRTABLE_MKDIR, location, 0, NULL, 0);

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_directory_location, location);

	return location;
}

/*
 * A table labelled a directory table by SECURITY LABEL rather than made one
 * -- as a restore of pg_dump's output labels it, with the location the table
 * had where it was dumped, a directory of another database's and of another
 * relation's OID -- is given a directory of its own here, as CREATE
 * DIRECTORY TABLE gives one, when the label names any other.  The files are
 * not carried: they are the old directory's, which neither pg_dump nor
 * Cloudberry's tools copy, so the rows restored beside them name files the
 * new directory does not have until they are copied into it or put again.
 */
void
GpDirTableRestored(Oid relid)
{
	char	   *location = GpDirTableLocation(relid);
	Oid			reltablespace;
	char	   *server;
	char	   *own;
	ObjectAddress addr;

	if (location == NULL)
		return;
	reltablespace = get_rel_tablespace(relid);
	server = GpStorageTablespaceServer(OidIsValid(reltablespace)
									   ? reltablespace : MyDatabaseTableSpace);
	own = server != NULL
		? psprintf("%u/%u%s", MyDatabaseId, relid, GP_DIRTABLE_SUFFIX)
		: dirtable_compute_location(relid);
	if (strcmp(location, own) == 0)
		return;

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_directory_location, NULL);
	GpLabelSet(&addr, GP_LABEL_storage_server, NULL);
	(void) GpDirTableClaim(relid);
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
	const GpStorageHandler *handler;
	GpStorageFile file;

	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("write a file of a directory table");
	path = dirtable_file_path(relid, relative_path, NULL);

	/* A storage server's: written by its handler, which refuses an existing file */
	if ((handler = dirtable_handler(relid, path, &file)) != NULL)
	{
		handler->write_file(&file, data, len);
		dirtable_remember_remote(handler, &file, false, false);
	}
	else
	{
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
	dirtable_wal_write(path, data, len);
	}

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
	const GpStorageHandler *handler;
	GpStorageFile file;

	if ((handler = dirtable_handler(relid, path, &file)) != NULL)
	{
		result = handler->read_file(&file);
		if (result == NULL)
			PG_RETURN_NULL();
		PG_RETURN_BYTEA_P(result);
	}

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
	const GpStorageHandler *handler;
	GpStorageFile file;

	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("remove a file of a directory table");
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
	if ((handler = dirtable_handler(relid, path, &file)) != NULL)
		dirtable_remember_remote(handler, &file, true, false);
	else if (removed > 0 || access(path, F_OK) == 0)
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
	const GpStorageHandler *handler;
	GpStorageFile file;

	if (location == NULL)
		return;
	if ((handler = dirtable_handler(relid, location, &file)) != NULL)
		dirtable_remember_remote(handler, &file, true, true);
	else
		dirtable_remember(location, true, true);
}
