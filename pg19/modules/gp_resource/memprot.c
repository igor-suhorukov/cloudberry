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
 * memprot.c
 *	  Memory protection: Cloudberry's vmem tracker, on the block allocation
 *	  hook.
 *
 * Cloudberry counts the memory each process takes in chunks of 1 MB, each a
 * process's, its session's on the node and the node's (vmem_tracker.c), and
 * refuses the chunk that would take a segment's executor past a query's limit,
 * gp_vmem_limit_per_query, or the node's, gp_vmem_protect_limit.  It counts
 * the blocks its memory contexts take from malloc, through gp_malloc(); the
 * port counts the same blocks through O25, memory_block_alloc_hook, which
 * PostgreSQL 19's four context types call as they make, free and grow one
 * (utils/memutils.h).  A refused block ends the statement with Cloudberry's
 * "Out of memory", raised here, in its words -- which O25 lets a hook do in
 * place of refusing -- and a backend that could not take the memory a
 * process starts with, 12 MB, ends at connection, as Cloudberry's does.
 *
 * Its red zone and runaway cleaner (redzone_handler.c, runaway_cleaner.c):
 * past runaway_detector_activation_percent of the node's limit, the session
 * that holds the most, of those running a statement, is flagged, and each of
 * its processes cancels its statement -- "Canceling query because of high
 * VMEM usage" -- the next time it takes a chunk, which is where Cloudberry's
 * checks too, and in CHECK_FOR_INTERRUPTS(), which PostgreSQL 19's macro
 * gives no way to reach; so one of its processes that takes no more memory
 * is sent a cancel a second later, by whichever process of the node next
 * takes a chunk in the red zone (runaway_nudge()).  The idle tracker
 * (idle_tracker.c) is told a process is running a statement from the first
 * hook of one -- its parse analysis, its utility statement, its executor --
 * to its end, where Cloudberry's is told from reading a command to
 * ReadyForQuery, which PostgreSQL 19 has no hook at.
 *
 * Those files are Cloudberry's, compiled where they lie (meson.build, and
 * the include overlay in compat/memprot/): this file is the rest, what
 * Cloudberry's memprot.c, postinit.c and postgres.c do around them.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/utils/mmgr/memprot.c, and the memory protection of
 *	  src/backend/utils/init/postinit.c and src/backend/tcop/postgres.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <signal.h>

#include "access/xact.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_resource.h"

/* Cloudberry's own headers, and what they ask of the port, through the overlay */
#include "../../compat/memprot/cdb/cdbvars.h"
#include "../../compat/memprot/utils/resource_manager.h"
#include "../../compat/memprot/utils/session_state.h"
#include "../../compat/memprot/utils/vmem_tracker.h"

/* Cloudberry's errcodes.txt: 53500, gp_memprot_kill */
#define ERRCODE_GP_MEMPROT_KILL		MAKE_SQLSTATE('5','3','5','0','0')

/* ------------------------------------------------------------------------- */
/* The settings                                                              */
/* ------------------------------------------------------------------------- */

int			gp_vmem_protect_limit = 8192;
int			gp_vmem_limit_per_query = 0;
bool		vmem_process_interrupt = false;
bool		coredump_on_memerror = false;
int			gp_sessionstate_loglevel = DEBUG1;
static int	gp_vmem_protect_segworker_cache_limit = 500;

/*
 * runaway_detector_activation_percent is redzone_handler.c's, which starts it
 * at 80, and Cloudberry's setting starts at 90: the setting has a variable of
 * its own, which it gives the file's as it is set.
 */
static int	runaway_percent = 90;

static void
assign_runaway_percent(int newval, void *extra)
{
	runaway_detector_activation_percent = newval;
}

static int	explain_memory_verbosity = 0;

static const struct config_enum_entry explain_memory_verbosity_options[] = {
	{"suppress", 0, false},
	{"summary", 1, false},
	{"detail", 2, false},
	{"debug", 3, false},
	{NULL, 0, false}
};

/*
 * Cloudberry hides some of these from SHOW ALL; the port does not, so that
 * pg_settings, which the test suites respell Cloudberry's names by, lists
 * them.
 */
