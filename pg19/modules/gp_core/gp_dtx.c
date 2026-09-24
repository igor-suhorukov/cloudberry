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
 * gp_dtx.c
 *	  Distributed transactions: two-phase commit and distributed snapshots.
 *
 * TWO-PHASE COMMIT.  A transaction that wrote on the segments is prepared on
 * each segment that wrote, under a gid made of the coordinator's own full
 * transaction ID, and then committed there (gp_dispatch.c).  What decides it
 * is the coordinator's commit record: a prepared part whose ID committed on
 * the coordinator is committed, one whose ID did not is rolled back.  So the
 * distributed transaction ID is the coordinator's transaction ID -- assigned
 * when the transaction commits, and to a transaction that wrote on a segment
 * only -- and the coordinator's clog is the record Cloudberry keeps in a
 * distributed log and a checkpoint's DTM tail of its own.  The recovery
 * process below finishes, by that record, whatever a failure left prepared:
 * a coordinator that went down between the phases, a segment that did not
 * answer the second.
 *
 * DISTRIBUTED SNAPSHOTS.  With every statement a transaction dispatches, the
 * segments are sent the coordinator's snapshot of it, as the setting
 * gp.distributed_snapshot: its xmin, its xmax and the transactions it saw in
 * progress, all coordinator IDs.  It says a distributed transaction committed
 * exactly when the coordinator's own snapshot does, and a segment makes each
 * snapshot it reads with agree with it:
 *
 *   a transaction the distributed snapshot says committed may still be only
 *   prepared here, its second phase on its way: the segment waits for it,
 *   when the setting arrives and before the statement takes a snapshot;
 *
 *   a transaction the distributed snapshot says in progress may already have
 *   committed here, its second phase having overtaken the snapshot: the
 *   segment hides it, adding it to the executor's snapshot as one in
 *   progress (dtx_craft).
 *
 * For both a segment needs the coordinator ID of each local transaction a
 * distributed one prepared here: a map, in shared memory, filled as each is
 * prepared, and emptied of those every snapshot now in use says committed.
 * Because a segment commits its part only after the coordinator committed,
 * the order of commits the coordinator's snapshots see is the order in which
 * anything on a segment could have seen them: a transaction that waited here
 * for another's row, or read it, committed after it there too.  That is why
 * the port needs neither Cloudberry's commit-ordering locks nor its
 * distributed "committing" array: Cloudberry's distributed snapshot keeps a
 * transaction in progress until its second phase is done everywhere, and so
 * has to order what depends on it; this one does not.
 *
 * A hidden transaction's old row versions must outlive it: vacuum, and the
 * pruning any scan does, would remove what a transaction that committed here
 * deleted, once no local snapshot needs it -- while a distributed one still
 * does.  Cloudberry holds them back with the distributed xmin in its
 * procarray; the port holds them back with a replication slot of its own,
 * gp_dtx_horizon, whose xmin is the oldest transaction in the map.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/cdbtm.c (the segment's half), cdbdtxrecovery.c,
 *	  cdbdistributedsnapshot.c, cdblocaldistribxact.c and
 *	  src/backend/access/transam/distributedlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_authid.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/slot.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
#include "utils/xid8.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_dtx.h"
#include "gp_fault.h"
#include "gp_gdd.h"
#include "gp_share.h"

/* The replication slot whose xmin holds back what a hidden transaction deleted. */
#define GP_DTX_SLOT			"gp_dtx_horizon"

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

/* The distributed snapshot of the statement being run; see the header. */
static char *dtx_snapshot_setting = NULL;

/* How often the recovery process looks, and how old a prepared part it takes. */
static int	dtx_recovery_interval = 60;
static int	dtx_recovery_prepared_period = 300;

/* ------------------------------------------------------------------------- */
/* Gids                                                                      */
/* ------------------------------------------------------------------------- */

void
GpDtxFormGid(FullTransactionId gxid, char *gid)
{
	snprintf(gid, GP_DTX_GIDLEN, GP_DTX_GID_PREFIX UINT64_FORMAT,
			 U64FromFullTransactionId(gxid));
}

void
GpDtxFormLoopbackGid(FullTransactionId gxid, Oid dboid, char *gid)
{
	snprintf(gid, GP_DTX_GIDLEN, GP_DTX_GID_PREFIX UINT64_FORMAT "_%u",
			 U64FromFullTransactionId(gxid), dboid);
}

bool
GpDtxParseGid(const char *gid, FullTransactionId *gxid)
{
	size_t		prefix = strlen(GP_DTX_GID_PREFIX);
	char	   *end;
	uint64		value;

	if (gid == NULL || strncmp(gid, GP_DTX_GID_PREFIX, prefix) != 0 ||
		!isdigit((unsigned char) gid[prefix]))
		return false;
	errno = 0;
	value = strtou64(gid + prefix, &end, 10);
	if (errno != 0 || value < FirstNormalTransactionId)
		return false;

	/* a part in a database of the coordinator's own: "_<database OID>" */
	if (*end == '_' && isdigit((unsigned char) end[1]))
	{
		end++;
		while (isdigit((unsigned char) *end))
			end++;
	}
	if (*end != '\0')
		return false;
	*gxid = FullTransactionIdFromU64(value);
	return true;
}

/* ------------------------------------------------------------------------- */
/* The distributed snapshot                                                  */
/* ------------------------------------------------------------------------- */

/*
 * What the setting carries: "xmin:xmax:prune:x1,x2,...", coordinator full
 * transaction IDs in decimal, the in-progress ones in ascending order.
 * "prune" is the oldest transaction any snapshot on the coordinator may still
 * see in progress, so that a segment knows which of its map it may forget.
 */
typedef struct GpDtxSnapshot
{
	FullTransactionId xmin;
	FullTransactionId xmax;
	FullTransactionId prune;
	int			n;
	FullTransactionId *xip;
} GpDtxSnapshot;

static int
fxid_cmp(const void *a, const void *b)
{
	uint64		x = U64FromFullTransactionId(*(const FullTransactionId *) a);
	uint64		y = U64FromFullTransactionId(*(const FullTransactionId *) b);

	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * The coordinator: a statement's snapshot, as the segments are sent it.  The
 * same snapshot is asked for by each dispatch of a statement, so the last
 * answer is kept.
 */
char *
GpDtxSnapshotString(Snapshot snapshot)
{
	static Snapshot last_snapshot = NULL;
	static TransactionId last_xmin = InvalidTransactionId;
	static TransactionId last_xmax = InvalidTransactionId;
	static uint32 last_xcnt = 0;
	static char *last = NULL;
	FullTransactionId next;
	FullTransactionId *xip;
	TransactionId prune;
	StringInfoData buf;

	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_MVCC)
		return NULL;

	if (last != NULL && snapshot == last_snapshot &&
		snapshot->xmin == last_xmin && snapshot->xmax == last_xmax &&
		snapshot->xcnt == last_xcnt)
		return last;

	next = ReadNextFullTransactionId();
	prune = GetOldestTransactionIdConsideredRunning();
	if (TransactionIdPrecedes(snapshot->xmin, prune))
		prune = snapshot->xmin;

	xip = palloc_array(FullTransactionId, Max(snapshot->xcnt, 1));
	for (uint32 i = 0; i < snapshot->xcnt; i++)
		xip[i] = FullTransactionIdFromAllowableAt(next, snapshot->xip[i]);
	qsort(xip, snapshot->xcnt, sizeof(FullTransactionId), fxid_cmp);

	initStringInfo(&buf);
	appendStringInfo(&buf, UINT64_FORMAT ":" UINT64_FORMAT ":" UINT64_FORMAT ":",
					 U64FromFullTransactionId(FullTransactionIdFromAllowableAt(next, snapshot->xmin)),
					 U64FromFullTransactionId(FullTransactionIdFromAllowableAt(next, snapshot->xmax)),
					 U64FromFullTransactionId(FullTransactionIdFromAllowableAt(next, prune)));
	for (uint32 i = 0; i < snapshot->xcnt; i++)
		appendStringInfo(&buf, "%s" UINT64_FORMAT, i > 0 ? "," : "",
						 U64FromFullTransactionId(xip[i]));
	pfree(xip);

	if (last != NULL)
		pfree(last);
	last = MemoryContextStrdup(TopMemoryContext, buf.data);
	pfree(buf.data);
	last_snapshot = snapshot;
	last_xmin = snapshot->xmin;
	last_xmax = snapshot->xmax;
	last_xcnt = snapshot->xcnt;
	return last;
}

/* Read one; false, and *why, when it is not one. */
static bool
dtx_parse_snapshot(const char *str, GpDtxSnapshot *ds, MemoryContext cxt,
				   const char **why)
{
	const char *p = str;
	uint64		v[3];
	int			n = 0;
	int			max = 0;
	FullTransactionId *xip = NULL;

	for (int i = 0; i < 3; i++)
	{
		char	   *end;

		if (!isdigit((unsigned char) *p))
		{
			*why = "a transaction ID was expected";
			return false;
		}
		errno = 0;
		v[i] = strtou64(p, &end, 10);
		if (errno != 0 || *end != ':')
		{
			*why = "the fields are separated by colons";
			return false;
		}
		p = end + 1;
	}

	while (*p != '\0')
	{
		char	   *end;
		uint64		x;

		if (!isdigit((unsigned char) *p))
		{
			*why = "a transaction ID was expected";
			return false;
		}
		errno = 0;
		x = strtou64(p, &end, 10);
		if (errno != 0 || (*end != ',' && *end != '\0'))
		{
			*why = "the transactions in progress are separated by commas";
			return false;
		}
		if (n == max)
		{
			max = Max(16, max * 2);
			xip = xip == NULL
				? MemoryContextAlloc(cxt, max * sizeof(FullTransactionId))
				: repalloc(xip, max * sizeof(FullTransactionId));
		}
		if (n > 0 && U64FromFullTransactionId(xip[n - 1]) >= x)
		{
			*why = "the transactions in progress are in ascending order";
			return false;
		}
		xip[n++] = FullTransactionIdFromU64(x);
		p = *end == ',' ? end + 1 : end;
	}

	if (v[0] > v[1] || v[2] > v[1])
	{
		*why = "xmin and the pruning horizon cannot follow xmax";
		return false;
	}
	ds->xmin = FullTransactionIdFromU64(v[0]);
	ds->xmax = FullTransactionIdFromU64(v[1]);
	ds->prune = FullTransactionIdFromU64(v[2]);
	ds->n = n;
	ds->xip = xip;
	return true;
}

static bool
dtx_snapshot_check(char **newval, void **extra, GucSource source)
{
	GpDtxSnapshot ds;
	const char *why = NULL;

	if (*newval == NULL || (*newval)[0] == '\0')
		return true;
	if (!dtx_parse_snapshot(*newval, &ds, CurrentMemoryContext, &why))
	{
		GUC_check_errdetail("%s", why);
		return false;
	}
	if (ds.xip != NULL)
		pfree(ds.xip);
	return true;
}

/* The setting, read; NULL when there is none. */
static GpDtxSnapshot *
dtx_current(void)
{
	static char *parsed_from = NULL;
	static GpDtxSnapshot parsed;
	const char *why;

	if (dtx_snapshot_setting == NULL || dtx_snapshot_setting[0] == '\0')
		return NULL;
	if (parsed_from != NULL && strcmp(parsed_from, dtx_snapshot_setting) == 0)
		return &parsed;

	if (parsed_from != NULL)
	{
		pfree(parsed_from);
		parsed_from = NULL;
	}
	if (parsed.xip != NULL)
		pfree(parsed.xip);
	memset(&parsed, 0, sizeof(parsed));
	if (!dtx_parse_snapshot(dtx_snapshot_setting, &parsed, TopMemoryContext,
							&why))
		elog(ERROR, "invalid distributed snapshot \"%s\": %s",
			 dtx_snapshot_setting, why);
	parsed_from = MemoryContextStrdup(TopMemoryContext, dtx_snapshot_setting);
	return &parsed;
}

/* Does the distributed snapshot see this transaction in progress? */
static bool
dtx_in_progress(const GpDtxSnapshot *ds, FullTransactionId gxid)
{
	if (!FullTransactionIdPrecedes(gxid, ds->xmax))
		return true;
	if (FullTransactionIdPrecedes(gxid, ds->xmin))
		return false;
	return bsearch(&gxid, ds->xip, ds->n, sizeof(FullTransactionId),
				   fxid_cmp) != NULL;
}

/* ------------------------------------------------------------------------- */
/* The map, on a segment                                                     */
/* ------------------------------------------------------------------------- */

/*
 * One distributed transaction's part here: its coordinator ID, its local
 * top-level ID, and the subtransactions it committed, which a snapshot that
 * hides it has to hide too -- unknown for a part a restart left prepared,
 * whose snapshot then finds them through pg_subtrans.
 */
typedef struct GpDtxEntry
{
	FullTransactionId gxid;
	TransactionId xid;
	bool		done;			/* committed or rolled back here */
	bool		committed;
	int			nchildren;		/* -1: not known */
	dsa_pointer children;		/* TransactionId[nchildren] */
} GpDtxEntry;

typedef struct GpDtxShared
{
	LWLock		lock;
	int			n;
	int			max;
	dsa_pointer entries;		/* GpDtxEntry[max], by gxid */
	bool		loaded;			/* the parts a restart left have been read */
	TransactionId held;			/* the slot's xmin, as set here */

	/* The coordinator's recovery process, to be woken. */
	ProcNumber	recovery_proc;
	int			recovery_pid;
} GpDtxShared;

static GpDtxShared *dtx_shared = NULL;
static dsa_area *dtx_area = NULL;

static void
dtx_init_shared(void *ptr, void *arg)
{
	GpDtxShared *s = (GpDtxShared *) ptr;

	memset(s, 0, sizeof(GpDtxShared));
	LWLockInitialize(&s->lock, LWLockNewTrancheId("gp_core distributed transactions"));
	s->entries = InvalidDsaPointer;
	s->held = InvalidTransactionId;
	s->recovery_proc = INVALID_PROC_NUMBER;
}

static void
dtx_attach(void)
{
	bool		found;

	if (dtx_shared != NULL)
		return;
	dtx_shared = GetNamedDSMSegment("gp_core distributed transactions",
									sizeof(GpDtxShared), dtx_init_shared,
									&found, NULL);
	dtx_area = GetNamedDSA("gp_core distributed transaction map", &found);
}

static GpDtxEntry *
map_entries(void)
{
	return DsaPointerIsValid(dtx_shared->entries)
		? (GpDtxEntry *) dsa_get_address(dtx_area, dtx_shared->entries)
		: NULL;
}

/* The first entry whose gxid is not before this one; the lock is held. */
static int
map_lower_bound(FullTransactionId gxid)
{
	GpDtxEntry *e = map_entries();
	int			lo = 0;
	int			hi = dtx_shared->n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (FullTransactionIdPrecedes(e[mid].gxid, gxid))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void
map_free_entry(GpDtxEntry *e)
{
	if (DsaPointerIsValid(e->children))
		dsa_free(dtx_area, e->children);
	e->children = InvalidDsaPointer;
}

/* Add a part, or replace the one of that gxid; the lock is held exclusively. */
static void
map_put(FullTransactionId gxid, TransactionId xid,
		const TransactionId *children, int nchildren)
{
	GpDtxEntry *e;
	int			pos;
	dsa_pointer cp = InvalidDsaPointer;

	if (nchildren > 0)
	{
		cp = dsa_allocate(dtx_area, nchildren * sizeof(TransactionId));
		memcpy(dsa_get_address(dtx_area, cp), children,
			   nchildren * sizeof(TransactionId));
	}

	pos = map_lower_bound(gxid);
	e = map_entries();
	if (e != NULL && pos < dtx_shared->n &&
		FullTransactionIdEquals(e[pos].gxid, gxid))
	{
		map_free_entry(&e[pos]);
		e[pos].xid = xid;
		e[pos].done = false;
		e[pos].committed = false;
		e[pos].nchildren = nchildren;
		e[pos].children = cp;
		return;
	}

	if (dtx_shared->n == dtx_shared->max)
	{
		int			newmax = Max(64, dtx_shared->max * 2);
		dsa_pointer np = dsa_allocate(dtx_area, newmax * sizeof(GpDtxEntry));

		if (dtx_shared->n > 0)
			memcpy(dsa_get_address(dtx_area, np), e,
				   dtx_shared->n * sizeof(GpDtxEntry));
		if (DsaPointerIsValid(dtx_shared->entries))
			dsa_free(dtx_area, dtx_shared->entries);
		dtx_shared->entries = np;
		dtx_shared->max = newmax;
		e = map_entries();
	}

	memmove(&e[pos + 1], &e[pos], (dtx_shared->n - pos) * sizeof(GpDtxEntry));
	e[pos].gxid = gxid;
	e[pos].xid = xid;
	e[pos].done = false;
	e[pos].committed = false;
	e[pos].nchildren = nchildren;
	e[pos].children = cp;
	dtx_shared->n++;
}

/*
 * Forget what no snapshot needs: a part rolled back here, which is invisible
 * either way, and a committed one every distributed snapshot in use says
 * committed -- its coordinator ID is older than "prune".  A part whose end
 * nobody recorded -- its backend went away -- is looked up.  The lock is
 * held exclusively.
 */
static void
map_prune(FullTransactionId prune)
{
	GpDtxEntry *e = map_entries();
	int			keep = 0;

	for (int i = 0; i < dtx_shared->n; i++)
	{
		bool		old = FullTransactionIdPrecedes(e[i].gxid, prune);
		bool		drop;

		if (!e[i].done && old && !TransactionIdIsInProgress(e[i].xid))
		{
			e[i].done = true;
			e[i].committed = TransactionIdDidCommit(e[i].xid);
		}
		drop = e[i].done && (!e[i].committed || old);
		if (drop)
		{
			map_free_entry(&e[i]);
			continue;
		}
		if (keep != i)
			e[keep] = e[i];
		keep++;
	}
	dtx_shared->n = keep;
}

/*
 * Hold back, with the slot, what the oldest part in the map deleted: the
 * xmin of gp_dtx_horizon.  Made on first need.  The slot's xmin on disk
 * stays unset -- only the one in memory holds -- so that a restart, which
 * empties the map, lets go of it too.  The lock is held exclusively.
 */
static void
map_hold(void)
{
	GpDtxEntry *e = map_entries();
	TransactionId xmin = InvalidTransactionId;
	ReplicationSlot *slot;

	for (int i = 0; i < dtx_shared->n; i++)
		if (!TransactionIdIsValid(xmin) || TransactionIdPrecedes(e[i].xid, xmin))
			xmin = e[i].xid;

	if (TransactionIdEquals(xmin, dtx_shared->held))
		return;

	slot = SearchNamedReplicationSlot(GP_DTX_SLOT, true);
	if (slot == NULL)
	{
		if (!TransactionIdIsValid(xmin))
		{
			dtx_shared->held = xmin;
			return;
		}
		CheckSlotRequirements(false);
		ReplicationSlotCreate(GP_DTX_SLOT, false, RS_PERSISTENT, false, false,
							  false, false);
		ReplicationSlotRelease();
		slot = SearchNamedReplicationSlot(GP_DTX_SLOT, true);
		if (slot == NULL)
			elog(ERROR, "replication slot \"%s\" vanished as it was made",
				 GP_DTX_SLOT);
	}

	SpinLockAcquire(&slot->mutex);
	slot->effective_xmin = xmin;
	SpinLockRelease(&slot->mutex);
	ReplicationSlotsComputeRequiredXmin(false);
	dtx_shared->held = xmin;
}

/*
 * After a restart: the parts it left prepared, which the map, being memory,
 * lost.  Read once per postmaster, by the first statement that needs the map,
 * from pg_prepared_xacts; their subtransactions are not known.
 */
static void
map_load(void)
{
	int			ret;

	if (dtx_shared->loaded)
		return;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	ret = SPI_execute("SELECT gid, transaction FROM pg_catalog.pg_prepared_xacts"
					  " WHERE gid LIKE 'gp\\_dtx\\_%'", true, 0);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "could not read pg_prepared_xacts");

	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	if (!dtx_shared->loaded)
	{
		for (uint64 r = 0; r < SPI_processed; r++)
		{
			char	   *gid = SPI_getvalue(SPI_tuptable->vals[r],
										   SPI_tuptable->tupdesc, 1);
			char	   *xidstr = SPI_getvalue(SPI_tuptable->vals[r],
											  SPI_tuptable->tupdesc, 2);
			FullTransactionId gxid;
			int			pos;
			GpDtxEntry *e;

			if (!GpDtxParseGid(gid, &gxid) || xidstr == NULL)
				continue;
			pos = map_lower_bound(gxid);
			e = map_entries();
			if (e != NULL && pos < dtx_shared->n &&
				FullTransactionIdEquals(e[pos].gxid, gxid))
				continue;
			map_put(gxid, (TransactionId) strtoul(xidstr, NULL, 10), NULL, -1);
		}
		map_hold();
		dtx_shared->loaded = true;
	}
	LWLockRelease(&dtx_shared->lock);
	SPI_finish();
}

/* ------------------------------------------------------------------------- */
/* A segment's snapshots                                                     */
/* ------------------------------------------------------------------------- */

static void
xid_append(TransactionId **arr, int *n, int *max, TransactionId xid)
{
	if (*n == *max)
	{
		*max = Max(16, *max * 2);
		*arr = *arr == NULL ? palloc(*max * sizeof(TransactionId))
			: repalloc(*arr, *max * sizeof(TransactionId));
	}
	(*arr)[(*n)++] = xid;
}

/*
 * The executor's snapshot, made to agree with the distributed one: every
 * part here of a transaction the distributed snapshot sees in progress that
 * this one sees committed is added to it as in progress, subtransactions
 * included.  They go with the subtransactions, whose array has room for
 * every backend's; where the snapshot has overflowed that array -- or a part
 * a restart left does not say what its subtransactions were -- they go with
 * the top-level ones, and a subtransaction is found through pg_subtrans.
 *
 * Its xmin is lowered to the oldest of them.  The same snapshot when nothing
 * is hidden -- as it is when the snapshot is one this made already: what the
 * map gained since was in progress for it.
 */
static Snapshot
dtx_craft(Snapshot snap, const GpDtxSnapshot *ds)
{
	TransactionId *top = NULL;
	TransactionId *sub = NULL;
	int			ntop = 0,
				maxtop = 0,
				nsub = 0,
				maxsub = 0;
	bool		unknown = false;
	TransactionId xmin = snap->xmin;
	GpDtxEntry *e;
	Snapshot	crafted;

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	e = map_entries();
	for (int i = map_lower_bound(ds->xmin); i < dtx_shared->n; i++)
	{
		if (!dtx_in_progress(ds, e[i].gxid))
			continue;
		/* already in progress for this snapshot */
		if (!TransactionIdPrecedes(e[i].xid, snap->xmax) ||
			pg_lfind32(e[i].xid, snap->xip, snap->xcnt) ||
			(!snap->suboverflowed &&
			 pg_lfind32(e[i].xid, snap->subxip, snap->subxcnt)))
			continue;
		xid_append(&top, &ntop, &maxtop, e[i].xid);
		if (TransactionIdPrecedes(e[i].xid, xmin))
			xmin = e[i].xid;
		if (e[i].nchildren < 0)
			unknown = true;
		else if (e[i].nchildren > 0)
		{
			TransactionId *children = dsa_get_address(dtx_area, e[i].children);

			for (int c = 0; c < e[i].nchildren; c++)
				xid_append(&sub, &nsub, &maxsub, children[c]);
		}
	}
	LWLockRelease(&dtx_shared->lock);

	if (ntop == 0)
		return snap;

	crafted = palloc(sizeof(SnapshotData));
	memcpy(crafted, snap, sizeof(SnapshotData));
	crafted->copied = false;
	crafted->regd_count = 0;
	crafted->active_count = 0;
	crafted->snapXactCompletionCount = 0;
	memset(&crafted->ph_node, 0, sizeof(crafted->ph_node));
	crafted->xmin = xmin;

	if (!snap->suboverflowed && !unknown &&
		snap->subxcnt + ntop + nsub <= GetMaxSnapshotSubxidCount())
	{
		crafted->subxip = palloc((snap->subxcnt + ntop + nsub) * sizeof(TransactionId));
		if (snap->subxcnt > 0)
			memcpy(crafted->subxip, snap->subxip,
				   snap->subxcnt * sizeof(TransactionId));
		memcpy(crafted->subxip + snap->subxcnt, top, ntop * sizeof(TransactionId));
		if (nsub > 0)
			memcpy(crafted->subxip + snap->subxcnt + ntop, sub,
				   nsub * sizeof(TransactionId));
		crafted->subxcnt = snap->subxcnt + ntop + nsub;
	}
	else
	{
		if (snap->xcnt + ntop > GetMaxSnapshotXidCount())
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many distributed transactions are committing to make a snapshot"),
					 errdetail("%d are committed here and in progress for the distributed snapshot.",
							   ntop)));
		crafted->xip = palloc((snap->xcnt + ntop) * sizeof(TransactionId));
		if (snap->xcnt > 0)
			memcpy(crafted->xip, snap->xip, snap->xcnt * sizeof(TransactionId));
		memcpy(crafted->xip + snap->xcnt, top, ntop * sizeof(TransactionId));
		crafted->xcnt = snap->xcnt + ntop;
		crafted->suboverflowed = true;
		crafted->subxcnt = 0;
		crafted->subxip = NULL;
	}

	return crafted;
}

