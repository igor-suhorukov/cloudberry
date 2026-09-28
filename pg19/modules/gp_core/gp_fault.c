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
 * gp_fault.c
 *	  Cloudberry's fault injector, for the port's tests.
 *
 * Cloudberry's tests set a fault by name on one node -- make the next
 * transaction that reaches "dtm_broadcast_commit_prepared" on the coordinator
 * wait there, or fail -- and then look at what the others did meanwhile:
 * gp_inject_fault(name, type, dbid).  Its core has some three hundred places
 * that ask (SIMPLE_FAULT_INJECTOR), in PostgreSQL's code as well as its own.
 * The port has the places in its own code, under Cloudberry's names where
 * they stand for the same moment, and asks with GP_FAULT(); and a name that
 * is not one of them is attached as a PostgreSQL 19 injection point too, so
 * that the ones PostgreSQL's code has fire the same way.  A few of
 * Cloudberry's names are for moments PostgreSQL has a point at under a name
 * of its own, and are attached there (fault_points).  The types and the
 * words of the answers are Cloudberry's, which its expected outputs hold.
 *
 * The faults of a node are in its shared memory, and gp_inject_fault() of
 * another node's dbid sets them there over a connection of its own.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/misc/faultinjector.c and
 *	  gpcontrib/gp_inject_fault/gp_inject_fault.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "commands/dbcommands.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_fts.h"

#define GP_FAULT_SLOTS		64
#define GP_FAULT_NAMELEN	64

/*
 * Cloudberry's ERRCODE_FAULT_INJECT, which a fault's error carries -- a
 * PL/pgSQL handler of its tests catches it, "when fault_inject" -- as a
 * fault's FATAL and PANIC do.
 */
#define ERRCODE_GP_FAULT_INJECT	MAKE_SQLSTATE('X','X','0','0','9')

/*
 * The connection gp_inject_fault() of another node's dbid opens there, whose
 * statement fires no fault: Cloudberry's fault handler runs none, and a fault
 * the statement's execution reached -- executor_pre_tuple_processed -- would
 * fail the call that is to reset it.
 */
#define GP_FAULT_APPNAME	"cloudberry fault injector"

/* The session a fault's session is compared with: the backend's own. */
#define FAULT_OWN_SESSION	(-2)

static const char *const fault_type_names[] = {
	"", "sleep", "fatal", "panic", "error", "infinite_loop", "suspend",
	"resume", "skip", "reset", "status", "segv", "interrupt",
	"finish_pending", "wait_until_triggered"
};

#ifdef USE_INJECTION_POINTS
/*
 * Cloudberry's faults inside PostgreSQL's own code whose moment has an
 * injection point of PostgreSQL's, under another name: a commit inside the
 * critical section that makes a checkpoint wait, its commit record not yet
 * written -- a prepared transaction's second phase
 * (RecordTransactionCommitPrepared, PostgreSQL 19's own point), and any
 * other commit (RecordTransactionCommit, the core series' O29).  And the
 * five in its replication code that its FTS tests hold or count, whose
 * points only the tests' build has (pg19/docker/patches): the WAL sender's
 * loop, and the moment in it after the sender has sent what it had --
 * where Cloudberry's has just looked whether it caught up within
 * gp.repl_catchup_within_range, which the port looks at from outside
 * (gp_standby.c, gp_fts.c) -- a standby's flush -- which a "skip" fault
 * skips, the point giving its callback a bool to set -- and a commit's wait
 * for its standby, as it goes on and as a cancel comes.  And the start of a
 * simple query (exec_simple_query()), which a test fails the coordinator
 * at, the tests' build's too.  And three moments of a spill, which segspace
 * and zlib interrupt or fail, the tests' build's as well: a hash join's move
 * to its next batch (ExecHashJoinNewBatch()), a temporary file just made
 * (BufFileCreateTemp()), and a write to one -- as its buffer is written out
 * (BufFileDumpBuffer()), once a block, where Cloudberry's asks at each
 * BufFileWrite().
 *
 * And the points of the tests' build that are Cloudberry's faults under
 * their own names, whose calls in Cloudberry say more than that they came:
 * an autovacuum worker's before it vacuums a database, and its update of
 * the database's row, which name the database, as a test's fault may; a
 * count of rows made past 2^32, which a "skip" asks for; and the top of the
 * checkpointer's loop, where a fault that held the checkpointer in a loop
 * has it checkpoint once let go (CheckpointerMain(), checkpointer.c).  The
 * build's other points under Cloudberry's names -- a database's copy and
 * its drop's replay, a tablespace's directories, a PREPARE's record, the
 * checkpointer's loop's end -- say only that they came, and need no line
 * here.
 */
