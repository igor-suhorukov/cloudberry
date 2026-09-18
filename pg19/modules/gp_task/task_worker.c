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
 * task_worker.c
 *	  The launcher, and the worker that runs one job.
 *
 * The launcher wakes on the minute, asks which jobs that minute is due, and
 * starts a background worker for each.  It stays connected to
 * gp.task_database, where the tables are; a job runs in its own database, as
 * its own role.
 *
 * That split is why the two talk through a shared memory segment.  The job
 * tables are ordinary tables in one database rather than the shared catalogs
 * Cloudberry uses, so the process running a job cannot write its own history
 * -- it is not connected to the database that holds it.  So the launcher
 * writes the history, and the worker leaves its answer in the segment for the
 * launcher to read once it has exited.  Cloudberry passes a segment the same
 * way and uses a shm_mq inside it; a bounded message needs no queue.
 *
 * The launcher does not wait for a job.  It keeps what it started in a list
 * and looks at that list every time round, so a long job delays nothing but
 * itself.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_database.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "gp_task.h"

#define GP_TASK_LIBRARY		"gp_task"

/* One job the launcher started and has not finished with. */
typedef struct GpTaskRunning
{
	int64		runid;
	int64		jobid;
	dsm_segment *seg;
	BackgroundWorkerHandle *handle;
	bool		pid_recorded;
} GpTaskRunning;

static List *gp_task_running = NIL;

/* Said once, not once a minute, while the tables are not where they should be. */
static bool gp_task_said_no_tables = false;

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

void
GpTaskRegisterLauncher(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 10;
	snprintf(worker.bgw_library_name, MAXPGPATH, "%s", GP_TASK_LIBRARY);
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "gp_task_launcher_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_task scheduler");
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_task scheduler");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}

/* ------------------------------------------------------------------------- */
/* Starting one job                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The database and the role a job names, looked up here rather than in the
 * worker: pg_database and pg_authid are shared, so the launcher can read them
 * from the database it is connected to, and a job that names something that
 * has been dropped is then reported rather than left to a worker that cannot
 * start.
 */
static bool
resolve_job(const GpTaskJob *job, Oid *dbid, Oid *roleid, char **why)
{
	*dbid = get_database_oid(job->database, true);
	if (!OidIsValid(*dbid))
	{
		*why = psprintf("database \"%s\" does not exist", job->database);
		return false;
	}

	*roleid = get_role_oid(job->username, true);
	if (!OidIsValid(*roleid))
	{
		*why = psprintf("role \"%s\" does not exist", job->username);
		return false;
	}

	return true;
}

static void
start_job(const GpTaskJob *job, int64 runid)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	GpTaskJobShared *shared;
	dsm_segment *seg;
	Size		commandlen = strlen(job->command) + 1;
	Size		size = offsetof(GpTaskJobShared, command) + commandlen;
	GpTaskRunning *running;
	MemoryContext oldcxt;
	Oid			dbid;
	Oid			roleid;
	char	   *why = NULL;

	if (!resolve_job(job, &dbid, &roleid, &why))
	{
		GpTaskRunStarted(runid, job);
		GpTaskRunFinished(runid, true, why);
		return;
	}

	seg = dsm_create(size, 0);
	shared = (GpTaskJobShared *) dsm_segment_address(seg);
	shared->dbid = dbid;
	shared->roleid = roleid;
	shared->jobid = job->jobid;
	shared->runid = runid;
	shared->pid = 0;
	shared->done = false;
	shared->failed = false;
	shared->message[0] = '\0';
	memcpy(shared->command, job->command, commandlen);

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, MAXPGPATH, "%s", GP_TASK_LIBRARY);
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "gp_task_job_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_task job " INT64_FORMAT, job->jobid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_task job");
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
	worker.bgw_notify_pid = MyProcPid;

	GpTaskRunStarted(runid, job);

	/*
	 * What follows outlives the transaction this runs in: the handle
	 * RegisterDynamicBackgroundWorker returns is palloc'd where it is called,
	 * and the launcher keeps it until the job exits.  A BackgroundWorkerHandle
	 * is opaque, so it cannot be copied afterwards -- it has to be allocated
	 * in the right place to begin with.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
	{
		MemoryContextSwitchTo(oldcxt);
		dsm_detach(seg);
		GpTaskRunFinished(runid, true,
						  "no background worker slot was free; "
						  "\"max_worker_processes\" may be too low");
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("gp_task could not start job " INT64_FORMAT, job->jobid),
				 errhint("You might need to raise \"max_worker_processes\".")));
		return;
	}

	/*
	 * The segment was created by this process and would go when the
	 * transaction that created it ends, so it is kept until the job is done
	 * with it.
	 */
	dsm_pin_mapping(seg);

	running = palloc0(sizeof(GpTaskRunning));
	running->runid = runid;
	running->jobid = job->jobid;
	running->seg = seg;
	running->handle = handle;
	gp_task_running = lappend(gp_task_running, running);

	MemoryContextSwitchTo(oldcxt);

	if (gp_task_log_run)
		ereport(LOG,
				(errmsg("gp_task job " INT64_FORMAT " started: %s",
						job->jobid, job->command)));
}