/*
 * This backend's xmin, lowered to a snapshot's it made: the slot already
 * holds back what the snapshot's hidden transactions deleted, so the horizon
 * does not move, and a reader restoring the snapshot finds its writer's xmin
 * no later than the snapshot's, as it must.
 */
static void
dtx_lower_xmin(TransactionId xmin)
{
	if (TransactionIdPrecedes(xmin, TransactionXmin))
		TransactionXmin = xmin;
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	if (!TransactionIdIsValid(MyProc->xmin) ||
		TransactionIdPrecedes(xmin, MyProc->xmin))
		MyProc->xmin = xmin;
	LWLockRelease(ProcArrayLock);
}

static ExecutorStart_hook_type prev_executor_start = NULL;

/*
 * Every statement a segment's dispatched backend runs -- a fragment, a
 * statement sent as text, a query a function in either runs -- reads with a
 * snapshot that agrees with the distributed one.  A reader's snapshot is its
 * writer's, which already does.
 *
 * The executor reads with the query's snapshot and requires it to be the
 * active one (execMain.c), which its caller pushed; so the made one takes
 * the place of both, and the caller pops it where it would have popped its
 * own.  Registered before the caller's is popped, so that this backend's
 * xmin, computed again as the caller's goes, stays no later than the new
 * one's.
 */
