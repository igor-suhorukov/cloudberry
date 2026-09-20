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
 * compat/selfuncs.c
 *	  What Cloudberry exports from PostgreSQL's selfuncs.c for ORCA.
 *
 * Both functions are static in PostgreSQL 19, so this is a re-implementation
 * rather than a call.  Cloudberry's own change to these two is to delete the
 * word "static"; the bodies here are PostgreSQL 19's, read from
 * src/backend/utils/adt/selfuncs.c and src/backend/utils/adt/numeric.c.
 *
 * See compat/cb_selfuncs.h for why this file exists at all, given that
 * cloudberry.md says it should not.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "cb_selfuncs.h"

/*
 * convert_timevalue_to_scalar
 *		A time-shaped value on one comparable scale.
 *
 * PostgreSQL 19's selfuncs.c:convert_timevalue_to_scalar, unchanged.  The
 * only thing worth saying about it is why each case is the number it is:
 * timestamps and timestamptz are already microseconds since 2000-01-01, a
 * date is converted to the same origin, and time and timetz are microseconds
 * since midnight.  Two values of different families are therefore not
 * comparable -- which is fine, because a histogram holds one type.
 */
double
convert_timevalue_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case TIMESTAMPOID:
			return DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return DatumGetTimestampTz(value);
		case DATEOID:
			return date2timestamp_no_overflow(DatumGetDateADT(value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * Convert the month part of Interval to days using assumed
				 * average month length of 365.25/12.0 days.  Not too
				 * accurate, but plenty good enough for our purposes.
				 *
				 * This also works for infinite intervals, which just have all
				 * fields set to INT_MIN/INT_MAX, and so will produce a result
				 * smaller/larger than any finite interval.
				 */
				return interval->time + interval->day * (double) USECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
		case TIMETZOID:
			{
				TimeTzADT  *timetz = DatumGetTimeTzADTP(value);

				/* use GMT-equivalent time */
				return (double) (timetz->time + (timetz->zone * 1000000.0));
			}
	}

	*failure = true;
	return 0;
}

/*
 * numeric_to_double_no_overflow
 *		A numeric as a double, saturating rather than raising.
 *
 * Cloudberry adds this to numeric.c beside the numeric machinery, where it
 * can reach the file's static init_var_from_num() and
 * numericvar_to_double_no_overflow().  Neither is reachable from outside, so
 * the port goes through numeric_float8_no_overflow() instead, which is the
 * SQL-callable function built on exactly those two.  Same conversion, same
 * saturation to ±Infinity, one fmgr call of overhead -- and ORCA does this
 * once per histogram bound, not once per row.
 */
double
numeric_to_double_no_overflow(Numeric num)
{
	Datum		result;

	result = DirectFunctionCall1(numeric_float8_no_overflow,
								 NumericGetDatum(num));

	return DatumGetFloat8(result);
}
