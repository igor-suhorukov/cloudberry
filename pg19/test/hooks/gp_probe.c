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
 * gp_probe.c
 *	  Sets every hook of the core patch series, and records the calls.
 *
 * The vanilla-equivalence suite shows that the series is dormant when nothing
 * sets it.  That is half of what the series claims; this module is the other
 * half.  It sets each hook, drives the code path that should reach it, and
 * lets SQL ask what happened -- so a hook that is never called, or called with
 * the wrong arguments, or called too late to be useful, fails a test rather
 * than being discovered when a module is built on it.
 *
 * It is a test module.  It is not part of the port, and it is installed only
 * where the hook tests run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "replication/syncrep.h"
#include "storage/md.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_probe",
					.version = "1.0"
);

/*
 * What each hook did, since the last reset.  Names are what SQL asks for.
 */
typedef enum ProbeEvent
{
	EV_NEW_OID,
	EV_COMBOCID_CREATE,
	EV_COMBOCID_MISS,
	EV_ANALYZE_SAMPLE,
	EV_EXPLAIN_LABEL,
	EV_MDUNLINK,
	EV_RAW_PARSER,
	EV_STAR_FILTER,
	EV_COUNT
} ProbeEvent;

static const char *const event_name[EV_COUNT] = {
	"new_oid", "combocid_create", "combocid_miss", "analyze_sample",
	"explain_label", "mdunlink", "raw_parser", "star_filter",
};

static int64 calls[EV_COUNT];
static char *last_detail[EV_COUNT];

static void
record(ProbeEvent ev, const char *fmt,...) pg_attribute_printf(2, 3);

static void
record(ProbeEvent ev, const char *fmt,...)
{
	va_list		ap;
	char		buf[256];

	calls[ev]++;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (last_detail[ev])
		pfree(last_detail[ev]);
	last_detail[ev] = MemoryContextStrdup(TopMemoryContext, buf);
}

/* ---- what each hook is armed to do --------------------------------------- */

static Oid	arm_oid_catalog = InvalidOid;	/* fire for this catalog only */
static Oid	arm_oid_value = InvalidOid;		/* and hand out this OID, once */

static Oid	arm_analyze_rel = InvalidOid;
static BlockNumber arm_analyze_pages = 0;
static double arm_analyze_rows = 0;

static Oid	arm_star_rel = InvalidOid;
static Bitmapset *arm_star_cols = NULL;

static bool arm_parser = false;
static bool arm_explain = false;
static bool arm_mdunlink = false;
static bool arm_combocid = false;

/* Combo CIDs this backend published, so the miss hook can answer for them. */
#define PROBE_COMBOCID_MAX	64
static struct
{
	CommandId	cmin;
	CommandId	cmax;
	bool		valid;
}			published_combocid[PROBE_COMBOCID_MAX];

/* the previous hook value, so several modules can coexist */
static planner_hook_type prev_planner_hook = NULL;

/* ------------------------------------------------------------------------- */
/* R1: new_oid_hook                                                          */
/* ------------------------------------------------------------------------- */

static Oid
probe_new_oid(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	Oid			result = InvalidOid;

	if (OidIsValid(arm_oid_catalog) &&
		RelationGetRelid(relation) == arm_oid_catalog)
	{
		result = arm_oid_value;
		/* Fire once: the next allocation for this catalog is ordinary. */
		arm_oid_catalog = InvalidOid;
		arm_oid_value = InvalidOid;
		record(EV_NEW_OID, "catalog %u -> oid %u",
			   RelationGetRelid(relation), result);
	}

	return result;				/* InvalidOid: generate as usual */
}

/* ------------------------------------------------------------------------- */
/* R2: combo command ID hooks                                                */
/* ------------------------------------------------------------------------- */

static void
probe_combocid_create(CommandId combocid, CommandId cmin, CommandId cmax)
{
	if (!arm_combocid)
		return;

	if (combocid < PROBE_COMBOCID_MAX)
	{
		published_combocid[combocid].cmin = cmin;
		published_combocid[combocid].cmax = cmax;
		published_combocid[combocid].valid = true;
	}
	record(EV_COMBOCID_CREATE, "combocid %u = (cmin %u, cmax %u)",
		   combocid, cmin, cmax);
}

