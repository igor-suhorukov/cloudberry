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
 * bitmapinspect.c
 *	  What a bitmap index's pages hold, for a look at them: Cloudberry's
 *	  pageinspect functions of the bitmap index.
 *
 * Portions Copyright (c) 2021-Present VMware, Inc. or its affiliates.
 *
 * Cloudberry's pageinspect has five functions of the bitmap index beside
 * PostgreSQL's of the other index kinds: bm_metap(), its meta page;
 * bm_lov_page_items(), the items of a page of its list of values;
 * bm_bitmap_page_header() and bm_bitmap_page_items(), the header and the
 * words of a page of a bitmap vector, the latter of a page read by name and
 * block or given as get_raw_page() returns it.  PostgreSQL 19's pageinspect
 * knows no bitmap index, which is gp_ao's here, so they are gp_ao's too, in
 * pg_catalog (gp_ao--1.0.sql), and read the pages as Cloudberry's do -- of a
 * node's own index, in a session of the node's own (bitmap_index_inspect).
 *
 * Made of Cloudberry's contrib/pageinspect/bmfuncs.c.  What changed: a
 * bitmap index is told by its access method's routine (RelationIsBitmapIndex()),
 * where Cloudberry's has a fixed OID; and a word is printed most significant
 * byte first by shifting, which is what Cloudberry's byte order test did.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "catalog/namespace.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/varlena.h"
#include "varatt.h"

#include "bitmap.h"
#include "bitmap_private.h"

PG_FUNCTION_INFO_V1(bm_metap);
PG_FUNCTION_INFO_V1(bm_lov_page_items);
PG_FUNCTION_INFO_V1(bm_bitmap_page_header);
PG_FUNCTION_INFO_V1(bm_bitmap_page_items);
PG_FUNCTION_INFO_V1(bm_bitmap_page_items_bytea);

/* note: BlockNumber is unsigned, hence can't be negative */
#define CHECK_RELATION_BLOCK_RANGE(rel, blkno) { \
		if ( RelationGetNumberOfBlocks(rel) <= (BlockNumber) (blkno) ) \
			 elog(ERROR, "block number out of range"); }

/* Only a superuser looks at pages, as Cloudberry's pageinspect has it. */
static void
bm_check_superuser(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pageinspect functions"))));
}

/*
 * The bitmap index of that name, open under AccessShareLock: one of the
 * session's own, never another session's temporary one, whose pages are in
 * that session's buffers.
 */
static Relation
bm_open_index(text *relname)
{
	RangeVar   *relrv;
	Relation	rel;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!RelationIsBitmapIndex(rel))
		elog(ERROR, "relation \"%s\" is not a bitmap index",
			 RelationGetRelationName(rel));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	return rel;
}

/* ------------------------------------------------
 * bm_metap()
 *
 * Get a bitmap index's meta-page information
 *
 * Usage: SELECT * FROM bm_metap('gender')
 * ------------------------------------------------
 */
Datum
bm_metap(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Datum		result;
	Relation	rel;
	BMMetaPageData *metad;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[5];
	Buffer		buffer;
	HeapTuple	tuple;

	bm_check_superuser();

	rel = bm_open_index(relname);

	buffer = ReadBuffer(rel, BM_METAPAGE);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	metad = _bitmap_get_metapage_data(rel, buffer);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	j = 0;
	values[j++] = psprintf("%d", metad->bm_magic);
	values[j++] = psprintf("%d", metad->bm_version);
	values[j++] = psprintf("%u", metad->bm_lov_heapId);
	values[j++] = psprintf("%u", metad->bm_lov_indexId);
	values[j] = psprintf("%u", metad->bm_lov_lastpage);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}

/*-------------------------------------------------------
 * bm_get_word_text()
 *
 * Get a user-friendly print version of a bitmap word (uint64): its bytes,
 * most significant first, e.g. 31 becomes '00 00 00 00 00 00 00 1f'.
 * ------------------------------------------------------
 */
static char *
bm_get_word_text(BM_HRL_WORD word)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int shift = 56; shift >= 0; shift -= 8)
		appendStringInfo(&buf, "%s%02x", shift < 56 ? " " : "",
						 (unsigned int) ((word >> shift) & 0xff));
	return buf.data;
}

/*-------------------------------------------------------
 * bm_print_lov_item()
 *
 * Form a tuple describing an LOV item at a given offset
 * ------------------------------------------------------
 */
