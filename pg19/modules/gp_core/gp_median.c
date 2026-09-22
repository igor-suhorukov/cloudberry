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
 * gp_median.c
 *	  median(), Cloudberry's, as a plain aggregate.
 *
 * Cloudberry's grammar makes MEDIAN(x) the ordered-set aggregate
 * median(0.5) WITHIN GROUP (ORDER BY x), whose final functions are
 * percentile_cont's (github/cloudberry/src/backend/parser/gram.y:19453-19480,
 * src/include/catalog/pg_aggregate.dat:721-735).  PostgreSQL 19 has no
 * MEDIAN keyword, so median(x) already parses as a function call, and what
 * it calls here is a plain one-argument aggregate.  The answer is the same;
 * three things around it are not:
 *
 *   - ORCA plans it.  Its core turns every ordered-set aggregate over a
 *     column into a gp_percentile_* aggregate named by a fixed OID that no
 *     PostgreSQL 19 catalog has, so it declines those, and a query with
 *     Cloudberry's median in it would go to the planner.  A plain aggregate
 *     it plans like any other.
 *   - A view prints it as median(x), which is how it was written.
 *   - It runs over a window.  Cloudberry's grammar has no OVER after MEDIAN
 *     (...), so there median(x) OVER (...) is a syntax error.
 *
 * The answer is percentile_cont(0.5)'s, computed the way percentile_cont
 * computes it: the non-null rows go into a tuplesort, which spills to disk
 * past work_mem as an ordered-set aggregate's does, and the final function
 * takes the middle one, or interpolates halfway between the middle two with
 * Cloudberry's interpolation for the type.
 *
 * Over a window it is computed another way, below: a window calls the final
 * function for every row and goes on adding rows to the same state, which a
 * sorted tuplesort cannot take.
 *
 * Cloudberry source this file is made of:
 *	  src/backend/utils/adt/orderedsetaggs.c: percentile_cont_final_common(),
 *	  and float8_lerp(), interval_lerp(), timestamp_lerp() and
 *	  timestamptz_lerp(), which interpolate
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

/*
 * One group's rows, as the transition function collects them.  It lives in
 * the aggregate's per-group memory context, and the tuplesort in it is ended
 * by the callback registered when it is made, at the end of the group.
 */
typedef struct GpMedianState
{
	Oid			typid;			/* float8, interval, timestamp or timestamptz */
	Tuplesortstate *sort;
	int64		nrows;			/* non-null rows put into the sort */
	bool		sorted;			/* has tuplesort_performsort run? */
} GpMedianState;

static void
median_shutdown(Datum arg)
{
	GpMedianState *state = (GpMedianState *) DatumGetPointer(arg);

	if (state->sort != NULL)
		tuplesort_end(state->sort);
	state->sort = NULL;
}

/*
 * The first row of a group.  The type is the aggregate's argument type, which
 * is the one thing the four transition functions do not share.
 */
static GpMedianState *
median_startup(FunctionCallInfo fcinfo, MemoryContext aggcontext)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	TypeCacheEntry *tce;
	GpMedianState *state;
	MemoryContext oldcontext;
	int			sortopt = TUPLESORT_NONE;

	if (typid != FLOAT8OID && typid != INTERVALOID &&
		typid != TIMESTAMPOID && typid != TIMESTAMPTZOID)
		elog(ERROR, "median() is not defined for type %s",
			 format_type_be(typid));

	tce = lookup_type_cache(typid, TYPECACHE_LT_OPR);
	if (!OidIsValid(tce->lt_opr))
		elog(ERROR, "could not identify an ordering operator for type %s",
			 format_type_be(typid));

	/*
	 * Two identical median() calls in one query share one state, and each
	 * calls the final function on it, so the sort has to be readable twice.
	 * This is how percentile_cont decides it (ordered_set_startup()).
	 */
	if (AggStateIsShared(fcinfo))
		sortopt |= TUPLESORT_RANDOMACCESS;

	oldcontext = MemoryContextSwitchTo(aggcontext);

	state = palloc_object(GpMedianState);
	state->typid = typid;
	state->sort = tuplesort_begin_datum(typid, tce->lt_opr, InvalidOid, false,
										work_mem, NULL, sortopt);
	state->nrows = 0;
	state->sorted = false;

	MemoryContextSwitchTo(oldcontext);

	AggRegisterCallback(fcinfo, median_shutdown, PointerGetDatum(state));

	return state;
}

