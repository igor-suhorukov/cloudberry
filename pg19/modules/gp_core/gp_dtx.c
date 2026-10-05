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
 * Which segments wrote, each says with the answer to every statement it is
 * sent: its part's transaction ID, a setting reported to the coordinator
 * (gp.dtx_xid), empty while it has none.  So the coordinator asks nobody as it
 * commits.  A part that wrote alone, the coordinator having written nothing,
 * commits in ONE PHASE, as Cloudberry's does: under the coordinator's
 * transaction ID, which the coordinator's snapshots see in progress until its
 * transaction ends.  The segment's commit is the decision, so the
 * coordinator's commit record needs no flush, and nothing is prepared: one
 * round trip, one flush.
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
 * distributed one prepared here, or committed in one phase: a map, in shared
 * memory, filled as each is prepared or commits so, and emptied of those
 * every snapshot now in use says committed.  Because a segment commits a
 * prepared part only after the coordinator committed, the order of commits
 * the coordinator's snapshots see is the order in which anything on a
 * segment could have seen them: a transaction that waited here for another's
 * row, or read it, committed after it there too.
 *
 * THE DISTRIBUTED LOG.  The map is logged too, as Cloudberry's distributed
 * log is, in a table of each database, gp_internal.distributed_log: a part
 * writes its row as it prepares or commits in one phase, so that the row
 * commits with it, and a restart and a mirror have it from the WAL; a later
 * part deletes rows no snapshot can need.  After a restart or a promotion,
 * whose map starts empty, a backend reads its database's rows into the map
 * the first time it needs the map, and the keeper, a background worker of
 * each segment's, reads every database's.
 *
 * COMMIT ORDERING.  A one-phase part is the exception: it commits here before
 * the coordinator's transaction ends.  For that moment a transaction here may
 * see it committed -- wait for its row lock, then update the row it wrote --
 * while a distributed snapshot still sees it in progress, and if that
 * transaction ended on the coordinator first, a snapshot would see its row
 * and not the row it replaced, nor the one-phase part's.  So a part that
 * commits in one phase or prepares reports the one-phase parts that have
 * committed here and are still in the map (gp.dtx_depends), and the
 * coordinator ends its transaction only after each of theirs.  That is
 * Cloudberry's commit ordering (lmgr.c, cdbtm.c), which records the
 * transactions a backend waited for; this reports a superset, which also
 * covers an update that reached a newer row version without waiting.
 *
 * A hidden transaction's old row versions must outlive it: vacuum, and the
 * pruning any scan does, would remove what a transaction that committed here
 * deleted, once no local snapshot needs it -- while a distributed one still
 * does.  Cloudberry holds them back with the distributed xmin in its
 * procarray; the port holds them back with a replication slot of its own,
 * gp_dtx_horizon, whose xmin is the oldest transaction in the map.  The
 * slot's xmin on disk is the oldest of the map's and of the transactions
 * running, which only moves forward, so that a restart holds from its first
 * moment, before the map is read again.  A mirror has no slot of its
 * primary's: the keeper makes one there, holding back whatever the oldest
 * transaction a table may still have unfrozen deleted, so that a promotion
 * holds from its first moment too.  Until the keeper has read every
 * database's rows, the hold a restart or a promotion came with is lowered
 * but not raised.
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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
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
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "replication/syncrep.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"
#include "utils/xid8.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_dtx.h"
#include "gp_settings.h"
#include "gp_fault.h"
#include "gp_gdd.h"
#include "gp_log.h"
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

/* What a segment's part says of itself, and one-phase commit; see gp_dtx.h. */
static char *dtx_xid_setting = NULL;
static char *dtx_depends_setting = NULL;
static char *dtx_one_phase_setting = NULL;

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
	bool		one_phase;		/* committed here in one phase, not prepared */
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

	/* The databases whose logged parts have been read into the map. */
	int			ndbs;
	int			maxdbs;
	dsa_pointer dbs;			/* Oid[maxdbs] */
	bool		complete;		/* every database's: the keeper read them all */

	/* The coordinator's recovery process, to be woken. */
	ProcNumber	recovery_proc;
	int			recovery_pid;

	/*
	 * How many times a backend has woken it to take everything
	 * (GpDtxWakeRecovery()): the latch alone is lost to any wait of its own
	 * that resets it first -- a round's connection to a node, a fault's
	 * suspend -- and the round after would leave a young part alone.
	 */
	pg_atomic_uint32 recovery_wakes;

	/*
	 * A round of it has reached every node since the server started:
	 * Cloudberry's "DTM Started" (shmDtmStarted), which its pg_ctl waits
	 * for, and gpstart waits for here (gp.dtx_recovered()).
	 */
	bool		recovered;

	/*
	 * The round the recovery process is running, if one is, and how far it
	 * has got: gp_stat_progress_dtx_recovery's row (DtxProgress).
	 */
	bool		progress_active;
	int			progress_phase;
	int64		progress[5];

	/*
	 * How many transactions have journalled a part of the loopback's and
	 * committed (GpDtxNoteLoopbackJournal()): the recovery process reads the
	 * journals when it changes.
	 */
	pg_atomic_uint32 loopback_journals;

	/*
	 * The newest coordinator ID a part here was prepared or committed in one
	 * phase under since the node started, or read back with the map:
	 * gp_get_next_gxid() on a segment.
	 */
	FullTransactionId newest;
} GpDtxShared;

static GpDtxShared *dtx_shared = NULL;
static dsa_area *dtx_area = NULL;

/* This node runs distributed transaction recovery, as the postmaster set up. */
static bool dtx_recovery_registered = false;

