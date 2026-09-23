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
 * gp_share.c
 *	  One transaction, several backends of a segment: the shared snapshot.
 *
 * When a query's slices run at once, a segment runs each in a backend of its
 * own, and all of them are one statement of one transaction: the writer's,
 * the session's first backend on the segment, which the coordinator's
 * transaction was begun in.  Its readers have to see what it wrote before
 * the statement began and not what it writes during it, and must not wait
 * for its locks.  Cloudberry does that in its core, with a snapshot the
 * writer publishes in shared memory (sharedsnapshot.c), a reader's branch in
 * TransactionIdIsCurrentTransactionId, and locks a reader takes as its
 * writer's.  The port does it the way PostgreSQL hands a transaction to its
 * parallel workers, with what PostgreSQL 19 exports and two patches of the
 * series:
 *
 *   the writer, as a fragment it runs starts: SerializeSnapshot() of the
 *   fragment's snapshot and SerializeTransactionState() -- its XIDs and its
 *   command -- published under the statement's key; its combo command IDs,
 *   published as it makes them (R2's combocid_create_hook); and
 *   BecomeLockGroupLeader();
 *
 *   a reader, in a transaction of its own that the coordinator begins
 *   REPEATABLE READ and READ ONLY and gives "gp.shared_snapshot" before
 *   anything else: waits for the publication, then BecomeLockGroupMember(),
 *   XactAdoptTransactionState() (R4: the writer's XIDs are current, and its
 *   command is ours, so a catalog row it wrote is seen as well as a table's),
 *   RestoreTransactionSnapshot() of the writer's snapshot, whose xmin the
 *   writer's own proves still held; and a combo command ID it does not have
 *   is looked up in what the writer published (R2's combocid_miss_hook).
 *
 * A reader's transaction reads only, which READ ONLY enforces, and is over
 * when the statement is.  Its caches are the thing that outlives it: a
 * reader that read the catalog as a transaction that has written saw rows
 * nobody else can, and they would stay cached if that transaction rolled
 * back.  So a reader discards its caches when the writer's transaction has
 * written anything, before it reads, and again at the next attach.
 *
 * A writer keeps each snapshot it published registered until its
 * transaction ends -- or the last of GP_SHARE_MAXPUB publications replaces
 * it -- so that the xmin a reader installs is one the writer still holds,
 * however soon the writer's own fragment ends.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/time/sharedsnapshot.c, and the reader's branches of
 *	  xact.c and lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/condition_variable.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procnumber.h"
#include "tcop/utility.h"
#include "utils/combocid.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_share.h"

/* How many statements' publications a writer keeps at once. */
#define GP_SHARE_MAXPUB		16

/* How long a gp.qe_identity may be to be compared. */
#define GP_SHARE_IDLEN		128

/* One statement's publication: its key and what it holds. */
typedef struct GpSharePub
{
	char		key[GP_SHARE_KEYLEN];	/* "" when free */
	uint64		seq;			/* the oldest is replaced first */
	dsa_pointer data;			/* GpShareData */
} GpSharePub;

/* A backend's slot, by its proc number: what it publishes as a writer. */
typedef struct GpShareSlot
{
	LWLock		lock;
	ConditionVariable cv;		/* broadcast at each publication */
	int			pid;			/* the writer; 0 before it first publishes */
	uint64		xact;			/* which of its transactions published */
	Oid			database;
	Oid			user;
	char		identity[GP_SHARE_IDLEN];
	uint64		nextseq;
	GpSharePub	pubs[GP_SHARE_MAXPUB];

	/* The combo command IDs of the transaction: cmin, cmax pairs. */
	dsa_pointer combos;
	int			ncombos;
	int			maxcombos;
} GpShareSlot;

typedef struct GpShareHeader
{
	int			nslots;
	GpShareSlot slots[FLEXIBLE_ARRAY_MEMBER];
} GpShareHeader;

/* What a publication holds, in the dynamic shared area. */
typedef struct GpShareData
{
	bool		wrote;			/* the writer's transaction has an XID */
	Size		snaplen;		/* SerializeSnapshot()'s */
	Size		tstatelen;		/* SerializeTransactionState()'s */
	char		bytes[FLEXIBLE_ARRAY_MEMBER];	/* the snapshot, then the state */
} GpShareData;

#define SHARE_TSTATE(d)	((d)->bytes + MAXALIGN((d)->snaplen))

static GpShareHeader *share_header = NULL;
static dsa_area *share_area = NULL;

/* The writer's side: its slot, while its transaction publishes. */
static GpShareSlot *writer_slot = NULL;
static List *writer_snapshots = NIL;	/* registered until the end */

/* The reader's side: the writer's slot, while this transaction reads. */
static GpShareSlot *reader_slot = NULL;
static uint64 reader_xact = 0;
static CommandId *reader_combos = NULL; /* cmin, cmax pairs copied so far */
static int	reader_ncombos = 0;

/* The session read a transaction that had written: its caches may be stale. */
static bool reader_caches_stale = false;

static char *shared_snapshot_setting = NULL;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static combocid_create_hook_type prev_combocid_create = NULL;
static combocid_miss_hook_type prev_combocid_miss = NULL;

/* ------------------------------------------------------------------------- */
/* The shared memory                                                         */
/* ------------------------------------------------------------------------- */

static void
share_init_segment(void *ptr, void *arg)
{
	GpShareHeader *header = (GpShareHeader *) ptr;
	int			tranche = LWLockNewTrancheId("gp_core shared snapshot");

	header->nslots = MaxBackends;
	for (int i = 0; i < header->nslots; i++)
	{
		GpShareSlot *slot = &header->slots[i];

		memset(slot, 0, sizeof(GpShareSlot));
		LWLockInitialize(&slot->lock, tranche);
		ConditionVariableInit(&slot->cv);
		for (int p = 0; p < GP_SHARE_MAXPUB; p++)
			slot->pubs[p].data = InvalidDsaPointer;
		slot->combos = InvalidDsaPointer;
	}
}

/* Attached on first use: most backends of a segment never share. */
static void
share_attach(void)
{
	bool		found;

	if (share_header != NULL)
		return;

	share_header = GetNamedDSMSegment("gp_core shared snapshots",
									  offsetof(GpShareHeader, slots) +
									  mul_size(MaxBackends, sizeof(GpShareSlot)),
									  share_init_segment, &found, NULL);
	share_area = GetNamedDSA("gp_core shared snapshot data", &found);
}

static uint32
share_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberrySharedSnapshot");
	return event;
}

/* Forget a slot's publications and combo command IDs; its lock is held. */
static void
slot_clear(GpShareSlot *slot)
{
	for (int p = 0; p < GP_SHARE_MAXPUB; p++)
	{
		if (DsaPointerIsValid(slot->pubs[p].data))
			dsa_free(share_area, slot->pubs[p].data);
		slot->pubs[p].data = InvalidDsaPointer;
		slot->pubs[p].key[0] = '\0';
	}
	if (DsaPointerIsValid(slot->combos))
		dsa_free(share_area, slot->combos);
	slot->combos = InvalidDsaPointer;
	slot->ncombos = 0;
	slot->maxcombos = 0;
}

/* Add a combo command ID to the slot's; its lock is held exclusively. */
static void
slot_add_combo(GpShareSlot *slot, CommandId combocid, CommandId cmin,
			   CommandId cmax)
{
	CommandId  *pairs;

	/* the table only grows, one at a time; a rebuild repeats what is there */
	if ((int) combocid < slot->ncombos)
		return;
	if ((int) combocid != slot->ncombos)
		elog(ERROR, "combo command id %u published out of order", combocid);

	if (slot->ncombos == slot->maxcombos)
	{
		int			newmax = Max(64, slot->maxcombos * 2);
		dsa_pointer newp = dsa_allocate(share_area,
										mul_size(newmax, 2 * sizeof(CommandId)));

		if (slot->ncombos > 0)
			memcpy(dsa_get_address(share_area, newp),
				   dsa_get_address(share_area, slot->combos),
				   slot->ncombos * 2 * sizeof(CommandId));
		if (DsaPointerIsValid(slot->combos))
			dsa_free(share_area, slot->combos);
		slot->combos = newp;
		slot->maxcombos = newmax;
	}

	pairs = (CommandId *) dsa_get_address(share_area, slot->combos);
	pairs[2 * slot->ncombos] = cmin;
	pairs[2 * slot->ncombos + 1] = cmax;
	slot->ncombos++;
}

/* ------------------------------------------------------------------------- */
/* The writer                                                                */
/* ------------------------------------------------------------------------- */

/*
 * The first publication of a transaction takes the slot and puts the combo
 * command IDs made so far in it; the hook below adds the rest as they come.
 */
static void
writer_take_slot(void)
{
	GpShareSlot *slot = &share_header->slots[MyProcNumber];
	Size		len = EstimateComboCIDStateSpace();
	char	   *state = palloc(len);
	int			n;
	CommandId  *pairs;

	SerializeComboCIDState(len, state);
	n = *(int *) state;
	pairs = (CommandId *) (state + sizeof(int));

	LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
	slot_clear(slot);
	slot->pid = MyProcPid;
	slot->xact++;
	slot->database = MyDatabaseId;
	slot->user = GetSessionUserId();
	strlcpy(slot->identity, GpClusterQeIdentity(), GP_SHARE_IDLEN);
	for (int i = 0; i < n; i++)
		slot_add_combo(slot, i, pairs[2 * i], pairs[2 * i + 1]);
	LWLockRelease(&slot->lock);

	pfree(state);
	writer_slot = slot;
}

void
GpSharePublish(const char *key, Snapshot snapshot)
{
	GpShareSlot *slot;
	Size		snaplen;
	Size		tstatelen;
	dsa_pointer dp;
	GpShareData *data;
	GpSharePub *pub = NULL;
	Snapshot	held;
	MemoryContext oldcxt;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a transaction is shared only for the coordinator")));
	if (strlen(key) >= GP_SHARE_KEYLEN)
		elog(ERROR, "shared snapshot key \"%s\" is too long", key);
	if (reader_slot != NULL ||
		(MyProc->lockGroupLeader != NULL && MyProc->lockGroupLeader != MyProc))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("a reader of another backend's transaction cannot share one of its own")));

	share_attach();

	/* Its readers take the locks this backend holds as held. */
	BecomeLockGroupLeader();

	if (writer_slot == NULL)
		writer_take_slot();
	slot = writer_slot;

	snaplen = EstimateSnapshotSpace(snapshot);
	tstatelen = EstimateTransactionStateSpace();
	dp = dsa_allocate(share_area, offsetof(GpShareData, bytes) +
					  MAXALIGN(snaplen) + tstatelen);
	data = (GpShareData *) dsa_get_address(share_area, dp);
	data->wrote = TransactionIdIsValid(GetTopTransactionIdIfAny());
	data->snaplen = snaplen;
	data->tstatelen = tstatelen;
	SerializeSnapshot(snapshot, data->bytes);
	SerializeTransactionState(tstatelen, SHARE_TSTATE(data));

	/* Held until the transaction ends, so that its xmin is ours. */
	held = RegisterSnapshotOnOwner(snapshot, TopTransactionResourceOwner);
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	writer_snapshots = lappend(writer_snapshots, held);
	MemoryContextSwitchTo(oldcxt);

	LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
	for (int p = 0; p < GP_SHARE_MAXPUB; p++)
	{
		GpSharePub *candidate = &slot->pubs[p];

		if (candidate->key[0] == '\0')
		{
			pub = candidate;
			break;
		}
		if (pub == NULL || candidate->seq < pub->seq)
			pub = candidate;
	}
	if (DsaPointerIsValid(pub->data))
		dsa_free(share_area, pub->data);
	strlcpy(pub->key, key, GP_SHARE_KEYLEN);
	pub->seq = ++slot->nextseq;
	pub->data = dp;
	LWLockRelease(&slot->lock);

	ConditionVariableBroadcast(&slot->cv);
}

