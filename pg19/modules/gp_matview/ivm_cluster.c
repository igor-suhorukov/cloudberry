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
 * ivm_cluster.c
 *	  Incremental materialized views on a cluster: the coordinator keeps them
 *	  up to date, from what the segments' triggers kept.
 *
 * On a cluster a base table's rows are written on the segments -- by the
 * COPY gp_core routes an INSERT's rows with, an UPDATE or DELETE sent to
 * each segment, the explicit write's statements, ORCA's slices -- and the
 * view's rows are on the segments too, by the view's own distribution.  The
 * view's triggers fire where the rows are written, each statement on each
 * segment firing them for that segment's rows, and keep the transition tables
 * they are handed (ivm_state.c).  Once the coordinator's statement is over
 * -- its executor finished, or its COPY or TRUNCATE done -- the coordinator
 * maintains each incremental view over a table it wrote:
 *
 *	1. it asks every segment what it kept for the view, and takes the rows:
 *	   gp_matview.ivm_stash() and gp_matview.ivm_take(), together with what
 *	   its own triggers kept, a TRUNCATE's;
 *	2. it computes the deltas as one node does (ivm_delta.c), its queries
 *	   reading the transition tables here and the other base tables from the
 *	   segments, as the planner's route reads a distributed table;
 *	3. it sends each segment the delta rows its rows of the view are in --
 *	   by the view's distribution key, which is why the key has to be among
 *	   the columns a view row is found by (GpIvmClusterCheckDistribution()),
 *	   or every row to every segment of a replicated view --
 *	   gp_matview.ivm_stage(); and
 *	4. each segment applies them to its rows with the statements one node
 *	   applies its own with, under the view's maintenance, as its owner:
 *	   gp_matview.ivm_apply().
 *
 * What a delta cannot express is recomputed: the view's query, run here, its
 * rows sent to the segments in place of theirs.  Either way the view is right
 * when the statement is over, as one node's is.
 *
 * The segments' functions are the coordinator's alone: each is refused unless
 * it is the statement the coordinator's maintenance sent, as it sent it --
 * the connection is the coordinator's, carrying the cluster's secret, and
 * the statement's text is the one this file writes for those arguments.  A
 * user's query a segment runs -- ORCA's slices, an UPDATE sent as written --
 * could otherwise call one with rows of its own, into a view it cannot write.
 *
 * Cloudberry's maintenance on a cluster is the same algebra, carried by
 * machinery of its own in the core: the QD exports the snapshot a view's
 * BEFORE trigger took to every QE (pg_export_snapshot_def), which imports it
 * there (ivm_import_snapshot, AddPreassignedMVEntry, from the dispatched
 * plan); each QE's transition tables are made shared and given a name
 * (SetTransitionTableName, tuplestore_make_sharedV2) that the QD's delta
 * queries -- dispatched plans -- read them by; and each delta is written into
 * a tuplestore on the segments by the view's policy (makeIvmIntoClause), which
 * the apply statements, dispatched, read.  None of that is open to an
 * extension, so the rows travel instead: the transition tables to the
 * coordinator, the deltas back.  The pre-update state a view over several
 * changed tables needs is built from rows here too (ivm_delta.c).
 *
 * Cloudberry sources this file stands in for:
 *	  the MPP half of src/backend/commands/matview.c's incremental
 *	  maintenance: ivm_export_snapshot(), ivm_import_snapshot(),
 *	  AddPreassignedMVEntry(), register_delta_ENRs() and apply_delta() as
 *	  dispatched, and trigger.c's SetTransitionTableName()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/matview.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_matview.h"
#include "gp_policy.h"

PG_FUNCTION_INFO_V1(gp_ivm_make_triggers);
PG_FUNCTION_INFO_V1(gp_ivm_stash);
PG_FUNCTION_INFO_V1(gp_ivm_take);
PG_FUNCTION_INFO_V1(gp_ivm_stage);
PG_FUNCTION_INFO_V1(gp_ivm_apply);

/* How many delta rows go to a segment in one gp_matview.ivm_stage(). */
#define IVM_STAGE_ROWS		2048

/* The statements the coordinator sends, which the segments check they are. */
#define SQL_MAKE_TRIGGERS	"SELECT gp_matview.ivm_make_triggers(%u)"
#define SQL_STASH			"SELECT relid, old_rows, new_rows, truncated FROM gp_matview.ivm_stash(%u)"
#define SQL_TAKE			"SELECT * FROM gp_matview.ivm_take(%u, %u, %s, NULL::%s)"
#define SQL_STAGE			"SELECT gp_matview.ivm_stage($1, $2, $3)"
#define SQL_APPLY			"SELECT gp_matview.ivm_apply($1, $2)"

/* The coordinator is maintaining a view: its own queries start none. */
static int	maintaining = 0;

/* ------------------------------------------------------------------------- */
/* Which views a statement reached                                           */
/* ------------------------------------------------------------------------- */

