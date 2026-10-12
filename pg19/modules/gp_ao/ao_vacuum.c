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
 * ao_vacuum.c
 *	  VACUUM of an append-optimized table: segment files compacted, and
 *	  recycled once no snapshot can see what they held.
 *
 * As Cloudberry's vacuum_ao.c does it, in its three phases.  A segment file
 * whose deleted rows are gp.appendonly_compaction_threshold percent of it or
 * more is compacted: its live rows are appended to another, with index
 * entries of their own, and it is marked awaiting drop, by this transaction.
 * A snapshot older than that still reads it, through the old entries, so
 * only once the compacting transaction is older than every snapshot is it
 * recycled: its index entries removed, its rows in gp_ao's tables deleted,
 * and it made empty for the next writer, who writes over its extents.  The
 * recycling runs as a VACUUM begins, for the compactions of earlier ones,
 * and again in a transaction of its own as a VACUUM statement ends
 * (gp_ao.c), for its own, as Cloudberry's post-cleanup phase does.
 *
 * On a segment, every snapshot is also every distributed one.  The
 * compacting transaction is the segment's own, which a snapshot taken here
 * afterwards sees committed -- one a statement arrives with later too, whose
 * distributed snapshot was taken on the coordinator before the DELETE that
 * left the file worth compacting had ended there.  That statement reads the
 * table as it was before the DELETE, the deleted rows still in the file; if
 * it saw the compaction as well, it would read the rows that were moved a
 * second time where they went, and after a recycling would not find the
 * file's rows at all.  So the compacting transaction follows the parts that
 * have committed here (gp_core's GpDtxFollowCommitted()): a distributed
 * snapshot that hides one of them hides it, and the file is not recycled
 * while any snapshot in use may (GpDtxIsFollowed()) -- which a VACUUM
 * knows afresh, the coordinator telling every segment as it sends one what
 * no snapshot of its can see in progress any more.  The file stays awaiting
 * drop meanwhile, as Cloudberry's does while a distributed snapshot is older
 * than its compaction.
 *
 * A VACUUM compacts no file once fewer of the table's 127 than
 * gp.appendonly_compaction_segfile_limit would be left for inserts -- those
 * the table does not have yet, and those it has that are in use and that
 * this VACUUM neither compacted nor moved rows into -- as Cloudberry's
 * stops (ChooseSegnoForCompaction()): a file compacted awaits its drop for
 * as long as an older snapshot is held, and a table all of whose files did
 * would take no insert.  And it leaves a file there is no other to move the
 * rows of into, with Cloudberry's warning.
 *
 * A VACUUM changes no segment file's modcount, Cloudberry's count of the
 * statements that changed a file's rows, by which an incremental backup
 * tells a table that changed: neither the file compacted, nor the file its
 * rows went to, nor the file recycled.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "gp_ao.h"
#include "gp_dtx.h"

/* The tables this backend's VACUUM compacted, for its last phase. */
static List *ao_compacted = NIL;

typedef struct AoRecycleState
{
	bool		recycle[AO_MAX_SEGNO + 1];
	int64		boundary[AO_MAX_SEGNO + 1];		/* rows below it are gone */
} AoRecycleState;

/* Is an index entry's row one of a segment file being recycled? */
static bool
ao_recycle_callback(ItemPointer itemptr, void *state)
{
	AoRecycleState *rs = (AoRecycleState *) state;
	int			segno = AoTidSegno(itemptr);

	return segno >= 1 && segno <= AO_MAX_SEGNO && rs->recycle[segno] &&
		AoTidRownum(itemptr) < rs->boundary[segno];
}

/* Remove from every index of rel the entries callback says are gone. */
static void
ao_vacuum_indexes(Relation rel, IndexBulkDeleteCallback callback, void *state,
				  double num_tuples, BufferAccessStrategy bstrategy,
				  bool update_stats)
{
	Relation   *indrels;
	int			nindexes;

	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {0};
		IndexBulkDeleteResult *stats = NULL;

		ivinfo.index = indrels[i];
		ivinfo.heaprel = rel;
		ivinfo.message_level = DEBUG2;
		ivinfo.num_heap_tuples = num_tuples;
		ivinfo.estimated_count = false;
		ivinfo.strategy = bstrategy;
		if (callback)
			stats = index_bulk_delete(&ivinfo, stats, callback, state);

		/*
		 * BRIN's cleanup summarizes the ranges that have no summary, and is
		 * not to wait for a transaction that writes the table: the files
		 * such a one holds are left to a later VACUUM (ao_am.c).
		 */
		ao_brin_leave_written(true);
		PG_TRY();
		{
			stats = index_vacuum_cleanup(&ivinfo, stats);
		}
		PG_FINALLY();
		{
			ao_brin_leave_written(false);
		}
		PG_END_TRY();
		if (stats && update_stats && !stats->estimated_count)
			vac_update_relstats(indrels[i], stats->num_pages,
								stats->num_index_tuples, 0, 0, false,
								InvalidTransactionId, InvalidMultiXactId,
								NULL, NULL, false);
	}
	vac_close_indexes(nindexes, indrels, NoLock);
}

