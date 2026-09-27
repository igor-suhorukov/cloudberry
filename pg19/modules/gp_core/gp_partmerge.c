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
 * gp_partmerge.c
 *	  A partitioned table's statistics merged from its leaves', as
 *	  Cloudberry's ANALYZE merges them.
 *
 * Cloudberry's ANALYZE of a root partitioned table samples none of its rows
 * where it can help it: once every leaf has statistics of a column, the
 * root's are made of the leaves' (merge_leaf_stats(), analyze.c) --
 *
 *   - the fraction of NULLs and the width, weighted by each leaf's rows;
 *   - the number of distinct values, from each leaf's HyperLogLog counter
 *     of the column merged into one (gp_hll.c), and the Haas-Stokes
 *     estimator over what the leaves' samples saw;
 *   - the most common values, the leaves' added up by count, those common
 *     enough across the whole kept (aggregate_leaf_partition_MCVs(),
 *     analyzeutils.c);
 *   - the histogram, the leaves' merged bucket by bucket with the values
 *     no longer common enough (aggregate_leaf_partition_histograms());
 *   - and no correlation.
 *
 * So a leaf's ANALYZE keeps a counter of each column: of the values of its
 * sample -- the NULLs and the values wider than 1 kB left out, as
 * Cloudberry's segments leave them out -- with what compute_scalar_stats()
 * or compute_distinct_stats() saw of them, how many distinct values and how
 * many seen more than once; or with ANALYZE FULLSCAN, of every row, by
 * gp_hyperloglog_accum().  Cloudberry keeps it in the last slot of the
 * leaf's pg_statistic row, under kinds 98 and 99, which are in PostgreSQL's
 * range of kinds and in the way of a type's own fifth; here it is a row of
 * gp_internal.leaf_hll, which goes with the pg_statistic row PostgreSQL
 * writes in the same transaction by that row's xmin -- a counter whose row
 * has been replaced since, by an ANALYZE that kept none or by
 * pg_restore_attribute_stats(), is not read.
 *
 * Where the root is merged: O3's hook, asked of the root for its tree
 * (gp_analyze.c), finds whether each column can be -- the column's own
 * type analyzed by PostgreSQL's standard functions, its "=" hashable, and
 * leaf_parts_analyzed() -- in examine_attribute()'s order, with Cloudberry's
 * words for why not.  Every column: the root's sampling function writes the
 * merged statistics and gives PostgreSQL no rows, with the leaves' rows
 * counted, so that PostgreSQL writes the root's pg_class and no statistics.
 * Some: PostgreSQL samples the tree, and the merged columns are written over
 * its statistics once they are written -- as the root's transaction
 * commits, or where the statement runs in one transaction, as it ends.
 * ANALYZE FULLSCAN's number of distinct values of a leaf is written over
 * PostgreSQL's alike.
 *
 * What differs from Cloudberry's: a value wider than 1 kB is left out of a
 * counter by its size, where Cloudberry's segments leave out one whose
 * stored, compressed, size is; the merged slots name the column's collation,
 * which Cloudberry leaves 0 -- PostgreSQL's planner uses a histogram only
 * of its query's collation; and the number of distinct values over the
 * segments, Cloudberry's kind 8, is not written, as it is not for a table.
 *
 * Cloudberry sources this file stands in for:
 *	  merge_leaf_stats() and acquire_hll_by_query() in
 *	  src/backend/commands/analyze.c, and src/backend/commands/analyzeutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "executor/spi.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "statistics/statistics.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"

#include "gp_fault.h"
#include "gp_hll.h"
#include "gp_partanalyze.h"

/* The width beyond which a value is left out of a sample's statistics */
#define WIDTH_THRESHOLD  1024

static object_access_hook_type prev_object_access_hook = NULL;

/* ------------------------------------------------------------------------- */
/* A leaf's counters                                                         */
/* ------------------------------------------------------------------------- */

/* gp_internal.leaf_hll, or InvalidOid in a database without gp_core. */
static Oid
counters_table(void)
{
	Oid			nsp = get_namespace_oid("gp_internal", true);

	return OidIsValid(nsp) ? get_relname_relid("leaf_hll", nsp) : InvalidOid;
}

/*
 * How Cloudberry's standard statistics analyze a column's type: with its
 * "<", compute_scalar_stats(); with only its "=", compute_distinct_stats();
 * and neither for a type of its own typanalyze but an array's, whose
 * array_typanalyze() calls the standard one first.  Those two are what keep
 * a leaf's counter.
 */
typedef enum ColumnStats
{
	COLUMN_STATS_NONE,
	COLUMN_STATS_DISTINCT,
	COLUMN_STATS_SCALAR,
} ColumnStats;

static ColumnStats
column_stats(Oid typid, Oid *ltopr, Oid *eqopr)
{
	HeapTuple	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	Oid			typanalyze;

	*ltopr = *eqopr = InvalidOid;
	if (!HeapTupleIsValid(tp))
		return COLUMN_STATS_NONE;
	typanalyze = ((Form_pg_type) GETSTRUCT(tp))->typanalyze;
	ReleaseSysCache(tp);
	if (OidIsValid(typanalyze) && typanalyze != F_ARRAY_TYPANALYZE)
		return COLUMN_STATS_NONE;

	get_sort_group_operators(typid, false, false, false, ltopr, eqopr, NULL, NULL);
	if (!OidIsValid(*eqopr))
		return COLUMN_STATS_NONE;
	return OidIsValid(*ltopr) ? COLUMN_STATS_SCALAR : COLUMN_STATS_DISTINCT;
}

/*
 * The columns of a relation a statement's ANALYZE takes -- those it names,
 * or each one PostgreSQL analyzes -- with the statistics target of each.
 */
static List *
analyzed_columns(Relation rel, List *va_cols, List **targets)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *attnums = NIL;

	*targets = NIL;
	if (va_cols != NIL)
	{
		foreach_node(String, col, va_cols)
		{
			AttrNumber	attnum = attnameAttNum(rel, strVal(col), false);
			int			target;

			if (attnum == InvalidAttrNumber || list_member_int(attnums, attnum) ||
				!attribute_is_analyzable(rel, attnum, TupleDescAttr(tupdesc, attnum - 1),
										 &target))
				continue;
			attnums = lappend_int(attnums, attnum);
			*targets = lappend_int(*targets, target < 0 ? default_statistics_target : target);
		}
		return attnums;
	}
	for (int i = 1; i <= tupdesc->natts; i++)
	{
		int			target;

		if (!attribute_is_analyzable(rel, i, TupleDescAttr(tupdesc, i - 1), &target))
			continue;
		attnums = lappend_int(attnums, i);
		*targets = lappend_int(*targets, target < 0 ? default_statistics_target : target);
	}
	return attnums;
}

/*
 * A counter written for a leaf's column, going with the pg_statistic row
 * this transaction writes: those of the column that go with no row there
 * is now, nor with the one about to be written, go.
 */