static void
dtx_executor_start(QueryDesc *queryDesc, int eflags)
{
	GpDtxSnapshot *ds;

	/* who this backend is, for the global deadlock detector */
	GpGddNoteBackend();

	/*
	 * Where Cloudberry's segment starts a statement it was dispatched
	 * (exec_mpp_query): each the coordinator sends the writer, but for the
	 * one that asks, as the transaction commits, whether it wrote.
	 */
	if (gp_fault_active != NULL && *gp_fault_active > 0 &&
		GpClusterIsDispatched() && !GpShareIsReader() &&
		queryDesc->sourceText != NULL &&
		strncmp(queryDesc->sourceText, GP_DTX_STATUS_QUERY,
				strlen(GP_DTX_STATUS_QUERY)) != 0)
		GP_FAULT("exec_mpp_query_start");

	if (queryDesc->snapshot != NULL &&
		queryDesc->snapshot->snapshot_type == SNAPSHOT_MVCC &&
		queryDesc->snapshot == GetActiveSnapshot() &&
		GpClusterIsDispatched() && !GpShareIsReader() &&
		(ds = dtx_current()) != NULL)
	{
		Snapshot	crafted = dtx_craft(queryDesc->snapshot, ds);

		if (crafted != queryDesc->snapshot)
		{
			Snapshot	old = queryDesc->snapshot;
			Snapshot	made = RegisterSnapshot(crafted);

			PopActiveSnapshot();
			PushActiveSnapshot(made);
			queryDesc->snapshot = made;
			UnregisterSnapshot(old);
			dtx_lower_xmin(made->xmin);
		}
	}

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * A distributed snapshot has arrived, before the statement that reads with
 * it takes its own: wait for what it says committed and is only prepared
 * here, and forget what no snapshot in use can need.
 */
static void
dtx_snapshot_arrived(void)
{
	GpDtxSnapshot *ds = dtx_current();
	TransactionId *wait = NULL;
	int			nwait = 0,
				maxwait = 0;
	bool		prune = false;
	GpDtxEntry *e;

	if (ds == NULL)
		return;
	dtx_attach();
	map_load();

	/*
	 * Where Cloudberry's segment advances its distributed log's oldest xmin
	 * from a distributed snapshot, which its tests hold a statement at, in
	 * one database.
	 */
	if (gp_fault_active != NULL && *gp_fault_active > 0)
		(void) GpFaultTrigger("distributedlog_advance_oldest_xmin",
							  get_database_name(MyDatabaseId), "");

	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	e = map_entries();
	for (int i = 0; i < dtx_shared->n; i++)
	{
		if (FullTransactionIdPrecedes(e[i].gxid, ds->prune) || (e[i].done && !e[i].committed))
			prune = true;
		if (e[i].done || dtx_in_progress(ds, e[i].gxid))
			continue;
		xid_append(&wait, &nwait, &maxwait, e[i].xid);
	}
	LWLockRelease(&dtx_shared->lock);

	if (prune)
	{
		LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
		map_prune(ds->prune);
		map_hold();
		LWLockRelease(&dtx_shared->lock);
	}

	for (int i = 0; i < nwait; i++)
		if (TransactionIdIsInProgress(wait[i]))
			XactLockTableWait(wait[i], NULL, NULL, XLTW_None);
}

/* ------------------------------------------------------------------------- */
/* A segment's part of two-phase commit                                      */
/* ------------------------------------------------------------------------- */

/* The distributed transaction a PREPARE TRANSACTION is preparing. */
static FullTransactionId dtx_preparing = {0};

/*
 * Temporary relations.  PostgreSQL will not prepare a transaction that used
 * one, and every distributed one that did has to be; Cloudberry lifts the
 * check (xact.c, "#if 0"), the port clears the flag it tests at PRE_PREPARE
 * -- after running the ON COMMIT actions, which PREPARE runs after the
 * callbacks and which set the flag again; run once, they find nothing left.
 * What PostgreSQL would have got wrong is then the port's to put right: a
 * temporary relation dropped in the transaction is left out of what its
 * COMMIT PREPARED unlinks -- ON COMMIT DROP drops one in every such
 * transaction -- so the backend that prepared it unlinks it after, and a
 * temporary schema the transaction made is gone if it is rolled back, which
 * leaves this backend pointing at nothing.
 */
typedef struct DroppedTemp
{
	RelFileLocatorBackend rlocator;
	int			level;			/* the subtransaction that dropped it */
} DroppedTemp;

static List *dropped_temp = NIL;	/* of DroppedTemp *, this transaction's */

/* What a prepared transaction of this backend's leaves for its second phase. */
typedef struct PreparedTemp
{
	FullTransactionId gxid;
	List	   *dropped;		/* of DroppedTemp * */
	bool		made_schema;	/* the transaction made the temporary schema */
} PreparedTemp;

static List *prepared_temp = NIL;	/* of PreparedTemp *, in TopMemoryContext */

/* Whether this backend had a temporary schema when its transaction began. */
static LocalTransactionId temp_seen_lxid = InvalidLocalTransactionId;
static bool temp_schema_before = false;

static object_access_hook_type prev_object_access = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static void
note_transaction(void)
{
	if (MyProc->vxid.lxid != temp_seen_lxid)
	{
		Oid			ns,
					toast;

		GetTempNamespaceState(&ns, &toast);
		temp_schema_before = OidIsValid(ns);
		temp_seen_lxid = MyProc->vxid.lxid;
	}
}

static void
dtx_object_access(ObjectAccessType access, Oid classId, Oid objectId,
				  int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (access == OAT_DROP && classId == RelationRelationId && subId == 0 &&
		GpClusterIsDispatched() &&
		get_rel_persistence(objectId) == RELPERSISTENCE_TEMP)
	{
		Relation	rel = RelationIdGetRelation(objectId);

		if (RelationIsValid(rel))
		{
			if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);
				DroppedTemp *d = palloc(sizeof(DroppedTemp));

				d->rlocator.locator = rel->rd_locator;
				d->rlocator.backend = rel->rd_backend;
				d->level = GetCurrentTransactionNestLevel();
				dropped_temp = lappend(dropped_temp, d);
				MemoryContextSwitchTo(oldcxt);
			}
			RelationClose(rel);
		}
	}
}

