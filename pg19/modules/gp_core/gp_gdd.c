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
 * gp_gdd.c
 *	  The global deadlock detector.
 *
 * With gp.enable_global_deadlock_detector an UPDATE or DELETE of a
 * distributed table locks the rows it changes rather than the table, and
 * two transactions may then wait for each other on different segments, a
 * deadlock no segment's own detector sees.  This process, on the
 * coordinator, every gp.global_deadlock_detector_period, asks each segment
 * which of its backends waits for which -- the edges of a graph whose
 * vertices are distributed transactions -- and then the coordinator itself,
 * reduces the graph as Cloudberry's detector does, and cancels the youngest
 * transaction of each cycle that is left.
 *
 * The graph and its reduction are Cloudberry's, compiled where they lie
 * (src/backend/utils/gdd/gdddetector.c): an edge is "solid" when what it
 * waits for is held until the transaction ends, "dotted" when it may be let
 * go sooner, and a vertex that waits for nothing anywhere, or for which
 * nothing waits, is no part of a cycle.  What the port supplies is the
 * vertices and the edges.  Cloudberry names a vertex by its gxid; the port's
 * distributed transaction ID is given only as a transaction commits, so a
 * vertex is named by a number the coordinator gives each transaction of each
 * session as it runs its first statement, larger for a younger one; a
 * segment's backend says which coordinator session it works for.  Whether
 * a lock is held until the transaction ends is Cloudberry's flag on the lock
 * (holdTillEndXact), which PostgreSQL 19 does not have; the port reads it
 * off the lock's kind: a transaction's, and a user table's, are held to the
 * end; a tuple's, a page's and a catalog's may not be.
 *
 * Cloudberry cancels the victim with a message, "cancelled by global
 * deadlock detector", which PostgreSQL 19's cancel cannot carry: the
 * victim's backend is marked, and the cancel's error reads so as it is
 * reported (gdd_emit_log).
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/gdd/gddbackend.c and gddfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/dsm_registry.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gdd/gdddetectorpriv.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_gdd.h"

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

bool		gp_enable_global_deadlock_detector = false;
static int	gp_global_deadlock_detector_period = 120;

/* ------------------------------------------------------------------------- */
/* Who each backend is                                                       */
/* ------------------------------------------------------------------------- */

/*
 * By proc number.  Written by the backend itself, read by others without a
 * lock: "pid" is written last, and a reader believes an entry only when the
 * process at that proc number has that pid.
 */
typedef struct GpGddBackend
{
	int			pid;
	int			session;		/* the coordinator session it works for */
	LocalTransactionId lxid;	/* the coordinator: whose number "seq" is */
	uint64		seq;			/* the coordinator: its transaction's number */
	bool		victim;			/* the detector cancelled it */
} GpGddBackend;

typedef struct GpGddShared
{
	pg_atomic_uint64 next_seq;
	int			nbackends;
	GpGddBackend backends[FLEXIBLE_ARRAY_MEMBER];
} GpGddShared;

static GpGddShared *gdd_shared = NULL;

static void
gdd_init_shared(void *ptr, void *arg)
{
	GpGddShared *s = (GpGddShared *) ptr;

	memset(s, 0, offsetof(GpGddShared, backends) +
		   mul_size(MaxBackends, sizeof(GpGddBackend)));
	pg_atomic_init_u64(&s->next_seq, 0);
	s->nbackends = MaxBackends;
}

static void
gdd_attach(void)
{
	bool		found;

	if (gdd_shared != NULL)
		return;
	gdd_shared = GetNamedDSMSegment("gp_core deadlock detector",
									offsetof(GpGddShared, backends) +
									mul_size(MaxBackends, sizeof(GpGddBackend)),
									gdd_init_shared, &found, NULL);
}

void
GpGddNoteBackend(void)
{
	GpGddBackend *b;

	if (MyProc == NULL || MyProcNumber < 0 || MyProcNumber >= MaxBackends)
		return;
	gdd_attach();
	b = &gdd_shared->backends[MyProcNumber];

	if (GpClusterIsDispatched())
	{
		if (b->pid != MyProcPid)
		{
			b->pid = 0;
			pg_write_barrier();
			b->session = GpClusterSessionId();
			b->seq = 0;
			b->victim = false;
			pg_write_barrier();
			b->pid = MyProcPid;
		}
	}
	else if (GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		if (b->pid != MyProcPid || b->lxid != MyProc->vxid.lxid)
		{
			b->pid = 0;
			pg_write_barrier();
			b->session = MyProcPid;
			b->lxid = MyProc->vxid.lxid;
			b->seq = pg_atomic_add_fetch_u64(&gdd_shared->next_seq, 1);
			b->victim = false;
			pg_write_barrier();
			b->pid = MyProcPid;
		}
	}
}

