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
 * gp_ao.h
 *	  What the files of gp_ao share: the table's layout on its pages, its
 *	  row numbers and TIDs, its metadata, its blocks of rows.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_AO_H
#define GP_AO_H

#include "access/tableam.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "gp_fault.h"

/*
 * A fault of Cloudberry's fault injector, set for this table or for any, at
 * the place in gp_ao that is Cloudberry's for it: what the tests set with
 * gp_inject_fault('appendonly_insert', ..., table) fires here.
 */
#define AO_FAULT(name, rel) \
	((gp_fault_active == NULL || *gp_fault_active == 0) ? GP_FAULT_NONE \
	 : GpFaultTrigger((name), "", RelationGetRelationName(rel)))

/* ------------------------------------------------------------------------- */
/* Segment files, row numbers and TIDs                                       */
/* ------------------------------------------------------------------------- */

/*
 * A table has up to 127 segment files, as Cloudberry's has, each written by
 * one writer at a time, so that as many can load it at once.  A row is
 * numbered within its segment file from 1, and a number is never used twice,
 * even by a transaction that rolls back, since an index entry may still name
 * it.
 *
 * A row's TID is its segment file's number in the block number's high 7 bits,
 * and its row number in the rest: 291 rows a block, the most PostgreSQL 19's
 * TID bitmap takes (MaxHeapTuplesPerPage), from offset 1.  So a segment file
 * holds up to 2^25 - 1 blocks of rows, about 9.8e9 rows, and block
 * 0xFFFFFFFF, which is no block, is never made.  BRIN walks each segment
 * file's run of blocks (O18).
 */
#define AO_MAX_SEGNO			127
#define AO_SEGNO_SHIFT			25
#define AO_ROWS_PER_TIDBLOCK	291
#define AO_MAX_TIDBLOCK_IN_SEG	((BlockNumber) 0x1FFFFFE)
#define AO_MAX_ROWNUM \
	((int64) (AO_MAX_TIDBLOCK_IN_SEG + 1) * AO_ROWS_PER_TIDBLOCK - 1)

static inline void
AoTidSet(ItemPointer tid, int segno, int64 rownum)
{
	BlockNumber blk = ((BlockNumber) segno << AO_SEGNO_SHIFT) |
		(BlockNumber) (rownum / AO_ROWS_PER_TIDBLOCK);

	ItemPointerSet(tid, blk,
				   (OffsetNumber) (rownum % AO_ROWS_PER_TIDBLOCK) + 1);
}

static inline int
AoTidSegno(ItemPointer tid)
{
	return (int) (ItemPointerGetBlockNumberNoCheck(tid) >> AO_SEGNO_SHIFT);
}

static inline int64
AoTidRownum(ItemPointer tid)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(tid) &
		(((BlockNumber) 1 << AO_SEGNO_SHIFT) - 1);

	return (int64) blk * AO_ROWS_PER_TIDBLOCK +
		ItemPointerGetOffsetNumberNoCheck(tid) - 1;
}

/* ------------------------------------------------------------------------- */
/* The table's options                                                       */
/* ------------------------------------------------------------------------- */

typedef enum AoCompressType
{
	AO_COMPRESS_NONE,
	AO_COMPRESS_ZLIB,
	AO_COMPRESS_ZSTD,
	AO_COMPRESS_RLE,			/* ao_column's rle_type: runs, then zlib */
} AoCompressType;

/*
 * What rd_options holds for an append-optimized table (O14): heap's first,
 * as the core reads every table's, then the access method's own.
 */
typedef struct AoRelOptions
{
	StdRdOptions std;
	int			blocksize;
	int			compresstype;	/* offset of the string, then AoCompressType */
	int			compresslevel;
	bool		checksum;
} AoRelOptions;

/* The options of rel, resolved, defaults filled in. */
typedef struct AoOptions
{
	int			blocksize;
	AoCompressType compresstype;
	int			compresslevel;
	bool		checksum;
} AoOptions;

extern void ao_options_init(void);
extern bytea *ao_row_reloptions(Datum reloptions, char relkind, bool validate);
extern bytea *ao_column_reloptions(Datum reloptions, char relkind,
								   bool validate);
extern void ao_get_options(Relation rel, AoOptions *opts);
extern void ao_options_from_reloptions(Datum reloptions, char relkind,
									   bool columnar, AoOptions *opts);
extern const char *ao_compresstype_name(AoCompressType type);

/* ------------------------------------------------------------------------- */
/* Storage: pages, files and WAL (ao_storage.c)                              */
/* ------------------------------------------------------------------------- */

