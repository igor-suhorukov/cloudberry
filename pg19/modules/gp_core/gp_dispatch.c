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
 * gp_dispatch.c
 *	  Reaching the segments: gangs, statements, and rows on the way back.
 *
 * Cloudberry's dispatcher marks its connections as internal with high bits in
 * the protocol version, skips pg_hba for them, and sends plans in messages of
 * its own.  PostgreSQL 19 will have none of it: a protocol major above 3 is
 * rejected before anything else, an unknown message type ends the session, and
 * there is no hook in the message loop.  So the port's dispatcher is an
 * ordinary libpq client of an ordinary backend, and what makes that backend a
 * segment process is a startup setting it carries.  That is the design
 * "Dispatch" in the plan describes, and it needs no core patch.
 *
 * Authentication is therefore real authentication: decision 5 asks for SCRAM
 * on the early milestones, and "gp.internal_passfile" names the password file
 * the dispatcher hands libpq.  A file, not a setting: a setting is readable by
 * any user who can SHOW it, and this is a password for every database in the
 * cluster.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/dispatcher/ (cdbdisp.c, cdbdisp_query.c, cdbconn.c,
 *	  cdbgang.c), less the parts that exist because Cloudberry speaks its own
 *	  protocol
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"

/* Where libpq finds the password for the segments; see the file header. */
static char *gp_internal_passfile = NULL;

/* One segment's connection. */
typedef struct GpSegmentConn
{
	int			content;
	const GpSegmentConfig *seg;
	PGconn	   *conn;
	bool		busy;			/* a statement was sent and has not finished */
} GpSegmentConn;

typedef struct GpGang
{
	int			nconns;
	GpSegmentConn *conns;
	WaitEventSet *wes;			/* MyLatch plus every connection's socket */
	int			wes_latch_pos;
} GpGang;

static GpGang *gang = NULL;
static bool exit_callback_registered = false;

/* What a segment answered when it failed. */
typedef struct GpSegmentError
{
	int			content;
	char	   *sqlstate;
	char	   *message;
	char	   *detail;
	char	   *hint;
	char	   *context;
} GpSegmentError;

static void gang_close(void);

/*
 * What pg_stat_activity shows while this backend is waiting for a segment.
 *
 * Registered on first use rather than in _PG_init: PostgreSQL 19 keeps custom
 * wait events in shared memory, which is not there yet while libraries are
 * being preloaded.
 */
static uint32
dispatch_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryDispatch");
	return event;
}

/* ------------------------------------------------------------------------- */
/* The gang                                                                  */
/* ------------------------------------------------------------------------- */

static void
gang_atexit(int code, Datum arg)
{
	gang_close();
}

/*
 * Give up on the connections.  Called when one of them breaks, when the
 * session ends, and when an abort finds a statement still running on a
 * segment that will not answer.
 */
static void
gang_close(void)
{
	if (gang == NULL)
		return;

	for (int i = 0; i < gang->nconns; i++)
	{
		if (gang->conns[i].conn != NULL)
		{
			libpqsrv_disconnect(gang->conns[i].conn);
			gang->conns[i].conn = NULL;
		}
	}
	if (gang->wes != NULL)
		FreeWaitEventSet(gang->wes);

	pfree(gang->conns);
	pfree(gang);
	gang = NULL;
}

void
GpDispatchResetGang(void)
{
	gang_close();
}

/*
 * The identity the coordinator gives a segment process.
 *
 * It is what makes the backend on the other end a segment process rather than
 * somebody's psql: gp_cluster.c reads it, and every role macro follows.  The
 * content id is in it as well, so that a log line on a segment says which
 * connection of which coordinator session it belongs to.
 */
static char *
qe_identity_option(int content)
{
	StringInfoData buf;

	initStringInfo(&buf);
	/*
	 * libpq's "options" splits on whitespace, so the value may hold none; the
	 * three numbers are joined with characters no shell or parser will take an
	 * interest in.
	 */
	appendStringInfo(&buf, "-c gp.qe_identity=seg%d/dbid%d/sess%d",
					 content, GpClusterDbid(), MyProcPid);

	return buf.data;
}

/*
 * Open the session's connections, one per segment.
 *
 * Every segment or none: a gang that is missing a segment would answer a
 * query with part of the table, which is the one failure that must not be
 * quiet.
 */
