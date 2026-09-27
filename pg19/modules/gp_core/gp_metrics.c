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
 * gp_metrics.c
 *	  Cloudberry's query metrics in shared memory: each plan node's
 *	  instrumentation in a slot any session of the node can read, while
 *	  gp.enable_query_metrics is on.
 *
 * With Cloudberry's gp_enable_query_metrics on -- a setting of the
 * postmaster's -- each node keeps gp_instrument_shmem_size of
 * instrumentation slots in shared memory, on a free list
 * (InstrShmemInit()).  A statement's plan nodes take their instrumentation
 * from it as the executor initializes them (GpInstrAlloc()), each slot
 * marked with the process, the session and the command it is of, and the
 * slots go back as the resource owner they were taken under is released.
 * A statement a client sends is instrumented for its rows whether anyone
 * asks or not (GP_INSTRUMENT_OPTS, pquery.c), so that the slots show what
 * runs; gpcontrib's gp_instrument_shmem reads them.
 *
 * PostgreSQL 19's executor allocates a node's instrumentation as it
 * initializes the node, and reaches it through the node's pointer only.  So
 * a statement's start asks for its rows instrumented, as Cloudberry's portal
 * does, and once the plan is initialized each node's instrumentation is
 * moved into a slot and the node pointed at it: the executor, EXPLAIN
 * ANALYZE and a parallel query's workers' totals write there.  The slots go
 * back as the statement ends, or as the resource owner it started under is
 * released, after an error.  A slot's command is gp.command_count,
 * Cloudberry's gp_command_count: the coordinator counts its session's
 * statements, and the segments are sent the count with the settings.
 * Nothing of this is set up where the setting is off.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/executor/instrument.c (InstrShmemSize(), InstrShmemInit(),
 *	  GpInstrAlloc(), instrShmemRecycleCallback()), gp_command_count of
 *	  src/backend/cdb/cdbvars.c, and the settings of
 *	  src/backend/utils/misc/guc_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_metrics.h"

/*
 * How many of a statement's sequential scans take a slot, as Cloudberry's
 * MAX_SCAN_ON_SHMEM: a table of many partitions scanned whole would take a
 * slot for each, and the rest keep their own.
 */
#define MAX_SCANS_IN_SLOTS	300

typedef struct GpMetricsShared
{
	slock_t		lock;
	int			nslots;
	int			nfree;
	int			free_head;		/* a slot's index, -1 for none */
	GpMetricsSlot slots[FLEXIBLE_ARRAY_MEMBER];
} GpMetricsShared;

bool		gp_enable_query_metrics = false;
static int	gp_instrument_shmem_size = 5120;	/* kB */
static int	gp_command_count = 0;

static GpMetricsShared *metrics = NULL;

/* This process's slots: which query took each, under which owner. */
typedef struct TakenSlot
{
	int			slot;
	ResourceOwner owner;
	QueryDesc  *query;
} TakenSlot;

static List *taken = NIL;		/* TakenSlot, in TopMemoryContext */
static int	nesting = 0;		/* in a statement's run, or a utility's */
static int	command_count = 0;	/* the coordinator's statements */

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

/* as Cloudberry's InstrShmemNumSlots() counts them */
static int
metrics_nslots(void)
{
	Size		bytes = (Size) gp_instrument_shmem_size * 1024;

	if (bytes <= offsetof(GpMetricsShared, slots))
		return 0;
	return (bytes - offsetof(GpMetricsShared, slots)) / sizeof(GpMetricsSlot);
}

static Size
metrics_size(void)
{
	return add_size(offsetof(GpMetricsShared, slots),
					mul_size(metrics_nslots(), sizeof(GpMetricsSlot)));
}

static void
metrics_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(metrics_size()));
}

static void
metrics_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	metrics = ShmemInitStruct("gp_core instrumentation slots", metrics_size(),
							  &found);
	if (!found)
	{
		int			n = metrics_nslots();

		memset(metrics, 0, metrics_size());
		SpinLockInit(&metrics->lock);
		metrics->nslots = n;
		metrics->nfree = n;
		metrics->free_head = n > 0 ? 0 : -1;
		for (int i = 0; i < n; i++)
			metrics->slots[i].next_free = i + 1 < n ? i + 1 : -1;
	}
	LWLockRelease(AddinShmemInitLock);
}

