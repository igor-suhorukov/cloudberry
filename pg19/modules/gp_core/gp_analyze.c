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
 * gp_internal.sample_rows(), and writes what it counted into its own
 * pg_class, as Cloudberry's segments do.  A segment is asked for the target
 * over the number of segments, as Cloudberry asks, so that the rows sent are
 * the target and not as many times it as there are segments.  Cloudberry
 * keeps every row sent, which weighs a segment with more rows than the
 * others as light as the rest; the coordinator here takes from each
 * segment's sample in proportion to how many rows the segment has, chosen at
 * random, so that the whole is a sample of the table -- and a segment whose
 * share is well above what it sent is asked once more, for its share
 * (gather_sample()).  In a database where gp_core's functions are not
 * installed, the coordinator samples the gathered rows itself -- the same
 * answer, at the cost of reading every row.
 *
 * A value over a kilobyte stays on its segment, as Cloudberry's does, which
 * sends NULL and the length: PostgreSQL's standard statistics count such a
 * value and its width and look at nothing else of it, so a sample of a
 * table of documents sent them whole for nothing, and held them here.  Only
 * of a column nothing else reads -- not one a type's own analysis has, an
 * array's or a geometry's, nor one an index expression, a partial index's
 * predicate or an extended statistics object takes (wide_columns()).  In
 * the value's place the sample here has what PostgreSQL's own sample of a
 * value stored out of line has, a pointer to it that says its length; the
 * pointer names no table, and nothing follows one of a value so long.  The
 * column's average width is then written as it was with the values here
 * (note_widths()): of the values as the coordinator would hold them, which
 * is what it holds of a row a query gathers.
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

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "access/visibilitymap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_func.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "utils/acl.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "varatt.h"
#include "utils/relcache.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_partanalyze.h"
#include "gp_policy.h"
#include "gp_scan.h"

static analyze_sample_rows_hook_type prev_analyze_sample_rows = NULL;

/*
 * The width over which PostgreSQL's standard statistics leave a value out,
 * and a sample leaves it on its segment: analyze.c's.
 */
#define WIDTH_THRESHOLD  1024

/*
 * The columns of a table whose values over WIDTH_THRESHOLD a sample may
 * leave on the segments: those nothing reads the values of.  PostgreSQL's
 * standard statistics count such a value and add its width, and compare
 * the others (compute_scalar_stats(), compute_distinct_stats(),
 * compute_trivial_stats()); a type's own analysis reads what it likes of
 * every value -- an array's elements up to 64 kB, a tsvector's lexemes,
 * a geometry's bounding box -- and so does the expression of an index, the
 * predicate of a partial one, and an extended statistics object, over each
 * sampled row.  A coordinator and its segments have the same types, indexes
 * and statistics objects, and so answer alike: the segment decides what it
 * sends, and the coordinator checks what comes.
 */
static Bitmapset *
wide_columns(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *used = NULL;	/* offset, as pull_varattnos() has them */
	Bitmapset  *result = NULL;

	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		Relation	index = index_open(indexoid, AccessShareLock);

		pull_varattnos((Node *) RelationGetIndexExpressions(index), 1, &used);
		pull_varattnos((Node *) RelationGetIndexPredicate(index), 1, &used);
		index_close(index, AccessShareLock);
	}
	foreach_oid(statoid, RelationGetStatExtList(rel))
	{
		HeapTuple	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statoid));
		Form_pg_statistic_ext form;
		Datum		exprs;
		bool		isnull;

		if (!HeapTupleIsValid(tuple))
			continue;
		form = (Form_pg_statistic_ext) GETSTRUCT(tuple);
		for (int i = 0; i < form->stxkeys.dim1; i++)
			used = bms_add_member(used, form->stxkeys.values[i] -
								  FirstLowInvalidHeapAttributeNumber);
		exprs = SysCacheGetAttr(STATEXTOID, tuple,
								Anum_pg_statistic_ext_stxexprs, &isnull);
		if (!isnull)
		{
			char	   *str = TextDatumGetCString(exprs);

			pull_varattnos((Node *) stringToNode(str), 1, &used);
			pfree(str);
		}
		ReleaseSysCache(tuple);
	}

	/* a whole row reads every column */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, used))
		return NULL;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		HeapTuple	type;
		bool		standard;

		if (att->attisdropped || att->attlen != -1 ||
			bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber, used))
			continue;
		type = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(type))
			continue;
		standard = !OidIsValid(((Form_pg_type) GETSTRUCT(type))->typanalyze);
		ReleaseSysCache(type);
		if (standard)
			result = bms_add_member(result, att->attnum);
	}
	return result;
}

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
 * stream, the table AM's analyze scan and Vitter's reservoir -- and saying
 * at elevel what it scanned, in its words.
 */
static int
segment_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
					double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;
	double		samplerows = 0;
	double		liverows = 0;
	double		deadrows = 0;
	double		rowstoskip = -1;
	BlockNumber totalblocks = RelationGetNumberOfBlocks(rel);
	BlockNumber nblocks;
	BlockNumber blksdone = 0;
	BlockSamplerData bs;
	ReservoirStateData rstate;
	TupleTableSlot *slot;
	TableScanDesc scan;
	ReadStream *stream;

	nblocks = BlockSampler_Init(&bs, totalblocks, targrows,
								pg_prng_uint32(&pg_global_prng_state));
	reservoir_init_selection_state(&rstate, targrows);

	/* the blocks to sample, as acquire_sample_rows() reports them */
	pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_TOTAL, nblocks);

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

		/*
		 * and each one sampled; Cloudberry's fault is here, after each block
		 * (acquire_sample_rows())
		 */
		pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_DONE, ++blksdone);
		(void) GP_FAULT("analyze_block");
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

	ereport(elevel,
			(errmsg("\"%s\": scanned %d of %u pages, "
					"containing %.0f live rows and %.0f dead rows; "
					"%d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(rel),
					bs.m, totalblocks,
					liverows, deadrows,
					numrows, *totalrows)));
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
	return segment_sample_rows(rel, DEBUG1, rows, targrows, totalrows, totaldeadrows);
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

/*
 * The rows and counts of a segment's sample, as the SQL functions give them:
 * the counts, then each row.  A row's values that stay here (wide_columns())
 * are NULL in it, and each has a row of its own after the row's: the
 * column's number and the value's length as it would have gone, in the two
 * columns of the counts.  "whole": every value goes.
 */
