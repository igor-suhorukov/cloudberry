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
 * gp_dirxact.c
 *	  A database's and a tablespace's directories follow the transaction
 *	  that made or dropped them.
 *
 * On a cluster CREATE DATABASE, CREATE and DROP TABLESPACE and ALTER
 * DATABASE ... SET TABLESPACE run inside the coordinator's distributed
 * transaction on every node, prepared on each segment and committed by the
 * coordinator's commit, as Cloudberry runs them (gp_ddl.c).  PostgreSQL's
 * own run each outside a transaction block, and a directory they make or
 * remove does not follow the transaction: nothing removes a new database's
 * directory, or a new tablespace's, when the transaction aborts after the
 * statement, and DROP TABLESPACE removes the directories before its
 * transaction commits.  So the directories are the transaction's here, as
 * Cloudberry's pendingDbDeletes and its pending tablespace for commit and
 * for abort make them: each statement asks for what is to go when the
 * transaction commits -- a moved database's old directory, a dropped
 * tablespace's -- or when it aborts -- a new database's, a moved one's copy,
 * a new tablespace's -- and the transaction's end does it.
 *
 * What goes is logged as PostgreSQL logs it, XLOG_DBASE_DROP and
 * XLOG_TBLSPC_DROP, so that a mirror or a standby removes its own, by
 * PostgreSQL's redo, after the transaction's end record; Cloudberry carries
 * the same in its commit and abort records (xl_xact_deldbs), which an
 * extension cannot add to.
 *
 * A segment's part is prepared, and ended later by COMMIT or ROLLBACK
 * PREPARED -- in another backend perhaps, after a restart even, the part
 * having been recovered from its PREPARE record.  So as a part prepares,
 * before its record is written, what it asked for is written to a file of
 * its gid's under gp_dirxact/, with its transaction ID; and each COMMIT or
 * ROLLBACK PREPARED, before and after it runs, does what every such file
 * asks for whose transaction has ended, by how it ended, and removes it.
 * That finds a part that ended without the backend that prepared it, and
 * one that died before its PREPARE record, whose ROLLBACK PREPARED finds no
 * transaction and fails, the file then saying an aborted transaction's.  A
 * part that dies before it writes the file leaves its directories, as
 * Cloudberry's part that dies before its PREPARE record does.  The file is
 * logged as it is written, and as it is removed, in records of gp_core's
 * resource manager, which a mirror replays into a file of its own: a mirror
 * promoted while the part is prepared ends it as its primary would have,
 * where Cloudberry's finds the same in the part's PREPARE record.
 *
 * DROP TABLESPACE is this file's too: PostgreSQL's DropTableSpace() removes
 * the directories as it runs, and so this one, as Cloudberry's does, checks
 * them empty and leaves them to the commit.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/catalog/storage_database.c and storage_tablespace.c, what
 *	  its xact.c and twophase.c do with them, and DropTableSpace() in
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands_xlog.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/md.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_dbcopy.h"
#include "gp_dirxact.h"
#include "gp_fault.h"

/* Where a prepared part's file is, in the data directory. */
#define DIRXACT_DIR		"gp_dirxact"

typedef struct DirAction
{
	char		kind;			/* 'd' a database's directory, 't' a tablespace's */
	bool		at_commit;		/* done as the transaction commits, else as it aborts */
	bool		created;		/* 'd': a database the transaction made */
	Oid			db;				/* 'd': the database */
	Oid			spc;			/* its tablespace, or InvalidOid: every one; 't': the tablespace */
	int			nest_level;		/* of the subtransaction that asked */
} DirAction;

/* This transaction's, in TopMemoryContext. */
static List *actions = NIL;

/* The gid PREPARE TRANSACTION names, and whether its file is written. */
static char *preparing_gid = NULL;
static bool file_written = false;

