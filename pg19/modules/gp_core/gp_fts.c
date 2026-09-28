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
 * gp_fts.c
 *	  FTS, the fault tolerance service: the coordinator's prober, and what a
 *	  segment answers it.
 *
 * A segment is a pair: a primary, and a mirror that replays the primary's
 * WAL, for which the primary's commits wait -- synchronous replication.  FTS,
 * a process on the coordinator, asks each primary every
 * gp.fts_probe_interval how it is and how its mirror is, and acts on the
 * answer as Cloudberry's does (src/backend/fts/ftsprobe.c):
 *
 *	 - a mirror that is down no longer holds the primary's commits: FTS marks
 *	   it down, and has the primary turn synchronous replication off;
 *	 - a mirror that is back and streaming holds them again: synchronous
 *	   replication on, and the two marked in sync;
 *	 - a primary that does not answer, gp.fts_probe_retries times over, each
 *	   within gp.fts_probe_timeout, is failed over from if its mirror is in
 *	   sync: the mirror is marked the primary, the primary a mirror and down,
 *	   and the mirror is promoted.  A mirror that is not in sync is not
 *	   promoted -- "double fault" -- because it may lack commits.
 *
 * What FTS finds is the cluster's state (gp_cluster.c): written to
 * gpsegconfig_dump before anything is acted on, then to shared memory, where
 * the dispatcher reads which node of a content is its primary now.
 *
 * Cloudberry's prober speaks a protocol of its own to a kind of connection a
 * segment's postmaster accepts even in recovery.  PostgreSQL 19 has neither,
 * so the port's prober is an ordinary libpq client of an ordinary backend,
 * as its dispatcher is, and each of Cloudberry's messages is SQL.  A probe is
 * the value of a setting that is computed as it is shown, gp.fts_status --
 * as gp.dist_wait_status is the deadlock detector's -- so that it needs
 * nothing in the database it reaches; turning synchronous replication off
 * and on is ALTER SYSTEM of synchronous_standby_names and a reload, which is
 * what Cloudberry's handler does inside; and a promotion is pg_promote(),
 * which a mirror takes because it is a hot standby, PostgreSQL's default,
 * where Cloudberry's mirrors are not (the plan's Track E, section 2.2).  The
 * loss is the one the plan names: a mirror that has not reached consistency
 * cannot be connected to, so it cannot be promoted before it has.
 *
 * The five answers of Cloudberry's handler are here: whether the mirror is
 * up, whether it is in sync, whether synchronous replication is on, whether
 * the node asked is a mirror -- in recovery -- and whether FTS should ask
 * again before it marks the mirror down, which it should while the mirror
 * may still be connecting: within gp.fts_mark_mirror_down_grace_period of the
 * primary's start, or of the end of its mirror's last WAL sender.  A primary
 * tells its mirror's WAL sender by the name the mirror's WAL receiver
 * connects under, gp_walreceiver, as Cloudberry's does, and notes the
 * sender's start and end itself (fts_client_auth).  And a probe reads and
 * writes a block of a file in the data directory, as Cloudberry's does, so
 * that a primary whose disk hangs is failed over from.
 *
 * R3.  A commit on a primary waits for its mirror to have it.  PostgreSQL
 * ends the wait when the backend is cancelled, with a warning, so the commit
 * can be missing on the mirror, and lost when FTS fails over to it;
 * Cloudberry's commit waits ignore a cancel.  With R3, SyncRepWaitForLSN()
 * keeps waiting while an extension sets SyncRepHoldCancelDuringWait and holds
 * cancel interrupts: gp_core does, on a segment, from the moment a
 * transaction commits or prepares -- or a prepared one is committed or rolled
 * back -- until it has, and then drops the cancel with Cloudberry's warning,
 * since what it would have cancelled is done.  FTS is what ends a wait for a
 * mirror that is not coming back, by turning synchronous replication off.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/fts/fts.c, ftsprobe.c and ftsmessagehandler.c,
 *	  src/backend/cdb/cdbfts.c, the mirror half of
 *	  src/backend/replication/gp_replication.c, and the commit wait of
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <netdb.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_authid.h"
#include "common/ip.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/waiteventset.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_fts.h"
#include "gp_standby.h"

/* The file a probe reads and writes, and what it holds; Cloudberry's. */
#define FTS_PROBE_FILE_NAME		"fts_probe_file.bak"
#define FTS_PROBE_MAGIC_STRING	"FtS PrObEr MaGiC StRiNg, pRoBiNg cHeCk......."

/* What FTS records of each change it makes, in the coordinator's data directory. */
#define FTS_HISTORY_FILE		"gp_configuration_history"

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

static int	gp_fts_probe_interval = 60;
static int	gp_fts_probe_timeout = 20;
static int	gp_fts_probe_retries = 5;
static int	gp_fts_mark_mirror_down_grace_period = 30;
static int	gp_fts_replication_attempt_count = 10;

/* gp.log_fts: Cloudberry's gp_log_fts, how much the prober says. */
typedef enum FtsLogLevel
{
	FTS_LOG_OFF,
	FTS_LOG_TERSE,
	FTS_LOG_VERBOSE,
	FTS_LOG_DEBUG,
} FtsLogLevel;

static int	gp_log_fts = FTS_LOG_TERSE;

static const struct config_enum_entry gp_log_fts_options[] = {
	{"off", FTS_LOG_OFF, false},
	{"terse", FTS_LOG_TERSE, false},
	{"verbose", FTS_LOG_VERBOSE, false},
	{"debug", FTS_LOG_DEBUG, false},
	{NULL, 0, false}
};

#define FTS_LOG(level, ...) \
	do { \
		if (gp_log_fts >= (level)) \
			ereport(LOG, errmsg_internal(__VA_ARGS__)); \
	} while (0)

/* gp.fts_status: shown, never set; see fts_show_status(). */
static char *gp_fts_status_setting = NULL;

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

typedef struct GpFtsShared
{
	slock_t		mutex;

	/* The coordinator's prober, and what the backends that wait for it read. */
	int			prober_pid;		/* 0 while no prober runs */
	ProcNumber	prober_procno;
	bool		probe_requested;	/* asked for since the last cycle began */
	uint32		start_count;	/* cycles begun */
	uint32		done_count;		/* the start_count of the last one ended */
	ConditionVariable cv;		/* broadcast as a cycle begins and as it ends */

	/*
	 * A primary's: its mirror's WAL sender, when the last one ended, and how
	 * many have ended since the mirror last streamed.
	 */
	int			walsender_pid;
	pg_time_t	walsender_ended;
	int			walsender_attempts;
} GpFtsShared;

static GpFtsShared *fts_shared = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;
static ClientAuthentication_hook_type prev_client_auth = NULL;
static ProcessUtility_hook_type prev_process_utility = NULL;

static void
fts_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(GpFtsShared)));
}

static void
fts_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	fts_shared = ShmemInitStruct("gp_core fts", sizeof(GpFtsShared), &found);
	if (!found)
	{
		memset(fts_shared, 0, sizeof(GpFtsShared));
		SpinLockInit(&fts_shared->mutex);
		fts_shared->prober_procno = INVALID_PROC_NUMBER;
		ConditionVariableInit(&fts_shared->cv);
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * What pg_stat_activity shows while the prober waits, and while a backend
 * waits for it.  Registered on first use: see dispatch_wait_event().
 */
static uint32
fts_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryFts");
	return event;
}

/* ------------------------------------------------------------------------- */
/* What a primary knows of its mirror                                        */
/* ------------------------------------------------------------------------- */

/*
 * The mirror's WAL sender ends: one more attempt, counted from the last one
 * that streamed, as Cloudberry's replication status counts them
 * (FTSReplicationStatusMarkDisconnect(), gp_replication.c).
 */
