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
 * THE TRANSACTION.  Whatever the segments are sent is done inside the
 * coordinator's transaction: the first statement a transaction dispatches
 * opens one on every segment, a savepoint here is a savepoint there once
 * something is sent inside it, and the segments commit when the coordinator
 * does and roll back when it does.  So BEGIN; CREATE TABLE ...; ROLLBACK
 * leaves no table anywhere, and a statement that fails on a segment undoes
 * itself on the coordinator.  Each statement is sent with the coordinator's
 * snapshot of it, which the segments read as a distributed one, and a
 * transaction that wrote on a segment commits in two phases, decided by the
 * coordinator's own commit record (gp_dtx.c).
 *
 * READERS.  A statement whose slices run at once needs more than one
 * backend on a segment: the writer runs one slice, and each other slice runs
 * on a reader -- Cloudberry's reader gangs -- which is one more connection of
 * the session to the segment, with the writer's identity, kept for the
 * session once opened.  A reader's transaction is its own, begun for each
 * slice REPEATABLE READ and READ ONLY and read as a part of the writer's
 * (gp_share.c), so the coordinator's transaction, its savepoints and its
 * commit remain the writer's alone.  The readers are in the gang's wait set:
 * a slice that fails on one fails whatever the coordinator is waiting for,
 * and every reader is stopped and heard first, so that the error raised is
 * the one that caused the rest.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/dispatcher/ (cdbdisp.c, cdbdisp_query.c, cdbconn.c,
 *	  cdbgang.c), less the parts that exist because Cloudberry speaks its own
 *	  protocol, and the one-phase half of cdbtm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/objectaddress.h"
#include "commands/seclabel.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
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
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_dtx.h"
#include "gp_fault.h"
#include "gp_label.h"

/* Where libpq finds the password for the segments; see the file header. */
static char *gp_internal_passfile = NULL;

/*
 * The settings a segment has to share with the coordinator for a statement to
 * mean the same thing there: which schema a name is looked up in, which role
 * is doing it, how a date is read and written, where a table goes.
 * Cloudberry marks the ones it ships with GUC_GPDB_NEED_SYNC, 189 of them;
 * these are the ones anything the port dispatches yet can tell apart.
 * default_tablespace is not among them: a tablespace is a directory on one
 * machine, and at M2 each node keeps its own.
 */
static const char *const synced_settings[] = {
	"search_path",
	"role",
	"DateStyle",
	"IntervalStyle",
	"TimeZone",
	"default_table_access_method",
	"check_function_bodies",
	"bytea_output",
	"extra_float_digits",
	"standard_conforming_strings",
	"xmloption",
	"lc_monetary",
	"lc_numeric",
	"lc_time",
};

#define NUM_SYNCED_SETTINGS	lengthof(synced_settings)

/* How many rows a segment sends at a time when a relation is read. */
#define GATHER_FETCH_ROWS	1000

/* One segment's connection. */
typedef struct GpSegmentConn
{
	int			content;
	const GpSegmentConfig *seg;
	PGconn	   *conn;
	bool		busy;			/* a statement was sent and has not finished */

	/*
	 * When what is in flight is a gather's next batch, whose it is.  Several
	 * gathers share the connections -- a join reads two tables -- and a batch
	 * one of them asked for ahead of need is set aside for it before anything
	 * else is sent; see conn_park().
	 */
	struct GpGatherSeg *fetching;

	/* Where its backend receives a Motion's rows; NULL until asked. */
	char	   *icaddress;
} GpSegmentConn;

/*
 * A reader: one more backend on a segment, for a slice of a statement whose
 * slices run at once.  Its transaction is its own, begun for each slice and
 * read as a part of the writer's (gp_share.c); the connection is the
 * session's, kept for the next statement.
 */
typedef struct GpReaderConn
{
	int			content;
	PGconn	   *conn;			/* NULL once broken */
	char	   *icaddress;
	bool		busy;			/* its slice is running */
	struct GpStream *stream;	/* the statement it is taken for */
	char	   *sent[NUM_SYNCED_SETTINGS];
} GpReaderConn;

typedef struct GpGang
{
	int			nconns;
	GpSegmentConn *conns;
	WaitEventSet *wes;			/* MyLatch plus every connection's socket */
	List	   *readers;		/* GpReaderConn, readers included in wes */

	/* What the segments have been told of the settings above; NULL unknown. */
	char	   *sent[NUM_SYNCED_SETTINGS];
} GpGang;

static GpGang *gang = NULL;
static bool exit_callback_registered = false;

/* The connection a COPY ... FROM STDIN is going through, if any. */
static GpSegmentConn *copying = NULL;

/*
 * The coordinator's transaction, as the segments know it.  "depth" counts the
 * transaction levels they have been given: 1 for the BEGIN, one more for each
 * savepoint, named after the level it stands for.
 */
static bool gang_in_xact = false;
static int	gang_xact_depth = 0;
static bool gang_xact_lost = false;	/* the gang closed with work in it */

/*
 * The distributed snapshot the segments were last sent in this transaction,
 * as the setting carries it; NULL before the first (gp_dtx.c).
 */
static char *gang_ds_sent = NULL;

/*
 * A transaction's parts prepared on the segments, between the two phases:
 * the gid, and which connections were asked to prepare one.
 */
static char dtx_gid[GP_DTX_GIDLEN];
static bool *dtx_prepared = NULL;	/* by connection, in TopMemoryContext */
static int	dtx_prepared_size = 0;
static int	dtx_nprepared = 0;

/* Names the cursors of the gathers of one transaction apart. */
static uint32 gather_counter = 0;

/* What a segment answered when it failed. */
typedef struct GpSegmentError
{
	int			content;
	char	   *sqlstate;
	char	   *message;
	char	   *detail;
	char	   *hint;
} GpSegmentError;

/* A statement whose slices run at once: the readers running them. */
struct GpStream
{
	List	   *readers;		/* GpReaderConn, as they were added */
	List	   *errors;			/* GpSegmentError, what they answered */
};

/* The ones running; in TopMemoryContext, as the readers point at them. */
static List *active_streams = NIL;

/* How many readers a segment may have for one session. */
#define MAX_READERS_PER_SEGMENT	64

static void gang_close(void);
static void gang_build_wes(GpGang *g);
static void segment_notice_receiver(void *arg, const struct pg_result *res);
static void readers_poll(void);
static void streams_raise_if_failed(void);
static void streams_release(void);
static void readers_cancel_and_drain(List **errors);

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
 * session ends, and when an abort finds a segment that will not answer.
 *
 * A gang that closes with a transaction open on it takes the segments' part of
 * that transaction with it, and the coordinator's part must not commit alone:
 * gang_xact_lost makes the commit fail instead.
 */
static void
gang_close(void)
{
	if (gang == NULL)
		return;

	if (gang_in_xact)
		gang_xact_lost = true;
	gang_in_xact = false;
	gang_xact_depth = 0;
	copying = NULL;
	if (gang_ds_sent != NULL)
		pfree(gang_ds_sent);
	gang_ds_sent = NULL;

	for (int i = 0; i < gang->nconns; i++)
	{
		if (gang->conns[i].conn != NULL)
		{
			libpqsrv_disconnect(gang->conns[i].conn);
			gang->conns[i].conn = NULL;
		}
	}
	streams_release();
	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (r->conn != NULL)
			libpqsrv_disconnect(r->conn);
		r->conn = NULL;
	}
	if (gang->wes != NULL)
		FreeWaitEventSet(gang->wes);
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
		if (gang->sent[i] != NULL)
			pfree(gang->sent[i]);

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
	/*
	 * libpq's "options" splits on whitespace, so the value may hold none; the
	 * three numbers are joined with characters no shell or parser will take an
	 * interest in.
	 */
	char	   *option = psprintf("-c gp.qe_identity=seg%d/dbid%d/sess%d",
									content, GpClusterDbid(), MyProcPid);

	/* And the secret, which says it is this coordinator; see gp_cluster.c. */
	if (GpClusterHasSecret())
		option = psprintf("%s -c gp.qe_secret=%s", option, GpClusterSecret());
	return option;
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
	const char *dbname;
	const char *username;

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
	 * means.  (Running it found a segment doing exactly that.)
	 */
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("only the coordinator dispatches to the segments"),
				 errdetail("This node has content id %d.", GpClusterContentId())));

	dbname = get_database_name(MyDatabaseId);
	username = GetUserNameFromId(GetSessionUserId(), false);

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
		int			n = 0;
		PGconn	   *conn;

		snprintf(portbuf, sizeof(portbuf), "%d", segs[i].port);

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
		values[n++] = qe_identity_option(segs[i].content);
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
		PQsetNoticeReceiver(conn, segment_notice_receiver, NULL);
	}

	gang_build_wes(gang);
}

/*
 * One wait set for the gang, built when its connections change: the sockets
 * do not change while the connections live, and building an epoll set per
 * row would cost more than the rows.  It has no resource owner, because the
 * gang outlives the transaction that opened it.  The readers are in it too,
 * so that a slice that fails on one is heard of while the coordinator waits
 * for anything.
 */
