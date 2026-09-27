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
 * gp_dbcopy.c
 *	  A database copied or moved takes its modules' directories with it.
 *
 * PostgreSQL copies a database's directory without its subdirectories.
 * CREATE DATABASE's FILE_COPY strategy copies the template's directory of
 * each tablespace file by file, and ALTER DATABASE ... SET TABLESPACE its
 * directory in the old default tablespace, which it then removes whole --
 * both with copydir(..., false) (dbcommands.c); the WAL_LOG strategy copies
 * the relations the template's pg_class lists.  Vanilla PostgreSQL keeps no
 * subdirectory there.  The port's modules do: a PAX table's files are in
 * <relfilenode>_pax, a directory table's in <relid>_dirtable, each in its
 * database's directory of the table's tablespace, and both are named by
 * O23's marks.  So a copy of a database had neither -- PAX tables whose aux
 * tables name files that are not there, and directory tables that read the
 * template's files -- and a database moved lost them.  Cloudberry's own
 * dbcommands.c copies the same way ("We don't need to copy subdirectories").
 *
 * This file copies each marked directory as part of the statement, before
 * its transaction commits: CREATE DATABASE's once createdb() returns, from
 * the template's directory of each tablespace to the new database's, which
 * the core maps as its strategies do (the template's default tablespace to
 * the new database's); ALTER DATABASE's as movedb() commits the move, which
 * it does before removing the old directory in a transaction of its own.
 * The copy is fsync'ed, so that a crash after the commit finds it whole,
 * and a record of each directory copied lets a mirror or a standby copy its
 * own, as the core's FILE_COPY record does.  The record is replayed where
 * the server follows a primary or recovers from an archive, and not in
 * crash recovery, which would copy the source again as it is by then, over
 * what the copy has been given since -- PAX's own records are replayed only
 * there too, its files being fsync'ed when they are written.
 *
 * On a cluster each node copies its own: CREATE DATABASE runs on every node,
 * ALTER DATABASE ... SET TABLESPACE on the coordinator alone (gp_ddl.c).
 * gp_core installs this hook before DDL dispatch's, so that it runs inside
 * the statement each node runs, before the coordinator dispatches it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "common/extmarkfile.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/bufmgr.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "tcop/utility.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_dbcopy.h"

/* A directory copied: its name, with its NUL, follows. */
typedef struct xl_gp_dbcopy
{
	Oid			src_db;
	Oid			src_spc;
	Oid			dst_db;
	Oid			dst_spc;
} xl_gp_dbcopy;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*
 * ALTER DATABASE ... SET TABLESPACE, between the statement's start and the
 * commit of its move: the database, and its default tablespace before and
 * after.
 */
static bool move_pending = false;
static Oid	move_db = InvalidOid;
static Oid	move_src_spc = InvalidOid;
static Oid	move_dst_spc = InvalidOid;

/* ------------------------------------------------------------------------- */
/* The copy                                                                  */
/* ------------------------------------------------------------------------- */

static void
dbcopy_log(Oid src_db, Oid src_spc, Oid dst_db, Oid dst_spc, const char *name)
{
	xl_gp_dbcopy rec;

	/* where WAL is minimal there is no replica to tell */
	if (!XLogIsNeeded())
		return;

	rec.src_db = src_db;
	rec.src_spc = src_spc;
	rec.dst_db = dst_db;
	rec.dst_spc = dst_spc;
	XLogBeginInsert();
	XLogRegisterData(&rec, sizeof(rec));
	XLogRegisterData(name, strlen(name) + 1);
	(void) XLogInsert(GP_CORE_RMGR_ID, XLOG_GP_CORE_DBCOPY);
}

/*
 * Copy each marked directory of src_db's directory in src_spc into dst_db's
 * in dst_spc, which exists.
 */
static void
copy_marked(const ExtensionMarks *marks, Oid src_db, Oid src_spc,
			Oid dst_db, Oid dst_spc)
{
	char	   *srcpath = GetDatabasePath(src_db, src_spc);
	char	   *dstpath = GetDatabasePath(dst_db, dst_spc);
	DIR		   *dir;
	struct dirent *de;
	bool		copied = false;

	dir = AllocateDir(srcpath);
	while ((de = ReadDir(dir, srcpath)) != NULL)
	{
		char	   *from;
		char	   *to;
		struct stat st;

		if (!ExtensionMarkedFileLookup(marks, de->d_name))
			continue;

		from = psprintf("%s/%s", srcpath, de->d_name);
		if (lstat(from, &st) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", from)));

		/* a marked file is a file, which the core has copied */
		if (!S_ISDIR(st.st_mode))
			continue;

		to = psprintf("%s/%s", dstpath, de->d_name);
		copydir(from, to, true);
		dbcopy_log(src_db, src_spc, dst_db, dst_spc, de->d_name);
		copied = true;
		pfree(from);
		pfree(to);
	}
	FreeDir(dir);

	/* copydir() made each copy durable; the entries for them, too */
	if (copied && enableFsync)
		fsync_fname(dstpath, true);

	pfree(srcpath);
	pfree(dstpath);
}

/* Does database db have a directory in tablespace spc? */
static bool
has_directory(Oid db, Oid spc)
{
	char	   *path = GetDatabasePath(db, spc);
	struct stat st;
	bool		found = stat(path, &st) == 0 && S_ISDIR(st.st_mode);

	pfree(path);
	return found;
}

/* ------------------------------------------------------------------------- */
/* CREATE DATABASE                                                           */
/* ------------------------------------------------------------------------- */

/* A database's default tablespace. */
static Oid
database_tablespace(Oid db)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db));
	Oid			spc;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", db);
	spc = ((Form_pg_database) GETSTRUCT(tup))->dattablespace;
	ReleaseSysCache(tup);
	return spc;
}

