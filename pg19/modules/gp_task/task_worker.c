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
 * A job whose schedule is an interval -- "30 seconds" -- runs by its own
 * clock instead, as Cloudberry's scheduler runs one (pg_cron.c,
 * StartAllPendingRuns and ManageCronTask): its first run comes one interval
 * after the launcher first reads it, each run one interval after the last
 * began, and never two at once -- a run that outlasts its interval is owed
 * one more, which starts as it ends, and then the job is back on its
 * cadence.  So the launcher keeps what it knows of each such job between one
 * look and the next, and looks at least every second, as Cloudberry's does;
 * a trigger on gp_task.job tells it when to read the jobs again, which it
 * otherwise does once a minute.
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
#include "catalog/namespace.h"
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
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
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
	bool		by_interval;	/* started by its interval, not its minute */
} GpTaskRunning;

static List *gp_task_running = NIL;

/*
 * What the launcher knows of a job between one look and the next, as
 * Cloudberry's scheduler keeps a CronTask for each job (task_states.c): when
 * its last run began, whether one is going, and whether one is owed.  Only an
 * interval's is used; a cron job is due by the minute it is.
 */
typedef struct GpTaskState
{
	int64		jobid;			/* the key */
	int			seconds;		/* its interval; 0 for cron's five fields */
	TimestampTz last_start;
	bool		running;
	bool		owed;
	bool		listed;			/* in the list last read */
} GpTaskState;

static HTAB *gp_task_states = NULL;

/* The jobs last read, and whether they are still what the table says. */
static List *gp_task_jobs = NIL;
static MemoryContext gp_task_jobs_cxt = NULL;
static bool gp_task_jobs_valid = false;
static Oid	gp_task_job_relid = InvalidOid;

/* The most the launcher sleeps, as Cloudberry's (pg_cron.c, MaxWait). */
#define GP_TASK_MAX_WAIT_MS		1000

/*
 * A transaction of the launcher's.  A background worker's statement
 * timestamp is set once, as it connects, and now() is that timestamp, so each
 * transaction here sets it first, as PostgreSQL's worker_spi does: the
 * history's start and end times, which are now(), would otherwise all be
 * when the launcher started.
 */
static void
begin_xact(void)
{
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
}

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

/*
 * Start one run of a job: true when a worker was started, false when the run
 * was recorded as failed at once.
 */
static bool
start_job(const GpTaskJob *job, int64 runid, bool by_interval)
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
		return false;
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
		return false;
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
	running->by_interval = by_interval;
	gp_task_running = lappend(gp_task_running, running);

	MemoryContextSwitchTo(oldcxt);

	if (gp_task_log_run)
		ereport(LOG,
				(errmsg("gp_task job " INT64_FORMAT " started: %s",
						job->jobid, job->command)));

	return true;
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
	begin_xact();
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
			begin_xact();
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
			begin_xact();
			PushActiveSnapshot(GetTransactionSnapshot());
			GpTaskRunFinished(running->runid, failed, message);
			PopActiveSnapshot();
			CommitTransactionCommand();
		}

		if (gp_task_log_run)
			ereport(LOG,
					(errmsg("gp_task job " INT64_FORMAT " %s",
							running->jobid, failed ? "failed" : "succeeded")));

		if (running->by_interval && gp_task_states != NULL)
		{
			GpTaskState *state = hash_search(gp_task_states, &running->jobid,
											 HASH_FIND, NULL);

			if (state != NULL)
				state->running = false;
		}

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

/*
 * The trigger on gp_task.job has said the jobs changed -- or everything was
 * invalidated at once, which says so too.  A relcache callback: it only
 * notes it, for the loop to read the jobs again.
 */
static void
jobs_changed(Datum arg, Oid relid)
{
	/*
	 * Until the launcher has seen the table -- the extension made after it
	 * started, or made again -- any relation's invalidation may be it.
	 */
	if (relid == InvalidOid || relid == gp_task_job_relid ||
		!OidIsValid(gp_task_job_relid))
		gp_task_jobs_valid = false;
}

/*
 * Read the jobs again, and bring what the launcher knows of each into line:
 * a job read for the first time starts its interval's clock now, as
 * Cloudberry's does ("the timer for the first run of an interval job starts
 * when pg_cron first learns about the job", task_states.c); one no longer
 * listed -- dropped, or switched off -- is forgotten once no run of it is
 * going.
 */
static void
load_jobs(void)
{
	TimestampTz now = GetCurrentTimestamp();
	HASH_SEQ_STATUS seq;
	GpTaskState *state;
	ListCell   *lc;
	List	   *jobs = NIL;
	Oid			relid = InvalidOid;

	/*
	 * Valid from before the read: a change committed while the jobs are being
	 * read is said to jobs_changed during it or after, and makes them invalid
	 * again, for the next round to read.
	 */
	gp_task_jobs_valid = true;
	MemoryContextReset(gp_task_jobs_cxt);
	gp_task_jobs = NIL;

	begin_xact();

	/*
	 * The jobs outlive the transaction, so they are read into their own
	 * context: starting a transaction switches to its context, which
	 * committing frees.
	 */
	MemoryContextSwitchTo(gp_task_jobs_cxt);
	PushActiveSnapshot(GetTransactionSnapshot());

	if (GpTaskTablesExist())
	{
		gp_task_said_no_tables = false;
		jobs = GpTaskLoadJobs();
		relid = get_relname_relid("job", get_namespace_oid("gp_task", true));
	}
	else if (!gp_task_said_no_tables)
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

	gp_task_jobs = jobs;
	gp_task_job_relid = relid;

	hash_seq_init(&seq, gp_task_states);
	while ((state = hash_seq_search(&seq)) != NULL)
		state->listed = false;

	foreach(lc, gp_task_jobs)
	{
		GpTaskJob  *job = (GpTaskJob *) lfirst(lc);
		bool		found;

		state = hash_search(gp_task_states, &job->jobid, HASH_ENTER, &found);
		if (!found)
		{
			state->last_start = now;
			state->running = false;
			state->owed = false;
		}
		state->seconds = GpTaskScheduleSeconds(job->schedule);
		state->listed = true;
	}

	hash_seq_init(&seq, gp_task_states);
	while ((state = hash_seq_search(&seq)) != NULL)
	{
		if (!state->listed && !state->running)
			(void) hash_search(gp_task_states, &state->jobid, HASH_REMOVE, NULL);
	}
}

