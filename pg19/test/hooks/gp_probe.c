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

#include "access/heapam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableamext.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "replication/syncrep.h"
#include "storage/lock.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

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
	EV_COLUMNREF,
	EV_DEPARSE_COLUMN,
	EV_QUERY_LOCKMODE,
	EV_DEPARSE_RANGE,
	EV_UNIQUE_CHECK,
	EV_ADD_COLUMNS,
	EV_BLOCK_SEQUENCES,
	EV_COUNT
} ProbeEvent;

static const char *const event_name[EV_COUNT] = {
	"new_oid", "combocid_create", "combocid_miss", "analyze_sample",
	"explain_label", "mdunlink", "raw_parser", "star_filter",
	"columnref", "deparse_column", "query_lockmode", "deparse_range",
	"unique_check", "add_columns", "block_sequences",
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

static char *arm_column_name = NULL;	/* O10: this name, where no column */
static Oid	arm_column_func = InvalidOid;	/* has it, is this function's call */

static Oid	arm_range_func = InvalidOid;	/* O31: this function in FROM */
static char *arm_range_text = NULL;	/* prints as this */
static bool arm_range_alias = false;	/* and its alias after it */

/* O30: this relation, when it is written or locked by a clause, in this mode */
static Oid	arm_lockmode_rel = InvalidOid;
static LOCKMODE arm_lockmode = NoLock;

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
/* O30: query_lockmode_hook                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The armed relation, as the target of an UPDATE or a DELETE, or under a
 * locking clause -- named by the query, or brought in by a view -- in the
 * armed mode: what gp_core does with a distributed table.  An INSERT and a
 * plain read keep PostgreSQL's mode.
 */
static LOCKMODE
probe_query_lockmode(Oid relid, LOCKMODE lockmode, AclMode requiredPerms)
{
	if (relid != arm_lockmode_rel ||
		!((requiredPerms & (ACL_UPDATE | ACL_DELETE)) != 0 ||
		  lockmode == RowShareLock))
		return lockmode;

	record(EV_QUERY_LOCKMODE, "%s of %s asked for %s",
		   GetLockmodeName(DEFAULT_LOCKMETHOD, lockmode), get_rel_name(relid),
		   GetLockmodeName(DEFAULT_LOCKMETHOD, arm_lockmode));
	return arm_lockmode;
}

/* ------------------------------------------------------------------------- */
/* O10: columnref_fallback_hook and deparse_function_as_column_hook          */
/* ------------------------------------------------------------------------- */

/*
 * The armed name, where no column has it, is the armed function of the row of
 * the first relation in the query -- what gp_core does with gp_segment_id.
 */
static Node *
probe_columnref_fallback(ParseState *pstate, ColumnRef *cref)
{
	Node	   *last = (Node *) llast(cref->fields);
	Var		   *var;
	FuncExpr   *fexpr;

	if (arm_column_name == NULL || !IsA(last, String) ||
		strcmp(strVal(last), arm_column_name) != 0)
		return NULL;

	foreach_ptr(ParseNamespaceItem, nsitem, pstate->p_namespace)
	{
		if (nsitem->p_rte->rtekind != RTE_RELATION)
			continue;

		record(EV_COLUMNREF, "\"%s\" of %s", arm_column_name,
			   nsitem->p_names->aliasname);
		var = makeWholeRowVar(nsitem->p_rte, nsitem->p_rtindex, 0, true);
		var->location = cref->location;
		markVarForSelectPriv(pstate, var);
		fexpr = makeFuncExpr(arm_column_func, get_func_rettype(arm_column_func),
							 list_make1(var), InvalidOid, InvalidOid,
							 COERCE_EXPLICIT_CALL);
		fexpr->location = cref->location;
		return (Node *) fexpr;
	}
	return NULL;
}

static const char *
probe_deparse_as_column(FuncExpr *expr)
{
	if (arm_column_name == NULL || expr->funcid != arm_column_func)
		return NULL;
	record(EV_DEPARSE_COLUMN, "call of %u printed as \"%s\"",
		   expr->funcid, arm_column_name);
	return arm_column_name;
}

/* ------------------------------------------------------------------------- */
/* O31: deparse_range_function_hook                                          */
/* ------------------------------------------------------------------------- */

/* The armed function in FROM prints as the armed text. */
static const char *
probe_deparse_range(RangeTblEntry *rte, const char *refname, bool *print_alias)
{
	RangeTblFunction *rtfunc = linitial_node(RangeTblFunction, rte->functions);

	if (!OidIsValid(arm_range_func) || !IsA(rtfunc->funcexpr, FuncExpr) ||
		((FuncExpr *) rtfunc->funcexpr)->funcid != arm_range_func)
		return NULL;
	record(EV_DEPARSE_RANGE, "call of %u as \"%s\", %d function, refname %s",
		   arm_range_func, arm_range_text, list_length(rte->functions),
		   refname);
	*print_alias = arm_range_alias;
	return arm_range_text;
}

/* ------------------------------------------------------------------------- */
/* O13: a table access method of the probe's own, and what it registers      */
/* ------------------------------------------------------------------------- */

/*
 * heap's routine, copied: a table of it is a heap table underneath, but its
 * rd_tableam is not heap's, so the registry tells the two apart.
 */
static TableAmRoutine probe_am_routine;
static TableAmExtRoutine probe_am_ext;

/* O14: the method's own option, probe_level, beside heap's */
static relopt_kind probe_relopt_kind;

typedef struct ProbeOwnOptions
{
	int32		vl_len_;
	int			probe_level;
} ProbeOwnOptions;

typedef struct ProbeAmOptions
{
	StdRdOptions std;			/* heap's, first: the core reads them */
	int			probe_level;
} ProbeAmOptions;

/*
 * O14: the options of a table of the probe's method.  Its own go to its own
 * parser and the rest to heap's, each validating when asked, so a name
 * neither knows is refused as heap refuses one.
 */
static bytea *
probe_am_reloptions(Datum reloptions, char relkind, bool validate)
{
	ArrayBuildState *own = NULL;
	ArrayBuildState *heaps = NULL;
	Datum		own_datum = (Datum) 0;
	Datum		heap_datum = (Datum) 0;
	StdRdOptions *std;
	ProbeOwnOptions *mine;
	ProbeAmOptions *result;
	static const relopt_parse_elt tab[] = {
		{"probe_level", RELOPT_TYPE_INT, offsetof(ProbeOwnOptions, probe_level)},
	};

	if (reloptions != (Datum) 0)
	{
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum	   *elems;
		int			nelems;

		deconstruct_array_builtin(array, TEXTOID, &elems, NULL, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			char	   *opt = TextDatumGetCString(elems[i]);

			if (strncmp(opt, "probe_level=", strlen("probe_level=")) == 0)
				own = accumArrayResult(own, elems[i], false, TEXTOID,
									   CurrentMemoryContext);
			else
				heaps = accumArrayResult(heaps, elems[i], false, TEXTOID,
										 CurrentMemoryContext);
		}
		if (own)
			own_datum = makeArrayResult(own, CurrentMemoryContext);
		if (heaps)
			heap_datum = makeArrayResult(heaps, CurrentMemoryContext);
	}

	std = (StdRdOptions *) heap_reloptions(relkind, heap_datum, validate);
	mine = (ProbeOwnOptions *) build_reloptions(own_datum, validate,
												probe_relopt_kind,
												sizeof(ProbeOwnOptions),
												tab, lengthof(tab));

	result = palloc0(sizeof(ProbeAmOptions));
	if (std)
		memcpy(&result->std, std, sizeof(StdRdOptions));
	result->probe_level = mine ? mine->probe_level : 0;
	SET_VARSIZE(result, sizeof(ProbeAmOptions));
	return (bytea *) result;
}

/*
 * heap's own functions, where they scan a table with heap_getnext(), refuse
 * one whose rd_tableam is not heap's: an index build is heap's scan with the
 * relation called heap for its length.
 */
static double
probe_index_build_range_scan(Relation table_rel, Relation index_rel,
							 IndexInfo *index_info, bool allow_sync,
							 bool anyvisible, bool progress,
							 BlockNumber start_blockno, BlockNumber numblocks,
							 IndexBuildCallback callback, void *callback_state,
							 TableScanDesc scan)
{
	const TableAmRoutine *heap = GetHeapamTableAmRoutine();
	double		result;

	table_rel->rd_tableam = heap;
	PG_TRY();
	{
		result = heap->index_build_range_scan(table_rel, index_rel, index_info,
											  allow_sync, anyvisible, progress,
											  start_blockno, numblocks,
											  callback, callback_state, scan);
	}
	PG_FINALLY();
	{
		table_rel->rd_tableam = &probe_am_routine;
	}
	PG_END_TRY();
	return result;
}

static void
probe_index_validate_scan(Relation table_rel, Relation index_rel,
						  IndexInfo *index_info, Snapshot snapshot,
						  ValidateIndexState *state)
{
	const TableAmRoutine *heap = GetHeapamTableAmRoutine();

	table_rel->rd_tableam = heap;
	PG_TRY();
	{
		heap->index_validate_scan(table_rel, index_rel, index_info, snapshot,
								  state);
	}
	PG_FINALLY();
	{
		table_rel->rd_tableam = &probe_am_routine;
	}
	PG_END_TRY();
}

/*
 * O15: each scan of a table of the probe's method that was given its plan
 * node says what kind of scan it is and which columns the node reads, in
 * order, until the next reset.
 */
static StringInfo scan_log = NULL;

static void
probe_scan_extractcolumns(TableScanDesc scan, PlanState *ps)
{
	Scan	   *plan = (Scan *) ps->plan;
	Bitmapset  *cols = NULL;
	const char *kind;
	int			col = -1;
	MemoryContext old;

	pull_varattnos((Node *) plan->plan.targetlist, plan->scanrelid, &cols);
	pull_varattnos((Node *) plan->plan.qual, plan->scanrelid, &cols);
	if (IsA(plan, BitmapHeapScan))
	{
		pull_varattnos((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
					   plan->scanrelid, &cols);
		kind = "bitmap";
	}
	else
		kind = scan->rs_parallel ? "parallel" : "seq";

	old = MemoryContextSwitchTo(TopMemoryContext);
	if (scan_log == NULL)
		scan_log = makeStringInfo();
	if (scan_log->len > 0)
		appendStringInfoChar(scan_log, ';');
	appendStringInfo(scan_log, "%s %s:", kind,
					 RelationGetRelationName(scan->rs_rd));
	while ((col = bms_next_member(cols, col)) >= 0)
		appendStringInfo(scan_log, " %d",
						 col + FirstLowInvalidHeapAttributeNumber);
	MemoryContextSwitchTo(old);
}

/*
 * O16: a unique index's probe, answered by the method: heap's fetch, made
 * here rather than through the core's table_index_fetch_tuple_check().
 * While armed, the method's own index fetch fails, so a probe that went
 * through the core would say so.
 */
static bool arm_fetch_fails = false;

static bool
probe_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
						Snapshot snapshot, TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	if (arm_fetch_fails)
		ereport(ERROR,
				(errmsg("gp_probe: the method's index fetch was called")));
	return GetHeapamTableAmRoutine()->index_fetch_tuple(scan, tid, snapshot,
														slot, call_again,
														all_dead);
}

static bool
probe_index_unique_check(Relation rel, ItemPointer tid, Snapshot snapshot,
						 bool *all_dead)
{
	const TableAmRoutine *heap = GetHeapamTableAmRoutine();
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
									heap->slot_callbacks(rel));
	scan = heap->index_fetch_begin(rel, SO_NONE);
	found = heap->index_fetch_tuple(scan, tid, snapshot, slot, &call_again,
									all_dead);
	heap->index_fetch_end(scan);
	ExecDropSingleTupleTableSlot(slot);

	record(EV_UNIQUE_CHECK, "%s (%u,%u): %s",
		   RelationGetRelationName(rel),
		   ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid),
		   found ? "live" : "not live");
	return found;
}