static void
dtx_init_shared(void *ptr, void *arg)
{
	GpDtxShared *s = (GpDtxShared *) ptr;

	memset(s, 0, sizeof(GpDtxShared));
	LWLockInitialize(&s->lock, LWLockNewTrancheId("gp_core distributed transactions"));
	s->entries = InvalidDsaPointer;
	s->held = InvalidTransactionId;
	s->dbs = InvalidDsaPointer;
	s->recovery_proc = INVALID_PROC_NUMBER;
	pg_atomic_init_u32(&s->recovery_wakes, 0);
	pg_atomic_init_u32(&s->loopback_journals, 0);
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
		const TransactionId *children, int nchildren, bool one_phase)
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

	if (FullTransactionIdFollows(gxid, dtx_shared->newest))
		dtx_shared->newest = gxid;

	pos = map_lower_bound(gxid);
	e = map_entries();
	if (e != NULL && pos < dtx_shared->n &&
		FullTransactionIdEquals(e[pos].gxid, gxid))
	{
		map_free_entry(&e[pos]);
		e[pos].xid = xid;
		e[pos].done = false;
		e[pos].committed = false;
		e[pos].one_phase = one_phase;
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
	e[pos].one_phase = one_phase;
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

/* The older of two transaction IDs, either of which may be invalid. */
static TransactionId
xid_older(TransactionId a, TransactionId b)
{
	if (!TransactionIdIsValid(a))
		return b;
	if (!TransactionIdIsValid(b) || TransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* The oldest transaction in the map; the lock is held. */
static TransactionId
map_oldest(void)
{
	GpDtxEntry *e = map_entries();
	TransactionId xmin = InvalidTransactionId;

	for (int i = 0; i < dtx_shared->n; i++)
		xmin = xid_older(xmin, e[i].xid);
	return xmin;
}

/*
 * The slot, made where there is none, holding "xmin" in memory and "ondisk"
 * on disk from the moment it is made, so that a server that stops at once
 * holds as it starts again.  The lock is held exclusively.
 */
static ReplicationSlot *
slot_make(TransactionId xmin, TransactionId ondisk)
{
	ReplicationSlot *slot = SearchNamedReplicationSlot(GP_DTX_SLOT, true);

	if (slot != NULL)
		return slot;

	CheckSlotRequirements(false);
	ReplicationSlotCreate(GP_DTX_SLOT, false, RS_PERSISTENT, false, false,
						  false, false);
	slot = MyReplicationSlot;
	SpinLockAcquire(&slot->mutex);
	slot->data.xmin = ondisk;
	slot->effective_xmin = xmin;
	SpinLockRelease(&slot->mutex);
	ReplicationSlotMarkDirty();
	ReplicationSlotSave();
	ReplicationSlotRelease();
	ReplicationSlotsComputeRequiredXmin(false);

	slot = SearchNamedReplicationSlot(GP_DTX_SLOT, true);
	if (slot == NULL)
		elog(ERROR, "replication slot \"%s\" vanished as it was made",
			 GP_DTX_SLOT);
	return slot;
}

/*
 * Hold back, with the slot, what the oldest part in the map deleted: the
 * xmin of gp_dtx_horizon, in memory.  Until the keeper has read every
 * database's logged parts, the map may lack some, and the hold the slot came
 * with -- from disk after a restart, from the keeper on a mirror before a
 * promotion -- is lowered but not raised.  The lock is held exclusively.
 */
static void
map_hold(void)
{
	TransactionId xmin = map_oldest();
	ReplicationSlot *slot = SearchNamedReplicationSlot(GP_DTX_SLOT, true);

	if (!dtx_shared->complete && slot != NULL)
	{
		TransactionId came;

		SpinLockAcquire(&slot->mutex);
		came = slot->effective_xmin;
		SpinLockRelease(&slot->mutex);
		xmin = xid_older(xmin, came);
	}

	if (TransactionIdEquals(xmin, dtx_shared->held))
		return;

	if (slot == NULL)
	{
		if (!TransactionIdIsValid(xmin))
		{
			dtx_shared->held = xmin;
			return;
		}
		/* on disk, a hold no later part can precede; see keeper_persist() */
		slot = slot_make(xmin,
						 xid_older(xmin, GetOldestActiveTransactionId(false, true)));
	}

	SpinLockAcquire(&slot->mutex);
	slot->effective_xmin = xmin;
	SpinLockRelease(&slot->mutex);
	ReplicationSlotsComputeRequiredXmin(false);
	dtx_shared->held = xmin;
}

/* ------------------------------------------------------------------------- */
/* The distributed log                                                       */
/* ------------------------------------------------------------------------- */

/* How many rows no snapshot needs a part deletes as it writes its own. */
#define LOG_PRUNE_PER_PART	2

/* gp_internal.distributed_log of this database; invalid where gp_core is not. */
static Oid
log_relid(void)
{
	Oid			nsp = get_namespace_oid("gp_internal", true);

	return OidIsValid(nsp) ? get_relname_relid("distributed_log", nsp) : InvalidOid;
}

/*
 * A few rows of parts no distributed snapshot can see in progress any more,
 * their coordinator transaction older than "prune", deleted in this
 * transaction, oldest first.  A row another part is deleting is left to it
 * rather than waited for, so that two commits never wait for each other
 * here.
 */
static void
log_prune(Relation rel, FullTransactionId prune)
{
	List	   *indexes = RelationGetIndexList(rel);
	Relation	index;
	ScanKeyData key;
	SysScanDesc scan;
	Snapshot	snapshot;
	HeapTuple	tup;
	int			deleted = 0;

	if (indexes == NIL)
		return;
	index = index_open(linitial_oid(indexes), AccessShareLock);
	ScanKeyInit(&key, 1, BTLessStrategyNumber, F_XID8LT,
				FullTransactionIdGetDatum(prune));
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = systable_beginscan_ordered(rel, index, snapshot, 1, &key);
	while (deleted < LOG_PRUNE_PER_PART &&
		   (tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		TM_FailureData tmfd;

		if (heap_delete(rel, &tup->t_self, GetCurrentCommandId(true), 0,
						InvalidSnapshot, false, &tmfd) == TM_Ok)
			deleted++;
	}
	systable_endscan_ordered(scan);
	UnregisterSnapshot(snapshot);
	index_close(index, AccessShareLock);
	list_free(indexes);
}

/*
 * A part's row, as it prepares or commits in one phase: written in its own
 * transaction, so that it commits or rolls back with the part, and with it a
 * few rows no snapshot needs deleted.  Where gp_core's extension is not, the
 * part is in the map alone, which a restart empties.
 */
static void
log_part(FullTransactionId gxid, const TransactionId *children, int nchildren,
		 bool one_phase)
{
	Oid			relid = log_relid();
	Relation	rel;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tup;
	GpDtxSnapshot *ds;

	if (!OidIsValid(relid))
		return;

	/* as it prepares or commits, the statement's snapshot is gone */
	PushActiveSnapshot(GetLatestSnapshot());
	rel = table_open(relid, RowExclusiveLock);
	values[0] = FullTransactionIdGetDatum(gxid);
	values[1] = FullTransactionIdGetDatum(GetTopFullTransactionId());
	values[2] = BoolGetDatum(one_phase);
	if (nchildren > 0)
	{
		Datum	   *elems = palloc_array(Datum, nchildren);

		for (int i = 0; i < nchildren; i++)
			elems[i] = TransactionIdGetDatum(children[i]);
		values[3] = PointerGetDatum(construct_array_builtin(elems, nchildren,
															XIDOID));
	}
	else
		nulls[3] = true;
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	/*
	 * Not in a SERIALIZABLE transaction, whose scan would take predicate
	 * locks every other such part's row would conflict with; a later part
	 * deletes them.
	 */
	if ((ds = dtx_current()) != NULL && !IsolationIsSerializable())
		log_prune(rel, ds->prune);
	table_close(rel, NoLock);
	PopActiveSnapshot();
}

/* A part the log has, read back. */
typedef struct LoggedPart
{
	FullTransactionId gxid;
	TransactionId xid;
	bool		one_phase;
	int			nchildren;
	TransactionId *children;
} LoggedPart;

static Oid *
map_dbs(void)
{
	return DsaPointerIsValid(dtx_shared->dbs)
		? (Oid *) dsa_get_address(dtx_area, dtx_shared->dbs)
		: NULL;
}

/* Have this database's logged parts been read?  The lock is held. */
static bool
map_db_read(Oid dboid)
{
	Oid		   *dbs = map_dbs();

	for (int i = 0; i < dtx_shared->ndbs; i++)
		if (dbs[i] == dboid)
			return true;
	return false;
}

/* They have; the lock is held exclusively. */
static void
map_db_add(Oid dboid)
{
	if (dtx_shared->ndbs == dtx_shared->maxdbs)
	{
		int			newmax = Max(16, dtx_shared->maxdbs * 2);
		dsa_pointer np = dsa_allocate(dtx_area, newmax * sizeof(Oid));

		if (dtx_shared->ndbs > 0)
			memcpy(dsa_get_address(dtx_area, np), map_dbs(),
				   dtx_shared->ndbs * sizeof(Oid));
		if (DsaPointerIsValid(dtx_shared->dbs))
			dsa_free(dtx_area, dtx_shared->dbs);
		dtx_shared->dbs = np;
		dtx_shared->maxdbs = newmax;
	}
	map_dbs()[dtx_shared->ndbs++] = dboid;
}

/*
 * This database's logged parts, into the map: after a restart or a
 * promotion, whose map starts empty.  Each row is a part that committed --
 * a prepared one's row is not seen until it is -- and one the map has
 * already is left as it is.  By the first statement of the database that
 * needs the map, and by the keeper's readers.
 */
static void
map_load_database(void)
{
	Oid			relid;
	List	   *parts = NIL;
	bool		read;

	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	read = dtx_shared->complete || map_db_read(MyDatabaseId);
	LWLockRelease(&dtx_shared->lock);
	if (read)
		return;

	relid = log_relid();
	if (OidIsValid(relid))
	{
		Relation	rel = table_open(relid, AccessShareLock);
		Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
		TableScanDesc scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
		TupleTableSlot *tslot = table_slot_create(rel, NULL);

		while (table_scan_getnextslot(scan, ForwardScanDirection, tslot))
		{
			LoggedPart *p = palloc0(sizeof(LoggedPart));
			bool		isnull;
			Datum		children;

			p->gxid = DatumGetFullTransactionId(slot_getattr(tslot, 1, &isnull));
			p->xid = XidFromFullTransactionId(DatumGetFullTransactionId(slot_getattr(tslot, 2, &isnull)));
			p->one_phase = DatumGetBool(slot_getattr(tslot, 3, &isnull));
			children = slot_getattr(tslot, 4, &isnull);
			if (!isnull)
			{
				Datum	   *elems;
				int			n;

				/* not deconstruct_array_builtin(), which knows no xid */
				deconstruct_array(DatumGetArrayTypeP(children), XIDOID,
								  sizeof(TransactionId), true, TYPALIGN_INT,
								  &elems, NULL, &n);
				p->children = palloc_array(TransactionId, Max(n, 1));
				for (int i = 0; i < n; i++)
					p->children[i] = DatumGetTransactionId(elems[i]);
				p->nchildren = n;
			}
			parts = lappend(parts, p);
		}
		ExecDropSingleTupleTableSlot(tslot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);
		table_close(rel, AccessShareLock);
	}

	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	if (!dtx_shared->complete && !map_db_read(MyDatabaseId))
	{
		foreach_ptr(LoggedPart, p, parts)
		{
			int			pos = map_lower_bound(p->gxid);
			GpDtxEntry *e = map_entries();

			if (e != NULL && pos < dtx_shared->n &&
				FullTransactionIdEquals(e[pos].gxid, p->gxid))
				continue;
			map_put(p->gxid, p->xid, p->children, p->nchildren, p->one_phase);
			e = map_entries();
			e[pos].done = true;
			e[pos].committed = true;
		}
		map_db_add(MyDatabaseId);
		map_hold();
	}
	LWLockRelease(&dtx_shared->lock);
}

/*
 * After a restart or a promotion: the parts it left prepared, which the map,
 * being memory, lost -- read once per postmaster, by the first statement that
 * needs the map, from pg_prepared_xacts; their subtransactions are not
 * known -- and this database's logged parts, which committed.
 */
static void
map_load(void)
{
	int			ret;

	if (dtx_shared->loaded)
	{
		map_load_database();
		return;
	}

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
			map_put(gxid, (TransactionId) strtoul(xidstr, NULL, 10), NULL, -1,
					false);
		}
		map_hold();
		dtx_shared->loaded = true;
	}
	LWLockRelease(&dtx_shared->lock);
	SPI_finish();

	map_load_database();
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
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;

/*
 * What a segment's part last showed of a statement it was sent, which
 * pg_stat_activity goes on showing while the part is prepared and finished,
 * as Cloudberry's does: its protocol's commands leave a backend's activity as
 * it was, where the port's PREPARE TRANSACTION is a statement that would
 * show itself (commit_blocking_on_standby finds a prepare waiting for its
 * mirror by the statement).  The activity, not the text the part was sent,
 * which for DDL is a tree whose activity is the client's statement
 * (gp_ddl.c).  Empty until a part has been sent one.
 */
static char *dtx_part_statement = NULL;

static bool dtx_is_settings_sync(const char *text);

static void
dtx_note_statement(void)
{
	const char *statement;

	if (!GpClusterIsDispatched() || !pgstat_track_activities ||
		MyBEEntry == NULL || MyBEEntry->st_activity_raw == NULL)
		return;
	if (dtx_part_statement == NULL)
		dtx_part_statement = MemoryContextAlloc(TopMemoryContext,
												pgstat_track_activity_query_size);

	/*
	 * The client's statement, where what the part was sent names it -- a
	 * gather's cursor, a COPY of the rows of an INSERT (gp_log.c) -- as
	 * Cloudberry's QE shows the statement it was dispatched.
	 */
	statement = GpLogCoordinatorStatement();
	strlcpy(dtx_part_statement,
			statement != NULL ? statement : MyBEEntry->st_activity_raw,
			pgstat_track_activity_query_size);
}

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

	/* the client's statement, not one a function or a setting runs */
	if (queryDesc->sourceText == debug_query_string &&
		!dtx_is_settings_sync(queryDesc->sourceText))
		dtx_note_statement();

	/*
	 * Where Cloudberry's segment starts a statement it was dispatched
	 * (exec_mpp_query): each the coordinator sends the writer.
	 */
	if (gp_fault_active != NULL && *gp_fault_active > 0 &&
		GpClusterIsDispatched() && !GpShareIsReader())
		GP_FAULT("exec_mpp_query_start");

	if (queryDesc->snapshot != NULL &&
		queryDesc->snapshot->snapshot_type == SNAPSHOT_MVCC &&
		queryDesc->snapshot == GetActiveSnapshot() &&
		GpClusterIsDispatched() && !GpShareIsReader() &&
		(ds = dtx_current()) != NULL)
	{
		Snapshot	crafted;

		/*
		 * Where Cloudberry's segment asks which distributed transaction a
		 * local one is, before it waits for its row (LocalXidGetDistributedXid()
		 * in XactLockTableWait()): here, as a statement that writes or locks
		 * rows looks up the distributed transactions of the local ones in the
		 * map.  A test holds such a statement here while the transaction it
		 * would have waited for commits everywhere, and the map, not the
		 * procarray, must answer for it (gdd/concurrent_update).
		 */
		if (gp_fault_active != NULL && *gp_fault_active > 0 &&
			(queryDesc->operation != CMD_SELECT ||
			 queryDesc->plannedstmt->rowMarks != NIL))
			GP_FAULT("before_get_distributed_xid");

		crafted = dtx_craft(queryDesc->snapshot, ds);
		if (crafted != queryDesc->snapshot)
		{
			Snapshot	old = queryDesc->snapshot;
			Snapshot	made = RegisterSnapshot(crafted);

			/*
			 * The active one may be a utility statement's portal's, which a
			 * read-only query of SPI's runs under -- as the map's own read of
			 * pg_prepared_xacts does, in the SET that brings a snapshot --
			 * and which the portal checks and pops as the statement ends
			 * (pquery.c): the made one takes its place there too.
			 */
			PopActiveSnapshot();
			if (ActivePortal != NULL && ActivePortal->portalSnapshot == old)
			{
				PushActiveSnapshotWithLevel(made, ActivePortal->createLevel);
				ActivePortal->portalSnapshot = GetActiveSnapshot();
			}
			else
				PushActiveSnapshot(made);
			queryDesc->snapshot = made;
			UnregisterSnapshot(old);
			dtx_lower_xmin(made->xmin);
		}
	}

	/*
	 * A parallel worker of a segment's writer (gp_parallel.c) reads with the
	 * snapshot the writer made, which PostgreSQL restores as its active one;
	 * under REPEATABLE READ its transaction's snapshot is the writer's own,
	 * not made to agree, whose xmin may be later.  Its xmin is lowered to the
	 * made one's, as the writer's was: the writer holds it, and a
	 * subtransaction the made snapshot hides is looked up in pg_subtrans only
	 * above the backend's own xmin.
	 */
	if (IsParallelWorker() && GpClusterIsDispatched() && ActiveSnapshotSet() &&
		GetActiveSnapshot()->snapshot_type == SNAPSHOT_MVCC)
		dtx_lower_xmin(GetActiveSnapshot()->xmin);

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/* A writer's transaction ID, reported as each executor run ends (above). */
static void dtx_report_xid(void);

static void
dtx_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	if (prev_executor_run)
		prev_executor_run(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
	dtx_report_xid();
}

static void
dtx_executor_end(QueryDesc *queryDesc)
{
	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
	dtx_report_xid();
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
/* What a segment's part says of itself, and one-phase commit               */
/* ------------------------------------------------------------------------- */

/* The transaction ID gp.dtx_xid says now. */
static TransactionId dtx_reported_xid = InvalidTransactionId;

/* The part committing in one phase, from PRE_COMMIT to its end. */
static FullTransactionId dtx_one_phase_gxid = {0};

/*
 * A dispatched writer's part of a distributed transaction: what the
 * coordinator commits.  A reader's transaction is its own and writes
 * nothing, and the loopback's part, on the coordinator, is the coordinator's
 * to decide.  A parallel worker of the writer (gp_parallel.c) is a part of
 * the writer's transaction, which the writer reports and ends: in parallel
 * mode it could set no setting either.
 */
static bool
dtx_is_writer_part(void)
{
	return GpClusterIsDispatched() && !GpShareIsReader() &&
		!IsParallelWorker() && GpClusterContentId() >= 0;
}

/*
 * gp.dtx_xid, after each statement a writer runs: this part's transaction ID,
 * or empty, so that the answer the coordinator reads carries it and the
 * coordinator knows as it commits which parts wrote, without asking.  After
 * a statement -- and after each executor run and at the executor's end, for
 * a portal of the extended protocol, whose end the coordinator's Close of it
 * brings within the same round trip (conn_send_params(), gp_dispatch.c) --
 * rather than as the ID is given, which no hook sees;
 * a transaction's first statement, its BEGIN, empties it again.  Set as the
 * server sets in_hot_standby, outside any transaction's undo: it says what is
 * so, not what a statement asked for.
 */
static void
dtx_report_xid(void)
{
	TransactionId xid;
	char		buf[16];

	if (!dtx_is_writer_part())
		return;
	xid = IsTransactionState() ? GetTopTransactionIdIfAny() : InvalidTransactionId;
	if (TransactionIdEquals(xid, dtx_reported_xid))
		return;
	dtx_reported_xid = xid;
	if (TransactionIdIsValid(xid))
		snprintf(buf, sizeof(buf), "%u", xid);
	else
		buf[0] = '\0';
	SetConfigOption(GP_DTX_XID_SETTING, buf, PGC_INTERNAL, PGC_S_OVERRIDE);
}

/*
 * The same, for a module whose ProcessUtility hook runs a statement itself,
 * rather than passing it on down to gp_core's (PAX's CLUSTER by its
 * cluster_columns): the coordinator learns from it whether this part wrote.
 */
void
GpDtxReportXid(void)
{
	dtx_report_xid();
}

/*
 * gp.dtx_depends, as this part commits in one phase or prepares: the
 * coordinator transactions whose one-phase parts have committed here and are
 * still in the map, which a transaction here may have seen committed while a
 * distributed snapshot saw them in progress.  The coordinator ends this
 * transaction after each of theirs (COMMIT ORDERING, above).  An entry
 * leaves the map once every distributed snapshot says it committed, so the
 * list is short: the parts that committed a moment ago.  "self" is this
 * part's own coordinator transaction.
 */
static void
dtx_report_depends(FullTransactionId self)
{
	StringInfoData buf;
	GpDtxEntry *e;

	initStringInfo(&buf);
	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	e = map_entries();
	for (int i = 0; i < dtx_shared->n; i++)
	{
		if (!e[i].one_phase || FullTransactionIdEquals(e[i].gxid, self) ||
			!(e[i].done ? e[i].committed : TransactionIdDidCommit(e[i].xid)))
			continue;
		appendStringInfo(&buf, "%s" UINT64_FORMAT, buf.len > 0 ? "," : "",
						 U64FromFullTransactionId(e[i].gxid));
	}
	LWLockRelease(&dtx_shared->lock);
	SetConfigOption(GP_DTX_DEPENDS_SETTING, buf.data, PGC_INTERNAL,
					PGC_S_OVERRIDE);
	pfree(buf.data);
}

/*
 * The coordinator transactions whose parts have committed here and are
 * still in the map, into *gxids: a part commits here -- in one phase, or in
 * its second -- before its coordinator transaction ends for the other
 * sessions, so a snapshot the coordinator takes now may still see any of
 * them in progress, and hide here what it wrote.  The explicit write waits
 * for them before it sends a statement again under a newer snapshot, which
 * must see a row version one of them made (explicit_latest(), gp_split.c).
 * As short as dtx_report_depends()'s list, for the same reason.
 */
int
GpDtxCommittedParts(uint64 **gxids)
{
	GpDtxEntry *e;
	int			n = 0;

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	*gxids = palloc_array(uint64, Max(dtx_shared->n, 1));
	e = map_entries();
	for (int i = 0; i < dtx_shared->n; i++)
		if (e[i].done ? e[i].committed : TransactionIdDidCommit(e[i].xid))
			(*gxids)[n++] = U64FromFullTransactionId(e[i].gxid);
	LWLockRelease(&dtx_shared->lock);
	return n;
}

static bool
dtx_one_phase_check(char **newval, void **extra, GucSource source)
{
	char	   *end;

	if (*newval == NULL || (*newval)[0] == '\0')
		return true;
	errno = 0;
	if (!isdigit((unsigned char) (*newval)[0]) ||
		strtou64(*newval, &end, 10) < FirstNormalTransactionId ||
		errno != 0 || *end != '\0')
	{
		GUC_check_errdetail("A coordinator transaction ID, in decimal, was expected.");
		return false;
	}
	return true;
}

/*
 * A writer's part commits.  In one phase when the coordinator says so, with
 * gp.dtx_one_phase set to the coordinator transaction it commits under: into
 * the map first, so that from the moment it is committed a snapshot here
 * hides it from a distributed snapshot that sees that transaction in
 * progress, and with what it may have seen committed of other one-phase
 * parts.  Otherwise the coordinator read that it wrote nothing, and a part
 * that did write -- which the coordinator would have prepared -- is refused
 * rather than committed on its own, apart from the parts that were prepared.
 */
static void
dtx_pre_commit(void)
{
	TransactionId xid = GetTopTransactionIdIfAny();

	if (dtx_one_phase_setting != NULL && dtx_one_phase_setting[0] != '\0')
	{
		FullTransactionId gxid = FullTransactionIdFromU64(strtou64(dtx_one_phase_setting,
																   NULL, 10));

		if (GpClusterHasSecret() && !GpClusterDispatchTrusted())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("only the coordinator commits a distributed transaction's part in one phase")));
		dtx_one_phase_gxid = gxid;
		if (TransactionIdIsValid(xid))
		{
			TransactionId *children;
			int			nchildren = xactGetCommittedChildren(&children);

			GP_FAULT("start_performDtxProtocolCommitOnePhase");
			log_part(gxid, children, nchildren, true);
			dtx_attach();
			LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
			map_put(gxid, xid, children, nchildren, true);
			map_hold();
			LWLockRelease(&dtx_shared->lock);
		}
		dtx_report_depends(gxid);
		return;
	}

	if (TransactionIdIsValid(xid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("a distributed transaction's part that wrote is committed by the coordinator's two phases, or by its one-phase commit"),
				 errdetail("The coordinator did not know this part wrote.")));
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
 * leaves this backend pointing at nothing.  And a prepared transaction's
 * record lists no temporary relation's files at all (smgrGetPendingDeletes()),
 * so what TRUNCATE, a rewrite or SET TABLESPACE left of one at a COMMIT
 * PREPARED, and what the transaction made at a ROLLBACK PREPARED, are this
 * backend's files that no temporary relation of its has: swept after the
 * second phase of a transaction that used one (sweep_temp_files()).
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
	bool		used_temp;		/* the transaction used a temporary relation */
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

/*
 * This backend's temporary relations' files that no temporary relation of its
 * has any more -- t<its proc number>_<relfilenumber>, in the database's
 * directory and in each tablespace's -- unlinked through the storage
 * manager, as a commit unlinks them, so that whoever follows files hears of
 * it (O21).  After the second phase, when the catalogs say what is left.
 */
static void
sweep_temp_files(void)
{
	Oid			ns,
				toast;
	HASHCTL		ctl;
	HTAB	   *live;
	Relation	pg_class;
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *dirs = NIL;
	List	   *orphans = NIL;
	DIR		   *dir;
	struct dirent *de;
	ProcNumber	procno = ProcNumberForTempRelations();

	/* what this backend's temporary relations have: (tablespace, number) */
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RelFileLocator);
	ctl.entrysize = sizeof(RelFileLocator);
	ctl.hcxt = CurrentMemoryContext;
	live = hash_create("gp temporary files", 64, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	GetTempNamespaceState(&ns, &toast);
	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(pg_class, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);
		RelFileLocator locator;

		if (form->relpersistence != RELPERSISTENCE_TEMP ||
			(form->relnamespace != ns && form->relnamespace != toast))
			continue;
		locator.spcOid = OidIsValid(form->reltablespace)
			? form->reltablespace : MyDatabaseTableSpace;
		locator.dbOid = MyDatabaseId;
		locator.relNumber = form->relfilenode;
		(void) hash_search(live, &locator, HASH_ENTER, NULL);
	}
	table_endscan(scan);
	table_close(pg_class, AccessShareLock);

	/* the database's directory, and its directory in every tablespace */
	dirs = lappend(dirs, list_make2_oid(MyDatabaseTableSpace, InvalidOid));
	if ((dir = AllocateDir(PG_TBLSPC_DIR)) != NULL)
	{
		while ((de = ReadDir(dir, PG_TBLSPC_DIR)) != NULL)
		{
			Oid			spc = atooid(de->d_name);

			if (OidIsValid(spc) && spc != MyDatabaseTableSpace)
				dirs = lappend(dirs, list_make2_oid(spc, InvalidOid));
		}
		FreeDir(dir);
	}

	foreach_ptr(List, d, dirs)
	{
		Oid			spc = linitial_oid(d);
		char	   *path = GetDatabasePath(MyDatabaseId, spc);

		if ((dir = AllocateDir(path)) == NULL)
			continue;
		while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
		{
			int			owner;
			unsigned int number;
			int			len;
			RelFileLocator locator;
			bool		found;

			/* t<proc>_<number>, then a fork's suffix or a segment's, or not */
			if (sscanf(de->d_name, "t%d_%u%n", &owner, &number, &len) != 2 ||
				owner != procno ||
				(de->d_name[len] != '\0' && de->d_name[len] != '_' &&
				 de->d_name[len] != '.'))
				continue;
			locator.spcOid = spc;
			locator.dbOid = MyDatabaseId;
			locator.relNumber = number;
			(void) hash_search(live, &locator, HASH_ENTER, &found);
			if (found)
				continue;		/* a relation's, or listed already */
			orphans = lappend(orphans,
							  smgropen(locator, procno));
		}
		FreeDir(dir);
		pfree(path);
	}

	if (orphans != NIL)
	{
		SMgrRelation *srels = palloc_array(SMgrRelation, list_length(orphans));
		int			n = 0;

		foreach_ptr(SMgrRelationData, srel, orphans)
			srels[n++] = srel;
		smgrdounlinkall(srels, n, false);
		for (int i = 0; i < n; i++)
			smgrclose(srels[i]);
		pfree(srels);
	}
	hash_destroy(live);
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
	bool		used_temp;

	GP_FAULT("start_prepare");

	/* see "Temporary relations" above */
	PreCommit_on_commit_actions();
	used_temp = (MyXactFlags & XACT_FLAGS_ACCESSEDTEMPNAMESPACE) != 0;
	MyXactFlags &= ~XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	GetTempNamespaceState(&ns, &toast);
	if (dropped_temp != NIL || used_temp || (!temp_schema_before && OidIsValid(ns)))
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
		pt->used_temp = used_temp;
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
	log_part(dtx_preparing, children, nchildren, false);

	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	map_put(dtx_preparing, xid, children, nchildren, false);
	map_hold();
	LWLockRelease(&dtx_shared->lock);
	dtx_report_depends(dtx_preparing);
}

/* A part's end here: committed, or rolled back. */
static void
map_mark_done(FullTransactionId gxid, bool commit)
{
	GpDtxEntry *e;
	int			pos;

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
	ListCell   *lc;

	map_mark_done(gxid, commit);

	foreach(lc, prepared_temp)
	{
		PreparedTemp *pt = (PreparedTemp *) lfirst(lc);

		if (!FullTransactionIdEquals(pt->gxid, gxid))
			continue;
		prepared_temp = foreach_delete_current(prepared_temp, lc);
		if (commit)
			unlink_temp(pt->dropped);
		if (pt->used_temp)
			sweep_temp_files();
		if (!commit && pt->made_schema)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the temporary schema of this session was made by a distributed transaction that rolled back"),
					 errdetail("The segment's session ends, and the coordinator opens another.")));
		list_free_deep(pt->dropped);
		pfree(pt);
	}
}

