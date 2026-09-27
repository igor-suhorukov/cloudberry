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
 * cb_regress.c
 *	  The functions of Cloudberry's regress.so that its tests load, served by
 *	  the port.
 *
 * Cloudberry's tests make functions from the regress.so of their build
 * directory, whose regress_gp.c has test functions of Cloudberry's own
 * beside PostgreSQL's.  The greenplum suite links this module into the
 * directory the tests load it from, under that name, and each function here
 * asks the module that does the work, found by name when the function is
 * called, so that this one loads whatever else is loaded.
 *
 * The rest are Cloudberry's own, which ask PostgreSQL alone, as they stand
 * there but for what PostgreSQL 19 renamed.
 *
 * It is a test module: built and installed only where the tests run.
 *
 * Cloudberry source this file stands in for:
 *	  src/test/regress/regress_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
					.name = "cb_regress",
					.version = "1.0"
);

typedef bool (*deny_allows_fn) (const char *rolename, TimestampTz when);
typedef bool (*queue_in_sync_fn) (const char *queuename);

PG_FUNCTION_INFO_V1(check_auth_time_constraints);

/*
 * check_auth_time_constraints(cstring, timestamptz) -> bool: whether the role
 * may log in at that time, by its DENY windows, which are gp_security's.
 */
Datum
check_auth_time_constraints(PG_FUNCTION_ARGS)
{
	static deny_allows_fn allows = NULL;

	if (allows == NULL)
		allows = (deny_allows_fn)
			load_external_function("$libdir/gp_security", "GpDenyRoleAllowed",
								   true, NULL);

	PG_RETURN_BOOL(allows(PG_GETARG_CSTRING(0), PG_GETARG_TIMESTAMPTZ(1)));
}

PG_FUNCTION_INFO_V1(checkResourceQueueMemoryLimits);

/*
 * checkResourceQueueMemoryLimits(cstring) -> bool: whether a resource queue's
 * memory limit in shared memory is the one it is defined with, which are
 * gp_resource's.
 */
Datum
checkResourceQueueMemoryLimits(PG_FUNCTION_ARGS)
{
	static queue_in_sync_fn in_sync = NULL;

	if (in_sync == NULL)
		in_sync = (queue_in_sync_fn)
			load_external_function("$libdir/gp_resource",
								   "GpResQueueMemoryLimitInSync", true, NULL);

	PG_RETURN_BOOL(in_sync(PG_GETARG_CSTRING(0)));
}

PG_FUNCTION_INFO_V1(test_consume_xids);

/*
 * test_consume_xids(int4): take that many transaction IDs, fast, to test
 * wraparound (autovacuum).
 */
Datum
test_consume_xids(PG_FUNCTION_ARGS)
{
	int32		nxids = PG_GETARG_INT32(0);
	TransactionId xid;
	TransactionId targetxid;

	/* a top-level transaction ID first */
	(void) GetCurrentTransactionId();

	/* the next ID, less one */
	xid = ReadNextTransactionId();
	targetxid = xid + nxids - 1;
	while (targetxid < FirstNormalTransactionId)
		targetxid++;

	while (TransactionIdPrecedes(xid, targetxid))
		xid = XidFromFullTransactionId(GetNewTransactionId(true));

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(check_shared_buffer_cache_for_dboid);

/*
 * check_shared_buffer_cache_for_dboid(oid) -> bool: whether a buffer holds a
 * page of that database's (dropdb_check_shared_buffer_cache).
 */
Datum
check_shared_buffer_cache_for_dboid(PG_FUNCTION_ARGS)
{
	Oid			dboid = PG_GETARG_OID(0);

	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *hdr = GetBufferDescriptor(i);

		if (hdr->tag.dbOid == dboid)
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(gp_set_next_oid);

/* gp_set_next_oid(oid): the node's OID counter, set (oid_wraparound). */
Datum
gp_set_next_oid(PG_FUNCTION_ARGS)
{
	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
	TransamVariables->nextOid = PG_GETARG_OID(0);
	LWLockRelease(OidGenLock);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_get_next_oid);

/* gp_get_next_oid() -> oid: the node's OID counter. */
Datum
gp_get_next_oid(PG_FUNCTION_ARGS)
{
	Oid			next;

	LWLockAcquire(OidGenLock, LW_SHARED);
	next = TransamVariables->nextOid;
	LWLockRelease(OidGenLock);
	PG_RETURN_OID(next);
}

PG_FUNCTION_INFO_V1(udf_setenv);

/* udf_setenv(cstring, cstring) -> bool: a variable of the backend's environment set. */
Datum
udf_setenv(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(setenv(PG_GETARG_CSTRING(0), PG_GETARG_CSTRING(1), 1) == 0);
}

PG_FUNCTION_INFO_V1(udf_unsetenv);

/* udf_unsetenv(cstring) -> bool: and taken away. */
Datum
udf_unsetenv(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(unsetenv(PG_GETARG_CSTRING(0)) == 0);
}

PG_FUNCTION_INFO_V1(assign_new_record);

/*
 * assign_new_record() -> SETOF record: on a segment, ten rows, each of a
 * record type of its own -- none, then one column more each time -- which a
 * Motion has to carry to its receiver (transient_types); on the
 * coordinator, none.
 */
Datum
assign_new_record(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	const char *role = GetConfigOption("gp.role", true, false);

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "c", INT4OID, -1, 0);
		TupleDescFinalize(tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->max_calls = 10;
	}

	funcctx = SRF_PERCALL_SETUP();
	if (role != NULL && strcmp(role, "dispatch") == 0)
		SRF_RETURN_DONE(funcctx);

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		int			natts = (int) funcctx->call_cntr;
		TupleDesc	tupdesc = CreateTemplateTupleDesc(natts);
		Datum		values[10];
		bool		nulls[10];
		HeapTuple	tuple;

		for (int i = 1; i <= natts; i++)
			TupleDescInitEntry(tupdesc, (AttrNumber) i, "c", INT4OID, -1, 0);
		TupleDescFinalize(tupdesc);
		tupdesc = BlessTupleDesc(tupdesc);
		for (int i = 0; i < natts; i++)
		{
			values[i] = Int32GetDatum(i);
			nulls[i] = false;
		}
		tuple = heap_form_tuple(tupdesc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}
