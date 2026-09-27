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
 * An inheritance tree -- a partitioned table's leaves -- is sampled as one,
 * each segment sampling its members, as Cloudberry's is, rather than member
 * by member (gp_internal.sample_tree()).  A partitioned table's statistics
 * are PostgreSQL's, from that sample, where Cloudberry's merge the leaves'
 * own; the fault Cloudberry's merge has once it has found the leaves is
 * where PostgreSQL's has found them.  With VERBOSE each sample says what it
 * sends the segments, as Cloudberry's does, in its words.
 *
 * Cloudberry sources this file stands in for:
 *	  acquire_sample_rows_dispatcher() and gp_acquire_sample_rows() in
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
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

/*
 * The sampling function a table on this node is sampled with, and its pages:
 * a table access method that samples its tables itself -- gp_ao's, whose
 * rows are not where the block sampler would look -- says so through O3's
 * hook, as it says so to an ANALYZE on this node; the others' rows are
 * sampled as acquire_sample_rows() samples a heap's.  On the coordinator the
 * hook is gp_core's own, which would ask the segments again.
 */
static AnalyzeSampleRowsFunc
local_sampler(Relation rel, BlockNumber *totalpages)
{
	AnalyzeSampleRowsFunc func = NULL;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH &&
		analyze_sample_rows_hook != NULL &&
		analyze_sample_rows_hook(rel, &func, totalpages) && func != NULL)
		return func;
	*totalpages = RelationGetNumberOfBlocks(rel);
	return NULL;
}

static int
local_sample_rows(Relation rel, AnalyzeSampleRowsFunc func, HeapTuple *rows,
				  int targrows, double *totalrows, double *totaldeadrows)
{
	if (func != NULL)
		return func(rel, DEBUG1, rows, targrows, totalrows, totaldeadrows);
	return segment_sample_rows(rel, rows, targrows, totalrows, totaldeadrows);
}

/* The table whose row type is the first argument's, for the SQL functions. */
static Oid
sampled_relation(FunctionCallInfo fcinfo)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = get_typ_typrelid(argtype);

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("type %s is not a relation's row type",
						format_type_be(argtype))));
	if (PG_GETARG_INT32(1) <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the sample size must be positive")));

	/* Whoever may read the rows, or ANALYZE the table, may sample it. */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));
	return relid;
}

