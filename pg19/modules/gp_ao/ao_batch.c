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
 * ao_batch.c
 *	  vexec's batch source for ao_row and ao_column: a scan's rows a batch
 *	  of columns at a time (pg_vector_executor.md §3.5.4; the contract is
 *	  vexec_source.h); and its batch sink for ao_column: an INSERT's rows a
 *	  batch of columns at a time (§3.16; vexec_sink.h), at the end.
 *
 * vexec begins the scan itself, through table_beginscan(), and hands its
 * descriptor to begin(): the batches come from gp_ao's own scan, over the
 * segment files, visibility maps and block directory its snapshot sees, as
 * ao_scan_start() and ao_scan_next_segfile() read them for getnextslot
 * (ao_am.c).  So MVCC, deletes, VACUUM's segment files and a parallel
 * scan's share stay gp_ao's (§3.1, principle 6), and the batches hold the
 * rows getnextslot would have returned, in its order, with its values; what
 * differs is only that a block's rows are taken as columns.
 *
 * A block of rows -- up to 16,384 (AO_MAX_ROWS_PER_BLOCK) -- is cut into
 * batches of up to the max_rows vexec asks for, by offset, and no batch
 * spans two blocks: a batch points into its block's decompressed bytes,
 * which stay as they are until the next block is read.
 *
 * ao_column.  Each column the scan reads is read, checksummed and
 * decompressed as getnextslot reads it (ao_reader_read()), and taken as it
 * lies in its block (ao_column_block_parts()) rather than value by value
 * (ao_column_decode()):
 *
 *	fixed-width, bool	a slice of the block where the batch's rows hold no
 *						NULL, since the block lays the values out one after
 *						another, each aligned as heap_fill_tuple() aligns it
 *						(ao_column_append()), which is PostgreSQL's array
 *						stride; where a row is NULL, which keeps no place in
 *						the block, each value copied to its row
 *	varlena, cstring	Datums pointing into the block, headers and all, as
 *						ao_column_decode() makes them; views over the block
 *						where vexec asks for views, the Datums beside them
 *						(§3.4.2)
 *
 * The block's bitmap of NULLs (1 = NULL) is inverted into validity.  A
 * column a block lacks -- one ALTER TABLE added without writing its values
 * -- is a constant, its missing value, as getnextslot gives it.  A block
 * whose rows are all deleted is not read at all.
 *
 * ao_row.  Each block is read as getnextslot reads it (ao_reader_load(),
 * which finds each row's MinimalTuple), and the columns the scan reads are
 * deformed from the MinimalTuples straight into the batch, as
 * heap_deform_tuple() deforms a heap tuple
 * (PG19:src/backend/access/common/heaptuple.c:1254-1366): fixed-width
 * values and bools into the batch's arrays, varlena values as Datums
 * pointing into the block, or views over it.
 *
 * Both.  The rows the visibility map deletes (1 = deleted, 32,768 rows an
 * entry, ao_meta.c) are left out of the batch's selection, never made NULL;
 * gp.select_invisible shows them, as it does to getnextslot.  A row's TID is
 * AoTidSet(segno, rownum), as getnextslot sets it.
 *
 * Memory.  A batch's arrays -- its selection, TIDs, validity, the values
 * copied or deformed -- are written over by the next batch, and its block by
 * the next block.  A batch vexec retains keeps both: the next batch gets
 * arrays of its own, and a reader that goes on to its next block reads it
 * into a new buffer, the retained batches keeping the old one until the
 * last of them is released (§3.4.5).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

#include "vexec_sink.h"
#include "vexec_source.h"

#include "gp_ao.h"

/* How a column is handed to vexec. */
typedef enum AoBatchKind
{
	AO_BATCH_FIXED,				/* VEXEC_FIXED: attlen bytes a value */
	AO_BATCH_BOOL,				/* VEXEC_BYTE_BOOL: a byte a value */
	AO_BATCH_DATUM,				/* VEXEC_DATUM, or VEXEC_VIEW: varlena and
								 * cstring values */
	AO_BATCH_TID				/* VEXEC_FIXED: the rows' TIDs, as ctid */
} AoBatchKind;

/*
 * Arrow's binary view, 16 bytes (arrow/docs/source/format/Columnar.rst:
 * 492-524): a value of up to 12 bytes inline, zero-padded; a longer one by
 * its size, its first 4 bytes, and the buffer and the offset it lies at.
 */
typedef union AoBatchView
{
	struct
	{
		int32		size;
		char		data[12];
	}			inlined;
	struct
	{
		int32		size;
		char		prefix[4];
		int32		buffer_index;
		int32		offset;
	}			ref;
	int64		align;
} AoBatchView;

#define AO_VIEW_INLINE	12

/*
 * A block's decompressed bytes that batches vexec retained point into.
 * Its reader reads its next block into another buffer, and the bytes go
 * with the last of those batches.
 */
typedef struct AoBatchKeep
{
	char	   *raw;
	int			refs;			/* retained batches that point into it */
	bool		reading;		/* still its reader's buffer */
} AoBatchKeep;

/* A reader of blocks that batches point into. */
typedef struct AoBatchBuf
{
	AoBlockReader *rd;
	AoBatchKeep *keep;			/* while a retained batch points into it */
} AoBatchBuf;

/* A column the scan reads. */
typedef struct AoBatchCol
{
	AttrNumber	attnum;
	AoBatchKind kind;
	int16		attlen;
	bool		attbyval;
	char		attalign;
	int32		width;			/* FIXED, BOOL, TID: bytes a value */
	int32		stride;			/* FIXED, BOOL, TID: from one value to the
								 * next, as vexec takes them */
	int32		blkstride;		/* FIXED, BOOL: as a column block lays them
								 * out */
	bool		views;			/* DATUM: vexec asks for views */
	int			dup_of;			/* a column asked for twice: its first; or
								 * -1 */
	/* where a row or a block lacks the column: getmissingattr()'s value */
	bool		missing_null;
	Datum		missing;
	const void *missing_value;	/* the same, in the column's layout */
	/* ao_column: the column's block of the directory entry being read */
	AoBlockReader rd;
	AoBatchBuf	buf;
	bool		present;		/* the entry has a block of the column */
	const uint8 *nulls;			/* its bitmap of NULLs, 1 = NULL; or NULL */
	const char *data;			/* its values */
	const char *end;			/* the end of its bytes */
	int			crow;			/* the row the cursor is at */
	uintptr_t	cval;			/* the cursor's value: FIXED, BOOL, its
								 * number; DATUM, its offset */
} AoBatchCol;

/*
 * The arrays one batch hands vexec, for max_rows rows, which the next batch
 * writes over, unless vexec retains this one.
 */
typedef struct AoBatchOut
{
	MemoryContext cxt;			/* what follows, and this */
	bool		retained;
	VexecColumn *columns;
	uint64	   *visible;
	ItemPointerData *tids;
	/* each column's own */
	uint64	  **validity;
	char	  **values;			/* FIXED, BOOL: values; DATUM: Datums */
	int		   *nnulls;			/* ao_row: the batch's NULLs */
	AoBatchView **views;
	char	  **copies;			/* bytes views point at that are not in the
								 * block */
	Size	   *copycap;
	const void **buffers;		/* a column's two view buffers: the block,
								 * the copies */
	int64	   *buffer_sizes;
	/* the readers whose blocks it points into, and what keeps them */
	int			nbufs;
	AoBatchBuf **bufs;
	AoBatchKeep **keeps;
} AoBatchOut;

/* A scan's batches: what begin() makes. */
typedef struct AoBatchState
{
	AoScanDesc	scan;			/* gp_ao's own, which vexec began */
	Relation	rel;
	TupleDesc	desc;
	bool		columnar;
	MemoryContext cxt;
	int			ncols;
	AoBatchCol *cols;
	int			max_rows;
	bool		tids;			/* the batches carry their rows' TIDs */
	bool		compact;		/* and hand their visible rows alone */
	/* ao_row: the attributes deformed, and the column each fills, or -1 */
	int			maxatt;
	int		   *colof;
	AoBatchBuf	rowbuf;			/* the scan's reader of blocks of rows */
	/* the block being cut into batches */
	int			segno;
	int64		first_rownum;
	int			nrows;
	int			pos;			/* its next row */
	bool		loaded;			/* ao_column: its columns are read */
	AoBatchOut *out;			/* the last batch's arrays */
} AoBatchState;

/* Whether vexec's spec has a member: it was built with a struct that long. */
#define AO_SPEC_HAS(spec, member) \
	((spec)->size >= offsetof(VexecSourceSpec, member) + sizeof((spec)->member))

static inline bool
ao_batch_bit(const uint64 *words, int i)
{
	return (words[i >> 6] >> (i & 63)) & 1;
}

static inline void
ao_batch_bit_clear(uint64 *words, int i)
{
	words[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
}

/* Is row row NULL in a column block's bitmap of NULLs? */
static inline bool
ao_batch_isnull(const uint8 *nulls, int row)
{
	return nulls != NULL && (nulls[row >> 3] & (1 << (row & 7))) != 0;
}

/* Words of nrows bits, every one set, the bits past them clear. */
static void
ao_batch_all_set(uint64 *words, int nrows)
{
	int			nwords = (nrows + 63) / 64;

	for (int w = 0; w < nwords; w++)
		words[w] = ~UINT64CONST(0);
	if (nrows % 64 != 0)
		words[nwords - 1] = (UINT64CONST(1) << (nrows % 64)) - 1;
}

/* One fixed-width value, of width bytes. */
static inline void
ao_batch_copy(char *dst, const char *src, int width)
{
	switch (width)
	{
		case 1:
			*dst = *src;
			break;
		case 2:
			memcpy(dst, src, 2);
			break;
		case 4:
			memcpy(dst, src, 4);
			break;
		case 8:
			memcpy(dst, src, 8);
			break;
		default:
			memcpy(dst, src, width);
			break;
	}
}

/* As ao_column_decode() says of a block whose values run past its end. */
static void
ao_batch_corrupt(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("column block of an append-optimized table ends before its values do")));
}