/* The session of a backend of this node, or 0 when it is none's. */
static int
backend_session(int pid)
{
	PGPROC	   *proc = BackendPidGetProc(pid);
	ProcNumber	n;
	GpGddBackend *b;

	if (proc == NULL)
		return 0;
	n = GetNumberFromPGProc(proc);
	if (n < 0 || n >= gdd_shared->nbackends)
		return 0;
	b = &gdd_shared->backends[n];
	if (b->pid != pid)
		return 0;
	pg_read_barrier();
	return b->session;
}

/*
 * The coordinator: the number of the transaction a session runs, or 0 when
 * the session runs none this detector knows of.
 */
static uint64
session_seq(int session, int *procno)
{
	for (int i = 0; i < gdd_shared->nbackends; i++)
	{
		GpGddBackend *b = &gdd_shared->backends[i];

		if (b->pid == session && b->seq != 0)
		{
			if (procno != NULL)
				*procno = i;
			return b->seq;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* The edges, on a node                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Is what a waiter waits for held until the holder's transaction ends?
 * Cloudberry's holdTillEndXact, read off the lock's kind; see the header.
 */
static bool
lock_is_solid(const LOCKTAG *tag)
{
	switch ((LockTagType) tag->locktag_type)
	{
		case LOCKTAG_TRANSACTION:
		case LOCKTAG_VIRTUALTRANSACTION:
		case LOCKTAG_OBJECT:
		case LOCKTAG_ADVISORY:
			return true;
		case LOCKTAG_RELATION:
			return tag->locktag_field2 >= FirstNormalObjectId;
		default:
			return false;
	}
}

static bool
lock_conflicts(LOCKMODE wait, LOCKMASK held)
{
	for (LOCKMODE m = 1; m <= MaxLockMode; m++)
		if ((held & LOCKBIT_ON(m)) && DoLockModesConflict(wait, m))
			return true;
	return false;
}

typedef struct GddEdgeRow
{
	int			waiter;
	int			holder;
	int			waiter_session;
	int			holder_session;
	bool		solid;
	const char *lockmode;
	const char *locktype;
} GddEdgeRow;

/*
 * Every waiting relation of this node: a backend waiting for a lock that
 * another backend holds in a conflicting mode -- not one of its own lock
 * group, which holds for it.  As Cloudberry's gp_dist_wait_status() finds
 * them, from the lock table.
 */
static List *
local_edges(void)
{
	LockData   *ld = GetLockStatusData();
	List	   *edges = NIL;

	gdd_attach();
	for (int w = 0; w < ld->nelements; w++)
	{
		LockInstanceData *wl = &ld->locks[w];

		if (wl->waitLockMode == NoLock)
			continue;
		for (int h = 0; h < ld->nelements; h++)
		{
			LockInstanceData *hl = &ld->locks[h];
			GddEdgeRow *e;

			if (h == w || hl->holdMask == 0 || hl->pid == wl->pid ||
				hl->leaderPid == wl->leaderPid ||
				memcmp(&hl->locktag, &wl->locktag, sizeof(LOCKTAG)) != 0 ||
				!lock_conflicts(wl->waitLockMode, hl->holdMask))
				continue;

			e = palloc(sizeof(GddEdgeRow));
			e->waiter = wl->pid;
			e->holder = hl->pid;
			e->waiter_session = backend_session(wl->pid);
			e->holder_session = backend_session(hl->pid);
			e->solid = lock_is_solid(&wl->locktag);
			e->lockmode = GetLockmodeName(wl->locktag.locktag_lockmethodid,
										  wl->waitLockMode);
			e->locktype = wl->locktag.locktag_type <= LOCKTAG_LAST_TYPE
				? LockTagTypeNames[wl->locktag.locktag_type] : "unknown";
			edges = lappend(edges, e);
		}
	}
	return edges;
}

/*
 * The same, as text, for the detector: a line per edge, its fields
 * separated by tabs -- what SHOW gp.dist_wait_status answers.  A setting
 * rather than a function, so that the detector can ask any database of a
 * segment without making anything there: an object a segment makes by
 * itself takes an OID from its own counter, which the coordinator's may
 * give out again (gp_ddl.c).
 */
static char *gdd_wait_status_setting = NULL;

static const char *
gdd_show_wait_status(void)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, local_edges())
	{
		GddEdgeRow *e = (GddEdgeRow *) lfirst(lc);

		appendStringInfo(&buf, "%d\t%d\t%d\t%d\t%c\t%s\t%s\n",
						 e->waiter, e->holder, e->waiter_session,
						 e->holder_session, e->solid ? 't' : 'f',
						 e->lockmode, e->locktype);
	}
	return buf.data;
}

PG_FUNCTION_INFO_V1(gp_dist_wait_status);

/*
 * gp_dist_wait_status()
 *		This node's waiting relations, each with the coordinator sessions of
 *		its two backends -- 0 for a backend no session dispatched to.
 */
Datum
gp_dist_wait_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, local_edges())
	{
		GddEdgeRow *e = (GddEdgeRow *) lfirst(lc);
		Datum		values[8];
		bool		nulls[8] = {false, false, false, false, false, false, false, false};

		values[0] = Int32GetDatum(GpClusterContentId());
		values[1] = Int32GetDatum(e->waiter);
		values[2] = Int32GetDatum(e->holder);
		values[3] = Int32GetDatum(e->waiter_session);
		values[4] = Int32GetDatum(e->holder_session);
		values[5] = BoolGetDatum(e->solid);
		values[6] = CStringGetTextDatum(e->lockmode);
		values[7] = CStringGetTextDatum(e->locktype);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* The victim's message                                                      */
/* ------------------------------------------------------------------------- */

static emit_log_hook_type prev_emit_log_hook = NULL;

/*
 * Cloudberry's words for the cancel, on the error of a backend the detector
 * cancelled: PostgreSQL 19's own is "canceling statement due to user
 * request", and a cancel carries nothing more.
 */
static void
gdd_emit_log(ErrorData *edata)
{
	if (edata->elevel == ERROR && edata->sqlerrcode == ERRCODE_QUERY_CANCELED &&
		gdd_shared != NULL && MyProcNumber >= 0 &&
		MyProcNumber < gdd_shared->nbackends &&
		gdd_shared->backends[MyProcNumber].victim &&
		gdd_shared->backends[MyProcNumber].pid == MyProcPid)
	{
		gdd_shared->backends[MyProcNumber].victim = false;
		edata->message = pstrdup("canceling statement due to user request: \"cancelled by global deadlock detector\"");
	}

	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);
}