/* Is this one of the functions this module's triggers call? */
static bool
is_ivm_trigger_function(Oid funcid)
{
	Oid			nsp = get_namespace_oid("gp_matview", true);
	char	   *name;
	bool		result;

	if (!OidIsValid(nsp) || get_func_namespace(funcid) != nsp)
		return false;
	name = get_func_name(funcid);
	result = name != NULL &&
		(strcmp(name, "ivm_immediate_maintenance") == 0 ||
		 strcmp(name, "ivm_immediate_before") == 0);
	return result;
}

/*
 * The incremental views a relation's triggers keep up to date, added to
 * "views": the matview each of this module's internal triggers names.
 */
static List *
views_of_relation(Oid relid, List *views)
{
	Relation	rel;
	TriggerDesc *td;

	if (get_rel_relkind(relid) != RELKIND_RELATION)
		return views;

	/* the statement that wrote it holds its lock */
	rel = table_open(relid, NoLock);
	td = rel->trigdesc;
	for (int i = 0; td != NULL && i < td->numtriggers; i++)
	{
		Trigger    *trig = &td->triggers[i];

		if (trig->tgisinternal && trig->tgnargs >= 1 &&
			is_ivm_trigger_function(trig->tgfoid))
			views = list_append_unique_oid(views,
										   (Oid) strtoul(trig->tgargs[0], NULL, 10));
	}
	table_close(rel, NoLock);

	return views;
}

/* ------------------------------------------------------------------------- */
/* The coordinator: the rows the segments kept                               */
/* ------------------------------------------------------------------------- */

/* One base table's rows a segment kept, by how many. */
typedef struct KeptTable
{
	Oid			relid;
	int64		old_rows;
	int64		new_rows;
} KeptTable;

/*
 * What the segments kept for a view, and what this node's own triggers did,
 * as one entry: the base tables it names, and whether a TRUNCATE was among
 * the statements.  "kept" is given how many rows of each table the segments
 * have, which fetch_rows() takes once the delta is known to need them.
 */
static IvmEntry *
collect(Oid matviewOid, List **kept)
{
	IvmEntry   *entry = GpIvmEntryTake(matviewOid);
	TupleDesc	desc;
	Tuplestorestate *store;
	TupleTableSlot *slot;
	int			nsegs = GpCoreApiLookup()->get_segment_count();
	uint64	   *counts = palloc0_array(uint64, nsegs);

	if (entry == NULL)
		entry = GpIvmEntryMake(matviewOid);

	desc = CreateTemplateTupleDesc(4);
	TupleDescInitEntry(desc, 1, "relid", OIDOID, -1, 0);
	TupleDescInitEntry(desc, 2, "old_rows", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 3, "new_rows", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 4, "truncated", BOOLOID, -1, 0);
	TupleDescFinalize(desc);

	store = tuplestore_begin_heap(false, false, work_mem);
	GpDispatchWriteReturning(psprintf(SQL_STASH, matviewOid), -1, NULL, 0,
							 desc, store, false, counts);

	slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	*kept = NIL;
	while (tuplestore_gettupleslot(store, true, false, slot))
	{
		Oid			relid;
		KeptTable  *k = NULL;
		ListCell   *lc;
		bool		isnull;
		Relation	rel;

		relid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
		if (DatumGetBool(slot_getattr(slot, 4, &isnull)))
			entry->truncated = true;

		/* a table dropped since cannot have changed what the view holds */
		rel = try_table_open(relid, AccessShareLock);
		if (rel == NULL)
			continue;
		(void) GpIvmEntryTable(entry, rel);
		table_close(rel, NoLock);

		foreach(lc, *kept)
			if (((KeptTable *) lfirst(lc))->relid == relid)
				k = (KeptTable *) lfirst(lc);
		if (k == NULL)
		{
			k = palloc0_object(KeptTable);
			k->relid = relid;
			*kept = lappend(*kept, k);
		}
		k->old_rows += DatumGetInt64(slot_getattr(slot, 2, &isnull));
		k->new_rows += DatumGetInt64(slot_getattr(slot, 3, &isnull));
	}
	ExecDropSingleTupleTableSlot(slot);
	tuplestore_end(store);

	return entry;
}

/* Did anything change: a row kept here or on a segment, or a TRUNCATE? */
static bool
anything_changed(IvmEntry *entry, List *kept)
{
	ListCell   *lc;

	if (entry->truncated)
		return true;
	foreach(lc, kept)
	{
		KeptTable  *k = (KeptTable *) lfirst(lc);

		if (k->old_rows > 0 || k->new_rows > 0)
			return true;
	}
	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, table->old_stores)
			if (tuplestore_tuple_count(((IvmTransition *) lfirst(lc2))->store) > 0)
				return true;
		foreach(lc2, table->new_stores)
			if (tuplestore_tuple_count(((IvmTransition *) lfirst(lc2))->store) > 0)
				return true;
	}
	return false;
}

/* The statement gp_matview.ivm_take() is sent as, for these arguments. */
static char *
take_sql(Oid matviewOid, Oid relid, bool old)
{
	return psprintf(SQL_TAKE, matviewOid, relid, old ? "true" : "false",
					format_type_be_qualified(get_rel_type_id(relid)));
}