static bool
probe_combocid_miss(CommandId combocid, CommandId *cmin, CommandId *cmax)
{
	if (!arm_combocid || combocid >= PROBE_COMBOCID_MAX ||
		!published_combocid[combocid].valid)
		return false;

	*cmin = published_combocid[combocid].cmin;
	*cmax = published_combocid[combocid].cmax;
	record(EV_COMBOCID_MISS, "combocid %u resolved to (cmin %u, cmax %u)",
		   combocid, *cmin, *cmax);
	return true;
}

/* ------------------------------------------------------------------------- */
/* O3: analyze_sample_rows_hook                                              */
/* ------------------------------------------------------------------------- */

/*
 * A sampler that returns no rows but reports a row count of its own, the way
 * a distributed table's sampler reports what the segments hold.  ANALYZE has
 * to believe it: that is what the hook is for.
 */
static int
probe_acquire_sample_rows(Relation relation, int elevel,
						  HeapTuple *rows, int targrows,
						  double *totalrows, double *totaldeadrows)
{
	*totalrows = arm_analyze_rows;
	*totaldeadrows = 0;
	return 0;
}

static bool
probe_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
						  BlockNumber *totalpages)
{
	if (RelationGetRelid(relation) != arm_analyze_rel)
		return false;

	*func = probe_acquire_sample_rows;
	*totalpages = arm_analyze_pages;
	record(EV_ANALYZE_SAMPLE, "relation %u: %u pages, %.0f rows",
		   RelationGetRelid(relation), arm_analyze_pages, arm_analyze_rows);
	return true;
}

/* ------------------------------------------------------------------------- */
/* O4: explain_node_label_hook, over a CustomScan of our own                 */
/* ------------------------------------------------------------------------- */

static Node *probe_create_custom_scan_state(CustomScan *cscan);
static void probe_begin_custom_scan(CustomScanState *node, EState *estate,
									int eflags);
static TupleTableSlot *probe_exec_custom_scan(CustomScanState *node);
static void probe_end_custom_scan(CustomScanState *node);
static void probe_rescan_custom_scan(CustomScanState *node);

static const CustomScanMethods probe_scan_methods = {
	.CustomName = "GpProbe",
	.CreateCustomScanState = probe_create_custom_scan_state,
};

static const CustomExecMethods probe_exec_methods = {
	.CustomName = "GpProbe",
	.BeginCustomScan = probe_begin_custom_scan,
	.ExecCustomScan = probe_exec_custom_scan,
	.EndCustomScan = probe_end_custom_scan,
	.ReScanCustomScan = probe_rescan_custom_scan,
};

static Node *
probe_create_custom_scan_state(CustomScan *cscan)
{
	CustomScanState *css = (CustomScanState *) newNode(sizeof(CustomScanState),
													   T_CustomScanState);

	css->methods = &probe_exec_methods;
	return (Node *) css;
}

static void
probe_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Plan	   *child = (Plan *) linitial(cscan->custom_plans);

	node->custom_ps = list_make1(ExecInitNode(child, estate, eflags));
}

static TupleTableSlot *
probe_exec_custom_scan(CustomScanState *node)
{
	/* A pass-through: the point is the node's presence, not what it does. */
	return ExecProcNode((PlanState *) linitial(node->custom_ps));
}

static void
probe_end_custom_scan(CustomScanState *node)
{
	ExecEndNode((PlanState *) linitial(node->custom_ps));
}

static void
probe_rescan_custom_scan(CustomScanState *node)
{
	ExecReScan((PlanState *) linitial(node->custom_ps));
}

/*
 * Wrap the finished plan in that CustomScan, so that EXPLAIN has one to label.
 * This is the shape the port uses for Motion, which is what O4 exists for.
 */