static void
put_sample(ReturnSetInfo *rsinfo, Relation rel, HeapTuple *rows,
		   int numrows, double totalrows, double totaldeadrows, bool whole)
{
	TupleDesc	rowdesc = RelationGetDescr(rel);
	Bitmapset  *wide = whole ? NULL : wide_columns(rel);
	Datum	   *rowvalues = palloc_array(Datum, rowdesc->natts);
	bool	   *rownulls = palloc_array(bool, rowdesc->natts);
	int		   *left = palloc_array(int, rowdesc->natts);
	Size	   *leftlen = palloc_array(Size, rowdesc->natts);
	Datum		values[3];
	bool		nulls[3];

	values[0] = Float8GetDatum(totalrows);
	values[1] = Float8GetDatum(totaldeadrows);
	values[2] = (Datum) 0;
	nulls[0] = nulls[1] = false;
	nulls[2] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	for (int i = 0; i < numrows; i++)
	{
		HeapTuple	row = rows[i];
		int			nleft = 0;

		if (wide != NULL)
		{
			int			attnum = -1;

			heap_deform_tuple(row, rowdesc, rowvalues, rownulls);
			while ((attnum = bms_next_member(wide, attnum)) >= 0)
			{
				Size		len;

				if (rownulls[attnum - 1])
					continue;
				len = toast_raw_datum_size(rowvalues[attnum - 1]);
				if (len <= WIDTH_THRESHOLD)
					continue;
				rownulls[attnum - 1] = true;
				left[nleft] = attnum;
				leftlen[nleft++] = len;
			}
			if (nleft > 0)
				row = heap_form_tuple(rowdesc, rowvalues, rownulls);
		}

		values[2] = heap_copy_tuple_as_datum(row, rowdesc);
		nulls[0] = nulls[1] = true;
		nulls[2] = false;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		pfree(DatumGetPointer(values[2]));
		if (row != rows[i])
			heap_freetuple(row);

		nulls[0] = nulls[1] = false;
		nulls[2] = true;
		for (int k = 0; k < nleft; k++)
		{
			values[0] = Float8GetDatum(left[k]);
			values[1] = Float8GetDatum(leftlen[k]);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}

		/* Cloudberry's fault at each row gp_acquire_sample_rows() returns */
		(void) GP_FAULT("returned_sample_row");
	}
	pfree(rowvalues);
	pfree(rownulls);
	pfree(left);
	pfree(leftlen);
}

/*
 * What this segment's sample counted of a table, into the segment's own
 * pg_class, as Cloudberry's gp_acquire_sample_rows() leaves it: its
 * do_analyze_rel() on the segment writes the segment's pages, rows and
 * all-visible pages, and its indexes' pages and rows (analyze.c), as
 * PostgreSQL's ANALYZE does on a node of its own.  A segment's VACUUM counts
 * the rows of the pages it scans alone -- a page or none, where a read has
 * made the others all-visible, as PostgreSQL 19's pruning does -- and keeps
 * the count it had for the rest (vac_estimate_reltuples()): with none, "never
 * counted", the coordinator could bring back no rows after it.
 *
 * Written in place, under the lock PostgreSQL 19 asks of that,
 * ShareUpdateExclusiveLock on the table, and only if nothing holds it: a
 * VACUUM or ANALYZE of the segment's own that does counts the table itself.
 * For a user who may ANALYZE the table, where the sample is anyone's who may
 * read it.  A partial index keeps its rows, which PostgreSQL counts by its
 * predicate over the sample.
 */
static void
own_counts(Relation rel, BlockNumber totalpages, double totalrows)
{
	Oid			relid = RelationGetRelid(rel);
	BlockNumber allvisible = 0;
	BlockNumber allfrozen = 0;

	if (GpClusterBackendRole() != GP_ROLE_EXECUTE ||
		pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK ||
		!ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock))
		return;

	if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		visibilitymap_count(rel, &allvisible, &allfrozen);
	vac_update_relstats(rel, totalpages, totalrows, allvisible, allfrozen,
						rel->rd_rel->relhasindex, InvalidTransactionId,
						InvalidMultiXactId, NULL, NULL, true);

	foreach_oid(indexoid, RelationGetIndexList(rel))
	{
		Relation	index = index_open(indexoid, AccessShareLock);

		vac_update_relstats(index, RelationGetNumberOfBlocks(index),
							RelationGetIndexPredicate(index) != NIL
							? index->rd_rel->reltuples : totalrows,
							0, 0, false, InvalidTransactionId,
							InvalidMultiXactId, NULL, NULL, true);
		index_close(index, AccessShareLock);
	}
	UnlockRelationOid(relid, ShareUpdateExclusiveLock);
}

/*
 * A segment's sample as the progress of an ANALYZE of the table, as
 * Cloudberry's segment reports it as its gp_acquire_sample_rows() runs
 * acquire_sample_rows(): pg_stat_progress_analyze's row, which
 * gp_stat_progress_analyze gathers from the segments -- acquiring sample
 * rows, or inherited ones for a tree.  Not where this backend reports a
 * command of its own already.
 */
static bool
sample_progress_begin(Oid relid, bool tree)
{
	if (MyBEEntry == NULL ||
		MyBEEntry->st_progress_command != PROGRESS_COMMAND_INVALID)
		return false;
	pgstat_progress_start_command(PROGRESS_COMMAND_ANALYZE, relid);
	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 tree ? PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH
								 : PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS);
	return true;
}

PG_FUNCTION_INFO_V1(gp_sample_rows);

/*
 * gp_internal.sample_rows(NULL::t, targrows)
 *		This segment's sample of a table, for the coordinator's ANALYZE.
 *
 * The first row says how many rows the segment holds, live and dead, as
 * ANALYZE estimates them; every row after it is a sampled row, as a value of
 * the table's own row type, or says of the row before it a value that was
 * not sent (put_sample()) -- none with "whole".  What it counted is the
 * segment's pg_class's too (own_counts()).
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

	bool		progress;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	rel = table_open(relid, AccessShareLock);
	progress = sample_progress_begin(relid, false);
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	func = local_sampler(rel, &totalpages);
	numrows = local_sample_rows(rel, func, rows, targrows, &totalrows,
								&totaldeadrows);
	own_counts(rel, totalpages, totalrows);
	put_sample(rsinfo, rel, rows, numrows, totalrows, totaldeadrows,
			   PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2));
	table_close(rel, AccessShareLock);
	if (progress)
		pgstat_progress_end_command();
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
	double		sampledpages = 0;
	double		before = 0;		/* the pages of the members before this one */
	double		offset;
	HeapTuple  *rows;
	int			numrows = 0;
	double		totalrows = 0;
	double		totaldeadrows = 0;
	int			n = 0;

	bool		progress;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	parent = table_open(relid, AccessShareLock);
	progress = sample_progress_begin(relid, true);
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
	pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_TOTAL, n);
	offset = pg_prng_double(&pg_global_prng_state);
	for (int i = 0; i < n; i++)
	{
		int			childtargrows;
		int			childrows;
		double		trows,
					tdrows;

		/* each member, as acquire_inherited_sample_rows() reports it */
		pgstat_progress_update_param(PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID,
									 RelationGetRelid(rels[i]));
		if (pages[i] <= 0)
		{
			pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE, i + 1);
			continue;
		}

		/*
		 * A member's share of the target is its share of the pages, as
		 * acquire_inherited_sample_rows() gives it, but not each share
		 * rounded by itself: the target here is a segment's part of the
		 * coordinator's, and where it is less than half the members, every
		 * share rounds to no row and the tree has no sample.  The shares are
		 * cut out of the target one after another instead, from an offset
		 * chosen at random, so that they make the target exactly and a member
		 * with less than a row's share gets a row as often as its share says.
		 */
		childtargrows = (int) (floor(targrows * (before + pages[i]) / totalpages + offset) -
							   floor(targrows * before / totalpages + offset));
		before += pages[i];
		childtargrows = Min(childtargrows, targrows - numrows);
		if (childtargrows <= 0)
		{
			pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE, i + 1);
			continue;
		}
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
		sampledpages += pages[i];
		pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE, i + 1);
	}

	/*
	 * The members that got no row were not counted either: their pages are
	 * taken to hold rows as densely as the pages of those that were, as
	 * vac_estimate_reltuples() takes the pages a VACUUM did not scan.
	 */
	if (sampledpages > 0 && sampledpages < totalpages)
	{
		totalrows = floor(totalrows * totalpages / sampledpages + 0.5);
		totaldeadrows = floor(totaldeadrows * totalpages / sampledpages + 0.5);
	}

	put_sample(rsinfo, parent, rows, numrows, totalrows, totaldeadrows,
			   PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2));
	for (int i = 0; i < n; i++)
		table_close(rels[i], NoLock);
	table_close(parent, AccessShareLock);
	if (progress)
		pgstat_progress_end_command();
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* On the coordinator                                                        */
/* ------------------------------------------------------------------------- */