/*
 * The transition tables the segments kept of one base table, as one of the
 * entry's, each segment's rows after another's.
 */
static void
fetch_rows(IvmEntry *entry, KeptTable *k, bool old)
{
	IvmModifiedTable *table = GpIvmFindTable(entry, k->relid);
	int			nsegs = GpCoreApiLookup()->get_segment_count();
	uint64	   *counts = palloc0_array(uint64, nsegs);
	Tuplestorestate *store;
	MemoryContext oldcxt;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (table == NULL || (old ? k->old_rows : k->new_rows) == 0)
		return;

	/*
	 * The entry ends it; a store that spills to a file has it released with
	 * the transaction, as the entry's own copies do (ivm_state.c).
	 */
	oldcxt = MemoryContextSwitchTo(entry->cxt);
	CurrentResourceOwner = TopTransactionResourceOwner;
	store = tuplestore_begin_heap(false, false, work_mem);
	CurrentResourceOwner = oldowner;
	MemoryContextSwitchTo(oldcxt);

	GpDispatchWriteReturning(take_sql(entry->matviewOid, k->relid, old), -1,
							 NULL, 0, table->tupdesc, store, false, counts);
	(void) GpIvmEntryAddTransition(entry, table, store, old, true);
}

/* ------------------------------------------------------------------------- */
/* The coordinator: the deltas to the segments                               */
/* ------------------------------------------------------------------------- */

/*
 * Where a view's delta rows go: the segment its distribution key names, or
 * every one for a replicated view.  Rows wait here, per segment, until there
 * are enough of them to send.
 */
typedef struct IvmRouter
{
	Relation	matviewRel;
	MemoryContext cxt;			/* where the waiting rows are kept */
	TupleDesc	desc;			/* the view's */
	GpHash	   *hash;			/* NULL: replicated */
	int			nsegs;
	Oid			rowtype;		/* the view's row type, and its array's */
	Oid			arraytype;
	Oid			array_out;
	ArrayBuildState **rows;		/* per segment, waiting to be sent */
	int		   *counts;
	bool	   *sent;			/* a segment given rows this step */
} IvmRouter;

static void
router_init(IvmRouter *r, Relation matviewRel)
{
	GpPolicy   *policy = GpPolicyGet(RelationGetRelid(matviewRel));
	bool		isvarlena;

	r->matviewRel = matviewRel;
	r->cxt = CurrentMemoryContext;
	r->desc = RelationGetDescr(matviewRel);
	r->nsegs = GpCoreApiLookup()->get_segment_count();
	r->hash = GpPolicyIsReplicated(policy) ? NULL : GpHashMake(policy, r->desc);
	r->rowtype = matviewRel->rd_rel->reltype;
	r->arraytype = get_array_type(r->rowtype);
	if (!OidIsValid(r->arraytype))
		elog(ERROR, "materialized view \"%s\" has no array type",
			 RelationGetRelationName(matviewRel));
	getTypeOutputInfo(r->arraytype, &r->array_out, &isvarlena);
	r->rows = palloc0_array(ArrayBuildState *, r->nsegs);
	r->counts = palloc0_array(int, r->nsegs);
	r->sent = palloc0_array(bool, r->nsegs);
}

/* The rows waiting for a segment, sent to it as "kind", 'o' or 'n'. */
static void
router_flush(IvmRouter *r, int seg, char kind)
{
	Datum		array;
	const char *values[3];

	if (r->counts[seg] == 0)
		return;

	array = makeArrayResult(r->rows[seg], r->cxt);
	values[0] = psprintf("%u", RelationGetRelid(r->matviewRel));
	values[1] = kind == 'o' ? "o" : "n";
	values[2] = OidOutputFunctionCall(r->array_out, array);
	GpDispatchParamsOnContent(seg, SQL_STAGE, 3, values, NULL, NULL);

	pfree((void *) values[2]);
	pfree(DatumGetPointer(array));
	r->rows[seg] = NULL;
	r->counts[seg] = 0;
	r->sent[seg] = true;
}

static void
router_add(IvmRouter *r, int seg, Datum row, char kind)
{
	r->rows[seg] = accumArrayResult(r->rows[seg], row, false, r->rowtype,
									r->cxt);
	if (++r->counts[seg] >= IVM_STAGE_ROWS)
		router_flush(r, seg, kind);
}

/*
 * Every row of a store to the segment that holds its view row -- a delta's,
 * in the view's columns -- sent as "kind".
 */
