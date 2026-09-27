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
 * ao_meta.c
 *	  What an append-optimized table's segment files hold, which rows are
 *	  deleted, and where each block of rows is: gp_ao's three tables.
 *
 * Cloudberry keeps these in pg_aoseg.pg_aoseg_<oid> (or pg_aocsseg_<oid>),
 * pg_aovisimap_<oid> and pg_aoblkdir_<oid>, three relations a table, made
 * with it and swapped with it by a rewrite through a table access method
 * callback PostgreSQL 19 does not have.  The port keeps one table of each,
 * shared by every append-optimized table of the database and keyed by the
 * storage ID of the files a row describes, which each table's metapage
 * holds (ao_storage.c): a new relfilenode is a new storage ID, and its rows
 * follow it through TRUNCATE, CLUSTER and every rewrite with nothing to
 * swap.  This is what citus columnar does (columnar.stripe and the rest).
 *
 * They are ordinary tables, read and written here as the catalogs are, and
 * as MVCC as any: a reader sees the segment files, the deletions and the
 * blocks its snapshot sees.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "common/int.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "port/pg_bitutils.h"
#include "utils/snapmgr.h"

#include "gp_ao.h"

/* gp_ao.segfile's columns */
#define Anum_segfile_storage_id		1
#define Anum_segfile_segno			2
#define Anum_segfile_eof			3
#define Anum_segfile_eof_unc		4
#define Anum_segfile_tupcount		5
#define Anum_segfile_varblockcount	6
#define Anum_segfile_modcount		7
#define Anum_segfile_state			8
#define Anum_segfile_formatversion	9
#define Anum_segfile_compacted_by	10
#define Natts_segfile				10

/* gp_ao.visimap's */
#define Anum_visimap_storage_id		1
#define Anum_visimap_segno			2
#define Anum_visimap_first_row		3
#define Anum_visimap_bitmap			4
#define Natts_visimap				4

/* gp_ao.blkdir's */
#define Anum_blkdir_storage_id		1
#define Anum_blkdir_segno			2
#define Anum_blkdir_first_row		3
#define Anum_blkdir_nrows			4
#define Anum_blkdir_offsets			5
#define Natts_blkdir				5

/* gp_ao.segfilecount's */
#define Anum_segfilecount_storage_id	1
#define Anum_segfilecount_count		2
#define Natts_segfilecount			2

/*
 * The OID of one of gp_ao's tables or indexes.  Looked up by name each time:
 * two syscache lookups, and never stale across a DROP EXTENSION.
 */
Oid
ao_meta_relid(const char *name, bool missing_ok)
{
	Oid			nsp = get_namespace_oid("gp_ao", missing_ok);
	Oid			relid = OidIsValid(nsp) ? get_relname_relid(name, nsp) : InvalidOid;

	if (!OidIsValid(relid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("gp_ao.%s does not exist", name),
				 errhint("CREATE EXTENSION gp_ao in this database.")));
	return relid;
}

/*
 * The snapshot a scan of these tables reads with, registered for as long as
 * it runs where it is an MVCC snapshot, which PostgreSQL 19 checks: the
 * latest snapshot is not otherwise.
 */
static Snapshot
meta_snapshot_begin(Snapshot snapshot)
{
	return IsMVCCSnapshot(snapshot) ? RegisterSnapshot(snapshot) : snapshot;
}

static void
meta_snapshot_end(Snapshot snapshot)
{
	if (IsMVCCSnapshot(snapshot))
		UnregisterSnapshot(snapshot);
}

static ArrayType *
int8_array(const int64 *values, int n)
{
	Datum	   *datums = palloc_array(Datum, Max(n, 1));

	for (int i = 0; i < n; i++)
		datums[i] = Int64GetDatum(values[i]);
	return construct_array_builtin(datums, n, INT8OID);
}

