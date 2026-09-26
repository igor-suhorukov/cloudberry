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
 * resgroup.c
 *	  Resource groups at run time.
 *
 * A transaction of a role runs, on the coordinator, in the role's resource
 * group -- admin_group for a superuser whose role names none,
 * default_group for anyone else -- and holds one of the group's slots, of
 * which there are as many as its concurrency, waiting while none is free.
 * Cloudberry takes the slot as the transaction starts
 * (AssignResGroupOnMaster()), where PostgreSQL 19 gives a module no hook;
 * the port takes it at the transaction's first statement, in
 * ExecutorStart_hook or ProcessUtility_hook, and gives it back in a
 * transaction callback, at the commit, abort or PREPARE that ends it, which
 * is where Cloudberry gives it back.
 *
 * What is Cloudberry's: the group a role runs in; the statements that run
 * without a slot, bypassed -- anything under gp_resource_group_bypass, a
 * statement string of SET, RESET, SHOW and SELECTs of pg_catalog's tables
 * alone, and, planned, outside a transaction block, a query that costs less
 * than its group's min_cost or reads the catalogs alone; the waiters woken
 * first come first as slots come free, and by an ALTER that raises the
 * concurrency; the timeout of a wait; a DROP refused while the group runs a
 * query, and its waiters sent to the group their role has now; the memory
 * a query is given, its group's memory_quota over its concurrency but never
 * less than statement_mem, which it runs with as its work_mem (as a queue's
 * is); and the statistics pg_resgroup_get_status() reports.
 *
 * A segment's backend runs in the group the coordinator's does, which the
 * dispatch tells it, with the group's limits (gp_resource.statement:
 * "group=6437 caps=20/20/100/-1/500 cpuset=-1"), as
 * SwitchResGroupOnSegment() takes it from the dispatched query; there it
 * takes no slot.  A segment has no copy of the groups' definitions -- a
 * shared object's label is the coordinator's (gp_dispatch.c) -- so what it
 * knows of a group is what the dispatch told it.
 *
 * The cgroups are Cloudberry's code, compiled where it lies (cgroup.c,
 * cgroup-ops-linux-v1.c and -v2.c, cgroup_io_limit.c; meson.build), driven
 * through its table of operations as Cloudberry's resgroup.c drives it: the
 * postmaster sets up the parent and system_group's cgroup as it starts,
 * before it forks anything; the first backend makes every group's; a
 * commit that changes a group changes its cgroup; a backend moves into its
 * group's as it takes the slot, and a segment's as it switches.  A segment
 * makes a group's cgroup, and sets its limits, as the dispatch first tells
 * it of them, so that a segment host has the coordinator's.
 *
 * What is not: Cloudberry's wait event of a group's waiter is of a type of
 * its own, "ResourceGroup", and names the group; PostgreSQL 19's waits of a
 * module are of type Extension, and a module registers each name once, of a
 * few hundred, so the wait is the Extension wait ResourceGroup, and
 * pg_stat_activity's rsgname says which group.  pg_resgroup_move_query()
 * moves a running transaction to another group as Cloudberry's does, but
 * its target takes the new slot in a signal handler of gp_resource's own,
 * and only as it waits on its latch ("Moving a transaction" below).
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/utils/resgroup/resgroup.c, resgroup_helper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <signal.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_gdd.h"
#include "gp_resource.h"
#include "utils/cgroup.h"
#include "utils/cgroup_io_limit.h"
#include "utils/cgroup-ops-v1.h"
#include "utils/cgroup-ops-v2.h"

/* Cloudberry's resgroup.h's defaults of a cpuset and an io_limit */
#define DefaultCpuset	"-1"
#define DefaultIOLimit	"-1"

/* Is the cpuset controller there to give a group cores of its own? */
bool		gp_resource_group_enable_cgroup_cpuset = false;

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

/* A group: what it may do, what it runs, and who waits for it */
typedef struct ResGroupEntry
{
	Oid			groupid;		/* InvalidOid: the entry is free */
	bool		defined;		/* its definition is there still */
	bool		locked_for_drop;	/* by a DROP that has not ended */
	int			concurrency;
	int			cpu_max_percent;
	int			cpu_weight;
	int			memory_quota;	/* MB; -1: none */
	int			min_cost;
	char		cpuset[MaxCpuSetLength];
	char		io_limit[MaxCpuSetLength];	/* as far as it fits */
	int			nrunning;		/* slots held */
	int			nbypassed;		/* transactions it runs without a slot */
	int			nwaiting;
	int64		total_executed;
	int64		total_queued;
	int64		total_queue_us;
	int			waithead;		/* procno of the first waiter, -1: none */
	int			waittail;
} ResGroupEntry;

/* A backend: the group it runs in, or waits for */
typedef struct ResGroupProc
{
	Oid			groupid;		/* InvalidOid: none */
	bool		has_slot;
	bool		bypassed;
	bool		waiting;		/* in a group's list */
	bool		granted;		/* given a slot as it waited */
	Oid			wait_group;		/* the group whose list it is in */
	int			next;			/* the next waiter of its group, -1: none */
	Oid			rsgid;			/* what pg_stat_activity shows (see below) */
	Oid			moved_into;		/* the group a mover put it in the cgroup of */
	Oid			moved_from;		/* and the one it was in, this transaction */

	/*
	 * A move of the backend's transaction to another group, which a mover
	 * asks of it and it takes (Cloudberry's PGPROC's moveto* fields; see
	 * "Moving a transaction").
	 */
	slock_t		move_mutex;
	Oid			moveto_group;	/* InvalidOid: none, or the target has looked */
	bool		moveto_slot;	/* the mover's slot, until the target takes it */
	int			move_caller;	/* the mover's pid, 0: none */
	int			move_caller_procno;
} ResGroupProc;

typedef struct ResGroupControl
{
	bool		loaded;			/* the definitions are read in */
	ResGroupEntry groups[MaxResourceGroups];
	ResGroupProc procs[FLEXIBLE_ARRAY_MEMBER];
} ResGroupControl;

static ResGroupControl *rg_ctl = NULL;
static LWLock *rg_lock = NULL;
static uint32 rg_wait_event = 0;

#define RG_TRANCHE		"gp_resource groups"

Size
ResGroupShmemSize(void)
{
	return add_size(offsetof(ResGroupControl, procs),
					mul_size(sizeof(ResGroupProc), MaxBackends));
}

void
ResGroupShmemRequest(void)
{
	RequestNamedLWLockTranche(RG_TRANCHE, 1);
}

void
ResGroupShmemInit(void)
{
	bool		found;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	rg_ctl = ShmemInitStruct("gp_resource groups", ResGroupShmemSize(), &found);
	if (!found)
	{
		memset(rg_ctl, 0, ResGroupShmemSize());
		for (int i = 0; i < MaxResourceGroups; i++)
			rg_ctl->groups[i].waithead = rg_ctl->groups[i].waittail = -1;
		for (int i = 0; i < MaxBackends; i++)
		{
			rg_ctl->procs[i].next = -1;
			SpinLockInit(&rg_ctl->procs[i].move_mutex);
		}
	}
	LWLockRelease(AddinShmemInitLock);
	rg_lock = &(GetNamedLWLockTranche(RG_TRANCHE))->lock;
}

static ResGroupEntry *
find_group(Oid groupid)
{
	for (int i = 0; i < MaxResourceGroups; i++)
	{
		if (rg_ctl->groups[i].groupid == groupid)
			return &rg_ctl->groups[i];
	}
	return NULL;
}

static bool
have_proc(void)
{
	return rg_ctl != NULL && MyProcNumber >= 0 && MyProcNumber < MaxBackends;
}

static ResGroupProc *
my_proc(void)
{
	return &rg_ctl->procs[MyProcNumber];
}

/* ------------------------------------------------------------------------- */
/* The cgroups: Cloudberry's code of them                                    */
/* ------------------------------------------------------------------------- */

/* Cloudberry's resgroup.c's, which its cgroup code reads */
CGroupOpsRoutine *cgroupOpsRoutine = NULL;
CGroupSystemInfo *cgroupSystemInfo = NULL;

/* guc_gp.c's, which the cgroup code sets to 0: every process nice 0 */
int			gp_segworker_relative_priority = 0;

/*
 * initCgroup(), in the postmaster as it starts, before it forks anything:
 * the cgroup file system of the version the manager names, checked -- a
 * server whose cgroups are not as the manager needs them does not start,
 * as Cloudberry's does not -- and set up: the controllers turned on under
 * the parent, the parent's CPU limits, and system_group's cgroup, which the
 * postmaster moves into, and so every process it forks.
 */
void
ResGroupCgroupInit(void)
{
	if (gp_resource_manager_policy == RESOURCE_MANAGER_POLICY_GROUP)
	{
		cgroupOpsRoutine = get_group_routine_v1();
		cgroupSystemInfo = get_cgroup_sysinfo_v1();
	}
	else
	{
		cgroupOpsRoutine = get_group_routine_v2();
		cgroupSystemInfo = get_cgroup_sysinfo_v2();
	}

	if (!cgroupOpsRoutine->probecgroup())
		elog(ERROR, "The control group is not well configured, please check your "
			 "system configuration.");
	cgroupOpsRoutine->checkcgroup();
	cgroupOpsRoutine->initcgroup();
}

/* getCpuSetByRole(): of "coordinator;segments", the part this node's */
char *
ResGroupCpusetOfRole(const char *cpuset)
{
	const char *semi = strchr(cpuset, ';');

	if (semi == NULL)
		return pstrdup(cpuset);
	if (GpResourceIsSegment())
		return pstrdup(semi + 1);
	return pnstrdup(cpuset, semi - cpuset);
}

/*
 * A group's limits, from what they were -- nothing, for a group the cgroup
 * code has not made -- to what they are.  The io_limit is parsed where the
 * catalogs can be read, before the commit that changes it.
 */
typedef struct GroupLimits
{
	Oid			groupid;
	int			cpu_max_percent;
	int			cpu_weight;
	char	   *cpuset;
	char	   *io_limit;
	List	   *io;				/* parsed, or NIL */
} GroupLimits;

static void
cgroup_apply(const GroupLimits *now, const GroupLimits *before)
{
	if (cgroupOpsRoutine == NULL)
		return;
	if (before == NULL)
		cgroupOpsRoutine->createcgroup(now->groupid);

	if (strcmp(now->cpuset, DefaultCpuset) == 0)
	{
		if (before == NULL || before->cpu_max_percent != now->cpu_max_percent ||
			strcmp(before->cpuset, DefaultCpuset) != 0)
			cgroupOpsRoutine->setcpulimit(now->groupid, now->cpu_max_percent);
		if (before == NULL || before->cpu_weight != now->cpu_weight ||
			strcmp(before->cpuset, DefaultCpuset) != 0)
			cgroupOpsRoutine->setcpuweight(now->groupid, now->cpu_weight);
	}
	else if (gp_resource_group_enable_cgroup_cpuset &&
			 (before == NULL || strcmp(before->cpuset, now->cpuset) != 0))
		cgroupOpsRoutine->setcpuset(now->groupid,
									ResGroupCpusetOfRole(now->cpuset));

	if (now->io_limit != NULL &&
		(before == NULL ? strcmp(now->io_limit, DefaultIOLimit) != 0 :
		 before->io_limit == NULL || strcmp(before->io_limit, now->io_limit) != 0))
	{
		if (strcmp(now->io_limit, DefaultIOLimit) == 0)
			cgroupOpsRoutine->cleario(now->groupid);
		else if (now->io != NIL)
			cgroupOpsRoutine->setio(now->groupid, now->io);
		else
		{
			/* a segment's, told the io_limit by the dispatch: its own disks */
			List	   *io = cgroupOpsRoutine->parseio(now->io_limit);

			cgroupOpsRoutine->setio(now->groupid, io);
			cgroupOpsRoutine->freeio(io);
		}
	}
}

