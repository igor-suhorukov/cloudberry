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
 * resqueue.c
 *	  Resource queues at run time.
 *
 * A query of a role that is not a superuser takes a slot of its role's queue
 * on the coordinator, as Cloudberry's ResLockPortal() takes one, and waits
 * while the queue has none for it: its count of statements, the sum of
 * their plans' costs and of their memory are each held under the queue's
 * limit.  A slot is a portal's, and is given back as the portal is cleaned
 * up -- a statement's once it is done, a cursor's as it is closed, a WITH
 * HOLD cursor's past the transaction's end -- which is where Cloudberry's
 * PortalCleanup() gives it back.
 *
 * What is Cloudberry's here: which statements take a slot and what they
 * count (a SELECT, INSERT, UPDATE and DELETE, DECLARE CURSOR, a SELECT a
 * DO or CALL runs through SPI, COPY and CREATE TABLE AS; not MERGE); the
 * limits' checks, the cost a statement is not queued under, overcommit, the
 * self-deadlock a backend's own portals make, and the words of each error;
 * portal ids; the waiters woken in order as a slot comes free, every one
 * that now fits, and none by ALTER RESOURCE QUEUE; the memory a statement
 * gets; and the statistics, as the stats kind Cloudberry keeps them in.
 *
 * What is not: Cloudberry's queue is a lock of a lock method of its own,
 * which pg_locks lists and PostgreSQL's deadlock detector sees.  PostgreSQL
 * 19's lock methods are fixed, so a wait here is on the backend's latch, in
 * a queue in this module's shared memory, and the detector cannot see it:
 * so a waiter looks for a cycle itself, every deadlock_timeout while it
 * waits, through the queues' holders and the heavyweight locks they wait
 * for, and reports it in PostgreSQL's words, the queue named as
 * Cloudberry's lock of it is.  pg_locks has no row of a queue, and the wait
 * is an Extension wait named ResourceQueue.  Cloudberry's priorities are
 * recorded and reported, and slow nobody: its backoff sleeps in
 * CHECK_FOR_INTERRUPTS(), which PostgreSQL 19's macro gives no way to reach.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/utils/resscheduler/resscheduler.c, resqueue.c,
 *	  utils/activity/pgstat_resqueue.c, and the queue code of pquery.c,
 *	  portalmem.c and proc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/pquery.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp_core_api.h"
#include "gp_fault.h"
#include "gp_resource.h"

#define RES_COUNT_LIMIT		0
#define RES_COST_LIMIT		1
#define RES_MEMORY_LIMIT	2
#define NUM_RES_LIMIT_TYPES	3

#define INVALID_PORTALID	((uint32) -1)

/* The statistics kind of the queues, one of PostgreSQL 19's for extensions */
#define PGSTAT_KIND_RESQUEUE	25

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

/* A queue: its limits and what its slots hold */
typedef struct ResQueueEntry
{
	Oid			queueid;		/* InvalidOid: the entry is free */
	bool		defined;		/* its definition is there still */
	double		threshold[NUM_RES_LIMIT_TYPES];	/* -1: no limit */
	double		current[NUM_RES_LIMIT_TYPES];
	bool		overcommit;
	double		ignorecostlimit;
	int			nrequested;		/* increments granted or waited for */
	int			ngranted;
	int			waithead;		/* procno of the first waiter, -1: none */
	int			waittail;
} ResQueueEntry;

/* A portal's increment: its slot, granted or waited for */
typedef struct ResPortalInc
{
	Oid			queueid;		/* InvalidOid: the entry is free */
	uint32		portalid;
	double		inc[NUM_RES_LIMIT_TYPES];
	bool		granted;
	bool		ishold;
} ResPortalInc;

/* A backend's increments, and where it waits */
typedef struct ResQueueProc
{
	int			pid;
	int			next;			/* the next waiter of its queue, -1: none */
	int			waitinc;		/* the increment it waits for, -1: none */
	TimestampTz waitstart;
	ResPortalInc incs[FLEXIBLE_ARRAY_MEMBER];
} ResQueueProc;

typedef struct ResQueueControl
{
	bool		loaded;			/* the definitions are read in */
	ResQueueEntry queues[FLEXIBLE_ARRAY_MEMBER];
} ResQueueControl;

static ResQueueControl *rq_ctl = NULL;
static char *rq_procs = NULL;
static LWLock *rq_lock = NULL;
static uint32 rq_wait_event = 0;

#define RQ_TRANCHE		"gp_resource queues"

static Size
proc_stride(void)
{
	return MAXALIGN(offsetof(ResQueueProc, incs) +
					sizeof(ResPortalInc) * gp_max_resource_portals_per_transaction);
}

static ResQueueProc *
proc_at(int procno)
{
	return (ResQueueProc *) (rq_procs + proc_stride() * procno);
}

static ResQueueProc *
my_proc(void)
{
	return proc_at(MyProcNumber);
}

Size
ResQueueShmemSize(void)
{
	Size		size;

	size = add_size(offsetof(ResQueueControl, queues),
					mul_size(sizeof(ResQueueEntry), gp_max_resource_queues));
	size = MAXALIGN(size);
	size = add_size(size, mul_size(proc_stride(), MaxBackends));
	return size;
}

void
ResQueueShmemRequest(void)
{
	RequestNamedLWLockTranche(RQ_TRANCHE, 1);
}

void
ResQueueShmemInit(void)
{
	bool		found;
	Size		ctlsize;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	ctlsize = MAXALIGN(add_size(offsetof(ResQueueControl, queues),
								mul_size(sizeof(ResQueueEntry), gp_max_resource_queues)));
	rq_ctl = ShmemInitStruct("gp_resource queues", ResQueueShmemSize(), &found);
	rq_procs = ((char *) rq_ctl) + ctlsize;
	if (!found)
	{
		memset(rq_ctl, 0, ResQueueShmemSize());
		for (int i = 0; i < MaxBackends; i++)
		{
			ResQueueProc *p = proc_at(i);

			p->next = -1;
			p->waitinc = -1;
		}
	}
	LWLockRelease(AddinShmemInitLock);
	rq_lock = &(GetNamedLWLockTranche(RQ_TRANCHE))->lock;
}

