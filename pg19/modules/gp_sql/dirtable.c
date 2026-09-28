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
 * treat anything that is not a relation file as a file to copy whole.  O23's
 * mark tells pg_checksums the directory is no relation's.
 *
 * A file follows its row.  A put writes the row first -- by the executor's
 * own insertion, so that the table's unique index turns away a second put of
 * the path, and firing only the AFTER ROW triggers, as Cloudberry's COPY does
 * -- then takes a lock on the path, and only then writes the file.  A
 * removal deletes the row, takes the path's lock, and moves the file aside,
 * under a name of its path's, the table locked against every reader as
 * Cloudberry's removal locks it.  A file whose row a dirty snapshot does not
 * see -- no row, an aborted insert, a committed delete -- is garbage: a put
 * of its path may write over it, a get answers NULL for it, and the sweep
 * removes it; a file moved aside whose row is back is read from there, and
 * the sweep moves it back.  What a transaction leaves for its end -- a
 * written file to remove if it rolls back, a file moved aside to remove if
 * it commits and move back if it rolls back -- is kept in a list that the
 * transaction callback drains, in the reverse order for a rollback, that a
 * subtransaction's end follows, and that PREPARE keeps under the
 * transaction's gid for the second phase, which this backend then sweeps by
 * the rule above; the path's lock, held until the list has been acted on,
 * keeps a put from writing a file another transaction's end is about to
 * remove.  The list is how soon a file goes; whether it goes is its row's.  A
 * second phase another backend runs, a restart, or a crash leaves the
 * garbage to gp_sql.directory_table_sweep(), which gp_task may schedule.
 *
 * On a cluster each file and its row are on the segment the file's relative
 * path hashes to, the table being distributed by it, as Cloudberry keeps them
 * (its pg_directory_table.c).  The coordinator sends a put, a get and a
 * removal there, the file as a binary parameter; a segment acts on a put or a
 * removal only for the coordinator's own connection and the statement it
 * sends.  directory_table() runs on every segment, as Cloudberry's does.  A
 * table whose tablespace reaches a storage server keeps its files through the
 * server's handler (gp_storage.h), on the coordinator, and only its rows on
 * the segments.
 *
 * WAL.  Cloudberry logs nothing of a directory table's files, so a mirror
 * has none of them.  The port logs them (M4, as the plan decided for Track
 * D, section 6, question 4), through a WAL resource manager of this
 * module's own, gp_dirtable: a directory as it is made, a file's bytes as it
 * is written, a record to each megabyte, a file as it is moved aside or back,
 * and a file or a directory as it is removed.  A mirror or a standby replays
 * them, and has every local file its primary has.  A server that replays
 * them must preload gp_sql, as a server that reads the rest of what the port
 * writes must: without it, recovery stops at the first such record, with
 * PostgreSQL's FATAL.  A file a storage server keeps is not on the node, and
 * nothing is logged of it.  A database's directory tables are in its
 * directory, so the replay of DROP DATABASE takes their files with it.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/commands/dirtablecmds.c, catalog/pg_directory_table.c,
 *	  catalog/storage_directory_table.c, storage/file/ufile.c (the local file
 *	  handler), and the directory table's part of executor/execMain.c and
 *	  commands/copyfrom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/hashfn.h"
#include "common/relpath.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_hash.h"
#include "gp_label.h"
#include "gp_policy.h"
#include "gp_sql.h"
#include "gp_storage.h"

/* Cloudberry's GetDirectoryTableSchema, by position. */
#define GP_DIRTABLE_NATTS		5
#define GP_DIRTABLE_PATH_ATTNUM	1
#define GP_DIRTABLE_TAG_ATTNUM	5	/* DIRECTORY_TABLE_TAG_COLUMN_ATTNUM: the
									 * only column DML may touch */

/* Cloudberry's allow_dml_directory_table, under the port's naming. */
bool		gp_allow_dml_directory_table = false;

/*
 * The statements the coordinator sends a segment: a put, the row alone of a
 * storage server's file, and a removal, whose table and path are written
 * into it, which a segment checks a call against before it acts; and a get,
 * which a segment answers for any path that hashes to it.
 */
#define DIRTABLE_ROUTED_PUT \
	"SELECT gp_sql.directory_table_put($1::pg_catalog.regclass, $2::pg_catalog.text, $3::pg_catalog.bytea, $4::pg_catalog.text)"
#define DIRTABLE_ROUTED_ROW \
	"SELECT gp_sql.directory_table_row($1::pg_catalog.regclass, $2::pg_catalog.text, $3::pg_catalog.int8, $4::pg_catalog.text, $5::pg_catalog.text)"
#define DIRTABLE_ROUTED_REMOVE \
	"SELECT gp_sql.remove_file(%u::pg_catalog.oid::pg_catalog.regclass, %s)"
#define DIRTABLE_ROUTED_GET \
	"SELECT gp_sql.directory_table_get(%u::pg_catalog.oid::pg_catalog.regclass, %s)"

/* ------------------------------------------------------------------------- */
/* Files to act on at the end of the transaction                             */
/* ------------------------------------------------------------------------- */

/*
 * What an action does as its transaction ends.  A file or directory made is
 * removed if the transaction rolls back; a dropped table's directory, and a
 * storage server's file whose row went, if it commits.  A local file whose
 * row went is moved aside at once, under a name of its own path's
 * (DIRTABLE_MOVED_SUFFIX), so that a put of the path in the same
 * transaction writes a new file and does not write over the one a rollback
 * has to give back: removed if the transaction commits, moved back if it
 * rolls back.  The table's AccessExclusiveLock, which a removal takes as
 * Cloudberry's does, keeps any other transaction from reading it meanwhile.
 */
typedef enum DirTableActionKind
{
	DIRTABLE_MADE,
	DIRTABLE_GONE,
	DIRTABLE_MOVED,
} DirTableActionKind;

typedef struct DirTableFileAction
{
	DirTableActionKind kind;
	char	   *path;			/* relative to the data directory */
	char	   *orig;			/* DIRTABLE_MOVED: where it goes back to */
	bool		is_dir;			/* a whole directory: a table's */
	SubTransactionId subid;		/* the subtransaction that queued it */
	Oid			relid;			/* the directory table it is of */
	char	   *relative_path;	/* a file's path in it (the path it goes back
								 * to, for a moved one); NULL for a directory */

	/* A storage server's file: its handler, and what reaching it takes. */
	const GpStorageHandler *handler;
	GpStorageFile file;
} DirTableFileAction;

/* What a removed file's name is given as it is moved aside. */
#define DIRTABLE_MOVED_SUFFIX	".gp_removed"

static List *dirtable_actions = NIL;
static int	dirtable_moved_count = 0;
static bool dirtable_callbacks_set = false;

/*
 * A prepared transaction's actions, kept for its second phase in this
 * backend, by its gid; and the gid a PREPARE TRANSACTION is about to use.
 */
typedef struct DirTablePrepared
{
	char	   *gid;
	Oid			dbid;
	List	   *actions;		/* of DirTableFileAction * */
} DirTablePrepared;

static List *dirtable_prepared = NIL;	/* in TopMemoryContext */
static char *dirtable_preparing_gid = NULL; /* in TopMemoryContext */

/* ------------------------------------------------------------------------- */
/* WAL                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * gp_dirtable's ID, among the custom ones (128-255): not one PostgreSQL's
 * wiki lists as taken (CustomWALResourceManagers), and not the 199
 * Cloudberry's PAX uses.  It is the path locks' too (dirtable_path_tag()).
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
#define XLOG_GP_DIRTABLE_RENAME 0x40	/* a file, renamed: its new path follows */

/*
 * A record: the path, relative to the data directory and with its NUL,
 * follows this, and a WRITE's bytes, or a RENAME's new path with its NUL,
 * follow the path.
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
 * A removal, which may be after the transaction's commit or abort record,
 * where nothing may be raised: a record that could not be written leaves a
 * file on the replicas, as a crash between the record and the removal would,
 * and says so.
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

/*
 * A file moved aside or back, logged where the move may be after the
 * transaction's end, as a removal is.
 */
static void
dirtable_wal_rename(const char *from, const char *to)
{
	MemoryContext cxt = CurrentMemoryContext;

	if (!XLogIsNeeded() || RecoveryInProgress())
		return;
	PG_TRY();
	{
		dirtable_wal(XLOG_GP_DIRTABLE_RENAME, from, 0, to, strlen(to) + 1);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not log the move of \"%s\" to \"%s\": %s", from, to,
						edata->message),
				 errdetail("A mirror or a standby keeps it where it was.")));
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

		case XLOG_GP_DIRTABLE_RENAME:
			{
				const char *to = path + rec.pathlen;

				dirtable_ensure_dir(to);
				if (rename(path, to) != 0 && errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not rename file \"%s\" to \"%s\": %m",
									path, to)));
				break;
			}

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
	else if (info == XLOG_GP_DIRTABLE_RENAME)
		appendStringInfo(buf, " to %s", data + SizeOfGpDirtable + rec.pathlen);
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
		case XLOG_GP_DIRTABLE_RENAME:
			return "RENAME";
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
 * one that is whole blocks.  O23's mark, gp_core's to write
 * (gp_extmark.c), names the directory for it and for a database's copy
 * (gp_dbcopy.c), from the postmaster of every node, so a mirror's data
 * directory and a standby's say so too.  pg_upgrade would leave them behind,
 * which matters only between two versions of the port, none before a
 * PostgreSQL 20 one.
 */
