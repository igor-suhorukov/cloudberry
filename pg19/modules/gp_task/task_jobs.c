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
 * task_jobs.c
 *	  The job tables, and the schedules in them.
 *
 * Cloudberry keeps jobs and their history in two shared catalogs, pg_task and
 * pg_task_run_history, reached through syscaches.  An extension cannot create
 * a shared catalog, so these are ordinary tables in one database and they are
 * read through SPI.  A job defined from another database is written here
 * through gp_core's loopback as the statement that defined it commits -- in
 * two phases with it, on a cluster's coordinator (gp_task.forward); what is
 * still lost is the history, which is not visible from the database the job
 * ran in.
 *
 * The schedules are Cloudberry's: cron's five fields, read by Cloudberry's
 * own parser, called where it lies, or an interval of 1 to 59 seconds, read
 * as Cloudberry's scheduler reads it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "task/cron.h"

#include "gp_task.h"

/* ------------------------------------------------------------------------- */
/* Schedules                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's parse_cron_entry answers NULL for anything it cannot read, and
 * frees what it had built.  It allocates with calloc, not palloc, so what it
 * returns is freed with free_entry rather than left to a memory context.
 */
static entry *
parse_schedule(const char *schedule)
{
	char	   *copy = pstrdup(schedule);
	entry	   *parsed = parse_cron_entry(copy);

	pfree(copy);
	return parsed;
}

/*
 * Cloudberry's other kind of schedule: "<n> second" or "<n> seconds", n from
 * 1 to 59, in any case and with spaces around it -- TryParseInterval in
 * src/backend/task/job_metadata.c, which is the scheduler's file rather than
 * the parser's and so is not compiled here.  This is it as it reads there,
 * sscanf and all, so that what one accepts the other does: "5 secondc" and
 * "50 seconds c" are refused, and so is "-1 seconds", which %u reads as a
 * number far past 59.  The answer is the interval, or 0 when the schedule is
 * no interval.
 */
static int
parse_interval(const char *schedule)
{
	unsigned int seconds = 0;
	char		lastChar = '\0';
	char		plural = '\0';
	char		extra = '\0';
	char	   *lower = asc_tolower(schedule, strlen(schedule));
	int			numParts;

	numParts = sscanf(lower, " %u secon%c%c %c", &seconds,
					  &lastChar, &plural, &extra);
	pfree(lower);

	/* no "second" at the end */
	if (lastChar != 'd')
		return 0;

	/* "<n> second", and "<n> seconds" */
	if (numParts == 2 || (numParts == 3 && plural == 's'))
		return (0 < seconds && seconds < 60) ? (int) seconds : 0;

	return 0;
}

/*
 * A schedule is one Cloudberry's ParseSchedule reads: cron's, or else an
 * interval.  Refused in Cloudberry's words.
 */
void
GpTaskCheckSchedule(const char *schedule)
{
	entry	   *parsed = parse_schedule(schedule);

	if (parsed == NULL && parse_interval(schedule) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid schedule: %s", schedule),
				 errhint("Use cron format (e.g. 5 4 * * *), or interval "
						 "format '[1-59] seconds'")));

	if (parsed != NULL)
		free_entry(parsed);
}

/*
 * The interval of a schedule that is one, in seconds; 0 for cron's.
 */
int
GpTaskScheduleSeconds(const char *schedule)
{
	return parse_interval(schedule);
}

/*
 * Is this schedule due in the minute that contains the given time?
 *
 * This is Cloudberry's ShouldRunTask without its wild/non-wild split, which
 * exists there to stagger jobs whose minute and hour are both "*" across a
 * catch-up window.  The launcher here looks at one minute at a time and does
 * not catch up, so every due job is due in the same way.  An interval is
 * never due by the minute: it runs by its own clock (task_worker.c).
 */
bool
GpTaskScheduleDue(const char *schedule, TimestampTz when)
{
	entry	   *parsed = parse_schedule(schedule);
	pg_time_t	when_t;
	struct pg_tm *tm;
	pg_tz	   *tz;
	bool		due;
	int			minute,
				hour,
				dom,
				month,
				dow;

	if (parsed == NULL)
		return false;			/* an interval, or refused when it was written */

	tz = pg_tzset(gp_task_timezone);
	if (tz == NULL)
		tz = log_timezone;

	when_t = timestamptz_to_time_t(when);
	tm = pg_localtime(&when_t, tz);

	minute = tm->tm_min - FIRST_MINUTE;
	hour = tm->tm_hour - FIRST_HOUR;
	dom = tm->tm_mday - FIRST_DOM;
	month = tm->tm_mon + 1 - FIRST_MONTH;
	dow = tm->tm_wday - FIRST_DOW;

	/*
	 * Day of month and day of week are ORed when both are given and ANDed
	 * when either is "*", which is what cron has always done.
	 */
	due = bit_test(parsed->minute, minute) &&
		bit_test(parsed->hour, hour) &&
		bit_test(parsed->month, month) &&
		(((parsed->flags & DOM_STAR) || (parsed->flags & DOW_STAR))
		 ? (bit_test(parsed->dow, dow) && bit_test(parsed->dom, dom))
		 : (bit_test(parsed->dow, dow) || bit_test(parsed->dom, dom)));

	free_entry(parsed);

	return due;
}

