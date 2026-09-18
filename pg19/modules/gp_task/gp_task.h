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
 * gp_task.h
 *	  The task scheduler, shared between this module's files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_TASK_H
#define GP_TASK_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "nodes/pg_list.h"

/* How much of a failing job's error message is carried back. */
#define GP_TASK_MSGLEN		1024

/*
 * What a running job needs, and what it leaves behind.
 *
 * A job runs in its own database, and the tables that record it live in
 * another one -- gp.task_database -- so the process that runs a job cannot
 * write its own history.  The launcher does that, and this is what passes
 * between them: the launcher fills the first half before starting the worker
 * and reads the second half after it exits.
 */
typedef struct GpTaskJobShared
{
	Oid			dbid;			/* the database to run in */
	Oid			roleid;			/* the role to run as */
	int64		jobid;
	int64		runid;
	int			pid;			/* the worker's, once it is running */
	bool		done;			/* the worker got as far as an answer */
	bool		failed;
	char		message[GP_TASK_MSGLEN];
	char		command[FLEXIBLE_ARRAY_MEMBER];
} GpTaskJobShared;

/* One row of gp_task.job. */
typedef struct GpTaskJob
{
	int64		jobid;
	char	   *jobname;
	char	   *schedule;
	char	   *command;
	char	   *database;
	char	   *username;
} GpTaskJob;

/* gp_task.c */
extern char *gp_task_database;
extern char *gp_task_timezone;
extern bool gp_task_enabled;
extern bool gp_task_log_run;
extern int	gp_task_max_running;

/* task_jobs.c: the tables, through SPI */
extern bool GpTaskTablesExist(void);
extern List *GpTaskLoadJobs(void);
extern int64 GpTaskNextRunId(void);
extern void GpTaskRunStarted(int64 runid, const GpTaskJob *job);
extern void GpTaskRunPid(int64 runid, int pid);
extern void GpTaskRunFinished(int64 runid, bool failed, const char *message);
extern void GpTaskMarkRunningAsFailed(void);

/* task_jobs.c: the schedule, through Cloudberry's cron parser */
extern void GpTaskCheckSchedule(const char *schedule);
extern bool GpTaskScheduleDue(const char *schedule, TimestampTz when);

/* task_worker.c */
extern void GpTaskRegisterLauncher(void);
extern PGDLLEXPORT void gp_task_launcher_main(Datum arg);
extern PGDLLEXPORT void gp_task_job_main(Datum arg);

#endif							/* GP_TASK_H */