/*
 * Block 0 of a table's main fork says which storage ID its files are, which
 * row numbers each segment file has handed out, and where the map of its
 * files' extents is.  A file of the table -- a segment file of a table by
 * row, or one column of a segment file of a table by column -- is a run of
 * extents of pages, each twice the last up to 1024 pages, taken from the end
 * of the relation as it grows; the map lists them, in pages of their own.
 * The map is not transactional: an extent a writer took stays its file's
 * when the writer rolls back, and the next writer of that file writes over
 * it, as Cloudberry's writer writes over what an aborted one left past the
 * end it committed.
 */
#define AO_METAPAGE_BLKNO		0
#define AO_MAX_MAP_PAGES		500
#define AO_PAYLOAD_OFFSET		MAXALIGN(SizeOfPageHeaderData)
#define AO_PAGE_PAYLOAD			(BLCKSZ - AO_PAYLOAD_OFFSET)

/* A file of segment file segno: column attno of a table by column, or 0. */
#define AoFileNum(segno, attno)	((uint32) (segno) + (uint32) (attno) * (AO_MAX_SEGNO + 1))

typedef struct AoFileExtents
{
	uint32		filenum;
	int			nextents;
	int			maxextents;
	BlockNumber *starts;		/* extent k's first block; its size is known */
} AoFileExtents;

extern int64 ao_storage_init(SMgrRelation srel, RelFileLocator rlocator,
							 char persistence, bool columnar);
extern int64 ao_storage_id(Relation rel);
extern bool ao_storage_is_columnar(Relation rel);
extern int64 ao_reserve_rownums(Relation rel, int segno, int64 count);
extern int64 ao_next_rownum(Relation rel, int segno);
extern void ao_file_write(Relation rel, uint32 filenum, uint64 offset,
						  const char *data, Size len);
extern void ao_file_read(Relation rel, uint32 filenum, uint64 offset,
						 char *data, Size len, BufferAccessStrategy strategy);
extern void ao_copy_storage(Relation rel, SMgrRelation dst,
							RelFileLocator dstlocator, char persistence);
extern void ao_storage_forget(Relation rel);
extern BlockNumber ao_file_end(Relation rel, uint32 filenum, int64 size);
extern void ao_register_rmgr(void);

/* ------------------------------------------------------------------------- */
/* Blocks of rows (ao_block.c)                                               */
/* ------------------------------------------------------------------------- */

/*
 * A file is a run of blocks, each of the rows first_rownum to first_rownum +
 * nrows - 1: a header, then the rows as the table stores them -- a table by
 * row, each row a MinimalTuple; a table by column, one column's values --
 * compressed as the table's options say.  A block is AO_BLOCK_HEADER_SIZE
 * plus stored_len bytes long, and the next begins where it ends.
 */
#define AO_BLOCK_MAGIC			0x414F4231	/* "AOB1" */

typedef struct AoBlockHeader
{
	uint32		magic;
	uint32		flags;			/* AoCompressType in the low byte */
	uint32		stored_len;		/* bytes that follow the header */
	uint32		raw_len;		/* their length uncompressed */
	int64		first_rownum;
	uint32		nrows;
	uint32		checksum;		/* CRC-32C of the header, then what follows */
} AoBlockHeader;

#define AO_BLOCK_HEADER_SIZE	MAXALIGN(sizeof(AoBlockHeader))

extern Size ao_block_encode(StringInfo raw, const AoOptions *opts,
							int64 first_rownum, uint32 nrows, StringInfo out);
extern void ao_block_decode(const char *stored, const AoBlockHeader *hdr,
							char *raw);
extern void ao_block_check_header(Relation rel, uint32 filenum, uint64 offset,
								  const AoBlockHeader *hdr);

/* ------------------------------------------------------------------------- */
/* Metadata: gp_ao.segfile, gp_ao.visimap, gp_ao.blkdir (ao_meta.c)          */
/* ------------------------------------------------------------------------- */

#define AO_SEGFILE_DEFAULT			1
#define AO_SEGFILE_AWAITING_DROP	2
#define AO_FORMAT_VERSION			1

/* A row of gp_ao.segfile: a segment file, as a snapshot sees it. */
typedef struct AoSegfile
{
	int			segno;
	int			ngroups;		/* files: 1, or one a column */
	int64	   *eof;
	int64	   *eof_uncompressed;
	int64		tupcount;
	int64		varblockcount;
	int64		modcount;
	int16		state;
	int16		formatversion;
	TransactionId compacted_by;
	ItemPointerData tid;		/* of the row, for an update */
} AoSegfile;