static Datum
bm_print_lov_item(FuncCallContext *fctx, Page page, OffsetNumber offset)
{
	char	   *values[9];
	HeapTuple	tuple;
	ItemId		id;
	BMLOVItem	lovItem;
	int			j;

	id = PageGetItemId(page, offset);

	if (!ItemIdIsValid(id))
		elog(ERROR, "invalid ItemId");

	lovItem = (BMLOVItem) PageGetItem(page, id);

	j = 0;
	values[j++] = psprintf("%d", offset);
	values[j++] = psprintf("%u", lovItem->bm_lov_head);
	values[j++] = psprintf("%u", lovItem->bm_lov_tail);
	values[j++] = bm_get_word_text(lovItem->bm_last_compword);
	values[j++] = bm_get_word_text(lovItem->bm_last_word);
	values[j++] = psprintf(UINT64_FORMAT, lovItem->bm_last_tid_location);
	values[j++] = psprintf(UINT64_FORMAT, lovItem->bm_last_setbit);
	values[j++] = psprintf("%d", (lovItem->lov_words_header & BM_LAST_COMPWORD_BIT) != 0);
	values[j] = psprintf("%d", (lovItem->lov_words_header & BM_LAST_WORD_BIT) != 0);

	tuple = BuildTupleFromCStrings(fctx->attinmeta, values);

	return HeapTupleGetDatum(tuple);
}

/*
 * cross-call data structure for bm_lov_page_items() SRF
 */
struct user_args_lov_items
{
	Page		page;
	OffsetNumber offset;
};

/*-------------------------------------------------------
 * bm_lov_page_items()
 *
 * Get LOV items present in a bitmap LOV page
 *
 * Usage: SELECT * FROM bm_lov_page_items('t1_pkey', 1);
 *-------------------------------------------------------
 */
Datum
bm_lov_page_items(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	Datum		result;
	FuncCallContext *fctx;
	MemoryContext mctx;
	struct user_args_lov_items *uargs;

	bm_check_superuser();

	if (SRF_IS_FIRSTCALL())
	{
		Relation	rel;
		Buffer		buffer;
		TupleDesc	tupleDesc;

		fctx = SRF_FIRSTCALL_INIT();

		rel = bm_open_index(relname);

		if (blkno == 0)
			elog(ERROR, "block 0 is a meta page");

		CHECK_RELATION_BLOCK_RANGE(rel, blkno);

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/*
		 * We copy the page into local storage to avoid holding pin on the
		 * buffer longer than we must, and possibly failing to release it at
		 * all if the calling query doesn't fetch all rows.
		 */
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(struct user_args_lov_items));

		uargs->page = palloc(BLCKSZ);
		memcpy(uargs->page, BufferGetPage(buffer), BLCKSZ);

		UnlockReleaseBuffer(buffer);
		relation_close(rel, AccessShareLock);

		/*
		 * Ensure that we are dealing with a LOV item page - they don't have a
		 * special section.
		 */
		if (PageGetSpecialSize(uargs->page))
			elog(ERROR, "block %u is not an LOV page, it is a bitmap page", blkno);

		uargs->offset = FirstOffsetNumber;

		fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		fctx->attinmeta = TupleDescGetAttInMetadata(tupleDesc);

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bm_print_lov_item(fctx, uargs->page, uargs->offset);
		uargs->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}
	else
	{
		pfree(uargs->page);
		pfree(uargs);
		SRF_RETURN_DONE(fctx);
	}
}

/*-------------------------------------------------------
 * bm_bitmap_page_header()
 *
 * Get the header information for a bitmap page. This
 * corresponds to the opaque section from the page
 * header.
 *
 * Usage: SELECT * FROM bm_bitmap_page_header('bm_index', 5);
 *-------------------------------------------------------
 */
Datum
bm_bitmap_page_header(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	Datum		result;
	Relation	rel;
	Buffer		buffer;
	Page		page;
	BMBitmapOpaque bm_opaque;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[3];
	HeapTuple	tuple;

	bm_check_superuser();

	rel = bm_open_index(relname);

	CHECK_RELATION_BLOCK_RANGE(rel, blkno);

	if (blkno == 0)
		elog(ERROR, "block 0 is a meta page");

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);

	/*
	 * Ensure that we are dealing with a bitmap page - they must have a
	 * special section.
	 */
	if (PageGetSpecialSize(page) <= 0)
		elog(ERROR, "block %u is not a bitmap page, it is a LOV item page", blkno);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	bm_opaque = (BMBitmapOpaque) PageGetSpecialPointer(page);

	j = 0;
	values[j++] = psprintf("%u", bm_opaque->bm_hrl_words_used);
	values[j++] = psprintf("%u", bm_opaque->bm_bitmap_next);
	values[j] = psprintf(UINT64_FORMAT, bm_opaque->bm_last_tid_location);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}

/*-------------------------------------------------------
 * bm_print_content_word()
 *
 * Print content word in given bitmap page along with its
 * compression status.
 * ------------------------------------------------------
 */
static Datum
bm_print_content_word(FuncCallContext *fctx, Page page, int word_num)
{
	char	   *values[3];
	HeapTuple	tuple;
	int			j;
	BMBitmap	bitmap = (BMBitmap) PageGetContentsMaxAligned(page);

	j = 0;
	values[j++] = psprintf("%d", word_num);
	values[j++] = psprintf("%d", IS_FILL_WORD(bitmap->hwords, word_num));
	values[j] = bm_get_word_text(bitmap->cwords[word_num]);

	tuple = BuildTupleFromCStrings(fctx->attinmeta, values);

	return HeapTupleGetDatum(tuple);
}