void
GpDirTableMarkFiles(void)
{
	GpCoreApiLookup()->extension_mark_add(GP_DIRTABLE_SUFFIX);
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

/* ------------------------------------------------------------------------- */
/* Where this backend is                                                     */
/* ------------------------------------------------------------------------- */

/* A cluster's coordinator, which sends a file's work to its segment. */
static bool
dirtable_on_coordinator(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && !core->is_single_node() &&
		core->get_role() == GP_ROLE_DISPATCH;
}

/* A node of a cluster that is not its coordinator's dispatching session. */
static bool
dirtable_on_segment(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && !core->is_single_node() &&
		core->get_role() != GP_ROLE_DISPATCH;
}

/*
 * Is this call the statement the coordinator sent this segment, on its own
 * connection -- rather than the same function in a user's statement a
 * segment runs, or in a session of the segment's own -- as gp_matview's
 * check_caller() asks?
 */
static bool
dirtable_routed(const char *statement)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && core->get_role() == GP_ROLE_EXECUTE &&
		GpClusterDispatchTrusted() && debug_query_string != NULL &&
		strcmp(debug_query_string, statement) == 0;
}

/* Cloudberry's SQLSTATE for a command that cannot run where it was asked. */
#define ERRCODE_GP_COMMAND_ERROR	MAKE_SQLSTATE('4','2','M','0','0')

/* A write anywhere but through the coordinator, in Cloudberry's words. */
static void
dirtable_refuse_here(const char *function)
{
	ereport(ERROR,
			(errcode(ERRCODE_GP_COMMAND_ERROR),
			 errmsg("%s() could only be called on QD", function),
			 errdetail("A directory table's files are written and removed on the segment its relative path hashes to, sent there by the coordinator.")));
}

/*
 * The segment a file and its row are on: the one its relative path hashes
 * to, the table being distributed by it, as Cloudberry's is.
 */
static int
dirtable_segment_of(Oid relid, const char *relative_path)
{
	GpPolicy   *policy = GpPolicyGet(relid);
	Oid			type = TEXTOID;
	Datum		value = CStringGetTextDatum(relative_path);
	bool		isnull = false;
	int			content;

	if (!GpPolicyIsHashPartitioned(policy) || policy->nattrs != 1 ||
		policy->attrs[0] != GP_DIRTABLE_PATH_ATTNUM)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("directory table \"%s\" is not distributed by relative_path",
						get_rel_name(relid)),
				 errdetail("Each file of a directory table is on the segment its relative path hashes to.")));
	content = GpHashSegmentForKey(policy, &type, &value, &isnull);
	if (content < 0)
		elog(ERROR, "gp_sql: no segment for a file of directory table \"%s\"",
			 get_rel_name(relid));
	return content;
}

/* ------------------------------------------------------------------------- */
/* The actions a transaction's end takes                                     */
/* ------------------------------------------------------------------------- */

static DirTableFileAction *
dirtable_remember(DirTableActionKind kind, const char *path, bool is_dir,
				  Oid relid, const char *relative_path)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	DirTableFileAction *act = palloc0(sizeof(DirTableFileAction));

	act->kind = kind;
	act->path = pstrdup(path);
	act->is_dir = is_dir;
	act->subid = GetCurrentSubTransactionId();
	act->relid = relid;
	act->relative_path = relative_path ? pstrdup(relative_path) : NULL;
	dirtable_actions = lappend(dirtable_actions, act);

	MemoryContextSwitchTo(old);
	return act;
}

/*
 * The same for a storage server's file, with what reaching it takes: read
 * now, since the transaction's end can read nothing.
 */
static void
dirtable_remember_remote(DirTableActionKind kind,
						 const GpStorageHandler *handler,
						 const GpStorageFile *file, bool is_dir,
						 Oid relid, const char *relative_path)
{
	DirTableFileAction *act = dirtable_remember(kind, file->path, is_dir,
												relid, relative_path);
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	act->handler = handler;
	act->file.server = pstrdup(file->server);
	act->file.server_options = copyObject(file->server_options);
	act->file.user_options = copyObject(file->user_options);
	act->file.path = act->path;

	MemoryContextSwitchTo(old);
}

/*
 * Does this transaction remove this storage server's file as it commits?  A
 * put of its path would write over the bytes a rollback has to leave, which
 * a handler cannot move aside, so it is refused (GpDirTablePut()).
 */
static bool
dirtable_remote_gone(const char *path)
{
	foreach_ptr(DirTableFileAction, act, dirtable_actions)
	{
		if (act->kind == DIRTABLE_GONE && act->handler != NULL &&
			strcmp(act->path, path) == 0)
			return true;
	}
	return false;
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

/*
 * Remove a directory and everything in it, complaining rather than failing;
 * one that is not there -- a segment's, which its first file makes, or a
 * cluster's coordinator's, which keeps none -- is nothing to remove.
 */
static void
dirtable_rmtree(const char *dir)
{
	struct stat st;

	if (stat(dir, &st) != 0 && errno == ENOENT)
		return;
	if (!rmtree(dir, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\" of a dropped directory table",
						dir),
				 errdetail("Its files are left behind and have to be removed by hand.")));
}

/* An action's file or directory removed, and the removal logged. */
static void
dirtable_remove(DirTableFileAction *act)
{
	if (act->handler != NULL)
	{
		dirtable_remove_remote(act);
		return;
	}
	if (act->is_dir)
		dirtable_rmtree(act->path);
	else if (unlink(act->path) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", act->path)));
	dirtable_wal_remove(act->path, act->is_dir);
}

/* A file moved aside moved back, over whatever is at its path now. */
static void
dirtable_move_back(DirTableFileAction *act)
{
	if (rename(act->path, act->orig) != 0)
	{
		if (errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not move file \"%s\" back to \"%s\": %m",
							act->path, act->orig),
					 errdetail("The file of a removal that rolled back is left under the name it was moved aside to.")));
		return;
	}
	dirtable_wal_rename(act->path, act->orig);
}

/* What an action does as its transaction commits, and as it rolls back. */
static void
dirtable_act_commit(DirTableFileAction *act)
{
	if (act->kind != DIRTABLE_MADE)
		dirtable_remove(act);
}

static void
dirtable_act_abort(DirTableFileAction *act)
{
	if (act->kind == DIRTABLE_MADE)
		dirtable_remove(act);
	else if (act->kind == DIRTABLE_MOVED)
		dirtable_move_back(act);
}

static void dirtable_sweep_action(DirTableFileAction *act);

/*
 * PREPARE: a prepared transaction outlives this backend's transaction, so
 * its actions are kept, under its gid, for this backend's second phase of
 * it, which sweeps each of them (GpDirTableSecondPhase()).
 */
static void
dirtable_keep_prepared(void)
{
	MemoryContext old;
	DirTablePrepared *prep;

	if (dirtable_actions == NIL || dirtable_preparing_gid == NULL)
		return;

	old = MemoryContextSwitchTo(TopMemoryContext);
	prep = palloc0(sizeof(DirTablePrepared));
	prep->gid = pstrdup(dirtable_preparing_gid);
	prep->dbid = MyDatabaseId;
	foreach_ptr(DirTableFileAction, act, dirtable_actions)
	{
		DirTableFileAction *copy = palloc0(sizeof(DirTableFileAction));

		*copy = *act;
		copy->path = pstrdup(act->path);
		copy->orig = act->orig ? pstrdup(act->orig) : NULL;
		copy->relative_path = act->relative_path ? pstrdup(act->relative_path) : NULL;
		if (act->handler != NULL)
		{
			copy->file.server = pstrdup(act->file.server);
			copy->file.server_options = copyObject(act->file.server_options);
			copy->file.user_options = copyObject(act->file.user_options);
			copy->file.path = copy->path;
		}
		prep->actions = lappend(prep->actions, copy);
	}
	dirtable_prepared = lappend(dirtable_prepared, prep);
	MemoryContextSwitchTo(old);
}

/*
 * The transaction is over: the files it created or removed follow it --
 * what a rollback undoes, undone in the reverse of the order it was done.
 *
 * Nothing here may throw.  By the time a commit reaches this point the
 * transaction is already durable, and a file that is left behind is a far
 * smaller problem than a PANIC.  The paths' locks are still held: they are
 * let go of after the callbacks.
 */