/*
 * The cgroup of a group, after a commit that changed it or as the first
 * backend reads the groups in.  After a commit, where an error would be too
 * late to undo anything, a failure is a WARNING; the definitions stand.
 */
static void
cgroup_apply_quietly(const GroupLimits *now, const GroupLimits *before,
					 Oid dropped)
{
	MemoryContext oldcxt = CurrentMemoryContext;

	PG_TRY();
	{
		if (OidIsValid(dropped))
			cgroupOpsRoutine->destroycgroup(dropped, true);
		else
			cgroup_apply(now, before);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not set up the cgroup of resource group %u: %s",
						OidIsValid(dropped) ? dropped : now->groupid,
						edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();
}

static GroupLimits *
limits_of_def(const ResGroupDef *def, bool parse_io)
{
	GroupLimits *l = palloc0(sizeof(GroupLimits));

	l->groupid = def->oid;
	l->cpu_max_percent = def->cpu_max_percent;
	l->cpu_weight = def->cpu_weight;
	l->cpuset = pstrdup(def->cpuset);
	l->io_limit = pstrdup(def->io_limit);
	if (parse_io && cgroupOpsRoutine != NULL &&
		strcmp(def->io_limit, DefaultIOLimit) != 0)
		l->io = cgroupOpsRoutine->parseio(def->io_limit);
	return l;
}

static GroupLimits *
limits_of_entry(const ResGroupEntry *e)
{
	GroupLimits *l = palloc0(sizeof(GroupLimits));

	l->groupid = e->groupid;
	l->cpu_max_percent = e->cpu_max_percent;
	l->cpu_weight = e->cpu_weight;
	l->cpuset = pstrdup(e->cpuset);
	l->io_limit = pstrdup(e->io_limit);
	return l;
}

/* ------------------------------------------------------------------------- */
/* The definitions, into shared memory                                       */
/* ------------------------------------------------------------------------- */

static void
set_caps(ResGroupEntry *e, const ResGroupDef *def)
{
	e->concurrency = def->concurrency;
	e->cpu_max_percent = def->cpu_max_percent;
	e->cpu_weight = def->cpu_weight;
	e->memory_quota = def->memory_quota;
	e->min_cost = def->min_cost;
	strlcpy(e->cpuset, def->cpuset, sizeof(e->cpuset));
	strlcpy(e->io_limit, def->io_limit, sizeof(e->io_limit));
}

/*
 * Wake a group's waiters, first come first: with a slot each, while the
 * group has one to give (wakeupSlots(group, true)); or all of them with
 * none, to decide their group again, when it is dropped.  With the lock
 * held exclusively.
 */
static void
wake_waiters(ResGroupEntry *e, bool grant)
{
	while (e->waithead >= 0)
	{
		int			procno = e->waithead;
		ResGroupProc *p = &rg_ctl->procs[procno];

		if (grant)
		{
			if (e->locked_for_drop || e->nrunning >= e->concurrency)
				break;
			e->nrunning++;
			p->granted = true;
		}
		e->waithead = p->next;
		if (e->waithead < 0)
			e->waittail = -1;
		p->next = -1;
		p->waiting = false;
		e->nwaiting--;
		SetLatch(&GetPGProcByNumber(procno)->procLatch);
	}
}

/* A dropped group's entry goes once nothing runs in it or waits for it */
static void
forget_if_gone(ResGroupEntry *e)
{
	if (!e->defined && e->nrunning == 0 && e->nbypassed == 0 &&
		e->waithead < 0)
		e->groupid = InvalidOid;
}

/* What a commit is to change: the definitions, and the cgroups they change */
typedef struct GroupChange
{
	GroupLimits *now;			/* NULL: the group is dropped */
	GroupLimits *before;		/* NULL: it is new here */
	Oid			dropped;
} GroupChange;

/*
 * What the definitions change of shared memory's groups, worked out before
 * the commit, where the catalogs can be read: each group's limits as they
 * were and as they will be, and the groups that go.
 */
static List *
changes_of(List *defs)
{
	List	   *changes = NIL;
	bool		loaded;

	LWLockAcquire(rg_lock, LW_SHARED);
	loaded = rg_ctl->loaded;
	foreach_ptr(ResGroupDef, def, defs)
	{
		ResGroupEntry *e = loaded ? find_group(def->oid) : NULL;
		GroupChange *c = palloc0(sizeof(GroupChange));

		c->before = e != NULL ? limits_of_entry(e) : NULL;
		changes = lappend(changes, c);
	}
	LWLockRelease(rg_lock);

	/* parsing an io_limit reads the catalogs: not under the lock */
	foreach_ptr(ResGroupDef, def, defs)
	{
		GroupChange *c = list_nth(changes, foreach_current_index(def));

		c->now = limits_of_def(def, c->before == NULL ||
							   strcmp(c->before->io_limit, def->io_limit) != 0);
	}
	return changes;
}

/*
 * The default cpuset group's cores (gpdb/1): the parent's that no group's
 * cpuset holds, or the lowest of them where the groups hold all, as
 * Cloudberry's CREATE, ALTER and DROP leave it (CpusetDifference()).
 */
static void
default_cpuset_refresh(List *defs)
{
	char		all[MaxCpuSetLength] = {0};
	Bitmapset  *rest;
	MemoryContext oldcxt = CurrentMemoryContext;

	if (cgroupOpsRoutine == NULL || !gp_resource_group_enable_cgroup_cpuset)
		return;
	PG_TRY();
	{
		cgroupOpsRoutine->getcpuset(CGROUP_ROOT_ID, all, MaxCpuSetLength);
		rest = ResGroupCpusetToBitset(all, MaxCpuSetLength);
		foreach_ptr(ResGroupDef, def, defs)
		{
			if (strcmp(def->cpuset, DefaultCpuset) != 0)
				rest = bms_del_members(rest,
									   ResGroupCpusetToBitset(ResGroupCpusetOfRole(def->cpuset),
															  MaxCpuSetLength));
		}
		if (bms_is_empty(rest))
			rest = bms_make_singleton(bms_next_member(ResGroupCpusetToBitset(all, MaxCpuSetLength), -1));
		cgroupOpsRoutine->setcpuset(DEFAULT_CPUSET_GROUP_ID, ResGroupBitsetToCpuset(rest));
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not set the default cpuset group's cores: %s", edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();
}

/*
 * Bring shared memory in step with the definitions: a group made is added,
 * one altered takes its new limits -- and a raised concurrency lets its
 * waiters in (ResGroupAlterOnCommit()) -- and one dropped wakes its waiters
 * to look again and goes once nothing runs in it (ResGroupDropFinish()).
 * Then the cgroups, after the lock.
 */
static void
sync_groups(List *defs, List *changes)
{
	List	   *dropped = NIL;

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	for (int i = 0; i < MaxResourceGroups; i++)
		rg_ctl->groups[i].defined = false;
	foreach_ptr(ResGroupDef, def, defs)
	{
		ResGroupEntry *e = find_group(def->oid);

		if (e == NULL)
		{
			e = find_group(InvalidOid);
			if (e == NULL)
			{
				elog(LOG, "gp_resource: no room for resource group \"%s\" in shared memory",
					 def->name);
				continue;
			}
			memset(e, 0, sizeof(*e));
			e->groupid = def->oid;
			e->waithead = e->waittail = -1;
		}
		set_caps(e, def);
		e->defined = true;
		e->locked_for_drop = false;
		wake_waiters(e, true);
	}
	for (int i = 0; i < MaxResourceGroups; i++)
	{
		ResGroupEntry *e = &rg_ctl->groups[i];

		if (!OidIsValid(e->groupid) || e->defined)
			continue;
		dropped = lappend_oid(dropped, e->groupid);
		wake_waiters(e, false);
		forget_if_gone(e);
	}
	rg_ctl->loaded = true;
	LWLockRelease(rg_lock);

	if (cgroupOpsRoutine == NULL)
		return;
	foreach_ptr(GroupChange, c, changes)
	{
		if (c->before == NULL || c->before->cpu_max_percent != c->now->cpu_max_percent ||
			c->before->cpu_weight != c->now->cpu_weight ||
			strcmp(c->before->cpuset, c->now->cpuset) != 0 ||
			strcmp(c->before->io_limit, c->now->io_limit) != 0)
			cgroup_apply_quietly(c->now, c->before, InvalidOid);
	}
	foreach_oid(groupid, dropped)
		cgroup_apply_quietly(NULL, NULL, groupid);
	default_cpuset_refresh(defs);
}

/*
 * The first backend to need the groups reads them in, and makes their
 * cgroups (InitResGroups()); a segment's knows only what dispatches tell it.
 */
static void
ensure_loaded(void)
{
	List	   *defs;

	if (rg_ctl == NULL || rg_ctl->loaded || GpResourceIsSegment())
		return;
	defs = ResGroupDefsLoad();
	sync_groups(defs, changes_of(defs));
}

/* What this transaction's statements did to the definitions, for its commit */
static bool defs_changed = false;
static List *pending_defs = NIL;
static List *pending_changes = NIL;
static Oid	drop_locked = InvalidOid;

void
ResGroupDefsChanged(void)
{
	defs_changed = true;
}

/* ------------------------------------------------------------------------- */
/* DDL: what CREATE, ALTER and DROP RESOURCE GROUP ask of the groups         */
/* ------------------------------------------------------------------------- */

/*
 * validateCapabilities(): a cpuset's cores are the system's, and no other
 * group's.
 */
void
ResGroupValidate(List *defs, ResGroupDef *def)
{
	Bitmapset  *mine;
	char		all[MaxCpuSetLength] = {0};

	if (strcmp(def->cpuset, DefaultCpuset) == 0 || !IsResGroupEnabled() ||
		!gp_resource_group_enable_cgroup_cpuset || cgroupOpsRoutine == NULL)
		return;

	mine = ResGroupCpusetToBitset(ResGroupCpusetOfRole(def->cpuset), MaxCpuSetLength);
	cgroupOpsRoutine->getcpuset(CGROUP_ROOT_ID, all, MaxCpuSetLength);
	{
		Bitmapset  *missing = bms_difference(mine,
											 ResGroupCpusetToBitset(all, MaxCpuSetLength));

		if (!bms_is_empty(missing))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cpu cores %s are unavailable on the system",
							ResGroupBitsetToCpuset(missing))));
	}
	foreach_ptr(ResGroupDef, other, defs)
	{
		Bitmapset  *common;

		if (other->oid == def->oid || strcmp(other->cpuset, DefaultCpuset) == 0)
			continue;
		common = bms_intersect(mine,
							   ResGroupCpusetToBitset(ResGroupCpusetOfRole(other->cpuset),
													  MaxCpuSetLength));
		if (!bms_is_empty(common))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cpu cores %s are used by resource group %s",
							ResGroupBitsetToCpuset(common), other->name)));
	}
}

void
ResGroupCreated(ResGroupDef *def)
{
}

void
ResGroupAltered(ResGroupDef *old, ResGroupDef *def, ResGroupLimitType type)
{
}

void
ResGroupDropped(Oid groupid)
{
}

/*
 * ResGroupCheckForDrop(): a group that runs a query is not dropped; one
 * that does not is locked until the DROP ends, so that a transaction that
 * would run in it waits.
 */