typedef enum GpPointArg
{
	POINT_ARG_NONE,				/* nothing */
	POINT_ARG_SKIP,				/* a bool *, set for a "skip" */
	POINT_ARG_NAMES,			/* a const char *[2]: the database, the table */
	POINT_ARG_CANCEL,			/* nothing; hit only where a cancel came */
	POINT_ARG_LOOPED,			/* a bool *, set for an "infinite_loop" */
} GpPointArg;

static const struct
{
	const char *fault;
	const char *point;
	GpPointArg	arg;
}			fault_points[] = {
	{"before_xlog_xact_commit_prepared", "commit-after-delay-checkpoint", POINT_ARG_NONE},
	{"onephase_transaction_commit", "transaction-commit-after-delay-checkpoint", POINT_ARG_NONE},
	{"wal_sender_loop", "wal-sender-loop", POINT_ARG_NONE},
	{"wal_sender_after_caughtup_within_range", "wal-sender-after-send", POINT_ARG_NONE},
	{"walrecv_skip_flush", "walrecv-skip-flush", POINT_ARG_SKIP},
	{"sync_rep_query_die", "sync-rep-query-die", POINT_ARG_NONE},
	{"sync_rep_query_cancel", "sync-rep-query-cancel", POINT_ARG_CANCEL},
	{"exec_simple_query_start", "exec-simple-query-start", POINT_ARG_NONE},
	{"exec_hashjoin_new_batch", "exec-hashjoin-new-batch", POINT_ARG_NONE},
	{"workfile_creation_failure", "workfile-creation-failure", POINT_ARG_NONE},
	{"workfile_write_failure", "workfile-write-failure", POINT_ARG_NONE},
	{"auto_vac_worker_before_do_autovacuum", "auto_vac_worker_before_do_autovacuum", POINT_ARG_NAMES},
	{"vacuum_update_dat_frozen_xid", "vacuum_update_dat_frozen_xid", POINT_ARG_NAMES},
	{"executor_run_high_processed", "executor_run_high_processed", POINT_ARG_SKIP},
	{"ckpt_loop_begin", "ckpt_loop_begin", POINT_ARG_LOOPED},
};

/* The injection point a fault is attached to: its own name, or PostgreSQL's. */
static const char *
fault_point_name(const char *fault)
{
	for (int i = 0; i < lengthof(fault_points); i++)
		if (strcmp(fault_points[i].fault, fault) == 0)
			return fault_points[i].point;
	return fault;
}

/* What the fault's point gives its callback. */
static GpPointArg
fault_point_arg(const char *fault)
{
	for (int i = 0; i < lengthof(fault_points); i++)
		if (strcmp(fault_points[i].fault, fault) == 0)
			return fault_points[i].arg;
	return POINT_ARG_NONE;
}
#endif

static const char *const fault_ddl_names[] = {
	"", "create_database", "drop_database", "create_table", "drop_table",
	"create_index", "alter_index", "reindex", "drop_index",
	"create_tablespaces", "drop_tablespaces", "truncate", "vacuum"
};