typedef struct SegmentSample
{
	double		totalrows;
	double		totaldeadrows;
	bool		counted;		/* it has said those: its answer's first row */
	int			asked;			/* the rows it was asked for */
	List	   *rows;			/* of HeapTuple */
} SegmentSample;

/*
 * How many times the rows a segment sent its share of the sample may be
 * before the segment is asked again.  Up to it the sample is cut to what the
 * segment sent, and so is no smaller than the target over this.
 */
#define SAMPLE_SHARE_TOLERANCE	1.25

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

/*
 * What stands in a sampled row for a value its segment did not send: a
 * pointer to a value stored out of line, of that length, as PostgreSQL's own
 * sample of a toasted value has -- which says the length and is never
 * followed, the value being over WIDTH_THRESHOLD.  It names no table.
 */
static bool
value_left(Datum value, Size *len)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(value);
	varatt_external pointer;

	if (!VARATT_IS_EXTERNAL_ONDISK(v))
		return false;
	VARATT_EXTERNAL_GET_POINTER(pointer, v);
	if (OidIsValid(pointer.va_toastrelid))
		return false;
	*len = pointer.va_rawsize;
	return true;
}

/* The row with that in a column's place, the segment's row freed. */
static HeapTuple
tuple_leaving(HeapTuple tuple, Relation rel, const Bitmapset *wide,
			  double attnum, double len)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = palloc_array(Datum, desc->natts);
	bool	   *nulls = palloc_array(bool, desc->natts);
	char		standin[TOAST_POINTER_SIZE];
	varatt_external pointer = {0};
	HeapTuple	result;

	/* a column the segment may leave values of, as this node finds them */
	if (attnum < 1 || attnum > desc->natts ||
		!bms_is_member((int) attnum, wide) ||
		len <= WIDTH_THRESHOLD || len > MaxAllocSize)
		elog(ERROR, "a segment's sample of \"%s\" left out a value of column %d, of length %.0f",
			 RelationGetRelationName(rel), (int) attnum, len);

	pointer.va_rawsize = (int32) len;
	pointer.va_extinfo = (uint32) len - VARHDRSZ;
	SET_VARTAG_EXTERNAL(standin, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(standin), &pointer, sizeof(pointer));

	heap_deform_tuple(tuple, desc, values, nulls);
	values[(int) attnum - 1] = PointerGetDatum(standin);
	nulls[(int) attnum - 1] = false;
	result = heap_form_tuple(desc, values, nulls);
	result->t_self = tuple->t_self;
	result->t_tableOid = tuple->t_tableOid;
	heap_freetuple(tuple);
	pfree(values);
	pfree(nulls);
	return result;
}

/*
 * The average width of each column whose sample has a value left on its
 * segment, as PostgreSQL's statistics would have it with the values here:
 * the bytes of every value over the values not NULL, a value left counted
 * at its length.  PostgreSQL counts the pointer; what it writes is written
 * over once it has (gp_partmerge.c).  "tree": the statistics are the
 * tree's.
 */
static void
note_widths(Relation rel, bool tree, const Bitmapset *wide, HeapTuple *rows,
			int numrows)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			attnum = -1;

	while ((attnum = bms_next_member(wide, attnum)) >= 0)
	{
		double		total = 0;
		int			nonnull = 0;
		bool		any = false;

		for (int i = 0; i < numrows; i++)
		{
			bool		isnull;
			Datum		value = heap_getattr(rows[i], attnum, desc, &isnull);
			Size		len;

			if (isnull)
				continue;
			nonnull++;
			if (value_left(value, &len))
			{
				total += len;
				any = true;
			}
			else
				total += VARSIZE_ANY(DatumGetPointer(value));
		}
		if (any)
			GpStatWidthLater(RelationGetRelid(rel), attnum, tree,
							 (int32) (total / (double) nonnull));
	}
}

/*
 * Does this database have gp_internal.sample_rows(), or sample_tree(), and
 * with how many arguments: three, or the two of a database gp_core was made
 * in before a sample could be asked for whole; 0 for neither.
 */
static int
have_sample_function(const char *name)
{
	Oid			argtypes[3] = {ANYELEMENTOID, INT4OID, BOOLOID};
	List	   *names;

	if (!OidIsValid(get_namespace_oid("gp_internal", true)))
		return 0;
	names = list_make2(makeString("gp_internal"), makeString(pstrdup(name)));
	if (OidIsValid(LookupFuncName(names, 3, argtypes, true)))
		return 3;
	return OidIsValid(LookupFuncName(names, 2, argtypes, true)) ? 2 : 0;
}

/*
 * The coordinator's sample: each segment's own, merged.  A segment
 * contributes in proportion to how many rows it has, the rows it contributes
 * chosen at random from its sample and kept in its order.  Where a segment
 * sent fewer rows than its share of the target, every segment's share is
 * cut by as much: a row is as likely to be in the sample on one segment as
 * on another, which is what PostgreSQL's estimators take a sample to be.
 * What is not chosen is freed.
 */
