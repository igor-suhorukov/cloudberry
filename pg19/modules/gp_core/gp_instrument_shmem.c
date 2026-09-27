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
 * gp_instrument_shmem.c
 *	  The slots of query metrics, read: how many are free and used, and
 *	  whose each used one is.
 *
 * A library of its own, as Cloudberry's gpcontrib has it, whose functions
 * its users make:
 *
 *	CREATE FUNCTION gp_instrument_shmem_summary_f() RETURNS SETOF RECORD
 *	AS '$libdir/gp_instrument_shmem', 'gp_instrument_shmem_summary'
 *	LANGUAGE C IMMUTABLE;
 *
 * and gp_instrument_shmem_detail likewise.  It reads gp_core's slots
 * (gp_metrics.c), of the node it runs on; where metrics are off, the
 * summary's counts are null and the detail has no rows, as Cloudberry's.
 *
 * Cloudberry source this file stands in for:
 *	  gpcontrib/gp_internal_tools/gp_instrument_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"

#include "gp_cluster.h"
#include "gp_metrics.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_instrument_shmem",
					.version = GP_VERSION
);

PG_FUNCTION_INFO_V1(gp_instrument_shmem_summary);
PG_FUNCTION_INFO_V1(gp_instrument_shmem_detail);

/* one row: the node's content, and its free and used slots */
Datum
gp_instrument_shmem_summary(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc = CreateTemplateTupleDesc(3);
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	int			nfree;
	int			nslots = GpMetricsSlotCount(&nfree);

	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segid", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "num_free", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "num_used", INT8OID, -1, 0);
	TupleDescFinalize(tupdesc);
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int32GetDatum(GpClusterContentId());
	values[1] = Int64GetDatum(nfree);
	values[2] = Int64GetDatum(nslots - nfree);
	if (nslots == 0)
		nulls[1] = nulls[2] = true;

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* a row for each used slot: whose it is, and the node's counts */
Datum
gp_instrument_shmem_detail(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *next;
	int			nfree;
	int			nslots;
	GpMetricsSlot slot;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(9);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "tmid", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "ssid", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "ccnt", INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "segid", INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "pid", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "nid", INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "tuplecount", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "nloops", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "ntuples", INT8OID, -1, 0);
		TupleDescFinalize(tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = palloc0(sizeof(int));
		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	next = (int *) funcctx->user_fctx;
	nslots = GpMetricsSlotCount(&nfree);
	while (*next < nslots)
	{
		if (GpMetricsSlotCopy((*next)++, &slot))
		{
			Datum		values[9];
			bool		nulls[9] = {false};

			values[0] = Int32GetDatum(slot.tmid);
			values[1] = Int32GetDatum(slot.ssid);
			values[2] = Int16GetDatum((int16) slot.ccnt);
			values[3] = Int16GetDatum(slot.segid);
			values[4] = Int32GetDatum(slot.pid);
			values[5] = Int16GetDatum(slot.nid);
			values[6] = Int64GetDatum((int64) slot.data.tuplecount);
			values[7] = Int64GetDatum((int64) slot.data.nloops);
			values[8] = Int64GetDatum((int64) slot.data.ntuples);
			SRF_RETURN_NEXT(funcctx,
							HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
															  values, nulls)));
		}
	}
	SRF_RETURN_DONE(funcctx);
}
