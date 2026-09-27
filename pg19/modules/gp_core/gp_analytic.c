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
 * gp_analytic.c
 *	  Cloudberry's functions of arrays and of time series, which PostgreSQL
 *	  19 has not: sum() of an array, element by element; linear_interpolate();
 *	  interval_bound(); and one interval divided by another, and its
 *	  remainder, as the operators / and %.
 *
 * Each answers as Cloudberry's does, in its words; gp_core--1.0.sql declares
 * them in pg_catalog, under Cloudberry's names.  Where Cloudberry reaches
 * into the arithmetic of numeric or of a timestamp, which PostgreSQL keeps
 * to itself, this asks PostgreSQL's own functions for the same step --
 * numeric_div() divides at the scale div_var() does in Cloudberry's
 * numeric_interval_bound_common(), timestamp_pl_interval() adds as
 * Cloudberry's timestamp_offset_internal() does.
 *
 * An interval is one count of microseconds here, a month taken as 30 days,
 * as Cloudberry's interval_div_internal() counts it -- in 64 bits, as it
 * does: an interval past some 292,000 years, or an infinite one, which
 * Cloudberry has not, is out of range.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/adt/matrix.c: matrix_add(), for the aggregates
 *	  src/backend/utils/adt/interpolate.c
 *	  src/backend/utils/adt/timestamp.c: interval_div_internal(),
 *		interval_interval_div(), interval_interval_mod(),
 *		timestamp[tz]_interval_bound*(), timestamp[tz]_li_fraction() and
 *		_li_value(), interval_li_fraction() and _li_value()
 *	  src/backend/utils/adt/date.c: time_li_fraction(), time_li_value()
 *	  src/backend/utils/adt/numeric.c: numeric_interval_bound*(),
 *		numeric_li_fraction(), numeric_li_value()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

/* Cloudberry's SQLSTATE of a width not positive, which PostgreSQL has not */
#define ERRCODE_INVALID_INTERVAL_WIDTH	MAKE_SQLSTATE('2','2','0','0','A')

/* ------------------------------------------------------------------------- */
/* sum() of an array                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The transition and combine function of sum(int2[]), sum(int4[]),
 * sum(int8[]) and sum(float8[]): the state is an int8[] or a float8[] of the
 * input's dimensions, to which each input array is added element by
 * element -- Cloudberry's int2_matrix_accum(), int4_matrix_accum(),
 * int8_matrix_accum() and float8_matrix_accum(), all its matrix_add().  An
 * array of float4 or of numeric comes as float8[], as it does to
 * Cloudberry's, by the implicit casts.
 *
 * A null input, or one of no dimensions, leaves the state as it was.  Arrays
 * of other dimensions than the state's, or with a null element, are refused
 * in Cloudberry's words.  Inside an aggregate the state is changed in place,
 * as Cloudberry's transition function changes it; called on its own the
 * function makes a new array.
 */
static void
matrix_elements_check(ArrayType *n)
{
	if (ARR_HASNULL(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("matrix_add: null array element not allowed in this context")));
}

PG_FUNCTION_INFO_V1(gp_matrix_accum);