static void
fts_walsender_exit(int code, Datum arg)
{
	bool		streamed = false;

	if (MyWalSnd != NULL)
	{
		SpinLockAcquire(&MyWalSnd->mutex);
		streamed = MyWalSnd->state == WALSNDSTATE_STREAMING;
		SpinLockRelease(&MyWalSnd->mutex);
	}

	SpinLockAcquire(&fts_shared->mutex);
	if (fts_shared->walsender_pid == MyProcPid)
	{
		fts_shared->walsender_pid = 0;
		fts_shared->walsender_ended = (pg_time_t) time(NULL);
		if (streamed)
			fts_shared->walsender_attempts = 0;
		fts_shared->walsender_attempts++;
	}
	SpinLockRelease(&fts_shared->mutex);

	/* on the coordinator, its standby is waited for no more */
	GpStandbyWake();
}

/*
 * Every connection passes here once it is authenticated.  A mirror's WAL
 * receiver is noted, so that a probe knows which WAL sender is the mirror's
 * and when the last one ended; and two of Cloudberry's faults are moments of
 * a connection's start: a probe's (fts_conn_startup_packet), which fails it
 * as a primary that cannot be reached fails it, and the mirror's WAL
 * sender's (initialize_wal_sender).
 */
static void
fts_client_auth(Port *port, int status)
{
	if (prev_client_auth)
		prev_client_auth(port, status);
	if (status != STATUS_OK || fts_shared == NULL ||
		port->application_name == NULL)
		return;

	if (strcmp(port->application_name, GP_FTS_APPNAME) == 0)
		(void) GP_FAULT("fts_conn_startup_packet");

	if (am_walsender &&
		strcmp(port->application_name, GP_WALRECEIVER_APPNAME) == 0)
	{
		(void) GP_FAULT("initialize_wal_sender");
		SpinLockAcquire(&fts_shared->mutex);
		fts_shared->walsender_pid = MyProcPid;
		SpinLockRelease(&fts_shared->mutex);
		before_shmem_exit(fts_walsender_exit, (Datum) 0);

		/* on the coordinator, its standby may be waited for again */
		GpStandbyWake();
	}
}

int
GpFtsWalreceiverSender(void)
{
	int			pid;

	if (fts_shared == NULL)
		return 0;
	SpinLockAcquire(&fts_shared->mutex);
	pid = fts_shared->walsender_pid;
	SpinLockRelease(&fts_shared->mutex);
	return pid;
}

/*
 * Cloudberry's GetMirrorStatus(): whether the mirror is up -- its WAL sender
 * has sent it some WAL -- whether it is in sync -- streaming -- whether it is
 * ready for synchronous replication -- streaming, or catching up and within
 * gp.repl_catchup_within_range of this node's WAL (gp_standby.c) -- whether
 * synchronous replication is on, and, for a mirror that is not up, whether
 * FTS should ask again before it says so.
 */
static void
fts_mirror_status(bool *mirror_up, bool *in_sync, bool *ready,
				  bool *syncrep_on, bool *retry)
{
	int			pid;
	pg_time_t	ended;
	int			attempts;

	*mirror_up = *in_sync = *ready = *retry = false;

	SpinLockAcquire(&fts_shared->mutex);
	pid = fts_shared->walsender_pid;
	ended = fts_shared->walsender_ended;
	attempts = fts_shared->walsender_attempts;
	SpinLockRelease(&fts_shared->mutex);

	LWLockAcquire(SyncRepLock, LW_SHARED);
	for (int i = 0; pid != 0 && i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		pid_t		walsnd_pid;
		WalSndState state;
		XLogRecPtr	write;
		XLogRecPtr	sent;

		SpinLockAcquire(&walsnd->mutex);
		walsnd_pid = walsnd->pid;
		state = walsnd->state;
		write = walsnd->write;
		sent = walsnd->sentPtr;
		SpinLockRelease(&walsnd->mutex);

		if (walsnd_pid != pid)
			continue;

		/*
		 * Up once it has received some WAL: a sender is in catch-up as soon
		 * as the mirror asks, and may fail right after if the WAL it asks for
		 * is gone (Cloudberry's is_mirror_up()).
		 */
		*mirror_up = (state == WALSNDSTATE_CATCHUP && XLogRecPtrIsValid(write)) ||
			state == WALSNDSTATE_STREAMING;
		*in_sync = *mirror_up && state == WALSNDSTATE_STREAMING;
		*ready = *in_sync ||
			(*mirror_up && state == WALSNDSTATE_CATCHUP && GpStandbyWithinRange(sent));
		break;
	}
	*syncrep_on = (WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) != 0;
	LWLockRelease(SyncRepLock);

	if (*in_sync)
	{
		SpinLockAcquire(&fts_shared->mutex);
		fts_shared->walsender_attempts = 0;
		SpinLockRelease(&fts_shared->mutex);
	}

	/*
	 * A mirror that has not connected yet, while the primary has only just
	 * started or has only just lost it, may be on its way: FTS is asked to
	 * come back rather than mark it down (Cloudberry's is_probe_retry_needed()).
	 * But not one whose WAL senders have ended more than
	 * gp.fts_replication_attempt_count times since it last streamed, whose
	 * attempts could go on for ever while a commit waits for it: only the
	 * primary's start counts then (FTSGetReplicationDisconnectTime()).
	 */
	if (!*mirror_up)
	{
		pg_time_t	since;
		pg_time_t	delta;

		if (attempts > gp_fts_replication_attempt_count)
		{
			ereport(LOG,
					(errmsg("Primary-mirror replication streaming already attempted %d times exceed limit gp_fts_replication_attempt_count %d",
							attempts, gp_fts_replication_attempt_count)));
			ended = 0;
		}
		since = Max(ended, timestamptz_to_time_t(PgStartTime));
		delta = (pg_time_t) time(NULL) - since;

		if (delta >= 0 && delta < gp_fts_mark_mirror_down_grace_period)
		{
			*retry = true;
			ereport(LOG,
					(errmsg("requesting fts retry as mirror didn't connect yet but in grace period: " INT64_FORMAT,
							(int64) delta)));
		}
	}
}

/*
 * Cloudberry's checkIODataDirectory(): read a block of a file in the data
 * directory and write it back, with O_DIRECT where the file system has it, so
 * that a disk that fails, or hangs, fails the probe.  A file system without
 * O_DIRECT -- tmpfs -- is checked through the page cache.
 */
static void
fts_check_io(void)
{
	char	   *block = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);
	size_t		magic_len = strlen(FTS_PROBE_MAGIC_STRING) + 1;
	bool		failed = false;
	int			fd;

	fd = BasicOpenFile(FTS_PROBE_FILE_NAME, O_RDWR | PG_BINARY | PG_O_DIRECT);
	if (fd < 0 && errno == EINVAL)
		fd = BasicOpenFile(FTS_PROBE_FILE_NAME, O_RDWR | PG_BINARY);
	if (fd < 0 && errno == ENOENT)
	{
		ereport(LOG,
				(errmsg("FTS: \"%s\" file doesn't exist, creating it once.",
						FTS_PROBE_FILE_NAME)));
		fd = BasicOpenFile(FTS_PROBE_FILE_NAME,
						   O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
		if (fd >= 0)
		{
			memcpy(block, FTS_PROBE_MAGIC_STRING, magic_len);
			if (write(fd, block, BLCKSZ) != BLCKSZ || lseek(fd, 0, SEEK_SET) < 0)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("FTS: could not write file \"%s\": %m",
								FTS_PROBE_FILE_NAME)));
				failed = true;
			}
		}
	}

	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("FTS: could not open file \"%s\": %m", FTS_PROBE_FILE_NAME)));
		failed = true;
	}
	else if (!failed)
	{
		ssize_t		len = read(fd, block, BLCKSZ);

		if (len != BLCKSZ)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("FTS: could not read file \"%s\" (actual bytes read %zd, required: %d): %m",
							FTS_PROBE_FILE_NAME, len, BLCKSZ)));
			failed = true;
		}
		else if (memcmp(block, FTS_PROBE_MAGIC_STRING, magic_len) != 0)
		{
			ereport(LOG,
					(errmsg("FTS: Read corrupted data from \"%s\" file",
							FTS_PROBE_FILE_NAME)));
			failed = true;
		}
		else if (lseek(fd, 0, SEEK_SET) < 0 ||
				 write(fd, block, BLCKSZ) != BLCKSZ)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("FTS: could not write file \"%s\": %m",
							FTS_PROBE_FILE_NAME)));
			failed = true;
		}
	}
	pfree(block);

	if (fd >= 0)
		close(fd);

	/*
	 * A file that failed once would fail every probe after it, the node's
	 * next turn as a primary included, so it goes.
	 */
	if (failed)
	{
		(void) unlink(FTS_PROBE_FILE_NAME);
		ereport(ERROR,
				(errmsg("disk IO check during FTS probe failed")));
	}
}