int
GpMetricsSlotCount(int *nfree)
{
	int			n;

	if (metrics == NULL)
	{
		*nfree = 0;
		return 0;
	}
	SpinLockAcquire(&metrics->lock);
	n = metrics->nslots;
	*nfree = metrics->nfree;
	SpinLockRelease(&metrics->lock);
	return n;
}

bool
GpMetricsSlotCopy(int i, GpMetricsSlot *copy)
{
	if (metrics == NULL || i < 0 || i >= metrics->nslots)
		return false;
	SpinLockAcquire(&metrics->lock);
	*copy = metrics->slots[i];
	SpinLockRelease(&metrics->lock);
	return copy->pid != 0;
}

/* ------------------------------------------------------------------------- */
/* Taking and giving back                                                    */
/* ------------------------------------------------------------------------- */

static bool
is_slot(NodeInstrumentation *instr)
{
	return (char *) instr >= (char *) metrics->slots &&
		(char *) instr < (char *) &metrics->slots[metrics->nslots];
}

/*
 * The process's start, as Cloudberry's gp_gettmid() makes it of the
 * postmaster's: its time_t, -1 where that does not fit.
 */
static int32
metrics_tmid(void)
{
	pg_time_t	t = timestamptz_to_time_t(PgStartTime);

	return (PgStartTime < 0 || t > PG_INT32_MAX) ? -1 : (int32) t;
}

typedef struct TakeContext
{
	QueryDesc  *query;
	int			nscans;
	int32		tmid;
	int32		ssid;
	int16		segid;
} TakeContext;

/*
 * A node's instrumentation moved into a free slot, the node pointed at it,
 * as Cloudberry's pickInstrFromShmem() gives it one; a node for which none
 * is free keeps its own.
 */
static bool
take_slots_walker(PlanState *ps, TakeContext *cxt)
{
	if (ps->instrument != NULL && !is_slot(ps->instrument) &&
		(!IsA(ps, SeqScanState) || cxt->nscans++ < MAX_SCANS_IN_SLOTS))
	{
		int			i;

		SpinLockAcquire(&metrics->lock);
		i = metrics->free_head;
		if (i >= 0)
		{
			GpMetricsSlot *slot = &metrics->slots[i];

			metrics->free_head = slot->next_free;
			metrics->nfree--;
			slot->data = *ps->instrument;
			slot->pid = MyProcPid;
			slot->tmid = cxt->tmid;
			slot->ssid = cxt->ssid;
			slot->ccnt = gp_command_count;
			slot->segid = cxt->segid;
			slot->nid = (int16) ps->plan->plan_node_id;
			slot->next_free = -1;
		}
		SpinLockRelease(&metrics->lock);

		if (i >= 0)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			TakenSlot  *t = palloc_object(TakenSlot);

			t->slot = i;
			t->owner = CurrentResourceOwner;
			t->query = cxt->query;
			taken = lappend(taken, t);
			MemoryContextSwitchTo(oldcxt);
			ps->instrument = &metrics->slots[i].data;
		}
	}
	return planstate_tree_walker(ps, take_slots_walker, cxt);
}

static bool
taken_by(TakenSlot *t, QueryDesc *query, ResourceOwner owner)
{
	return (query == NULL || t->query == query) &&
		(owner == NULL || t->owner == owner);
}

/* The slots of a query, or of a resource owner, or all this process has. */
static void
give_back(QueryDesc *query, ResourceOwner owner)
{
	SpinLockAcquire(&metrics->lock);
	foreach_ptr(TakenSlot, t, taken)
	{
		GpMetricsSlot *slot = &metrics->slots[t->slot];

		if (!taken_by(t, query, owner))
			continue;
		memset(slot, 0, sizeof(GpMetricsSlot));
		slot->next_free = metrics->free_head;
		metrics->free_head = t->slot;
		metrics->nfree++;
	}
	SpinLockRelease(&metrics->lock);

	foreach_ptr(TakenSlot, t, taken)
	{
		if (!taken_by(t, query, owner))
			continue;
		taken = foreach_delete_current(taken, t);
		pfree(t);
	}
}

