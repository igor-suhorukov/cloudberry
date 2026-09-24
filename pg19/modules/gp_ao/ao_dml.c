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
 * ao_dml.c
 *	  Writing an append-optimized table: rows appended in blocks to a
 *	  segment file the writer holds, and rows deleted in the visibility map.
 *
 * A statement that writes a table has one writer of it in this backend.  The
 * writer takes a segment file no other writer holds, and holds it to the end
 * of the transaction, as Cloudberry's appendonlywriter.c does; appends rows
 * to a block, and writes each block full, with its row in gp_ao.blkdir; and,
 * as the statement ends, writes the last block and the segment file's new
 * length.  Deletions are gathered the same way and written to the
 * visibility map as the statement ends.
 *
 * PostgreSQL 19's executor says when an insert ends only for COPY and the
 * statements that fill a new table (finish_bulk_insert); Cloudberry's has
 * dml_init and dml_fini callbacks for the rest.  So the writers are finished
 * where the executor finishes -- ExecutorFinish, before the statement's
 * AFTER triggers fire, which read the rows -- and, for any other way of
 * writing, before the transaction commits.
 *
 * A writer is the table's, the query's that began it and the
 * subtransaction's it began in, and is only ever written to and finished in
 * that subtransaction: a query a function runs from inside another has
 * writers of its own, through segment files of their own, and a
 * subtransaction that aborts drops its writers and leaves its parent's as
 * they were.  What the dropped ones wrote is past the end their segment
 * files' rows say, and the next writer writes over it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_ao.h"

struct AoInsertState
{
	Oid			relid;
	void	   *owner;			/* the query it writes for, or NULL */
	SubTransactionId subid;		/* the subtransaction it writes in */
	int64		storage_id;
	bool		columnar;
	int			natts;
	int			ngroups;
	AoOptions	opts;
	int			segno;
	AoSegfile  *sf;
	int64		next_rownum;
	int64		reserved_end;
	int64		reserve_chunk;
	int64		block_first;
	int			block_nrows;
	bool		has_unique;		/* the table has a unique index */
	ItemPointerData placeholder;	/* the block's placeholder, if it has one */
	/* a table by row: the block's MinimalTuples */
	StringInfoData rows;
	/* a table by column: each column's values */
	AoColumnBuilder *cols;
	StringInfoData raw;
	StringInfoData out;
	int64	   *offsets;
	int64		inserted;
	int64		blocks;
};

/* A statement's deletions from one table, one list a segment file. */
typedef struct AoDeleteState
{
	Oid			relid;
	void	   *owner;
	SubTransactionId subid;
	int64		storage_id;
	int			n[AO_MAX_SEGNO + 1];
	int			max[AO_MAX_SEGNO + 1];
	int64	   *rownums[AO_MAX_SEGNO + 1];
} AoDeleteState;

static MemoryContext ao_dml_cxt = NULL;
static List *ao_inserts = NIL;	/* AoInsertState * */
static List *ao_deletes = NIL;	/* AoDeleteState * */

/*
 * The queries whose ExecutorRun is in progress, innermost first.  A writer
 * belongs to the query that was running when it began, and is finished at
 * that query's ExecutorFinish -- not at the end of a query a function it
 * called ran through SPI, which would cut the outer query's block short and
 * write its segment file's row once a call.
 */
static List *ao_running = NIL;

/* The segment files this transaction holds, and the subtransaction whose
 * lock each is. */
typedef struct AoHeldSegno
{
	Oid			relid;
	int			segno;
	SubTransactionId subid;
} AoHeldSegno;
static List *ao_held_segnos = NIL;

/* The query a writer begun now writes for: the innermost running. */
static void *
ao_current_owner(void)
{
	return ao_running ? linitial(ao_running) : NULL;
}

static MemoryContext
ao_dml_context(void)
{
	if (ao_dml_cxt == NULL)
		ao_dml_cxt = AllocSetContextCreate(TopTransactionContext,
										   "gp_ao writers",
										   ALLOCSET_DEFAULT_SIZES);
	return ao_dml_cxt;
}

/* ------------------------------------------------------------------------- */
/* Segment files                                                             */
/* ------------------------------------------------------------------------- */

/* The class ID of a segment file's lock: gp_ao.segfile's OID. */
int
ao_segfile_lock_classid(void)
{
	return (int) ao_meta_relid("segfile", false);
}

