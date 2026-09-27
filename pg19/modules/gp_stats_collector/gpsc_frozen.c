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
 * gpsc_frozen.c
 *	  A row of gp_stats_collector's "tbl" log, written frozen and without a
 *	  transaction id.
 *
 * Cloudberry writes a query's event into gpsc.__log with its heapam.c's
 * frozen_heap_insert(), heap_insert() given FrozenTransactionId for the
 * row's xmin: the row is every transaction's at once and is not rolled back
 * with the one that wrote it, so the event of a query that fails is kept;
 * and writing it gives that transaction no id, so a query that only reads
 * -- on the coordinator, or a slice on a segment, a reader's among them --
 * still writes nothing, and asks for no two-phase commit.  PostgreSQL 19's
 * heap_insert() gives its row the transaction's id, which it assigns.  So
 * the row is placed here as heap_insert() places one -- into a buffer with
 * room for it, the page's all-visible bit cleared -- and logged as it logs
 * one, with XLOG_HEAP_INSERT, whose replay makes the row frozen, as its
 * infomask says, the record having no transaction id; and without the row
 * for logical decoding, which has no transaction to decode it in.
 *
 * A row is never toasted, since a value's TOAST rows would be inserted in
 * the transaction: its texts are compressed in line, and a row still larger
 * than a page, which Cloudberry's heap_insert() would have toasted, has its
 * longest text cut in half, at a character's boundary, until it fits.
 *
 * Cloudberry sources this file stands in for:
 *	  frozen_heap_insert() and heap_insert() in
 *	  src/backend/access/heap/heapam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "access/toast_internals.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "varatt.h"

#include "gpsc_frozen.h"

/* A text worth compressing: shorter ones stay as they are. */
#define GPSC_COMPRESS_MIN	128

/* A value of the row in line: compressed where that makes it smaller. */
static Datum
in_line(Datum value)
{
	Datum		compressed;

	if (VARSIZE_ANY(DatumGetPointer(value)) < GPSC_COMPRESS_MIN)
		return value;
	compressed = toast_compress_datum(value, default_toast_compression);
	return compressed != (Datum) 0 ? compressed : value;
}

/*
 * The row, its texts in line, and cut where it would not fit a page: the
 * longest text of it halved, at a character's boundary, until it does.
 */
static HeapTuple
form_in_line(TupleDesc desc, Datum *values, bool *nulls)
{
	Datum	   *stored = palloc_array(Datum, desc->natts);
	HeapTuple	tuple;

	for (;;)
	{
		int			longest = -1;
		Size		longest_len = 0;

		for (int i = 0; i < desc->natts; i++)
		{
			stored[i] = values[i];
			if (!nulls[i] && TupleDescAttr(desc, i)->attlen == -1)
				stored[i] = in_line(values[i]);
		}
		tuple = heap_form_tuple(desc, stored, nulls);
		if (tuple->t_len <= MaxHeapTupleSize)
			break;
		heap_freetuple(tuple);

		for (int i = 0; i < desc->natts; i++)
		{
			if (!nulls[i] && TupleDescAttr(desc, i)->atttypid == TEXTOID &&
				VARSIZE_ANY_EXHDR(DatumGetPointer(values[i])) > longest_len)
			{
				longest = i;
				longest_len = VARSIZE_ANY_EXHDR(DatumGetPointer(values[i]));
			}
		}
		if (longest < 0)
			elog(ERROR, "gp_stats_collector: a log row does not fit a page");
		values[longest] = PointerGetDatum(
			cstring_to_text_with_len(VARDATA_ANY(DatumGetPointer(values[longest])),
									 pg_mbcliplen(VARDATA_ANY(DatumGetPointer(values[longest])),
												  longest_len, longest_len / 2)));
	}

	pfree(stored);
	return tuple;
}

void
gpsc_frozen_insert(Relation rel, Datum *values, bool *nulls)
{
	HeapTuple	tuple;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	Page		page;
	bool		clear_all_visible = false;
	bool		vmbuffer_modified = false;

	tuple = form_in_line(RelationGetDescr(rel), values, nulls);

	/* heap_prepare_insert()'s header, xmin frozen */
	tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	tuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tuple->t_data, FrozenTransactionId);
	HeapTupleHeaderSetXminFrozen(tuple->t_data);
	HeapTupleHeaderSetCmin(tuple->t_data, FirstCommandId);
	HeapTupleHeaderSetXmax(tuple->t_data, 0);
	tuple->t_tableOid = RelationGetRelid(rel);

	buffer = RelationGetBufferForTuple(rel, tuple->t_len, InvalidBuffer, 0,
									   NULL, &vmbuffer, NULL, 0);
	page = BufferGetPage(buffer);
	if (PageIsAllVisible(page))
	{
		LockBuffer(vmbuffer, BUFFER_LOCK_EXCLUSIVE);
		clear_all_visible = true;
	}

	START_CRIT_SECTION();

	RelationPutHeapTuple(rel, buffer, tuple, false);
	if (clear_all_visible)
	{
		if (visibilitymap_clear(rel, ItemPointerGetBlockNumber(&tuple->t_self),
								vmbuffer, VISIBILITYMAP_VALID_BITS))
			vmbuffer_modified = true;
		PageClearAllVisible(page);
	}
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(rel))
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		uint8		info = XLOG_HEAP_INSERT;
		int			bufflags = 0;

		if (ItemPointerGetOffsetNumber(&tuple->t_self) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}
		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.flags = clear_all_visible ? XLH_INSERT_ALL_VISIBLE_CLEARED : 0;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapInsert);
		xlhdr.t_infomask2 = tuple->t_data->t_infomask2;
		xlhdr.t_infomask = tuple->t_data->t_infomask;
		xlhdr.t_hoff = tuple->t_data->t_hoff;
		XLogRegisterBuffer(HEAP_INSERT_BLKREF_HEAP, buffer,
						   REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(HEAP_INSERT_BLKREF_HEAP, &xlhdr, SizeOfHeapHeader);
		XLogRegisterBufData(HEAP_INSERT_BLKREF_HEAP,
							(char *) tuple->t_data + SizeofHeapTupleHeader,
							tuple->t_len - SizeofHeapTupleHeader);
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);
		if (vmbuffer_modified)
			XLogRegisterBuffer(HEAP_INSERT_BLKREF_VM, vmbuffer, 0);

		recptr = XLogInsert(RM_HEAP_ID, info);

		PageSetLSN(page, recptr);
		if (vmbuffer_modified)
			PageSetLSN(BufferGetPage(vmbuffer), recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	if (clear_all_visible)
		LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	heap_freetuple(tuple);
}
