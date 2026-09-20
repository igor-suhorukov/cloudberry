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
 * gp_orca_planner.c
 *	  ORCA behind planner_hook, and the count of what it would not plan.
 *
 * Route A, as decision 1 takes it: ORCA runs from planner_hook, and a query
 * it will not plan falls back to PostgreSQL's planner rather than failing.
 * The fallback is correct, not a workaround -- a plan is a plan -- but at
 * scale it is also the difference between a distributed query and one that
 * gathers everything first, so the decision between Route A alone and also
 * building Route B is to be made at M7 from how often this happens and why.
 *
 * THAT IS WHY THE COUNTERS ARE HERE BEFORE THE TRANSLATOR IS.  Decision 1
 * asks for them "from the first milestone, on real workloads", and numbers
 * that only start being collected once everything works would not answer the
 * question they exist for.  At M1 every query falls back for one reason --
 * there is no translator yet -- and the machinery that will report the
 * interesting reasons is in place and tested around it.
 *
 * WHAT COULD NOT BE PORTED AS WRITTEN.  Cloudberry gates ORCA on two cursor
 * option bits (planner.c:399-403): CURSOR_OPT_PARALLEL_RETRIEVE, which it
 * defines as 0x0400, and CURSOR_OPT_SKIP_FOREIGN_PARTITIONS, 0x1000.
 * PostgreSQL 19 uses 0x0400 for CURSOR_OPT_CUSTOM_PLAN and has nothing at
 * 0x1000.  Testing those bits here would therefore not mean what it means on
 * Cloudberry: it would turn ORCA off for every statement whose plan was
 * forced to be a custom one, which is a common thing and has nothing to do
 * with cursors.  Neither feature exists on the port yet -- parallel retrieve
 * cursors are Track B and foreign partitions Track D -- and when they arrive
 * each needs a signal of its own rather than a bit in a field PostgreSQL
 * owns.  GP_FALLBACK_cursor_option is the reason they will report.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "storage/dsm_registry.h"
#include "utils/guc.h"

#include "cb_compat.h"
#include "gp_orca_api.h"
#include "gp_orca_planner.h"

/* gp.optimizer, and gp.optimizer_trace_fallback. */
bool		gp_optimizer = true;
bool		gp_optimizer_trace_fallback = false;

#define GP_ORCA_COUNTERS_NAME	"gp_orca_counters"

/*
 * What the counters are kept in.
 *
 * Shared, because the question they answer is about a workload rather than a
 * session: a backend's own tally dies with it, and "how often does this
 * application fall back" is asked of the server.  pg_atomic_uint64 rather
 * than a lock, because this is on the planning path of every statement and
 * nothing reads a single counter and a total together.
 */
typedef struct GpOrcaCounters
{
	pg_atomic_uint64 planned;	/* ORCA produced the plan */
	pg_atomic_uint64 fallback[GP_FALLBACK_NREASONS];
} GpOrcaCounters;

static GpOrcaCounters *counters = NULL;
static planner_hook_type prev_planner_hook = NULL;

static const struct
{
	const char *name;
	const char *doc;
}			fallback_reasons[] = {
#define X(key, docstring)	{#key, docstring},
	GP_FALLBACK_KEYS(X)
#undef X
};

static void
counters_init(void *ptr, void *arg)
{
	GpOrcaCounters *c = (GpOrcaCounters *) ptr;

	pg_atomic_init_u64(&c->planned, 0);
	for (int i = 0; i < GP_FALLBACK_NREASONS; i++)
		pg_atomic_init_u64(&c->fallback[i], 0);
}

static void
counters_attach(void)
{
	bool		found;

	if (counters != NULL)
		return;

	counters = GetNamedDSMSegment(GP_ORCA_COUNTERS_NAME,
								  sizeof(GpOrcaCounters),
								  counters_init, &found, NULL);
}

const char *
GpOrcaFallbackReasonName(GpFallbackReason reason)
{
	if (reason < 0 || reason >= GP_FALLBACK_NREASONS)
		return NULL;
	return fallback_reasons[reason].name;
}

const char *
GpOrcaFallbackReasonDoc(GpFallbackReason reason)
{
	if (reason < 0 || reason >= GP_FALLBACK_NREASONS)
		return NULL;
	return fallback_reasons[reason].doc;
}