static int
merge_segment_samples(SegmentSample *samples, int nsegs, HeapTuple *rows,
					  int targrows, double *totalrows, double *totaldeadrows)
{
	double		total = 0;
	double		dead = 0;
	double		fraction;
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

	/* the part of its rows every segment can give */
	fraction = Min(1.0, targrows / total);
	for (int i = 0; i < nsegs; i++)
	{
		int			have = list_length(samples[i].rows);

		if (have > 0 && have < samples[i].totalrows * fraction)
			fraction = have / samples[i].totalrows;
	}

	for (int i = 0; i < nsegs; i++)
	{
		int			have = list_length(samples[i].rows);
		int			take = (int) floor(samples[i].totalrows * fraction + 0.5);
		HeapTuple  *from;
		bool	   *chosen;

		take = Min(take, have);
		take = Min(take, targrows - numrows);
		if (have <= 0)
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
		{
			if (chosen[k])
				rows[numrows++] = from[k];
			else
				heap_freetuple(from[k]);
		}
		pfree(from);
		pfree(chosen);
		list_free(samples[i].rows);
		samples[i].rows = NIL;
	}

	return numrows;
}

/*
 * One round of asking: the query sent to the table's segments, or to those
 * listed, and what each answers taken in place of what it sent before -- a
 * segment's second sample is drawn apart from its first, so the two are not
 * added.  Answers whether a segment left a value out of a row (put_sample()),
 * which is then in the row as value_left() finds it.
 */
static bool
ask_segments(const char *sql, TupleDesc tupdesc, Relation rel, int content,
			 const GpPolicy *policy, const int *again, int nagain,
			 const Bitmapset *wide, SegmentSample *samples)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	Oid			relid = RelationGetRelid(rel);
	GpGatherState *gather;
	bool		left = false;
	int			from;

	for (int i = 0; i < nagain; i++)
	{
		list_free_deep(samples[again[i]].rows);
		samples[again[i]].rows = NIL;
		samples[again[i]].counted = false;
	}

	gather = again != NULL
		? GpGatherStartOnContents(sql, tupdesc, again, nagain)
		: start_on_table(sql, tupdesc, content, policy);
	while (GpGatherNext(gather, slot, &from))
	{
		SegmentSample *s = &samples[from];

		slot_getallattrs(slot);
		if (!slot->tts_isnull[2])
			s->rows = lappend(s->rows, tuple_from_composite(slot->tts_values[2], relid));
		else if (slot->tts_isnull[0] || slot->tts_isnull[1])
			continue;
		else if (!s->counted)
		{
			s->totalrows = DatumGetFloat8(slot->tts_values[0]);
			s->totaldeadrows = DatumGetFloat8(slot->tts_values[1]);
			s->counted = true;
		}
		else if (s->rows != NIL)
		{
			/* of the row before it: a value its segment did not send */
			ListCell   *last = list_tail(s->rows);

			lfirst(last) = tuple_leaving((HeapTuple) lfirst(last), rel, wide,
										 DatumGetFloat8(slot->tts_values[0]),
										 DatumGetFloat8(slot->tts_values[1]));
			left = true;
		}
	}
	GpGatherEnd(gather);
	ExecDropSingleTupleTableSlot(slot);
	return left;
}

/*
 * A distributed table's sample, or with "tree" its inheritance tree's, as
 * one sample of the table's rows: each segment's own, merged.  Cloudberry's
 * acquire_sample_rows_dispatcher(), which says at elevel what it sends.
 *
 * Each segment is asked for the target over the number of segments, which
 * is what the merge takes of it where the segments hold as many rows each.
 * Where they do not, a segment with more than the others has a share above
 * what it sent.  Up to SAMPLE_SHARE_TOLERANCE times that, the merge cuts
 * the sample to fit; past it the segment is asked again, for its share, and
 * that sample is the one merged.  So a table spread evenly costs the rows of
 * one target and one round, where a target a segment was asked before; a
 * table that is not costs a second round, of the segments that hold most of
 * it; and neither is weighed by segment, as Cloudberry's sample is, whose
 * source says so (analyze.c, "the sample will biased, too").
 *
 * A segment that sent fewer rows than it was asked for and still has more
 * -- the pages it sampled were sparse, or a tree's members shared the
 * target by their pages and not their rows -- is asked for as many times
 * its share as it fell short.  No segment is asked for more than the
 * target, which is what each was asked for before.
 *
 * "whole": every value is to come, for a sample that is a part of another
 * PostgreSQL puts together, a member's of its tree's, whose widths could
 * not be put right here.
 */