static void
store_counter(Oid leaf, AttrNumber attnum, bool fullscan, GpHLLCounter counter)
{
	Oid			relid = counters_table();
	TransactionId xid;
	TransactionId current = InvalidTransactionId;
	Relation	rel;
	Relation	statrel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		values[5];
	bool		nulls[5] = {0};

	if (!OidIsValid(relid))
		return;
	xid = GetCurrentTransactionId();

	/* the xmin of the statistics row there is now */
	statrel = table_open(StatisticRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_statistic_starelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(leaf));
	ScanKeyInit(&key[1], Anum_pg_statistic_staattnum, BTEqualStrategyNumber,
				F_INT2EQ, Int16GetDatum(attnum));
	ScanKeyInit(&key[2], Anum_pg_statistic_stainherit, BTEqualStrategyNumber,
				F_BOOLEQ, BoolGetDatum(false));
	scan = systable_beginscan(statrel, StatisticRelidAttnumInhIndexId, true,
							  NULL, 3, key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		current = HeapTupleHeaderGetRawXmin(tuple->t_data);
	systable_endscan(scan);
	table_close(statrel, AccessShareLock);

	rel = table_open(relid, RowExclusiveLock);
	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(leaf));
	ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(rel, linitial_oid(RelationGetIndexList(rel)), true,
							  NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		TransactionId staxmin = DatumGetTransactionId(heap_getattr(tuple, 3,
																   RelationGetDescr(rel),
																   &isnull));

		if (staxmin != current || staxmin == xid)
			CatalogTupleDelete(rel, &tuple->t_self);
	}
	systable_endscan(scan);

	values[0] = ObjectIdGetDatum(leaf);
	values[1] = Int16GetDatum(attnum);
	values[2] = TransactionIdGetDatum(xid);
	values[3] = BoolGetDatum(fullscan);
	values[4] = PointerGetDatum(counter);
	CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();
}

/*
 * The counter that goes with a leaf's statistics row, or NULL: the one
 * written with the row, by the row's xmin.
 */
static GpHLLCounter
leaf_counter(Oid relid, Oid leaf, HeapTuple stats, bool *fullscan)
{
	Relation	rel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Form_pg_statistic form = (Form_pg_statistic) GETSTRUCT(stats);
	TransactionId xmin = HeapTupleHeaderGetRawXmin(stats->t_data);
	GpHLLCounter result = NULL;

	if (!OidIsValid(relid))
		return NULL;
	rel = table_open(relid, AccessShareLock);
	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(leaf));
	ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(form->staattnum));
	scan = systable_beginscan(rel, linitial_oid(RelationGetIndexList(rel)), true,
							  NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		TupleDesc	tupdesc = RelationGetDescr(rel);

		if (DatumGetTransactionId(heap_getattr(tuple, 3, tupdesc, &isnull)) != xmin)
			continue;
		*fullscan = DatumGetBool(heap_getattr(tuple, 4, tupdesc, &isnull));
		result = (GpHLLCounter) DatumGetByteaPCopy(heap_getattr(tuple, 5, tupdesc, &isnull));
		break;
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return result;
}

/* Is this a value of the column's statistics: not NULL, not too wide? */
static bool
sample_value(HeapTuple row, TupleDesc tupdesc, AttrNumber attnum,
			 Datum *value, bool *toowide)
{
	Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
	bool		isnull;

	*toowide = false;
	*value = heap_getattr(row, attnum, tupdesc, &isnull);
	if (isnull)
		return false;
	if (att->attlen == -1)
	{
		if (toast_raw_datum_size(*value) > WIDTH_THRESHOLD)
		{
			*toowide = true;
			return false;
		}
		*value = PointerGetDatum(PG_DETOAST_DATUM(*value));
	}
	return true;
}

/* The comparator of compute_scalar_stats()'s sort of a sample's values. */
typedef struct CounterSortContext
{
	SortSupport ssup;
} CounterSortContext;

static int
compare_values(const void *a, const void *b, void *arg)
{
	SortSupport ssup = ((CounterSortContext *) arg)->ssup;

	return ApplySortComparator(*(const Datum *) a, false,
							   *(const Datum *) b, false, ssup);
}

/*
 * A leaf's counter of one column of its sample, with what Cloudberry's
 * standard statistics would have seen: compute_scalar_stats()'s distinct
 * values and those seen more than once, counted over the sorted values, or
 * compute_distinct_stats()'s, from its track list of at most twice the
 * target's values, which the values seen more than once lead.
 */
static GpHLLCounter
column_counter(Relation leaf, HeapTuple *rows, int numrows, AttrNumber attnum,
			   int target)
{
	TupleDesc	tupdesc = RelationGetDescr(leaf);
	Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
	Oid			ltopr;
	Oid			eqopr;
	ColumnStats kind = column_stats(att->atttypid, &ltopr, &eqopr);
	GpHLLCounter counter;
	Datum	   *values;
	int			nvalues = 0;
	int			toowide_cnt = 0;
	int			ndistinct = 0;
	int			nmultiple = 0;

	if (kind == COLUMN_STATS_NONE)
		return NULL;

	counter = GpHllCreate();
	values = palloc_array(Datum, numrows);
	for (int i = 0; i < numrows; i++)
	{
		Datum		value;
		bool		toowide;

		if (!sample_value(rows[i], tupdesc, attnum, &value, &toowide))
		{
			toowide_cnt += toowide;
			continue;
		}
		counter = GpHllAddItem(counter, value, att->attlen, att->attbyval);
		values[nvalues++] = value;
	}

	if (kind == COLUMN_STATS_SCALAR)
	{
		SortSupportData ssup = {0};
		CounterSortContext cxt = {&ssup};
		int			dups = 0;

		if (nvalues == 0)
			return counter;
		ssup.ssup_cxt = CurrentMemoryContext;
		ssup.ssup_collation = att->attcollation;
		ssup.ssup_nulls_first = false;
		PrepareSortSupportFromOrderingOp(ltopr, &ssup);
		qsort_arg(values, nvalues, sizeof(Datum), compare_values, &cxt);
		for (int i = 0; i < nvalues; i++)
		{
			dups++;
			if (i + 1 == nvalues ||
				ApplySortComparator(values[i], false, values[i + 1], false, &ssup) != 0)
			{
				ndistinct++;
				if (dups > 1)
					nmultiple++;
				dups = 0;
			}
		}
	}
	else
	{
		/* compute_distinct_stats()'s track list, as it keeps it */
		typedef struct
		{
			Datum		value;
			int			count;
		} TrackItem;
		int			track_max = Max(2 * target, 10);
		TrackItem  *track = palloc_array(TrackItem, track_max);
		int			track_cnt = 0;
		FmgrInfo	f_cmpeq;

		if (nvalues == 0)
			return counter;
		fmgr_info(get_opcode(eqopr), &f_cmpeq);
		for (int i = 0; i < nvalues; i++)
		{
			bool		match = false;
			int			firstcount1 = track_cnt;
			int			j;

			for (j = 0; j < track_cnt; j++)
			{
				if (DatumGetBool(FunctionCall2Coll(&f_cmpeq, att->attcollation,
												   values[i], track[j].value)))
				{
					match = true;
					break;
				}
				if (j < firstcount1 && track[j].count == 1)
					firstcount1 = j;
			}

			if (match)
			{
				track[j].count++;
				while (j > 0 && track[j].count > track[j - 1].count)
				{
					TrackItem	swap = track[j];

					track[j] = track[j - 1];
					track[j - 1] = swap;
					j--;
				}
			}
			else
			{
				if (track_cnt < track_max)
					track_cnt++;
				for (j = track_cnt - 1; j > firstcount1; j--)
					track[j] = track[j - 1];
				if (firstcount1 < track_cnt)
				{
					track[firstcount1].value = values[i];
					track[firstcount1].count = 1;
				}
			}
		}
		for (nmultiple = 0; nmultiple < track_cnt; nmultiple++)
		{
			if (track[nmultiple].count == 1)
				break;
		}
		ndistinct = track_cnt;
	}

	counter->ndistinct = ndistinct;
	counter->nmultiples = nmultiple;
	counter->samplerows = numrows - toowide_cnt;
	return counter;
}

/*
 * The counters of a leaf partition's columns, of the sample its ANALYZE took:
 * of the columns the statement names of it, or each it analyzes.  Nothing
 * for an empty sample, whose ANALYZE writes no statistics.
 */