static void
share_combocid_create(CommandId combocid, CommandId cmin, CommandId cmax)
{
	if (writer_slot != NULL)
	{
		LWLockAcquire(&writer_slot->lock, LW_EXCLUSIVE);
		slot_add_combo(writer_slot, combocid, cmin, cmax);
		LWLockRelease(&writer_slot->lock);
	}

	if (prev_combocid_create)
		prev_combocid_create(combocid, cmin, cmax);
}

/* ------------------------------------------------------------------------- */
/* The reader                                                                */
/* ------------------------------------------------------------------------- */

bool
GpShareIsReader(void)
{
	return reader_slot != NULL;
}

/*
 * Read as a part of the writer's transaction: "<writer pid>/<key>", given
 * as this transaction's gp.shared_snapshot before anything else.
 */
static void
share_attach_reader(const char *value)
{
	char	   *copy = pstrdup(value);
	char	   *slash = strchr(copy, '/');
	char	   *end;
	long		pid;
	const char *key;
	PGPROC	   *proc;
	GpShareSlot *slot;
	char	   *bytes = NULL;
	GpShareData header;
	uint64		xact = 0;
	Snapshot	snapshot;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a transaction is shared only for the coordinator")));
	if (slash == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid value for \"%s\": \"%s\"", GP_SHARE_SETTING,
						value)));
	*slash = '\0';
	key = slash + 1;
	errno = 0;
	pid = strtol(copy, &end, 10);
	if (errno != 0 || *end != '\0' || pid <= 0 || pid > INT_MAX ||
		strlen(key) == 0 || strlen(key) >= GP_SHARE_KEYLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid value for \"%s\": \"%s\"", GP_SHARE_SETTING,
						value)));

	/*
	 * Before the transaction has read anything, and as a transaction that
	 * keeps its first snapshot and cannot write: the snapshot is the
	 * writer's, and a row this backend wrote would be nobody's.
	 */
	if (!IsTransactionBlock() || !XactReadOnly ||
		XactIsoLevel != XACT_REPEATABLE_READ || FirstSnapshotSet ||
		reader_slot != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("\"%s\" is set only first, in a REPEATABLE READ, READ ONLY transaction block",
						GP_SHARE_SETTING)));

	proc = BackendPidGetProc((int) pid);
	if (proc == NULL || proc == MyProc)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the writer of this transaction, process %ld, is not running",
						pid)));

	share_attach();
	slot = &share_header->slots[GetNumberFromPGProc(proc)];

	/*
	 * Wait for the writer to publish: the coordinator starts the writer's
	 * fragment and its readers' at once, and this is where they meet.
	 */
	ConditionVariablePrepareToSleep(&slot->cv);
	for (;;)
	{
		bool		mine = true;

		LWLockAcquire(&slot->lock, LW_SHARED);
		if (slot->pid == (int) pid)
		{
			mine = slot->database == MyDatabaseId &&
				slot->user == GetSessionUserId() &&
				strcmp(slot->identity, GpClusterQeIdentity()) == 0;
			for (int p = 0; mine && p < GP_SHARE_MAXPUB; p++)
			{
				GpSharePub *pub = &slot->pubs[p];

				if (pub->key[0] == '\0' || strcmp(pub->key, key) != 0)
					continue;
				memcpy(&header, dsa_get_address(share_area, pub->data),
					   offsetof(GpShareData, bytes));
				bytes = palloc(offsetof(GpShareData, bytes) +
							   MAXALIGN(header.snaplen) + header.tstatelen);
				memcpy(bytes, dsa_get_address(share_area, pub->data),
					   offsetof(GpShareData, bytes) +
					   MAXALIGN(header.snaplen) + header.tstatelen);
				xact = slot->xact;
				break;
			}
		}
		LWLockRelease(&slot->lock);

		if (!mine)
		{
			ConditionVariableCancelSleep();
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("process %ld is not a writer of this session", pid)));
		}
		if (bytes != NULL)
			break;
		ConditionVariableSleep(&slot->cv, share_wait_event());
	}
	ConditionVariableCancelSleep();

	/* Its locks are ours, for as long as this backend lives. */
	if (MyProc->lockGroupLeader == NULL)
	{
		if (!BecomeLockGroupMember(proc, (int) pid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the writer of this transaction, process %ld, is not running",
							pid)));
	}
	else if (MyProc->lockGroupLeader != proc)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("this backend reads for another writer")));

	/* R4: the writer's XIDs are current, and its command is ours. */
	XactAdoptTransactionState(SHARE_TSTATE((GpShareData *) bytes));

	/*
	 * What the caches hold may be another transaction's uncommitted catalog,
	 * or may not yet be this one's.
	 */
	if (reader_caches_stale || ((GpShareData *) bytes)->wrote)
		InvalidateSystemCaches();
	reader_caches_stale = ((GpShareData *) bytes)->wrote;

	/* The writer's snapshot, whose xmin the writer holds. */
	snapshot = RestoreSnapshot(((GpShareData *) bytes)->bytes);
	RestoreTransactionSnapshot(snapshot, proc);

	reader_slot = slot;
	reader_xact = xact;
	reader_combos = NULL;
	reader_ncombos = 0;
}