/* A row of gp_ao.blkdir: where a block of rows is in each file. */
typedef struct AoBlkdirEntry
{
	ItemPointerData tid;		/* of the directory's row */
	int			segno;
	int64		first_row;
	int			nrows;
	int			noffsets;
	int64	   *offsets;
} AoBlkdirEntry;

/* The rows of one segment file a snapshot sees deleted. */
#define AO_VISIMAP_ROWS		32768

/* A table's block size where its options give none: Cloudberry's. */
#define AO_DEFAULT_BLOCKSIZE	32768
#define AO_VISIMAP_BYTES	(AO_VISIMAP_ROWS / 8)

typedef struct AoVisimap
{
	int			segno;
	int			nentries;
	int64	   *first_rows;		/* in order */
	uint8	  **bitmaps;
} AoVisimap;

extern Oid	ao_meta_relid(const char *name, bool missing_ok);
extern AoSegfile *ao_segfiles_read(int64 storage_id, Snapshot snapshot,
								   int *nsegfiles);
extern AoSegfile *ao_segfiles_history(int64 storage_id, int *nsegfiles);
extern AoSegfile *ao_segfile_read(int64 storage_id, int segno,
								  Snapshot snapshot);
extern void ao_segfile_insert(int64 storage_id, int segno, int ngroups);
extern void ao_segfile_update(int64 storage_id, AoSegfile *sf);
extern void ao_segfile_delete(int64 storage_id, int segno);
extern void ao_meta_delete_storage(int64 storage_id);
extern int	ao_segfilecount_get(int64 storage_id);
extern void ao_segfilecount_set(int64 storage_id, int segfilecount);

extern void ao_blkdir_insert(int64 storage_id, int segno, int64 first_row,
							 int nrows, const int64 *offsets, int noffsets);
extern bool ao_blkdir_lookup(int64 storage_id, int segno, int64 rownum,
							 Snapshot snapshot, AoBlkdirEntry *entry);
extern void ao_blkdir_delete_segfile(int64 storage_id, int segno);
extern void ao_blkdir_insert_placeholder(int64 storage_id, int segno,
										 int64 first_row, ItemPointer tid);
extern void ao_blkdir_replace(ItemPointer tid, int64 storage_id, int segno,
							  int64 first_row, int nrows,
							  const int64 *offsets, int noffsets);

typedef struct AoBlkdirScan AoBlkdirScan;
extern AoBlkdirScan *ao_blkdir_scan_begin(int64 storage_id, int segno,
										  Snapshot snapshot);
extern bool ao_blkdir_scan_next(AoBlkdirScan *bs, AoBlkdirEntry *entry);
extern void ao_blkdir_scan_end(AoBlkdirScan *bs);

extern AoVisimap *ao_visimap_load(int64 storage_id, int segno,
								  Snapshot snapshot);
extern bool ao_visimap_is_deleted(AoVisimap *vm, int64 rownum);
extern void ao_visimap_delete_rows(int64 storage_id, int segno,
								   const int64 *rownums, int nrows);
extern void ao_visimap_delete_segfile(int64 storage_id, int segno);
extern int64 ao_visimap_count(AoVisimap *vm);

/* ------------------------------------------------------------------------- */
/* Rows in blocks (ao_block.c)                                               */
/* ------------------------------------------------------------------------- */

/*
 * A table by column keeps each column of a block apart: a header of its
 * flags and row count, a bitmap of its nulls where it has any, then its
 * values, laid out as a heap tuple lays out a column's, from a MAXALIGNed
 * start.
 */
typedef struct AoColumnBuilder
{
	int			nrows;
	bool		hasnulls;
	StringInfoData nulls;		/* a bit a row, 1 where NULL */
	StringInfoData values;
} AoColumnBuilder;

extern void ao_column_builder_init(AoColumnBuilder *cb);
extern void ao_column_builder_reset(AoColumnBuilder *cb);
extern void ao_column_append(AoColumnBuilder *cb, Form_pg_attribute att,
							 Datum value, bool isnull);
extern void ao_column_finish(AoColumnBuilder *cb, StringInfo raw);
extern void ao_column_decode(const char *raw, Size rawlen, Form_pg_attribute att,
							 int nrows, Datum *values, bool *isnull);
extern Datum ao_detoast_value(Form_pg_attribute att, Datum value);

/* ------------------------------------------------------------------------- */
/* Writing (ao_dml.c)                                                        */
/* ------------------------------------------------------------------------- */

/* The most rows a block holds, whatever its size. */
#define AO_MAX_ROWS_PER_BLOCK	16384