static void
gang_build_wes(GpGang *g)
{
	if (g->wes != NULL)
		FreeWaitEventSet(g->wes);
	g->wes = CreateWaitEventSet(NULL, g->nconns + list_length(g->readers) + 2);
	AddWaitEventToSet(g->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
	/* A backend with no postmaster -- single-user mode -- has none to lose. */
	if (IsUnderPostmaster)
		AddWaitEventToSet(g->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  NULL, NULL);
	for (int i = 0; i < g->nconns; i++)
		AddWaitEventToSet(g->wes, WL_SOCKET_READABLE,
						  PQsocket(g->conns[i].conn), NULL, &g->conns[i]);
	foreach_ptr(GpReaderConn, r, g->readers)
	{
		if (r->conn != NULL)
			AddWaitEventToSet(g->wes, WL_SOCKET_READABLE, PQsocket(r->conn),
							  NULL, NULL);
	}
}

static GpGang *
gang_get(void)
{
	if (gang == NULL)
		gang_connect();
	return gang;
}

/* ------------------------------------------------------------------------- */
/* What the segments say besides their answers                               */
/* ------------------------------------------------------------------------- */

/*
 * A NOTICE, WARNING or INFO a segment sent -- a trigger's RAISE NOTICE on
 * the rows it inserted, say -- is the client's, as Cloudberry relays a QE's
 * (MPPnoticeReceiver, cdbconn.c).  libpq hands it to a receiver in the middle
 * of reading a result, where nothing may be raised, so it is queued there, in
 * malloc'd memory as Cloudberry's is, and raised here once the dispatcher is
 * back on its own ground: before a segment's error, since it came first, and
 * whenever the dispatcher waits.  Without Cloudberry's "(seg0 host:port
 * pid=...)" after the message, which its tests' init_file masks anyway.
 *
 * What the port's own statements to the segments make them say is not the
 * client's: a DDL statement's NOTICE is the coordinator's to give, once, and
 * it has; settings, labels and the transaction's BEGIN and COMMIT say nothing
 * the user asked about.  Those are sent quietly (notices_quiet).
 */
typedef struct SegmentNotice
{
	struct SegmentNotice *next;
	int			elevel;
	int			sqlerrcode;
	char	   *message;
	char	   *detail;
	char	   *hint;
	char		buf[FLEXIBLE_ARRAY_MEMBER];
} SegmentNotice;

static SegmentNotice *notices_head = NULL;
static SegmentNotice **notices_tail = &notices_head;
static int	notices_quiet = 0;

/*
 * libpq calls it with its own PGresult, not the wrapper that libpq-be-fe.h's
 * macros put in the name's place, so they are set aside around it, as
 * PostgreSQL's own libpqsrv_notice_receiver sets them aside.
 */
#undef PGresult
#undef PQresultErrorField
static void
segment_notice_receiver(void *arg, const struct pg_result *res)
{
	const char *severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
	const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	const char *fields[3];
	size_t		size = offsetof(SegmentNotice, buf);
	SegmentNotice *n;
	char	   *p;
	int			elevel;

	if (notices_quiet > 0 || severity == NULL)
		return;
	if (strcmp(severity, "NOTICE") == 0)
		elevel = NOTICE;
	else if (strcmp(severity, "WARNING") == 0)
		elevel = WARNING;
	else if (strcmp(severity, "INFO") == 0)
		elevel = INFO;
	else
		return;					/* LOG and DEBUG are the segment's own log's */

	fields[0] = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	fields[1] = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	fields[2] = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	if (fields[0] == NULL)
		return;
	for (int i = 0; i < 3; i++)
		if (fields[i] != NULL)
			size += strlen(fields[i]) + 1;

	/* nothing can be raised here: a notice there is no memory for is lost */
	n = malloc(size);
	if (n == NULL)
		return;
	n->next = NULL;
	n->elevel = elevel;
	n->sqlerrcode = (sqlstate != NULL && strlen(sqlstate) == 5)
		? MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2], sqlstate[3],
						sqlstate[4])
		: ERRCODE_SUCCESSFUL_COMPLETION;
	p = n->buf;
	n->message = n->detail = n->hint = NULL;
	for (int i = 0; i < 3; i++)
	{
		char	  **dest = (i == 0) ? &n->message : (i == 1) ? &n->detail : &n->hint;

		if (fields[i] == NULL)
			continue;
		strcpy(p, fields[i]);
		*dest = p;
		p += strlen(fields[i]) + 1;
	}

	*notices_tail = n;
	notices_tail = &n->next;
}
#define PGresult libpqsrv_PGresult
#define PQresultErrorField libpqsrv_PQresultErrorField

/* Raise what the segments said, in the order they said it. */
static void
flush_segment_notices(void)
{
	while (notices_head != NULL)
	{
		SegmentNotice *n = notices_head;
		SegmentNotice copy = *n;
		char	   *message = pstrdup(n->message);
		char	   *detail = n->detail ? pstrdup(n->detail) : NULL;
		char	   *hint = n->hint ? pstrdup(n->hint) : NULL;

		notices_head = n->next;
		if (notices_head == NULL)
			notices_tail = &notices_head;
		free(n);

		ereport(copy.elevel,
				(errcode(copy.sqlerrcode),
				 errmsg_internal("%s", message),
				 detail ? errdetail_internal("%s", detail) : 0,
				 hint ? errhint("%s", hint) : 0));
	}
}

/* Forget them: the statement failed, and what it said went with it. */
static void
drop_segment_notices(void)
{
	while (notices_head != NULL)
	{
		SegmentNotice *n = notices_head;

		notices_head = n->next;
		free(n);
	}
	notices_tail = &notices_head;
	notices_quiet = 0;
}

/* ------------------------------------------------------------------------- */
/* Sending, and waiting                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Wait until at least one connection has something to say, or an interrupt
 * arrives.  The caller loops over the connections afterwards; this only stops
 * the backend from spinning.
 */
static bool gather_poll(struct GpGatherSeg *s);

static void
gang_wait(GpGang *g)
{
	WaitEvent	occurred[1];

	flush_segment_notices();
	CHECK_FOR_INTERRUPTS();

	if (WaitEventSetWait(g->wes, -1, occurred, 1, dispatch_wait_event()) > 0)
	{
		if (occurred[0].events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}

	/*
	 * Take in whatever has arrived for a gather, whichever gather is waiting:
	 * the sockets are waited on level-triggered, so a batch nobody reads -- a
	 * join's other side prefetching, a merge waiting on one segment while the
	 * others answer -- would make every wait after this one return at once.
	 */
	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (c->busy && c->fetching != NULL)
			(void) gather_poll(c->fetching);
	}

	/* And whatever the readers have said: a slice that failed fails this. */
	readers_poll();
	streams_raise_if_failed();
}

static void conn_park(GpSegmentConn *c);
static void gang_wait_all_counting(GpGang *g, uint64 *counts, int content,
								   int nsegments);

/*
 * Is this connection's segment one a statement is for: the one content
 * named, or else the first nsegments -- a partial table's -- or every one
 * where that is 0.
 */
static inline bool
conn_asked(const GpSegmentConn *c, int content, int nsegments)
{
	if (content >= 0)
		return c->content == content;
	return nsegments <= 0 || c->content < nsegments;
}

/* Send a statement to one segment, as the simple protocol sends it. */
static void
conn_send(GpSegmentConn *c, const char *sql)
{
	/* A gather's batch asked for ahead of need is set aside for it first. */
	if (c->busy && c->fetching != NULL)
		conn_park(c);
	if (c->busy)
		elog(ERROR, "segment %d is still busy with an earlier statement",
			 c->content);

	if (!PQsendQuery(c->conn, sql))
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));
		int			content = c->content;

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a statement to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
	c->busy = true;
}

static void
gang_send_all(GpGang *g, const char *sql)
{
	for (int i = 0; i < g->nconns; i++)
		conn_send(&g->conns[i], sql);
}

/* Remember why a segment failed, in the caller's context. */
static void
collect_error(List **errors, int content, PGresult *res, PGconn *conn,
			  const char *why)
{
	GpSegmentError *err = (GpSegmentError *) palloc0(sizeof(GpSegmentError));
	const char *field;

	err->content = content;

	field = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	err->sqlstate = field ? pstrdup(field) : NULL;

	if (why != NULL)
		field = why;
	else
	{
		field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
		if (field == NULL)
			field = PQerrorMessage(conn);
	}
	err->message = pstrdup(field ? field : "unknown error");
	/* libpq's connection-level message ends in a newline; a message does not. */
	if (err->message[0] != '\0' &&
		err->message[strlen(err->message) - 1] == '\n')
		err->message[strlen(err->message) - 1] = '\0';

	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL) : NULL;
	err->detail = field ? pstrdup(field) : NULL;
	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_HINT) : NULL;
	err->hint = field ? pstrdup(field) : NULL;

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
	GpSegmentError *first;
	const GpSegmentConfig *seg;
	StringInfoData detail;

	flush_segment_notices();

	/*
	 * With slices running at once, what fails first is often only where the
	 * failure arrived: a receiver whose sender stopped, a slice cancelled
	 * because another failed.  Every reader is stopped and heard, and the
	 * message is the first that is neither.
	 */
	if (active_streams != NIL)
	{
		readers_cancel_and_drain(&errors);
		foreach_ptr(GpSegmentError, err, errors)
		{
			if (err->sqlstate == NULL ||
				(strcmp(err->sqlstate, "58M01") != 0 &&
				 strcmp(err->sqlstate, "57014") != 0))
			{
				errors = list_delete_ptr(errors, err);
				errors = lcons(err, errors);
				break;
			}
		}
	}

	first = (GpSegmentError *) linitial(errors);
	seg = GpClusterSegmentByContent(first->content);

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
 * takes the gang with it.  "keep", when given, receives each segment's first
 * result with rows.  "commit" says the statement was a COMMIT, whose answer
 * is ROLLBACK -- and no error -- when the segment's transaction had already
 * failed; that is an error here.
 */
static void gang_wait_all_ex(GpGang *g, PGresult **keep, bool commit,
							 bool keep_commands);

static void
gang_wait_all(GpGang *g, PGresult **keep, bool commit)
{
	gang_wait_all_ex(g, keep, commit, false);
}

/* Keep each segment's last successful result, whatever it was. */
static void
gang_wait_all_keeping_commands(GpGang *g, PGresult **keep)
{
	gang_wait_all_ex(g, keep, false, true);
}

