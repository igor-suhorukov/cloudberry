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
 * gp_hll.h
 *	  Cloudberry's HyperLogLog counter, gp_hyperloglog_estimator: how many
 *	  distinct values a column has, in a form counters of several tables
 *	  can be merged in (gp_hll.c).
 *
 * The counter is Cloudberry's own byte for byte -- its struct, its hash,
 * its registers -- so that an estimate made of the port's counters is the
 * estimate Cloudberry makes of the same values.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_HLL_H
#define GP_HLL_H

#include "fmgr.h"

/*
 * A counter: a varlena, as Cloudberry's GpHLLData (gp_hyperloglog.h).  "b"
 * is the number of bits of a hash that choose the register, negative where
 * the registers are compressed; "format" says whether they are packed,
 * binbits bits each, or one to a byte.  ndistinct, nmultiples and
 * samplerows are what the ANALYZE that made a leaf's counter saw in its
 * sample, which the merge of leaves' statistics reads.  The registers are
 * data, declared of one byte as Cloudberry declares them, so that the
 * struct is as long as Cloudberry's.
 */
typedef struct GpHLLData
{
	char		vl_len_[4];
	int8_t		b;
	uint8_t		binbits;
	uint8_t		version;
	uint8_t		format;
	int32_t		idx;
	int32_t		nmultiples;
	int32_t		ndistinct;
	int32_t		samplerows;
	float4		relTuples;
	float4		relPages;
	int32_t		padding[11];
	char		data[1];
} GpHLLData;

typedef GpHLLData *GpHLLCounter;

/* The formats of the registers */
#define GP_HLL_PACKED			0
#define GP_HLL_PACKED_UNPACKED	1
#define GP_HLL_UNPACKED			2
#define GP_HLL_UNPACKED_UNPACKED 3

/*
 * How far off an estimate of a full scan's counter is let be from the rows
 * counted for every value to be taken as distinct: 0.3%, Cloudberry's
 * GP_HLL_ERROR_MARGIN (analyze.c).
 */
#define GP_HLL_ERROR_MARGIN		0.003

/* A new, empty counter of Cloudberry's size: 2^63 values, 0.8125% error. */
extern GpHLLCounter GpHllCreate(void);

/* A copy of a counter. */
extern GpHLLCounter GpHllCopy(GpHLLCounter counter);

/*
 * A value of a column added to a counter, hashed as Cloudberry hashes it:
 * the bytes of a varlena's data, a cstring's, or a fixed-length value's.
 */
extern GpHLLCounter GpHllAddItem(GpHLLCounter counter, Datum value,
								 int16 typlen, bool typbyval);

/* The number of distinct values a counter has seen. */
extern double GpHllEstimate(GpHLLCounter counter);

/*
 * The counter of the values either has seen; either may be NULL, and both
 * NULL gives NULL.  As Cloudberry's, a counter it unpacks is marked
 * unpacked where it lies.
 */
extern GpHLLCounter GpHllMerge(GpHLLCounter counter1, GpHLLCounter counter2);

#endif							/* GP_HLL_H */