/* One backend at a time ends the prepared parts' files. */
static LWLock *files_lock = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ------------------------------------------------------------------------- */
/* What a transaction asks for                                               */
/* ------------------------------------------------------------------------- */

static void
ask(char kind, Oid db, Oid spc, bool at_commit, bool created)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	DirAction  *a = palloc0(sizeof(DirAction));

	a->kind = kind;
	a->db = db;
	a->spc = spc;
	a->at_commit = at_commit;
	a->created = created;
	a->nest_level = GetCurrentTransactionNestLevel();
	actions = lappend(actions, a);
	MemoryContextSwitchTo(old);
}

void
GpDirxactDatabase(Oid db, Oid spc, bool at_commit, bool created)
{
	ask('d', db, spc, at_commit, created);
}

void
GpDirxactTablespace(Oid spc, bool at_commit)
{
	ask('t', InvalidOid, spc, at_commit, false);
}

/* ------------------------------------------------------------------------- */
/* Doing it                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A removal's record, flushed at once: what asked for it may end with no
 * flush of its own -- an abort's record is written lazily, and a ROLLBACK
 * PREPARED that found no transaction fails -- and a mirror would keep what
 * its primary removed.
 */
static void
log_now(XLogRecPtr recptr)
{
	XLogFlush(recptr);
}

/*
 * A database's directory in spc, or in every tablespace -- the default one
 * and each pg_tblspc/ links to, read from the directory, since no catalog may
 * be read as a transaction aborts -- as remove_dbtablespaces() removes a new
 * database's (dbcommands.c): the buffers of one the transaction made first,
 * which its WAL_LOG strategy read in, and the requests to sync its files.
 * Logged as PostgreSQL logs a drop, which a mirror's redo removes its own by.
 */
static void
remove_database(Oid db, Oid spc, bool created)
{
	List	   *spcs = NIL;
	List	   *removed = NIL;

	if (created)
	{
		DropDatabaseBuffers(db);
		ForgetDatabaseSyncRequests(db);
	}

	if (OidIsValid(spc))
		spcs = list_make1_oid(spc);
	else
	{
		DIR		   *dir;
		struct dirent *de;

		spcs = list_make1_oid(DEFAULTTABLESPACE_OID);
		if ((dir = AllocateDir(PG_TBLSPC_DIR)) != NULL)
		{
			while ((de = ReadDirExtended(dir, PG_TBLSPC_DIR, LOG)) != NULL)
			{
				Oid			ts = atooid(de->d_name);

				if (OidIsValid(ts))
					spcs = lappend_oid(spcs, ts);
			}
			FreeDir(dir);
		}
	}

	foreach_oid(ts, spcs)
	{
		char	   *path = GetDatabasePath(db, ts);
		struct stat st;

		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (!rmtree(path, true))
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								path)));
			removed = lappend_oid(removed, ts);
		}
		pfree(path);
	}

	if (removed != NIL)
	{
		xl_dbase_drop_rec xlrec;
		Oid		   *ids = palloc_array(Oid, list_length(removed));
		int			n = 0;

		foreach_oid(ts, removed)
			ids[n++] = ts;
		xlrec.db_id = db;
		xlrec.ntablespaces = n;
		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfDbaseDropRec);
		XLogRegisterData(ids, n * sizeof(Oid));
		log_now(XLogInsert(RM_DBASE_ID, XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE));

		/* Cloudberry's, at the end of DropDatabaseDirectories() */
		(void) GP_FAULT("after_drop_database_directories");
	}
}

/*
 * A tablespace's directories on this node, as destroy_tablespace_directories()
 * removes them in redo, saying a problem in a LOG (tablespace.c): each
 * emptied database's directory, the version's, and the link -- with the
 * directory of this node's own its target is, which O32's hook removes as the
 * link goes (gp_ddl.c) -- or an in-place tablespace's directory.  Logged as
 * PostgreSQL logs DROP TABLESPACE, whose redo removes a mirror's.
 */