void
ResGroupCheckDrop(Oid groupid, const char *name)
{
	ResGroupEntry *e;

	if (!GpResourceIsCoordinator() || rg_ctl == NULL)
		return;
	ensure_loaded();
	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (e != NULL && e->nrunning + e->nbypassed > 0)
	{
		int			nquery = e->nrunning + e->nbypassed + e->nwaiting;

		LWLockRelease(rg_lock);
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop resource group \"%s\"", name),
				 errhint(" The resource group is currently managing %d query(ies) and cannot be dropped.\n"
						 "\tTerminate the queries first or try dropping the group later.\n"
						 "\tThe view pg_stat_activity tracks the queries managed by resource groups.",
						 nquery)));
	}
	if (e != NULL)
	{
		e->locked_for_drop = true;
		drop_locked = groupid;
	}
	LWLockRelease(rg_lock);
}

/* ------------------------------------------------------------------------- */
/* The coordinator: a transaction's slot                                     */
/* ------------------------------------------------------------------------- */

/* This transaction's: its group, and how it runs there */
static bool xact_decided = false;
static Oid	my_group = InvalidOid;
static bool my_bypassed = false;
static TimestampTz wait_start = 0;

/* selfIsAssigned(): the transaction holds a slot -- running bypassed is not */
bool
ResGroupIsAssigned(void)
{
	return OidIsValid(my_group) && !my_bypassed;
}

/*
 * decideResGroup(): the group the current role names, where it is still
 * there, and admin_group for a superuser or default_group otherwise.
 */
static Oid
decide_group(void)
{
	Oid			roleid = GetUserId();
	Oid			groupid;
	bool		found;

	/*
	 * GetResGroupIdForRole(): a role another backend dropped runs nothing.
	 * Its drop may have come as the backend waited for a slot, which the
	 * backend reads of here, as Cloudberry's does opening pg_authid.
	 */
	AcceptInvalidationMessages();
	if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(roleid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with Oid %u was dropped", roleid),
				 errdetail("Cannot execute commands anymore, please terminate this session.")));
	groupid = GetResGroupForRole(roleid);

	LWLockAcquire(rg_lock, LW_SHARED);
	found = find_group(groupid) != NULL && find_group(groupid)->defined;
	LWLockRelease(rg_lock);
	if (!found)
		groupid = superuser() ? ADMINRESGROUP_OID : DEFAULTRESGROUP_OID;
	return groupid;
}

/* checkBypassWalker(): a SELECT whose relations are all pg_catalog's */
static bool
bypass_walker(Node *node, void *context)
{
	bool	   *bypass = context;

	if (node == NULL)
		return false;
	if (IsA(node, RangeVar))
	{
		RangeVar   *rv = (RangeVar *) node;

		if (rv->schemaname == NULL || strcmp(rv->schemaname, "pg_catalog") != 0)
		{
			*bypass = false;
			return true;
		}
		*bypass = true;
	}
	return raw_expression_tree_walker(node, bypass_walker, context);
}

/*
 * shouldBypassQuery(): every statement of the string a SET, RESET or SHOW,
 * or a SELECT of pg_catalog's tables alone -- at least one -- and anything
 * under gp_resource_group_bypass.
 */
static bool
query_is_bypassed(const char *query_string)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	List	   *parsetrees;
	bool		bypass = true;

	if (gp_resource_group_bypass)
		return true;
	if (query_string == NULL)
		return false;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "resgroup bypass check",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);
	parsetrees = pg_parse_query(query_string);
	foreach_node(RawStmt, raw, parsetrees)
	{
		Node	   *stmt = raw->stmt;

		if (IsA(stmt, SelectStmt))
		{
			bool		catalog = false;

			if (gp_resource_group_bypass_catalog_query)
				(void) raw_expression_tree_walker(stmt, bypass_walker, &catalog);
			if (!catalog)
			{
				bypass = false;
				break;
			}
		}
		else if (!IsA(stmt, VariableSetStmt) && !IsA(stmt, VariableShowStmt))
		{
			bypass = false;
			break;
		}
	}
	if (parsetrees == NIL)
		bypass = false;
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
	return bypass;
}

/* How long the wait was, for the group's total */
static void
add_queue_duration(ResGroupEntry *e)
{
	if (e != NULL && wait_start != 0)
		e->total_queue_us += GetCurrentTimestamp() - wait_start;
}

/*
 * groupWaitCancel(): a waiter that gives up leaves the list, and one given
 * a slot as it gave up gives it back, letting the next in.  A mover, which
 * waits for a slot for another backend, keeps the group it runs in itself.
 */
static void
wait_cancel(Oid groupid, bool mover)
{
	ResGroupProc *me = my_proc();
	ResGroupEntry *e;

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (me->waiting && e != NULL)
	{
		int			prev = -1;

		for (int procno = e->waithead; procno >= 0;
			 procno = rg_ctl->procs[procno].next)
		{
			if (procno == MyProcNumber)
			{
				if (prev < 0)
					e->waithead = me->next;
				else
					rg_ctl->procs[prev].next = me->next;
				if (e->waittail == MyProcNumber)
					e->waittail = prev;
				break;
			}
			prev = procno;
		}
		me->next = -1;
		me->waiting = false;
		e->nwaiting--;
		add_queue_duration(e);
	}
	else if (me->granted && e != NULL)
	{
		me->granted = false;
		e->nrunning--;
		e->total_executed++;
		add_queue_duration(e);
		wake_waiters(e, true);
	}
	me->granted = false;
	if (!mover)
		me->groupid = InvalidOid;
	if (e != NULL)
		forget_if_gone(e);
	LWLockRelease(rg_lock);
	wait_start = 0;
}

/*
 * waitOnGroup(): on the backend's latch, until a slot is given it or the
 * group goes, for as long as gp_resource_group_queuing_timeout allows.
 */
static void
wait_for_slot(Oid groupid, bool mover)
{
	ResGroupProc *me = my_proc();
	const char *old_status = NULL;
	int			len = 0;

	if (update_process_title)
	{
		old_status = get_ps_display(&len);
		old_status = pnstrdup(old_status, len);
		set_ps_display(psprintf("%s queuing", old_status));
	}

	pgstat_report_wait_start(rg_wait_event);
	PG_TRY();
	{
		for (;;)
		{
			bool		waiting;
			long		timeout = -1;

			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();

			LWLockAcquire(rg_lock, LW_SHARED);
			waiting = me->waiting;
			LWLockRelease(rg_lock);
			if (!waiting)
				break;

			if (gp_resource_group_queuing_timeout > 0)
			{
				long		waited = (long) ((GetCurrentTimestamp() - wait_start) / 1000);

				timeout = gp_resource_group_queuing_timeout - waited;
				if (timeout < 0)
					ereport(ERROR,
							(errcode(ERRCODE_QUERY_CANCELED),
							 errmsg("canceling statement due to resource group waiting timeout")));
			}
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
							 (timeout >= 0 ? WL_TIMEOUT : 0),
							 timeout, rg_wait_event);
		}
	}
	PG_CATCH();
	{
		pgstat_report_wait_end();
		if (update_process_title && old_status != NULL)
			set_ps_display(old_status);
		wait_cancel(groupid, mover);
		PG_RE_THROW();
	}
	PG_END_TRY();
	pgstat_report_wait_end();
	if (update_process_title && old_status != NULL)
		set_ps_display(old_status);
}

/*
 * groupAcquireSlot(): a slot of the group, at once where it has one free and
 * is not being dropped, or after a wait.  False where the group went while
 * the backend waited: the caller decides the group again.
 */
static bool
acquire_slot(Oid groupid)
{
	ResGroupProc *me = my_proc();
	ResGroupEntry *e;

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (e == NULL || !e->defined)
	{
		LWLockRelease(rg_lock);
		return false;
	}
	if (!e->locked_for_drop && e->nrunning < e->concurrency)
	{
		e->nrunning++;
		e->total_executed++;
		me->groupid = groupid;
		me->rsgid = groupid;
		me->has_slot = true;
		LWLockRelease(rg_lock);
		return true;
	}

	/* the end of the group's list */
	me->groupid = groupid;
	me->rsgid = groupid;
	me->wait_group = groupid;
	me->waiting = true;
	me->granted = false;
	me->next = -1;
	if (e->waittail < 0)
		e->waithead = MyProcNumber;
	else
		rg_ctl->procs[e->waittail].next = MyProcNumber;
	e->waittail = MyProcNumber;
	e->nwaiting++;
	if (!e->locked_for_drop)
		e->total_queued++;
	wait_start = GetCurrentTimestamp();
	LWLockRelease(rg_lock);

	wait_for_slot(groupid, false);

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (!me->granted)
	{
		/* woken by a DROP: nothing is held */
		me->groupid = InvalidOid;
		if (e != NULL)
		{
			add_queue_duration(e);
			forget_if_gone(e);
		}
		LWLockRelease(rg_lock);
		wait_start = 0;
		return false;
	}
	me->granted = false;
	me->has_slot = true;
	if (e != NULL)
	{
		add_queue_duration(e);
		e->total_executed++;
	}
	LWLockRelease(rg_lock);
	wait_start = 0;
	return true;
}

/*
 * groupIncBypassedRef(): the transaction runs in the group without a slot,
 * counted as one the group ran unless it gave a slot back to (the slot
 * counted it).  A group being dropped is waited out, a little at a time,
 * where Cloudberry retries at once.
 */
static void
enter_bypassed(bool count)
{
	ResGroupProc *me = my_proc();

	for (;;)
	{
		Oid			groupid = decide_group();
		ResGroupEntry *e;

		LWLockAcquire(rg_lock, LW_EXCLUSIVE);
		e = find_group(groupid);
		if (e != NULL && e->defined && !e->locked_for_drop)
		{
			e->nbypassed++;
			if (count)
				e->total_executed++;
			me->groupid = groupid;
			me->rsgid = groupid;
			me->bypassed = true;
			LWLockRelease(rg_lock);
			my_group = groupid;
			my_bypassed = true;
			return;
		}
		LWLockRelease(rg_lock);
		CHECK_FOR_INTERRUPTS();
		pg_usleep(10000L);
	}
}

static int	group_cpu_max_percent(Oid groupid);
static void group_words(StringInfo buf, Oid groupid);
static void move_poll(void);
static void move_signal_handler(SIGNAL_ARGS);
static void move_exit(void);

/*
 * The backend in its group's cgroup, as it takes the slot or bypasses.  The
 * cgroup code writes the backend's pid only for a group other than the one
 * it last put the backend in; so where a mover has put the backend in
 * another group's since (move_session()), the code is told of that one
 * first, and then writes the pid for the group asked for.
 */
static void
attach_cgroup(Oid groupid, int cpu_max_percent)
{
	Oid			moved = InvalidOid;

	if (cgroupOpsRoutine == NULL)
		return;
	if (have_proc())
	{
		LWLockAcquire(rg_lock, LW_EXCLUSIVE);
		moved = my_proc()->moved_into;
		my_proc()->moved_into = InvalidOid;
		LWLockRelease(rg_lock);
	}
	if (OidIsValid(moved) && moved != groupid)
		cgroupOpsRoutine->attachcgroup(moved, MyProcPid,
									   group_cpu_max_percent(moved) == CPU_MAX_PERCENT_DISABLED);
	cgroupOpsRoutine->attachcgroup(groupid, MyProcPid,
								   cpu_max_percent == CPU_MAX_PERCENT_DISABLED);
}