/*
 * O17: the values of new columns, written without a rewrite.  The probe's
 * tables are heap underneath, which has no place for a column of its own,
 * so each row is updated in place, in the same relfilenumber, with its new
 * values: the defaults first, then the stored generated columns, which may
 * read them, as ATRewriteTable() computes them.  The ALTER holds the table
 * in AccessExclusiveLock, and the rows written here are this command's own,
 * which its snapshot does not see.
 */
static void
probe_relation_add_columns(Relation rel, int ncolumns,
						   const AttrNumber *attnums, Expr *const *exprs,
						   const bool *generated)
{
	TupleDesc	desc = RelationGetDescr(rel);
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState **states = palloc_array(ExprState *, ncolumns);
	TupleTableSlot *oldslot = table_slot_create(rel, NULL);
	TupleTableSlot *newslot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan;
	int64		rows = 0;
	StringInfoData cols;

	initStringInfo(&cols);
	for (int i = 0; i < ncolumns; i++)
	{
		states[i] = ExecPrepareExpr(exprs[i], estate);
		appendStringInfo(&cols, "%s%d%s", i ? "," : "", attnums[i],
						 generated[i] ? "g" : "");
	}

	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, oldslot))
	{
		HeapTuple	tuple;
		TU_UpdateIndexes update_indexes;

		ResetExprContext(econtext);
		slot_getallattrs(oldslot);
		ExecClearTuple(newslot);
		memcpy(newslot->tts_values, oldslot->tts_values,
			   sizeof(Datum) * desc->natts);
		memcpy(newslot->tts_isnull, oldslot->tts_isnull,
			   sizeof(bool) * desc->natts);
		ExecStoreVirtualTuple(newslot);

		econtext->ecxt_scantuple = newslot;
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < ncolumns; i++)
			{
				if (generated[i] != (pass == 1))
					continue;
				newslot->tts_values[attnums[i] - 1] =
					ExecEvalExpr(states[i], econtext,
								 &newslot->tts_isnull[attnums[i] - 1]);
			}

		tuple = heap_form_tuple(desc, newslot->tts_values, newslot->tts_isnull);
		simple_heap_update(rel, &oldslot->tts_tid, tuple, &update_indexes);
		heap_freetuple(tuple);
		rows++;
	}
	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(oldslot);
	ExecDropSingleTupleTableSlot(newslot);
	FreeExecutorState(estate);

	record(EV_ADD_COLUMNS, "%s: columns %s, %lld rows",
		   RelationGetRelationName(rel), cols.data, (long long) rows);
}

