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
 * ao_am.c
 *	  The table access methods ao_row and ao_column.
 *
 * A table by row reads each segment file's blocks one after another, each
 * row a MinimalTuple of the block.  A table by column reads a segment file
 * block of rows by block of rows, as the block directory lists them, and in
 * each only the columns the scan's plan node reads (O15): a count of rows
 * reads the directory alone.  A row's TID is its segment file and row
 * number (gp_ao.h); an index finds it through the directory, and a row is
 * deleted when the visibility map says so.
 *
 * UPDATE takes the old row from the plan (O20), since there is no fetching
 * one here that is cheaper than the scan that found it, and appends the new
 * one; DELETE marks the visibility map.  Both hold the table in
 * ExclusiveLock, as Cloudberry's do.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tableamext.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/storage.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "gp_ao.h"

static const TableAmRoutine ao_row_methods;
static const TableAmRoutine ao_column_methods;

bool
ao_is_ao_table(Relation rel)
{
	return rel->rd_tableam == &ao_row_methods ||
		rel->rd_tableam == &ao_column_methods;
}

/* ------------------------------------------------------------------------- */
/* Reading blocks                                                            */
/* ------------------------------------------------------------------------- */

typedef struct AoBlockReader
{
	MemoryContext cxt;			/* what its buffers are allocated in */
	uint32		filenum;
	bool		loaded;
	uint32		loaded_filenum;	/* of the loaded block */
	uint64		offset;			/* of the loaded block */
	uint64		next;			/* of the one after it */
	AoBlockHeader hdr;
	char	   *raw;
	Size		rawcap;
	char	   *stored;
	Size		storedcap;
	/* a table by row: where each row's MinimalTuple is in raw */
	uint32	   *rowoffs;
	int			rowcap;
	/* a table by column: the values */
	Datum	   *values;
	bool	   *isnull;
	int			valcap;
} AoBlockReader;

static void
ao_reader_init(AoBlockReader *rd, MemoryContext cxt)
{
	memset(rd, 0, sizeof(AoBlockReader));
	rd->cxt = cxt;
	rd->filenum = UINT32_MAX;
}

static void
ao_reader_reset(AoBlockReader *rd)
{
	rd->loaded = false;
}

/*
 * Read the block at offset of rd's file; att is the column it holds, or NULL
 * for a table by row.
 */
static void
ao_reader_load(Relation rel, AoBlockReader *rd, uint64 offset,
			   Form_pg_attribute att, BufferAccessStrategy strategy)
{
	AoBlockHeader hdr;

	if (rd->loaded && rd->offset == offset && rd->loaded_filenum == rd->filenum)
		return;

	ao_file_read(rel, rd->filenum, offset, (char *) &hdr, sizeof(hdr), strategy);
	ao_block_check_header(rel, rd->filenum, offset, &hdr);

	if (rd->storedcap < hdr.stored_len)
	{
		rd->stored = rd->stored ? repalloc(rd->stored, hdr.stored_len) :
			MemoryContextAlloc(rd->cxt, hdr.stored_len);
		rd->storedcap = hdr.stored_len;
	}
	if (rd->rawcap < Max(hdr.raw_len, 1))
	{
		Size		cap = Max(hdr.raw_len, 1);

		rd->raw = rd->raw ? repalloc(rd->raw, cap) :
			MemoryContextAlloc(rd->cxt, cap);
		rd->rawcap = cap;
	}
	ao_file_read(rel, rd->filenum, offset + AO_BLOCK_HEADER_SIZE, rd->stored,
				 hdr.stored_len, strategy);
	ao_block_decode(rd->stored, &hdr, rd->raw);

	if (att == NULL)
	{
		uint32		pos = 0;

		if (rd->rowcap < (int) hdr.nrows)
		{
			rd->rowcap = Max(hdr.nrows, 64);
			rd->rowoffs = rd->rowoffs ?
				repalloc_array(rd->rowoffs, uint32, rd->rowcap) :
				MemoryContextAlloc(rd->cxt,
								   sizeof(uint32) * rd->rowcap);
		}
		for (uint32 i = 0; i < hdr.nrows; i++)
		{
			MinimalTuple tup = (MinimalTuple) (rd->raw + pos);

			if (pos + MINIMAL_TUPLE_OFFSET > hdr.raw_len ||
				pos + tup->t_len > hdr.raw_len)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("block of rows " INT64_FORMAT " to " INT64_FORMAT " of \"%s\" ends in its row %u",
								hdr.first_rownum, hdr.first_rownum + hdr.nrows - 1,
								RelationGetRelationName(rel), i)));
			rd->rowoffs[i] = pos;
			pos += MAXALIGN(tup->t_len);
		}
	}
	else
	{
		if (rd->valcap < (int) hdr.nrows)
		{
			rd->valcap = Max(hdr.nrows, 64);
			rd->values = rd->values ?
				repalloc_array(rd->values, Datum, rd->valcap) :
				MemoryContextAlloc(rd->cxt,
								   sizeof(Datum) * rd->valcap);
			rd->isnull = rd->isnull ?
				repalloc_array(rd->isnull, bool, rd->valcap) :
				MemoryContextAlloc(rd->cxt,
								   sizeof(bool) * rd->valcap);
		}
		ao_column_decode(rd->raw, hdr.raw_len, att, hdr.nrows, rd->values,
						 rd->isnull);
	}

	rd->hdr = hdr;
	rd->loaded_filenum = rd->filenum;
	rd->offset = offset;
	rd->next = offset + AO_BLOCK_HEADER_SIZE + hdr.stored_len;
	rd->loaded = true;
}

/*
 * The metadata of rel is read with the snapshot a scan or a fetch is given,
 * where that is one MVCC can read; any other sees every version of a
 * segment file's row, and gets the latest instead.
 */
static Snapshot
ao_meta_snapshot(Snapshot snapshot)
{
	if (snapshot == NULL || snapshot == SnapshotAny ||
		snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE)
	{
		/*
		 * A parallel index build's participants may take no snapshot of
		 * their own: the one the leader shared, which each has as its
		 * active snapshot, sees what the build does.
		 */
		if (IsInParallelMode())
			return ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
		return GetLatestSnapshot();
	}
	return snapshot;
}

/* ------------------------------------------------------------------------- */
/* Fetching a row by its TID                                                 */
/* ------------------------------------------------------------------------- */

typedef struct AoFetchDescData
{
	IndexFetchTableData base;
	int64		storage_id;
	bool		columnar;
	int			natts;
	/* the block of rows last found, and the snapshot it was found with */
	bool		has_entry;
	Snapshot	entry_snapshot;
	AoBlkdirEntry entry;
	AoBlockReader row;
	AoBlockReader *cols;
	/* the visibility map last read */
	int			vm_segno;
	Snapshot	vm_snapshot;
	AoVisimap  *vm;
	/* which segment files hold rows, as the snapshot last used sees them */
	bool		sf_loaded;
	Snapshot	sf_snapshot;
	bool		sf_live[AO_MAX_SEGNO + 1];
	MemoryContext cxt;
} AoFetchDescData;

typedef AoFetchDescData *AoFetchDesc;