/*
 * The new database's directories, all of them, as createdb()'s failure
 * callback removes them -- its buffers first, which the WAL_LOG strategy
 * read in -- when the copy fails after createdb() has returned.
 */
static void
remove_new_database(Oid db, List *spcs)
{
	ListCell   *lc;

	DropDatabaseBuffers(db);
	ForgetDatabaseSyncRequests(db);
	foreach(lc, spcs)
	{
		char	   *path = GetDatabasePath(db, lfirst_oid(lc));

		(void) rmtree(path, true);
		pfree(path);
	}
}

static void
copy_for_createdb(CreatedbStmt *stmt)
{
	ExtensionMarks *marks = ExtensionMarksLoad(DataDir);
	const char *template = "template1";
	Oid			src_db;
	Oid			dst_db;
	Oid			src_def;
	Oid			dst_def;
	List	   *spcs = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	ListCell   *lc;

	/* no module keeps a directory in a database's */
	if (marks == NULL)
		return;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "template") == 0)
			template = defGetString(opt);
	}

	/* The new database's row, which this transaction wrote. */
	CommandCounterIncrement();
	src_db = get_database_oid(template, false);
	dst_db = get_database_oid(stmt->dbname, false);
	src_def = database_tablespace(src_db);
	dst_def = database_tablespace(dst_db);

	/* Every tablespace but the global one, as the FILE_COPY strategy. */
	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	PG_TRY();
	{
		foreach(lc, spcs)
		{
			Oid			spc = lfirst_oid(lc);
			Oid			dst_spc = spc == src_def ? dst_def : spc;

			if (!has_directory(src_db, spc))
				continue;
			TablespaceCreateDbspace(dst_spc, dst_db, false);
			copy_marked(marks, src_db, spc, dst_db, dst_spc);
		}
	}
	PG_CATCH();
	{
		remove_new_database(dst_db, spcs);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* ALTER DATABASE ... SET TABLESPACE                                         */
/* ------------------------------------------------------------------------- */

/*
 * Before movedb(): which database moves, from where to where.  Nothing when
 * the statement is no move, or one movedb() will refuse or find already
 * done, which it says in its own words.
 *
 * The database is locked first, as movedb() locks it, so that the tablespace
 * read is the one it moves from: a move of it in another session finishes
 * before this one reads.
 */
static void
note_move(AlterDatabaseStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);
		Oid			db;
		Oid			spc;
		HeapTuple	tup;

		if (strcmp(opt->defname, "tablespace") != 0)
			continue;

		db = get_database_oid(stmt->dbname, true);
		spc = get_tablespace_oid(defGetString(opt), true);
		if (!OidIsValid(db) || !OidIsValid(spc))
			return;

		LockSharedObject(DatabaseRelationId, db, 0, AccessExclusiveLock);
		tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db));
		if (!HeapTupleIsValid(tup))
			return;
		move_src_spc = ((Form_pg_database) GETSTRUCT(tup))->dattablespace;
		ReleaseSysCache(tup);

		if (move_src_spc != spc)
		{
			move_db = db;
			move_dst_spc = spc;
			move_pending = true;
		}
		return;
	}
}