static void
unlink_temp(List *dropped)
{
	int			n = list_length(dropped);
	SMgrRelation *srels;
	int			i = 0;

	if (n == 0)
		return;
	srels = palloc_array(SMgrRelation, n);
	foreach_ptr(DroppedTemp, d, dropped)
		srels[i++] = smgropen(d->rlocator.locator, d->rlocator.backend);
	smgrdounlinkall(srels, n, false);
	for (i = 0; i < n; i++)
		smgrclose(srels[i]);
	pfree(srels);
}

/* A PREPARE TRANSACTION of a distributed transaction: its part in the map. */
static void
dtx_pre_prepare(void)
{
	TransactionId xid = GetTopTransactionIdIfAny();
	TransactionId *children;
	int			nchildren;
	Oid			ns,
				toast;
	PreparedTemp *pt;
	MemoryContext oldcxt;

	GP_FAULT("start_prepare");

	/* see "Temporary relations" above */
	PreCommit_on_commit_actions();
	MyXactFlags &= ~XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	GetTempNamespaceState(&ns, &toast);
	if (dropped_temp != NIL || (!temp_schema_before && OidIsValid(ns)))
	{
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		pt = palloc0(sizeof(PreparedTemp));
		pt->gxid = dtx_preparing;
		foreach_ptr(DroppedTemp, d, dropped_temp)
		{
			DroppedTemp *copy = palloc(sizeof(DroppedTemp));

			*copy = *d;
			pt->dropped = lappend(pt->dropped, copy);
		}
		pt->made_schema = !temp_schema_before && OidIsValid(ns);
		prepared_temp = lappend(prepared_temp, pt);
		MemoryContextSwitchTo(oldcxt);
	}
	dropped_temp = NIL;

	/*
	 * The map is a segment's, for its distributed snapshots.  A part prepared
	 * on the coordinator itself -- the loopback's, in another of its
	 * databases -- is read by the coordinator's own snapshots, and no
	 * distributed snapshot would ever prune it.
	 */
	if (!TransactionIdIsValid(xid) || GpClusterContentId() < 0)
		return;
	nchildren = xactGetCommittedChildren(&children);

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	map_put(dtx_preparing, xid, children, nchildren);
	map_hold();
	LWLockRelease(&dtx_shared->lock);
}