/* as Cloudberry's instrShmemRecycleCallback(), after an error too */
static void
metrics_resource_release(ResourceReleasePhase phase, bool isCommit,
						 bool isTopLevel, void *arg)
{
	if (phase == RESOURCE_RELEASE_AFTER_LOCKS && taken != NIL)
		give_back(NULL, CurrentResourceOwner);
}

static void
metrics_before_shmem_exit(int code, Datum arg)
{
	if (taken != NIL)
		give_back(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* The executor's hooks                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A session's own statements: a client's, the coordinator's or one a
 * segment is dispatched; not a background process's, nor a utility
 * session's on a segment, as Cloudberry's utility mode takes none.
 */
static bool
metrics_session(void)
{
	return AmRegularBackendProcess() &&
		(GpClusterBackendRole() != GP_ROLE_UTILITY || GpClusterIsSingleNode());
}

static void
metrics_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		session = metrics_session();

	/*
	 * A statement the session runs, rather than one a statement runs:
	 * instrumented for its rows, and on the coordinator counted.
	 */
	if (session && nesting == 0 && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		if (queryDesc->instrument_options == 0)
			queryDesc->instrument_options = INSTRUMENT_ROWS | GP_INSTR_METRICS_ONLY;
		if (GpClusterBackendRole() != GP_ROLE_EXECUTE)
		{
			char		count[16];

			snprintf(count, sizeof(count), "%d", ++command_count);
			(void) set_config_option("gp.command_count", count, PGC_USERSET,
									 PGC_S_SESSION, GUC_ACTION_SET, true, 0,
									 false);
		}
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (session && queryDesc->planstate != NULL &&
		queryDesc->estate->es_instrument != 0)
	{
		TakeContext cxt;
		static bool registered = false;

		if (!registered)
		{
			RegisterResourceReleaseCallback(metrics_resource_release, NULL);
			before_shmem_exit(metrics_before_shmem_exit, (Datum) 0);
			registered = true;
		}
		cxt.query = queryDesc;
		cxt.nscans = 0;
		cxt.tmid = metrics_tmid();
		cxt.ssid = GpClusterSessionId();
		cxt.segid = (int16) GpClusterContentId();
		(void) take_slots_walker(queryDesc->planstate, &cxt);
	}
}

static void
metrics_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count)
{
	nesting++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting--;
	}
	PG_END_TRY();
}

static void
metrics_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting--;
	}
	PG_END_TRY();
}

/* The slots go back once the statement has ended: EXPLAIN has read them. */
static void
metrics_ExecutorEnd(QueryDesc *queryDesc)
{
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (taken != NIL)
		give_back(queryDesc, NULL);
}

static void
metrics_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	nesting++;
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		nesting--;
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpMetricsInit(void)
{
	DefineCustomBoolVariable("gp.enable_query_metrics",
							 "Enable all query metrics collection.",
							 "Cloudberry calls this gp_enable_query_metrics.",
							 &gp_enable_query_metrics,
							 false,
							 PGC_POSTMASTER, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.instrument_shmem_size",
							"Sets the size of shmem allocated for instrumentation.",
							"Cloudberry calls this gp_instrument_shmem_size.",
							&gp_instrument_shmem_size,
							5120, 0, 131072,
							PGC_POSTMASTER, GUC_UNIT_KB,
							NULL, NULL, NULL);
	/*
	 * The coordinator's count of its session's statements, which it sets
	 * and sends the segments: a setting a session may set, which a segment
	 * is sent it by, where Cloudberry's is internal.
	 */
	DefineCustomIntVariable("gp.command_count",
							"Shows the number of commands received from the client in this session.",
							"Cloudberry calls this gp_command_count.",
							&gp_command_count,
							0, 0, INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							NULL, NULL, NULL);

	if (!gp_enable_query_metrics || metrics_nslots() == 0)
		return;

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = metrics_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = metrics_shmem_startup;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = metrics_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = metrics_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = metrics_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = metrics_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = metrics_ProcessUtility;
}
