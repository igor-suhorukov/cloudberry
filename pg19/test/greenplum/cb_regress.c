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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC_EXT(
					.name = "cb_regress",
					.version = "1.0"
);

/* gp.debug_burn_xids: test_consume_xids() takes XIDs fast; see there */
static bool debug_burn_xids = false;

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomBoolVariable("gp.debug_burn_xids",
							 "Consume XIDs faster, in test_consume_xids(), as Cloudberry's debug_burn_xids does.",
							 NULL, &debug_burn_xids, false, PGC_USERSET, 0,
							 NULL, NULL, NULL);
}

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

typedef void (*reset_gang_fn) (void);

PG_FUNCTION_INFO_V1(cleanupAllGangs);

/*
 * cleanupAllGangs() -> bool: the session's connections to the segments
 * closed, which gp_core's dispatcher makes again as the next statement
 * needs them -- Cloudberry's DisconnectAndDestroyAllGangs().  On the
 * coordinator alone, as there.
 */
Datum
cleanupAllGangs(PG_FUNCTION_ARGS)
{
	static reset_gang_fn reset = NULL;
	const char *role = GetConfigOption("gp.role", true, false);

	if (role == NULL || strcmp(role, "dispatch") != 0)
		elog(ERROR, "cleanupAllGangs can only be executed on master");
	if (reset == NULL)
		reset = (reset_gang_fn)
			load_external_function("$libdir/gp_core", "GpDispatchResetGang",
								   true, NULL);
	reset();
	PG_RETURN_BOOL(true);
}

typedef int (*gang_sockets_fn) (int *contents, bool *writers, int *sockets,
								int max);

/* One socket option of a connection to a segment, as an int. */
static int
socket_option(int fd, int level, int option, const char *name)
{
	int			value;
	socklen_t	size = sizeof(value);

	if (getsockopt(fd, level, option, &value, &size) < 0)
		elog(ERROR, "getsockopt(%s) failed: %m", name);
	return value;
}

PG_FUNCTION_INFO_V1(gp_keepalives_check);

/*
 * gp_keepalives_check() -> setof (qe_id, is_writer, keepalives_enabled,
 * keepalives_interval, keepalives_count, keepalives_idle): the TCP
 * keepalives each of the session's connections to the segments has, as its
 * socket says -- what gp.dispatch_keepalives_* asked of it.  A connection
 * over a Unix socket has no TCP options, and fails the call, as
 * Cloudberry's does.  The connections are gp_core's dispatcher's
 * (GpDispatchGangSockets()), a writer on each segment and the readers the
 * session keeps, where Cloudberry's walks its gangs' free lists.
 */
Datum
gp_keepalives_check(PG_FUNCTION_ARGS)
{
	static gang_sockets_fn gang_sockets = NULL;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			max = 1024;
	int		   *contents = palloc_array(int, max);
	bool	   *writers = palloc_array(bool, max);
	int		   *sockets = palloc_array(int, max);
	int			n;

	if (gang_sockets == NULL)
		gang_sockets = (gang_sockets_fn)
			load_external_function("$libdir/gp_core", "GpDispatchGangSockets",
								   true, NULL);
	InitMaterializedSRF(fcinfo, 0);

	n = gang_sockets(contents, writers, sockets, max);
	for (int i = 0; i < n; i++)
	{
		Datum		values[6];
		bool		nulls[6] = {false};

		values[0] = Int16GetDatum(contents[i]);
		values[1] = BoolGetDatum(writers[i]);
		values[2] = BoolGetDatum(socket_option(sockets[i], SOL_SOCKET,
											   SO_KEEPALIVE,
											   "SO_KEEPALIVE") > 0);
		values[3] = Int32GetDatum(socket_option(sockets[i], IPPROTO_TCP,
												TCP_KEEPINTVL,
												"TCP_KEEPINTVL"));
		values[4] = Int32GetDatum(socket_option(sockets[i], IPPROTO_TCP,
												TCP_KEEPCNT,
												"TCP_KEEPCNT"));
		values[5] = Int32GetDatum(socket_option(sockets[i], IPPROTO_TCP,
												TCP_KEEPIDLE,
												"TCP_KEEPIDLE"));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
							 nulls);
	}
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(test_consume_xids);

/*
 * Under gp.debug_burn_xids, the next transaction ID made the last of its
 * page but one, as Cloudberry's GetNewTransactionId() makes it under its
 * debug_burn_xids: what is skipped is a stretch of IDs that are like one
 * another, and each page's first IDs are still made one at a time, so the
 * pages of the SLRUs that follow the IDs are made as they are reached.
 * The page is pg_subtrans's, the smallest of those PostgreSQL 19 makes as
 * the IDs reach them -- 2,048 IDs of 8K, where Cloudberry's steps are
 * 4,096 of its 32K pages -- but for pg_commit_ts's, which a setting turns
 * on and under which nothing is skipped.
 */