/* A column's layout as vexec names it, for its kind (vexec_source.h). */
static void
ao_batch_shape(const AoBatchCol *c, VexecColumn *col)
{
	switch (c->kind)
	{
		case AO_BATCH_FIXED:
		case AO_BATCH_TID:
			col->layout = VEXEC_FIXED;
			break;
		case AO_BATCH_BOOL:
			col->layout = VEXEC_BYTE_BOOL;
			break;
		case AO_BATCH_DATUM:
			col->layout = VEXEC_DATUM;
			break;
	}
	/* vexec compares these too: 1 and 1 for a byte a bool, 0 for Datums */
	col->width = c->width;
	col->stride = c->stride;
}

/* ------------------------------------------------------------------------- */
/* A batch's arrays                                                          */
/* ------------------------------------------------------------------------- */

static AoBatchOut *
ao_batch_out_make(AoBatchState *st)
{
	MemoryContext cxt = AllocSetContextCreate(st->cxt, "gp_ao batch",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	AoBatchOut *out = palloc0_object(AoBatchOut);
	int			nwords = (st->max_rows + 63) / 64;
	int			n = Max(st->ncols, 1);

	out->cxt = cxt;
	out->columns = palloc0_array(VexecColumn, n);
	out->visible = palloc_array(uint64, nwords);
	if (st->tids)
		out->tids = palloc_array(ItemPointerData, st->max_rows);
	out->validity = palloc0_array(uint64 *, n);
	out->values = palloc0_array(char *, n);
	out->nnulls = palloc0_array(int, n);
	out->views = palloc0_array(AoBatchView *, n);
	out->copies = palloc0_array(char *, n);
	out->copycap = palloc0_array(Size, n);
	out->buffers = palloc0_array(const void *, 2 * n);
	out->buffer_sizes = palloc0_array(int64, 2 * n);
	out->bufs = palloc0_array(AoBatchBuf *, n + 1);
	out->keeps = palloc0_array(AoBatchKeep *, n + 1);
	for (int i = 0; i < st->ncols; i++)
	{
		AoBatchCol *c = &st->cols[i];

		if (c->dup_of >= 0 || c->kind == AO_BATCH_TID)
			continue;
		out->validity[i] = palloc_array(uint64, nwords);
		if (c->kind == AO_BATCH_DATUM)
		{
			out->values[i] = palloc0(sizeof(Datum) * st->max_rows);
			if (c->views)
				out->views[i] = palloc_array(AoBatchView, st->max_rows);
		}
		else
			out->values[i] = palloc0((Size) c->stride * st->max_rows);
	}
	MemoryContextSwitchTo(old);
	return out;
}

/* The arrays of the next batch: the last batch's, unless vexec retained it. */
static AoBatchOut *
ao_batch_out(AoBatchState *st)
{
	if (st->out == NULL || st->out->retained)
		st->out = ao_batch_out_make(st);
	return st->out;
}

/*
 * Before a reader reads its next block: if a retained batch points into its
 * buffer, the buffer is the retained batches' now, and the reader reads into
 * a new one.
 */
static void
ao_batch_unkeep(AoBatchBuf *b)
{
	AoBatchKeep *keep = b->keep;

	if (keep == NULL)
		return;
	b->keep = NULL;
	keep->reading = false;
	if (keep->refs == 0)
	{
		/* each batch that kept it is released: the reader's again */
		pfree(keep);
		return;
	}
	b->rd->raw = NULL;
	b->rd->rawcap = 0;
	b->rd->loaded = false;
}

/*
 * Views over the block for a column's Datums, where vexec asks for views
 * (§3.4.2).  A value of up to 12 bytes is inline; a longer one is pointed
 * at where it lies in the block, base.  A value a view cannot point at
 * there -- compressed inline, which a view needs decompressed, or not in the
 * block at all, as a missing value -- is decompressed or copied into the
 * column's buffer of copies, its Datum kept as it is stored, as vexec's own
 * conversion from Datums keeps it.  False, the column left Datums for vexec
 * to convert, where the copies would pass int32's offsets.
 */
static bool
ao_batch_views(const AoBatchCol *c, AoBatchOut *out, int i, VexecColumn *col,
			   int nrows, const char *base, Size baselen)
{
	const Datum *datums = (const Datum *) col->values;
	AoBatchView *views = out->views[i];
	Size		used = 0;

	for (int j = 0; j < nrows; j++)
	{
		AoBatchView *v = &views[j];
		const char *p;
		Size		len;
		varlena    *plain = NULL;

		memset(v, 0, sizeof(AoBatchView));
		if (col->validity != NULL && !ao_batch_bit(col->validity, j))
			continue;
		p = DatumGetPointer(datums[j]);
		if (c->attlen == -2)
			len = strlen(p);
		else
		{
			if (VARATT_IS_EXTERNAL(p) || VARATT_IS_COMPRESSED(p))
				p = (const char *) (plain = detoast_attr((varlena *) p));
			len = VARSIZE_ANY_EXHDR(p);
			p = VARDATA_ANY(p);
		}
		v->inlined.size = (int32) len;
		if (len <= AO_VIEW_INLINE)
			memcpy(v->inlined.data, p, len);
		else if (plain == NULL && p >= base && p + len <= base + baselen)
		{
			memcpy(v->ref.prefix, p, 4);
			v->ref.buffer_index = 0;
			v->ref.offset = (int32) (p - base);
		}
		else
		{
			if (used + len > (Size) PG_INT32_MAX)
			{
				if (plain != NULL)
					pfree(plain);
				return false;
			}
			if (out->copycap[i] < used + len)
			{
				Size		cap = Max(used + len, Max(out->copycap[i] * 2, 8192));

				out->copies[i] = out->copies[i] ?
					repalloc_huge(out->copies[i], cap) :
					MemoryContextAllocHuge(out->cxt, cap);
				out->copycap[i] = cap;
			}
			memcpy(out->copies[i] + used, p, len);
			memcpy(v->ref.prefix, p, 4);
			v->ref.buffer_index = 1;
			v->ref.offset = (int32) used;
			used += len;
		}
		if (plain != NULL)
			pfree(plain);
	}

	out->buffers[2 * i] = base;
	out->buffer_sizes[2 * i] = (int64) baselen;
	out->buffers[2 * i + 1] = out->copies[i];
	out->buffer_sizes[2 * i + 1] = (int64) used;
	col->layout = VEXEC_VIEW;
	col->datums = datums;
	col->values = views;
	col->buffers = &out->buffers[2 * i];
	col->buffer_sizes = &out->buffer_sizes[2 * i];
	col->nbuffers = used > 0 ? 2 : 1;
	return true;
}

/* A column a block or a row lacks: its missing value, for every row. */
static void
ao_batch_missing(const AoBatchCol *c, VexecColumn *col)
{
	static const uint64 null_row = 0;	/* its one value, NULL */

	ao_batch_shape(c, col);
	col->encoding = VEXEC_CONST;
	col->values = c->missing_value;
	col->validity = c->missing_null ? &null_row : NULL;
}

/* ------------------------------------------------------------------------- */
/* ao_column: a block of each column, taken as it lies                       */
/* ------------------------------------------------------------------------- */

/*
 * The directory entry's block of each column the scan reads, once a batch
 * of its rows has one to hand.
 */
static void
ao_batch_load(AoBatchState *st)
{
	AoScanDesc	scan = st->scan;
	AoBlkdirEntry *e = &scan->entry;

	for (int i = 0; i < st->ncols; i++)
	{
		AoBatchCol *c = &st->cols[i];
		int			a = c->attnum - 1;

		if (c->dup_of >= 0 || c->kind == AO_BATCH_TID)
			continue;
		/* as getnextslot finds it (ao_column_getnextslot()) */
		c->present = a < e->noffsets && e->offsets[a] >= 0;
		if (!c->present)
			continue;
		ao_batch_unkeep(&c->buf);
		c->rd.filenum = AoFileNum(st->segno, c->attnum);
		ao_reader_read(st->rel, &c->rd, e->offsets[a], scan->strategy);
		if (c->rd.hdr.nrows != (uint32) e->nrows)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block of rows " INT64_FORMAT " of column %d of append-optimized table \"%s\" holds %u rows where its directory says %d",
							e->first_row, c->attnum,
							RelationGetRelationName(st->rel),
							c->rd.hdr.nrows, e->nrows)));
		ao_column_block_parts(c->rd.raw, c->rd.hdr.raw_len, e->nrows,
							  &c->nulls, &c->data);
		c->end = c->rd.raw + c->rd.hdr.raw_len;
		/* and so its bitmap of NULLs, which lies before its values, too */
		if (c->data > c->end)
			ao_batch_corrupt();
		c->rd.loaded = true;
		c->crow = 0;
		c->cval = 0;
	}
	st->loaded = true;
}