/*
 * O19: what the probe's method says its tables take.  While armed, the main
 * fork takes arm_size bytes, whatever its files hold; otherwise it answers
 * as heap's files would, 0 for a fork the table does not have.
 */
static int64 arm_size = -1;

static uint64
probe_relation_size(Relation rel, ForkNumber forknum)
{
	uint64		size = 0;

	for (ForkNumber f = 0; f <= MAX_FORKNUM; f++)
	{
		if (forknum != InvalidForkNumber && f != forknum)
			continue;
		if (f == MAIN_FORKNUM && arm_size >= 0)
			size += arm_size;
		else if (smgrexists(RelationGetSmgr(rel), f))
			size += (uint64) smgrnblocks(RelationGetSmgr(rel), f) * BLCKSZ;
	}
	return size;
}

/*
 * O20: while armed, the method cannot fetch a row by its TID, as a method
 * that stores no row where its TID says cannot: UPDATE, DELETE ...
 * RETURNING and MERGE have to take the old row from the plan.
 */
static bool arm_rowfetch_fails = false;

static bool
probe_tuple_fetch_row_version(Relation rel, ItemPointer tid,
							  Snapshot snapshot, TupleTableSlot *slot)
{
	if (arm_rowfetch_fails)
		ereport(ERROR,
				(errmsg("gp_probe: the method was asked to fetch (%u,%u) by its TID",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid))));
	return GetHeapamTableAmRoutine()->tuple_fetch_row_version(rel, tid,
															  snapshot, slot);
}