static int
gather_sample(Relation rel, GpPolicy *policy, bool tree, bool whole, int elevel,
			  HeapTuple *rows, int targrows,
			  double *totalrows, double *totaldeadrows)
{
	Oid			relid = RelationGetRelid(rel);
	bool		replicated = GpPolicyIsReplicated(policy);
	int			content = replicated ? replicated_segment(policy) : -1;
	char	   *qualified = GpDispatchRelationName(RelationGetRelid(rel));
	const char *function = tree ? "sample_tree" : "sample_rows";
	int			nargs = have_sample_function(function);
	Bitmapset  *wide = NULL;
	bool		left = false;
	int			nsegs;
	SegmentSample *samples;
	char	   *sql;
	int			nagain = 0;
	int			numrows;

	GpClusterSegments(&nsegs);
	samples = palloc0_array(SegmentSample, nsegs);

	if (nargs > 0)
	{
		const char *all = whole && nargs > 2 ? ", true" : "";
		TupleDesc	tupdesc = CreateTemplateTupleDesc(3);
		int			nasked = replicated ? 1 : Max(policy->numsegments, 1);
		int			ask = Max((targrows + nasked - 1) / nasked, 1);
		int		   *again = palloc_array(int, nsegs);
		double		total = 0;
		StringInfoData targets;

		TupleDescInitEntry(tupdesc, 1, "totalrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "totaldeadrows", FLOAT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "sample", RelationGetForm(rel)->reltype, -1, 0);
		TupleDescFinalize(tupdesc);

		/* what a segment may leave out, unless it was asked for all and can be */
		if (all[0] == '\0')
			wide = wide_columns(rel);

		sql = psprintf("SELECT * FROM gp_internal.%s(NULL::%s, %d%s)",
					   function, qualified, ask, all);
		ereport(elevel, (errmsg("Executing SQL: %s", sql)));
		for (int i = 0; i < nsegs; i++)
			samples[i].asked = ask;
		left = ask_segments(sql, tupdesc, rel, content, policy, NULL, 0, wide,
							samples);

		/* The segments whose share is well above what they sent, again */
		initStringInfo(&targets);
		for (int i = 0; i < nsegs; i++)
			total += samples[i].totalrows;
		for (int i = 0; i < nsegs && total > 0; i++)
		{
			SegmentSample *s = &samples[i];
			int			have = list_length(s->rows);
			double		share = s->totalrows * Min(1.0, targrows / total);
			double		target = share;

			if (share <= have * SAMPLE_SHARE_TOLERANCE)
				continue;
			if (have < s->asked)
				target = share * s->asked / Max(have, 1);
			target = Min(ceil(target), targrows);
			if (target <= s->asked)
				continue;
			s->asked = (int) target;
			again[nagain++] = i;
			appendStringInfo(&targets, " WHEN %d THEN %d", i, s->asked);
		}
		if (nagain > 0)
		{
			sql = psprintf("SELECT * FROM gp_internal.%s(NULL::%s, CASE pg_catalog.gp_execution_segment()%s END%s)",
						   function, qualified, targets.data, all);
			ereport(elevel, (errmsg("Executing SQL: %s", sql)));
			if (ask_segments(sql, tupdesc, rel, -1, policy, again, nagain, wide,
							 samples))
				left = true;
		}
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
		TupleTableSlot *slot;
		GpGatherState *gather;

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

	numrows = merge_segment_samples(samples, nsegs, rows, targrows,
									totalrows, totaldeadrows);
	if (left && !whole)
		note_widths(rel, tree, wide, rows, numrows);
	ereport(DEBUG2,
			(errmsg_internal("\"%s\": %d rows in sample of a target of %d, %.0f estimated total rows, %d segments asked again",
							 RelationGetRelationName(rel), numrows, targrows,
							 *totalrows, nagain)));
	return numrows;
}

/*
 * What goes before the sample of a table ANALYZE analyzes itself: with
 * FULLSCAN, a leaf partition's counters of every row, and Cloudberry's
 * do_analyze_rel()'s look for the relation's inheritance tree, which says
 * it skips one there is none of -- and where the catalog said there was,
 * says so no more, as PostgreSQL's own look does, which then has nothing to
 * look for (Cloudberry's issue 14644).
 */
static List *
before_own_sample(Relation rel, int elevel)
{
	List	   *va_cols;
	List	   *fullscan = NIL;

	if (rel->rd_rel->relispartition && GpPartAnalyzeFullscan() &&
		GpPartAnalyzeTarget(RelationGetRelid(rel), &va_cols))
		fullscan = GpLeafFullScan(rel, va_cols, elevel);

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
	return fullscan;
}

/*
 * And after it: a leaf partition's counter of each column's distinct values,
 * which its root's statistics are merged with (gp_partmerge.c) -- of the
 * sample, or of the full scan, whose numbers of distinct values are the
 * leaf's own.
 */
static void
after_own_sample(Relation rel, List *fullscan, HeapTuple *rows, int numrows,
				 double totalrows)
{
	List	   *va_cols;

	if (fullscan != NIL)
		GpLeafFullScanNdistinct(rel, fullscan, rows, numrows, totalrows);
	else if (rel->rd_rel->relispartition)
	{
		(void) GpPartAnalyzeTarget(RelationGetRelid(rel), &va_cols);
		GpLeafSampleCounters(rel, rows, numrows, va_cols);
	}
}

/* The sampling function O3 hands ANALYZE for a distributed table analyzed itself. */
static int
distributed_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
						double *totalrows, double *totaldeadrows)
{
	List	   *fullscan = before_own_sample(rel, elevel);
	int			numrows;

	numrows = gather_sample(rel, GpScanDistributedPolicy(RelationGetRelid(rel)),
							false, false, elevel, rows, targrows, totalrows,
							totaldeadrows);
	after_own_sample(rel, fullscan, rows, numrows, *totalrows);
	return numrows;
}

/*
 * On one node, a leaf partition analyzed itself is sampled as PostgreSQL,
 * or the hook of its table access method, samples it -- whose function the
 * hook finds by asking the hooks outside gp_core's -- for its counters.
 */
static AnalyzeSampleRowsFunc local_leaf_inner = NULL;
static bool asking_outer_hooks = false;

static int
local_leaf_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
					   double *totalrows, double *totaldeadrows)
{
	List	   *fullscan = before_own_sample(rel, elevel);
	int			numrows;

	if (local_leaf_inner != NULL)
		numrows = local_leaf_inner(rel, elevel, rows, targrows, totalrows,
								   totaldeadrows);
	else
		numrows = segment_sample_rows(rel, elevel, rows, targrows, totalrows,
									  totaldeadrows);
	after_own_sample(rel, fullscan, rows, numrows, *totalrows);
	return numrows;
}

/* The same, for a member of a tree whose members are sampled one by one. */
static int
member_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
				   double *totalrows, double *totaldeadrows)
{
	return gather_sample(rel, GpScanDistributedPolicy(RelationGetRelid(rel)),
						 false, true, elevel, rows, targrows, totalrows,
						 totaldeadrows);
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
static List *walk_merged = NIL; /* a root's columns, merged in place of a sample */
static bool walk_merging = false;

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
	return gather_sample(rel, walk_policy, true, false, elevel, rows, targrows,
						 totalrows, totaldeadrows);
}

/*
 * A root's sampling function where every column it takes is merged from its
 * leaves' statistics: no rows, and the leaves' rows as the root's.
 */
static int
merged_sample_rows(Relation rel, int elevel, HeapTuple *rows, int targrows,
				   double *totalrows, double *totaldeadrows)
{
	*totalrows = GpRootMerge(rel, walk_merged);
	*totaldeadrows = 0;
	return 0;
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
 * as none, which the planner takes for a table never analyzed.  And an
 * empty table has one, which is what Cloudberry's vac_update_relstats()
 * writes of it: so its root's merge, later in the same statement, tells it
 * analyzed from never analyzed (gp_partanalyze.c).
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

	*totalpages = (BlockNumber) Min(Max(ceil(bytes / BLCKSZ), 1), (double) MaxBlockNumber);
	*func = sampler;
	return true;
}

/*
 * On one node: a leaf partition's pages and sampling function, as above; its
 * pages as its table access method counts them, and one for an empty one,
 * as a distributed table's are.
 */
static bool
local_leaf(Relation relation, AnalyzeSampleRowsFunc *func, BlockNumber *totalpages)
{
	BlockNumber pages = 0;
	bool		found;

	if (!relation->rd_rel->relispartition ||
		relation->rd_rel->relkind != RELKIND_RELATION)
		return prev_analyze_sample_rows
			? prev_analyze_sample_rows(relation, func, totalpages) : false;

	asking_outer_hooks = true;
	PG_TRY();
	{
		found = analyze_sample_rows_hook(relation, &local_leaf_inner, &pages);
	}
	PG_FINALLY();
	{
		asking_outer_hooks = false;
	}
	PG_END_TRY();
	if (!found)
	{
		local_leaf_inner = NULL;
		pages = RelationGetNumberOfBlocks(relation);
	}
	*totalpages = Max(pages, 1);
	*func = local_leaf_sample_rows;
	return true;
}