/*
 * PREPARE TRANSACTION under a distributed transaction's gid: the
 * coordinator's to use.  The recovery process commits or rolls back a part by
 * the coordinator's clog, so one prepared under such a gid by anybody else
 * would be decided by a transaction that has nothing to do with it, and a
 * snapshot would wait for it.  The secret says which connection is the
 * coordinator's where there is one; where there is none, any dispatched one
 * is.  On one node the loopback's connection is that (gp_loopback.c).
 */
static void
dtx_check_gid_reserved(const char *gid)
{
	if (GpClusterHasSecret() ? !GpClusterDispatchTrusted()
		: !GpClusterIsDispatched())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("transaction identifier \"%s\" is reserved for distributed transactions",
						gid),
				 errhint("Choose a transaction identifier that does not begin with \"%s\".",
						 GP_DTX_GID_PREFIX)));
}

/* Does this REINDEX TABLE or INDEX rebuild an index of a mapped table? */
static bool
reindex_of_mapped_table(ReindexStmt *stmt)
{
	Oid			relid;
	HeapTuple	tuple;
	bool		mapped;

	if ((stmt->kind != REINDEX_OBJECT_TABLE && stmt->kind != REINDEX_OBJECT_INDEX) ||
		stmt->relation == NULL)
		return false;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (OidIsValid(relid) && stmt->kind == REINDEX_OBJECT_INDEX)
		relid = IndexGetRelation(relid, true);
	if (!OidIsValid(relid))
		return false;
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return false;
	/* a mapped relation's pg_class row has no file number of its own */
	mapped = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode == InvalidOid &&
		RELKIND_HAS_STORAGE(((Form_pg_class) GETSTRUCT(tuple))->relkind);
	ReleaseSysCache(tuple);
	return mapped;
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

	/*
	 * A session's own PREPARE TRANSACTION, on a node of a cluster: Cloudberry
	 * prepares only its distributed transactions' parts, and refuses the
	 * user's -- on the coordinator, and in a session of a node's own, which
	 * is Cloudberry's utility mode (its utility.c and postgres.c).  A gid of
	 * the distributed kind says it is reserved first, as before.
	 */
	if (IsA(parsetree, TransactionStmt) &&
		((TransactionStmt *) parsetree)->kind == TRANS_STMT_PREPARE &&
		!GpClusterIsDispatched())
	{
		const char *gid = ((TransactionStmt *) parsetree)->gid;

		if (GpDtxParseGid(gid, &gxid))
			dtx_check_gid_reserved(gid);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 GpClusterBackendRole() == GP_ROLE_DISPATCH ?
				 errmsg("PREPARE TRANSACTION is not yet supported in Apache Cloudberry") :
				 errmsg("PREPARE TRANSACTION is not supported in utility mode")));
	}

	/*
	 * REINDEX of a mapped catalog table -- pg_class, say -- in a transaction
	 * block: its parts on the segments change relation mappings, which
	 * PREPARE TRANSACTION cannot take, so the commit would fail; refused now,
	 * in Cloudberry's words (reindex_index(), catalog/index.c).
	 */
	if (IsA(parsetree, ReindexStmt) &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		reindex_of_mapped_table((ReindexStmt *) parsetree))
		PreventInTransactionBlock(true, "REINDEX of a catalog table");

	if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *ts = (TransactionStmt *) parsetree;

		/*
		 * Cloudberry sends a savepoint's command to every segment as it runs
		 * it, where the port sends it with the next statement; its commit's
		 * INFO lines name them all (DefineDispatchSavepoint(), xact.c).
		 */
		if ((ts->kind == TRANS_STMT_SAVEPOINT || ts->kind == TRANS_STMT_RELEASE ||
			 ts->kind == TRANS_STMT_ROLLBACK_TO) && IsTransactionBlock() &&
			GpClusterBackendRole() == GP_ROLE_DISPATCH)
			GpReportDtxReached(NULL, NULL, 0);

		/* the segments' part gone with its gang: no savepoint to go back to */
		if (ts->kind == TRANS_STMT_ROLLBACK_TO &&
			GpClusterBackendRole() == GP_ROLE_DISPATCH)
			GpDispatchCheckRollbackTo(ts->savepoint_name);

		if (ts->gid != NULL && GpDtxParseGid(ts->gid, &gxid))
		{
			/* the part's statement is what its phases show */
			if (dtx_part_statement != NULL && dtx_part_statement[0] != '\0' &&
				GpClusterIsDispatched())
				pgstat_report_activity(STATE_RUNNING, dtx_part_statement);

			switch (ts->kind)
			{
				case TRANS_STMT_PREPARE:
					dtx_check_gid_reserved(ts->gid);
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
	else if (!IsA(parsetree, VariableSetStmt) &&
			 context == PROCESS_UTILITY_TOPLEVEL && queryString == debug_query_string)
		dtx_note_statement();

	/* Cloudberry's, at the start of FinishPreparedTransaction() (twophase.c) */
	if (finishing)
		GP_FAULT("finish_prepared_start_of_function");

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
	dtx_report_xid();
}

/*
 * One node: no segment and no distributed snapshot, but the parts the
 * loopback prepares in its other databases (gp_loopback.c), decided by this
 * server's clog as a coordinator's are.  Their gid is reserved as there.
 */
static void
dtx_single_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	FullTransactionId gxid;

	if (IsA(parsetree, TransactionStmt) &&
		((TransactionStmt *) parsetree)->kind == TRANS_STMT_PREPARE &&
		GpDtxParseGid(((TransactionStmt *) parsetree)->gid, &gxid))
		dtx_check_gid_reserved(((TransactionStmt *) parsetree)->gid);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

static void
dtx_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (dtx_is_writer_part() && IsTransactionBlock())
				dtx_pre_commit();
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (FullTransactionIdIsValid(dtx_preparing))
				dtx_pre_prepare();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
			if (FullTransactionIdIsValid(dtx_one_phase_gxid))
				map_mark_done(dtx_one_phase_gxid, event == XACT_EVENT_COMMIT);
			dtx_one_phase_gxid = InvalidFullTransactionId;
			/* FALLTHROUGH */
		case XACT_EVENT_PREPARE:
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
	pg_atomic_fetch_add_u32(&dtx_shared->recovery_wakes, 1);
	proc = dtx_shared->recovery_proc;
	if (proc != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(proc)->procLatch);
}

