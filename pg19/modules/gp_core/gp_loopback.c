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
 * gp_loopback.c
 *	  Another database of this server, reached from any database, its work
 *	  committed with the transaction that asked for it.
 *
 * An extension cannot make a shared catalog, and some of what Cloudberry
 * keeps in one can be no label: task jobs, whose run history grows at every
 * run, and storage servers, whose user mappings hold credentials.  They live
 * in one database -- gp.task_database, gp.maintenance_database -- and a
 * backend in any other reaches it as the dispatcher reaches a segment: a
 * libpq connection, to this same server, as the session's user.
 *
 * WRITES are deferred.  A module asks for a statement to be run there, and
 * it is run as this transaction commits, at PRE_COMMIT, in the order asked,
 * in one transaction there -- so a statement, subtransaction or transaction
 * that rolls back never writes.  That transaction is then a part of this
 * one: it is prepared under this server's transaction ID, gp_dtx_<xid>_<database
 * OID>, as a segment's part is under gp_dtx_<xid>, committed after the commit
 * record, rolled back if the transaction is, and finished by the DTX recovery
 * process if the server fails between the two (gp_dtx.c) -- on a cluster's
 * coordinator, where it joins the distributed transaction, and on one node
 * alike.  For that its connection carries a dispatched backend's identity
 * and the cluster secret, without which PREPARE under such a gid is refused.
 *
 * A server that cannot prepare -- max_prepared_transactions at zero, which is
 * PostgreSQL's default -- runs the part's statements at PRE_COMMIT all the
 * same, raising their errors there, and leaves its transaction open: it is
 * committed once this transaction's commit record is written, or rolled back
 * with this transaction.  So a failure after PRE_COMMIT -- a SERIALIZABLE
 * transaction's serialization check, an ON COMMIT action, NOTIFY's queue,
 * which PostgreSQL runs after the callbacks -- rolls back both.  What is left
 * is a crash, or a lost connection, between the commit record and the part's
 * COMMIT: the part is then lost, which only a prepared part survives.
 *
 * READS are run at once, in a read-only transaction of their own, as the
 * session's current user, and see what is committed there: what this
 * transaction has deferred is not, yet.
 *
 * Only statements a module builds from values it quoted go over it, never a
 * caller's SQL, because the connection may carry the cluster secret, and a
 * backend that holds it is trusted with plans.  The wait for the other
 * database is one PostgreSQL's deadlock detector cannot see; what is written
 * there is written at pre-commit, holding nothing the writer waits for, and
 * the global deadlock detector counts the loopback's backend as a part of the
 * session that opened it, by the identity it carries.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_dtx.h"
#include "gp_fault.h"
#include "gp_loopback.h"

static char *gp_maintenance_database = NULL;

/* A statement to be run in another database as this transaction commits. */
typedef struct LoopbackWrite
{
	char	   *dbname;
	char	   *sql;
	int			level;			/* the (sub)transaction that asked for it */
} LoopbackWrite;

/* A connection of the session's, kept for the next time. */
typedef struct LoopbackConn
{
	int			dbid;			/* the node's; 0: this server */
	char		dbname[NAMEDATALEN];
	char		user[NAMEDATALEN];
	bool		trusted;		/* a dispatched backend's identity and the secret */
	PGconn	   *conn;
} LoopbackConn;

/*
 * This transaction's part in another database, prepared at pre-commit -- or,
 * where the server cannot prepare, left open there, its gid empty.
 */
typedef struct LoopbackPart
{
	LoopbackConn *lc;
	char		gid[GP_DTX_GIDLEN];
} LoopbackPart;

static List *writes = NIL;		/* this transaction's; TopTransactionContext */
static List *conns = NIL;		/* the session's; TopMemoryContext */
static List *parts = NIL;		/* this transaction's; TopMemoryContext */
static bool exit_registered = false;

static uint32
loopback_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryLoopback");
	return event;
}

const char *
GpLoopbackMaintenanceDatabase(void)
{
	return gp_maintenance_database;
}

bool
GpLoopbackHasWrites(void)
{
	return writes != NIL;
}

bool
GpLoopbackIsHere(const char *dbname)
{
	return OidIsValid(MyDatabaseId) &&
		get_database_oid(dbname, true) == MyDatabaseId;
}