static int
group_cpu_max_percent(Oid groupid)
{
	ResGroupEntry *e;
	int			pct = 0;

	LWLockAcquire(rg_lock, LW_SHARED);
	e = find_group(groupid);
	if (e != NULL)
		pct = e->cpu_max_percent;
	LWLockRelease(rg_lock);
	return pct;
}

/*
 * The transaction's first statement, on the coordinator
 * (AssignResGroupOnMaster()): the transaction runs bypassed, or takes a
 * slot of its group, waiting for one.
 */
void
ResGroupStatementStart(const char *query_string)
{
	Oid			groupid;

	move_poll();
	if (xact_decided || !IsResGroupEnabled() || rg_ctl == NULL ||
		!GpResourceIsCoordinator() || MyBackendType != B_BACKEND ||
		!IsNormalProcessingMode() || proc_exit_inprogress ||
		!IsTransactionState() || !have_proc() || my_proc()->waiting)
		return;
	xact_decided = true;
	if (rg_wait_event == 0)
		rg_wait_event = WaitEventExtensionNew("ResourceGroup");
	ensure_loaded();

	if (query_is_bypassed(query_string))
	{
		enter_bypassed(true);
		attach_cgroup(my_group, group_cpu_max_percent(my_group));
		return;
	}

	do
		groupid = decide_group();
	while (!acquire_slot(groupid));
	my_group = groupid;
	my_bypassed = false;

	(void) GP_FAULT("resgroup_assigned_on_master");
	attach_cgroup(my_group, group_cpu_max_percent(my_group));
}

/*
 * UnassignResGroup(): the slot given back, or the bypass ended; its fault,
 * unassign_resgroup_end_qd, after a slot the coordinator gives back, and
 * not for a transaction that ran bypassed.
 */
static void
unassign(void)
{
	ResGroupProc *me;
	ResGroupEntry *e;
	bool		had_slot;

	if (!OidIsValid(my_group) || !have_proc())
	{
		my_group = InvalidOid;
		my_bypassed = false;
		return;
	}
	me = my_proc();
	had_slot = !my_bypassed && me->has_slot;
	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(my_group);
	if (e != NULL)
	{
		if (my_bypassed)
			e->nbypassed--;
		else if (me->has_slot)
		{
			e->nrunning--;
			wake_waiters(e, true);
		}
		forget_if_gone(e);
	}
	me->has_slot = false;
	me->bypassed = false;
	me->groupid = InvalidOid;
	me->rsgid = InvalidOid;
	LWLockRelease(rg_lock);
	my_group = InvalidOid;
	my_bypassed = false;

	if (had_slot && GpResourceIsCoordinator())
		(void) GP_FAULT("unassign_resgroup_end_qd");
}

static bool
funcexpr_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
		return true;
	return expression_tree_walker(node, funcexpr_walker, context);
}

/* is_pure_catalog_plan(): a SELECT of the catalogs alone, no functions */
static bool
plan_is_catalog_only(PlannedStmt *stmt)
{
	if (stmt->commandType != CMD_SELECT)
		return false;
	if (funcexpr_walker((Node *) stmt->planTree->targetlist, NULL))
		return false;
	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		if (rte->rtekind == RTE_FUNCTION || rte->rtekind == RTE_TABLEFUNC)
			return false;
		if (rte->rtekind != RTE_RELATION)
			continue;
		if (rte->relkind == RELKIND_MATVIEW)
			return false;
		if (rte->relkind == RELKIND_VIEW)
			continue;
		if (!IsCatalogRelationOid(rte->relid))
			return false;
	}
	return true;
}

/*
 * A query planned, on the coordinator (check_and_unassign_from_resgroup()):
 * outside a transaction block, and not inside another statement, a query
 * that costs less than its group's min_cost -- its cost as Cloudberry's would
 * count it, without what the planner charges its gathers for starting
 * (GpPlanCostLessGathers()) -- that goes to one segment, or that reads the
 * catalogs alone gives its slot back and runs bypassed.
 */
void
ResGroupExecutorStart(QueryDesc *queryDesc, bool toplevel)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	int			min_cost = 0;
	ResGroupEntry *e;

	if (!IsResGroupEnabled() || rg_ctl == NULL || !GpResourceIsCoordinator())
		return;
	move_poll();
	(void) GP_FAULT("check_and_unassign_from_resgroup_entry");
	if (!OidIsValid(my_group) || my_bypassed || !toplevel || IsTransactionBlock())
		return;

	LWLockAcquire(rg_lock, LW_SHARED);
	e = find_group(my_group);
	if (e != NULL)
		min_cost = e->min_cost;
	LWLockRelease(rg_lock);

	if (!(GpPlanCostLessGathers(stmt) < min_cost) &&
		!(gp_resource_group_bypass_direct_dispatch && GpPlanIsDirectDispatch(stmt)) &&
		!(gp_resource_group_bypass_catalog_query && plan_is_catalog_only(stmt)))
		return;

	unassign();
	enter_bypassed(false);
	attach_cgroup(my_group, group_cpu_max_percent(my_group));
}

/*
 * ResourceGroupGetQueryMemoryLimit(): the memory a query of the group is
 * given, in kB -- statement_mem where it runs bypassed or the group has no
 * memory_quota, gp_resgroup_memory_query_fixed_mem where that is set, and
 * otherwise the quota over the concurrency, but never less than
 * statement_mem.  0 where no group gives it any.
 */
int
ResGroupQueryBudgetKB(void)
{
	ResGroupEntry *e;
	int64		kb;

	if (!IsResGroupEnabled() || rg_ctl == NULL || !OidIsValid(my_group) ||
		gp_resgroup_memory_policy == RESMANAGER_MEMORY_POLICY_NONE)
		return 0;
	if (my_bypassed)
		return gp_statement_mem;
	if (gp_resgroup_memory_query_fixed_mem > 0)
		return gp_resgroup_memory_query_fixed_mem;

	LWLockAcquire(rg_lock, LW_SHARED);
	e = find_group(my_group);
	kb = e == NULL || e->memory_quota < 0 || e->concurrency <= 0 ? -1 :
		(int64) e->memory_quota * 1024 / e->concurrency;
	LWLockRelease(rg_lock);
	if (kb < gp_statement_mem)
		return gp_statement_mem;
	return (int) Min(kb, (int64) MAX_KILOBYTES);
}

/*
 * What the dispatch tells a segment of the group (SerializeResGroupInfo()):
 * "group=6437 caps=20/20/100/-1/500 cpuset=-1" -- concurrency,
 * cpu_max_percent, cpu_weight, memory_quota, min_cost -- or nothing where
 * the transaction runs in none.
 */
void
ResGroupDispatchInfo(StringInfo buf)
{
	if (!IsResGroupEnabled() || rg_ctl == NULL || !OidIsValid(my_group))
		return;
	group_words(buf, my_group);
}

/* ------------------------------------------------------------------------- */
/* A segment: the group the dispatch names                                   */
/* ------------------------------------------------------------------------- */

/*
 * What a segment is told of a group, in gp_resource.statement and by a move:
 * its OID, its limits, and its io_limit where it has one (group_here()).
 */
static void
group_words(StringInfo buf, Oid groupid)
{
	ResGroupEntry *e;

	LWLockAcquire(rg_lock, LW_SHARED);
	e = find_group(groupid);
	if (e != NULL)
	{
		appendStringInfo(buf, "%sgroup=%u caps=%d/%d/%d/%d/%d cpuset=%s",
						 buf->len > 0 ? " " : "", e->groupid, e->concurrency,
						 e->cpu_max_percent, e->cpu_weight, e->memory_quota,
						 e->min_cost, e->cpuset);
		if (strcmp(e->io_limit, DefaultIOLimit) != 0)
			appendStringInfo(buf, " io=%s", e->io_limit);
	}
	LWLockRelease(rg_lock);
}

/*
 * The group a coordinator names -- gp_resource.statement's words of it,
 * "group=6437 caps=20/20/100/-1/500 cpuset=-1", and " io=..." where it has
 * an io_limit (group_words()) -- made this node's: its entry, with the
 * limits the coordinator gave, and a group this node has not seen, or has
 * seen other limits of, has its cgroup made and set as the coordinator's is,
 * its io_limit on the disks of this node's tablespaces.  The group's OID,
 * and its cpu_max_percent; InvalidOid where the words are none of that.
 */
static Oid
group_here(const char *words, int *cpu_max_percent_out)
{
	unsigned int groupid;
	int			concurrency,
				cpu_max_percent,
				cpu_weight,
				memory_quota,
				min_cost;
	char		cpuset[MaxCpuSetLength];
	char		io_limit[MaxCpuSetLength] = DefaultIOLimit;
	const char *io;
	ResGroupEntry *e;
	GroupLimits *before = NULL;
	GroupLimits now;
	bool		fresh = false;

	if (words == NULL ||
		sscanf(words, "group=%u caps=%d/%d/%d/%d/%d cpuset=%1023s", &groupid,
			   &concurrency, &cpu_max_percent, &cpu_weight, &memory_quota,
			   &min_cost, cpuset) != 7)
		return InvalidOid;
	if ((io = strstr(words, " io=")) != NULL)
		strlcpy(io_limit, io + strlen(" io="),
				Min(sizeof(io_limit), strcspn(io + strlen(" io="), " ") + 1));

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (e == NULL && (e = find_group(InvalidOid)) != NULL)
	{
		memset(e, 0, sizeof(*e));
		e->groupid = groupid;
		e->waithead = e->waittail = -1;
		strlcpy(e->io_limit, DefaultIOLimit, sizeof(e->io_limit));
		fresh = true;
	}
	if (e != NULL)
	{
		if (!fresh)
			before = limits_of_entry(e);
		e->defined = true;
		e->concurrency = concurrency;
		e->cpu_max_percent = cpu_max_percent;
		e->cpu_weight = cpu_weight;
		e->memory_quota = memory_quota;
		e->min_cost = min_cost;
		strlcpy(e->cpuset, cpuset, sizeof(e->cpuset));
		strlcpy(e->io_limit, io_limit, sizeof(e->io_limit));
	}
	LWLockRelease(rg_lock);

	if (e != NULL && cgroupOpsRoutine != NULL &&
		(fresh || before->cpu_max_percent != cpu_max_percent ||
		 before->cpu_weight != cpu_weight || strcmp(before->cpuset, cpuset) != 0 ||
		 strcmp(before->io_limit, io_limit) != 0))
	{
		memset(&now, 0, sizeof(now));
		now.groupid = groupid;
		now.cpu_max_percent = cpu_max_percent;
		now.cpu_weight = cpu_weight;
		now.cpuset = cpuset;
		now.io_limit = io_limit;
		cgroup_apply_quietly(&now, fresh ? NULL : before, InvalidOid);
	}
	*cpu_max_percent_out = cpu_max_percent;
	return groupid;
}

/*
 * SwitchResGroupOnSegment(): the backend moves into the group the
 * coordinator's runs in, which it learns of, and of its limits, from the
 * dispatch.
 */
