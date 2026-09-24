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
 * dynamic.c
 *	  Dynamic tables: a materialized view that refreshes on a schedule.
 *
 * Cloudberry spells this CREATE DYNAMIC TABLE ... SCHEDULE '...', which needs
 * a keyword, a `relisdynamic` column in pg_class and a statement type of its
 * own.  What it actually is, in Cloudberry's own code, is a materialized view
 * with a flag set and a task that runs REFRESH on it -- so here it is a
 * materialized view with an option, a label and a task:
 *
 *	  CREATE MATERIALIZED VIEW dt WITH (gp.dynamic_schedule = '0 * * * *')
 *	    AS SELECT ...
 *
 * `relisdynamic` becomes the `dynamic_schedule` key of the object's `gp`
 * label, which also holds the schedule -- so the label is both the flag and
 * the value Cloudberry keeps in pg_task.  The task is an ordinary gp_task
 * job, named as Cloudberry names it, so a user can see it, change its
 * schedule and read its history with the functions that module already has.
 *
 * Cloudberry makes the task an internal dependency of the view, so that
 * dropping one drops the other.  A job here is a row in another extension's
 * table rather than a catalog object, so the dependency is an
 * object_access_hook instead.
 *
 * Cloudberry's pg_task is a shared catalog, the same from everywhere; these
 * jobs live in the database gp.task_database names.  A dynamic table in any
 * other database has its job written there by gp_task's procedures, through
 * gp_core's loopback, as the statement that made it commits -- in two phases
 * with it on a cluster -- and the job refreshes it where it is.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_matview.h"

/* What Cloudberry calls the task, and what it refreshes on by default. */
#define GP_DYN_TASK_PREFIX		"gp_dynamic_table_refresh_"
#define GP_DYN_DEFAULT_SCHEDULE	"*/5 * * * *"

PG_FUNCTION_INFO_V1(gp_dynamic_schedule);

static char *
dyn_task_name(Oid matviewOid)
{
	return psprintf(GP_DYN_TASK_PREFIX "%u", matviewOid);
}

static ObjectAddress
matview_address(Oid matviewOid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, matviewOid);
	return addr;
}

/*
 * Take WITH (gp.dynamic_schedule [= '...']) out of an option list, so that
 * what is left is something PostgreSQL will accept.  Returns whether it was
 * there, and what it said; written without a schedule it means the default,
 * which is what Cloudberry's SCHEDULE clause means when it is left out.
 */
bool
GpDynTakeOption(List **options, char **schedule)
{
	ListCell   *lc;
	bool		found = false;

	*schedule = NULL;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		char	   *name;

		if (def->defnamespace)
			name = psprintf("%s.%s", def->defnamespace, def->defname);
		else
			name = pstrdup(def->defname);

		if (strcmp(name, GP_DYN_OPTION) == 0)
		{
			found = true;
			*schedule = (def->arg != NULL)
				? pstrdup(defGetString(def))
				: pstrdup(GP_DYN_DEFAULT_SCHEDULE);
			*options = foreach_delete_current(*options, lc);
		}
		pfree(name);
	}

	return found;
}

/*
 * Is gp_task installed here?  A materialized view that refreshes itself needs
 * something to refresh it.
 */
static bool
task_extension_present(void)
{
	return OidIsValid(get_namespace_oid("gp_task", true));
}

/* Is this the database the scheduler reads its jobs from? */
static bool
is_task_database(void)
{
	const char *task_database = GetConfigOption("gp.task_database", true, false);

	return task_database != NULL &&
		strcmp(get_database_name(MyDatabaseId), task_database) == 0;
}

/* The scheduler, which the view's task needs, is loaded. */
static void
check_the_scheduler_is_loaded(void)
{
	if (GetConfigOption("gp.task_database", true, false) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a dynamic table needs the task scheduler"),
				 errhint("Add \"gp_task\" to \"shared_preload_libraries\".")));
}

/*
 * A segment's backend makes the view the coordinator dispatched, and marks
 * it; the task is the coordinator's, which the coordinator has written.
 */
static bool
task_is_elsewhere(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && core->get_role() == GP_ROLE_EXECUTE;
}

static void
run(const char *sql, int expected)
{
	if (SPI_execute(sql, false, 0) != expected)
		elog(ERROR, "gp_matview: %s failed", sql);
}

/*
 * The view exists; mark it and give it the task that keeps it fresh.
 */
void
GpDynAfterCreate(Oid matviewOid, const char *schedule)
{
	ObjectAddress addr = matview_address(matviewOid);
	StringInfoData buf;
	char	   *viewname;

	check_the_scheduler_is_loaded();

	/*
	 * Cloudberry sets pg_class.relisdynamic and keeps the schedule in the
	 * task.  One label says both things, and it is dropped with the view.
	 */
	if (task_is_elsewhere())
	{
		GpLabelSet(&addr, GP_LABEL_dynamic_schedule, schedule);
		return;
	}

	/*
	 * gp_task's procedures write the job, here or in gp.task_database, so
	 * they are what this database needs.
	 */
	if (!task_extension_present())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a dynamic table needs the task scheduler"),
				 errhint("Run \"CREATE EXTENSION gp_task\" first.")));

	GpLabelSet(&addr, GP_LABEL_dynamic_schedule, schedule);
	CommandCounterIncrement();

	viewname = quote_qualified_identifier(get_namespace_name(get_rel_namespace(matviewOid)),
										  get_rel_name(matviewOid));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * gp_task.create_task rather than an INSERT: it is what reads the
	 * schedule, so a schedule nothing can run is refused here rather than
	 * logged every minute afterwards.
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "CALL gp_task.create_task(%s, %s, %s)",
					 quote_literal_cstr(dyn_task_name(matviewOid)),
					 quote_literal_cstr(schedule),
					 quote_literal_cstr(psprintf("REFRESH MATERIALIZED VIEW %s", viewname)));
	run(buf.data, SPI_OK_UTILITY);
	pfree(buf.data);

	SPI_finish();
}

/*
 * A materialized view is being dropped.  Cloudberry records the task as an
 * internal dependency of the view; here the view's row in another extension's
 * table has to be removed by hand.
 *
 * In the task database, asked of every materialized view rather than only of
 * the dynamic ones, since a job left behind would refresh a view that is not
 * there, and dropping a task that was never there is what missing_ok is for.
 * In any other, of a dynamic one only -- the view's label is still there as
 * it goes -- rather than a write to the task database at every DROP.
 */
void
GpDynDropped(Oid matviewOid)
{
	StringInfoData buf;

	if (!task_extension_present() || task_is_elsewhere())
		return;
	if (!is_task_database())
	{
		ObjectAddress addr = matview_address(matviewOid);

		if (GpLabelGet(&addr, GP_LABEL_dynamic_schedule) == NULL)
			return;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "CALL gp_task.drop_task(ARRAY[%s], missing_ok => true)",
					 quote_literal_cstr(dyn_task_name(matviewOid)));
	run(buf.data, SPI_OK_UTILITY);
	pfree(buf.data);

	SPI_finish();
}

/*
 * gp_matview.dynamic_schedule(regclass) -> text
 *
 * What Cloudberry answers with pg_get_dynamic_table_schedule.  NULL for a
 * view that is not dynamic, which is what makes it the flag as well.
 */
Datum
gp_dynamic_schedule(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ObjectAddress addr = matview_address(relid);
	char	   *schedule;

	if (get_rel_relkind(relid) != RELKIND_MATVIEW)
		PG_RETURN_NULL();

	schedule = GpLabelGet(&addr, GP_LABEL_dynamic_schedule);
	if (schedule == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(schedule));
}