static ResQueueEntry *
find_queue(Oid queueid)
{
	for (int i = 0; i < gp_max_resource_queues; i++)
	{
		if (rq_ctl->queues[i].queueid == queueid)
			return &rq_ctl->queues[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* The definitions, into shared memory                                       */
/* ------------------------------------------------------------------------- */

/*
 * A queue's memory limit in bytes, -1 for none: ResourceQueueGetMemoryLimit(),
 * which answers -1 in a backend whose policy is none -- and the backend that
 * loads a queue, or alters it, is whose policy counts, as in Cloudberry.
 */
static double
memory_threshold(const ResQueueDef *def)
{
	int64		kb;

	if (gp_resqueue_memory_policy == RESMANAGER_MEMORY_POLICY_NONE)
		return -1;
	kb = ResQueueDefMemoryLimitKB(def);
	return kb < 0 ? -1 : (double) kb * 1024.0;
}

static void
set_thresholds(ResQueueEntry *q, const ResQueueDef *def)
{
	q->threshold[RES_COUNT_LIMIT] = def->active_statements;
	q->threshold[RES_COST_LIMIT] = def->max_cost;
	q->threshold[RES_MEMORY_LIMIT] = memory_threshold(def);
	q->overcommit = def->cost_overcommit;
	q->ignorecostlimit = def->min_cost;
}

/*
 * Bring shared memory in step with the definitions: a queue made is added,
 * one altered takes its new limits -- waking nobody, as ALTER RESOURCE QUEUE
 * does not -- and one dropped goes once nothing holds it.  The definitions
 * are read before the lock is taken, as reading them may fail.
 */
static void
sync_queues(List *defs)
{
	LWLockAcquire(rq_lock, LW_EXCLUSIVE);
	for (int i = 0; i < gp_max_resource_queues; i++)
		rq_ctl->queues[i].defined = false;
	foreach_ptr(ResQueueDef, def, defs)
	{
		ResQueueEntry *q = find_queue(def->oid);

		if (q == NULL)
		{
			q = find_queue(InvalidOid);
			if (q == NULL)
			{
				elog(LOG, "gp_resource: no room for resource queue \"%s\" in shared memory: max_resource_queues is %d",
					 def->name, gp_max_resource_queues);
				continue;
			}
			memset(q, 0, sizeof(*q));
			q->queueid = def->oid;
			q->waithead = q->waittail = -1;
		}
		set_thresholds(q, def);
		q->defined = true;
	}
	for (int i = 0; i < gp_max_resource_queues; i++)
	{
		ResQueueEntry *q = &rq_ctl->queues[i];

		if (OidIsValid(q->queueid) && !q->defined && q->nrequested == 0)
			q->queueid = InvalidOid;
	}
	rq_ctl->loaded = true;
	LWLockRelease(rq_lock);
}

/* The first backend to need the queues reads them in */
static void
ensure_loaded(void)
{
	if (rq_ctl->loaded)
		return;
	sync_queues(ResQueueDefsLoad());
}

/* What this transaction's statements did to the definitions, for its commit */
static bool defs_changed = false;
static List *pending_defs = NIL;

void
ResQueueDefsChanged(void)
{
	defs_changed = true;
}

/* CREATE RESOURCE QUEUE: is there room for one more? */
bool
ResQueueCanCreate(void)
{
	int			used = 0;

	ensure_loaded();
	LWLockAcquire(rq_lock, LW_SHARED);
	for (int i = 0; i < gp_max_resource_queues; i++)
	{
		if (OidIsValid(rq_ctl->queues[i].queueid))
			used++;
	}
	LWLockRelease(rq_lock);
	return used < gp_max_resource_queues;
}

/* DROP RESOURCE QUEUE: ResDestroyQueue() refuses a queue a slot is held of */
void
ResQueueCheckDrop(Oid queueid)
{
	ResQueueEntry *q;
	bool		inuse = false;

	ensure_loaded();
	LWLockAcquire(rq_lock, LW_SHARED);
	q = find_queue(queueid);
	if (q != NULL)
	{
		for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
		{
			if (q->current[i] != 0)
				inuse = true;
		}
		if (q->nrequested > 0)
			inuse = true;
	}
	LWLockRelease(rq_lock);
	if (inuse)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("resource queue cannot be dropped as is in use")));
}

/*
 * ALTER RESOURCE QUEUE: ResAlterQueue() refuses to take overcommit away from
 * a queue whose cost is over its limit already.
 */
void
ResQueueCheckAlter(Oid queueid, const ResQueueDef *def)
{
	ResQueueEntry *q;
	bool		overcommitted = false;

	ensure_loaded();
	LWLockAcquire(rq_lock, LW_SHARED);
	q = find_queue(queueid);
	if (q != NULL && q->overcommit && !def->cost_overcommit &&
		def->max_cost != INVALID_RES_LIMIT_THRESHOLD &&
		q->current[RES_COST_LIMIT] > def->max_cost)
		overcommitted = true;
	LWLockRelease(rq_lock);
	if (overcommitted)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_LIMIT_VALUE),
				 errmsg("disabling overcommit cannot leave queue in possibly overcommitted state")));
}

/* A queue's memory limit in kB, for the budget of a query; -1 for none */
int64
ResQueueGetMemoryKB(Oid queueid)
{
	ResQueueDef *def = ResQueueDefByOid(ResQueueDefsLoad(), queueid);

	return def != NULL ? ResQueueDefMemoryLimitKB(def) : -1;
}

/* ------------------------------------------------------------------------- */
/* Statistics: Cloudberry's pgstat_resqueue.c, as a kind of an extension's   */
/* ------------------------------------------------------------------------- */

typedef struct ResQueueStats
{
	PgStat_Counter queries_submitted;
	PgStat_Counter queries_admitted;
	PgStat_Counter queries_rejected;
	PgStat_Counter queries_completed;
	PgStat_Counter elapsed_wait_secs;
	PgStat_Counter max_wait_secs;
	PgStat_Counter elapsed_exec_secs;
	PgStat_Counter max_exec_secs;
	PgStat_Counter total_cost;
	PgStat_Counter total_memory_kb;
	TimestampTz stat_reset_timestamp;
} ResQueueStats;

typedef struct PgStatShared_ResQueue
{
	PgStatShared_Common header;
	ResQueueStats stats;
} PgStatShared_ResQueue;

static void
resqueue_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_ResQueue *) header)->stats.stat_reset_timestamp = ts;
}