Datum
gp_matrix_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *m;
	ArrayType  *n;
	Oid			mtype;
	Oid			ntype;
	int			ndim;
	int			len;
	bool		in_agg = AggCheckCallContext(fcinfo, NULL) != 0;

	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	}

	n = PG_GETARG_ARRAYTYPE_P(1);
	ndim = ARR_NDIM(n);
	ntype = ARR_ELEMTYPE(n);

	if (ndim == 0)
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P(0));
	}

	if (ntype != INT2OID && ntype != INT4OID && ntype != INT8OID &&
		ntype != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("matrix_add: unsupported datatype")));
	matrix_elements_check(n);
	len = ArrayGetNItems(ndim, ARR_DIMS(n));

	if (PG_ARGISNULL(0))
	{
		/* The state: the input's shape, of int8 or of float8, all zero. */
		int			elsize;
		Size		size;

		mtype = (ntype == FLOAT8OID) ? FLOAT8OID : INT8OID;
		elsize = (mtype == FLOAT8OID) ? sizeof(float8) : sizeof(int64);
		size = ARR_OVERHEAD_NONULLS(ndim) + (Size) len * elsize;
		m = (ArrayType *) palloc0(size);
		SET_VARSIZE(m, size);
		m->ndim = ndim;
		m->dataoffset = 0;
		m->elemtype = mtype;
		for (int i = 0; i < ndim; i++)
		{
			ARR_DIMS(m)[i] = ARR_DIMS(n)[i];
			ARR_LBOUND(m)[i] = 1;
		}
	}
	else
	{
		m = PG_GETARG_ARRAYTYPE_P(0);
		mtype = ARR_ELEMTYPE(m);
		if (ndim != ARR_NDIM(m))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("matrix_add: Dimensionality of both arrays must match")));
		for (int i = 0; i < ndim; i++)
			if (ARR_DIMS(m)[i] != ARR_DIMS(n)[i])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("matrix_add: non-conformable arrays")));
		matrix_elements_check(m);
		if (mtype != INT8OID && mtype != FLOAT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("matrix_add: unsupported datatype")));
		if (mtype == INT8OID && ntype == FLOAT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("matrix_add: can not downconvert state")));
		/* outside an aggregate, the argument is not ours to change */
		if (!in_agg)
			m = (ArrayType *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0));
	}

	if (mtype == INT8OID)
	{
		int64	   *data_m = (int64 *) ARR_DATA_PTR(m);

		for (int i = 0; i < len; i++)
		{
			int64		addend;

			switch (ntype)
			{
				case INT2OID:
					addend = ((int16 *) ARR_DATA_PTR(n))[i];
					break;
				case INT4OID:
					addend = ((int32 *) ARR_DATA_PTR(n))[i];
					break;
				default:
					addend = ((int64 *) ARR_DATA_PTR(n))[i];
					break;
			}
			if (pg_add_s64_overflow(data_m[i], addend, &data_m[i]))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("int value out of range: overflow")));
		}
	}
	else
	{
		float8	   *data_m = (float8 *) ARR_DATA_PTR(m);

		for (int i = 0; i < len; i++)
		{
			float8		addend;
			float8		sum;

			switch (ntype)
			{
				case INT2OID:
					addend = ((int16 *) ARR_DATA_PTR(n))[i];
					break;
				case INT4OID:
					addend = ((int32 *) ARR_DATA_PTR(n))[i];
					break;
				case INT8OID:
					addend = (float8) ((int64 *) ARR_DATA_PTR(n))[i];
					break;
				default:
					addend = ((float8 *) ARR_DATA_PTR(n))[i];
					break;
			}
			sum = data_m[i] + addend;
			if (isinf(sum) && !isinf(data_m[i]) && !isinf(addend))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value out of range: overflow")));
			data_m[i] = sum;
		}
	}

	PG_RETURN_ARRAYTYPE_P(m);
}

/* ------------------------------------------------------------------------- */
/* An interval divided by another                                            */
/* ------------------------------------------------------------------------- */