/*
 * COMMIT PREPARED or ROLLBACK PREPARED of a distributed transaction: done
 * with here.  A part a restart left, which the map does not have yet, is
 * read in first by the statement that commits it: a snapshot that says it
 * in progress must still hide it.
 */
static void
dtx_finished(FullTransactionId gxid, bool commit)
{
	GpDtxEntry *e;
	int			pos;
	ListCell   *lc;

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	pos = map_lower_bound(gxid);
	e = map_entries();
	if (e != NULL && pos < dtx_shared->n &&
		FullTransactionIdEquals(e[pos].gxid, gxid))
	{
		e[pos].done = true;
		e[pos].committed = commit;
	}
	LWLockRelease(&dtx_shared->lock);

	foreach(lc, prepared_temp)
	{
		PreparedTemp *pt = (PreparedTemp *) lfirst(lc);

		if (!FullTransactionIdEquals(pt->gxid, gxid))
			continue;
		prepared_temp = foreach_delete_current(prepared_temp, lc);
		if (commit)
			unlink_temp(pt->dropped);
		else if (pt->made_schema)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the temporary schema of this session was made by a distributed transaction that rolled back"),
					 errdetail("The segment's session ends, and the coordinator opens another.")));
		list_free_deep(pt->dropped);
		pfree(pt);
	}
}