/*
 * O18: the runs of block numbers of a table of the probe's method.  Armed
 * for one table, they are what it was told; otherwise one run of every
 * block the table has, which is what BRIN walks for any table.
 */
static Oid	arm_seq_rel = InvalidOid;
static TableAmBlockSequence *arm_seqs = NULL;
static int	arm_nseqs = 0;

static TableAmBlockSequence *
probe_relation_get_block_sequences(Relation rel, int *nseqs)
{
	TableAmBlockSequence *seqs;

	if (RelationGetRelid(rel) == arm_seq_rel)
	{
		seqs = palloc_array(TableAmBlockSequence, Max(arm_nseqs, 1));
		memcpy(seqs, arm_seqs, sizeof(TableAmBlockSequence) * arm_nseqs);
		*nseqs = arm_nseqs;
		record(EV_BLOCK_SEQUENCES, "%s: %d runs",
			   RelationGetRelationName(rel), arm_nseqs);
		return seqs;
	}

	seqs = palloc_object(TableAmBlockSequence);
	seqs[0].startblknum = 0;
	seqs[0].nblocks = RelationGetNumberOfBlocks(rel);
	*nseqs = 1;
	return seqs;
}

/* ------------------------------------------------------------------------- */
/* O21: smgr_file_event_hook                                                 */
/* ------------------------------------------------------------------------- */