/*
 * The varlena or cstring value at offset *off of a column block's values,
 * and *off past it, as ao_column_decode() walks them.
 */
static inline Datum
ao_batch_value_at(const AoBatchCol *c, uintptr_t *off)
{
	const char *data = c->data;
	uintptr_t	o = *off;

	if (data + o >= c->end)
		ao_batch_corrupt();
	if (c->attlen == -1)
	{
		o = att_align_pointer(o, c->attalign, -1, data + o);
		if (data + o >= c->end)
			ao_batch_corrupt();
	}
	*off = att_addlength_pointer(o, c->attlen, data + o);
	return PointerGetDatum(data + o);
}

/*
 * The cursor on to row row, past the values of the rows before it: those of
 * batches whose rows were all deleted.
 */
static void
ao_batch_seek(AoBatchCol *c, int row)
{
	for (; c->crow < row; c->crow++)
	{
		if (ao_batch_isnull(c->nulls, c->crow))
			continue;
		if (c->kind == AO_BATCH_DATUM)
			(void) ao_batch_value_at(c, &c->cval);
		else
			c->cval++;
	}
}

/*
 * The validity of n rows of a column block from row row: its bitmap of
 * NULLs inverted, into words.  Returns how many are NULL.
 */
static int
ao_batch_validity(const uint8 *nulls, int row, int n, uint64 *words)
{
	int			nnull = 0;

	for (int w = 0; w * 64 < n; w++)
	{
		int			base = row + w * 64;
		int			m = Min(64, n - w * 64);
		uint64		word = 0;

		if ((base & 7) == 0)
		{
			for (int b = 0; b * 8 < m; b++)
				word |= (uint64) nulls[(base >> 3) + b] << (8 * b);
		}
		else
		{
			for (int b = 0; b < m; b++)
				word |= (uint64) ((nulls[(base + b) >> 3] >> ((base + b) & 7)) & 1) << b;
		}
		word = ~word;
		if (m < 64)
			word &= (UINT64CONST(1) << m) - 1;
		nnull += m - pg_popcount64(word);
		words[w] = word;
	}
	return nnull;
}