/*
 * gp.fts_status: this node's answer to a probe, computed as it is shown --
 * "dbid content mirror_up in_sync syncrep_on role_mirror retry ready", the
 * last six 't' or 'f' -- as Cloudberry's handler answers its PROBE message,
 * with the node's dbid and content id in front, so that the prober knows it
 * reached the node it meant.
 */
static const char *
fts_show_status(void)
{
	static char answer[64];
	bool		mirror_up = false;
	bool		in_sync = false;
	bool		ready = false;
	bool		syncrep_on = false;
	bool		retry = false;
	bool		role_mirror = RecoveryInProgress();

	(void) GP_FAULT("fts_handle_message");

	if (role_mirror)
		ereport(LOG, (errmsg("received probe message while acting as mirror")));
	else if (fts_shared != NULL)
		fts_mirror_status(&mirror_up, &in_sync, &ready, &syncrep_on, &retry);

	fts_check_io();

	snprintf(answer, sizeof(answer), "%d %d %c %c %c %c %c %c",
			 GpClusterDbid(), GpClusterContentId(),
			 mirror_up ? 't' : 'f', in_sync ? 't' : 'f',
			 syncrep_on ? 't' : 'f', role_mirror ? 't' : 'f',
			 retry ? 't' : 'f', ready ? 't' : 'f');
	return answer;
}

/* ------------------------------------------------------------------------- */
/* R3: a commit waits for the mirror, cancelled or not                       */
/* ------------------------------------------------------------------------- */

static bool commit_cancel_held = false;

static void
commit_hold_cancel(void)
{
	if (commit_cancel_held)
		return;
	HOLD_CANCEL_INTERRUPTS();
	commit_cancel_held = true;
}

/*
 * The commit is done: a cancel that came while it waited cancelled nothing,
 * and is dropped, in Cloudberry's words (syncrep.c), rather than left to
 * fail whatever this backend does next.  An error lets go of every hold,
 * ours too (errfinish), and then there is nothing to let go of.
 */
static void
commit_release_cancel(void)
{
	if (!commit_cancel_held)
		return;
	commit_cancel_held = false;
	if (QueryCancelHoldoffCount == 0)
		return;
	if (QueryCancelPending)
	{
		QueryCancelPending = false;
		ereport(WARNING,
				(errmsg("ignoring query cancel request for synchronous replication to ensure cluster consistency"),
				 errdetail("The transaction has already changed locally, it has to be replicated to standby.")));
	}
	RESUME_CANCEL_INTERRUPTS();
}

static void
fts_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			commit_hold_cancel();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_ABORT:
			commit_release_cancel();
			break;
		default:
			break;
	}
}

/*
 * COMMIT PREPARED and ROLLBACK PREPARED finish their transaction, and wait
 * for the mirror, inside the statement, before the transaction that runs
 * them commits.
 */
static void
fts_process_utility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		hold = false;

	if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmtKind kind = ((TransactionStmt *) parsetree)->kind;

		hold = kind == TRANS_STMT_COMMIT_PREPARED ||
			kind == TRANS_STMT_ROLLBACK_PREPARED;
	}

	if (hold)
		commit_hold_cancel();
	if (prev_process_utility)
		prev_process_utility(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
	if (hold)
		commit_release_cancel();
}

/* ------------------------------------------------------------------------- */
/* The prober: messages                                                      */
/* ------------------------------------------------------------------------- */

/* Cloudberry's messages, and SYNCREP_ON, which its handler does in a PROBE. */
typedef enum FtsMessage
{
	FTS_MSG_PROBE,
	FTS_MSG_SYNCREP_ON,
	FTS_MSG_SYNCREP_OFF,
	FTS_MSG_PROMOTE,
} FtsMessage;

static const char *const fts_message_names[] = {
	"PROBE", "SYNCREP_ON", "SYNCREP_OFF", "PROMOTE"
};

/*
 * The statements of each message, sent one after the other on one
 * connection.  ALTER SYSTEM is a statement of its own, as it cannot run in a
 * transaction block, which several statements in one query string are.
 */
static const char *const fts_probe_sql[] = {
	"SELECT pg_catalog.current_setting('gp.fts_status')",
	NULL
};

static const char *const fts_syncrep_on_sql[] = {
	"ALTER SYSTEM SET synchronous_standby_names = '*'",
	"SELECT pg_catalog.pg_reload_conf()",
	NULL
};

static const char *const fts_syncrep_off_sql[] = {
	"ALTER SYSTEM SET synchronous_standby_names = ''",
	"SELECT pg_catalog.pg_reload_conf()",
	NULL
};

/*
 * A promotion, as Cloudberry's handler makes one: synchronous replication
 * off, since the new primary has no mirror to wait for; the slot the primary
 * it replaces will stream from once it is recovered as its mirror, keeping
 * WAL from now on for that (CreateReplicationSlotOnPromote); and the
 * promotion, unless it has already happened -- a message sent again because
 * the answer to the last one was lost is to change nothing.
 */
static const char *const fts_promote_sql[] = {
	"ALTER SYSTEM SET synchronous_standby_names = ''",
	"SELECT pg_catalog.pg_reload_conf()",
	"SELECT pg_catalog.pg_create_physical_replication_slot('" GP_WAL_REPLICATION_SLOT "', true)"
	" WHERE NOT EXISTS (SELECT FROM pg_catalog.pg_replication_slots"
	" WHERE slot_name = '" GP_WAL_REPLICATION_SLOT "')",
	"SELECT CASE WHEN pg_catalog.pg_is_in_recovery() THEN pg_catalog.pg_promote(false) END",
	NULL
};

static const char *const *const fts_message_sql[] = {
	fts_probe_sql, fts_syncrep_on_sql, fts_syncrep_off_sql, fts_promote_sql
};

/* Where a message is. */
typedef enum FtsStep
{
	FTS_STEP_CONNECT,			/* connecting */
	FTS_STEP_SEND,				/* connected: the next statement is to go */
	FTS_STEP_RECEIVE,			/* its results are coming */
	FTS_STEP_RETRY_WAIT,		/* the next attempt is at retry_at */
	FTS_STEP_DONE				/* it succeeded, or failed for good */
} FtsStep;

/*
 * A content's pair, through a cycle: the messages the prober sends it, one at
 * a time, to its primary or -- to promote it -- to its mirror.
 */
typedef struct FtsPair
{
	int			primary;		/* its primary, and its mirror, as indexes of */
	int			mirror;			/* the prober's nodes; swapped by a failover */
	int			target;			/* whom the message is for */
	FtsMessage	message;
	FtsStep		step;
	bool		succeeded;
	bool		processed;		/* nothing more is to be sent it this cycle */
	int			retries;		/* attempts after the first */
	int			stmt;			/* the message's next statement */
	PGconn	   *conn;
	int			want;			/* WL_SOCKET_* its socket is waited for */
	bool		ready;			/* ... and has been found to be */
	TimestampTz attempt_start;
	TimestampTz retry_at;
	char	   *error;			/* a statement's, until the rest are read */
	bool		restarting;		/* the primary said it is starting up */

	/* The answer to a probe. */
	bool		answered;
	bool		mirror_up;
	bool		in_sync;
	bool		syncrep_on;
	bool		role_mirror;
	bool		retry_requested;
	bool		ready_for_syncrep;
} FtsPair;