/*
 * How many events of each kind each relfilenumber had, while armed.  Kept
 * in a fixed array: a truncation is reported inside a critical section,
 * where nothing may be allocated.
 */
#define PROBE_FILE_EVENTS 1024
static struct
{
	RelFileNumber relnumber;
	SmgrFileEvent event;
	int64		count;
}			file_events[PROBE_FILE_EVENTS];
static int	nfile_events = 0;
static bool arm_file_events = false;
static RelFileNumber arm_extend_fails = InvalidRelFileNumber;

static void
probe_smgr_file_event(RelFileLocatorBackend rlocator, ForkNumber forknum,
					  SmgrFileEvent event)
{
	int			i;

	if (!arm_file_events)
		return;

	for (i = 0; i < nfile_events; i++)
	{
		if (file_events[i].relnumber == rlocator.locator.relNumber &&
			file_events[i].event == event)
			break;
	}
	if (i == nfile_events && nfile_events < PROBE_FILE_EVENTS)
	{
		file_events[i].relnumber = rlocator.locator.relNumber;
		file_events[i].event = event;
		file_events[i].count = 0;
		nfile_events++;
	}
	if (i < PROBE_FILE_EVENTS)
		file_events[i].count++;

	/* As a quota would, where the storage manager itself may raise. */
	if (event == SMGR_FILE_EXTEND && !InRecovery &&
		rlocator.locator.relNumber == arm_extend_fails)
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("gp_probe: relation file %u may not grow",
						rlocator.locator.relNumber)));
}

/* A table of the probe's method keeps its TOAST in a heap table. */
static Oid
probe_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