static void
burn_xids(void)
{
	const uint64 per_page = BLCKSZ / sizeof(TransactionId);
	uint64		next;
	uint64		r;

	if (!debug_burn_xids || track_commit_timestamp)
		return;
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	next = U64FromFullTransactionId(TransamVariables->nextXid);
	r = next % per_page;
	if (r > 1 && r < per_page - 1)
		TransamVariables->nextXid = FullTransactionIdFromU64(next + per_page - r - 1);
	LWLockRelease(XidGenLock);
}

/*
 * test_consume_xids(int4): take that many transaction IDs, fast, to test
 * wraparound (autovacuum) -- faster still under gp.debug_burn_xids.
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
	{
		xid = XidFromFullTransactionId(GetNewTransactionId(true));
		burn_xids();
	}

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

/* ------------------------------------------------------------------------- */
/* The dispatch test's gangs and interconnect                                */
/* ------------------------------------------------------------------------- */

typedef int (*session_backends_fn) (void);
typedef int (*active_connections_fn) (void);

PG_FUNCTION_INFO_V1(hasGangsExist);

/*
 * hasGangsExist() -> bool: whether the session keeps connections to the
 * segments for its next statement -- Cloudberry's cdbcomponent_qesExist(),
 * of its gangs -- which gp_core's dispatcher lists (GpDispatchGangSockets()).
 * On the coordinator alone, as there.
 */
Datum
hasGangsExist(PG_FUNCTION_ARGS)
{
	static gang_sockets_fn gang_sockets = NULL;
	const char *role = GetConfigOption("gp.role", true, false);
	int			content;
	bool		writer;
	int			socket;

	if (role == NULL || strcmp(role, "dispatch") != 0)
		elog(ERROR, "hasGangsExist can only be executed on master");
	if (gang_sockets == NULL)
		gang_sockets = (gang_sockets_fn)
			load_external_function("$libdir/gp_core", "GpDispatchGangSockets",
								   true, NULL);
	PG_RETURN_BOOL(gang_sockets(&content, &writer, &socket, 1) > 0);
}

PG_FUNCTION_INFO_V1(hasBackendsExist);

/*
 * hasBackendsExist(timeout) -> bool: on a segment, whether processes of this
 * session other than this one are there -- those of a gang that should have
 * gone -- waiting for them to end up to timeout seconds, a second at a time,
 * as Cloudberry's counts the rows of its pg_stat_activity of the session's
 * gp_session_id.  The processes are those gp_core's table of the backends
 * says work for the session (GpGddSessionBackends()).
 */
Datum
hasBackendsExist(PG_FUNCTION_ARGS)
{
	static session_backends_fn session_backends = NULL;
	int			timeout = PG_GETARG_INT32(0);
	int			n;

	if (timeout < 0)
		elog(ERROR, "timeout is expected not to be negative");
	if (session_backends == NULL)
		session_backends = (session_backends_fn)
			load_external_function("$libdir/gp_core", "GpGddSessionBackends",
								   true, NULL);
	while ((n = session_backends()) > 0 && timeout-- > 0)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000000L);
	}
	PG_RETURN_BOOL(n > 0);
}

PG_FUNCTION_INFO_V1(gangRaiseInfo);

/*
 * gangRaiseInfo() -> bool: an INFO with a detail, a hint and a context, which
 * a segment raises and the coordinator relays, as Cloudberry's is (its
 * MPPnoticeReceiver()).
 */
Datum
gangRaiseInfo(PG_FUNCTION_ARGS)
{
	ereport(INFO,
			(errmsg("testing hook function MPPnoticeReceiver"),
			 errdetail("this test aims at covering code paths not hit before"),
			 errhint("no special hint"),
			 errcontext("PL/C function defined in regress.c"),
			 errposition(0)));

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(numActiveMotionConns);

/*
 * numActiveMotionConns() -> int: how many interconnect connections this
 * backend has open, gp_core's senders and receivers (GpIcActiveConnections()),
 * which Cloudberry's counts of its UDP interconnect's.
 */
Datum
numActiveMotionConns(PG_FUNCTION_ARGS)
{
	static active_connections_fn active_connections = NULL;

	if (active_connections == NULL)
		active_connections = (active_connections_fn)
			load_external_function("$libdir/gp_core", "GpIcActiveConnections",
								   true, NULL);
	PG_RETURN_INT32(active_connections());
}
