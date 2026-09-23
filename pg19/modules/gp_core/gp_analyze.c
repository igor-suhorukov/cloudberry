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
 * gp_analyze.c
 *	  ANALYZE of a distributed table: O3's first consumer.
 *
 * On the coordinator a distributed table is empty, and it is an ordinary
 * heap table, so nothing about it tells ANALYZE that its rows are elsewhere.
 * O3's hook is how: gp_core takes such a relation, says how many pages it has
 * on the segments together, and supplies the rows.  PostgreSQL 19's own
 * do_analyze_rel() then computes the statistics from them as it would from
 * its own sample -- which keeps every statistic kind a data type defines,
 * PostGIS's 2-D and N-D histograms among them.  Track A's other two options
 * would have copied about 950 lines of analyze.c or lost those kinds.
 *
 * Each segment samples its own rows, with the same block sampler and
 * reservoir PostgreSQL's acquire_sample_rows() uses, through
 * gp_internal.sample_rows(); the coordinator takes from each segment's sample
 * in proportion to how many rows the segment has, chosen at random, so that
 * the whole is a sample of the table.  That is Cloudberry's
 * gp_acquire_sample_rows() in shape.  In a database where gp_core's functions
 * are not installed, the coordinator samples the gathered rows itself -- the
 * same answer, at the cost of reading every row.
 *
 * Cloudberry sources this file stands in for:
 *	  acquire_sample_rows_dispatcher() and gp_acquire_sample_rows() in
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_policy.h"
#include "gp_scan.h"

static analyze_sample_rows_hook_type prev_analyze_sample_rows = NULL;

/* The segment that answers for a replicated table, as a gather of it reads. */
static int
replicated_segment(const GpPolicy *policy)
{
	return GpScanReplicatedContent(policy);
}

/* A gather from that one segment, or else from the table's segments. */
static GpGatherState *
start_on_table(const char *sql, TupleDesc tupdesc, int content,
			   const GpPolicy *policy)
{
	if (content >= 0)
		return GpGatherStartOn(sql, tupdesc, content);
	return GpGatherStartOnSegments(sql, tupdesc, policy->numsegments);
}

/* ------------------------------------------------------------------------- */
/* On a segment                                                              */
/* ------------------------------------------------------------------------- */

static BlockNumber
sample_next_block(ReadStream *stream, void *callback_private_data,
				  void *per_buffer_data)
{
	BlockSamplerData *bs = callback_private_data;

	return BlockSampler_HasMore(bs) ? BlockSampler_Next(bs) : InvalidBlockNumber;
}

static int
compare_rows(const void *a, const void *b)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;

	return ItemPointerCompare(&ha->t_self, &hb->t_self);
}

/*
 * PostgreSQL's acquire_sample_rows(), which is static in analyze.c, written
 * again from the primitives it is made of: the block sampler, the read
 * stream, the table AM's analyze scan and Vitter's reservoir.
 */
static int
segment_sample_rows(Relation rel, HeapTuple *rows, int targrows,
					double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;
	double		samplerows = 0;
	double		liverows = 0;
	double		deadrows = 0;
	double		rowstoskip = -1;
	BlockNumber totalblocks = RelationGetNumberOfBlocks(rel);
	BlockSamplerData bs;
	ReservoirStateData rstate;
	TupleTableSlot *slot;
	TableScanDesc scan;
	ReadStream *stream;

	(void) BlockSampler_Init(&bs, totalblocks, targrows,
							 pg_prng_uint32(&pg_global_prng_state));
	reservoir_init_selection_state(&rstate, targrows);

	scan = table_beginscan_analyze(rel);
	slot = table_slot_create(rel, NULL);
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_USE_BATCHING,
										NULL, scan->rs_rd, MAIN_FORKNUM,
										sample_next_block, &bs, 0);

	while (table_scan_analyze_next_block(scan, stream))
	{
		CHECK_FOR_INTERRUPTS();

		while (table_scan_analyze_next_tuple(scan, &liverows, &deadrows, slot))
		{
			if (numrows < targrows)
				rows[numrows++] = ExecCopySlotHeapTuple(slot);
			else
			{
				if (rowstoskip < 0)
					rowstoskip = reservoir_get_next_S(&rstate, samplerows, targrows);
				if (rowstoskip <= 0)
				{
					int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

					heap_freetuple(rows[k]);
					rows[k] = ExecCopySlotHeapTuple(slot);
				}
				rowstoskip -= 1;
			}
			samplerows += 1;
		}
	}

	read_stream_end(stream);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/* In position order, as ANALYZE's correlation statistic expects. */
	if (numrows == targrows)
		qsort(rows, numrows, sizeof(HeapTuple), compare_rows);

	if (bs.m > 0)
	{
		*totalrows = floor((liverows / bs.m) * totalblocks + 0.5);
		*totaldeadrows = floor((deadrows / bs.m) * totalblocks + 0.5);
	}
	else
	{
		*totalrows = 0.0;
		*totaldeadrows = 0.0;
	}
	return numrows;
}