static void
define_settings(void)
{
	DefineCustomIntVariable("gp.vmem_protect_limit",
							"Virtual memory limit (in MB) of Cloudberry memory protection.",
							NULL, &gp_vmem_protect_limit,
							8192, 0, INT_MAX / 2, PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.vmem_limit_per_query",
							"Sets the maximum allowed memory per-statement on each segment.",
							NULL, &gp_vmem_limit_per_query,
							0, 0, INT_MAX / 2, PGC_POSTMASTER,
							GUC_UNIT_KB | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.runaway_detector_activation_percent",
							"The runaway detector activates if the used vmem exceeds this percentage of the vmem quota. Set to 0 or 100 to disable runaway detection.",
							NULL, &runaway_percent,
							90, 0, 100, PGC_POSTMASTER, 0,
							NULL, assign_runaway_percent, NULL);
	DefineCustomIntVariable("gp.vmem_protect_segworker_cache_limit",
							"Max virtual memory limit (in MB) for a segworker to be cachable.",
							"Accepted for Cloudberry's scripts: a session keeps its segment connections whatever they hold.",
							&gp_vmem_protect_segworker_cache_limit,
							500, 1, INT_MAX / 2, PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.vmem_process_interrupt",
							 "Checks for interrupts before reserving VMEM",
							 NULL, &vmem_process_interrupt,
							 false, PGC_USERSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.coredump_on_memerror",
							 "Generate core dump on memory error.",
							 NULL, &coredump_on_memerror,
							 false, PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.sessionstate_loglevel",
							 "Sets the logging level for session state debugging messages",
							 NULL, &gp_sessionstate_loglevel,
							 DEBUG1, server_message_level_options,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.explain_memory_verbosity",
							 "Experimental feature: show memory account usage in EXPLAIN ANALYZE.",
							 "Accepted for Cloudberry's scripts: EXPLAIN ANALYZE shows PostgreSQL's memory, and no accounts of Cloudberry's.",
							 &explain_memory_verbosity,
							 0, explain_memory_verbosity_options,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* What the overlay asks of the port                                         */
/* ------------------------------------------------------------------------- */

/*
 * The statement the backend runs: counted from 1 as each begins, and 0 on a
 * segment between two, as Cloudberry's QE resets gp_command_count before it
 * reads its next command.  The runaway cleaner cancels no process that runs
 * none.
 */
int			gp_command_count = 0;

int
GpMemProtSessionId(void)
{
	return GpClusterSessionId();
}

/* gp_resource.h's, which the overlay's macro of the name would hide */
bool
GpMemProtResGroupEnabled(void)
{
	return (IsResGroupEnabled) ();
}

#define SESSION_STATE_TRANCHE	"gp_resource session states"

static LWLock *session_state_lock = NULL;

LWLock *
GpMemProtSessionStateLock(void)
{
	if (session_state_lock == NULL)
		session_state_lock = &(GetNamedLWLockTranche(SESSION_STATE_TRANCHE))->lock;
	return session_state_lock;
}

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

/* Cloudberry's, which its files define (the overlay declares them) */
extern void EventVersion_ShmemInit(void);

#define SHMEM_OOM_TIME "last vmem oom time"

/* The last time a segment ran out of memory, and this process's view of it */
volatile OOMTimeType *segmentOOMTime = NULL;
volatile OOMTimeType alreadyReportedOOMTime = 0;
volatile OOMTimeType oomTrackerStartTime = 0;

/* Is memory protection on in this process? */
bool		gp_mp_inited = false;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/*
 * The port's own: which session each backend of the node works for, and
 * whether it runs a statement -- so that a runaway's process that has
 * stopped taking memory can be found and cancelled -- and which runaway
 * event was seen first when, and which has been cancelled.
 */
typedef struct MemProtBackend
{
	int			session;		/* -1: none */
	bool		active;
} MemProtBackend;

typedef struct MemProtShared
{
	pg_atomic_uint64 seen_version;	/* the runaway event seen last */
	pg_atomic_uint64 seen_at;	/* when it was first seen */
	pg_atomic_uint64 nudged_version;	/* the one whose stragglers were cancelled */
	MemProtBackend backends[FLEXIBLE_ARRAY_MEMBER];
} MemProtShared;

static MemProtShared *memprot_shared = NULL;

static Size
memprot_shared_size(void)
{
	return add_size(offsetof(MemProtShared, backends),
					mul_size(sizeof(MemProtBackend), MaxBackends));
}

/*
 * Each struct is ShmemInitStruct()'s, and aligned to a cache line: the
 * node's vmem, its last OOM, the two event versions, the runaway detector's
 * flag -- and the session states, and the port's record of the backends.
 */
static void
memprot_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(add_size(add_size(SessionState_ShmemSize(),
											 memprot_shared_size()),
									6 * PG_CACHE_LINE_SIZE + 1024));
	RequestNamedLWLockTranche(SESSION_STATE_TRANCHE, 1);
}

/*
 * GPMemoryProtect_ShmemInit(), and SessionState_ShmemInit() before it, which
 * CreateSharedMemoryAndSemaphores() calls in Cloudberry.  Run by the
 * postmaster, where the vmem tracker works out its chunk size and limits
 * once, for every process it forks.
 */
static void
memprot_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	SessionState_ShmemInit();
	VmemTracker_ShmemInit();
	segmentOOMTime = (OOMTimeType *) ShmemInitStruct(SHMEM_OOM_TIME,
													 sizeof(OOMTimeType),
													 &found);
	if (!found)
		*segmentOOMTime = 0;
	memprot_shared = ShmemInitStruct("gp_resource memory protection",
									 memprot_shared_size(), &found);
	if (!found)
	{
		pg_atomic_init_u64(&memprot_shared->seen_version, 0);
		pg_atomic_init_u64(&memprot_shared->seen_at, 0);
		pg_atomic_init_u64(&memprot_shared->nudged_version, 0);
		for (int i = 0; i < MaxBackends; i++)
		{
			memprot_shared->backends[i].session = -1;
			memprot_shared->backends[i].active = false;
		}
	}
	LWLockRelease(AddinShmemInitLock);
}

