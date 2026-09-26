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
 * gp_standby.c
 *	  The coordinator's standby, whose commits wait for it while it streams.
 *
 * Cloudberry's coordinator waits for its standby as a primary waits for its
 * mirror -- a commit returns once the standby has flushed it -- with two
 * differences its syncrep.c and walsender.c make for the coordinator
 * (IS_QUERY_DISPATCHER()).  The standby, the WAL sender whose receiver
 * connects as gp_walreceiver, is synchronous whatever
 * synchronous_standby_names says; and only while it streams, or has caught
 * up to within gp.repl_catchup_within_range WAL segments of what the
 * coordinator has flushed.  A coordinator whose standby is gone, or far
 * behind, does not wait -- so its pair needs no FTS, and stopping the
 * standby holds no commit -- and one whose standby is promoted has had
 * every commit it acknowledged reach the standby first, which is what the
 * standby's distributed transaction recovery decides by (gp_dtx.c).
 *
 * PostgreSQL 19 waits whenever synchronous_standby_names names a standby,
 * connected or not.  So a process of the coordinator's sets it: to
 * gp_walreceiver while that WAL sender streams or has caught up within
 * range, and to nothing otherwise -- with ALTER SYSTEM and a reload, as FTS
 * turns a primary's on and off (gp_fts.c), and the reload wakes a commit
 * that was waiting for a standby that has gone.  It is woken as that WAL
 * sender starts and ends (gp_fts.c), and looks every 100 ms while one is
 * catching up.  It runs wherever a coordinator has finished its recovery, so
 * a standby that is promoted drops at once the name its copy of the
 * coordinator's configuration gave it.
 *
 * What differs.  Cloudberry decides at each commit; the port as soon as the
 * process has seen a change and the reload has been taken, a commit in
 * between waiting, or not, as the setting was.  Cloudberry's sender is
 * caught up within range once it has sent what it had since it started, the
 * port's as soon as what it is to send next is within range.  And a commit
 * that begins to wait while the sender catches up is released once it
 * streams, where Cloudberry's may be released before: PostgreSQL 19 takes a
 * standby's acknowledgement only from a sender that streams.
 *
 * On a segment, gp.repl_catchup_within_range says when FTS may turn a
 * primary's synchronous replication back on: when its mirror streams, or
 * has caught up within range (gp_fts.c), as Cloudberry's GetMirrorStatus()
 * says it.
 *
 * Cloudberry sources this file stands in for:
 *	  the IS_QUERY_DISPATCHER() cases of src/backend/replication/syncrep.c,
 *	  and WalSndIsCatchupWithinRange() in walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_fts.h"
#include "gp_standby.h"

/*
 * Cloudberry's repl_catchup_within_range: WAL segments a sender may be
 * behind and still be waited for while it catches up.  Not its upper bound,
 * INT_MAX / WalSegMaxSize, which the unparenthesized macro turns into an
 * overflow: any number of segments.
 */
static int	repl_catchup_within_range = 1;

/* The coordinator's process, which the WAL sender's start and end wake. */
typedef struct GpStandbyShared
{
	slock_t		mutex;
	ProcNumber	procno;			/* INVALID_PROC_NUMBER while none runs */
} GpStandbyShared;

static GpStandbyShared *standby_shared = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

static void
standby_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(GpStandbyShared)));
}

static void
standby_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	standby_shared = ShmemInitStruct("gp_core standby", sizeof(GpStandbyShared),
									 &found);
	if (!found)
	{
		SpinLockInit(&standby_shared->mutex);
		standby_shared->procno = INVALID_PROC_NUMBER;
	}
	LWLockRelease(AddinShmemInitLock);
}

static uint32
standby_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryStandby");
	return event;
}

bool
GpStandbyWithinRange(XLogRecPtr sent)
{
	XLogRecPtr	flushed;
	XLogSegNo	sent_seg;
	XLogSegNo	flushed_seg;

	if (XLogRecPtrIsInvalid(sent) || RecoveryInProgress())
		return false;
	flushed = GetFlushRecPtr(NULL);
	if (sent >= flushed)
		return true;
	XLByteToSeg(sent, sent_seg, wal_segment_size);
	XLByteToSeg(flushed, flushed_seg, wal_segment_size);
	return flushed_seg - sent_seg <= (XLogSegNo) repl_catchup_within_range;
}

void
GpStandbyWake(void)
{
	ProcNumber	procno;

	if (standby_shared == NULL)
		return;
	SpinLockAcquire(&standby_shared->mutex);
	procno = standby_shared->procno;
	SpinLockRelease(&standby_shared->mutex);
	if (procno != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(procno)->procLatch);
}

/* ------------------------------------------------------------------------- */
/* The coordinator's process                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Is the standby's WAL sender there, and streaming or caught up within
 * range?  Once one has caught up it stays so until it ends, as Cloudberry's
 * caughtup_within_range does: "caught_up" keeps whose it was.  "*unsettled"
 * says a sender is there that has not reached streaming, which is looked at
 * again soon.
 */