static void
gang_connect(void)
{
	const GpSegmentConfig *segs;
	int			nsegs;
	MemoryContext oldcxt;
	const char *dbname = get_database_name(MyDatabaseId);
	const char *username = GetUserNameFromId(GetSessionUserId(), false);

	Assert(gang == NULL);

	segs = GpClusterSegments(&nsegs);
	if (nsegs == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this server has no segments to dispatch to"),
				 errhint("\"gp.cluster_config\" names the file that lists this cluster's nodes.")));

	/*
	 * Every node reads the same file, so a segment knows where the other
	 * segments are and would dispatch to them -- and to itself -- if it were
	 * asked to.  Only the coordinator dispatches: that is what makes one
	 * statement one statement, and it is what Cloudberry's own role check
	 * means.  (Running it here found a segment doing exactly that.)
	 */
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("only the coordinator dispatches to the segments"),
				 errdetail("This node has content id %d.", GpClusterContentId())));

	if (!exit_callback_registered)
	{
		before_shmem_exit(gang_atexit, 0);
		exit_callback_registered = true;
	}

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	gang = (GpGang *) palloc0(sizeof(GpGang));
	gang->conns = (GpSegmentConn *) palloc0_array(GpSegmentConn, nsegs);
	gang->nconns = nsegs;
	MemoryContextSwitchTo(oldcxt);

	for (int i = 0; i < nsegs; i++)
	{
		const char *keywords[10];
		const char *values[10];
		char		portbuf[16];
		char	   *options;
		int			n = 0;
		PGconn	   *conn;

		snprintf(portbuf, sizeof(portbuf), "%d", segs[i].port);
		options = qe_identity_option(segs[i].content);

		keywords[n] = "host";
		values[n++] = segs[i].hostname;
		keywords[n] = "port";
		values[n++] = portbuf;
		keywords[n] = "dbname";
		values[n++] = dbname;
		keywords[n] = "user";
		values[n++] = username;
		keywords[n] = "application_name";
		values[n++] = "cloudberry dispatcher";
		keywords[n] = "client_encoding";
		values[n++] = GetDatabaseEncodingName();
		keywords[n] = "options";
		values[n++] = options;
		if (gp_internal_passfile != NULL && gp_internal_passfile[0] != '\0')
		{
			keywords[n] = "passfile";
			values[n++] = gp_internal_passfile;
		}
		keywords[n] = NULL;
		values[n] = NULL;

		conn = libpqsrv_connect_params(keywords, values, false,
									   dispatch_wait_event());

		if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
		{
			char	   *msg = conn ? pstrdup(PQerrorMessage(conn)) : "out of memory";

			/* Take the whole gang down: a partial one answers with part of a table. */
			gang->conns[i].conn = conn;
			gang_close();

			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to segment %d (%s:%d)",
							segs[i].content, segs[i].hostname, segs[i].port),
					 errdetail_internal("%s", msg)));
		}

		gang->conns[i].content = segs[i].content;
		gang->conns[i].seg = &segs[i];
		gang->conns[i].conn = conn;
		gang->conns[i].busy = false;
	}

	/*
	 * One wait set for the gang, built once: the sockets do not change while
	 * the connections live, and building an epoll set per row would cost more
	 * than the rows.  It has no resource owner, because the gang outlives the
	 * transaction that opened it.
	 */
	gang->wes = CreateWaitEventSet(NULL, nsegs + 2);
	gang->wes_latch_pos = AddWaitEventToSet(gang->wes, WL_LATCH_SET,
											PGINVALID_SOCKET, MyLatch, NULL);
	AddWaitEventToSet(gang->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);
	for (int i = 0; i < nsegs; i++)
		AddWaitEventToSet(gang->wes, WL_SOCKET_READABLE,
						  PQsocket(gang->conns[i].conn), NULL,
						  &gang->conns[i]);
}

static GpGang *
gang_get(void)
{
	if (gang == NULL)
		gang_connect();
	return gang;
}

/* ------------------------------------------------------------------------- */
/* Sending, and waiting                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Wait until at least one connection has something to say, or an interrupt
 * arrives.  The caller loops over the connections afterwards; this only stops
 * the backend from spinning.
 */