static PlannedStmt *
probe_planner(Query *parse, const char *query_string, int cursorOptions,
			  ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;
	CustomScan *cscan;
	Plan	   *top;

	if (prev_planner_hook)
		stmt = prev_planner_hook(parse, query_string, cursorOptions,
								 boundParams, es);
	else
		stmt = standard_planner(parse, query_string, cursorOptions,
								boundParams, es);

	if (!arm_explain || stmt->commandType != CMD_SELECT)
		return stmt;

	top = stmt->planTree;

	cscan = makeNode(CustomScan);
	cscan->methods = &probe_scan_methods;
	cscan->custom_plans = list_make1(top);
	cscan->scan.plan.targetlist = top->targetlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = NULL;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.plan.plan_rows = top->plan_rows;
	cscan->scan.plan.plan_width = top->plan_width;
	cscan->scan.plan.startup_cost = top->startup_cost;
	cscan->scan.plan.total_cost = top->total_cost;
	cscan->scan.scanrelid = 0;	/* not a base relation scan */

	stmt->planTree = (Plan *) cscan;
	return stmt;
}

static void
probe_explain_label(PlanState *planstate, ExplainState *es,
					const char **pname, const char **suffix)
{
	CustomScanState *css = (CustomScanState *) planstate;

	if (!IsA(planstate, CustomScanState) || css->methods != &probe_exec_methods)
		return;

	/*
	 * The shape Cloudberry prints: a name of its own, then text between the
	 * name and the costs.
	 */
	*pname = "Probe Motion 3:1";
	*suffix = "  (slice1; segments: 3)";
	record(EV_EXPLAIN_LABEL, "labelled a CustomScan");
}

/* ------------------------------------------------------------------------- */
/* O22: mdunlink_hook                                                        */
/* ------------------------------------------------------------------------- */

static void
probe_mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	if (!arm_mdunlink)
		return;

	record(EV_MDUNLINK, "relfilenumber " UINT64_FORMAT ", fork %d, redo %d",
		   (uint64) rlocator.locator.relNumber, (int) forknum, (int) isRedo);
}

/* ------------------------------------------------------------------------- */
/* O26: raw_parser_hook                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Accept one statement PostgreSQL's grammar cannot parse, and hand everything
 * else back.  That is what the desugaring grammar will do, in miniature: the
 * point is that the hook may return PG19 parse trees for syntax of its own,
 * and that passing a statement through is transparent -- including for
 * PL/pgSQL and type names, which reach raw_parser in other modes.
 */
static List *
probe_raw_parser(const char *str, RawParseMode mode)
{
	const char *skip = str;

	while (*skip == ' ' || *skip == '\n' || *skip == '\t')
		skip++;

	if (arm_parser && pg_strncasecmp(skip, "PROBE ME", 8) == 0)
	{
		record(EV_RAW_PARSER, "accepted \"PROBE ME\" in mode %d", (int) mode);
		return standard_raw_parser("SELECT 'probe_result=probed'::text", mode);
	}

	return standard_raw_parser(str, mode);
}

/* ------------------------------------------------------------------------- */
/* O28: star_expansion_filter_hook                                           */
/* ------------------------------------------------------------------------- */

static Bitmapset *
probe_star_filter(Oid relid)
{
	if (relid != arm_star_rel)
		return NULL;

	record(EV_STAR_FILTER, "relation %u", relid);
	return arm_star_cols;		/* lives in TopMemoryContext */
}

/* ------------------------------------------------------------------------- */
/* SQL interface                                                             */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_probe_reset);
PG_FUNCTION_INFO_V1(gp_probe_calls);
PG_FUNCTION_INFO_V1(gp_probe_detail);
PG_FUNCTION_INFO_V1(gp_probe_arm_new_oid);
PG_FUNCTION_INFO_V1(gp_probe_arm_analyze);
PG_FUNCTION_INFO_V1(gp_probe_arm_star_filter);
PG_FUNCTION_INFO_V1(gp_probe_arm_parser);
PG_FUNCTION_INFO_V1(gp_probe_arm_explain);
PG_FUNCTION_INFO_V1(gp_probe_arm_mdunlink);
PG_FUNCTION_INFO_V1(gp_probe_arm_combocid);
PG_FUNCTION_INFO_V1(gp_probe_published_combocids);
PG_FUNCTION_INFO_V1(gp_probe_load_combocids);
PG_FUNCTION_INFO_V1(gp_probe_current_xids);
PG_FUNCTION_INFO_V1(gp_probe_adopt_xids);
PG_FUNCTION_INFO_V1(gp_probe_matview_maintenance);
PG_FUNCTION_INFO_V1(gp_probe_syncrep_hold);