typedef enum GpFaultState
{
	GP_FAULT_STATE_NOT_INITIALIZED = 0,
	GP_FAULT_STATE_WAITING,
	GP_FAULT_STATE_TRIGGERED,
	GP_FAULT_STATE_COMPLETED,
	GP_FAULT_STATE_FAILED,
} GpFaultState;

static const char *const fault_state_names[] = {
	"not initialized", "set", "triggered", "completed", "failed"
};

typedef struct GpFaultEntry
{
	char		name[GP_FAULT_NAMELEN];	/* "" when the slot is free */
	GpFaultType type;
	int			ddl;
	char		database[NAMEDATALEN];
	char		table[NAMEDATALEN];
	int			start;
	int			end;			/* -1: for ever */
	int			extra;
	int			session;		/* -1: any */
	int			hits;
	GpFaultState state;
	bool		point;			/* attached as an injection point as well */
} GpFaultEntry;

typedef struct GpFaultShared
{
	LWLock	   *lock;
	int			nactive;
	GpFaultEntry faults[GP_FAULT_SLOTS];
} GpFaultShared;

static GpFaultShared *fault_shared = NULL;
volatile int *gp_fault_active = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

static void
fault_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(GpFaultShared)));
	RequestNamedLWLockTranche("gp_core faults", 1);
}

static void
fault_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	fault_shared = ShmemInitStruct("gp_core faults", sizeof(GpFaultShared),
								   &found);
	if (!found)
	{
		memset(fault_shared, 0, sizeof(GpFaultShared));
		fault_shared->lock = &(GetNamedLWLockTranche("gp_core faults"))->lock;
	}
	LWLockRelease(AddinShmemInitLock);
	gp_fault_active = &fault_shared->nactive;
}

static GpFaultEntry *
fault_lookup(const char *name)
{
	for (int i = 0; i < GP_FAULT_SLOTS; i++)
		if (fault_shared->faults[i].name[0] != '\0' &&
			strcmp(fault_shared->faults[i].name, name) == 0)
			return &fault_shared->faults[i];
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Firing                                                                    */
/* ------------------------------------------------------------------------- */

static uint32
fault_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryFault");
	return event;
}

/* Is the fault of that name set, and to what?  Locks, briefly. */
static GpFaultType
fault_current_type(const char *name)
{
	GpFaultEntry *e;
	GpFaultType type = GP_FAULT_NONE;

	LWLockAcquire(fault_shared->lock, LW_SHARED);
	e = fault_lookup(name);
	if (e != NULL)
		type = e->type;
	LWLockRelease(fault_shared->lock);
	return type;
}

static void
fault_log(const char *name, GpFaultType type)
{
	ereport(LOG,
			(errcode(ERRCODE_GP_FAULT_INJECT),
			 errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
					name, fault_type_names[type])));
}

/* Is this backend a fault injector's connection (GP_FAULT_APPNAME)? */
static bool
fault_injector_connection(void)
{
	return MyProcPort != NULL && MyProcPort->application_name != NULL &&
		strcmp(MyProcPort->application_name, GP_FAULT_APPNAME) == 0;
}

static GpFaultType fault_trigger(const char *name, const char *database,
								 const char *table, int session);

GpFaultType
GpFaultTrigger(const char *name, const char *database, const char *table)
{
	return fault_trigger(name, database, table, FAULT_OWN_SESSION);
}

GpFaultType
GpFaultTriggerSession(const char *name, const char *database,
					  const char *table, int session)
{
	return fault_trigger(name, database, table, session);
}