static AoFetchDesc
ao_fetch_begin(Relation rel)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "gp_ao fetch",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	AoFetchDesc fd = palloc0_object(AoFetchDescData);

	fd->base.rel = rel;
	fd->cxt = cxt;
	fd->storage_id = ao_storage_id(rel);
	fd->columnar = rel->rd_tableam == &ao_column_methods;
	fd->natts = RelationGetDescr(rel)->natts;
	fd->vm_segno = -1;
	ao_reader_init(&fd->row, cxt);
	if (fd->columnar)
	{
		fd->cols = palloc_array(AoBlockReader, fd->natts);
		for (int i = 0; i < fd->natts; i++)
			ao_reader_init(&fd->cols[i], cxt);
	}
	MemoryContextSwitchTo(old);
	return fd;
}

static void
ao_fetch_end(AoFetchDesc fd)
{
	MemoryContextDelete(fd->cxt);
}

/*
 * Is row rownum of segment file segno visible to snapshot, and where is its
 * block: fd->entry, on true.  A placeholder is a block a writer is building:
 * *placeholder says so, and there is nothing to read in it yet.
 *
 * A dirty snapshot -- a unique index's probe, an exclusion constraint's --
 * also says whom to wait for, in its xmin, as a heap's visibility test sets
 * it: the transaction that wrote the block the row is in, or the
 * placeholder, while it is in progress.  Every row of gp_ao's tables read
 * with it sets and clears that, so it is taken where the block's row is
 * found and set again at the end; nothing is kept between calls, since a
 * dirty snapshot's answer changes as others commit.
 */
static bool
ao_fetch_locate(AoFetchDesc fd, int segno, int64 rownum, Snapshot snapshot,
				bool *placeholder)
{
	Snapshot	msnap = ao_meta_snapshot(snapshot);
	bool		dirty = (snapshot != NULL &&
						 snapshot->snapshot_type == SNAPSHOT_DIRTY);
	TransactionId xwait = InvalidTransactionId;
	bool		live;

	*placeholder = false;

	/*
	 * A segment file a VACUUM compacted holds no row for a snapshot that sees
	 * the VACUUM: its rows are in another, under TIDs of their own, until the
	 * next VACUUM removes the index entries that still name them here.  A
	 * snapshot that sees two versions of its row, a dirty one while the
	 * VACUUM is at work, sees it in use where either says so.
	 */
	if (dirty || !fd->sf_loaded || fd->sf_snapshot != snapshot)
	{
		AoSegfile  *segfiles;
		int			nsegfiles;

		segfiles = ao_segfiles_read(fd->storage_id, msnap, &nsegfiles);
		memset(fd->sf_live, 0, sizeof(fd->sf_live));
		for (int i = 0; i < nsegfiles; i++)
			if (segfiles[i].state == AO_SEGFILE_DEFAULT)
				fd->sf_live[segfiles[i].segno] = true;
		pfree(segfiles);
		fd->sf_loaded = true;
		fd->sf_snapshot = snapshot;
	}
	if (!fd->sf_live[segno])
	{
		live = false;
		goto done;
	}

	if (dirty ||
		!(fd->has_entry && fd->entry_snapshot == snapshot &&
		  fd->entry.segno == segno && fd->entry.nrows >= 0 &&
		  rownum >= fd->entry.first_row &&
		  rownum < fd->entry.first_row + fd->entry.nrows))
	{
		MemoryContext old = MemoryContextSwitchTo(fd->cxt);
		bool		found;

		fd->has_entry = false;
		found = ao_blkdir_lookup(fd->storage_id, segno, rownum, msnap,
								 &fd->entry);
		MemoryContextSwitchTo(old);
		if (!found)
		{
			live = false;
			goto done;
		}
		if (dirty)
			xwait = snapshot->xmin;
		fd->has_entry = true;
		fd->entry_snapshot = snapshot;
	}
	if (fd->entry.nrows < 0)
	{
		*placeholder = true;
		fd->has_entry = false;
		live = true;
		goto done;
	}

	if (dirty || fd->vm_segno != segno || fd->vm_snapshot != snapshot)
	{
		MemoryContext old = MemoryContextSwitchTo(fd->cxt);

		fd->vm = ao_visimap_load(fd->storage_id, segno, msnap);
		fd->vm_segno = segno;
		fd->vm_snapshot = snapshot;
		MemoryContextSwitchTo(old);
	}
	live = gp_select_invisible || !ao_visimap_is_deleted(fd->vm, rownum);

done:
	if (dirty)
	{
		snapshot->xmin = live ? xwait : InvalidTransactionId;
		snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	return live;
}

/* The row the located block has at rownum, into slot. */
static void
ao_fetch_store(AoFetchDesc fd, int segno, int64 rownum, TupleTableSlot *slot)
{
	Relation	rel = fd->base.rel;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	int			i = (int) (rownum - fd->entry.first_row);

	if (!fd->columnar)
	{
		fd->row.filenum = AoFileNum(segno, 0);
		ao_reader_load(rel, &fd->row, fd->entry.offsets[0], NULL, NULL);
		ExecStoreMinimalTuple((MinimalTuple) (fd->row.raw + fd->row.rowoffs[i]),
							  slot, false);
	}
	else
	{
		ExecClearTuple(slot);
		for (int a = 0; a < desc->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, a);

			if (att->attisdropped)
			{
				slot->tts_values[a] = (Datum) 0;
				slot->tts_isnull[a] = true;
				continue;
			}
			if (a >= fd->natts || a >= fd->entry.noffsets ||
				fd->entry.offsets[a] < 0)
			{
				slot->tts_values[a] = getmissingattr(desc, a + 1,
													 &slot->tts_isnull[a]);
				continue;
			}
			fd->cols[a].filenum = AoFileNum(segno, a + 1);
			ao_reader_load(rel, &fd->cols[a], fd->entry.offsets[a], att, NULL);
			slot->tts_values[a] = fd->cols[a].values[i];
			slot->tts_isnull[a] = fd->cols[a].isnull[i];
		}
		ExecStoreVirtualTuple(slot);
	}
	AoTidSet(&slot->tts_tid, segno, rownum);
	slot->tts_tableOid = RelationGetRelid(rel);
}

static bool
ao_fetch_row(AoFetchDesc fd, ItemPointer tid, Snapshot snapshot,
			 TupleTableSlot *slot)
{
	int			segno = AoTidSegno(tid);
	int64		rownum = AoTidRownum(tid);
	bool		placeholder;

	if (segno < 1 || segno > AO_MAX_SEGNO || rownum < 1)
		return false;
	if (!ao_fetch_locate(fd, segno, rownum, snapshot, &placeholder) ||
		placeholder)
		return false;
	ao_fetch_store(fd, segno, rownum, slot);
	return true;
}