void
GpLeafSampleCounters(Relation leaf, HeapTuple *rows, int numrows, List *va_cols)
{
	List	   *targets;
	List	   *attnums;
	ListCell   *lc;
	ListCell   *lt;
	MemoryContext cxt;
	MemoryContext old;

	if (numrows <= 0 || !OidIsValid(counters_table()))
		return;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "gp_core leaf counters",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	attnums = analyzed_columns(leaf, va_cols, &targets);
	forboth(lc, attnums, lt, targets)
	{
		GpHLLCounter counter = column_counter(leaf, rows, numrows, lfirst_int(lc),
											  lfirst_int(lt));

		if (counter != NULL)
			store_counter(RelationGetRelid(leaf), lfirst_int(lc), false, counter);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/*
 * ANALYZE FULLSCAN of a leaf: a counter of each column of every row, by
 * gp_hyperloglog_accum() over the table -- acquire_hll_by_query() -- which
 * the leaf keeps in place of its sample's, saying at elevel what it runs,
 * as Cloudberry does.  The counters, by column, for GpLeafFullScanNdistinct().
 */
List *
GpLeafFullScan(Relation leaf, List *va_cols, int elevel)
{
	List	   *targets;
	List	   *attnums = analyzed_columns(leaf, va_cols, &targets);
	List	   *result = NIL;
	StringInfoData sql;
	MemoryContext caller = CurrentMemoryContext;
	int			n = 0;

	if (attnums == NIL || !OidIsValid(counters_table()))
		return NIL;

	initStringInfo(&sql);
	appendStringInfoString(&sql, "select ");
	foreach_int(attnum, attnums)
		appendStringInfo(&sql, "%spg_catalog.gp_hyperloglog_accum(%s)",
						 n++ > 0 ? ", " : "",
						 quote_identifier(NameStr(TupleDescAttr(RelationGetDescr(leaf),
																attnum - 1)->attname)));
	appendStringInfo(&sql, " from %s.%s as Ta",
					 quote_identifier(get_namespace_name(RelationGetNamespace(leaf))),
					 quote_identifier(RelationGetRelationName(leaf)));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "unable to connect to execute internal query");
	ereport(elevel, (errmsg("Executing SQL: %s", sql.data)));
	if (SPI_execute(sql.data, false, 0) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "the full scan of \"%s\" returned no row",
			 RelationGetRelationName(leaf));

	n = 0;
	foreach_int(attnum, attnums)
	{
		bool		isnull;
		Datum		value = heap_getattr(SPI_tuptable->vals[0], ++n,
										 SPI_tuptable->tupdesc, &isnull);
		MemoryContext old = MemoryContextSwitchTo(caller);
		GpHLLCounter counter = isnull ? GpHllCreate()
			: (GpHLLCounter) DatumGetByteaPCopy(value);

		result = lappend(result, list_make2(makeInteger(attnum), counter));
		MemoryContextSwitchTo(old);
	}
	SPI_finish();

	foreach_node(List, pair, result)
		store_counter(RelationGetRelid(leaf), intVal(linitial(pair)), true,
					  (GpHLLCounter) lsecond(pair));
	ereport(elevel, (errmsg("HLL FULL SCAN")));
	return result;
}

/* ------------------------------------------------------------------------- */
/* What is written after PostgreSQL has written a relation's statistics     */
/* ------------------------------------------------------------------------- */

/*
 * A root's columns merged from its leaves' where PostgreSQL sampled the
 * root for others, or a leaf's number of distinct values a full scan
 * counted, each written over the row PostgreSQL writes: as the
 * transaction commits, or as the statement ends where it runs in the
 * caller's.  Forgotten with the subtransaction that asked for them.
 */
typedef struct PendingWrite
{
	Oid			relid;
	List	   *attnums;		/* a root's, to merge */
	AttrNumber	attnum;			/* or a leaf's, */
	float4		stadistinct;	/* and its number of distinct values */
	SubTransactionId subid;
} PendingWrite;

static List *pending = NIL;		/* of PendingWrite, in TopTransactionContext */

static void
add_pending(PendingWrite *w)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	PendingWrite *copy = palloc_object(PendingWrite);

	*copy = *w;
	copy->attnums = list_copy(w->attnums);
	copy->subid = GetCurrentSubTransactionId();
	pending = lappend(pending, copy);
	MemoryContextSwitchTo(old);
}

/*
 * The number of distinct values of a full scan's counter as the leaf's own,
 * as Cloudberry's compute_scalar_stats() takes it: of a column its "<"
 * orders, where the sample had a value of it, rounded, every row distinct
 * where that is within 5% of the rows, and scaled with them above a tenth.
 */
void
GpLeafFullScanNdistinct(Relation leaf, List *counters, HeapTuple *rows,
						int numrows, double totalrows)
{
	TupleDesc	tupdesc = RelationGetDescr(leaf);

	foreach_node(List, pair, counters)
	{
		AttrNumber	attnum = intVal(linitial(pair));
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
		Oid			ltopr;
		Oid			eqopr;
		bool		have = false;
		PendingWrite w = {0};
		double		stadistinct;

		if (column_stats(att->atttypid, &ltopr, &eqopr) != COLUMN_STATS_SCALAR)
			continue;
		for (int i = 0; i < numrows && !have; i++)
		{
			Datum		value;
			bool		toowide;

			have = sample_value(rows[i], tupdesc, attnum, &value, &toowide);
		}
		if (!have)
			continue;

		stadistinct = round(GpHllEstimate((GpHLLCounter) lsecond(pair)));
		if ((fabs(totalrows - stadistinct) / (float) totalrows) < 0.05)
			stadistinct = -1;
		if (stadistinct > 0.1 * totalrows)
			stadistinct = -(stadistinct / totalrows);

		w.relid = RelationGetRelid(leaf);
		w.attnum = attnum;
		w.stadistinct = stadistinct;
		add_pending(&w);
	}
}

void
GpRootMergeLater(Oid root, List *attnums)
{
	PendingWrite w = {0};

	w.relid = root;
	w.attnums = attnums;
	add_pending(&w);
}

static void merge_and_write(Relation root, List *attnums);

/* A leaf's number of distinct values, over PostgreSQL's. */
static void
write_ndistinct(Oid relid, AttrNumber attnum, float4 stadistinct)
{
	Relation	sd = table_open(StatisticRelationId, RowExclusiveLock);
	HeapTuple	oldtup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid),
										 Int16GetDatum(attnum), BoolGetDatum(false));
	HeapTuple	tup;

	if (HeapTupleIsValid(oldtup))
	{
		tup = heap_copytuple(oldtup);
		((Form_pg_statistic) GETSTRUCT(tup))->stadistinct = stadistinct;
		ReleaseSysCache(oldtup);
		CatalogTupleUpdate(sd, &tup->t_self, tup);
	}
	table_close(sd, RowExclusiveLock);
}

/*
 * Write what is pending now: after the statement where it ran in the
 * caller's transaction, and at the commit of each of its own -- where
 * vacuum() has taken its snapshot away, which a catalog's update needs --
 * or before a PREPARE TRANSACTION of the caller's.
 */
void
GpPartMergeFinish(void)
{
	bool		pushed = false;

	if (pending != NIL && !ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	while (pending != NIL)
	{
		PendingWrite *w = linitial(pending);

		pending = list_delete_first(pending);
		CommandCounterIncrement();
		if (w->attnums != NIL)
		{
			Relation	root = try_relation_open(w->relid, ShareUpdateExclusiveLock);

			if (root == NULL)
				continue;
			merge_and_write(root, w->attnums);
			relation_close(root, NoLock);
		}
		else
			write_ndistinct(w->relid, w->attnum, w->stadistinct);
	}
	if (pushed)
		PopActiveSnapshot();
}

static void
partmerge_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			GpPartMergeFinish();
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			pending = NIL;
			break;
		default:
			break;
	}
}

