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
 * ivm_maintain.c
 *	  Keeping an incrementally maintained materialized view up to date.
 *
 * The triggers on the base tables call in here.  Writing to a materialized
 * view is what maintenance means, and the executor refuses that unless
 * maintenance mode is on, which is what O27 exports.
 *
 * The depth is saved and put back rather than closed from a PG_CATCH, because
 * that is what RefreshMatViewByOid does and because nothing lowers the depth
 * when a transaction aborts: a depth left raised would leave DML on
 * materialized views permitted for the rest of the session.
 *
 * What is applied is a delta wherever ivm_delta.c can express one, and a
 * whole recomputation otherwise -- an outer join, an aggregate it does not
 * know how to carry, a TRUNCATE, which leaves no transition tables to work
 * from.  Both leave the view correct after every statement, which is what an
 * incrementally maintained view promises; only the first has the cost of one.
 * The counters below are how a test tells which happened, since the contents
 * of a view do not say.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/matview.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "gp_matview.h"

PG_FUNCTION_INFO_V1(gp_ivm_immediate_before);
PG_FUNCTION_INFO_V1(gp_ivm_immediate_maintenance);

/*
 * How the views have been kept up to date since the counters were reset: by a
 * delta, or by computing the whole view again.  A test can then tell which
 * happened, which the view's contents alone do not say.
 */
static int64 maintained_by_delta = 0;
static int64 maintained_by_recompute = 0;

PG_FUNCTION_INFO_V1(gp_ivm_stats_reset);
PG_FUNCTION_INFO_V1(gp_ivm_stats_delta);
PG_FUNCTION_INFO_V1(gp_ivm_stats_recompute);

Datum
gp_ivm_stats_reset(PG_FUNCTION_ARGS)
{
	maintained_by_delta = maintained_by_recompute = 0;
	PG_RETURN_VOID();
}

Datum
gp_ivm_stats_delta(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(maintained_by_delta);
}

Datum
gp_ivm_stats_recompute(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(maintained_by_recompute);
}

/*
 * Recompute a view from its own definition, writing it with ordinary DML.
 *
 * The definition already carries the hidden columns, because the query was
 * rewritten before the view was created, so reading it back with
 * pg_get_viewdef gives what the view should hold.
 */
static void
gp_ivm_apply(Oid matviewOid)
{
	StringInfoData buf;
	char	   *viewdef;
	char	   *nspname = get_namespace_name(get_rel_namespace(matviewOid));
	char	   *relname = get_rel_name(matviewOid);
	int			ret;

	if (relname == NULL)		/* dropped underneath us */
		return;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT pg_catalog.pg_get_viewdef(%u)", matviewOid);
	ret = SPI_execute(buf.data, true, 0);
	if (ret != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "could not read the definition of materialized view %u",
			 matviewOid);
	viewdef = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	if (viewdef == NULL)
		elog(ERROR, "materialized view %u has no definition", matviewOid);

	/* pg_get_viewdef ends the definition with a semicolon. */
	{
		int			len = strlen(viewdef);

		while (len > 0 && (viewdef[len - 1] == ';' || viewdef[len - 1] == '\n' ||
						   viewdef[len - 1] == ' '))
			viewdef[--len] = '\0';
	}

	resetStringInfo(&buf);
	appendStringInfo(&buf, "DELETE FROM %s.%s",
					 quote_identifier(nspname), quote_identifier(relname));
	if (SPI_execute(buf.data, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not empty materialized view %s.%s", nspname, relname);

	resetStringInfo(&buf);
	appendStringInfo(&buf, "INSERT INTO %s.%s %s",
					 quote_identifier(nspname), quote_identifier(relname),
					 viewdef);
	if (SPI_execute(buf.data, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not refill materialized view %s.%s", nspname, relname);

	pfree(buf.data);
	SPI_finish();
}

static Oid
matview_from_trigger_args(TriggerData *trigdata, const char *caller)
{
	if (trigdata->tg_trigger->tgnargs < 1)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s must be told which materialized view it maintains",
						caller)));

	return (Oid) strtoul(trigdata->tg_trigger->tgargs[0], NULL, 10);
}

/*
 * BEFORE: take the snapshot that says what the base tables held before this
 * statement, and start counting the triggers it fires for this view.  A delta
 * over more than one table is taken against that snapshot; ivm_state.c says
 * why.  The trigger also forces the base table to be locked before the
 * statement runs.
 */
Datum
gp_ivm_immediate_before(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "gp_ivm_immediate_before is a trigger function");

	GpIvmEntryBefore(matview_from_trigger_args(trigdata,
											   "gp_ivm_immediate_before"));

	return PointerGetDatum(NULL);
}

/*
 * AFTER: collect what this trigger changed, and once the statement has fired
 * the last of them, bring the view up to date.
 *
 * Waiting for the last one is what lets a statement that writes two of a
 * view's base tables -- which a data-modifying CTE does, and MERGE does by
 * firing three triggers -- be accounted for once, as one delta per table.
 */
Datum
gp_ivm_immediate_maintenance(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Oid			matviewOid;
	IvmEntry   *entry;
	bool		is_last;
	int			save_depth;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "gp_ivm_immediate_maintenance is a trigger function");

	matviewOid = matview_from_trigger_args(trigdata, "gp_ivm_immediate_maintenance");

	entry = GpIvmEntryAfter(matviewOid, trigdata, &is_last);
	if (!is_last)
		return PointerGetDatum(NULL);

	/*
	 * What follows reads the base tables as the statement left them -- a
	 * table whose delta has been taken is read again for the next one -- so
	 * the statement's own writes have to be visible first.
	 */
	CommandCounterIncrement();
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/*
	 * Writing to a materialized view needs maintenance mode.  Remember the
	 * depth and put it back on error: closing from the PG_CATCH would be
	 * wrong if the error came before the open, and nothing lowers the depth
	 * when the transaction aborts.
	 */
	save_depth = MatViewIncrementalMaintenanceDepthExternal();

	PG_TRY();
	{
		OpenMatViewIncrementalMaintenanceExternal();

		/*
		 * A delta if this view's shape allows one, and the whole view
		 * otherwise -- both leave the view correct, and only the first is
		 * incremental.
		 */
		if (GpIvmApplyDelta(entry))
			maintained_by_delta++;
		else
		{
			gp_ivm_apply(matviewOid);
			maintained_by_recompute++;
		}

		CloseMatViewIncrementalMaintenanceExternal();
	}
	PG_CATCH();
	{
		RestoreMatViewIncrementalMaintenanceDepthExternal(save_depth);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PopActiveSnapshot();
	GpIvmEntryForget(entry);

	return PointerGetDatum(NULL);
}

void
GpIvmRefresh(Oid matviewOid)
{
	int			save_depth = MatViewIncrementalMaintenanceDepthExternal();

	PG_TRY();
	{
		OpenMatViewIncrementalMaintenanceExternal();
		gp_ivm_apply(matviewOid);
		CloseMatViewIncrementalMaintenanceExternal();
	}
	PG_CATCH();
	{
		RestoreMatViewIncrementalMaintenanceDepthExternal(save_depth);
		PG_RE_THROW();
	}
	PG_END_TRY();
}