PG_FUNCTION_INFO_V1(gp_median_transfn);
PG_FUNCTION_INFO_V1(gp_median_finalfn);

/*
 * gp.median_transfn(internal, float8 | interval | timestamp | timestamptz)
 *
 * Not strict, so that it sees the group's first row whatever it is and makes
 * the state then; a null row is not counted, as percentile_cont counts none.
 */
Datum
gp_median_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggcontext;
	GpMedianState *state;

	if (AggCheckCallContext(fcinfo, &aggcontext) != AGG_CONTEXT_AGGREGATE)
		elog(ERROR, "median() called in non-aggregate context");

	if (PG_ARGISNULL(0))
		state = median_startup(fcinfo, aggcontext);
	else
		state = (GpMedianState *) PG_GETARG_POINTER(0);

	if (!PG_ARGISNULL(1))
	{
		tuplesort_putdatum(state->sort, PG_GETARG_DATUM(1), false);
		state->nrows++;
	}

	PG_RETURN_POINTER(state);
}

/*
 * Halfway between two timestamps, as Cloudberry's timestamp_lerp() and
 * timestamptz_lerp() compute a point between them: the difference, scaled and
 * rounded, added to the lower one.
 *
 * Cloudberry subtracts without looking, which overflows for an infinite
 * timestamp and for two finite ones further apart than an int64 counts; this
 * does not.  Halfway between a timestamp and an infinity is that infinity,
 * and between the two infinities there is no answer, which is what
 * PostgreSQL 19's interval arithmetic says of the same question.
 */
static Timestamp
timestamp_halfway(Timestamp lo, Timestamp hi, double pct)
{
	int64		diff;

	if (lo == hi)
		return lo;
	if (TIMESTAMP_IS_NOBEGIN(lo) && TIMESTAMP_IS_NOEND(hi))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	if (TIMESTAMP_IS_NOBEGIN(lo))
		return lo;
	if (TIMESTAMP_IS_NOEND(hi))
		return hi;

	if (pg_sub_s64_overflow(hi, lo, &diff))
		return lo + (Timestamp) round(((double) hi - (double) lo) * pct);

	return lo + (Timestamp) round(diff * pct);
}

/*
 * The point `pct` of the way from lo to hi, for the median's type.  Each is
 * Cloudberry's own: float8_lerp() and interval_lerp() as they are, the
 * timestamps through timestamp_halfway().
 */
static Datum
median_lerp(Oid typid, Datum lo, Datum hi, double pct)
{
	switch (typid)
	{
		case FLOAT8OID:
			{
				double		loval = DatumGetFloat8(lo);
				double		hival = DatumGetFloat8(hi);

				return Float8GetDatum(loval + (pct * (hival - loval)));
			}
		case INTERVALOID:
			{
				Datum		diff = DirectFunctionCall2(interval_mi, hi, lo);
				Datum		mul = DirectFunctionCall2(interval_mul, diff,
													  Float8GetDatum(pct));

				return DirectFunctionCall2(interval_pl, mul, lo);
			}
		case TIMESTAMPOID:
			return TimestampGetDatum(timestamp_halfway(DatumGetTimestamp(lo),
													   DatumGetTimestamp(hi),
													   pct));
		case TIMESTAMPTZOID:
			return TimestampTzGetDatum(timestamp_halfway(DatumGetTimestampTz(lo),
														 DatumGetTimestampTz(hi),
														 pct));
		default:
			elog(ERROR, "median() is not defined for type %s",
				 format_type_be(typid));
	}

	return (Datum) 0;			/* keep compiler quiet */
}

/*
 * gp.median_<type>_final(internal)
 *
 * percentile_cont_final_common() with the percentile fixed at 0.5: the row at
 * floor(0.5 * (n - 1)) and the one at ceil(0.5 * (n - 1)), which are the same
 * row when n is odd and the two middle ones when it is even, where the answer
 * is halfway between them.  NULL for a group with no rows or only null ones.
 *
 * Declared FINALFUNC_MODIFY = SHAREABLE: this sorts the state, after which no
 * row can be added, but it may be called again on the same state, which
 * reads the sort again.
 */