static void
gang_wait_all_ex(GpGang *g, PGresult **keep, bool commit, bool keep_commands)
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

			/* Not ours: a gather's batch, which that gather will read. */
			if (!c->busy || c->fetching != NULL)
				continue;

			if (PQconsumeInput(c->conn) == 0)
			{
				collect_error(&errors, c->content, NULL, c->conn, NULL);
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
					collect_error(&errors, c->content, res, c->conn, NULL);
					if (PQstatus(c->conn) == CONNECTION_BAD)
						broken = true;
				}
				else if (commit && strcmp(PQcmdStatus(res), "ROLLBACK") == 0)
					collect_error(&errors, c->content, res, c->conn,
								  "the segment's part of this transaction had already failed");
				else if (keep != NULL && keep_commands)
				{
					if (keep[i] != NULL)
						PQclear(keep[i]);
					keep[i] = res;
					continue;	/* the caller frees it */
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

	flush_segment_notices();
	if (errors != NIL)
		raise_segment_errors(errors);
}

/*
 * gang_wait_all(), keeping how many rows each statement changed: one count
 * per segment waited for (all, or the one "content" names), in content order.
 */
static void
gang_wait_all_counting(GpGang *g, uint64 *counts, int content, int nsegments)
{
	PGresult  **results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	int			n = 0;

	gang_wait_all_keeping_commands(g, results);

	for (int i = 0; i < g->nconns; i++)
	{
		if (!conn_asked(&g->conns[i], content, nsegments))
			continue;
		counts[n++] = results[i] ? strtou64(PQcmdTuples(results[i]), NULL, 10) : 0;
		if (results[i] != NULL)
			PQclear(results[i]);
	}
	pfree(results);
}

/*
 * Read whatever is in flight and throw it away, without raising: the paths
 * that call this are handling an error already.  A segment that does not
 * answer within a while, or whose connection breaks, costs the gang.
 */
static void
gang_drain_quietly(void)
{
	GpGang	   *g = gang;

	if (g == NULL)
		return;

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		c->fetching = NULL;
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
				 * answers would hang the abort, so the wait has a deadline.
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

/* Stop whatever the segments are doing, and read what is left. */
static void
gang_cancel_and_drain(void)
{
	if (gang == NULL)
		return;

	readers_cancel_and_drain(NULL);

	for (int i = 0; i < gang->nconns; i++)
	{
		if (gang->conns[i].busy)
		{
			const char *err = libpqsrv_cancel(gang->conns[i].conn,
											  GetCurrentTimestamp() +
											  30 * USECS_PER_SEC);

			if (err != NULL)
				elog(DEBUG1, "could not cancel the query on segment %d: %s",
					 gang->conns[i].content, err);
		}
	}
	gang_drain_quietly();
}

/* Send a statement to every segment and wait for it, without raising. */
static void
gang_send_all_quietly(const char *sql)
{
	if (gang == NULL)
		return;

	for (int i = 0; i < gang->nconns; i++)
	{
		if (!PQsendQuery(gang->conns[i].conn, sql))
		{
			gang_close();
			return;
		}
		gang->conns[i].busy = true;
	}
	gang_drain_quietly();
}

/* ------------------------------------------------------------------------- */
/* Readers, for statements whose slices run at once                          */
/* ------------------------------------------------------------------------- */

/*
 * Read whatever the busy readers have answered, without waiting; an error is
 * kept with the stream the reader runs a slice of.  An idle reader is read
 * too: a connection that broke says so there, and its socket would wake
 * every wait after this one.
 */
static void
readers_poll(void)
{
	bool		broken = false;

	if (gang == NULL)
		return;

	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (r->conn == NULL)
			continue;

		if (PQconsumeInput(r->conn) == 0)
		{
			if (r->busy && r->stream != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				collect_error(&r->stream->errors, r->content, NULL, r->conn,
							  NULL);
				MemoryContextSwitchTo(oldcxt);
			}
			libpqsrv_disconnect(r->conn);
			r->conn = NULL;
			r->busy = false;
			broken = true;
			continue;
		}

		while (r->busy && !PQisBusy(r->conn))
		{
			PGresult   *res = PQgetResult(r->conn);
			ExecStatusType status;

			if (res == NULL)
			{
				r->busy = false;
				break;
			}
			status = PQresultStatus(res);
			if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
				status != PGRES_EMPTY_QUERY && r->stream != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				collect_error(&r->stream->errors, r->content, res, r->conn,
							  NULL);
				MemoryContextSwitchTo(oldcxt);
			}
			PQclear(res);
		}
	}

	if (broken)
		gang_build_wes(gang);
}

/* A slice failed on a reader: the statement fails, with the best reason. */
static void
streams_raise_if_failed(void)
{
	List	   *errors = NIL;

	foreach_ptr(GpStream, stream, active_streams)
	{
		errors = list_concat(errors, stream->errors);
		stream->errors = NIL;
	}
	if (errors != NIL)
		raise_segment_errors(errors);
}

/*
 * Stop every reader that is still running a slice and read it to the end,
 * adding what the readers answered to *errors, when given.  Called with an
 * error on its way, so it raises nothing; a reader that does not answer
 * within a while is dropped.
 */
static void
readers_cancel_and_drain(List **errors)
{
	TimestampTz deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	bool		any;

	if (gang == NULL)
		return;

	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (r->conn != NULL && r->busy)
		{
			const char *err = libpqsrv_cancel(r->conn, deadline);

			if (err != NULL)
				elog(DEBUG1, "could not cancel the slice on segment %d: %s",
					 r->content, err);
		}
	}

	do
	{
		WaitEvent	occurred[1];

		readers_poll();
		any = false;
		foreach_ptr(GpReaderConn, r, gang->readers)
			if (r->conn != NULL && r->busy)
				any = true;
		if (!any)
			break;
		if (GetCurrentTimestamp() >= deadline)
		{
			foreach_ptr(GpReaderConn, r, gang->readers)
			{
				if (r->conn != NULL && r->busy)
				{
					libpqsrv_disconnect(r->conn);
					r->conn = NULL;
					r->busy = false;
				}
			}
			gang_build_wes(gang);
			break;
		}
		if (WaitEventSetWait(gang->wes, 1000, occurred, 1,
							 dispatch_wait_event()) > 0 &&
			(occurred[0].events & WL_LATCH_SET))
			ResetLatch(MyLatch);
	} while (any);

	foreach_ptr(GpStream, stream, active_streams)
	{
		if (errors != NULL)
			*errors = list_concat(*errors, stream->errors);
		stream->errors = NIL;
	}
}

/* The readers go back to the session, and the streams are forgotten. */
static void
streams_release(void)
{
	foreach_ptr(GpStream, stream, active_streams)
	{
		foreach_ptr(GpReaderConn, r, stream->readers)
			r->stream = NULL;
		list_free(stream->readers);
		pfree(stream);
	}
	list_free(active_streams);
	active_streams = NIL;
}

/* Run a statement on a reader and wait for it, raising what it answers. */
static void
reader_exec(GpReaderConn *r, const char *sql, char **value)
{
	PGresult   *res = libpqsrv_exec(r->conn, sql, dispatch_wait_event());
	ExecStatusType status = res ? PQresultStatus(res) : PGRES_FATAL_ERROR;

	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
	{
		List	   *errors = NIL;

		collect_error(&errors, r->content, res, r->conn, NULL);
		if (res != NULL)
			PQclear(res);
		if (PQstatus(r->conn) == CONNECTION_BAD)
		{
			libpqsrv_disconnect(r->conn);
			r->conn = NULL;
			gang_build_wes(gang);
		}
		raise_segment_errors(errors);
	}
	if (value != NULL)
		*value = PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)
			? MemoryContextStrdup(TopMemoryContext, PQgetvalue(res, 0, 0))
			: NULL;
	PQclear(res);
}

/* One more reader on a segment, with the writer's identity. */
static GpReaderConn *
reader_connect(GpGang *g, int content)
{
	const GpSegmentConfig *seg = GpClusterSegmentByContent(content);
	const char *keywords[10];
	const char *values[10];
	char		portbuf[16];
	int			n = 0;
	PGconn	   *conn;
	GpReaderConn *r;

	snprintf(portbuf, sizeof(portbuf), "%d", seg->port);
	keywords[n] = "host";
	values[n++] = seg->hostname;
	keywords[n] = "port";
	values[n++] = portbuf;
	keywords[n] = "dbname";
	values[n++] = get_database_name(MyDatabaseId);
	keywords[n] = "user";
	values[n++] = GetUserNameFromId(GetSessionUserId(), false);
	keywords[n] = "application_name";
	values[n++] = "cloudberry reader";
	keywords[n] = "client_encoding";
	values[n++] = GetDatabaseEncodingName();
	keywords[n] = "options";

	/*
	 * A reader is a member of its writer's lock group, and a member cannot
	 * lead a group of its own: it starts no parallel workers.
	 */
	values[n++] = psprintf("%s -c max_parallel_workers_per_gather=0",
						   qe_identity_option(content));
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

		if (conn != NULL)
			libpqsrv_disconnect(conn);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect a reader to segment %d (%s:%d)",
						content, seg->hostname, seg->port),
				 errdetail_internal("%s", msg)));
	}

	r = MemoryContextAllocZero(TopMemoryContext, sizeof(GpReaderConn));
	r->content = content;
	r->conn = conn;
	PQsetNoticeReceiver(conn, segment_notice_receiver, NULL);
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		g->readers = lappend(g->readers, r);
		MemoryContextSwitchTo(oldcxt);
	}
	gang_build_wes(g);

	reader_exec(r, "SELECT gp_internal.interconnect_address()", &r->icaddress);
	if (r->icaddress == NULL)
		elog(ERROR, "segment %d gave its reader no interconnect address",
			 content);
	return r;
}