static void
remove_tablespace(Oid spc)
{
	char	   *linkloc = psprintf("%s/%u", PG_TBLSPC_DIR, spc);
	char	   *version = psprintf("%s/%s", linkloc, TABLESPACE_VERSION_DIRECTORY);
	DIR		   *dir;
	struct dirent *de;
	struct stat st;
	xl_tblspc_drop_rec xlrec;

	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);
	if ((dir = AllocateDir(version)) != NULL)
	{
		while ((de = ReadDirExtended(dir, version, LOG)) != NULL)
		{
			char	   *sub;

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			sub = psprintf("%s/%s", version, de->d_name);
			if (rmdir(sub) < 0)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove directory \"%s\": %m", sub)));
			pfree(sub);
		}
		FreeDir(dir);
		if (rmdir(version) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", version)));
	}
	if (lstat(linkloc, &st) == 0)
	{
		if (S_ISLNK(st.st_mode))
		{
			if (tablespace_location_drop_hook)
				(*tablespace_location_drop_hook) (linkloc, spc, true);
			if (unlink(linkloc) < 0)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove symbolic link \"%s\": %m", linkloc)));
		}
		else if (S_ISDIR(st.st_mode) && rmdir(linkloc) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", linkloc)));
	}
	LWLockRelease(TablespaceCreateLock);

	xlrec.ts_id = spc;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xl_tblspc_drop_rec));
	log_now(XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP));

	pfree(linkloc);
	pfree(version);
}

/*
 * The actions of a transaction that ended, committed or not.  Past the
 * commit, or in the abort, nothing may be raised: an action that fails says
 * so in a WARNING, and the rest are done.
 */
static void
do_actions(List *acts, bool committed)
{
	uint32		holdoff = InterruptHoldoffCount;
	MemoryContext cxt = CurrentMemoryContext;

	foreach_ptr(DirAction, a, acts)
	{
		if (a->at_commit != committed)
			continue;
		PG_TRY();
		{
			if (a->kind == 'd')
				remove_database(a->db, a->spc, a->created);
			else
				remove_tablespace(a->spc);
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			InterruptHoldoffCount = holdoff;
			MemoryContextSwitchTo(cxt);
			edata = CopyErrorData();
			FlushErrorState();
			ereport(WARNING,
					(errmsg("could not remove the directories of %s %u: %s",
							a->kind == 'd' ? "database" : "tablespace",
							a->kind == 'd' ? a->db : a->spc, edata->message)));
			FreeErrorData(edata);
		}
		PG_END_TRY();
	}
}

/* ------------------------------------------------------------------------- */
/* A prepared part's file                                                    */
/* ------------------------------------------------------------------------- */

static char *
file_of(const char *gid)
{
	return psprintf("%s/%s", DIRXACT_DIR, gid);
}

/*
 * A file whole, synced and renamed into place: a prepared part's, before its
 * PREPARE record, or a mirror's copy of one, as it replays its record.
 */
static void
write_path(const char *path, const char *text, int len)
{
	char	   *tmp = psprintf("%s.tmp", path);
	int			fd;

	if (MakePGDirectory(DIRXACT_DIR) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", DIRXACT_DIR)));
	fd = OpenTransientFile(tmp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp)));
	errno = 0;
	if (write(fd, text, len) != len)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	}
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmp)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmp)));
	(void) durable_rename(tmp, path, ERROR);
	pfree(tmp);
}

/*
 * A file's record: its gid, with its NUL, and the file's text after it.  The
 * part's PREPARE record, which follows, flushes it.
 */
static void
log_file(uint8 info, const char *gid, const char *text, int len)
{
	/* where WAL is minimal there is no replica to tell */
	if (!XLogIsNeeded())
		return;
	XLogBeginInsert();
	XLogRegisterData(gid, strlen(gid) + 1);
	if (len > 0)
		XLogRegisterData(text, len);
	(void) XLogInsert(GP_CORE_RMGR_ID, info);
}

