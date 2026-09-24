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
 * gp_task.c
 *	  The task scheduler: CREATE TASK and what runs it.
 *
 * Cloudberry's scheduler is pg_cron moved into the server: a launcher that
 * the postmaster starts from a hard-coded list, and job metadata in two
 * shared catalogs.  Neither is available to an extension, and neither needs
 * to be.  Upstream pg_cron is an extension, and this is its shape:
 *
 *	- the launcher is a background worker registered during preload;
 *	- the job tables live in one database, named by gp.task_database, the way
 *	  pg_cron keeps them in cron.database_name.  Shared catalogs are what
 *	  Cloudberry uses to make them the same from every database; here a task
 *	  written in another database is written in that one, through gp_core's
 *	  loopback, as the writing transaction commits;
 *	- a job runs in a background worker of its own.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/task/, src/backend/commands/taskcmds.c
 *
 * The cron expression parser is Cloudberry's own file, compiled where it
 * lies: src/backend/task/entry.c and misc.c, which are Paul Vixie's cron.
 * pg19/compat/task/ is how the build reaches them.  Nothing of the parser is
 * copied or rewritten here.
 *
 * What is not ported is pg_cron's libpq machinery -- its connection state
 * machine is for reaching another node, which is Track B's dispatch rather
 * than this module's business.  Jobs run in background workers, which is
 * Cloudberry's task_use_background_worker path.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_loopback.h"
#include "gp_task.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_task",
					.version = GP_VERSION
);

char	   *gp_task_database = NULL;
char	   *gp_task_timezone = NULL;
bool		gp_task_enabled = true;
bool		gp_task_log_run = false;
int			gp_task_max_running = 5;

PG_FUNCTION_INFO_V1(gp_task_validate_schedule);
PG_FUNCTION_INFO_V1(gp_task_forward);

/*
 * gp_task.validate_schedule(text) -- raise if this is not a schedule.
 *
 * The SQL functions that write the job table call this, so that a schedule
 * nothing can run is refused when the task is created rather than logged
 * every minute afterwards.
 */
Datum
gp_task_validate_schedule(PG_FUNCTION_ARGS)
{
	GpTaskCheckSchedule(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	PG_RETURN_VOID();
}

/*
 * gp_task.forward(procedure text, args text[])
 *
 * CREATE, ALTER or DROP TASK in a database other than gp.task_database: the
 * same procedure, with the same arguments, called there as this transaction
 * commits, through gp_core's loopback -- in two phases with it on a cluster.
 * Only gp_task's own three, each argument quoted here, so that what reaches
 * the other database is one of them and nothing else.
 */
Datum
gp_task_forward(PG_FUNCTION_ARGS)
{
	static const char *const procedures[] = {"create_task", "alter_task", "drop_task"};
	char	   *procedure = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	bool		known = false;
	StringInfoData sql;

	for (int i = 0; i < lengthof(procedures); i++)
		known |= strcmp(procedure, procedures[i]) == 0;
	if (!known)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a procedure of gp_task's", procedure)));

	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(1), TEXTOID, &elems, &nulls, &n);
	initStringInfo(&sql);
	appendStringInfo(&sql, "CALL gp_task.%s(", procedure);
	for (int i = 0; i < n; i++)
		appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "",
						 nulls[i] ? "NULL"
						 : quote_literal_cstr(TextDatumGetCString(elems[i])));
	appendStringInfoChar(&sql, ')');

	GpLoopbackDefer(gp_task_database, sql.data);
	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	/*
	 * A background worker can only be registered while libraries are being
	 * preloaded: later on RegisterBackgroundWorker writes a line to the log
	 * and returns, which would leave the module loaded and nothing running
	 * its jobs.  So this module is preloaded, like the ones that install a
	 * ProcessUtility hook, and says so rather than starting up half working.
	 */
	CB_REQUIRE_PRELOAD("gp_task");
	CB_REQUIRE_CORE("gp_task");

	DefineCustomStringVariable("gp.task_database",
							   "Database holding the task tables.",
							   "The scheduler reads its jobs from this database, "
							   "as pg_cron reads them from cron.database_name. "
							   "A job may run in any database.",
							   &gp_task_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("gp.task_timezone",
							   "Time zone the schedules are read in.",
							   NULL,
							   &gp_task_timezone,
							   "GMT",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.task_enabled",
							 "Whether scheduled tasks run.",
							 "Off leaves the scheduler running but idle, so that "
							 "a task can be defined without being run.",
							 &gp_task_enabled,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.task_log_run",
							 "Log a line as each job starts and finishes.",
							 NULL,
							 &gp_task_log_run,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("gp.task_max_running",
							"How many jobs may run at once.",
							"Each running job holds a background worker slot.",
							&gp_task_max_running,
							5, 1, 1000,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	/*
	 * No MarkGUCPrefixReserved here, for the reason gp_core gives: a prefix is
	 * everything before the first dot, so reserving "gp" would drop the
	 * placeholders of every module that has not been loaded yet.
	 */

	/*
	 * The scheduler belongs to the node that dispatches, or to a node running
	 * on its own.  A segment has one of its own coordinator's jobs to do, not
	 * jobs of its own, so it registers nothing.  By its content id: a
	 * segment's postmaster is no dispatched backend, and its role here would
	 * be a utility session's.
	 */
	if (GpCoreApiLookup()->get_content_id() < 0)
		GpTaskRegisterLauncher();
}