static void
router_send(IvmRouter *r, Tuplestorestate *store, TupleDesc desc, char kind)
{
	TupleTableSlot *slot;
	MemoryContext rowcxt;

	if (store == NULL)
		return;
	if (desc->natts != r->desc->natts)
		elog(ERROR, "a delta of \"%s\" has %d columns, not %d",
			 RelationGetRelationName(r->matviewRel), desc->natts, r->desc->natts);

	slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "gp_matview delta row",
								   ALLOCSET_DEFAULT_SIZES);
	tuplestore_rescan(store);
	while (tuplestore_gettupleslot(store, true, false, slot))
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(rowcxt);
		HeapTuple	tuple;
		Datum		row;
		int			seg;

		/* the row, as a value of the view's row type, which the array copies */
		slot_getallattrs(slot);
		tuple = heap_form_tuple(r->desc, slot->tts_values, slot->tts_isnull);
		row = heap_copy_tuple_as_datum(tuple, r->desc);
		seg = r->hash != NULL
			? GpHashSegment(r->hash, slot->tts_values, slot->tts_isnull)
			: GP_HASH_ALL_SEGMENTS;
		MemoryContextSwitchTo(oldcxt);

		if (seg == GP_HASH_ALL_SEGMENTS)
			for (int i = 0; i < r->nsegs; i++)
				router_add(r, i, row, kind);
		else
			router_add(r, seg, row, kind);
		MemoryContextReset(rowcxt);
	}
	for (int i = 0; i < r->nsegs; i++)
		router_flush(r, i, kind);
	MemoryContextDelete(rowcxt);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * The segments given rows apply them -- every segment, for "replace", whose
 * rows go whether or not new ones came.
 */
static void
router_apply(IvmRouter *r, bool replace)
{
	int		   *contents = palloc_array(int, r->nsegs);
	uint64	   *counts = palloc0_array(uint64, r->nsegs);
	int			n = 0;
	const char *values[2];

	for (int i = 0; i < r->nsegs; i++)
	{
		if (replace || r->sent[i])
			contents[n++] = i;
		r->sent[i] = false;
	}
	if (n == 0)
		return;

	values[0] = psprintf("%u", RelationGetRelid(r->matviewRel));
	values[1] = replace ? "true" : "false";
	GpDispatchCommandParamsOnContents(SQL_APPLY, 2, NULL, values, contents, n,
									  counts);
}

/* One step of the delta, to the segments, and applied there. */
static void
apply_on_segments(Relation matviewRel, Tuplestorestate *old_rows,
				  Tuplestorestate *new_rows, TupleDesc desc, void *arg)
{
	IvmRouter  *r = (IvmRouter *) arg;

	router_send(r, old_rows, desc, 'o');
	router_send(r, new_rows, desc, 'n');
	router_apply(r, false);
}

/*
 * What the delta cannot express, recomputed: the view's query, run here as
 * a delta's is, its rows sent to the segments in place of theirs.
 */
static void
recompute(IvmRouter *r)
{
	Query	   *query = copyObject(GpIvmGetViewQuery(r->matviewRel));
	Tuplestorestate *rows;
	TupleDesc	desc;
	double		ntuples;

	AcquireRewriteLocks(query, true, false);
	rows = GpIvmRunQuery(query, NULL, &desc, &ntuples);
	router_send(r, rows, desc, 'n');
	router_apply(r, true);
	tuplestore_end(rows);
}

/* ------------------------------------------------------------------------- */
/* The coordinator: maintaining a view                                       */
/* ------------------------------------------------------------------------- */

static void
maintain(Oid matviewOid)
{
	Relation	matviewRel;
	IvmEntry   *entry;
	List	   *kept;
	IvmRouter	router;
	GpIvmOwnerState owner;
	ListCell   *lc;

	/* A view the statement dropped keeps nothing up to date. */
	matviewRel = try_table_open(matviewOid, ExclusiveLock);
	if (matviewRel == NULL)
	{
		entry = GpIvmEntryTake(matviewOid);
		if (entry != NULL)
			GpIvmEntryForget(entry);
		return;
	}

	/* The coordinator's queries below see what the statement wrote. */
	CommandCounterIncrement();
	if (ActiveSnapshotSet())
		PushCopiedSnapshot(GetActiveSnapshot());
	else
		PushActiveSnapshot(GetTransactionSnapshot());
	UpdateActiveSnapshotCommandId();

	entry = collect(matviewOid, &kept);

	/* One not populated has nothing to keep up to date: REFRESH fills it. */
	if (!RelationIsPopulated(matviewRel) || !anything_changed(entry, kept))
	{
		GpIvmEntryForget(entry);
		PopActiveSnapshot();
		table_close(matviewRel, NoLock);
		return;
	}

	/*
	 * As the view's owner, as one node's maintenance runs, and planned by the
	 * planner, whose queries read transition tables ORCA has no plan for.
	 */
	GpIvmAsOwnerBegin(matviewOid, &owner);
	if (GetConfigOption("gp.optimizer", true, false) != NULL)
		(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0,
								 false);

	router_init(&router, matviewRel);
	if (GpIvmDeltaSupported(entry, matviewRel, true))
	{
		foreach(lc, kept)
		{
			fetch_rows(entry, (KeptTable *) lfirst(lc), true);
			fetch_rows(entry, (KeptTable *) lfirst(lc), false);
		}
		if (!GpIvmComputeDeltas(entry, matviewRel, true, apply_on_segments,
								&router))
			elog(ERROR, "the delta of \"%s\" could not be computed",
				 RelationGetRelationName(matviewRel));
		GpIvmCount(true);
	}
	else
	{
		recompute(&router);
		GpIvmCount(false);
	}

	GpIvmAsOwnerEnd(&owner);
	GpIvmEntryForget(entry);
	PopActiveSnapshot();
	table_close(matviewRel, NoLock);
}

