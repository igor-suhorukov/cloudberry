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
 * gp_workfile.c
 *	  Cloudberry's workfile manager, as far as a module sees it: the limits
 *	  on a statement's temporary files and on the node's, gp_toolkit's views
 *	  of the files, and the words of a segment's cancel.
 *
 * Cloudberry's fd.c and buffile.c tell its workfile manager of every
 * temporary file an operator spills to -- a workfile -- as it is made,
 * grows and is closed (workfile_mgr.c), and the manager keeps the counts in
 * shared memory: per statement and node, against gp_workfile_limit_per_query
 * and gp_workfile_limit_files_per_query, and per node, against
 * gp_workfile_limit_per_segment.  Its errors say which limit a statement
 * passed, and gp_toolkit's views read the counts.  PostgreSQL 19's
 * temporary files have no hook, and the port takes no core patch for them,
 * so each of these is done here with what PostgreSQL 19 has to do it with:
 *
 *	 the size of a statement's files: temp_file_limit, PostgreSQL's limit on
 *	 the temporary files a process has, which fd.c checks before a write
 *	 that grows one.  While the statement runs -- its ExecutorRun and
 *	 ExecutorFinish -- temp_file_limit is gp.workfile_limit_per_query where
 *	 that is lower, and fd.c's error for it is raised again in Cloudberry's
 *	 words.  At the write, as Cloudberry's, but per process where
 *	 Cloudberry's is per statement and node: a statement with several slices
 *	 on a segment may have the limit in each; and every temporary file of the
 *	 process counts, the rows the coordinator relays to a segment's writer
 *	 too (gp_motion.c), which Cloudberry's interconnect never writes.
 *
 *	 the number of a statement's files, and the size of the node's: the
 *	 temporary files' directories, read as a run of the statement ends if the
 *	 process wrote to a temporary file meanwhile (pgBufferUsage's
 *	 temp_blks_written), the files of its session's processes on the node
 *	 counted -- later than Cloudberry, which counts a file as it is made, and
 *	 blind to a file made and removed between two looks.
 *
 *	 the views: the same directories, a row for each file or FileSet
 *	 directory, with the session of the process that made it.  The
 *	 operator, slice and command, which Cloudberry's workfile set records,
 *	 are not in a file's name, and are NULL.
 *
 * A utility statement's files -- a CREATE INDEX's sort -- are held to none
 * of these, as no executor's run is theirs.
 *
 * Cloudberry's words for a segment's cancel are here too, which segspace
 * reads of a cancel that interrupts a spill: a segment's backend -- a QE --
 * says "canceling MPP operation" where PostgreSQL says "canceling statement
 * due to user request".
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/workfile_manager/workfile_mgr.c (RegisterFileWithSet(),
 *	  UpdateWorkFileSize(), WorkfileSegspace_GetSize()),
 *	  gpcontrib/gp_internal_tools/gp_workfile_mgr.c, the workfile settings of
 *	  src/backend/utils/misc/guc_gp.c, and ProcessInterrupts()'s cancel of a
 *	  QE in src/backend/tcop/postgres.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <sys/stat.h>

#include "access/parallel.h"
#include "common/file_utils.h"
#include "common/relpath.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_endpoint.h"
#include "gp_gdd.h"
#include "gp_workfile.h"

/* fd.c's error for temp_file_limit, as errmsg() is given it */
#define TEMP_FILE_LIMIT_MSGID	"temporary file size exceeds \"temp_file_limit\" (%dkB)"

/* ProcessInterrupts()'s for a cancel, likewise */
#define CANCEL_MSGID			"canceling statement due to user request"

/*
 * The FileSets of the rows the coordinator relays to a segment's readers,
 * which gp_motion.c numbers from here up: not workfiles, as Cloudberry's
 * interconnect writes no file.
 */
#define RELAY_FILESET_NUMBER	0x80000000UL

static const char workfile_per_query_msg[] = "workfile per query size limit exceeded";

static int	gp_workfile_limit_per_query = 0;	/* kB */
static int	gp_workfile_limit_files_per_query = 100000;
static int	gp_workfile_limit_per_segment = 0;	/* kB */
static int	gp_workfile_max_entries = 8192;

/* pgBufferUsage.temp_blks_written as the directories were last read */
static int64 polled_temp_blks = 0;

static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;

/* ------------------------------------------------------------------------- */
/* The temporary files' directories                                          */
/* ------------------------------------------------------------------------- */

/* A temporary file of the node, or a FileSet's directory and its files. */
typedef struct WorkfileEntry
{
	char		name[MAXPGPATH];	/* its name in the directory */
	int			pid;			/* the process that made it */
	int64		size;
	int			nfiles;
} WorkfileEntry;