/*
 * Take segment file segno of relid for writing, or for VACUUM, if no one
 * else holds it; held to the end of the transaction.
 */
bool
ao_segfile_try_lock(Oid relid, int segno)
{
	return ConditionalLockDatabaseObject(ao_segfile_lock_classid(), relid,
										 (uint16) segno, ExclusiveLock);
}

/* Is segment file segno of relid another writer's of this backend? */
static bool
ao_segno_in_use(Oid relid, int segno)
{
	ListCell   *lc;

	foreach(lc, ao_inserts)
	{
		AoInsertState *other = lfirst(lc);

		if (other->relid == relid && other->segno == segno)
			return true;
	}
	return false;
}

/*
 * Choose the segment file this writer appends to: one this transaction
 * already holds and no other writer of it uses, or the first in use no one
 * else holds, or a new one.
 */
static void
ao_choose_segfile(AoInsertState *st, Relation rel)
{
	AoSegfile  *segfiles;
	int			nsegfiles;
	bool		taken[AO_MAX_SEGNO + 1] = {0};
	ListCell   *lc;

	foreach(lc, ao_held_segnos)
	{
		AoHeldSegno *h = lfirst(lc);

		if (h->relid == st->relid && !ao_segno_in_use(st->relid, h->segno))
		{
			st->segno = h->segno;
			st->sf = ao_segfile_read(st->storage_id, h->segno, SnapshotSelf);
			if (st->sf != NULL && st->sf->state == AO_SEGFILE_DEFAULT)
				return;
		}
	}

	segfiles = ao_segfiles_read(st->storage_id, SnapshotSelf, &nsegfiles);
	for (int i = 0; i < nsegfiles; i++)
	{
		AoSegfile  *sf = &segfiles[i];

		taken[sf->segno] = true;
		if (sf->state != AO_SEGFILE_DEFAULT ||
			ao_segno_in_use(st->relid, sf->segno))
			continue;
		if (!ao_segfile_try_lock(st->relid, sf->segno))
			continue;
		/* Read again now it is ours: a writer may have just let it go. */
		st->sf = ao_segfile_read(st->storage_id, sf->segno, SnapshotSelf);
		if (st->sf == NULL || st->sf->state != AO_SEGFILE_DEFAULT)
			continue;
		st->segno = sf->segno;
		goto chosen;
	}

	/* A new one, whose number no one else is taking. */
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
	{
		if (taken[segno] || ao_segno_in_use(st->relid, segno) ||
			!ao_segfile_try_lock(st->relid, segno))
			continue;
		st->sf = ao_segfile_read(st->storage_id, segno, SnapshotSelf);
		if (st->sf != NULL)
			continue;			/* committed while we looked */
		ao_segfile_insert(st->storage_id, segno, st->ngroups);
		CommandCounterIncrement();
		st->sf = ao_segfile_read(st->storage_id, segno, SnapshotSelf);
		st->segno = segno;
		goto chosen;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			 errmsg("could not find segment file to use for inserting into relation \"%s\"",
					RelationGetRelationName(rel)),
			 errdetail("All %d segment files are written by other transactions.",
					   AO_MAX_SEGNO)));

chosen:
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		AoHeldSegno *h = palloc(sizeof(AoHeldSegno));

		h->relid = st->relid;
		h->segno = st->segno;
		h->subid = GetCurrentSubTransactionId();
		ao_held_segnos = lappend(ao_held_segnos, h);
		MemoryContextSwitchTo(old);
	}
}

/* ------------------------------------------------------------------------- */
/* Inserting                                                                 */
/* ------------------------------------------------------------------------- */

/* Has rel a unique index, whose probes a block being built must answer? */
bool
ao_has_unique_index(Relation rel)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	bool		result = false;

	foreach(lc, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));

		if (HeapTupleIsValid(tup))
		{
			result = ((Form_pg_index) GETSTRUCT(tup))->indisunique;
			ReleaseSysCache(tup);
		}
		if (result)
			break;
	}
	list_free(indexes);
	return result;
}

static AoInsertState *
ao_find_insert(Oid relid, void *owner, SubTransactionId subid)
{
	ListCell   *lc;

	foreach(lc, ao_inserts)
	{
		AoInsertState *st = lfirst(lc);

		if (st->relid == relid && st->owner == owner && st->subid == subid)
			return st;
	}
	return NULL;
}