/*
 * Two-phase wherever this server may prepare: a cluster's coordinator, or one
 * node, each with a recovery process that finishes a part it did not.  A
 * segment writes nothing here.
 */
static bool
loopback_two_phase(void)
{
	return GpClusterContentId() < 0 && max_prepared_xacts > 0;
}

/* ------------------------------------------------------------------------- */
/* Connections                                                               */
/* ------------------------------------------------------------------------- */

static void
loopback_atexit(int code, Datum arg)
{
	foreach_ptr(LoopbackConn, lc, conns)
	{
		if (lc->conn != NULL)
			libpqsrv_disconnect(lc->conn);
	}
	conns = NIL;
}

static void
loopback_forget(LoopbackConn *lc)
{
	if (lc->conn != NULL)
		libpqsrv_disconnect(lc->conn);
	conns = list_delete_ptr(conns, lc);
	pfree(lc);
}

/*
 * The session's connection to that database, opened if there is none: to
 * this server -- as the segments reach it on a cluster, and otherwise by its
 * first Unix socket, or TCP on this host -- or to another node of the
 * cluster, "node".  A trusted one says it is a dispatched backend of this
 * session, as the dispatcher's do.
 */
static LoopbackConn *
loopback_conn(const GpSegmentConfig *node, const char *dbname, bool trusted)
{
	const char *user = GetUserNameFromId(GetSessionUserId(), false);
	const GpSegmentConfig *self = node != NULL ? node : GpClusterSelf();
	int			dbid = node != NULL ? node->dbid : 0;
	const char *keywords[7 + GP_INTERNAL_CONN_OPTIONS];
	const char *values[7 + GP_INTERNAL_CONN_OPTIONS];
	const char *host = "localhost";
	char		portbuf[16];
	char	   *options = NULL;
	int			n = 0;
	LoopbackConn *lc;
	PGconn	   *conn;

	foreach_ptr(LoopbackConn, c, conns)
	{
		if (c->dbid != dbid || c->trusted != trusted ||
			strcmp(c->dbname, dbname) != 0 || strcmp(c->user, user) != 0)
			continue;
		if (PQstatus(c->conn) == CONNECTION_OK &&
			PQtransactionStatus(c->conn) == PQTRANS_IDLE)
			return c;
		loopback_forget(c);		/* gone, or left mid-transaction */
		break;
	}

	if (self != NULL)
	{
		host = self->hostname;
		snprintf(portbuf, sizeof(portbuf), "%d", self->port);
	}
	else
	{
		char	   *dirs = pstrdup(Unix_socket_directories ? Unix_socket_directories : "");
		List	   *list;

		if (SplitDirectoriesString(dirs, ',', &list) && list != NIL)
			host = (char *) linitial(list);
		snprintf(portbuf, sizeof(portbuf), "%d", PostPortNumber);
	}
	if (trusted)
	{
		options = psprintf("-c gp.qe_identity=seg-1/dbid%d/sess%d",
						   GpClusterDbid(), MyProcPid);
		if (GpClusterHasSecret())
			options = psprintf("%s -c gp.qe_secret=%s", options, GpClusterSecret());
	}

	keywords[n] = "host";
	values[n++] = host;
	keywords[n] = "port";
	values[n++] = portbuf;
	keywords[n] = "dbname";
	values[n++] = dbname;
	keywords[n] = "user";
	values[n++] = user;
	keywords[n] = "application_name";
	values[n++] = "cloudberry loopback";
	keywords[n] = "client_encoding";
	values[n++] = GetDatabaseEncodingName();
	if (options != NULL)
	{
		keywords[n] = "options";
		values[n++] = options;
	}
	n = GpInternalConnOptions(keywords, values, n);

	conn = libpqsrv_connect_params(keywords, values, false, loopback_wait_event());
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		char	   *msg = conn ? pstrdup(PQerrorMessage(conn)) : "out of memory";

		if (conn != NULL)
			libpqsrv_disconnect(conn);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to database \"%s\" of this server", dbname),
				 errdetail_internal("%s", msg)));
	}
	GpDispatchRelayNotices(conn);

	if (!exit_registered)
	{
		before_shmem_exit(loopback_atexit, 0);
		exit_registered = true;
	}
	lc = MemoryContextAllocZero(TopMemoryContext, sizeof(LoopbackConn));
	lc->dbid = dbid;
	strlcpy(lc->dbname, dbname, NAMEDATALEN);
	strlcpy(lc->user, user, NAMEDATALEN);
	lc->trusted = trusted;
	lc->conn = conn;
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		conns = lappend(conns, lc);
		MemoryContextSwitchTo(oldcxt);
	}
	return lc;
}

