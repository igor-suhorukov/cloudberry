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
 * ao_toolkit.c
 *	  What Cloudberry shows of an append-optimized table: pg_appendonly,
 *	  gp_toolkit's __gp_aoseg and the rest, get_ao_compression_ratio().
 *
 * Cloudberry keeps an append-optimized table's options in pg_appendonly and
 * its segment files, deletions and block directory in relations of its own,
 * which gp_toolkit's functions (gp_ao_co_diagnostics.c) and a few built-in
 * ones read.  The port keeps them in pg_class.reloptions and gp_ao's three
 * tables (ao_meta.c), so these read those, and answer in Cloudberry's shape,
 * as far as the port's storage has the same things to say: a TID is the
 * port's (gp_ao.h), and a block directory row covers one block of rows in
 * each file rather than a minipage of them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_ao.h"

PG_FUNCTION_INFO_V1(gp_ao_reloption_values);
PG_FUNCTION_INFO_V1(gp_ao_segfile_count);
PG_FUNCTION_INFO_V1(gp_ao_segfilecount_of);
PG_FUNCTION_INFO_V1(gp_ao_aoseg);
PG_FUNCTION_INFO_V1(gp_ao_aocsseg);
PG_FUNCTION_INFO_V1(gp_ao_segment_files);
PG_FUNCTION_INFO_V1(gp_ao_aovisimap);
PG_FUNCTION_INFO_V1(gp_ao_aovisimap_hidden_info);
PG_FUNCTION_INFO_V1(gp_ao_aovisimap_entry);
PG_FUNCTION_INFO_V1(gp_ao_aovisimap_compaction_info);
PG_FUNCTION_INFO_V1(gp_ao_aoblkdir);
PG_FUNCTION_INFO_V1(gp_ao_compression_ratio);

/* This node's content id, as Cloudberry's GpIdentity.segindex. */
static int
content_id(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core ? core->get_content_id() : -1;
}

/*
 * What each function of Cloudberry's says of a table that is not one it
 * reads, in its words: __gp_aoseg and __gp_aocsseg of the other method's
 * (aosegfiles.c, aocssegfiles.c), the visibility map's and the block
 * directory's of a table that is neither (appendonly_visimap_udf.c,
 * appendonly_blkdir_udf.c).
 */
typedef enum AoWanted
{
	AO_WANT_ROW,
	AO_WANT_COLUMN,
	AO_WANT_VISIMAP,
	AO_WANT_BLKDIR,
} AoWanted;

/* The table named, opened for reading, if it is one the function reads. */
static Relation
open_ao(Oid relid, AoWanted wanted)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		ao = ao_is_ao_table(rel) && RELKIND_HAS_STORAGE(rel->rd_rel->relkind);

	switch (wanted)
	{
		case AO_WANT_ROW:
			if (!ao || ao_storage_is_columnar(rel))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Relation '%s' does not have appendoptimized row-oriented storage",
								RelationGetRelationName(rel))));
			break;
		case AO_WANT_COLUMN:
			if (!ao || !ao_storage_is_columnar(rel))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Relation '%s' does not have append-optimized column-oriented storage",
								RelationGetRelationName(rel))));
			break;
		case AO_WANT_VISIMAP:
			if (!ao)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function not supported on relation")));
			break;
		case AO_WANT_BLKDIR:
			if (!ao)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function not supported on non append-optimized relation")));
			break;
	}
	return rel;
}

/* ------------------------------------------------------------------------- */
/* pg_appendonly                                                             */
/* ------------------------------------------------------------------------- */

/*
 * A table's options, from what pg_class holds of it: blocksize,
 * compresstype, compresslevel, checksum and whether it is stored by column.
 * The table is not opened, so the view over every table locks none.
 */
Datum
gp_ao_reloption_values(PG_FUNCTION_ARGS)
{
	Oid			relam = PG_GETARG_OID(0);
	char		relkind = PG_GETARG_CHAR(1);
	Datum		reloptions = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2);
	bool		columnar = (relam == get_table_am_oid("ao_column", true));
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {0};
	AoOptions	opts;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	ao_options_from_reloptions(reloptions, relkind, columnar, &opts);
	values[0] = Int32GetDatum(opts.blocksize);
	values[1] = CStringGetTextDatum(ao_compresstype_name(opts.compresstype));
	values[2] = Int32GetDatum(opts.compresslevel);
	values[3] = BoolGetDatum(opts.checksum);
	values[4] = BoolGetDatum(columnar);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* How many segment files a table has, or NULL where it is gone. */