static GpFaultType
fault_trigger(const char *name, const char *database, const char *table,
			  int session)
{
	GpFaultEntry *e;
	GpFaultEntry local;
	GpFaultType type = GP_FAULT_NONE;

	if (fault_shared == NULL || fault_shared->nactive == 0)
		return GP_FAULT_NONE;

	/*
	 * A fault is the process's that runs its part of a statement, as it is
	 * Cloudberry's QE's: a parallel worker of a segment's writer, which reads
	 * a share of a scan for it (gp_parallel.c), fires none, and counts no
	 * hit twice.  Nor does a fault injector's connection.
	 */
	if (IsParallelWorker() || fault_injector_connection())
		return GP_FAULT_NONE;
	memset(&local, 0, sizeof(local));

	LWLockAcquire(fault_shared->lock, LW_EXCLUSIVE);
	e = fault_lookup(name);
	do
	{
		if (e == NULL)
			break;
		if (e->session != -1 &&
			e->session != (session == FAULT_OWN_SESSION ?
						   GpClusterSessionId() : session))
			break;
		if (strcmp(e->database, database) != 0)
			break;
		if (e->table[0] != '\0' && strcmp(e->table, table) != 0)
			break;
		if (e->state == GP_FAULT_STATE_COMPLETED ||
			e->state == GP_FAULT_STATE_FAILED)
			break;
		e->hits++;
		if (e->hits < e->start)
			break;
		e->state = GP_FAULT_STATE_TRIGGERED;
		if (e->end != -1 && e->hits >= e->end)
			e->state = GP_FAULT_STATE_COMPLETED;
		local = *e;
		type = local.type;
	} while (0);
	LWLockRelease(fault_shared->lock);

	if (type == GP_FAULT_NONE)
		return GP_FAULT_NONE;

	switch (type)
	{
		case GP_FAULT_SLEEP:
			fault_log(local.name, type);
			pg_usleep(local.extra * 1000000L);
			break;
		case GP_FAULT_FATAL:
			ereport(FATAL,
					(errcode(ERRCODE_GP_FAULT_INJECT),
					 errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							local.name, fault_type_names[type])));
			break;
		case GP_FAULT_PANIC:
			ereport(PANIC,
					(errcode(ERRCODE_GP_FAULT_INJECT),
					 errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							local.name, fault_type_names[type])));
			break;
		case GP_FAULT_ERROR:
			ereport(ERROR,
					(errcode(ERRCODE_GP_FAULT_INJECT),
					 errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							local.name, fault_type_names[type])));
			break;
		case GP_FAULT_INFINITE_LOOP:
			fault_log(local.name, type);
			for (int i = 0; i < 3600 && fault_current_type(local.name) != GP_FAULT_NONE; i++)
			{
				pg_usleep(1000000L);
				CHECK_FOR_INTERRUPTS();
			}
			break;
		case GP_FAULT_SUSPEND:
			{
				GpFaultType now;

				/*
				 * Held until the fault is resumed or reset, looked at once a
				 * second, as Cloudberry's suspend looks at it
				 * (FaultInjector_InjectFaultIfSet(), pg_usleep(1000000L)) --
				 * and no more often, since a test may reset a fault and at once
				 * suspend at it again for another session, and a look that
				 * falls between the two lets this one go on
				 * (startup_rename_prepared_xlog).  An interrupt is taken at
				 * once, as the latch wakes the wait.  It is no wait event, as
				 * Cloudberry's pg_usleep() is none: a test finds a suspended
				 * backend among those whose wait_event_type is null
				 * (resgroup_bypass).
				 */
				fault_log(local.name, type);
				while ((now = fault_current_type(local.name)) != GP_FAULT_NONE &&
					   now != GP_FAULT_RESUME)
				{
					CHECK_FOR_INTERRUPTS();
					(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									 1000L, 0);
					ResetLatch(MyLatch);
				}
				if (now == GP_FAULT_RESUME)
					fault_log(local.name, now);
				break;
			}
		case GP_FAULT_SKIP:
			fault_log(local.name, type);
			break;
		case GP_FAULT_SEGV:
			*(volatile int *) 0 = 1234;
			break;
		case GP_FAULT_INTERRUPT:
			fault_log(local.name, type);
			InterruptPending = true;
			QueryCancelPending = true;
			break;
		default:
			fault_log(local.name, type);
			break;
	}
	return type;
}

