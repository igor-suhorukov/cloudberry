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
 * gp_dtx_standby.c
 *	  The distributed transactions in their second phase, which a hot
 *	  standby coordinator's queries see in progress still.
 *
 * A query on the standby coordinator is dispatched to the mirrors
 * (gp_dispatch.c), under the standby's own snapshot as its distributed one:
 * the coordinator's transaction ID is the distributed one (gp_dtx.c), so
 * what a hot standby's snapshot says of the coordinator's transactions it
 * says of the distributed ones.  But the standby has a transaction
 * committed as it replays the coordinator's commit record, which is written
 * between the two phases: before any segment has committed its part, and so
 * before any mirror has.  A query then would find the transaction's rows on
 * the mirrors that had replayed its second phase and not on the others.  On
 * the coordinator itself that moment does not show, the transaction being
 * in progress for every snapshot until its second phase is done
 * (dispatch_commit_recorded(), gp_dispatch.c); the standby has to be told.
 *
 * THE SET.  So the coordinator keeps the set of the transactions in their
 * second phase, and its WAL carries it, in three records of gp_core's
 * resource manager (gp_dbcopy.h):
 *
 *   COMMITTING(gxid), as the first phase ends, before the commit record;
 *
 *   FORGET(gxid), once every part has its second phase -- Cloudberry's
 *   distributed forget record -- or the transaction did not commit after
 *   all;
 *
 *   RUNNING(the set), from time to time, as PostgreSQL logs its running
 *   transactions: from it a standby that has just started, and replays
 *   from a point after some COMMITTING, knows the set whole.
 *
 * The standby keeps the set as it replays them, and a query there sees
 * every member in progress though its snapshot has the commit: in the
 * distributed snapshot it sends the mirrors, and in the snapshot it reads
 * its own tables with (gp_dtx.c).  A mirror hides the parts of a
 * transaction the distributed snapshot sees in progress as a primary does,
 * by its distributed log.  One the standby has forgotten has committed on
 * every primary, and a mirror in sync has that commit; it replays what it
 * has before it reads (gp_dtx.c).
 *
 * The records are written wherever a standby may be: where wal_level is
 * replica or more, PostgreSQL's own rule for its running-transactions
 * record (XLogStandbyInfoActive()).  A COMMITTING is carried to disk and to
 * the standby by the commit record's flush, which follows it.  A FORGET
 * needs neither for the set to be right -- one lost hides a transaction
 * longer -- and is left to the WAL writer, but under synchronous_commit =
 * remote_apply, where the commit waits until the standby has applied it, as
 * Cloudberry's does: a query on the standby then sees a transaction whose
 * COMMIT has returned.
 *
 * WHO FORGETS.  The backend that committed, where its second phase reached
 * every part.  Where it did not, the transaction is left to the recovery
 * process (gp_dtx.c), "abandoned" here, and forgotten by it once a round of
 * its has reached every node and finished every part of the transaction it
 * found.  So is one a restart left: the set is memory, and after a restart
 * holds what the redo of the WAL since the last checkpoint gave it; the
 * recovery process adds each transaction it finds a prepared part of that
 * committed here.  Until a round has reached every node the set may lack
 * such a one, and no RUNNING is logged: a standby keeps what it has.  A
 * standby promoted has the set its redo left, every member abandoned.
 *
 * Cloudberry sources this file stands in for:
 *	  the distributed commit and forget records of src/backend/cdb/cdbtm.c
 *	  and access/transam/xact.c, their redo and the checkpoint's list of
 *	  distributed transactions in cdbdtxrecovery.c, and the standby's
 *	  distributed snapshot in storage/ipc/procarray.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"
#include "utils/xid8.h"

#include "gp_dbcopy.h"
#include "gp_dtx_standby.h"
#include "gp_fault.h"
#include "gp_standby.h"

/* How often the whole set is logged, at most: PostgreSQL's LOG_SNAPSHOT_INTERVAL_MS. */
#define GP_DTX_RUNNING_INTERVAL_MS	15000