static void
dirtable_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PREPARE)
		dirtable_keep_prepared();
	else if (event == XACT_EVENT_COMMIT)
	{
		foreach_ptr(DirTableFileAction, act, dirtable_actions)
			dirtable_act_commit(act);
	}
	else if (event == XACT_EVENT_ABORT)
	{
		for (int i = list_length(dirtable_actions) - 1; i >= 0; i--)
			dirtable_act_abort((DirTableFileAction *) list_nth(dirtable_actions, i));
	}
	else
		return;

	dirtable_actions = NIL;
	if (dirtable_preparing_gid != NULL)
	{
		pfree(dirtable_preparing_gid);
		dirtable_preparing_gid = NULL;
	}
}

/*
 * A subtransaction is over.  One that commits hands what it queued to its
 * parent; one that rolls back undoes at once what it did -- its rows are
 * gone already, or back, and its paths still locked -- and forgets it.
 */
static void
dirtable_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		foreach_ptr(DirTableFileAction, act, dirtable_actions)
		{
			if (act->subid == mySubid)
				act->subid = parentSubid;
		}
	}
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		for (int i = list_length(dirtable_actions) - 1; i >= 0; i--)
		{
			DirTableFileAction *act = list_nth(dirtable_actions, i);

			if (act->subid == mySubid)
			{
				dirtable_act_abort(act);
				dirtable_actions = list_delete_nth_cell(dirtable_actions, i);
			}
		}
	}
}

void
GpDirTableRegisterXactCallback(void)
{
	if (!dirtable_callbacks_set)
	{
		RegisterXactCallback(dirtable_xact_callback, NULL);
		RegisterSubXactCallback(dirtable_subxact_callback, NULL);
		dirtable_callbacks_set = true;
	}
}

/* PREPARE TRANSACTION, before it runs: the gid its actions are kept under. */
void
GpDirTableNotePrepare(const char *gid)
{
	if (dirtable_preparing_gid != NULL)
		pfree(dirtable_preparing_gid);
	dirtable_preparing_gid = MemoryContextStrdup(TopMemoryContext, gid);
}

/*
 * COMMIT PREPARED or ROLLBACK PREPARED of a transaction this backend
 * prepared, once it has run: each file or directory the transaction left for
 * its end is swept -- dealt with as what its row says of it now, whichever
 * way the transaction went, and left alone if another transaction holds its
 * table or its path, for the sweep (dirtable_sweep_action()).
 */
void
GpDirTableSecondPhase(const char *gid)
{
	foreach_ptr(DirTablePrepared, prep, dirtable_prepared)
	{
		if (strcmp(prep->gid, gid) != 0 || prep->dbid != MyDatabaseId)
			continue;
		dirtable_prepared = foreach_delete_current(dirtable_prepared, prep);
		foreach_ptr(DirTableFileAction, act, prep->actions)
		{
			dirtable_sweep_action(act);
			pfree(act->path);
			if (act->orig != NULL)
				pfree(act->orig);
			if (act->relative_path != NULL)
				pfree(act->relative_path);
		}
		list_free_deep(prep->actions);
		pfree(prep->gid);
		pfree(prep);
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
 *
 * A table whose files are this node's has them where
 * dirtable_compute_location() says, in its database's directory of its
 * tablespace, whatever path the label holds: that is the path in the
 * database the table was made in, and a copy of that database (CREATE
 * DATABASE ... TEMPLATE) has the template's label, and a database moved
 * (ALTER DATABASE ... SET TABLESPACE) the one it had.  The files go with the
 * database (gp_core's gp_dbcopy.c), and the table cannot go to another
 * tablespace without them (GpDirTableCheckStatement()).  A storage server's
 * files are where the label says.
 */
char *
GpDirTableLocation(Oid relid)
{
	ObjectAddress addr;
	char	   *location;

	if (get_rel_relkind(relid) != RELKIND_RELATION)
		return NULL;

	ObjectAddressSet(addr, RelationRelationId, relid);
	location = GpLabelGet(&addr, GP_LABEL_directory_location);
	if (location == NULL || GpLabelGet(&addr, GP_LABEL_storage_server) != NULL)
		return location;
	return dirtable_compute_location(relid);
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

/* Does this directory table keep its files on a storage server? */
static bool
dirtable_on_server(Oid relid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	return GpLabelHas(&addr, GP_LABEL_storage_server);
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
/* A file's row, and its path's lock                                         */
/* ------------------------------------------------------------------------- */

/*
 * The lock on a path of a directory table, which a put takes after its row
 * and before its file, and a removal before it queues the file's: held to
 * the transaction's end, after its callbacks, and to the second phase of a
 * prepared one.  An advisory lock's tag, with the resource manager's ID in
 * the field advisory locks keep 1 or 2 in, so that none of a user's is it.
 */
static void
dirtable_path_tag(LOCKTAG *tag, Oid relid, const char *relative_path)
{
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, relid,
						 hash_bytes((const unsigned char *) relative_path,
									strlen(relative_path)),
						 GP_DIRTABLE_RMGR_ID);
}

static bool
dirtable_lock_path(Oid relid, const char *relative_path, bool nowait)
{
	LOCKTAG		tag;

	dirtable_path_tag(&tag, relid, relative_path);
	return LockAcquire(&tag, ExclusiveLock, false, nowait) != LOCKACQUIRE_NOT_AVAIL;
}

static void
dirtable_unlock_path(Oid relid, const char *relative_path)
{
	LOCKTAG		tag;

	dirtable_path_tag(&tag, relid, relative_path);
	LockRelease(&tag, ExclusiveLock, false);
}

/*
 * The row of a path, by the table's primary key on relative_path, as this
 * snapshot sees it; its TID into *tid, and the md5 it keeps of its file into
 * *md5, where they are asked for.
 */
static bool
dirtable_find_row(Relation rel, const char *relative_path, Snapshot snapshot,
				  ItemPointer tid, char **md5)
{
	Oid			indexoid = RelationGetPrimaryKeyIndex(rel, false);
	Relation	index;
	ScanKeyData key;
	IndexScanDesc scan;
	TupleTableSlot *slot;
	bool		found;

	if (!OidIsValid(indexoid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("directory table \"%s\" has no primary key",
						RelationGetRelationName(rel))));
	index = index_open(indexoid, AccessShareLock);
	ScanKeyEntryInitialize(&key, 0, 1, BTEqualStrategyNumber, InvalidOid,
						   index->rd_indcollation[0], F_TEXTEQ,
						   CStringGetTextDatum(relative_path));
	slot = table_slot_create(rel, NULL);
	scan = index_beginscan(rel, index, snapshot, NULL, 1, 0, SO_NONE);
	index_rescan(scan, &key, 1, NULL, 0);
	found = index_getnext_slot(scan, ForwardScanDirection, slot);
	if (found && tid != NULL)
		*tid = slot->tts_tid;
	if (found && md5 != NULL)
	{
		bool		isnull;
		Datum		value = slot_getattr(slot, 4, &isnull);

		*md5 = isnull ? NULL : TextDatumGetCString(value);
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(index, AccessShareLock);
	return found;
}

/*
 * A file's row, put in by the executor's own insertion, as Cloudberry's COPY
 * of a file puts it in: the table's indexes, whose unique one turns away a
 * second row of the path -- waiting for a transaction that has one in
 * progress -- and the AFTER ROW triggers, and not the BEFORE ROW ones, which
 * could change the row the file is written for.
 */
static void
dirtable_insert_row(Relation rel, const char *relative_path, int64 size,
					const char *md5, const char *tag)
{
	EState	   *estate = CreateExecutorState();
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	List	   *perminfos = NIL;
	ResultRelInfo *rri = makeNode(ResultRelInfo);
	TupleTableSlot *slot;
	List	   *recheck;
	int			natts = RelationGetDescr(rel)->natts;

	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = RowExclusiveLock;
	addRTEPermissionInfo(&perminfos, rte);
	ExecInitRangeTable(estate, list_make1(rte), perminfos, bms_make_singleton(1));
	InitResultRelInfo(rri, rel, 1, NULL, 0);
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, rri);
	estate->es_output_cid = GetCurrentCommandId(true);
	ExecOpenIndices(rri, false);
	AfterTriggerBeginQuery();

	slot = table_slot_create(rel, &estate->es_tupleTable);
	ExecClearTuple(slot);
	for (int i = 0; i < natts; i++)
		slot->tts_isnull[i] = true;
	slot->tts_values[0] = CStringGetTextDatum(relative_path);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = Int64GetDatum(size);
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = TimestampTzGetDatum(GetCurrentTransactionStartTimestamp());
	slot->tts_isnull[2] = false;
	slot->tts_values[3] = CStringGetTextDatum(md5);
	slot->tts_isnull[3] = false;
	if (tag != NULL)
	{
		slot->tts_values[4] = CStringGetTextDatum(tag);
		slot->tts_isnull[4] = false;
	}
	ExecStoreVirtualTuple(slot);

	if (rel->rd_att->constr != NULL)
		ExecConstraints(rri, slot, estate);
	table_tuple_insert(rel, slot, estate->es_output_cid, 0, NULL);
	recheck = ExecInsertIndexTuples(rri, estate, 0, slot, NIL, NULL);
	ExecARInsertTriggers(estate, rri, slot, recheck, NULL);
	list_free(recheck);

	AfterTriggerEndQuery(estate);
	ExecCloseIndices(rri);
	ExecCloseTrigTargetRelations(estate);
	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);
	CommandCounterIncrement();
}

/* ------------------------------------------------------------------------- */
/* Garbage                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Is this a directory table whose files are this node's, at this location?
 * A cluster's coordinator keeps none: its tables' files are on the
 * segments, and a directory of one it has is left from before they were.
 */
static bool
dirtable_keeps_here(Oid relid, const char *dir)
{
	char	   *location;

	if (dirtable_on_coordinator() && !dirtable_on_server(relid))
		return false;
	location = GpDirTableLocation(relid);
	return location != NULL && strcmp(location, dir) == 0;
}

static bytea *dirtable_read_local(const char *path);
static char *dirtable_md5(bytea *content);

/*
 * Does a row of this path -- one a dirty snapshot sees, in progress or not --
 * describe the local file at file_path, by the md5 it keeps of it?
 */
static bool
dirtable_row_describes(Relation rel, const char *relative_path,
					   const char *file_path)
{
	SnapshotData dirty;
	char	   *md5 = NULL;
	bytea	   *content;

	InitDirtySnapshot(dirty);
	if (!dirtable_find_row(rel, relative_path, &dirty, NULL, &md5) || md5 == NULL)
		return false;
	content = dirtable_read_local(file_path);
	return content != NULL && strcmp(dirtable_md5(content), md5) == 0;
}

/* A local file removed, and the removal logged. */
static void
dirtable_unlink(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
	dirtable_wal_remove(path, false);
}

/*
 * A file a removal moved aside, of relative_path: moved back to orig if a row
 * of the path describes it and does not describe what is at orig -- the
 * removal rolled back -- and else removed.  True when it was removed.
 */
static bool
dirtable_sweep_moved(Relation rel, const char *relative_path,
					 const char *moved, const char *orig)
{
	if (dirtable_row_describes(rel, relative_path, moved) &&
		!dirtable_row_describes(rel, relative_path, orig))
	{
		if (rename(moved, orig) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not move file \"%s\" back to \"%s\": %m",
							moved, orig)));
		else
			dirtable_wal_rename(moved, orig);
		return false;
	}
	dirtable_unlink(moved);
	return true;
}

/*
 * The path a file moved aside by a removal was moved from -- its name less
 * ".<xid>.<n>.gp_removed" -- or NULL for any other name.
 */
static char *
dirtable_moved_from(const char *relative_path)
{
	int			len = strlen(relative_path);
	int			slen = strlen(DIRTABLE_MOVED_SUFFIX);
	char	   *name;
	char	   *dot;

	if (len <= slen || strcmp(relative_path + len - slen, DIRTABLE_MOVED_SUFFIX) != 0)
		return NULL;
	name = pnstrdup(relative_path, len - slen);
	for (int part = 0; part < 2; part++)
	{
		dot = strrchr(name, '.');
		if (dot == NULL || dot == name || dot[1] == '\0' ||
			strspn(dot + 1, "0123456789") != strlen(dot + 1))
			return NULL;
		*dot = '\0';
	}
	return name;
}

/*
 * A file of a live directory table, as the sweep finds it: removed if no row
 * of its path is seen by a dirty snapshot, and, if it is a file a removal
 * moved aside, moved back or removed as its row says; left alone when its
 * row describes it, or when another transaction holds its path.  The caller
 * holds a lock on the table.  True when it was removed.
 */
static bool
dirtable_sweep_file(Relation rel, const char *top, const char *relative_path)
{
	Oid			relid = RelationGetRelid(rel);
	char	   *path = psprintf("%s/%s", top, relative_path);
	char	   *orig_relative = dirtable_moved_from(relative_path);
	const char *locked = relative_path;
	SnapshotData dirty;
	bool		removed = false;

	InitDirtySnapshot(dirty);
	if (orig_relative != NULL &&
		!dirtable_find_row(rel, relative_path, &dirty, NULL, NULL))
		locked = orig_relative;
	else
		orig_relative = NULL;

	if (!dirtable_lock_path(relid, locked, true))
		return false;
	if (orig_relative != NULL)
		removed = dirtable_sweep_moved(rel, orig_relative, path,
									   psprintf("%s/%s", top, orig_relative));
	else if (!dirtable_find_row(rel, relative_path, &dirty, NULL, NULL))
	{
		dirtable_unlink(path);
		removed = true;
	}
	dirtable_unlock_path(relid, locked);
	return removed;
}

/*
 * One of the actions a prepared transaction left, at its second phase in
 * this backend: a directory removed if its table is gone; a file made kept
 * if a row of its path describes it, and else removed; a storage server's
 * file removed if no row of its path is seen; a file moved aside moved back
 * or removed as its row says.  Whatever lock it would wait for is passed
 * over: another transaction is at work there, and the sweep will see to it.
 */
static void
dirtable_sweep_action(DirTableFileAction *act)
{
	Relation	rel;
	SnapshotData dirty;

	if (!ConditionalLockRelationOid(act->relid, AccessShareLock))
		return;
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(act->relid)) ||
		GpDirTableLocation(act->relid) == NULL)
	{
		if (act->is_dir)
			dirtable_remove(act);
		UnlockRelationOid(act->relid, AccessShareLock);
		return;
	}
	if (act->is_dir || !dirtable_lock_path(act->relid, act->relative_path, true))
	{
		UnlockRelationOid(act->relid, AccessShareLock);
		return;
	}

	rel = relation_open(act->relid, NoLock);
	InitDirtySnapshot(dirty);
	if (act->kind == DIRTABLE_MOVED)
		(void) dirtable_sweep_moved(rel, act->relative_path, act->path, act->orig);
	else if (act->handler != NULL)
	{
		if (!dirtable_find_row(rel, act->relative_path, &dirty, NULL, NULL))
			dirtable_remove(act);
	}
	else if (act->kind == DIRTABLE_MADE &&
			 !dirtable_row_describes(rel, act->relative_path, act->path))
		dirtable_remove(act);
	relation_close(rel, NoLock);

	dirtable_unlock_path(act->relid, act->relative_path);
	UnlockRelationOid(act->relid, AccessShareLock);
}