/* The running query's writer of rel, begun if it has none. */
AoInsertState *
ao_insert_state(Relation rel)
{
	void	   *owner = ao_current_owner();
	SubTransactionId subid = GetCurrentSubTransactionId();
	AoInsertState *st = ao_find_insert(RelationGetRelid(rel), owner, subid);
	MemoryContext old;

	if (st != NULL)
		return st;

	old = MemoryContextSwitchTo(ao_dml_context());
	st = palloc0(sizeof(AoInsertState));
	st->relid = RelationGetRelid(rel);
	st->owner = owner;
	st->subid = subid;
	st->storage_id = ao_storage_id(rel);
	st->columnar = ao_storage_is_columnar(rel);
	st->natts = RelationGetDescr(rel)->natts;
	st->ngroups = st->columnar ? st->natts : 1;
	ao_get_options(rel, &st->opts);
	initStringInfo(&st->rows);
	initStringInfo(&st->raw);
	initStringInfo(&st->out);
	st->offsets = palloc0_array(int64, st->ngroups);
	if (st->columnar)
	{
		st->cols = palloc_array(AoColumnBuilder, st->natts);
		for (int i = 0; i < st->natts; i++)
			ao_column_builder_init(&st->cols[i]);
	}
	ao_choose_segfile(st, rel);
	st->has_unique = ao_has_unique_index(rel);
	ItemPointerSetInvalid(&st->placeholder);
	if (st->sf->ngroups < st->ngroups)
	{
		/* Columns added since the segment file was begun: files of none. */
		st->sf->eof = repalloc0_array(st->sf->eof, int64, st->sf->ngroups,
									  st->ngroups);
		st->sf->eof_uncompressed = repalloc0_array(st->sf->eof_uncompressed,
												   int64, st->sf->ngroups,
												   st->ngroups);
		st->sf->ngroups = st->ngroups;
	}
	st->reserve_chunk = 16;
	st->next_rownum = st->reserved_end = 0;
	st->block_first = 0;
	ao_inserts = lappend(ao_inserts, st);
	MemoryContextSwitchTo(old);
	return st;
}

/* Write the block being built, if it has rows, and its directory row. */
static void
ao_flush_block(AoInsertState *st, Relation rel)
{
	if (st->block_nrows == 0)
		return;

	for (int g = 0; g < st->ngroups; g++)
	{
		StringInfo	raw;
		Size		len;

		if (st->columnar)
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), g);

			if (att->attisdropped)
			{
				st->offsets[g] = -1;
				ao_column_builder_reset(&st->cols[g]);
				continue;
			}
			ao_column_finish(&st->cols[g], &st->raw);
			ao_column_builder_reset(&st->cols[g]);
			raw = &st->raw;
		}
		else
			raw = &st->rows;

		len = ao_block_encode(raw, &st->opts, st->block_first,
							  st->block_nrows, &st->out);
		st->offsets[g] = st->sf->eof[g];
		ao_file_write(rel, AoFileNum(st->segno, st->columnar ? g + 1 : 0),
					  st->sf->eof[g], st->out.data, len);
		st->sf->eof[g] += len;
		st->sf->eof_uncompressed[g] += AO_BLOCK_HEADER_SIZE + raw->len;
	}
	resetStringInfo(&st->rows);

	if (ItemPointerIsValid(&st->placeholder))
	{
		ao_blkdir_replace(&st->placeholder, st->storage_id, st->segno,
						  st->block_first, st->block_nrows, st->offsets,
						  st->ngroups);
		ItemPointerSetInvalid(&st->placeholder);
	}
	else
		ao_blkdir_insert(st->storage_id, st->segno, st->block_first,
						 st->block_nrows, st->offsets, st->ngroups);
	st->sf->varblockcount++;
	st->blocks++;
	st->block_nrows = 0;
}