/* ------------------------------------------------------------------------- */
/* The detector                                                              */
/* ------------------------------------------------------------------------- */

typedef struct GddVertInfo
{
	int			pid;			/* the coordinator backend */
	int			session;
} GddVertInfo;

typedef struct GddEdgeInfo
{
	int			waiter;			/* the node's backends */
	int			holder;
	char	   *lockmode;
	char	   *locktype;
} GddEdgeInfo;

static char *gdd_user = NULL;
static PGconn **gdd_conns = NULL;	/* by content id, kept */

static uint32
gdd_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryGlobalDeadlockDetector");
	return event;
}

/*
 * A segment's connection, kept from round to round: a utility session as
 * the bootstrap superuser, in a database every node has.
 */
static PGconn *
gdd_conn(const GpSegmentConfig *seg)
{
	PGconn	   *conn = gdd_conns[seg->content];
	const char *dbs[] = {"postgres", "template1"};

	if (conn != NULL && PQstatus(conn) == CONNECTION_OK)
		return conn;
	if (conn != NULL)
		libpqsrv_disconnect(conn);
	gdd_conns[seg->content] = NULL;

	for (int d = 0; d < lengthof(dbs); d++)
	{
		const char *keywords[8];
		const char *values[8];
		char		portbuf[16];
		int			n = 0;
		const char *passfile = GpDispatchPassfile();

		snprintf(portbuf, sizeof(portbuf), "%d", seg->port);
		keywords[n] = "host";
		values[n++] = seg->hostname;
		keywords[n] = "port";
		values[n++] = portbuf;
		keywords[n] = "dbname";
		values[n++] = dbs[d];
		keywords[n] = "user";
		values[n++] = gdd_user;
		keywords[n] = "application_name";
		values[n++] = "cloudberry global deadlock detector";
		if (passfile != NULL && passfile[0] != '\0')
		{
			keywords[n] = "passfile";
			values[n++] = passfile;
		}
		keywords[n] = NULL;
		values[n] = NULL;

		conn = libpqsrv_connect_params(keywords, values, false, gdd_wait_event());
		if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
		{
			if (conn != NULL)
				libpqsrv_disconnect(conn);
			continue;
		}
		gdd_conns[seg->content] = conn;
		return conn;
	}
	ereport(LOG,
			(errmsg("global deadlock detector could not connect to segment %d (%s:%d)",
					seg->content, seg->hostname, seg->port)));
	return NULL;
}

