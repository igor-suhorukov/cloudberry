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
 * login.c
 *	  Counting failed logins, and locking an account that has too many.
 *
 * Cloudberry keeps the count and the lock date in pg_authid, and updates them
 * from a "login monitor": a pair of postmaster children, because the backend
 * whose login just failed is about to exit and can commit nothing.  The port
 * has the same problem and the same shape of answer, with two differences.
 *
 * First, the live state is in shared memory and the label is its durable
 * copy.  So the backend that sees the failure decides on the spot whether
 * this was the last attempt, and the worker writes the answer down
 * afterwards; nothing has to wait for another process to make up its mind.
 * A backend that finds nothing in shared memory reads the label itself, so a
 * server that has just started locks out exactly whom it locked out before.
 *
 * Second, a locked account is refused here rather than by taking its LOGIN
 * away.  Cloudberry sets pg_authid.rolaccountstatus; the plan for the port
 * was ALTER ROLE ... NOLOGIN, which then has to be told apart from a NOLOGIN
 * an administrator set.  Refusing in the hook needs neither: rolcanlogin
 * keeps meaning what an administrator said, and the lock is only ever the
 * label.  It also keeps Cloudberry's message, which "Profiles, login monitor
 * and login restrictions" expected to lose.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/postmaster/loginmonitor.c, and the profile code of auth.c
 *	  and postinit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "commands/seclabel.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "gp_label.h"
#include "gp_security.h"

#define GP_SECURITY_LIBRARY		"gp_security"
#define GP_LOGIN_HASH_NAME		"gp_security login state"
#define GP_LOGIN_CONTROL_NAME	"gp_security control"

/* How often the worker looks for locks whose time is up. */
#define GP_LOGIN_SWEEP_MS		10000

typedef struct GpLoginEntry
{
	Oid			roleid;			/* the dshash key */
	int			failed_logins;
	TimestampTz locked_until;	/* 0 not locked; DT_NOEND until unlocked */
	bool		dirty;			/* not written to the label yet */
} GpLoginEntry;

typedef struct GpLoginControl
{
	pid_t		worker_pid;
} GpLoginControl;

static ClientAuthentication_hook_type prev_ClientAuthentication = NULL;

static dshash_table *login_hash = NULL;
static GpLoginControl *login_control = NULL;
static int	login_tranche_id = 0;

/* ------------------------------------------------------------------------- */
/* The shared state                                                          */
/* ------------------------------------------------------------------------- */

static void
login_control_init(void *ptr, void *arg)
{
	GpLoginControl *ctl = (GpLoginControl *) ptr;

	ctl->worker_pid = 0;
}

static void
login_attach(void)
{
	static const dshash_parameters params = {
		.key_size = sizeof(Oid),
		.entry_size = sizeof(GpLoginEntry),
		.compare_function = dshash_memcmp,
		.hash_function = dshash_memhash,
		.copy_function = dshash_memcpy,
	};
	dshash_parameters p = params;
	bool		found;

	if (login_hash != NULL)
		return;

	login_control = GetNamedDSMSegment(GP_LOGIN_CONTROL_NAME,
									   sizeof(GpLoginControl),
									   login_control_init, &found, NULL);

	if (login_tranche_id == 0)
		login_tranche_id = LWLockNewTrancheId(GP_LOGIN_HASH_NAME);
	p.tranche_id = login_tranche_id;

	login_hash = GetNamedDSHash(GP_LOGIN_HASH_NAME, &p, &found);
}

/* The label is the durable copy; read it when shared memory has nothing. */
static void
login_state_from_label(Oid roleid, GpLoginState *out)
{
	ObjectAddress addr;
	char	   *value;

	out->failed_logins = 0;
	out->locked_until = 0;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);

	value = GpLabelGet(&addr, GP_LABEL_failed_logins);
	if (value != NULL)
		out->failed_logins = atoi(value);

	value = GpLabelGet(&addr, GP_LABEL_locked_until);
	if (value != NULL)
	{
		if (strcmp(value, "forever") == 0)
			out->locked_until = DT_NOEND;
		else
			out->locked_until =
				DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
														CStringGetDatum(value),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
	}
}