/* One distributed transaction in its second phase. */
typedef struct GpDtxCommittingEntry
{
	uint64		gxid;
	bool		abandoned;		/* the recovery process forgets it */
} GpDtxCommittingEntry;

typedef struct GpDtxStandbyShared
{
	LWLock	   *lock;

	/*
	 * On a node in recovery: has the set been whole since the server
	 * started -- a RUNNING replayed, with room for all it named and for
	 * every COMMITTING since?
	 */
	bool		known;

	/*
	 * On a coordinator that writes: has a round of the recovery process
	 * found every transaction a restart left in its second phase?  And is
	 * the set to be logged at once because of it?
	 */
	bool		gathered;
	bool		log_now;

	/* The end of the last RUNNING logged. */
	XLogRecPtr	running_lsn;

	int			n;
	int			max;
	GpDtxCommittingEntry entries[FLEXIBLE_ARRAY_MEMBER];
} GpDtxStandbyShared;

static GpDtxStandbyShared *registry = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

/*
 * Room for every backend's own commit and as many left to the recovery
 * process again, and some: a second phase is left to it where a segment
 * failed between the phases, which fails every transaction after at its
 * first.
 */
static int
registry_max(void)
{
	return 2 * MaxBackends + 64;
}

static Size
registry_size(void)
{
	return add_size(offsetof(GpDtxStandbyShared, entries),
					mul_size(registry_max(), sizeof(GpDtxCommittingEntry)));
}

static void
registry_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(registry_size()));
	RequestNamedLWLockTranche("gp_core commits in their second phase", 1);
}

static void
registry_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	registry = ShmemInitStruct("gp_core commits in their second phase",
							   registry_size(), &found);
	if (!found)
	{
		memset(registry, 0, registry_size());
		registry->lock = &(GetNamedLWLockTranche("gp_core commits in their second phase"))->lock;
		registry->max = registry_max();
	}
	LWLockRelease(AddinShmemInitLock);
}

/* The entry of that transaction, or -1; the lock is held. */
static int
registry_find(uint64 gxid)
{
	for (int i = 0; i < registry->n; i++)
		if (registry->entries[i].gxid == gxid)
			return i;
	return -1;
}

/* Add one, unless it is there; false where there is no room.  Exclusively. */
static bool
registry_add(uint64 gxid, bool abandoned)
{
	int			i = registry_find(gxid);

	if (i >= 0)
	{
		registry->entries[i].abandoned = abandoned;
		return true;
	}
	if (registry->n >= registry->max)
		return false;
	registry->entries[registry->n].gxid = gxid;
	registry->entries[registry->n].abandoned = abandoned;
	registry->n++;
	return true;
}

/* Exclusively. */
static void
registry_remove(uint64 gxid)
{
	int			i = registry_find(gxid);

	if (i < 0)
		return;
	registry->entries[i] = registry->entries[registry->n - 1];
	registry->n--;
}

/*
 * A record of one transaction, written with the lock held, so that the
 * records are in the WAL in the order the set changed in: a RUNNING then
 * holds exactly what the records before it leave.
 */
static XLogRecPtr
registry_log(uint8 info, uint64 gxid)
{
	if (!XLogStandbyInfoActive() || RecoveryInProgress())
		return InvalidXLogRecPtr;
	XLogBeginInsert();
	XLogRegisterData(&gxid, sizeof(gxid));
	return XLogInsert(GP_CORE_RMGR_ID, info);
}

/* ------------------------------------------------------------------------- */
/* The coordinator                                                           */
/* ------------------------------------------------------------------------- */

void
GpDtxCommitting(FullTransactionId gxid)
{
	bool		added;

	if (registry == NULL)
		return;
	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	added = registry_add(U64FromFullTransactionId(gxid), false);
	if (added)
		(void) registry_log(XLOG_GP_CORE_DTX_COMMITTING,
							U64FromFullTransactionId(gxid));
	LWLockRelease(registry->lock);
	if (!added)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many distributed transactions are waiting for their second phase"),
				 errdetail("%d are committed here and have a part on a segment that is not.",
						   registry_max()),
				 errhint("Distributed transaction recovery finishes them once it reaches their segments.")));
}