static const PgStat_KindInfo resqueue_stats_kind = {
	.name = "gp_resource_queue",
	.fixed_amount = false,
	.accessed_across_databases = true,
	.write_to_file = true,
	.shared_size = sizeof(PgStatShared_ResQueue),
	.shared_data_off = offsetof(PgStatShared_ResQueue, stats),
	.shared_data_len = sizeof(((PgStatShared_ResQueue *) 0)->stats),
	.pending_size = 0,
	.reset_timestamp_cb = resqueue_reset_timestamp_cb,
};

/* A portal's times, for the statistics, as pgstat_resqueue.c keeps them */
typedef struct PortalStats
{
	Oid			queueid;
	time_t		wait_start;
	time_t		exec_start;
} PortalStats;

/*
 * The shared entry, updated in place with its lock held, as Cloudberry's is:
 * safe in an error's cleanup, and seen by other sessions at once.
 */
static ResQueueStats *
stats_lock(Oid queueid, PgStat_EntryRef **ref)
{
	if (pgStatLocal.shared_hash == NULL)
		return NULL;
	*ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_RESQUEUE, InvalidOid,
									   queueid, false);
	if (*ref == NULL)
		return NULL;
	return &((PgStatShared_ResQueue *) (*ref)->shared_stats)->stats;
}

static void
stats_submitted(Oid queueid, double cost, double memory)
{
	PgStat_EntryRef *ref;
	ResQueueStats *st = stats_lock(queueid, &ref);

	if (st == NULL)
		return;
	st->queries_submitted++;
	st->total_cost += (PgStat_Counter) cost;
	st->total_memory_kb += (PgStat_Counter) (memory / 1024);
	pgstat_unlock_entry(ref);
}

static void
stats_waited(Oid queueid, time_t wait_start, bool admitted)
{
	PgStat_EntryRef *ref;
	ResQueueStats *st = stats_lock(queueid, &ref);
	time_t		secs = Max(time(NULL) - wait_start, 0);

	if (st == NULL)
		return;
	if (admitted)
		st->queries_admitted++;
	else
		st->queries_rejected++;
	st->elapsed_wait_secs += secs;
	if (secs > st->max_wait_secs)
		st->max_wait_secs = secs;
	pgstat_unlock_entry(ref);
}

static void
stats_completed(Oid queueid, time_t exec_start)
{
	PgStat_EntryRef *ref;
	ResQueueStats *st = stats_lock(queueid, &ref);
	time_t		secs = exec_start > 0 ? Max(time(NULL) - exec_start, 0) : 0;

	if (st == NULL)
		return;
	st->queries_completed++;
	st->elapsed_exec_secs += secs;
	if (secs > st->max_exec_secs)
		st->max_exec_secs = secs;
	pgstat_unlock_entry(ref);
}

/* ------------------------------------------------------------------------- */
/* A backend's portals                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The portals of this backend that took a slot, or tried to: the portal, its
 * increment's place in shared memory, and the cleanup it had.
 */
typedef struct PortalSlot
{
	Portal		portal;
	int			inc;			/* in my_proc()->incs, -1: none held */
	uint32		portalid;
	bool		ishold;
	void		(*prev_cleanup) (Portal portal);
	PortalStats stats;
} PortalSlot;

static List *portal_slots = NIL;
static uint32 portal_id_counter = 0;
static int	num_hold_portals = 0;

/* The role whose queue a query takes a slot of, and that queue */
static Oid	cached_user = InvalidOid;
static Oid	cached_queue = InvalidOid;

/* The list outlives every query, so its cells are TopMemoryContext's */
static void
remember_slot(PortalSlot *slot)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	portal_slots = lappend(portal_slots, slot);
	MemoryContextSwitchTo(oldcxt);
}

static PortalSlot *
find_slot(Portal portal)
{
	foreach_ptr(PortalSlot, s, portal_slots)
	{
		if (s->portal == portal)
			return s;
	}
	return NULL;
}

static Oid
current_queue(void)
{
	Oid			user = GetUserId();

	if (user != cached_user)
	{
		cached_queue = GetResQueueForRole(user);
		cached_user = user;
	}
	return cached_queue;
}

void
ResQueueRoleChanged(Oid roleid)
{
	if (roleid == cached_user)
		cached_user = InvalidOid;
}

/*
 * ResCreatePortalId(): 0 for the unnamed portal, and for a named one the
 * lowest id no live increment of this backend has, or the next.
 */
static uint32
create_portal_id(const char *name)
{
	ResQueueProc *me = my_proc();

	if (name[0] == '\0')
		return 0;

	for (uint32 id = 1; id <= portal_id_counter; id++)
	{
		bool		used = false;

		for (int i = 0; i < gp_max_resource_portals_per_transaction; i++)
		{
			if (OidIsValid(me->incs[i].queueid) && me->incs[i].portalid == id)
				used = true;
		}
		if (!used)
			return id;
	}

	if (portal_id_counter >= (uint32) gp_max_resource_portals_per_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient portal ids available"),
				 errhint("Increase max_resource_portals_per_transaction.")));
	return ++portal_id_counter;
}

/* ------------------------------------------------------------------------- */
/* Taking a slot                                                             */
/* ------------------------------------------------------------------------- */

typedef enum LimitCheck
{
	LIMIT_CHECK_OK,
	LIMIT_CHECK_FOUND,			/* the queue has no room yet: wait */
	LIMIT_CHECK_ERROR			/* more than the queue will ever allow */
} LimitCheck;

/* ResLockCheckLimit(), with the lock held */
static LimitCheck
check_limit(const ResQueueEntry *q, const double *inc)
{
	bool		over_limit = false;
	bool		will_overcommit = false;

	for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		if (q->threshold[i] == INVALID_RES_LIMIT_THRESHOLD)
			continue;
		switch (i)
		{
			case RES_COUNT_LIMIT:
			case RES_MEMORY_LIMIT:
				if (q->current[i] + inc[i] > q->threshold[i])
					over_limit = true;
				break;
			case RES_COST_LIMIT:
				if (inc[i] > q->threshold[i])
					will_overcommit = true;
				if (q->overcommit)
				{
					/* one statement over the limit runs alone */
					if (q->current[i] + inc[i] > q->threshold[i] &&
						q->current[i] > 0.1)
						over_limit = true;
				}
				else if (q->current[i] + inc[i] > q->threshold[i])
					over_limit = true;
				break;
		}
	}

	if (will_overcommit && !q->overcommit)
		return LIMIT_CHECK_ERROR;
	if (over_limit)
		return LIMIT_CHECK_FOUND;
	return LIMIT_CHECK_OK;
}