/* The nvalues fixed-width values from the cursor lie within the block. */
static inline void
ao_batch_check_fixed(const AoBatchCol *c, int nvalues)
{
	if (nvalues > 0 &&
		(Size) (c->end - c->data) <
		(Size) (c->cval + nvalues - 1) * c->blkstride + c->width)
		ao_batch_corrupt();
}

/*
 * A fixed-width or bool column's n rows from its cursor: a slice of the
 * block where none is NULL and the block's stride is vexec's; else each
 * value copied to its row.  keep, where the batch hands its visible rows
 * alone, says which of the n it keeps: nout of them.
 */
static void
ao_batch_take_fixed(AoBatchCol *c, AoBatchOut *out, int i, VexecColumn *col,
					int n, int nout, const uint64 *keep)
{
	const char *src = c->data + c->cval * c->blkstride;
	char	   *dst = out->values[i];
	uint64	   *valid = out->validity[i];
	int			nnull = 0;
	int			nvalues;

	ao_batch_shape(c, col);
	if (keep == NULL)
	{
		if (c->nulls != NULL)
			nnull = ao_batch_validity(c->nulls, c->crow, n, valid);
		nvalues = n - nnull;
		ao_batch_check_fixed(c, nvalues);
		if (nnull == 0 && c->blkstride == c->stride)
			col->values = src;	/* the block's own bytes */
		else
		{
			for (int j = 0; j < n; j++)
			{
				char	   *d = dst + (Size) j * c->stride;

				if (nnull > 0 && !ao_batch_bit(valid, j))
				{
					memset(d, 0, c->width);
					continue;
				}
				ao_batch_copy(d, src, c->width);
				src += c->blkstride;
			}
			col->values = dst;
		}
	}
	else
	{
		int			k = 0;

		nvalues = 0;
		for (int j = 0; j < n; j++)
			nvalues += !ao_batch_isnull(c->nulls, c->crow + j);
		ao_batch_check_fixed(c, nvalues);
		ao_batch_all_set(valid, nout);
		for (int j = 0; j < n; j++)
		{
			bool		isnull = ao_batch_isnull(c->nulls, c->crow + j);

			if (ao_batch_bit(keep, j))
			{
				char	   *d = dst + (Size) k * c->stride;

				if (isnull)
				{
					memset(d, 0, c->width);
					ao_batch_bit_clear(valid, k);
					nnull++;
				}
				else
					ao_batch_copy(d, src, c->width);
				k++;
			}
			if (!isnull)
				src += c->blkstride;
		}
		col->values = dst;
	}
	col->validity = nnull > 0 ? valid : NULL;
	c->cval += nvalues;
	c->crow += n;
}