/* ------------------------------------------------------------------------- */
/* Out of memory                                                             */
/* ------------------------------------------------------------------------- */

/* UpdateTimeAtomically() */
void
UpdateTimeAtomically(volatile OOMTimeType *time_var)
{
	bool		done = false;

	while (!done)
	{
		OOMTimeType newtime = GetCurrentTimestamp();
		OOMTimeType oldtime = *time_var;

		done = pg_atomic_compare_exchange_u64((pg_atomic_uint64 *) time_var,
											  (uint64 *) &oldtime,
											  (uint64) newtime);
	}
}

/*
 * gp_failed_to_alloc(): a chunk the tracker refused, in Cloudberry's words,
 * after its report of what every session holds and of this process's memory
 * contexts; with a waiver of 1 MB past the limit, so that the error can be
 * made.  At elevel, which is FATAL for the memory a process starts with.
 */
static void
gp_failed_to_alloc(MemoryAllocationStatus ec, int sz, int elevel)
{
	if (ec != MemoryFailure_QueryMemoryExhausted)
		UpdateTimeAtomically(segmentOOMTime);
	UpdateTimeAtomically(&alreadyReportedOOMTime);

	VmemTracker_RequestWaiver(1024 * 1024);

	if (ec == MemoryFailure_QueryMemoryExhausted)
		elog(LOG, "Logging memory usage for reaching per-query memory limit");
	else if (ec == MemoryFailure_VmemExhausted)
		write_stderr("Logging memory usage for reaching Vmem limit");
	else if (ec == MemoryFailure_SystemMemoryExhausted)
		write_stderr("Logging memory usage for reaching system memory limit");
	else if (ec == MemoryFailure_ResourceGroupMemoryExhausted)
		write_stderr("Logging memory usage for reaching resource group limit");
	else
		elog(ERROR, "Unknown memory failure error code");

	RedZoneHandler_LogVmemUsageOfAllSessions();
	MemoryContextStats(TopMemoryContext);

	if (coredump_on_memerror)
		*(volatile int *) NULL = ec;

	if (ec == MemoryFailure_VmemExhausted)
		ereport(elevel,
				(errcode(ERRCODE_GP_MEMPROT_KILL),
				 errmsg("Out of memory"),
				 errdetail("Vmem limit reached, failed to allocate %d bytes from tracker, which has %d MB available",
						   sz, VmemTracker_GetAvailableVmemMB())));
	else if (ec == MemoryFailure_QueryMemoryExhausted)
		ereport(elevel,
				(errcode(ERRCODE_GP_MEMPROT_KILL),
				 errmsg("Out of memory"),
				 errdetail("Per-query memory limit reached: current limit is %d kB, requested %d bytes, has %d MB available for this query",
						   gp_vmem_limit_per_query, sz,
						   VmemTracker_GetAvailableQueryVmemMB())));
	else if (ec == MemoryFailure_SystemMemoryExhausted)
		ereport(elevel,
				(errcode(ERRCODE_GP_MEMPROT_KILL),
				 errmsg("Out of memory"),
				 errdetail("System memory limit reached, failed to allocate %d bytes from system", sz)));
	else
		ereport(elevel,
				(errcode(ERRCODE_GP_MEMPROT_KILL),
				 errmsg("Out of memory"),
				 errdetail("Resource group memory limit reached")));
}