/* The rows and counts of a segment's sample, as the SQL functions give them. */
static void
put_sample(ReturnSetInfo *rsinfo, TupleDesc rowdesc, HeapTuple *rows,
		   int numrows, double totalrows, double totaldeadrows)
{
	Datum		values[3];
	bool		nulls[3];

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
		values[2] = heap_copy_tuple_as_datum(rows[i], rowdesc);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
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
	Oid			relid = sampled_relation(fcinfo);
	int32		targrows = PG_GETARG_INT32(1);
	Relation	rel;
	HeapTuple  *rows;
	int			numrows;
	double		totalrows;
	double		totaldeadrows;
	AnalyzeSampleRowsFunc func;
	BlockNumber totalpages;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	rel = table_open(relid, AccessShareLock);
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	func = local_sampler(rel, &totalpages);
	numrows = local_sample_rows(rel, func, rows, targrows, &totalrows,
								&totaldeadrows);
	put_sample(rsinfo, RelationGetDescr(rel), rows, numrows, totalrows,
			   totaldeadrows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_sample_tree);

/*
 * gp_internal.sample_tree(NULL::t, targrows)
 *		This segment's sample of a table and every table under it, as one
 *		sample of the table's rows, for the coordinator's ANALYZE of the
 *		tree -- Cloudberry's gp_acquire_sample_rows(t, n, 't').
 *
 * PostgreSQL's acquire_inherited_sample_rows() on this segment's rows: each
 * member sampled in proportion to its pages here, and its rows made rows of
 * the table's own type.  The rows come as sample_rows()'s do.
 */
Datum
gp_sample_tree(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = sampled_relation(fcinfo);
	int32		targrows = PG_GETARG_INT32(1);
	Relation	parent;
	List	   *members;
	int			nmembers;
	Relation   *rels;
	AnalyzeSampleRowsFunc *funcs;
	double	   *pages;
	double		totalpages = 0;
	HeapTuple  *rows;
	int			numrows = 0;
	double		totalrows = 0;
	double		totaldeadrows = 0;
	int			n = 0;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	parent = table_open(relid, AccessShareLock);
	members = find_all_inheritors(relid, AccessShareLock, NULL);
	nmembers = list_length(members);
	rels = palloc_array(Relation, nmembers);
	funcs = palloc0_array(AnalyzeSampleRowsFunc, nmembers);
	pages = palloc0_array(double, nmembers);
	foreach_oid(member, members)
	{
		Relation	rel = table_open(member, NoLock);
		BlockNumber relpages;

		if (RELATION_IS_OTHER_TEMP(rel) ||
			(rel->rd_rel->relkind != RELKIND_RELATION &&
			 rel->rd_rel->relkind != RELKIND_MATVIEW))
		{
			table_close(rel, NoLock);
			continue;
		}
		funcs[n] = local_sampler(rel, &relpages);
		rels[n] = rel;
		pages[n] = relpages;
		totalpages += relpages;
		n++;
	}

	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	for (int i = 0; i < n; i++)
	{
		int			childtargrows;
		int			childrows;
		double		trows,
					tdrows;

		if (pages[i] <= 0)
			continue;
		childtargrows = Min((int) rint(targrows * pages[i] / totalpages),
							targrows - numrows);
		if (childtargrows <= 0)
			continue;
		childrows = local_sample_rows(rels[i], funcs[i], rows + numrows,
									  childtargrows, &trows, &tdrows);

		/* a member's rows as rows of the table's own type */
		if (childrows > 0 &&
			!equalRowTypes(RelationGetDescr(rels[i]), RelationGetDescr(parent)))
		{
			TupleConversionMap *map = convert_tuples_by_name(RelationGetDescr(rels[i]),
															 RelationGetDescr(parent));

			if (map != NULL)
			{
				for (int j = 0; j < childrows; j++)
					rows[numrows + j] = execute_attr_map_tuple(rows[numrows + j], map);
				free_conversion_map(map);
			}
		}
		numrows += childrows;
		totalrows += trows;
		totaldeadrows += tdrows;
	}

	put_sample(rsinfo, RelationGetDescr(parent), rows, numrows, totalrows,
			   totaldeadrows);
	for (int i = 0; i < n; i++)
		table_close(rels[i], NoLock);
	table_close(parent, AccessShareLock);
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

/* Does this database have gp_internal.sample_rows(), or sample_tree()? */
static bool
have_sample_function(const char *name)
{
	Oid			argtypes[2] = {ANYELEMENTOID, INT4OID};

	if (!OidIsValid(get_namespace_oid("gp_internal", true)))
		return false;
	return OidIsValid(LookupFuncName(list_make2(makeString("gp_internal"),
												makeString(pstrdup(name))),
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
 * A distributed table's sample, or with "tree" its inheritance tree's, as
 * one sample of the table's rows: each segment's own, merged.  Cloudberry's
 * acquire_sample_rows_dispatcher(), which says at elevel what it sends.
 */
static int
gather_sample(Relation rel, GpPolicy *policy, bool tree, int elevel,
			  HeapTuple *rows, int targrows,
			  double *totalrows, double *totaldeadrows)
{
	Oid			relid = RelationGetRelid(rel);
	bool		replicated = GpPolicyIsReplicated(policy);
	int			content = replicated ? replicated_segment(policy) : -1;
	char	   *qualified = GpDispatchRelationName(RelationGetRelid(rel));
	int			nsegs;
	SegmentSample *samples;
	TupleTableSlot *slot;
	GpGatherState *gather;
	int			from;
	char	   *sql;

	GpClusterSegments(&nsegs);
	samples = palloc0_array(SegmentSample, nsegs);

	if (have_sample_function(tree ? "sample_tree" : "sample_rows"))
	{
		TupleDesc	tupdesc = CreateTemplateTupleDesc(3);

		TupleDescInitEntry(tupdesc, 1, "totalrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "totaldeadrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "sample", RelationGetForm(rel)->reltype, -1, 0);
		TupleDescFinalize(tupdesc);

		sql = psprintf("SELECT * FROM gp_internal.%s(NULL::%s, %d)",
					   tree ? "sample_tree" : "sample_rows", qualified, targrows);
		ereport(elevel, (errmsg("Executing SQL: %s", sql)));
		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		gather = start_on_table(sql, tupdesc, content, policy);
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

		sql = psprintf("SELECT * FROM %s%s", tree ? "" : "ONLY ", qualified);
		ereport(elevel, (errmsg("Executing SQL: %s", sql)));
		reservoir_init_selection_state(&rstate, targrows);
		slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
		gather = start_on_table(sql, RelationGetDescr(rel), content, policy);

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

	return merge_segment_samples(samples, nsegs, rows, targrows,
								 totalrows, totaldeadrows);
}

/*
 * The sampling function O3 hands ANALYZE for a distributed table analyzed
 * itself.  Cloudberry's do_analyze_rel() looks for the relation's
 * inheritance tree first, and says it skips one there is none of -- and
 * where the catalog said there was, says so no more, as PostgreSQL's own
 * look does, which then has nothing to look for (Cloudberry's issue 14644).
 */
static int
distributed_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
						double *totalrows, double *totaldeadrows)
{
	if (find_inheritance_children(RelationGetRelid(rel), NoLock) == NIL)
	{
		if (rel->rd_rel->relhassubclass)
		{
			CommandCounterIncrement();
			SetRelationHasSubclass(RelationGetRelid(rel), false);
		}
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no child tables",
						get_namespace_name(RelationGetNamespace(rel)),
						RelationGetRelationName(rel))));
	}
	return gather_sample(rel, GpScanDistributedPolicy(RelationGetRelid(rel)),
						 false, elevel, rows, targrows, totalrows, totaldeadrows);
}

/* The same, for a member of a tree whose members are sampled one by one. */
static int
member_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
				   double *totalrows, double *totaldeadrows)
{
	return gather_sample(rel, GpScanDistributedPolicy(RelationGetRelid(rel)),
						 false, elevel, rows, targrows, totalrows, totaldeadrows);
}

/* ------------------------------------------------------------------------- */
/* An inheritance tree, sampled at once                                      */
/* ------------------------------------------------------------------------- */

/*
 * PostgreSQL's acquire_inherited_sample_rows() asks O3's hook of the tree's
 * parent and then of each member find_all_inheritors() found, in that order,
 * and samples each member in proportion to its pages.  Cloudberry's samples
 * the whole tree in one dispatch instead, each segment sampling its members
 * (gp_acquire_sample_rows(t, n, 't')); so does this, where it can: the
 * parent is given the tree's sampling function and the members no pages, so
 * that PostgreSQL samples none of them itself.  A round trip or two a
 * member was the cost otherwise, which a table of a thousand partitions
 * pays a thousand times.
 */
static List *walk_members = NIL;	/* not yet asked of, in TopTransactionContext */
static LocalTransactionId walk_lxid = InvalidLocalTransactionId;
static GpPolicy *walk_policy = NULL;	/* where the tree is sampled at once */

/* The relation analyze_rel() asked of last, whose next ask is its tree's */
static Oid	last_asked = InvalidOid;
static LocalTransactionId last_asked_lxid = InvalidLocalTransactionId;

/*
 * The segments a tree can be sampled on at once, or NULL: every member with
 * rows a table distributed over the same segments, and none of them
 * replicated, a foreign table or a temporary table.
 */
static GpPolicy *
tree_policy(List *members)
{
	GpPolicy   *policy = NULL;

	foreach_oid(member, members)
	{
		char		relkind = get_rel_relkind(member);
		GpPolicy   *p;

		if (relkind == RELKIND_PARTITIONED_TABLE)
			continue;
		if ((relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW) ||
			get_rel_persistence(member) == RELPERSISTENCE_TEMP ||
			(p = GpScanDistributedPolicy(member)) == NULL ||
			GpPolicyIsReplicated(p) ||
			(policy != NULL && p->numsegments != policy->numsegments))
			return NULL;
		if (policy == NULL)
			policy = p;
	}
	return policy;
}

/* The tree's sampling function, given to its parent. */
static int
tree_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
				 double *totalrows, double *totaldeadrows)
{
	return gather_sample(rel, walk_policy, true, elevel, rows, targrows,
						 totalrows, totaldeadrows);
}