typedef struct AoInsertState AoInsertState;

extern bool ao_has_unique_index(Relation rel);
extern AoInsertState *ao_insert_state(Relation rel);
extern AoInsertState *ao_insert_turn(AoInsertState *st, Relation rel);
extern void ao_dml_set_compaction_writer(bool on);
extern void ao_insert_slot(AoInsertState *st, Relation rel,
						   TupleTableSlot *slot);
extern bool ao_pending_fetch(Relation rel, ItemPointer tid,
							 TupleTableSlot *slot);
extern bool ao_pending_flush(Relation rel, ItemPointer tid);
extern bool ao_delete_row(Relation rel, ItemPointer tid);
extern bool ao_deleted_by_this_command(Relation rel, ItemPointer tid);
extern bool ao_deleted_pending(Relation rel, ItemPointer tid);
extern void ao_dml_flush(Oid relid);
extern void ao_dml_forget_rel(Oid relid);
extern void ao_dml_run_begin(void *query);
extern void ao_dml_run_end(void *query);
extern void *ao_dml_current_query(void);
extern void ao_dml_finish_query(void *query);
extern void ao_dml_init(void);
extern int	ao_segfile_lock_classid(void);
extern bool ao_segfile_try_lock(Oid relid, int segno);

/* ------------------------------------------------------------------------- */
/* A column's own options, a partitioned table's (ao_encoding.c)             */
/* ------------------------------------------------------------------------- */

/* An ENCODING clause: a column's, or DEFAULT COLUMN ENCODING. */
typedef struct AoColumnEncoding
{
	char	   *colname;		/* NULL for the default */
	bool		is_default;
	bool		directive;		/* COLUMN c ENCODING, not in c's definition */
	List	   *opts;			/* DefElem, of String */
} AoColumnEncoding;

extern PGDLLIMPORT char *gp_default_storage_options;

extern void ao_encoding_init(void);
extern List *ao_enc_parse(const char *text);
extern char *ao_enc_format(List *opts);
extern void ao_enc_validate(List *opts, bool table);
extern List *ao_enc_fillin(List *given, const AoOptions *dflt);
extern void ao_column_options(Relation rel, AoOptions *colopts);
extern void ao_encoding_take(List **options, List **encodings);
struct GpEncodingMethod;
extern void ao_encoding_check(List *encodings,
							  const struct GpEncodingMethod *method);
extern List *ao_storage_opts_of(List *options);
extern List *ao_compression_opts_of(List *options);
extern List *ao_partitioned_take(List **options);
extern void ao_partitioned_set(Oid relid, List *opts);
extern void ao_partition_inherit(Oid parentid, List *pending, List **options);
extern void ao_encoding_clear(Oid relid);
extern void ao_replace_reloptions(Relation rel, Oid newam, List *opts);
extern void ao_encoding_apply(Oid relid, List *encodings, List *withopts,
							  List *only, bool replace);
extern void ao_encoding_set_column(Oid relid, const char *colname, List *opts);
extern void ao_encoding_apply_given(Oid relid, List *encodings, List *withopts);
extern void ao_encoding_set_column_given(Oid relid, const char *colname,
										 List *opts,
										 const struct GpEncodingMethod *method);
extern char *ao_encoding_type_label(const char *label);
extern bool ao_default_storage_options_check(char **newval, void **extra,
											 GucSource source);
extern void ao_default_storage_options_add(List **options);

/* ------------------------------------------------------------------------- */
/* The access methods (ao_am.c), VACUUM (ao_vacuum.c)                        */
/* ------------------------------------------------------------------------- */

extern PGDLLIMPORT int gp_appendonly_compaction_threshold;
extern PGDLLIMPORT int gp_appendonly_insert_files;
extern PGDLLIMPORT int gp_appendonly_insert_files_tuples_range;
extern PGDLLIMPORT bool gp_appendonly_compaction;
extern PGDLLIMPORT bool gp_select_invisible;

/* The bitmap index (bitmap/), which gp_ao carries, as Cloudberry's is built in. */
extern void bm_init(void);

extern bool ao_is_ao_table(Relation rel);
extern void ao_register_table_ams(void);
extern void ao_fetch_cache_reset(void);
extern void ao_scan_set_segno(TableScanDesc scan, int segno);
extern void ao_vacuum_rel(Relation rel, const struct VacuumParams *params,
						  BufferAccessStrategy bstrategy);
extern List *ao_vacuum_take_compacted(void);
extern void ao_vacuum_recycle_rel(Oid relid);

#endif							/* GP_AO_H */