static void
partmerge_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg)
{
	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
	foreach_ptr(PendingWrite, w, pending)
	{
		if (w->subid >= mySubid)
			pending = foreach_delete_current(pending, w);
	}
}

/* ------------------------------------------------------------------------- */
/* The most common values and the histogram, the leaves' merged              */
/* ------------------------------------------------------------------------- */

/* A column's type, as analyzeutils.c's TypInfo describes it. */
typedef struct TypInfo
{
	Oid			typOid;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	Oid			collid;
	FmgrInfo	ltfunc;
	FmgrInfo	eqfunc;
	FmgrInfo	hashfunc;
} TypInfo;

/* A most common value of the leaves', and how many rows have it. */
typedef struct MCVFreqPair
{
	Datum		mcv;
	float4		count;
	int			position;		/* in the order the leaves' lists give */
	TypInfo    *typinfo;
} MCVFreqPair;

typedef struct MCVFreqEntry
{
	MCVFreqPair *entry;
} MCVFreqEntry;

/* A bound of one of the histograms merged, and whose */
typedef struct PartDatum
{
	int			partId;
	Datum		datum;
} PartDatum;

static void
init_typinfo(TypInfo *typInfo, Oid relationOid, AttrNumber attnum)
{
	Oid			ltOpr;
	Oid			eqOpr;
	Oid			hashFunc;
	int32		typmod;

	get_atttypetypmodcoll(relationOid, attnum, &typInfo->typOid, &typmod,
						  &typInfo->collid);
	get_typlenbyvalalign(typInfo->typOid, &typInfo->typlen, &typInfo->typbyval,
						 &typInfo->typalign);
	get_sort_group_operators(typInfo->typOid, false, true, false, &ltOpr, &eqOpr,
							 NULL, NULL);
	if (OidIsValid(ltOpr))
		fmgr_info(get_opcode(ltOpr), &typInfo->ltfunc);
	fmgr_info(get_opcode(eqOpr), &typInfo->eqfunc);
	if (!get_op_hash_functions(eqOpr, &hashFunc, NULL))
		elog(ERROR, "could not find hash function for hash operator %u", eqOpr);
	fmgr_info(hashFunc, &typInfo->hashfunc);
}

static uint32
datum_hash(const void *keyPtr, Size keysize)
{
	MCVFreqPair *pair = *((MCVFreqPair **) keyPtr);

	return DatumGetUInt32(FunctionCall1Coll(&pair->typinfo->hashfunc,
											pair->typinfo->collid, pair->mcv));
}

static int
datum_match(const void *keyPtr1, const void *keyPtr2, Size keysize)
{
	MCVFreqPair *left = *((MCVFreqPair **) keyPtr1);
	MCVFreqPair *right = *((MCVFreqPair **) keyPtr2);

	return DatumGetBool(FunctionCall2Coll(&left->typinfo->eqfunc,
										  left->typinfo->collid,
										  left->mcv, right->mcv)) ? 0 : 1;
}

/*
 * Each leaf's most common values into the table, a value's rows added up:
 * addLeafPartitionMCVsToHashTable().
 */