/* Start one run of a job, in a transaction of its own. */
static bool
start_run(const GpTaskJob *job, bool by_interval)
{
	bool		started;
	int64		runid;

	begin_xact();
	PushActiveSnapshot(GetTransactionSnapshot());
	runid = GpTaskNextRunId();
	started = start_job(job, runid, by_interval);
	PopActiveSnapshot();
	CommitTransactionCommand();

	return started;
}

/* The jobs of cron's five fields that are due in this minute. */
static void
run_due_jobs(TimestampTz minute)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	cxt = AllocSetContextCreate(TopMemoryContext,
								"gp_task scan",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	foreach(lc, gp_task_jobs)
	{
		GpTaskJob  *job = (GpTaskJob *) lfirst(lc);

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

		(void) start_run(job, false);

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

/*
 * The jobs whose schedule is an interval, as Cloudberry's scheduler runs
 * them: one whose interval has passed since its last run began is owed a
 * run -- once, however long the one going takes -- and a run owed starts
 * when none of the job's is going and there is room among the running jobs,
 * which is gp.task_max_running's; until then it waits, and says nothing, as
 * Cloudberry's CanStartTask does.
 */
static void
run_interval_jobs(TimestampTz now)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	cxt = AllocSetContextCreate(TopMemoryContext,
								"gp_task intervals",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	foreach(lc, gp_task_jobs)
	{
		GpTaskJob  *job = (GpTaskJob *) lfirst(lc);
		GpTaskState *state = hash_search(gp_task_states, &job->jobid,
										 HASH_FIND, NULL);

		if (state == NULL || state->seconds == 0)
			continue;

		if (!state->owed &&
			now >= state->last_start + (TimestampTz) state->seconds * USECS_PER_SEC)
			state->owed = true;

		if (!state->owed || state->running ||
			list_length(gp_task_running) >= gp_task_max_running)
			continue;

		state->owed = false;
		state->last_start = now;
		state->running = start_run(job, true);

		MemoryContextSwitchTo(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * How long the launcher may sleep: until the next minute, or the next run an
 * interval is owed, and a second at most, so that a job written meanwhile is
 * read within one.
 */
static long
sleep_ms(TimestampTz now, TimestampTz minute)
{
	TimestampTz until = Min(minute, now + GP_TASK_MAX_WAIT_MS * INT64CONST(1000));
	HASH_SEQ_STATUS seq;
	GpTaskState *state;

	hash_seq_init(&seq, gp_task_states);
	while ((state = hash_seq_search(&seq)) != NULL)
	{
		if (state->seconds > 0 && !state->running && !state->owed)
			until = Min(until, state->last_start +
						(TimestampTz) state->seconds * USECS_PER_SEC);
	}

	return (until > now) ? (long) ((until - now) / 1000) : 0;
}

void
gp_task_launcher_main(Datum arg)
{
	TimestampTz wake;
	HASHCTL		ctl;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_task_database, NULL, 0);

	/*
	 * A job that was running when the server stopped is not running now, and
	 * its row still says it is.
	 */
	begin_xact();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (GpTaskTablesExist())
		GpTaskMarkRunningAsFailed();
	PopActiveSnapshot();
	CommitTransactionCommand();

	ereport(LOG,
			(errmsg("gp_task scheduler is reading jobs from database \"%s\"",
					gp_task_database)));

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(int64);
	ctl.entrysize = sizeof(GpTaskState);
	gp_task_states = hash_create("gp_task jobs", 64, &ctl,
								 HASH_ELEM | HASH_BLOBS);
	gp_task_jobs_cxt = AllocSetContextCreate(TopMemoryContext,
											 "gp_task jobs",
											 ALLOCSET_DEFAULT_SIZES);
	CacheRegisterRelcacheCallback(jobs_changed, (Datum) 0);

	load_jobs();
	wake = next_minute(GetCurrentTimestamp());

	for (;;)
	{
		TimestampTz now;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 sleep_ms(GetCurrentTimestamp(), wake), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* A worker that finished set the latch; deal with it either way. */
		reap_jobs();

		/* What the trigger on gp_task.job said since, if anything. */
		AcceptInvalidationMessages();

		now = GetCurrentTimestamp();
		if (now >= wake)
		{
			/* The jobs are read once a minute, whatever the trigger said. */
			load_jobs();
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
		else if (!gp_task_jobs_valid)
			load_jobs();

		if (gp_task_enabled)
			run_interval_jobs(GetCurrentTimestamp());
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

	SetCurrentStatementStartTimestamp();
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