static void
gang_wait(GpGang *g)
{
	WaitEvent	occurred[1];

	/* One event is enough: the caller re-reads every busy connection. */
	CHECK_FOR_INTERRUPTS();

	if (WaitEventSetWait(g->wes, -1, occurred, 1, dispatch_wait_event()) > 0)
	{
		if (occurred[0].events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

/*
 * Remember why a segment failed, in the caller's context.
 */
static void
collect_error(List **errors, int content, PGresult *res, PGconn *conn)
{
	GpSegmentError *err = (GpSegmentError *) palloc0(sizeof(GpSegmentError));
	const char *field;

	err->content = content;

	field = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	err->sqlstate = field ? pstrdup(field) : NULL;

	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
	if (field == NULL)
		field = PQerrorMessage(conn);
	err->message = pstrdup(field ? field : "unknown error");
	/* libpq's connection-level message ends in a newline; a message does not. */
	if (err->message[0] != '\0' &&
		err->message[strlen(err->message) - 1] == '\n')
		err->message[strlen(err->message) - 1] = '\0';

	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL) : NULL;
	err->detail = field ? pstrdup(field) : NULL;
	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_HINT) : NULL;
	err->hint = field ? pstrdup(field) : NULL;
	field = res ? PQresultErrorField(res, PG_DIAG_CONTEXT) : NULL;
	err->context = field ? pstrdup(field) : NULL;

	*errors = lappend(*errors, err);
}

/*
 * Raise what the segments said.
 *
 * The first segment's message is the message, with its own SQLSTATE, so that a
 * unique violation on a segment is a unique violation here; the segment it came
 * from is in the detail, as Cloudberry puts "(seg0 host:port)" in its own.  The
 * rest are counted, because a statement that fails on one segment usually fails
 * on all of them and repeating it three times helps nobody.
 */
static void
raise_segment_errors(List *errors)
{
	GpSegmentError *first = (GpSegmentError *) linitial(errors);
	const GpSegmentConfig *seg = GpClusterSegmentByContent(first->content);
	StringInfoData detail;

	initStringInfo(&detail);
	appendStringInfo(&detail, "segment %d (%s:%d)", first->content,
					 seg ? seg->hostname : "?", seg ? seg->port : 0);
	if (first->detail != NULL)
		appendStringInfo(&detail, ": %s", first->detail);
	if (list_length(errors) > 1)
		appendStringInfo(&detail, "; %d other segments failed too",
						 list_length(errors) - 1);

	ereport(ERROR,
			(errcode(first->sqlstate ? MAKE_SQLSTATE(first->sqlstate[0],
													 first->sqlstate[1],
													 first->sqlstate[2],
													 first->sqlstate[3],
													 first->sqlstate[4])
			 : ERRCODE_INTERNAL_ERROR),
			 errmsg("%s", first->message),
			 errdetail_internal("%s", detail.data),
			 first->hint ? errhint("%s", first->hint) : 0));
}

/*
 * Read every busy connection to the end of its results.
 *
 * Every segment is waited for even after one has failed, so that the
 * connections are left idle and usable; a connection that broke is not, and
 * takes the gang with it.
 */
static void
gang_wait_all(GpGang *g, PGresult **keep)
{
	List	   *errors = NIL;
	bool		broken = false;
	int			nbusy;

	do
	{
		nbusy = 0;
		for (int i = 0; i < g->nconns; i++)
		{
			GpSegmentConn *c = &g->conns[i];

			if (!c->busy)
				continue;

			if (PQconsumeInput(c->conn) == 0)
			{
				collect_error(&errors, c->content, NULL, c->conn);
				c->busy = false;
				broken = true;
				continue;
			}

			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);
				ExecStatusType status;

				if (res == NULL)
				{
					c->busy = false;
					break;
				}

				status = PQresultStatus(res);
				if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
					status != PGRES_EMPTY_QUERY)
				{
					collect_error(&errors, c->content, res, c->conn);
					if (status == PGRES_FATAL_ERROR &&
						PQstatus(c->conn) == CONNECTION_BAD)
						broken = true;
				}
				else if (keep != NULL && keep[i] == NULL &&
						 status == PGRES_TUPLES_OK)
				{
					keep[i] = res;
					continue;	/* the caller frees it */
				}
				PQclear(res);
			}

			if (c->busy)
				nbusy++;
		}

		if (nbusy > 0)
			gang_wait(g);
	} while (nbusy > 0);

	if (broken)
		gang_close();

	if (errors != NIL)
		raise_segment_errors(errors);
}