/*
 * Run one statement there.  Its error is raised here as its own -- the
 * message, SQLSTATE and the rest the other backend gave -- and the
 * connection is closed, which rolls back whatever it had begun.
 */
static PGresult *
loopback_exec(LoopbackConn *lc, const char *sql, const char *context)
{
	PGresult   *res = libpqsrv_exec(lc->conn, sql, loopback_wait_event());
	ExecStatusType status = res != NULL ? PQresultStatus(res) : PGRES_FATAL_ERROR;
	const char *fields[4] = {NULL, NULL, NULL, NULL};
	char	   *copies[4];
	const char *sqlstate;
	int			code = ERRCODE_CONNECTION_FAILURE;

	GpDispatchFlushNotices();
	if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
		return res;

	if (res != NULL)
	{
		fields[0] = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
		fields[1] = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
		fields[2] = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
		fields[3] = PQresultErrorField(res, PG_DIAG_CONTEXT);
		sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		if (sqlstate != NULL && strlen(sqlstate) == 5)
			code = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
								 sqlstate[3], sqlstate[4]);
	}
	if (fields[0] == NULL)
		fields[0] = pchomp(PQerrorMessage(lc->conn));
	for (int i = 0; i < 4; i++)
		copies[i] = fields[i] != NULL ? pstrdup(fields[i]) : NULL;
	if (res != NULL)
		PQclear(res);
	loopback_forget(lc);

	ereport(ERROR,
			(errcode(code),
			 errmsg_internal("%s", copies[0]),
			 copies[1] ? errdetail_internal("%s", copies[1]) : 0,
			 copies[2] ? errhint("%s", copies[2]) : 0,
			 copies[3] ? errcontext("%s", copies[3]) : 0,
			 errcontext("%s", context)));
	return NULL;				/* keep the compiler quiet */
}

/* The current user, where a SECURITY DEFINER function or SET ROLE made one. */
static void
loopback_set_role(LoopbackConn *lc, const char *context)
{
	if (GetUserId() != GetSessionUserId())
		PQclear(loopback_exec(lc,
							  psprintf("SET LOCAL ROLE %s",
									   quote_identifier(GetUserNameFromId(GetUserId(), false))),
							  context));
}

/* ------------------------------------------------------------------------- */
/* Writes                                                                    */
/* ------------------------------------------------------------------------- */