static void
dtx_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		finishing = false;
	bool		commit = false;
	bool		snapshot_set = false;
	FullTransactionId gxid = InvalidFullTransactionId;

	if (IsTransactionState())
	{
		note_transaction();
		GpGddNoteBackend();
	}

	if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *ts = (TransactionStmt *) parsetree;

		if (ts->gid != NULL && GpDtxParseGid(ts->gid, &gxid))
		{
			switch (ts->kind)
			{
				case TRANS_STMT_PREPARE:

					/*
					 * The coordinator's to use: the recovery process commits
					 * or rolls back a part by the coordinator's clog, so one
					 * prepared under such a gid by anybody else would be
					 * decided by a transaction that has nothing to do with
					 * it, and a snapshot would wait for it.  The secret says
					 * which connection is the coordinator's where there is
					 * one; where there is none, any dispatched one is.
					 */
					if (GpClusterHasSecret() ? !GpClusterDispatchTrusted()
						: !GpClusterIsDispatched())
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("transaction identifier \"%s\" is reserved for distributed transactions",
										ts->gid),
								 errhint("Choose a transaction identifier that does not begin with \"%s\".",
										 GP_DTX_GID_PREFIX)));
					dtx_preparing = gxid;
					break;
				case TRANS_STMT_COMMIT_PREPARED:
				case TRANS_STMT_ROLLBACK_PREPARED:
					finishing = true;
					commit = ts->kind == TRANS_STMT_COMMIT_PREPARED;
					if (commit && GpClusterContentId() >= 0)
					{
						dtx_attach();
						map_load();
					}
					break;
				default:
					break;
			}
		}
	}
	else if (IsA(parsetree, VariableSetStmt) &&
			 ((VariableSetStmt *) parsetree)->name != NULL &&
			 strcmp(((VariableSetStmt *) parsetree)->name,
					GP_DTX_SNAPSHOT_SETTING) == 0 &&
			 ((VariableSetStmt *) parsetree)->kind == VAR_SET_VALUE)
		snapshot_set = true;

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (finishing)
		dtx_finished(gxid, commit);
	else if (snapshot_set && GpClusterIsDispatched() && !GpShareIsReader())
		dtx_snapshot_arrived();
}