void
GpDtxForget(FullTransactionId gxid, bool wait)
{
	XLogRecPtr	lsn;
	XLogRecPtr	last = XactLastRecEnd;

	if (registry == NULL)
		return;
	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	registry_remove(U64FromFullTransactionId(gxid));
	lsn = registry_log(XLOG_GP_CORE_DTX_FORGET, U64FromFullTransactionId(gxid));
	LWLockRelease(registry->lock);

	/*
	 * Written after its transaction's commit or abort record, the record is
	 * no part of what the backend's next transaction wrote: that one's
	 * commit is not to take itself for one that logged something.  The WAL
	 * writer is told of it instead, as of a commit nobody waits for.
	 */
	XactLastRecEnd = last;
	if (XLogRecPtrIsValid(lsn))
		XLogSetAsyncXactLSN(lsn);

	/*
	 * The commit that waits for the standby to have applied it waits for
	 * this as well, as a commit record is waited for: the standby says how
	 * far it has applied as it replays the record (GpDtxStandbyRedo()).
	 * Interrupts are held by the caller, as SyncRepWaitForLSN() expects of a
	 * commit.
	 */
	if (wait && XLogRecPtrIsValid(lsn) &&
		synchronous_commit >= SYNCHRONOUS_COMMIT_REMOTE_APPLY)
	{
		XLogFlush(lsn);
		SyncRepWaitForLSN(lsn, true);
	}
}

void
GpDtxAbandon(FullTransactionId gxid)
{
	int			i;

	if (registry == NULL)
		return;
	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	i = registry_find(U64FromFullTransactionId(gxid));
	if (i >= 0)
		registry->entries[i].abandoned = true;
	LWLockRelease(registry->lock);
}

/*
 * The oldest transaction in its second phase, or an invalid ID: what the
 * segments are told to keep their word of with every distributed snapshot
 * (gp_dtx.c), though no snapshot here sees it in progress -- one left to the
 * recovery process has ended for the coordinator's snapshots, and a query
 * on the standby is still to have its parts hidden by the mirrors.
 */
FullTransactionId
GpDtxCommittingOldest(void)
{
	uint64		oldest = 0;

	/*
	 * None, mostly, and then no lock: asked with every distributed snapshot.
	 * One added this moment is its backend's still, in progress for every
	 * snapshot here, and need not be found.
	 */
	if (registry == NULL || registry->n == 0)
		return InvalidFullTransactionId;
	LWLockAcquire(registry->lock, LW_SHARED);
	for (int i = 0; i < registry->n; i++)
		if (oldest == 0 || registry->entries[i].gxid < oldest)
			oldest = registry->entries[i].gxid;
	LWLockRelease(registry->lock);
	return FullTransactionIdFromU64(oldest);
}

/* ------------------------------------------------------------------------- */
/* The recovery process                                                      */
/* ------------------------------------------------------------------------- */

int
GpDtxAbandonedList(uint64 **gxids)
{
	int			n = 0;

	*gxids = palloc_array(uint64, registry == NULL ? 1 : registry->max);
	if (registry == NULL)
		return 0;
	LWLockAcquire(registry->lock, LW_SHARED);
	for (int i = 0; i < registry->n; i++)
		if (registry->entries[i].abandoned)
			(*gxids)[n++] = registry->entries[i].gxid;
	LWLockRelease(registry->lock);
	return n;
}

bool
GpDtxAnyAbandoned(void)
{
	bool		any = false;

	if (registry == NULL)
		return false;
	LWLockAcquire(registry->lock, LW_SHARED);
	for (int i = 0; i < registry->n && !any; i++)
		any = registry->entries[i].abandoned;
	LWLockRelease(registry->lock);
	return any;
}