static bool
share_combocid_miss(CommandId combocid, CommandId *cmin, CommandId *cmax)
{
	if (reader_slot == NULL)
		return prev_combocid_miss ? prev_combocid_miss(combocid, cmin, cmax)
			: false;

	if ((int) combocid >= reader_ncombos)
	{
		/* copy what the writer has published since we last looked */
		LWLockAcquire(&reader_slot->lock, LW_SHARED);
		if (reader_slot->xact != reader_xact)
		{
			LWLockRelease(&reader_slot->lock);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the transaction this backend reads for has ended")));
		}
		if (reader_slot->ncombos > reader_ncombos)
		{
			int			n = reader_slot->ncombos;
			CommandId  *pairs = (CommandId *) dsa_get_address(share_area,
														  reader_slot->combos);
			CommandId  *copy;

			copy = MemoryContextAlloc(TopTransactionContext,
									  n * 2 * sizeof(CommandId));
			memcpy(copy, pairs, n * 2 * sizeof(CommandId));
			if (reader_combos != NULL)
				pfree(reader_combos);
			reader_combos = copy;
			reader_ncombos = n;
		}
		LWLockRelease(&reader_slot->lock);
	}

	if ((int) combocid >= reader_ncombos)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("combo command id %u is not one the writer made", combocid)));

	*cmin = reader_combos[2 * combocid];
	*cmax = reader_combos[2 * combocid + 1];
	return true;
}