static void
dtx_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_PREPARE:
			if (FullTransactionIdIsValid(dtx_preparing))
				dtx_pre_prepare();
			break;
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
			dtx_preparing = InvalidFullTransactionId;
			dropped_temp = NIL;
			break;
		default:
			break;
	}
}

static void
dtx_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					 SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	ListCell   *lc;

	foreach(lc, dropped_temp)
	{
		DroppedTemp *d = (DroppedTemp *) lfirst(lc);

		if (d->level < level)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			dropped_temp = foreach_delete_current(dropped_temp, lc);
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			d->level = level - 1;
	}
}

/* ------------------------------------------------------------------------- */
/* The coordinator's recovery process                                        */
/* ------------------------------------------------------------------------- */

void
GpDtxWakeRecovery(void)
{
	ProcNumber	proc;

	dtx_attach();
	proc = dtx_shared->recovery_proc;
	if (proc != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(proc)->procLatch);
}

typedef enum DtxOutcome
{
	DTX_IN_PROGRESS,
	DTX_COMMITTED,
	DTX_ABORTED,
	DTX_UNKNOWN,				/* in the future, or older than the clog */
} DtxOutcome;

/*
 * What became of a coordinator transaction, from the clog: pg_xact_status(),
 * less the SQL.  "In progress" before "committed", as a visibility check
 * asks, so that a transaction whose commit is on its way is left to its own
 * backend.
 */
static DtxOutcome
dtx_outcome(FullTransactionId gxid)
{
	FullTransactionId next = ReadNextFullTransactionId();
	TransactionId xid = XidFromFullTransactionId(gxid);
	DtxOutcome	result;

	if (!FullTransactionIdPrecedes(gxid, next))
		return DTX_UNKNOWN;

	LWLockAcquire(XactTruncationLock, LW_SHARED);
	if (FullTransactionIdPrecedes(gxid,
								  FullTransactionIdFromAllowableAt(next,
																   TransamVariables->oldestClogXid)))
		result = DTX_UNKNOWN;
	else if (TransactionIdIsInProgress(xid))
		result = DTX_IN_PROGRESS;
	else if (TransactionIdDidCommit(xid))
		result = DTX_COMMITTED;
	else
		result = DTX_ABORTED;
	LWLockRelease(XactTruncationLock);
	return result;
}

static char *recovery_user = NULL;

static uint32
recovery_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryDtxRecovery");
	return event;
}

/* "segment 0", or "the coordinator", for the messages. */
static char *
node_name(const GpSegmentConfig *node)
{
	return node->content < 0 ? pstrdup("the coordinator")
		: psprintf("segment %d", node->content);
}

/* A connection to one database of a node, or NULL, logged. */
static PGconn *
recovery_connect(const GpSegmentConfig *seg, const char *dbname)
{
	const char *keywords[8];
	const char *values[8];
	char		portbuf[16];
	int			n = 0;
	PGconn	   *conn;
	const char *passfile = GpDispatchPassfile();

	snprintf(portbuf, sizeof(portbuf), "%d", seg->port);
	keywords[n] = "host";
	values[n++] = seg->hostname;
	keywords[n] = "port";
	values[n++] = portbuf;
	keywords[n] = "dbname";
	values[n++] = dbname;
	keywords[n] = "user";
	values[n++] = recovery_user;
	keywords[n] = "application_name";
	values[n++] = "cloudberry dtx recovery";
	if (passfile != NULL && passfile[0] != '\0')
	{
		keywords[n] = "passfile";
		values[n++] = passfile;
	}
	keywords[n] = NULL;
	values[n] = NULL;

	conn = libpqsrv_connect_params(keywords, values, false,
								   recovery_wait_event());
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		ereport(LOG,
				(errmsg("distributed transaction recovery could not connect to %s (%s:%d), database \"%s\"",
						node_name(seg), seg->hostname, seg->port, dbname),
				 conn ? errdetail_internal("%s", PQerrorMessage(conn)) : 0));
		if (conn != NULL)
			libpqsrv_disconnect(conn);
		return NULL;
	}
	return conn;
}

/*
 * One round: every part a node holds prepared under a distributed gid,
 * committed or rolled back by what the coordinator's clog says of its
 * transaction -- each segment's, and the coordinator's own, which the
 * loopback prepared in another of its databases (gp_loopback.c).  One still
 * in progress here is its backend's.  "min_age" leaves alone what was
 * prepared less than that many seconds ago, whose second phase is on its way
 * from the backend that prepared it; the round after a restart, and one a
 * backend asked for, take everything.  Returns whether every node was
 * reached.
 */