/*
 * A varlena or cstring column's n rows from its cursor: Datums pointing
 * into the block.  keep and nout as for ao_batch_take_fixed().
 */
static void
ao_batch_take_datums(AoBatchCol *c, AoBatchOut *out, int i, VexecColumn *col,
					 int n, int nout, const uint64 *keep)
{
	Datum	   *datums = (Datum *) out->values[i];
	uint64	   *valid = out->validity[i];
	int			nnull = 0;
	int			k = 0;

	ao_batch_all_set(valid, nout);
	for (int j = 0; j < n; j++)
	{
		bool		isnull = ao_batch_isnull(c->nulls, c->crow + j);
		Datum		d = isnull ? (Datum) 0 : ao_batch_value_at(c, &c->cval);

		if (keep != NULL && !ao_batch_bit(keep, j))
			continue;
		datums[k] = d;
		if (isnull)
		{
			ao_batch_bit_clear(valid, k);
			nnull++;
		}
		k++;
	}
	c->crow += n;
	ao_batch_shape(c, col);
	col->values = datums;
	col->validity = nnull > 0 ? valid : NULL;
}

/* ao_column: rows r0 to r0 + n - 1 of the entry, nout of them kept. */
static void
ao_batch_columns(AoBatchState *st, AoBatchOut *out, int r0, int n, int nout,
				 const uint64 *keep)
{
	if (!st->loaded)
		ao_batch_load(st);
	out->nbufs = 0;
	for (int i = 0; i < st->ncols; i++)
	{
		AoBatchCol *c = &st->cols[i];
		VexecColumn *col = &out->columns[i];

		if (c->dup_of >= 0 || c->kind == AO_BATCH_TID)
			continue;
		memset(col, 0, sizeof(VexecColumn));
		if (!c->present)
		{
			ao_batch_missing(c, col);
			continue;
		}
		out->bufs[out->nbufs++] = &c->buf;
		ao_batch_seek(c, r0);
		if (c->kind != AO_BATCH_DATUM)
			ao_batch_take_fixed(c, out, i, col, n, nout, keep);
		else
		{
			ao_batch_take_datums(c, out, i, col, n, nout, keep);
			if (c->views)
				(void) ao_batch_views(c, out, i, col, nout, c->rd.raw,
									  c->rd.hdr.raw_len);
		}
	}
}

/*
 * ao_column: on to the scan's next entry of its block directory, and to its
 * next segment file at the end of one; false at the end of the last.  The
 * entry's blocks are read when a batch of it has a row to hand.
 */
static bool
ao_batch_next_entry(AoBatchState *st)
{
	AoScanDesc	scan = st->scan;

	for (;;)
	{
		if (scan->bds != NULL)
		{
			/* into the scan's memory, as getnextslot reads it */
			MemoryContext old = MemoryContextSwitchTo(scan->cxt);
			bool		more = ao_blkdir_scan_next(scan->bds, &scan->entry);

			MemoryContextSwitchTo(old);
			if (more)
			{
				st->first_rownum = scan->entry.first_row;
				st->nrows = Max(scan->entry.nrows, 0);
				st->pos = 0;
				st->loaded = false;
				return true;
			}
		}
		if (!ao_scan_next_segfile(scan))
			return false;
		st->segno = scan->segfiles[scan->cursf].segno;
	}
}

/* ------------------------------------------------------------------------- */
/* ao_row: a block's MinimalTuples, deformed into columns                    */
/* ------------------------------------------------------------------------- */

/* A value of row k into column ci's array. */
static inline void
ao_batch_put(AoBatchState *st, AoBatchOut *out, int ci, int k, Datum d)
{
	const AoBatchCol *c = &st->cols[ci];

	switch (c->kind)
	{
		case AO_BATCH_FIXED:
			if (c->attbyval)
				store_att_byval(out->values[ci] + (Size) k * c->stride, d,
								c->attlen);
			else
				memcpy(out->values[ci] + (Size) k * c->stride,
					   DatumGetPointer(d), c->attlen);
			break;
		case AO_BATCH_BOOL:
			/* the byte as it is stored, as fetch_att() gives it */
			((uint8 *) out->values[ci])[k] = (uint8) DatumGetChar(d);
			break;
		case AO_BATCH_DATUM:
			((Datum *) out->values[ci])[k] = d;
			break;
		case AO_BATCH_TID:
			break;
	}
}

static inline void
ao_batch_put_null(AoBatchState *st, AoBatchOut *out, int ci, int k)
{
	const AoBatchCol *c = &st->cols[ci];

	ao_batch_bit_clear(out->validity[ci], k);
	out->nnulls[ci]++;
	if (c->kind == AO_BATCH_DATUM)
		((Datum *) out->values[ci])[k] = (Datum) 0;
	else
		memset(out->values[ci] + (Size) k * c->stride, 0, c->width);
}

/*
 * ao_row: rows r0 to r0 + n - 1 of the block deformed into the batch, those
 * keep keeps where it is given, nout of them.  Each row as heap_deform_tuple()
 * deforms a heap tuple: its bitmap of NULLs, and each attribute aligned,
 * fetched and passed with align_fetch_then_add(), as far as the last column
 * the scan reads; past the attributes a row has, the missing values
 * getmissingattr() gives, as slot_getmissingattrs() does.
 */
