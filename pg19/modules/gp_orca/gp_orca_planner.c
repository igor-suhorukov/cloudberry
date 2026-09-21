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
 * THAT IS WHY THE COUNTERS CAME BEFORE THE TRANSLATOR DID.  Decision 1 asks
 * for them "from the first milestone, on real workloads", and numbers that
 * only start being collected once everything works would not answer the
 * question they exist for.  Until the translator's first group of operators
 * landed every query fell back for one reason, that there was no translator;
 * now ORCA is asked, and a fallback is counted as ORCA declining the query or
 * as ORCA failing on it, with ORCA's own reason in the trace.
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

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "storage/dsm_registry.h"
#include "utils/guc.h"

#include "cb_assertop.h"
#include "cb_compat.h"
#include "cb_dynamicscan.h"
#include "gp_orca_api.h"
#include "gp_orca_planner.h"
#include "optimizer/orca.h"

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
static explain_per_plan_hook_type prev_explain_per_plan_hook = NULL;
static explain_node_label_hook_type prev_explain_node_label_hook = NULL;

/*
 * How a plan says ORCA made it: an entry in PlannedStmt.extension_state,
 * which PostgreSQL 19 provides for exactly this -- one DefElem per extension,
 * named after it, whose argument survives copyObject(), so the mark stays
 * with a plan the plan cache keeps.  Cloudberry has PlannedStmt.planGen for
 * it, which PostgreSQL 19 does not.
 */
#define GP_ORCA_PLAN_MARK	"gp_orca"

static bool
planned_by_orca(PlannedStmt *plannedstmt)
{
	ListCell   *lc;

	foreach(lc, plannedstmt->extension_state)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (strcmp(def->defname, GP_ORCA_PLAN_MARK) == 0)
			return true;
	}
	return false;
}

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
 * Record one fallback, and say so if asked.
 *
 * Cloudberry's optimizer_trace_fallback reports, to the client, each
 * statement ORCA was asked to plan and did not, with ORCA's reason; the port
 * keeps that, word for word, because a counter says how often and the
 * report says which statement and why.  A statement ORCA was not asked about
 * at all -- gp.optimizer off, a segment, a utility statement -- is counted
 * and not reported, as Cloudberry does not report it either.
 */
static void
record_fallback(GpFallbackReason reason, const char *detail)
{
	counters_attach();
	pg_atomic_fetch_add_u64(&counters->fallback[reason], 1);

	if (gp_optimizer_trace_fallback &&
		(reason == GP_FALLBACK_declined || reason == GP_FALLBACK_error))
		ereport(INFO,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GPORCA failed to produce a plan, falling back to Postgres-based planner"),
				 detail ? errdetail("%s", detail) : 0));
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
	GpFallbackReason reason = GP_FALLBACK_declined;
	GpOrcaFailure failure = {false, false, NULL};

	if (orca_should_try(parse, cursorOptions, &reason))
	{
		/*
		 * What orcaopt.h says the port decides for itself: no vectorised
		 * plan, which needs an executor the open-source tree does not have,
		 * and no parallel one, which decision 2 defers until after M7.
		 */
		OptimizerOptions options = {false, false};

		result = optimize_query(parse, cursorOptions, boundParams, &options,
								&failure);
		if (result == NULL)
			reason = failure.unexpected ? GP_FALLBACK_error
				: GP_FALLBACK_declined;
	}

	if (result == NULL)
	{
		record_fallback(reason, failure.message);

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

		result->extension_state =
			lappend(result->extension_state,
					makeDefElem(pstrdup(GP_ORCA_PLAN_MARK),
								(Node *) makeString(pstrdup("GPORCA")), -1));
	}

	return result;
}

/*
 * explain_per_plan_hook: which optimizer made the plan.
 *
 * Cloudberry's EXPLAIN ends every text-format plan with this line, and its
 * expected test output is full of it, so the port prints it the same way and
 * in the same words.  PostgreSQL 19 lets an extension add to EXPLAIN only
 * here, after the planning time where Cloudberry prints it before; in the
 * form its tests read -- no summary -- the line is last either way.
 */
static void
gp_orca_explain_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
						 ExplainState *es, const char *queryString,
						 ParamListInfo params, QueryEnvironment *queryEnv)
{
	if (prev_explain_per_plan_hook)
		prev_explain_per_plan_hook(plannedstmt, into, es, queryString,
								   params, queryEnv);

	if (es->format == EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("Optimizer",
							planned_by_orca(plannedstmt) ?
							"GPORCA" : "Postgres query optimizer",
							es);
}

/*
 * explain_node_label_hook, which is O4: what the nodes of ORCA's plans are
 * called.
 *
 * Cloudberry's executor has Assert, the dynamic scans and the Partition
 * Selector as nodes of its own.  PostgreSQL 19's does not, and a module
 * cannot add a node type, so here each is a CustomScan -- which EXPLAIN would
 * print as "Custom Scan (Assert)".  Cloudberry's users know them by the names
 * its EXPLAIN gives them, and its expected test output is written in those
 * names, so the port prints them the same way.  Each node's own file says
 * what it is called; this asks them in turn.  Only text output prints a
 * node's name: other formats keep "Custom Scan", with the plan provider's
 * name beside it.
 */
static void
gp_orca_explain_node_label(PlanState *planstate, ExplainState *es,
						   const char **pname, const char **suffix)
{
	Assert(IsA(planstate->plan, CustomScan));

	if (gp_orca_label_assert(planstate, es, pname, suffix) ||
		gp_orca_label_dynamic_scans(planstate, es, pname, suffix))
		return;

	if (prev_explain_node_label_hook)
		prev_explain_node_label_hook(planstate, es, pname, suffix);
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
							 "Report each statement ORCA was asked to plan and did not, and why.",
							 NULL,
							 &gp_optimizer_trace_fallback,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	prev_planner_hook = planner_hook;
	planner_hook = gp_orca_planner;

	prev_explain_per_plan_hook = explain_per_plan_hook;
	explain_per_plan_hook = gp_orca_explain_per_plan;

	prev_explain_node_label_hook = explain_node_label_hook;
	explain_node_label_hook = gp_orca_explain_node_label;
}