/* Tell a reader the settings that changed since it was last told. */
static void
reader_sync_settings(GpReaderConn *r)
{
	StringInfoData sql;
	const char *values[NUM_SYNCED_SETTINGS];
	bool		any = false;

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		values[i] = GetConfigOption(synced_settings[i], true, false);
		if (values[i] == NULL ||
			(r->sent[i] != NULL && strcmp(r->sent[i], values[i]) == 0))
			continue;
		appendStringInfo(&sql, "%spg_catalog.set_config(%s, %s, false)",
						 any ? ", " : "",
						 quote_literal_cstr(synced_settings[i]),
						 quote_literal_cstr(values[i]));
		any = true;
	}
	if (!any)
		return;

	reader_exec(r, sql.data, NULL);
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (values[i] == NULL)
			continue;
		if (r->sent[i] != NULL)
			pfree(r->sent[i]);
		r->sent[i] = MemoryContextStrdup(TopMemoryContext, values[i]);
	}
}

GpStream *
GpStreamBegin(void)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	GpStream   *stream = palloc0(sizeof(GpStream));

	(void) gang_get();
	active_streams = lappend(active_streams, stream);
	MemoryContextSwitchTo(oldcxt);
	return stream;
}

const char *
GpStreamWriterAddress(int content, int *pid)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		elog(ERROR, "there is no segment with content id %d", content);

	if (c->icaddress == NULL)
	{
		char	  **values = palloc0_array(char *, g->nconns);

		GpDispatchQueryFirstValues("SELECT gp_internal.interconnect_address()",
								   -1, values);
		for (int i = 0; i < g->nconns; i++)
		{
			if (values[i] == NULL)
				elog(ERROR, "segment %d gave no interconnect address",
					 g->conns[i].content);
			g->conns[i].icaddress = MemoryContextStrdup(TopMemoryContext,
														values[i]);
		}
	}
	*pid = PQbackendPID(c->conn);
	return c->icaddress;
}

int
GpStreamAddReader(GpStream *stream, int content, const char **address)
{
	GpGang	   *g = gang_get();
	GpReaderConn *found = NULL;
	int			count = 0;
	MemoryContext oldcxt;

	readers_poll();				/* a broken one is found broken now */
	foreach_ptr(GpReaderConn, r, g->readers)
	{
		if (r->content != content || r->conn == NULL)
			continue;
		count++;
		if (found == NULL && !r->busy && r->stream == NULL)
			found = r;
	}
	if (found == NULL)
	{
		if (count >= MAX_READERS_PER_SEGMENT)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("a statement needs more than %d readers on segment %d",
							MAX_READERS_PER_SEGMENT, content)));
		found = reader_connect(g, content);
	}

	found->stream = stream;
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	stream->readers = lappend(stream->readers, found);
	MemoryContextSwitchTo(oldcxt);
	*address = found->icaddress;
	return list_length(stream->readers) - 1;
}

void
GpStreamStartReader(GpStream *stream, int reader, const char *sql)
{
	GpReaderConn *r = (GpReaderConn *) list_nth(stream->readers, reader);

	if (r->conn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost a reader's connection to segment %d", r->content)));

	/* a slice that failed left its transaction open */
	if (PQtransactionStatus(r->conn) != PQTRANS_IDLE)
		reader_exec(r, "ROLLBACK", NULL);
	reader_sync_settings(r);

	if (!PQsendQuery(r->conn, sql))
	{
		char	   *msg = pstrdup(PQerrorMessage(r->conn));

		libpqsrv_disconnect(r->conn);
		r->conn = NULL;
		gang_build_wes(gang);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a slice to segment %d", r->content),
				 errdetail_internal("%s", msg)));
	}
	r->busy = true;
}

void
GpStreamEnd(GpStream *stream)
{
	for (;;)
	{
		bool		busy = false;

		readers_poll();
		streams_raise_if_failed();
		foreach_ptr(GpReaderConn, r, stream->readers)
			if (r->conn != NULL && r->busy)
				busy = true;
		if (!busy || gang == NULL)
			break;
		gang_wait(gang);
	}

	foreach_ptr(GpReaderConn, r, stream->readers)
		r->stream = NULL;
	active_streams = list_delete_ptr(active_streams, stream);
	list_free(stream->readers);
	pfree(stream);
}

/* ------------------------------------------------------------------------- */
/* The segments' part of the coordinator's transaction                       */
/* ------------------------------------------------------------------------- */

/*
 * Tell the segments the settings that changed since they were last told.
 *
 * set_config() rather than SET: it takes a value as SHOW prints it, which is
 * the one form every setting reads back -- a list like search_path included.
 */
static void
gang_sync_settings(GpGang *g)
{
	StringInfoData sql;
	const char *values[NUM_SYNCED_SETTINGS];
	bool		any = false;

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");

	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		values[i] = GetConfigOption(synced_settings[i], true, false);
		if (values[i] == NULL)
			continue;
		if (g->sent[i] != NULL && strcmp(g->sent[i], values[i]) == 0)
			continue;

		appendStringInfo(&sql, "%spg_catalog.set_config(%s, %s, false)",
						 any ? ", " : "",
						 quote_literal_cstr(synced_settings[i]),
						 quote_literal_cstr(values[i]));
		any = true;
	}

	if (!any)
		return;

	notices_quiet++;
	gang_send_all(g, sql.data);
	gang_wait_all(g, NULL, false);
	notices_quiet--;

	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (values[i] == NULL)
			continue;
		if (g->sent[i] != NULL)
			pfree(g->sent[i]);
		g->sent[i] = MemoryContextStrdup(TopMemoryContext, values[i]);
	}
}

/* Forget what the segments were told: a rollback may have undone it. */
static void
gang_forget_settings(void)
{
	if (gang == NULL)
		return;
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (gang->sent[i] != NULL)
			pfree(gang->sent[i]);
		gang->sent[i] = NULL;
	}
}

static const char *
isolation_level_name(void)
{
	switch (XactIsoLevel)
	{
		case XACT_SERIALIZABLE:
			return "SERIALIZABLE";
		case XACT_REPEATABLE_READ:
			return "REPEATABLE READ";
		case XACT_READ_UNCOMMITTED:
			return "READ UNCOMMITTED";
		default:
			return "READ COMMITTED";
	}
}

/*
 * The objects whose "gp" label this transaction changed and the segments have
 * not been sent yet.  What is sent is the label as it is when it is sent --
 * a savepoint rolled back, an object dropped, both come out right -- so an
 * object is noted once however often it changes.  In the transaction's
 * memory, and forgotten when it ends.
 */
static List *labels_pending = NIL;	/* of ObjectAddress * */
static bool labels_held = false;	/* a DDL tree is on its way */

void
GpDispatchNoteLabel(const ObjectAddress *object)
{
	ListCell   *lc;
	ObjectAddress *copy;
	MemoryContext oldcxt;

	/*
	 * Only what the coordinator changes, and only where there are segments.
	 * A shared object's label is not the segments' business: a tablespace,
	 * for one, is this node's alone.
	 */
	if (GpClusterIsSingleNode() || GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		IsSharedRelation(object->classId))
		return;

	foreach(lc, labels_pending)
	{
		ObjectAddress *o = (ObjectAddress *) lfirst(lc);

		if (o->classId == object->classId && o->objectId == object->objectId &&
			o->objectSubId == object->objectSubId)
			return;
	}

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	copy = palloc_object(ObjectAddress);
	*copy = *object;
	labels_pending = lappend(labels_pending, copy);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Send the segments the labels noted since they were last sent, each as the
 * SECURITY LABEL that writes it.  Inside the segments' transaction, so that
 * they commit or roll back with the coordinator's own.
 */
static void
gang_sync_labels(GpGang *g)
{
	List	   *pending = labels_pending;
	ListCell   *lc;

	if (labels_held || pending == NIL)
		return;
	labels_pending = NIL;

	foreach(lc, pending)
	{
		ObjectAddress *o = (ObjectAddress *) lfirst(lc);
		char	   *payload = GpDdlLabelPayload(o, GetSecurityLabel(o, GP_LABEL_PROVIDER));

		if (payload == NULL)
			continue;
		notices_quiet++;
		gang_send_all(g, payload);
		gang_wait_all(g, NULL, false);
		notices_quiet--;
		pfree(payload);
	}
}

/* Forget the distributed snapshot sent: a rollback may have undone it. */
static void
gang_forget_snapshot(void)
{
	if (gang_ds_sent != NULL)
		pfree(gang_ds_sent);
	gang_ds_sent = NULL;
}

/*
 * The statement's snapshot, as a distributed one: sent before the statement,
 * once per snapshot -- once per transaction when it is REPEATABLE READ, once
 * per statement when it is READ COMMITTED.  A segment may wait here for a
 * transaction the snapshot says committed and that it holds only prepared,
 * before its statement takes a snapshot of its own (gp_dtx.c).
 */
static void
gang_sync_snapshot(GpGang *g)
{
	char	   *ds;

	if (!ActiveSnapshotSet())
		return;
	ds = GpDtxSnapshotString(GetActiveSnapshot());
	if (ds == NULL || (gang_ds_sent != NULL && strcmp(ds, gang_ds_sent) == 0))
		return;

	notices_quiet++;
	gang_send_all(g, psprintf("SET LOCAL " GP_DTX_SNAPSHOT_SETTING " = '%s'", ds));
	gang_wait_all(g, NULL, false);
	notices_quiet--;

	gang_forget_snapshot();
	gang_ds_sent = MemoryContextStrdup(TopMemoryContext, ds);
}

/*
 * Get the segments ready for a statement: the settings it depends on, and --
 * unless it is one that runs in a transaction of its own -- the coordinator's
 * transaction, down to the savepoint it is being run in, and its snapshot.
 */
static void
gang_prepare(GpGang *g, bool in_xact)
{
	int			level;

	gang_sync_settings(g);

	if (!in_xact)
	{
		if (gang_in_xact)
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("cannot run this statement on the segments inside a transaction that has already used them")));
		return;
	}

	if (!gang_in_xact)
	{
		notices_quiet++;
		gang_send_all(g, psprintf("BEGIN ISOLATION LEVEL %s%s",
								  isolation_level_name(),
								  XactReadOnly ? " READ ONLY" : ""));
		gang_wait_all(g, NULL, false);
		notices_quiet--;
		gang_in_xact = true;
		gang_xact_depth = 1;
		gather_counter = 0;
	}

	level = GetCurrentTransactionNestLevel();
	while (gang_xact_depth < level)
	{
		notices_quiet++;
		gang_send_all(g, psprintf("SAVEPOINT gp_sp_%d", gang_xact_depth + 1));
		gang_wait_all(g, NULL, false);
		notices_quiet--;
		gang_xact_depth++;
	}

	gang_sync_snapshot(g);
	gang_sync_labels(g);
}

