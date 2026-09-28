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
 * With gp.appendonly_insert_files above one, a statement's writer of a
 * table is the first of a group, as Cloudberry's get_insert_descriptor()
 * keeps a list: each writer takes gp.appendonly_insert_files_tuples_range
 * rows, and the group then turns to the next, begun on another segment file
 * until it has as many as the setting says -- so that a scan in parallel
 * would have files to share out.  Not in a utility session, for VACUUM's
 * compaction, or for a table this transaction made or rewrote, as
 * Cloudberry's ShouldUseReservedSegno() says.
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
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_ao.h"
#include "gp_core_api.h"

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
	AoOptions  *colopts;		/* a table by column: each column's own */
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
	int64		first_rownum;	/* of the first row it appended */
	int64		blocks;
	bool		unseen;			/* it wrote a block the command has not seen */
	int			range;			/* rows appended since its group turned to it */
	AoInsertState *lead;		/* its group's first writer, itself for that one */
	List	   *files;			/* the first's: the group's writers, as begun */
	int			cur;			/* the first's: which of them is written to */
	int			nfiles;			/* the first's: how many the group may have */
};

/*
 * A statement's deletions from one table: each segment file's row numbers,
 * in the order they came, written to the visibility map as the statement
 * ends; and, to say at once whether a row is one of them -- each deletion
 * asks, an UPDATE of a million rows a million times -- a bitmap of each
 * range of AO_VISIMAP_ROWS row numbers that has any, as the map has one.
 */
typedef struct AoDeleteRange
{
	int64		key;			/* segno << 40 | the range's first row / ROWS */
	uint8		bits[AO_VISIMAP_BYTES];
} AoDeleteRange;

typedef struct AoDeleteState
{
	Oid			relid;
	void	   *owner;
	SubTransactionId subid;
	int64		storage_id;
	int			n[AO_MAX_SEGNO + 1];
	int			max[AO_MAX_SEGNO + 1];
	int64	   *rownums[AO_MAX_SEGNO + 1];
	HTAB	   *ranges;			/* AoDeleteRange, by key; NULL before any */
	AoDeleteRange *last;		/* the range last looked in */
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

/* Fewer rows first, as Cloudberry's compare_candidates() puts them. */
static int
segfile_fewer_rows(const void *a, const void *b)
{
	const AoSegfile *x = *(const AoSegfile *const *) a;
	const AoSegfile *y = *(const AoSegfile *const *) b;

	if (x->tupcount != y->tupcount)
		return (x->tupcount > y->tupcount) - (x->tupcount < y->tupcount);
	return (x->segno > y->segno) - (x->segno < y->segno);
}

/* Is the writer being made VACUUM's, compacting into it? */
static bool ao_compaction_writer = false;

void
ao_dml_set_compaction_writer(bool on)
{
	ao_compaction_writer = on;
}

/* A new segment file for st, whose number no one else is taking. */
static bool
ao_new_segfile(AoInsertState *st, const bool *taken)
{
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
		return true;
	}
	return false;
}

/*
 * Choose the segment file this writer appends to: one this transaction
 * already holds and no other writer of it uses, or of those in use no one
 * else holds the one with the fewest rows, as Cloudberry's
 * choose_segno_internal() prefers, or a new one.  VACUUM's writer, which
 * compacts, takes a new one before one with rows, as Cloudberry's
 * CHOOSE_MODE_COMPACTION_WRITE does: rows moved into a file that has rows
 * already would be moved again when that file is compacted.
 */