/* ------------------------------------------------------------------------- */
/* The tables                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Run one statement and leave its result in SPI_tuptable for the caller.  The
 * caller is inside a transaction and an SPI connection of its own.
 */
static void
run(const char *sql, int expected)
{
	int			ret = SPI_execute(sql, false, 0);

	if (ret != expected)
		elog(ERROR, "gp_task: %s failed (%d)", sql, ret);
}

static char *
column_text(HeapTuple tuple, TupleDesc desc, int col)
{
	bool		isnull;
	Datum		value = SPI_getbinval(tuple, desc, col, &isnull);

	return isnull ? NULL : TextDatumGetCString(value);
}

static int64
column_int8(HeapTuple tuple, TupleDesc desc, int col)
{
	bool		isnull;
	Datum		value = SPI_getbinval(tuple, desc, col, &isnull);

	return isnull ? 0 : DatumGetInt64(value);
}

/*
 * Is this database the one the tables are in?
 *
 * The library can be preloaded without the extension having been created, or
 * created in a database other than the one gp.task_database names.  Both are
 * ordinary mistakes, and the scheduler has to say so once rather than fail
 * every time it looks.
 */
bool
GpTaskTablesExist(void)
{
	bool		exists;
	bool		isnull;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	run("SELECT pg_catalog.to_regclass('gp_task.job') IS NOT NULL", SPI_OK_SELECT);
	exists = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();

	return !isnull && exists;
}

/*
 * Every job that is switched on, in the caller's memory context.
 */
List *
GpTaskLoadJobs(void)
{
	MemoryContext caller = CurrentMemoryContext;
	List	   *jobs = NIL;
	uint64		i;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	run("SELECT jobid, jobname, schedule, command, database, username"
		" FROM gp_task.job WHERE active ORDER BY jobid", SPI_OK_SELECT);

	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tuple = SPI_tuptable->vals[i];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		int64		jobid = column_int8(tuple, desc, 1);
		char	   *jobname = column_text(tuple, desc, 2);
		char	   *schedule = column_text(tuple, desc, 3);
		char	   *command = column_text(tuple, desc, 4);
		char	   *database = column_text(tuple, desc, 5);
		char	   *username = column_text(tuple, desc, 6);
		MemoryContext oldcxt;
		GpTaskJob  *job;

		/* SPI frees what it built at finish, so the job is copied out. */
		oldcxt = MemoryContextSwitchTo(caller);

		job = palloc0(sizeof(GpTaskJob));
		job->jobid = jobid;
		job->jobname = jobname ? pstrdup(jobname) : NULL;
		job->schedule = pstrdup(schedule);
		job->command = pstrdup(command);
		job->database = pstrdup(database);
		job->username = pstrdup(username);
		jobs = lappend(jobs, job);

		MemoryContextSwitchTo(oldcxt);
	}

	SPI_finish();

	return jobs;
}

int64
GpTaskNextRunId(void)
{
	int64		runid;
	bool		isnull;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	run("SELECT pg_catalog.nextval('gp_task.run_history_runid_seq')",
		SPI_OK_SELECT);
	if (SPI_processed != 1)
		elog(ERROR, "gp_task: could not take the next run id");

	runid = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();

	return runid;
}

void
GpTaskRunStarted(int64 runid, const GpTaskJob *job)
{
	StringInfoData buf;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "INSERT INTO gp_task.run_history"
					 " (runid, jobid, database, username, command, status, start_time)"
					 " VALUES (" INT64_FORMAT ", " INT64_FORMAT ", %s, %s, %s, 'running',"
					 " pg_catalog.now())",
					 runid, job->jobid,
					 quote_literal_cstr(job->database),
					 quote_literal_cstr(job->username),
					 quote_literal_cstr(job->command));
	run(buf.data, SPI_OK_INSERT);
	pfree(buf.data);

	SPI_finish();
}

void
GpTaskRunPid(int64 runid, int pid)
{
	StringInfoData buf;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "UPDATE gp_task.run_history SET job_pid = %d"
					 " WHERE runid = " INT64_FORMAT, pid, runid);
	run(buf.data, SPI_OK_UPDATE);
	pfree(buf.data);

	SPI_finish();
}

void
GpTaskRunFinished(int64 runid, bool failed, const char *message)
{
	StringInfoData buf;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "UPDATE gp_task.run_history"
					 " SET status = '%s', end_time = pg_catalog.now(), return_message = %s"
					 " WHERE runid = " INT64_FORMAT,
					 failed ? "failed" : "succeeded",
					 (message != NULL && message[0] != '\0')
					 ? quote_literal_cstr(message) : "NULL",
					 runid);
	run(buf.data, SPI_OK_UPDATE);
	pfree(buf.data);

	SPI_finish();
}

/*
 * A row can say a job is running when nothing is: the server stopped, or the
 * launcher did and was restarted.  Either way the answer is lost, because it
 * was left in a shared memory segment that went with the process that was
 * waiting for it.  The launcher calls this once at startup, as Cloudberry's
 * MarkPendingRunsAsFailed does.
 */
void
GpTaskMarkRunningAsFailed(void)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	run("UPDATE gp_task.run_history"
		" SET status = 'failed', end_time = pg_catalog.now(),"
		"     return_message = 'the scheduler was not running when this job ended'"
		" WHERE status = 'running'", SPI_OK_UPDATE);

	SPI_finish();
}