/*
 * Might a snapshot still see the compacting transaction xid running?  What
 * one that did would read is the file's rows, through their old index
 * entries; one that sees it committed passes the file by.  So what counts is
 * the snapshots running -- a backend whose xmin is xid or older -- and not
 * the horizon that also holds back what committed transactions deleted, for
 * snapshots to come: on a segment gp_core holds that one back to the oldest
 * part of a distributed transaction a distributed snapshot may not see yet
 * (gp_dtx.c), and a VACUUM's compaction is never such a part.  A backend
 * that takes its snapshot later sees xid committed.
 */
static bool
compaction_may_be_seen_running(TransactionId xid)
{
	int			n;

	if (TransactionIdIsInProgress(xid))
		return true;
	pfree(GetCurrentVirtualXIDs(xid, true, false, 0, &n));
	return n > 0;
}

/*
 * The rows of the table's segment files that are in use, as the latest
 * snapshot sees them: those not deleted, and into *dead those that are.
 */
static double
ao_live_tuples(int64 storage_id, double *dead)
{
	AoSegfile  *segfiles;
	int			nsegfiles;
	double		live = 0;

	*dead = 0;
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &nsegfiles);
	for (int i = 0; i < nsegfiles; i++)
	{
		AoSegfile  *sf = &segfiles[i];
		double		deleted;

		if (sf->state != AO_SEGFILE_DEFAULT)
			continue;
		deleted = ao_visimap_count(ao_visimap_load(storage_id, sf->segno,
												   GetLatestSnapshot()));
		live += sf->tupcount - deleted;
		*dead += deleted;
	}
	return live;
}

/*
 * Recycle the segment files of rel a VACUUM compacted, whose compacting
 * transaction every snapshot now sees committed: none here that began
 * before it ended, and on a segment no distributed one that hides it with a
 * part it followed.  Returns how many.
 */