typedef void (*WorkfileVisit) (const WorkfileEntry *entry, void *arg);

/* A FileSet's directory: its files, and their size. */
static void
scan_fileset(const char *path, WorkfileEntry *entry)
{
	DIR		   *dir = AllocateDir(path);
	struct dirent *de;

	if (dir == NULL && errno == ENOENT)
		return;
	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		char		file[MAXPGPATH * 2];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(file, sizeof(file), "%s/%s", path, de->d_name);
		if (stat(file, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		entry->size += st.st_size;
		entry->nfiles++;
	}
	FreeDir(dir);
}

/*
 * One pgsql_tmp directory: each file pgsql_tmp<pid>.<n> of it, and each
 * FileSet's directory pgsql_tmp<pid>.<n>.fileset, as fd.c and fileset.c
 * name them.  A file may be removed as it is read, which is no error.
 */
static void
scan_dir(const char *path, WorkfileVisit visit, void *arg)
{
	DIR		   *dir = AllocateDir(path);
	struct dirent *de;

	if (dir == NULL && errno == ENOENT)
		return;
	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		WorkfileEntry entry;
		char		file[MAXPGPATH * 2];
		const char *rest;
		unsigned long pid;
		unsigned long number;
		int			len = 0;
		struct stat st;

		if (strncmp(de->d_name, PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) != 0)
			continue;
		rest = de->d_name + strlen(PG_TEMP_FILE_PREFIX);
		if (sscanf(rest, "%lu.%lu%n", &pid, &number, &len) != 2)
			continue;
		snprintf(file, sizeof(file), "%s/%s", path, de->d_name);
		if (stat(file, &st) != 0)
			continue;

		strlcpy(entry.name, de->d_name, sizeof(entry.name));
		entry.pid = (int) pid;
		entry.size = 0;
		entry.nfiles = 0;
		if (S_ISDIR(st.st_mode) && strcmp(rest + len, ".fileset") == 0)
		{
			if (number & RELAY_FILESET_NUMBER)
				continue;
			scan_fileset(file, &entry);
		}
		else if (S_ISREG(st.st_mode) && rest[len] == '\0')
		{
			entry.size = st.st_size;
			entry.nfiles = 1;
		}
		else
			continue;
		visit(&entry, arg);
	}
	FreeDir(dir);
}

/*
 * Every temporary file of the node, where RemovePgTempFiles() looks for them
 * (fd.c): base/pgsql_tmp, and the pgsql_tmp of each tablespace.
 */
static void
scan_node(WorkfileVisit visit, void *arg)
{
	char		path[MAXPGPATH];
	DIR		   *spc_dir;
	struct dirent *spc_de;

	snprintf(path, sizeof(path), "base/%s", PG_TEMP_FILES_DIR);
	scan_dir(path, visit, arg);

	spc_dir = AllocateDir(PG_TBLSPC_DIR);
	while ((spc_de = ReadDirExtended(spc_dir, PG_TBLSPC_DIR, LOG)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/%s/%s", PG_TBLSPC_DIR,
				 spc_de->d_name, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
		scan_dir(path, visit, arg);
	}
	FreeDir(spc_dir);
}

/*
 * The session a process of this node works for, as gp_stat_activity's
 * sess_id gives it (gp_activity_session(), gp_segment.c): the one it told
 * the global deadlock detector, and on one node a client's own process ID;
 * -1 for a process that works for none.
 */
static int
process_session(int pid)
{
	int			session;
	bool		reader;
	PGPROC	   *proc;

	if (GpGddBackendIdentity(pid, &session, &reader))
		return session;
	proc = GpClusterIsSingleNode() ? BackendPidGetProc(pid) : NULL;
	if (proc != NULL && proc->backendType == B_BACKEND)
		return pid;
	return -1;
}

/* ------------------------------------------------------------------------- */
/* The limits                                                                */
/* ------------------------------------------------------------------------- */

/* What a look at the directories finds of a statement's files. */
typedef struct WorkfileTally
{
	int			session;		/* the statement's, gp_session_id */
	int			files;			/* of its processes on this node */
	int64		bytes;			/* of every process of the node */
	int			lastpid;		/* the process last asked about */
	bool		lastours;		/* whether it works for the session */
} WorkfileTally;

static void
tally_entry(const WorkfileEntry *entry, void *arg)
{
	WorkfileTally *tally = (WorkfileTally *) arg;

	tally->bytes += entry->size;
	if (entry->pid != tally->lastpid)
	{
		tally->lastpid = entry->pid;
		tally->lastours = entry->pid == MyProcPid ||
			process_session(entry->pid) == tally->session;
	}
	if (tally->lastours)
		tally->files += entry->nfiles;
}

/*
 * The files of the statement's session on this node, and the node's bytes,
 * against gp.workfile_limit_files_per_query and
 * gp.workfile_limit_per_segment, as Cloudberry's RegisterFileWithSet() and
 * UpdateWorkFileSize() hold them, and in their words: as a run ends, where
 * the process wrote to a temporary file since the last look.  Not in a
 * parallel worker, whose leader counts its blocks when it ends.
 */
static void
workfile_poll(void)
{
	WorkfileTally tally;

	if (pgBufferUsage.temp_blks_written == polled_temp_blks ||
		IsParallelWorker())
		return;
	polled_temp_blks = pgBufferUsage.temp_blks_written;
	if (gp_workfile_limit_files_per_query <= 0 &&
		gp_workfile_limit_per_segment <= 0)
		return;

	memset(&tally, 0, sizeof(tally));
	tally.session = GpClusterSessionId();
	scan_node(tally_entry, &tally);

	if (gp_workfile_limit_files_per_query > 0 &&
		tally.files > gp_workfile_limit_files_per_query)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("number of workfiles per query limit exceeded")));
	if (gp_workfile_limit_per_segment > 0 &&
		tally.bytes / 1024 > gp_workfile_limit_per_segment)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("workfile per segment size limit exceeded")));
}