/* Append the row in slot, and give the slot its TID. */
void
ao_insert_slot(AoInsertState *st, Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext old = MemoryContextSwitchTo(ao_dml_context());
	Size		blocksize;

	slot_getallattrs(slot);

	if (st->next_rownum == st->reserved_end)
	{
		int64		first = ao_reserve_rownums(rel, st->segno,
											   st->reserve_chunk);

		/* A block's rows are numbered one after another. */
		if (first != st->reserved_end && st->block_nrows > 0)
			ao_flush_block(st, rel);
		st->next_rownum = first;
		st->reserved_end = first + st->reserve_chunk;
		st->reserve_chunk = Min(st->reserve_chunk * 2, 65536);
	}
	if (st->block_nrows == 0)
	{
		st->block_first = st->next_rownum;
		if (st->has_unique)
		{
			/* See ao_blkdir_insert_placeholder(). */
			ao_blkdir_insert_placeholder(st->storage_id, st->segno,
										 st->block_first, &st->placeholder);
			CommandCounterIncrement();
		}
	}

	if (st->columnar)
	{
		blocksize = 0;
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);
			Datum		value = slot->tts_values[i];
			bool		isnull = slot->tts_isnull[i];

			if (att->attisdropped)
				isnull = true;
			else if (!isnull)
				value = ao_detoast_value(att, value);
			ao_column_append(&st->cols[i], att, value, isnull);
			blocksize = Max(blocksize, (Size) st->cols[i].values.len);
		}
	}
	else
	{
		Datum	   *values = slot->tts_values;
		MinimalTuple tup;
		bool		copied = false;

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!slot->tts_isnull[i] && att->attlen == -1 &&
				VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			{
				if (!copied)
				{
					values = palloc_array(Datum, desc->natts);
					memcpy(values, slot->tts_values, sizeof(Datum) * desc->natts);
					copied = true;
				}
				values[i] = ao_detoast_value(att, values[i]);
			}
		}
		tup = heap_form_minimal_tuple(desc, values, slot->tts_isnull, 0);
		enlargeStringInfo(&st->rows, MAXALIGN(tup->t_len));
		memcpy(st->rows.data + st->rows.len, tup, tup->t_len);
		memset(st->rows.data + st->rows.len + tup->t_len, 0,
			   MAXALIGN(tup->t_len) - tup->t_len);
		st->rows.len += MAXALIGN(tup->t_len);
		blocksize = st->rows.len;
		pfree(tup);
		if (copied)
			pfree(values);
	}

	AoTidSet(&slot->tts_tid, st->segno, st->next_rownum);
	slot->tts_tableOid = st->relid;
	st->next_rownum++;
	st->block_nrows++;
	st->inserted++;
	st->sf->tupcount++;

	if (blocksize >= (Size) st->opts.blocksize ||
		st->block_nrows >= AO_MAX_ROWS_PER_BLOCK)
		ao_flush_block(st, rel);

	MemoryContextSwitchTo(old);
}

/*
 * A row of rel this backend is writing and has not written yet?  For a
 * unique index's probe, which counts it live; anything that has to read the
 * row finds it written, since the writers are finished before AFTER
 * triggers fire.
 */
bool
ao_pending_fetch(Relation rel, ItemPointer tid, TupleTableSlot *slot)
{
	int64		rownum = AoTidRownum(tid);
	ListCell   *lc;

	foreach(lc, ao_inserts)
	{
		AoInsertState *st = lfirst(lc);

		if (st->relid == RelationGetRelid(rel) &&
			AoTidSegno(tid) == st->segno && st->block_nrows > 0 &&
			rownum >= st->block_first &&
			rownum < st->block_first + st->block_nrows)
			return true;
	}
	return false;
}

/* Finish one writer: its last block, and its segment file's new length. */
static void
ao_finish_insert(AoInsertState *st)
{
	Relation	rel = table_open(st->relid, NoLock);

	ao_flush_block(st, rel);

	/* Row numbers taken and not used go back: no row has them. */
	if (st->next_rownum < st->reserved_end &&
		ao_next_rownum(rel, st->segno) == st->reserved_end)
		(void) ao_reserve_rownums(rel, st->segno,
								  st->next_rownum - st->reserved_end);

	if (st->inserted > 0)
	{
		st->sf->modcount++;
		ao_segfile_update(st->storage_id, st->sf);
	}
	table_close(rel, NoLock);
}

/* ------------------------------------------------------------------------- */
/* Deleting                                                                  */
/* ------------------------------------------------------------------------- */