/* ------------------------------------------------------------------------- */
/* Scans                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct AoParallelScanDescData
{
	ParallelTableScanDescData base;
	pg_atomic_uint32 next_segfile;
} AoParallelScanDescData;

typedef struct AoScanDescData
{
	TableScanDescData rs_base;
	MemoryContext cxt;
	int64		storage_id;
	bool		columnar;
	int			natts;
	BufferAccessStrategy strategy;
	/* the segment files the snapshot sees, and the one being read */
	bool		started;
	AoSegfile  *segfiles;
	int			nsegfiles;
	int			cursf;
	AoVisimap  *vm;
	/* a table by row */
	AoBlockReader row;
	uint64		eof;
	uint32		rowidx;
	/* a table by column */
	AoBlkdirScan *bds;
	AoBlkdirEntry entry;
	bool		entry_valid;
	int			colidx;
	AoBlockReader *cols;
	bool	   *needed;
	/* the one segment file to read, for VACUUM; 0 for all */
	int			only_segno;
	/* TABLESAMPLE: a row is a block, numbered as the scan reaches it */
	int64		sample_total;	/* rows the segment files hold; -1 unknown */
	int64		sample_next;	/* the number of the row the scan reads next */
	int64		sample_target;	/* the row sampled */
	bool		sample_done;	/* the scan has read every row */
	BlockNumber sample_hash_next;	/* BERNOULLI's next row to hash */
	/* a bitmap scan */
	AoFetchDesc fetch;
	BlockNumber bm_block;
	OffsetNumber bm_offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			bm_noffsets;
	int			bm_idx;
} AoScanDescData;

typedef AoScanDescData *AoScanDesc;

static TableScanDesc
ao_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
			  ParallelTableScanDesc pscan, uint32 flags)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "gp_ao scan",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	AoScanDesc scan = palloc0_object(AoScanDescData);

	RelationIncrementReferenceCount(rel);
	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;
	if (nkeys > 0)
	{
		scan->rs_base.rs_key = palloc_array(ScanKeyData, nkeys);
		memcpy(scan->rs_base.rs_key, key, sizeof(ScanKeyData) * nkeys);
	}
	scan->cxt = cxt;
	scan->columnar = rel->rd_tableam == &ao_column_methods;
	scan->natts = RelationGetDescr(rel)->natts;
	scan->cursf = -1;
	if (flags & SO_ALLOW_STRAT)
		scan->strategy = GetAccessStrategy(BAS_BULKREAD);
	ao_reader_init(&scan->row, cxt);
	if (scan->columnar)
	{
		scan->cols = palloc_array(AoBlockReader, scan->natts);
		scan->needed = palloc_array(bool, scan->natts);
		for (int i = 0; i < scan->natts; i++)
		{
			ao_reader_init(&scan->cols[i], cxt);
			scan->needed[i] = !TupleDescAttr(RelationGetDescr(rel), i)->attisdropped;
		}
	}
	scan->sample_total = -1;
	/* A snapshot SO_TEMP_SNAPSHOT names, its caller registered for us. */
	MemoryContextSwitchTo(old);
	return (TableScanDesc) scan;
}

/* The segment files, read once the scan is asked for its first row. */
static void
ao_scan_start(AoScanDesc scan)
{
	MemoryContext old = MemoryContextSwitchTo(scan->cxt);
	Relation	rel = scan->rs_base.rs_rd;

	scan->storage_id = ao_storage_id(rel);
	scan->segfiles = ao_segfiles_read(scan->storage_id,
									  ao_meta_snapshot(scan->rs_base.rs_snapshot),
									  &scan->nsegfiles);
	scan->started = true;
	MemoryContextSwitchTo(old);
}

static void
ao_scan_close_segfile(AoScanDesc scan)
{
	if (scan->bds)
	{
		ao_blkdir_scan_end(scan->bds);
		scan->bds = NULL;
	}
	scan->entry_valid = false;
	ao_reader_reset(&scan->row);
	if (scan->cols)
		for (int i = 0; i < scan->natts; i++)
			ao_reader_reset(&scan->cols[i]);
}

/* On to the next segment file this scan reads; false when there is none. */
static bool
ao_scan_next_segfile(AoScanDesc scan)
{
	MemoryContext old;
	AoSegfile  *sf;

	ao_scan_close_segfile(scan);
	for (;;)
	{
		if (scan->rs_base.rs_parallel)
		{
			AoParallelScanDescData *ps = (AoParallelScanDescData *) scan->rs_base.rs_parallel;

			scan->cursf = (int) pg_atomic_fetch_add_u32(&ps->next_segfile, 1);
		}
		else
			scan->cursf++;
		if (scan->cursf >= scan->nsegfiles)
			return false;
		sf = &scan->segfiles[scan->cursf];
		/* Its rows were moved elsewhere by a VACUUM this snapshot sees. */
		if (sf->state == AO_SEGFILE_AWAITING_DROP || sf->tupcount == 0)
			continue;
		if (scan->only_segno != 0 && sf->segno != scan->only_segno)
			continue;
		break;
	}

	old = MemoryContextSwitchTo(scan->cxt);
	scan->vm = ao_visimap_load(scan->storage_id, sf->segno,
							   ao_meta_snapshot(scan->rs_base.rs_snapshot));
	if (scan->columnar)
		scan->bds = ao_blkdir_scan_begin(scan->storage_id, sf->segno,
										 ao_meta_snapshot(scan->rs_base.rs_snapshot));
	else
	{
		scan->row.filenum = AoFileNum(sf->segno, 0);
		scan->row.next = 0;
		scan->eof = sf->eof[0];
		scan->rowidx = 0;
	}
	MemoryContextSwitchTo(old);
	return true;
}

static bool
ao_scan_keys_ok(AoScanDesc scan, TupleTableSlot *slot)
{
	for (int i = 0; i < scan->rs_base.rs_nkeys; i++)
	{
		ScanKey		key = &scan->rs_base.rs_key[i];
		bool		isnull;
		Datum		value = slot_getattr(slot, key->sk_attno, &isnull);

		if (isnull || !DatumGetBool(FunctionCall2Coll(&key->sk_func,
													  key->sk_collation,
													  value, key->sk_argument)))
			return false;
	}
	return true;
}

static bool
ao_row_getnextslot(AoScanDesc scan, TupleTableSlot *slot)
{
	Relation	rel = scan->rs_base.rs_rd;

	for (;;)
	{
		if (scan->row.loaded && scan->rowidx < scan->row.hdr.nrows)
		{
			uint32		i = scan->rowidx++;
			int64		rownum = scan->row.hdr.first_rownum + i;
			int			segno = scan->segfiles[scan->cursf].segno;

			if (!gp_select_invisible && ao_visimap_is_deleted(scan->vm, rownum))
				continue;
			ExecStoreMinimalTuple((MinimalTuple) (scan->row.raw + scan->row.rowoffs[i]),
								  slot, false);
			AoTidSet(&slot->tts_tid, segno, rownum);
			slot->tts_tableOid = RelationGetRelid(rel);
			if (scan->rs_base.rs_nkeys > 0 && !ao_scan_keys_ok(scan, slot))
				continue;
			return true;
		}
		if (scan->cursf >= 0 && scan->cursf < scan->nsegfiles &&
			scan->row.next < scan->eof)
		{
			ao_reader_load(rel, &scan->row, scan->row.next, NULL, scan->strategy);
			scan->rowidx = 0;
			continue;
		}
		if (!ao_scan_next_segfile(scan))
			return false;
	}
}

/*
 * A table by column is read into a virtual slot, a column's values decoded
 * with the slot's descriptor, as a heap tuple is deformed with its slot's:
 * ALTER TABLE's rewrite reads the table through a slot of the columns as
 * they were, when the relation already says a column is of its new type.
 */