static void
add_leaf_mcvs(HTAB *datumHash, HeapTuple stats, float4 partReltuples,
			  TypInfo *typInfo, int *idx)
{
	AttStatsSlot mcvSlot;
	int			position = *idx;

	(void) get_attstatsslot(&mcvSlot, stats, STATISTIC_KIND_MCV, InvalidOid,
							ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	for (int i = 0; i < mcvSlot.nvalues; i++)
	{
		MCVFreqPair pair;
		MCVFreqPair *key = &pair;
		MCVFreqEntry *entry;
		bool		found;

		pair.mcv = mcvSlot.values[i];
		pair.count = partReltuples * mcvSlot.numbers[i];
		pair.position = position++;
		pair.typinfo = typInfo;

		entry = hash_search(datumHash, &key, HASH_FIND, &found);
		if (found)
			entry->entry->count += pair.count;
		else
		{
			MCVFreqPair *copy = palloc_object(MCVFreqPair);

			*copy = pair;
			copy->mcv = datumCopy(pair.mcv, typInfo->typbyval, typInfo->typlen);
			entry = hash_search(datumHash, &copy, HASH_ENTER, &found);
			entry->entry = copy;
		}
	}
	*idx = position;
	free_attstatsslot(&mcvSlot);
}

/* By count, the higher first, and then in the leaves' order. */
static int
mcvpair_cmp(const void *a, const void *b)
{
	MCVFreqPair *pair1 = *(MCVFreqPair **) a;
	MCVFreqPair *pair2 = *(MCVFreqPair **) b;

	if (pair1->count > pair2->count)
		return -1;
	if (pair1->count < pair2->count)
		return 1;
	return pair1->position - pair2->position;
}

/*
 * The values common enough over all the leaves' rows to keep, at most
 * nEntries: those counted at least 80% of a typical value's rows -- 2 at
 * least, and no more than 1/K of all -- unless every distinct value fits:
 * buildMCVArrayForStatsEntry().  NULL where not even the first is.
 */
static Datum *
build_mcv_array(MCVFreqPair **mcvpairArray, int *nEntries, float4 ndistinct,
				float4 nrows)
{
	Datum	   *out = palloc_array(Datum, *nEntries);
	double		mincount = -1.0;

	if (!(*nEntries == (int) ndistinct && ndistinct > 0))
	{
		double		avgcount = (double) nrows / ndistinct;
		double		maxmincount;

		mincount = avgcount * 0.80;
		if (mincount < 2)
			mincount = 2;
		maxmincount = (double) nrows / (double) *nEntries;
		if (mincount > maxmincount)
			mincount = maxmincount;
	}
	for (int i = 0; i < *nEntries; i++)
	{
		if (mcvpairArray[i]->count < mincount)
		{
			if (i == 0)
			{
				pfree(out);
				return NULL;
			}
			*nEntries = i;
			break;
		}
		out[i] = mcvpairArray[i]->mcv;
	}
	return out;
}

/*
 * The leaves' most common values merged: aggregate_leaf_partition_MCVs().
 * The pairs in order of count, the first *num_mcv of them kept, with their
 * frequencies in *freqs; *rem_mcv of them left over, for the histogram.
 */
static MCVFreqPair **
aggregate_mcvs(TypInfo *typInfo, int numPartitions, HeapTuple *heaptupleStats,
			   float4 *relTuples, unsigned int nEntries, double ndistinct,
			   int *num_mcv, int *rem_mcv, Datum **mcvs, float4 **freqs)
{
	HASHCTL		hash_ctl;
	HTAB	   *datumHash;
	float4		sumReltuples = 0;
	int			orderIdx = 0;
	int			i = 0;
	HASH_SEQ_STATUS hash_seq;
	MCVFreqEntry *mcvfreq;
	MCVFreqPair **mcvpairArray;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(MCVFreqPair *);
	hash_ctl.entrysize = sizeof(MCVFreqEntry);
	hash_ctl.hash = datum_hash;
	hash_ctl.match = datum_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	datumHash = hash_create("gp_core merged MCVs", nEntries, &hash_ctl,
							HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	for (int p = 0; p < numPartitions; p++)
	{
		if (!HeapTupleIsValid(heaptupleStats[p]))
			continue;
		add_leaf_mcvs(datumHash, heaptupleStats[p], relTuples[p], typInfo, &orderIdx);
		sumReltuples += relTuples[p];
	}

	*mcvs = NULL;
	*freqs = NULL;
	*rem_mcv = hash_get_num_entries(datumHash);
	if (*rem_mcv == 0)
	{
		hash_destroy(datumHash);
		return NULL;
	}

	mcvpairArray = palloc_array(MCVFreqPair *, *rem_mcv);
	hash_seq_init(&hash_seq, datumHash);
	while ((mcvfreq = hash_seq_search(&hash_seq)) != NULL)
		mcvpairArray[i++] = mcvfreq->entry;
	qsort(mcvpairArray, i, sizeof(MCVFreqPair *), mcvpair_cmp);

	*num_mcv = Min(i, nEntries);
	*mcvs = build_mcv_array(mcvpairArray, num_mcv, ndistinct, sumReltuples);
	if (*mcvs == NULL)
	{
		hash_destroy(datumHash);
		*num_mcv = 0;
		return mcvpairArray;
	}

	*freqs = palloc_array(float4, *num_mcv);
	for (int k = 0; k < *num_mcv; k++)
		(*freqs)[k] = mcvpairArray[k]->count / sumReltuples;

	hash_destroy(datumHash);
	*rem_mcv -= *num_mcv;
	return mcvpairArray;
}

/* The lower bound first: the heap is a max-heap. */
static int
datum_heap_cmp(Datum lhs, Datum rhs, void *context)
{
	Datum		d1 = ((PartDatum *) DatumGetPointer(lhs))->datum;
	Datum		d2 = ((PartDatum *) DatumGetPointer(rhs))->datum;
	TypInfo    *typInfo = (TypInfo *) context;

	if (DatumGetBool(FunctionCall2Coll(&typInfo->ltfunc, typInfo->collid, d1, d2)))
		return 1;
	if (DatumGetBool(FunctionCall2Coll(&typInfo->eqfunc, typInfo->collid, d1, d2)))
		return 0;
	return -1;
}

/* A histogram's next bucket, or -1 past its last. */
static void
advance_cursor(int pid, int *cursors, AttStatsSlot **histSlots)
{
	cursors[pid]++;
	if (cursors[pid] >= histSlots[pid]->nvalues)
		cursors[pid] = -1;
}

/*
 * Each histogram's size of bucket, in rows: its leaf's rows less its NULLs
 * and its most common values, over its buckets; a value left over from the
 * most common, its rows -- getBucketSizes().  And all of them together.
 */
static float4
bucket_sizes(HeapTuple *heaptupleStats, float4 *relTuples, int nParts,
			 MCVFreqPair **mcvPairRemaining, int rem_mcv, float4 *eachBucket)
{
	float4	   *total = palloc_array(float4, nParts);
	float4		sumTotal = 0;
	int			pid = 0;

	for (int i = 0; i < nParts; ++i)
	{
		AttStatsSlot slot;

		total[i] = relTuples[i];
		if (heaptupleStats[i] == NULL)
			continue;

		if (get_attstatsslot(&slot, heaptupleStats[i], STATISTIC_KIND_MCV,
							 InvalidOid, ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			for (int j = 0; j < slot.nnumbers; ++j)
				total[i] -= relTuples[i] * slot.numbers[j];
			free_attstatsslot(&slot);
		}
		total[i] -= relTuples[i] * ((Form_pg_statistic) GETSTRUCT(heaptupleStats[i]))->stanullfrac;
		if (total[i] < 0.0)
			total[i] = 0.0;

		if (get_attstatsslot(&slot, heaptupleStats[i], STATISTIC_KIND_HISTOGRAM,
							 InvalidOid, ATTSTATSSLOT_VALUES))
		{
			eachBucket[pid] = total[i] / (slot.nvalues - 1);
			pid++;
			free_attstatsslot(&slot);
		}
		sumTotal += total[i];
	}

	for (int i = pid; i < pid + rem_mcv; ++i)
	{
		eachBucket[i] = mcvPairRemaining[i - pid]->count;
		sumTotal += eachBucket[i];
	}
	pfree(total);
	return sumTotal;
}

/*
 * The leaves' histograms, and the values no longer common enough as
 * histograms of one value each, merged into one of at most nEntries bounds
 * -- aggregate_leaf_partition_histograms().  Each merged bucket is filled
 * with the leaves' buckets in order of their upper bounds, a heap of each
 * histogram's next, and closed with the bound of the bucket that fills it:
 * the least lower bound first, the greatest upper bound last, a bound equal
 * to the one before left out.
 */
static int
aggregate_histograms(TypInfo *typInfo, int nParts, HeapTuple *heaptupleStats,
					 float4 *relTuples, unsigned int nEntries,
					 MCVFreqPair **mcvpairArray, int rem_mcv, Datum **result)
{
	AttStatsSlot **histSlots = palloc0_array(AttStatsSlot *, nParts + rem_mcv);
	float4	   *eachBucket = palloc0_array(float4, nParts + rem_mcv);
	int			numNotNullParts = 0;
	float4		sumReltuples;
	float4		bucketSize;
	float4		nTuplesToFill;
	int		   *cursors;
	float4	   *remainingSize;
	binaryheap *dhp;
	PartDatum  *pds;
	List	   *ldatum = NIL;
	Datum		minBound;
	Datum		maxBound;
	Datum	   *out;
	Datum		prev;
	int			num_hist = 0;

	/* the leaves' histograms, and the values left over */
	for (int i = 0; i < nParts; i++)
	{
		if (!HeapTupleIsValid(heaptupleStats[i]))
			continue;
		histSlots[numNotNullParts] = palloc_object(AttStatsSlot);
		(void) get_attstatsslot(histSlots[numNotNullParts], heaptupleStats[i],
								STATISTIC_KIND_HISTOGRAM, InvalidOid,
								ATTSTATSSLOT_VALUES);
		if (histSlots[numNotNullParts]->nvalues > 0)
			numNotNullParts++;
	}
	if (numNotNullParts + rem_mcv == 0)
	{
		*result = NULL;
		return 0;
	}
	for (int i = 0; i < rem_mcv; i++)
	{
		AttStatsSlot *slot = palloc0_object(AttStatsSlot);

		slot->nvalues = 2;
		slot->values = palloc_array(Datum, 2);
		slot->values[0] = slot->values[1] = mcvpairArray[i]->mcv;
		histSlots[numNotNullParts + i] = slot;
	}
	sumReltuples = bucket_sizes(heaptupleStats, relTuples, nParts, mcvpairArray,
								rem_mcv, eachBucket);
	nParts = numNotNullParts + rem_mcv;

	bucketSize = sumReltuples / nEntries;
	nTuplesToFill = bucketSize;
	cursors = palloc0_array(int, nParts);
	remainingSize = palloc0_array(float4, nParts);
	for (int i = 0; i < nParts; i++)
	{
		if (1 < histSlots[i]->nvalues)
			remainingSize[i] = eachBucket[i];
	}

	/* the least of the first bounds */
	minBound = histSlots[0]->values[0];
	for (int pid = 0; pid < nParts; pid++)
	{
		if (DatumGetBool(FunctionCall2Coll(&typInfo->ltfunc, typInfo->collid,
										   histSlots[pid]->values[0], minBound)))
			minBound = histSlots[pid]->values[0];
		advance_cursor(pid, cursors, histSlots);
	}
	ldatum = lappend(ldatum, &minBound);

	/* a heap of each histogram's second bound */
	dhp = binaryheap_allocate(nParts, datum_heap_cmp, typInfo);
	pds = palloc_array(PartDatum, nParts);
	for (int pid = 0; pid < nParts; pid++)
	{
		if (cursors[pid] > 0)
		{
			pds[pid].partId = pid;
			pds[pid].datum = histSlots[pid]->values[cursors[pid]];
			binaryheap_add_unordered(dhp, PointerGetDatum(&pds[pid]));
		}
	}
	binaryheap_build(dhp);

	while (!binaryheap_empty(dhp) && list_length(ldatum) < nEntries)
	{
		PartDatum  *pd = (PartDatum *) DatumGetPointer(binaryheap_first(dhp));
		int			pid = pd->partId;

		if (remainingSize[pid] < nTuplesToFill)
		{
			nTuplesToFill -= remainingSize[pid];
			advance_cursor(pid, cursors, histSlots);
			remainingSize[pid] = eachBucket[pid];
			if (cursors[pid] > 0)
			{
				pd->datum = histSlots[pid]->values[cursors[pid]];
				binaryheap_replace_first(dhp, PointerGetDatum(pd));
			}
			else
				(void) binaryheap_remove_first(dhp);
		}
		else
		{
			ldatum = lappend(ldatum, &histSlots[pid]->values[cursors[pid]]);
			remainingSize[pid] -= nTuplesToFill;
			nTuplesToFill = bucketSize;
		}
	}

	/* the greatest of the last bounds */
	maxBound = histSlots[0]->values[histSlots[0]->nvalues - 1];
	for (int pid = 0; pid < nParts; pid++)
	{
		if (DatumGetBool(FunctionCall2Coll(&typInfo->ltfunc, typInfo->collid, maxBound,
										   histSlots[pid]->values[histSlots[pid]->nvalues - 1])))
			maxBound = histSlots[pid]->values[histSlots[pid]->nvalues - 1];
	}
	ldatum = lappend(ldatum, &maxBound);

	/* the bounds, one equal to the bound before left out */
	out = palloc_array(Datum, list_length(ldatum));
	prev = *(Datum *) linitial(ldatum);
	foreach_ptr(Datum, pdatum, ldatum)
	{
		if (DatumGetBool(FunctionCall2Coll(&typInfo->eqfunc, typInfo->collid,
										   *pdatum, prev)) &&
			foreach_current_index(pdatum) > 0)
			continue;
		out[num_hist++] = *pdatum;
		prev = *pdatum;
	}

	binaryheap_free(dhp);
	*result = out;
	return num_hist;
}

/* ------------------------------------------------------------------------- */
/* The merge                                                                 */
/* ------------------------------------------------------------------------- */

static float4
leaf_reltuples(Oid leaf)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(leaf));
	float4		result = 0;

	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_class) GETSTRUCT(tp))->reltuples;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * A leaf's statistics row of the root's column, found by the column's name,
 * or NULL -- read from the catalog, as fetch_leaf_att_stats() reads it.
 */