static void
gdd_add_edge(GddCtx *ctx, int segid, GddEdgeRow *row)
{
	int			wproc = -1,
				hproc = -1;
	uint64		wseq = session_seq(row->waiter_session, &wproc);
	uint64		hseq = session_seq(row->holder_session, &hproc);
	GddEdge    *edge;
	GddEdgeInfo *einfo;
	GddVertInfo *winfo;
	GddVertInfo *hinfo;

	/* a backend no distributed transaction runs, or one waiting for itself */
	if (wseq == 0 || hseq == 0 || wseq == hseq)
		return;

	edge = GddCtxAddEdge(ctx, segid, wseq, hseq, row->solid);
	einfo = palloc(sizeof(GddEdgeInfo));
	einfo->waiter = row->waiter;
	einfo->holder = row->holder;
	einfo->lockmode = pstrdup(row->lockmode);
	einfo->locktype = pstrdup(row->locktype);
	edge->data = einfo;
	winfo = palloc(sizeof(GddVertInfo));
	winfo->pid = row->waiter_session;
	winfo->session = row->waiter_session;
	edge->from->data = winfo;
	hinfo = palloc(sizeof(GddVertInfo));
	hinfo->pid = row->holder_session;
	hinfo->session = row->holder_session;
	edge->to->data = hinfo;
}

/* The graph that is left, as Cloudberry's gddbackend.c logs it. */
static void
gdd_dump(GddCtx *ctx, StringInfo str)
{
	GddMapIter	iter;

	appendStringInfoChar(str, '{');
	gdd_ctx_foreach_graph(iter, ctx)
	{
		GddGraph   *graph = gdd_map_iter_get_ptr(iter);
		GddMapIter	vertiter;
		GddListIter edgeiter;
		bool		first = true;

		appendStringInfo(str, "\"seg%d\": [", graph->id);
		gdd_graph_foreach_out_edge(vertiter, edgeiter, graph)
		{
			GddEdge    *edge = gdd_list_iter_get_ptr(edgeiter);
			GddEdgeInfo *einfo = (GddEdgeInfo *) edge->data;

			if (!first)
				appendStringInfoChar(str, ',');
			first = false;
			appendStringInfo(str,
							 "\"p%d of dtx" UINT64_FORMAT " con%d waits for a %s lock on %s mode, "
							 "blocked by p%d of dtx" UINT64_FORMAT " con%d\"",
							 einfo->waiter, edge->from->id,
							 ((GddVertInfo *) edge->from->data)->session,
							 einfo->locktype, einfo->lockmode,
							 einfo->holder, edge->to->id,
							 ((GddVertInfo *) edge->to->data)->session);
		}
		appendStringInfoChar(str, ']');
		if (gdd_map_iter_has_next(iter))
			appendStringInfoChar(str, ',');
	}
	appendStringInfoChar(str, '}');
}

/*
 * One round: the segments' waits first, then the coordinator's -- so that
 * what the coordinator says is the later of the two, as Cloudberry's
 * gp_dist_wait_status() orders them -- the graph reduced, and what is left
 * broken.
 */