static int64 *
int8_array_values(Datum d, int *n)
{
	ArrayType  *array = DatumGetArrayTypeP(d);
	Datum	   *datums;
	int64	   *values;

	deconstruct_array(array, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE,
					  &datums, NULL, n);
	values = palloc_array(int64, Max(*n, 1));
	for (int i = 0; i < *n; i++)
		values[i] = DatumGetInt64(datums[i]);
	return values;
}

/* ------------------------------------------------------------------------- */
/* gp_ao.segfile                                                             */
/* ------------------------------------------------------------------------- */

static void
segfile_from_tuple(HeapTuple tup, TupleDesc desc, AoSegfile *sf)
{
	Datum		values[Natts_segfile];
	bool		nulls[Natts_segfile];
	int			n;

	heap_deform_tuple(tup, desc, values, nulls);
	sf->segno = DatumGetInt32(values[Anum_segfile_segno - 1]);
	sf->eof = int8_array_values(values[Anum_segfile_eof - 1], &sf->ngroups);
	sf->eof_uncompressed = int8_array_values(values[Anum_segfile_eof_unc - 1],
											 &n);
	if (n != sf->ngroups)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment file %d of append-optimized storage has %d ends and %d uncompressed ends",
						sf->segno, sf->ngroups, n)));
	sf->tupcount = DatumGetInt64(values[Anum_segfile_tupcount - 1]);
	sf->varblockcount = DatumGetInt64(values[Anum_segfile_varblockcount - 1]);
	sf->modcount = DatumGetInt64(values[Anum_segfile_modcount - 1]);
	sf->state = DatumGetInt16(values[Anum_segfile_state - 1]);
	sf->formatversion = DatumGetInt16(values[Anum_segfile_formatversion - 1]);
	sf->compacted_by = nulls[Anum_segfile_compacted_by - 1] ?
		InvalidTransactionId :
		DatumGetTransactionId(values[Anum_segfile_compacted_by - 1]);
	sf->tid = tup->t_self;
}

static int
segfile_cmp(const void *a, const void *b)
{
	return pg_cmp_s32(((const AoSegfile *) a)->segno,
					  ((const AoSegfile *) b)->segno);
}

/* Every segment file of storage_id snapshot sees, by segno. */
AoSegfile *
ao_segfiles_read(int64 storage_id, Snapshot snapshot, int *nsegfiles)
{
	Relation	rel = table_open(ao_meta_relid("segfile", false), AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	AoSegfile  *result = palloc_array(AoSegfile, AO_MAX_SEGNO + 1);
	int			n = 0;

	snapshot = meta_snapshot_begin(snapshot);
	ScanKeyInit(&key, Anum_segfile_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	scan = systable_beginscan(rel, ao_meta_relid("segfile_key", false), true,
							  snapshot, 1, &key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		if (n > AO_MAX_SEGNO)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("append-optimized storage " INT64_FORMAT " has more than %d segment files",
							storage_id, AO_MAX_SEGNO)));
		segfile_from_tuple(tup, RelationGetDescr(rel), &result[n++]);
	}
	systable_endscan(scan);
	meta_snapshot_end(snapshot);
	table_close(rel, AccessShareLock);

	qsort(result, n, sizeof(AoSegfile), segfile_cmp);
	*nsegfiles = n;
	return result;
}