/* ------------------------------------------------------------------------- */
/* Two-phase commit                                                          */
/* ------------------------------------------------------------------------- */

static void
dtx_forget(void)
{
	dtx_nprepared = 0;
	dtx_gid[0] = '\0';
	if (dtx_prepared != NULL)
		memset(dtx_prepared, 0, dtx_prepared_size * sizeof(bool));
}

/*
 * The first phase, at PRE_COMMIT, while raising still undoes the
 * coordinator's part.  Each segment is asked whether its part wrote.  One
 * that did not commits now, having nothing to decide; one that did is
 * prepared under the coordinator's transaction ID, which this gives the
 * transaction if it had none -- a transaction that wrote on a segment only
 * has none -- and whose commit record, forced to disk before any segment is
 * told (ForceSyncCommit), is the decision (gp_dtx.c).  A failure here
 * raises, and the abort rolls back whatever was prepared.
 */
static void
gang_commit_first_phase(GpGang *g)
{
	PGresult  **status = palloc0_array(PGresult *, g->nconns);
	bool	   *writes = palloc0_array(bool, g->nconns);
	int			nwriters = 0;

	notices_quiet++;
	gang_send_all(g, "SELECT pg_catalog.pg_current_xact_id_if_assigned() IS NOT NULL");
	gang_wait_all(g, status, false);
	for (int i = 0; i < g->nconns; i++)
	{
		writes[i] = status[i] != NULL && PQntuples(status[i]) == 1 &&
			strcmp(PQgetvalue(status[i], 0, 0), "t") == 0;
		if (writes[i])
			nwriters++;
		if (status[i] != NULL)
			PQclear(status[i]);
	}

	/* whatever happens now, no segment is left in the transaction */
	gang_in_xact = false;
	gang_xact_depth = 0;

	if (nwriters > 0)
	{
		GpDtxFormGid(GetTopFullTransactionId(), dtx_gid);
		ForceSyncCommit();
		GP_FAULT("dtm_broadcast_prepare");
		if (dtx_prepared_size < g->nconns)
		{
			if (dtx_prepared != NULL)
				pfree(dtx_prepared);
			dtx_prepared = MemoryContextAllocZero(TopMemoryContext,
												  g->nconns * sizeof(bool));
			dtx_prepared_size = g->nconns;
		}
	}

	for (int i = 0; i < g->nconns; i++)
	{
		if (writes[i])
		{
			/* the abort rolls it back, whether or not it was prepared */
			dtx_prepared[i] = true;
			dtx_nprepared++;
			conn_send(&g->conns[i], psprintf("PREPARE TRANSACTION '%s'", dtx_gid));
		}
		else
			conn_send(&g->conns[i], "COMMIT");
	}
	gang_wait_all(g, NULL, true);
	notices_quiet--;
}

/*
 * COMMIT PREPARED or ROLLBACK PREPARED on the segments asked to prepare, and
 * how many of them it did not reach -- without raising: the second phase
 * and the abort are both past it.  An answer that the part does not exist,
 * or is busy, is no failure: the recovery process finished it or is
 * finishing it, and so did a PREPARE that failed.  A segment that does not
 * answer within a while, or whose connection broke, costs the gang.
 */
static int
gang_finish_prepared(bool commit)
{
	GpGang	   *g = gang;
	char	   *sql;
	int			nfailed = 0;
	TimestampTz deadline;

	if (dtx_nprepared == 0)
		return 0;
	if (g == NULL)
		return dtx_nprepared;

	sql = psprintf("%s PREPARED '%s'", commit ? "COMMIT" : "ROLLBACK", dtx_gid);
	for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (!dtx_prepared[i])
			continue;
		c->fetching = NULL;
		if (c->busy || !PQsendQuery(c->conn, sql))
		{
			nfailed++;
			dtx_prepared[i] = false;
			continue;
		}
		c->busy = true;
	}

	deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	for (;;)
	{
		bool		waiting = false;
		bool		broken = false;
		WaitEvent	occurred[1];

		for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
		{
			GpSegmentConn *c = &g->conns[i];

			if (!dtx_prepared[i] || !c->busy)
				continue;
			if (PQconsumeInput(c->conn) == 0)
			{
				nfailed++;
				c->busy = false;
				broken = true;
				continue;
			}
			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);
				const char *state;

				if (res == NULL)
				{
					c->busy = false;
					break;
				}
				state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
				if (PQresultStatus(res) != PGRES_COMMAND_OK &&
					(state == NULL ||
					 (strcmp(state, "42704") != 0 && strcmp(state, "55000") != 0)))
				{
					nfailed++;
					ereport(LOG,
							(errmsg("%s on segment %d failed: %s", sql, c->content,
									PQresultErrorMessage(res))));
				}
				PQclear(res);
			}
			if (c->busy)
				waiting = true;
		}
		if (broken)
		{
			gang_close();
			break;
		}
		if (!waiting)
			break;
		if (GetCurrentTimestamp() >= deadline)
		{
			for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
				if (dtx_prepared[i] && g->conns[i].busy)
					nfailed++;
			gang_close();
			break;
		}
		if (WaitEventSetWait(g->wes, 1000, occurred, 1, dispatch_wait_event()) > 0 &&
			(occurred[0].events & WL_LATCH_SET))
			ResetLatch(MyLatch);
	}
	return nfailed;
}

/*
 * The second phase, at COMMIT: the coordinator's commit record is on disk,
 * and nothing may be raised.  What a segment was not told, the recovery
 * process commits by the same record; this says so, and wakes it.
 */
static void
gang_commit_second_phase(void)
{
	int			nfailed;

	PG_TRY();
	{
		GP_FAULT("dtm_broadcast_commit_prepared");
		notices_quiet++;
		nfailed = gang_finish_prepared(true);
		notices_quiet--;
	}
	PG_CATCH();
	{
		FlushErrorState();
		gang_close();
		nfailed = Max(dtx_nprepared, 1);
	}
	PG_END_TRY();

	if (nfailed > 0)
	{
		ereport(WARNING,
				(errmsg("the distributed transaction \"%s\" was committed, but %d of its segments have not been told yet",
						dtx_gid, nfailed),
				 errdetail("Distributed transaction recovery commits their parts.")));
		GpDtxWakeRecovery();
	}
	dtx_forget();
}

static void
dispatch_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			if (gang_xact_lost)
			{
				gang_xact_lost = false;
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("lost the segments' part of this transaction"),
						 errdetail("A connection to a segment closed while the transaction was open.")));
			}

			/*
			 * Labels no statement has carried to the segments yet go now,
			 * in their transaction -- opening it, if nothing else did.
			 */
			if (labels_pending != NIL)
				gang_prepare(gang_get(), true);

			/*
			 * Before the coordinator commits: raising here still undoes the
			 * coordinator's part.  After it, nothing could, and the second
			 * phase follows at COMMIT.
			 */
			if (gang != NULL && gang_in_xact)
				gang_commit_first_phase(gang);
			break;

		case XACT_EVENT_PRE_PREPARE:
			if (gang_in_xact)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has used the segments"),
						 errdetail("A distributed transaction is prepared on the segments by the coordinator, which commits it.")));
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:

			/*
			 * An error is already being handled here, so nothing may be
			 * raised; libpqsrv_cancel() can raise on an out-of-memory, so it
			 * is caught, and the gang dropped instead.
			 */
			PG_TRY();
			{
				gang_cancel_and_drain();
				if (gang != NULL && gang_in_xact)
					gang_send_all_quietly("ROLLBACK");

				/*
				 * The first phase failed: what it prepared is rolled back,
				 * or left to the recovery process, which rolls it back too
				 * -- the coordinator's transaction did not commit.
				 */
				if (dtx_nprepared > 0)
				{
					GP_FAULT("dtm_broadcast_abort_prepared");
					if (gang_finish_prepared(false) > 0)
						GpDtxWakeRecovery();
				}
			}
			PG_CATCH();
			{
				FlushErrorState();
				gang_close();
				if (dtx_nprepared > 0)
					GpDtxWakeRecovery();
			}
			PG_END_TRY();

			gang_in_xact = false;
			gang_xact_depth = 0;
			gang_xact_lost = false;
			gang_forget_settings();
			gang_forget_snapshot();
			dtx_forget();
			streams_release();
			labels_pending = NIL;
			drop_segment_notices();
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			/* sent at PRE_COMMIT; the memory goes with the transaction */
			labels_pending = NIL;
			gang_forget_snapshot();
			if (dtx_nprepared > 0)
				gang_commit_second_phase();
			break;

		default:
			break;
	}
}