/* A member's, which is never called: the member has no pages to sample. */
static int
no_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
			   double *totalrows, double *totaldeadrows)
{
	*totalrows = 0;
	*totaldeadrows = 0;
	return 0;
}

/*
 * A distributed table's pages and sampling function: its pages the
 * segments' pages together, their bytes rounded up to pages, as Cloudberry's
 * AcquireNumberOfBlocks() rounds them.  An append-optimized or PAX table's
 * files are no whole pages, and a small one rounded down would be counted
 * as none, which the planner takes for a table never analyzed.
 */
static bool
distributed_table(Relation relation, AnalyzeSampleRowsFunc sampler,
				  AnalyzeSampleRowsFunc *func, BlockNumber *totalpages)
{
	GpPolicy   *policy;
	int			nsegs;
	char	  **sizes;
	double		bytes = 0;

	if ((relation->rd_rel->relkind != RELKIND_RELATION &&
		 relation->rd_rel->relkind != RELKIND_MATVIEW) ||
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

	*totalpages = (BlockNumber) Min(ceil(bytes / BLCKSZ), (double) MaxBlockNumber);
	*func = sampler;
	return true;
}

/*
 * O3's hook: a distributed table is sampled on the segments, and an
 * inheritance tree of them at once, as above.
 *
 * Which ask is which: analyze_rel() asks of the relation it analyzes; if
 * that relation has children, acquire_inherited_sample_rows() asks of it
 * again, in the same transaction, and then of each member in turn.  A
 * partitioned table's second ask is also where Cloudberry's
 * merge_leaf_stats() has found the leaves, whose fault is there.
 */
static bool
gp_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
					   BlockNumber *totalpages)
{
	Oid			relid = RelationGetRelid(relation);
	LocalTransactionId lxid = MyProc->vxid.lxid;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH || AmAutoVacuumWorkerProcess())
		return prev_analyze_sample_rows
			? prev_analyze_sample_rows(relation, func, totalpages) : false;

	/* A member of the tree being walked, and those skipped before it gone */
	if (walk_lxid == lxid && list_member_oid(walk_members, relid))
	{
		while (linitial_oid(walk_members) != relid)
			walk_members = list_delete_first(walk_members);
		walk_members = list_delete_first(walk_members);
		if (walk_policy != NULL)
		{
			/* gp_ao's, outside this one, has counted its segment files */
			*func = no_sample_rows;
			*totalpages = 0;
			return true;
		}
		return distributed_table(relation, member_sample_rows, func, totalpages);
	}
	walk_members = NIL;

	/* The second ask of a parent: its tree's */
	if (relid == last_asked && lxid == last_asked_lxid &&
		relation->rd_rel->relhassubclass)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		List	   *members = find_all_inheritors(relid, NoLock, NULL);

		last_asked = InvalidOid;
		walk_policy = tree_policy(members);
		walk_members = list_delete_first(members);
		walk_lxid = lxid;
		MemoryContextSwitchTo(old);

		if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			GP_FAULT("merge_leaf_stats_after_find_children");

		if (walk_policy != NULL)
		{
			*func = tree_sample_rows;
			*totalpages = 1;
			return true;
		}
		return distributed_table(relation, member_sample_rows, func, totalpages);
	}

	last_asked = relid;
	last_asked_lxid = lxid;
	return distributed_table(relation, distributed_sample_rows, func, totalpages);
}