static bool
ao_column_getnextslot(AoScanDesc scan, TupleTableSlot *slot)
{
	Relation	rel = scan->rs_base.rs_rd;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	int			natts = Min(desc->natts, scan->natts);

	for (;;)
	{
		if (scan->entry_valid && scan->colidx < scan->entry.nrows)
		{
			int			i = scan->colidx++;
			int64		rownum = scan->entry.first_row + i;
			int			segno = scan->segfiles[scan->cursf].segno;

			if (!gp_select_invisible && ao_visimap_is_deleted(scan->vm, rownum))
				continue;
			ExecClearTuple(slot);
			for (int a = natts; a < desc->natts; a++)
				slot->tts_values[a] = getmissingattr(desc, a + 1,
													 &slot->tts_isnull[a]);
			for (int a = 0; a < natts; a++)
			{
				if (!scan->needed[a])
				{
					slot->tts_values[a] = (Datum) 0;
					slot->tts_isnull[a] = true;
				}
				else if (a >= scan->entry.noffsets || scan->entry.offsets[a] < 0)
					slot->tts_values[a] = getmissingattr(desc, a + 1,
														 &slot->tts_isnull[a]);
				else
				{
					slot->tts_values[a] = scan->cols[a].values[i];
					slot->tts_isnull[a] = scan->cols[a].isnull[i];
				}
			}
			ExecStoreVirtualTuple(slot);
			AoTidSet(&slot->tts_tid, segno, rownum);
			slot->tts_tableOid = RelationGetRelid(rel);
			if (scan->rs_base.rs_nkeys > 0 && !ao_scan_keys_ok(scan, slot))
				continue;
			return true;
		}
		if (scan->bds != NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(scan->cxt);
			bool		more = ao_blkdir_scan_next(scan->bds, &scan->entry);

			MemoryContextSwitchTo(old);
			if (more)
			{
				int			segno = scan->segfiles[scan->cursf].segno;

				for (int a = 0; a < natts; a++)
				{
					if (!scan->needed[a] || a >= scan->entry.noffsets ||
						scan->entry.offsets[a] < 0)
						continue;
					scan->cols[a].filenum = AoFileNum(segno, a + 1);
					ao_reader_load(rel, &scan->cols[a], scan->entry.offsets[a],
								   TupleDescAttr(desc, a), scan->strategy);
				}
				scan->entry_valid = true;
				scan->colidx = 0;
				continue;
			}
		}
		if (!ao_scan_next_segfile(scan))
			return false;
	}
}

static bool
ao_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
					TupleTableSlot *slot)
{
	AoScanDesc	scan = (AoScanDesc) sscan;

	if (direction != ForwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("backward scan is not supported on append-optimized tables")));
	if (!scan->started)
		ao_scan_start(scan);
	if (scan->columnar ? ao_column_getnextslot(scan, slot) :
		ao_row_getnextslot(scan, slot))
	{
		pgstat_count_heap_getnext(scan->rs_base.rs_rd);
		return true;
	}
	ExecClearTuple(slot);
	return false;
}

static void
ao_scan_rescan(TableScanDesc sscan, ScanKeyData *key, bool set_params,
			   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	AoScanDesc	scan = (AoScanDesc) sscan;

	if (key && scan->rs_base.rs_nkeys > 0)
		memcpy(scan->rs_base.rs_key, key,
			   sizeof(ScanKeyData) * scan->rs_base.rs_nkeys);
	ao_scan_close_segfile(scan);
	scan->cursf = -1;
	scan->started = false;
	scan->sample_total = -1;
	scan->sample_next = 0;
	scan->sample_done = false;
	scan->sample_hash_next = 0;
}

static void
ao_scan_end(TableScanDesc sscan)
{
	AoScanDesc	scan = (AoScanDesc) sscan;

	ao_scan_close_segfile(scan);
	if (scan->fetch)
		ao_fetch_end(scan->fetch);
	if (scan->strategy)
		FreeAccessStrategy(scan->strategy);
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);
	RelationDecrementReferenceCount(scan->rs_base.rs_rd);
	MemoryContextDelete(scan->cxt);
}

/* VACUUM's scan of the one segment file it compacts. */
void
ao_scan_set_segno(TableScanDesc sscan, int segno)
{
	((AoScanDesc) sscan)->only_segno = segno;
}

/*
 * O15: the scan's plan node says which columns it reads.  A table by row
 * reads every one of each block in any case; a table by column reads these
 * alone, and no column at all for a count of rows.
 */
static void
ao_scan_extractcolumns(TableScanDesc sscan, PlanState *ps)
{
	AoScanDesc	scan = (AoScanDesc) sscan;
	Scan	   *plan = (Scan *) ps->plan;
	Bitmapset  *cols = NULL;
	int			col = -1;

	if (!scan->columnar)
		return;

	pull_varattnos((Node *) plan->plan.targetlist, plan->scanrelid, &cols);
	pull_varattnos((Node *) plan->plan.qual, plan->scanrelid, &cols);
	if (IsA(plan, BitmapHeapScan))
		pull_varattnos((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
					   plan->scanrelid, &cols);

	/* A whole-row reference reads every column. */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, cols))
		return;

	for (int i = 0; i < scan->natts; i++)
		scan->needed[i] = false;
	while ((col = bms_next_member(cols, col)) >= 0)
	{
		int			attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno > 0 && attno <= scan->natts &&
			!TupleDescAttr(RelationGetDescr(scan->rs_base.rs_rd), attno - 1)->attisdropped)
			scan->needed[attno - 1] = true;
	}
}

/* ---- parallel scans: each participant takes whole segment files ---------- */

static Size
ao_parallelscan_estimate(Relation rel)
{
	return sizeof(AoParallelScanDescData);
}

static Size
ao_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	AoParallelScanDescData *ps = (AoParallelScanDescData *) pscan;

	ps->base.phs_locator = rel->rd_locator;
	ps->base.phs_syncscan = false;
	pg_atomic_init_u32(&ps->next_segfile, 0);
	return sizeof(AoParallelScanDescData);
}

static void
ao_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	AoParallelScanDescData *ps = (AoParallelScanDescData *) pscan;

	pg_atomic_write_u32(&ps->next_segfile, 0);
}

/* ---- bitmap scans ------------------------------------------------------- */