/* The interval as microseconds, a month as 30 days (see the file's head). */
static int64
interval_usecs(const Interval *iv)
{
	int64		days = (int64) iv->month * DAYS_PER_MONTH + iv->day;
	int64		result;

	if (INTERVAL_NOT_FINITE(iv) ||
		pg_mul_s64_overflow(days, USECS_PER_DAY, &result) ||
		pg_add_s64_overflow(result, iv->time, &result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	return result;
}

/* Refuse an infinite interval, which interval arithmetic below cannot take. */
static void
interval_finite_check(const Interval *iv)
{
	if (INTERVAL_NOT_FINITE(iv))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
}

/*
 * Cloudberry's interval_div_internal(): the quotient, and the remainder, of
 * one span by another -- a remainder of the dividend's sign, in microseconds
 * alone.  False when the divisor is none.
 */
static bool
interval_div_internal(const Interval *dividend, const Interval *divisor,
					  float8 *quo, Interval *rem)
{
	int64		span1 = interval_usecs(dividend);
	int64		span2 = interval_usecs(divisor);

	if (span2 == 0)
		return false;

	if (quo)
		*quo = (float8) span1 / (float8) span2;
	if (rem)
	{
		/* span1 % -1 is 0, and would trap for PG_INT64_MIN */
		rem->time = (span2 == -1) ? 0 : span1 % span2;
		rem->day = 0;
		rem->month = 0;
	}
	return true;
}

PG_FUNCTION_INFO_V1(gp_interval_interval_div);
PG_FUNCTION_INFO_V1(gp_interval_interval_mod);

/* interval_interval_div(interval, interval), the operator / */
Datum
gp_interval_interval_div(PG_FUNCTION_ARGS)
{
	Interval   *dividend = PG_GETARG_INTERVAL_P(0);
	Interval   *divisor = PG_GETARG_INTERVAL_P(1);
	float8		result = 0.0;

	if (!interval_div_internal(dividend, divisor, &result, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	PG_RETURN_FLOAT8(result);
}

/*
 * interval_interval_mod(interval, interval), the operator %: the remainder,
 * as Cloudberry names it after numeric_mod.
 */
Datum
gp_interval_interval_mod(PG_FUNCTION_ARGS)
{
	Interval   *dividend = PG_GETARG_INTERVAL_P(0);
	Interval   *divisor = PG_GETARG_INTERVAL_P(1);
	Interval   *result = palloc0_object(Interval);

	if (!interval_div_internal(dividend, divisor, NULL, result))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	PG_RETURN_INTERVAL_P(result);
}

/* ------------------------------------------------------------------------- */
/* interval_bound()                                                          */
/* ------------------------------------------------------------------------- */

/*
 * A timestamp and the interval `unit` taken `mul` times, as PostgreSQL adds
 * them: months, then days in the session's time zone for a timestamptz, then
 * the time -- Cloudberry's timestamp[tz]_offset_multiple().
 */
static Timestamp
offset_multiple(bool tz, Timestamp base, const Interval *unit, int64 mul)
{
	Interval	span;
	int64		month;
	int64		day;

	if (pg_mul_s64_overflow(unit->month, mul, &month) ||
		pg_mul_s64_overflow(unit->day, mul, &day) ||
		pg_mul_s64_overflow(unit->time, mul, &span.time) ||
		month < PG_INT32_MIN || month > PG_INT32_MAX ||
		day < PG_INT32_MIN || day > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	span.month = (int32) month;
	span.day = (int32) day;

	return DatumGetTimestamp(DirectFunctionCall2(tz ? timestamptz_pl_interval :
												 timestamp_pl_interval,
												 TimestampGetDatum(base),
												 IntervalPGetDatum(&span)));
}

/*
 * The lower bound of the interval of width `width`, counted from `reg`,
 * that holds `val`, moved `shift` widths on: Cloudberry's
 * timestamp_interval_bound_common() and timestamptz_interval_bound_common(),
 * which are one but for the time zone the widths are added in.  A guess from
 * the quotient of the spans, then steps to the interval that holds it.
 */
static Timestamp
interval_bound_common(bool tz, Timestamp val, const Interval *width,
					  int32 shift, Timestamp reg)
{
	int64		index;
	float8		quo = 0.0;
	Interval	span;
	Timestamp	low;
	Timestamp	high;

	/* Insist on a positive width. */
	if (INTERVAL_NOT_FINITE(width) || interval_usecs(width) <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_INTERVAL_WIDTH),
				 errmsg("width of time interval not positive")));

	/* A timestamp that is not finite is its own bound. */
	if (TIMESTAMP_NOT_FINITE(val))
		return val;

	if (TIMESTAMP_NOT_FINITE(reg))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("bound for registration is not finite")));

	/* How many widths val is from reg, estimated. */
	span.month = 0;
	span.day = 0;
	if (pg_sub_s64_overflow(val, reg, &span.time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	if (!interval_div_internal(&span, width, &quo, NULL))
		elog(ERROR, "invalid call to interval_div_internal");
	index = (int64) quo;

	/* Search for a satisfactory bound. */
	for (int safety = 64;; safety--)
	{
		if (safety <= 0)
			elog(ERROR, "interval_bound failed to converge");

		low = offset_multiple(tz, reg, width, index);
		high = offset_multiple(tz, low, width, 1);

		Assert(high > low);

		if (val >= high)
		{
			span.time = val - high;
			if (!interval_div_internal(&span, width, &quo, NULL))
				elog(ERROR, "invalid call to interval_div_internal");
			index += ((int64) quo > 0) ? (int64) quo : 1;
		}
		else if (val < low)
		{
			span.time = low - val;
			if (!interval_div_internal(&span, width, &quo, NULL))
				elog(ERROR, "invalid call to interval_div_internal");
			index -= ((int64) quo > 0) ? (int64) quo : 1;
		}
		else
			break;
	}

	/* If asked, the interval shift widths on. */
	if (shift)
		low = offset_multiple(tz, reg, width, index + shift);

	return low;
}

/*
 * interval_bound(timestamp[tz], interval [, int [, timestamp[tz]]]): the
 * shift 0 and the registration the epoch where they are not given, or are
 * NULL; NULL where the value or the width is.
 */
static Datum
interval_bound_args(FunctionCallInfo fcinfo, bool tz)
{
	int32		shift = 0;
	Timestamp	reg = SetEpochTimestamp();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		shift = PG_GETARG_INT32(2);
	if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
		reg = PG_GETARG_TIMESTAMP(3);

	PG_RETURN_TIMESTAMP(interval_bound_common(tz, PG_GETARG_TIMESTAMP(0),
											  PG_GETARG_INTERVAL_P(1),
											  shift, reg));
}

PG_FUNCTION_INFO_V1(gp_timestamp_interval_bound);
PG_FUNCTION_INFO_V1(gp_timestamptz_interval_bound);

Datum
gp_timestamp_interval_bound(PG_FUNCTION_ARGS)
{
	return interval_bound_args(fcinfo, false);
}

Datum
gp_timestamptz_interval_bound(PG_FUNCTION_ARGS)
{
	return interval_bound_args(fcinfo, true);
}

/*
 * interval_bound(numeric, numeric [, int [, numeric]]):
 * floor((value - rbound) / width) * width + shift * width + rbound, at the
 * scales Cloudberry's numeric_interval_bound_common() keeps -- the quotient
 * at numeric_div()'s, the rest at what each step's operands carry.  NaN for
 * a NaN value, width or rbound.
 */
PG_FUNCTION_INFO_V1(gp_numeric_interval_bound);

Datum
gp_numeric_interval_bound(PG_FUNCTION_ARGS)
{
	Numeric		value;
	Numeric		width;
	int32		shift = 0;
	Numeric		rbound = NULL;
	Datum		result;
	Datum		zero;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	value = PG_GETARG_NUMERIC(0);
	width = PG_GETARG_NUMERIC(1);
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		shift = PG_GETARG_INT32(2);
	if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
		rbound = PG_GETARG_NUMERIC(3);

	if (numeric_is_nan(value) || numeric_is_nan(width) ||
		(rbound != NULL && numeric_is_nan(rbound)))
		PG_RETURN_DATUM(DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"),
											ObjectIdGetDatum(InvalidOid),
											Int32GetDatum(-1)));

	result = NumericGetDatum(value);
	if (rbound != NULL)
		result = DirectFunctionCall2(numeric_sub, result, NumericGetDatum(rbound));

	zero = DirectFunctionCall1(int4_numeric, Int32GetDatum(0));
	if (DatumGetInt32(DirectFunctionCall2(numeric_cmp, NumericGetDatum(width),
										  zero)) <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_INTERVAL_WIDTH),
				 errmsg("width of numeric interval not positive")));

	result = DirectFunctionCall2(numeric_div, result, NumericGetDatum(width));
	result = DirectFunctionCall1(numeric_floor, result);
	result = DirectFunctionCall2(numeric_mul, result, NumericGetDatum(width));

	if (shift != 0)
		result = DirectFunctionCall2(numeric_add, result,
									 DirectFunctionCall2(numeric_mul,
														 DirectFunctionCall1(int4_numeric,
																			 Int32GetDatum(shift)),
														 NumericGetDatum(width)));
	if (rbound != NULL)
		result = DirectFunctionCall2(numeric_add, result, NumericGetDatum(rbound));

	PG_RETURN_DATUM(result);
}