/*
 * Stop whatever the segments are still doing and read what is left, so that
 * every connection is idle again.  One that will not come back idle is closed,
 * and the gang with it: half a gang answers with half a table.
 *
 * It may not raise -- an abort calls it while an error is being handled -- so
 * a segment that does not answer costs the gang rather than an error.
 */
static void
gang_cancel_and_drain(void)
{
	GpGang	   *g = gang;

	if (g == NULL)
		return;

	for (int i = 0; i < g->nconns; i++)
	{
		if (g->conns[i].busy)
		{
			const char *err = libpqsrv_cancel(g->conns[i].conn,
											  GetCurrentTimestamp() +
											  30 * USECS_PER_SEC);

			if (err != NULL)
				elog(DEBUG1, "could not cancel the query on segment %d: %s",
					 g->conns[i].content, err);
		}
	}

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		while (c->busy)
		{
			if (PQconsumeInput(c->conn) == 0)
			{
				gang_close();
				return;
			}

			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);

				if (res == NULL)
				{
					c->busy = false;
					break;
				}
				PQclear(res);
			}

			if (c->busy)
			{
				WaitEvent	occurred[1];

				/*
				 * Not gang_wait(): that checks for interrupts, and this runs
				 * where an error is already on its way.  A segment that never
				 * answers would hang the abort, so the wait has a deadline and
				 * the gang is dropped when it passes.
				 */
				if (WaitEventSetWait(g->wes, 30 * 1000, occurred, 1,
									 dispatch_wait_event()) == 0)
				{
					gang_close();
					return;
				}
				if (occurred[0].events & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}
	}
}

void
GpDispatchCommand(const char *sql)
{
	GpGang	   *g = gang_get();

	for (int i = 0; i < g->nconns; i++)
	{
		if (!PQsendQuery(g->conns[i].conn, sql))
		{
			char	   *msg = pstrdup(PQerrorMessage(g->conns[i].conn));

			gang_close();
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send a statement to segment %d",
							i),
					 errdetail_internal("%s", msg)));
		}
		g->conns[i].busy = true;
	}

	gang_wait_all(g, NULL);
}

void
GpDispatchCommandOnContent(int content, const char *sql)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];

	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	if (!PQsendQuery(c->conn, sql))
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a statement to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
	c->busy = true;

	gang_wait_all(g, NULL);
}

/* ------------------------------------------------------------------------- */
/* Rows on the way back                                                      */
/* ------------------------------------------------------------------------- */

/*
 * How a column of a segment's answer becomes a Datum.
 *
 * Binary for the whole result or text for the whole result, because libpq asks
 * for one format for all of the columns: a type with no binary send function
 * -- an extension's, usually -- makes it text for its neighbours too.  Text
 * costs a conversion and loses nothing: PostgreSQL 19's float output is
 * round-trip exact, which is what made text safe to fall back to.
 */
typedef struct GpColumnIn
{
	FmgrInfo	proc;
	Oid			ioparam;
	int32		typmod;
} GpColumnIn;

struct GpGatherState
{
	GpGang	   *gang;
	TupleDesc	tupdesc;
	bool		binary;
	GpColumnIn *columns;
	int			next;			/* which connection to look at first */
	List	   *errors;
	bool		broken;
};

static bool
gather_can_use_binary(TupleDesc tupdesc)
{
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			typreceive;
		Oid			ioparam;

		if (att->attisdropped)
			continue;
		getTypeBinaryInputInfo(att->atttypid, &typreceive, &ioparam);
		if (!OidIsValid(typreceive))
			return false;
	}
	return true;
}

