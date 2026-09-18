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
 * ivm_state.c
 *	  What one statement's maintenance of a view has to remember.
 *
 * A statement can change several of a view's base tables, and can change one
 * of them more than once: a data-modifying CTE does the first, MERGE does the
 * second.  The view is brought up to date once, after the last of those
 * changes, so what the earlier triggers saw has to be kept until then.  That
 * is what the entry below is: the transition tables each trigger left, and a
 * snapshot of how the tables looked before the statement ran.
 *
 * The snapshot is what makes a delta over several tables possible.  One
 * table's delta has to be joined against the others as they were *before* the
 * statement, and by the time an AFTER trigger runs the statement has already
 * changed them.  The statement's own snapshot is exactly that earlier state:
 * under it the statement's inserts are not yet visible, because their command
 * id is not below its curcid, and its deletes still are.
 * gp_ivm_visible_in_prestate tests a row against it, and ivm_delta.c builds
 * the subquery that calls it.
 *
 * Cloudberry keeps the same state in a hash table in TopMemoryContext, and
 * has to export the snapshot to the segments and import it there
 * (pg_export_snapshot_def, ivm_import_snapshot).  On one node the snapshot is
 * simply registered, so none of that machinery is ported.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "gp_matview.h"

PG_FUNCTION_INFO_V1(gp_ivm_visible_in_prestate);

/*
 * The statements being maintained right now.  One entry per view, and only
 * between that view's first BEFORE trigger and its last AFTER trigger, so
 * this list holds one element in all but the woven cases above.
 */
static List *ivm_entries = NIL;
static bool ivm_cleanup_registered = false;

static void ivm_forget_all(XactEvent event, void *arg);
static void ivm_forget_subxact(SubXactEvent event, SubTransactionId mySubid,
							   SubTransactionId parentSubid, void *arg);
static void ivm_discard(IvmEntry *entry);

/*
 * An independent copy of the statement's snapshot.
 *
 * Not the active snapshot itself, which would be wrong twice over:
 * UpdateActiveSnapshotCommandId writes the current command id into that
 * snapshot in place, so it would stop being the pre-statement state as soon
 * as the statement advanced the counter, and it asserts that the snapshot it
 * writes to is not registered, which registering it here would make false.
 *
 * CopySnapshot is private to snapmgr.c.  The serializer parallel workers use
 * is not, it carries curcid -- the field that decides what "before this
 * statement" means -- and it returns a snapshot that is already marked
 * copied, so registering it takes a reference rather than making a second
 * copy.
 */
static Snapshot
copy_statement_snapshot(void)
{
	Snapshot	active = GetActiveSnapshot();
	Size		size;
	char	   *buf;
	Snapshot	copy;

	if (active == NULL)
		elog(ERROR, "gp_matview: a maintenance trigger ran with no active snapshot");

	size = EstimateSnapshotSpace(active);
	buf = palloc(size);
	SerializeSnapshot(active, buf);
	copy = RestoreSnapshot(buf);
	pfree(buf);

	return RegisterSnapshotOnOwner(copy, TopTransactionResourceOwner);
}

/*
 * Take a transition table out of the query that made it.
 *
 * PostgreSQL frees every one of a query's transition tables as soon as that
 * query's AFTER triggers have run.  The trigger that finishes a view reads
 * them where they lie, which is what the common case does.  An earlier one
 * may belong to a query that ends first -- a row trigger that writes another
 * of the view's base tables is the case that does -- and what it left would
 * then be gone by the time the view is brought up to date.  So anything the
 * last trigger will not use immediately is copied out.
 *
 * The copy is made under the transaction's resource owner, because it may
 * spill to a file and the statement's own owner is released first.
 */
static Tuplestorestate *
copy_transition(Tuplestorestate *src, TupleDesc desc)
{
	ResourceOwner oldowner = CurrentResourceOwner;
	Tuplestorestate *dst;
	TupleTableSlot *slot;
	int			ptr;

	CurrentResourceOwner = TopTransactionResourceOwner;
	dst = tuplestore_begin_heap(false, false, work_mem);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);

	/* A read pointer of our own, so that nobody else's position moves. */
	ptr = tuplestore_alloc_read_pointer(src, EXEC_FLAG_REWIND);
	tuplestore_select_read_pointer(src, ptr);
	tuplestore_rescan(src);

	while (tuplestore_gettupleslot(src, true, false, slot))
		tuplestore_puttupleslot(dst, slot);

	tuplestore_select_read_pointer(src, 0);
	ExecDropSingleTupleTableSlot(slot);
	CurrentResourceOwner = oldowner;

	return dst;
}