/*
 * gp.workfile_limit_per_query as temp_file_limit for a run of the
 * statement, where it is lower: at a GUC nest level of the run's own, which
 * the run's end pops as a function's SET clause is popped, and an abort
 * with the rest.  A SET of temp_file_limit in the run stays, as it would
 * there.  The level, or 0 where nothing is set.
 */
static int
workfile_limit_begin(void)
{
	char		value[16];
	int			nestlevel;

	if (gp_workfile_limit_per_query <= 0 ||
		(temp_file_limit >= 0 &&
		 temp_file_limit <= gp_workfile_limit_per_query))
		return 0;
	nestlevel = NewGUCNestLevel();
	snprintf(value, sizeof(value), "%d", gp_workfile_limit_per_query);
	(void) set_config_option("temp_file_limit", value, PGC_SUSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return nestlevel;
}

static void
workfile_limit_end(int nestlevel)
{
	if (nestlevel > 0)
		AtEOXact_GUC(true, nestlevel);
}

/*
 * An error of the statement's run, raised again: fd.c's for temp_file_limit,
 * where the limit it passed is gp.workfile_limit_per_query, in Cloudberry's
 * words and with its SQLSTATE (UpdateWorkFileSize()), in every run it passes
 * through -- a function's query's too, whose handler reads the words; any
 * other error as it is.
 */
static void
workfile_rethrow(MemoryContext cxt)
{
	ErrorData  *edata;

	if (gp_workfile_limit_per_query <= 0 ||
		temp_file_limit != gp_workfile_limit_per_query)
		PG_RE_THROW();

	MemoryContextSwitchTo(cxt);
	edata = CopyErrorData();
	if (edata->message_id == NULL ||
		strcmp(edata->message_id, TEMP_FILE_LIMIT_MSGID) != 0)
	{
		FreeErrorData(edata);
		PG_RE_THROW();
	}
	FlushErrorState();
	edata->sqlerrcode = ERRCODE_INSUFFICIENT_RESOURCES;
	edata->message = pstrdup(workfile_per_query_msg);
	edata->message_id = workfile_per_query_msg;
	ReThrowError(edata);
}

static void
workfile_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					 uint64 count)
{
	MemoryContext cxt = CurrentMemoryContext;
	int			nestlevel = workfile_limit_begin();

	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_CATCH();
	{
		workfile_rethrow(cxt);
	}
	PG_END_TRY();
	workfile_limit_end(nestlevel);
	workfile_poll();
}

static void
workfile_ExecutorFinish(QueryDesc *queryDesc)
{
	MemoryContext cxt = CurrentMemoryContext;
	int			nestlevel = workfile_limit_begin();

	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_CATCH();
	{
		workfile_rethrow(cxt);
	}
	PG_END_TRY();
	workfile_limit_end(nestlevel);
	workfile_poll();
}

/* ------------------------------------------------------------------------- */
/* A segment's cancel                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A dispatched backend's cancel in the words of Cloudberry's QE: "canceling
 * MPP operation" (ProcessInterrupts()).  Its SQLSTATE stays PostgreSQL's
 * query_canceled, by which the coordinator tells a slice cancelled because
 * another failed from the failure (gp_dispatch.c).  An endpoint's sender
 * that its retrieve session cancelled says why after it, as Cloudberry's
 * does with the cancel message the session set (gp_endpoint.c).
 */
