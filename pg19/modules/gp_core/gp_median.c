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
 *   - OVER is refused when the query starts rather than when it is parsed.
 *     The final function sorts the rows it was given, so it is declared to
 *     modify its state, and PostgreSQL will not run such an aggregate over a
 *     window; Cloudberry refuses OVER for every ordered-set aggregate.
 *
 * The answer is percentile_cont(0.5)'s, computed the way percentile_cont
 * computes it: the non-null rows go into a tuplesort, which spills to disk
 * past work_mem as an ordered-set aggregate's does, and the final function
 * takes the middle one, or interpolates halfway between the middle two with
 * Cloudberry's interpolation for the type.
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