/*
 * cross-call data structure for bm_bitmap_page_items() SRF
 */
struct user_args_page_items
{
	Page		page;
	uint32		word_num;
};

/*
 * The first call's work of either bm_bitmap_page_items(): the words of the
 * bitmap page, a copy in the call's memory, to be returned one by one.
 */
static void
bm_bitmap_page_items_setup(FunctionCallInfo fcinfo, FuncCallContext *fctx,
						   Page page, const char *notbitmap)
{
	MemoryContext mctx;
	struct user_args_page_items *uargs;
	BMBitmapOpaque bm_opaque;
	TupleDesc	tupleDesc;

	mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

	uargs = palloc(sizeof(struct user_args_page_items));
	uargs->page = palloc(BLCKSZ);
	memcpy(uargs->page, page, BLCKSZ);

	/*
	 * Ensure that we are dealing with a bitmap page - they must have a special
	 * section.
	 */
	if (PageGetSpecialSize(uargs->page) <= 0)
		elog(ERROR, "%s", notbitmap);

	uargs->word_num = 0;
	bm_opaque = (BMBitmapOpaque) PageGetSpecialPointer(uargs->page);
	fctx->max_calls = bm_opaque->bm_hrl_words_used;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	fctx->attinmeta = TupleDescGetAttInMetadata(tupleDesc);
	fctx->user_fctx = uargs;

	MemoryContextSwitchTo(mctx);
}

/* Each call after the first of either bm_bitmap_page_items(). */
static Datum
bm_bitmap_page_items_next(FunctionCallInfo fcinfo)
{
	FuncCallContext *fctx;
	struct user_args_page_items *uargs;
	Datum		result;

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bm_print_content_word(fctx, uargs->page, uargs->word_num);
		uargs->word_num++;
		SRF_RETURN_NEXT(fctx, result);
	}
	else
	{
		pfree(uargs->page);
		pfree(uargs);
		SRF_RETURN_DONE(fctx);
	}
}

/*-------------------------------------------------------
 * bm_bitmap_page_items()
 *
 * Get the content words from a bitmap page along with
 * their compression statuses.
 *
 * Usage: SELECT * FROM bm_bitmap_page_items('t1_pkey', 5);
 *-------------------------------------------------------
 */
Datum
bm_bitmap_page_items(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);

	bm_check_superuser();

	if (SRF_IS_FIRSTCALL())
	{
		FuncCallContext *fctx = SRF_FIRSTCALL_INIT();
		Relation	rel;
		Buffer		buffer;
		Page		page;

		rel = bm_open_index(relname);

		if (blkno == 0)
			elog(ERROR, "block 0 is a meta page");

		CHECK_RELATION_BLOCK_RANGE(rel, blkno);

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/*
		 * We copy the page into local storage to avoid holding pin on the
		 * buffer longer than we must, and possibly failing to release it at
		 * all if the calling query doesn't fetch all rows.
		 */
		page = palloc(BLCKSZ);
		memcpy(page, BufferGetPage(buffer), BLCKSZ);

		UnlockReleaseBuffer(buffer);
		relation_close(rel, AccessShareLock);

		bm_bitmap_page_items_setup(fcinfo, fctx, page,
								   psprintf("block %u is not a bitmap page, it is a LOV item page",
											blkno));
		pfree(page);
	}

	return bm_bitmap_page_items_next(fcinfo);
}

/*-------------------------------------------------------
 * bm_bitmap_page_items_bytea()
 *
 * Get the content words from a bitmap page along with
 * their compression statuses.
 *
 * Usage: SELECT * FROM bm_bitmap_page_items(get_raw_page('t1_pkey', 5));
 *-------------------------------------------------------
 */
Datum
bm_bitmap_page_items_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);

	bm_check_superuser();

	if (SRF_IS_FIRSTCALL())
	{
		FuncCallContext *fctx;
		int			raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

		if (raw_page_size < SizeOfPageHeaderData)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page too small (%d bytes)", raw_page_size)));

		/*
		 * A whole page, which the setup copies: PostgreSQL 19's pageinspect
		 * refuses any other size in these words (get_page_from_raw()), where
		 * Cloudberry's read past a short one.
		 */
		if (raw_page_size != BLCKSZ)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid page size"),
					 errdetail("Expected %d bytes, got %d.",
							   BLCKSZ, raw_page_size)));

		fctx = SRF_FIRSTCALL_INIT();
		bm_bitmap_page_items_setup(fcinfo, fctx, (Page) VARDATA(raw_page),
								   "page is not a bitmap page");
	}

	return bm_bitmap_page_items_next(fcinfo);
}