void
GpLoopbackDefer(const char *dbname, const char *sql)
{
	MemoryContext oldcxt;
	LoopbackWrite *w;

	if (GpClusterBackendRole() == GP_ROLE_EXECUTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a segment does not write to database \"%s\"", dbname),
				 errdetail("What is kept there is the coordinator's to write.")));
	PreventCommandIfReadOnly(psprintf("a write to database \"%s\"", dbname));
	if (!OidIsValid(get_database_oid(dbname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	w = palloc(sizeof(LoopbackWrite));
	w->dbname = pstrdup(dbname);
	w->sql = pstrdup(sql);
	w->level = GetCurrentTransactionNestLevel();
	writes = lappend(writes, w);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * As this transaction commits: each database's statements, in one
 * transaction there, prepared, or left open where this server cannot
 * prepare.  Raising here still rolls this one back, and at ABORT what was
 * prepared or left open with it.
 */
static void
loopback_pre_commit(void)
{
	List	   *dbs = NIL;
	bool		two_phase;
	FullTransactionId gxid = InvalidFullTransactionId;

	if (writes == NIL)
		return;

	foreach_ptr(LoopbackWrite, w, writes)
	{
		bool		seen = false;

		foreach_ptr(char, db, dbs)
			seen |= strcmp(db, w->dbname) == 0;
		if (!seen)
			dbs = lappend(dbs, w->dbname);
	}

	two_phase = loopback_two_phase();
	if (two_phase)
		gxid = GetTopFullTransactionId();

	foreach_ptr(char, db, dbs)
	{
		char	   *context = psprintf("run in database \"%s\" as the transaction commits", db);
		LoopbackConn *lc = loopback_conn(NULL, db, two_phase);

		PQclear(loopback_exec(lc, "BEGIN", context));
		loopback_set_role(lc, context);
		foreach_ptr(LoopbackWrite, w, writes)
		{
			if (strcmp(w->dbname, db) == 0)
				PQclear(loopback_exec(lc, w->sql, context));
		}

		{
			char		gid[GP_DTX_GIDLEN];
			LoopbackPart *part;
			MemoryContext oldcxt;

			gid[0] = '\0';
			if (two_phase)
			{
				GpDtxFormLoopbackGid(gxid, get_database_oid(db, false), gid);
				PQclear(loopback_exec(lc, psprintf("PREPARE TRANSACTION '%s'", gid),
									  context));
			}
			oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			part = palloc0(sizeof(LoopbackPart));
			part->lc = lc;
			strlcpy(part->gid, gid, GP_DTX_GIDLEN);
			parts = lappend(parts, part);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	/*
	 * The commit record decides a prepared part, and comes before an open
	 * one's COMMIT: on disk before either is told, so that a crash never
	 * finds the part committed and this transaction not.
	 */
	if (parts != NIL)
		ForceSyncCommit();
}

/*
 * COMMIT or ROLLBACK PREPARED of one part, or COMMIT or ROLLBACK of one left
 * open, without raising: this runs after the commit record, or as the
 * transaction aborts, with interrupts held, so it waits no longer than the
 * dispatcher's second phase does.  A prepared part the recovery process
 * finished first, or is finishing, is done.
 */
static bool
loopback_finish(LoopbackPart *part, bool commit)
{
	PGconn	   *conn = part->lc->conn;
	char		sql[GP_DTX_GIDLEN + 32];
	TimestampTz deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	bool		prepared = part->gid[0] != '\0';
	bool		ok = true;

	if (prepared)
		snprintf(sql, sizeof(sql), "%s PREPARED '%s'",
				 commit ? "COMMIT" : "ROLLBACK", part->gid);
	else
		snprintf(sql, sizeof(sql), "%s", commit ? "COMMIT" : "ROLLBACK");
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK || !PQsendQuery(conn, sql))
		return false;

	for (;;)
	{
		long		timeout;

		if (PQconsumeInput(conn) == 0)
			return false;
		while (!PQisBusy(conn))
		{
			PGresult   *res = PQgetResult(conn);
			const char *state;

			if (res == NULL)
				return ok;
			state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
			if (PQresultStatus(res) != PGRES_COMMAND_OK &&
				(!prepared || state == NULL ||
				 (strcmp(state, "42704") != 0 && strcmp(state, "55000") != 0)))
			{
				ok = false;
				ereport(LOG,
						(errmsg("%s in database \"%s\" failed: %s", sql,
								part->lc->dbname, PQresultErrorMessage(res))));
			}
			/* an open part's COMMIT answers ROLLBACK when it had failed */
			else if (!prepared && commit &&
					 strcmp(PQcmdStatus(res), "ROLLBACK") == 0)
				ok = false;
			PQclear(res);
		}
		timeout = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline);
		if (timeout <= 0)
			return false;
		(void) WaitLatchOrSocket(MyLatch,
								 WL_LATCH_SET | WL_SOCKET_READABLE | WL_TIMEOUT |
								 WL_EXIT_ON_PM_DEATH,
								 PQsocket(conn), timeout, loopback_wait_event());
		ResetLatch(MyLatch);
	}
}

static void
loopback_second_phase(bool commit)
{
	foreach_ptr(LoopbackPart, part, parts)
	{
		bool		done = false;

		PG_TRY();
		{
			if (commit)
				GP_FAULT("loopback_commit_prepared");
			done = loopback_finish(part, commit);
		}
		PG_CATCH();
		{
			FlushErrorState();
		}
		PG_END_TRY();

		if (!done)
		{
			/* its state is not known: nothing more is sent on it */
			if (part->lc->conn != NULL)
			{
				libpqsrv_disconnect(part->lc->conn);
				part->lc->conn = NULL;
			}
			if (part->gid[0] != '\0')
			{
				ereport(WARNING,
						(errmsg("the part of this transaction in database \"%s\" was not %s yet",
								part->lc->dbname, commit ? "committed" : "rolled back"),
						 errdetail("Distributed transaction recovery finishes \"%s\".",
								   part->gid)));
				GpDtxWakeRecovery();
			}

			/*
			 * An open part whose connection is gone is rolled back by its
			 * backend's exit; one whose COMMIT went unanswered may be
			 * either, and nothing will finish it.
			 */
			else if (commit)
				ereport(WARNING,
						(errmsg("the part of this transaction in database \"%s\" may not have been committed",
								part->lc->dbname),
						 errdetail("Its COMMIT was not answered, and what this transaction asked for there may not be written."),
						 errhint("With \"max_prepared_transactions\" above zero such a part is prepared, and finished after a failure.")));
		}
	}
	list_free_deep(parts);
	parts = NIL;
}

/*
 * O33: the loopback's second phase, as the segments' (gp_dispatch.c): once
 * the commit is recorded, and before the transaction ends for the other
 * sessions, which see it in progress until its part in the other database
 * is committed too.
 */
static xact_commit_recorded_hook_type prev_commit_recorded_hook = NULL;

static void
loopback_commit_recorded(TransactionId latestXid)
{
	if (prev_commit_recorded_hook)
		prev_commit_recorded_hook(latestXid);
	if (parts != NIL)
		loopback_second_phase(true);
}

static void
loopback_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			loopback_pre_commit();
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (writes != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that writes to another database"),
						 errdetail("What it writes there is prepared as it commits, which a prepared transaction does not.")));
			break;
		case XACT_EVENT_COMMIT:
			writes = NIL;
			/* the second phase is done already (loopback_commit_recorded()) */
			Assert(parts == NIL);
			break;
		case XACT_EVENT_ABORT:
			writes = NIL;
			if (parts != NIL)
				loopback_second_phase(false);

			/*
			 * A connection left in a transaction there, by an error on
			 * another, is closed, which rolls it back.
			 */
			foreach_ptr(LoopbackConn, lc, conns)
			{
				if (lc->conn == NULL || PQstatus(lc->conn) != CONNECTION_OK ||
					PQtransactionStatus(lc->conn) != PQTRANS_IDLE)
				{
					if (lc->conn != NULL)
						libpqsrv_disconnect(lc->conn);
					lc->conn = NULL;
				}
			}
			break;
		case XACT_EVENT_PREPARE:
			writes = NIL;
			break;
		default:
			break;
	}
}