Datum
gp_probe_reset(PG_FUNCTION_ARGS)
{
	for (int i = 0; i < EV_COUNT; i++)
	{
		calls[i] = 0;
		if (last_detail[i])
		{
			pfree(last_detail[i]);
			last_detail[i] = NULL;
		}
	}
	arm_oid_catalog = arm_oid_value = InvalidOid;
	arm_analyze_rel = InvalidOid;
	arm_star_rel = InvalidOid;
	arm_parser = arm_explain = arm_mdunlink = arm_combocid = false;
	memset(published_combocid, 0, sizeof(published_combocid));
	PG_RETURN_VOID();
}

static int
event_by_name(text *t)
{
	char	   *name = text_to_cstring(t);

	for (int i = 0; i < EV_COUNT; i++)
		if (strcmp(name, event_name[i]) == 0)
			return i;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("no such probe event: %s", name)));
	return -1;					/* keep the compiler quiet */
}

Datum
gp_probe_calls(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(calls[event_by_name(PG_GETARG_TEXT_PP(0))]);
}

Datum
gp_probe_detail(PG_FUNCTION_ARGS)
{
	int			ev = event_by_name(PG_GETARG_TEXT_PP(0));

	if (last_detail[ev] == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(last_detail[ev]));
}

Datum
gp_probe_arm_new_oid(PG_FUNCTION_ARGS)
{
	arm_oid_catalog = PG_GETARG_OID(0);
	arm_oid_value = PG_GETARG_OID(1);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_analyze(PG_FUNCTION_ARGS)
{
	arm_analyze_rel = PG_GETARG_OID(0);
	arm_analyze_pages = (BlockNumber) PG_GETARG_INT64(1);
	arm_analyze_rows = (double) PG_GETARG_INT64(2);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_star_filter(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *elems;
	int			n;
	MemoryContext old;

	deconstruct_array(arr, INT2OID, 2, true, 's', &elems, NULL, &n);

	old = MemoryContextSwitchTo(TopMemoryContext);
	bms_free(arm_star_cols);
	arm_star_cols = NULL;
	for (int i = 0; i < n; i++)
		arm_star_cols = bms_add_member(arm_star_cols,
									   DatumGetInt16(elems[i]));
	MemoryContextSwitchTo(old);

	arm_star_rel = relid;
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_parser(PG_FUNCTION_ARGS)
{
	arm_parser = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_explain(PG_FUNCTION_ARGS)
{
	arm_explain = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_mdunlink(PG_FUNCTION_ARGS)
{
	arm_mdunlink = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_combocid(PG_FUNCTION_ARGS)
{
	arm_combocid = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

/*
 * The combo CIDs this backend created, flattened to (combocid, cmin, cmax)
 * triples, and the other side of the same exchange.
 *
 * A combo CID means nothing outside the backend that made it: the table is
 * per-backend, and index N there is unrelated to index N anywhere else.  So a
 * reader of another backend's uncommitted rows has to be given the mapping,
 * which is what Cloudberry ships in its shared snapshot -- and what the miss
 * hook then answers from.
 */
Datum
gp_probe_published_combocids(PG_FUNCTION_ARGS)
{
	Datum		values[PROBE_COMBOCID_MAX * 3];
	int			n = 0;

	for (int i = 0; i < PROBE_COMBOCID_MAX; i++)
	{
		if (!published_combocid[i].valid)
			continue;
		values[n++] = Int64GetDatum(i);
		values[n++] = Int64GetDatum(published_combocid[i].cmin);
		values[n++] = Int64GetDatum(published_combocid[i].cmax);
	}

	PG_RETURN_ARRAYTYPE_P(construct_array(values, n, INT8OID,
										  sizeof(int64), true, TYPALIGN_DOUBLE));
}

Datum
gp_probe_load_combocids(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *elems;
	int			n;

	deconstruct_array(arr, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE,
					  &elems, NULL, &n);

	if (n % 3 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("combo CID triples expected, got %d values", n)));

	memset(published_combocid, 0, sizeof(published_combocid));
	for (int i = 0; i < n; i += 3)
	{
		int64		idx = DatumGetInt64(elems[i]);

		if (idx < 0 || idx >= PROBE_COMBOCID_MAX)
			continue;
		published_combocid[idx].cmin = (CommandId) DatumGetInt64(elems[i + 1]);
		published_combocid[idx].cmax = (CommandId) DatumGetInt64(elems[i + 2]);
		published_combocid[idx].valid = true;
	}

	PG_RETURN_INT32(n / 3);
}

/*
 * The XIDs this backend holds, for another backend to adopt.  This is what a
 * writer segment process would hand its readers.
 */
Datum
gp_probe_current_xids(PG_FUNCTION_ARGS)
{
	TransactionId top = GetTopTransactionIdIfAny();
	TransactionId cur = GetCurrentTransactionIdIfAny();
	Datum		values[2];
	int			nxids = 0;

	/*
	 * The top XID, and the current subtransaction's if it has one of its own.
	 * A writer with a deeper stack would hand over all of them; two is enough
	 * to show that the array is what the reader consults.
	 */
	if (TransactionIdIsValid(top))
		values[nxids++] = TransactionIdGetDatum(top);
	if (TransactionIdIsValid(cur) && cur != top)
		values[nxids++] = TransactionIdGetDatum(cur);

	PG_RETURN_ARRAYTYPE_P(construct_array(values, nxids, XIDOID,
										  sizeof(TransactionId), true, TYPALIGN_INT));
}

/*
 * Adopt them, which is R2's whole purpose: after this, this backend sees that
 * transaction's uncommitted rows as a parallel worker sees its leader's.
 */
Datum
gp_probe_adopt_xids(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *elems;
	int			n;
	TransactionId *xids;

	deconstruct_array(arr, XIDOID, sizeof(TransactionId), true, TYPALIGN_INT,
					  &elems, NULL, &n);

	xids = palloc(sizeof(TransactionId) * Max(n, 1));
	for (int i = 0; i < n; i++)
		xids[i] = DatumGetTransactionId(elems[i]);

	XactAdoptCurrentXids(n, xids);
	PG_RETURN_VOID();
}

/*
 * O27: maintenance mode, so that an incremental view can be updated with
 * ordinary DML.  Opening and closing are separate calls, because the extension
 * that uses them applies its deltas in between.
 */
Datum
gp_probe_matview_maintenance(PG_FUNCTION_ARGS)
{
	if (PG_GETARG_BOOL(0))
		OpenMatViewIncrementalMaintenanceExternal();
	else if (MatViewIncrementalMaintenanceIsEnabled())
		CloseMatViewIncrementalMaintenanceExternal();
	else
	{
		/*
		 * Closing what was never opened drives the depth below zero, and the
		 * assertion in CloseMatViewIncrementalMaintenance() then takes the
		 * server down.  Core treats pairing as the caller's duty, so refuse
		 * here rather than crash a whole test run over one unbalanced call.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view maintenance mode is not open")));
	}
	PG_RETURN_BOOL(MatViewIncrementalMaintenanceIsEnabled());
}

/* R3: the flag exists and is settable from an extension. */
Datum
gp_probe_syncrep_hold(PG_FUNCTION_ARGS)
{
	SyncRepHoldCancelDuringWait = PG_GETARG_BOOL(0);
	PG_RETURN_BOOL(SyncRepHoldCancelDuringWait);
}

/* ------------------------------------------------------------------------- */

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("gp_probe can only be loaded through \"shared_preload_libraries\"")));

	new_oid_hook = probe_new_oid;
	combocid_create_hook = probe_combocid_create;
	combocid_miss_hook = probe_combocid_miss;
	analyze_sample_rows_hook = probe_analyze_sample_rows;
	explain_node_label_hook = probe_explain_label;
	mdunlink_hook = probe_mdunlink;
	raw_parser_hook = probe_raw_parser;
	star_expansion_filter_hook = probe_star_filter;

	prev_planner_hook = planner_hook;
	planner_hook = probe_planner;

	RegisterCustomScanMethods(&probe_scan_methods);
}
