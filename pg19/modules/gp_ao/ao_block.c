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
 * ao_block.c
 *	  A block of rows of an append-optimized table: a header, and the rows,
 *	  compressed as the table's options say.
 *
 * Cloudberry's blocks have several formats -- a single row, a varblock of
 * rows, a large content split over several, bulk dense content -- because a
 * block is written to a file of its own and read back past the buffer
 * manager.  The port's are in pages, which hold any length, so a block here
 * is one thing: a header, then stored_len bytes, which are raw_len bytes of
 * rows compressed, or the rows themselves where compressing would not have
 * made them shorter.  The header's CRC-32C covers it and what follows, as
 * Cloudberry's checksum option asks.
 *
 * rle_type, Cloudberry's run-length encoding of a column's values, is
 * compressed with zlib here, at its level: the values of a column block are
 * laid out one after another (below), so runs are runs of bytes, which
 * zlib's deflate encodes.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <zlib.h>
#ifdef GP_AO_HAVE_ZSTD
#include <zstd.h>
#endif

#include "access/detoast.h"
#include "access/tupmacs.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "port/pg_crc32c.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "gp_ao.h"

static uint32
block_checksum(const AoBlockHeader *hdr, const char *stored)
{
	pg_crc32c	crc;
	AoBlockHeader copy = *hdr;

	copy.checksum = 0;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &copy, sizeof(AoBlockHeader));
	COMP_CRC32C(crc, stored, hdr->stored_len);
	FIN_CRC32C(crc);
	return (uint32) crc;
}

#define AO_BLOCK_CHECKSUMMED	0x0100

/*
 * Compress raw into out, after a header; return how many bytes the block is.
 * out is reset first.
 */
Size
ao_block_encode(StringInfo raw, const AoOptions *opts, int64 first_rownum,
				uint32 nrows, StringInfo out)
{
	AoBlockHeader hdr = {0};
	AoCompressType method = opts->compresstype;
	char	   *stored;
	Size		stored_len = 0;
	bool		compressed = false;

	resetStringInfo(out);
	enlargeStringInfo(out, AO_BLOCK_HEADER_SIZE + raw->len + 64);
	out->len = AO_BLOCK_HEADER_SIZE;
	stored = out->data + AO_BLOCK_HEADER_SIZE;

	switch (method)
	{
		case AO_COMPRESS_NONE:
			break;

		case AO_COMPRESS_ZLIB:
		case AO_COMPRESS_RLE:
			{
				uLongf		destlen = compressBound(raw->len);
				int			level = opts->compresslevel > 0 ?
					Min(opts->compresslevel, 9) : Z_DEFAULT_COMPRESSION;

				enlargeStringInfo(out, destlen);
				stored = out->data + AO_BLOCK_HEADER_SIZE;
				if (compress2((Bytef *) stored, &destlen,
							  (const Bytef *) raw->data, raw->len, level) != Z_OK)
					elog(ERROR, "zlib could not compress a block of %d bytes",
						 raw->len);
				stored_len = destlen;
				compressed = true;
				break;
			}

		case AO_COMPRESS_ZSTD:
#ifdef GP_AO_HAVE_ZSTD
			{
				size_t		bound = ZSTD_compressBound(raw->len);
				size_t		r;

				enlargeStringInfo(out, bound);
				stored = out->data + AO_BLOCK_HEADER_SIZE;
				r = ZSTD_compress(stored, bound, raw->data, raw->len,
								  opts->compresslevel > 0 ? opts->compresslevel : 1);
				if (ZSTD_isError(r))
					elog(ERROR, "zstd could not compress a block of %d bytes: %s",
						 raw->len, ZSTD_getErrorName(r));
				stored_len = r;
				compressed = true;
				break;
			}
#else
			elog(ERROR, "gp_ao was built without zstd");
#endif
	}

	/* Stored as it is where compressing it would not have made it shorter. */
	if (!compressed || stored_len >= (Size) raw->len)
	{
		method = AO_COMPRESS_NONE;
		stored_len = raw->len;
		memcpy(stored, raw->data, raw->len);
	}

	hdr.magic = AO_BLOCK_MAGIC;
	hdr.flags = (uint32) method | (opts->checksum ? AO_BLOCK_CHECKSUMMED : 0);
	hdr.stored_len = stored_len;
	hdr.raw_len = raw->len;
	hdr.first_rownum = first_rownum;
	hdr.nrows = nrows;
	if (opts->checksum)
		hdr.checksum = block_checksum(&hdr, stored);
	memcpy(out->data, &hdr, sizeof(AoBlockHeader));
	memset(out->data + sizeof(AoBlockHeader), 0,
		   AO_BLOCK_HEADER_SIZE - sizeof(AoBlockHeader));
	out->len = AO_BLOCK_HEADER_SIZE + stored_len;
	return out->len;
}