static void
ao_batch_rows(AoBatchState *st, AoBatchOut *out, int r0, int n, int nout,
			  const uint64 *keep)
{
	AoBlockReader *rd = &st->scan->row;
	int			k = 0;

	for (int i = 0; i < st->ncols; i++)
	{
		if (st->cols[i].dup_of >= 0 || st->cols[i].kind == AO_BATCH_TID)
			continue;
		ao_batch_all_set(out->validity[i], nout);
		out->nnulls[i] = 0;
	}

	for (int j = 0; j < n && st->maxatt > 0; j++)
	{
		MinimalTuple mtup;
		HeapTupleHeader tup;
		bool		hasnulls;
		char	   *tp;
		uint32		off = 0;
		int			natts;
		int			a;

		if (keep != NULL && !ao_batch_bit(keep, j))
			continue;
		mtup = (MinimalTuple) (rd->raw + rd->rowoffs[r0 + j]);
		tup = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
		hasnulls = (tup->t_infomask & HEAP_HASNULL) != 0;
		tp = (char *) tup + tup->t_hoff;
		natts = Min(HeapTupleHeaderGetNatts(tup), st->maxatt);

		for (a = 0; a < natts; a++)
		{
			int			ci = st->colof[a];
			CompactAttribute *cattr;
			Datum		d;

			if (hasnulls && att_isnull(a, tup->t_bits))
			{
				if (ci >= 0)
					ao_batch_put_null(st, out, ci, k);
				continue;
			}
			cattr = TupleDescCompactAttr(st->desc, a);
			d = align_fetch_then_add(tp, &off, cattr->attbyval, cattr->attlen,
									 cattr->attalignby);
			if (ci >= 0)
				ao_batch_put(st, out, ci, k, d);
		}
		for (; a < st->maxatt; a++)
		{
			int			ci = st->colof[a];

			if (ci < 0)
				continue;
			if (st->cols[ci].missing_null)
				ao_batch_put_null(st, out, ci, k);
			else
				ao_batch_put(st, out, ci, k, st->cols[ci].missing);
		}
		k++;
	}

	for (int i = 0; i < st->ncols; i++)
	{
		AoBatchCol *c = &st->cols[i];
		VexecColumn *col = &out->columns[i];

		if (c->dup_of >= 0 || c->kind == AO_BATCH_TID)
			continue;
		memset(col, 0, sizeof(VexecColumn));
		ao_batch_shape(c, col);
		col->values = out->values[i];
		col->validity = out->nnulls[i] > 0 ? out->validity[i] : NULL;
		if (c->views)
			(void) ao_batch_views(c, out, i, col, nout, rd->raw,
								  rd->hdr.raw_len);
	}
	out->bufs[0] = &st->rowbuf;
	out->nbufs = 1;
}

/*
 * ao_row: on to the scan's next block of rows, and to its next segment file
 * at the end of one, as getnextslot goes (ao_row_getnextslot()); false at
 * the end of the last.
 */
static bool
ao_batch_next_block(AoBatchState *st)
{
	AoScanDesc	scan = st->scan;

	for (;;)
	{
		if (scan->cursf >= 0 && scan->cursf < scan->nsegfiles &&
			scan->row.next < scan->eof)
		{
			ao_batch_unkeep(&st->rowbuf);
			ao_reader_load(st->rel, &scan->row, scan->row.next, NULL,
						   scan->strategy);
			st->first_rownum = scan->row.hdr.first_rownum;
			st->nrows = (int) scan->row.hdr.nrows;
			st->pos = 0;
			return true;
		}
		if (!ao_scan_next_segfile(scan))
			return false;
		st->segno = scan->segfiles[scan->cursf].segno;
	}
}

/* ------------------------------------------------------------------------- */
/* The source                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Every column of either access method can be read: a type of fixed width
 * is taken as its bytes, any other as Datums, and vexec converts each to
 * the layout its format gives the type.  So the slot path is never needed
 * for a column; only a dropped one, which vexec never asks for, and a
 * system attribute other than ctid are refused.
 */
static bool
ao_batch_supports(Relation rel, AttrNumber attnum)
{
	if (attnum == SelfItemPointerAttributeNumber)
		return true;
	return attnum >= 1 && attnum <= RelationGetNumberOfAttributes(rel) &&
		!TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attisdropped;
}

/* A column the scan reads: how it is taken, and its missing value. */
static void
ao_batch_col_init(AoBatchState *st, AoBatchCol *c, Form_pg_attribute att,
				  uint8 asked)
{
	c->attlen = att->attlen;
	c->attbyval = att->attbyval;
	c->attalign = att->attalign;
	if (att->attlen > 0)
	{
		c->kind = getBaseType(att->atttypid) == BOOLOID ? AO_BATCH_BOOL :
			AO_BATCH_FIXED;
		c->width = att->attlen;

		/*
		 * A column block lays a value out at the type's alignment, as heap
		 * lays out a tuple's (ao_column_append()), so one after another
		 * they are TYPEALIGN(typalign, typlen) apart: the stride
		 * PostgreSQL's arrays give a type
		 * (PG19:src/backend/utils/adt/arrayfuncs.c:3546-3547), at which
		 * vexec takes a type passed by reference.  One passed by value it
		 * takes at its width, which is that stride for every type but one
		 * aligned wider than it is long.
		 */
		c->blkstride = att_align_nominal(att->attlen, att->attalign);
		c->stride = att->attbyval ? att->attlen : c->blkstride;
	}
	else
	{
		c->kind = AO_BATCH_DATUM;
		c->views = asked == VEXEC_VIEW;
	}

	c->missing = getmissingattr(st->desc, att->attnum, &c->missing_null);
	if (c->kind == AO_BATCH_DATUM)
	{
		Datum	   *d = palloc_object(Datum);

		*d = c->missing;
		c->missing_value = d;
	}
	else
	{
		char	   *v = palloc0(Max(c->stride, (int32) sizeof(Datum)));

		if (!c->missing_null && c->attbyval)
			store_att_byval(v, c->missing, c->attlen);
		else if (!c->missing_null)
			memcpy(v, DatumGetPointer(c->missing), c->attlen);
		c->missing_value = v;
	}
}