/*
 * A line an action: "d <db> <spc> <commit|abort> <created>" or
 * "t <spc> <commit|abort>", after "xid <xid>".
 */
static void
write_file(const char *gid, TransactionId xid)
{
	char	   *path = file_of(gid);
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "xid %u\n", xid);
	foreach_ptr(DirAction, a, actions)
	{
		if (a->kind == 'd')
			appendStringInfo(&buf, "d %u %u %s %d\n", a->db, a->spc,
							 a->at_commit ? "commit" : "abort", a->created ? 1 : 0);
		else
			appendStringInfo(&buf, "t %u %s\n", a->spc,
							 a->at_commit ? "commit" : "abort");
	}
	write_path(path, buf.data, buf.len);
	log_file(XLOG_GP_CORE_DIRXACT, gid, buf.data, buf.len);
	pfree(buf.data);
	pfree(path);
}

/* A file's transaction and actions; false where it cannot be read. */
static bool
read_file(const char *path, TransactionId *xid, List **acts)
{
	FILE	   *f = AllocateFile(path, "r");
	char		line[256];
	bool		ok = false;

	*acts = NIL;
	if (f == NULL)
		return false;
	if (fgets(line, sizeof(line), f) != NULL && sscanf(line, "xid %u", xid) == 1)
	{
		ok = true;
		while (fgets(line, sizeof(line), f) != NULL)
		{
			DirAction  *a = palloc0(sizeof(DirAction));
			char		when[8];
			int			created = 0;

			if (sscanf(line, "d %u %u %7s %d", &a->db, &a->spc, when, &created) == 4)
				a->kind = 'd';
			else if (sscanf(line, "t %u %7s", &a->spc, when) == 2)
				a->kind = 't';
			else
			{
				ok = false;
				break;
			}
			a->at_commit = strcmp(when, "commit") == 0;
			a->created = created != 0;
			*acts = lappend(*acts, a);
		}
	}
	FreeFile(f);
	return ok;
}

/*
 * Every prepared part's file whose transaction has ended: its actions, as it
 * ended, and then the file.  A transaction still in progress -- prepared, or
 * preparing -- keeps its file; one older than what the commit log still
 * knows ended long ago, and whoever ended it did what its file said.
 */
static void
end_files(void)
{
	DIR		   *dir;
	struct dirent *de;

	if (files_lock == NULL)
		return;
	LWLockAcquire(files_lock, LW_EXCLUSIVE);
	if ((dir = AllocateDir(DIRXACT_DIR)) != NULL)
	{
		while ((de = ReadDirExtended(dir, DIRXACT_DIR, LOG)) != NULL)
		{
			char	   *path;
			TransactionId xid;
			List	   *acts;
			bool		known;
			bool		committed = false;

			if (de->d_name[0] == '.' || strstr(de->d_name, ".tmp") != NULL)
				continue;
			path = file_of(de->d_name);
			if (!read_file(path, &xid, &acts) || !TransactionIdIsNormal(xid))
			{
				ereport(LOG,
						(errmsg("removing file \"%s\", which says no transaction", path)));
				(void) unlink(path);
				continue;
			}
			if (TransactionIdIsInProgress(xid))
				continue;

			LWLockAcquire(XactTruncationLock, LW_SHARED);
			known = !TransactionIdPrecedes(xid, TransamVariables->oldestClogXid);
			if (known)
				committed = TransactionIdDidCommit(xid);
			LWLockRelease(XactTruncationLock);

			if (known)
				do_actions(acts, committed);
			if (unlink(path) < 0)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
			log_file(XLOG_GP_CORE_DIRXACT_END, de->d_name, NULL, 0);
			list_free_deep(acts);
			pfree(path);
		}
		FreeDir(dir);
	}
	LWLockRelease(files_lock);
}