static void
dispatch_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();

	if (gang == NULL || !gang_in_xact || gang_xact_depth < level)
		return;

	switch (event)
	{
		case SUBXACT_EVENT_PRE_COMMIT_SUB:
			notices_quiet++;
			gang_send_all(gang, psprintf("RELEASE SAVEPOINT gp_sp_%d", level));
			gang_wait_all(gang, NULL, false);
			notices_quiet--;
			gang_xact_depth = level - 1;
			break;

		case SUBXACT_EVENT_ABORT_SUB:
			PG_TRY();
			{
				gang_cancel_and_drain();
				if (gang != NULL)
					gang_send_all_quietly(psprintf("ROLLBACK TO SAVEPOINT gp_sp_%d; RELEASE SAVEPOINT gp_sp_%d",
												   level, level));
			}
			PG_CATCH();
			{
				FlushErrorState();
				gang_close();
			}
			PG_END_TRY();
			gang_xact_depth = level - 1;
			gang_forget_settings();
			gang_forget_snapshot();
			streams_release();
			drop_segment_notices();
			break;

		default:
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* Statements                                                                */
/* ------------------------------------------------------------------------- */

void
GpDispatchCommand(const char *sql)
{
	GpGang	   *g = gang_get();

	gang_prepare(g, true);
	gang_send_all(g, sql);
	gang_wait_all(g, NULL, false);
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

	gang_prepare(g, true);
	conn_send(c, sql);
	gang_wait_all(g, NULL, false);
}

void
GpDispatchUtility(const char *payload, bool own_xact)
{
	GpGang	   *g = gang_get();

	/*
	 * A label the statement wrote may be of an object the statement makes --
	 * an extension's script labels its tables -- which the segments have
	 * only once they have run it.  So the labels follow it.
	 */
	labels_held = true;
	PG_TRY();
	{
		gang_prepare(g, !own_xact);
	}
	PG_FINALLY();
	{
		labels_held = false;
	}
	PG_END_TRY();
	/* the coordinator has said what the statement says, once */
	notices_quiet++;
	gang_send_all(g, payload);
	gang_wait_all(g, NULL, false);
	notices_quiet--;
	if (!own_xact)
		gang_sync_labels(g);
}

/*
 * A statement with parameters on every segment, the first nsegments of them,
 * or one, and how many rows each changed.  The parameters travel as text, of
 * the types given; "counts" gets one entry per segment asked, in content
 * order.
 */
void
GpDispatchCommandParams(const char *sql, int nparams, const Oid *types,
						const char *const *values, int content, int nsegments,
						uint64 *counts)
{
	GpGang	   *g = gang_get();
	PGresult  **results;
	int			n = 0;

	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (!conn_asked(c, content, nsegments))
			continue;
		if (c->busy && c->fetching != NULL)
			conn_park(c);
		if (!PQsendQueryParams(c->conn, sql, nparams, types, values, NULL, NULL, 0))
		{
			char	   *msg = pstrdup(PQerrorMessage(c->conn));
			int			failed = c->content;

			gang_close();
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send a statement to segment %d", failed),
					 errdetail_internal("%s", msg)));
		}
		c->busy = true;
	}

	/* The counts come back as command tags, which "keep" does not keep. */
	gang_wait_all_counting(g, counts, content, nsegments);
	pfree(results);
	(void) n;
}

/*
 * A statement with parameters, some of them binary, on one segment, waited
 * for.  What a Motion's batches of rows travel in: bytea sent as it is,
 * rather than as the hex text of it.
 */
void
GpDispatchParamsOnContent(int content, const char *sql, int nparams,
						  const char *const *values, const int *lengths,
						  const int *formats)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	if (c->busy && c->fetching != NULL)
		conn_park(c);
	if (!PQsendQueryParams(c->conn, sql, nparams, NULL, values, lengths,
						   formats, 0))
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a statement to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
	c->busy = true;
	gang_wait_all(g, NULL, false);
}

/*
 * A write with parameters, as text, on one segment, and what it said: how
 * many rows it changed and, into "store" when it is not NULL, the rows its
 * RETURNING gave, read by "tupdesc"'s input functions.
 */
uint64
GpDispatchWriteOnContent(int content, const char *sql, int nparams,
						 const char *const *values, TupleDesc tupdesc,
						 Tuplestorestate *store)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;
	PGresult  **results;
	PGresult   *res;
	uint64		count = 0;
	int			idx = -1;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
		{
			c = &g->conns[i];
			idx = i;
		}
	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	if (c->busy && c->fetching != NULL)
		conn_park(c);
	if (!PQsendQueryParams(c->conn, sql, nparams, NULL, values, NULL, NULL, 0))
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a statement to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
	c->busy = true;

	results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	gang_wait_all_keeping_commands(g, results);
	res = results[idx];

	if (res != NULL)
	{
		const char *tuples = PQcmdTuples(res);

		if (tuples[0] != '\0')
			count = strtou64(tuples, NULL, 10);

		if (store != NULL && PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			int			natts = tupdesc->natts;
			FmgrInfo   *in = palloc_array(FmgrInfo, natts);
			Oid		   *ioparams = palloc_array(Oid, natts);
			Datum	   *datums = palloc_array(Datum, natts);
			bool	   *nulls = palloc_array(bool, natts);

			if (PQnfields(res) != natts)
				elog(ERROR, "segment %d returned %d columns, not %d",
					 content, PQnfields(res), natts);
			for (int j = 0; j < natts; j++)
			{
				Oid			func;

				getTypeInputInfo(GpTransferType(TupleDescAttr(tupdesc, j)->atttypid), &func,
								 &ioparams[j]);
				fmgr_info(func, &in[j]);
			}
			for (int r = 0; r < PQntuples(res); r++)
			{
				for (int j = 0; j < natts; j++)
				{
					nulls[j] = PQgetisnull(res, r, j);
					datums[j] = InputFunctionCall(&in[j],
												  nulls[j] ? NULL : PQgetvalue(res, r, j),
												  ioparams[j],
												  TupleDescAttr(tupdesc, j)->atttypmod);
				}
				tuplestore_putvalues(store, tupdesc, datums, nulls);
			}
		}
	}

	for (int i = 0; i < g->nconns; i++)
		if (results[i] != NULL)
			PQclear(results[i]);
	pfree(results);
	return count;
}

/*
 * A relation's name in SQL a segment is sent.  A temporary relation is in
 * this session's temporary schema, whose name -- pg_temp_N -- is the
 * coordinator's backend's; the segment backend's own is another number, and
 * "pg_temp" names whichever is the session's own, on either.
 */
char *
GpDispatchRelationName(Oid relid)
{
	Oid			nsp = get_rel_namespace(relid);

	if (isAnyTempNamespace(nsp))
		return psprintf("pg_temp.%s", quote_identifier(get_rel_name(relid)));
	return quote_qualified_identifier(get_namespace_name(nsp),
									  get_rel_name(relid));
}

/* ------------------------------------------------------------------------- */
/* Rows on the way out                                                       */
/* ------------------------------------------------------------------------- */

/*
 * COPY ... FROM STDIN on one segment: the way rows the coordinator routed
 * reach it.  One segment at a time, which is what a connection in COPY mode
 * allows, and why the rows are held on the coordinator until the statement
 * that produced them has finished with the gang.
 */
void
GpCopyInBegin(int content, const char *sql)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;
	List	   *errors = NIL;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		elog(ERROR, "there is no segment with content id %d", content);

	conn_send(c, sql);

	for (;;)
	{
		PGresult   *res;

		if (PQconsumeInput(c->conn) == 0)
		{
			collect_error(&errors, c->content, NULL, c->conn, NULL);
			gang_close();
			raise_segment_errors(errors);
		}
		if (PQisBusy(c->conn))
		{
			gang_wait(g);
			continue;
		}
		res = PQgetResult(c->conn);
		if (res != NULL && PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			break;
		}

		/* The statement failed before it began to read. */
		collect_error(&errors, c->content, res, c->conn, NULL);
		if (res != NULL)
			PQclear(res);
		while ((res = PQgetResult(c->conn)) != NULL)
			PQclear(res);
		c->busy = false;
		raise_segment_errors(errors);
	}

	copying = c;
}

void
GpCopyInData(const char *data, int len)
{
	Assert(copying != NULL);

	if (PQputCopyData(copying->conn, data, len) != 1)
	{
		char	   *msg = pstrdup(PQerrorMessage(copying->conn));
		int			content = copying->content;

		copying = NULL;
		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send rows to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
}

uint64
GpCopyInEnd(void)
{
	GpSegmentConn *c = copying;
	uint64		count = 0;

	Assert(c != NULL);
	copying = NULL;

	if (PQputCopyEnd(c->conn, NULL) != 1)
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));
		int			content = c->content;

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not finish sending rows to segment %d", content),
				 errdetail_internal("%s", msg)));
	}

	/* The COPY's own result: its row count, or why it failed. */
	gang_wait_all_counting(gang, &count, c->content, 0);
	return count;
}

/* ------------------------------------------------------------------------- */
/* Rows on the way back                                                      */
/* ------------------------------------------------------------------------- */

/*
 * How a column of a segment's answer becomes a Datum.
 *
 * Binary for the whole result or text for the whole result, because a cursor
 * is one or the other: a type with no binary send function -- an extension's,
 * usually -- makes it text for its neighbours too.  Text costs a conversion
 * and loses nothing: PostgreSQL 19's float output is round-trip exact, which
 * is what made text safe to fall back to.
 */
typedef struct GpColumnIn
{
	FmgrInfo	proc;
	Oid			ioparam;
	int32		typmod;
} GpColumnIn;

/* One segment's side of a gather. */
typedef struct GpGatherSeg
{
	struct GpGatherState *gather;
	GpSegmentConn *conn;
	PGresult   *batch;			/* the rows being handed out */
	int			row;			/* the next of them */
	PGresult   *arrived;		/* a batch read but not yet handed out */
	bool		declared;		/* the cursor exists there */
	bool		done;			/* the cursor has nothing more */
} GpGatherSeg;

struct GpGatherState
{
	/*
	 * Where its batches live.  PostgreSQL 19 wraps every PGresult a backend
	 * receives in a palloc'd object that frees it when its context is reset
	 * (libpq-be-fe.h), and a scan reads rows from a per-tuple context that is
	 * reset between them: a batch received there was freed while rows were
	 * still being taken from it.
	 */
	MemoryContext cxt;
	GpGang	   *gang;
	TupleDesc	tupdesc;
	bool		binary;
	GpColumnIn *columns;
	int			nsegs;			/* the segments read from: all, or one */
	GpGatherSeg *segs;
	char	   *cursor;
	int			next;			/* which segment to look at first */
};