/*
 * A part found prepared whose transaction committed here.  Its backend
 * added it as its first phase ended, unless a restart came between: one
 * nobody has is one a restart left, which no backend finishes, and is the
 * recovery process's from now.  No record: a standby has it from the
 * COMMITTING its backend wrote, or has it committed whole.
 */
bool
GpDtxNoteInDoubt(FullTransactionId gxid, bool *full)
{
	bool		added = false;

	if (registry == NULL)
		return false;
	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	if (registry_find(U64FromFullTransactionId(gxid)) < 0)
	{
		added = registry_add(U64FromFullTransactionId(gxid), true);
		if (!added)
			*full = true;
	}
	LWLockRelease(registry->lock);
	return added;
}

void
GpDtxSetGathered(bool gathered)
{
	bool		now;

	if (registry == NULL)
		return;

	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	now = gathered && !registry->gathered;
	if (now)
		registry->log_now = true;
	registry->gathered = gathered;
	LWLockRelease(registry->lock);

	/* the standby process logs it (GpDtxLogRunning()) */
	if (now)
		GpStandbyWake();
}

/* ------------------------------------------------------------------------- */
/* The whole set, logged                                                     */
/* ------------------------------------------------------------------------- */

void
GpDtxLogRunning(bool now)
{
	static TimestampTz logged_at = 0;
	uint64	   *gxids;
	uint32		n;
	bool		due;

	if (registry == NULL || !XLogStandbyInfoActive() || RecoveryInProgress())
		return;

	LWLockAcquire(registry->lock, LW_SHARED);
	due = registry->gathered &&
		(now || registry->log_now ||
		 (TimestampDifferenceExceeds(logged_at, GetCurrentTimestamp(),
									 GP_DTX_RUNNING_INTERVAL_MS) &&
		  GetLastImportantRecPtr() >= registry->running_lsn));
	LWLockRelease(registry->lock);
	if (!due)
		return;

	/*
	 * Exclusively, though the set is only read: the two fields written below
	 * are the lock's, and no COMMITTING or FORGET may come between the copy
	 * and its record.
	 */
	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	n = registry->n;
	gxids = palloc_array(uint64, Max(n, 1));
	for (uint32 i = 0; i < n; i++)
		gxids[i] = registry->entries[i].gxid;
	XLogBeginInsert();
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	XLogRegisterData(&n, sizeof(n));
	if (n > 0)
		XLogRegisterData(gxids, n * sizeof(uint64));
	registry->running_lsn = XLogInsert(GP_CORE_RMGR_ID, XLOG_GP_CORE_DTX_RUNNING);
	registry->log_now = false;
	LWLockRelease(registry->lock);
	pfree(gxids);
	logged_at = GetCurrentTimestamp();
}

/* ------------------------------------------------------------------------- */
/* Redo                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * The set as the WAL has it: on a standby, what its queries hide; in a
 * coordinator's own recovery after a crash, what the transactions since
 * the last checkpoint left, which the recovery process then takes over --
 * every member is abandoned, no backend of this server having it.
 */