/* What a subtransaction that rolls back asked for is never written. */
static void
loopback_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	ListCell   *lc;

	if (event != SUBXACT_EVENT_ABORT_SUB && event != SUBXACT_EVENT_COMMIT_SUB)
		return;
	foreach(lc, writes)
	{
		LoopbackWrite *w = (LoopbackWrite *) lfirst(lc);

		if (w->level < level)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			writes = foreach_delete_current(writes, lc);
		else
			w->level = level - 1;
	}
}

/* ------------------------------------------------------------------------- */
/* Reads                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A read: the query's rows, each an array of ncols strings, NULL for a null,
 * in the caller's memory.  From this server's database "dbname" -- by SPI
 * when that is this backend's own -- or from the coordinator's, on a segment,
 * which keeps none of what that database holds.
 */
static List *
loopback_read(const char *dbname, const char *sql, int ncols)
{
	const GpSegmentConfig *node = NULL;
	MemoryContext cxt = CurrentMemoryContext;
	List	   *rows = NIL;
	char	   *context;
	LoopbackConn *lc;
	PGresult   *res;

	if (GpClusterContentId() >= 0 && !GpClusterIsSingleNode())
	{
		node = GpClusterCoordinator();
		if (node == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("this segment's cluster has no coordinator to read database \"%s\" of",
							dbname)));
	}

	if (node == NULL && GpLoopbackIsHere(dbname))
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql, true, 0) != SPI_OK_SELECT)
			elog(ERROR, "gp_core: %s failed", sql);
		if (SPI_tuptable->tupdesc->natts != ncols)
			elog(ERROR, "gp_core: %s gave %d columns, where %d were wanted",
				 sql, SPI_tuptable->tupdesc->natts, ncols);
		for (uint64 r = 0; r < SPI_processed; r++)
		{
			MemoryContext spicxt = MemoryContextSwitchTo(cxt);
			char	  **row = palloc_array(char *, ncols);

			for (int i = 0; i < ncols; i++)
			{
				char	   *text = SPI_getvalue(SPI_tuptable->vals[r],
												SPI_tuptable->tupdesc, i + 1);

				row[i] = text != NULL ? pstrdup(text) : NULL;
			}
			rows = lappend(rows, row);
			MemoryContextSwitchTo(spicxt);
		}
		SPI_finish();
		return rows;
	}

	context = psprintf("read from database \"%s\"%s", dbname,
					   node != NULL ? " of the coordinator" : "");
	lc = loopback_conn(node, dbname, false);
	PQclear(loopback_exec(lc, "BEGIN READ ONLY", context));
	loopback_set_role(lc, context);
	res = loopback_exec(lc, sql, context);
	if (PQnfields(res) != ncols)
		elog(ERROR, "gp_core: %s gave %d columns, where %d were wanted",
			 sql, PQnfields(res), ncols);
	for (int r = 0; r < PQntuples(res); r++)
	{
		char	  **row = palloc_array(char *, ncols);

		for (int i = 0; i < ncols; i++)
			row[i] = PQgetisnull(res, r, i) ? NULL : pstrdup(PQgetvalue(res, r, i));
		rows = lappend(rows, row);
	}
	PQclear(res);
	PQclear(loopback_exec(lc, "COMMIT", context));
	return rows;
}