/* ------------------------------------------------------------------------- */
/* What the segments count, brought back                                     */
/* ------------------------------------------------------------------------- */

/* One relation's counts, summed over the segments that reported it. */
typedef struct SegmentCounts
{
	Oid			relid;			/* the hash key */
	Oid			table;			/* the table it is, or whose index */
	double		pages;
	double		tuples;
	double		allvisible;
	double		allfrozen;
	int			nsegs;
} SegmentCounts;

/*
 * The relations a VACUUM or ANALYZE statement took whose rows are on the
 * segments, as vacuum() finds them: one given by its OID as it is -- which
 * is how gp_partanalyze.c hands on the list Cloudberry's rules make -- one
 * named with its partitions and children unless ONLY says not, or, when it
 * named none, every table of the database, as get_all_vacuum_rels() takes
 * them -- those the user may maintain, the others having been passed over
 * with a warning.
 */
static List *
distributed_relids(VacuumStmt *stmt)
{
	List	   *candidates = NIL;
	List	   *result = NIL;

	if (stmt->rels == NIL)
	{
		Relation	pgclass = table_open(RelationRelationId, AccessShareLock);
		TableScanDesc scan = table_beginscan_catalog(pgclass, 0, NULL);
		HeapTuple	tuple;

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

			if (form->relkind == RELKIND_RELATION ||
				form->relkind == RELKIND_MATVIEW)
				candidates = lappend_oid(candidates, form->oid);
		}
		table_endscan(scan);
		table_close(pgclass, AccessShareLock);
	}
	else
	{
		foreach_node(VacuumRelation, vrel, stmt->rels)
		{
			Oid			relid = OidIsValid(vrel->oid) ? vrel->oid
				: RangeVarGetRelid(vrel->relation, NoLock, true);

			if (!OidIsValid(relid))
				continue;
			if (!OidIsValid(vrel->oid) && vrel->relation->inh)
				candidates = list_concat(candidates,
										 find_all_inheritors(relid, NoLock, NULL));
			else
				candidates = lappend_oid(candidates, relid);
		}
	}

	foreach_oid(relid, candidates)
	{
		if ((get_rel_relkind(relid) == RELKIND_RELATION ||
			 get_rel_relkind(relid) == RELKIND_MATVIEW) &&
			pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK &&
			GpScanDistributedPolicy(relid) != NULL)
			result = list_append_unique_oid(result, relid);
	}
	return result;
}