PG_FUNCTION_INFO_V1(gp_sample_rows);

/*
 * gp_internal.sample_rows(NULL::t, targrows)
 *		This segment's sample of a table, for the coordinator's ANALYZE.
 *
 * The first row says how many rows the segment holds, live and dead, as
 * ANALYZE estimates them; every row after it is a sampled row, as a value of
 * the table's own row type.
 */
Datum
gp_sample_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	int32		targrows = PG_GETARG_INT32(1);
	Oid			relid = get_typ_typrelid(argtype);
	Relation	rel;
	HeapTuple  *rows;
	int			numrows;
	double		totalrows;
	double		totaldeadrows;
	Datum		values[3];
	bool		nulls[3];

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("type %s is not a relation's row type",
						format_type_be(argtype))));
	if (targrows <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the sample size must be positive")));

	/* Whoever may read the rows, or ANALYZE the table, may sample it. */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	rel = table_open(relid, AccessShareLock);
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	numrows = segment_sample_rows(rel, rows, targrows, &totalrows, &totaldeadrows);

	values[0] = Float8GetDatum(totalrows);
	values[1] = Float8GetDatum(totaldeadrows);
	values[2] = (Datum) 0;
	nulls[0] = nulls[1] = false;
	nulls[2] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	nulls[0] = nulls[1] = true;
	nulls[2] = false;
	for (int i = 0; i < numrows; i++)
	{
		values[2] = heap_copy_tuple_as_datum(rows[i], RelationGetDescr(rel));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* On the coordinator                                                        */
/* ------------------------------------------------------------------------- */

typedef struct SegmentSample
{
	double		totalrows;
	double		totaldeadrows;
	List	   *rows;			/* of HeapTuple */
} SegmentSample;

static HeapTuple
tuple_from_composite(Datum value, Oid relid)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	HeapTupleData tmp;

	tmp.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&tmp.t_self);
	tmp.t_tableOid = relid;
	tmp.t_data = td;
	return heap_copytuple(&tmp);
}

/* Does this database have gp_internal.sample_rows()? */
static bool
have_sample_rows_function(void)
{
	Oid			argtypes[2] = {ANYELEMENTOID, INT4OID};

	if (!OidIsValid(get_namespace_oid("gp_internal", true)))
		return false;
	return OidIsValid(LookupFuncName(list_make2(makeString("gp_internal"),
												makeString("sample_rows")),
									 2, argtypes, true));
}

/*
 * The coordinator's sample: each segment's own, merged.  A segment
 * contributes in proportion to how many rows it has, the rows it contributes
 * chosen at random from its sample and kept in its order.
 */
static int
merge_segment_samples(SegmentSample *samples, int nsegs, HeapTuple *rows,
					  int targrows, double *totalrows, double *totaldeadrows)
{
	double		total = 0;
	double		dead = 0;
	int			numrows = 0;

	for (int i = 0; i < nsegs; i++)
	{
		total += samples[i].totalrows;
		dead += samples[i].totaldeadrows;
	}
	*totalrows = total;
	*totaldeadrows = dead;
	if (total <= 0)
		return 0;

	for (int i = 0; i < nsegs && numrows < targrows; i++)
	{
		int			have = list_length(samples[i].rows);
		int			take = (int) floor(targrows * samples[i].totalrows / total + 0.5);
		HeapTuple  *from;
		bool	   *chosen;

		take = Min(take, have);
		take = Min(take, targrows - numrows);
		if (take <= 0)
			continue;

		/* Which of the segment's rows: take at random, keep their order. */
		from = palloc_array(HeapTuple, have);
		chosen = palloc0_array(bool, have);
		for (int k = 0; k < have; k++)
			from[k] = (HeapTuple) list_nth(samples[i].rows, k);
		for (int k = have - take; k < have; k++)
		{
			int			pick = (int) pg_prng_uint64_range(&pg_global_prng_state, 0, k);

			if (chosen[pick])
				pick = k;
			chosen[pick] = true;
		}
		for (int k = 0; k < have; k++)
			if (chosen[k])
				rows[numrows++] = from[k];
	}

	return numrows;
}

/*
 * The sampling function O3 hands ANALYZE for a distributed table.
 */