Datum
gp_ao_segfile_count(PG_FUNCTION_ARGS)
{
	Relation	rel = try_relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			n = 0;

	if (rel == NULL)
		PG_RETURN_NULL();
	if (ao_is_ao_table(rel) && RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		pfree(ao_segfiles_read(ao_storage_id(rel), GetLatestSnapshot(), &n));
	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(n);
}

/* pg_appendonly.segfilecount: what ANALYZE last counted, or NULL where gone. */
Datum
gp_ao_segfilecount_of(PG_FUNCTION_ARGS)
{
	Relation	rel = try_relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			n = 0;

	if (rel == NULL)
		PG_RETURN_NULL();
	if (ao_is_ao_table(rel) && RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		n = ao_segfilecount_get(ao_storage_id(rel));
	relation_close(rel, AccessShareLock);
	PG_RETURN_INT16((int16) n);
}

/* ------------------------------------------------------------------------- */
/* gp_toolkit's functions                                                    */
/* ------------------------------------------------------------------------- */

/* __gp_aoseg(regclass): each segment file of a table by row. */
Datum
gp_ao_aoseg(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	AoSegfile  *segfiles;
	int			n;

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_ROW);
	segfiles = ao_segfiles_read(ao_storage_id(rel), GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		AoSegfile  *sf = &segfiles[i];
		Datum		values[9];
		bool		nulls[9] = {0};

		values[0] = Int32GetDatum(content_id());
		values[1] = Int32GetDatum(sf->segno);
		values[2] = Int64GetDatum(sf->eof[0]);
		values[3] = Int64GetDatum(sf->tupcount);
		values[4] = Int64GetDatum(sf->varblockcount);
		values[5] = Int64GetDatum(sf->eof_uncompressed[0]);
		values[6] = Int64GetDatum(sf->modcount);
		values[7] = Int16GetDatum(sf->formatversion);
		values[8] = Int16GetDatum(sf->state);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * __gp_aocsseg(regclass): each column of each segment file of a table by
 * column.  A column the segment file has no file of -- added after a
 * VACUUM compacted it -- has ends of -1, as in Cloudberry.
 */
Datum
gp_ao_aocsseg(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	AoSegfile  *segfiles;
	int			n;
	int			natts;

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_COLUMN);
	natts = RelationGetDescr(rel)->natts;
	segfiles = ao_segfiles_read(ao_storage_id(rel), GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		AoSegfile  *sf = &segfiles[i];

		for (int col = 0; col < natts; col++)
		{
			Datum		values[10];
			bool		nulls[10] = {0};
			bool		has = col < sf->ngroups;

			values[0] = Int32GetDatum(content_id());
			values[1] = Int32GetDatum(sf->segno);
			values[2] = Int16GetDatum(col);
			values[3] = Int32GetDatum(col * (AO_MAX_SEGNO + 1) + sf->segno);
			values[4] = Int64GetDatum(sf->tupcount);
			values[5] = Int64GetDatum(has ? sf->eof[col] : -1);
			values[6] = Int64GetDatum(has ? sf->eof_uncompressed[col] : -1);
			values[7] = Int64GetDatum(sf->modcount);
			values[8] = Int16GetDatum(sf->formatversion);
			values[9] = Int16GetDatum(sf->state);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * __gp_ao_segment_files(regclass): the files of a table's relation past its
 * first that hold its segment files' bytes -- relfilenode.1 and on,
 * PostgreSQL's segments of the relation -- and each one's bytes, up to the
 * last page a segment file reaches: what gp_toolkit's __get_ao_segno_list()
 * lists of a table, where Cloudberry keeps each segment file as a file of
 * its own, relfilenode.<segno>.  This node's, which the checks for missing
 * files ask on each.
 */
Datum
gp_ao_segment_files(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	AoSegfile  *segfiles;
	int			n;
	bool		columnar;
	BlockNumber end = 0;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_VISIMAP);
	columnar = ao_storage_is_columnar(rel);
	segfiles = ao_segfiles_read(ao_storage_id(rel), GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
		for (int g = 0; g < segfiles[i].ngroups; g++)
			end = Max(end, ao_file_end(rel,
									   AoFileNum(segfiles[i].segno, columnar ? g + 1 : 0),
									   segfiles[i].eof[g]));
	for (BlockNumber k = 1; (uint64) k * RELSEG_SIZE < end; k++)
	{
		Datum		values[2];
		bool		nulls[2] = {0};
		uint64		last = Min((uint64) end, (uint64) (k + 1) * RELSEG_SIZE);

		values[0] = Int32GetDatum((int32) k);
		values[1] = Int64GetDatum((int64) ((last - (uint64) k * RELSEG_SIZE) * BLCKSZ));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/* __gp_aovisimap(regclass): each row the visibility map says is deleted. */
Datum
gp_ao_aovisimap(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	int64		storage_id;
	AoSegfile  *segfiles;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_VISIMAP);
	storage_id = ao_storage_id(rel);
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		AoVisimap  *vm = ao_visimap_load(storage_id, segfiles[i].segno,
										 GetLatestSnapshot());

		for (int e = 0; e < vm->nentries; e++)
		{
			for (int bit = 0; bit < AO_VISIMAP_ROWS; bit++)
			{
				ItemPointerData tid;
				Datum		values[3];
				bool		nulls[3] = {0};
				int64		rownum = vm->first_rows[e] + bit;

				if ((vm->bitmaps[e][bit / 8] & (1 << (bit % 8))) == 0)
					continue;
				AoTidSet(&tid, segfiles[i].segno, rownum);
				values[0] = ItemPointerGetDatum(&tid);
				values[1] = Int32GetDatum(segfiles[i].segno);
				values[2] = Int64GetDatum(rownum);
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
			}
		}
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * __gp_aovisimap_hidden_info(regclass): how many rows of each segment file
 * are deleted, of how many.
 */
Datum
gp_ao_aovisimap_hidden_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	int64		storage_id;
	AoSegfile  *segfiles;
	int			n;

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_VISIMAP);
	storage_id = ao_storage_id(rel);
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		Datum		values[3];
		bool		nulls[3] = {0};

		values[0] = Int32GetDatum(segfiles[i].segno);
		values[1] = Int64GetDatum(ao_visimap_count(ao_visimap_load(storage_id,
																   segfiles[i].segno,
																   GetLatestSnapshot())));
		values[2] = Int64GetDatum(segfiles[i].tupcount);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * __gp_aovisimap_compaction_info(oid): each segment file of each segment,
 * with how many of its rows are deleted and whether VACUUM would compact it
 * at gp.appendonly_compaction_threshold -- Cloudberry's PL/pgSQL function,
 * which reads the segments' hidden rows through gp_dist_random('gp_id'),
 * here asking each segment for its own.
 */
Datum
gp_ao_aovisimap_compaction_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const GpCoreApi *core = GpCoreApiLookup();
	Relation	rel;
	int64		storage_id;
	AoSegfile  *segfiles;
	int			n;
	Datum		threshold = DirectFunctionCall1(int4_numeric,
												Int32GetDatum(gp_appendonly_compaction_threshold));

	if (!GpDispatchFunctionToSegments(fcinfo))
	{
		InitMaterializedSRF(fcinfo, 0);
		rel = open_ao(PG_GETARG_OID(0), AO_WANT_VISIMAP);
		storage_id = ao_storage_id(rel);
		segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &n);
		for (int i = 0; i < n; i++)
		{
			Datum		values[6];
			bool		nulls[6] = {0};
			int64		hidden = ao_visimap_count(ao_visimap_load(storage_id,
																  segfiles[i].segno,
																  GetLatestSnapshot()));
			int64		total = segfiles[i].tupcount;
			Datum		percent;

			if (total > 0)
				percent = DirectFunctionCall2(numeric_round,
											  DirectFunctionCall2(numeric_div,
																  NumericGetDatum(int64_to_numeric(100 * hidden)),
																  NumericGetDatum(int64_to_numeric(total))),
											  Int32GetDatum(2));
			else
				percent = DirectFunctionCall2(numeric_round,
											  NumericGetDatum(int64_to_numeric(0)),
											  Int32GetDatum(2));
			values[0] = Int32GetDatum(content_id());
			values[1] = Int32GetDatum(segfiles[i].segno);
			values[2] = BoolGetDatum(DatumGetBool(DirectFunctionCall2(numeric_gt,
																	  percent,
																	  threshold)));
			values[3] = Int64GetDatum(hidden);
			values[4] = Int64GetDatum(total);
			values[5] = percent;
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
		relation_close(rel, AccessShareLock);
	}

	/* once, where the caller is, as Cloudberry's RAISE NOTICE says it */
	if (core == NULL || core->get_role() != GP_ROLE_EXECUTE)
		ereport(NOTICE,
				(errmsg("gp_appendonly_compaction_threshold = %d",
						gp_appendonly_compaction_threshold)));
	return (Datum) 0;
}

/*
 * __gp_aovisimap_entry(regclass): each entry of the visibility map, its
 * bitmap as Cloudberry prints one, a digit a row, first row first.
 */
Datum
gp_ao_aovisimap_entry(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	int64		storage_id;
	AoSegfile  *segfiles;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_VISIMAP);
	storage_id = ao_storage_id(rel);
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		AoVisimap  *vm = ao_visimap_load(storage_id, segfiles[i].segno,
										 GetLatestSnapshot());

		for (int e = 0; e < vm->nentries; e++)
		{
			Datum		values[4];
			bool		nulls[4] = {0};
			int			hidden = 0;
			int			last = -1;
			char	   *bits;

			for (int b = 0; b < AO_VISIMAP_BYTES; b++)
			{
				hidden += pg_popcount32(vm->bitmaps[e][b]);
				if (vm->bitmaps[e][b] != 0)
					last = b;
			}
			/* Up to the last byte with a row deleted in it. */
			bits = palloc((last + 1) * 8 + 1);
			for (int bit = 0; bit < (last + 1) * 8; bit++)
				bits[bit] = (vm->bitmaps[e][bit / 8] & (1 << (bit % 8))) ? '1' : '0';
			bits[(last + 1) * 8] = '\0';

			values[0] = Int32GetDatum(segfiles[i].segno);
			values[1] = Int64GetDatum(vm->first_rows[e]);
			values[2] = Int32GetDatum(hidden);
			values[3] = CStringGetTextDatum(bits);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * __gp_aoblkdir(regclass): the block directory, a row for each block of
 * rows in each file -- a column group of Cloudberry's is a column here, and
 * each directory row has one entry.
 */
Datum
gp_ao_aoblkdir(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	int64		storage_id;
	AoSegfile  *segfiles;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_ao(PG_GETARG_OID(0), AO_WANT_BLKDIR);
	storage_id = ao_storage_id(rel);
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &n);
	for (int i = 0; i < n; i++)
	{
		AoBlkdirScan *bs = ao_blkdir_scan_begin(storage_id, segfiles[i].segno,
												GetLatestSnapshot());
		AoBlkdirEntry entry;

		while (ao_blkdir_scan_next(bs, &entry))
		{
			for (int g = 0; g < entry.noffsets; g++)
			{
				Datum		values[7];
				bool		nulls[7] = {0};

				if (entry.offsets[g] < 0)
					continue;
				values[0] = ItemPointerGetDatum(&entry.tid);
				values[1] = Int32GetDatum(entry.segno);
				values[2] = Int32GetDatum(g);
				values[3] = Int32GetDatum(0);
				values[4] = Int64GetDatum(entry.first_row);
				values[5] = Int64GetDatum(entry.offsets[g]);
				values[6] = Int64GetDatum(entry.nrows);
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
			}
		}
		ao_blkdir_scan_end(bs);
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* get_ao_compression_ratio                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The table's bytes uncompressed over its bytes stored, to two decimals, over
 * every segment -- which gp_toolkit's functions, running on each, give it --
 * as Cloudberry's does: for a table by row with no segment file yet 1, and
 * -1 for one whose files are empty, a table by column with none, or a
 * partitioned table.
 */
Datum
gp_ao_compression_ratio(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = relation_open(relid, AccessShareLock);
	float8		ratio = -1;
	bool		columnar;
	const char *sql;

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, AccessShareLock);
		PG_RETURN_FLOAT8(ratio);
	}
	if (!ao_is_ao_table(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("'%s' is not an append-only relation",
						RelationGetRelationName(rel))));
	columnar = ao_storage_is_columnar(rel);
	relation_close(rel, AccessShareLock);

	sql = columnar ?
		"SELECT sum(eof)::int8, sum(eof_uncompressed)::int8 FROM gp_toolkit.__gp_aocsseg($1) WHERE eof >= 0" :
		"SELECT sum(eof)::int8, sum(eof_uncompressed)::int8 FROM gp_toolkit.__gp_aoseg($1)";

	SPI_connect();
	{
		Oid			argtypes[1] = {REGCLASSOID};
		Datum		args[1] = {ObjectIdGetDatum(relid)};
		bool		isnull1;
		bool		isnull2;
		int64		eof;
		int64		eof_uncompressed;

		if (SPI_execute_with_args(sql, 1, argtypes, args, NULL, true, 1) != SPI_OK_SELECT ||
			SPI_processed != 1)
			elog(ERROR, "could not read the segment files of \"%s\"",
				 get_rel_name(relid));
		eof = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1, &isnull1));
		eof_uncompressed = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
													   SPI_tuptable->tupdesc, 2,
													   &isnull2));
		/* No segment file yet: 1 by row, -1 by column, as in Cloudberry. */
		if (isnull1 || isnull2)
			ratio = columnar ? -1 : 1;
		else if (eof > 0)
			ratio = round((float8) eof_uncompressed / (float8) eof * 100.0) / 100.0;
	}
	SPI_finish();
	PG_RETURN_FLOAT8(ratio);
}