void
GpLoginStateGet(Oid roleid, GpLoginState *out)
{
	GpLoginEntry *entry;
	bool		found;

	login_attach();

	entry = dshash_find_or_insert(login_hash, &roleid, &found);
	if (!found)
	{
		GpLoginState fromlabel;

		/*
		 * Nothing here yet.  Read the durable copy, so that a server which
		 * has just started keeps locking out whoever it locked out before.
		 */
		login_state_from_label(roleid, &fromlabel);
		entry->failed_logins = fromlabel.failed_logins;
		entry->locked_until = fromlabel.locked_until;
		entry->dirty = false;
	}

	out->failed_logins = entry->failed_logins;
	out->locked_until = entry->locked_until;

	dshash_release_lock(login_hash, entry);
}

void
GpLoginStateSet(Oid roleid, const GpLoginState *state)
{
	GpLoginEntry *entry;
	bool		found;

	login_attach();

	entry = dshash_find_or_insert(login_hash, &roleid, &found);
	entry->failed_logins = state->failed_logins;
	entry->locked_until = state->locked_until;
	entry->dirty = true;
	dshash_release_lock(login_hash, entry);

	GpSecurityWakeWorker(roleid);
}

/* Tell the worker there is something to write down. */
void
GpSecurityWakeWorker(Oid roleid)
{
	pid_t		pid;
	PGPROC	   *proc;

	login_attach();

	pid = login_control->worker_pid;
	if (pid == 0)
		return;

	proc = BackendPidGetProc(pid);
	if (proc != NULL)
		SetLatch(&proc->procLatch);
}

/* ------------------------------------------------------------------------- */
/* The hook                                                                  */
/* ------------------------------------------------------------------------- */

static bool
login_is_locked(const GpLoginState *st, TimestampTz now)
{
	return st->locked_until == DT_NOEND ||
		(st->locked_until != 0 && st->locked_until > now);
}

/*
 * Every authentication comes through here, successful or not.
 */
static void
gp_security_ClientAuthentication(Port *port, int status)
{
	Oid			roleid;
	GpProfile	profile;
	GpLoginState st;
	TimestampTz now;

	if (prev_ClientAuthentication)
		prev_ClientAuthentication(port, status);

	if (!gp_enable_password_profile || port->user_name == NULL)
		return;

	roleid = get_role_oid(port->user_name, true);
	if (!OidIsValid(roleid))
		return;

	/* A role with no profile is held to nothing. */
	if (!GpProfileForRole(roleid, &profile))
		return;

	now = GetCurrentTimestamp();
	GpLoginStateGet(roleid, &st);

	if (status == STATUS_OK)
	{
		if (login_is_locked(&st, now))
		{
			if (st.locked_until == DT_NOEND)
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("role \"%s\" is locked", port->user_name),
						 errdetail("Too many failed login attempts."),
						 errhint("An administrator can unlock it with %s.unlock_role().",
								 GP_SECURITY_SCHEMA)));
			else
				ereport(FATAL,
						(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
						 errmsg("role \"%s\" is locked until %s",
								port->user_name,
								timestamptz_to_str(st.locked_until)),
						 errdetail("Too many failed login attempts.")));
		}

		/* A good login clears what came before it. */
		if (st.failed_logins != 0)
		{
			st.failed_logins = 0;
			GpLoginStateSet(roleid, &st);
		}

		return;
	}

	/*
	 * A failed attempt.  Nothing is counted unless the profile says how many
	 * are too many, which is what Cloudberry's FAILED_LOGIN_ATTEMPTS does.
	 */
	if (profile.failed_login_attempts <= 0)
		return;

	st.failed_logins++;

	if (st.failed_logins >= profile.failed_login_attempts)
	{
		/*
		 * PASSWORD_LOCK_TIME says for how long.  Left unset it means until an
		 * administrator unlocks the account, which is the safe reading.
		 */
		if (profile.password_lock_time > 0)
			st.locked_until = TimestampTzPlusMilliseconds(now,
														  (int64) profile.password_lock_time *
														  86400 * 1000);
		else
			st.locked_until = DT_NOEND;

		st.failed_logins = 0;

		ereport(LOG,
				(errmsg("locking role \"%s\": %d failed login attempts",
						port->user_name, profile.failed_login_attempts)));
	}

	GpLoginStateSet(roleid, &st);
}