/* Has this single node's database gp_core?  Asked once a transaction. */
static bool
single_node_active(void)
{
	static LocalTransactionId asked = InvalidLocalTransactionId;
	static bool active = false;

	if (asked != MyProc->vxid.lxid)
	{
		active = GpPartAnalyzeActive();
		asked = MyProc->vxid.lxid;
	}
	return active;
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
	bool		single = GpClusterIsSingleNode();

	/*
	 * A cluster's coordinator; and a single node where gp_core is made,
	 * whose ANALYZE is Cloudberry's for partitioned tables alone
	 * (gp_partanalyze.c) -- PostgreSQL's tests run where it is not.
	 */
	if (asking_outer_hooks || AmAutoVacuumWorkerProcess() ||
		(single ? !single_node_active()
		 : GpClusterBackendRole() != GP_ROLE_DISPATCH))
		return prev_analyze_sample_rows
			? prev_analyze_sample_rows(relation, func, totalpages) : false;

	/* A member of the tree being walked, and those skipped before it gone */
	if (walk_lxid == lxid && list_member_oid(walk_members, relid))
	{
		while (linitial_oid(walk_members) != relid)
			walk_members = list_delete_first(walk_members);
		walk_members = list_delete_first(walk_members);
		if (walk_policy != NULL || walk_merging)
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
		List	   *va_cols;

		last_asked = InvalidOid;
		walk_policy = tree_policy(members);
		walk_members = list_delete_first(members);
		walk_lxid = lxid;
		walk_merging = false;
		MemoryContextSwitchTo(old);

		/*
		 * A root whose columns can be merged from its leaves' statistics,
		 * as Cloudberry's std_typanalyze() finds them: merged in place of a
		 * sample where every one can; where some can, merged over
		 * PostgreSQL's once PostgreSQL has written the sample's.
		 */
		if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
			!relation->rd_rel->relispartition &&
			GpPartAnalyzeTarget(relid, &va_cols))
		{
			bool		all;
			List	   *mergeable = GpRootMergeableColumns(relation, va_cols,
														   GpPartAnalyzeVerbose() ? INFO : DEBUG2,
														   &all);

			if (all)
			{
				old = MemoryContextSwitchTo(TopTransactionContext);
				walk_merged = list_copy(mergeable);
				walk_merging = true;
				MemoryContextSwitchTo(old);
				*func = merged_sample_rows;
				*totalpages = 1;
				return true;
			}
			if (mergeable != NIL)
				GpRootMergeLater(relid, mergeable);
		}

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
	if (single)
		return local_leaf(relation, func, totalpages);
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
	double		tuples;			/* of the segments that have counted them */
	double		counted_pages;	/* those segments' pages */
	double		allvisible;
	double		allfrozen;
	double		bytes;			/* an index's files, after an ANALYZE */
	int			nsegs;
	bool		counted;		/* a segment has counted its rows */
} SegmentCounts;

/*
 * A relation's rows over the segments: those the segments counted, and for
 * a segment that has not -- its VACUUM scanned too little of it to count
 * anew, and nothing had counted it before -- as many to a page as the
 * others hold, as vac_estimate_reltuples() takes the pages it did not scan
 * to hold rows as densely as it knew.  A replicated table's ANALYZE samples
 * one of its segments.  -1, not known, where no segment has counted, or
 * where those that have have no pages to say how densely.
 */
static double
segment_rows(const SegmentCounts *c)
{
	double		uncounted = c->pages - c->counted_pages;

	if (!c->counted)
		return -1;
	if (uncounted <= 0)
		return c->tuples;
	if (c->counted_pages <= 0)
		return -1;
	return c->tuples + floor(uncounted * c->tuples / c->counted_pages + 0.5);
}

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
 * all-visible and all-frozen pages, and the pages of its indexes, which the
 * coordinator's ANALYZE counts of its empty copy -- from the size of their
 * files on the segments, as Cloudberry's acquire_index_number_of_blocks()
 * counts them.  Summed over the segments -- a replicated table's over one
 * segment's worth -- and written only when every segment answered, as
 * Cloudberry's are.  A pg_class row is written in place, which PostgreSQL 19
 * allows under ShareUpdateExclusiveLock on the table, as VACUUM and ANALYZE
 * hold it: taken table by table in OID order, so that two of these never
 * wait for each other.
 */
static void segment_counts(List *tables, bool vacuumed, bool built,
						   bool skip_locked);

void
GpAnalyzeSegmentCounts(VacuumStmt *stmt)
{
	List	   *tables;
	bool		skip_locked = false;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	foreach_node(DefElem, opt, stmt->options)
	{
		/* VACUUM (ONLY_DATABASE_STATS) takes no relation */
		if (strcmp(opt->defname, "only_database_stats") == 0 && defGetBoolean(opt))
			return;
		if (strcmp(opt->defname, "skip_locked") == 0)
			skip_locked = defGetBoolean(opt);
	}
	tables = distributed_relids(stmt);
	if (tables == NIL)
		return;
	segment_counts(tables, stmt->is_vacuumcmd, false, skip_locked);
}

/*
 * A table's policy as its label records it, where it is distributed:
 * unchecked, since a statement that has just changed a key column's type is
 * followed by gp_sql's own change of the policy (GpDistributionAlterTableDone()),
 * which has not run yet -- the label names a column whose new type may have
 * no hash class, which GpPolicyGet() refuses.
 */
static GpPolicy *
recorded_policy(Oid relid)
{
	GpPolicy   *policy;

	if (GpClusterIsSingleNode())
		return NULL;
	policy = GpPolicyGetRecorded(relid);
	if (policy == NULL || GpPolicyIsEntry(policy))
		return NULL;
	return policy;
}

/*
 * The distributed tables of "candidates", and of a partitioned table among
 * them its leaves, that the user may maintain.
 */
static List *
distributed_of(List *candidates)
{
	List	   *leaves = NIL;
	List	   *result = NIL;

	foreach_oid(relid, candidates)
	{
		if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
			leaves = list_concat(leaves, find_all_inheritors(relid, NoLock, NULL));
		else
			leaves = lappend_oid(leaves, relid);
	}
	foreach_oid(relid, leaves)
	{
		char		relkind = get_rel_relkind(relid);

		if ((relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW) &&
			pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK &&
			recorded_policy(relid) != NULL)
			result = list_append_unique_oid(result, relid);
	}
	return result;
}

/* The tables and materialized views of a namespace, or of the database. */
static List *
tables_in(Oid nspid)
{
	Relation	pgclass = table_open(RelationRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(pgclass, 0, NULL);
	HeapTuple	tuple;
	List	   *result = NIL;

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

		if ((form->relkind == RELKIND_RELATION ||
			 form->relkind == RELKIND_MATVIEW) &&
			(!OidIsValid(nspid) || form->relnamespace == nspid))
			result = lappend_oid(result, form->oid);
	}
	table_endscan(scan);
	table_close(pgclass, AccessShareLock);
	return result;
}