/* Every file under a directory, by its path relative to the top one. */
static int64
dirtable_sweep_tree(Relation rel, const char *top, const char *sub)
{
	char	   *dirpath = sub ? psprintf("%s/%s", top, sub) : pstrdup(top);
	DIR		   *dir = AllocateDir(dirpath);
	struct dirent *de;
	int64		removed = 0;

	if (dir == NULL)
		return 0;
	while ((de = ReadDir(dir, dirpath)) != NULL)
	{
		char	   *rel_path;
		char	   *path;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		rel_path = sub ? psprintf("%s/%s", sub, de->d_name) : pstrdup(de->d_name);
		path = psprintf("%s/%s", top, rel_path);
		if (lstat(path, &st) == 0)
		{
			if (S_ISDIR(st.st_mode))
				removed += dirtable_sweep_tree(rel, top, rel_path);
			else if (S_ISREG(st.st_mode) &&
					 dirtable_sweep_file(rel, top, rel_path))
				removed++;
		}
		pfree(rel_path);
		pfree(path);
	}
	FreeDir(dir);
	pfree(dirpath);
	return removed;
}

/*
 * A <relid>_dirtable directory: removed whole if it is no directory table's
 * here -- the table dropped, or this a cluster's coordinator -- and else each
 * file of it that is garbage.  How many were removed, a directory counting
 * one.
 */
static int64
dirtable_sweep_directory(Oid relid, const char *dir)
{
	int64		removed = 0;

	if (!ConditionalLockRelationOid(relid, AccessShareLock))
		return 0;
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)) ||
		!dirtable_keeps_here(relid, dir))
	{
		dirtable_rmtree(dir);
		dirtable_wal_remove(dir, true);
		removed = 1;
	}
	else
	{
		Relation	rel = relation_open(relid, NoLock);

		removed = dirtable_sweep_tree(rel, dir, NULL);
		relation_close(rel, NoLock);
	}
	UnlockRelationOid(relid, AccessShareLock);
	return removed;
}

/*
 * The garbage of this database on this node: in each tablespace's directory
 * of it, every directory table's directory.  It never waits: a table or a
 * path another transaction holds is passed over.
 */