void
GpDtxNoteLoopbackJournal(void)
{
	dtx_attach();
	pg_atomic_fetch_add_u32(&dtx_shared->loopback_journals, 1);
	GpDtxWakeRecovery();
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

/* "segment 0", "the coordinator" or, on one node, "this server". */
static char *
node_name(const GpSegmentConfig *node)
{
	if (node->content >= 0)
		return psprintf("segment %d", node->content);
	return pstrdup(GpClusterIsSingleNode() ? "this server" : "the coordinator");
}

/*
 * The node the recovery process runs on: the coordinator the cluster file
 * names, or one node, which no file names, reached as the loopback reaches it
 * -- by its first Unix socket, or TCP on this host.
 */
static const GpSegmentConfig *
recovery_self(void)
{
	static GpSegmentConfig single = {0};
	const GpSegmentConfig *self = GpClusterSelf();

	if (self != NULL)
		return self;
	if (single.hostname == NULL)
	{
		char	   *dirs = pstrdup(Unix_socket_directories ? Unix_socket_directories : "");
		List	   *list;

		single.dbid = GpClusterDbid();
		single.content = -1;
		single.role = 'p';
		single.port = PostPortNumber;
		single.hostname = MemoryContextStrdup(TopMemoryContext,
											  SplitDirectoriesString(dirs, ',', &list) &&
											  list != NIL
											  ? (char *) linitial(list) : "localhost");
	}
	return &single;
}

/* A connection to one database of a node, or NULL, logged. */
static PGconn *
recovery_connect(const GpSegmentConfig *seg, const char *dbname)
{
	const char *keywords[5 + GP_INTERNAL_CONN_OPTIONS];
	const char *values[5 + GP_INTERNAL_CONN_OPTIONS];
	char		portbuf[16];
	int			n = 0;
	PGconn	   *conn;

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
	n = GpInternalConnOptions(keywords, values, n);

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
 * Where a round is, for gp_stat_progress_dtx_recovery: Cloudberry's phases
 * of its recovery (PROGRESS_DTX_RECOVERY_*, commands/progress.h), numbered
 * as its view names them -- the first round after a start its start-up's,
 * committing what committed and rolling back what is in doubt, and every
 * round after it one of its periodic ones, which look for orphans -- and its
 * counts, of distributed transactions: the parts on every node of one
 * coordinator transaction.
 */
#define DTX_PHASE_INITIALIZING			0
#define DTX_PHASE_RECOVER_COMMITTED		1
#define DTX_PHASE_GATHER_IN_DOUBT		2
#define DTX_PHASE_ABORT_IN_DOUBT		3
#define DTX_PHASE_GATHER_ORPHANED		4
#define DTX_PHASE_MANAGE_ORPHANED		5

typedef enum DtxProgress
{
	DTX_COMMITTED_TOTAL,
	DTX_COMMITTED_DONE,
	DTX_IN_DOUBT_TOTAL,
	DTX_IN_DOUBT_IN_PROGRESS,
	DTX_IN_DOUBT_ABORTED,
} DtxProgress;

static void
recovery_phase(int phase, bool active)
{
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	if (active && !dtx_shared->progress_active)
		memset(dtx_shared->progress, 0, sizeof(dtx_shared->progress));
	dtx_shared->progress_active = active;
	dtx_shared->progress_phase = phase;
	LWLockRelease(&dtx_shared->lock);
}

static void
recovery_count(DtxProgress which, int64 value)
{
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	dtx_shared->progress[which] = value;
	LWLockRelease(&dtx_shared->lock);
}

/* A part a round found prepared on a node, and what became of its transaction. */
typedef struct RecoveryPart
{
	const GpSegmentConfig *node;
	char	   *gid;
	char	   *dbname;
	FullTransactionId gxid;
	DtxOutcome	outcome;
} RecoveryPart;

/* How many distributed transactions the parts of this outcome are. */
static int64
parts_transactions(const RecoveryPart *parts, int nparts, DtxOutcome outcome)
{
	int64		n = 0;

	for (int i = 0; i < nparts; i++)
	{
		bool		first = parts[i].outcome == outcome;

		for (int j = 0; j < i && first; j++)
			first = !(parts[j].outcome == outcome &&
					  FullTransactionIdEquals(parts[j].gxid, parts[i].gxid));
		if (first)
			n++;
	}
	return n;
}

typedef enum RecoveryFinish
{
	FINISH_DONE,				/* or done already */
	FINISH_FAILED,				/* refused, logged */
	FINISH_UNREACHED,			/* no connection, or busy past the wait */
} RecoveryFinish;

/*
 * A part's COMMIT or ROLLBACK PREPARED, over a connection to the database it
 * was prepared in: done, or done already -- by its own backend, whose commit
 * is then waited for on the mirror, as dtx_finish_again() waits
 * (gp_dispatch.c), or by a round before -- or not, logged.  A part another
 * backend is still finishing, "is busy" -- its second phase, waiting for the
 * mirror -- is asked for again each second, for up to five minutes:
 * Cloudberry's recovery waits for such a part too, its COMMIT PREPARED
 * waiting for the mirror (FinishPreparedTransaction(), twophase.c) until
 * FTS lets it go.
 */
static RecoveryFinish
recovery_finish(const RecoveryPart *part)
{
	bool		commit = part->outcome == DTX_COMMITTED;
	char	   *sql = psprintf("%s PREPARED '%s'", commit ? "COMMIT" : "ROLLBACK",
							   part->gid);
	TimestampTz deadline = GetCurrentTimestamp() + 300 * USECS_PER_SEC;

	for (;;)
	{
		PGconn	   *conn = recovery_connect(part->node, part->dbname);
		PGresult   *res;
		const char *state;
		bool		done;
		bool		gone;
		bool		busy;

		if (conn == NULL)
			return FINISH_UNREACHED;
		res = libpqsrv_exec(conn, sql, recovery_wait_event());
		state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		done = PQresultStatus(res) == PGRES_COMMAND_OK;
		gone = state != NULL && strcmp(state, "42704") == 0;
		busy = state != NULL && strcmp(state, "55000") == 0;
		if (done)
			ereport(LOG,
					(errmsg("distributed transaction recovery: %s on %s",
							sql, node_name(part->node))));
		else if (!gone && (!busy || GetCurrentTimestamp() >= deadline))
			ereport(LOG,
					(errmsg("distributed transaction recovery could not finish \"%s\" on %s",
							part->gid, node_name(part->node)),
					 errdetail_internal("%s", PQerrorMessage(conn))));
		PQclear(res);
		if (gone && commit)
			PQclear(libpqsrv_exec(conn, "SELECT gp_internal.dtx_wait_mirror()",
								  recovery_wait_event()));
		libpqsrv_disconnect(conn);

		if (done || gone)
			return FINISH_DONE;
		if (!busy)
			return FINISH_FAILED;
		if (GetCurrentTimestamp() >= deadline)
			return FINISH_UNREACHED;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000, recovery_wait_event());
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Finish the parts of every distributed transaction of one outcome, a
 * transaction's parts together, counting the transactions finished on
 * every node as "done".  Whether every part was reached is returned: one
 * refused is logged, and left for the next round, as it always was.
 */
static bool
recovery_finish_all(const RecoveryPart *parts, int nparts, DtxOutcome outcome,
					DtxProgress done)
{
	bool		complete = true;
	int64		ndone = 0;

	for (int i = 0; i < nparts; i++)
	{
		bool		first = parts[i].outcome == outcome;
		bool		all = true;

		for (int j = 0; j < i && first; j++)
			first = !(parts[j].outcome == outcome &&
					  FullTransactionIdEquals(parts[j].gxid, parts[i].gxid));
		if (!first)
			continue;
		for (int j = i; j < nparts; j++)
		{
			RecoveryFinish finish;

			if (parts[j].outcome != outcome ||
				!FullTransactionIdEquals(parts[j].gxid, parts[i].gxid))
				continue;
			finish = recovery_finish(&parts[j]);
			if (finish != FINISH_DONE)
				all = false;
			if (finish == FINISH_UNREACHED)
				complete = false;
		}
		if (all)
			recovery_count(done, ++ndone);
	}
	return complete;
}

/*
 * One round: every part a node holds prepared under a distributed gid,
 * committed or rolled back by what the coordinator's clog says of its
 * transaction -- each segment's, and the coordinator's own, which the
 * loopback prepared in another of its databases (gp_loopback.c); on one node
 * those alone.  One still in progress here is its backend's.  "min_age"
 * leaves alone what was prepared less than that many seconds ago, whose
 * second phase is on its way from the backend that prepared it; the round
 * after a restart, and one a backend asked for, take everything.
 *
 * As Cloudberry's recovery goes, and its view says: the parts of every node
 * are gathered first, then those whose transaction committed are committed,
 * then the rest rolled back -- in "startup" rounds, those until one has
 * reached every node, its start-up's phases, and after them its periodic
 * rounds', which find orphans.  Returns whether every node was reached, and
 * every part it found busy finished.
 */
static bool
recovery_round(int min_age, bool startup)
{
	const GpSegmentConfig *segs;
	int			nsegs;
	bool		complete = true;
	RecoveryPart *parts = NULL;
	int			nparts = 0;
	int			maxparts = 0;

	recovery_phase(startup ? DTX_PHASE_INITIALIZING : DTX_PHASE_GATHER_ORPHANED,
				   true);

	/*
	 * The primaries FTS last published: a part prepared on a primary it
	 * failed over from is on the mirror it promoted, from PREPARE's WAL.
	 * And the segments gpexpand added, or gpshrink left.
	 */
	if (!GpClusterAdoptSegments())
		(void) GpClusterRefresh();
	segs = GpClusterSegments(&nsegs);
	for (int s = 0; s <= nsegs; s++)
	{
		const GpSegmentConfig *node = s < nsegs ? &segs[s] : recovery_self();
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

			if (!GpDtxParseGid(gid, &gxid) || age < min_age)
				continue;
			outcome = dtx_outcome(gxid);
			if (outcome == DTX_UNKNOWN)
			{
				ereport(WARNING,
						(errmsg("%s holds prepared transaction \"%s\", which the coordinator has no record of",
								node_name(node), gid),
						 errhint("Commit or roll it back by hand, in database \"%s\" there.",
								 dbname)));
				continue;
			}
			if (nparts == maxparts)
			{
				maxparts = Max(16, maxparts * 2);
				parts = parts == NULL ? palloc_array(RecoveryPart, maxparts)
					: repalloc_array(parts, RecoveryPart, maxparts);
			}
			parts[nparts].node = node;
			parts[nparts].gid = pstrdup(gid);
			parts[nparts].dbname = pstrdup(dbname);
			parts[nparts].gxid = gxid;
			parts[nparts].outcome = outcome;
			nparts++;
		}
		PQclear(res);
		libpqsrv_disconnect(conn);
	}

	/* what committed is committed: COMMIT PREPARED in each part's database */
	if (startup)
		recovery_phase(DTX_PHASE_RECOVER_COMMITTED, true);
	else
		recovery_phase(DTX_PHASE_MANAGE_ORPHANED, true);
	recovery_count(DTX_COMMITTED_TOTAL,
				   parts_transactions(parts, nparts, DTX_COMMITTED));
	if (!recovery_finish_all(parts, nparts, DTX_COMMITTED, DTX_COMMITTED_DONE))
		complete = false;
	if (startup)
		GP_FAULT("post_progress_recovery_comitted");

	/*
	 * The rest is in doubt: one still in progress here is its backend's, and
	 * one that did not commit is rolled back
	 */
	if (startup)
		recovery_phase(DTX_PHASE_GATHER_IN_DOUBT, true);
	recovery_count(DTX_IN_DOUBT_IN_PROGRESS,
				   parts_transactions(parts, nparts, DTX_IN_PROGRESS));
	recovery_count(DTX_IN_DOUBT_TOTAL,
				   parts_transactions(parts, nparts, DTX_IN_PROGRESS) +
				   parts_transactions(parts, nparts, DTX_ABORTED));
	if (startup)
		recovery_phase(DTX_PHASE_ABORT_IN_DOUBT, true);
	if (!recovery_finish_all(parts, nparts, DTX_ABORTED, DTX_IN_DOUBT_ABORTED))
		complete = false;

	recovery_phase(DTX_PHASE_INITIALIZING, false);
	for (int i = 0; i < nparts; i++)
	{
		pfree(parts[i].gid);
		pfree(parts[i].dbname);
	}
	if (parts != NULL)
		pfree(parts);
	return complete;
}

/* ------------------------------------------------------------------------- */
/* The loopback's journal                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The loopback's parts a server that cannot prepare leaves open until the
 * commit record of the transaction they are part of (gp_loopback.c), each
 * journalled in the asking database with that transaction: the database it
 * writes to, its own transaction there, the session's user and role it ran
 * as, and its statements.  A part whose transaction there committed is done,
 * and its row goes.  One whose transaction there did not -- a crash between
 * the two commits, or a connection lost -- is run there again, once: its new
 * transaction there is journalled before it commits, so that a failure then
 * leaves the row naming a transaction that did not commit, and the next
 * round runs it again.  One still in progress is its own backend's.
 */

/* Run a statement, quietly; its result, or NULL with the error logged. */
static PGresult *
journal_exec(PGconn *conn, const char *sql, ExecStatusType want,
			 const char *where)
{
	PGresult   *res = libpqsrv_exec(conn, sql, recovery_wait_event());

	if (res != NULL && PQresultStatus(res) == want)
		return res;
	ereport(LOG,
			(errmsg("distributed transaction recovery could not read or write the loopback's journal in database \"%s\"",
					where),
			 errdetail_internal("%s", res != NULL ? PQresultErrorMessage(res)
								: PQerrorMessage(conn))));
	if (res != NULL)
		PQclear(res);
	return NULL;
}

/*
 * One journalled part whose transaction there did not commit, run there
 * again; false where it could not be, and is to be tried again.
 */
static bool
journal_rewrite(PGconn *here, const char *here_db, const char *xid,
				const char *dbname, const char *part_xid, const char *session_role,
				const char *current_role)
{
	const GpSegmentConfig *self = recovery_self();
	char	   *key = psprintf("xid = %s AND dbname = %s",
							   quote_literal_cstr(xid), quote_literal_cstr(dbname));
	PGconn	   *there;
	PGresult   *res;
	PGresult   *stmts;
	char	   *new_xid;
	bool		ok = false;

	stmts = journal_exec(here,
						 psprintf("SELECT s FROM gp_internal.loopback_journal,"
								  " unnest(statements) WITH ORDINALITY AS u(s, n)"
								  " WHERE %s AND part_xid = %s ORDER BY n",
								  key, quote_literal_cstr(part_xid)),
						 PGRES_TUPLES_OK, here_db);
	if (stmts == NULL)
		return false;
	if ((there = recovery_connect(self, dbname)) == NULL)
	{
		PQclear(stmts);
		return false;
	}

	res = libpqsrv_exec(there,
						psprintf("BEGIN; SET LOCAL SESSION AUTHORIZATION %s;%s"
								 " SELECT pg_catalog.pg_current_xact_id()",
								 quote_identifier(session_role),
								 current_role[0] != '\0'
								 ? psprintf(" SET LOCAL ROLE %s;", quote_identifier(current_role))
								 : ""),
						recovery_wait_event());
	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK)
		goto failed;
	new_xid = pstrdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	res = NULL;

	for (int i = 0; i < PQntuples(stmts); i++)
	{
		res = libpqsrv_exec(there, PQgetvalue(stmts, i, 0), recovery_wait_event());
		if (res == NULL ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
			goto failed;
		PQclear(res);
		res = NULL;
	}

	/* its new transaction there, journalled before it commits */
	res = journal_exec(here,
					   psprintf("UPDATE gp_internal.loopback_journal SET part_xid = %s"
								" WHERE %s AND part_xid = %s",
								quote_literal_cstr(new_xid), key,
								quote_literal_cstr(part_xid)),
					   PGRES_COMMAND_OK, here_db);
	if (res == NULL || strcmp(PQcmdTuples(res), "1") != 0)
		goto failed;
	PQclear(res);
	res = libpqsrv_exec(there, "COMMIT", recovery_wait_event());
	if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK ||
		strcmp(PQcmdStatus(res), "COMMIT") != 0)
		goto failed;
	PQclear(res);
	res = journal_exec(here,
					   psprintf("DELETE FROM gp_internal.loopback_journal"
								" WHERE %s AND part_xid = %s",
								key, quote_literal_cstr(new_xid)),
					   PGRES_COMMAND_OK, here_db);
	if (res != NULL)
		PQclear(res);
	ereport(LOG,
			(errmsg("distributed transaction recovery wrote in database \"%s\" the part of transaction %s of database \"%s\", which a failure had lost",
					dbname, xid, here_db)));
	ok = true;
	res = NULL;

failed:
	if (!ok)
	{
		ereport(WARNING,
				(errmsg("distributed transaction recovery could not write in database \"%s\" the part of transaction %s of database \"%s\"",
						dbname, xid, here_db),
				 errdetail_internal("%s", res != NULL ? PQresultErrorMessage(res)
									: PQerrorMessage(there)),
				 errhint("It is journalled in gp_internal.loopback_journal of database \"%s\", and tried again as the server starts.",
						 here_db)));
		if (res != NULL)
			PQclear(res);
	}
	PQclear(stmts);
	libpqsrv_disconnect(there);
	return ok;
}

/*
 * Every database's journal, on the node the recovery process runs on: true
 * where each was read and nothing is left to try again.
 */
static bool
recovery_journals(void)
{
	const GpSegmentConfig *self = recovery_self();
	PGconn	   *conn = recovery_connect(self, "postgres");
	PGresult   *dbs;
	bool		complete = true;

	if (conn == NULL)
		conn = recovery_connect(self, "template1");
	if (conn == NULL)
		return false;
	dbs = journal_exec(conn,
					   "SELECT datname FROM pg_catalog.pg_database"
					   " WHERE datallowconn ORDER BY datname",
					   PGRES_TUPLES_OK, "postgres");
	libpqsrv_disconnect(conn);
	if (dbs == NULL)
		return false;

	for (int d = 0; d < PQntuples(dbs); d++)
	{
		const char *here_db = PQgetvalue(dbs, d, 0);
		PGconn	   *here = recovery_connect(self, here_db);
		PGresult   *rows;

		if (here == NULL)
		{
			complete = false;
			continue;
		}
		/* a database without gp_core's extension has no journal */
		rows = journal_exec(here,
							"SELECT pg_catalog.to_regclass('gp_internal.loopback_journal') IS NOT NULL",
							PGRES_TUPLES_OK, here_db);
		if (rows == NULL || strcmp(PQgetvalue(rows, 0, 0), "t") != 0)
		{
			if (rows == NULL)
				complete = false;
			else
				PQclear(rows);
			libpqsrv_disconnect(here);
			continue;
		}
		PQclear(rows);

		rows = journal_exec(here,
							"SELECT xid, dbname, part_xid, session_role,"
							" coalesce(current_role_name, ''),"
							" pg_catalog.pg_xact_status(part_xid)"
							" FROM gp_internal.loopback_journal ORDER BY xid",
							PGRES_TUPLES_OK, here_db);
		if (rows == NULL)
		{
			complete = false;
			libpqsrv_disconnect(here);
			continue;
		}

		for (int r = 0; r < PQntuples(rows); r++)
		{
			const char *xid = PQgetvalue(rows, r, 0);
			const char *dbname = PQgetvalue(rows, r, 1);
			const char *part_xid = PQgetvalue(rows, r, 2);
			const char *status = PQgetvalue(rows, r, 5);

			if (strcmp(status, "committed") == 0)
			{
				PGresult   *res = journal_exec(here,
											   psprintf("DELETE FROM gp_internal.loopback_journal"
														" WHERE xid = %s AND dbname = %s AND part_xid = %s",
														quote_literal_cstr(xid),
														quote_literal_cstr(dbname),
														quote_literal_cstr(part_xid)),
											   PGRES_COMMAND_OK, here_db);

				if (res != NULL)
					PQclear(res);
				else
					complete = false;
			}
			else if (strcmp(status, "aborted") == 0)
			{
				if (!journal_rewrite(here, here_db, xid, dbname, part_xid,
									 PQgetvalue(rows, r, 3), PQgetvalue(rows, r, 4)))
					complete = false;
			}
			else
				complete = false;	/* its own backend's, or older than the clog */
		}
		PQclear(rows);
		libpqsrv_disconnect(here);
	}
	PQclear(dbs);
	return complete;
}

PGDLLEXPORT void GpDtxRecoveryMain(Datum main_arg);

static bool recovery_orphan_check(bool after);

void
GpDtxRecoveryMain(Datum main_arg)
{
	bool		everything = true;
	bool		journals_due = true;	/* every database's, at start */
	uint32		journals_seen = 0;
	uint32		wakes_seen;
	bool		periodic;

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
	wakes_seen = pg_atomic_read_u32(&dtx_shared->recovery_wakes);

	for (;;)
	{
		int			rc;
		uint32		wakes;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		GP_FAULT("dtx_recovery_round");

		/*
		 * A round a backend asked for takes everything, whatever reset the
		 * latch it set: a part its second phase did not reach is young, and
		 * a periodic round would leave it alone for
		 * gp.dtx_recovery_prepared_period, while a segment's statements
		 * whose snapshots say it committed wait for it there.
		 */
		wakes = pg_atomic_read_u32(&dtx_shared->recovery_wakes);
		if (wakes != wakes_seen)
		{
			wakes_seen = wakes;
			everything = true;
		}

		/*
		 * Until a round reaches every segment, each takes everything.  One
		 * node that cannot prepare has nothing prepared to finish.  A round
		 * after those is Cloudberry's periodic check of orphaned prepared
		 * transactions, which its faults mark, and one skips
		 * (recovery_orphan_check()): a round skipped leaves what it would
		 * have taken to the next.
		 */
		periodic = dtx_shared->recovered;
		if (periodic && recovery_orphan_check(false))
			;
		else if ((GpClusterIsSingleNode() && max_prepared_xacts == 0) ||
				 recovery_round(everything ? 0 : dtx_recovery_prepared_period,
								!periodic))
		{
			everything = false;
			if (!dtx_shared->recovered)
			{
				LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
				dtx_shared->recovered = true;
				LWLockRelease(&dtx_shared->lock);
				ereport(LOG,
						(errmsg("DTM Started"),
						 errdetail("Distributed transaction recovery has reached every node.")));
			}
			if (periodic)
				(void) recovery_orphan_check(true);
		}
		else if (periodic)
			(void) recovery_orphan_check(true);

		/*
		 * The loopback's journals: as the server starts, and whenever a
		 * transaction that journalled a part has committed since, and until
		 * none is left to try again.
		 */
		{
			uint32		journals = pg_atomic_read_u32(&dtx_shared->loopback_journals);

			if (journals_due || journals != journals_seen)
			{
				journals_seen = journals;
				journals_due = !recovery_journals();
			}
		}

		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   (everything ? 5 : dtx_recovery_interval) * 1000L,
					   recovery_wait_event());
		ResetLatch(MyLatch);
		if (rc & WL_LATCH_SET)
			everything = true;
	}
}