/* SET LOCAL gp.shared_snapshot = '...': the setting, then the attach. */
static void
share_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	VariableSetStmt *set = NULL;

	if (IsA(parsetree, VariableSetStmt) &&
		((VariableSetStmt *) parsetree)->name != NULL &&
		strcmp(((VariableSetStmt *) parsetree)->name, GP_SHARE_SETTING) == 0)
	{
		set = (VariableSetStmt *) parsetree;
		if (set->kind != VAR_SET_VALUE || !set->is_local)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" is only ever SET LOCAL to a value",
							GP_SHARE_SETTING)));
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (set != NULL)
		share_attach_reader(shared_snapshot_setting);
}

/* ------------------------------------------------------------------------- */
/* The end of a transaction                                                  */
/* ------------------------------------------------------------------------- */

static void
share_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			{
				ListCell   *lc;

				/* a commit would call them leaked */
				foreach(lc, writer_snapshots)
					UnregisterSnapshotFromOwner((Snapshot) lfirst(lc),
												TopTransactionResourceOwner);
				writer_snapshots = NIL;
				break;
			}
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			if (writer_slot != NULL)
			{
				LWLockAcquire(&writer_slot->lock, LW_EXCLUSIVE);
				slot_clear(writer_slot);
				writer_slot->xact++;
				LWLockRelease(&writer_slot->lock);
				writer_slot = NULL;
			}
			/* an abort's resource owner lets go of them */
			writer_snapshots = NIL;

			reader_slot = NULL;
			reader_combos = NULL;
			reader_ncombos = 0;
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpShareInit(void)
{
	DefineCustomStringVariable(GP_SHARE_SETTING,
							   "The writer's transaction this one reads as a part of.",
							   "Set by the coordinator, first in a reader's "
							   "transaction, as \"<writer pid>/<key>\".",
							   &shared_snapshot_setting,
							   "",
							   PGC_USERSET,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, NULL);

	if (GpClusterIsSingleNode())
		return;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = share_ProcessUtility;

	prev_combocid_create = combocid_create_hook;
	combocid_create_hook = share_combocid_create;
	prev_combocid_miss = combocid_miss_hook;
	combocid_miss_hook = share_combocid_miss;

	RegisterXactCallback(share_xact_callback, NULL);
}