static IvmEntry *
find_entry(Oid matviewOid)
{
	ListCell   *lc;

	foreach(lc, ivm_entries)
	{
		IvmEntry   *entry = (IvmEntry *) lfirst(lc);

		if (entry->matviewOid == matviewOid)
			return entry;
	}

	return NULL;
}

/*
 * A view's BEFORE trigger: start the entry if this is the statement's first,
 * and take the snapshot that says what the tables held before it ran.
 */
void
GpIvmEntryBefore(Oid matviewOid)
{
	IvmEntry   *entry = find_entry(matviewOid);

	if (entry == NULL)
	{
		MemoryContext cxt;
		MemoryContext oldcxt;

		if (!ivm_cleanup_registered)
		{
			RegisterXactCallback(ivm_forget_all, NULL);
			RegisterSubXactCallback(ivm_forget_subxact, NULL);
			ivm_cleanup_registered = true;
		}

		cxt = AllocSetContextCreate(TopTransactionContext,
									"gp_matview maintenance",
									ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(cxt);

		entry = palloc0(sizeof(IvmEntry));
		entry->matviewOid = matviewOid;
		entry->cxt = cxt;
		entry->subid = GetCurrentSubTransactionId();
		entry->snapshot = copy_statement_snapshot();

		/*
		 * The list has to outlive the entry's context, which is deleted when
		 * the statement is done with the view, so it is built in the
		 * transaction's context rather than in that one.
		 */
		MemoryContextSwitchTo(TopTransactionContext);
		ivm_entries = lappend(ivm_entries, entry);

		MemoryContextSwitchTo(oldcxt);
	}

	entry->before_count++;
}

/*
 * A view's AFTER trigger: record what this trigger is handing over, and say
 * whether it is the last one the statement will fire for this view.  Only
 * then is the view brought up to date.
 */
IvmEntry *
GpIvmEntryAfter(Oid matviewOid, TriggerData *trigdata, bool *is_last)
{
	IvmEntry   *entry = find_entry(matviewOid);
	MemoryContext oldcxt;
	IvmModifiedTable *table;
	Oid			relid = RelationGetRelid(trigdata->tg_relation);
	ListCell   *lc;

	if (entry == NULL)
		elog(ERROR, "gp_matview: an AFTER trigger fired for view %u with no BEFORE trigger",
			 matviewOid);

	entry->after_count++;

	oldcxt = MemoryContextSwitchTo(entry->cxt);

	table = NULL;
	foreach(lc, entry->tables)
	{
		IvmModifiedTable *cand = (IvmModifiedTable *) lfirst(lc);

		if (cand->relid == relid)
		{
			table = cand;
			break;
		}
	}

	if (table == NULL)
	{
		table = palloc0(sizeof(IvmModifiedTable));
		table->relid = relid;
		table->tupdesc = CreateTupleDescCopy(RelationGetDescr(trigdata->tg_relation));
		entry->tables = lappend(entry->tables, table);
	}

	/*
	 * TRUNCATE leaves no transition tables at all, so a statement containing
	 * one has no delta to compute; the view is recomputed instead.
	 */
	if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		entry->truncated = true;

	/*
	 * Each transition table gets a name of its own.  The trigger calls them
	 * all "__ivm_oldtable" and "__ivm_newtable", which is enough while one
	 * table is in play and ambiguous as soon as two are.
	 */
	if (trigdata->tg_oldtable != NULL)
	{
		IvmTransition *tr = palloc0(sizeof(IvmTransition));

		tr->store = trigdata->tg_oldtable;
		tr->name = psprintf("__ivm_old_%u_%d", relid,
							list_length(table->old_stores) + 1);
		table->old_stores = lappend(table->old_stores, tr);
	}
	if (trigdata->tg_newtable != NULL)
	{
		IvmTransition *tr = palloc0(sizeof(IvmTransition));

		tr->store = trigdata->tg_newtable;
		tr->name = psprintf("__ivm_new_%u_%d", relid,
							list_length(table->new_stores) + 1);
		table->new_stores = lappend(table->new_stores, tr);
	}

	/*
	 * Every BEFORE trigger this statement fired for this view has a matching
	 * AFTER trigger, so the last AFTER trigger is the one that has seen them
	 * all, and that is where the view is brought up to date.
	 */
	Assert(entry->before_count >= entry->after_count);
	*is_last = (entry->before_count == entry->after_count);

	/* What the last trigger will not read straight away is taken out. */
	if (!*is_last)
	{
		if (table->old_stores != NIL)
		{
			IvmTransition *tr = (IvmTransition *) llast(table->old_stores);

			if (!tr->owned)
			{
				tr->store = copy_transition(tr->store, table->tupdesc);
				tr->owned = true;
			}
		}
		if (table->new_stores != NIL)
		{
			IvmTransition *tr = (IvmTransition *) llast(table->new_stores);

			if (!tr->owned)
			{
				tr->store = copy_transition(tr->store, table->tupdesc);
				tr->owned = true;
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);

	return entry;
}

/*
 * Does this view's query read a table this statement changed?
 */
IvmModifiedTable *
GpIvmFindTable(IvmEntry *entry, Oid relid)
{
	ListCell   *lc;

	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);

		if (table->relid == relid)
			return table;
	}

	return NULL;
}

/*
 * The statement is done with this view.  Anything the prestate probes opened
 * is closed here; everything else goes with the context.
 */
void
GpIvmEntryForget(IvmEntry *entry)
{
	ListCell   *lc;

	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		ListCell   *lc2;

		if (table->slot != NULL)
			ExecDropSingleTupleTableSlot(table->slot);
		if (table->rel != NULL)
			table_close(table->rel, NoLock);
		table->slot = NULL;
		table->rel = NULL;

		/* The transition tables this module copied are its to end. */
		foreach(lc2, table->old_stores)
		{
			IvmTransition *tr = (IvmTransition *) lfirst(lc2);

			if (tr->owned)
				tuplestore_end(tr->store);
		}
		foreach(lc2, table->new_stores)
		{
			IvmTransition *tr = (IvmTransition *) lfirst(lc2);

			if (tr->owned)
				tuplestore_end(tr->store);
		}
	}

	if (entry->snapshot != NULL)
		UnregisterSnapshotFromOwner(entry->snapshot, TopTransactionResourceOwner);

	ivm_discard(entry);
}

