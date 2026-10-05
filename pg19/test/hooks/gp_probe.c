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

#include <sys/stat.h>

#include "access/heapam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableamext.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "commands/tablespace.h"
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
#include "replication/slot.h"
#include "replication/walsender.h"
#include "storage/lock.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/guc.h"
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
	"raw_parser", "star_filter",
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

/* ------------------------------------------------------------------------- */
/* O32: tablespace_location_hook                                             */
/* ------------------------------------------------------------------------- */

/*
 * A subdirectory of the location, named by gp_probe.tablespace_subdir, as a
 * node of a cluster names one by its dbid; the location as given where the
 * setting is empty.  The setting is read where the hook is asked, so the
 * startup process, replaying CREATE TABLESPACE, reads the value the server
 * was started with.
 */
static char *probe_tablespace_subdir = NULL;

static const char *
probe_tablespace_location(const char *location, Oid tablespaceoid)
{
	char	   *dir;

	if (probe_tablespace_subdir == NULL || probe_tablespace_subdir[0] == '\0')
		return location;
	dir = psprintf("%s/%s", location, probe_tablespace_subdir);
	if (mkdir(dir, S_IRWXU) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));
	return dir;
}

/*
 * As the link goes: the directory it points to removed if it is one of the
 * probe's, named by gp_probe.tablespace_subdir, and never the location
 * itself; each call logged, with the link and whether it is redo's.
 */
static void
probe_tablespace_location_drop(const char *linkloc, Oid tablespaceoid, bool redo)
{
	char		target[MAXPGPATH];
	ssize_t		len = readlink(linkloc, target, sizeof(target) - 1);
	const char *base;

	if (len < 0)
		return;
	target[len] = '\0';
	base = strrchr(target, '/');
	ereport(LOG,
			(errmsg("gp_probe: tablespace %u: %s dropped, linking %s, redo %s",
					tablespaceoid, linkloc, target, redo ? "true" : "false")));
	if (base != NULL && probe_tablespace_subdir != NULL &&
		probe_tablespace_subdir[0] != '\0' &&
		strcmp(base + 1, probe_tablespace_subdir) == 0 &&
		rmdir(target) < 0)
		ereport(redo ? LOG : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m", target)));
}

/* ------------------------------------------------------------------------- */
/* O33: xact_commit_recorded_hook                                            */
/* ------------------------------------------------------------------------- */

/*
 * What the hook was given while armed: how many commits, how many of them
 * with an XID, how many the clog already had committed, and how many were
 * still this backend's own transaction.  And gp_probe.commit_recorded_sleep,
 * milliseconds to wait in the hook, so that another session can look at the
 * transaction meanwhile.
 */
static bool arm_commits = false;
static int	probe_commit_sleep = 0;
static struct
{
	int64		calls;
	int64		with_xid;
	int64		committed;
	int64		current;
}			commits;

static void
probe_commit_recorded(TransactionId latestXid)
{
	if (arm_commits)
	{
		commits.calls++;
		if (TransactionIdIsValid(latestXid))
		{
			commits.with_xid++;
			if (TransactionIdDidCommit(latestXid))
				commits.committed++;
			if (TransactionIdIsCurrentTransactionId(latestXid))
				commits.current++;
		}
	}
	if (probe_commit_sleep > 0)
		pg_usleep(probe_commit_sleep * 1000L);
}

/* ------------------------------------------------------------------------- */
/* O36: physical_slot_restart_lsn_hook                                       */
/* ------------------------------------------------------------------------- */

/*
 * gp_probe.slot_restart_lsn, which the WAL sender of a physical slot reads
 * as its standby replies -- a setting of the server's, since the sender is
 * no session that could arm it: "flushed" passes on what the standby has
 * flushed; "hold" returns the slot's restart_lsn, which leaves the slot
 * where it was; "redo" keeps it at the last checkpoint's redo point once it
 * is behind it, as gp_core does (gp_fts.c).
 */
typedef enum ProbeSlotMode
{
	PROBE_SLOT_FLUSHED,
	PROBE_SLOT_HOLD,
	PROBE_SLOT_REDO,
} ProbeSlotMode;

static int	probe_slot_mode = PROBE_SLOT_FLUSHED;

static const struct config_enum_entry probe_slot_modes[] = {
	{"flushed", PROBE_SLOT_FLUSHED, false},
	{"hold", PROBE_SLOT_HOLD, false},
	{"redo", PROBE_SLOT_REDO, false},
	{NULL, 0, false}
};

static XLogRecPtr
probe_slot_restart_lsn(XLogRecPtr flushed)
{
	XLogRecPtr	restart = MyReplicationSlot->data.restart_lsn;
	XLogRecPtr	redo;

	switch ((ProbeSlotMode) probe_slot_mode)
	{
		case PROBE_SLOT_FLUSHED:
			break;
		case PROBE_SLOT_HOLD:
			if (XLogRecPtrIsValid(restart))
				return restart;
			break;
		case PROBE_SLOT_REDO:
			redo = GetRedoRecPtr();
			if (restart >= redo)
				return restart;
			return Min(redo, flushed);
	}
	return flushed;
}

/* ------------------------------------------------------------------------- */
/* O37: XactAbortAgainAfterEnd                                               */
/* ------------------------------------------------------------------------- */

/*
 * Armed, the probe sets the flag in this backend and raises an error once,
 * in its abort callback, which runs after ProcArrayEndTransaction(): the
 * error has AbortTransaction() run again, and the callback with it, which
 * counts the calls.
 */
static bool arm_abort_error = false;
static int64 abort_calls = 0;

static void
probe_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_ABORT)
		return;
	abort_calls++;
	if (arm_abort_error)
	{
		arm_abort_error = false;
		ereport(ERROR,
				(errmsg("gp_probe: an error in an abort, the transaction ended")));
	}
}