static bool
ao_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot,
						  bool *recheck, uint64 *lossy_pages,
						  uint64 *exact_pages)
{
	AoScanDesc	scan = (AoScanDesc) sscan;
	Relation	rel = scan->rs_base.rs_rd;

	if (scan->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(scan->cxt);

		scan->fetch = ao_fetch_begin(rel);
		MemoryContextSwitchTo(old);
	}

	for (;;)
	{
		while (scan->bm_idx < scan->bm_noffsets)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, scan->bm_block, scan->bm_offsets[scan->bm_idx++]);
			if (ao_fetch_row(scan->fetch, &tid, scan->rs_base.rs_snapshot, slot))
			{
				if (scan->columnar)
				{
					/* Only the columns the plan reads mean anything. */
					for (int a = 0; a < scan->natts; a++)
						if (!scan->needed[a])
						{
							slot->tts_values[a] = (Datum) 0;
							slot->tts_isnull[a] = true;
						}
				}
				pgstat_count_heap_fetch(rel);
				return true;
			}
		}

		{
			TBMIterateResult tbmres;

			if (!tbm_iterate(&scan->rs_base.st.rs_tbmiterator, &tbmres))
				return false;
			scan->bm_block = tbmres.blockno;
			scan->bm_idx = 0;
			*recheck = tbmres.recheck;
			if (tbmres.lossy)
			{
				for (int i = 0; i < AO_ROWS_PER_TIDBLOCK; i++)
					scan->bm_offsets[i] = (OffsetNumber) (i + 1);
				scan->bm_noffsets = AO_ROWS_PER_TIDBLOCK;
				(*lossy_pages)++;
			}
			else
			{
				scan->bm_noffsets = tbm_extract_page_tuple(&tbmres,
														   scan->bm_offsets,
														   TBM_MAX_TUPLES_PER_PAGE);
				(*exact_pages)++;
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Index fetches                                                             */
/* ------------------------------------------------------------------------- */

static IndexFetchTableData *
ao_index_fetch_begin(Relation rel, uint32 flags)
{
	return (IndexFetchTableData *) ao_fetch_begin(rel);
}

static void
ao_index_fetch_reset(IndexFetchTableData *scan)
{
}

static void
ao_index_fetch_end(IndexFetchTableData *scan)
{
	ao_fetch_end((AoFetchDesc) scan);
}

static bool
ao_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid,
					 Snapshot snapshot, TupleTableSlot *slot,
					 bool *call_again, bool *all_dead)
{
	*call_again = false;
	if (all_dead)
		*all_dead = false;
	return ao_fetch_row((AoFetchDesc) scan, tid, snapshot, slot);
}

/*
 * O16: a unique index's probe.  A row this backend is writing counts, and
 * so does a block another is writing, whose placeholder a dirty snapshot
 * finds with its writer to wait for; a row this command deleted does not.
 */
static bool
ao_index_unique_check(Relation rel, ItemPointer tid, Snapshot snapshot,
					  bool *all_dead)
{
	AoFetchDesc fd;
	bool		placeholder;
	bool		live;

	if (all_dead)
		*all_dead = false;
	/* As ao_fetch_locate() sets them: this backend's rows wait for no one. */
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	if (ao_pending_fetch(rel, tid, NULL))
		return true;
	if (ao_deleted_pending(rel, tid))
		return false;
	if (AoTidSegno(tid) < 1 || AoTidSegno(tid) > AO_MAX_SEGNO)
		return false;

	fd = ao_fetch_begin(rel);
	live = ao_fetch_locate(fd, AoTidSegno(tid), AoTidRownum(tid), snapshot,
						   &placeholder);
	ao_fetch_end(fd);
	return live;
}

static bool
ao_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	AoFetchDesc fd;
	bool		found;

	(void) ao_pending_flush(rel, tid);
	fd = ao_fetch_begin(rel);
	found = ao_fetch_row(fd, tid, snapshot, slot);

	/* The slot's values point into the fetch's memory: keep a copy. */
	if (found)
		ExecMaterializeSlot(slot);
	ao_fetch_end(fd);
	return found;
}

static bool
ao_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	int			segno = AoTidSegno(tid);

	return segno >= 1 && segno <= AO_MAX_SEGNO && AoTidRownum(tid) >= 1;
}

static void
ao_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	/* A row of an append-optimized table has no newer version. */
}

static bool
ao_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							Snapshot snapshot)
{
	AoFetchDesc fd = ao_fetch_begin(rel);
	bool		placeholder;
	bool		visible;

	visible = ao_fetch_locate(fd, AoTidSegno(&slot->tts_tid),
							  AoTidRownum(&slot->tts_tid), snapshot,
							  &placeholder) && !placeholder;
	ao_fetch_end(fd);
	return visible;
}

/*
 * An index's bottom-up deletion asks which of its entries are of rows no
 * snapshot sees: none, as far as this method can say without reading them,
 * and VACUUM removes an append-optimized table's index entries when it
 * recycles their segment file.  The entries the index knows dead already
 * are its to delete; there are none, since no fetch reports a row dead.
 */
static TransactionId
ao_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	int			n = 0;

	for (int i = 0; i < delstate->ndeltids; i++)
		if (delstate->status[delstate->deltids[i].id].knowndeletable)
			delstate->deltids[n++] = delstate->deltids[i];
	delstate->ndeltids = n;
	return InvalidTransactionId;
}

/* ------------------------------------------------------------------------- */
/* Writing                                                                   */
/* ------------------------------------------------------------------------- */

static void
ao_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				uint32 options, BulkInsertStateData *bistate)
{
	ao_insert_slot(ao_insert_state(rel), rel, slot);
	pgstat_count_heap_insert(rel, 1);
}

static void
ao_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	AoInsertState *st = ao_insert_state(rel);

	/* each row to the writer its group turns to, as a row inserted alone */
	for (int i = 0; i < nslots; i++)
	{
		st = ao_insert_turn(st, rel);
		ao_insert_slot(st, rel, slots[i]);
	}
	pgstat_count_heap_insert(rel, nslots);
}

static void
ao_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
							CommandId cid, uint32 options,
							BulkInsertStateData *bistate, uint32 specToken)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("INSERT ... ON CONFLICT is not supported on append-optimized table \"%s\"",
					RelationGetRelationName(rel))));
}

static void
ao_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
							  uint32 specToken, bool succeeded)
{
}

static void
ao_finish_bulk_insert(Relation rel, uint32 options)
{
	ao_dml_flush(RelationGetRelid(rel));
}

/*
 * UPDATE and DELETE of an append-optimized table hold it in ExclusiveLock,
 * as Cloudberry's do, so no one else deletes its rows: what a delete meets
 * is only what this command did already.
 */
static void
ao_lock_for_write(Relation rel)
{
	if (!CheckRelationLockedByMe(rel, ExclusiveLock, true))
		LockRelationOid(RelationGetRelid(rel), ExclusiveLock);
}

static TM_Result
ao_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options,
				Snapshot snapshot, Snapshot crosscheck, bool wait,
				TM_FailureData *tmfd)
{
	ao_lock_for_write(rel);
	if (!ao_delete_row(rel, tid))
	{
		tmfd->ctid = *tid;
		tmfd->xmax = GetCurrentTransactionId();
		tmfd->cmax = cid;
		return TM_SelfModified;
	}
	pgstat_count_heap_delete(rel);
	return TM_Ok;
}

static TM_Result
ao_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				CommandId cid, uint32 options, Snapshot snapshot,
				Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
				LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	ItemPointerData old = *otid;

	ao_lock_for_write(rel);
	(void) AO_FAULT("appendonly_update", rel);
	*lockmode = LockTupleExclusive;
	if (!ao_delete_row(rel, &old))
	{
		tmfd->ctid = old;
		tmfd->xmax = GetCurrentTransactionId();
		tmfd->cmax = cid;
		*update_indexes = TU_None;
		return TM_SelfModified;
	}
	ao_insert_slot(ao_insert_state(rel), rel, slot);
	*update_indexes = TU_All;
	pgstat_count_heap_update(rel, false, false);
	return TM_Ok;
}

/*
 * A row lock: the query that asks holds the table in a mode ExclusiveLock
 * conflicts with, so no UPDATE or DELETE can change the row while it runs;
 * what is left is to say the row is there.
 */
static TM_Result
ao_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
			  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
			  LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	if (ao_deleted_by_this_command(rel, tid))
	{
		tmfd->ctid = *tid;
		tmfd->xmax = GetCurrentTransactionId();
		tmfd->cmax = cid;
		return TM_SelfModified;
	}
	if (!ao_tuple_fetch_row_version(rel, tid, snapshot, slot))
		return TM_Deleted;
	return TM_Ok;
}