/* Maintain each view of the list, and nothing inside that starts another. */
static void
maintain_views(List *views)
{
	ListCell   *lc;

	if (views == NIL)
		return;

	maintaining++;
	PG_TRY();
	{
		foreach(lc, views)
			maintain(lfirst_oid(lc));
	}
	PG_FINALLY();
	{
		maintaining--;
	}
	PG_END_TRY();
}

/* Is this backend a cluster's coordinator, not already maintaining a view? */
static bool
coordinator_may_maintain(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return maintaining == 0 && core != NULL && !core->is_single_node() &&
		core->get_role() == GP_ROLE_DISPATCH;
}

/*
 * A statement's executor is finished: the incremental views over the tables
 * it wrote are brought up to date, and any view this node's own triggers
 * kept something for.
 */
void
GpIvmClusterStatementEnd(PlannedStmt *stmt)
{
	List	   *views;
	int			rti = -1;

	if (!coordinator_may_maintain())
		return;

	views = GpIvmEntryViews();
	while ((rti = bms_next_member(stmt->resultRelationRelids, rti)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rti, stmt->rtable);

		if (rte->rtekind == RTE_RELATION)
			views = views_of_relation(rte->relid, views);
	}
	maintain_views(views);
}

/*
 * A utility statement is done: a COPY FROM into "relid", when it is valid,
 * brought its views rows; and a TRUNCATE, or anything else whose triggers
 * fired here, left what they kept.
 */
void
GpIvmClusterUtilityEnd(Oid relid)
{
	List	   *views;

	if (!coordinator_may_maintain())
		return;

	views = GpIvmEntryViews();
	if (OidIsValid(relid))
		views = views_of_relation(relid, views);
	maintain_views(views);
}

/* Is the coordinator maintaining a view right now? */
bool
GpIvmClusterMaintaining(void)
{
	return maintaining > 0;
}

/* ------------------------------------------------------------------------- */
/* Making one: where its rows go, and its triggers on the segments            */
/* ------------------------------------------------------------------------- */

/* The resnos of the columns a count view's rows are found by: its GROUP BY. */
static List *
group_resnos(Query *rewritten)
{
	List	   *resnos = NIL;
	ListCell   *lc;

	foreach(lc, rewritten->groupClause)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(scl, rewritten->targetList);

		resnos = lappend_int(resnos, tle->resno);
	}
	return resnos;
}

/* Does the rewritten query count its rows: a GROUP BY, DISTINCT or aggregate? */
static bool
counts_rows(Query *rewritten)
{
	return rewritten->hasAggs || rewritten->groupClause != NIL ||
		rewritten->distinctClause != NIL;
}

/* A view column's name: the one the statement gave it, or its query's. */
static char *
column_name(TargetEntry *tle, List *colNames)
{
	if (tle->resno <= list_length(colNames))
		return strVal(list_nth(colNames, tle->resno - 1));
	return tle->resname;
}

/*
 * Of the view's columns, those its one base table is distributed by, in the
 * key's order: the view's rows are then where the table's are, as
 * Cloudberry's planner, which reads a query's distribution off its plan,
 * would put them.  NIL where the view reads more than one table, or one of
 * the key's columns is not a column of the view -- or not among "among",
 * when given.
 */
static List *
base_key_columns(Query *rewritten, List *among)
{
	RangeTblEntry *base = NULL;
	GpPolicy   *policy;
	List	   *resnos = NIL;
	ListCell   *lc;
	int			rti = 0;
	int			baserti = 0;

	foreach(lc, rewritten->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		rti++;
		if (rte->rtekind != RTE_RELATION)
			continue;
		if (base != NULL)
			return NIL;
		base = rte;
		baserti = rti;
	}
	if (base == NULL)
		return NIL;
	policy = GpPolicyGet(base->relid);
	if (!GpPolicyIsHashPartitioned(policy) || policy->nattrs == 0)
		return NIL;

	for (int k = 0; k < policy->nattrs; k++)
	{
		TargetEntry *found = NULL;

		foreach(lc, rewritten->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Var		   *var = (Var *) tle->expr;

			if (!tle->resjunk && IsA(var, Var) && var->varno == baserti &&
				var->varlevelsup == 0 && var->varattno == policy->attrs[k] &&
				(among == NIL || list_member_int(among, tle->resno)))
			{
				found = tle;
				break;
			}
		}
		if (found == NULL)
			return NIL;
		resnos = lappend_int(resnos, found->resno);
	}
	return resnos;
}