static void *
ao_batch_begin(TableScanDesc sscan, const VexecSourceSpec *spec)
{
	AoScanDesc	scan = (AoScanDesc) sscan;
	Relation	rel = scan->rs_base.rs_rd;
	TupleDesc	desc = RelationGetDescr(rel);
	int			max_rows = AO_SPEC_HAS(spec, max_rows) ? spec->max_rows : 0;
	uint32		flags = AO_SPEC_HAS(spec, flags) ? spec->flags : 0;
	const uint8 *layouts = AO_SPEC_HAS(spec, layouts) ? spec->layouts : NULL;
	MemoryContext cxt;
	MemoryContext old;
	AoBatchState *st;

	if (!ao_is_ao_table(rel))
		elog(ERROR, "gp_ao's batch source was given a scan of \"%s\", which is not append-optimized",
			 RelationGetRelationName(rel));

	/*
	 * getnextslot tests a scan's keys row by row (ao_scan_keys_ok()); vexec
	 * begins its scans with none, and the batches have no place to test
	 * them.
	 */
	if (scan->rs_base.rs_nkeys > 0)
		elog(ERROR, "gp_ao's batch source cannot read a scan with scan keys");

	/* the scan's child: whatever happens, it goes with the scan */
	cxt = AllocSetContextCreate(scan->cxt, "gp_ao batches",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	st = palloc0_object(AoBatchState);
	st->scan = scan;
	st->rel = rel;
	st->desc = desc;
	st->columnar = scan->columnar;
	st->cxt = cxt;
	st->max_rows = max_rows > 0 ? Min(max_rows, AO_MAX_ROWS_PER_BLOCK) :
		AO_MAX_ROWS_PER_BLOCK;
	st->tids = (flags & VEXEC_SRC_TIDS) != 0;
	st->compact = (flags & VEXEC_SRC_COMPACT) != 0;
	st->ncols = spec->ncolumns;
	st->cols = palloc0_array(AoBatchCol, Max(st->ncols, 1));
	st->colof = palloc_array(int, Max(desc->natts, 1));
	for (int a = 0; a < desc->natts; a++)
		st->colof[a] = -1;
	st->rowbuf.rd = &scan->row;

	for (int i = 0; i < st->ncols; i++)
	{
		AoBatchCol *c = &st->cols[i];
		AttrNumber	attnum = spec->attnums[i];

		c->attnum = attnum;
		c->dup_of = -1;
		if (attnum == SelfItemPointerAttributeNumber)
		{
			c->kind = AO_BATCH_TID;
			c->width = c->stride = sizeof(ItemPointerData);
			st->tids = true;
			continue;
		}
		if (!ao_batch_supports(rel, attnum))
			elog(ERROR, "gp_ao's batch source cannot read attribute %d of \"%s\"",
				 attnum, RelationGetRelationName(rel));
		if (st->colof[attnum - 1] >= 0)
		{
			c->dup_of = st->colof[attnum - 1];
			continue;
		}
		st->colof[attnum - 1] = i;
		st->maxatt = Max(st->maxatt, attnum);
		ao_batch_col_init(st, c, TupleDescAttr(desc, attnum - 1),
						  layouts ? layouts[i] : VEXEC_DATUM);
		ao_reader_init(&c->rd, cxt);
		c->buf.rd = &c->rd;
	}
	MemoryContextSwitchTo(old);
	return st;
}

static bool
ao_batch_next(void *arg, VexecSourceBatch *batch)
{
	AoBatchState *st = (AoBatchState *) arg;
	AoScanDesc	scan = st->scan;
	Relation	rel = st->rel;

	if (!scan->started)
		ao_scan_start(scan);

	for (;;)
	{
		AoBatchOut *out;
		const uint64 *keep = NULL;
		int			r0;
		int			n;
		int			nout;
		int			ndeleted;

		/* once a batch, and a block or a segment file passed over */
		CHECK_FOR_INTERRUPTS();
		if (st->pos >= st->nrows)
		{
			if (!(st->columnar ? ao_batch_next_entry(st) :
				  ao_batch_next_block(st)))
				return false;
			continue;
		}

		r0 = st->pos;
		n = Min(st->max_rows, st->nrows - r0);
		st->pos += n;
		out = ao_batch_out(st);
		ndeleted = gp_select_invisible ? 0 :
			ao_visimap_visible_words(scan->vm, st->first_rownum + r0, n,
									 out->visible);
		if (ndeleted == n)
			continue;			/* every row deleted: nothing to hand */
		nout = n;
		if (ndeleted > 0 && st->compact)
		{
			keep = out->visible;
			nout = n - ndeleted;
		}

		if (st->columnar)
			ao_batch_columns(st, out, r0, n, nout, keep);
		else
			ao_batch_rows(st, out, r0, n, nout, keep);

		if (st->tids)
		{
			int			k = 0;

			for (int j = 0; j < n; j++)
				if (keep == NULL || ao_batch_bit(keep, j))
					AoTidSet(&out->tids[k++], st->segno,
							 st->first_rownum + r0 + j);
		}
		for (int i = 0; i < st->ncols; i++)
		{
			AoBatchCol *c = &st->cols[i];
			VexecColumn *col = &out->columns[i];

			if (c->kind == AO_BATCH_TID)
			{
				memset(col, 0, sizeof(VexecColumn));
				ao_batch_shape(c, col);
				col->values = out->tids;
			}
			else if (c->dup_of >= 0)
				*col = out->columns[c->dup_of];
		}

		batch->nrows = nout;
		batch->visible = ndeleted > 0 && keep == NULL ? out->visible : NULL;
		batch->tids = st->tids ? out->tids : NULL;
		batch->columns = out->columns;
		batch->owner = out;

		/* as getnextslot counts each row it returns */
		if (pgstat_should_count_relation(rel))
			rel->pgstat_info->counts.tuples_returned += n - ndeleted;
		return true;
	}
}

static void
ao_batch_retain(void *arg, void *owner)
{
	AoBatchState *st = (AoBatchState *) arg;
	AoBatchOut *out = (AoBatchOut *) owner;

	if (out->retained)
		return;
	out->retained = true;
	for (int k = 0; k < out->nbufs; k++)
	{
		AoBatchBuf *b = out->bufs[k];

		if (b->keep == NULL)
		{
			b->keep = MemoryContextAllocZero(st->cxt, sizeof(AoBatchKeep));
			b->keep->raw = b->rd->raw;
			b->keep->reading = true;
		}
		b->keep->refs++;
		out->keeps[k] = b->keep;
	}
}

static void
ao_batch_release(void *arg, void *owner)
{
	AoBatchState *st = (AoBatchState *) arg;
	AoBatchOut *out = (AoBatchOut *) owner;

	if (!out->retained)
		return;
	for (int k = 0; k < out->nbufs; k++)
	{
		AoBatchKeep *keep = out->keeps[k];

		if (--keep->refs == 0 && !keep->reading)
		{
			pfree(keep->raw);
			pfree(keep);
		}
	}
	out->retained = false;
	if (out != st->out)
		MemoryContextDelete(out->cxt);
}

/* After table_rescan(), which began gp_ao's scan again (ao_scan_rescan()). */
static void
ao_batch_rescan(void *arg)
{
	AoBatchState *st = (AoBatchState *) arg;

	st->segno = 0;
	st->nrows = 0;
	st->pos = 0;
	st->loaded = false;
	for (int i = 0; i < st->ncols; i++)
		ao_reader_reset(&st->cols[i].rd);
}

/* Before table_endscan(), which frees the scan's memory and so this. */
static void
ao_batch_end(void *arg)
{
	AoBatchState *st = (AoBatchState *) arg;

	MemoryContextDelete(st->cxt);
}

/* ------------------------------------------------------------------------- */
/* The sink, for ao_column                                                   */
/* ------------------------------------------------------------------------- */

/*
 * vexec's VecInsert writes an INSERT's batches into a table by column
 * through the sink (vexec_sink.h) where it would form rows for
 * table_multi_insert(): each column's values go into its block as
 * ao_column_append() lays a row's out (ao_column_append_batch()), a run of
 * rows at a time, through the statement's writer of the table, which
 * ao_multi_insert() appends to (ao_insert_batch(), ao_dml.c).  The layouts,
 * as a column block holds the values:
 *
 *	by-value, and		FIXED, the type's width, at its stride in an array;
 *	fixed-length		bool BYTE_BOOL, a byte a row
 *	by-reference
 *	text, varchar,		OFFSETS: the bytes, to which the block's header,
 *	bpchar, bytea		short where heap_fill_tuple() makes it short, is
 *						added as they are appended
 *	any other varlena	DATUM, headered, as a row's
 *	cstring				none: VecInsert forms rows
 *
 * A table by row takes rows: it has no sink.
 */
static bool
ao_sink_supports(Relation rel, AttrNumber attnum, VexecSinkLayout *layout)
{
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);

	memset(layout, 0, sizeof(VexecSinkLayout));
	if (att->attbyval)
	{
		layout->layout = att->atttypid == BOOLOID ? VEXEC_BYTE_BOOL : VEXEC_FIXED;
		layout->width = att->attlen;
		layout->stride = att->attlen;
		return true;
	}
	if (att->attlen > 0)
	{
		layout->layout = VEXEC_FIXED;
		layout->width = att->attlen;
		layout->stride = att_align_nominal(att->attlen, att->attalign);
		return true;
	}
	if (att->attlen != -1)
		return false;
	if (att->atttypid == TEXTOID || att->atttypid == VARCHAROID ||
		att->atttypid == BPCHAROID || att->atttypid == BYTEAOID)
		layout->layout = VEXEC_OFFSETS;
	else
		layout->layout = VEXEC_DATUM;
	return true;
}