static bool
standby_wanted(int *caught_up, bool *unsettled)
{
	int			pid = GpFtsWalreceiverSender();

	*unsettled = false;
	if (pid == 0)
	{
		*caught_up = 0;
		return false;
	}

	/* started, and perhaps not yet in the table: looked at again */
	*unsettled = true;
	for (int i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		pid_t		walsnd_pid;
		WalSndState state;
		XLogRecPtr	sent;

		SpinLockAcquire(&walsnd->mutex);
		walsnd_pid = walsnd->pid;
		state = walsnd->state;
		sent = walsnd->sentPtr;
		SpinLockRelease(&walsnd->mutex);

		if (walsnd_pid != pid)
			continue;
		if (state == WALSNDSTATE_STREAMING)
		{
			*unsettled = false;
			*caught_up = pid;
			return true;
		}
		if (state == WALSNDSTATE_CATCHUP &&
			(*caught_up == pid || GpStandbyWithinRange(sent)))
		{
			*caught_up = pid;
			return true;
		}
		break;
	}
	if (*caught_up != pid)
		*caught_up = 0;
	return false;
}

/*
 * ALTER SYSTEM SET synchronous_standby_names, and a reload: the standby is
 * waited for, or not.  False, with a warning, where it could not be set --
 * with allow_alter_system off, say -- and it is tried again later.
 */
static bool
standby_set_names(bool on)
{
	AlterSystemStmt *stmt = makeNode(AlterSystemStmt);
	VariableSetStmt *set = makeNode(VariableSetStmt);
	A_Const    *value = makeNode(A_Const);
	const char *names = on ? GP_WALRECEIVER_APPNAME : "";
	MemoryContext cxt = CurrentMemoryContext;
	bool		done = false;

	value->val.sval.type = T_String;
	value->val.sval.sval = pstrdup(names);
	value->location = -1;
	set->kind = VAR_SET_VALUE;
	set->name = "synchronous_standby_names";
	set->args = list_make1(value);
	set->location = -1;
	stmt->setstmt = set;

	PG_TRY();
	{
		StartTransactionCommand();
		AlterSystemSetConfigFile(stmt);
		CommitTransactionCommand();
		done = true;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		AbortCurrentTransaction();
		ereport(WARNING,
				(errmsg("could not set synchronous_standby_names to '%s' for the standby: %s",
						names, edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(cxt);

	if (!done)
		return false;

	ereport(LOG,
			(errmsg("signaling configuration reload: setting synchronous_standby_names to '%s'",
					names)));
	if (kill(PostmasterPid, SIGHUP) != 0)
		ereport(WARNING,
				(errmsg("could not signal the postmaster to reload: %m")));
	return true;
}

static bool
standby_names_set(void)
{
	return SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0';
}

static void
standby_exit(int code, Datum arg)
{
	SpinLockAcquire(&standby_shared->mutex);
	standby_shared->procno = INVALID_PROC_NUMBER;
	SpinLockRelease(&standby_shared->mutex);
}

PGDLLEXPORT void GpStandbyMain(Datum main_arg);

void
GpStandbyMain(Datum main_arg)
{
	int			written = -1;	/* what this process last set: 1 on, 0 off */
	int			caught_up = 0;
	MemoryContext loop_cxt;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* No database: ALTER SYSTEM's checks read only the shared catalogs. */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	SpinLockAcquire(&standby_shared->mutex);
	standby_shared->procno = MyProcNumber;
	SpinLockRelease(&standby_shared->mutex);
	before_shmem_exit(standby_exit, (Datum) 0);

	loop_cxt = AllocSetContextCreate(TopMemoryContext, "gp_core standby",
									 ALLOCSET_SMALL_SIZES);

	for (;;)
	{
		bool		want;
		bool		unsettled;
		long		timeout_ms;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/* what somebody else set is set again below */
			if (written != -1 && standby_names_set() != (written == 1))
				written = -1;
		}

		want = standby_wanted(&caught_up, &unsettled);
		if (written == -1 ? want != standby_names_set() : want != (written == 1))
		{
			MemoryContext old = MemoryContextSwitchTo(loop_cxt);

			if (standby_set_names(want))
				written = want ? 1 : 0;
			MemoryContextSwitchTo(old);
			MemoryContextReset(loop_cxt);
		}
		else if (written == -1)
			written = want ? 1 : 0;

		/*
		 * A sender catching up is looked at every 100 ms, until it streams
		 * or is within range; one that failed to set is tried again in ten
		 * seconds; otherwise the sender's start or end wakes this, and a
		 * minute passes at most.
		 */
		if (written == -1 || (written == 1) != want)
			timeout_ms = 10000;
		else if (unsettled && !want)
			timeout_ms = 100;
		else if (unsettled)
			timeout_ms = 1000;
		else
			timeout_ms = 60000;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms, standby_wait_event());
		ResetLatch(MyLatch);
	}
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpStandbyInit(void)
{
	const GpSegmentConfig *self;

	DefineCustomIntVariable("gp.repl_catchup_within_range",
							"How many WAL segments a standby or mirror may lag while commits start to wait for it as it catches up.",
							"Cloudberry's repl_catchup_within_range.  On the "
							"coordinator, a standby that has caught up this far is "
							"waited for; on a segment, FTS turns synchronous "
							"replication on for a mirror that has.",
							&repl_catchup_within_range,
							1, 0, INT_MAX,
							PGC_SUSET,
							GUC_SUPERUSER_ONLY,
							NULL, NULL, NULL);

	if (GpClusterIsSingleNode())
		return;

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = standby_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = standby_shmem_startup;

	/*
	 * On a coordinator, the file's or a standby promoted since: from the end
	 * of its recovery, so never on a standby that is one still.
	 */
	self = GpClusterSelf();
	if (self != NULL && self->content == -1)
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 1;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpStandbyMain");
		snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core standby replication");
		snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core standby replication");
		RegisterBackgroundWorker(&worker);
	}
}