void
GpDtxStandbyRedo(uint8 info, const char *data, int len)
{
	uint64		gxid;

	if (registry == NULL)
		return;

	if (info == XLOG_GP_CORE_DTX_RUNNING)
	{
		uint32		n;

		if (len < (int) sizeof(n))
			elog(PANIC, "gp_core_redo: a record of the commits in their second phase of %d bytes", len);
		memcpy(&n, data, sizeof(n));
		if (len != (int) (sizeof(n) + n * sizeof(uint64)))
			elog(PANIC, "gp_core_redo: a record of %u commits in their second phase of %d bytes",
				 n, len);

		LWLockAcquire(registry->lock, LW_EXCLUSIVE);
		registry->n = 0;
		registry->known = n <= (uint32) registry->max;
		for (uint32 i = 0; i < n && registry->known; i++)
		{
			memcpy(&gxid, data + sizeof(n) + i * sizeof(uint64), sizeof(gxid));
			(void) registry_add(gxid, true);
		}
		LWLockRelease(registry->lock);
		return;
	}

	if (len != sizeof(gxid))
		elog(PANIC, "gp_core_redo: a record of a commit in its second phase of %d bytes", len);
	memcpy(&gxid, data, sizeof(gxid));

	LWLockAcquire(registry->lock, LW_EXCLUSIVE);
	if (info == XLOG_GP_CORE_DTX_COMMITTING)
	{
		/* no room: the set is not whole until a RUNNING that fits */
		if (!registry_add(gxid, true))
			registry->known = false;
	}
	else
		registry_remove(gxid);
	LWLockRelease(registry->lock);

	if (info == XLOG_GP_CORE_DTX_FORGET)
	{
		/*
		 * A commit may be waiting until this is applied (GpDtxForget()), and
		 * a WAL receiver says how far its node has applied only when asked,
		 * as the redo of a commit record asks it.
		 */
		XLogRequestWalReceiverReply();

		/* where Cloudberry's standby replays its forget record */
		(void) GP_FAULT("redoDistributedForgetCommitRecord");
	}
}

/* ------------------------------------------------------------------------- */
/* A hot standby coordinator's queries                                       */
/* ------------------------------------------------------------------------- */

static uint32
standby_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryDtxStandby");
	return event;
}

/*
 * A standby that has just started knows the set once it has replayed a
 * RUNNING, which the coordinator logs as the standby's WAL receiver reaches
 * it, at the latest: waited for while the receiver streams, and for a
 * while before it does -- a standby replays what WAL it has before it
 * connects, and tries a coordinator that does not answer again every
 * wal_retrieve_retry_interval.  With no coordinator to stream from, none
 * will come.
 */
void
GpDtxStandbyWaitKnown(void)
{
	TimestampTz give_up = 0;

	if (registry == NULL)
		return;
	for (;;)
	{
		bool		known;

		LWLockAcquire(registry->lock, LW_SHARED);
		known = registry->known;
		LWLockRelease(registry->lock);
		if (known || !RecoveryInProgress())
			return;
		if (WalRcvStreaming())
			give_up = 0;
		else if (give_up == 0)
			give_up = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
												  2 * wal_retrieve_retry_interval);
		else if (GetCurrentTimestamp() >= give_up)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot dispatch from the standby coordinator yet"),
					 errdetail("The distributed transactions in their second phase are not yet known on this standby, and it is not streaming from its coordinator.")));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 20L, standby_wait_event());
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The members of the set a snapshot is to see in progress: every one older
 * than its xmax, the others being in progress for it already.
 *
 * Read after the snapshot was taken, which is the order that makes it
 * right.  A member the snapshot has committed had its COMMITTING replayed
 * before the snapshot was taken, the record preceding the commit's; if it
 * is still a member now it is hidden, and if it has been forgotten since,
 * every part of it has committed, and it is seen whole.
 *
 * The answer is kept: one statement dispatches more than once, and under
 * REPEATABLE READ a transaction's statements read with one snapshot, which
 * must hide the same transactions each time -- one forgotten while the
 * transaction is open stays hidden from it to its end.  So under
 * REPEATABLE READ the transaction's first answer is its answer: its
 * snapshot is one, whichever copy of it is asked about, as taken or as
 * made to hide these (dtx_craft_standby(), gp_dtx.c).  Under READ COMMITTED
 * a snapshot is known again by its numbers -- a hot standby's holds every
 * transaction in progress under its xmax, a set that only shrinks while
 * the xmax stands -- within the client's statement: the next one's
 * snapshot, though its numbers be the same, is to see what was forgotten
 * since.  The copy of it made to hide the first answer has other numbers,
 * and is answered anew; it carries the first answer itself, and whatever
 * the set has gained since is in progress for it anyway.
 */