/*
 * After a statement that builds an index of a distributed table on the
 * coordinator -- CREATE INDEX, REINDEX, CLUSTER and REPACK, and ALTER TABLE, which adds
 * constraints with indexes and rewrites tables: the build counted the
 * coordinator's copy of the table, which is empty, and wrote no pages and no
 * rows into the table's pg_class and the index's (index_update_stats()),
 * where Cloudberry's coordinator writes none (its index.c).  Both planners
 * then read a table ANALYZE had counted as an empty one: a join of it a
 * nested loop over rows the gathers bring again for each outer row --
 * minutes, for PostgreSQL's join test over tenk1, whose indexes are made
 * after it is analyzed.  What the segments count, which their own builds
 * wrote, is brought back instead, as after a VACUUM: the tables' pages,
 * rows and all-visible pages, and their indexes'.
 */
void
GpAnalyzeSegmentCountsAfterBuild(Node *stmt)
{
	List	   *candidates = NIL;
	List	   *tables;
	Oid			relid;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;

	switch (nodeTag(stmt))
	{
		case T_IndexStmt:
			relid = RangeVarGetRelid(((IndexStmt *) stmt)->relation, NoLock, true);
			if (OidIsValid(relid))
				candidates = list_make1_oid(relid);
			break;
		case T_ReindexStmt:
			{
				ReindexStmt *r = (ReindexStmt *) stmt;

				if (r->kind == REINDEX_OBJECT_INDEX)
				{
					relid = RangeVarGetRelid(r->relation, NoLock, true);
					if (OidIsValid(relid) &&
						(get_rel_relkind(relid) == RELKIND_INDEX ||
						 get_rel_relkind(relid) == RELKIND_PARTITIONED_INDEX))
						candidates = list_make1_oid(IndexGetRelation(relid, false));
				}
				else if (r->kind == REINDEX_OBJECT_TABLE)
				{
					relid = RangeVarGetRelid(r->relation, NoLock, true);
					if (OidIsValid(relid))
						candidates = list_make1_oid(relid);
				}
				else if (r->kind == REINDEX_OBJECT_SCHEMA)
				{
					Oid			nspid = get_namespace_oid(r->name, true);

					if (OidIsValid(nspid))
						candidates = tables_in(nspid);
				}
				else
					candidates = tables_in(InvalidOid);
			}
			break;
		case T_RepackStmt:
			{
				VacuumRelation *vrel = ((RepackStmt *) stmt)->relation;

				if (vrel == NULL)
					candidates = tables_in(InvalidOid);
				else
				{
					relid = OidIsValid(vrel->oid) ? vrel->oid
						: RangeVarGetRelid(vrel->relation, NoLock, true);
					if (OidIsValid(relid))
						candidates = list_make1_oid(relid);
				}
			}
			break;
		case T_AlterTableStmt:
			if (((AlterTableStmt *) stmt)->objtype == OBJECT_TABLE ||
				((AlterTableStmt *) stmt)->objtype == OBJECT_MATVIEW)
			{
				relid = RangeVarGetRelid(((AlterTableStmt *) stmt)->relation, NoLock, true);
				if (OidIsValid(relid))
					candidates = list_make1_oid(relid);
			}
			break;
		default:
			break;
	}

	tables = distributed_of(candidates);
	if (tables != NIL)
		segment_counts(tables, true, true, false);
}

/*
 * "tables"' counts on the segments, into the coordinator's pg_class: after a
 * VACUUM ("vacuumed") their pages, rows, all-visible and all-frozen pages,
 * and their indexes' pages and rows; after an ANALYZE their all-visible and
 * all-frozen pages, and their indexes' pages.
 *
 * After a statement with SKIP_LOCKED, a table whose lock another session
 * holds is passed over, with its indexes, as the statement passed it over:
 * the counts are the statement's, and waiting here for the lock would hold
 * the statement that was not to wait -- an auto-stats ANALYZE after an
 * INSERT, which Cloudberry's ExecVacuum() runs with SKIP_LOCKED unless
 * gp_autostats_lock_wait says to wait (vacuum.c), as gp_settings.c's
 * issue_analyze() does (autostats_locking).
 */