/* ResLockUpdateLimit(): counted up or down, whole, never below 0 */
static void
update_limit(ResQueueEntry *q, const double *inc, bool increment)
{
	for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		double		v = ceil(q->current[i] + (increment ? inc[i] : -inc[i]));

		q->current[i] = Max(v, 0.0);
	}
}

/*
 * ResCheckSelfDeadLock(): would this backend's own portals on the queue,
 * with the one asking, be more than the queue allows?  Then it would wait
 * for itself.  An overcommitted queue lets one portal over its cost.
 */
static bool
self_deadlock(const ResQueueEntry *q, const double *inc)
{
	ResQueueProc *me = my_proc();
	double		totals[NUM_RES_LIMIT_TYPES];
	int			nportals = 1;
	bool		count_over = false;
	bool		over = false;

	for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
		totals[i] = inc[i];
	for (int k = 0; k < gp_max_resource_portals_per_transaction; k++)
	{
		if (me->incs[k].queueid != q->queueid || !me->incs[k].granted)
			continue;
		nportals++;
		for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
			totals[i] += me->incs[k].inc[i];
	}

	for (int i = 0; i < NUM_RES_LIMIT_TYPES; i++)
	{
		if (q->threshold[i] == INVALID_RES_LIMIT_THRESHOLD)
			continue;
		if (totals[i] > q->threshold[i])
		{
			over = true;
			if (i == RES_COUNT_LIMIT)
				count_over = true;
		}
	}
	if (q->overcommit && nportals == 1 && !count_over)
		over = false;
	return over;
}

/* A free increment slot of this backend's, or -1 */
static int
free_inc(void)
{
	ResQueueProc *me = my_proc();

	for (int k = 0; k < gp_max_resource_portals_per_transaction; k++)
	{
		if (!OidIsValid(me->incs[k].queueid))
			return k;
	}
	return -1;
}

/*
 * ResProcLockRemoveSelfAndWakeup(): walk the queue's waiters in order and
 * grant each that now fits -- a small one behind a large one may pass it.
 * With the lock held.
 */
static void
wake_waiters(ResQueueEntry *q)
{
	int			prev = -1;
	int			procno = q->waithead;

	while (procno >= 0)
	{
		ResQueueProc *p = proc_at(procno);
		int			next = p->next;
		ResPortalInc *pi = &p->incs[p->waitinc];

		if (check_limit(q, pi->inc) == LIMIT_CHECK_OK)
		{
			update_limit(q, pi->inc, true);
			pi->granted = true;
			q->ngranted++;
			/* unlink it */
			if (prev < 0)
				q->waithead = next;
			else
				proc_at(prev)->next = next;
			if (q->waittail == procno)
				q->waittail = prev;
			p->next = -1;
			p->waitinc = -1;
			SetLatch(&GetPGProcByNumber(procno)->procLatch);
		}
		else
			prev = procno;
		procno = next;
	}
}

/* Take this backend out of its queue's waiters, with the lock held */
static void
unlink_waiter(ResQueueEntry *q, int me)
{
	int			prev = -1;

	for (int procno = q->waithead; procno >= 0; procno = proc_at(procno)->next)
	{
		if (procno == me)
		{
			int			next = proc_at(procno)->next;

			if (prev < 0)
				q->waithead = next;
			else
				proc_at(prev)->next = next;
			if (q->waittail == procno)
				q->waittail = prev;
			proc_at(procno)->next = -1;
			return;
		}
		prev = procno;
	}
}

/* Give an increment back, with the lock held, and wake whoever now fits */
static void
release_inc(int k)
{
	ResQueueProc *me = my_proc();
	ResPortalInc *pi = &me->incs[k];
	ResQueueEntry *q = find_queue(pi->queueid);

	if (q != NULL)
	{
		if (pi->granted)
		{
			update_limit(q, pi->inc, false);
			q->ngranted--;
		}
		else
			unlink_waiter(q, MyProcNumber);
		q->nrequested--;
		wake_waiters(q);
		/* a queue dropped, now that nothing holds it */
		if (!q->defined && q->nrequested == 0)
			q->queueid = InvalidOid;
	}
	if (me->waitinc == k)
		me->waitinc = -1;
	memset(pi, 0, sizeof(*pi));
}

/*
 * The cycle a wait of this backend's closes, if there is one, as the DETAIL
 * of PostgreSQL's deadlock report: a backend waiting on a queue waits for
 * every other backend that holds a slot of it, and one waiting on a
 * heavyweight lock for the lock's holders.  With the lock held.
 */
typedef struct WaitEdge
{
	int			pid;
	char	   *what;			/* what it waits for, as the report says it */
	int			blocker;
} WaitEdge;

static char *
describe_queue_wait(Oid queueid)
{
	return psprintf("ExclusiveLock on resource queue %u", queueid);
}

/* The backends a backend waits for, by pid, with what it waits for */
static List *
blockers_of(int procno, char **what)
{
	ResQueueProc *p = proc_at(procno);
	PGPROC	   *proc = GetPGProcByNumber(procno);
	List	   *pids = NIL;

	*what = NULL;
	if (p->waitinc >= 0)
	{
		Oid			queueid = p->incs[p->waitinc].queueid;

		*what = describe_queue_wait(queueid);
		for (int i = 0; i < MaxBackends; i++)
		{
			ResQueueProc *o = proc_at(i);

			if (i == procno || o->pid == 0)
				continue;
			for (int k = 0; k < gp_max_resource_portals_per_transaction; k++)
			{
				if (o->incs[k].queueid == queueid && o->incs[k].granted)
				{
					pids = lappend_int(pids, o->pid);
					break;
				}
			}
		}
		return pids;
	}

	if (proc->pid != 0 && proc->waitLock != NULL)
	{
		BlockedProcsData *data = GetBlockerStatusData(proc->pid);

		for (int b = 0; b < data->nprocs; b++)
		{
			BlockedProcData *bproc = &data->procs[b];
			LOCKTAG		tag;
			LOCKMODE	mode = NoLock;
			bool		have_tag = false;

			for (int l = bproc->first_lock; l < bproc->first_lock + bproc->num_locks; l++)
			{
				LockInstanceData *li = &data->locks[l];

				if (li->pid == bproc->pid && li->waitLockMode != NoLock)
				{
					tag = li->locktag;
					mode = li->waitLockMode;
					have_tag = true;
				}
			}
			for (int l = bproc->first_lock; l < bproc->first_lock + bproc->num_locks; l++)
			{
				LockInstanceData *li = &data->locks[l];

				if (li->pid != bproc->pid && li->holdMask != 0)
					pids = list_append_unique_int(pids, li->pid);
			}
			if (have_tag && *what == NULL)
			{
				StringInfoData buf;

				initStringInfo(&buf);
				appendStringInfo(&buf, "%s on ",
								 GetLockmodeName(tag.locktag_lockmethodid, mode));
				DescribeLockTag(&buf, &tag);
				*what = buf.data;
			}
		}
	}
	return pids;
}