/* ------------------------------------------------------------------------- */
/* The hook                                                                  */
/* ------------------------------------------------------------------------- */

static memory_block_alloc_hook_type prev_memory_block_alloc_hook = NULL;
static bool memprot_starting = false;
static bool memprot_never = false;

static void memprot_start(void);

/* Cloudberry's, which its files define */
extern volatile uint32 *isRunawayDetector;
extern volatile EventVersion *latestRunawayVersion;

/* How long a runaway's process is given to cancel itself before it is sent a cancel */
#define RUNAWAY_GRACE_MS	1000

static bool memprot_nudging = false;

/*
 * A runaway's process cancels itself the next time it takes a chunk, where
 * Cloudberry's also does in CHECK_FOR_INTERRUPTS(), which PostgreSQL 19's
 * macro gives no way to reach: so one that takes none -- it waits, or
 * works in memory it has -- would hold the red zone, and the runaway
 * detector with it.  Asked by a backend of the node as it takes a chunk in
 * the red zone: once the event is a second old, the runaway's processes
 * that still run a statement are sent a cancel, once, which is PostgreSQL's
 * own "canceling statement" -- and as a cancelled one ends its statement it
 * cleans up, as the cleaner's other processes do.
 */
static void
runaway_nudge(void)
{
	EventVersion version = *latestRunawayVersion;
	TimestampTz now;
	uint64		seen;
	uint64		nudged;
	int			runaway = -1;

	if (memprot_nudging || version == 0 || *isRunawayDetector == 0 ||
		pg_atomic_read_u64(&memprot_shared->nudged_version) == (uint64) version)
		return;
	memprot_nudging = true;

	now = GetCurrentTimestamp();
	seen = pg_atomic_read_u64(&memprot_shared->seen_version);
	if (seen != (uint64) version)
	{
		if (pg_atomic_compare_exchange_u64(&memprot_shared->seen_version,
										   &seen, (uint64) version))
			pg_atomic_write_u64(&memprot_shared->seen_at, (uint64) now);
		memprot_nudging = false;
		return;
	}
	if (!TimestampDifferenceExceeds((TimestampTz) pg_atomic_read_u64(&memprot_shared->seen_at),
									now, RUNAWAY_GRACE_MS))
	{
		memprot_nudging = false;
		return;
	}

	LWLockAcquire(SessionStateLock, LW_SHARED);
	for (SessionState *state = AllSessionStateEntries->usedList; state != NULL;
		 state = state->next)
	{
		if (state->runawayStatus != RunawayStatus_NotRunaway &&
			state->cleanupCountdown > 0)
			runaway = state->sessionId;
	}
	LWLockRelease(SessionStateLock);

	nudged = pg_atomic_read_u64(&memprot_shared->nudged_version);
	if (runaway >= 0 && nudged != (uint64) version &&
		pg_atomic_compare_exchange_u64(&memprot_shared->nudged_version,
									   &nudged, (uint64) version))
	{
		for (int i = 0; i < MaxBackends; i++)
		{
			volatile MemProtBackend *b = &memprot_shared->backends[i];
			int			pid = GetPGProcByNumber(i)->pid;

			if (b->session == runaway && b->active && pid != 0 &&
				pid != MyProcPid)
				(void) kill(pid, SIGINT);
		}
	}
	memprot_nudging = false;
}

/*
 * O25's hook: every block a memory context takes from malloc is reserved
 * from the tracker first, and every one it gives back is released, as
 * gp_malloc() and gp_free() reserve and release theirs.  A refusal is
 * raised as Cloudberry's "Out of memory"; a malloc() that fails after the
 * reservation comes back as the opposite change, and is released.
 */