static void
probe_am_init(void)
{
	probe_relopt_kind = add_reloption_kind();
	add_int_reloption(probe_relopt_kind, "probe_level",
					  "The probe's own table option (O14).",
					  0, 0, 10, AccessExclusiveLock);

	probe_am_routine = *GetHeapamTableAmRoutine();
	probe_am_routine.index_build_range_scan = probe_index_build_range_scan;
	probe_am_routine.index_validate_scan = probe_index_validate_scan;
	probe_am_routine.relation_toast_am = probe_relation_toast_am;
	probe_am_routine.index_fetch_tuple = probe_index_fetch_tuple;
	probe_am_routine.relation_size = probe_relation_size;
	probe_am_routine.tuple_fetch_row_version = probe_tuple_fetch_row_version;

	memset(&probe_am_ext, 0, sizeof(probe_am_ext));
	probe_am_ext.size = sizeof(TableAmExtRoutine);
	probe_am_ext.reloptions = probe_am_reloptions;
	probe_am_ext.scan_extractcolumns = probe_scan_extractcolumns;
	probe_am_ext.scan_by_column = true;
	probe_am_ext.index_unique_check = probe_index_unique_check;
	probe_am_ext.relation_add_columns = probe_relation_add_columns;
	probe_am_ext.size_from_am = true;
	probe_am_ext.old_row_from_plan = true;
	probe_am_ext.relation_get_block_sequences = probe_relation_get_block_sequences;
	RegisterTableAmExtension(&probe_am_routine, &probe_am_ext);
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
PG_FUNCTION_INFO_V1(gp_probe_arm_column);
PG_FUNCTION_INFO_V1(gp_probe_arm_range);
PG_FUNCTION_INFO_V1(gp_probe_arm_lockmode);
PG_FUNCTION_INFO_V1(gp_probe_arm_parser);
PG_FUNCTION_INFO_V1(gp_probe_arm_explain);
PG_FUNCTION_INFO_V1(gp_probe_arm_mdunlink);
PG_FUNCTION_INFO_V1(gp_probe_arm_combocid);
PG_FUNCTION_INFO_V1(gp_probe_published_combocids);
PG_FUNCTION_INFO_V1(gp_probe_load_combocids);
PG_FUNCTION_INFO_V1(gp_probe_current_xids);
PG_FUNCTION_INFO_V1(gp_probe_adopt_xids);
PG_FUNCTION_INFO_V1(gp_probe_transaction_state);
PG_FUNCTION_INFO_V1(gp_probe_matview_maintenance);
PG_FUNCTION_INFO_V1(gp_probe_matview_depth);
PG_FUNCTION_INFO_V1(gp_probe_matview_restore_depth);
PG_FUNCTION_INFO_V1(gp_probe_matview_apply_failing);
PG_FUNCTION_INFO_V1(gp_probe_syncrep_hold);
PG_FUNCTION_INFO_V1(gp_probe_am_handler);
PG_FUNCTION_INFO_V1(gp_probe_am_level);
PG_FUNCTION_INFO_V1(gp_probe_am_fillfactor);
PG_FUNCTION_INFO_V1(gp_probe_scan_log);
PG_FUNCTION_INFO_V1(gp_probe_arm_fetch_fails);
PG_FUNCTION_INFO_V1(gp_probe_arm_size);
PG_FUNCTION_INFO_V1(gp_probe_arm_rowfetch_fails);
PG_FUNCTION_INFO_V1(gp_probe_arm_block_sequences);
PG_FUNCTION_INFO_V1(gp_probe_arm_file_events);
PG_FUNCTION_INFO_V1(gp_probe_arm_extend_fails);
PG_FUNCTION_INFO_V1(gp_probe_file_events);

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
	if (scan_log)
		resetStringInfo(scan_log);
	arm_fetch_fails = false;
	arm_size = -1;
	arm_rowfetch_fails = false;
	arm_star_rel = InvalidOid;
	if (arm_column_name)
		pfree(arm_column_name);
	arm_column_name = NULL;
	arm_column_func = InvalidOid;
	if (arm_range_text)
		pfree(arm_range_text);
	arm_range_text = NULL;
	arm_range_func = InvalidOid;
	arm_range_alias = false;
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

/* O30: the relation, and the mode by its name in pg_locks */
Datum
gp_probe_arm_lockmode(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));

	for (LOCKMODE mode = 1; mode <= MaxLockMode; mode++)
	{
		if (strcmp(GetLockmodeName(DEFAULT_LOCKMETHOD, mode), name) == 0)
		{
			arm_lockmode_rel = PG_GETARG_OID(0);
			arm_lockmode = mode;
			PG_RETURN_VOID();
		}
	}
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("gp_probe: no lock mode is called \"%s\"", name)));
	PG_RETURN_VOID();			/* keep the compiler quiet */
}

Datum
gp_probe_arm_column(PG_FUNCTION_ARGS)
{
	if (arm_column_name)
		pfree(arm_column_name);
	arm_column_name = MemoryContextStrdup(TopMemoryContext,
										  text_to_cstring(PG_GETARG_TEXT_PP(0)));
	arm_column_func = PG_GETARG_OID(1);
	PG_RETURN_VOID();
}