/*
 * A statement that fires a BEFORE trigger and then fails never reaches the
 * AFTER trigger that would have finished with the entry, and an entry is only
 * meaningful inside the statement that made it.  So whatever is left when a
 * transaction or a subtransaction ends is dropped here.
 *
 * Nothing is closed on the way out: the resource owner being released is what
 * lets go of the relations and the snapshot, and the memory goes with the
 * context.  Only GpIvmEntryForget, which runs while the statement is still
 * good, has to put those back itself.
 */
static void
ivm_discard(IvmEntry *entry)
{
	ivm_entries = list_delete_ptr(ivm_entries, entry);
	MemoryContextDelete(entry->cxt);
}

static void
ivm_forget_all(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
		return;

	/* The contexts are children of TopTransactionContext, which goes too. */
	ivm_entries = NIL;
}

static void
ivm_forget_subxact(SubXactEvent event, SubTransactionId mySubid,
				   SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;

	foreach(lc, list_copy(ivm_entries))
	{
		IvmEntry   *entry = (IvmEntry *) lfirst(lc);

		if (entry->subid == mySubid)
			ivm_discard(entry);
	}
}

/* ------------------------------------------------------------------------- */

/*
 * gp_matview.visible_in_prestate(tableoid oid, ctid tid, matview oid) -> bool
 *
 * Was this row there before the statement ran?  The prestate subquery that
 * ivm_delta.c builds scans the table as it is now and keeps the rows this
 * answers for; the rows the statement deleted are added back from the old
 * transition tables, because a scan cannot reach them any more.
 */
Datum
gp_ivm_visible_in_prestate(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	Oid			matviewOid = PG_GETARG_OID(2);
	IvmEntry   *entry = find_entry(matviewOid);
	IvmModifiedTable *table;
	bool		visible;

	if (entry == NULL)
		elog(ERROR, "gp_matview: no maintenance is running for view %u", matviewOid);

	table = GpIvmFindTable(entry, tableoid);
	if (table == NULL)
		elog(ERROR, "gp_matview: table %u is not one this statement changed", tableoid);

	if (table->rel == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(entry->cxt);

		/* The statement holds a lock on it already. */
		table->rel = table_open(tableoid, NoLock);
		table->slot = MakeSingleTupleTableSlot(RelationGetDescr(table->rel),
											   table_slot_callbacks(table->rel));
		MemoryContextSwitchTo(oldcxt);
	}

	visible = table_tuple_fetch_row_version(table->rel, tid, entry->snapshot,
											table->slot);
	ExecClearTuple(table->slot);

	PG_RETURN_BOOL(visible);
}