int
GpDtxStandbyHidden(Snapshot snapshot, const uint64 **gxids)
{
	static LocalTransactionId kept_lxid = InvalidLocalTransactionId;
	static TimestampTz kept_statement = 0;
	static TransactionId kept_xmin = InvalidTransactionId;
	static TransactionId kept_xmax = InvalidTransactionId;
	static int32 kept_subxcnt = -1;
	static uint64 *kept = NULL;
	static int	kept_n = 0;
	bool		xact = IsolationUsesXactSnapshot();
	TimestampTz statement = xact ? 0 : GetCurrentStatementStartTimestamp();
	uint64		xmax;

	*gxids = NULL;
	if (registry == NULL)
		return 0;

	if (kept_lxid == MyProc->vxid.lxid && kept_statement == statement &&
		(xact || (kept_xmin == snapshot->xmin && kept_xmax == snapshot->xmax &&
				  kept_subxcnt == snapshot->subxcnt)))
	{
		*gxids = kept;
		return kept_n;
	}

	xmax = U64FromFullTransactionId(FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(),
																	  snapshot->xmax));
	if (kept == NULL)
		kept = MemoryContextAlloc(TopMemoryContext,
								  registry->max * sizeof(uint64));
	kept_n = 0;
	kept_lxid = InvalidLocalTransactionId;
	LWLockAcquire(registry->lock, LW_SHARED);
	for (int i = 0; i < registry->n; i++)
		if (registry->entries[i].gxid < xmax)
			kept[kept_n++] = registry->entries[i].gxid;
	LWLockRelease(registry->lock);

	kept_lxid = MyProc->vxid.lxid;
	kept_statement = statement;
	kept_xmin = snapshot->xmin;
	kept_xmax = snapshot->xmax;
	kept_subxcnt = snapshot->subxcnt;
	*gxids = kept;
	return kept_n;
}

/* ------------------------------------------------------------------------- */
/* Seeing it                                                                 */
/* ------------------------------------------------------------------------- */

int
GpDtxCommittingList(uint64 **gxids, bool **abandoned, bool *known,
					bool *gathered)
{
	int			n;

	*gxids = NULL;
	*abandoned = NULL;
	*known = *gathered = false;
	if (registry == NULL)
		return 0;
	LWLockAcquire(registry->lock, LW_SHARED);
	n = registry->n;
	*gxids = palloc_array(uint64, Max(n, 1));
	*abandoned = palloc_array(bool, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		(*gxids)[i] = registry->entries[i].gxid;
		(*abandoned)[i] = registry->entries[i].abandoned;
	}
	*known = registry->known;
	*gathered = registry->gathered;
	LWLockRelease(registry->lock);
	return n;
}

PG_FUNCTION_INFO_V1(gp_dtx_committing);

/*
 * gp_internal.dtx_committing()
 *		The distributed transactions in their second phase, as this node
 *		has them: a coordinator's own, each left to the recovery process or
 *		not; a standby's as its redo keeps them.  And of the node whether
 *		the set is whole there: on a standby, known since it started; on a
 *		coordinator, gathered by a round of the recovery process.
 */
Datum
gp_dtx_committing(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	uint64	   *gxids;
	bool	   *abandoned;
	bool		known;
	bool		gathered;
	int			n;

	InitMaterializedSRF(fcinfo, 0);
	n = GpDtxCommittingList(&gxids, &abandoned, &known, &gathered);
	for (int i = 0; i < n; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = FullTransactionIdGetDatum(FullTransactionIdFromU64(gxids[i]));
		values[1] = BoolGetDatum(abandoned[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_dtx_committing_whole);

/* gp_internal.dtx_committing_whole(): is the set whole on this node? */
Datum
gp_dtx_committing_whole(PG_FUNCTION_ARGS)
{
	uint64	   *gxids;
	bool	   *abandoned;
	bool		known;
	bool		gathered;

	(void) GpDtxCommittingList(&gxids, &abandoned, &known, &gathered);
	PG_RETURN_BOOL(RecoveryInProgress() ? known : gathered);
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpDtxStandbyInit(void)
{
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = registry_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = registry_shmem_startup;
}