static void
ao_choose_segfile(AoInsertState *st, Relation rel)
{
	AoSegfile  *segfiles;
	AoSegfile **byrows;
	int			nsegfiles;
	bool		taken[AO_MAX_SEGNO + 1] = {0};
	bool		tried_new = false;
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
	byrows = palloc_array(AoSegfile *, Max(nsegfiles, 1));
	for (int i = 0; i < nsegfiles; i++)
	{
		taken[segfiles[i].segno] = true;
		byrows[i] = &segfiles[i];
	}
	qsort(byrows, nsegfiles, sizeof(AoSegfile *), segfile_fewer_rows);
	for (int i = 0; i < nsegfiles; i++)
	{
		AoSegfile  *sf = byrows[i];

		if (sf->state != AO_SEGFILE_DEFAULT ||
			ao_segno_in_use(st->relid, sf->segno))
			continue;
		if (ao_compaction_writer && sf->tupcount > 0 && !tried_new)
		{
			tried_new = true;
			if (ao_new_segfile(st, taken))
				goto chosen;
		}
		if (!ao_segfile_try_lock(st->relid, sf->segno))
			continue;
		/* Read again now it is ours: a writer may have just let it go. */
		st->sf = ao_segfile_read(st->storage_id, sf->segno, SnapshotSelf);
		if (st->sf == NULL || st->sf->state != AO_SEGFILE_DEFAULT)
			continue;
		st->segno = sf->segno;
		goto chosen;
	}

	if (!tried_new && ao_new_segfile(st, taken))
		goto chosen;

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

/* The first writer of a group of rel's for this query and subtransaction. */
static AoInsertState *
ao_find_insert(Oid relid, void *owner, SubTransactionId subid)
{
	ListCell   *lc;

	foreach(lc, ao_inserts)
	{
		AoInsertState *st = lfirst(lc);

		if (st->relid == relid && st->owner == owner && st->subid == subid &&
			st->lead == st)
			return st;
	}
	return NULL;
}

/*
 * How many segment files a statement's insert into rel spreads over:
 * gp.appendonly_insert_files, but for Cloudberry's exceptions -- a utility
 * session, VACUUM's compaction, and a table whose pg_class row this
 * transaction wrote, which it made or rewrote (ShouldUseReservedSegno()):
 * CREATE TABLE AS, REFRESH and every rewrite write one.
 */
static int
ao_insert_files(Relation rel)
{
	const GpCoreApi *core = GpCoreApiLookup();
	HeapTuple	tup;
	bool		made_here;

	if (gp_appendonly_insert_files <= 1 || ao_compaction_writer ||
		core == NULL || core->get_role() == GP_ROLE_UTILITY)
		return 1;
	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tup))
		return 1;
	made_here = TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tup->t_data));
	ReleaseSysCache(tup);
	return made_here ? 1 : gp_appendonly_insert_files;
}