static void
segment_counts(List *tables, bool vacuumed, bool built, bool skip_locked)
{
	List	   *skipped = NIL;
	List	   *order = NIL;
	HASHCTL		ctl;
	HTAB	   *counts;
	SegmentCounts *c;
	StringInfoData oids;
	char	  **values;
	int			nsegs;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SegmentCounts);
	ctl.hcxt = CurrentMemoryContext;
	counts = hash_create("gp_core segment counts", list_length(tables) * 2,
						 &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	initStringInfo(&oids);
	list_sort(tables, list_oid_cmp);
	foreach_oid(table, tables)
	{
		Relation	rel;

		note_relation(counts, &order, table, table, &oids);
		if ((rel = try_relation_open(table, AccessShareLock)) == NULL)
			continue;
		foreach_oid(index, RelationGetIndexList(rel))
			note_relation(counts, &order, index, table, &oids);
		relation_close(rel, AccessShareLock);
	}

	/*
	 * An index's files are measured after an ANALYZE alone: the statement
	 * holds ShareUpdateExclusiveLock on the table, so no other session's
	 * index build or REINDEX, whose lock on the index a segment's
	 * pg_relation_size() would wait for, runs beside it.
	 */
	GpClusterSegments(&nsegs);
	values = palloc0_array(char *, Max(nsegs, 1));
	GpDispatchQueryFirstValues(psprintf("SELECT pg_catalog.string_agg(oid || ' ' || relpages || ' ' ||"
										" reltuples || ' ' || relallvisible || ' ' || relallfrozen || ' ' ||"
										" %s, ',')"
										"  FROM pg_catalog.pg_class WHERE oid IN (%s)",
										vacuumed ? "0"
										: "CASE relkind WHEN 'i' THEN pg_catalog.pg_relation_size(oid) ELSE 0 END",
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
						allfrozen,
						bytes;

			if (sscanf(tok, "%u %lf %lf %lf %lf %lf", &relid, &pages, &tuples,
					   &allvisible, &allfrozen, &bytes) != 6 ||
				(c = hash_search(counts, &relid, HASH_FIND, NULL)) == NULL)
				continue;
			c->pages += pages;
			if (tuples >= 0)
			{
				c->tuples += tuples;
				c->counted_pages += pages;
				c->counted = true;
			}
			c->allvisible += allvisible;
			c->allfrozen += allfrozen;
			c->bytes += bytes;
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
		/*
		 * A statement that built an index holds ShareLock or more, under
		 * which PostgreSQL writes the row in place itself; asking for more
		 * would wait for another session's REINDEX, which holds ShareLock
		 * too, to end its transaction.
		 */
		if (c->relid == c->table &&
			!CheckRelationOidLockedByMe(c->table, ShareLock, true))
		{
			if (!skip_locked)
				LockRelationOid(c->table, ShareUpdateExclusiveLock);
			else if (!ConditionalLockRelationOid(c->table, ShareUpdateExclusiveLock))
				skipped = lappend_oid(skipped, c->table);
		}
		if (list_member_oid(skipped, c->table))
			continue;
		if (c->nsegs < nsegs ||
			(policy = recorded_policy(c->table)) == NULL)
			continue;

		/*
		 * After a statement that may have built an index or rewritten the
		 * table, only a count it emptied -- the coordinator's empty copy's --
		 * is the segments' to replace.  Rows counted before it, which a
		 * statement that rebuilt nothing leaves (ALTER TABLE ... ADD COLUMN),
		 * stay; and so does a count the segments have none to give for, as
		 * "never counted" would read as a table ANALYZE never saw
		 * (gp_partanalyze.c).
		 */
		if (built)
		{
			BlockNumber cur_pages;
			double		cur_tuples;

			current_counts(c->relid, &cur_pages, &cur_tuples);
			if (cur_tuples > 0 || (!c->counted && cur_tuples >= 0))
				continue;
		}

		/*
		 * An index another session holds -- a REINDEX's AccessExclusiveLock --
		 * is that session's to count: waiting for it here would hold this
		 * statement, and a CREATE INDEX beside a REINDEX of the table's other
		 * index would wait for the REINDEX's transaction.
		 */
		if (c->relid != c->table)
		{
			if (!ConditionalLockRelationOid(c->relid, AccessShareLock))
				continue;
			if ((rel = try_relation_open(c->relid, NoLock)) == NULL)
			{
				UnlockRelationOid(c->relid, AccessShareLock);
				continue;
			}
		}
		else if ((rel = try_relation_open(c->relid, AccessShareLock)) == NULL)
			continue;
		share = GpPolicyIsReplicated(policy) ? Max(policy->numsegments, 1) : 1;

		if (vacuumed)
		{
			/* a table no segment has counted yet is not known to be empty */
			pages = (BlockNumber) (c->pages / share);
			tuples = segment_rows(c);
			if (tuples > 0)
				tuples /= share;
		}
		else
		{
			current_counts(c->relid, &pages, &tuples);
			if (c->relid != c->table)
				pages = (BlockNumber) ceil(c->bytes / share / BLCKSZ);
		}

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

/* A table's counts as the coordinator's pg_class has them, kept. */
typedef struct KeptCounts
{
	Oid			relid;
	int32		pages;
	float4		tuples;
	int32		allvisible;
	int32		allfrozen;
} KeptCounts;

/* The tables an index build of this statement's reads the rows of. */
static List *
index_build_tables(Node *parsetree)
{
	RangeVar   *rv = NULL;
	Oid			relid;

	switch (nodeTag(parsetree))
	{
		case T_IndexStmt:
			rv = ((IndexStmt *) parsetree)->relation;
			break;
		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				if (stmt->kind == REINDEX_OBJECT_TABLE)
					rv = stmt->relation;
				else if (stmt->kind == REINDEX_OBJECT_INDEX && stmt->relation != NULL)
				{
					Oid			index = RangeVarGetRelid(stmt->relation, NoLock, true);

					if (!OidIsValid(index) ||
						(get_rel_relkind(index) != RELKIND_INDEX &&
						 get_rel_relkind(index) != RELKIND_PARTITIONED_INDEX))
						return NIL;
					relid = IndexGetRelation(index, true);
					return OidIsValid(relid)
						? find_all_inheritors(relid, NoLock, NULL) : NIL;
				}
			}
			break;
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
				bool		builds = false;

				foreach_node(AlterTableCmd, cmd, stmt->cmds)
				{
					if (cmd->subtype == AT_AddIndex ||
						cmd->subtype == AT_AddIndexConstraint)
						builds = true;
					else if (cmd->subtype == AT_AddConstraint &&
							 IsA(cmd->def, Constraint) &&
							 (((Constraint *) cmd->def)->contype == CONSTR_PRIMARY ||
							  ((Constraint *) cmd->def)->contype == CONSTR_UNIQUE ||
							  ((Constraint *) cmd->def)->contype == CONSTR_EXCLUSION))
						builds = true;
				}
				if (builds && stmt->objtype == OBJECT_TABLE)
					rv = stmt->relation;
			}
			break;
		default:
			break;
	}
	if (rv == NULL)
		return NIL;
	relid = RangeVarGetRelid(rv, NoLock, true);
	if (!OidIsValid(relid))
		return NIL;
	return find_all_inheritors(relid, NoLock, NULL);
}

/*
 * Before a statement that builds an index on the coordinator: the counts of
 * the distributed tables whose rows it indexes.  PostgreSQL's index build
 * writes the table's pages, rows and all-visible pages as it finds them
 * (index_update_stats()), and the coordinator's copy of a distributed table
 * is empty: a table ANALYZE had counted the rows of read as empty after a
 * CREATE INDEX, which ORCA plans as such -- a query of nested CTEs over one
 * took minutes to plan.  Cloudberry's coordinator writes none of them
 * (index.c, "Gp_role != GP_ROLE_DISPATCH").
 */
List *
GpAnalyzeKeepCounts(Node *parsetree)
{
	List	   *kept = NIL;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return NIL;
	foreach_oid(relid, index_build_tables(parsetree))
	{
		HeapTuple	tuple;
		Form_pg_class form;
		KeptCounts *k;

		if (GpScanDistributedPolicy(relid) == NULL)
			continue;
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			continue;
		form = (Form_pg_class) GETSTRUCT(tuple);
		if (form->relkind == RELKIND_RELATION || form->relkind == RELKIND_MATVIEW)
		{
			k = palloc_object(KeptCounts);
			k->relid = relid;
			k->pages = form->relpages;
			k->tuples = form->reltuples;
			k->allvisible = form->relallvisible;
			k->allfrozen = form->relallfrozen;
			kept = lappend(kept, k);
		}
		ReleaseSysCache(tuple);
	}
	return kept;
}

/* And after it: the counts kept, where the statement changed them. */
void
GpAnalyzeRestoreCounts(List *kept)
{
	foreach_ptr(KeptCounts, k, kept)
	{
		Relation	rel = try_relation_open(k->relid, AccessShareLock);
		BlockNumber pages;
		double		tuples;

		if (rel == NULL)
			continue;
		current_counts(k->relid, &pages, &tuples);
		if ((int32) pages != k->pages || (float4) tuples != k->tuples)
			vac_update_relstats(rel, (BlockNumber) Max(k->pages, 0), k->tuples,
								(BlockNumber) Max(k->allvisible, 0),
								(BlockNumber) Max(k->allfrozen, 0),
								true, InvalidTransactionId, InvalidMultiXactId,
								NULL, NULL, true);
		relation_close(rel, AccessShareLock);
	}
}

void
GpAnalyzeInit(void)
{
	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = gp_analyze_sample_rows;
}