/* ------------------------------------------------------------------------- */
/* DDL                                                                       */
/* ------------------------------------------------------------------------- */

static void
ao_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator,
								char persistence, TransactionId *freezeXid,
								MultiXactId *minmulti)
{
	SMgrRelation srel;

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	/*
	 * TRUNCATE gives the table new files; the rows of the old ones go, and
	 * come back if it rolls back.  A new relation has no files yet.
	 */
	if (!RelFileLocatorEquals(rel->rd_locator, *newrlocator) &&
		smgrexists(RelationGetSmgr(rel), MAIN_FORKNUM) &&
		smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM) > 0)
	{
		ao_dml_flush(RelationGetRelid(rel));
		ao_meta_delete_storage(ao_storage_id(rel));
	}

	srel = RelationCreateStorage(*newrlocator, persistence, true);
	(void) ao_storage_init(srel, *newrlocator, persistence,
						   rel->rd_tableam == &ao_column_methods);
	smgrclose(srel);
}

static void
ao_relation_nontransactional_truncate(Relation rel)
{
	ao_dml_flush(RelationGetRelid(rel));
	ao_meta_delete_storage(ao_storage_id(rel));
	RelationTruncate(rel, 0);
	ao_storage_forget(rel);
	(void) ao_storage_init(RelationGetSmgr(rel), rel->rd_locator,
						   rel->rd_rel->relpersistence,
						   rel->rd_tableam == &ao_column_methods);
}

static void
ao_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	ao_dml_flush(RelationGetRelid(rel));
	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence,
								   true);
	ao_copy_storage(rel, dstrel, *newrlocator, rel->rd_rel->relpersistence);
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * CLUSTER and VACUUM FULL: the rows the snapshot sees, into the new table's
 * files, in the order they are in; an append-optimized table is not kept in
 * an index's order.
 */
static void
ao_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
							 Relation OldIndex, bool use_sort,
							 TransactionId OldestXmin, Snapshot snapshot,
							 TransactionId *xid_cutoff,
							 MultiXactId *multi_cutoff, double *num_tuples,
							 double *tups_vacuumed, double *tups_recently_dead)
{
	TableScanDesc scan;
	TupleTableSlot *src;
	TupleTableSlot *dst;
	AoInsertState *st = NULL;
	bool		ao_dest = ao_is_ao_table(NewTable);
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());

	*num_tuples = 0;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;

	src = table_slot_create(OldTable, NULL);
	dst = table_slot_create(NewTable, NULL);
	scan = table_beginscan(OldTable, snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, src))
	{
		CHECK_FOR_INTERRUPTS();
		ExecCopySlot(dst, src);
		if (ao_dest)
		{
			if (st == NULL)
				st = ao_insert_state(NewTable);
			ao_insert_slot(st, NewTable, dst);
		}
		else
			table_tuple_insert(NewTable, dst, GetCurrentCommandId(true), 0, NULL);
		*num_tuples += 1;
	}
	table_endscan(scan);
	UnregisterSnapshot(snap);
	ExecDropSingleTupleTableSlot(src);
	ExecDropSingleTupleTableSlot(dst);
	if (ao_dest)
	{
		ao_dml_flush(RelationGetRelid(NewTable));
		/* No transaction IDs in its rows, so none to freeze: see gp_ao.c. */
		*xid_cutoff = InvalidTransactionId;
		*multi_cutoff = InvalidMultiXactId;
	}
}

static bool
ao_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	/* ANALYZE samples an append-optimized table itself: gp_ao.c. */
	return false;
}

static bool
ao_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
						   double *deadrows, TupleTableSlot *slot)
{
	return false;
}

/*
 * The rows of an index's build: every row the latest snapshot sees, in row
 * order, those of blocks start_blockno to start_blockno + numblocks - 1 of
 * their TIDs where BRIN asks for a range.
 */
static double
ao_index_build_range_scan(Relation table_rel, Relation index_rel,
						  IndexInfo *index_info, bool allow_sync,
						  bool anyvisible, bool progress,
						  BlockNumber start_blockno, BlockNumber numblocks,
						  IndexBuildCallback callback, void *callback_state,
						  TableScanDesc scan)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples = 0;
	Snapshot	snapshot = NULL;
	bool		own_scan = (scan == NULL);
	uint64		end_blockno = (uint64) start_blockno + numblocks;

	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	if (own_scan)
	{
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(table_rel, snapshot, 0, NULL, SO_NONE);
	}

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&slot->tts_tid);

		CHECK_FOR_INTERRUPTS();
		if (numblocks != InvalidBlockNumber &&
			(blk < start_blockno || (uint64) blk >= end_blockno))
			continue;

		reltuples += 1;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;

		FormIndexDatum(index_info, slot, estate, values, isnull);
		callback(index_rel, &slot->tts_tid, values, isnull, true,
				 callback_state);
	}

	/* A scan given, a parallel build's, is ours to end too, as heap's is. */
	table_endscan(scan);
	if (own_scan)
		UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	/* A snapshot older than the build may still see rows it left out. */
	index_info->ii_BrokenHotChain = true;
	return reltuples;
}

static void
ao_index_validate_scan(Relation table_rel, Relation index_rel,
					   IndexInfo *index_info, Snapshot snapshot,
					   ValidateIndexState *state)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("CREATE INDEX CONCURRENTLY is not supported on append-optimized table \"%s\"",
					RelationGetRelationName(table_rel))));
}

static bool
ao_relation_needs_toast_table(Relation rel)
{
	/* A value of any length is stored in the table's own blocks. */
	return false;
}

static Oid
ao_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * The planner's estimate: the pages the relation has, and the rows the
 * segment files' rows say it has, less the deleted, where ANALYZE has not
 * said more lately.
 */
static void
ao_relation_estimate_size(Relation rel, int32 *attr_widths,
						  BlockNumber *pages, double *tuples,
						  double *allvisfrac)
{
	BlockNumber curpages = RelationGetNumberOfBlocks(rel);
	AoSegfile  *segfiles;
	int			nsegfiles;
	double		rows = 0;
	int64		storage_id;

	*pages = curpages;
	*allvisfrac = 0;
	if (curpages <= 1)
	{
		*tuples = 0;
		return;
	}

	storage_id = ao_storage_id(rel);
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &nsegfiles);
	for (int i = 0; i < nsegfiles; i++)
		if (segfiles[i].state == AO_SEGFILE_DEFAULT)
			rows += segfiles[i].tupcount;
	if (rel->rd_rel->reltuples >= 0 && rel->rd_rel->relpages > 0)
		rows = Min(rows, rel->rd_rel->reltuples *
				   ((double) curpages / rel->rd_rel->relpages));
	*tuples = rows;
}

/*
 * TABLESAMPLE, as Cloudberry samples an append-optimized table: its blocks
 * are of any length and cannot be reached by number, so a sample's "block"
 * is a row, numbered as a scan in the table's order reaches it, out of as
 * many as the segment files hold.  SYSTEM chooses the numbers as it chooses
 * a heap's blocks; BERNOULLI, which chooses rows within a block and has no
 * choice of blocks of its own, the same way, with its own seed and cutoff,
 * which is what Cloudberry's reuse of SYSTEM's function for it comes to.
 * The row chosen is then read by scanning on to it.
 */