static bool
memprot_block_hook(MemoryContext context, Size oldsize, Size newsize)
{
	MemoryAllocationStatus status;

	/* a hook before this one that refuses the block leaves nothing to count */
	if (prev_memory_block_alloc_hook &&
		!prev_memory_block_alloc_hook(context, oldsize, newsize))
		return false;

	if (unlikely(!gp_mp_inited))
	{
		if (memprot_never || memprot_starting)
			return true;
		memprot_start();
		if (!gp_mp_inited)
			return true;
	}

	if (newsize > oldsize)
	{
		int32		chunks = VmemTracker_GetReservedVmemChunks();

		status = VmemTracker_ReserveVmem((int64) (newsize - oldsize));
		if (status != MemoryAllocation_Success)
			gp_failed_to_alloc(status, (int) Min(newsize - oldsize, INT_MAX),
							   ERROR);
		if (VmemTracker_GetReservedVmemChunks() > chunks &&
			RedZoneHandler_IsVmemRedZone())
			runaway_nudge();
	}
	else if (oldsize > newsize)
		VmemTracker_ReleaseVmem((int64) (oldsize - newsize));
	return true;
}

/* ------------------------------------------------------------------------- */
/* A backend's memory protection                                             */
/* ------------------------------------------------------------------------- */

static void memprot_exit(int code, Datum arg);

/*
 * GPMemoryProtect_TrackStartupMemory(): what a process has taken before its
 * memory is counted, which Cloudberry reckons as 6 MB, 6 MB more for ORCA
 * less the 2 MB ORCA's allocator has counted anyway, and 2 MB for an
 * extension's: 12 MB.  A node that cannot give it refuses the connection.
 */
static void
track_startup_memory(void)
{
	int64		bytes = 0;
	MemoryAllocationStatus status;

	bytes += 6L << BITS_IN_MB;
	bytes += 6L << BITS_IN_MB;
	bytes -= 2L << BITS_IN_MB;
	bytes += 2L << BITS_IN_MB;

	status = VmemTracker_RegisterStartupMemory(bytes);
	if (status != MemoryAllocation_Success)
		gp_failed_to_alloc(status, (int) bytes, FATAL);
}

/*
 * SessionState_Init(), GPMemoryProtect_Init() and the startup memory, which
 * Cloudberry's InitPostgres() and InitResManager() do: a backend of a
 * client's, or of the coordinator's dispatch, counts from its first block
 * after InitPostgres() -- PostgresMain() makes one at once -- and before its
 * first command, so that a node without the memory for it refuses the
 * connection.  It begins idle, as Cloudberry's is by the time it reads a
 * command.
 */
static void
memprot_start(void)
{
	if (!IsUnderPostmaster || MyBackendType != B_BACKEND || MyProc == NULL ||
		!IsNormalProcessingMode())
		return;

	memprot_starting = true;
	SessionState_Init();
	if (MySessionState == NULL)
	{
		memprot_never = true;
		memprot_starting = false;
		return;
	}

	VmemTracker_Init();
	memprot_shared->backends[MyProcNumber].session = MySessionState->sessionId;
	memprot_shared->backends[MyProcNumber].active = false;
	oomTrackerStartTime = GetCurrentTimestamp();
	alreadyReportedOOMTime = 0;
	gp_mp_inited = true;
	before_shmem_exit(memprot_exit, 0);
	memprot_starting = false;

	track_startup_memory();
	IdleTracker_DeactivateProcess();
}

/* GPMemoryProtect_Shutdown() and SessionState_Shutdown(), as a backend exits */
static void
memprot_exit(int code, Datum arg)
{
	if (!gp_mp_inited)
		return;
	ReportOOMConsumption();
	memprot_shared->backends[MyProcNumber].session = -1;
	memprot_shared->backends[MyProcNumber].active = false;
	VmemTracker_UnregisterStartupMemory();
	gp_mp_inited = false;
	VmemTracker_Shutdown();
	SessionState_Shutdown();
	memprot_never = true;
}

/* ------------------------------------------------------------------------- */
/* The statements: the idle tracker's activity                               */
/* ------------------------------------------------------------------------- */

extern bool isProcessActive;	/* idle_tracker.c's */

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* How deep in statements the backend is, the outermost's run or utility */
static int	statement_depth = 0;

/*
 * A statement begins, and the process is active: IdleTracker_ActivateProcess()
 * after a command is read, with the waiver and the peak of the last reset,
 * which PostgresMain() resets as the next command is read.
 */