/*
 * What an injection point runs, when one of PostgreSQL's own is set by the
 * name of a fault: the fault its private data names.  A point that is
 * skipped cannot say so to its caller, PostgreSQL's points having no answer,
 * but for one that gives its callback a bool to set (fault_points), as one
 * that held the caller in a loop says so to the checkpointer's.  It may
 * run in a critical section, as the two commits' of fault_points do, where
 * it allocates nothing but its log line, which the error context may.
 *
 * The point of a commit's wait for its standby that Cloudberry's
 * sync_rep_query_cancel is, reached each time the wait wakes, is a hit only
 * where a cancel came: pending, or kept by gp_core for the commit's end
 * (gp_fts.c), which is where PostgreSQL's cancel would have ended the wait
 * and Cloudberry's fault is.
 */
PGDLLEXPORT void gp_fault_injection_point(const char *name,
										  const void *private_data, void *arg);

void
gp_fault_injection_point(const char *name, const void *private_data,
						 void *arg)
{
	const char *fault = private_data != NULL ? (const char *) private_data : name;
	const char *database = "";
	const char *table = "";
	GpFaultType answer = GP_FAULT_NONE;
	GpFaultType type;

#ifdef USE_INJECTION_POINTS
	switch (fault_point_arg(fault))
	{
		case POINT_ARG_SKIP:
			if (arg != NULL)
				answer = GP_FAULT_SKIP;
			break;
		case POINT_ARG_LOOPED:
			if (arg != NULL)
				answer = GP_FAULT_INFINITE_LOOP;
			break;
		case POINT_ARG_NAMES:
			if (arg != NULL)
			{
				database = ((const char *const *) arg)[0];
				table = ((const char *const *) arg)[1];
			}
			break;
		case POINT_ARG_CANCEL:
			if (!QueryCancelPending && !GpFtsCancelKept())
				return;
			break;
		case POINT_ARG_NONE:
			break;
	}
#endif

	type = GpFaultTrigger(fault, database, table);
	if (answer != GP_FAULT_NONE && type == answer)
		*(bool *) arg = true;
}

/* ------------------------------------------------------------------------- */
/* Setting                                                                   */
/* ------------------------------------------------------------------------- */

static GpFaultType
fault_type_from_name(const char *name)
{
	for (int i = 1; i < lengthof(fault_type_names); i++)
		if (strcmp(fault_type_names[i], name) == 0)
			return (GpFaultType) i;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not recognize fault type '%s'", name)));
	return GP_FAULT_NONE;
}

static int
fault_ddl_from_name(const char *name)
{
	for (int i = 0; i < lengthof(fault_ddl_names); i++)
		if (strcmp(fault_ddl_names[i], name) == 0)
			return i;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not recognize DDL statement '%s'", name)));
	return 0;
}

static void
fault_detach_point(GpFaultEntry *e)
{
#ifdef USE_INJECTION_POINTS
	if (e->point)
		(void) InjectionPointDetach(fault_point_name(e->name));
#endif
	e->point = false;
}