Datum
gp_probe_arm_range(PG_FUNCTION_ARGS)
{
	arm_range_func = PG_GETARG_OID(0);
	if (arm_range_text)
		pfree(arm_range_text);
	arm_range_text = MemoryContextStrdup(TopMemoryContext,
										 text_to_cstring(PG_GETARG_TEXT_PP(1)));
	arm_range_alias = PG_GETARG_BOOL(2);
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
 * R4: this transaction's state as SerializeTransactionState() writes it for a
 * parallel worker -- its XIDs and its command -- for another backend to read
 * as a part of it.
 */
Datum
gp_probe_transaction_state(PG_FUNCTION_ARGS)
{
	Size		len = EstimateTransactionStateSpace();
	char	   *state = palloc(len);	/* aligned, as the struct wants */
	bytea	   *result = palloc(VARHDRSZ + len);

	SerializeTransactionState(len, state);
	SET_VARSIZE(result, VARHDRSZ + len);
	memcpy(VARDATA(result), state, len);
	PG_RETURN_BYTEA_P(result);
}

/*
 * SET LOCAL gp_probe.adopt_state = '<the state, in hex>': R4, before the
 * transaction's first snapshot, which a SET does not take and a function
 * call would.
 */
static ProcessUtility_hook_type prev_process_utility = NULL;

static void
probe_process_utility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, VariableSetStmt) &&
		((VariableSetStmt *) parsetree)->name != NULL &&
		strcmp(((VariableSetStmt *) parsetree)->name, "gp_probe.adopt_state") == 0)
	{
		VariableSetStmt *set = (VariableSetStmt *) parsetree;
		A_Const    *arg;
		char	   *hex;
		char	   *state;
		int			len;

		if (list_length(set->args) != 1 || !IsA(linitial(set->args), A_Const))
			elog(ERROR, "gp_probe.adopt_state takes one string");
		arg = (A_Const *) linitial(set->args);
		hex = strVal(&arg->val);
		if (strncmp(hex, "\\x", 2) == 0)
			hex += 2;
		len = strlen(hex) / 2;
		state = palloc(Max(len, 1));
		hex_decode(hex, len * 2, state);
		XactAdoptTransactionState(state);
		return;
	}

	if (prev_process_utility)
		prev_process_utility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
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

Datum
gp_probe_matview_depth(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MatViewIncrementalMaintenanceDepthExternal());
}

Datum
gp_probe_matview_restore_depth(PG_FUNCTION_ARGS)
{
	RestoreMatViewIncrementalMaintenanceDepthExternal(PG_GETARG_INT32(0));
	PG_RETURN_INT32(MatViewIncrementalMaintenanceDepthExternal());
}

/*
 * What an extension applying a delta really does: open maintenance mode, run
 * code that may fail, and leave the depth where it found it either way.  This
 * one always fails, which is the case worth testing -- without the restore,
 * maintenance mode would stay open for the rest of the session.
 */
Datum
gp_probe_matview_apply_failing(PG_FUNCTION_ARGS)
{
	int			save = MatViewIncrementalMaintenanceDepthExternal();

	PG_TRY();
	{
		OpenMatViewIncrementalMaintenanceExternal();
		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("gp_probe: pretending the delta failed")));
		CloseMatViewIncrementalMaintenanceExternal();	/* not reached */
	}
	PG_CATCH();
	{
		RestoreMatViewIncrementalMaintenanceDepthExternal(save);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}

/* R3: the flag exists and is settable from an extension. */
Datum
gp_probe_syncrep_hold(PG_FUNCTION_ARGS)
{
	SyncRepHoldCancelDuringWait = PG_GETARG_BOOL(0);
	PG_RETURN_BOOL(SyncRepHoldCancelDuringWait);
}

/* ------------------------------------------------------------------------- */

/* O13: the handler of the probe's table access method */
Datum
gp_probe_am_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&probe_am_routine);
}

/* O14: the probe's own option, as the relcache parsed it for a table */
Datum
gp_probe_am_level(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			level = -1;

	if (rel->rd_tableam == &probe_am_routine && rel->rd_options != NULL)
		level = ((ProbeAmOptions *) rel->rd_options)->probe_level;
	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(level);
}

/* O14: heap's fillfactor, as the core reads it from any table's options */
Datum
gp_probe_am_fillfactor(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			fillfactor = RelationGetFillFactor(rel, HEAP_DEFAULT_FILLFACTOR);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(fillfactor);
}