static void
workfile_emit_log(ErrorData *edata)
{
	if (edata->elevel == ERROR && edata->sqlerrcode == ERRCODE_QUERY_CANCELED &&
		edata->message_id != NULL &&
		strcmp(edata->message_id, CANCEL_MSGID) == 0 &&
		GpClusterIsDispatched())
	{
		const char *why = GpEndpointCancelMessage();

		edata->message = why != NULL
			? psprintf("canceling MPP operation: \"%s\"", why)
			: pstrdup("canceling MPP operation");
	}

	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);
}

/* ------------------------------------------------------------------------- */
/* gp_toolkit's views                                                        */
/* ------------------------------------------------------------------------- */

static void
entry_row(const WorkfileEntry *entry, void *arg)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) arg;
	int			session = process_session(entry->pid);
	Datum		values[8];
	bool		nulls[8] = {false, false, false, true, true, false, true, false};

	values[0] = Int32GetDatum(GpClusterContentId());
	values[1] = CStringGetTextDatum(entry->name);
	values[2] = Int64GetDatum(entry->size);
	values[5] = Int32GetDatum(session);
	nulls[5] = session < 0;
	values[7] = Int32GetDatum(entry->nfiles);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

static void
add_bytes(const WorkfileEntry *entry, void *arg)
{
	*(int64 *) arg += entry->size;
}

PG_FUNCTION_INFO_V1(gp_workfile_entries);
PG_FUNCTION_INFO_V1(gp_workfile_used_diskspace);

/*
 * gp_toolkit.__gp_workfile_entries_here(): each temporary file of this node,
 * as Cloudberry's gp_workfile_mgr_cache_entries() gives each workfile set:
 * the node's content id, the file's name as its prefix, its size, the
 * session of the process that made it, and how many files it is.
 */
Datum
gp_workfile_entries(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	scan_node(entry_row, fcinfo->resultinfo);
	return (Datum) 0;
}

/*
 * gp_toolkit.__gp_workfile_mgr_used_diskspace_here(): the bytes of this
 * node's temporary files, as Cloudberry's gp_workfile_mgr_used_diskspace()
 * gives its manager's (WorkfileSegspace_GetSize()).
 */
Datum
gp_workfile_used_diskspace(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int64		bytes = 0;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	InitMaterializedSRF(fcinfo, 0);
	scan_node(add_bytes, &bytes);
	values[0] = Int32GetDatum(GpClusterContentId());
	values[1] = Int64GetDatum(bytes);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpWorkfileInit(void)
{
	/* segments are sent the statement's two (gp_dispatch.c) */
	DefineCustomIntVariable("gp.workfile_limit_per_query",
							"Maximum disk space (in KB) used for workfiles per query per segment.",
							"0 for no limit. Current query is terminated when limit is exceeded: here by a process of it on a node, whose temporary files temp_file_limit counts while it runs. Cloudberry calls this gp_workfile_limit_per_query.",
							&gp_workfile_limit_per_query,
							0, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.workfile_limit_files_per_query",
							"Maximum number of workfiles allowed per query per segment.",
							"0 for no limit. Current query is terminated when limit is exceeded: here as a run of it ends. Cloudberry calls this gp_workfile_limit_files_per_query.",
							&gp_workfile_limit_files_per_query,
							100000, 0, INT_MAX,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.workfile_limit_per_segment",
							"Maximum disk space (in KB) used for workfiles per segment.",
							"0 for no limit. Current query is terminated when limit is exceeded: here as a run of it ends. Cloudberry calls this gp_workfile_limit_per_segment.",
							&gp_workfile_limit_per_segment,
							0, 0, INT_MAX,
							PGC_POSTMASTER, GUC_UNIT_KB,
							NULL, NULL, NULL);

	/*
	 * Cloudberry's is also GUC_NO_SHOW_ALL, which here would hide it from
	 * pg_settings, where the test harnesses find the names they respell.
	 */
	DefineCustomIntVariable("gp.workfile_max_entries",
							"Sets the maximum number of entries that can be stored in the workfile directory",
							"Accepted for Cloudberry's scripts: the files are counted where they lie, with no directory of entries to size. Cloudberry calls this gp_workfile_max_entries.",
							&gp_workfile_max_entries,
							8192, 32, INT_MAX,
							PGC_POSTMASTER, GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = workfile_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = workfile_ExecutorFinish;
	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = workfile_emit_log;
}