/* ------------------------------------------------------------------------- */
/* linear_interpolate()                                                      */
/* ------------------------------------------------------------------------- */

/*
 * linear_interpolate(x, x0, y0, x1, y1): the y on the line through (x0, y0)
 * and (x1, y1) at x.  The abscissas x, x0 and x1 are declared anyelement and
 * must be of one type of eleven; the ordinates are the function's own type.
 * Where x0 = x1 there is no line: the answer is y0 if x is x0 too and y0 =
 * y1, and NULL otherwise.
 *
 * What fraction of the way from x0 to x1 x lies, as a float8 --
 * Cloudberry's linterp_abscissa() -- and whether x0 = x1 (eq_bounds) and x =
 * x0 as well (eq_abscissas).
 */
static float8
linterp_abscissa(FunctionCallInfo fcinfo, bool *eq_bounds, bool *eq_abscissas)
{
	Oid			x_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			x0_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	Oid			x1_type = get_fn_expr_argtype(fcinfo->flinfo, 3);
	float8		p = 0;

	*eq_bounds = false;
	*eq_abscissas = false;

	if (!OidIsValid(x_type) || !OidIsValid(x0_type) || !OidIsValid(x1_type))
		elog(ERROR, "could not determine argument data types");
	if (x_type != x0_type || x_type != x1_type)
		elog(ERROR, "abscissa types unequal");
	if (get_fn_expr_argtype(fcinfo->flinfo, 2) !=
		get_fn_expr_argtype(fcinfo->flinfo, 4))
		elog(ERROR, "mismatched ordinate types");

	switch (x_type)
	{
		case INT8OID:
		case INT4OID:
		case INT2OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case DATEOID:
			{
				float8		x,
							x0,
							x1;

				switch (x_type)
				{
					case INT8OID:
						x = (float8) PG_GETARG_INT64(0);
						x0 = (float8) PG_GETARG_INT64(1);
						x1 = (float8) PG_GETARG_INT64(3);
						break;
					case INT4OID:
						x = (float8) PG_GETARG_INT32(0);
						x0 = (float8) PG_GETARG_INT32(1);
						x1 = (float8) PG_GETARG_INT32(3);
						break;
					case INT2OID:
						x = (float8) PG_GETARG_INT16(0);
						x0 = (float8) PG_GETARG_INT16(1);
						x1 = (float8) PG_GETARG_INT16(3);
						break;
					case FLOAT4OID:
						x = (float8) PG_GETARG_FLOAT4(0);
						x0 = (float8) PG_GETARG_FLOAT4(1);
						x1 = (float8) PG_GETARG_FLOAT4(3);
						break;
					case DATEOID:
						x = (float8) PG_GETARG_DATEADT(0);
						x0 = (float8) PG_GETARG_DATEADT(1);
						x1 = (float8) PG_GETARG_DATEADT(3);
						break;
					default:
						x = PG_GETARG_FLOAT8(0);
						x0 = PG_GETARG_FLOAT8(1);
						x1 = PG_GETARG_FLOAT8(3);
						break;
				}
				if (x1 == x0)
				{
					*eq_bounds = true;
					*eq_abscissas = (x == x0);
				}
				else
					p = (x - x0) / (x1 - x0);
			}
			break;
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				/* times and timestamps alike: their microseconds */
				int64		x = DatumGetInt64(PG_GETARG_DATUM(0));
				int64		x0 = DatumGetInt64(PG_GETARG_DATUM(1));
				int64		x1 = DatumGetInt64(PG_GETARG_DATUM(3));
				Interval	diffx = {0};
				Interval	diffx1 = {0};

				if (x_type != TIMEOID &&
					(TIMESTAMP_NOT_FINITE(x) || TIMESTAMP_NOT_FINITE(x0) ||
					 TIMESTAMP_NOT_FINITE(x1)))
				{
					/* as a division by zero, with no equality */
					*eq_bounds = true;
					return get_float8_nan();
				}
				if (pg_sub_s64_overflow(x, x0, &diffx.time) ||
					pg_sub_s64_overflow(x1, x0, &diffx1.time))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				if (!interval_div_internal(&diffx, &diffx1, &p, NULL))
				{
					*eq_bounds = true;
					*eq_abscissas = (x == x0);
					p = get_float8_nan();
				}
			}
			break;
		case INTERVALOID:
			{
				Interval   *x = PG_GETARG_INTERVAL_P(0);
				Interval   *x0 = PG_GETARG_INTERVAL_P(1);
				Interval   *x1 = PG_GETARG_INTERVAL_P(3);
				Interval	diffx;
				Interval	diffx1;

				interval_finite_check(x);
				interval_finite_check(x0);
				interval_finite_check(x1);
				diffx.month = x->month - x0->month;
				diffx.day = x->day - x0->day;
				diffx.time = x->time - x0->time;
				diffx1.month = x1->month - x0->month;
				diffx1.day = x1->day - x0->day;
				diffx1.time = x1->time - x0->time;
				if (!interval_div_internal(&diffx, &diffx1, &p, NULL))
				{
					*eq_bounds = true;
					*eq_abscissas = interval_usecs(x) == interval_usecs(x0);
					p = get_float8_nan();
				}
			}
			break;
		case NUMERICOID:
			{
				Numeric		x = PG_GETARG_NUMERIC(0);
				Numeric		x0 = PG_GETARG_NUMERIC(1);
				Numeric		x1 = PG_GETARG_NUMERIC(3);
				Datum		dx;
				Datum		dx1;

				if (numeric_is_nan(x) || numeric_is_nan(x0) || numeric_is_nan(x1))
				{
					*eq_bounds = true;
					return get_float8_nan();
				}
				dx = DirectFunctionCall2(numeric_sub, NumericGetDatum(x),
										 NumericGetDatum(x0));
				dx1 = DirectFunctionCall2(numeric_sub, NumericGetDatum(x1),
										  NumericGetDatum(x0));
				if (DatumGetInt32(DirectFunctionCall2(numeric_cmp, dx1,
													  DirectFunctionCall1(int4_numeric,
																		  Int32GetDatum(0)))) == 0)
				{
					*eq_bounds = true;
					*eq_abscissas = DatumGetInt32(DirectFunctionCall2(numeric_cmp,
																	  NumericGetDatum(x),
																	  NumericGetDatum(x0))) == 0;
					p = 0;
				}
				else
					p = DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,
														   DirectFunctionCall2(numeric_div, dx, dx1)));
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("abscissa type not supported")));
	}

	return p;
}