/* ------------------------------------------------------------------------- */
/* The keeper, on a segment                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A background worker of each segment's, a mirror's too, that keeps the slot
 * and the map through a restart and a promotion (THE DISTRIBUTED LOG, above):
 *
 *   on a mirror, it holds back what any transaction a table may still have
 *   unfrozen deleted -- the oldest transaction ID the WAL says is not frozen
 *   everywhere, which no part in its primary's map can precede, since the
 *   primary's slot held its tables back from freezing past any -- so that
 *   the moment a promotion ends, the slot holds;
 *
 *   on a primary, until the map has every database's logged parts, it reads
 *   them, a database at a time, with a worker connected to each;
 *
 *   and then it writes the slot's xmin on disk: the oldest of the map's and
 *   of the transactions running, which no part the map may yet gain can
 *   precede, so that a restart holds from its first moment.  Marked dirty,
 *   it reaches the disk at the next checkpoint, and an older one there holds
 *   back more, never too little.
 */

static uint32
keeper_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryDtxKeeper");
	return event;
}

/* A mirror's hold: see above. */
static void
keeper_hold_all(void)
{
	TransactionId oldest;
	ReplicationSlot *slot;

	LWLockAcquire(XidGenLock, LW_SHARED);
	oldest = TransamVariables->oldestXid;
	LWLockRelease(XidGenLock);
	if (!TransactionIdIsNormal(oldest))
		return;

	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	slot = slot_make(oldest, oldest);
	SpinLockAcquire(&slot->mutex);
	if (!TransactionIdEquals(slot->effective_xmin, oldest) ||
		!TransactionIdEquals(slot->data.xmin, oldest))
	{
		slot->effective_xmin = oldest;
		slot->data.xmin = oldest;
		slot->just_dirtied = true;
		slot->dirty = true;
	}
	SpinLockRelease(&slot->mutex);
	ReplicationSlotsComputeRequiredXmin(false);
	dtx_shared->held = oldest;
	LWLockRelease(&dtx_shared->lock);
}