void
GpLoopbackQueryInto(const char *dbname, const char *sql, ReturnSetInfo *rsinfo)
{
	TupleDesc	desc = rsinfo->setDesc;
	int			natts = desc->natts;
	FmgrInfo   *infuncs = palloc_array(FmgrInfo, natts);
	Oid		   *ioparams = palloc_array(Oid, natts);
	Datum	   *values = palloc_array(Datum, natts);
	bool	   *nulls = palloc_array(bool, natts);
	ListCell   *lc;

	for (int i = 0; i < natts; i++)
	{
		Oid			infunc;

		getTypeInputInfo(TupleDescAttr(desc, i)->atttypid, &infunc, &ioparams[i]);
		fmgr_info(infunc, &infuncs[i]);
	}

	foreach(lc, loopback_read(dbname, sql, natts))
	{
		char	  **row = (char **) lfirst(lc);

		for (int i = 0; i < natts; i++)
		{
			nulls[i] = row[i] == NULL;
			values[i] = InputFunctionCall(&infuncs[i], row[i], ioparams[i],
										  TupleDescAttr(desc, i)->atttypmod);
		}
		tuplestore_putvalues(rsinfo->setResult, desc, values, nulls);
	}
}

List *
GpLoopbackReadRows(const char *dbname, const char *sql, int ncols)
{
	return loopback_read(dbname, sql, ncols);
}

char *
GpLoopbackRunApart(const char *dbname, const char *sql)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	LoopbackConn *volatile lc = NULL;
	char	   *failure = NULL;

	PG_TRY();
	{
		lc = loopback_conn(NULL, dbname, false);
		PQclear(loopback_exec(lc, sql, dbname));
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		failure = edata->message;
	}
	PG_END_TRY();

	/* loopback_exec() closes a connection its statement failed on */
	foreach_ptr(LoopbackConn, c, conns)
	{
		if (c == lc)
		{
			loopback_forget(c);
			break;
		}
	}
	return failure;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpLoopbackInit(void)
{
	DefineCustomStringVariable("gp.maintenance_database",
							   "Database holding what the cluster keeps in one database.",
							   "The storage servers and their user mappings, which "
							   "Cloudberry keeps in shared catalogs; any other database "
							   "reaches them through a connection to this one.",
							   &gp_maintenance_database,
							   "postgres",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	RegisterXactCallback(loopback_xact_callback, NULL);
	RegisterSubXactCallback(loopback_subxact_callback, NULL);
	prev_commit_recorded_hook = xact_commit_recorded_hook;
	xact_commit_recorded_hook = loopback_commit_recorded;
}