uint64
GpOrcaFallbackCount(GpFallbackReason reason)
{
	counters_attach();
	if (reason < 0 || reason >= GP_FALLBACK_NREASONS)
		return 0;
	return pg_atomic_read_u64(&counters->fallback[reason]);
}

uint64
GpOrcaPlanCount(void)
{
	counters_attach();
	return pg_atomic_read_u64(&counters->planned);
}

void
GpOrcaResetCounters(void)
{
	counters_attach();
	pg_atomic_write_u64(&counters->planned, 0);
	for (int i = 0; i < GP_FALLBACK_NREASONS; i++)
		pg_atomic_write_u64(&counters->fallback[i], 0);
}

/*
 * Record one fallback, and say so in the log if asked.
 *
 * Cloudberry's optimizer_trace_fallback prints a line per fallback; the port
 * keeps that, because a counter says how often and a log line says which
 * statement.
 */
static void
record_fallback(GpFallbackReason reason)
{
	counters_attach();
	pg_atomic_fetch_add_u64(&counters->fallback[reason], 1);

	if (gp_optimizer_trace_fallback)
		ereport(LOG,
				(errmsg("ORCA did not plan this statement: %s",
						fallback_reasons[reason].doc)));
}

/*
 * Would ORCA be asked at all?
 *
 * These are the conditions Cloudberry checks before calling the optimizer
 * (planner.c:396-403), minus the two cursor-option bits whose meanings
 * PostgreSQL 19 has given to other things; see the file header.
 */
static bool
orca_should_try(Query *parse, int cursorOptions, GpFallbackReason *reason)
{
	if (!gp_optimizer)
	{
		*reason = GP_FALLBACK_disabled;
		return false;
	}

	/*
	 * Only where the whole cluster's plan is made.  A segment process cannot
	 * dispatch a query inside another one, so a plan built there could not be
	 * carried out; on one node the same backend is both, which is what
	 * IS_QD_OR_SINGLENODE asks.
	 */
	if (!IS_QD_OR_SINGLENODE())
	{
		*reason = GP_FALLBACK_not_dispatcher;
		return false;
	}

	/*
	 * ORCA plans queries.  A utility statement reaches the planner only
	 * through the paths that wrap one, and those carry the query inside.
	 */
	if (parse->commandType == CMD_UTILITY)
	{
		*reason = GP_FALLBACK_utility;
		return false;
	}

	return true;
}

/*
 * planner_hook.
 *
 * PostgreSQL 19 passes an ExplainState the hook did not use to get: an
 * extension that plans for itself can register EXPLAIN options and read them
 * here, which is how the LOCUS, SLICETABLE and DXL options will reach ORCA.
 * It is passed straight down for now.
 */
static PlannedStmt *
gp_orca_planner(Query *parse, const char *query_string, int cursorOptions,
				ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *result = NULL;
	GpFallbackReason reason = GP_FALLBACK_no_translator;

	if (orca_should_try(parse, cursorOptions, &reason))
	{
		/*
		 * Where ORCA is asked.  It is not asked yet: the translator that
		 * turns a Query into DXL and DXL back into a PlannedStmt is the next
		 * thing to be built, and until it exists there is nothing to ask.
		 * The reason is recorded rather than assumed, so that the day the
		 * translator lands this counter goes to zero and the interesting ones
		 * start moving.
		 */
		reason = GP_FALLBACK_no_translator;
	}

	if (result == NULL)
	{
		record_fallback(reason);

		if (prev_planner_hook)
			result = prev_planner_hook(parse, query_string, cursorOptions,
									   boundParams, es);
		else
			result = standard_planner(parse, query_string, cursorOptions,
									  boundParams, es);
	}
	else
	{
		counters_attach();
		pg_atomic_fetch_add_u64(&counters->planned, 1);
	}

	return result;
}

void
GpOrcaInstallPlannerHook(void)
{
	DefineCustomBoolVariable("gp.optimizer",
							 "Plan with ORCA where it can.",
							 "A statement ORCA will not plan is planned by "
							 "PostgreSQL instead, and counted; see "
							 "gp_orca.fallbacks().",
							 &gp_optimizer,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.optimizer_trace_fallback",
							 "Log a line for every statement ORCA did not plan.",
							 NULL,
							 &gp_optimizer_trace_fallback,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	prev_planner_hook = planner_hook;
	planner_hook = gp_orca_planner;
}