Datum
gp_median_finalfn(PG_FUNCTION_ARGS)
{
	GpMedianState *state;
	int64		first_row;
	int64		second_row;
	Datum		first_val;
	Datum		second_val;
	bool		isnull;

	Assert(AggCheckCallContext(fcinfo, NULL) == AGG_CONTEXT_AGGREGATE);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	state = (GpMedianState *) PG_GETARG_POINTER(0);
	if (state->nrows == 0)
		PG_RETURN_NULL();

	if (!state->sorted)
	{
		tuplesort_performsort(state->sort);
		state->sorted = true;
	}
	else
		tuplesort_rescan(state->sort);

	first_row = floor(0.5 * (state->nrows - 1));
	second_row = ceil(0.5 * (state->nrows - 1));

	if (!tuplesort_skiptuples(state->sort, first_row, true))
		elog(ERROR, "missing row in median");
	if (!tuplesort_getdatum(state->sort, true, true, &first_val, &isnull, NULL))
		elog(ERROR, "missing row in median");

	if (first_row == second_row)
		PG_RETURN_DATUM(first_val);

	if (!tuplesort_getdatum(state->sort, true, true, &second_val, &isnull, NULL))
		elog(ERROR, "missing row in median");

	PG_RETURN_DATUM(median_lerp(state->typid, first_val, second_val,
								0.5 * (state->nrows - 1) - first_row));
}

/* ------------------------------------------------------------------------- */
/* Over a window: the moving-aggregate implementation                        */
/* ------------------------------------------------------------------------- */

/*
 * PostgreSQL runs an aggregate over a window only if its final function
 * leaves the state as it found it (nodeWindowAgg.c, initialize_peragg): the
 * window calls it for each row, and then goes on adding the next rows to the
 * same state.  The final function above sorts the tuplesort, after which no
 * row can be added.  So the aggregates also carry a moving-aggregate
 * implementation, whose final function only reads; and because its final
 * function is READ_ONLY where the plain one is not, a window takes it for
 * every frame, moving or not ("decision forced by safety"), while GROUP BY
 * goes on sorting, and spilling past work_mem.
 *
 * The state is the frame's non-null values in two heaps: the lower half in
 * a max-heap, the upper half in a min-heap, the lower holding the one more
 * when the count is odd.  The median is then the lower heap's top, or halfway
 * between the two tops -- the middle row, or the two middle rows, of the
 * frame sorted, which is what percentile_cont(0.5) reads.  A row entering
 * the frame is pushed into one heap and the two rebalanced; one leaving is
 * taken out of the heap it is in.  Each costs a logarithm of the frame, so a
 * running median over a long partition, whose frame only grows, stays cheap.
 *
 * Which value is leaving is known without looking for it.  A window takes
 * rows out of a frame in the order it put them in, from the frame's head, so
 * the value leaving is the oldest, which a queue keeps in front with its
 * place in its heap.  The inverse function checks that it is the same bytes
 * it is given, which is what a volatile argument breaks -- evaluated again as
 * the row leaves, it is another value -- and answers NULL then, on which the
 * window aggregates the frame again from nothing.
 */
typedef struct MedianItem
{
	Datum		value;			/* a copy, in the state's context */
	int			pos;			/* where it is in its heap */
	bool		low;			/* in the lower half's heap */
} MedianItem;

typedef struct MedianHeaps
{
	Oid			typid;
	int16		typlen;
	bool		typbyval;
	SortSupportData ssup;		/* the type's btree ordering, tuplesort's own */
	MemoryContext cxt;			/* the window aggregate's context */
	MedianItem **low;			/* max-heap of the lower half */
	int			nlow;
	int			maxlow;
	MedianItem **high;			/* min-heap of the upper half */
	int			nhigh;
	int			maxhigh;
	MedianItem **queue;			/* ring of the items, oldest at qhead */
	int			qhead;
	int			qlen;
	int			qmax;
} MedianHeaps;

/* Does a belong above b in the heap of that half? */
static inline bool
item_above(MedianHeaps *mh, bool low, const MedianItem *a, const MedianItem *b)
{
	int			c = ApplySortComparator(a->value, false, b->value, false, &mh->ssup);

	return low ? (c > 0) : (c < 0);
}

