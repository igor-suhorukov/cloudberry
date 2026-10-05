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
 * gp_signal.c
 *	  pg_terminate_backend(pid, message) and pg_cancel_backend(pid,
 *	  message): a backend ended, or its statement cancelled, with a word
 *	  from whoever did it.
 *
 * Cloudberry's variants of PostgreSQL's two functions take the message as
 * text, where PostgreSQL 19's pg_terminate_backend() takes a timeout as its
 * second argument; an unknown literal is text, so a call with a quoted
 * message takes these.  Cloudberry keeps the message in a slot of shared
 * memory for each backend (backend_cancel.c), which the backend reads as it
 * ends or cancels (ProcessInterrupts()) and says after PostgreSQL's words:
 * "terminating connection due to administrator command: "<message>"", and
 * "canceling statement due to user request: "<message>"".  The port has the
 * slots, by proc number, and its emit_log_hook says the message after those
 * two errors of the backend it is for; the signal is PostgreSQL's own
 * function's, with its checks of who may send it.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/misc/backend_cancel.c, and pg_terminate_backend_msg()
 *	  and pg_cancel_backend_msg() in src/backend/storage/ipc/signalfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/fmgrprotos.h"

#include "gp_signal.h"

/* Cloudberry's MAX_CANCEL_MSG, the terminating zero with it. */
#define GP_SIGNAL_MESSAGE_LEN	128

/* PostgreSQL 19's words for the two, as errmsg() is given them */
#define TERMINATE_MSGID		"terminating connection due to administrator command"
#define CANCEL_MSGID		"canceling statement due to user request"

typedef struct GpSignalSlot
{
	slock_t		mutex;
	int			pid;			/* the backend it is for; 0, none */
	char		message[GP_SIGNAL_MESSAGE_LEN];
} GpSignalSlot;

static GpSignalSlot *signal_slots = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;

static void
signal_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(mul_size(MaxBackends, sizeof(GpSignalSlot)));
}

static void
signal_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	signal_slots = ShmemInitStruct("gp_core signal messages",
								   mul_size(MaxBackends, sizeof(GpSignalSlot)),
								   &found);
	if (!found)
	{
		memset(signal_slots, 0, mul_size(MaxBackends, sizeof(GpSignalSlot)));
		for (int i = 0; i < MaxBackends; i++)
			SpinLockInit(&signal_slots[i].mutex);
	}
	LWLockRelease(AddinShmemInitLock);
}

/* The slot of the backend of that pid, or NULL where it is none. */
static GpSignalSlot *
slot_of(int pid)
{
	PGPROC	   *proc = BackendPidGetProc(pid);
	int			procno;

	if (proc == NULL || signal_slots == NULL)
		return NULL;
	procno = GetNumberFromPGProc(proc);
	if (procno < 0 || procno >= MaxBackends)
		return NULL;
	return &signal_slots[procno];
}

/*
 * The message for the backend of that pid, before it is signalled; as
 * Cloudberry's, one that does not fit is cut, and said so.
 */
static GpSignalSlot *
message_set(int pid, const char *message)
{
	GpSignalSlot *slot = slot_of(pid);
	int			len = strlen(message);

	if (slot == NULL)
		return NULL;
	SpinLockAcquire(&slot->mutex);
	slot->pid = pid;
	strlcpy(slot->message, message, GP_SIGNAL_MESSAGE_LEN);
	SpinLockRelease(&slot->mutex);

	if (len >= GP_SIGNAL_MESSAGE_LEN)
		ereport(NOTICE,
				(errmsg("message is too long and has been truncated")));
	return slot;
}

/* The message taken back, where the signal did not go: still that backend's. */
static void
message_unset(GpSignalSlot *slot, int pid)
{
	if (slot == NULL)
		return;
	SpinLockAcquire(&slot->mutex);
	if (slot->pid == pid)
	{
		slot->pid = 0;
		slot->message[0] = '\0';
	}
	SpinLockRelease(&slot->mutex);
}

/*
 * PostgreSQL's function, which checks who may signal the backend and
 * signals it, with the message set first, so that the backend finds it
 * as it takes the signal.
 */
static Datum
signal_with_message(FunctionCallInfo fcinfo, bool terminate)
{
	int			pid = PG_GETARG_INT32(0);
	char	   *message = text_to_cstring(PG_GETARG_TEXT_PP(1));
	GpSignalSlot *slot = message_set(pid, message);
	bool		signalled;

	PG_TRY();
	{
		signalled = terminate
			? DatumGetBool(DirectFunctionCall2(pg_terminate_backend,
											   Int32GetDatum(pid),
											   Int64GetDatum(0)))
			: DatumGetBool(DirectFunctionCall1(pg_cancel_backend,
											   Int32GetDatum(pid)));
	}
	PG_CATCH();
	{
		message_unset(slot, pid);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!signalled)
		message_unset(slot, pid);
	PG_RETURN_BOOL(signalled);
}

PG_FUNCTION_INFO_V1(gp_terminate_backend_msg);
PG_FUNCTION_INFO_V1(gp_cancel_backend_msg);

/*
 * pg_catalog.pg_terminate_backend(pid int, message text)
 *		End the backend, which says the message as it ends.
 */
Datum
gp_terminate_backend_msg(PG_FUNCTION_ARGS)
{
	return signal_with_message(fcinfo, true);
}

/*
 * pg_catalog.pg_cancel_backend(pid int, message text)
 *		Cancel the backend's statement, which says the message as it fails.
 */
Datum
gp_cancel_backend_msg(PG_FUNCTION_ARGS)
{
	return signal_with_message(fcinfo, false);
}

/*
 * The message of this backend's, after PostgreSQL's words for its end or its
 * cancel, as Cloudberry's ProcessInterrupts() says it; taken, so that it is
 * said once.  Any other error is left as it is.
 */
static void
signal_emit_log(ErrorData *edata)
{
	if (signal_slots != NULL && MyProcNumber >= 0 && MyProcNumber < MaxBackends &&
		edata->message_id != NULL &&
		((edata->elevel == FATAL && edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN &&
		  strcmp(edata->message_id, TERMINATE_MSGID) == 0) ||
		 (edata->elevel == ERROR && edata->sqlerrcode == ERRCODE_QUERY_CANCELED &&
		  strcmp(edata->message_id, CANCEL_MSGID) == 0)))
	{
		GpSignalSlot *slot = &signal_slots[MyProcNumber];
		char		message[GP_SIGNAL_MESSAGE_LEN];

		message[0] = '\0';
		SpinLockAcquire(&slot->mutex);
		if (slot->pid == MyProcPid)
		{
			strlcpy(message, slot->message, GP_SIGNAL_MESSAGE_LEN);
			slot->pid = 0;
			slot->message[0] = '\0';
		}
		SpinLockRelease(&slot->mutex);

		if (message[0] != '\0')
			edata->message = psprintf("%s: \"%s\"", edata->message, message);
	}

	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);
}

void
GpSignalInit(void)
{
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = signal_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = signal_shmem_startup;
	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = signal_emit_log;
}