static void
gdd_round(void)
{
	const GpSegmentConfig *segs;
	int			nsegs;
	GddCtx	   *ctx = GddCtxNew();
	ListCell   *lc;
	List	   *victims;

	segs = GpClusterSegments(&nsegs);
	for (int s = 0; s < nsegs; s++)
	{
		PGconn	   *conn = gdd_conn(&segs[s]);
		PGresult   *res;

		if (conn == NULL)
			return;				/* a graph with a segment missing proves nothing */
		res = libpqsrv_exec(conn, "SHOW gp.dist_wait_status", gdd_wait_event());
		if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
		{
			ereport(LOG,
					(errmsg("global deadlock detector could not read the waits of segment %d: %s",
							segs[s].content, PQerrorMessage(conn))));
			PQclear(res);
			libpqsrv_disconnect(conn);
			gdd_conns[segs[s].content] = NULL;
			return;
		}
		{
			char	   *text = pstrdup(PQgetvalue(res, 0, 0));
			char	   *line;
			char	   *save = NULL;

			for (line = strtok_r(text, "\n", &save); line != NULL;
				 line = strtok_r(NULL, "\n", &save))
			{
				GddEdgeRow	row;
				char	   *f[7];
				char	   *fsave = NULL;
				int			nf = 0;

				for (char *tok = strtok_r(line, "\t", &fsave); tok != NULL && nf < 7;
					 tok = strtok_r(NULL, "\t", &fsave))
					f[nf++] = tok;
				if (nf != 7)
					continue;
				row.waiter = atoi(f[0]);
				row.holder = atoi(f[1]);
				row.waiter_session = atoi(f[2]);
				row.holder_session = atoi(f[3]);
				row.solid = f[4][0] == 't';
				row.lockmode = f[5];
				row.locktype = f[6];
				gdd_add_edge(ctx, segs[s].content, &row);
			}
		}
		PQclear(res);
	}

	foreach(lc, local_edges())
		gdd_add_edge(ctx, -1, (GddEdgeRow *) lfirst(lc));

	GddCtxReduce(ctx);
	if (GddCtxEmpty(ctx))
		return;

	{
		StringInfoData graph;

		initStringInfo(&graph);
		gdd_dump(ctx, &graph);
		ereport(LOG,
				(errmsg("global deadlock detected! Final graph is :%s", graph.data)));
	}

	victims = GddCtxBreakDeadLock(ctx);
	foreach(lc, victims)
	{
		uint64		seq = *(uint64 *) lfirst(lc);

		for (int i = 0; i < gdd_shared->nbackends; i++)
		{
			GpGddBackend *b = &gdd_shared->backends[i];
			int			pid = b->pid;

			if (pid == 0 || b->seq != seq)
				continue;
			ereport(LOG,
					(errmsg("these gxids will be cancelled to break global deadlock: " UINT64_FORMAT "(Master Pid: %d)",
							seq, pid)));
			b->victim = true;
			if (kill(pid, SIGINT) != 0)
				b->victim = false;
			break;
		}
	}
}

PGDLLEXPORT void GpGddMain(Datum main_arg);

void
GpGddMain(Datum main_arg)
{
	MemoryContext round_cxt;
	int			nsegs;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	StartTransactionCommand();
	gdd_user = MemoryContextStrdup(TopMemoryContext,
								   GetUserNameFromId(BOOTSTRAP_SUPERUSERID, false));
	CommitTransactionCommand();

	gdd_attach();
	GpClusterSegments(&nsegs);
	gdd_conns = MemoryContextAllocZero(TopMemoryContext,
									   Max(nsegs, 1) * sizeof(PGconn *));
	round_cxt = AllocSetContextCreate(TopMemoryContext, "gp_core deadlock detector",
									  ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (GP_FAULT("gdd_probe") != GP_FAULT_SKIP)
		{
			MemoryContext old = MemoryContextSwitchTo(round_cxt);

			gdd_round();
			MemoryContextSwitchTo(old);
			MemoryContextReset(round_cxt);
		}

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 gp_global_deadlock_detector_period * 1000L,
						 gdd_wait_event());
		ResetLatch(MyLatch);
	}
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpGddInit(void)
{
	const GpSegmentConfig *self;

	DefineCustomBoolVariable("gp.enable_global_deadlock_detector",
							 "Lock the rows an UPDATE or DELETE changes, and detect distributed deadlocks.",
							 "Off, an UPDATE or DELETE of a distributed table, and SELECT ... "
							 "FOR UPDATE of one, lock the whole table, as Cloudberry's do "
							 "without its detector.",
							 &gp_enable_global_deadlock_detector,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gp.dist_wait_status",
							   "This node's waits, for the global deadlock detector.",
							   "Read-only: a line per waiting relation -- waiter, holder, "
							   "their coordinator sessions, whether it lasts to the end "
							   "of the transaction, the lock mode and kind.",
							   &gdd_wait_status_setting,
							   "",
							   PGC_INTERNAL,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, gdd_show_wait_status);

	DefineCustomIntVariable("gp.global_deadlock_detector_period",
							"How often the global deadlock detector looks for a deadlock.",
							NULL,
							&gp_global_deadlock_detector_period,
							120, 5, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	if (GpClusterIsSingleNode())
		return;

	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = gdd_emit_log;

	self = GpClusterSelf();
	if (self != NULL && self->content == -1 && gp_enable_global_deadlock_detector)
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 5;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpGddMain");
		snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core global deadlock detector");
		snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core global deadlock detector");
		RegisterBackgroundWorker(&worker);
	}
}