void
GpLoginInstallHook(void)
{
	prev_ClientAuthentication = ClientAuthentication_hook;
	ClientAuthentication_hook = gp_security_ClientAuthentication;
}

/* ------------------------------------------------------------------------- */
/* The worker                                                                */
/* ------------------------------------------------------------------------- */

/* Write one role's state into its label.  Called inside a transaction. */
static void
login_persist(Oid roleid, const GpLoginState *st)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);

	/* The role may have been dropped since the hook saw it. */
	if (!SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(roleid)))
		return;

	if (st->failed_logins > 0)
		GpLabelSet(&addr, GP_LABEL_failed_logins, psprintf("%d", st->failed_logins));
	else
		GpLabelSet(&addr, GP_LABEL_failed_logins, NULL);

	if (st->locked_until == DT_NOEND)
		GpLabelSet(&addr, GP_LABEL_locked_until, "forever");
	else if (st->locked_until != 0)
		GpLabelSet(&addr, GP_LABEL_locked_until,
				   timestamptz_to_str(st->locked_until));
	else
		GpLabelSet(&addr, GP_LABEL_locked_until, NULL);
}

/*
 * One pass: write down what the backends decided, and let go of the locks
 * whose time is up.
 *
 * What is written down is collected first and written afterwards.  A scan
 * holds the hash's partition locks, and writing a label takes catalog locks;
 * a backend doing the reverse -- unlock_role() writes the label and then
 * updates the hash -- would deadlock against a worker that did both at once.
 */
static void
login_sweep(void)
{
	dshash_seq_status seq;
	GpLoginEntry *entry;
	TimestampTz now = GetCurrentTimestamp();
	int			ntodo = 0;
	int			maxtodo = 16;
	struct
	{
		Oid			roleid;
		GpLoginState st;
	}		   *todo;

	login_attach();

	todo = palloc(maxtodo * sizeof(*todo));

	dshash_seq_init(&seq, login_hash, true);
	while ((entry = dshash_seq_next(&seq)) != NULL)
	{
		/* A lock whose time is up is no lock at all. */
		if (entry->locked_until != 0 && entry->locked_until != DT_NOEND &&
			entry->locked_until <= now)
		{
			entry->locked_until = 0;
			entry->dirty = true;
		}

		if (!entry->dirty)
			continue;

		if (ntodo == maxtodo)
		{
			maxtodo *= 2;
			todo = repalloc(todo, maxtodo * sizeof(*todo));
		}

		todo[ntodo].roleid = entry->roleid;
		todo[ntodo].st.failed_logins = entry->failed_logins;
		todo[ntodo].st.locked_until = entry->locked_until;
		ntodo++;

		entry->dirty = false;
	}
	dshash_seq_term(&seq);

	if (ntodo == 0)
	{
		pfree(todo);
		return;
	}

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	for (int i = 0; i < ntodo; i++)
		login_persist(todo[i].roleid, &todo[i].st);

	PopActiveSnapshot();
	CommitTransactionCommand();

	pfree(todo);
}

PGDLLEXPORT void gp_security_worker_main(Datum arg);

void
gp_security_worker_main(Datum arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_security_database, NULL, 0);

	login_attach();
	login_control->worker_pid = MyProcPid;

	ereport(LOG,
			(errmsg("gp_security is watching logins, with its tables in database \"%s\"",
					gp_security_database)));

	for (;;)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 GP_LOGIN_SWEEP_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		login_sweep();
	}
}

void
GpSecurityRegisterWorker(void)
{
	BackgroundWorker worker;

	MemSet(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 10;
	snprintf(worker.bgw_library_name, MAXPGPATH, "%s", GP_SECURITY_LIBRARY);
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "gp_security_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_security login monitor");
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_security login monitor");
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}