GpGatherState *
GpGatherStart(const char *sql, TupleDesc tupdesc)
{
	GpGatherState *gather = (GpGatherState *) palloc0(sizeof(GpGatherState));
	GpGang	   *g = gang_get();

	gather->gang = g;
	gather->tupdesc = tupdesc;
	gather->binary = gather_can_use_binary(tupdesc);
	gather->columns = (GpColumnIn *) palloc0_array(GpColumnIn, tupdesc->natts);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			proc;
		Oid			ioparam;

		if (att->attisdropped)
			continue;
		if (gather->binary)
			getTypeBinaryInputInfo(att->atttypid, &proc, &ioparam);
		else
			getTypeInputInfo(att->atttypid, &proc, &ioparam);
		fmgr_info(proc, &gather->columns[i].proc);
		gather->columns[i].ioparam = ioparam;
		gather->columns[i].typmod = att->atttypmod;
	}

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (!PQsendQueryParams(c->conn, sql, 0, NULL, NULL, NULL, NULL,
							   gather->binary ? 1 : 0))
		{
			char	   *msg = pstrdup(PQerrorMessage(c->conn));

			gang_close();
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send a query to segment %d", c->content),
					 errdetail_internal("%s", msg)));
		}

		/*
		 * One row at a time, so that the coordinator holds a row per segment
		 * rather than a segment's whole answer, and a segment with more rows
		 * does not wait for one with fewer.
		 */
		if (!PQsetSingleRowMode(c->conn))
			elog(ERROR, "could not switch segment %d to single-row mode",
				 c->content);
		c->busy = true;
	}

	return gather;
}

/*
 * One row of a segment's answer, into the slot.
 */
static void
gather_store_row(GpGatherState *gather, PGresult *res, TupleTableSlot *slot)
{
	TupleDesc	tupdesc = gather->tupdesc;

	if (PQnfields(res) != tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a segment answered with %d columns, not %d",
						PQnfields(res), tupdesc->natts)));

	ExecClearTuple(slot);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (PQgetisnull(res, 0, i))
		{
			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			continue;
		}

		slot->tts_isnull[i] = false;

		if (gather->binary)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			appendBinaryStringInfo(&buf, PQgetvalue(res, 0, i),
								   PQgetlength(res, 0, i));
			slot->tts_values[i] = ReceiveFunctionCall(&gather->columns[i].proc,
													  &buf,
													  gather->columns[i].ioparam,
													  gather->columns[i].typmod);
			pfree(buf.data);
		}
		else
			slot->tts_values[i] = InputFunctionCall(&gather->columns[i].proc,
												   PQgetvalue(res, 0, i),
												   gather->columns[i].ioparam,
												   gather->columns[i].typmod);
	}

	ExecStoreVirtualTuple(slot);
}

bool
GpGatherNext(GpGatherState *gather, TupleTableSlot *slot, int *content)
{
	GpGang	   *g = gather->gang;

	for (;;)
	{
		bool		any_busy = false;
		bool		made_progress = false;

		for (int n = 0; n < g->nconns; n++)
		{
			int			i = (gather->next + n) % g->nconns;
			GpSegmentConn *c = &g->conns[i];

			if (!c->busy)
				continue;

			any_busy = true;

			if (PQconsumeInput(c->conn) == 0)
			{
				collect_error(&gather->errors, c->content, NULL, c->conn);
				c->busy = false;
				gather->broken = true;
				continue;
			}

			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);
				ExecStatusType status;

				if (res == NULL)
				{
					c->busy = false;
					made_progress = true;
					break;
				}

				status = PQresultStatus(res);
				if (status == PGRES_SINGLE_TUPLE)
				{
					gather_store_row(gather, res, slot);
					PQclear(res);
					if (content != NULL)
						*content = c->content;
					/* Look at the next segment first, to take turns. */
					gather->next = (i + 1) % g->nconns;
					return true;
				}
				if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
				{
					collect_error(&gather->errors, c->content, res, c->conn);
					if (PQstatus(c->conn) == CONNECTION_BAD)
						gather->broken = true;
				}
				PQclear(res);
			}
		}

		if (!any_busy)
			break;
		if (!made_progress)
			gang_wait(g);
	}

	if (gather->broken)
		gang_close();
	if (gather->errors != NIL)
		raise_segment_errors(gather->errors);

	return false;
}