static int
procno_of_pid(int pid)
{
	PGPROC	   *proc = BackendPidGetProc(pid);

	return proc != NULL ? GetNumberFromPGProc(proc) : -1;
}

/* Depth first from this backend, for a path back to it */
static bool
find_cycle(int procno, int depth, List **path, Bitmapset **seen)
{
	char	   *what;
	List	   *blockers;

	if (depth > MaxBackends)
		return false;
	blockers = blockers_of(procno, &what);
	foreach_int(pid, blockers)
	{
		int			next = procno_of_pid(pid);
		WaitEdge   *edge;

		if (next < 0 || next >= MaxBackends)
			continue;
		edge = palloc(sizeof(WaitEdge));
		edge->pid = GetPGProcByNumber(procno)->pid;
		edge->what = what;
		edge->blocker = pid;
		*path = lappend(*path, edge);
		if (next == MyProcNumber)
			return true;
		if (!bms_is_member(next, *seen))
		{
			*seen = bms_add_member(*seen, next);
			if (find_cycle(next, depth + 1, path, seen))
				return true;
		}
		*path = list_delete_last(*path);
	}
	return false;
}

static void
report_deadlock(List *path)
{
	StringInfoData detail;

	initStringInfo(&detail);
	foreach_ptr(WaitEdge, edge, path)
	{
		if (detail.len > 0)
			appendStringInfoChar(&detail, '\n');
		appendStringInfo(&detail, "Process %d waits for %s; blocked by process %d.",
						 edge->pid, edge->what, edge->blocker);
	}
	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("deadlock detected"),
			 errdetail_internal("%s", detail.data),
			 errhint("See server log for query details.")));
}

/*
 * Wait until the increment is granted: on the latch, looking for a deadlock
 * every deadlock_timeout, until lock_timeout if it is set, and giving up on
 * a cancel, a timeout or a termination as CHECK_FOR_INTERRUPTS() raises it.
 */
static void
wait_for_slot(int k)
{
	ResQueueProc *me = my_proc();
	TimestampTz start = GetCurrentTimestamp();
	TimestampTz next_check = TimestampTzPlusMilliseconds(start, DeadlockTimeout);
	const char *old_status = NULL;
	int			len = 0;

	if (update_process_title)
	{
		old_status = get_ps_display(&len);
		set_ps_display(psprintf("%.*s queuing", len, old_status));
	}

	for (;;)
	{
		long		timeout;
		TimestampTz now;

		LWLockAcquire(rq_lock, LW_SHARED);
		if (me->incs[k].granted)
		{
			LWLockRelease(rq_lock);
			break;
		}
		LWLockRelease(rq_lock);

		now = GetCurrentTimestamp();
		if (now >= next_check)
		{
			List	   *path = NIL;
			Bitmapset  *seen = NULL;
			bool		found;

			LWLockAcquire(rq_lock, LW_SHARED);
			found = !me->incs[k].granted &&
				find_cycle(MyProcNumber, 0, &path, &seen);
			LWLockRelease(rq_lock);
			if (found)
				report_deadlock(path);
			next_check = TimestampTzPlusMilliseconds(now, DeadlockTimeout);
		}
		if (LockTimeout > 0 &&
			TimestampDifferenceExceeds(start, now, LockTimeout))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("canceling statement due to lock timeout")));

		timeout = TimestampDifferenceMilliseconds(now, next_check);
		if (LockTimeout > 0)
			timeout = Min(timeout, LockTimeout);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 Max(timeout, 1), rq_wait_event);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}

	if (update_process_title && old_status != NULL)
		set_ps_display(pnstrdup(old_status, len));
}

static void resqueue_portal_cleanup(Portal portal);

/*
 * ResLockPortal() and ResLockUtilityPortal(): take a slot of the queue for
 * the portal, waiting if the queue has no room, with Cloudberry's errors
 * where it will never have.  False where the statement costs less than the
 * queue minds, which takes no slot.
 */