void
ResGroupSegmentStatementStart(void)
{
	const char *g;
	unsigned int named;
	Oid			groupid;
	int			cpu_max_percent;
	ResGroupProc *me;

	if (!IsResGroupEnabled() || rg_ctl == NULL || !GpResourceIsSegment() ||
		!have_proc() || gp_resource_statement == NULL)
		return;
	g = strstr(gp_resource_statement, "group=");
	if (g == NULL || sscanf(g, "group=%u", &named) != 1)
		return;
	me = my_proc();
	if (me->groupid == named && OidIsValid(my_group))
		return;

	/*
	 * A mover moved the backend out of the group named (move_session()):
	 * the coordinator's backend names the group it had as its statement
	 * began, for as long as the statement sends commands, where
	 * Cloudberry's names the one it has now; so the backend stays where it
	 * was moved to, until the coordinator names that one or another.
	 */
	LWLockAcquire(rg_lock, LW_SHARED);
	if (OidIsValid(me->moved_from) && me->moved_from == named)
	{
		LWLockRelease(rg_lock);
		return;
	}
	LWLockRelease(rg_lock);

	groupid = group_here(g, &cpu_max_percent);
	if (!OidIsValid(groupid))
		return;
	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	me->groupid = groupid;
	me->rsgid = groupid;
	LWLockRelease(rg_lock);
	my_group = groupid;
	attach_cgroup(groupid, cpu_max_percent);
}

/* ------------------------------------------------------------------------- */
/* Transaction and process ends                                              */
/* ------------------------------------------------------------------------- */

static void
resgroup_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			/* the definitions as this transaction leaves them */
			if (defs_changed && rg_ctl != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				pending_defs = ResGroupDefsLoad();
				pending_changes = changes_of(pending_defs);
				MemoryContextSwitchTo(oldcxt);
			}
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			if (pending_defs != NIL &&
				(event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT))
				sync_groups(pending_defs, pending_changes);
			else if (OidIsValid(drop_locked) && rg_ctl != NULL)
			{
				/* a DROP that did not happen lets its group go on */
				ResGroupEntry *e;

				LWLockAcquire(rg_lock, LW_EXCLUSIVE);
				e = find_group(drop_locked);
				if (e != NULL)
				{
					e->locked_for_drop = false;
					wake_waiters(e, true);
				}
				LWLockRelease(rg_lock);
			}
			pending_defs = NIL;
			pending_changes = NIL;
			defs_changed = false;
			drop_locked = InvalidOid;

			/* an offer to move the transaction comes too late now */
			move_poll();

			/* the slot, given back; a segment's backend leaves its group */
			if (GpResourceIsSegment())
			{
				if (have_proc())
				{
					LWLockAcquire(rg_lock, LW_EXCLUSIVE);
					my_proc()->groupid = InvalidOid;
					my_proc()->rsgid = InvalidOid;
					my_proc()->moved_from = InvalidOid;
					LWLockRelease(rg_lock);
				}
				my_group = InvalidOid;
			}
			else
				unassign();
			xact_decided = false;
			break;
		default:
			break;
	}
}

/* AtProcExit_ResGroup(): whatever the backend held or waited for */
static void
resgroup_exit(int code, Datum arg)
{
	if (!have_proc())
		return;
	pqsignal(SIGUSR2, PG_SIG_IGN);
	move_exit();
	if (my_proc()->waiting || my_proc()->granted)
		wait_cancel(my_proc()->wait_group, false);
	unassign();
	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	my_proc()->rsgid = InvalidOid;
	my_proc()->moved_into = InvalidOid;
	LWLockRelease(rg_lock);
}

void
ResGroupInit(void)
{
	RegisterXactCallback(resgroup_xact_callback, NULL);
}

/* The backend, as it first runs something (InitResGroups()'s own part) */
void
ResGroupBackendStart(void)
{
	static bool registered = false;

	if (registered || rg_ctl == NULL)
		return;
	before_shmem_exit(resgroup_exit, 0);
	registered = true;
	if (have_proc())
	{
		ResGroupProc *me = my_proc();

		LWLockAcquire(rg_lock, LW_EXCLUSIVE);
		me->rsgid = InvalidOid;
		me->moved_into = InvalidOid;
		me->moved_from = InvalidOid;
		LWLockRelease(rg_lock);
		SpinLockAcquire(&me->move_mutex);
		me->moveto_group = InvalidOid;
		me->moveto_slot = false;
		me->move_caller = 0;
		me->move_caller_procno = -1;
		SpinLockRelease(&me->move_mutex);
		/* where a transaction may be moved to another group: see below */
		if (IsResGroupEnabled() && GpResourceIsDispatcher())
			pqsignal(SIGUSR2, move_signal_handler);
	}
	/* the settings the cgroup code needs of every process */
	if (cgroupOpsRoutine != NULL)
		cgroupOpsRoutine->adjustgucs();
}

/* ------------------------------------------------------------------------- */
/* Cpusets                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * CpusetToBitset(): "1,3-5" is cores 1, 3, 4 and 5; NULL where the text is
 * not a cpuset.
 */
Bitmapset *
ResGroupCpusetToBitset(const char *cpuset, int len)
{
	int			pos = 0;
	int			num1 = 0;
	int			num2 = 0;
	enum
	{
		Initial,
		Begin,
		Number,
		Interval,
		Number2
	}			s = Initial;
	Bitmapset  *bms = NULL;

	if (cpuset == NULL || len <= 0)
		return NULL;
	while (pos < len && cpuset[pos])
	{
		char		c = cpuset[pos++];

		if (c == ',')
		{
			if (s == Initial || s == Begin)
				continue;
			else if (s == Interval)
				return NULL;
			else if (s == Number)
			{
				bms = bms_add_member(bms, num1);
				num1 = 0;
				s = Begin;
			}
			else if (s == Number2)
			{
				if (num1 > num2)
					return NULL;
				for (int i = num1; i <= num2; ++i)
					bms = bms_add_member(bms, i);
				num1 = num2 = 0;
				s = Begin;
			}
		}
		else if (c == '-')
		{
			if (s != Number)
				return NULL;
			s = Interval;
		}
		else if (isdigit((unsigned char) c))
		{
			if (s == Initial || s == Begin)
				s = Number;
			else if (s == Interval)
				s = Number2;
			if (s == Number)
				num1 = num1 * 10 + (c - '0');
			else
				num2 = num2 * 10 + (c - '0');
		}
		else if (c == '\n')
			break;
		else
			return NULL;
	}
	if (s == Number)
		bms = bms_add_member(bms, num1);
	else if (s == Number2)
	{
		if (num1 > num2)
			return NULL;
		for (int i = num1; i <= num2; ++i)
			bms = bms_add_member(bms, i);
	}
	else if (s == Interval)
		return NULL;
	return bms;
}

/* BitsetToCpuset(): cores 1, 3, 4 and 5 are "1,3-5" */
char *
ResGroupBitsetToCpuset(const Bitmapset *bms)
{
	StringInfoData buf;
	int			first = -1;
	int			last = -2;
	int			i = -1;

	initStringInfo(&buf);
	for (;;)
	{
		i = bms_next_member(bms, i);
		if (i >= 0 && i == last + 1)
		{
			last = i;
			continue;
		}
		if (first >= 0)
		{
			if (buf.len > 0)
				appendStringInfoChar(&buf, ',');
			if (first == last)
				appendStringInfo(&buf, "%d", first);
			else
				appendStringInfo(&buf, "%d-%d", first, last);
		}
		if (i < 0)
			break;
		first = last = i;
	}
	return buf.data;
}

bool
ResGroupCpusetIsValid(const char *cpuset)
{
	return ResGroupCpusetToBitset(cpuset, strlen(cpuset)) != NULL;
}

/* EnsureCpusetIsAvailable(ERROR) */
void
ResGroupEnsureCpusetIsAvailable(void)
{
	if (!IsResGroupEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("resource group must be enabled to use cpuset feature")));
	if (!gp_resource_group_enable_cgroup_cpuset)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cgroup is not properly configured to use the cpuset feature"),
				 errhint("Extra cgroup configurations are required to enable this feature, "
						 "please refer to the Cloudberry Documentations for details")));
}

/*
 * An io_limit as the cgroup code parses it and writes it back, as
 * Cloudberry keeps it: each tablespace by its OID, and every one of its four
 * limits, "max" where none is given ("1663:rbps=1000,wbps=1000,riops=max,
 * wiops=max").
 */
char *
ResGroupNormalizeIoLimit(const char *io_limit)
{
	if (!IsResGroupEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("resource group must be enabled to use io limit feature")));
	if (cgroupOpsRoutine != NULL && strcmp(io_limit, DefaultIOLimit) != 0)
	{
		List	   *limits = cgroupOpsRoutine->parseio(io_limit);
		char	   *dumped = cgroupOpsRoutine->dumpio(limits);

		cgroupOpsRoutine->freeio(limits);
		return dumped;
	}
	return pstrdup(io_limit);
}

/*
 * checkTablespaceInIOlimit(tablespace, true), as DROP TABLESPACE asks it on
 * cgroup v2: a tablespace a group's io_limit names is not dropped, the
 * groups named in Cloudberry's message.
 */
void
ResGroupCheckTablespaceDrop(Oid tablespace)
{
	StringInfoData log;

	if (!IsResGroupEnabled() || cgroupOpsRoutine == NULL ||
		gp_resource_manager_policy != RESOURCE_MANAGER_POLICY_GROUP_V2)
		return;

	initStringInfo(&log);
	foreach_ptr(ResGroupDef, def, ResGroupDefsLoad())
	{
		List	   *limits;
		bool		names = false;

		if (def->io_limit == NULL || strcmp(def->io_limit, DefaultIOLimit) == 0)
			continue;
		limits = cgroupOpsRoutine->parseio(def->io_limit);
		foreach_ptr(TblSpcIOLimit, limit, limits)
		{
			if (limit->tablespace_oid == tablespace)
				names = true;
		}
		cgroupOpsRoutine->freeio(limits);
		if (!names)
			continue;
		if (log.len == 0)
			appendStringInfo(&log, "io limit: following resource groups depend on tablespace %s:",
							 get_tablespace_name(tablespace));
		appendStringInfo(&log, " %s", def->name);
	}
	if (log.len > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("%s", log.data),
				 errhint("you can remove those resource groups or remove tablespace %s from io_limit of those resource groups.",
						 get_tablespace_name(tablespace))));
	pfree(log.data);
}

Oid
ResGroupNewOid(void)
{
	Oid			result = GetNewObjectId();

	/*
	 * Cloudberry's fault in GetNewObjectId(), which its tests of a group
	 * whose OID is past a signed int's take one by: the OID given, moved
	 * there, the counter left where it is.
	 */
	if (GP_FAULT("bump_oid") == GP_FAULT_SKIP && result <= PG_INT32_MAX)
		result = PG_INT32_MAX + result % (PG_UINT32_MAX - PG_INT32_MAX) + 1;
	return result;
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_resource_resgroup_status);
PG_FUNCTION_INFO_V1(gp_resource_resgroup_status_kv);
PG_FUNCTION_INFO_V1(gp_resource_resgroup_iostats);
PG_FUNCTION_INFO_V1(gp_resource_activity_rsgid);
PG_FUNCTION_INFO_V1(gp_resource_resgroup_move_query);

/* calcCpuUsage(): of the cores this host has, how much, in percent */
static double
cpu_usage(int64 begin, TimestampTz tbegin, int64 end, TimestampTz tend)
{
	int64		duration = tend - tbegin;

	if (duration <= 0 || cgroupOpsRoutine == NULL)
		return 0;
	return cgroupOpsRoutine->convertcpuusage(end - begin, duration);
}

/*
 * pg_resgroup_get_status(groupid): each group, or the one asked for --
 * its running and queueing transactions, how many it has queued and run,
 * how long its transactions have waited, and its CPU and memory now, over a
 * third of a second, as JSON keyed by the node's content id.  The counts
 * are the coordinator's; the usage is this host's cgroups', which the
 * coordinator and every segment on it share, and is given for each node
 * the cluster has, as Cloudberry's asks each segment for its own.
 */