static HeapTuple
leaf_stats(Oid leaf, const char *attname)
{
	HeapTuple	tp = SearchSysCacheAttName(leaf, attname);
	AttrNumber	attnum;
	Relation	rel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tuple;

	if (!HeapTupleIsValid(tp))
		return NULL;
	attnum = ((Form_pg_attribute) GETSTRUCT(tp))->attnum;
	ReleaseSysCache(tp);

	rel = table_open(StatisticRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_statistic_starelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(leaf));
	ScanKeyInit(&key[1], Anum_pg_statistic_staattnum, BTEqualStrategyNumber,
				F_INT2EQ, Int16GetDatum(attnum));
	ScanKeyInit(&key[2], Anum_pg_statistic_stainherit, BTEqualStrategyNumber,
				F_BOOLEQ, BoolGetDatum(false));
	scan = systable_beginscan(rel, StatisticRelidAttnumInhIndexId, true,
							  NULL, 3, key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return tuple;
}

/*
 * One column of the root, its statistics merged from its leaves' into
 * stats: Cloudberry's merge_leaf_stats(), step for step.  False where the
 * leaves have no rows, which leaves the column's statistics as they are.
 */
static bool
merge_column(Relation root, AttrNumber attnum, int target, VacAttrStats *stats)
{
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(root), attnum - 1);
	const char *attname = NameStr(att->attname);
	Oid			counters = counters_table();
	Oid			ltopr;
	Oid			eqopr;
	List	   *members;
	List	   *oid_list = NIL;
	int			numPartitions;
	float	   *relTuples;
	float	   *nDistincts;
	float	   *nMultiples;
	float		totalTuples = 0;
	float		nmultiple = 0;
	bool		allDistinct = false;
	int			slot_idx = 0;
	int			sampleCount = 0;
	HeapTuple  *heaptupleStats;
	float4		colAvgWidth = 0;
	float4		nullCount = 0;
	GpHLLCounter *hllcounters;
	GpHLLCounter *hllcounters_copy;
	GpHLLCounter finalHLL = NULL;
	GpHLLCounter finalHLLFull = NULL;
	double		ndistinct = 0.0;
	int			fullhll_count = 0;
	int			samplehll_count = 0;
	int			totalhll_count = 0;
	int			i = 0;
	TypInfo		typInfo;
	MCVFreqPair **mcvpairArray = NULL;
	int			rem_mcv = 0;
	int			num_mcv = 0;

	get_sort_group_operators(att->atttypid, false, false, false, &ltopr, &eqopr,
							 NULL, NULL);

	/*
	 * The leaves, which the root's ShareUpdateExclusiveLock keeps: dropping
	 * one takes the root's AccessExclusiveLock.
	 */
	members = find_all_inheritors(RelationGetRelid(root), NoLock, NULL);
	GP_FAULT("merge_leaf_stats_after_find_children");
	foreach_oid(relid, members)
	{
		if (get_rel_relkind(relid) == RELKIND_RELATION)
			oid_list = lappend_oid(oid_list, relid);
	}
	numPartitions = list_length(oid_list);
	if (numPartitions == 0)
		return false;

	relTuples = palloc0_array(float, numPartitions);
	nDistincts = palloc0_array(float, numPartitions);
	nMultiples = palloc0_array(float, numPartitions);
	foreach_oid(relid, oid_list)
	{
		relTuples[i] = leaf_reltuples(relid);
		totalTuples = totalTuples + relTuples[i];
		i++;
	}
	if (totalTuples == 0.0)
		return false;

	/* each leaf's row, its NULLs and width, and its counter merged */
	heaptupleStats = palloc0_array(HeapTuple, numPartitions);
	hllcounters = palloc0_array(GpHLLCounter, numPartitions);
	hllcounters_copy = palloc0_array(GpHLLCounter, numPartitions);
	i = 0;
	foreach_oid(relid, oid_list)
	{
		Form_pg_statistic form;
		GpHLLCounter counter;
		bool		fullscan = false;

		heaptupleStats[i] = leaf_stats(relid, attname);
		if (!HeapTupleIsValid(heaptupleStats[i]))
		{
			i++;
			continue;
		}
		form = (Form_pg_statistic) GETSTRUCT(heaptupleStats[i]);
		colAvgWidth = colAvgWidth + (form->stawidth > 0 ? form->stawidth : 0) * relTuples[i];
		nullCount = nullCount + (form->stanullfrac > 0.0 ? form->stanullfrac : 0.0) * relTuples[i];

		counter = leaf_counter(counters, relid, heaptupleStats[i], &fullscan);
		if (counter != NULL && fullscan)
		{
			GpHLLCounter previous = finalHLLFull;

			finalHLLFull = GpHllMerge(previous, counter);
			if (previous != NULL)
				pfree(previous);
			fullhll_count++;
			totalhll_count++;
		}
		else if (counter != NULL)
		{
			GpHLLCounter previous = finalHLL;

			hllcounters[i] = counter;
			nDistincts[i] = (float) counter->ndistinct;
			nMultiples[i] = (float) counter->nmultiples;
			sampleCount += counter->samplerows;
			hllcounters_copy[i] = GpHllCopy(counter);
			finalHLL = GpHllMerge(previous, counter);
			if (previous != NULL)
				pfree(previous);
			samplehll_count++;
			totalhll_count++;
		}
		i++;
	}

	/*
	 * The number of distinct values: with no counters, as the defaults say;
	 * of full scans' counters, their merged estimate -- every row distinct
	 * within the error; of samples' counters, of the samples, and the values
	 * seen more than once apportioned by the values each leaf alone has.
	 */
	if (totalhll_count > 0)
	{
		if (fullhll_count == totalhll_count)
		{
			ndistinct = GpHllEstimate(finalHLLFull);
			if ((fabs(totalTuples - ndistinct) / (float) totalTuples) < GP_HLL_ERROR_MARGIN)
				allDistinct = true;
			nmultiple = ndistinct;
		}
		else if (finalHLL != NULL && samplehll_count == totalhll_count)
		{
			ndistinct = GpHllEstimate(finalHLL);
			if ((fabs(sampleCount - ndistinct) / (float) sampleCount) < GP_HLL_ERROR_MARGIN)
				allDistinct = true;
			else
			{
				/*
				 * A leaf's values of its own: the root's less those of the
				 * leaves to its left and to its right merged.
				 */
				GpHLLCounter *left = palloc0_array(GpHLLCounter, numPartitions);
				GpHLLCounter *right = palloc0_array(GpHLLCounter, numPartitions);
				int			nUnique = 0;

				left[0] = GpHllCreate();
				right[numPartitions - 1] = GpHllCreate();
				for (i = 1; i < numPartitions; i++)
				{
					if (nDistincts[i - 1] == 0)
						left[i] = GpHllCopy(left[i - 1]);
					else
					{
						GpHLLCounter temp1 = GpHllCopy(hllcounters_copy[i - 1]);
						GpHLLCounter temp2 = GpHllCopy(left[i - 1]);

						left[i] = GpHllMerge(temp1, temp2);
						pfree(temp1);
						pfree(temp2);
					}

					if (nDistincts[numPartitions - i] == 0)
						right[numPartitions - i - 1] = GpHllCopy(right[numPartitions - i]);
					else
					{
						GpHLLCounter temp1 = GpHllCopy(hllcounters_copy[numPartitions - i]);
						GpHLLCounter temp2 = GpHllCopy(right[numPartitions - i]);

						right[numPartitions - i - 1] = GpHllMerge(temp1, temp2);
						pfree(temp1);
						pfree(temp2);
					}
				}

				for (i = 0; i < numPartitions; i++)
				{
					GpHLLCounter temp1;
					GpHLLCounter temp2;
					GpHLLCounter final;

					if (nDistincts[i] == 0)
						continue;
					temp1 = GpHllCopy(left[i]);
					temp2 = GpHllCopy(right[i]);
					final = GpHllMerge(temp1, temp2);
					pfree(temp1);
					pfree(temp2);
					if (final != NULL)
					{
						float		nUniques = ndistinct - GpHllEstimate(final);

						nUnique += nUniques;
						nmultiple += nMultiples[i] * (nUniques / nDistincts[i]);
						pfree(final);
					}
					else
					{
						nUnique = ndistinct;
						break;
					}
				}

				nmultiple += ndistinct - nUnique;
				if (nmultiple < 0)
					nmultiple = 0;
			}
		}
		else
			ereport(ERROR,
					(errmsg("ANALYZE cannot merge since not all non-empty leaf partitions have consistent hyperloglog statistics for merge"),
					 errhint("Re-run ANALYZE or ANALYZE FULLSCAN")));
	}

	if (allDistinct || (!OidIsValid(eqopr) && !OidIsValid(ltopr)))
		ndistinct = -1.0;
	else if ((int) nmultiple >= (int) ndistinct)
	{
		/* every value seen more than once: just these values */
	}
	else
	{
		/* Haas and Stokes' Duj1, n*d / (n - f1 + f1*n/N), over the samples */
		int			f1 = ndistinct - nmultiple;
		int			d = f1 + nmultiple;
		double		numer,
					denom,
					stadistinct;

		numer = (double) sampleCount * (double) d;
		denom = (double) (sampleCount - f1) +
			(double) f1 * (double) sampleCount / totalTuples;
		stadistinct = numer / denom;
		if (stadistinct < (double) d)
			stadistinct = (double) d;
		if (stadistinct > totalTuples)
			stadistinct = totalTuples;
		ndistinct = floor(stadistinct + 0.5);
	}

	ndistinct = round(ndistinct);
	if (ndistinct > 0.1 * totalTuples)
		ndistinct = -(ndistinct / totalTuples);

	stats->stadistinct = ndistinct;
	stats->stats_valid = true;
	stats->stawidth = colAvgWidth / totalTuples;
	stats->stanullfrac = (float4) nullCount / (float4) totalTuples;

	init_typinfo(&typInfo, RelationGetRelid(root), attnum);

	/* the most common values */
	if (ndistinct > -1 && OidIsValid(eqopr))
	{
		Datum	   *mcvs;
		float4	   *freqs;

		if (ndistinct < 0)
			ndistinct = -ndistinct * totalTuples;
		mcvpairArray = aggregate_mcvs(&typInfo, numPartitions, heaptupleStats,
									  relTuples, target, ndistinct,
									  &num_mcv, &rem_mcv, &mcvs, &freqs);
		if (num_mcv > 0)
		{
			stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
			stats->staop[slot_idx] = eqopr;
			stats->stacoll[slot_idx] = att->attcollation;
			stats->stavalues[slot_idx] = mcvs;
			stats->numvalues[slot_idx] = num_mcv;
			stats->stanumbers[slot_idx] = freqs;
			stats->numnumbers[slot_idx] = num_mcv;
			slot_idx++;
		}
	}

	/* the histogram, and the values no longer common enough in it */
	if (OidIsValid(eqopr) && OidIsValid(ltopr))
	{
		Datum	   *bounds;
		int			num_hist = aggregate_histograms(&typInfo, numPartitions, heaptupleStats,
													relTuples, target,
													mcvpairArray != NULL ? mcvpairArray + num_mcv : NULL,
													rem_mcv, &bounds);

		if (num_hist > 0)
		{
			stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
			stats->staop[slot_idx] = ltopr;
			stats->stacoll[slot_idx] = att->attcollation;
			stats->stavalues[slot_idx] = bounds;
			stats->numvalues[slot_idx] = num_hist;
			slot_idx++;
		}
	}

	for (int k = 0; k < STATISTIC_NUM_SLOTS; k++)
	{
		stats->statypid[k] = typInfo.typOid;
		stats->statyplen[k] = typInfo.typlen;
		stats->statypbyval[k] = typInfo.typbyval;
		stats->statypalign[k] = typInfo.typalign;
	}
	return true;
}