static bool
lock_portal(Portal portal, Oid queueid, double cost, double memory,
			bool utility)
{
	ResQueueProc *me = my_proc();
	ResQueueEntry *q;
	PortalSlot *slot;
	double		inc[NUM_RES_LIMIT_TYPES];
	uint32		portalid;
	int			k;
	LimitCheck	status;

	portalid = create_portal_id(portal->name);

	inc[RES_COUNT_LIMIT] = 1;
	inc[RES_COST_LIMIT] = utility ? cost : ceil(cost);
	inc[RES_MEMORY_LIMIT] = memory;

	/*
	 * The portal's record, kept once it holds a slot -- or once it is known
	 * to take none, costing less than the queue minds, so that none of its
	 * statements asks again -- and freed with the portal; a request that
	 * fails keeps none, as another portal may come to have its address.
	 */
	slot = MemoryContextAllocZero(TopMemoryContext, sizeof(PortalSlot));
	slot->portal = portal;
	slot->inc = -1;
	slot->portalid = portalid;
	slot->stats.queueid = queueid;
	slot->stats.wait_start = time(NULL);

	stats_submitted(queueid, inc[RES_COST_LIMIT], memory);

	/* Cloudberry's fault, as if the increments' table were full */
	if (GP_FAULT("res_increment_add_oosm") == GP_FAULT_SKIP)
	{
		pfree(slot);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory adding portal increments"),
				 errhint("You may need to increase max_resource_portals_per_transaction.")));
	}

	LWLockAcquire(rq_lock, LW_EXCLUSIVE);
	q = find_queue(queueid);
	if (q == NULL)
	{
		LWLockRelease(rq_lock);
		pfree(slot);
		elog(LOG, "gp_resource: resource queue %u is not in shared memory; the statement takes no slot",
			 queueid);
		return false;
	}

	/* the cost the queue does not mind: no slot, for any of its statements */
	if (inc[RES_COST_LIMIT] < q->ignorecostlimit)
	{
		LWLockRelease(rq_lock);
		stats_waited(queueid, slot->stats.wait_start, false);
		remember_slot(slot);
		slot->prev_cleanup = portal->cleanup;
		portal->cleanup = resqueue_portal_cleanup;
		return false;
	}

	k = free_inc();
	if (k < 0)
	{
		LWLockRelease(rq_lock);
		pfree(slot);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory adding portal increments"),
				 errhint("You may need to increase max_resource_portals_per_transaction.")));
	}

	status = check_limit(q, inc);
	if (status == LIMIT_CHECK_ERROR)
	{
		LWLockRelease(rq_lock);
		stats_waited(queueid, slot->stats.wait_start, false);
		pfree(slot);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("statement requires more resources than resource queue allows"),
				 errdetail("resource queue id: %u, portal id: %u",
						   queueid, portalid)));
	}
	if (status == LIMIT_CHECK_FOUND && self_deadlock(q, inc))
	{
		LWLockRelease(rq_lock);
		stats_waited(queueid, slot->stats.wait_start, false);
		pfree(slot);
		(void) GP_FAULT("res_lock_acquire_self_deadlock_error");
		ereport(ERROR,
				(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
				 errmsg("deadlock detected, locking against self"),
				 errdetail("resource queue id: %u, portal id: %u",
						   queueid, portalid)));
	}

	/* the increment, granted or waited for */
	me->pid = MyProcPid;
	me->incs[k].queueid = queueid;
	me->incs[k].portalid = portalid;
	memcpy(me->incs[k].inc, inc, sizeof(inc));
	me->incs[k].ishold = (portal->cursorOptions & CURSOR_OPT_HOLD) != 0;
	me->incs[k].granted = false;
	q->nrequested++;
	slot->inc = k;
	slot->ishold = me->incs[k].ishold;

	if (status == LIMIT_CHECK_OK)
	{
		update_limit(q, inc, true);
		me->incs[k].granted = true;
		q->ngranted++;
		LWLockRelease(rq_lock);
	}
	else
	{
		me->waitinc = k;
		me->next = -1;
		me->waitstart = GetCurrentTimestamp();
		if (q->waittail < 0)
			q->waithead = MyProcNumber;
		else
			proc_at(q->waittail)->next = MyProcNumber;
		q->waittail = MyProcNumber;
		LWLockRelease(rq_lock);

		pgstat_report_wait_start(rq_wait_event);
		PG_TRY();
		{
			wait_for_slot(k);
		}
		PG_CATCH();
		{
			pgstat_report_wait_end();
			/* a waiter that gives up, or one granted as it did */
			LWLockAcquire(rq_lock, LW_EXCLUSIVE);
			release_inc(k);
			LWLockRelease(rq_lock);
			stats_waited(queueid, slot->stats.wait_start, false);
			pfree(slot);
			PG_RE_THROW();
		}
		PG_END_TRY();
		pgstat_report_wait_end();
	}

	stats_waited(queueid, slot->stats.wait_start, true);
	slot->stats.exec_start = time(NULL);
	if (slot->ishold)
		num_hold_portals++;

	/* given back as the portal is cleaned up */
	remember_slot(slot);
	slot->prev_cleanup = portal->cleanup;
	portal->cleanup = resqueue_portal_cleanup;
	return true;
}

/* ResUnLockPortal() */
static void
unlock_portal(PortalSlot *slot)
{
	if (slot->inc >= 0)
	{
		stats_completed(slot->stats.queueid, slot->stats.exec_start);
		LWLockAcquire(rq_lock, LW_EXCLUSIVE);
		release_inc(slot->inc);
		LWLockRelease(rq_lock);
		slot->inc = -1;
		if (slot->ishold)
			num_hold_portals--;
	}
	portal_slots = list_delete_ptr(portal_slots, slot);
	pfree(slot);
}

/*
 * A portal whose slot this backend holds is cleaned up: its own cleanup
 * first -- ExecutorEnd(), and the autostats that runs there -- then the
 * slot, as Cloudberry's PortalCleanup() gives it back last.
 */