/*
 * Can a value of this type travel in binary?  Only if it has both halves, and
 * an array or a domain only if what it is made of has them too: array_send()
 * calls the element's send function, and fails at the first row if there is
 * none, which is too late to fall back to text.
 */
/*
 * The type a value of this type travels as between the nodes.  Most travel
 * as themselves.  A few refuse to be read back, on purpose, by either input
 * or receive -- a node tree, the extended statistics' values -- because
 * nothing may make one from outside; each is binary-coercible to text or
 * bytea (pg_cast), with the same bytes, so it travels as that and is kept
 * as it arrives.  pg_catalog's pg_class.relpartbound and pg_rewrite.ev_action
 * are among them, which gp.dist_random() of a catalog reads.
 */
Oid
GpTransferType(Oid type)
{
	switch (type)
	{
		case PG_NODE_TREEOID:
			return TEXTOID;
		case PG_NDISTINCTOID:
		case PG_DEPENDENCIESOID:
		case PG_MCV_LISTOID:
			return BYTEAOID;
		default:
			return type;
	}
}

/*
 * A column as a segment's query is to produce it: the cast to what it
 * travels as, where it needs one.
 */
void
GpAppendTransferColumn(StringInfo buf, const char *column, Oid type)
{
	Oid			transfer = GpTransferType(type);

	appendStringInfoString(buf, column);
	if (transfer != type)
		appendStringInfo(buf, "::pg_catalog.%s", transfer == TEXTOID ? "text" : "bytea");
}

/*
 * "SELECT" and every column of the relation, as "*" would give them, each
 * cast to what it travels as.
 */
char *
GpTransferSelectList(TupleDesc tupdesc)
{
	StringInfoData buf;
	bool		first = true;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "SELECT ");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (!first)
			appendStringInfoString(&buf, ", ");
		GpAppendTransferColumn(&buf, quote_identifier(NameStr(att->attname)),
							   att->atttypid);
		first = false;
	}
	if (first)
		appendStringInfoString(&buf, "*");
	return buf.data;
}

static bool
type_has_binary_io(Oid typid)
{
	HeapTuple	tp;
	Form_pg_type typ;
	bool		result;
	Oid			inner = InvalidOid;

	typid = GpTransferType(typid);
	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tp);

	result = OidIsValid(typ->typsend) && OidIsValid(typ->typreceive);
	if (OidIsValid(typ->typelem) && IsTrueArrayType(typ))
		inner = typ->typelem;
	else if (typ->typtype == TYPTYPE_DOMAIN)
		inner = typ->typbasetype;
	else if (typ->typtype == TYPTYPE_COMPOSITE && result)
	{
		/* record_send() calls each column's send function in turn */
		TupleDesc	td = lookup_rowtype_tupdesc(typid, -1);

		result = GpTupleDescHasBinaryIO(td);
		ReleaseTupleDesc(td);
	}
	ReleaseSysCache(tp);

	if (result && OidIsValid(inner))
		result = type_has_binary_io(inner);
	return result;
}

bool
GpTupleDescHasBinaryIO(TupleDesc tupdesc)
{
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (!type_has_binary_io(att->atttypid))
			return false;
	}
	return true;
}

static GpGatherState *gather_start(const char *sql, TupleDesc tupdesc,
									int content, int nsegments);

GpGatherState *
GpGatherStart(const char *sql, TupleDesc tupdesc)
{
	return gather_start(sql, tupdesc, -1, 0);
}

GpGatherState *
GpGatherStartOn(const char *sql, TupleDesc tupdesc, int content)
{
	return gather_start(sql, tupdesc, content, 0);
}

GpGatherState *
GpGatherStartOnSegments(const char *sql, TupleDesc tupdesc, int nsegments)
{
	return gather_start(sql, tupdesc, -1, nsegments);
}

static GpGatherState *
gather_start(const char *sql, TupleDesc tupdesc, int content, int nsegments)
{
	GpGatherState *gather = (GpGatherState *) palloc0(sizeof(GpGatherState));
	GpGang	   *g = gang_get();
	int			n = 0;

	/*
	 * Through a cursor, inside the coordinator's transaction.  A gather that
	 * is not read to the end -- a LIMIT above it -- closes the cursor, where
	 * reading a plain query to the end would cost the rest of the table and
	 * cancelling it would abort the segment's transaction.  Each next batch
	 * is asked for as soon as the one before has arrived, so a segment is
	 * producing rows while the coordinator hands out the ones it already has.
	 */
	gang_prepare(g, true);

	gather->cxt = CurrentMemoryContext;
	gather->gang = g;
	gather->tupdesc = tupdesc;
	gather->binary = GpTupleDescHasBinaryIO(tupdesc);
	gather->columns = (GpColumnIn *) palloc0_array(GpColumnIn, tupdesc->natts);
	gather->segs = (GpGatherSeg *) palloc0_array(GpGatherSeg, g->nconns);
	gather->cursor = psprintf("gp_gather_%u", ++gather_counter);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			proc;
		Oid			ioparam;

		if (att->attisdropped)
			continue;
		if (gather->binary)
			getTypeBinaryInputInfo(GpTransferType(att->atttypid), &proc, &ioparam);
		else
			getTypeInputInfo(GpTransferType(att->atttypid), &proc, &ioparam);
		fmgr_info(proc, &gather->columns[i].proc);
		gather->columns[i].ioparam = ioparam;
		gather->columns[i].typmod = att->atttypmod;
	}

	for (int i = 0; i < g->nconns; i++)
	{
		GpGatherSeg *s;

		if (!conn_asked(&g->conns[i], content, nsegments))
			continue;

		s = &gather->segs[n++];
		s->gather = gather;
		s->conn = &g->conns[i];
		conn_send(s->conn,
				  psprintf("DECLARE %s %sNO SCROLL CURSOR FOR %s; FETCH %d FROM %s",
						   gather->cursor, gather->binary ? "BINARY " : "",
						   sql, GATHER_FETCH_ROWS, gather->cursor));
		s->conn->fetching = s;
		s->declared = true;
	}
	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));
	gather->nsegs = n;

	return gather;
}

/* One row of a segment's answer, into the slot. */
static void
gather_store_row(GpGatherState *gather, PGresult *res, int row,
				 TupleTableSlot *slot)
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
		if (PQgetisnull(res, row, i) || TupleDescAttr(tupdesc, i)->attisdropped)
		{
			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			continue;
		}

		slot->tts_isnull[i] = false;

		if (gather->binary)
		{
			StringInfoData buf;

			initReadOnlyStringInfo(&buf, PQgetvalue(res, row, i),
								   PQgetlength(res, row, i));
			slot->tts_values[i] = ReceiveFunctionCall(&gather->columns[i].proc,
													  &buf,
													  gather->columns[i].ioparam,
													  gather->columns[i].typmod);
		}
		else
			slot->tts_values[i] = InputFunctionCall(&gather->columns[i].proc,
												   PQgetvalue(res, row, i),
												   gather->columns[i].ioparam,
												   gather->columns[i].typmod);
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * Read whatever has arrived of a gather segment's batch, without waiting.
 * When the whole answer is in, the batch becomes the segment's "arrived" one.
 * Returns whether anything was read.
 */
static bool
gather_poll(GpGatherSeg *s)
{
	GpSegmentConn *c = s->conn;
	bool		progress = false;

	if (!c->busy || c->fetching != s)
		return false;

	if (PQconsumeInput(c->conn) == 0)
	{
		List	   *errors = NIL;

		collect_error(&errors, c->content, NULL, c->conn, NULL);
		gang_close();
		raise_segment_errors(errors);
	}

	while (!PQisBusy(c->conn))
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(s->gather->cxt);
		PGresult   *res = PQgetResult(c->conn);
		ExecStatusType status;

		MemoryContextSwitchTo(oldcxt);
		progress = true;

		if (res == NULL)
		{
			c->busy = false;
			c->fetching = NULL;
			if (s->arrived == NULL || PQntuples(s->arrived) < GATHER_FETCH_ROWS)
				s->done = true;
			break;
		}

		status = PQresultStatus(res);
		if (status == PGRES_TUPLES_OK)
		{
			Assert(s->arrived == NULL);
			s->arrived = res;
			continue;
		}
		if (status == PGRES_COMMAND_OK)
		{
			PQclear(res);		/* the DECLARE */
			continue;
		}

		{
			List	   *errors = NIL;

			collect_error(&errors, c->content, res, c->conn, NULL);
			PQclear(res);
			if (PQstatus(c->conn) == CONNECTION_BAD)
				gang_close();
			raise_segment_errors(errors);
		}
	}

	if (progress)
		flush_segment_notices();
	return progress;
}

/*
 * Set a gather's batch aside so that the connection can be used for something
 * else: read it to the end, into the gather segment it was asked for.
 */
static void
conn_park(GpSegmentConn *c)
{
	GpGatherSeg *s = c->fetching;

	while (c->busy && c->fetching == s)
	{
		if (!gather_poll(s))
			gang_wait(gang);
	}
}

/* Ask for a segment's next batch. */
static void
gather_fetch(GpGatherSeg *s)
{
	conn_send(s->conn, psprintf("FETCH %d FROM %s", GATHER_FETCH_ROWS,
								s->gather->cursor));
	s->conn->fetching = s;
}

/*
 * The next row from any segment: the segment's side of the gather, whose
 * batch holds it at *row; NULL when every segment has finished.
 */