static inline void
heap_place(MedianItem **heap, int i, MedianItem *it)
{
	heap[i] = it;
	it->pos = i;
}

static void
heap_sift_up(MedianHeaps *mh, bool low, int i)
{
	MedianItem **heap = low ? mh->low : mh->high;
	MedianItem *it = heap[i];

	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (!item_above(mh, low, it, heap[parent]))
			break;
		heap_place(heap, i, heap[parent]);
		i = parent;
	}
	heap_place(heap, i, it);
}

static void
heap_sift_down(MedianHeaps *mh, bool low, int i)
{
	MedianItem **heap = low ? mh->low : mh->high;
	int			n = low ? mh->nlow : mh->nhigh;
	MedianItem *it = heap[i];

	for (;;)
	{
		int			child = 2 * i + 1;

		if (child >= n)
			break;
		if (child + 1 < n && item_above(mh, low, heap[child + 1], heap[child]))
			child++;
		if (!item_above(mh, low, heap[child], it))
			break;
		heap_place(heap, i, heap[child]);
		i = child;
	}
	heap_place(heap, i, it);
}

static void
heap_push(MedianHeaps *mh, bool low, MedianItem *it)
{
	MedianItem ***heap = low ? &mh->low : &mh->high;
	int		   *n = low ? &mh->nlow : &mh->nhigh;
	int		   *max = low ? &mh->maxlow : &mh->maxhigh;

	if (*n == *max)
	{
		*max *= 2;
		*heap = repalloc(*heap, *max * sizeof(MedianItem *));
	}
	it->low = low;
	heap_place(*heap, (*n)++, it);
	heap_sift_up(mh, low, *n - 1);
}

/* Take the item at position i out of its heap. */
static MedianItem *
heap_take(MedianHeaps *mh, bool low, int i)
{
	MedianItem **heap = low ? mh->low : mh->high;
	int		   *n = low ? &mh->nlow : &mh->nhigh;
	MedianItem *it = heap[i];
	MedianItem *last = heap[--(*n)];

	if (i < *n)
	{
		/* the last item fills the hole, and may belong above it or below */
		heap_place(heap, i, last);
		heap_sift_up(mh, low, i);
		heap_sift_down(mh, low, last->pos);
	}
	return it;
}

/* The lower half holds as many as the upper, or one more. */
static void
heaps_rebalance(MedianHeaps *mh)
{
	if (mh->nlow > mh->nhigh + 1)
		heap_push(mh, false, heap_take(mh, true, 0));
	else if (mh->nhigh > mh->nlow)
		heap_push(mh, true, heap_take(mh, false, 0));
}

static MedianHeaps *
heaps_create(FunctionCallInfo fcinfo, MemoryContext aggcontext)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	TypeCacheEntry *tce;
	MedianHeaps *mh;
	MemoryContext oldcontext;

	if (typid != FLOAT8OID && typid != INTERVALOID &&
		typid != TIMESTAMPOID && typid != TIMESTAMPTZOID)
		elog(ERROR, "median() is not defined for type %s",
			 format_type_be(typid));

	tce = lookup_type_cache(typid, TYPECACHE_LT_OPR);
	if (!OidIsValid(tce->lt_opr))
		elog(ERROR, "could not identify an ordering operator for type %s",
			 format_type_be(typid));

	oldcontext = MemoryContextSwitchTo(aggcontext);

	mh = palloc0_object(MedianHeaps);
	mh->typid = typid;
	get_typlenbyval(typid, &mh->typlen, &mh->typbyval);
	mh->ssup.ssup_cxt = aggcontext;
	mh->ssup.ssup_collation = InvalidOid;
	mh->ssup.ssup_nulls_first = false;
	PrepareSortSupportFromOrderingOp(tce->lt_opr, &mh->ssup);
	mh->cxt = aggcontext;
	mh->maxlow = mh->maxhigh = mh->qmax = 16;
	mh->low = palloc(mh->maxlow * sizeof(MedianItem *));
	mh->high = palloc(mh->maxhigh * sizeof(MedianItem *));
	mh->queue = palloc(mh->qmax * sizeof(MedianItem *));

	MemoryContextSwitchTo(oldcontext);

	return mh;
}