static void
resqueue_portal_cleanup(Portal portal)
{
	PortalSlot *slot = find_slot(portal);
	void		(*cleanup) (Portal portal) = slot != NULL ? slot->prev_cleanup : PortalCleanup;

	PG_TRY();
	{
		/* PortalCleanup() asserts it is called as the portal's own cleanup */
		portal->cleanup = cleanup;
		if (cleanup != NULL)
			cleanup(portal);
	}
	PG_FINALLY();
	{
		if (slot != NULL)
			unlock_portal(slot);
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* Which statements take a slot                                              */
/* ------------------------------------------------------------------------- */

static bool
portal_is_utility(Portal portal)
{
	PlannedStmt *pstmt = PortalGetPrimaryStmt(portal);

	return pstmt == NULL || pstmt->commandType == CMD_UTILITY;
}

/*
 * The budget of a query, in bytes: ResourceQueueGetQueryMemoryLimit() --
 * statement_mem for a superuser, nothing where the policy is none, and
 * otherwise the queue's memory over its slots, or over its cost limit by the
 * plan's cost, whichever is less, but never less than statement_mem.
 */
static double
query_memory(Oid queueid, double plancost)
{
	ResQueueEntry *q;
	double		limit;
	double		slots;
	double		costlimit;
	double		ratio;
	double		mem;

	if (superuser())
		return (double) gp_statement_mem * 1024.0;
	if (gp_resqueue_memory_policy == RESMANAGER_MEMORY_POLICY_NONE)
		return 0;

	LWLockAcquire(rq_lock, LW_SHARED);
	q = find_queue(queueid);
	if (q == NULL)
	{
		LWLockRelease(rq_lock);
		return (double) gp_statement_mem * 1024.0;
	}
	limit = q->threshold[RES_MEMORY_LIMIT];
	slots = ceil(q->threshold[RES_COUNT_LIMIT]);
	costlimit = q->threshold[RES_COST_LIMIT];
	LWLockRelease(rq_lock);

	if (limit < 0)
		return (double) gp_statement_mem * 1024.0;
	if (plancost < 1.0)
		plancost = 1.0;
	if (slots < 1)
		slots = 1;
	if (costlimit < 0)
		costlimit = plancost;
	ratio = Min(Min(1.0 / slots, plancost / costlimit), 1.0);
	mem = floor(limit * ratio);
	if (mem < (double) gp_statement_mem * 1024.0)
		mem = (double) gp_statement_mem * 1024.0;
	return mem;
}

/* The budget the executor runs the current query with, in kB; 0: none */
static int	query_budget_kb = 0;

int
ResQueueQueryBudgetKB(void)
{
	return query_budget_kb;
}

/*
 * ExecutorStart: the query's portal takes its slot, as ResLockPortal() and
 * _SPI_pquery() take one.  A portal takes one slot, the first statement of
 * it that may; one that did, or would not, takes no more.
 */
void
ResQueueExecutorStart(QueryDesc *queryDesc)
{
	Portal		portal = ActivePortal;
	PlannedStmt *pstmt = queryDesc->plannedstmt;
	bool		spi = queryDesc->dest != NULL &&
		queryDesc->dest->mydest == DestSPI;
	Oid			queueid;
	double		memory;
	double		cost;

	query_budget_kb = 0;
	if (!IsResQueueEnabled() || !GpResourceIsDispatcher() || portal == NULL ||
		!IsUnderPostmaster)
		return;
	if (find_slot(portal) != NULL || superuser())
	{
		if (superuser() && gp_resqueue_memory_policy != RESMANAGER_MEMORY_POLICY_NONE)
			query_budget_kb = gp_statement_mem;
		return;
	}

	if (portal_is_utility(portal))
	{
		/* a SELECT a DO or a CALL runs through SPI, as _SPI_pquery() locks */
		if (!spi || queryDesc->operation != CMD_SELECT)
			return;
	}
	else if (!spi)
	{
		if (!pstmt->canSetTag)
			return;
		switch (queryDesc->operation)
		{
			case CMD_SELECT:
				break;
			case CMD_INSERT:
			case CMD_UPDATE:
			case CMD_DELETE:
				if (gp_resource_select_only)
					return;
				break;
			default:
				return;
		}
	}
	else if (queryDesc->operation != CMD_SELECT)
		return;

	ensure_loaded();
	queueid = current_queue();
	cost = pstmt->planTree->total_cost;
	memory = query_memory(queueid, cost);
	if (memory > 0 && gp_log_resqueue_memory)
		ereport(NOTICE,
				(errmsg("query requested %.0fKB", memory / 1024.0)));

	if (lock_portal(portal, queueid, cost, memory, false) && memory > 0)
		query_budget_kb = (int) Min(memory / 1024.0, (double) MAX_KILOBYTES);
}

/*
 * A utility statement: COPY and CREATE TABLE AS take a slot as
 * ResHandleUtilityStmt() has them take one -- where the queue counts
 * statements, costing its minimum -- and the query CREATE TABLE AS runs
 * then takes none more.
 */
void
ResQueueUtilityStart(PlannedStmt *pstmt)
{
	Node	   *stmt = pstmt->utilityStmt;
	Portal		portal = ActivePortal;
	Oid			queueid;
	ResQueueEntry *q;
	double		ignorecost;
	double		slots;

	if (!IsA(stmt, CopyStmt) && !IsA(stmt, CreateTableAsStmt))
		return;
	if (!IsResQueueEnabled() || !GpResourceIsDispatcher() || portal == NULL ||
		gp_resource_select_only || superuser() || find_slot(portal) != NULL ||
		!IsUnderPostmaster)
		return;

	ensure_loaded();
	queueid = current_queue();
	LWLockAcquire(rq_lock, LW_SHARED);
	q = find_queue(queueid);
	slots = q != NULL ? ceil(q->threshold[RES_COUNT_LIMIT]) : -1;
	ignorecost = q != NULL ? q->ignorecostlimit : 0;
	LWLockRelease(rq_lock);
	if (slots >= 1)
		(void) lock_portal(portal, queueid, ignorecost, 0, true);
}

/* ------------------------------------------------------------------------- */
/* Transactions and the backend's end                                        */
/* ------------------------------------------------------------------------- */

static void
resqueue_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			/* the definitions this transaction wrote, read while it can */
			if (defs_changed)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				pending_defs = ResQueueDefsLoad();
				MemoryContextSwitchTo(oldcxt);
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			if (defs_changed && pending_defs != NIL && rq_ctl != NULL)
				sync_queues(pending_defs);
			defs_changed = false;
			pending_defs = NIL;
			/* AtCommit_ResScheduler() */
			if (num_hold_portals == 0)
				portal_id_counter = 0;
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			defs_changed = false;
			pending_defs = NIL;
			if (num_hold_portals == 0)
				portal_id_counter = 0;
			break;
	}
}

/* Whatever this backend still holds, given back as it exits */
static void
resqueue_exit(int code, Datum arg)
{
	ResQueueProc *me;

	if (rq_ctl == NULL || MyProcNumber < 0 || MyProcNumber >= MaxBackends)
		return;
	me = my_proc();
	LWLockAcquire(rq_lock, LW_EXCLUSIVE);
	for (int k = 0; k < gp_max_resource_portals_per_transaction; k++)
	{
		if (OidIsValid(me->incs[k].queueid))
			release_inc(k);
	}
	me->pid = 0;
	LWLockRelease(rq_lock);
}

static bool exit_registered = false;

void
ResQueueBackendStart(void)
{
	if (exit_registered || rq_ctl == NULL)
		return;
	before_shmem_exit(resqueue_exit, 0);
	exit_registered = true;
	ResQueueWaitEventInit();
	if (MyProcNumber >= 0 && MyProcNumber < MaxBackends)
		my_proc()->pid = MyProcPid;
}

void
ResQueueInit(void)
{
	pgstat_register_kind(PGSTAT_KIND_RESQUEUE, &resqueue_stats_kind);
	RegisterXactCallback(resqueue_xact_callback, NULL);
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_resource_resqueue_status);
PG_FUNCTION_INFO_V1(gp_resource_resqueue_status_kv);
PG_FUNCTION_INFO_V1(gp_resource_resqueue_stats);
PG_FUNCTION_INFO_V1(gp_resource_resqueue_locks);

/*
 * pg_resqueue_status(): queueid, queuecountvalue, queuecostvalue,
 * queuewaiters, queueholders -- a value NULL where its limit is none, and no
 * row at all where queues are off, as Cloudberry's.
 */