/* The fault set, reset or asked about here; Cloudberry's words for it. */
static char *
fault_inject_here(const char *name, const char *typename, const char *ddl,
				  const char *database, const char *table, int start,
				  int end, int extra, int session)
{
	GpFaultType type = fault_type_from_name(typename);
	GpFaultEntry *e;

	if (strlen(name) >= GP_FAULT_NAMELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fault name too long: '%s'", name)));
	if (strlen(database) >= NAMEDATALEN || strlen(table) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("database or table name too long")));

	switch (type)
	{
		case GP_FAULT_RESET:
			LWLockAcquire(fault_shared->lock, LW_EXCLUSIVE);
			for (int i = 0; i < GP_FAULT_SLOTS; i++)
			{
				e = &fault_shared->faults[i];
				if (e->name[0] == '\0' ||
					(strcmp(name, "all") != 0 && strcmp(e->name, name) != 0))
					continue;
				fault_detach_point(e);
				memset(e, 0, sizeof(GpFaultEntry));
				fault_shared->nactive--;
			}
			LWLockRelease(fault_shared->lock);
			return pstrdup("Success:");

		case GP_FAULT_RESUME:
			LWLockAcquire(fault_shared->lock, LW_EXCLUSIVE);
			e = fault_lookup(name);
			if (e == NULL || e->type != GP_FAULT_SUSPEND)
			{
				LWLockRelease(fault_shared->lock);
				return psprintf("Failure: fault name:'%s' is not suspended", name);
			}
			e->type = GP_FAULT_RESUME;
			LWLockRelease(fault_shared->lock);
			fault_log(name, type);
			return pstrdup("Success:");

		case GP_FAULT_STATUS:
			{
				char	   *answer;

				LWLockAcquire(fault_shared->lock, LW_SHARED);
				e = fault_lookup(name);
				if (e == NULL)
					answer = psprintf("Failure: fault name:'%s' not set", name);
				else
					answer = psprintf("Success: fault name:'%s' fault type:'%s' "
									  "ddl statement:'%s' database name:'%s' "
									  "table name:'%s' start occurrence:'%d' "
									  "end occurrence:'%d' extra arg:'%d' "
									  "fault injection state:'%s'  "
									  "num times hit:'%d' \n",
									  e->name, fault_type_names[e->type],
									  fault_ddl_names[e->ddl], e->database,
									  e->table, e->start, e->end, e->extra,
									  fault_state_names[e->state], e->hits);
				LWLockRelease(fault_shared->lock);
				return answer;
			}

		case GP_FAULT_WAIT_UNTIL_TRIGGERED:
			for (int tries = 0;; tries++)
			{
				bool		set;
				bool		done;
				int			hits = 0;

				LWLockAcquire(fault_shared->lock, LW_SHARED);
				e = fault_lookup(name);
				set = e != NULL;
				done = set && (e->state == GP_FAULT_STATE_COMPLETED ||
							   e->hits - e->start >= extra - 1);
				if (set)
					hits = e->hits;
				LWLockRelease(fault_shared->lock);

				if (!set)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("fault not set, fault name:'%s'  ", name)));
				if (done)
				{
					ereport(LOG,
							(errmsg("fault triggered %d times, fault name:'%s' fault type:'%s' ",
									hits, name, typename)));
					return pstrdup("Success:");
				}
				if (tries >= 3000)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("fault not triggered, fault name:'%s' fault type:'%s' ",
									name, typename),
							 errdetail("Timed-out as 10 minutes max wait happens until triggered.")));
				CHECK_FOR_INTERRUPTS();
				pg_usleep(200000L);
			}

		case GP_FAULT_NONE:
			break;

		default:
			{
				GpFaultEntry *free_slot = NULL;

				LWLockAcquire(fault_shared->lock, LW_EXCLUSIVE);
				if (fault_lookup(name) != NULL)
				{
					LWLockRelease(fault_shared->lock);
					ereport(WARNING,
							(errmsg("cannot insert fault injection entry into table, entry already exists"),
							 errdetail("Fault name:'%s' fault type:'%s' ", name, typename)));
					return pstrdup("Failure: could not insert fault injection, entry already exists");
				}
				for (int i = 0; i < GP_FAULT_SLOTS && free_slot == NULL; i++)
					if (fault_shared->faults[i].name[0] == '\0')
						free_slot = &fault_shared->faults[i];
				if (free_slot == NULL)
				{
					LWLockRelease(fault_shared->lock);
					return psprintf("Failure: could not insert fault injection, max slots:'%d' reached",
									GP_FAULT_SLOTS);
				}
				e = free_slot;
				memset(e, 0, sizeof(GpFaultEntry));
				strlcpy(e->name, name, GP_FAULT_NAMELEN);
				e->type = type;
				e->ddl = fault_ddl_from_name(ddl);
				strlcpy(e->database, database, NAMEDATALEN);
				strlcpy(e->table, table, NAMEDATALEN);
				e->start = start;
				e->end = end;
				e->extra = extra;
				e->session = session;
				e->state = GP_FAULT_STATE_WAITING;
				fault_shared->nactive++;
				LWLockRelease(fault_shared->lock);

#ifdef USE_INJECTION_POINTS
				/*
				 * PostgreSQL's own point of that name, if there is one, or
				 * the one of its own name for that moment; the callback is
				 * given the fault's.
				 */
				PG_TRY();
				{
					InjectionPointAttach(fault_point_name(name), "gp_core",
										 "gp_fault_injection_point",
										 name, strlen(name) + 1);
					LWLockAcquire(fault_shared->lock, LW_EXCLUSIVE);
					e = fault_lookup(name);
					if (e != NULL)
						e->point = true;
					LWLockRelease(fault_shared->lock);
				}
				PG_CATCH();
				{
					/* already attached by somebody else: theirs, then */
					FlushErrorState();
				}
				PG_END_TRY();
#endif
				return pstrdup("Success:");
			}
	}
	return pstrdup("Success:");
}