static void
note_relation(HTAB *counts, List **order, Oid relid, Oid table, StringInfo oids)
{
	bool		found;
	SegmentCounts *c = hash_search(counts, &relid, HASH_ENTER, &found);

	if (found)
		return;
	memset(c, 0, sizeof(SegmentCounts));
	c->relid = relid;
	c->table = table;
	*order = lappend_oid(*order, relid);
	appendStringInfo(oids, "%s%u", oids->len > 0 ? "," : "", relid);
}

/*
 * The pages and rows pg_class has for a relation now.  Read from the catalog
 * rather than the caches: ANALYZE has just updated them in place.
 */
static void
current_counts(Oid relid, BlockNumber *pages, double *tuples)
{
	Relation	pgclass = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	*pages = 0;
	*tuples = -1;
	ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(pgclass, ClassOidIndexId, true, NULL, 1, &key);
	if ((tuple = systable_getnext(scan)) != NULL)
	{
		*pages = (BlockNumber) Max(((Form_pg_class) GETSTRUCT(tuple))->relpages, 0);
		*tuples = ((Form_pg_class) GETSTRUCT(tuple))->reltuples;
	}
	systable_endscan(scan);
	table_close(pgclass, AccessShareLock);
}

/*
 * After a VACUUM or ANALYZE of distributed tables on the coordinator: what
 * the segments count of them, brought back to its pg_class, as Cloudberry's
 * vac_update_relstats_from_list() and AcquireNumberOfAllVisibleBlocks() bring
 * it back (vacuum.c, analyze.c).  The coordinator's copy of such a table is
 * empty, so its own VACUUM counts no pages, no rows and nothing all-visible,
 * and its own ANALYZE nothing all-visible -- the pages and rows ANALYZE
 * writes are the segments', through O3.  ORCA then costs an index-only scan
 * as though every row had to be fetched, and after a plain VACUUM both
 * planners read a table of rows as an empty one until the next ANALYZE.
 *
 * After a VACUUM: the pages, rows, all-visible and all-frozen pages of each
 * table, and the pages and rows of its indexes.  After an ANALYZE: the
 * all-visible and all-frozen pages alone.  Summed over the segments -- a
 * replicated table's over one segment's worth -- and written only when every
 * segment answered, as Cloudberry's are.  A pg_class row is written in place,
 * which PostgreSQL 19 allows under ShareUpdateExclusiveLock on the table, as
 * VACUUM and ANALYZE hold it: taken table by table in OID order, so that two
 * of these never wait for each other.
 */