static int
ao_recycle(Relation rel, int64 storage_id, BufferAccessStrategy bstrategy)
{
	AoSegfile  *segfiles;
	int			nsegfiles;
	AoRecycleState rs = {0};
	int			n = 0;
	double		dead;

	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &nsegfiles);
	for (int i = 0; i < nsegfiles; i++)
	{
		AoSegfile  *sf = &segfiles[i];

		if (sf->state != AO_SEGFILE_AWAITING_DROP ||
			!TransactionIdIsValid(sf->compacted_by) ||
			compaction_may_be_seen_running(sf->compacted_by) ||
			GpDtxIsFollowed(sf->compacted_by) ||
			!ao_segfile_try_lock(RelationGetRelid(rel), sf->segno))
			continue;
		rs.recycle[sf->segno] = true;
		rs.boundary[sf->segno] = ao_next_rownum(rel, sf->segno);
		n++;
		elog(AO_PRINT_LEVEL(gp_debug_appendonly_print_compaction),
			 "recycling segment file %d of \"%s\", compacted by transaction %u",
			 sf->segno, RelationGetRelationName(rel), sf->compacted_by);
	}
	if (n == 0)
		return 0;

	/*
	 * The entries of the files' rows go, and each index is left with those
	 * of the rows in use, which it then says it has: Cloudberry's VACUUM
	 * counts an index's entries again where it dropped a file, and only
	 * there (uao/vacuum_index_stats).
	 */
	ao_vacuum_indexes(rel, ao_recycle_callback, &rs,
					  ao_live_tuples(storage_id, &dead), bstrategy, true);
	(void) AO_FAULT("vacuum_ao_after_index_delete", rel);

	for (int i = 0; i < nsegfiles; i++)
	{
		AoSegfile  *sf;

		if (!rs.recycle[segfiles[i].segno])
			continue;
		sf = ao_segfile_read(storage_id, segfiles[i].segno, SnapshotSelf);
		if (sf == NULL)
			continue;
		ao_blkdir_delete_segfile(storage_id, sf->segno);
		ao_visimap_delete_segfile(storage_id, sf->segno);
		for (int g = 0; g < sf->ngroups; g++)
			sf->eof[g] = sf->eof_uncompressed[g] = 0;
		sf->tupcount = 0;
		sf->varblockcount = 0;
		sf->state = AO_SEGFILE_DEFAULT;
		sf->compacted_by = InvalidTransactionId;
		ao_segfile_update(storage_id, sf);
		(void) AO_FAULT("appendonly_after_truncate_segment_file", rel);
	}
	CommandCounterIncrement();
	return n;
}

/* Insert into every index of rel the entry of the row in slot. */
static void
ao_index_moved_row(Relation *indrels, IndexInfo **indinfos, ExprState **preds,
				   int nindexes, Relation rel, TupleTableSlot *slot,
				   EState *estate)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
	for (int i = 0; i < nindexes; i++)
	{
		IndexInfo  *ii = indinfos[i];

		if (preds[i] != NULL && !ExecQual(preds[i], GetPerTupleExprContext(estate)))
			continue;
		FormIndexDatum(ii, slot, estate, values, isnull);
		/* The row is the one its old entry names, moved: no duplicate. */
		index_insert(indrels[i], values, isnull, &slot->tts_tid, rel,
					 UNIQUE_CHECK_NO, false, ii);
	}
	ResetPerTupleExprContext(estate);
}

/*
 * Compact segment file sf of rel: its live rows appended elsewhere, with
 * index entries of their own, and it marked awaiting drop.  Returns the
 * segment file they went to, or 0 where there was none to take and sf is
 * left as it was.
 */