static bool
recovery_round(int min_age)
{
	const GpSegmentConfig *segs;
	int			nsegs;
	bool		complete = true;

	segs = GpClusterSegments(&nsegs);
	for (int s = 0; s <= nsegs; s++)
	{
		const GpSegmentConfig *node = s < nsegs ? &segs[s] : GpClusterSelf();
		PGconn	   *conn = recovery_connect(node, "postgres");
		PGresult   *res;

		if (conn == NULL)
			conn = recovery_connect(node, "template1");
		if (conn == NULL)
		{
			complete = false;
			continue;
		}

		res = libpqsrv_exec(conn,
							"SELECT gid, database,"
							" extract(epoch FROM now() - prepared)::int"
							" FROM pg_catalog.pg_prepared_xacts"
							" WHERE gid LIKE 'gp\\_dtx\\_%' ORDER BY database, gid",
							recovery_wait_event());
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			ereport(LOG,
					(errmsg("distributed transaction recovery could not read the prepared transactions of %s",
							node_name(node)),
					 errdetail_internal("%s", PQerrorMessage(conn))));
			PQclear(res);
			libpqsrv_disconnect(conn);
			complete = false;
			continue;
		}

		for (int r = 0; r < PQntuples(res); r++)
		{
			const char *gid = PQgetvalue(res, r, 0);
			const char *dbname = PQgetvalue(res, r, 1);
			int			age = atoi(PQgetvalue(res, r, 2));
			FullTransactionId gxid;
			DtxOutcome	outcome;
			PGconn	   *dbconn;
			PGresult   *done;
			char	   *sql;

			if (!GpDtxParseGid(gid, &gxid) || age < min_age)
				continue;
			outcome = dtx_outcome(gxid);
			if (outcome == DTX_IN_PROGRESS)
				continue;
			if (outcome == DTX_UNKNOWN)
			{
				ereport(WARNING,
						(errmsg("%s holds prepared transaction \"%s\", which the coordinator has no record of",
								node_name(node), gid),
						 errhint("Commit or roll it back by hand, in database \"%s\" there.",
								 dbname)));
				continue;
			}

			/* COMMIT PREPARED runs in the database it was prepared in */
			dbconn = recovery_connect(node, dbname);
			if (dbconn == NULL)
			{
				complete = false;
				continue;
			}
			sql = psprintf("%s PREPARED '%s'",
						   outcome == DTX_COMMITTED ? "COMMIT" : "ROLLBACK", gid);
			done = libpqsrv_exec(dbconn, sql, recovery_wait_event());
			if (PQresultStatus(done) != PGRES_COMMAND_OK)
				ereport(LOG,
						(errmsg("distributed transaction recovery could not finish \"%s\" on %s",
								gid, node_name(node)),
						 errdetail_internal("%s", PQerrorMessage(dbconn))));
			else
				ereport(LOG,
						(errmsg("distributed transaction recovery: %s on %s",
								sql, node_name(node))));
			PQclear(done);
			libpqsrv_disconnect(dbconn);
		}
		PQclear(res);
		libpqsrv_disconnect(conn);
	}
	return complete;
}

PGDLLEXPORT void GpDtxRecoveryMain(Datum main_arg);

void
GpDtxRecoveryMain(Datum main_arg)
{
	bool		everything = true;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* No database: only the shared catalogs, for the superuser's name. */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	StartTransactionCommand();
	recovery_user = MemoryContextStrdup(TopMemoryContext,
										GetUserNameFromId(BOOTSTRAP_SUPERUSERID, false));
	CommitTransactionCommand();

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	dtx_shared->recovery_proc = MyProcNumber;
	dtx_shared->recovery_pid = MyProcPid;
	LWLockRelease(&dtx_shared->lock);

	for (;;)
	{
		int			rc;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		GP_FAULT("dtx_recovery_round");

		/* until a round reaches every segment, each takes everything */
		if (recovery_round(everything ? 0 : dtx_recovery_prepared_period))
			everything = false;

		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   (everything ? 5 : dtx_recovery_interval) * 1000L,
					   recovery_wait_event());
		ResetLatch(MyLatch);
		if (rc & WL_LATCH_SET)
			everything = true;
	}
}

/* ------------------------------------------------------------------------- */
/* Seeing it                                                                 */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_dtx_map);

/*
 * gp_internal.dtx_map()
 *		The map of a segment: each distributed transaction's part here.
 */
Datum
gp_dtx_map(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	GpDtxEntry *e;

	InitMaterializedSRF(fcinfo, 0);
	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	e = map_entries();
	for (int i = 0; i < dtx_shared->n; i++)
	{
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};

		values[0] = FullTransactionIdGetDatum(e[i].gxid);
		values[1] = TransactionIdGetDatum(e[i].xid);
		values[2] = BoolGetDatum(e[i].done);
		values[3] = BoolGetDatum(e[i].committed);
		values[4] = Int32GetDatum(e[i].nchildren);
		if (!e[i].done)
			nulls[3] = true;
		if (e[i].nchildren < 0)
			nulls[4] = true;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	LWLockRelease(&dtx_shared->lock);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpDtxInit(void)
{
	const GpSegmentConfig *self;

	DefineCustomStringVariable(GP_DTX_SNAPSHOT_SETTING,
							   "The distributed snapshot of the statement being run.",
							   "Set by the coordinator, on a segment, with each "
							   "statement a transaction sends it.",
							   &dtx_snapshot_setting,
							   "",
							   PGC_USERSET,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   dtx_snapshot_check, NULL, NULL);

	DefineCustomIntVariable("gp.dtx_recovery_interval",
							"How often distributed transaction recovery looks for prepared transactions to finish.",
							NULL,
							&dtx_recovery_interval,
							60, 1, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.dtx_recovery_prepared_period",
							"How long a transaction stays prepared before distributed transaction recovery finishes it.",
							"Younger ones are left to the backend that prepared "
							"them, whose second phase is on its way.",
							&dtx_recovery_prepared_period,
							300, 0, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	if (GpClusterIsSingleNode())
		return;

	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = dtx_executor_start;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = dtx_ProcessUtility;
	prev_object_access = object_access_hook;
	object_access_hook = dtx_object_access;
	RegisterXactCallback(dtx_xact_callback, NULL);
	RegisterSubXactCallback(dtx_subxact_callback, NULL);

	/* The coordinator finishes what a failure left prepared. */
	self = GpClusterSelf();
	if (self != NULL && self->content == -1)
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 5;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpDtxRecoveryMain");
		snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core distributed transaction recovery");
		snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core dtx recovery");
		RegisterBackgroundWorker(&worker);
	}
}