void
GpAnalyzeSegmentCounts(VacuumStmt *stmt)
{
	bool		vacuumed = stmt->is_vacuumcmd;
	List	   *tables;
	List	   *order = NIL;
	HASHCTL		ctl;
	HTAB	   *counts;
	SegmentCounts *c;
	StringInfoData oids;
	char	  **values;
	int			nsegs;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	foreach_node(DefElem, opt, stmt->options)
	{
		/* VACUUM (ONLY_DATABASE_STATS) takes no relation */
		if (strcmp(opt->defname, "only_database_stats") == 0 && defGetBoolean(opt))
			return;
	}
	tables = distributed_relids(stmt);
	if (tables == NIL)
		return;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SegmentCounts);
	ctl.hcxt = CurrentMemoryContext;
	counts = hash_create("gp_core segment counts", list_length(tables) * 2,
						 &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	initStringInfo(&oids);
	list_sort(tables, list_oid_cmp);
	foreach_oid(table, tables)
	{
		note_relation(counts, &order, table, table, &oids);
		if (vacuumed)
		{
			Relation	rel = try_relation_open(table, AccessShareLock);

			if (rel == NULL)
				continue;
			foreach_oid(index, RelationGetIndexList(rel))
				note_relation(counts, &order, index, table, &oids);
			relation_close(rel, AccessShareLock);
		}
	}

	GpClusterSegments(&nsegs);
	values = palloc0_array(char *, Max(nsegs, 1));
	GpDispatchQueryFirstValues(psprintf("SELECT pg_catalog.string_agg(oid || ' ' || relpages || ' ' ||"
										" reltuples || ' ' || relallvisible || ' ' || relallfrozen, ',')"
										"  FROM pg_catalog.pg_class WHERE oid IN (%s)",
										oids.data),
							   -1, values);
	for (int i = 0; i < nsegs; i++)
	{
		char	   *save = NULL;

		if (values[i] == NULL)
			continue;
		for (char *tok = strtok_r(values[i], ",", &save); tok != NULL;
			 tok = strtok_r(NULL, ",", &save))
		{
			Oid			relid;
			double		pages,
						tuples,
						allvisible,
						allfrozen;

			if (sscanf(tok, "%u %lf %lf %lf %lf", &relid, &pages, &tuples,
					   &allvisible, &allfrozen) != 5 ||
				(c = hash_search(counts, &relid, HASH_FIND, NULL)) == NULL)
				continue;
			c->pages += pages;
			c->tuples += Max(tuples, 0);
			c->allvisible += allvisible;
			c->allfrozen += allfrozen;
			c->nsegs++;
		}
	}

	foreach_oid(relid, order)
	{
		GpPolicy   *policy;
		double		share;
		Relation	rel;
		BlockNumber pages;
		double		tuples;
		BlockNumber allvisible;
		BlockNumber allfrozen;

		c = hash_search(counts, &relid, HASH_FIND, NULL);
		if (c->relid == c->table)
			LockRelationOid(c->table, ShareUpdateExclusiveLock);
		if (c->nsegs < nsegs ||
			(policy = GpScanDistributedPolicy(c->table)) == NULL ||
			(rel = try_relation_open(c->relid, AccessShareLock)) == NULL)
			continue;
		share = GpPolicyIsReplicated(policy) ? Max(policy->numsegments, 1) : 1;

		if (vacuumed)
		{
			pages = (BlockNumber) (c->pages / share);
			tuples = c->tuples / share;
		}
		else
			current_counts(c->relid, &pages, &tuples);

		/*
		 * A table that has no rows has a page, as Cloudberry's
		 * vac_update_relstats() gives one to every relation it counts none
		 * of, so that "analyzed, and empty" is not taken for "never
		 * analyzed" (gp_partanalyze.c).
		 */
		if (pages < 1 && tuples >= 0)
			pages = 1;
		allvisible = (BlockNumber) Min(c->allvisible / share, (double) pages);
		allfrozen = (BlockNumber) Min(c->allfrozen / share, (double) allvisible);

		vac_update_relstats(rel, pages, tuples, allvisible, allfrozen,
							rel->rd_rel->relhasindex,
							InvalidTransactionId, InvalidMultiXactId,
							NULL, NULL, true);
		relation_close(rel, AccessShareLock);
	}
	hash_destroy(counts);
}

void
GpAnalyzeInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = gp_analyze_sample_rows;
}