/* Is FTS's probe of this node skipped, by its own fts_probe fault? */
static bool
fts_probe_skipped_here(void)
{
	GpFaultEntry *e;
	bool		skipped;

	if (fault_shared == NULL)
		return false;
	LWLockAcquire(fault_shared->lock, LW_SHARED);
	e = fault_lookup("fts_probe");
	skipped = (e != NULL && e->type == GP_FAULT_SKIP);
	LWLockRelease(fault_shared->lock);
	return skipped;
}

PG_FUNCTION_INFO_V1(gp_inject_fault);

/*
 * gp_inject_fault(faultname, type, ddl, database, tablename,
 *				   start_occurrence, end_occurrence, extra_arg, db_id,
 *				   gp_session_id)
 *		Set, reset or ask about a fault on the node of that dbid.
 */
Datum
gp_inject_fault(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *type = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *ddl = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *database = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *table = text_to_cstring(PG_GETARG_TEXT_PP(4));
	int			start = PG_GETARG_INT32(5);
	int			end = PG_GETARG_INT32(6);
	int			extra = PG_GETARG_INT32(7);
	int			dbid = PG_GETARG_INT32(8);
	int			session = PG_GETARG_INT32(9);
	const GpSegmentConfig *node;
	char	   *answer;
	bool		fts_skipped = false;

	/*
	 * Who may is who may EXECUTE it: the extension's script grants PUBLIC
	 * none, so a superuser, until someone grants a role -- as a test
	 * cluster's setup grants PUBLIC, which Cloudberry's tests assume.
	 */
	node = GpClusterNodeByDbid(dbid);
	if (GpClusterIsSingleNode() || GpClusterDbid() == dbid || node == NULL)
	{
		if (!GpClusterIsSingleNode() && node == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("there is no node with dbid %d", dbid)));
		answer = fault_inject_here(name, type, ddl, database, table, start,
								   end, extra, session);
		fts_skipped = fts_probe_skipped_here();
	}
	else
	{
		const char *keywords[5 + GP_INTERNAL_CONN_OPTIONS];
		const char *values[5 + GP_INTERNAL_CONN_OPTIONS];
		char		portbuf[16];
		const char *params[10];
		int			n = 0;
		PGconn	   *conn;
		PGresult   *res;

		snprintf(portbuf, sizeof(portbuf), "%d", node->port);
		keywords[n] = "host";
		values[n++] = node->hostname;
		keywords[n] = "port";
		values[n++] = portbuf;
		keywords[n] = "dbname";
		values[n++] = get_database_name(MyDatabaseId);
		keywords[n] = "user";
		values[n++] = GetUserNameFromId(GetUserId(), false);
		keywords[n] = "application_name";
		values[n++] = GP_FAULT_APPNAME;
		n = GpInternalConnOptions(keywords, values, n);

		params[0] = name;
		params[1] = type;
		params[2] = ddl;
		params[3] = database;
		params[4] = table;
		params[5] = psprintf("%d", start);
		params[6] = psprintf("%d", end);
		params[7] = psprintf("%d", extra);
		params[8] = psprintf("%d", dbid);
		params[9] = psprintf("%d", session);

		/*
		 * A node restarting -- after a panic a fault of this function's made
		 * -- refuses the connection, or closes it as its postmaster ends the
		 * backend it had started, before the fault's query is answered: tried
		 * again, five times, two seconds apart, as the dispatcher tries a
		 * segment in recovery (gp_dispatch.c).  The restart forgot every
		 * fault, so asking again sets or resets this one once.
		 */
		for (int attempt = 0;; attempt++)
		{
			char	   *msg;
			bool		connected;

			conn = libpqsrv_connect_params(keywords, values, false, fault_wait_event());
			connected = (conn != NULL && PQstatus(conn) == CONNECTION_OK);
			if (connected)
			{
				res = libpqsrv_exec_params(conn,
										   "SELECT gp_inject_fault($1, $2, $3, $4, $5, $6::int4, $7::int4, $8::int4, $9::int4, $10::int4)",
										   10, NULL, params, NULL, NULL, 0,
										   fault_wait_event());
				if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
					break;
				PQclear(res);
			}
			msg = conn ? pstrdup(PQerrorMessage(conn)) : "out of memory";
			if (conn != NULL)
				libpqsrv_disconnect(conn);
			if (attempt < 5 &&
				(strstr(msg, "the database system is starting up") != NULL ||
				 strstr(msg, "the database system is in recovery mode") != NULL ||
				 strstr(msg, "the database system is not yet accepting connections") != NULL ||
				 strstr(msg, "server closed the connection unexpectedly") != NULL))
			{
				(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 2000, fault_wait_event());
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
				continue;
			}
			if (!connected)
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection to dbid %d %s:%d failed", dbid,
								node->hostname, node->port),
						 errdetail_internal("%s", msg)));
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to inject fault: %s", msg)));
		}
		answer = pstrdup(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* whether the node's own fts_probe fault skips its probes */
		if (strcmp(type, "panic") == 0)
		{
			const char *status_params[2];

			status_params[0] = psprintf("%d", dbid);
			res = libpqsrv_exec_params(conn,
									   "SELECT gp_inject_fault('fts_probe', 'status', '', '', '', 1, -1, 0, $1::int4, -1)",
									   1, NULL, status_params, NULL, NULL, 0,
									   fault_wait_event());
			fts_skipped = (PQresultStatus(res) == PGRES_TUPLES_OK &&
						   PQntuples(res) == 1 &&
						   strstr(PQgetvalue(res, 0, 0), "fault type:'skip'") != NULL);
			PQclear(res);
		}
		libpqsrv_disconnect(conn);
	}

	if (strncmp(answer, "Success:", strlen("Success:")) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("%s", answer)));

	/*
	 * A panic on a segment, which FTS may take for the segment down and fail
	 * over from: Cloudberry's gp_inject_fault warns where FTS probes
	 * (fts_with_panic_warning()), as the node's own fts_probe fault does not
	 * skip them.
	 */
	if (strcmp(type, "panic") == 0 && node != NULL && node->content >= 0 &&
		!fts_skipped)
		ereport(WARNING,
				(errmsg("consider disabling FTS probes while injecting a panic."),
				 errhint("Inject an infinite 'skip' into the 'fts_probe' fault to disable FTS probing.")));
	PG_RETURN_TEXT_P(cstring_to_text(answer));
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpFaultInit(void)
{
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = fault_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = fault_shmem_startup;
}