/* ------------------------------------------------------------------------- */
/* O25: memory_block_alloc_hook                                              */
/* ------------------------------------------------------------------------- */

/*
 * What the blocks of every memory context did while armed, in plain
 * variables: the hook runs inside the allocator, and whatever it allocated
 * would come back to it.  A block over arm_block_limit is refused, as a
 * process over its limit would have one refused.
 */
static bool arm_blocks = false;
static Size arm_block_limit = 0;
static struct
{
	int64		taken;			/* new blocks, a context's own first one among them */
	int64		freed;
	int64		resized;		/* realloc()s */
	int64		made;			/* allocations that make a context */
	int64		refused;
	int64		net;			/* bytes taken, less bytes given back */
	int64		aset;			/* new blocks by the type of their context */
	int64		generation;
	int64		slab;
	int64		bump;
	int64		largest;		/* the largest block taken, or grown to */
}			blocks;

static bool
probe_memory_block(MemoryContext context, Size oldsize, Size newsize)
{
	if (!arm_blocks)
		return true;

	if (newsize > oldsize && arm_block_limit > 0 && newsize > arm_block_limit)
	{
		blocks.refused++;
		return false;
	}

	if (oldsize == 0)
	{
		blocks.taken++;
		if (context == NULL)
			blocks.made++;
		else if (IsA(context, AllocSetContext))
			blocks.aset++;
		else if (IsA(context, GenerationContext))
			blocks.generation++;
		else if (IsA(context, SlabContext))
			blocks.slab++;
		else if (IsA(context, BumpContext))
			blocks.bump++;
	}
	else if (newsize == 0)
		blocks.freed++;
	else
		blocks.resized++;

	blocks.net += (int64) newsize - (int64) oldsize;
	if ((int64) newsize > blocks.largest)
		blocks.largest = newsize;
	return true;
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

	memset(&probe_am_ext, 0, sizeof(probe_am_ext));
	probe_am_ext.size = sizeof(TableAmExtRoutine);
	probe_am_ext.reloptions = probe_am_reloptions;
	probe_am_ext.scan_extractcolumns = probe_scan_extractcolumns;
	probe_am_ext.scan_by_column = true;
	probe_am_ext.index_unique_check = probe_index_unique_check;
	probe_am_ext.relation_add_columns = probe_relation_add_columns;
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
PG_FUNCTION_INFO_V1(gp_probe_arm_combocid);
PG_FUNCTION_INFO_V1(gp_probe_published_combocids);
PG_FUNCTION_INFO_V1(gp_probe_load_combocids);
PG_FUNCTION_INFO_V1(gp_probe_current_xids);
PG_FUNCTION_INFO_V1(gp_probe_transaction_state);
PG_FUNCTION_INFO_V1(gp_probe_matview_maintenance);
PG_FUNCTION_INFO_V1(gp_probe_matview_apply_failing);
PG_FUNCTION_INFO_V1(gp_probe_am_handler);
PG_FUNCTION_INFO_V1(gp_probe_am_level);
PG_FUNCTION_INFO_V1(gp_probe_am_fillfactor);
PG_FUNCTION_INFO_V1(gp_probe_scan_log);
PG_FUNCTION_INFO_V1(gp_probe_arm_fetch_fails);
PG_FUNCTION_INFO_V1(gp_probe_arm_block_sequences);
PG_FUNCTION_INFO_V1(gp_probe_arm_blocks);
PG_FUNCTION_INFO_V1(gp_probe_blocks);
PG_FUNCTION_INFO_V1(gp_probe_exercise_context);
PG_FUNCTION_INFO_V1(gp_probe_arm_file_events);
PG_FUNCTION_INFO_V1(gp_probe_arm_extend_fails);
PG_FUNCTION_INFO_V1(gp_probe_file_events);
PG_FUNCTION_INFO_V1(gp_probe_arm_commits);
PG_FUNCTION_INFO_V1(gp_probe_commits);
PG_FUNCTION_INFO_V1(gp_probe_arm_abort_again);
PG_FUNCTION_INFO_V1(gp_probe_abort_calls);

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
	arm_parser = arm_combocid = false;
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

/*
 * What an extension applying a delta really does: open maintenance mode, run
 * code that may fail, and close what it opened either way.  This one always
 * fails, which is the case worth testing -- without the close on the way out,
 * maintenance mode would stay open for the rest of the session.
 */
Datum
gp_probe_matview_apply_failing(PG_FUNCTION_ARGS)
{
	volatile bool opened = false;

	PG_TRY();
	{
		OpenMatViewIncrementalMaintenanceExternal();
		opened = true;
		ereport(ERROR,
				(errcode(ERRCODE_RAISE_EXCEPTION),
				 errmsg("gp_probe: pretending the delta failed")));
		CloseMatViewIncrementalMaintenanceExternal();	/* not reached */
		opened = false;
	}
	PG_CATCH();
	{
		if (opened)
			CloseMatViewIncrementalMaintenanceExternal();
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_VOID();
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

/*
 * O25: count the blocks of every memory context from now on, refusing any
 * new or larger one over "limit" bytes (0: none), or stop counting.
 */
Datum
gp_probe_arm_blocks(PG_FUNCTION_ARGS)
{
	bool		on = PG_GETARG_BOOL(0);

	arm_blocks = false;
	if (on)
		memset(&blocks, 0, sizeof(blocks));
	arm_block_limit = (Size) PG_GETARG_INT64(1);
	arm_blocks = on;
	PG_RETURN_VOID();
}

/* O25: one of the counts since the blocks were armed */
Datum
gp_probe_blocks(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	static const struct
	{
		const char *name;
		int64	   *count;
	}			counts[] = {
		{"taken", &blocks.taken}, {"freed", &blocks.freed},
		{"resized", &blocks.resized}, {"made", &blocks.made},
		{"refused", &blocks.refused}, {"net", &blocks.net},
		{"aset", &blocks.aset}, {"generation", &blocks.generation},
		{"slab", &blocks.slab}, {"bump", &blocks.bump},
		{"largest", &blocks.largest},
	};

	for (int i = 0; i < lengthof(counts); i++)
	{
		if (strcmp(kind, counts[i].name) == 0)
			PG_RETURN_INT64(*counts[i].count);
	}
	elog(ERROR, "unknown block count \"%s\"", kind);
}

/*
 * O25: a context of the kind asked for made, "nchunks" chunks of
 * "chunk_size" bytes taken from it, grown where the kind can grow one in its
 * block (an AllocSet's large chunk, which is realloc()ed), every other one
 * freed where the kind frees one, reset and filled again where it is an
 * AllocSet, and deleted: each path by which the kind takes a block and gives
 * one back.  "first_block" is its first allocation's size where the kind
 * takes one.  Returns the bytes the hook was told of over it all, 0 when
 * every block given back was one it had been told was taken.
 */
Datum
gp_probe_exercise_context(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			chunk_size = PG_GETARG_INT32(1);
	int			nchunks = PG_GETARG_INT32(2);
	int			first_block = PG_GETARG_INT32(3);
	void	  **chunks = palloc(sizeof(void *) * Max(nchunks, 1));
	bool		aset = false;
	bool		frees = true;
	MemoryContext cxt;
	int64		before = blocks.net;
	int64		result;

	/* Not a freelist's default sizes, so that its own block is malloc()ed. */
	if (strcmp(kind, "aset") == 0)
	{
		cxt = AllocSetContextCreate(CurrentMemoryContext, "gp_probe aset",
									first_block, 16 * 1024,
									ALLOCSET_DEFAULT_MAXSIZE);
		aset = true;
	}
	else if (strcmp(kind, "generation") == 0)
		cxt = GenerationContextCreate(CurrentMemoryContext, "gp_probe generation",
									  first_block, 16 * 1024,
									  ALLOCSET_DEFAULT_MAXSIZE);
	else if (strcmp(kind, "slab") == 0)
		cxt = SlabContextCreate(CurrentMemoryContext, "gp_probe slab",
								SLAB_DEFAULT_BLOCK_SIZE, chunk_size);
	else if (strcmp(kind, "bump") == 0)
	{
		cxt = BumpContextCreate(CurrentMemoryContext, "gp_probe bump",
								first_block, 16 * 1024,
								ALLOCSET_DEFAULT_MAXSIZE);
		frees = false;
	}
	else
		elog(ERROR, "unknown memory context kind \"%s\"", kind);

	for (int i = 0; i < nchunks; i++)
		chunks[i] = MemoryContextAlloc(cxt, chunk_size);
	if (aset)
	{
		for (int i = 0; i < nchunks; i++)
			chunks[i] = repalloc(chunks[i], (Size) chunk_size * 2);
	}
	if (frees)
	{
		for (int i = 0; i < nchunks; i += 2)
			pfree(chunks[i]);
	}
	if (aset)
	{
		MemoryContextReset(cxt);
		for (int i = 0; i < nchunks; i++)
			chunks[i] = MemoryContextAlloc(cxt, chunk_size);
	}
	MemoryContextDelete(cxt);

	result = blocks.net - before;
	pfree(chunks);
	PG_RETURN_INT64(result);
}

/* O33: count what the hook is given from now on, or stop counting */
Datum
gp_probe_arm_commits(PG_FUNCTION_ARGS)
{
	arm_commits = PG_GETARG_BOOL(0);
	if (arm_commits)
		memset(&commits, 0, sizeof(commits));
	PG_RETURN_VOID();
}

/* O33: one of the counts: calls, with_xid, committed or current */
Datum
gp_probe_commits(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (strcmp(kind, "calls") == 0)
		PG_RETURN_INT64(commits.calls);
	if (strcmp(kind, "with_xid") == 0)
		PG_RETURN_INT64(commits.with_xid);
	if (strcmp(kind, "committed") == 0)
		PG_RETURN_INT64(commits.committed);
	if (strcmp(kind, "current") == 0)
		PG_RETURN_INT64(commits.current);
	elog(ERROR, "unknown commit count \"%s\"", kind);
}

/*
 * O37: set XactAbortAgainAfterEnd in this backend and raise an error in the
 * next abort's callback, or neither; and count the callback's calls anew
 */
Datum
gp_probe_arm_abort_again(PG_FUNCTION_ARGS)
{
	XactAbortAgainAfterEnd = PG_GETARG_BOOL(0);
	arm_abort_error = XactAbortAgainAfterEnd;
	abort_calls = 0;
	PG_RETURN_VOID();
}

/* O37: the abort callback's calls since it was armed */
Datum
gp_probe_abort_calls(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(abort_calls);
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
	raw_parser_hook = probe_raw_parser;
	star_expansion_filter_hook = probe_star_filter;
	columnref_fallback_hook = probe_columnref_fallback;
	deparse_function_as_column_hook = probe_deparse_as_column;
	query_lockmode_hook = probe_query_lockmode;
	deparse_range_function_hook = probe_deparse_range;
	smgr_file_event_hook = probe_smgr_file_event;

	DefineCustomStringVariable("gp_probe.tablespace_subdir",
							   "O32: the subdirectory of a tablespace's location this server links to.",
							   "Empty: the location as CREATE TABLESPACE gave it.",
							   &probe_tablespace_subdir,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);
	tablespace_location_hook = probe_tablespace_location;
	tablespace_location_drop_hook = probe_tablespace_location_drop;
	memory_block_alloc_hook = probe_memory_block;

	DefineCustomIntVariable("gp_probe.commit_recorded_sleep",
							"O33: milliseconds a commit waits in xact_commit_recorded_hook.",
							NULL,
							&probe_commit_sleep,
							0, 0, 60000,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
	xact_commit_recorded_hook = probe_commit_recorded;

	DefineCustomEnumVariable("gp_probe.slot_restart_lsn",
							 "O36: where a standby's reply moves its physical slot.",
							 "flushed: to what the standby flushed; hold: nowhere; "
							 "redo: to the last checkpoint's redo point, once behind it.",
							 &probe_slot_mode,
							 PROBE_SLOT_FLUSHED,
							 probe_slot_modes,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	physical_slot_restart_lsn_hook = probe_slot_restart_lsn;

	/* O37: the abort callback, which raises an error once armed */
	RegisterXactCallback(probe_xact_callback, NULL);

	/* O13 and the registry's members: the probe's table access method. */
	probe_am_init();

	prev_process_utility = ProcessUtility_hook;
	ProcessUtility_hook = probe_process_utility;
}