/*
 * The root's statistics row of the column, stainherit, written as
 * PostgreSQL's update_attstats() writes one.
 */
static void
write_root_stats(Oid relid, AttrNumber attnum, VacAttrStats *stats)
{
	Relation	sd = table_open(StatisticRelationId, RowExclusiveLock);
	Datum		values[Natts_pg_statistic];
	bool		nulls[Natts_pg_statistic];
	bool		replaces[Natts_pg_statistic];
	HeapTuple	oldtup;
	int			i;

	for (i = 0; i < Natts_pg_statistic; ++i)
	{
		nulls[i] = false;
		replaces[i] = true;
	}
	values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(true);
	values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(stats->stanullfrac);
	values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(stats->stawidth);
	values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(stats->stadistinct);
	for (int k = 0; k < STATISTIC_NUM_SLOTS; k++)
	{
		values[Anum_pg_statistic_stakind1 - 1 + k] = Int16GetDatum(stats->stakind[k]);
		values[Anum_pg_statistic_staop1 - 1 + k] = ObjectIdGetDatum(stats->staop[k]);
		values[Anum_pg_statistic_stacoll1 - 1 + k] = ObjectIdGetDatum(stats->stacoll[k]);

		i = Anum_pg_statistic_stanumbers1 - 1 + k;
		if (stats->stanumbers[k] != NULL)
		{
			Datum	   *numdatums = palloc_array(Datum, stats->numnumbers[k]);

			for (int n = 0; n < stats->numnumbers[k]; n++)
				numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
			values[i] = PointerGetDatum(construct_array_builtin(numdatums,
																stats->numnumbers[k],
																FLOAT4OID));
		}
		else
		{
			nulls[i] = true;
			values[i] = (Datum) 0;
		}

		i = Anum_pg_statistic_stavalues1 - 1 + k;
		if (stats->stavalues[k] != NULL)
			values[i] = PointerGetDatum(construct_array(stats->stavalues[k],
														stats->numvalues[k],
														stats->statypid[k],
														stats->statyplen[k],
														stats->statypbyval[k],
														stats->statypalign[k]));
		else
		{
			nulls[i] = true;
			values[i] = (Datum) 0;
		}
	}

	oldtup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid),
							 Int16GetDatum(attnum), BoolGetDatum(true));
	if (HeapTupleIsValid(oldtup))
	{
		HeapTuple	stup = heap_modify_tuple(oldtup, RelationGetDescr(sd),
											 values, nulls, replaces);

		ReleaseSysCache(oldtup);
		CatalogTupleUpdate(sd, &stup->t_self, stup);
	}
	else
		CatalogTupleInsert(sd, heap_form_tuple(RelationGetDescr(sd), values, nulls));
	table_close(sd, RowExclusiveLock);
}