/* tsm_bernoulli's state, as bernoulli.c lays it out. */
typedef struct AoBernoulliSamplerData
{
	uint64		cutoff;
	uint32		seed;
} AoBernoulliSamplerData;

static BlockNumber
ao_sample_bernoulli_next(AoScanDesc scan, SampleScanState *scanstate,
						 BlockNumber nrows)
{
	AoBernoulliSamplerData *sampler = (AoBernoulliSamplerData *) scanstate->tsm_state;
	uint32		hashinput[2];

	hashinput[1] = sampler->seed;
	for (; scan->sample_hash_next < nrows; scan->sample_hash_next++)
	{
		hashinput[0] = scan->sample_hash_next;
		if (DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									(int) sizeof(hashinput))) < sampler->cutoff)
			return scan->sample_hash_next++;
	}
	scan->sample_hash_next = 0;
	return InvalidBlockNumber;
}

static bool
ao_scan_sample_next_block(TableScanDesc sscan, SampleScanState *scanstate)
{
	AoScanDesc	scan = (AoScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber nrows;
	BlockNumber row;

	if (!scan->started)
		ao_scan_start(scan);
	if (scan->sample_total < 0)
	{
		scan->sample_total = 0;
		for (int i = 0; i < scan->nsegfiles; i++)
			if (scan->segfiles[i].state == AO_SEGFILE_DEFAULT)
				scan->sample_total += scan->segfiles[i].tupcount;
	}
	nrows = (BlockNumber) Min(scan->sample_total, (int64) MaxBlockNumber);

	if (tsm->NextSampleBlock)
		row = tsm->NextSampleBlock(scanstate, nrows);
	else
		row = ao_sample_bernoulli_next(scan, scanstate, nrows);
	if (row == InvalidBlockNumber || scan->sample_done)
		return false;
	scan->sample_target = row;
	return true;
}

static bool
ao_scan_sample_next_tuple(TableScanDesc sscan, SampleScanState *scanstate,
						  TupleTableSlot *slot)
{
	AoScanDesc	scan = (AoScanDesc) sscan;

	while (!scan->sample_done && scan->sample_next < scan->sample_target)
	{
		if (!ao_scan_getnextslot(sscan, ForwardScanDirection, slot))
			scan->sample_done = true;
		else
			scan->sample_next++;
	}
	if (scan->sample_done || scan->sample_next != scan->sample_target)
		return false;
	scan->sample_next++;
	if (ao_scan_getnextslot(sscan, ForwardScanDirection, slot))
		return true;
	scan->sample_done = true;
	return false;
}

static const TupleTableSlotOps *
ao_row_slot_callbacks(Relation rel)
{
	return &TTSOpsMinimalTuple;
}

static const TupleTableSlotOps *
ao_column_slot_callbacks(Relation rel)
{
	return &TTSOpsVirtual;
}

static void
ao_relation_vacuum_cb(Relation rel, const VacuumParams *params,
					  BufferAccessStrategy bstrategy)
{
	ao_vacuum_rel(rel, params, bstrategy);
}

#define AO_METHODS(slotcb) \
	.type = T_TableAmRoutine, \
	.slot_callbacks = slotcb, \
	.scan_begin = ao_scan_begin, \
	.scan_end = ao_scan_end, \
	.scan_rescan = ao_scan_rescan, \
	.scan_getnextslot = ao_scan_getnextslot, \
	.parallelscan_estimate = ao_parallelscan_estimate, \
	.parallelscan_initialize = ao_parallelscan_initialize, \
	.parallelscan_reinitialize = ao_parallelscan_reinitialize, \
	.index_fetch_begin = ao_index_fetch_begin, \
	.index_fetch_reset = ao_index_fetch_reset, \
	.index_fetch_end = ao_index_fetch_end, \
	.index_fetch_tuple = ao_index_fetch_tuple, \
	.tuple_fetch_row_version = ao_tuple_fetch_row_version, \
	.tuple_tid_valid = ao_tuple_tid_valid, \
	.tuple_get_latest_tid = ao_tuple_get_latest_tid, \
	.tuple_satisfies_snapshot = ao_tuple_satisfies_snapshot, \
	.index_delete_tuples = ao_index_delete_tuples, \
	.tuple_insert = ao_tuple_insert, \
	.tuple_insert_speculative = ao_tuple_insert_speculative, \
	.tuple_complete_speculative = ao_tuple_complete_speculative, \
	.multi_insert = ao_multi_insert, \
	.tuple_delete = ao_tuple_delete, \
	.tuple_update = ao_tuple_update, \
	.tuple_lock = ao_tuple_lock, \
	.finish_bulk_insert = ao_finish_bulk_insert, \
	.relation_set_new_filelocator = ao_relation_set_new_filelocator, \
	.relation_nontransactional_truncate = ao_relation_nontransactional_truncate, \
	.relation_copy_data = ao_relation_copy_data, \
	.relation_copy_for_cluster = ao_relation_copy_for_cluster, \
	.relation_vacuum = ao_relation_vacuum_cb, \
	.scan_analyze_next_block = ao_scan_analyze_next_block, \
	.scan_analyze_next_tuple = ao_scan_analyze_next_tuple, \
	.index_build_range_scan = ao_index_build_range_scan, \
	.index_validate_scan = ao_index_validate_scan, \
	.relation_size = table_block_relation_size, \
	.relation_needs_toast_table = ao_relation_needs_toast_table, \
	.relation_toast_am = ao_relation_toast_am, \
	.relation_estimate_size = ao_relation_estimate_size, \
	.scan_bitmap_next_tuple = ao_scan_bitmap_next_tuple, \
	.scan_sample_next_block = ao_scan_sample_next_block, \
	.scan_sample_next_tuple = ao_scan_sample_next_tuple

static const TableAmRoutine ao_row_methods = {
	AO_METHODS(ao_row_slot_callbacks)
};

static const TableAmRoutine ao_column_methods = {
	AO_METHODS(ao_column_slot_callbacks)
};

PG_FUNCTION_INFO_V1(gp_ao_row_handler);
PG_FUNCTION_INFO_V1(gp_ao_column_handler);

Datum
gp_ao_row_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&ao_row_methods);
}

Datum
gp_ao_column_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&ao_column_methods);
}

/* ------------------------------------------------------------------------- */
/* What the core asks of the registry (O13)                                  */
/* ------------------------------------------------------------------------- */

/* O18: each segment file's run of TID blocks, as far as it has numbered rows. */
static TableAmBlockSequence *
ao_relation_get_block_sequences(Relation rel, int *nseqs)
{
	TableAmBlockSequence *seqs = palloc_array(TableAmBlockSequence, AO_MAX_SEGNO);

	*nseqs = 0;
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
	{
		int64		next = ao_next_rownum(rel, segno);

		if (next <= 1)
			continue;
		seqs[*nseqs].startblknum = (BlockNumber) segno << AO_SEGNO_SHIFT;
		seqs[*nseqs].nblocks = (BlockNumber) ((next - 1) / AO_ROWS_PER_TIDBLOCK) + 1;
		(*nseqs)++;
	}
	return seqs;
}