/* The running query's deletions from relid, begun if asked. */
static AoDeleteState *
ao_find_delete(Oid relid, bool create)
{
	void	   *owner = ao_current_owner();
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;
	AoDeleteState *ds;
	MemoryContext old;

	foreach(lc, ao_deletes)
	{
		ds = lfirst(lc);
		if (ds->relid == relid && ds->owner == owner && ds->subid == subid)
			return ds;
	}
	if (!create)
		return NULL;

	old = MemoryContextSwitchTo(ao_dml_context());
	ds = palloc0(sizeof(AoDeleteState));
	ds->relid = relid;
	ds->owner = owner;
	ds->subid = subid;
	ao_deletes = lappend(ao_deletes, ds);
	MemoryContextSwitchTo(old);
	return ds;
}

static bool
ao_delete_state_has(AoDeleteState *ds, int segno, int64 rownum)
{
	for (int i = ds->n[segno] - 1; i >= 0; i--)
		if (ds->rownums[segno][i] == rownum)
			return true;
	return false;
}

/*
 * Has the running query deleted tid already?  An UPDATE or DELETE that
 * reaches a row twice, through a join, gets TM_SelfModified the second time.
 */
bool
ao_deleted_by_this_command(Relation rel, ItemPointer tid)
{
	AoDeleteState *ds = ao_find_delete(RelationGetRelid(rel), false);
	int			segno = AoTidSegno(tid);

	if (ds == NULL || segno < 1 || segno > AO_MAX_SEGNO)
		return false;
	return ao_delete_state_has(ds, segno, AoTidRownum(tid));
}

/*
 * Has any query of this backend deleted tid, and not written it to the
 * visibility map yet?  For a unique index's probe, which counts such a row
 * gone, as a heap's probe counts a row this transaction deleted.
 */
bool
ao_deleted_pending(Relation rel, ItemPointer tid)
{
	int			segno = AoTidSegno(tid);
	ListCell   *lc;

	if (segno < 1 || segno > AO_MAX_SEGNO)
		return false;
	foreach(lc, ao_deletes)
	{
		AoDeleteState *ds = lfirst(lc);

		if (ds->relid == RelationGetRelid(rel) &&
			ao_delete_state_has(ds, segno, AoTidRownum(tid)))
			return true;
	}
	return false;
}

/* Delete tid, as this statement ends; false if this command did already. */
bool
ao_delete_row(Relation rel, ItemPointer tid)
{
	AoDeleteState *ds;
	int			segno = AoTidSegno(tid);
	MemoryContext old;

	if (segno < 1 || segno > AO_MAX_SEGNO)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("TID (%u,%u) names no row of an append-optimized table",
						ItemPointerGetBlockNumberNoCheck(tid),
						ItemPointerGetOffsetNumberNoCheck(tid))));
	if (ao_deleted_by_this_command(rel, tid))
		return false;

	ds = ao_find_delete(RelationGetRelid(rel), true);
	if (ds->storage_id == 0)
		ds->storage_id = ao_storage_id(rel);
	old = MemoryContextSwitchTo(ao_dml_context());
	if (ds->n[segno] == ds->max[segno])
	{
		ds->max[segno] = Max(64, ds->max[segno] * 2);
		ds->rownums[segno] = ds->rownums[segno] ?
			repalloc_array(ds->rownums[segno], int64, ds->max[segno]) :
			palloc_array(int64, ds->max[segno]);
	}
	ds->rownums[segno][ds->n[segno]++] = AoTidRownum(tid);
	MemoryContextSwitchTo(old);
	return true;
}

static int
int64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a;
	int64		y = *(const int64 *) b;

	return (x > y) - (x < y);
}

static void
ao_finish_delete(AoDeleteState *ds)
{
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
	{
		if (ds->n[segno] == 0)
			continue;
		qsort(ds->rownums[segno], ds->n[segno], sizeof(int64), int64_cmp);
		ao_visimap_delete_rows(ds->storage_id, segno, ds->rownums[segno],
							   ds->n[segno]);
	}
}

/* ------------------------------------------------------------------------- */
/* When statements and transactions end                                      */
/* ------------------------------------------------------------------------- */

/*
 * Finish the writers and deletions of this subtransaction that belong to
 * owner, or with owner NULL those of relid, or of every table with relid 0.
 * Those of a parent are left alone: what they write, written here, would go
 * with this subtransaction if it aborted, and the parent would go on as if
 * it were written.
 */