/* O15: what the scans given their plan node said, since the last reset */
Datum
gp_probe_scan_log(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(scan_log ? scan_log->data : ""));
}

/* O16: make the method's index fetch fail, or not */
Datum
gp_probe_arm_fetch_fails(PG_FUNCTION_ARGS)
{
	arm_fetch_fails = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

/* O19: what the method says its tables' main fork takes; -1, its files */
Datum
gp_probe_arm_size(PG_FUNCTION_ARGS)
{
	arm_size = PG_GETARG_INT64(0);
	PG_RETURN_VOID();
}

/* O20: make the method's fetch by TID fail, or not */
Datum
gp_probe_arm_rowfetch_fails(PG_FUNCTION_ARGS)
{
	arm_rowfetch_fails = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}

/*
 * O18: the runs of block numbers the probe's table rel has, as start and
 * length pairs, {start1, length1, start2, length2, ...}.
 */
Datum
gp_probe_arm_block_sequences(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *elems;
	int			nelems;

	deconstruct_array(array, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE,
					  &elems, NULL, &nelems);
	if (nelems % 2 != 0)
		elog(ERROR, "block sequences come in start and length pairs");

	arm_seq_rel = PG_GETARG_OID(0);
	arm_nseqs = nelems / 2;
	arm_seqs = MemoryContextAlloc(TopMemoryContext,
								  sizeof(TableAmBlockSequence) * Max(arm_nseqs, 1));
	for (int i = 0; i < arm_nseqs; i++)
	{
		arm_seqs[i].startblknum = (BlockNumber) DatumGetInt64(elems[2 * i]);
		arm_seqs[i].nblocks = (BlockNumber) DatumGetInt64(elems[2 * i + 1]);
	}
	PG_RETURN_VOID();
}

/* O21: count relations' file events, or stop counting and forget them */
Datum
gp_probe_arm_file_events(PG_FUNCTION_ARGS)
{
	arm_file_events = PG_GETARG_BOOL(0);
	if (!arm_file_events)
		nfile_events = 0;
	PG_RETURN_VOID();
}

/* O21: refuse to let this relfilenumber grow; 0 to let every one */
Datum
gp_probe_arm_extend_fails(PG_FUNCTION_ARGS)
{
	arm_extend_fails = PG_GETARG_OID(0);
	PG_RETURN_VOID();
}

/* O21: how many events of a kind a relfilenumber had while counted */
Datum
gp_probe_file_events(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	RelFileNumber relnumber = PG_GETARG_OID(1);
	SmgrFileEvent event;

	if (strcmp(kind, "create") == 0)
		event = SMGR_FILE_CREATE;
	else if (strcmp(kind, "extend") == 0)
		event = SMGR_FILE_EXTEND;
	else if (strcmp(kind, "truncate") == 0)
		event = SMGR_FILE_TRUNCATE;
	else if (strcmp(kind, "unlink") == 0)
		event = SMGR_FILE_UNLINK;
	else
		elog(ERROR, "unknown file event \"%s\"", kind);

	for (int i = 0; i < nfile_events; i++)
	{
		if (file_events[i].relnumber == relnumber &&
			file_events[i].event == event)
			PG_RETURN_INT64(file_events[i].count);
	}
	PG_RETURN_INT64(0);
}

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
	columnref_fallback_hook = probe_columnref_fallback;
	deparse_function_as_column_hook = probe_deparse_as_column;
	query_lockmode_hook = probe_query_lockmode;
	deparse_range_function_hook = probe_deparse_range;
	smgr_file_event_hook = probe_smgr_file_event;

	/*
	 * O23: entries of a database directory named by a number and "_probe"
	 * are this module's, for pg_checksums to pass over and pg_upgrade to
	 * carry.  The hook tests make one by hand.
	 */
	ExtensionMarkAdd("_probe");

	/* O13 and the registry's members: the probe's table access method. */
	probe_am_init();

	prev_planner_hook = planner_hook;
	planner_hook = probe_planner;

	prev_process_utility = ProcessUtility_hook;
	ProcessUtility_hook = probe_process_utility;

	RegisterCustomScanMethods(&probe_scan_methods);
}