/* The prober's view of the cluster: the nodes, and their states, which it keeps. */
static const GpSegmentConfig *fts_nodes = NULL;
static int	fts_nnodes = 0;
static GpClusterNodeState *fts_states = NULL;

/* When each node was first found starting up, 0 while it is not. */
static TimestampTz *fts_restarting_since = NULL;

/* Whose each place was at the last cycle, so that a new node starts afresh. */
static int *fts_dbids = NULL;

/*
 * A node was added, removed or moved during this cycle (gp_segadmin.c):
 * nothing it found is published or acted on, and a cycle follows at once.
 */
static bool fts_nodes_changed = false;

/* Who the prober connects as: the bootstrap superuser. */
static char *fts_user = NULL;

/*
 * The addresses of the nodes' host names, as Cloudberry's prober keeps them
 * (getDnsCachedAddress(), cdbutil.c): a name looked up once, and its address
 * kept for the process, so that a primary is still reached, and failed over
 * from, while the name service is down.  Keyed by the name.
 */
typedef struct FtsAddress
{
	char		name[NAMEDATALEN];
	char		address[NI_MAXHOST];
} FtsAddress;

static HTAB *fts_addresses = NULL;

/* Each node's, in the order of fts_nodes, for the probe to connect to. */
static const char **fts_node_address = NULL;

static void
fts_close(FtsPair *p)
{
	if (p->conn != NULL)
		libpqsrv_disconnect(p->conn);
	p->conn = NULL;
}

/*
 * An attempt has failed.  Another is made a second later, as Cloudberry's
 * prober makes one, until gp.fts_probe_retries have been; then the message
 * has failed.
 */
static void
fts_attempt_failed(FtsPair *p, const char *why)
{
	const GpSegmentConfig *node = &fts_nodes[p->target];
	char	   *reason = pchomp(why != NULL ? why : "");

	/*
	 * A primary that refuses because it is starting up, or recovering from a
	 * crash, is restarting rather than gone (Cloudberry's
	 * checkIfFailedDueToNormalRestart()).
	 */
	if (p->message == FTS_MSG_PROBE)
		p->restarting =
			strstr(reason, "the database system is starting up") != NULL ||
			strstr(reason, "the database system is in recovery mode") != NULL;

	ereport(LOG,
			(errmsg("FTS: %s to (content=%d, dbid=%d) failed, retry_count=%d: %s",
					fts_message_names[p->message], node->content, node->dbid,
					p->retries, reason)));
	fts_close(p);

	if (p->retries < gp_fts_probe_retries)
	{
		p->retries++;
		p->step = FTS_STEP_RETRY_WAIT;
		p->retry_at = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 1000);
	}
	else
	{
		if (gp_fts_probe_retries > 0)
			ereport(LOG,
					(errmsg("FTS max (%d) retries exhausted (content=%d, dbid=%d)",
							p->retries, node->content, node->dbid)));
		p->step = FTS_STEP_DONE;
		p->succeeded = false;
	}
}

/*
 * A node's address, the one its host name had when first looked up: NULL,
 * with Cloudberry's message, for a name that does not resolve, and the host
 * as it is for one that is a directory -- a socket's, as a cluster on one
 * machine names its nodes.  An IPv4 address where the name has one, as
 * Cloudberry's lookup prefers, and else the first.
 */
static const char *
fts_address(const char *name, int port)
{
	FtsAddress *e;
	struct addrinfo hint;
	struct addrinfo *addrs = NULL;
	struct addrinfo *pick = NULL;
	char		service[16];
	char		address[NI_MAXHOST];
	int			ret;

	if (name == NULL || name[0] == '/' || name[0] == '@')
		return name;

	if (fts_addresses == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = sizeof(FtsAddress);
		fts_addresses = hash_create("gp_core fts addresses", 64, &ctl,
									HASH_ELEM | HASH_STRINGS);
	}
	e = (FtsAddress *) hash_search(fts_addresses, name, HASH_FIND, NULL);
	if (e != NULL)
		return e->address;

	memset(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_family = AF_UNSPEC;
	snprintf(service, sizeof(service), "%d", port);
	ret = pg_getaddrinfo_all(name, service, &hint, &addrs);
	if (ret != 0 || addrs == NULL)
	{
		if (addrs != NULL)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		ereport(LOG,
				(errmsg("could not translate host name \"%s\", port \"%d\" to address: %s",
						name, port, gai_strerror(ret))));
		return NULL;
	}
	for (struct addrinfo *a = addrs; a != NULL; a = a->ai_next)
	{
		if (a->ai_family == AF_INET)
		{
			pick = a;
			break;
		}
		if (pick == NULL && a->ai_family == AF_INET6)
			pick = a;
	}
	if (pick == NULL ||
		pg_getnameinfo_all((const struct sockaddr_storage *) pick->ai_addr,
						   pick->ai_addrlen, address, sizeof(address),
						   NULL, 0, NI_NUMERICHOST) != 0)
	{
		pg_freeaddrinfo_all(hint.ai_family, addrs);
		ereport(LOG,
				(errmsg("could not translate host name \"%s\", port \"%d\" to address: %s",
						name, port, "no address of a family this server connects to")));
		return NULL;
	}
	pg_freeaddrinfo_all(hint.ai_family, addrs);

	if (strlen(name) >= NAMEDATALEN)
		return MemoryContextStrdup(TopMemoryContext, address);
	e = (FtsAddress *) hash_search(fts_addresses, name, HASH_ENTER, NULL);
	strlcpy(e->address, address, sizeof(e->address));
	return e->address;
}

/*
 * Every node's address, as Cloudberry's prober reads them with the cluster's
 * configuration (getAddressesForDBid(), cdbutil.c): false, and nothing
 * probed, where a primary's name does not resolve -- Cloudberry's "cannot
 * resolve network address for dbid=%d", which fails its prober's round, so
 * that no segment is marked down for the name service's fault.  A mirror's
 * name that does not resolve is its connection's to fail.  Cloudberry's
 * fault get_dns_cached_address, a skip, gives content 0's preferred primary
 * a name that does not (fts_errors).
 */
static bool
fts_resolve_nodes(void)
{
	if (fts_node_address == NULL)
		fts_node_address = MemoryContextAllocZero(TopMemoryContext,
												  Max(fts_nnodes, 1) * sizeof(char *));
	for (int i = 0; i < fts_nnodes; i++)
	{
		const char *name = fts_nodes[i].hostname;

		if (fts_nodes[i].content == 0 && fts_nodes[i].preferred_role == 'p' &&
			gp_fault_active != NULL && *gp_fault_active > 0 &&
			GP_FAULT("get_dns_cached_address") == GP_FAULT_SKIP)
			name = "dnserrordummyaddress";
		fts_node_address[i] = fts_address(name, fts_nodes[i].port);
		if (fts_node_address[i] == NULL && fts_states[i].role == 'p')
		{
			ereport(LOG,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("cannot resolve network address for dbid=%d",
							fts_nodes[i].dbid),
					 errdetail("FTS probes no segment this round.")));
			return false;
		}
	}
	return true;
}