static int
ao_compact(Relation rel, int64 storage_id, AoSegfile *sf)
{
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	AoSegfile  *mine;
	TableScanDesc scan;
	TupleTableSlot *src;
	TupleTableSlot *dst;
	AoInsertState *st;
	Relation   *indrels;
	IndexInfo **indinfos;
	ExprState **preds;
	int			nindexes;
	EState	   *estate;
	int64		moved = 0;
	int			target;

	elog(AO_PRINT_LEVEL(gp_debug_appendonly_print_compaction),
		 "compacting segment file %d of \"%s\": " INT64_FORMAT " rows",
		 sf->segno, RelationGetRelationName(rel), sf->tupcount);

	/*
	 * The file its rows go to, any but itself, taken before any is found to
	 * move, as Cloudberry's is (AppendOnlyCompact()): a file whose rows are
	 * all deleted leaves an empty one beside it.  The one an earlier file of
	 * this VACUUM went to, where there is one.
	 */
	ao_dml_set_compaction_writer(true, sf->segno);
	PG_TRY();
	{
		st = ao_insert_state(rel);
	}
	PG_FINALLY();
	{
		ao_dml_set_compaction_writer(false, 0);
	}
	PG_END_TRY();
	if (st == NULL)
	{
		UnregisterSnapshot(snapshot);
		ereport(WARNING,
				(errmsg("could not find a free segment file to use for compacting segfile %d of relation %s",
						sf->segno, RelationGetRelationName(rel))));
		return 0;
	}
	target = ao_insert_segno(st);

	/*
	 * Awaiting drop from here on -- to every snapshot but the one its rows
	 * are read with below, taken before.
	 */
	mine = ao_segfile_read(storage_id, sf->segno, SnapshotSelf);
	mine->state = AO_SEGFILE_AWAITING_DROP;
	mine->compacted_by = GetCurrentTransactionId();
	ao_segfile_update(storage_id, mine);
	CommandCounterIncrement();

	estate = CreateExecutorState();
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	indinfos = palloc_array(IndexInfo *, Max(nindexes, 1));
	preds = palloc_array(ExprState *, Max(nindexes, 1));
	for (int i = 0; i < nindexes; i++)
	{
		indinfos[i] = BuildIndexInfo(indrels[i]);
		preds[i] = indinfos[i]->ii_Predicate != NIL ?
			ExecPrepareQual(indinfos[i]->ii_Predicate, estate) : NULL;
	}

	src = table_slot_create(rel, NULL);
	dst = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	ao_scan_set_segno(scan, sf->segno);
	while (table_scan_getnextslot(scan, ForwardScanDirection, src))
	{
		CHECK_FOR_INTERRUPTS();
		ExecCopySlot(dst, src);
		ao_insert_slot(st, rel, dst);
		ao_index_moved_row(indrels, indinfos, preds, nindexes, rel, dst,
						   estate);
		moved++;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(src);
	ExecDropSingleTupleTableSlot(dst);
	/* What an index keeps between inserts -- BRIN's revmap, pinned -- goes. */
	for (int i = 0; i < nindexes; i++)
		index_insert_cleanup(indrels[i], indinfos[i]);
	vac_close_indexes(nindexes, indrels, NoLock);
	FreeExecutorState(estate);
	UnregisterSnapshot(snapshot);

	ao_dml_flush(RelationGetRelid(rel));
	(void) AO_FAULT("vacuum_ao_after_compact", rel);
	elog(gp_debug_appendonly_print_compaction ? LOG : DEBUG1,
		 "compacted segment file %d of \"%s\": " INT64_FORMAT " rows moved to segment file %d",
		 sf->segno, RelationGetRelationName(rel), moved, target);

	if (!list_member_oid(ao_compacted, RelationGetRelid(rel)))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		ao_compacted = lappend_oid(ao_compacted, RelationGetRelid(rel));
		MemoryContextSwitchTo(old);
	}
	return target;
}

/*
 * How many of the table's segment files are for inserts, were this VACUUM
 * to go on: those the table has not, and those it has that are in use, but
 * for the ones this VACUUM compacted or moved rows into ("touched").
 */
static int
ao_files_for_inserts(const AoSegfile *segfiles, int nsegfiles,
					 const bool *touched)
{
	bool		unavailable[AO_MAX_SEGNO + 1] = {0};
	int			n = 0;

	for (int i = 0; i < nsegfiles; i++)
		if (segfiles[i].state != AO_SEGFILE_DEFAULT)
			unavailable[segfiles[i].segno] = true;
	for (int segno = 1; segno <= AO_MAX_SEGNO; segno++)
		if (!unavailable[segno] && !touched[segno])
			n++;
	return n;
}

/*
 * A lazy VACUUM runs with PROC_IN_VACUUM set, which hides its transaction
 * from every other snapshot: it is meant never to write rows, and one that
 * did would have them taken for an aborted transaction's by any reader.
 * This one writes gp_ao's tables, as Cloudberry's does its aoseg relations,
 * so the flag is cleared before its transaction takes an ID, as Cloudberry
 * never sets it for an append-optimized table.  Its snapshots are taken
 * afresh from here on, so none of them needs a row another VACUUM removed
 * while this one was hidden.
 */
static void
ao_vacuum_unhide(void)
{
	if ((MyProc->statusFlags & PROC_IN_VACUUM) == 0)
		return;
	Assert(!TransactionIdIsValid(GetTopTransactionIdIfAny()));
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags &= ~PROC_IN_VACUUM;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}