/* Is the header one, of a block the file's end reaches? */
void
ao_block_check_header(Relation rel, uint32 filenum, uint64 offset,
					  const AoBlockHeader *hdr)
{
	if (hdr->magic != AO_BLOCK_MAGIC ||
		(hdr->flags & 0xFF) > AO_COMPRESS_RLE ||
		hdr->raw_len > MaxAllocSize || hdr->stored_len > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("append-optimized table \"%s\" has no valid block at offset " UINT64_FORMAT " of file %u",
						RelationGetRelationName(rel), offset, filenum)));
}

/* The rows of a block, into raw, which has room for hdr->raw_len bytes. */
void
ao_block_decode(const char *stored, const AoBlockHeader *hdr, char *raw)
{
	if ((hdr->flags & AO_BLOCK_CHECKSUMMED) &&
		block_checksum(hdr, stored) != hdr->checksum)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block of rows " INT64_FORMAT " to " INT64_FORMAT " of an append-optimized table fails its checksum",
						hdr->first_rownum,
						hdr->first_rownum + hdr->nrows - 1)));

	switch ((AoCompressType) (hdr->flags & 0xFF))
	{
		case AO_COMPRESS_NONE:
			memcpy(raw, stored, hdr->raw_len);
			break;

		case AO_COMPRESS_ZLIB:
		case AO_COMPRESS_RLE:
			{
				uLongf		destlen = hdr->raw_len;

				int			zresult;

				zresult = uncompress((Bytef *) raw, &destlen,
									 (const Bytef *) stored, hdr->stored_len);

				/*
				 * Cloudberry's, once zlib has decompressed (zlib_decompress(),
				 * pg_compression.c), whatever it answered: a test holds a
				 * scan or an ANALYZE's sample here, and cancels it.
				 */
				if ((AoCompressType) (hdr->flags & 0xFF) == AO_COMPRESS_ZLIB)
					(void) GP_FAULT("zlib_decompress_after_decompress_fn");

				if (zresult != Z_OK || destlen != hdr->raw_len)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("zlib could not decompress a block of an append-optimized table")));
				break;
			}

		case AO_COMPRESS_ZSTD:
#ifdef GP_AO_HAVE_ZSTD
			{
				size_t		r = ZSTD_decompress(raw, hdr->raw_len, stored,
												hdr->stored_len);

				if (ZSTD_isError(r) || r != hdr->raw_len)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("zstd could not decompress a block of an append-optimized table")));
				break;
			}
#else
			elog(ERROR, "gp_ao was built without zstd");
#endif
	}
}

/* ------------------------------------------------------------------------- */
/* A column's values                                                         */
/* ------------------------------------------------------------------------- */

#define AO_COLUMN_HASNULLS	0x0001

typedef struct AoColumnHeader
{
	uint32		flags;
	uint32		nrows;
} AoColumnHeader;

void
ao_column_builder_init(AoColumnBuilder *cb)
{
	cb->nrows = 0;
	cb->hasnulls = false;
	initStringInfo(&cb->nulls);
	initStringInfo(&cb->values);
}

void
ao_column_builder_reset(AoColumnBuilder *cb)
{
	cb->nrows = 0;
	cb->hasnulls = false;
	resetStringInfo(&cb->nulls);
	resetStringInfo(&cb->values);
}

/*
 * A value stored in an append-optimized table cannot point into another
 * table's TOAST, nor be expanded in memory: it is flattened here.  A value
 * compressed inline stays as it is.
 */
Datum
ao_detoast_value(Form_pg_attribute att, Datum value)
{
	if (att->attlen == -1 && VARATT_IS_EXTENDED(DatumGetPointer(value)) &&
		VARATT_IS_EXTERNAL(DatumGetPointer(value)))
		return PointerGetDatum(detoast_external_attr((varlena *) DatumGetPointer(value)));
	return value;
}

static void
pad_to(StringInfo buf, Size offset)
{
	while ((Size) buf->len < offset)
		appendStringInfoChar(buf, '\0');
}