/*
 * The distribution an incremental view is given on a cluster where its
 * statement names none, as gp.distributed_by's value: one its maintenance can
 * send a delta row by (GpIvmClusterCheckDistribution()), said nothing of --
 * Cloudberry's incremental views choose theirs without the notice a table
 * made by CREATE TABLE AS gives.
 *
 *	- A view whose rows are counted -- GROUP BY, DISTINCT, an aggregate -- is
 *	  found by its GROUP BY columns, so it is distributed by them: by those
 *	  its base table is distributed by, where every one of the table's key
 *	  is among them, and otherwise by all of them.  An aggregate without
 *	  GROUP BY is one row, whose columns are all counts and sums, which change
 *	  with every delta: it is replicated, one row on every segment.
 *	- Any other view's row is one base row, found by all its columns, so any
 *	  of them will do: its base table's key, as above, or else its first
 *	  column that can be hashed.
 */
char *
GpIvmClusterDefaultDistribution(Query *rewritten, List *colNames)
{
	List	   *group = counts_rows(rewritten) ? group_resnos(rewritten) : NIL;
	List	   *resnos;
	StringInfoData buf;
	ListCell   *lc;

	if (counts_rows(rewritten))
	{
		resnos = base_key_columns(rewritten, group);
		if (resnos == NIL)
		{
			/* the GROUP BY columns that can be hashed */
			foreach(lc, group)
			{
				TargetEntry *tle = get_tle_by_resno(rewritten->targetList, lfirst_int(lc));

				if (OidIsValid(GpPolicyDefaultOpclass(exprType((Node *) tle->expr))))
					resnos = lappend_int(resnos, tle->resno);
			}
		}
	}
	else
	{
		resnos = base_key_columns(rewritten, NIL);
		if (resnos == NIL)
		{
			foreach(lc, rewritten->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				if (!tle->resjunk &&
					OidIsValid(GpPolicyDefaultOpclass(exprType((Node *) tle->expr))))
				{
					resnos = list_make1_int(tle->resno);
					break;
				}
			}
		}
	}

	if (resnos == NIL)
		return pstrdup("replicated");

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '(');
	foreach(lc, resnos)
	{
		TargetEntry *tle = get_tle_by_resno(rewritten->targetList, lfirst_int(lc));

		if (buf.len > 1)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, quote_identifier(column_name(tle, colNames)));
	}
	appendStringInfoChar(&buf, ')');
	return buf.data;
}

/*
 * An incremental view on a cluster, once made: is its distribution one its
 * maintenance can send a delta row by?  A delta row goes to the segment its
 * distribution key hashes to, which has to be the segment of the view row it
 * changes: so the key has to be among the columns a view row is found by --
 * a counted view's GROUP BY columns, which the counts and sums beside them
 * are not, or any column of a view without -- or the view replicated, every
 * row on every segment.  A random one is refused: no segment is the one a
 * row is on.
 */
void
GpIvmClusterCheckDistribution(Oid matviewOid, Query *rewritten)
{
	GpPolicy   *policy = GpPolicyGet(matviewOid);
	List	   *group = group_resnos(rewritten);

	if (GpPolicyIsReplicated(policy))
		return;
	if (!GpPolicyIsHashPartitioned(policy))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("an incremental materialized view on a cluster cannot be distributed randomly"),
				 errhint("Distribute it by its GROUP BY columns, or DISTRIBUTED REPLICATED.")));
	if (!counts_rows(rewritten))
		return;
	for (int k = 0; k < policy->nattrs; k++)
		if (!list_member_int(group, policy->attrs[k]))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the distribution key of incremental materialized view \"%s\" must be among its GROUP BY columns",
							get_rel_name(matviewOid)),
					 errdetail("Column \"%s\" is not one of them.",
							   get_attname(matviewOid, policy->attrs[k], false)),
					 errhint("Distribute it by its GROUP BY columns, or DISTRIBUTED REPLICATED.")));
}

/*
 * On a cluster's coordinator, the view's triggers on the segments' base
 * tables too: made there by each segment from its own copy of the view,
 * whose label -- sent before this -- says it is incremental.
 */
void
GpIvmClusterMakeTriggers(Oid matviewOid)
{
	const GpCoreApi *core = GpCoreApiLookup();

	if (core == NULL || core->is_single_node() ||
		core->get_role() != GP_ROLE_DISPATCH)
		return;
	GpDispatchCommand(psprintf(SQL_MAKE_TRIGGERS, matviewOid));
}

/* ------------------------------------------------------------------------- */
/* A segment                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Is this call the coordinator's maintenance -- the statement it sent, as
 * this file writes it for these arguments, on the coordinator's own
 * connection -- rather than the same function in a user's statement that a
 * segment runs?  Raises if not.
 */
static void
check_caller(const char *expected)
{
	const GpCoreApi *core = GpCoreApiLookup();

	if (core == NULL || core->get_role() != GP_ROLE_EXECUTE ||
		!GpClusterDispatchTrusted() || debug_query_string == NULL ||
		strcmp(debug_query_string, expected) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only the coordinator's maintenance of an incremental materialized view may call this"),
				 errdetail("It is called on a segment, as the statement the coordinator sends, on a connection that carries the cluster's secret.")));
}

