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
 * pax_numeric.c
 *	  numeric.c's init_var(), free_var(), alloc_var(), make_result() and
 *	  cmp_numerics(), which Cloudberry exports for PAX's vectorized numeric
 *	  and its statistics and PostgreSQL 19 keeps static: copied from
 *	  PostgreSQL 19's numeric.c, under the names Cloudberry gave them
 *	  (pax_numeric.h), and cmp_numerics() asked of numeric_cmp().
 *	  Portions Copyright (c) 1998-2026, PostgreSQL Global Development Group.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "varatt.h"

#include "pax_numeric.h"

void
init_numeric_var(NumericVar *var)
{
	memset(var, 0, sizeof(NumericVar));
}

void
free_numeric_var(NumericVar *var)
{
	if (var->buf != NULL)
		pfree(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->sign = NUMERIC_NAN;
}

/* A digit buffer of ndigits digits, plus a spare digit for rounding. */
void
alloc_numeric_var(NumericVar *var, int ndigits)
{
	if (var->buf != NULL)
		pfree(var->buf);
	var->buf = (NumericDigit *) palloc((ndigits + 1) * sizeof(NumericDigit));
	var->buf[0] = 0;			/* spare digit for rounding */
	var->digits = var->buf + 1;
	var->ndigits = ndigits;
}

/* The packed numeric of var, in palloc'd memory; NaN and infinities too. */
Numeric
make_numeric_result(const NumericVar *var)
{
	Numeric		result;
	NumericDigit *digits = var->digits;
	int			weight = var->weight;
	int			sign = var->sign;
	int			n;
	Size		len;

	if ((sign & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL)
	{
		if (!(sign == NUMERIC_NAN ||
			  sign == NUMERIC_PINF ||
			  sign == NUMERIC_NINF))
			elog(ERROR, "invalid numeric sign value 0x%x", sign);

		result = (Numeric) palloc(NUMERIC_HDRSZ_SHORT);

		SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
		result->choice.n_header = sign;
		return result;
	}

	n = var->ndigits;

	/* truncate leading zeroes */
	while (n > 0 && *digits == 0)
	{
		digits++;
		weight--;
		n--;
	}
	/* truncate trailing zeroes */
	while (n > 0 && digits[n - 1] == 0)
		n--;

	/* If zero result, force to weight=0 and positive sign */
	if (n == 0)
	{
		weight = 0;
		sign = NUMERIC_POS;
	}

	/* Build the result */
	if (NUMERIC_CAN_BE_SHORT(var->dscale, weight))
	{
		len = NUMERIC_HDRSZ_SHORT + n * sizeof(NumericDigit);
		result = (Numeric) palloc(len);
		SET_VARSIZE(result, len);
		result->choice.n_short.n_header =
			(sign == NUMERIC_NEG ? (NUMERIC_SHORT | NUMERIC_SHORT_SIGN_MASK)
			 : NUMERIC_SHORT)
			| (var->dscale << NUMERIC_SHORT_DSCALE_SHIFT)
			| (weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0)
			| (weight & NUMERIC_SHORT_WEIGHT_MASK);
	}
	else
	{
		len = NUMERIC_HDRSZ + n * sizeof(NumericDigit);
		result = (Numeric) palloc(len);
		SET_VARSIZE(result, len);
		result->choice.n_long.n_sign_dscale =
			sign | (var->dscale & NUMERIC_DSCALE_MASK);
		result->choice.n_long.n_weight = weight;
	}

	Assert(NUMERIC_NDIGITS(result) == n);
	if (n > 0)
		memcpy(NUMERIC_DIGITS(result), digits, n * sizeof(NumericDigit));

	/* Check for overflow of int16 fields */
	if (NUMERIC_WEIGHT(result) != weight ||
		NUMERIC_DSCALE(result) != var->dscale)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value overflows numeric format")));

	return result;
}

static Numeric
make_special(int sign)
{
	NumericVar	var = {0, 0, sign, 0, NULL, NULL};

	return make_numeric_result(&var);
}

Numeric
make_nan_numeric_result(void)
{
	return make_special(NUMERIC_NAN);
}

Numeric
make_pinf_numeric_result(void)
{
	return make_special(NUMERIC_PINF);
}

Numeric
make_ninf_numeric_result(void)
{
	return make_special(NUMERIC_NINF);
}

int
cmp_numerics(Numeric num1, Numeric num2)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
											 NumericGetDatum(num1),
											 NumericGetDatum(num2)));
}