/* An attempt at the pair's message: a new connection to its target. */
static void
fts_attempt_begin(FtsPair *p)
{
	const GpSegmentConfig *node = &fts_nodes[p->target];
	const char *keywords[6 + GP_INTERNAL_CONN_OPTIONS];
	const char *values[6 + GP_INTERNAL_CONN_OPTIONS];
	char		portbuf[16];
	int			n = 0;

	snprintf(portbuf, sizeof(portbuf), "%d", node->port);
	keywords[n] = "host";
	values[n++] = node->hostname;
	/* the address its name had, where it has one (fts_resolve_nodes()) */
	if (fts_node_address != NULL && fts_node_address[p->target] != NULL &&
		fts_node_address[p->target] != node->hostname)
	{
		keywords[n] = "hostaddr";
		values[n++] = fts_node_address[p->target];
	}
	keywords[n] = "port";
	values[n++] = portbuf;
	keywords[n] = "dbname";
	values[n++] = "postgres";
	keywords[n] = "user";
	values[n++] = fts_user;
	keywords[n] = "application_name";
	values[n++] = GP_FTS_APPNAME;
	n = GpInternalConnOptions(keywords, values, n);

	p->stmt = 0;
	p->ready = false;
	p->answered = false;
	p->attempt_start = GetCurrentTimestamp();
	p->conn = libpqsrv_connect_params_start(keywords, values, false);
	if (p->conn == NULL)
	{
		ReleaseExternalFD();
		fts_attempt_failed(p, "out of memory");
		return;
	}
	if (PQstatus(p->conn) == CONNECTION_BAD)
	{
		fts_attempt_failed(p, PQerrorMessage(p->conn));
		return;
	}
	p->step = FTS_STEP_CONNECT;
	p->want = WL_SOCKET_WRITEABLE;	/* as PQconnectPoll() asks at first */
	FTS_LOG(FTS_LOG_DEBUG, "FTS: sending %s to (content=%d, dbid=%d), retry_count=%d",
			fts_message_names[p->message], node->content, node->dbid, p->retries);
}

/* Record a probe's answer; false when it is not one, or not the target's. */
static bool
fts_record_answer(FtsPair *p, const char *answer)
{
	const GpSegmentConfig *node = &fts_nodes[p->target];
	int			dbid;
	int			content;
	char		f[6];

	if (sscanf(answer, "%d %d %c %c %c %c %c %c", &dbid, &content,
			   &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) != 8)
		return false;
	if (dbid != node->dbid || content != node->content)
	{
		ereport(LOG,
				(errmsg("FTS: (content=%d, dbid=%d) at %s:%d answered as dbid %d with content id %d",
						node->content, node->dbid, node->hostname, node->port,
						dbid, content)));
		return false;
	}

	p->mirror_up = f[0] == 't';
	p->in_sync = f[1] == 't';
	p->syncrep_on = f[2] == 't';
	p->role_mirror = f[3] == 't';
	p->retry_requested = f[4] == 't';
	p->ready_for_syncrep = f[5] == 't';
	p->answered = true;

	FTS_LOG(FTS_LOG_DEBUG,
			"FTS: segment (content=%d, dbid=%d) reported isMirrorUp %d, isInSync %d, "
			"isSyncRepEnabled %d, isRoleMirror %d, and retryRequested %d to the prober.",
			node->content, node->dbid, p->mirror_up, p->in_sync, p->syncrep_on,
			p->role_mirror, p->retry_requested);
	return true;
}

static void
fts_send(FtsPair *p)
{
	if (!PQsendQuery(p->conn, fts_message_sql[p->message][p->stmt]))
	{
		fts_attempt_failed(p, PQerrorMessage(p->conn));
		return;
	}
	p->step = FTS_STEP_RECEIVE;
	p->want = WL_SOCKET_READABLE;
}

/* Read what has come of the statement in flight, without waiting for more. */
static void
fts_receive(FtsPair *p)
{
	if (!PQconsumeInput(p->conn))
	{
		fts_attempt_failed(p, PQerrorMessage(p->conn));
		return;
	}

	for (;;)
	{
		PGresult   *res;

		if (PQisBusy(p->conn))
			return;				/* more is coming */
		res = PQgetResult(p->conn);
		if (res == NULL)
			break;				/* the statement is done */

		switch (PQresultStatus(res))
		{
			case PGRES_TUPLES_OK:
				if (p->message == FTS_MSG_PROBE &&
					(PQntuples(res) != 1 || PQnfields(res) != 1 ||
					 !fts_record_answer(p, PQgetvalue(res, 0, 0))) &&
					p->error == NULL)
					p->error = pstrdup("invalid response to the probe");
				break;
			case PGRES_COMMAND_OK:
				break;
			default:
				if (p->error == NULL)
					p->error = pstrdup(PQresultErrorMessage(res));
				break;
		}
		PQclear(res);
	}

	if (p->error != NULL)
	{
		char	   *error = p->error;

		p->error = NULL;
		fts_attempt_failed(p, error);
		return;
	}

	if (fts_message_sql[p->message][++p->stmt] != NULL)
	{
		p->step = FTS_STEP_SEND;
		return;
	}

	fts_close(p);
	p->step = FTS_STEP_DONE;
	p->succeeded = true;
}

/* Move a pair's message on as far as it goes without waiting. */
static void
fts_advance(FtsPair *p)
{
	TimestampTz now = GetCurrentTimestamp();

	switch (p->step)
	{
		case FTS_STEP_RETRY_WAIT:
			if (now >= p->retry_at)
				fts_attempt_begin(p);
			break;
		case FTS_STEP_CONNECT:
			if (!p->ready)
				break;
			p->ready = false;
			switch (PQconnectPoll(p->conn))
			{
				case PGRES_POLLING_OK:
					p->step = FTS_STEP_SEND;
					break;
				case PGRES_POLLING_READING:
					p->want = WL_SOCKET_READABLE;
					break;
				case PGRES_POLLING_WRITING:
					p->want = WL_SOCKET_WRITEABLE;
					break;
				default:
					fts_attempt_failed(p, PQerrorMessage(p->conn));
					return;
			}
			break;
		case FTS_STEP_RECEIVE:
			if (!p->ready)
				break;
			p->ready = false;
			fts_receive(p);
			break;
		default:
			break;
	}

	if (p->step == FTS_STEP_SEND)
		fts_send(p);

	/*
	 * A message that has taken longer than gp.fts_probe_timeout since its
	 * connection began has failed, whatever it is waiting for.
	 */
	if ((p->step == FTS_STEP_CONNECT || p->step == FTS_STEP_RECEIVE) &&
		now - p->attempt_start > (TimestampTz) gp_fts_probe_timeout * USECS_PER_SEC)
	{
		ereport(LOG,
				(errmsg("FTS timeout detected for (content=%d, dbid=%d) message=%s, retry_count=%d",
						fts_nodes[p->target].content, fts_nodes[p->target].dbid,
						fts_message_names[p->message], p->retries)));
		fts_attempt_failed(p, "timeout");
	}
}

/* The pair's next message, begun at once. */
static void
fts_next_message(FtsPair *p, FtsMessage message, int target)
{
	p->message = message;
	p->target = target;
	p->retries = 0;
	p->succeeded = false;
	p->step = FTS_STEP_RETRY_WAIT;
	p->retry_at = 0;
}

/* ------------------------------------------------------------------------- */
/* The prober: what it makes of the answers                                  */
/* ------------------------------------------------------------------------- */

/*
 * Append a line to FTS's history, as Cloudberry's inserts a row into
 * gp_configuration_history, synced: "time dbid desc", the time in
 * microseconds since 2000, as a TimestampTz is kept.
 */