/*
 * O17: the columns an ALTER TABLE adds to a table by column, written alone,
 * as Cloudberry's AOCO writes them (aocs_addcol): each block of each segment
 * file given the new columns' values for its rows, computed from the rest
 * of the row, in files of their own, and its directory row the offsets of
 * them.  A row deleted is given NULLs, and nothing it holds is evaluated.
 * A segment file awaiting drop gets none: no snapshot that sees the new
 * columns reads it, and the old ones read it as they did.  The core checks
 * the new columns' constraints against what was written.
 */
static void
ao_relation_add_columns(Relation rel, int ncolumns, const AttrNumber *attnums,
						Expr *const *exprs, const bool *generated)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			natts = desc->natts;
	int64		storage_id = ao_storage_id(rel);
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	ExprState **states = palloc_array(ExprState *, ncolumns);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	AoOptions  *colopts = palloc_array(AoOptions, natts);
	AoColumnBuilder *builders = palloc_array(AoColumnBuilder, ncolumns);
	AoBlockReader *readers = palloc_array(AoBlockReader, natts);
	StringInfoData raw;
	StringInfoData out;
	AoSegfile  *segfiles;
	int			nsegfiles;

	ao_column_options(rel, colopts);
	for (int k = 0; k < ncolumns; k++)
	{
		states[k] = ExecPrepareExpr(exprs[k], estate);
		ao_column_builder_init(&builders[k]);
	}
	for (int a = 0; a < natts; a++)
		ao_reader_init(&readers[a], CurrentMemoryContext);
	initStringInfo(&raw);
	initStringInfo(&out);

	segfiles = ao_segfiles_read(storage_id, snapshot, &nsegfiles);
	for (int s = 0; s < nsegfiles; s++)
	{
		AoSegfile  *sf;
		AoVisimap  *vm;
		AoBlkdirScan *bs;
		AoBlkdirEntry entry;
		List	   *entries = NIL;
		int64	   *eof;
		int64	   *eof_unc;
		ListCell   *lc;

		if (segfiles[s].state != AO_SEGFILE_DEFAULT)
			continue;
		sf = ao_segfile_read(storage_id, segfiles[s].segno, SnapshotSelf);
		if (sf == NULL)
			continue;
		vm = ao_visimap_load(storage_id, sf->segno, snapshot);
		eof = palloc0_array(int64, natts);
		eof_unc = palloc0_array(int64, natts);
		for (int g = 0; g < sf->ngroups && g < natts; g++)
		{
			eof[g] = sf->eof[g];
			eof_unc[g] = sf->eof_uncompressed[g];
		}

		/* The segment file's blocks, read before any directory row changes. */
		bs = ao_blkdir_scan_begin(storage_id, sf->segno, snapshot);
		while (ao_blkdir_scan_next(bs, &entry))
		{
			AoBlkdirEntry *e = palloc_object(AoBlkdirEntry);

			*e = entry;
			entries = lappend(entries, e);
		}
		ao_blkdir_scan_end(bs);

		foreach(lc, entries)
		{
			AoBlkdirEntry *e = lfirst(lc);
			int64	   *offsets = palloc_array(int64, natts);

			for (int a = 0; a < natts; a++)
			{
				Form_pg_attribute att = TupleDescAttr(desc, a);

				offsets[a] = a < e->noffsets ? e->offsets[a] : -1;
				if (att->attisdropped || offsets[a] < 0)
					continue;
				readers[a].filenum = AoFileNum(sf->segno, a + 1);
				ao_reader_load(rel, &readers[a], offsets[a], att, NULL);
			}

			for (int i = 0; i < e->nrows; i++)
			{
				bool		deleted = ao_visimap_is_deleted(vm, e->first_row + i);

				ExecClearTuple(slot);
				for (int a = 0; a < natts; a++)
				{
					if (TupleDescAttr(desc, a)->attisdropped)
					{
						slot->tts_values[a] = (Datum) 0;
						slot->tts_isnull[a] = true;
					}
					else if (offsets[a] >= 0)
					{
						slot->tts_values[a] = readers[a].values[i];
						slot->tts_isnull[a] = readers[a].isnull[i];
					}
					else
						slot->tts_values[a] = getmissingattr(desc, a + 1,
															 &slot->tts_isnull[a]);
				}
				for (int k = 0; k < ncolumns; k++)
				{
					slot->tts_values[attnums[k] - 1] = (Datum) 0;
					slot->tts_isnull[attnums[k] - 1] = true;
				}
				ExecStoreVirtualTuple(slot);

				/* Plain expressions of the old row first, then the generated. */
				for (int pass = 0; pass < 2 && !deleted; pass++)
				{
					for (int k = 0; k < ncolumns; k++)
					{
						AttrNumber	attno = attnums[k];

						if (generated[k] != (pass == 1))
							continue;
						econtext->ecxt_scantuple = slot;
						slot->tts_values[attno - 1] =
							ExecEvalExpr(states[k], econtext, &slot->tts_isnull[attno - 1]);
					}
				}
				for (int k = 0; k < ncolumns; k++)
				{
					Form_pg_attribute att = TupleDescAttr(desc, attnums[k] - 1);
					Datum		v = slot->tts_values[attnums[k] - 1];
					bool		isnull = slot->tts_isnull[attnums[k] - 1];

					if (!isnull)
						v = ao_detoast_value(att, v);
					ao_column_append(&builders[k], att, v, isnull);
				}
				ResetExprContext(econtext);
			}

			for (int k = 0; k < ncolumns; k++)
			{
				int			g = attnums[k] - 1;
				Size		len;

				ao_column_finish(&builders[k], &raw);
				ao_column_builder_reset(&builders[k]);
				len = ao_block_encode(&raw, &colopts[g], e->first_row, e->nrows, &out);
				offsets[g] = eof[g];
				ao_file_write(rel, AoFileNum(sf->segno, g + 1), eof[g], out.data, len);
				eof[g] += len;
				eof_unc[g] += AO_BLOCK_HEADER_SIZE + raw.len;
			}
			ao_blkdir_replace(&e->tid, storage_id, sf->segno, e->first_row,
							  e->nrows, offsets, natts);
		}

		sf->eof = eof;
		sf->eof_uncompressed = eof_unc;
		sf->ngroups = natts;
		sf->modcount++;
		ao_segfile_update(storage_id, sf);
	}

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	UnregisterSnapshot(snapshot);
}

static TableAmExtRoutine ao_row_ext;
static TableAmExtRoutine ao_column_ext;

void
ao_register_table_ams(void)
{
	memset(&ao_row_ext, 0, sizeof(ao_row_ext));
	ao_row_ext.size = sizeof(TableAmExtRoutine);
	ao_row_ext.reloptions = ao_row_reloptions;
	ao_row_ext.index_unique_check = ao_index_unique_check;
	ao_row_ext.relation_get_block_sequences = ao_relation_get_block_sequences;

	ao_column_ext = ao_row_ext;
	ao_column_ext.reloptions = ao_column_reloptions;
	ao_column_ext.scan_extractcolumns = ao_scan_extractcolumns;
	ao_column_ext.scan_by_column = true;
	ao_column_ext.relation_add_columns = ao_relation_add_columns;

	RegisterTableAmExtension(&ao_row_methods, &ao_row_ext);
	RegisterTableAmExtension(&ao_column_methods, &ao_column_ext);
}