/* The databases a worker can connect to, in "cxt". */
static List *
keeper_databases(MemoryContext cxt)
{
	List	   *dbs = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;

	StartTransactionCommand();
	(void) GetTransactionSnapshot();
	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);
		MemoryContext old;

		if (!db->datallowconn || database_is_invalid_form(db))
			continue;
		old = MemoryContextSwitchTo(cxt);
		dbs = lappend_oid(dbs, db->oid);
		MemoryContextSwitchTo(old);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	CommitTransactionCommand();
	return dbs;
}

/* One database's logged parts, read by a worker connected to it. */
static bool
keeper_read_database(Oid dboid)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	pid_t		pid;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpDtxLogReaderMain");
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core distributed log reader");
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core distributed log reader");
	worker.bgw_main_arg = ObjectIdGetDatum(dboid);
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		return false;
	if (WaitForBackgroundWorkerStartup(handle, &pid) == BGWH_STARTED)
		(void) WaitForBackgroundWorkerShutdown(handle);
	pfree(handle);
	return true;
}

/*
 * A primary's map, completed: every database's logged parts read, but those
 * of a database that went away meanwhile.  False when it has to be tried
 * again: a worker could not be started, or one did not read its database.
 */
static bool
keeper_complete(MemoryContext cxt)
{
	List	   *dbs = keeper_databases(cxt);
	List	   *after;
	bool		complete = true;

	foreach_oid(dboid, dbs)
	{
		bool		read;

		LWLockAcquire(&dtx_shared->lock, LW_SHARED);
		read = map_db_read(dboid);
		LWLockRelease(&dtx_shared->lock);
		if (!read && !keeper_read_database(dboid))
			return false;
	}

	/* Each still there has been read: the map has everything. */
	after = keeper_databases(cxt);
	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	foreach_oid(dboid, dbs)
		if (list_member_oid(after, dboid) && !map_db_read(dboid))
			complete = false;
	if (complete)
	{
		dtx_shared->complete = true;
		map_hold();
	}
	LWLockRelease(&dtx_shared->lock);
	return complete;
}