static GpGatherSeg *
gather_next_row(GpGatherState *gather, int *row)
{
	if (gather->gang != gang)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost the connections to the segments while reading from them")));

	for (;;)
	{
		bool		unfinished = false;
		bool		progress = false;

		for (int n = 0; n < gather->nsegs; n++)
		{
			int			i = (gather->next + n) % gather->nsegs;
			GpGatherSeg *s = &gather->segs[i];

			if (s->batch != NULL && s->row < PQntuples(s->batch))
			{
				*row = s->row++;
				/* The next row from the next segment: they take turns. */
				gather->next = (i + 1) % gather->nsegs;
				return s;
			}

			if (s->batch != NULL)
			{
				PQclear(s->batch);
				s->batch = NULL;
			}

			if (gather_poll(s))
				progress = true;

			if (s->arrived != NULL)
			{
				s->batch = s->arrived;
				s->arrived = NULL;
				s->row = 0;
				progress = true;

				/*
				 * Ask for the next batch while this one is handed out, unless
				 * this was the last, or the connection is busy with somebody
				 * else's statement.
				 */
				if (!s->done && !s->conn->busy)
					gather_fetch(s);

				n--;			/* look at this segment again */
				continue;
			}

			if (s->done)
				continue;

			unfinished = true;

			/* Nothing in hand and nothing asked for: ask. */
			if (s->conn->fetching != s)
			{
				gather_fetch(s);
				progress = true;
			}
		}

		if (!unfinished)
			return NULL;
		if (!progress)
			gang_wait(gather->gang);
	}
}

bool
GpGatherNext(GpGatherState *gather, TupleTableSlot *slot, int *content)
{
	int			row;
	GpGatherSeg *s = gather_next_row(gather, &row);

	if (s == NULL)
		return false;
	gather_store_row(gather, s->batch, row, slot);
	if (content != NULL)
		*content = s->conn->content;
	return true;
}

bool
GpGatherNextRaw(GpGatherState *gather, const char **values, int *lengths)
{
	int			row;
	GpGatherSeg *s = gather_next_row(gather, &row);
	int			natts = gather->tupdesc->natts;

	if (s == NULL)
		return false;
	if (PQnfields(s->batch) != natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a segment answered with %d columns, not %d",
						PQnfields(s->batch), natts)));

	for (int i = 0; i < natts; i++)
	{
		if (PQgetisnull(s->batch, row, i))
		{
			values[i] = NULL;
			lengths[i] = -1;
		}
		else
		{
			values[i] = PQgetvalue(s->batch, row, i);
			lengths[i] = PQgetlength(s->batch, row, i);
		}
	}
	return true;
}

bool
GpGatherIsBinary(GpGatherState *gather)
{
	return gather->binary;
}

Datum
GpGatherDecodeValue(GpGatherState *gather, int col, const char *value,
					int length)
{
	GpColumnIn *in = &gather->columns[col];

	if (gather->binary)
	{
		StringInfoData buf;

		initReadOnlyStringInfo(&buf, (char *) value, length);
		return ReceiveFunctionCall(&in->proc, &buf, in->ioparam, in->typmod);
	}
	return InputFunctionCall(&in->proc, (char *) value, in->ioparam,
							 in->typmod);
}

int
GpGatherSegmentCount(GpGatherState *gather)
{
	return gather->nsegs;
}

/*
 * The next row from one of the gather's segments, waiting for it if it has
 * not arrived; false when that segment has no more.  What a merge needs: it
 * takes the least row of the segments' next ones, so it has to be able to ask
 * for a particular segment's.
 */
bool
GpGatherNextFrom(GpGatherState *gather, int seg, TupleTableSlot *slot)
{
	GpGatherSeg *s;

	if (gather->gang != gang)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost the connections to the segments while reading from them")));
	Assert(seg >= 0 && seg < gather->nsegs);
	s = &gather->segs[seg];

	for (;;)
	{
		if (s->batch != NULL && s->row < PQntuples(s->batch))
		{
			gather_store_row(gather, s->batch, s->row++, slot);
			return true;
		}

		if (s->batch != NULL)
		{
			PQclear(s->batch);
			s->batch = NULL;
		}

		(void) gather_poll(s);

		if (s->arrived != NULL)
		{
			s->batch = s->arrived;
			s->arrived = NULL;
			s->row = 0;
			if (!s->done && !s->conn->busy)
				gather_fetch(s);
			continue;
		}

		if (s->done)
			return false;

		if (s->conn->fetching != s)
		{
			gather_fetch(s);
			continue;
		}

		gang_wait(gather->gang);
	}
}

void
GpGatherEnd(GpGatherState *gather)
{
	GpGang	   *g = gather->gang;

	if (gang != g)
		return;					/* the gang was lost, and its cursors with it */

	/*
	 * Read what is still on its way, which is at most a batch, and close the
	 * cursors: the segments' transaction goes on, and may gather again.
	 */
	for (int i = 0; i < gather->nsegs; i++)
	{
		GpGatherSeg *s = &gather->segs[i];

		if (s->conn->fetching == s)
			conn_park(s->conn);
		if (s->arrived != NULL)
			PQclear(s->arrived);
		if (s->batch != NULL)
			PQclear(s->batch);
		s->arrived = s->batch = NULL;
	}

	for (int i = 0; i < gather->nsegs; i++)
		conn_send(gather->segs[i].conn, psprintf("CLOSE %s", gather->cursor));
	gang_wait_all(g, NULL, false);
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Run a query on every segment (content -1) or one, and answer the first
 * column of each one's first row, as text, NULL where there was none; one
 * entry per segment asked, in content order.
 */
void
GpDispatchQueryFirstValues(const char *sql, int content, char **values)
{
	GpGang	   *g = gang_get();
	PGresult  **results;
	int			n = 0;

	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	for (int i = 0; i < g->nconns; i++)
		if (content < 0 || g->conns[i].content == content)
			conn_send(&g->conns[i], sql);
	gang_wait_all(g, results, false);

	for (int i = 0; i < g->nconns; i++)
	{
		if (content >= 0 && g->conns[i].content != content)
			continue;
		values[n++] = (results[i] != NULL && PQntuples(results[i]) > 0 &&
					   PQnfields(results[i]) > 0 && !PQgetisnull(results[i], 0, 0))
			? pstrdup(PQgetvalue(results[i], 0, 0)) : NULL;
		if (results[i] != NULL)
			PQclear(results[i]);
	}
}

PG_FUNCTION_INFO_V1(gp_exec_on_segments);

/*
 * gp.exec_on_segments(sql)
 *		Run a statement on every segment, and report what each one said.
 *
 * The answer is the first column of the first row, as text, because this is
 * for asking a cluster about itself -- "what does each segment think it is",
 * "how many rows does each one hold" -- and not for reading a table, which is
 * what the scan of a distributed table does.  It runs in the coordinator's
 * transaction, like everything else sent to the segments.
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
	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	gang_send_all(g, sql);
	gang_wait_all(g, results, false);

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
 * qual pushed down, no column left out.  Where there is nothing to dispatch
 * to, it reads the relation here, as Cloudberry's does on a single node.
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

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	if (GpDistRandomIsLocal())
	{
		GpDistRandomLocal(relid, rsinfo->setResult, rsinfo->setDesc, false);
		table_close(rel, AccessShareLock);
		return (Datum) 0;
	}

	initStringInfo(&sql);
	appendStringInfo(&sql, "%s FROM %s", GpTransferSelectList(tupdesc),
					 GpDispatchRelationName(RelationGetRelid(rel)));

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	gather = GpGatherStart(sql.data, tupdesc);
	while (GpGatherNext(gather, slot, NULL))
		tuplestore_puttupleslot(rsinfo->setResult, slot);
	GpGatherEnd(gather);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * Is there nothing for gp.dist_random() to dispatch to: one node, or a
 * session that is not the coordinator's dispatching one?
 */
bool
GpDistRandomIsLocal(void)
{
	return GpClusterIsSingleNode() ||
		GpClusterBackendRole() != GP_ROLE_DISPATCH;
}

/*
 * gp.dist_random() where there is nothing to dispatch to: the relation's
 * rows here, inheritance children included as a gather's are, into `store`
 * as `desc` says -- the relation's row type, or with gp_segment_id after it,
 * which is this node's content id.
 */
void
GpDistRandomLocal(Oid relid, Tuplestorestate *store, TupleDesc desc,
				  bool with_content)
{
	Oid			typid = get_rel_type_id(relid);
	TupleDesc	rowdesc = lookup_rowtype_tupdesc_copy(typid, -1);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	MemoryContext outer = CurrentMemoryContext;
	char	   *sql;

	if (desc->natts != rowdesc->natts + (with_content ? 1 : 0))
		elog(ERROR, "gp.dist_random() called with %d columns for a relation of %d",
			 desc->natts, rowdesc->natts);

	sql = psprintf("SELECT r FROM %s r", GpDispatchRelationName(relid));

	SPI_connect();
	if (SPI_execute(sql, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read relation %u", relid);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		bool		isnull;
		Datum		row = SPI_getbinval(SPI_tuptable->vals[r],
										SPI_tuptable->tupdesc, 1, &isnull);
		HeapTupleHeader hdr = DatumGetHeapTupleHeader(row);
		HeapTupleData tuple;
		MemoryContext old;

		tuple.t_len = HeapTupleHeaderGetDatumLength(hdr);
		ItemPointerSetInvalid(&tuple.t_self);
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = hdr;
		heap_deform_tuple(&tuple, rowdesc, values, nulls);
		if (with_content)
		{
			values[desc->natts - 1] = Int32GetDatum(GpClusterContentId());
			nulls[desc->natts - 1] = false;
		}

		old = MemoryContextSwitchTo(outer);
		tuplestore_putvalues(store, desc, values, nulls);
		MemoryContextSwitchTo(old);
	}
	SPI_finish();
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

const char *
GpDispatchPassfile(void)
{
	return gp_internal_passfile;
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
	RegisterSubXactCallback(dispatch_subxact_callback, NULL);
}