static int64
dirtable_sweep_database(void)
{
	Relation	spcrel;
	TableScanDesc scan;
	HeapTuple	tup;
	int64		removed = 0;

	spcrel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(spcrel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spcoid = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;
		char	   *dbpath;
		DIR		   *dir;
		struct dirent *de;

		if (spcoid == GLOBALTABLESPACE_OID)
			continue;
		dbpath = GetDatabasePath(MyDatabaseId, spcoid);
		dir = AllocateDir(dbpath);
		if (dir == NULL)
			continue;
		while ((de = ReadDir(dir, dbpath)) != NULL)
		{
			char	   *end;
			unsigned long relid;

			relid = strtoul(de->d_name, &end, 10);
			if (end == de->d_name || strcmp(end, GP_DIRTABLE_SUFFIX) != 0 ||
				relid == 0 || relid > PG_UINT32_MAX)
				continue;
			removed += dirtable_sweep_directory((Oid) relid,
												psprintf("%s/%s", dbpath, de->d_name));
		}
		FreeDir(dir);
	}
	table_endscan(scan);
	table_close(spcrel, AccessShareLock);
	return removed;
}

/* ------------------------------------------------------------------------- */
/* The DML guard                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry refuses INSERT and DELETE on a directory table, and allows
 * UPDATE only of the tag column, in ExecMain.  PG19 has no such check to
 * change, so the same question is asked of the finished plan: of its result
 * relations and the columns its UPDATE sets, as the range table says them,
 * whatever carries the rows out on a cluster.
 *
 * Only a statement that writes pays for this: reads never reach it.
 */
void
GpDirTableCheckDML(QueryDesc *queryDesc)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	int			rti = -1;

	if (gp_allow_dml_directory_table)
		return;

	if (stmt == NULL || stmt->commandType == CMD_SELECT ||
		bms_is_empty(stmt->resultRelationRelids))
		return;

	while ((rti = bms_next_member(stmt->resultRelationRelids, rti)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rti, stmt->rtable);
		RTEPermissionInfo *perminfo;
		int			col = -1;

		if (rte->rtekind != RTE_RELATION ||
			GpDirTableLocation(rte->relid) == NULL)
			continue;

		if (stmt->commandType != CMD_UPDATE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change directory table \"%s\"",
							get_rel_name(rte->relid))));

		/* An UPDATE may set the tag and nothing else. */
		if (rte->perminfoindex == 0)
			continue;
		perminfo = getRTEPermissionInfo(stmt->permInfos, rte);
		while ((col = bms_next_member(perminfo->updatedCols, col)) >= 0)
		{
			if (col + FirstLowInvalidHeapAttributeNumber != GP_DIRTABLE_TAG_ATTNUM)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("Only allow to update directory \"tag\" column.")));
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
/* What a directory table's definition may not do                          */
/* ------------------------------------------------------------------------- */

/* "ALTER action X cannot be performed", in Cloudberry's words. */
static void
dirtable_refuse_action(const char *action, Oid relid)
{
	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("ALTER action %s cannot be performed on relation \"%s\"",
					action, get_rel_name(relid)),
			 errdetail("This operation is not supported for directory tables.")));
}

/*
 * What an ALTER TABLE subcommand does to a directory table's definition that
 * its files and its functions cannot follow -- the five columns they know by
 * position, the primary key they find a row by -- as the name Cloudberry's
 * message gives it; NULL for one that leaves them as they are, as the ones
 * Cloudberry allows on a directory table do (ATSimplePermissions() with
 * ATT_DIRECTORY_TABLE, tablecmds.c).
 */
static const char *
dirtable_alter_refused(AlterTableCmd *cmd)
{
	switch (cmd->subtype)
	{
		case AT_AddColumn:
			return "ADD COLUMN";
		case AT_DropColumn:
			return "DROP COLUMN";
		case AT_AlterColumnType:
			return "ALTER COLUMN TYPE";
		case AT_ColumnDefault:
			return "ALTER COLUMN SET DEFAULT";
		case AT_DropNotNull:
			return "ALTER COLUMN DROP NOT NULL";
		case AT_SetNotNull:
			return "ALTER COLUMN SET NOT NULL";
		case AT_AddIdentity:
		case AT_SetIdentity:
		case AT_DropIdentity:
			return "ALTER COLUMN IDENTITY";
		case AT_AddConstraint:
			return "ADD CONSTRAINT";
		case AT_DropConstraint:
			return "DROP CONSTRAINT";
		case AT_AlterConstraint:
			return "ALTER CONSTRAINT";
		case AT_AddInherit:
			return "INHERIT";
		case AT_AddOf:
			return "OF";
		case AT_SetLogged:
			return "SET LOGGED";
		case AT_SetUnLogged:
			return "SET UNLOGGED";
		case AT_SetAccessMethod:
			return "SET ACCESS METHOD";
		case AT_AttachPartition:
			return "ATTACH PARTITION";
		default:
			return NULL;
	}
}

static bool
dirtable_has_pkey(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		has = OidIsValid(RelationGetPrimaryKeyIndex(rel, false));

	relation_close(rel, AccessShareLock);
	return has;
}

/*
 * DROP TABLESPACE of one a directory table of this database is in: refused
 * as Cloudberry's dependency of the table on its tablespace refuses it,
 * naming each, in the order they were made.
 */
static void
dirtable_check_drop_tablespace(DropTableSpaceStmt *stmt)
{
	Oid			spc = get_tablespace_oid(stmt->tablespacename, true);
	Relation	rel;
	ScanKeyData key;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;
	StringInfoData detail;

	if (!OidIsValid(spc))
		return;
	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_class_reltablespace, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(spc == MyDatabaseTableSpace ? InvalidOid : spc));
	scan = table_beginscan_catalog(rel, 1, &key);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if (form->relkind == RELKIND_RELATION &&
			GpDirTableLocation(form->oid) != NULL)
			relids = lappend_oid(relids, form->oid);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	if (relids == NIL)
		return;

	list_sort(relids, list_oid_cmp);
	initStringInfo(&detail);
	foreach_oid(relid, relids)
		appendStringInfo(&detail, "%stablespace for directory table %s",
						 detail.len > 0 ? "\n" : "", get_rel_name(relid));
	ereport(ERROR,
			(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
			 errmsg("tablespace \"%s\" cannot be dropped because some objects depend on it",
					stmt->tablespacename),
			 errdetail_internal("%s", detail.data)));
}

/*
 * What gp_sql's ProcessUtility hook refuses of a directory table, before the
 * statement runs:
 *
 * - ALTER TABLE ... SET TABLESPACE of one, or ALTER TABLE ALL IN TABLESPACE
 *	 of the one it is in: refused, as Cloudberry refuses the first and passes
 *	 a directory table over in the second.  Its files are in its database's
 *	 directory of its tablespace, and would stay behind.
 * - a subcommand that changes the columns or the constraints its files and
 *	 functions rely on, and a rename of it or of a column: refused, as
 *	 Cloudberry refuses them.
 * - DROP INDEX of its primary key, which finds a file's row: refused, in
 *	 Cloudberry's words; and DROP TABLESPACE of the one it is in.
 */
void
GpDirTableCheckStatement(Node *parsetree)
{
	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
		Oid			relid;

		if (stmt->objtype != OBJECT_TABLE || stmt->relation == NULL)
			return;
		relid = RangeVarGetRelid(stmt->relation, NoLock, true);
		if (!OidIsValid(relid) || GpDirTableLocation(relid) == NULL)
			return;

		foreach_node(AlterTableCmd, cmd, stmt->cmds)
		{
			const char *action;

			/*
			 * Its primary key, which a restore of pg_dump's output adds once
			 * the rows are in, where it has none.
			 */
			if (cmd->subtype == AT_AddConstraint && IsA(cmd->def, Constraint) &&
				((Constraint *) cmd->def)->contype == CONSTR_PRIMARY &&
				!dirtable_has_pkey(relid))
				continue;
			if (cmd->subtype == AT_SetTableSpace)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("ALTER action SET TABLESPACE cannot be performed on relation \"%s\"",
								get_rel_name(relid)),
						 errdetail("This operation is not supported for directory tables.")));
			if ((action = dirtable_alter_refused(cmd)) != NULL)
				dirtable_refuse_action(action, relid);
		}
	}
	else if (IsA(parsetree, AlterTableMoveAllStmt))
	{
		AlterTableMoveAllStmt *stmt = (AlterTableMoveAllStmt *) parsetree;
		Oid			spc;
		List	   *roles = NIL;
		ListCell   *lc;
		Relation	rel;
		ScanKeyData key;
		TableScanDesc scan;
		HeapTuple	tup;

		if (stmt->objtype != OBJECT_TABLE)
			return;
		spc = get_tablespace_oid(stmt->orig_tablespacename, true);
		if (!OidIsValid(spc))
			return;

		/* the tables AlterTableMoveAll() would move */
		if (spc == MyDatabaseTableSpace)
			spc = InvalidOid;
		foreach(lc, stmt->roles)
			roles = lappend_oid(roles,
								get_rolespec_oid(lfirst(lc), false));

		rel = table_open(RelationRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_class_reltablespace, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(spc));
		scan = table_beginscan_catalog(rel, 1, &key);
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

			if (form->relkind != RELKIND_RELATION ||
				IsCatalogNamespace(form->relnamespace) ||
				IsToastNamespace(form->relnamespace) ||
				(roles != NIL && !list_member_oid(roles, form->relowner)) ||
				GpDirTableLocation(form->oid) == NULL)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot move directory table \"%s\" to another tablespace",
							NameStr(form->relname)),
					 errdetail("A directory table's files stay in the tablespace it was made in."),
					 errhint("Move the other tables one by one.")));
		}
		table_endscan(scan);
		table_close(rel, AccessShareLock);
	}
	else if (IsA(parsetree, RenameStmt))
	{
		RenameStmt *stmt = (RenameStmt *) parsetree;
		Oid			relid;

		if ((stmt->renameType != OBJECT_TABLE && stmt->renameType != OBJECT_COLUMN) ||
			stmt->relation == NULL)
			return;
		relid = RangeVarGetRelid(stmt->relation, NoLock, true);
		if (OidIsValid(relid) && GpDirTableLocation(relid) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("Rename directory table is not allowed.")));
	}
	else if (IsA(parsetree, DropTableSpaceStmt))
		dirtable_check_drop_tablespace((DropTableSpaceStmt *) parsetree);
	else if (IsA(parsetree, DropStmt) &&
			 ((DropStmt *) parsetree)->removeType == OBJECT_INDEX)
	{
		foreach_node(List, name, ((DropStmt *) parsetree)->objects)
		{
			Oid			indexoid = RangeVarGetRelid(makeRangeVarFromNameList(name),
													NoLock, true);
			Oid			relid;
			Relation	rel;
			bool		pkey;

			if (!OidIsValid(indexoid) || get_rel_relkind(indexoid) != RELKIND_INDEX)
				continue;
			relid = IndexGetRelation(indexoid, true);
			if (!OidIsValid(relid) || GpDirTableLocation(relid) == NULL)
				continue;
			rel = relation_open(relid, AccessShareLock);
			pkey = RelationGetPrimaryKeyIndex(rel, false) == indexoid;
			relation_close(rel, AccessShareLock);
			if (pkey)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Disallowed to drop primary index \"%s\" on directory table \"%s\"",
								get_rel_name(indexoid), get_rel_name(relid))));
		}
	}
}