/*
 * Look at the jobs that are running: note the pid of any that has one now,
 * and finish with any that has exited.
 *
 * Each write is its own transaction with a snapshot of its own.  SPI will not
 * run a statement without one -- "cannot execute SQL without an outer snapshot
 * or portal" -- and an error here would take the launcher down with it.
 */
static void
reap_jobs(void)
{
	ListCell   *lc;
	List	   *done = NIL;
	bool		record;

	if (gp_task_running == NIL)
		return;

	/*
	 * The extension can be dropped while a job is still running.  What the
	 * job did is then unrecorded, because there is nowhere to record it, and
	 * the launcher carries on rather than failing on every write.
	 */
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	record = GpTaskTablesExist();
	PopActiveSnapshot();
	CommitTransactionCommand();

	foreach(lc, gp_task_running)
	{
		GpTaskRunning *running = (GpTaskRunning *) lfirst(lc);
		GpTaskJobShared *shared = (GpTaskJobShared *) dsm_segment_address(running->seg);
		pid_t		pid;

		if (record && !running->pid_recorded && shared->pid != 0)
		{
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());
			GpTaskRunPid(running->runid, shared->pid);
			PopActiveSnapshot();
			CommitTransactionCommand();
			running->pid_recorded = true;
		}

		if (GetBackgroundWorkerPid(running->handle, &pid) != BGWH_STOPPED)
			continue;

		done = lappend(done, running);
	}

	foreach(lc, done)
	{
		GpTaskRunning *running = (GpTaskRunning *) lfirst(lc);
		GpTaskJobShared *shared = (GpTaskJobShared *) dsm_segment_address(running->seg);
		bool		failed = true;
		char		message[GP_TASK_MSGLEN];

		if (shared->done)
		{
			failed = shared->failed;
			strlcpy(message, shared->message, sizeof(message));
		}
		else
		{
			/*
			 * The worker left nothing behind, so it did not reach the end of
			 * its own error handling: it was killed, or it crashed.
			 */
			strlcpy(message, "the job's process ended without an answer",
					sizeof(message));
		}

		if (record)
		{
			StartTransactionCommand();
			PushActiveSnapshot(GetTransactionSnapshot());
			GpTaskRunFinished(running->runid, failed, message);
			PopActiveSnapshot();
			CommitTransactionCommand();
		}

		if (gp_task_log_run)
			ereport(LOG,
					(errmsg("gp_task job " INT64_FORMAT " %s",
							running->jobid, failed ? "failed" : "succeeded")));

		dsm_detach(running->seg);
		pfree(running->handle);
		gp_task_running = list_delete_ptr(gp_task_running, running);
		pfree(running);
	}

	list_free(done);
}

/* ------------------------------------------------------------------------- */
/* The launcher                                                              */
/* ------------------------------------------------------------------------- */

/*
 * The start of the minute after the given time.  The launcher sleeps until
 * then, so that a schedule means the same thing however long a scan took.
 */
static TimestampTz
next_minute(TimestampTz now)
{
	return (now / USECS_PER_MINUTE + 1) * USECS_PER_MINUTE;
}