/* A writer of rel, on a segment file of its own. */
static AoInsertState *
ao_insert_begin(Relation rel, void *owner, SubTransactionId subid)
{
	AoInsertState *st;
	MemoryContext old;

	old = MemoryContextSwitchTo(ao_dml_context());
	st = palloc0(sizeof(AoInsertState));
	st->relid = RelationGetRelid(rel);
	st->owner = owner;
	st->subid = subid;
	st->lead = st;
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
		st->colopts = palloc_array(AoOptions, st->natts);
		ao_column_options(rel, st->colopts);
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

/* The running query's writer of rel to append to now, begun if it has none. */
AoInsertState *
ao_insert_state(Relation rel)
{
	void	   *owner = ao_current_owner();
	SubTransactionId subid = GetCurrentSubTransactionId();
	AoInsertState *lead = ao_find_insert(RelationGetRelid(rel), owner, subid);

	if (lead == NULL)
	{
		MemoryContext old;

		lead = ao_insert_begin(rel, owner, subid);
		lead->nfiles = ao_insert_files(rel);
		old = MemoryContextSwitchTo(ao_dml_context());
		lead->files = list_make1(lead);
		MemoryContextSwitchTo(old);
	}
	return ao_insert_turn(list_nth(lead->files, lead->cur), rel);
}

/*
 * The writer of st's group the next row goes to: st, until it has taken
 * gp.appendonly_insert_files_tuples_range rows since the group turned to it;
 * then the next, begun on a segment file of its own while the group has
 * fewer than it may -- ao_choose_segfile() passes over its others' --
 * and after the last the first again, as Cloudberry's
 * get_insert_descriptor() turns.  Its test is Cloudberry's, an equality.
 */
AoInsertState *
ao_insert_turn(AoInsertState *st, Relation rel)
{
	AoInsertState *lead = st->lead;

	if (lead->nfiles <= 1 || st->range != gp_appendonly_insert_files_tuples_range)
		return st;
	st->range = 0;
	if (list_length(lead->files) < lead->nfiles)
	{
		AoInsertState *next = ao_insert_begin(rel, lead->owner, lead->subid);
		MemoryContext old = MemoryContextSwitchTo(ao_dml_context());

		next->lead = lead;
		lead->files = lappend(lead->files, next);
		MemoryContextSwitchTo(old);
	}
	lead->cur = (lead->cur + 1) % list_length(lead->files);
	return list_nth(lead->files, lead->cur);
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

		{
			AoOptions	o = st->columnar ? st->colopts[g] : st->opts;

			/* Cloudberry's test of a block too big compressed to be so */
			if (AO_FAULT("appendonly_skip_compression", rel) == GP_FAULT_SKIP)
				o.compresstype = AO_COMPRESS_NONE;
			len = ao_block_encode(raw, &o, st->block_first, st->block_nrows,
								  &st->out);
		}
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
	st->unseen = true;
	st->block_nrows = 0;
}

/* Append the row in slot, and give the slot its TID. */
void
ao_insert_slot(AoInsertState *st, Relation rel, TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext old = MemoryContextSwitchTo(ao_dml_context());
	Size		blocksize;

	(void) AO_FAULT("appendonly_insert", rel);
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
		/* A block ends where any column's reaches that column's size. */
		bool		full = false;

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
			if (st->cols[i].values.len >= st->colopts[i].blocksize)
				full = true;
		}
		blocksize = full ? (Size) st->opts.blocksize : 0;
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
	if (st->inserted == 0)
		st->first_rownum = st->next_rownum;
	st->next_rownum++;
	st->block_nrows++;
	st->inserted++;
	st->range++;
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

/*
 * A fetch of a row this backend's writer has not written yet: its block
 * written now, so that the fetch finds the row.  COPY fires its AFTER
 * triggers, which fetch the rows it inserted, before it says the insert is
 * over (finish_bulk_insert), where ExecutorFinish writes a statement's
 * blocks before its triggers fire.  Only a writer of this subtransaction's:
 * what it writes goes with the subtransaction if it aborts, as the writer
 * does.
 *
 * And a row of a block the writer wrote already, as each filled, whose
 * directory row the fetch's snapshot, taken in the same command, would not
 * see: the command counter goes on, once for the blocks written since it
 * last did here.
 */
bool
ao_pending_flush(Relation rel, ItemPointer tid)
{
	int64		rownum = AoTidRownum(tid);
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;

	foreach(lc, ao_inserts)
	{
		AoInsertState *st = lfirst(lc);

		if (st->relid != RelationGetRelid(rel) || st->subid != subid ||
			AoTidSegno(tid) != st->segno)
			continue;
		if (st->block_nrows > 0 && rownum >= st->block_first &&
			rownum < st->block_first + st->block_nrows)
		{
			MemoryContext old = MemoryContextSwitchTo(ao_dml_context());

			ao_flush_block(st, rel);
			MemoryContextSwitchTo(old);
			CommandCounterIncrement();
			st->unseen = false;
			return true;
		}
		if (st->unseen && st->inserted > 0 && rownum >= st->first_rownum &&
			rownum < st->next_rownum)
		{
			CommandCounterIncrement();
			st->unseen = false;
			return true;
		}
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

/* The range of ds's bitmaps rownum of segno is in, made if asked; or NULL. */
static AoDeleteRange *
ao_delete_range(AoDeleteState *ds, int segno, int64 rownum, bool create)
{
	int64		key = ((int64) segno << 40) | (rownum / AO_VISIMAP_ROWS);
	AoDeleteRange *range;
	bool		found;

	if (ds->last != NULL && ds->last->key == key)
		return ds->last;
	if (ds->ranges == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(int64);
		ctl.entrysize = sizeof(AoDeleteRange);
		ctl.hcxt = ao_dml_context();
		ds->ranges = hash_create("gp_ao deletions", 16, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	range = hash_search(ds->ranges, &key, create ? HASH_ENTER : HASH_FIND,
						&found);
	if (range == NULL)
		return NULL;
	if (!found)
		memset(range->bits, 0, sizeof(range->bits));
	ds->last = range;
	return range;
}

static bool
ao_delete_state_has(AoDeleteState *ds, int segno, int64 rownum)
{
	AoDeleteRange *range = ao_delete_range(ds, segno, rownum, false);
	int64		bit = rownum % AO_VISIMAP_ROWS;

	return range != NULL && (range->bits[bit / 8] & (1 << (bit % 8))) != 0;
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
	AoDeleteRange *range;
	int			segno = AoTidSegno(tid);
	int64		rownum = AoTidRownum(tid);
	int64		bit = rownum % AO_VISIMAP_ROWS;
	MemoryContext old;

	if (segno < 1 || segno > AO_MAX_SEGNO)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("TID (%u,%u) names no row of an append-optimized table",
						ItemPointerGetBlockNumberNoCheck(tid),
						ItemPointerGetOffsetNumberNoCheck(tid))));
	(void) AO_FAULT("appendonly_delete", rel);
	if (ao_deleted_by_this_command(rel, tid))
		return false;

	/*
	 * gp.select_invisible shows a scan the rows deleted too, and an UPDATE
	 * or DELETE under it reaches them: a row deleted already is left as it
	 * is, and is given no new version, as Cloudberry's visibility map leaves
	 * it (uao_dml's "we should not re-activate the deleted tuples").
	 */
	if (gp_select_invisible)
	{
		Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
		AoVisimap  *vm = ao_visimap_load(ao_storage_id(rel), segno, snapshot);
		bool		deleted = ao_visimap_is_deleted(vm, rownum);

		UnregisterSnapshot(snapshot);
		if (deleted)
			return false;
	}

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
	ds->rownums[segno][ds->n[segno]++] = rownum;
	range = ao_delete_range(ds, segno, rownum, true);
	range->bits[bit / 8] |= 1 << (bit % 8);
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

/*
 * Write a statement's deletions to the visibility map, and count each
 * segment file they touched as modified, as Cloudberry's modcount counts a
 * DELETE for an incremental backup to find: after the writers' own updates
 * of the rows, which the same statement's UPDATE may have made.
 */
static void
ao_finish_delete(AoDeleteState *ds)
{
	CommandCounterIncrement();
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
	{
		AoSegfile  *sf;

		if (ds->n[segno] == 0)
			continue;
		qsort(ds->rownums[segno], ds->n[segno], sizeof(int64), int64_cmp);
		ao_visimap_delete_rows(ds->storage_id, segno, ds->rownums[segno],
							   ds->n[segno]);
		sf = ao_segfile_read(ds->storage_id, segno, SnapshotSelf);
		if (sf != NULL)
		{
			sf->modcount++;
			ao_segfile_update(ds->storage_id, sf);
		}
	}

	/* no longer listed, and read no more */
	if (ds->ranges != NULL)
		hash_destroy(ds->ranges);
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
		if (ds->rownums[segno] != NULL)
			pfree(ds->rownums[segno]);
	pfree(ds);
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

/* The innermost query running now, which a kept fetch belongs to (ao_am.c). */
void *
ao_dml_current_query(void)
{
	return ao_current_owner();
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
			/*
			 * What no statement's end finished: its catalog rows need a
			 * snapshot, which a transaction ending has none of -- gp_ao's
			 * tables have TOAST tables, and heap_insert() asserts one.
			 */
			if (ao_inserts != NIL || ao_deletes != NIL)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				ao_dml_flush(InvalidOid);
				PopActiveSnapshot();
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/* a fetch's descriptor, in TopTransactionContext (ao_am.c) */
			ao_fetch_cache_reset();
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
		 * is past the end their segment files' rows say, for the next writer
		 * to write over: a fetch's block may be one of theirs (ao_am.c).  A
		 * segment file taken in the subtransaction was let go with its lock.
		 * Their memory goes with the transaction's.
		 */
		ao_fetch_cache_reset();
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