/*
 * The y that lies fraction p of the way from y0 to y1, for the ordinates
 * that are a count of microseconds: time, timestamp and timestamptz --
 * Cloudberry's time_li_value() and timestamp_li_value(), the difference
 * scaled by interval_mul() and added to y0.
 */
static Datum
linterp_usecs(Oid typid, float8 p, int64 y0, int64 y1)
{
	Interval	diffy = {0};
	Datum		offset;

	if (typid != TIMEOID && (TIMESTAMP_NOT_FINITE(y0) || TIMESTAMP_NOT_FINITE(y1)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	if (pg_sub_s64_overflow(y1, y0, &diffy.time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	offset = DirectFunctionCall2(interval_mul, IntervalPGetDatum(&diffy),
								 Float8GetDatum(p));
	switch (typid)
	{
		case TIMEOID:
			return DirectFunctionCall2(time_pl_interval, TimeADTGetDatum(y0), offset);
		case TIMESTAMPTZOID:
			return DirectFunctionCall2(timestamptz_pl_interval,
									   TimestampTzGetDatum(y0), offset);
		default:
			return DirectFunctionCall2(timestamp_pl_interval,
									   TimestampGetDatum(y0), offset);
	}
}

/*
 * The ordinate functions, one C function for the eleven types, each
 * Cloudberry's linterp_<type>(): an integer rounded, and refused where it
 * does not fit the type; a date moved by whole days; an interval and a
 * numeric as their arithmetic scales them.
 */
PG_FUNCTION_INFO_V1(gp_linear_interpolate);

Datum
gp_linear_interpolate(PG_FUNCTION_ARGS)
{
	Oid			ytype = get_fn_expr_argtype(fcinfo->flinfo, 2);
	bool		eq_bounds;
	bool		eq_abscissas;
	float8		p;

	p = linterp_abscissa(fcinfo, &eq_bounds, &eq_abscissas);

	switch (ytype)
	{
		case INT8OID:
		case INT4OID:
		case INT2OID:
			{
				float8		y0;
				float8		y1;
				float8		r;

				if (ytype == INT8OID)
				{
					y0 = (float8) PG_GETARG_INT64(2);
					y1 = (float8) PG_GETARG_INT64(4);
				}
				else if (ytype == INT4OID)
				{
					y0 = (float8) PG_GETARG_INT32(2);
					y1 = (float8) PG_GETARG_INT32(4);
				}
				else
				{
					y0 = (float8) PG_GETARG_INT16(2);
					y1 = (float8) PG_GETARG_INT16(4);
				}

				if (eq_bounds)
				{
					if (eq_abscissas && y0 == y1)
						r = y0;
					else
						PG_RETURN_NULL();
				}
				else
					r = round(y0 + p * (y1 - y0));

				if (ytype == INT8OID)
				{
					if (isnan(r) || !FLOAT8_FITS_IN_INT64(r))
						ereport(ERROR,
								(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								 errmsg("value \"%f\" is out of range for type bigint", r)));
					PG_RETURN_INT64((int64) r);
				}
				if (ytype == INT4OID)
				{
					if (isnan(r) || r < INT_MIN || r > INT_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								 errmsg("value \"%f\" is out of range for type integer", r)));
					PG_RETURN_INT32((int32) r);
				}
				if (isnan(r) || r < SHRT_MIN || r > SHRT_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%f\" is out of range for type smallint", r)));
				PG_RETURN_INT16((int16) r);
			}
		case FLOAT8OID:
		case FLOAT4OID:
			{
				float8		y0;
				float8		y1;
				float8		r;

				if (ytype == FLOAT8OID)
				{
					y0 = PG_GETARG_FLOAT8(2);
					y1 = PG_GETARG_FLOAT8(4);
				}
				else
				{
					y0 = (float8) PG_GETARG_FLOAT4(2);
					y1 = (float8) PG_GETARG_FLOAT4(4);
				}

				if (eq_bounds)
				{
					if (eq_abscissas && y0 == y1)
						r = y0;
					else
						PG_RETURN_NULL();
				}
				else
					r = y0 + p * (y1 - y0);

				if (ytype == FLOAT8OID)
					PG_RETURN_FLOAT8(r);
				PG_RETURN_FLOAT4((float4) r);
			}
		case DATEOID:
			{
				DateADT		y0 = PG_GETARG_DATEADT(2);
				DateADT		y1 = PG_GETARG_DATEADT(4);
				int64		dy = (int64) y1 - y0;

				if (eq_bounds)
				{
					if (eq_abscissas && dy == 0)
						PG_RETURN_DATEADT(y0);
					PG_RETURN_NULL();
				}
				PG_RETURN_DATEADT((DateADT) (y0 + (int32) (p * (float8) dy)));
			}
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				int64		y0 = DatumGetInt64(PG_GETARG_DATUM(2));
				int64		y1 = DatumGetInt64(PG_GETARG_DATUM(4));

				if (eq_bounds)
				{
					if (eq_abscissas && y0 == y1)
						PG_RETURN_DATUM(PG_GETARG_DATUM(2));
					PG_RETURN_NULL();
				}
				PG_RETURN_DATUM(linterp_usecs(ytype, p, y0, y1));
			}
		case INTERVALOID:
			{
				Interval   *y0 = PG_GETARG_INTERVAL_P(2);
				Interval   *y1 = PG_GETARG_INTERVAL_P(4);
				Interval	diffy;
				Interval   *y;

				interval_finite_check(y0);
				interval_finite_check(y1);
				if (eq_bounds)
				{
					if (eq_abscissas && interval_usecs(y0) == interval_usecs(y1))
						PG_RETURN_INTERVAL_P(y0);
					PG_RETURN_NULL();
				}
				diffy.month = y1->month - y0->month;
				diffy.day = y1->day - y0->day;
				diffy.time = y1->time - y0->time;
				y = DatumGetIntervalP(DirectFunctionCall2(interval_mul,
														  IntervalPGetDatum(&diffy),
														  Float8GetDatum(p)));
				y->month += y0->month;
				y->day += y0->day;
				y->time += y0->time;
				PG_RETURN_INTERVAL_P(y);
			}
		case NUMERICOID:
			{
				Numeric		y0 = PG_GETARG_NUMERIC(2);
				Numeric		y1 = PG_GETARG_NUMERIC(4);
				char		buf[DBL_DIG + 100];
				Datum		f;

				if (eq_bounds)
				{
					if (eq_abscissas &&
						DatumGetInt32(DirectFunctionCall2(numeric_cmp,
														  NumericGetDatum(y0),
														  NumericGetDatum(y1))) == 0)
						PG_RETURN_NUMERIC(y0);
					PG_RETURN_NULL();
				}
				if (numeric_is_nan(y0) || numeric_is_nan(y1) || isnan(p))
					PG_RETURN_DATUM(DirectFunctionCall3(numeric_in,
														CStringGetDatum("NaN"),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
				/* p as a numeric, to DBL_DIG digits, as Cloudberry makes it */
				snprintf(buf, sizeof(buf), "%.*g", DBL_DIG, p);
				f = DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
										ObjectIdGetDatum(InvalidOid),
										Int32GetDatum(-1));
				PG_RETURN_DATUM(DirectFunctionCall2(numeric_add, NumericGetDatum(y0),
													DirectFunctionCall2(numeric_mul, f,
																		DirectFunctionCall2(numeric_sub,
																							NumericGetDatum(y1),
																							NumericGetDatum(y0)))));
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ordinate type not supported")));
	}

	PG_RETURN_NULL();			/* keep compiler quiet */
}