/*
 * CREATE TABLE ... INHERITS of a directory table, which Cloudberry refuses
 * as it refuses any relation that is not a table (MergeAttributes()); the
 * port's is a table, so the words are said here.
 */
void
GpDirTableCheckInherits(CreateStmt *stmt)
{
	foreach_node(RangeVar, parent, stmt->inhRelations)
	{
		Oid			relid = RangeVarGetRelid(parent, NoLock, true);

		if (OidIsValid(relid) && GpDirTableLocation(relid) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table or foreign table",
							parent->relname)));
	}
}

/*
 * ALTER TABLE ... SET DISTRIBUTED, WITH (REORGANIZE = ...), EXPAND TABLE or
 * SHRINK TABLE of a directory table: refused, as SET TABLESPACE is.  Its
 * files are on the segments their paths hash to and do not move with the
 * rows.
 */
void
GpDirTableCheckDistribution(Oid relid, const char *action)
{
	if (GpDirTableLocation(relid) != NULL)
		dirtable_refuse_action(action, relid);
}

/* ------------------------------------------------------------------------- */
/* Making one                                                                */
/* ------------------------------------------------------------------------- */

static void
dirtable_require_owner(Oid relid)
{
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));
}

/*
 * A table a directory table can be made of: Cloudberry's five columns, in
 * its order, which a file's row is written in by position.  Its primary key
 * on relative_path, which finds a row, may come after, as a restore of
 * pg_dump's output adds it once the rows are in.
 */
static void
dirtable_check_shape(Oid relid)
{
	static const struct
	{
		const char *name;
		Oid			type;
	}			cols[GP_DIRTABLE_NATTS] = {
		{"relative_path", TEXTOID},
		{"size", INT8OID},
		{"last_modified", TIMESTAMPTZOID},
		{"md5", TEXTOID},
		{"tag", TEXTOID},
	};
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	bool		shaped = desc->natts == GP_DIRTABLE_NATTS;

	for (int i = 0; shaped && i < GP_DIRTABLE_NATTS; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		shaped = !att->attisdropped && att->atttypid == cols[i].type &&
			strcmp(NameStr(att->attname), cols[i].name) == 0;
	}
	table_close(rel, AccessShareLock);

	if (!shaped)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" does not have the columns of a directory table",
						get_rel_name(relid)),
				 errdetail("A directory table has the columns relative_path text PRIMARY KEY, size bigint, last_modified timestamptz, md5 text and tag text, in that order.")));
}

PG_FUNCTION_INFO_V1(gp_sql_dirtable_claim);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_location);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_put);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_row);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_get);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_remove);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_scan);
PG_FUNCTION_INFO_V1(gp_sql_dirtable_sweep);

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
 *
 * On a cluster the label is the segments' too, and a segment makes the
 * directory as its first file needs it; the coordinator, which keeps no
 * file, makes none, and asks that the table be distributed by its relative
 * path, as Cloudberry distributes one.
 */
char *
GpDirTableClaim(Oid relid)
{
	ObjectAddress addr;
	char	   *location;
	struct stat st;

	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("make a directory table");
	if (dirtable_on_segment())
		dirtable_refuse_here("claim_directory_table");

	if (GpDirTableLocation(relid) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is already a directory table", get_rel_name(relid))));
	dirtable_check_shape(relid);
	if (dirtable_on_coordinator())
		(void) dirtable_segment_of(relid, "");

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
	ObjectAddressSet(addr, RelationRelationId, relid);

	if (dirtable_on_coordinator())
	{
		GpLabelSet(&addr, GP_LABEL_directory_location, location);
		return location;
	}

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
	dirtable_remember(DIRTABLE_MADE, location, true, relid, NULL);
	if (XLogIsNeeded())
		dirtable_wal(XLOG_GP_DIRTABLE_MKDIR, location, 0, NULL, 0);

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
	char	   *location;
	Oid			reltablespace;
	char	   *server;
	char	   *own;
	ObjectAddress addr;

	/*
	 * The label as it was written, not GpDirTableLocation(), which gives a
	 * table whose files are this node's the directory it has here.
	 */
	if (get_rel_relkind(relid) != RELKIND_RELATION)
		return;
	ObjectAddressSet(addr, RelationRelationId, relid);
	location = GpLabelGet(&addr, GP_LABEL_directory_location);
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

/* ------------------------------------------------------------------------- */
/* A file put                                                                */
/* ------------------------------------------------------------------------- */

/* A file written whole into its directory here, over a garbage one. */
static void
dirtable_write_local(const char *path, const char *data, int len)
{
	int			fd;

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
}

/*
 * A storage server's file written by its handler, over a garbage one of the
 * path -- but not over one this transaction removes as it commits, whose
 * bytes a rollback has to leave, and which the handler cannot move aside.
 */
static void
dirtable_write_remote(const GpStorageHandler *handler, GpStorageFile *file,
					  const char *data, int64 size, Oid relid,
					  const char *relative_path)
{
	if (dirtable_remote_gone(file->path))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot write file \"%s\" of directory table \"%s\" again in the transaction that removed it",
						relative_path, get_rel_name(relid)),
				 errdetail("Its storage server keeps it until the transaction commits.")));
	(void) handler->remove_file(file);
	handler->write_file(file, data, size);
	dirtable_remember_remote(DIRTABLE_MADE, handler, file, false, relid,
							 relative_path);
}

/*
 * A file's row, and with write_file its file, here: the row first, then the
 * path's lock, then the file, written over any garbage one of the path --
 * the row being in, nobody else's can be.  A storage server's file is written
 * by its handler.
 */
static void
dirtable_put_here(Oid relid, const char *relative_path, const char *data,
				  int64 size, const char *md5, const char *tag, bool write_file)
{
	Relation	rel = table_open(relid, RowExclusiveLock);
	char	   *path;
	const GpStorageHandler *handler;
	GpStorageFile file;

	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("write a file of a directory table");
	path = dirtable_file_path(relid, relative_path, NULL);

	dirtable_insert_row(rel, relative_path, size, md5, tag);

	if (write_file)
	{
		dirtable_lock_path(relid, relative_path, false);
		if ((handler = dirtable_handler(relid, path, &file)) != NULL)
			dirtable_write_remote(handler, &file, data, size, relid,
								  relative_path);
		else
		{
			dirtable_write_local(path, data, (int) size);

			/* If this transaction rolls back, the file it wrote goes with it. */
			dirtable_remember(DIRTABLE_MADE, path, false, relid, relative_path);
			dirtable_wal_write(path, data, (int) size);
		}
	}

	table_close(rel, NoLock);
}

/* The md5 Cloudberry's row keeps of a file's content. */
static char *
dirtable_md5(bytea *content)
{
	return TextDatumGetCString(DirectFunctionCall1(md5_bytea,
												   PointerGetDatum(content)));
}