/* ------------------------------------------------------------------------- */
/* DROP TABLESPACE                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Does tablespace spc hold nothing on this node -- no file in any
 * database's directory there?  A version directory that is not there holds
 * nothing: its files were removed from under it.
 */
static bool
tablespace_is_empty(Oid spc)
{
	char	   *version = psprintf("%s/%u/%s", PG_TBLSPC_DIR, spc,
								   TABLESPACE_VERSION_DIRECTORY);
	DIR		   *dir = AllocateDir(version);
	struct dirent *de;
	bool		empty = true;

	if (dir == NULL && errno == ENOENT)
		return true;
	while (empty && (de = ReadDir(dir, version)) != NULL)
	{
		char	   *sub;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		sub = psprintf("%s/%s", version, de->d_name);
		empty = directory_is_empty(sub);
		pfree(sub);
	}
	FreeDir(dir);
	pfree(version);
	return empty;
}

/*
 * PostgreSQL's DropTableSpace() (tablespace.c) to its catalog's change and
 * its check that the tablespace is empty, in its words, and the directories
 * left to the commit, as Cloudberry's leaves them
 * (ScheduleTablespaceDirectoryDeletionForCommit()): a transaction that
 * rolls back keeps them, and a mirror, which replays the drop's record the
 * commit writes, keeps its own until then.  A tablespace a table was made in
 * between the check and the commit keeps its files, as Cloudberry's does.
 */
void
GpDropTableSpace(DropTableSpaceStmt *stmt)
{
	char	   *tablespacename = stmt->tablespacename;
	TableScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	Oid			tablespaceoid;
	char	   *detail;
	char	   *detail_log;

	rel = table_open(TableSpaceRelationId, RowExclusiveLock);
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		ereport(NOTICE,
				(errmsg("tablespace \"%s\" does not exist, skipping",
						tablespacename)));
		table_endscan(scandesc);
		table_close(rel, NoLock);
		return;
	}

	tablespaceoid = ((Form_pg_tablespace) GETSTRUCT(tuple))->oid;

	if (!object_ownercheck(TableSpaceRelationId, tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   tablespacename);
	if (IsPinnedObject(TableSpaceRelationId, tablespaceoid))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE,
					   tablespacename);

	LockSharedObject(TableSpaceRelationId, tablespaceoid, 0,
					 AccessExclusiveLock);
	if (checkSharedDependencies(TableSpaceRelationId, tablespaceoid,
								&detail, &detail_log))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("tablespace \"%s\" cannot be dropped because some objects depend on it",
						tablespacename),
				 errdetail_internal("%s", detail),
				 errdetail_log("%s", detail_log)));

	InvokeObjectDropHook(TableSpaceRelationId, tablespaceoid, 0);
	CatalogTupleDelete(rel, &tuple->t_self);
	table_endscan(scandesc);
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);
	DeleteSharedSecurityLabel(tablespaceoid, TableSpaceRelationId);
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Files a DROP TABLE left for the next checkpoint to unlink, and ones
	 * other backends still hold open, are not the tablespace's: a checkpoint
	 * and a barrier, and the check again, as DropTableSpace() does.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);
	if (!tablespace_is_empty(tablespaceoid))
	{
		RequestCheckpoint(CHECKPOINT_FAST | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
		LWLockRelease(TablespaceCreateLock);
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));
		LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);
		if (!tablespace_is_empty(tablespaceoid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));
	}

	GpDirxactTablespace(tablespaceoid, true);

	/* Cloudberry's, where its DROP has written its record */
	(void) GP_FAULT("after_xlog_tblspc_drop");

	ForceSyncCommit();
	LWLockRelease(TablespaceCreateLock);
	table_close(rel, NoLock);
}

/* ------------------------------------------------------------------------- */
/* The transaction's end                                                     */
/* ------------------------------------------------------------------------- */