/* The statement's writer of the table is ao_insert_state()'s: no state of its own. */
static void *
ao_sink_begin(Relation rel, const VexecSinkSpec *spec)
{
	(void) spec;
	return rel;
}

static void
ao_sink_put(void *state, VexecSinkBatch *batch)
{
	Relation	rel = (Relation) state;

	CHECK_FOR_INTERRUPTS();
	ao_insert_batch(rel, batch->columns, batch->nrows, batch->tids);
	pgstat_count_heap_insert(rel, batch->nrows);
}

/* The statement's blocks written, as finish_bulk_insert writes them. */
static void
ao_sink_end(void *state)
{
	ao_dml_flush(RelationGetRelid((Relation) state));
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

static VexecSourceRoutine ao_row_source;
static VexecSourceRoutine ao_column_source;
static VexecSinkRoutine ao_column_sink;

static void
ao_batch_routine(VexecSourceRoutine *r, bool columnar)
{
	memset(r, 0, sizeof(VexecSourceRoutine));
	r->size = sizeof(VexecSourceRoutine);
	r->minor = VEXEC_SOURCE_MINOR;
	r->am = ao_table_am_routine(columnar);
	r->name = "gp_ao";
	r->supports = ao_batch_supports;
	r->begin = ao_batch_begin;
	r->next = ao_batch_next;
	r->retain = ao_batch_retain;
	r->release = ao_batch_release;
	r->rescan = ao_batch_rescan;
	r->end = ao_batch_end;
	r->estimate = NULL;
}

/*
 * vexec's batch sources for ao_row and ao_column, and its sink for
 * ao_column, keyed by their routines, from _PG_init while the postmaster
 * preloads libraries.  Without vexec they are unused entries of the
 * registries (vexec_source.h, vexec_sink.h).
 */
void
ao_batch_register(void)
{
	ao_batch_routine(&ao_row_source, false);
	ao_batch_routine(&ao_column_source, true);
	vexec_register_source(&ao_row_source);
	vexec_register_source(&ao_column_source);

	memset(&ao_column_sink, 0, sizeof(VexecSinkRoutine));
	ao_column_sink.size = sizeof(VexecSinkRoutine);
	ao_column_sink.minor = VEXEC_SINK_MINOR;
	ao_column_sink.am = ao_table_am_routine(true);
	ao_column_sink.name = "gp_ao";
	ao_column_sink.supports = ao_sink_supports;
	ao_column_sink.begin = ao_sink_begin;
	ao_column_sink.put = ao_sink_put;
	ao_column_sink.end = ao_sink_end;
	vexec_register_sink(&ao_column_sink);
}