/*
 * Write a file and its row: here, or, on a cluster's coordinator, on the
 * segment its path hashes to -- the file sent as a binary parameter -- or,
 * for a storage server's, the row there and the file through the handler
 * here, after the row.
 */
void
GpDirTablePut(Oid relid, const char *relative_path, bytea *content,
			  const char *tag)
{
	int64		size = VARSIZE_ANY_EXHDR(content);
	Relation	rel;
	char	   *path;
	int			segment;
	const GpStorageHandler *handler;
	GpStorageFile file;

	if (!dirtable_on_coordinator())
	{
		if (dirtable_on_segment())
			dirtable_refuse_here("directory_table_put");
		dirtable_put_here(relid, relative_path, VARDATA_ANY(content), size,
						  dirtable_md5(content), tag, true);
		return;
	}

	/* the lock COPY takes on the table, as Cloudberry's COPY of a file does */
	rel = table_open(relid, RowExclusiveLock);
	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("write a file of a directory table");
	path = dirtable_file_path(relid, relative_path, NULL);
	segment = dirtable_segment_of(relid, relative_path);

	if ((handler = dirtable_handler(relid, path, &file)) == NULL)
	{
		const char *values[4];
		int			lengths[4] = {0, 0, (int) size, 0};
		int			formats[4] = {0, 0, 1, 0};

		values[0] = psprintf("%u", relid);
		values[1] = relative_path;
		values[2] = VARDATA_ANY(content);
		values[3] = tag;
		GpDispatchParamsOnContent(segment, DIRTABLE_ROUTED_PUT, 4, values,
								  lengths, formats);
	}
	else
	{
		const char *values[5];

		values[0] = psprintf("%u", relid);
		values[1] = relative_path;
		values[2] = psprintf(INT64_FORMAT, size);
		values[3] = dirtable_md5(content);
		values[4] = tag;
		GpDispatchParamsOnContent(segment, DIRTABLE_ROUTED_ROW, 5, values,
								  NULL, NULL);

		dirtable_lock_path(relid, relative_path, false);
		dirtable_write_remote(handler, &file, VARDATA_ANY(content), size,
							  relid, relative_path);
	}

	table_close(rel, NoLock);
}

/*
 * gp_sql.directory_table_put(regclass, path, content, tag) -> bigint
 *
 * Write a file and the row that describes it.  Cloudberry writes both with
 * COPY into a directory table, which O26's grammar turns into a COPY this
 * module carries out by calling what this does (dircopy.c).
 *
 * Not STRICT, the tag being NULL where the COPY gives none: the table, the
 * path and the content are refused NULL here, before they are read.
 */
Datum
gp_sql_dirtable_put(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *relative_path;
	bytea	   *content;
	char	   *tag;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("directory table must not be null")));
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("file path must not be null")));
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("file content must not be null"),
				 errhint("An empty file's content is ''.")));
	relid = PG_GETARG_OID(0);
	relative_path = text_to_cstring(PG_GETARG_TEXT_PP(1));
	content = PG_GETARG_BYTEA_PP(2);
	tag = PG_ARGISNULL(3) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(3));

	/* on a segment, the put the coordinator sent: its file and row here */
	if (dirtable_on_segment() && dirtable_routed(DIRTABLE_ROUTED_PUT))
		dirtable_put_here(relid, relative_path, VARDATA_ANY(content),
						  VARSIZE_ANY_EXHDR(content), dirtable_md5(content),
						  tag, true);
	else
		GpDirTablePut(relid, relative_path, content, tag);

	PG_RETURN_INT64((int64) VARSIZE_ANY_EXHDR(content));
}

/*
 * gp_sql.directory_table_row(regclass, path, size, md5, tag) -> void
 *
 * On a segment, the row of a file a storage server keeps, which the
 * coordinator writes through the server's handler: the call it sends, and
 * nothing else.
 */
Datum
gp_sql_dirtable_row(PG_FUNCTION_ARGS)
{
	if (!dirtable_routed(DIRTABLE_ROUTED_ROW) ||
		PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
		dirtable_refuse_here("directory_table_row");

	dirtable_put_here(PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)),
					  NULL, PG_GETARG_INT64(2),
					  text_to_cstring(PG_GETARG_TEXT_PP(3)),
					  PG_ARGISNULL(4) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(4)),
					  false);
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------- */
/* A file read                                                               */
/* ------------------------------------------------------------------------- */

/* A local file's content, or NULL when it is not there. */
static bytea *
dirtable_read_local(const char *path)
{
	int			fd;
	struct stat st;
	bytea	   *result;
	int			nbytes;

	if (stat(path, &st) != 0)
	{
		if (errno == ENOENT)
			return NULL;
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
	{
		if (errno == ENOENT)
			return NULL;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

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
	return result;
}

/* A file's content, locally or through its storage server's handler. */
static bytea *
dirtable_read(Oid relid, const char *path)
{
	const GpStorageHandler *handler;
	GpStorageFile file;

	if ((handler = dirtable_handler(relid, path, &file)) != NULL)
		return handler->read_file(&file);
	return dirtable_read_local(path);
}

/*
 * The content a row describes, where its file is not at its path but aside:
 * a removal moved it there that another backend's second phase, or one after
 * a restart, rolled back, and the sweep has yet to move it back.  Found among
 * the files moved aside from the path, by the md5 the row keeps.
 */
static bytea *
dirtable_read_moved(const char *path, const char *md5)
{
	char	   *dirpath = pstrdup(path);
	char	   *slash = strrchr(dirpath, '/');
	DIR		   *dir;
	struct dirent *de;
	bytea	   *found = NULL;

	if (md5 == NULL || slash == NULL)
		return NULL;
	*slash = '\0';
	dir = AllocateDir(dirpath);
	if (dir == NULL)
		return NULL;
	while (found == NULL && (de = ReadDir(dir, dirpath)) != NULL)
	{
		char	   *from = dirtable_moved_from(de->d_name);
		bytea	   *content;

		if (from == NULL || strcmp(from, slash + 1) != 0)
			continue;
		content = dirtable_read_local(psprintf("%s/%s", dirpath, de->d_name));
		if (content != NULL && strcmp(dirtable_md5(content), md5) == 0)
			found = content;
	}
	FreeDir(dir);
	return found;
}

/*
 * A file's content as its row, seen, describes it: the file at its path, or,
 * where there is none, the one a removal rolled back left aside.
 */
static bytea *
dirtable_read_described(Oid relid, const char *path, const char *md5)
{
	bytea	   *content = dirtable_read(relid, path);

	if (content == NULL && !dirtable_on_server(relid))
		content = dirtable_read_moved(path, md5);
	return content;
}

/*
 * A file read here: NULL unless the caller's snapshot sees its row, a file
 * without one being garbage.
 */
static bytea *
dirtable_get_here(Oid relid, const char *relative_path)
{
	Relation	rel = table_open(relid, AccessShareLock);
	char	   *path = dirtable_file_path(relid, relative_path, NULL);
	bytea	   *content = NULL;
	char	   *md5 = NULL;

	if (dirtable_find_row(rel, relative_path, GetActiveSnapshot(), NULL, &md5))
		content = dirtable_read_described(relid, path, md5);
	table_close(rel, AccessShareLock);
	return content;
}

/*
 * A file of a directory table, or NULL when it has none of this path: read
 * here, or, on a cluster's coordinator, on the segment its path hashes to --
 * but a storage server's, which the coordinator reads through its handler.
 * On a segment, a path that hashes to another is refused: its file is there.
 */
bytea *
GpDirTableGet(Oid relid, const char *relative_path)
{
	char	   *path;
	int			segment;
	TupleDesc	desc;
	GpGatherState *gather;
	TupleTableSlot *slot;
	bytea	   *content = NULL;

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));
	path = dirtable_file_path(relid, relative_path, NULL);

	if (!dirtable_on_coordinator() && !dirtable_on_segment())
		return dirtable_get_here(relid, relative_path);

	if (dirtable_on_server(relid))
	{
		if (dirtable_on_segment())
			return dirtable_get_here(relid, relative_path);
		return dirtable_read(relid, path);
	}

	segment = dirtable_segment_of(relid, relative_path);
	if (dirtable_on_segment())
	{
		if (segment != GpClusterContentId())
			ereport(ERROR,
					(errcode(ERRCODE_GP_COMMAND_ERROR),
					 errmsg("file \"%s\" of directory table \"%s\" is on segment %d",
							relative_path, get_rel_name(relid), segment),
					 errdetail("A segment reads the files its relative paths hash to; read the others through the coordinator.")));
		return dirtable_get_here(relid, relative_path);
	}

	desc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(desc, (AttrNumber) 1, "content", BYTEAOID, -1, 0);
	TupleDescFinalize(desc);
	gather = GpGatherStartOn(psprintf(DIRTABLE_ROUTED_GET, relid,
									  quote_literal_cstr(relative_path)),
							 desc, segment);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	if (GpGatherNext(gather, slot, NULL))
	{
		bool		isnull;
		Datum		value = slot_getattr(slot, 1, &isnull);

		if (!isnull)
			content = DatumGetByteaPCopy(value);
	}
	GpGatherEnd(gather);
	ExecDropSingleTupleTableSlot(slot);
	return content;
}