static void
forget(void)
{
	list_free_deep(actions);
	actions = NIL;
	if (preparing_gid != NULL)
		pfree(preparing_gid);
	preparing_gid = NULL;
	file_written = false;
}

static void
dirxact_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_PREPARE:
			if (actions != NIL && preparing_gid != NULL)
			{
				write_file(preparing_gid, GetTopTransactionIdIfAny());
				file_written = true;
			}
			break;

		case XACT_EVENT_PREPARE:
			/* the file has them now, for whoever ends the part */
			forget();
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			do_actions(actions, true);
			forget();
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			do_actions(actions, false);
			if (file_written && preparing_gid != NULL)
			{
				char	   *path = file_of(preparing_gid);

				(void) unlink(path);
				log_file(XLOG_GP_CORE_DIRXACT_END, preparing_gid, NULL, 0);
				pfree(path);
			}
			forget();
			break;

		default:
			break;
	}
}

/*
 * A subtransaction's: what it asked for becomes its parent's as it commits,
 * and goes as it aborts, the directories it made with it.
 */
static void
dirxact_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	List	   *gone = NIL;
	ListCell   *lc;

	if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB)
		return;
	foreach(lc, actions)
	{
		DirAction  *a = (DirAction *) lfirst(lc);

		if (a->nest_level < level)
			continue;
		if (event == SUBXACT_EVENT_COMMIT_SUB)
			a->nest_level = level - 1;
		else
		{
			gone = lappend(gone, a);
			actions = foreach_delete_current(actions, lc);
		}
	}
	do_actions(gone, false);
	list_free_deep(gone);
}

/*
 * The two phases, as they run: the gid a PREPARE writes its file under, and
 * the files of the parts that ended, before COMMIT or ROLLBACK PREPARED runs
 * and after.
 */
static void
dirxact_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	TransactionStmt *ts = IsA(pstmt->utilityStmt, TransactionStmt) ?
		(TransactionStmt *) pstmt->utilityStmt : NULL;
	bool		second_phase = ts != NULL &&
		(ts->kind == TRANS_STMT_COMMIT_PREPARED ||
		 ts->kind == TRANS_STMT_ROLLBACK_PREPARED);

	if (ts != NULL && ts->kind == TRANS_STMT_PREPARE)
	{
		if (preparing_gid != NULL)
			pfree(preparing_gid);
		preparing_gid = MemoryContextStrdup(TopMemoryContext, ts->gid);
	}
	if (second_phase)
		end_files();

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (second_phase)
		end_files();
}

/* ------------------------------------------------------------------------- */
/* WAL                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * A prepared part's file, as a mirror, a standby or a server recovering from
 * an archive replays its records: written as its primary wrote it, and
 * removed as its primary removed it.  A server recovering from a crash has
 * its own files already, and keeps the ones it removed since removed.
 */
void
GpDirxactRedo(uint8 info, const char *data, int len)
{
	int			gidlen = strnlen(data, len) + 1;
	char	   *path;

	if (gidlen > len)
		elog(PANIC, "gp_core_redo: a prepared part's file with no gid");
	path = file_of(data);
	if (info == XLOG_GP_CORE_DIRXACT)
	{
		if (ArchiveRecoveryRequested)
			write_path(path, data + gidlen, len - gidlen);
	}
	else if (unlink(path) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
	pfree(path);
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

static void
dirxact_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestNamedLWLockTranche("gp_core dirxact", 1);
}

static void
dirxact_shmem_startup(void)
{
	if (prev_shmem_startup)
		prev_shmem_startup();
	files_lock = &(GetNamedLWLockTranche("gp_core dirxact"))->lock;
}

void
GpDirxactInit(void)
{
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = dirxact_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = dirxact_shmem_startup;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = dirxact_ProcessUtility;

	RegisterXactCallback(dirxact_xact_callback, NULL);
	RegisterSubXactCallback(dirxact_subxact_callback, NULL);
}