Datum
gp_resource_resqueue_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	if (!IsResQueueEnabled() || !GpResourceIsDispatcher())
		return (Datum) 0;
	ensure_loaded();

	LWLockAcquire(rq_lock, LW_SHARED);
	for (int i = 0; i < gp_max_resource_queues; i++)
	{
		ResQueueEntry *q = &rq_ctl->queues[i];
		Datum		values[5];
		bool		nulls[5] = {0};

		if (!OidIsValid(q->queueid) || !q->defined)
			continue;
		values[0] = ObjectIdGetDatum(q->queueid);
		if (q->threshold[RES_COUNT_LIMIT] == INVALID_RES_LIMIT_THRESHOLD)
			nulls[1] = true;
		else
			values[1] = Float4GetDatum((float4) q->current[RES_COUNT_LIMIT]);
		if (q->threshold[RES_COST_LIMIT] == INVALID_RES_LIMIT_THRESHOLD)
			nulls[2] = true;
		else
			values[2] = Float4GetDatum((float4) q->current[RES_COST_LIMIT]);
		values[3] = Int32GetDatum(q->nrequested - q->ngranted);
		values[4] = Int32GetDatum(q->ngranted);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	LWLockRelease(rq_lock);
	return (Datum) 0;
}

/*
 * pg_resqueue_status_kv(): queueid, key, value, eight keys a queue, which
 * gp_toolkit.gp_resqueue_status reads.
 */
Datum
gp_resource_resqueue_status_kv(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	if (!IsResQueueEnabled() || !GpResourceIsDispatcher())
		return (Datum) 0;
	ensure_loaded();

	LWLockAcquire(rq_lock, LW_SHARED);
	for (int i = 0; i < gp_max_resource_queues; i++)
	{
		ResQueueEntry *q = &rq_ctl->queues[i];
		char	   *kv[8][2];

		if (!OidIsValid(q->queueid) || !q->defined)
			continue;
		kv[0][0] = "rsqcountlimit";
		kv[0][1] = psprintf("%d", (int) ceil(q->threshold[RES_COUNT_LIMIT]));
		kv[1][0] = "rsqcountvalue";
		kv[1][1] = psprintf("%d", (int) ceil(q->current[RES_COUNT_LIMIT]));
		kv[2][0] = "rsqcostlimit";
		kv[2][1] = psprintf("%.2f", q->threshold[RES_COST_LIMIT]);
		kv[3][0] = "rsqcostvalue";
		kv[3][1] = psprintf("%.2f", q->current[RES_COST_LIMIT]);
		kv[4][0] = "rsqmemorylimit";
		kv[4][1] = psprintf("%.2f", q->threshold[RES_MEMORY_LIMIT]);
		kv[5][0] = "rsqmemoryvalue";
		kv[5][1] = psprintf("%.2f", q->current[RES_MEMORY_LIMIT]);
		kv[6][0] = "rsqwaiters";
		kv[6][1] = psprintf("%d", q->nrequested - q->ngranted);
		kv[7][0] = "rsqholders";
		kv[7][1] = psprintf("%d", q->ngranted);
		for (int j = 0; j < 8; j++)
		{
			Datum		values[3];
			bool		nulls[3] = {0};

			values[0] = ObjectIdGetDatum(q->queueid);
			values[1] = CStringGetTextDatum(kv[j][0]);
			values[2] = CStringGetTextDatum(kv[j][1]);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	LWLockRelease(rq_lock);
	return (Datum) 0;
}

/*
 * pg_stat_get_resqueue_stats(queueid): the statistics of a queue, and
 * have_stats, which pg_stat_resqueues reads.
 */
Datum
gp_resource_resqueue_stats(PG_FUNCTION_ARGS)
{
	Oid			queueid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[12];
	bool		nulls[12] = {0};
	ResQueueStats *st;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	st = (ResQueueStats *) pgstat_fetch_entry(PGSTAT_KIND_RESQUEUE, InvalidOid,
											  queueid, NULL);
	if (st == NULL)
	{
		for (int i = 0; i < 10; i++)
			values[i] = Int64GetDatum(0);
		nulls[10] = true;
		values[11] = BoolGetDatum(false);
	}
	else
	{
		values[0] = Int64GetDatum(st->queries_submitted);
		values[1] = Int64GetDatum(st->queries_admitted);
		values[2] = Int64GetDatum(st->queries_rejected);
		values[3] = Int64GetDatum(st->queries_completed);
		values[4] = Int64GetDatum(st->elapsed_wait_secs);
		values[5] = Int64GetDatum(st->max_wait_secs);
		values[6] = Int64GetDatum(st->elapsed_exec_secs);
		values[7] = Int64GetDatum(st->max_exec_secs);
		values[8] = Int64GetDatum(st->total_cost);
		values[9] = Int64GetDatum(st->total_memory_kb);
		if (st->stat_reset_timestamp == 0)
			nulls[10] = true;
		else
			values[10] = TimestampTzGetDatum(st->stat_reset_timestamp);
		values[11] = BoolGetDatum(true);
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * The backends that hold a queue's slot or wait for one: pid, queue,
 * granted -- what Cloudberry's pg_locks rows of its queues say, one a
 * backend and queue, granted where it holds a slot, and another not granted
 * where it also waits.  gp_toolkit's views of the queues read this.
 */
Datum
gp_resource_resqueue_locks(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	if (rq_ctl == NULL)
		return (Datum) 0;

	LWLockAcquire(rq_lock, LW_SHARED);
	for (int i = 0; i < MaxBackends; i++)
	{
		ResQueueProc *p = proc_at(i);
		List	   *held = NIL;

		if (p->pid == 0)
			continue;
		for (int k = 0; k < gp_max_resource_portals_per_transaction; k++)
		{
			Datum		values[3];
			bool		nulls[3] = {0};

			if (!OidIsValid(p->incs[k].queueid))
				continue;
			if (p->incs[k].granted)
			{
				if (list_member_oid(held, p->incs[k].queueid))
					continue;
				held = lappend_oid(held, p->incs[k].queueid);
			}
			values[0] = Int32GetDatum(p->pid);
			values[1] = ObjectIdGetDatum(p->incs[k].queueid);
			values[2] = BoolGetDatum(p->incs[k].granted);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	LWLockRelease(rq_lock);
	return (Datum) 0;
}

void
ResQueueWaitEventInit(void)
{
	if (rq_wait_event == 0)
		rq_wait_event = WaitEventExtensionNew("ResourceQueue");
}