static void
fts_history(int dbid, const char *desc)
{
	int			fd;
	char	   *line;
	size_t		len;

	line = psprintf(INT64_FORMAT "\t%d\t%s\n", (int64) GetCurrentTimestamp(),
					dbid, desc);
	len = strlen(line);

	fd = OpenTransientFile(FTS_HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", FTS_HISTORY_FILE)));
	if (write(fd, line, len) != (ssize_t) len || pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", FTS_HISTORY_FILE)));
	}
	CloseTransientFile(fd);
	pfree(line);
}

/*
 * Cloudberry's updateConfiguration(): record what the answer changes of the
 * pair -- the primary's status, the mirror's, their mode, and on a failover
 * their roles -- in the history and in the cluster's state, which is
 * published before anything is done about it.  True when anything changed.
 */
static bool
fts_update(FtsPair *p, char new_primary_role, char new_mirror_role,
		   bool in_sync, bool primary_alive, bool mirror_alive)
{
	GpClusterNodeState *ps = &fts_states[p->primary];
	GpClusterNodeState *ms = &fts_states[p->mirror];
	bool		update_primary = primary_alive != (ps->status == 'u');
	bool		update_mirror = mirror_alive != (ms->status == 'u');

	/* A primary and its mirror are always in the same mode. */
	if (in_sync != (ps->mode == 's'))
		update_primary = update_mirror = true;
	if (!update_primary && !update_mirror)
		return false;

	for (int k = 0; k < 2; k++)
	{
		int			i = k == 0 ? p->primary : p->mirror;
		GpClusterNodeState *s = &fts_states[i];
		char	   *desc;

		if (!(k == 0 ? update_primary : update_mirror))
			continue;
		s->role = k == 0 ? new_primary_role : new_mirror_role;
		s->status = (k == 0 ? primary_alive : mirror_alive) ? 'u' : 'd';
		s->mode = in_sync ? 's' : 'n';

		desc = psprintf("FTS: update role, status, and mode for dbid %d with contentid %d to %c, %c, and %c",
						fts_nodes[i].dbid, fts_nodes[i].content, s->role,
						s->status, s->mode);
		ereport(LOG, (errmsg_internal("%s", desc)));
		fts_history(fts_nodes[i].dbid, desc);
		(void) GP_FAULT("fts_update_config");
		pfree(desc);
	}

	if (!GpClusterPublish(fts_states))
	{
		FTS_LOG(FTS_LOG_TERSE,
				"FTS: the cluster's nodes changed during the probe; probing again");
		fts_nodes_changed = true;
		return false;
	}
	return true;
}

/*
 * Has a primary been starting up for longer than a cycle's patience?  One
 * that is gets no more: Cloudberry tells a restart that is making progress
 * from one that is not by the WAL position its message carries, which
 * PostgreSQL 19's does not, so the port gives a restart as long as it gives
 * a primary that does not answer at all -- gp.fts_probe_retries attempts of
 * gp.fts_probe_timeout each -- and then fails over from it.
 */
static bool
fts_restart_too_long(int node)
{
	TimestampTz now = GetCurrentTimestamp();

	if (fts_restarting_since[node] == 0)
		fts_restarting_since[node] = now;
	return now - fts_restarting_since[node] >
		(TimestampTz) Max(gp_fts_probe_retries, 1) * gp_fts_probe_timeout * USECS_PER_SEC;
}

/* Cloudberry's processResponse(), for a probe that was answered. */
static void
fts_probe_answered(FtsPair *p)
{
	const GpSegmentConfig *primary = &fts_nodes[p->primary];

	fts_restarting_since[p->primary] = 0;

	/*
	 * A mirror that may still be connecting, and is marked up, is asked about
	 * again rather than marked down (processRetry()).
	 */
	if (p->retry_requested && !p->mirror_up &&
		fts_states[p->mirror].status == 'u' &&
		p->retries < gp_fts_probe_retries)
	{
		p->retries++;
		p->step = FTS_STEP_RETRY_WAIT;
		p->retry_at = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 1000);
		p->succeeded = false;
		return;
	}

	if (p->syncrep_on && !p->mirror_up)
	{
		if (p->retry_requested)
		{
			FTS_LOG(FTS_LOG_VERBOSE,
					"FTS skipping mirror down update for (content=%d) as retryRequested",
					primary->content);
			p->processed = true;
			return;
		}

		/*
		 * The primary's commits wait for a mirror that is gone: marked down
		 * first, and then the primary is told to stop waiting for it.
		 */
		(void) fts_update(p, 'p', 'm', p->in_sync, true, false);
		if (fts_nodes_changed)
		{
			p->processed = true;
			return;
		}
		FTS_LOG(FTS_LOG_VERBOSE, "FTS turning syncrep off on (content=%d, dbid=%d)",
				primary->content, primary->dbid);
		fts_next_message(p, FTS_MSG_SYNCREP_OFF, p->primary);
	}
	else if (p->role_mirror)
	{
		/* A promotion that did not take: it is asked for again. */
		FTS_LOG(FTS_LOG_VERBOSE, "FTS resending promote request to (content=%d, dbid=%d)",
				primary->content, primary->dbid);
		fts_next_message(p, FTS_MSG_PROMOTE, p->primary);
	}
	else if (!p->syncrep_on && p->ready_for_syncrep)
	{
		/* The mirror is back and streaming: commits wait for it again. */
		fts_next_message(p, FTS_MSG_SYNCREP_ON, p->primary);
	}
	else
	{
		(void) fts_update(p, 'p', 'm', p->in_sync, true, p->mirror_up);
		p->processed = true;
	}
}

/* Cloudberry's processResponse(), for a primary that did not answer. */
static void
fts_primary_down(FtsPair *p)
{
	const GpSegmentConfig *primary = &fts_nodes[p->primary];
	const GpSegmentConfig *mirror = &fts_nodes[p->mirror];
	int			swap;

	if (p->restarting && !fts_restart_too_long(p->primary))
	{
		FTS_LOG(FTS_LOG_VERBOSE,
				"FTS: detected segment is starting up (content=%d) primary dbid=%d, mirror dbid=%d",
				primary->content, primary->dbid, mirror->dbid);
		p->processed = true;
		return;
	}
	fts_restarting_since[p->primary] = 0;

	if (fts_states[p->mirror].mode != 's')
	{
		ereport(WARNING,
				(errmsg("ERROR: FTS double fault detected (content=%d) primary dbid=%d, mirror dbid=%d",
						primary->content, primary->dbid, mirror->dbid)));
		p->processed = true;
		return;
	}

	/*
	 * The roles are swapped, and the primary marked down, before the mirror
	 * is asked to be promoted: from then on the dispatcher connects to the
	 * mirror, and FTS no longer probes the primary.
	 */
	(void) fts_update(p, 'm', 'p', false, false, true);
	if (fts_nodes_changed)
	{
		p->processed = true;
		return;
	}
	FTS_LOG(FTS_LOG_VERBOSE, "FTS promoting mirror (content=%d, dbid=%d) to be the new primary",
			mirror->content, mirror->dbid);
	swap = p->primary;
	p->primary = p->mirror;
	p->mirror = swap;
	fts_next_message(p, FTS_MSG_PROMOTE, p->primary);
}