void
ao_vacuum_rel(Relation rel, const VacuumParams *params,
			  BufferAccessStrategy bstrategy)
{
	int64		storage_id;
	AoSegfile  *segfiles;
	int			nsegfiles;
	double		livetuples;
	double		deadtuples;
	bool		compacted = false;
	bool		touched[AO_MAX_SEGNO + 1] = {0};
	TimestampTz starttime = GetCurrentTimestamp();

	ao_vacuum_unhide();
	storage_id = ao_storage_id(rel);

	/* 1. What earlier VACUUMs compacted, and no snapshot sees any more. */
	(void) ao_recycle(rel, storage_id, bstrategy);

	/* 2. Compaction, unless gp.appendonly_compaction says none. */
	segfiles = ao_segfiles_read(storage_id, GetLatestSnapshot(), &nsegfiles);
	for (int i = 0; gp_appendonly_compaction && i < nsegfiles; i++)
	{
		AoSegfile  *sf = &segfiles[i];
		AoVisimap  *vm;
		int64		deleted;
		int			available;
		int			target;

		/* Not the file an earlier one's rows went to, either. */
		if (sf->state != AO_SEGFILE_DEFAULT || sf->tupcount == 0 ||
			touched[sf->segno])
			continue;
		vm = ao_visimap_load(storage_id, sf->segno, GetLatestSnapshot());
		deleted = ao_visimap_count(vm);
		if (deleted == 0 ||
			deleted * 100 < (int64) gp_appendonly_compaction_threshold * sf->tupcount)
			continue;
		available = ao_files_for_inserts(segfiles, nsegfiles, touched);
		if (available < gp_appendonly_compaction_segfile_limit)
		{
			ereport(AO_PRINT_LEVEL(gp_debug_appendonly_print_segfile_choice),
					(errmsg("number of available segfiles for inserts is below gp.appendonly_compaction_segfile_limit"),
					 errdetail("available segfile count = %d, limit = %d",
							   available, gp_appendonly_compaction_segfile_limit)));
			break;
		}
		if (!ao_segfile_try_lock(RelationGetRelid(rel), sf->segno))
			continue;
		target = ao_compact(rel, storage_id, sf);
		if (target == 0)
			continue;
		touched[sf->segno] = touched[target] = true;
		compacted = true;
	}

	/*
	 * What it compacted, a distributed snapshot that reads the table as it
	 * was before a part that has committed here must not see.
	 */
	if (compacted)
		GpDtxFollowCommitted();

	/* The statistics, of what is left. */
	livetuples = ao_live_tuples(storage_id, &deadtuples);

	ao_vacuum_indexes(rel, NULL, NULL, livetuples, bstrategy, true);
	vac_update_relstats(rel, RelationGetNumberOfBlocks(rel), livetuples, 0, 0,
						rel->rd_rel->relhasindex, InvalidTransactionId,
						InvalidMultiXactId, NULL, NULL, false);
	pgstat_report_vacuum(rel, (PgStat_Counter) livetuples,
						 (PgStat_Counter) deadtuples, starttime);
}

/* The tables compacted since the last call, for the VACUUM that ends. */
List *
ao_vacuum_take_compacted(void)
{
	List	   *result = ao_compacted;

	ao_compacted = NIL;
	return result;
}

/*
 * As a VACUUM statement ends, in a transaction of its own: the segment files
 * it compacted, recycled if no snapshot sees them any more.
 */
void
ao_vacuum_recycle_rel(Oid relid)
{
	Relation	rel;

	/*
	 * Cloudberry's faults of its post-cleanup phase, as vacuum_rel() begins
	 * it and once it has the table: here on the segment that compacted,
	 * where Cloudberry's coordinator, which sends each phase, has the first
	 * too.
	 */
	(void) GP_FAULT("vacuum_relation_open_relation_during_drop_phase");
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = try_relation_open(relid, ShareUpdateExclusiveLock);
	if (rel != NULL)
	{
		if (ao_is_ao_table(rel))
		{
			(void) AO_FAULT("compaction_before_cleanup_phase", rel);
			(void) ao_recycle(rel, ao_storage_id(rel), NULL);
			(void) AO_FAULT("vacuum_ao_post_cleanup_end", rel);
		}
		relation_close(rel, ShareUpdateExclusiveLock);
	}
	PopActiveSnapshot();
}