/* One more row's value of a column, as heap_fill_tuple() would store it. */
void
ao_column_append(AoColumnBuilder *cb, Form_pg_attribute att, Datum value,
				 bool isnull)
{
	int			bit = cb->nrows;

	if (bit % 8 == 0)
		appendStringInfoChar(&cb->nulls, 0);
	cb->nrows++;
	if (isnull)
	{
		cb->nulls.data[bit / 8] |= (char) (1 << (bit % 8));
		cb->hasnulls = true;
		return;
	}

	if (att->attbyval)
	{
		pad_to(&cb->values, att_align_nominal(cb->values.len, att->attalign));
		enlargeStringInfo(&cb->values, att->attlen);
		store_att_byval(cb->values.data + cb->values.len, value, att->attlen);
		cb->values.len += att->attlen;
	}
	else if (att->attlen == -1)
	{
		varlena    *v = (varlena *) DatumGetPointer(value);

		Assert(!VARATT_IS_EXTERNAL(v));
		if (VARATT_IS_SHORT(v))
			appendBinaryStringInfo(&cb->values, (char *) v, VARSIZE_SHORT(v));

		/*
		 * A short header only where heap_fill_tuple() gives one: a value of
		 * a type stored plain is read as it was written, with its 4-byte
		 * header, by code that never looks for another (tsquery's).
		 */
		else if (att->attstorage != TYPSTORAGE_PLAIN && VARATT_CAN_MAKE_SHORT(v))
		{
			Size		len = VARATT_CONVERTED_SHORT_SIZE(v);
			char		hdr;

			SET_VARSIZE_1B(&hdr, len);
			appendStringInfoChar(&cb->values, hdr);
			appendBinaryStringInfo(&cb->values, VARDATA(v), len - 1);
		}
		else
		{
			pad_to(&cb->values, att_align_nominal(cb->values.len, att->attalign));
			appendBinaryStringInfo(&cb->values, (char *) v, VARSIZE_ANY(v));
		}
	}
	else if (att->attlen == -2)
	{
		const char *str = DatumGetCString(value);

		appendBinaryStringInfo(&cb->values, str, strlen(str) + 1);
	}
	else
	{
		pad_to(&cb->values, att_align_nominal(cb->values.len, att->attalign));
		appendBinaryStringInfo(&cb->values, DatumGetPointer(value), att->attlen);
	}
}

/* The column's block, as ao_column_decode() reads it, into raw. */
void
ao_column_finish(AoColumnBuilder *cb, StringInfo raw)
{
	AoColumnHeader hdr;
	Size		start;

	hdr.flags = cb->hasnulls ? AO_COLUMN_HASNULLS : 0;
	hdr.nrows = cb->nrows;
	resetStringInfo(raw);
	appendBinaryStringInfo(raw, &hdr, sizeof(hdr));
	if (cb->hasnulls)
		appendBinaryStringInfo(raw, cb->nulls.data, cb->nulls.len);
	start = MAXALIGN(raw->len);
	pad_to(raw, start);
	appendBinaryStringInfo(raw, cb->values.data, cb->values.len);
}

/*
 * Where the parts of a column block of nrows rows are: its bitmap of NULLs,
 * a bit a row, 1 where NULL, or NULL where it has none; and its values, laid
 * out from a MAXALIGNed start, a NULL keeping no place.  ao_column_decode()
 * takes the values one by one; vexec's batches take them as they lie
 * (ao_batch.c).
 */
void
ao_column_block_parts(const char *raw, Size rawlen, int nrows,
					  const uint8 **nulls, const char **values)
{
	AoColumnHeader hdr;

	if (rawlen < sizeof(hdr))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("column block of an append-optimized table ends before its values do")));
	memcpy(&hdr, raw, sizeof(hdr));
	if (hdr.nrows != (uint32) nrows)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("column block of an append-optimized table holds %u rows where its block says %d",
						hdr.nrows, nrows)));
	*nulls = NULL;
	if (hdr.flags & AO_COLUMN_HASNULLS)
		*nulls = (const uint8 *) raw + sizeof(hdr);
	*values = raw + MAXALIGN(sizeof(hdr) + (*nulls ? (nrows + 7) / 8 : 0));
}

/*
 * The nrows values of a column block, the values pointing into raw, which
 * begins MAXALIGNed and outlives them.
 */
void
ao_column_decode(const char *raw, Size rawlen, Form_pg_attribute att,
				 int nrows, Datum *values, bool *isnull)
{
	const uint8 *nulls;
	const char *data;
	uintptr_t	off = 0;

	ao_column_block_parts(raw, rawlen, nrows, &nulls, &data);

	for (int i = 0; i < nrows; i++)
	{
		if (nulls && (nulls[i / 8] & (1 << (i % 8))))
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
			continue;
		}
		isnull[i] = false;
		if (att->attlen == -1)
			off = att_align_pointer(off, att->attalign, -1, data + off);
		else if (att->attlen != -2)
			off = att_align_nominal(off, att->attalign);
		if (data + off >= raw + rawlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("column block of an append-optimized table ends before its values do")));
		values[i] = fetch_att(data + off, att->attbyval, att->attlen);
		off = att_addlength_pointer(off, att->attlen, data + off);
	}
}