Datum
gp_resource_resgroup_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			want = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	List	   *defs;
	int			n;
	int64	   *cpu_begin;
	TimestampTz *t_begin;
	int			i;
	const GpSegmentConfig *nodes;
	int			nnodes;

	InitMaterializedSRF(fcinfo, 0);
	if (!IsResGroupEnabled() || rg_ctl == NULL)
		return (Datum) 0;
	ensure_loaded();

	defs = ResGroupDefsLoad();
	n = list_length(defs);
	cpu_begin = palloc0(sizeof(int64) * Max(n, 1));
	t_begin = palloc0(sizeof(TimestampTz) * Max(n, 1));
	if (cgroupOpsRoutine != NULL)
	{
		i = 0;
		foreach_ptr(ResGroupDef, def, defs)
		{
			if (!OidIsValid(want) || want == def->oid)
			{
				cpu_begin[i] = cgroupOpsRoutine->getcpuusage(def->oid);
				t_begin[i] = GetCurrentTimestamp();
			}
			i++;
		}
		pg_usleep(300000);
	}
	nodes = GpClusterSegments(&nnodes);

	i = -1;
	foreach_ptr(ResGroupDef, def, defs)
	{
		Datum		values[8];
		bool		nulls[8] = {false};
		ResGroupEntry *e;
		Interval   *queued;
		StringInfoData cpu;
		StringInfoData mem;
		double		usage = 0;
		double		memory = 0;

		i++;
		if (OidIsValid(want) && want != def->oid)
			continue;

		values[0] = ObjectIdGetDatum(def->oid);
		queued = palloc0(sizeof(Interval));
		LWLockAcquire(rg_lock, LW_SHARED);
		e = find_group(def->oid);
		values[1] = Int32GetDatum(e != NULL ? e->nrunning + e->nbypassed : 0);
		values[2] = Int32GetDatum(e != NULL ? e->nwaiting : 0);
		values[3] = Int64GetDatum(e != NULL ? e->total_queued : 0);
		values[4] = Int64GetDatum(e != NULL ? e->total_executed : 0);
		queued->time = e != NULL ? e->total_queue_us : 0;
		LWLockRelease(rg_lock);
		values[5] = IntervalPGetDatum(queued);

		if (cgroupOpsRoutine != NULL)
		{
			usage = cpu_usage(cpu_begin[i], t_begin[i],
							  cgroupOpsRoutine->getcpuusage(def->oid),
							  GetCurrentTimestamp());
			memory = cgroupOpsRoutine->getmemoryusage(def->oid) / 1024.0 / 1024.0;
		}
		initStringInfo(&cpu);
		initStringInfo(&mem);
		appendStringInfo(&cpu, "{\"-1\":%.2f", usage);
		appendStringInfo(&mem, "{\"-1\":%.2f", memory);
		for (int k = 0; k < nnodes; k++)
		{
			appendStringInfo(&cpu, ", \"%d\":%.2f", nodes[k].content, usage);
			appendStringInfo(&mem, ", \"%d\":%.2f", nodes[k].content, memory);
		}
		appendStringInfoChar(&cpu, '}');
		appendStringInfoChar(&mem, '}');
		values[6] = CStringGetTextDatum(cpu.data);
		values[7] = CStringGetTextDatum(mem.data);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * ResGroupDumpInfo(): this node's groups, as JSON -- each group's running
 * and bypassed transactions, whether a DROP holds it, its waiters and its
 * limits -- in the keys Cloudberry's dump has for them.  Its slots and their
 * free list are none of this port's: a backend holds its group's slot as a
 * count (ResGroupEntry), and "slots" lists the backends that hold one.
 */
static void
dump_node(StringInfo str)
{
	const GpSegmentConfig *coordinator = GpClusterCoordinator();
	const GpSegmentConfig *nodes;
	int			nnodes;
	int			on_coordinator = 0;
	bool		first = true;

	nodes = GpClusterSegments(&nnodes);
	for (int i = 0; coordinator != NULL && i < nnodes; i++)
		if (nodes[i].role == 'p' &&
			strcmp(nodes[i].hostname, coordinator->hostname) == 0)
			on_coordinator++;

	appendStringInfo(str, "{\"segid\":%d,", GpClusterContentId());
	appendStringInfo(str, "\"segmentsOnMaster\":%d,", on_coordinator);

	LWLockAcquire(rg_lock, LW_SHARED);
	appendStringInfo(str, "\"loaded\":%s,", rg_ctl->loaded ? "true" : "false");
	appendStringInfoString(str, "\"groups\":[");
	for (int i = 0; i < MaxResourceGroups; i++)
	{
		const ResGroupEntry *e = &rg_ctl->groups[i];
		bool		first_waiter = true;

		if (!OidIsValid(e->groupid) || !e->defined)
			continue;
		if (!first)
			appendStringInfoChar(str, ',');
		first = false;
		appendStringInfo(str, "{\"group_id\":%u,", e->groupid);
		appendStringInfo(str, "\"nRunning\":%d,", e->nrunning);
		appendStringInfo(str, "\"nRunningBypassed\":%d,", e->nbypassed);
		appendStringInfo(str, "\"locked_for_drop\":%d,", e->locked_for_drop ? 1 : 0);
		appendStringInfo(str, "\"wait_queue\":{\"wait_queue_size\":%d,", e->nwaiting);
		appendStringInfoString(str, "\"wait_queue_content\":[");
		for (int p = e->waithead; p >= 0; p = rg_ctl->procs[p].next)
		{
			appendStringInfo(str, "%s{\"pid\":%d,\"resWaiting\":%s}",
							 first_waiter ? "" : ",",
							 GetPGProcByNumber(p)->pid,
							 rg_ctl->procs[p].waiting ? "true" : "false");
			first_waiter = false;
		}
		appendStringInfoString(str, "]},");
		appendStringInfo(str, "\"caps\":{\"concurrency\":%d,\"cpu_max_percent\":%d,"
						 "\"cpu_weight\":%d,\"memory_quota\":%d,\"min_cost\":%d,"
						 "\"cpuset\":",
						 e->concurrency, e->cpu_max_percent, e->cpu_weight,
						 e->memory_quota, e->min_cost);
		escape_json(str, e->cpuset);
		appendStringInfoString(str, "}}");
	}
	appendStringInfoString(str, "],\"slots\":[");
	first = true;
	for (int p = 0; p < MaxBackends; p++)
	{
		const ResGroupProc *proc = &rg_ctl->procs[p];

		if (!OidIsValid(proc->groupid) || (!proc->has_slot && !proc->bypassed))
			continue;
		appendStringInfo(str, "%s{\"pid\":%d,\"groupId\":%u,\"bypassed\":%s}",
						 first ? "" : ",", GetPGProcByNumber(p)->pid,
						 proc->groupid, proc->bypassed ? "true" : "false");
		first = false;
	}
	LWLockRelease(rg_lock);
	appendStringInfoString(str, "]}");
}

/*
 * pg_resgroup_get_status_kv(prop): with 'dump', one row, whose value is
 * every primary's groups as JSON, {"info":[<the coordinator's>, <each
 * segment's>]}, which a superuser only may see; with anything else, none.
 */
Datum
gp_resource_resgroup_status_kv(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		dump;
	StringInfoData str;
	Datum		values[3];
	bool		nulls[3] = {true, true, false};

	dump = !PG_ARGISNULL(0) &&
		strncmp(text_to_cstring(PG_GETARG_TEXT_PP(0)), "dump", 4) == 0;
	if (dump && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superusers can call this function")));

	InitMaterializedSRF(fcinfo, 0);
	if (!dump || !IsResGroupEnabled() || rg_ctl == NULL)
		return (Datum) 0;

	initStringInfo(&str);
	if (GpResourceIsCoordinator() && GpClusterSegmentCount() > 0)
	{
		int			nsegs = GpClusterSegmentCount();
		char	  **segs = palloc0_array(char *, nsegs);

		GpDispatchQueryFirstValues("SELECT value FROM pg_catalog.pg_resgroup_get_status_kv('dump')",
								   -1, segs);
		appendStringInfoString(&str, "{\"info\":[");
		dump_node(&str);
		for (int i = 0; i < nsegs; i++)
		{
			if (segs[i] == NULL)
				ereport(ERROR,
						(errmsg("pg_resgroup_get_status_kv(): a segment gave no dump")));
			appendStringInfo(&str, ",%s", segs[i]);
		}
		appendStringInfoString(&str, "]}");
	}
	else if (GpResourceIsSegment())
		dump_node(&str);
	else
	{
		appendStringInfoString(&str, "{\"info\":[");
		dump_node(&str);
		appendStringInfoString(&str, "]}");
	}

	values[0] = (Datum) 0;
	values[1] = (Datum) 0;
	values[2] = CStringGetTextDatum(str.data);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return (Datum) 0;
}

/* getIOLimitStats(): the I/O of this node's groups that have an io_limit */
static List *
io_stats(void)
{
	List	   *result = NIL;
	List	   *groups = NIL;
	List	   *limits = NIL;
	ListCell   *g;
	ListCell   *l;

	LWLockAcquire(rg_lock, LW_SHARED);
	for (int i = 0; i < MaxResourceGroups; i++)
	{
		ResGroupEntry *e = &rg_ctl->groups[i];

		if (!OidIsValid(e->groupid) || !e->defined ||
			strcmp(e->io_limit, DefaultIOLimit) == 0)
			continue;
		groups = lappend_oid(groups, e->groupid);
		limits = lappend(limits, pstrdup(e->io_limit));
	}
	LWLockRelease(rg_lock);

	/* parsed and not validated, as getiostat() fills in the disks itself */
	forboth(g, groups, l, limits)
		result = list_concat(result,
							 cgroupOpsRoutine->getiostat(lfirst_oid(g),
														 io_limit_parse((char *) lfirst(l))));
	return result;
}

/*
 * gp_toolkit.__gp_resgroup_iostats(): each group's I/O on this node, in
 * bytes and operations a second over one second, by tablespace, of the
 * groups whose io_limit names any -- Cloudberry's pg_resgroup_get_iostats()
 * (gp_toolkit's resgroup.c).  A segment knows a group's io_limit as the
 * dispatch told it, and not the group's name, which the view takes from the
 * coordinator's pg_resgroup.
 */
Datum
gp_resource_resgroup_iostats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *before;
	List	   *after;
	TimestampTz start;
	double		interval;
	ListCell   *b;
	ListCell   *a;

	InitMaterializedSRF(fcinfo, 0);
	if (!IsResGroupEnabled() || rg_ctl == NULL || cgroupOpsRoutine == NULL)
		return (Datum) 0;
	if (!GpResourceIsSegment())
		ensure_loaded();

	start = GetCurrentTimestamp();
	before = io_stats();
	pg_usleep(1000000L);
	after = io_stats();
	interval = (GetCurrentTimestamp() - start) / 1000000.0;
	if (list_length(before) != list_length(after))
		ereport(ERROR, (errmsg("stats count differs between runs")));
	list_sort(before, compare_iostat);
	list_sort(after, compare_iostat);

	forboth(b, before, a, after)
	{
		IOStat	   *s0 = (IOStat *) lfirst(b);
		IOStat	   *s1 = (IOStat *) lfirst(a);
		Datum		values[8];
		bool		nulls[8] = {false};
		char	   *name = NULL;
		char	   *tablespace = "*";

		if (s0->groupid != s1->groupid || s0->tablespace != s1->tablespace)
			ereport(ERROR,
					(errmsg("get different result from io.stat after little interval")));
		if (!GpResourceIsSegment())
			name = ResGroupNameByOid(s0->groupid);
		if (OidIsValid(s0->tablespace))
			tablespace = get_tablespace_name(s0->tablespace);

		values[0] = Int32GetDatum(GpClusterContentId());
		values[1] = name != NULL ? CStringGetTextDatum(name) : (Datum) 0;
		nulls[1] = name == NULL;
		values[2] = ObjectIdGetDatum(s0->groupid);
		values[3] = tablespace != NULL ? CStringGetTextDatum(tablespace) : (Datum) 0;
		nulls[3] = tablespace == NULL;
		values[4] = Int64GetDatum((int64) ((s1->items.rbytes - s0->items.rbytes) / interval));
		values[5] = Int64GetDatum((int64) ((s1->items.wbytes - s0->items.wbytes) / interval));
		values[6] = Int64GetDatum((int64) ((s1->items.rios - s0->items.rios) / interval));
		values[7] = Int64GetDatum((int64) ((s1->items.wios - s0->items.wios) / interval));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * rsgid of pg_stat_activity's row: the group the backend runs in or waits
 * for, 0 for none, as Cloudberry's backend entry has it -- which keeps the
 * group a backend gave up waiting for, until it next runs in one.
 */
Datum
gp_resource_activity_rsgid(PG_FUNCTION_ARGS)
{
	HeapTupleHeader row = PG_GETARG_HEAPTUPLEHEADER(0);
	TupleDesc	desc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(row),
											  HeapTupleHeaderGetTypMod(row));
	HeapTupleData tuple;
	bool		isnull;
	int			pid = 0;
	Oid			groupid = InvalidOid;

	tuple.t_len = HeapTupleHeaderGetDatumLength(row);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = row;
	for (int i = 0; i < desc->natts; i++)
	{
		if (strcmp(NameStr(TupleDescAttr(desc, i)->attname), "pid") == 0)
		{
			Datum		d = heap_getattr(&tuple, i + 1, desc, &isnull);

			if (!isnull)
				pid = DatumGetInt32(d);
			break;
		}
	}
	ReleaseTupleDesc(desc);

	if (pid != 0 && rg_ctl != NULL)
	{
		PGPROC	   *proc = BackendPidGetProc(pid);

		if (proc != NULL)
		{
			ProcNumber	procno = GetNumberFromPGProc(proc);

			if (procno >= 0 && procno < MaxBackends)
			{
				LWLockAcquire(rg_lock, LW_SHARED);
				groupid = rg_ctl->procs[procno].rsgid;
				LWLockRelease(rg_lock);
			}
		}
	}
	PG_RETURN_OID(groupid);
}

/* ------------------------------------------------------------------------- */
/* Moving a transaction to another group                                     */
/* ------------------------------------------------------------------------- */

/*
 * pg_resgroup_move_query(pid, group), as Cloudberry's moves a transaction:
 * the mover takes a slot of the group for it, waiting for one as a
 * transaction would, and offers it to the target -- the backend on the
 * coordinator of the session whose pid it is given -- which gives back its
 * own slot, takes the new one and goes into the group's cgroup; then the
 * mover puts the session's other backends, on the segments and any on the
 * coordinator, in the group's cgroup too.  Offering the slot is Cloudberry's
 * handshake: the offer in the target's shared state (moveto_*), a signal,
 * and the mover waiting, as long as gp_resource_group_move_timeout gives it,
 * for the target to say it has looked; a target that has not taken the slot
 * by then never takes it, and the mover gives it back.
 *
 * The target has to act as it runs whatever it runs, and PostgreSQL 19 has
 * no interrupt a module may add, nor a hook in CHECK_FOR_INTERRUPTS(); so
 * the signal is SIGUSR2, which a backend otherwise ignores, and its handler
 * -- gp_resource's -- takes the slot, where Cloudberry's SIGUSR1 handler
 * does.  A signal handler must not come upon the backend inside malloc(),
 * an LWLock or anything else it would use itself, so the handler acts only
 * where the backend waits on its latch (the latch's maybe_sleeping, which
 * WaitEventSetWait() sets just before it sleeps) holding no LWLock and in no
 * critical section; anywhere else the offer waits for the backend's next
 * statement or transaction end, which look for one, or for the mover, which
 * sends the signal again every MOVE_RESIGNAL_MS as it waits.  So a target
 * that computes without waiting takes the slot at its statement's end.
 */
#define MOVE_RESIGNAL_MS	100

static volatile sig_atomic_t move_pending = false;
static volatile sig_atomic_t move_taking = false;

/* A move taken since the statement set gp_resource.statement (gp_resource.c) */
static volatile sig_atomic_t dispatch_stale = false;

/* A mover's slot for its target, until the target takes it */
static Oid	move_slot_group = InvalidOid;
static int	move_target_procno = -1;

static void move_take(void);

/* The mover's latch, set to tell it the target has looked */
static void
move_notify(int caller_procno)
{
	if (caller_procno >= 0 && caller_procno < MaxBackends)
		SetLatch(&GetPGProcByNumber(caller_procno)->procLatch);
}

/* SIGUSR2: a mover's offer (see above) */
static void
move_signal_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	move_pending = true;
	if (!move_taking && InterruptHoldoffCount == 0 && CritSectionCount == 0 &&
		MyLatch != NULL && MyLatch->maybe_sleeping && have_proc())
		move_take();
	SetLatch(MyLatch);
	errno = save_errno;
}

/* A statement or a transaction's end: an offer the handler put off */
static void
move_poll(void)
{
	if (move_pending && !move_taking && have_proc())
		move_take();
}

/*
 * HandleMoveResourceGroup(), the target's part: outside a transaction, or
 * holding no slot, it says it has looked and takes nothing; else, where the
 * mover still offers the slot, it takes it -- saying so first, since from
 * then on the slot is the target's -- gives back its own, letting a waiter
 * of that group in, and goes into the new group's cgroup.
 */
static void
move_take(void)
{
	ResGroupProc *me = my_proc();
	ResGroupEntry *e;
	bool		slot;
	Oid			groupid;
	int			caller;

	move_taking = true;
	move_pending = false;

	SpinLockAcquire(&me->move_mutex);
	groupid = me->moveto_group;
	caller = me->move_caller_procno;
	if (me->move_caller == 0 || !OidIsValid(groupid))
	{
		/* no offer, or one looked at already */
		SpinLockRelease(&me->move_mutex);
		move_taking = false;
		return;
	}
	if (!IsTransactionState() || !ResGroupIsAssigned())
	{
		/* the transaction has ended: the offer is of nothing */
		me->moveto_group = InvalidOid;
		SpinLockRelease(&me->move_mutex);
		move_notify(caller);
		move_taking = false;
		return;
	}
	SpinLockRelease(&me->move_mutex);

	(void) GP_FAULT("resource_group_move_handler_before_qd_control");

	SpinLockAcquire(&me->move_mutex);
	slot = me->moveto_slot;
	groupid = me->moveto_group;
	caller = me->move_caller_procno;
	/* looked at, and the slot taken, where it is still offered */
	me->moveto_slot = false;
	me->moveto_group = InvalidOid;
	SpinLockRelease(&me->move_mutex);
	if (!slot || !OidIsValid(groupid))
	{
		/* the mover gave up */
		move_taking = false;
		return;
	}

	(void) GP_FAULT("resource_group_move_handler_after_qd_control");
	move_notify(caller);

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(my_group);
	if (e != NULL)
	{
		e->nrunning--;
		wake_waiters(e, true);
		forget_if_gone(e);
	}
	me->groupid = groupid;
	me->rsgid = groupid;
	me->has_slot = true;
	LWLockRelease(rg_lock);
	my_group = groupid;
	my_bypassed = false;
	dispatch_stale = true;
	attach_cgroup(groupid, group_cpu_max_percent(groupid));
	move_taking = false;
}

/*
 * ResGroupMoveSignalTarget(), the mover's: the offer in the target's state,
 * and the signal.  False where the target has gone, is being moved already,
 * or cannot be signalled, with Cloudberry's NOTICE of which.
 */
static bool
move_offer(int pid, int procno, Oid groupid)
{
	ResGroupProc *tp = &rg_ctl->procs[procno];

	if (GetPGProcByNumber(procno)->pid != pid)
	{
		ereport(NOTICE, (errmsg("cannot find target process")));
		return false;
	}
	SpinLockAcquire(&tp->move_mutex);
	if (tp->move_caller != 0)
	{
		SpinLockRelease(&tp->move_mutex);
		ereport(NOTICE, (errmsg("cannot move process, which is already moving")));
		return false;
	}
	tp->moveto_slot = true;
	tp->moveto_group = groupid;
	tp->move_caller = MyProcPid;
	tp->move_caller_procno = MyProcNumber;
	SpinLockRelease(&tp->move_mutex);

	if (kill(pid, SIGUSR2) != 0)
	{
		SpinLockAcquire(&tp->move_mutex);
		tp->moveto_slot = false;
		tp->moveto_group = InvalidOid;
		tp->move_caller = 0;
		SpinLockRelease(&tp->move_mutex);
		ereport(NOTICE,
				(errmsg("cannot send signal to backend %d with PID %d",
						procno, pid)));
		return false;
	}
	return true;
}

/*
 * The signal again, to a target that has not looked at the offer yet; false
 * where it has, whose word the mover may have missed as it reset its latch.
 */
static bool
move_resignal(int pid, int procno)
{
	ResGroupProc *tp = &rg_ctl->procs[procno];
	bool		again;

	SpinLockAcquire(&tp->move_mutex);
	again = tp->move_caller == MyProcPid && OidIsValid(tp->moveto_group);
	SpinLockRelease(&tp->move_mutex);
	if (again && GetPGProcByNumber(procno)->pid == pid)
		(void) kill(pid, SIGUSR2);
	return again;
}

/*
 * ResGroupMoveCheckTargetReady(): whether the target has looked at the
 * offer, and so is done with it (*clean), and whether it took the slot
 * (*result); an offer that is done with, or that the mover gives up on
 * (*clean given true), is withdrawn, which lets another mover make one.
 */
static void
move_check(int procno, bool *clean, bool *result)
{
	ResGroupProc *tp = &rg_ctl->procs[procno];

	*result = false;
	SpinLockAcquire(&tp->move_mutex);
	if (tp->move_caller == MyProcPid)
	{
		if (!OidIsValid(tp->moveto_group))
		{
			*result = !tp->moveto_slot;
			*clean = true;
		}
		if (*clean)
		{
			tp->moveto_slot = false;
			tp->moveto_group = InvalidOid;
			tp->move_caller = 0;
		}
	}
	SpinLockRelease(&tp->move_mutex);
}

/*
 * groupAcquireSlot(..., isMoveQuery): a slot of the group, for another
 * backend -- the mover's own group and slot stay as they are.
 */
static bool
move_acquire_slot(Oid groupid)
{
	ResGroupProc *me = my_proc();
	ResGroupEntry *e;

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (e == NULL || !e->defined)
	{
		LWLockRelease(rg_lock);
		return false;
	}
	if (!e->locked_for_drop && e->nrunning < e->concurrency)
	{
		e->nrunning++;
		e->total_executed++;
		LWLockRelease(rg_lock);
		return true;
	}

	me->wait_group = groupid;
	me->waiting = true;
	me->granted = false;
	me->next = -1;
	if (e->waittail < 0)
		e->waithead = MyProcNumber;
	else
		rg_ctl->procs[e->waittail].next = MyProcNumber;
	e->waittail = MyProcNumber;
	e->nwaiting++;
	if (!e->locked_for_drop)
		e->total_queued++;
	wait_start = GetCurrentTimestamp();
	LWLockRelease(rg_lock);

	wait_for_slot(groupid, true);

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (!me->granted)
	{
		if (e != NULL)
		{
			add_queue_duration(e);
			forget_if_gone(e);
		}
		LWLockRelease(rg_lock);
		wait_start = 0;
		return false;
	}
	me->granted = false;
	if (e != NULL)
	{
		add_queue_duration(e);
		e->total_executed++;
	}
	LWLockRelease(rg_lock);
	wait_start = 0;
	return true;
}

/* Whether a move was taken since the last ask, which it answers once */
bool
ResGroupDispatchStale(void)
{
	bool		stale = dispatch_stale;

	dispatch_stale = false;
	return stale;
}

/* A statement's end: an offer the handler put off (gp_resource.c) */
void
ResGroupMovePoll(void)
{
	if (IsResGroupEnabled() && rg_ctl != NULL)
		move_poll();
}

static void move_release_slot(Oid groupid);
static void move_check(int procno, bool *clean, bool *result);

/*
 * A backend's exit: a slot it holds as a mover and its target has not
 * taken, given back; and an offer to it, which it will not take, answered.
 */
static void
move_exit(void)
{
	ResGroupProc *me = my_proc();
	int			caller = -1;

	if (OidIsValid(move_slot_group) && move_target_procno >= 0)
	{
		bool		clean = true;
		bool		result = false;

		move_check(move_target_procno, &clean, &result);
		if (!result)
			move_release_slot(move_slot_group);
		move_slot_group = InvalidOid;
	}
	SpinLockAcquire(&me->move_mutex);
	if (me->move_caller != 0 && OidIsValid(me->moveto_group))
	{
		me->moveto_group = InvalidOid;
		caller = me->move_caller_procno;
	}
	SpinLockRelease(&me->move_mutex);
	if (caller >= 0)
		move_notify(caller);
}

/* groupReleaseSlot(..., isMoveQuery): the mover's slot, back */
static void
move_release_slot(Oid groupid)
{
	ResGroupEntry *e;

	LWLockAcquire(rg_lock, LW_EXCLUSIVE);
	e = find_group(groupid);
	if (e != NULL)
	{
		e->nrunning--;
		wake_waiters(e, true);
		forget_if_gone(e);
	}
	LWLockRelease(rg_lock);
}

/*
 * resGroupGiveSlotAway(): the offer made, and waited on.  A mover that
 * errors out after the target has taken the slot leaves it to the target,
 * with Cloudberry's WARNING.
 */
static void
move_give_slot_away(int pid, int procno, Oid groupid)
{
	TimestampTz start;
	bool		clean;
	bool		result = false;

	(void) GP_FAULT("resource_group_give_away_begin");
	if (!move_offer(pid, procno, groupid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot send signal to process")));

	start = GetCurrentTimestamp();
	for (;;)
	{
		int			rc = WL_TIMEOUT;
		long		left;

		left = gp_resource_group_move_timeout -
			(long) ((GetCurrentTimestamp() - start) / 1000);
		if (left > 0)
		{
			PG_TRY();
			{
				(void) GP_FAULT("resource_group_give_away_wait_latch");
				for (;;)
				{
					CHECK_FOR_INTERRUPTS();
					left = gp_resource_group_move_timeout -
						(long) ((GetCurrentTimestamp() - start) / 1000);
					if (left <= 0)
					{
						rc = WL_TIMEOUT;
						break;
					}
					rc = WaitLatch(MyLatch,
								   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								   Min(left, MOVE_RESIGNAL_MS), rg_wait_event);
					if (rc & WL_LATCH_SET)
						break;
					if (!move_resignal(pid, procno))
					{
						rc = WL_LATCH_SET;
						break;
					}
				}
			}
			PG_CATCH();
			{
				clean = true;
				move_check(procno, &clean, &result);
				if (result)
				{
					move_slot_group = InvalidOid;
					ereport(WARNING,
							(errmsg("got exception, but slot control is on the target process side"),
							 errhint("QEs weren't moved. They'll be moved by the next command dispatched in the target transaction, if any.")));
				}
				PG_RE_THROW();
			}
			PG_END_TRY();
		}

		(void) GP_FAULT("resource_group_give_away_after_latch");

		clean = (rc & WL_TIMEOUT) != 0;
		move_check(procno, &clean, &result);
		if (clean)
			break;
		ResetLatch(MyLatch);
	}

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("target process failed to move to a new group")));
	move_slot_group = InvalidOid;
}

/* A pid written to a group's cgroup: the cgroup code's attachcgroup()'s files */
static void
attach_pid(Oid groupid, int cpu_max_percent, int pid)
{
	if (cgroupOpsRoutine == NULL)
		return;
	if (gp_resource_manager_policy == RESOURCE_MANAGER_POLICY_GROUP_V2)
	{
		writeInt64(groupid, BASEDIR_GPDB, CGROUP_COMPONENT_PLAIN,
				   CGROUPV2_LEAF_INDENTIFIER "/cgroup.procs", pid);
		return;
	}
	writeInt64(groupid, BASEDIR_GPDB, CGROUP_COMPONENT_CPU, "cgroup.procs", pid);
	writeInt64(groupid, BASEDIR_GPDB, CGROUP_COMPONENT_CPUACCT, "cgroup.procs", pid);
	writeInt64(groupid, BASEDIR_GPDB, CGROUP_COMPONENT_MEMORY, "cgroup.procs", pid);
	if (gp_resource_group_enable_cgroup_cpuset)
		writeInt64(cpu_max_percent == CPU_MAX_PERCENT_DISABLED ? groupid :
				   DEFAULT_CPUSET_GROUP_ID,
				   BASEDIR_GPDB, CGROUP_COMPONENT_CPUSET, "cgroup.procs", pid);
}

/*
 * The executors of a session on this node -- a segment's, or the
 * coordinator's other than the target -- in the group a mover names, in
 * gp_resource.statement's words of it: each that runs in a group now is put
 * in this one, its cgroup and its state, as Cloudberry's executors move
 * themselves when signalled.  One whose pid can no longer be written, having
 * ended, is passed over.
 */
static void
move_session_here(int session, const char *words, int except_pid)
{
	Oid			groupid;
	int			cpu_max_percent;

	groupid = group_here(words, &cpu_max_percent);
	if (!OidIsValid(groupid))
		return;
	for (int n = 0; n < MaxBackends; n++)
	{
		int			pid = GetPGProcByNumber(n)->pid;
		int			s;
		bool		reader;
		bool		running;

		if (pid == 0 || pid == MyProcPid || pid == except_pid ||
			!GpGddBackendIdentity(pid, &s, &reader) || s != session)
			continue;

		LWLockAcquire(rg_lock, LW_EXCLUSIVE);
		running = OidIsValid(rg_ctl->procs[n].groupid);
		if (running)
		{
			rg_ctl->procs[n].moved_from = rg_ctl->procs[n].groupid;
			rg_ctl->procs[n].groupid = groupid;
			rg_ctl->procs[n].rsgid = groupid;
			rg_ctl->procs[n].moved_into = groupid;
		}
		LWLockRelease(rg_lock);
		if (!running)
			continue;

		PG_TRY();
		{
			attach_pid(groupid, cpu_max_percent, pid);
		}
		PG_CATCH();
		{
			if (GetPGProcByNumber(n)->pid == pid)
				PG_RE_THROW();
			FlushErrorState();
		}
		PG_END_TRY();
	}
}

PG_FUNCTION_INFO_V1(gp_resource_move_session);

/*
 * gp_resource.move_session(session, words): what the mover sends each
 * segment, once the target holds the new group's slot -- Cloudberry's
 * pg_resgroup_move_query(session, group) of a QE.
 */
Datum
gp_resource_move_session(PG_FUNCTION_ARGS)
{
	if (!ResDefsCanManageGroups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to move query between resource groups"),
				 errhint("Must be superuser or have privileges of the pg_manage_resource_groups role.")));
	if (IsResGroupEnabled() && rg_ctl != NULL && have_proc())
		move_session_here(PG_GETARG_INT32(0),
						  text_to_cstring(PG_GETARG_TEXT_PP(1)), 0);
	PG_RETURN_VOID();
}