/* The root's columns merged and written, each as its own statistics are. */
static void
merge_and_write(Relation root, List *attnums)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "gp_core merged statistics",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	TupleDesc	tupdesc = RelationGetDescr(root);

	foreach_int(attnum, attnums)
	{
		VacAttrStats *stats = palloc0_object(VacAttrStats);
		int			target;

		if (!attribute_is_analyzable(root, attnum, TupleDescAttr(tupdesc, attnum - 1),
									 &target))
			continue;
		if (merge_column(root, attnum, target < 0 ? default_statistics_target : target,
						 stats))
			write_root_stats(RelationGetRelid(root), attnum, stats);
		MemoryContextReset(cxt);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	CommandCounterIncrement();
}

/*
 * The root's statistics of these columns merged and written, for ANALYZE
 * of the root in place of a sample; the rows its leaves have, which
 * PostgreSQL writes as the root's.
 */
double
GpRootMerge(Relation root, List *attnums)
{
	double		totalrows = 0;

	merge_and_write(root, attnums);
	foreach_oid(relid, find_all_inheritors(RelationGetRelid(root), NoLock, NULL))
	{
		if (get_rel_relkind(relid) == RELKIND_RELATION)
			totalrows += leaf_reltuples(relid);
	}
	return totalrows;
}

/*
 * Can the merged most common values of a column be counted by hashing its
 * values, as Cloudberry's op_hashjoinable() finds?  Cloudberry's catalog
 * marks money's, bit's and varbit's "=" hashable, which PostgreSQL 19's
 * does not; gp_core gives the three the hash classes Cloudberry's has
 * (gp_core--1.0.sql), so a hash function of the type's own is taken too --
 * not a container's, an array's or a record's, which hashes the elements
 * by their types' default classes, as PostgreSQL finds.
 */
static bool
hashable(Oid eqopr, Oid typid)
{
	RegProcedure hashfn;

	if (!OidIsValid(eqopr))
		return false;
	if (op_hashjoinable(eqopr, typid))
		return true;
	return get_op_hash_functions(eqopr, &hashfn, NULL) &&
		hashfn != F_HASH_ARRAY && hashfn != F_HASH_RECORD &&
		hashfn != F_HASH_RANGE && hashfn != F_HASH_MULTIRANGE;
}

/* Does the type's statistics come of PostgreSQL's standard typanalyze? */
static bool
standard_typanalyze(Oid typid)
{
	HeapTuple	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	Oid			typanalyze;

	if (!HeapTupleIsValid(tp))
		return false;
	typanalyze = ((Form_pg_type) GETSTRUCT(tp))->typanalyze;
	ReleaseSysCache(tp);
	return !OidIsValid(typanalyze) || typanalyze == F_ARRAY_TYPANALYZE;
}

/*
 * The columns of a root partitioned table ANALYZE takes whose statistics
 * can be merged from its leaves', and in *all whether that is every one of
 * them and the root has no extended statistics, which need a sample -- as
 * Cloudberry's std_typanalyze() and needs_sample() find them: a column
 * analyzed by the standard functions, every leaf having statistics of it
 * (leaf_parts_analyzed(), which says at elevel where not), and its "="
 * hashable (see hashable()).
 */
List *
GpRootMergeableColumns(Relation root, List *va_cols, int elevel, bool *all)
{
	List	   *targets;
	List	   *attnums = analyzed_columns(root, va_cols, &targets);
	List	   *mergeable = NIL;
	List	   *statext = RelationGetStatExtList(root);

	*all = statext == NIL;
	list_free(statext);
	foreach_int(attnum, attnums)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(root), attnum - 1);
		Oid			ltopr;
		Oid			eqopr;

		if (!standard_typanalyze(att->atttypid))
		{
			*all = false;
			continue;
		}
		get_sort_group_operators(att->atttypid, false, false, false, &ltopr, &eqopr,
								 NULL, NULL);
		if (GpLeafPartsAnalyzed(RelationGetRelid(root), InvalidOid,
								list_make1(makeString(pstrdup(NameStr(att->attname)))),
								elevel) &&
			hashable(eqopr, att->atttypid))
			mergeable = lappend_int(mergeable, attnum);
		else
			*all = false;
	}
	return mergeable;
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                     */
/* ------------------------------------------------------------------------- */

/* A relation dropped, or a column: its counters go. */
static void
partmerge_object_access(ObjectAccessType access, Oid classId, Oid objectId,
						int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access == OAT_DROP && classId == RelationRelationId &&
		get_rel_relkind(objectId) == RELKIND_RELATION && IsTransactionState())
	{
		Oid			relid = counters_table();
		Relation	rel;
		ScanKeyData key[2];
		SysScanDesc scan;
		HeapTuple	tuple;

		if (!OidIsValid(relid) || relid == objectId)
			return;
		rel = table_open(relid, RowExclusiveLock);
		ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectId));
		ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(subId));
		scan = systable_beginscan(rel, linitial_oid(RelationGetIndexList(rel)), true,
								  NULL, subId != 0 ? 2 : 1, key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			CatalogTupleDelete(rel, &tuple->t_self);
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
	}
}

void
GpPartMergeInit(void)
{
	RegisterXactCallback(partmerge_xact_callback, NULL);
	RegisterSubXactCallback(partmerge_subxact_callback, NULL);
	prev_object_access_hook = object_access_hook;
	object_access_hook = partmerge_object_access;
}