/*
 * As the move commits: movedb() has copied the database's files to the new
 * tablespace and changed its row, and after the commit removes the old
 * directory.  A failure takes away what it copied, as movedb()'s own failure
 * callback would, and the database stays where it was.
 */
static void
copy_for_movedb(void)
{
	ExtensionMarks *marks = ExtensionMarksLoad(DataDir);

	if (marks == NULL)
		return;

	PG_TRY();
	{
		copy_marked(marks, move_db, move_src_spc, move_db, move_dst_spc);
	}
	PG_CATCH();
	{
		char	   *dstpath = GetDatabasePath(move_db, move_dst_spc);

		(void) rmtree(dstpath, true);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
dbcopy_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (move_pending)
			{
				move_pending = false;
				copy_for_movedb();
			}
			break;
		case XACT_EVENT_ABORT:
			move_pending = false;
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* The statements                                                            */
/* ------------------------------------------------------------------------- */

static void
dbcopy_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, AlterDatabaseStmt))
		note_move((AlterDatabaseStmt *) parsetree);

	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		/* a move that returned without committing moved nothing */
		move_pending = false;
	}
	PG_END_TRY();

	if (IsA(parsetree, CreatedbStmt))
		copy_for_createdb((CreatedbStmt *) parsetree);
}

/* ------------------------------------------------------------------------- */
/* WAL                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The replay, on a mirror, a standby or a server recovering from an archive:
 * the same copy, of this server's own source.  A source that is gone was
 * removed after the copy was made here, by a drop replayed since: what was
 * copied then stays.  The resource manager's other record, the nodes'
 * states, is gp_cluster.c's.
 */
static void
dbcopy_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xl_gp_dbcopy rec;
	const char *name;
	char	   *srcdb;
	char	   *dstdb;
	char	   *from;
	char	   *to;
	struct stat st;

	if (info == XLOG_GP_CORE_CLUSTER)
	{
		GpClusterRedo(XLogRecGetData(record), XLogRecGetDataLen(record));
		return;
	}
	if (info != XLOG_GP_CORE_DBCOPY)
		elog(PANIC, "gp_core_redo: unknown op code %u", info);

	if (!ArchiveRecoveryRequested)
		return;

	memcpy(&rec, XLogRecGetData(record), sizeof(rec));
	name = XLogRecGetData(record) + sizeof(rec);

	srcdb = GetDatabasePath(rec.src_db, rec.src_spc);
	from = psprintf("%s/%s", srcdb, name);
	if (stat(from, &st) < 0 || !S_ISDIR(st.st_mode))
		return;

	TablespaceCreateDbspace(rec.dst_spc, rec.dst_db, true);
	dstdb = GetDatabasePath(rec.dst_db, rec.dst_spc);
	to = psprintf("%s/%s", dstdb, name);

	/* replayed before, by a server that has since restarted */
	if (stat(to, &st) == 0 && !rmtree(to, true))
		ereport(WARNING,
				(errmsg("some useless files may be left behind in old directory \"%s\"",
						to)));

	copydir(from, to, true);
	fsync_fname(dstdb, true);
}

static void
dbcopy_desc(StringInfo buf, XLogReaderState *record)
{
	xl_gp_dbcopy rec;
	const char *name = XLogRecGetData(record) + sizeof(rec);

	if ((XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_GP_CORE_CLUSTER)
	{
		appendStringInfo(buf, "nodes' states, %u bytes", XLogRecGetDataLen(record));
		return;
	}
	memcpy(&rec, XLogRecGetData(record), sizeof(rec));
	appendStringInfo(buf, "copy dir %u/%u/%s to %u/%u/%s",
					 rec.src_spc, rec.src_db, name,
					 rec.dst_spc, rec.dst_db, name);
}

static const char *
dbcopy_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_GP_CORE_DBCOPY)
		return "DBCOPY";
	if ((info & ~XLR_INFO_MASK) == XLOG_GP_CORE_CLUSTER)
		return "CLUSTER";
	return NULL;
}

static const RmgrData gp_core_rmgr = {
	.rm_name = "gp_core",
	.rm_redo = dbcopy_redo,
	.rm_desc = dbcopy_desc,
	.rm_identify = dbcopy_identify,
};

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpDbcopyInit(void)
{
	RegisterCustomRmgr(GP_CORE_RMGR_ID, &gp_core_rmgr);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = dbcopy_ProcessUtility;

	RegisterXactCallback(dbcopy_xact_callback, NULL);
}