void
GpGatherEnd(GpGatherState *gather)
{
	/*
	 * A gather that was not read to the end -- a LIMIT above it, or an error
	 * -- leaves rows on the way.  Cloudberry stops a segment with its own
	 * "squelch" message; here the honest thing is to cancel what is still
	 * running and read what is already in flight, so that the connections are
	 * idle again and the next statement can use them.
	 */
	gang_cancel_and_drain();
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A transaction that ends while a segment is still working leaves a connection
 * that will not answer the next statement.  Cancel what is running and read
 * what is left; a connection that will not come back is closed, and the gang
 * with it, because half a gang answers with half a table.
 */
static void
dispatch_xact_callback(XactEvent event, void *arg)
{
	if (gang == NULL)
		return;
	if (event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT)
		return;

	/*
	 * An error is already being handled here, so nothing this raises would be
	 * reported; libpqsrv_cancel() can raise on an out-of-memory, so it is
	 * caught and the gang dropped instead.
	 */
	PG_TRY();
	{
		gang_cancel_and_drain();
	}
	PG_CATCH();
	{
		FlushErrorState();
		gang_close();
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_exec_on_segments);

/*
 * gp.exec_on_segments(sql)
 *		Run a statement on every segment, and report what each one said.
 *
 * The answer is the first column of the first row, as text, because this is
 * for asking a cluster about itself -- "what does each segment think it is",
 * "how many rows does each one hold" -- and not for reading a table, which is
 * what the scan of a distributed table does.
 *
 * Superuser only.  It runs arbitrary SQL on a machine the caller may have no
 * other way to reach, and a segment is not a place to widen anyone's reach.
 */
Datum
gp_exec_on_segments(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	GpGang	   *g;
	PGresult  **results;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to run a statement on the segments")));

	InitMaterializedSRF(fcinfo, 0);

	g = gang_get();
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	for (int i = 0; i < g->nconns; i++)
	{
		if (!PQsendQuery(g->conns[i].conn, sql))
		{
			char	   *msg = pstrdup(PQerrorMessage(g->conns[i].conn));

			gang_close();
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send a statement to segment %d",
							g->conns[i].content),
					 errdetail_internal("%s", msg)));
		}
		g->conns[i].busy = true;
	}

	gang_wait_all(g, results);

	for (int i = 0; i < g->nconns; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, true};

		values[0] = Int32GetDatum(g->conns[i].content);
		if (results[i] != NULL && PQntuples(results[i]) > 0 &&
			PQnfields(results[i]) > 0 && !PQgetisnull(results[i], 0, 0))
		{
			values[1] = CStringGetTextDatum(PQgetvalue(results[i], 0, 0));
			nulls[1] = false;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		if (results[i] != NULL)
			PQclear(results[i]);
	}

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_dist_random);

/*
 * gp.dist_random(NULL::t)
 *		The rows of a relation as the segments hold them.
 *
 * Cloudberry's gp_dist_random('t') is a function whose result type its planner
 * fills in from the argument, which an extension cannot do.  PostgreSQL has
 * the same effect through polymorphism: the argument is a value of the
 * relation's own row type -- NULL::t says which relation without reading one --
 * and the result is a set of that type.  Cloudberry's own tests reach for this
 * constantly, which is why it is here rather than later.
 *
 * It is the scan of a distributed table with nothing planned around it: no
 * qual pushed down, no column left out.  What the executor will do with a
 * distributed table is the same gather, under a plan.
 */
Datum
gp_dist_random(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid;
	Relation	rel;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	GpGatherState *gather;
	StringInfoData sql;

	if (!OidIsValid(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine which relation to read"),
				 errhint("Write the relation's row type, as in gp.dist_random(NULL::mytable).")));

	relid = get_typ_typrelid(argtype);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("type %s is not a relation's row type",
						format_type_be(argtype))));

	/* The same lock an ordinary scan of it would take. */
	rel = table_open(relid, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT * FROM %s",
					 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
												RelationGetRelationName(rel)));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	gather = GpGatherStart(sql.data, tupdesc);

	PG_TRY();
	{
		while (GpGatherNext(gather, slot, NULL))
			tuplestore_puttupleslot(rsinfo->setResult, slot);
	}
	PG_FINALLY();
	{
		GpGatherEnd(gather);
	}
	PG_END_TRY();

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

void
GpDispatchInit(void)
{
	DefineCustomStringVariable("gp.internal_passfile",
							   "Password file the dispatcher hands libpq.",
							   "The dispatcher authenticates like any other "
							   "client, so a cluster that asks for SCRAM needs "
							   "the password somewhere the server can read and "
							   "a user cannot: a file, not a setting.",
							   &gp_internal_passfile,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	RegisterXactCallback(dispatch_xact_callback, NULL);
}