static int
distributed_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
						double *totalrows, double *totaldeadrows)
{
	Oid			relid = RelationGetRelid(rel);
	GpPolicy   *policy = GpScanDistributedPolicy(relid);
	bool		replicated = GpPolicyIsReplicated(policy);
	int			content = replicated ? replicated_segment(policy) : -1;
	char	   *qualified = GpDispatchRelationName(RelationGetRelid(rel));
	int			nsegs;
	SegmentSample *samples;
	TupleTableSlot *slot;
	GpGatherState *gather;
	int			from;
	int			numrows;

	GpClusterSegments(&nsegs);
	samples = palloc0_array(SegmentSample, nsegs);

	if (have_sample_rows_function())
	{
		TupleDesc	tupdesc = CreateTemplateTupleDesc(3);

		TupleDescInitEntry(tupdesc, 1, "totalrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "totaldeadrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "sample", RelationGetForm(rel)->reltype, -1, 0);
		TupleDescFinalize(tupdesc);

		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		gather = start_on_table(psprintf("SELECT * FROM gp_internal.sample_rows(NULL::%s, %d)",
										 qualified, targrows),
								tupdesc, content, policy);
		while (GpGatherNext(gather, slot, &from))
		{
			SegmentSample *s = &samples[from];

			slot_getallattrs(slot);
			if (!slot->tts_isnull[0])
			{
				s->totalrows = DatumGetFloat8(slot->tts_values[0]);
				s->totaldeadrows = DatumGetFloat8(slot->tts_values[1]);
			}
			else if (!slot->tts_isnull[2])
				s->rows = lappend(s->rows, tuple_from_composite(slot->tts_values[2], relid));
		}
		GpGatherEnd(gather);
		ExecDropSingleTupleTableSlot(slot);
	}
	else
	{
		/*
		 * No gp_core in this database: sample the gathered rows here, with
		 * the same reservoir.  Every row travels, which is the price.
		 */
		ReservoirStateData rstate;
		double		rowstoskip = -1;
		double		seen = 0;

		reservoir_init_selection_state(&rstate, targrows);
		slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
		gather = start_on_table(psprintf("SELECT * FROM ONLY %s", qualified),
								RelationGetDescr(rel), content, policy);

		/* one list for the whole table: it is one sample already */
		while (GpGatherNext(gather, slot, NULL))
		{
			HeapTuple	tuple = ExecCopySlotHeapTuple(slot);

			tuple->t_tableOid = relid;
			if (seen < targrows)
				samples[0].rows = lappend(samples[0].rows, tuple);
			else
			{
				if (rowstoskip < 0)
					rowstoskip = reservoir_get_next_S(&rstate, seen, targrows);
				if (rowstoskip <= 0)
				{
					int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

					list_nth_cell(samples[0].rows, k)->ptr_value = tuple;
				}
				rowstoskip -= 1;
			}
			seen += 1;
		}
		GpGatherEnd(gather);
		ExecDropSingleTupleTableSlot(slot);
		samples[0].totalrows = seen;
		nsegs = 1;
	}

	/* A replicated table's one segment answered for the whole table. */
	if (replicated && nsegs > 1)
	{
		samples[0] = samples[content];
		nsegs = 1;
	}

	numrows = merge_segment_samples(samples, nsegs, rows, targrows,
									totalrows, totaldeadrows);

	ereport(elevel,
			(errmsg("\"%s\": sampled from %s: %d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(rel),
					replicated ? "one segment" : "every segment",
					numrows, *totalrows)));

	return numrows;
}

/*
 * O3's hook: a distributed table is sampled on the segments, and its pages
 * are the segments' pages together.
 */
static bool
gp_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
					   BlockNumber *totalpages)
{
	GpPolicy   *policy;
	int			nsegs;
	char	  **sizes;
	double		bytes = 0;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		relation->rd_rel->relkind != RELKIND_RELATION ||
		AmAutoVacuumWorkerProcess() ||
		(policy = GpScanDistributedPolicy(RelationGetRelid(relation))) == NULL)
		return prev_analyze_sample_rows
			? prev_analyze_sample_rows(relation, func, totalpages) : false;

	GpClusterSegments(&nsegs);
	sizes = palloc0_array(char *, nsegs);
	GpDispatchQueryFirstValues(psprintf("SELECT pg_catalog.pg_relation_size(%u)",
										RelationGetRelid(relation)),
							   GpPolicyIsReplicated(policy) ? replicated_segment(policy) : -1,
							   sizes);
	for (int i = 0; i < (GpPolicyIsReplicated(policy) ? 1 : nsegs); i++)
		if (sizes[i] != NULL)
			bytes += strtod(sizes[i], NULL);

	*totalpages = (BlockNumber) Min(bytes / BLCKSZ, (double) MaxBlockNumber);
	*func = distributed_sample_rows;
	return true;
}

void
GpAnalyzeInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = gp_analyze_sample_rows;
}