static void
run_due_jobs(TimestampTz minute)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	List	   *jobs;
	ListCell   *lc;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"gp_task scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	StartTransactionCommand();

	/*
	 * Back to this scan's own context: starting a transaction switches to the
	 * transaction's, and committing it frees that one, which would take the
	 * jobs just read with it.
	 */
	MemoryContextSwitchTo(cxt);

	PushActiveSnapshot(GetTransactionSnapshot());

	if (!GpTaskTablesExist())
	{
		if (!gp_task_said_no_tables)
		{
			ereport(LOG,
					(errmsg("gp_task has no jobs to read in database \"%s\"",
							gp_task_database),
					 errhint("Run \"CREATE EXTENSION gp_task\" in that database, "
							 "or point \"gp.task_database\" at the one that has it.")));
			gp_task_said_no_tables = true;
		}

		PopActiveSnapshot();
		CommitTransactionCommand();
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
		return;
	}
	gp_task_said_no_tables = false;

	jobs = GpTaskLoadJobs();
	PopActiveSnapshot();
	CommitTransactionCommand();
	MemoryContextSwitchTo(cxt);

	foreach(lc, jobs)
	{
		GpTaskJob  *job = (GpTaskJob *) lfirst(lc);
		int64		runid;

		if (!GpTaskScheduleDue(job->schedule, minute))
			continue;

		/*
		 * Asked after the schedule, so that what the warning names is a job
		 * that wanted to run rather than the next one in the table.
		 */
		if (list_length(gp_task_running) >= gp_task_max_running)
		{
			ereport(WARNING,
					(errmsg("gp_task is already running %d jobs, so job " INT64_FORMAT " was skipped",
							gp_task_max_running, job->jobid),
					 errhint("Raise \"gp.task_max_running\" if jobs should overlap further.")));
			break;
		}

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		runid = GpTaskNextRunId();
		start_job(job, runid);
		PopActiveSnapshot();
		CommitTransactionCommand();

		/*
		 * Committing leaves the process in TopMemoryContext, which lives as
		 * long as the launcher does.  What the next job's schedule allocates
		 * belongs to this scan.
		 */
		MemoryContextSwitchTo(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

void
gp_task_launcher_main(Datum arg)
{
	TimestampTz wake;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_task_database, NULL, 0);

	/*
	 * A job that was running when the server stopped is not running now, and
	 * its row still says it is.
	 */
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (GpTaskTablesExist())
		GpTaskMarkRunningAsFailed();
	PopActiveSnapshot();
	CommitTransactionCommand();

	ereport(LOG,
			(errmsg("gp_task scheduler is reading jobs from database \"%s\"",
					gp_task_database)));

	wake = next_minute(GetCurrentTimestamp());

	for (;;)
	{
		TimestampTz now;
		long		ms;

		now = GetCurrentTimestamp();
		ms = (wake > now) ? (long) ((wake - now) / 1000) : 0;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* A worker that finished set the latch; deal with it either way. */
		reap_jobs();

		now = GetCurrentTimestamp();
		if (now < wake)
			continue;			/* woken by a worker, not by the clock */

		if (gp_task_enabled)
			run_due_jobs(wake);

		/*
		 * From the minute that has just passed, not from now: a scan that
		 * took longer than a minute must not skip one.
		 */
		wake = next_minute(wake);
		if (wake <= GetCurrentTimestamp())
			wake = next_minute(GetCurrentTimestamp());
	}
}

/* ------------------------------------------------------------------------- */
/* One job                                                                   */
/* ------------------------------------------------------------------------- */

void
gp_task_job_main(Datum arg)
{
	dsm_segment *seg;
	GpTaskJobShared *shared;
	char	   *command;
	bool		failed = false;
	char		message[GP_TASK_MSGLEN];

	message[0] = '\0';

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	seg = dsm_attach(DatumGetUInt32(arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("gp_task job worker could not attach to its shared memory")));
	shared = (GpTaskJobShared *) dsm_segment_address(seg);

	/*
	 * Said here rather than left for the launcher to notice.  A job that ends
	 * quickly is never seen running -- the launcher is asleep until the
	 * worker's exit wakes it, and by then there is no pid to ask for.
	 */
	shared->pid = MyProcPid;

	/*
	 * The command is copied out first: what runs below may error, and the
	 * answer is written back into the segment afterwards.
	 */
	command = pstrdup(shared->command);

	BackgroundWorkerInitializeConnectionByOid(shared->dbid, shared->roleid, 0);

	pgstat_report_appname("gp_task job");
	pgstat_report_activity(STATE_RUNNING, command);

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		SPI_execute(command, false, 0);
		SPI_finish();

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(TopMemoryContext);
		edata = CopyErrorData();
		FlushErrorState();

		failed = true;
		strlcpy(message, edata->message ? edata->message : "the job failed",
				sizeof(message));
		FreeErrorData(edata);

		AbortCurrentTransaction();
	}
	PG_END_TRY();

	/*
	 * The launcher is what writes this job's history, because the tables are
	 * in a database this process is not connected to.  So the answer goes
	 * back in the segment rather than into a row.
	 */
	shared->failed = failed;
	strlcpy(shared->message, message, GP_TASK_MSGLEN);
	shared->done = true;

	pgstat_report_activity(STATE_IDLE, NULL);
	dsm_detach(seg);

	proc_exit(0);
}