static void
statement_active(void)
{
	if (!gp_mp_inited || isProcessActive)
		return;
	VmemTracker_ResetMaxVmemReserved();
	VmemTracker_ResetWaiver();
	gp_command_count++;
	IdleTracker_ActivateProcess();
	memprot_shared->backends[MyProcNumber].active = true;
}

/*
 * The outermost statement ended: IdleTracker_DeactivateProcess(), which a
 * flagged runaway's process cancels in, before ReadyForQuery.  After an
 * error it is done as the transaction aborts, where the cleaner cancels
 * nothing (it asks IsTransactionState()).
 */
static void
statement_idle(void)
{
	if (!gp_mp_inited || !isProcessActive)
		return;
	if (GpClusterBackendRole() == GP_ROLE_EXECUTE)
		gp_command_count = 0;
	memprot_shared->backends[MyProcNumber].active = false;
	ReportOOMConsumption();
	IdleTracker_DeactivateProcess();
}

static void
memprot_post_parse_analyze(ParseState *pstate, Query *query,
						   const JumbleState *jstate)
{
	if (statement_depth == 0)
		statement_active();
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);
}

static void
memprot_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (statement_depth == 0)
		statement_active();
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
memprot_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	if (statement_depth == 0)
		statement_active();
	statement_depth++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_CATCH();
	{
		statement_depth--;
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (--statement_depth == 0)
		statement_idle();
}

static void
memprot_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	if (statement_depth == 0)
		statement_active();
	statement_depth++;
	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_CATCH();
	{
		statement_depth--;
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (--statement_depth == 0)
		statement_idle();
}

/*
 * A transaction that ends outside a statement -- one that failed, or a
 * COMMIT of the extended protocol's -- leaves the process idle.  One that
 * failed has cleaned up after its runaway's cancel, if it was one, as
 * PostgresMain()'s error recovery tells the cleaner.
 */
static void
memprot_xact_callback(XactEvent event, void *arg)
{
	if (!gp_mp_inited)
		return;
	if (event == XACT_EVENT_ABORT)
		RunawayCleaner_RunawayCleanupDoneForProcess(false);
	if ((event == XACT_EVENT_ABORT || event == XACT_EVENT_COMMIT ||
		 event == XACT_EVENT_PREPARE) && statement_depth == 0)
		statement_idle();
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_resource_session_state_memory_entries);

/*
 * gp_session_state_memory_entries(): each session's memory on this node, as
 * Cloudberry's gp_internal_tools gives it -- in MB, whether it is a runaway,
 * how many of its processes the node has, how many run a statement, how
 * many have still to clean up after its runaway, and what it held and ran
 * when it was flagged.
 */
Datum
gp_resource_session_state_memory_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			segid = GpCoreApiLookup()->get_content_id();

	/* its columns the caller's, where gp_toolkit's function returns record */
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (AllSessionStateEntries == NULL)
		return (Datum) 0;

	for (int i = 0; i < AllSessionStateEntries->maxSession; i++)
	{
		SessionState state = AllSessionStateEntries->sessions[i];
		Datum		values[10];
		bool		nulls[10] = {false};

		if (!SessionState_IsAcquired(&state))
			continue;
		values[0] = Int32GetDatum(segid);
		values[1] = Int32GetDatum(state.sessionId);
		values[2] = Int32GetDatum(VmemTracker_ConvertVmemChunksToMB(state.sessionVmem));
		values[3] = Int32GetDatum(state.runawayStatus);
		values[4] = Int32GetDatum(state.pinCount);
		values[5] = Int32GetDatum(state.activeProcessCount);
		values[6] = Int32GetDatum(state.cleanupCountdown);
		values[7] = Int32GetDatum(state.sessionVmemRunaway);
		values[8] = Int32GetDatum(state.commandCountRunaway);
		values[9] = TimestampTzGetDatum(state.idle_start);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Loading                                                                   */
/* ------------------------------------------------------------------------- */

/* From gp_resource's _PG_init(), in the postmaster */
void
MemProtInit(void)
{
	define_settings();

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = memprot_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = memprot_shmem_startup;

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = memprot_post_parse_analyze;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = memprot_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = memprot_ExecutorRun;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = memprot_ProcessUtility;
	RegisterXactCallback(memprot_xact_callback, NULL);

	prev_memory_block_alloc_hook = memory_block_alloc_hook;
	memory_block_alloc_hook = memprot_block_hook;
}