static void
ao_dml_finish(void *owner, Oid relid)
{
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;
	bool		any = false;

	foreach(lc, ao_inserts)
	{
		AoInsertState *st = lfirst(lc);

		if (st->subid != subid ||
			(owner != NULL ? st->owner != owner :
			 (relid != InvalidOid && st->relid != relid)))
			continue;
		ao_inserts = foreach_delete_current(ao_inserts, lc);
		ao_finish_insert(st);
		any = true;
	}
	foreach(lc, ao_deletes)
	{
		AoDeleteState *ds = lfirst(lc);

		if (ds->subid != subid ||
			(owner != NULL ? ds->owner != owner :
			 (relid != InvalidOid && ds->relid != relid)))
			continue;
		ao_deletes = foreach_delete_current(ao_deletes, lc);
		ao_finish_delete(ds);
		any = true;
	}
	if (any)
		CommandCounterIncrement();
}

/* finish_bulk_insert, and before a commit: relid's writers, or all. */
void
ao_dml_flush(Oid relid)
{
	ao_dml_finish(NULL, relid);
}

/*
 * The writers and deletions of a table being dropped, forgotten: there is
 * nothing left to write them to.
 */
void
ao_dml_forget_rel(Oid relid)
{
	ListCell   *lc;

	foreach(lc, ao_inserts)
		if (((AoInsertState *) lfirst(lc))->relid == relid)
			ao_inserts = foreach_delete_current(ao_inserts, lc);
	foreach(lc, ao_deletes)
		if (((AoDeleteState *) lfirst(lc))->relid == relid)
			ao_deletes = foreach_delete_current(ao_deletes, lc);
}

/* A query's ExecutorRun begins, and ends. */
void
ao_dml_run_begin(void *query)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	ao_running = lcons(query, ao_running);
	MemoryContextSwitchTo(old);
}

void
ao_dml_run_end(void *query)
{
	if (ao_running != NIL && linitial(ao_running) == query)
		ao_running = list_delete_first(ao_running);
}

/* A query's ExecutorFinish: its writers, before its AFTER triggers. */
void
ao_dml_finish_query(void *query)
{
	if (ao_inserts != NIL || ao_deletes != NIL)
		ao_dml_finish(query, InvalidOid);
}

static void
ao_dml_forget(void)
{
	ao_inserts = NIL;
	ao_deletes = NIL;
	ao_dml_cxt = NULL;			/* TopTransactionContext's child, gone with it */
}

static void
ao_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			if (ao_inserts != NIL || ao_deletes != NIL)
				ao_dml_flush(InvalidOid);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			ao_dml_forget();
			ao_held_segnos = NIL;
			list_free(ao_running);
			ao_running = NIL;
			break;
	}
}

static void
ao_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		/* The parent's now, as the locks of their segment files are. */
		foreach(lc, ao_inserts)
			if (((AoInsertState *) lfirst(lc))->subid == mySubid)
				((AoInsertState *) lfirst(lc))->subid = parentSubid;
		foreach(lc, ao_deletes)
			if (((AoDeleteState *) lfirst(lc))->subid == mySubid)
				((AoDeleteState *) lfirst(lc))->subid = parentSubid;
		foreach(lc, ao_held_segnos)
			if (((AoHeldSegno *) lfirst(lc))->subid == mySubid)
				((AoHeldSegno *) lfirst(lc))->subid = parentSubid;
	}
	else if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		/*
		 * What the aborted writers built in memory goes, and what they wrote
		 * is past the end their segment files' rows say.  A segment file
		 * taken in the subtransaction was let go with its lock.  Their memory
		 * goes with the transaction's.
		 */
		foreach(lc, ao_inserts)
			if (((AoInsertState *) lfirst(lc))->subid >= mySubid)
				ao_inserts = foreach_delete_current(ao_inserts, lc);
		foreach(lc, ao_deletes)
			if (((AoDeleteState *) lfirst(lc))->subid >= mySubid)
				ao_deletes = foreach_delete_current(ao_deletes, lc);
		foreach(lc, ao_held_segnos)
			if (((AoHeldSegno *) lfirst(lc))->subid >= mySubid)
				ao_held_segnos = foreach_delete_current(ao_held_segnos, lc);
	}
}

void
ao_dml_init(void)
{
	RegisterXactCallback(ao_xact_callback, NULL);
	RegisterSubXactCallback(ao_subxact_callback, NULL);
}