/*
 * What a segment is doing for the coordinator: the entry gp_matview.ivm_stash()
 * took, which gp_matview.ivm_take() reads; and the delta rows
 * gp_matview.ivm_stage() brought, which gp_matview.ivm_apply() applies.  All
 * of it the transaction's, and forgotten with it or with a subtransaction
 * that ends in an error.
 */
static IvmEntry *taken = NULL;
static Oid	staged_view = InvalidOid;
static Tuplestorestate *staged_old = NULL;
static Tuplestorestate *staged_new = NULL;
static MemoryContext staged_cxt = NULL;
static bool segment_callbacks = false;

static void
forget_staged(void)
{
	if (staged_cxt != NULL)
	{
		if (staged_old != NULL)
			tuplestore_end(staged_old);
		if (staged_new != NULL)
			tuplestore_end(staged_new);
		MemoryContextDelete(staged_cxt);
	}
	staged_view = InvalidOid;
	staged_old = staged_new = NULL;
	staged_cxt = NULL;
}

static void
segment_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
		return;

	/* the memory goes with the transaction's context, the files with its owner */
	taken = NULL;
	staged_view = InvalidOid;
	staged_old = staged_new = NULL;
	staged_cxt = NULL;
}

static void
segment_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		forget_staged();
}

static void
segment_callbacks_registered(void)
{
	if (segment_callbacks)
		return;
	RegisterXactCallback(segment_xact_callback, NULL);
	RegisterSubXactCallback(segment_subxact_callback, NULL);
	segment_callbacks = true;
}

/*
 * gp_matview.ivm_make_triggers(matview oid)
 *
 * A segment gives the view's base tables the triggers that keep for the
 * coordinator what each statement changed, as the coordinator's copy of them
 * has -- where the view is labelled incremental and has none yet.
 */
Datum
gp_ivm_make_triggers(PG_FUNCTION_ARGS)
{
	Oid			matviewOid = PG_GETARG_OID(0);

	check_caller(psprintf(SQL_MAKE_TRIGGERS, matviewOid));
	GpIvmRestored(matviewOid);
	PG_RETURN_VOID();
}

/*
 * gp_matview.ivm_stash(matview oid)
 *	  RETURNS SETOF (relid oid, old_rows bigint, new_rows bigint, truncated bool)
 *
 * What this segment's triggers kept for the view since the coordinator last
 * asked: a row per base table, with how many rows it lost and gained, and
 * whether a TRUNCATE was among the statements.  The entry is taken out of
 * the triggers' way, for gp_matview.ivm_take() to read.
 */