/*
 * gp_sql.directory_table_get(regclass, path) -> bytea
 */
Datum
gp_sql_dirtable_get(PG_FUNCTION_ARGS)
{
	bytea	   *content = GpDirTableGet(PG_GETARG_OID(0),
										text_to_cstring(PG_GETARG_TEXT_PP(1)));

	if (content == NULL)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(content);
}

/* ------------------------------------------------------------------------- */
/* A file removed                                                            */
/* ------------------------------------------------------------------------- */

/*
 * A file's row removed here, found by the primary key -- after any
 * transaction still writing it -- and deleted without a trigger, as
 * Cloudberry's remove_file_segment() deletes it; and with remove_file its
 * file queued for the commit, under the path's lock.
 */
static bool
dirtable_remove_here(Oid relid, const char *relative_path, bool remove_file)
{
	Relation	rel;
	char	   *path;
	SnapshotData dirty;
	ItemPointerData tid;
	bool		found;
	const GpStorageHandler *handler;
	GpStorageFile file;

	/* Cloudberry's lock, which the table's readers and writers wait for */
	rel = table_open(relid, AccessExclusiveLock);
	GP_FAULT("remove_file_inject");
	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("remove a file of a directory table");
	path = dirtable_file_path(relid, relative_path, NULL);

	InitDirtySnapshot(dirty);
	for (;;)
	{
		TransactionId xwait;

		found = dirtable_find_row(rel, relative_path, &dirty, &tid, NULL);
		xwait = TransactionIdIsValid(dirty.xmin) ? dirty.xmin : dirty.xmax;
		if (!found || !TransactionIdIsValid(xwait))
			break;
		XactLockTableWait(xwait, rel, &tid, XLTW_Delete);
	}

	if (found)
	{
		simple_table_tuple_delete(rel, &tid, GetActiveSnapshot());
		CommandCounterIncrement();
		if (remove_file)
		{
			dirtable_lock_path(relid, relative_path, false);
			if ((handler = dirtable_handler(relid, path, &file)) != NULL)
				dirtable_remember_remote(DIRTABLE_GONE, handler, &file, false,
										 relid, relative_path);
			else if (access(path, F_OK) == 0)
			{
				char	   *moved = psprintf("%s.%u.%d%s", path,
											 GetTopTransactionId(),
											 ++dirtable_moved_count,
											 DIRTABLE_MOVED_SUFFIX);
				DirTableFileAction *act;

				act = dirtable_remember(DIRTABLE_MOVED, moved, false, relid,
										relative_path);
				act->orig = MemoryContextStrdup(TopTransactionContext, path);
				durable_rename(path, moved, ERROR);
				if (XLogIsNeeded())
					dirtable_wal(XLOG_GP_DIRTABLE_RENAME, path, 0, moved,
								 strlen(moved) + 1);
			}
		}
	}

	table_close(rel, NoLock);
	return found;
}

/*
 * gp_sql.remove_file(regclass, path) -> boolean
 *
 * The row goes now and the file goes when the transaction commits, so a
 * rollback leaves both where they were.  On a cluster's coordinator,
 * Cloudberry's remove_file(): the table locked here, and the removal sent to
 * every segment, as Cloudberry sends remove_file_segment() -- the one the
 * path hashes to has the row -- true if one removed it; a storage server's
 * file is removed through its handler here.
 */
Datum
gp_sql_dirtable_remove(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *relative_path = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *statement = psprintf(DIRTABLE_ROUTED_REMOVE, relid,
									 quote_literal_cstr(relative_path));
	int			nsegs;
	char	  **answers;
	bool		removed = false;
	char	   *path;
	const GpStorageHandler *handler;
	GpStorageFile file;

	if (dirtable_on_segment())
	{
		if (!dirtable_routed(statement))
			dirtable_refuse_here("remove_file");
		PG_RETURN_BOOL(dirtable_remove_here(relid, relative_path,
											!dirtable_on_server(relid)));
	}
	if (!dirtable_on_coordinator())
		PG_RETURN_BOOL(dirtable_remove_here(relid, relative_path, true));

	LockRelationOid(relid, AccessExclusiveLock);
	dirtable_require_owner(relid);
	dirtable_refuse_in_recovery("remove a file of a directory table");
	path = dirtable_file_path(relid, relative_path, NULL);

	nsegs = GpClusterSegmentCount();
	answers = palloc0_array(char *, Max(nsegs, 1));
	GpDispatchQueryFirstValues(statement, -1, answers);
	for (int i = 0; i < nsegs; i++)
		removed |= answers[i] != NULL && strcmp(answers[i], "t") == 0;

	if (removed && (handler = dirtable_handler(relid, path, &file)) != NULL)
	{
		dirtable_lock_path(relid, relative_path, false);
		dirtable_remember_remote(DIRTABLE_GONE, handler, &file, false, relid,
								 relative_path);
	}

	PG_RETURN_BOOL(removed);
}

/*
 * A directory table is being dropped: its files follow it, once the drop
 * commits -- on each node its own, and a storage server's through its
 * handler, on the coordinator.
 */
void
GpDirTableDropped(Oid relid)
{
	char	   *location = GpDirTableLocation(relid);
	const GpStorageHandler *handler;
	GpStorageFile file;

	if (location == NULL)
		return;
	if (!dirtable_on_server(relid))
		dirtable_remember(DIRTABLE_GONE, location, true, relid, NULL);
	else if (!dirtable_on_segment() &&
			 (handler = dirtable_handler(relid, location, &file)) != NULL)
		dirtable_remember_remote(DIRTABLE_GONE, handler, &file, true, relid,
								 NULL);
}

/* ------------------------------------------------------------------------- */
/* Every file                                                                */
/* ------------------------------------------------------------------------- */

/*
 * gp_sql.directory_table(regclass) -> setof (scoped_file_url, relative_path,
 *										  tag, size, last_modified, md5, content)
 *
 * Cloudberry's directory_table(), which runs on every segment: each reads its
 * own rows and the files they describe, and the coordinator gathers them
 * (GpDispatchFunctionToSegments()).  A row whose file is not there -- a
 * restored one, before its files are copied in -- has no content.
 */
Datum
gp_sql_dirtable_scan(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	char	   *location;
	Snapshot	snapshot;
	TableScanDesc scan;
	TupleTableSlot *slot;

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	rel = table_open(relid, AccessShareLock);
	GP_FAULT("directory_table_inject");
	location = GpDirTableLocation(relid);
	if (location == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("'%s' is not a directory table",
						RelationGetRelationName(rel))));
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(rel));

	/*
	 * Cloudberry's snapshot: the latest, taken once the table's lock is
	 * held, so that a removal the scan waited for is seen with its file gone.
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum		values[7];
		bool		nulls[7] = {false, false, false, false, false, false, false};
		char	   *relative_path;
		bytea	   *content;

		slot_getallattrs(slot);
		relative_path = TextDatumGetCString(slot->tts_values[0]);
		content = dirtable_read_described(relid,
										  psprintf("%s/%s", location, relative_path),
										  slot->tts_isnull[3] ? NULL :
										  TextDatumGetCString(slot->tts_values[3]));

		values[0] = CStringGetTextDatum(psprintf("%s/%s", location, relative_path));
		values[1] = slot->tts_values[0];
		values[2] = slot->tts_values[4];
		nulls[2] = slot->tts_isnull[4];
		values[3] = slot->tts_values[1];
		nulls[3] = slot->tts_isnull[1];
		values[4] = slot->tts_values[2];
		nulls[4] = slot->tts_isnull[2];
		values[5] = slot->tts_values[3];
		nulls[5] = slot->tts_isnull[3];
		values[6] = PointerGetDatum(content);
		nulls[6] = content == NULL;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * gp_sql.directory_table_sweep() -> bigint
 *
 * Remove this database's garbage: every file of a directory table whose row
 * nothing sees, and the directory of every directory table that is gone --
 * here and, on a cluster's coordinator, on every segment -- logging each
 * removal; how many were removed.  A table or a path another transaction
 * holds is passed over, for the next sweep.
 */
Datum
gp_sql_dirtable_sweep(PG_FUNCTION_ARGS)
{
	int64		removed;

	dirtable_refuse_in_recovery("sweep the files of directory tables");
	removed = dirtable_sweep_database();

	if (dirtable_on_coordinator())
	{
		int			nsegs = GpClusterSegmentCount();
		char	  **answers = palloc0_array(char *, Max(nsegs, 1));

		GpDispatchQueryFirstValues("SELECT gp_sql.directory_table_sweep()", -1,
								   answers);
		for (int i = 0; i < nsegs; i++)
			if (answers[i] != NULL)
				removed += pg_strtoint64(answers[i]);
	}
	PG_RETURN_INT64(removed);
}