/* A primary's hold on disk: see above. */
static void
keeper_persist(void)
{
	/* first, so that no part the map gains after can precede it */
	TransactionId xmin = GetOldestActiveTransactionId(false, true);
	ReplicationSlot *slot;

	LWLockAcquire(&dtx_shared->lock, LW_EXCLUSIVE);
	xmin = xid_older(xmin, map_oldest());
	slot = slot_make(dtx_shared->held, xmin);
	SpinLockAcquire(&slot->mutex);
	if (!TransactionIdEquals(slot->data.xmin, xmin))
	{
		slot->data.xmin = xmin;
		slot->just_dirtied = true;
		slot->dirty = true;
	}
	SpinLockRelease(&slot->mutex);
	LWLockRelease(&dtx_shared->lock);
}

PGDLLEXPORT void GpDtxKeeperMain(Datum main_arg);

void
GpDtxKeeperMain(Datum main_arg)
{
	MemoryContext cxt;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* No database: only the shared catalogs, for the list of databases. */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	dtx_attach();
	cxt = AllocSetContextCreate(TopMemoryContext, "gp_core dtx keeper",
								ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		bool		complete;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Where a test holds the keeper, which makes the slot again within
		 * a second of its going: port/dtx_horizon drops it, to show what
		 * VACUUM does without it.
		 */
		GP_FAULT("dtx_keeper_round");

		LWLockAcquire(&dtx_shared->lock, LW_SHARED);
		complete = dtx_shared->complete;
		LWLockRelease(&dtx_shared->lock);

		if (RecoveryInProgress())
			keeper_hold_all();
		else if (complete || keeper_complete(cxt))
			keeper_persist();
		MemoryContextReset(cxt);

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L, keeper_wait_event());
		ResetLatch(MyLatch);
	}
}

PGDLLEXPORT void GpDtxLogReaderMain(Datum main_arg);

/* One database's logged parts, into the map; the keeper waits for it. */
void
GpDtxLogReaderMain(Datum main_arg)
{
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(DatumGetObjectId(main_arg),
											  InvalidOid, 0);
	dtx_attach();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	map_load_database();
	PopActiveSnapshot();
	CommitTransactionCommand();
	proc_exit(0);
}

PG_FUNCTION_INFO_V1(gp_dtx_wait_mirror);

/*
 * gp_internal.dtx_wait_mirror()
 *		Wait until this node's mirror has what the node has flushed:
 *		Cloudberry's wait_for_mirror() (xlog.c), which the coordinator runs
 *		when a COMMIT PREPARED it sends again finds the part gone -- committed
 *		by a backend that ended as it waited for the mirror, whose commit the
 *		mirror may not have yet (gp_dispatch.c, dtx_finish_again()).  With
 *		interrupts held, as Cloudberry's are, which SyncRepWaitForLSN()
 *		expects of its caller.
 */
Datum
gp_dtx_wait_mirror(PG_FUNCTION_ARGS)
{
	XLogRecPtr	flushed = GetFlushRecPtr(NULL);

	HOLD_INTERRUPTS();
	SyncRepWaitForLSN(flushed, false);
	RESUME_INTERRUPTS();
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------- */
/* Seeing it                                                                 */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_dtx_recovered);

/*
 * gp.dtx_recovered()
 *		Whether this node's distributed transaction recovery has reached every
 *		node since the server started -- what Cloudberry's pg_ctl waits for
 *		on a coordinator, "DTM recovered", and gpstart polls for here.  True
 *		on a node that runs none: a segment, a standby until its promotion
 *		has ended, and one node that prepares nothing.
 */