/*
 * pg_resgroup_move_query(pid, group): the transaction of the session whose
 * backend on the coordinator this is moved into the group; true once it is,
 * or already was, there.  Cloudberry's checks, in its order: groups on, the
 * caller a superuser or of pg_manage_resource_groups, not itself, not into
 * system_group, the session there, and running in a group now -- idle, or
 * waiting for a slot, it runs in none.
 */
Datum
gp_resource_resgroup_move_query(PG_FUNCTION_ARGS)
{
	int			pid = PG_GETARG_INT32(0);
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Oid			groupid;
	Oid			current = InvalidOid;
	PGPROC	   *proc;
	int			procno = -1;
	int			session = -1;
	bool		reader;
	StringInfoData words;

	if (!IsResGroupEnabled() || rg_ctl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("resource group is not enabled")));
	if (!ResDefsCanManageGroups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to move query between resource groups"),
				 errhint("Must be superuser or have privileges of the pg_manage_resource_groups role.")));
	if (!GpResourceIsDispatcher() || !have_proc())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a query is moved from the coordinator")));
	if (pid == MyProcPid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move myself")));

	groupid = ResGroupOidByName(name, false);
	proc = BackendPidGetProc(pid);
	if (proc != NULL)
	{
		procno = GetNumberFromPGProc(proc);
		if (procno < 0 || procno >= MaxBackends)
			procno = -1;
		else if (!GpGddBackendIdentity(pid, &session, &reader))
			session = GpClusterIsSingleNode() ? 0 : -1;
	}
	if (groupid == SYSTEMRESGROUP_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move a process to the system_group")));
	if (procno < 0 || session == -1)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot find process: %d", pid)));

	LWLockAcquire(rg_lock, LW_SHARED);
	if (rg_ctl->procs[procno].has_slot && !rg_ctl->procs[procno].bypassed &&
		!rg_ctl->procs[procno].waiting)
		current = rg_ctl->procs[procno].groupid;
	LWLockRelease(rg_lock);
	if (!OidIsValid(current))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("process %d is in IDLE state", pid)));
	if (current == groupid)
		PG_RETURN_BOOL(true);

	/* ResGroupMoveQuery() */
	if (rg_wait_event == 0)
		rg_wait_event = WaitEventExtensionNew("ResourceGroup");
	ensure_loaded();
	if (!move_acquire_slot(groupid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("cannot get slot in resource group %u", groupid)));
	move_slot_group = groupid;
	move_target_procno = procno;
	PG_TRY();
	{
		move_give_slot_away(pid, procno, groupid);
	}
	PG_CATCH();
	{
		if (OidIsValid(move_slot_group))
			move_release_slot(move_slot_group);
		move_slot_group = InvalidOid;
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* from here the slot is the target's: the session's executors follow */
	initStringInfo(&words);
	group_words(&words, groupid);
	if (words.len > 0)
	{
		move_session_here(session, words.data, pid);
		if (!GpClusterIsSingleNode() && GpResourceIsCoordinator())
			GpDispatchCommand(psprintf("SELECT gp_resource.move_session(%d, %s)",
									   session, quote_literal_cstr(words.data)));
	}
	PG_RETURN_BOOL(true);
}