/* A pair's message has succeeded or failed for good: what follows from it. */
static void
fts_process(FtsPair *p)
{
	const GpSegmentConfig *target = &fts_nodes[p->target];

	switch (p->message)
	{
		case FTS_MSG_PROBE:
			if (p->succeeded)
				fts_probe_answered(p);
			else
				fts_primary_down(p);
			break;

		case FTS_MSG_SYNCREP_ON:
			if (p->succeeded)
			{
				p->syncrep_on = true;
				fts_probe_answered(p);
			}
			else
			{
				ereport(WARNING,
						(errmsg("FTS failed to turn on syncrep on (content=%d, dbid=%d)",
								target->content, target->dbid)));
				p->processed = true;
			}
			break;

		case FTS_MSG_SYNCREP_OFF:
			/*
			 * Another attempt is made in the next cycle; until then the
			 * commits wait, a better thing to do than to PANIC.
			 */
			if (!p->succeeded)
				ereport(WARNING,
						(errmsg("FTS failed to turn off syncrep on (content=%d, dbid=%d)",
								target->content, target->dbid)));
			else
				FTS_LOG(FTS_LOG_VERBOSE,
						"FTS primary (content=%d, dbid=%d) notified to turn syncrep off",
						target->content, target->dbid);
			p->processed = true;
			break;

		case FTS_MSG_PROMOTE:
			if (!p->succeeded)
				ereport(WARNING,
						(errmsg("ERROR: FTS double fault detected (content=%d) primary dbid=%d, mirror dbid=%d",
								target->content, target->dbid,
								fts_nodes[p->mirror].dbid)));
			else
				FTS_LOG(FTS_LOG_VERBOSE,
						"FTS mirror (content=%d, dbid=%d) promotion triggered successfully",
						target->content, target->dbid);
			p->processed = true;
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* The prober: a cycle                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Probe every primary that has a mirror, at once, and act on the answers:
 * Cloudberry's FtsWalRepMessageSegments().  Only on the coordinator: on a
 * standby promoted, until gp_activate_standby() has made it the coordinator,
 * a cycle does nothing.
 */
static void
fts_cycle(void)
{
	FtsPair    *pairs;
	int			npairs = 0;
	const GpSegmentConfig *self = GpClusterSelf();

	/*
	 * The nodes as they are now, and their states: a place whose node is
	 * not the one this process adopted -- one added, removed or moved just
	 * now -- makes the cycle wait for the next.
	 */
	fts_nodes_changed = false;
	(void) GpClusterRefresh();
	(void) GpClusterLiveStates(fts_states);
	for (int i = 0; i < fts_nnodes; i++)
	{
		if (fts_states[i].dbid != fts_nodes[i].dbid)
		{
			fts_nodes_changed = true;
			return;
		}
		if (fts_dbids[i] != fts_nodes[i].dbid)
		{
			fts_dbids[i] = fts_nodes[i].dbid;
			fts_restarting_since[i] = 0;
		}
	}
	if (self == NULL || !GpClusterIsPrimaryNow(self->dbid))
		return;
	if (!fts_resolve_nodes())
		return;

	pairs = palloc0_array(FtsPair, fts_nnodes);
	for (int i = 0; i < fts_nnodes; i++)
	{
		FtsPair    *p;
		int			mirror = -1;

		if (fts_nodes[i].content < 0 || fts_states[i].role != 'p')
			continue;
		for (int j = 0; j < fts_nnodes; j++)
			if (j != i && fts_nodes[j].content == fts_nodes[i].content)
				mirror = j;
		if (mirror < 0)
			continue;			/* no mirror, nothing to fail over to */

		p = &pairs[npairs++];
		p->primary = i;
		p->mirror = mirror;
		fts_next_message(p, FTS_MSG_PROBE, i);
	}

	FTS_LOG(FTS_LOG_DEBUG, "FTS: starting scan with %d contents", npairs);

	for (;;)
	{
		bool		all_processed = true;
		TimestampTz now;
		long		timeout_ms = 1000;
		WaitEventSet *wes;
		WaitEvent  *events;
		int			nevents;

		CHECK_FOR_INTERRUPTS();

		for (int i = 0; i < npairs; i++)
		{
			FtsPair    *p = &pairs[i];

			if (p->processed)
				continue;
			fts_advance(p);
			if (p->step == FTS_STEP_DONE)
				fts_process(p);
			if (!p->processed)
				all_processed = false;
		}
		if (all_processed)
			break;

		/* Wait for a socket, a retry's time, or a timeout, whichever is first. */
		now = GetCurrentTimestamp();
		wes = CreateWaitEventSet(NULL, npairs + 2);
		events = palloc_array(WaitEvent, npairs + 2);
		AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		AddWaitEventToSet(wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
		for (int i = 0; i < npairs; i++)
		{
			FtsPair    *p = &pairs[i];
			TimestampTz deadline;

			if (p->processed)
				continue;
			if (p->step == FTS_STEP_RETRY_WAIT)
				deadline = p->retry_at;
			else if (p->step == FTS_STEP_CONNECT || p->step == FTS_STEP_RECEIVE)
			{
				AddWaitEventToSet(wes, p->want, PQsocket(p->conn), NULL, p);
				deadline = TimestampTzPlusMilliseconds(p->attempt_start,
													   gp_fts_probe_timeout * 1000L + 1);
			}
			else
				continue;
			timeout_ms = Min(timeout_ms, TimestampDifferenceMilliseconds(now, deadline));
		}

		nevents = WaitEventSetWait(wes, timeout_ms, events, npairs + 2,
								   fts_wait_event());
		for (int e = 0; e < nevents; e++)
		{
			if (events[e].events & WL_LATCH_SET)
				ResetLatch(MyLatch);
			else if (events[e].user_data != NULL)
				((FtsPair *) events[e].user_data)->ready = true;
		}
		FreeWaitEventSet(wes);
		pfree(events);
	}

	for (int i = 0; i < npairs; i++)
		fts_close(&pairs[i]);
	pfree(pairs);
}

/* ------------------------------------------------------------------------- */
/* The prober: its process                                                   */
/* ------------------------------------------------------------------------- */

static void
fts_prober_exit(int code, Datum arg)
{
	SpinLockAcquire(&fts_shared->mutex);
	fts_shared->prober_pid = 0;
	fts_shared->prober_procno = INVALID_PROC_NUMBER;
	SpinLockRelease(&fts_shared->mutex);
	ConditionVariableBroadcast(&fts_shared->cv);
}

PGDLLEXPORT void GpFtsProberMain(Datum main_arg);

/*
 * Cloudberry's FtsLoop(): a cycle every gp.fts_probe_interval, or at once
 * when a backend asks for one, and the counts a backend that asked waits on.
 */
void
GpFtsProberMain(Datum main_arg)
{
	MemoryContext cycle_cxt;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	StartTransactionCommand();
	fts_user = MemoryContextStrdup(TopMemoryContext,
								   GetUserNameFromId(BOOTSTRAP_SUPERUSERID, false));
	CommitTransactionCommand();

	fts_nnodes = GpClusterNodes(&fts_nodes);
	fts_states = MemoryContextAllocZero(TopMemoryContext,
										Max(fts_nnodes, 1) * sizeof(GpClusterNodeState));
	fts_restarting_since = MemoryContextAllocZero(TopMemoryContext,
												  Max(fts_nnodes, 1) * sizeof(TimestampTz));
	fts_dbids = MemoryContextAllocZero(TopMemoryContext,
									   Max(fts_nnodes, 1) * sizeof(int));
	cycle_cxt = AllocSetContextCreate(TopMemoryContext, "gp_core fts",
									  ALLOCSET_DEFAULT_SIZES);

	SpinLockAcquire(&fts_shared->mutex);
	fts_shared->prober_pid = MyProcPid;
	fts_shared->prober_procno = MyProcNumber;
	SpinLockRelease(&fts_shared->mutex);
	before_shmem_exit(fts_prober_exit, (Datum) 0);

	for (;;)
	{
		TimestampTz start;
		bool		requested;
		long		timeout_ms;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		(void) GP_FAULT("ftsLoop_before_probe");

		start = GetCurrentTimestamp();
		SpinLockAcquire(&fts_shared->mutex);
		fts_shared->start_count++;
		fts_shared->probe_requested = false;
		SpinLockRelease(&fts_shared->mutex);
		ConditionVariableBroadcast(&fts_shared->cv);

		if (GP_FAULT("fts_probe") == GP_FAULT_SKIP)
			FTS_LOG(FTS_LOG_VERBOSE, "skipping FTS probes due to %s", "fts_probe fault");
		else
		{
			MemoryContext old = MemoryContextSwitchTo(cycle_cxt);

			fts_cycle();
			MemoryContextSwitchTo(old);
			MemoryContextReset(cycle_cxt);
		}

		(void) GP_FAULT("ftsLoop_after_probe");

		SpinLockAcquire(&fts_shared->mutex);
		fts_shared->done_count = fts_shared->start_count;
		requested = fts_shared->probe_requested || fts_nodes_changed;
		SpinLockRelease(&fts_shared->mutex);
		ConditionVariableBroadcast(&fts_shared->cv);

		/*
		 * A request that came during the cycle may have had its latch taken by
		 * the cycle's own waits, so the flag is what says whether to go again
		 * at once.
		 */
		if (requested)
			timeout_ms = 0;
		else
			timeout_ms = Max(0, (long) gp_fts_probe_interval * 1000L -
							 TimestampDifferenceMilliseconds(start, GetCurrentTimestamp()));

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms, fts_wait_event());
		ResetLatch(MyLatch);
		(void) GP_FAULT("ftsLoop_after_latch");
	}
}

void
GpFtsNotifyProber(void)
{
	uint32		initial;
	uint32		started;
	int			pid;
	ProcNumber	procno;

	if (fts_shared == NULL)
		return;

	SpinLockAcquire(&fts_shared->mutex);
	pid = fts_shared->prober_pid;
	procno = fts_shared->prober_procno;
	initial = fts_shared->start_count;
	if (pid != 0)
		fts_shared->probe_requested = true;
	SpinLockRelease(&fts_shared->mutex);

	/* None runs: a standby coordinator, until it is promoted. */
	if (pid == 0 || procno == INVALID_PROC_NUMBER)
		return;
	SetLatch(&GetPGProcByNumber(procno)->procLatch);

	(void) GP_FAULT("ftsNotify_before");

	/*
	 * A cycle that began after the asking, and its end: what one that was
	 * already under way found may be older than what the caller wants to
	 * know.  Two that ask before a cycle begins share it.
	 */
	ConditionVariablePrepareToSleep(&fts_shared->cv);
	for (;;)
	{
		SpinLockAcquire(&fts_shared->mutex);
		started = fts_shared->start_count;
		pid = fts_shared->prober_pid;
		SpinLockRelease(&fts_shared->mutex);
		if (started != initial || pid == 0)
			break;
		(void) ConditionVariableTimedSleep(&fts_shared->cv, 1000, fts_wait_event());
	}
	for (;;)
	{
		uint32		done;

		SpinLockAcquire(&fts_shared->mutex);
		done = fts_shared->done_count;
		pid = fts_shared->prober_pid;
		SpinLockRelease(&fts_shared->mutex);
		if ((int32) (done - started) >= 0 || pid == 0)
			break;
		(void) ConditionVariableTimedSleep(&fts_shared->cv, 1000, fts_wait_event());
	}
	ConditionVariableCancelSleep();
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_request_fts_probe_scan);

/*
 * gp_request_fts_probe_scan()
 *		Probe now, and return once a probe that began after the call has
 *		ended; Cloudberry's, only on the coordinator.
 */
Datum
gp_request_fts_probe_scan(PG_FUNCTION_ARGS)
{
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errmsg("this function can only be called by master (without utility mode)")));

	GpFtsNotifyProber();

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(gp_fts_history);

/*
 * gp_internal.fts_history()
 *		What FTS recorded of the changes it made, from its file in the
 *		coordinator's data directory: gp_configuration_history's rows,
 *		readable from every database, as Cloudberry's shared catalog is.
 *		None on a segment, whose FTS never ran.
 */
Datum
gp_fts_history(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	FILE	   *fp;
	char		buf[1024];

	InitMaterializedSRF(fcinfo, 0);

	fp = AllocateFile(FTS_HISTORY_FILE, "r");
	if (fp == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", FTS_HISTORY_FILE)));
		return (Datum) 0;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL)
	{
		Datum		values[3];
		bool		nulls[3] = {false, false, false};
		char	   *tab1 = strchr(buf, '\t');
		char	   *tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
		size_t		len;

		if (tab2 == NULL)
			continue;			/* a line a crash cut short */
		*tab1 = *tab2 = '\0';
		len = strlen(tab2 + 1);
		if (len > 0 && tab2[len] == '\n')
			tab2[len] = '\0';

		values[0] = TimestampTzGetDatum((TimestampTz) strtoi64(buf, NULL, 10));
		values[1] = Int16GetDatum((int16) atoi(tab1 + 1));
		values[2] = CStringGetTextDatum(tab2 + 1);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	FreeFile(fp);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_segment_replication);

/*
 * gp_internal.segment_replication()
 *		On the coordinator, each content that has a mirror, and its
 *		primary's WAL sender to it as that primary's pg_stat_replication
 *		shows it, as a json object -- NULL where it has none, the mirror
 *		being gone -- for gp_stat_replication's rows of the segments, which
 *		Cloudberry gathers so (gp_stat_get_segment_replication()).  Nothing
 *		on any other node, which has only its own.
 */
Datum
gp_segment_replication(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const GpSegmentConfig *nodes;
	int			nnodes;
	int			nsegs;
	char	  **values;

	InitMaterializedSRF(fcinfo, 0);
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH || !GpClusterHasMirrors())
		return (Datum) 0;

	(void) GpClusterSegments(&nsegs);
	values = palloc0_array(char *, nsegs);
	GpDispatchQueryFirstValues("SELECT pg_catalog.row_to_json(r)::text"
							   " FROM pg_catalog.pg_stat_replication r"
							   " WHERE r.application_name = '" GP_WALRECEIVER_APPNAME "'"
							   " LIMIT 1", -1, values);

	nnodes = GpClusterNodes(&nodes);
	for (int content = 0; content < nsegs; content++)
	{
		Datum		v[2];
		bool		nulls[2] = {false, false};
		bool		mirrored = false;

		for (int i = 0; i < nnodes; i++)
			if (nodes[i].content == content && nodes[i].preferred_role == 'm')
				mirrored = true;
		if (!mirrored)
			continue;

		v[0] = Int32GetDatum(content);
		if (values[content] != NULL)
			v[1] = CStringGetTextDatum(values[content]);
		else
			nulls[1] = true;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}

	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpFtsInit(void)
{
	const GpSegmentConfig *self;

	DefineCustomIntVariable("gp.fts_probe_interval",
							"How often FTS probes the primaries.",
							"A complete probe of all segments starts each time a timer with this period expires.",
							&gp_fts_probe_interval,
							60, 10, 3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.fts_probe_timeout",
							"How long FTS waits for a segment's answer.",
							NULL,
							&gp_fts_probe_timeout,
							20, 0, 3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.fts_probe_retries",
							"How many times FTS asks a segment again before it gives it up.",
							NULL,
							&gp_fts_probe_retries,
							5, 0, 100,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.fts_mark_mirror_down_grace_period",
							"How long a mirror may take to connect before FTS marks it down.",
							"Counted from the primary's start, or from the end of its "
							"mirror's last WAL sender.",
							&gp_fts_mark_mirror_down_grace_period,
							30, 0, 3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.fts_replication_attempt_count",
							"How many times a mirror's WAL sender may end, since it last streamed, before FTS marks the mirror down.",
							"Past it, the grace period counts from the primary's "
							"start alone.",
							&gp_fts_replication_attempt_count,
							10, 0, 100,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.log_fts",
							 "How much FTS logs.",
							 NULL,
							 &gp_log_fts,
							 FTS_LOG_TERSE,
							 gp_log_fts_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gp.fts_status",
							   "This node's answer to an FTS probe.",
							   "Read-only: dbid, content id, and whether the mirror is up, "
							   "in sync, synchronous replication on, this node a mirror, "
							   "FTS to ask again, the mirror ready for synchronous "
							   "replication.",
							   &gp_fts_status_setting,
							   "",
							   PGC_INTERNAL,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, fts_show_status);

	if (GpClusterIsSingleNode())
		return;

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = fts_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = fts_shmem_startup;

	prev_client_auth = ClientAuthentication_hook;
	ClientAuthentication_hook = fts_client_auth;

	self = GpClusterSelf();

	/*
	 * R3, on a segment: its commits wait for its mirror, and a cancel does not
	 * end the wait.
	 */
	if (self != NULL && self->content >= 0)
	{
		SyncRepHoldCancelDuringWait = true;
		RegisterXactCallback(fts_xact_callback, NULL);
		prev_process_utility = ProcessUtility_hook;
		ProcessUtility_hook = fts_process_utility;
	}

	/*
	 * The prober, on the coordinator, and on its standby, where it starts once
	 * a promotion has ended recovery and probes once gp_activate_standby() has
	 * made the node the coordinator (fts_cycle()).  A segment's mirror may be
	 * added while the coordinator runs -- gpinitsystem's
	 * gp_add_segment_mirror(), gpaddmirrors' gp_add_segment() -- and FTS is
	 * what marks it up: so the prober runs on a coordinator of segments that
	 * have no mirror too, a cycle finding nothing to probe.
	 */
	if (self != NULL && self->content == -1)
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 1;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpFtsProberMain");
		snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core fts prober");
		snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core fts prober");
		RegisterBackgroundWorker(&worker);
	}
}