Datum
gp_ivm_stash(PG_FUNCTION_ARGS)
{
	Oid			matviewOid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	check_caller(psprintf(SQL_STASH, matviewOid));
	segment_callbacks_registered();

	InitMaterializedSRF(fcinfo, 0);

	if (taken != NULL)
		GpIvmEntryForget(taken);
	taken = GpIvmEntryTake(matviewOid);
	if (taken == NULL)
		return (Datum) 0;

	foreach(lc, taken->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};
		int64		old_rows = 0;
		int64		new_rows = 0;
		ListCell   *lc2;

		foreach(lc2, table->old_stores)
			old_rows += tuplestore_tuple_count(((IvmTransition *) lfirst(lc2))->store);
		foreach(lc2, table->new_stores)
			new_rows += tuplestore_tuple_count(((IvmTransition *) lfirst(lc2))->store);

		values[0] = ObjectIdGetDatum(table->relid);
		values[1] = Int64GetDatum(old_rows);
		values[2] = Int64GetDatum(new_rows);
		values[3] = BoolGetDatum(taken->truncated);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * gp_matview.ivm_take(matview oid, relid oid, old bool, rowtype anyelement)
 *	  RETURNS SETOF anyelement
 *
 * The rows of one base table the entry gp_matview.ivm_stash() took holds:
 * those the statements deleted ("old") or inserted, as the table's rows --
 * NULL::t says which row type that is.
 */
Datum
gp_ivm_take(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			matviewOid;
	Oid			relid;
	bool		old;
	IvmModifiedTable *table;
	TupleTableSlot *slot;
	ListCell   *lc;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("gp_matview.ivm_take()'s view, table and kind must not be null")));
	matviewOid = PG_GETARG_OID(0);
	relid = PG_GETARG_OID(1);
	old = PG_GETARG_BOOL(2);
	if (get_rel_type_id(relid) != get_fn_expr_argtype(fcinfo->flinfo, 3))
		elog(ERROR, "gp_matview.ivm_take() is not given the table's row type");
	check_caller(take_sql(matviewOid, relid, old));

	InitMaterializedSRF(fcinfo, 0);

	if (taken == NULL || taken->matviewOid != matviewOid ||
		(table = GpIvmFindTable(taken, relid)) == NULL)
		return (Datum) 0;

	slot = MakeSingleTupleTableSlot(table->tupdesc, &TTSOpsMinimalTuple);
	foreach(lc, old ? table->old_stores : table->new_stores)
	{
		Tuplestorestate *store = ((IvmTransition *) lfirst(lc))->store;

		tuplestore_rescan(store);
		while (tuplestore_gettupleslot(store, true, false, slot))
			tuplestore_puttupleslot(rsinfo->setResult, slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	return (Datum) 0;
}

/*
 * gp_matview.ivm_stage(matview oid, kind "char", rows text)
 *
 * Delta rows of the view for this segment, the text of an array of the
 * view's row type: rows it loses ('o') or gains ('n'), kept until
 * gp_matview.ivm_apply().
 */
Datum
gp_ivm_stage(PG_FUNCTION_ARGS)
{
	Oid			matviewOid = PG_GETARG_OID(0);
	char		kind = PG_GETARG_CHAR(1);
	char	   *rows_text = text_to_cstring(PG_GETARG_TEXT_PP(2));
	Oid			rowtype;
	Oid			arraytype;
	Oid			input;
	Oid			ioparam;
	Datum		array;
	Datum	   *rows;
	bool	   *nulls;
	int			nrows;
	Tuplestorestate **store;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	ResourceOwner oldowner = CurrentResourceOwner;
	MemoryContext oldcxt;

	check_caller(SQL_STAGE);
	segment_callbacks_registered();
	if (kind != 'o' && kind != 'n')
		elog(ERROR, "unrecognized delta kind \"%c\"", kind);

	rowtype = get_rel_type_id(matviewOid);
	arraytype = OidIsValid(rowtype) ? get_array_type(rowtype) : InvalidOid;
	if (!OidIsValid(arraytype) || get_rel_relkind(matviewOid) != RELKIND_MATVIEW)
		elog(ERROR, "relation %u is not a materialized view", matviewOid);

	if (staged_view != matviewOid)
	{
		forget_staged();
		staged_cxt = AllocSetContextCreate(TopTransactionContext,
										   "gp_matview staged delta",
										   ALLOCSET_DEFAULT_SIZES);
		staged_view = matviewOid;
	}
	store = kind == 'o' ? &staged_old : &staged_new;
	if (*store == NULL)
	{
		oldcxt = MemoryContextSwitchTo(staged_cxt);
		CurrentResourceOwner = TopTransactionResourceOwner;
		*store = tuplestore_begin_heap(false, false, work_mem);
		CurrentResourceOwner = oldowner;
		MemoryContextSwitchTo(oldcxt);
	}

	getTypeInputInfo(arraytype, &input, &ioparam);
	array = OidInputFunctionCall(input, rows_text, ioparam, -1);
	get_typlenbyvalalign(rowtype, &typlen, &typbyval, &typalign);
	deconstruct_array(DatumGetArrayTypeP(array), rowtype, typlen, typbyval,
					  typalign, &rows, &nulls, &nrows);

	for (int i = 0; i < nrows; i++)
	{
		HeapTupleHeader td;
		HeapTupleData tuple;

		if (nulls[i])
			elog(ERROR, "a delta row of materialized view %u is null", matviewOid);
		td = DatumGetHeapTupleHeader(rows[i]);
		tuple.t_len = HeapTupleHeaderGetDatumLength(td);
		ItemPointerSetInvalid(&tuple.t_self);
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = td;
		tuplestore_puttuple(*store, &tuple);
	}

	PG_RETURN_VOID();
}

/*
 * gp_matview.ivm_apply(matview oid, replace bool)
 *
 * The delta rows gp_matview.ivm_stage() brought, applied to this segment's
 * rows of the view -- or, with "replace", its rows replaced by them -- as
 * the view's owner, under its maintenance, and forgotten.
 */
Datum
gp_ivm_apply(PG_FUNCTION_ARGS)
{
	Oid			matviewOid = PG_GETARG_OID(0);
	bool		replace = PG_GETARG_BOOL(1);
	Relation	rel;
	GpIvmOwnerState owner;
	int			save_depth;

	check_caller(SQL_APPLY);

	rel = table_open(matviewOid, RowExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_MATVIEW)
		elog(ERROR, "\"%s\" is not a materialized view", RelationGetRelationName(rel));
	if (staged_view != matviewOid)
		forget_staged();

	save_depth = MatViewIncrementalMaintenanceDepthExternal();
	PG_TRY();
	{
		GpIvmAsOwnerBegin(matviewOid, &owner);
		OpenMatViewIncrementalMaintenanceExternal();
		GpIvmApplyStaged(rel, staged_old, staged_new, replace);
		CloseMatViewIncrementalMaintenanceExternal();
		GpIvmAsOwnerEnd(&owner);
	}
	PG_CATCH();
	{
		RestoreMatViewIncrementalMaintenanceDepthExternal(save_depth);
		PG_RE_THROW();
	}
	PG_END_TRY();

	forget_staged();
	table_close(rel, NoLock);
	PG_RETURN_VOID();
}