/* Segment file segno of storage_id, as snapshot sees it, or NULL. */
AoSegfile *
ao_segfile_read(int64 storage_id, int segno, Snapshot snapshot)
{
	Relation	rel = table_open(ao_meta_relid("segfile", false), AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	AoSegfile  *result = NULL;

	snapshot = meta_snapshot_begin(snapshot);
	ScanKeyInit(&key[0], Anum_segfile_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	ScanKeyInit(&key[1], Anum_segfile_segno, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(segno));
	scan = systable_beginscan(rel, ao_meta_relid("segfile_key", false), true,
							  snapshot, 2, key);
	if ((tup = systable_getnext(scan)) != NULL)
	{
		result = palloc_object(AoSegfile);
		segfile_from_tuple(tup, RelationGetDescr(rel), result);
	}
	systable_endscan(scan);
	meta_snapshot_end(snapshot);
	table_close(rel, AccessShareLock);
	return result;
}

static HeapTuple
segfile_to_tuple(TupleDesc desc, int64 storage_id, const AoSegfile *sf)
{
	Datum		values[Natts_segfile];
	bool		nulls[Natts_segfile] = {0};

	values[Anum_segfile_storage_id - 1] = Int64GetDatum(storage_id);
	values[Anum_segfile_segno - 1] = Int32GetDatum(sf->segno);
	values[Anum_segfile_eof - 1] = PointerGetDatum(int8_array(sf->eof, sf->ngroups));
	values[Anum_segfile_eof_unc - 1] =
		PointerGetDatum(int8_array(sf->eof_uncompressed, sf->ngroups));
	values[Anum_segfile_tupcount - 1] = Int64GetDatum(sf->tupcount);
	values[Anum_segfile_varblockcount - 1] = Int64GetDatum(sf->varblockcount);
	values[Anum_segfile_modcount - 1] = Int64GetDatum(sf->modcount);
	values[Anum_segfile_state - 1] = Int16GetDatum(sf->state);
	values[Anum_segfile_formatversion - 1] = Int16GetDatum(sf->formatversion);
	if (TransactionIdIsValid(sf->compacted_by))
		values[Anum_segfile_compacted_by - 1] =
			TransactionIdGetDatum(sf->compacted_by);
	else
		nulls[Anum_segfile_compacted_by - 1] = true;
	return heap_form_tuple(desc, values, nulls);
}

/* A new, empty segment file: ngroups files, each of no bytes. */
void
ao_segfile_insert(int64 storage_id, int segno, int ngroups)
{
	Relation	rel = table_open(ao_meta_relid("segfile", false), RowExclusiveLock);
	AoSegfile	sf = {0};

	sf.segno = segno;
	sf.ngroups = ngroups;
	sf.eof = palloc0_array(int64, ngroups);
	sf.eof_uncompressed = palloc0_array(int64, ngroups);
	sf.state = AO_SEGFILE_DEFAULT;
	sf.formatversion = AO_FORMAT_VERSION;
	CatalogTupleInsert(rel, segfile_to_tuple(RelationGetDescr(rel), storage_id, &sf));
	table_close(rel, RowExclusiveLock);
}

/*
 * A segment file's row, rewritten.  The caller holds the segment file's lock,
 * read the row with a snapshot taken after it took the lock, and changed it.
 */
void
ao_segfile_update(int64 storage_id, AoSegfile *sf)
{
	Relation	rel = table_open(ao_meta_relid("segfile", false), RowExclusiveLock);
	HeapTuple	tup = segfile_to_tuple(RelationGetDescr(rel), storage_id, sf);

	CatalogTupleUpdate(rel, &sf->tid, tup);
	sf->tid = tup->t_self;
	table_close(rel, RowExclusiveLock);
}

void
ao_segfile_delete(int64 storage_id, int segno)
{
	AoSegfile  *sf = ao_segfile_read(storage_id, segno, GetLatestSnapshot());

	if (sf != NULL)
	{
		Relation	rel = table_open(ao_meta_relid("segfile", false),
									 RowExclusiveLock);

		CatalogTupleDelete(rel, &sf->tid);
		table_close(rel, RowExclusiveLock);
	}
}

/* Every row of one table keyed by storage_id, deleted. */
static void
delete_storage_rows(const char *table, const char *index, int64 storage_id)
{
	Relation	rel = table_open(ao_meta_relid(table, false), RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	Snapshot	snapshot = meta_snapshot_begin(GetLatestSnapshot());

	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ,
				Int64GetDatum(storage_id));
	scan = systable_beginscan(rel, ao_meta_relid(index, false), true,
							  snapshot, 1, &key);
	while ((tup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	meta_snapshot_end(snapshot);
	table_close(rel, RowExclusiveLock);
}

/* Files that are no more: DROP TABLE, TRUNCATE, a rewrite's old files. */
void
ao_meta_delete_storage(int64 storage_id)
{
	delete_storage_rows("segfile", "segfile_key", storage_id);
	delete_storage_rows("visimap", "visimap_key", storage_id);
	delete_storage_rows("blkdir", "blkdir_key", storage_id);
	delete_storage_rows("segfilecount", "segfilecount_key", storage_id);
	CommandCounterIncrement();
}

/* ------------------------------------------------------------------------- */
/* gp_ao.segfilecount                                                        */
/* ------------------------------------------------------------------------- */

/* The row of a storage ID, as the latest snapshot sees it, or NULL. */
static HeapTuple
segfilecount_row(Relation rel, int64 storage_id)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	Snapshot	snapshot = meta_snapshot_begin(GetLatestSnapshot());

	ScanKeyInit(&key, Anum_segfilecount_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	scan = systable_beginscan(rel, ao_meta_relid("segfilecount_key", false),
							  true, snapshot, 1, &key);
	tup = systable_getnext(scan);
	if (tup != NULL)
		tup = heap_copytuple(tup);
	systable_endscan(scan);
	meta_snapshot_end(snapshot);
	return tup;
}

/* What ANALYZE last counted of a table's segment files; 0 before it has. */
int
ao_segfilecount_get(int64 storage_id)
{
	Oid			relid = ao_meta_relid("segfilecount", true);
	Relation	rel;
	HeapTuple	tup;
	int			result = 0;

	if (!OidIsValid(relid))
		return 0;
	rel = table_open(relid, AccessShareLock);
	tup = segfilecount_row(rel, storage_id);
	if (tup != NULL)
	{
		bool		isnull;
		Datum		d = heap_getattr(tup, Anum_segfilecount_count,
									 RelationGetDescr(rel), &isnull);

		result = isnull ? 0 : DatumGetInt16(d);
	}
	table_close(rel, AccessShareLock);
	return result;
}

void
ao_segfilecount_set(int64 storage_id, int segfilecount)
{
	Relation	rel = table_open(ao_meta_relid("segfilecount", false),
								 RowExclusiveLock);
	HeapTuple	old = segfilecount_row(rel, storage_id);
	Datum		values[Natts_segfilecount];
	bool		nulls[Natts_segfilecount] = {0};
	HeapTuple	tup;

	values[Anum_segfilecount_storage_id - 1] = Int64GetDatum(storage_id);
	values[Anum_segfilecount_count - 1] = Int16GetDatum((int16) segfilecount);
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	if (old == NULL)
		CatalogTupleInsert(rel, tup);
	else
		CatalogTupleUpdate(rel, &old->t_self, tup);
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();
}

/* ------------------------------------------------------------------------- */
/* gp_ao.blkdir                                                              */
/* ------------------------------------------------------------------------- */

void
ao_blkdir_insert(int64 storage_id, int segno, int64 first_row, int nrows,
				 const int64 *offsets, int noffsets)
{
	Relation	rel = table_open(ao_meta_relid("blkdir", false), RowExclusiveLock);
	Datum		values[Natts_blkdir];
	bool		nulls[Natts_blkdir] = {0};

	values[Anum_blkdir_storage_id - 1] = Int64GetDatum(storage_id);
	values[Anum_blkdir_segno - 1] = Int32GetDatum(segno);
	values[Anum_blkdir_first_row - 1] = Int64GetDatum(first_row);
	values[Anum_blkdir_nrows - 1] = Int32GetDatum(nrows);
	values[Anum_blkdir_offsets - 1] = PointerGetDatum(int8_array(offsets, noffsets));
	CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));
	table_close(rel, RowExclusiveLock);
}

/*
 * The block row rownum of segment file segno is in, as snapshot sees the
 * directory: the entry with the greatest first_row not past rownum, if its
 * rows reach it.
 */
bool
ao_blkdir_lookup(int64 storage_id, int segno, int64 rownum, Snapshot snapshot,
				 AoBlkdirEntry *entry)
{
	Relation	rel = table_open(ao_meta_relid("blkdir", false), AccessShareLock);
	Relation	idx = index_open(ao_meta_relid("blkdir_key", false), AccessShareLock);
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	ScanKeyInit(&key[0], Anum_blkdir_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	ScanKeyInit(&key[1], Anum_blkdir_segno, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(segno));
	ScanKeyInit(&key[2], Anum_blkdir_first_row, BTLessEqualStrategyNumber,
				F_INT8LE, Int64GetDatum(rownum));
	snapshot = meta_snapshot_begin(snapshot);
	scan = systable_beginscan_ordered(rel, idx, snapshot, 3, key);
	while ((tup = systable_getnext_ordered(scan, BackwardScanDirection)) != NULL)
	{
		Datum		values[Natts_blkdir];
		bool		nulls[Natts_blkdir];

		heap_deform_tuple(tup, RelationGetDescr(rel), values, nulls);
		entry->tid = tup->t_self;
		entry->segno = segno;
		entry->first_row = DatumGetInt64(values[Anum_blkdir_first_row - 1]);
		entry->nrows = DatumGetInt32(values[Anum_blkdir_nrows - 1]);
		if (entry->nrows < 0 || rownum < entry->first_row + entry->nrows)
		{
			entry->offsets = int8_array_values(values[Anum_blkdir_offsets - 1],
											   &entry->noffsets);
			found = true;
		}
		/*
		 * With a snapshot that sees more than one version -- a dirty one
		 * sees a placeholder being replaced as both -- the next one down may
		 * be the one that covers rownum.
		 */
		if (found || IsMVCCSnapshot(snapshot))
			break;
	}
	systable_endscan_ordered(scan);
	meta_snapshot_end(snapshot);
	index_close(idx, AccessShareLock);
	table_close(rel, AccessShareLock);
	return found;
}

/*
 * A placeholder for the block a writer is building, where the table has a
 * unique index: it covers every row number from first_row, so a unique
 * index's probe from another transaction, with a dirty snapshot, finds it
 * and waits for this one, as Cloudberry's placeholders make it.  The block's
 * real row replaces it when the block is written.
 */
void
ao_blkdir_insert_placeholder(int64 storage_id, int segno, int64 first_row,
							 ItemPointer tid)
{
	Relation	rel = table_open(ao_meta_relid("blkdir", false), RowExclusiveLock);
	Datum		values[Natts_blkdir];
	bool		nulls[Natts_blkdir] = {0};
	int64		none = -1;
	HeapTuple	tup;

	values[Anum_blkdir_storage_id - 1] = Int64GetDatum(storage_id);
	values[Anum_blkdir_segno - 1] = Int32GetDatum(segno);
	values[Anum_blkdir_first_row - 1] = Int64GetDatum(first_row);
	values[Anum_blkdir_nrows - 1] = Int32GetDatum(-1);
	values[Anum_blkdir_offsets - 1] = PointerGetDatum(int8_array(&none, 1));
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	*tid = tup->t_self;
	table_close(rel, RowExclusiveLock);
}

/* The placeholder at tid, replaced by its block's real row. */
void
ao_blkdir_replace(ItemPointer tid, int64 storage_id, int segno,
				  int64 first_row, int nrows, const int64 *offsets,
				  int noffsets)
{
	Relation	rel = table_open(ao_meta_relid("blkdir", false), RowExclusiveLock);
	Datum		values[Natts_blkdir];
	bool		nulls[Natts_blkdir] = {0};

	values[Anum_blkdir_storage_id - 1] = Int64GetDatum(storage_id);
	values[Anum_blkdir_segno - 1] = Int32GetDatum(segno);
	values[Anum_blkdir_first_row - 1] = Int64GetDatum(first_row);
	values[Anum_blkdir_nrows - 1] = Int32GetDatum(nrows);
	values[Anum_blkdir_offsets - 1] = PointerGetDatum(int8_array(offsets, noffsets));
	CatalogTupleUpdate(rel, tid, heap_form_tuple(RelationGetDescr(rel), values, nulls));
	table_close(rel, RowExclusiveLock);
}

/* The directory rows of one segment file a snapshot sees, in order. */
struct AoBlkdirScan
{
	Relation	rel;
	Relation	idx;
	Snapshot	snapshot;
	SysScanDesc scan;
	ScanKeyData key[2];
};

AoBlkdirScan *
ao_blkdir_scan_begin(int64 storage_id, int segno, Snapshot snapshot)
{
	AoBlkdirScan *bs = palloc0_object(AoBlkdirScan);

	bs->rel = table_open(ao_meta_relid("blkdir", false), AccessShareLock);
	bs->idx = index_open(ao_meta_relid("blkdir_key", false), AccessShareLock);
	ScanKeyInit(&bs->key[0], Anum_blkdir_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	ScanKeyInit(&bs->key[1], Anum_blkdir_segno, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(segno));
	bs->snapshot = meta_snapshot_begin(snapshot);
	bs->scan = systable_beginscan_ordered(bs->rel, bs->idx, bs->snapshot, 2,
										  bs->key);
	return bs;
}

/* The next row, placeholders passed over; false at the end. */
bool
ao_blkdir_scan_next(AoBlkdirScan *bs, AoBlkdirEntry *entry)
{
	HeapTuple	tup;

	while ((tup = systable_getnext_ordered(bs->scan, ForwardScanDirection)) != NULL)
	{
		Datum		values[Natts_blkdir];
		bool		nulls[Natts_blkdir];

		heap_deform_tuple(tup, RelationGetDescr(bs->rel), values, nulls);
		entry->tid = tup->t_self;
		entry->segno = DatumGetInt32(values[Anum_blkdir_segno - 1]);
		entry->first_row = DatumGetInt64(values[Anum_blkdir_first_row - 1]);
		entry->nrows = DatumGetInt32(values[Anum_blkdir_nrows - 1]);
		if (entry->nrows < 0)
			continue;
		entry->offsets = int8_array_values(values[Anum_blkdir_offsets - 1],
										   &entry->noffsets);
		return true;
	}
	return false;
}

void
ao_blkdir_scan_end(AoBlkdirScan *bs)
{
	systable_endscan_ordered(bs->scan);
	meta_snapshot_end(bs->snapshot);
	index_close(bs->idx, AccessShareLock);
	table_close(bs->rel, AccessShareLock);
	pfree(bs);
}

static void
delete_segfile_rows(const char *table, const char *index, int64 storage_id,
					int segno)
{
	Relation	rel = table_open(ao_meta_relid(table, false), RowExclusiveLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Snapshot	snapshot;

	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_INT8EQ,
				Int64GetDatum(storage_id));
	ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(segno));
	snapshot = meta_snapshot_begin(GetLatestSnapshot());
	scan = systable_beginscan(rel, ao_meta_relid(index, false), true,
							  snapshot, 2, key);
	while ((tup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	meta_snapshot_end(snapshot);
	table_close(rel, RowExclusiveLock);
}

void
ao_blkdir_delete_segfile(int64 storage_id, int segno)
{
	delete_segfile_rows("blkdir", "blkdir_key", storage_id, segno);
}

/* ------------------------------------------------------------------------- */
/* gp_ao.visimap                                                             */
/* ------------------------------------------------------------------------- */

static int64
visimap_first_row(int64 rownum)
{
	return (rownum / AO_VISIMAP_ROWS) * AO_VISIMAP_ROWS;
}

/* The rows of segment file segno snapshot sees deleted. */
AoVisimap *
ao_visimap_load(int64 storage_id, int segno, Snapshot snapshot)
{
	Relation	rel = table_open(ao_meta_relid("visimap", false), AccessShareLock);
	Relation	idx = index_open(ao_meta_relid("visimap_key", false), AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	AoVisimap  *vm = palloc0_object(AoVisimap);
	int			max = 4;

	vm->segno = segno;
	vm->first_rows = palloc_array(int64, max);
	vm->bitmaps = palloc_array(uint8 *, max);

	ScanKeyInit(&key[0], Anum_visimap_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum(storage_id));
	ScanKeyInit(&key[1], Anum_visimap_segno, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(segno));
	snapshot = meta_snapshot_begin(snapshot);
	scan = systable_beginscan_ordered(rel, idx, snapshot, 2, key);
	while ((tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		Datum		values[Natts_visimap];
		bool		nulls[Natts_visimap];
		bytea	   *bits;

		heap_deform_tuple(tup, RelationGetDescr(rel), values, nulls);
		bits = DatumGetByteaPP(values[Anum_visimap_bitmap - 1]);
		if (VARSIZE_ANY_EXHDR(bits) != AO_VISIMAP_BYTES)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("visibility map entry of segment file %d of append-optimized storage " INT64_FORMAT " is %zu bytes",
							segno, storage_id, VARSIZE_ANY_EXHDR(bits))));
		/*
		 * A snapshot that sees two versions of an entry -- a dirty one, while
		 * a deleter is at work -- sees a row deleted only where both say so:
		 * a deletion in progress is waited for, not taken as done.
		 */
		if (vm->nentries > 0 &&
			vm->first_rows[vm->nentries - 1] ==
			DatumGetInt64(values[Anum_visimap_first_row - 1]))
		{
			uint8	   *prev = vm->bitmaps[vm->nentries - 1];
			const uint8 *these = (const uint8 *) VARDATA_ANY(bits);

			for (int b = 0; b < AO_VISIMAP_BYTES; b++)
				prev[b] &= these[b];
			continue;
		}
		if (vm->nentries == max)
		{
			max *= 2;
			vm->first_rows = repalloc_array(vm->first_rows, int64, max);
			vm->bitmaps = repalloc_array(vm->bitmaps, uint8 *, max);
		}
		vm->first_rows[vm->nentries] =
			DatumGetInt64(values[Anum_visimap_first_row - 1]);
		vm->bitmaps[vm->nentries] = palloc(AO_VISIMAP_BYTES);
		memcpy(vm->bitmaps[vm->nentries], VARDATA_ANY(bits), AO_VISIMAP_BYTES);
		vm->nentries++;
	}
	systable_endscan_ordered(scan);
	meta_snapshot_end(snapshot);
	index_close(idx, AccessShareLock);
	table_close(rel, AccessShareLock);
	return vm;
}

bool
ao_visimap_is_deleted(AoVisimap *vm, int64 rownum)
{
	int64		first = visimap_first_row(rownum);
	int			lo = 0,
				hi = vm->nentries - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (vm->first_rows[mid] == first)
		{
			int64		bit = rownum - first;

			return (vm->bitmaps[mid][bit / 8] & (1 << (bit % 8))) != 0;
		}
		if (vm->first_rows[mid] < first)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/* How many rows the map says are deleted. */
int64
ao_visimap_count(AoVisimap *vm)
{
	int64		n = 0;

	for (int i = 0; i < vm->nentries; i++)
		for (int b = 0; b < AO_VISIMAP_BYTES; b++)
			n += pg_popcount32(vm->bitmaps[i][b]);
	return n;
}

/*
 * Mark rownums of segment file segno deleted, in order.  The table is held
 * in ExclusiveLock by whoever deletes, so no one else writes these rows; a
 * snapshot taken now sees this transaction's earlier deletions too.
 *
 * A row the map has deleted already was deleted where the deleting query's
 * snapshot could not see it: by a transaction that committed after a
 * REPEATABLE READ transaction's snapshot was taken, which is a serialization
 * failure as a heap's concurrent update is, or by a later query of this
 * transaction, one a function or a trigger of the deleting query ran, which
 * a heap refuses as well.  Neither can go on: an UPDATE has appended the
 * row's new version already.
 */
void
ao_visimap_delete_rows(int64 storage_id, int segno, const int64 *rownums,
					   int nrows)
{
	Relation	rel = table_open(ao_meta_relid("visimap", false), RowExclusiveLock);
	Relation	idx = index_open(ao_meta_relid("visimap_key", false), AccessShareLock);
	int			i = 0;

	while (i < nrows)
	{
		int64		first = visimap_first_row(rownums[i]);
		ScanKeyData key[3];
		SysScanDesc scan;
		Snapshot	snapshot;
		HeapTuple	tup;
		uint8		bits[AO_VISIMAP_BYTES];
		ItemPointerData oldtid;
		bool		exists = false;
		bytea	   *value;
		Datum		values[Natts_visimap];
		bool		nulls[Natts_visimap] = {0};

		ScanKeyInit(&key[0], Anum_visimap_storage_id, BTEqualStrategyNumber,
					F_INT8EQ, Int64GetDatum(storage_id));
		ScanKeyInit(&key[1], Anum_visimap_segno, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(segno));
		ScanKeyInit(&key[2], Anum_visimap_first_row, BTEqualStrategyNumber,
					F_INT8EQ, Int64GetDatum(first));
		snapshot = meta_snapshot_begin(GetLatestSnapshot());
		scan = systable_beginscan_ordered(rel, idx, snapshot, 3, key);
		if ((tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
		{
			Datum		d;
			bool		isnull;
			bytea	   *b;

			d = heap_getattr(tup, Anum_visimap_bitmap, RelationGetDescr(rel),
							 &isnull);
			b = DatumGetByteaPP(d);
			memcpy(bits, VARDATA_ANY(b), AO_VISIMAP_BYTES);
			oldtid = tup->t_self;
			exists = true;
		}
		else
			memset(bits, 0, AO_VISIMAP_BYTES);
		systable_endscan_ordered(scan);
		meta_snapshot_end(snapshot);

		for (; i < nrows && visimap_first_row(rownums[i]) == first; i++)
		{
			int64		bit = rownums[i] - first;

			if (bits[bit / 8] & (uint8) (1 << (bit % 8)))
			{
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				ereport(ERROR,
						(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
						 errmsg("row " INT64_FORMAT " of segment file %d to be deleted was already deleted by another query of this transaction",
								rownums[i], segno),
						 errhint("A function or trigger the statement ran deleted or updated the same row of the append-optimized table.")));
			}
			bits[bit / 8] |= (uint8) (1 << (bit % 8));
		}

		value = palloc(VARHDRSZ + AO_VISIMAP_BYTES);
		SET_VARSIZE(value, VARHDRSZ + AO_VISIMAP_BYTES);
		memcpy(VARDATA(value), bits, AO_VISIMAP_BYTES);
		values[Anum_visimap_storage_id - 1] = Int64GetDatum(storage_id);
		values[Anum_visimap_segno - 1] = Int32GetDatum(segno);
		values[Anum_visimap_first_row - 1] = Int64GetDatum(first);
		values[Anum_visimap_bitmap - 1] = PointerGetDatum(value);
		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		if (exists)
			CatalogTupleUpdate(rel, &oldtid, tup);
		else
			CatalogTupleInsert(rel, tup);
	}

	index_close(idx, AccessShareLock);
	table_close(rel, RowExclusiveLock);
}

void
ao_visimap_delete_segfile(int64 storage_id, int segno)
{
	delete_segfile_rows("visimap", "visimap_key", storage_id, segno);
}