Datum
gp_dtx_recovered(PG_FUNCTION_ARGS)
{
	bool		recovered = true;

	if (dtx_recovery_registered && !RecoveryInProgress())
	{
		dtx_attach();
		LWLockAcquire(&dtx_shared->lock, LW_SHARED);
		recovered = dtx_shared->recovered;
		LWLockRelease(&dtx_shared->lock);
	}
	PG_RETURN_BOOL(recovered);
}

PG_FUNCTION_INFO_V1(gp_dtx_recovery_progress);

/*
 * gp_internal.dtx_recovery_progress()
 *		The round the recovery process is running, if one is: its phase and
 *		its counts, gp_stat_progress_dtx_recovery's row -- what Cloudberry
 *		reports of its recovery as a command's progress
 *		(pg_stat_get_progress_info('DTX RECOVERY')), and PostgreSQL 19's
 *		fixed list of progress commands has no place for.
 */
Datum
gp_dtx_recovery_progress(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	dtx_attach();
	LWLockAcquire(&dtx_shared->lock, LW_SHARED);
	if (dtx_shared->progress_active)
	{
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};

		values[0] = Int32GetDatum(dtx_shared->progress_phase);
		for (int i = 0; i < 5; i++)
			values[i + 1] = Int64GetDatum(dtx_shared->progress[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	LWLockRelease(&dtx_shared->lock);
	return (Datum) 0;
}

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
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};

		values[0] = FullTransactionIdGetDatum(e[i].gxid);
		values[1] = TransactionIdGetDatum(e[i].xid);
		values[2] = BoolGetDatum(e[i].done);
		values[3] = BoolGetDatum(e[i].committed);
		values[4] = Int32GetDatum(e[i].nchildren);
		values[5] = BoolGetDatum(e[i].one_phase);
		if (!e[i].done)
			nulls[3] = true;
		if (e[i].nchildren < 0)
			nulls[4] = true;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	LWLockRelease(&dtx_shared->lock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_get_next_gxid);

/*
 * pg_catalog.gp_get_next_gxid()
 *		Cloudberry's next distributed transaction ID (cdbtm.c), a
 *		superuser's to ask, as Cloudberry's.  On the coordinator, the next of
 *		its own transaction IDs: a distributed transaction takes one as its
 *		gxid as it commits, so none it has given is as new, a restart's
 *		recovery having brought the counter back past every one its WAL has.
 *		On a segment, the newest a part here was prepared or committed in
 *		one phase under since the node started, as Cloudberry's segment keeps
 *		the newest it was sent (postgres.c) -- below the coordinator's next
 *		unless the coordinator gave one out twice.
 */
Datum
gp_get_next_gxid(PG_FUNCTION_ARGS)
{
	FullTransactionId next;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("Superuser only to execute it")));

	if (GpClusterContentId() < 0)
		next = ReadNextFullTransactionId();
	else
	{
		dtx_attach();
		LWLockAcquire(&dtx_shared->lock, LW_SHARED);
		next = dtx_shared->newest;
		LWLockRelease(&dtx_shared->lock);
	}
	PG_RETURN_INT64((int64) U64FromFullTransactionId(next));
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

static void
dtx_register_recovery(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 5;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpDtxRecoveryMain");
	dtx_recovery_registered = true;
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core distributed transaction recovery");
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core dtx recovery");
	RegisterBackgroundWorker(&worker);
}

/*
 * A segment's keeper, from the moment a mirror takes read-only connections
 * or a primary has recovered; it never exits of itself, since one that did
 * would not be started again after a crash.
 */
static void
dtx_register_keeper(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 5;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "gp_core");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpDtxKeeperMain");
	snprintf(worker.bgw_name, BGW_MAXLEN, "gp_core distributed transaction keeper");
	snprintf(worker.bgw_type, BGW_MAXLEN, "gp_core dtx keeper");
	RegisterBackgroundWorker(&worker);
}

/* ------------------------------------------------------------------------- */
/* SERIALIZABLE, which a cluster's transactions do not have                  */
/* ------------------------------------------------------------------------- */

/*
 * Each node's serializable snapshot isolation sees the reads and writes of
 * the rows it has, and none of what a distributed transaction does on
 * another: two transactions whose conflict is split over two segments -- a
 * write skew across them -- both commit, so a cluster's transactions are not
 * serializable, whatever each node's are.  Cloudberry, as Greenplum did,
 * falls back to REPEATABLE READ, and says so in the log
 * (check_XactIsoLevel() and check_DefaultXactIsoLevel(), variable.c), and so
 * does the port on every node of a cluster: the two settings' checks,
 * PostgreSQL's first, then this.  One node keeps PostgreSQL's, which is
 * serializable there.
 */
static GucEnumCheckHook prev_check_transaction_isolation = NULL;
static GucEnumCheckHook prev_check_default_isolation = NULL;

static bool
dtx_check_transaction_isolation(int *newval, void **extra, GucSource source)
{
	if (prev_check_transaction_isolation != NULL &&
		!prev_check_transaction_isolation(newval, extra, source))
		return false;
	if (*newval == XACT_SERIALIZABLE)
	{
		elog(LOG, "serializable isolation requested, falling back to "
			 "repeatable read until serializable is supported in Cloudberry");
		*newval = XACT_REPEATABLE_READ;
	}
	return true;
}

static bool
dtx_check_default_isolation(int *newval, void **extra, GucSource source)
{
	if (prev_check_default_isolation != NULL &&
		!prev_check_default_isolation(newval, extra, source))
		return false;
	if (*newval == XACT_SERIALIZABLE)
	{
		elog(LOG, "default serializable isolation requested, falling back to "
			 "repeatable read until serializable is supported in Cloudberry");
		*newval = XACT_REPEATABLE_READ;
	}
	return true;
}

static void
dtx_isolation_install(void)
{
	struct config_generic *conf;

	conf = find_option("transaction_isolation", false, false, ERROR);
	prev_check_transaction_isolation = conf->_enum.check_hook;
	conf->_enum.check_hook = dtx_check_transaction_isolation;

	conf = find_option("default_transaction_isolation", false, false, ERROR);
	prev_check_default_isolation = conf->_enum.check_hook;
	conf->_enum.check_hook = dtx_check_default_isolation;

	/*
	 * The configuration files were read before this module was loaded, and
	 * a backend takes what they said without asking the check again: once
	 * through it here.
	 */
	if (DefaultXactIsoLevel == XACT_SERIALIZABLE)
		SetConfigOption("default_transaction_isolation", "serializable",
						PGC_POSTMASTER, PGC_S_FILE);
}

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

	/*
	 * One node prepares only the loopback's parts, when it may prepare at
	 * all: the gid is reserved, and the recovery process finishes what a
	 * failure left.  Where it may not, the recovery process writes again
	 * the loopback's journalled parts a failure lost.
	 */
	if (GpClusterIsSingleNode())
	{
		if (max_prepared_xacts > 0)
		{
			prev_ProcessUtility = ProcessUtility_hook;
			ProcessUtility_hook = dtx_single_ProcessUtility;
		}
		dtx_register_recovery();
		return;
	}

	/*
	 * Reported, so that the coordinator reads them with each answer; a
	 * cluster's, so that one node sends its clients nothing more.
	 */
	DefineCustomStringVariable(GP_DTX_XID_SETTING,
							   "Transaction ID of this segment's part of a distributed transaction.",
							   "Reported to the coordinator with the answer to each "
							   "statement; empty while the part has written nothing.",
							   &dtx_xid_setting,
							   "",
							   PGC_INTERNAL,
							   GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, NULL);

	DefineCustomStringVariable(GP_DTX_DEPENDS_SETTING,
							   "Coordinator transactions a segment's part waits for before its transaction ends.",
							   "Their parts committed here in one phase, and this part "
							   "may have seen them committed; reported as it commits or "
							   "prepares.",
							   &dtx_depends_setting,
							   "",
							   PGC_INTERNAL,
							   GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, NULL);

	DefineCustomStringVariable(GP_DTX_ONE_PHASE_SETTING,
							   "Coordinator transaction a segment's part commits under, in one phase.",
							   "Set by the coordinator as it commits a part that wrote "
							   "alone.",
							   &dtx_one_phase_setting,
							   "",
							   PGC_USERSET,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							   dtx_one_phase_check, NULL, NULL);

	dtx_isolation_install();

	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = dtx_executor_start;
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = dtx_executor_run;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = dtx_executor_end;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = dtx_ProcessUtility;
	prev_object_access = object_access_hook;
	object_access_hook = dtx_object_access;
	RegisterXactCallback(dtx_xact_callback, NULL);
	RegisterSubXactCallback(dtx_subxact_callback, NULL);

	/*
	 * The coordinator finishes what a failure left prepared; a segment keeps
	 * its map and its slot through a restart and a promotion.
	 */
	self = GpClusterSelf();
	if (self != NULL && self->content == -1)
		dtx_register_recovery();
	else if (self != NULL && self->content >= 0)
		dtx_register_keeper();
}

/*
 * The settings the coordinator syncs a segment's process with, before a
 * statement it sends (gp_dispatch.c): no statement of the client's, which a
 * part's phases show, but the settings the client's statements run under.
 */
static bool
dtx_is_settings_sync(const char *text)
{
	return text != NULL &&
		strncmp(text, GP_SETTINGS_MARKER, strlen(GP_SETTINGS_MARKER)) == 0;
}

/*
 * A fragment's start, on a segment, once it shows the coordinator's
 * statement it is a part of (gp_motion.c): that statement is what the
 * part's PREPARE and second phase go on showing, as Cloudberry's protocol
 * commands leave a QE's activity at the statement it was dispatched.
 */
void
GpDtxNoteStatement(void)
{
	dtx_note_statement();
}

/*
 * Cloudberry's faults of its recovery process's periodic check of orphaned
 * prepared transactions (AbortOrphanedPreparedTransactions(),
 * cdbdtxrecovery.c), the port's periodic rounds: before_orphaned_check as
 * one begins, which a "skip" skips -- whether it was skipped is the answer
 * -- and after_orphaned_check once it is done.  A test skips the rounds to
 * keep the recovery process's connections out of what it holds; the port's
 * are its own, made with libpq, and no gang's.
 */
static bool
recovery_orphan_check(bool after)
{
	if (after)
	{
		(void) GP_FAULT("after_orphaned_check");
		return false;
	}
	return GP_FAULT("before_orphaned_check") == GP_FAULT_SKIP;
}