static void
heaps_add(MedianHeaps *mh, Datum value)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(mh->cxt);
	MedianItem *it = palloc_object(MedianItem);

	it->value = datumCopy(value, mh->typbyval, mh->typlen);
	if (mh->nlow == 0 ||
		ApplySortComparator(it->value, false, mh->low[0]->value, false,
							&mh->ssup) <= 0)
		heap_push(mh, true, it);
	else
		heap_push(mh, false, it);
	heaps_rebalance(mh);

	/* at the back of the queue, the ring unrolled when it is full */
	if (mh->qlen == mh->qmax)
	{
		MedianItem **queue = palloc(mh->qmax * 2 * sizeof(MedianItem *));

		for (int k = 0; k < mh->qlen; k++)
			queue[k] = mh->queue[(mh->qhead + k) % mh->qmax];
		pfree(mh->queue);
		mh->queue = queue;
		mh->qhead = 0;
		mh->qmax *= 2;
	}
	mh->queue[(mh->qhead + mh->qlen++) % mh->qmax] = it;

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Take out the oldest value, which has to be `value`, byte for byte; false,
 * with nothing changed, if it is not.
 */
static bool
heaps_remove(MedianHeaps *mh, Datum value)
{
	MedianItem *it;

	if (mh->qlen == 0)
		return false;
	it = mh->queue[mh->qhead];
	if (!datumIsEqual(it->value, value, mh->typbyval, mh->typlen))
		return false;

	mh->qhead = (mh->qhead + 1) % mh->qmax;
	mh->qlen--;
	(void) heap_take(mh, it->low, it->pos);
	heaps_rebalance(mh);

	if (!mh->typbyval)
		pfree(DatumGetPointer(it->value));
	pfree(it);
	return true;
}

PG_FUNCTION_INFO_V1(gp_median_mtransfn);
PG_FUNCTION_INFO_V1(gp_median_minvfn);
PG_FUNCTION_INFO_V1(gp_median_mfinalfn);

/*
 * gp.median_mtransfn(internal, float8 | interval | timestamp | timestamptz)
 *
 * A row entering the frame.  Not strict, like the plain one, and so neither
 * is the inverse, which PostgreSQL requires of the pair: a null row is not
 * counted, and nothing is taken out for it when it leaves.
 */
Datum
gp_median_mtransfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggcontext;
	MedianHeaps *mh;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "median() called in non-aggregate context");

	if (PG_ARGISNULL(0))
		mh = heaps_create(fcinfo, aggcontext);
	else
		mh = (MedianHeaps *) PG_GETARG_POINTER(0);

	if (!PG_ARGISNULL(1))
		heaps_add(mh, PG_GETARG_DATUM(1));

	PG_RETURN_POINTER(mh);
}

/*
 * gp.median_minvfn(internal, float8 | interval | timestamp | timestamptz)
 *
 * A row leaving the frame, which is the oldest in it.  NULL -- "cannot take
 * it out" -- if the value is not the oldest one's, and the window starts the
 * frame again.
 */
Datum
gp_median_minvfn(PG_FUNCTION_ARGS)
{
	MedianHeaps *mh;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "median() called in non-aggregate context");

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	mh = (MedianHeaps *) PG_GETARG_POINTER(0);

	if (!PG_ARGISNULL(1) && !heaps_remove(mh, PG_GETARG_DATUM(1)))
		PG_RETURN_NULL();

	PG_RETURN_POINTER(mh);
}

/*
 * gp.median_<type>_mfinal(internal)
 *
 * The median of the frame, read from the tops of the heaps and changing
 * neither: MFINALFUNC_MODIFY = READ_ONLY, which is what lets a window call it
 * and go on.  The value is a copy, so that none of the state is handed out.
 */
Datum
gp_median_mfinalfn(PG_FUNCTION_ARGS)
{
	MedianHeaps *mh;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	mh = (MedianHeaps *) PG_GETARG_POINTER(0);

	if (mh->nlow == 0)
		PG_RETURN_NULL();
	if (mh->nlow > mh->nhigh)
		PG_RETURN_DATUM(datumCopy(mh->low[0]->value, mh->typbyval, mh->typlen));

	PG_RETURN_DATUM(median_lerp(mh->typid, mh->low[0]->value,
								mh->high[0]->value, 0.5));
}
