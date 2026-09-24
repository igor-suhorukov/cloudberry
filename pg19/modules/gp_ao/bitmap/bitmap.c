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
 * bitmap.c
 *	  The on-disk bitmap index, Cloudberry's "bitmap" access method: its
 *	  interface routines.
 *
 * Portions Copyright (c) 2007-2010 Greenplum Inc
 * Portions Copyright (c) 2010-2012 EMC Corporation
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 * Made of Cloudberry's src/backend/access/bitmap/bitmap.c.  What changed:
 * PostgreSQL 19's amgetbitmap fills a TIDBitmap, where Cloudberry's gave a
 * StreamBitmap its executor ANDed and ORed page by page, so a bitmap scan
 * reads the matching rows' TIDs into it (Track D, 2.4); the handler returns
 * a routine of its own, and the index's WAL goes through gp_ao's second
 * resource manager (bitmapxlog.c).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/index_selfuncs.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "bitmap.h"
#include "bitmap_private.h"

static void bmbuildCallback(Relation index, ItemPointer tupleId, Datum *attdata,
							bool *nulls, bool tupleIsAlive, void *state);
static void cleanup_pos(BMScanPosition pos);

/*
 * bmbuild() -- Build a new bitmap index.
 */
IndexBuildResult *
bmbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	double		reltuples;
	BMBuildState bmstate;
	IndexBuildResult *result;

	if (indexInfo->ii_Concurrent)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY is not supported when creating bitmap indexes")));

	/* We expect this to be called exactly once. */
	if (RelationGetNumberOfBlocks(index) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" already contains data",
						RelationGetRelationName(index))));

	/* initialize the bitmap index for MAIN_FORKNUM. */
	_bitmap_init(index, RelationNeedsWAL(index), false);

	/* initialize the build state. */
	_bitmap_init_buildstate(index, &bmstate);

	/* do the heap scan */
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   bmbuildCallback, (void *) &bmstate,
									   NULL);
	/* clean up the build state */
	_bitmap_cleanup_buildstate(index, &bmstate);

	/* return statistics */
	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = bmstate.ituples;

	return result;
}

/*
 *	bmbuildempty() -- build an empty bitmap index in the initialization fork
 */
void
bmbuildempty(Relation indexrel)
{
	/* initialize meta page and first LOV page for INIT_FORKNUM */
	_bitmap_init(indexrel, true, true);
}

/*
 * bminsert() -- insert an index tuple into a bitmap index.
 */
bool
bminsert(Relation rel, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	_bitmap_doinsert(rel, *ht_ctid, values, isnull);
	return true;
}

/*
 * bmgettuple() -- return the next tuple in a scan.
 */
bool
bmgettuple(IndexScanDesc scan, ScanDirection dir)
{
	BMScanOpaque so = (BMScanOpaque) scan->opaque;
	bool		res;

	/* This implementation of a bitmap index is never lossy */
	scan->xs_recheck = false;

	/*
	 * If we have already begun our scan, continue in the same direction.
	 * Otherwise, start up the scan.
	 */
	if (so->bm_currPos && so->cur_pos_valid)
		res = _bitmap_next(scan, dir);
	else
		res = _bitmap_first(scan, dir);

	return res;
}

/*
 * bmgetbitmap() -- the TIDs of the rows that match, into tbm.
 *
 * Cloudberry's gave a stream of the matching pages, which its executor
 * combined with other indexes' a page at a time; PostgreSQL 19's bitmap
 * scan takes a TIDBitmap, so the vectors are read to the end here, as a
 * plain scan reads them, and their TIDs added in batches.
 */
int64
bmgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	ItemPointerData tids[1024];
	int			n = 0;
	int64		ntids = 0;

	while (bmgettuple(scan, ForwardScanDirection))
	{
		tids[n++] = scan->xs_heaptid;
		if (n == lengthof(tids))
		{
			tbm_add_tuples(tbm, tids, n, false);
			ntids += n;
			n = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (n > 0)
	{
		tbm_add_tuples(tbm, tids, n, false);
		ntids += n;
	}
	return ntids;
}

/*
 * bmbeginscan() -- start a scan on the bitmap index.
 */
IndexScanDesc
bmbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BMScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (BMScanOpaque) palloc(sizeof(BMScanOpaqueData));
	so->bm_currPos = NULL;
	so->bm_markPos = NULL;
	so->cur_pos_valid = false;
	so->mark_pos_valid = false;

	scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	return scan;
}

/*
 * bmrescan() -- restart a scan on the bitmap index.
 */
void
bmrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BMScanOpaque so = (BMScanOpaque) scan->opaque;

	if (so->bm_currPos != NULL)
	{
		cleanup_pos(so->bm_currPos);
		MemSet(so->bm_currPos, 0, sizeof(BMScanPositionData));
		so->cur_pos_valid = false;
	}

	if (so->bm_markPos != NULL)
	{
		cleanup_pos(so->bm_markPos);
		MemSet(so->bm_markPos, 0, sizeof(BMScanPositionData));
		so->cur_pos_valid = false;
	}
	/* reset the scan key */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
}

/*
 * bmendscan() -- close a scan.
 */
void
bmendscan(IndexScanDesc scan)
{
	BMScanOpaque so = (BMScanOpaque) scan->opaque;

	/* free the space */
	if (so->bm_currPos != NULL)
	{
		/*
		 * release the buffers that have been stored for each related bitmap
		 * vector.
		 */
		cleanup_pos(so->bm_currPos);
		pfree(so->bm_currPos);
		so->bm_currPos = NULL;
	}

	if (so->bm_markPos != NULL)
	{
		cleanup_pos(so->bm_markPos);
		pfree(so->bm_markPos);
		so->bm_markPos = NULL;
	}

	pfree(so);
	scan->opaque = NULL;
}

/*
 * bmmarkpos() -- save the current scan position.
 */
void
bmmarkpos(IndexScanDesc scan)
{
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;
	BMVector	bmScanPos;
	uint32 vectorNo;

	/* free the space */
	if (so->mark_pos_valid)
	{
		/*
		 * release the buffers that have been stored for each
		 * related bitmap.
		 */
		bmScanPos = so->bm_markPos->posvecs;

		for (vectorNo=0; vectorNo < so->bm_markPos->nvec; vectorNo++)
		{
			if (BufferIsValid((bmScanPos[vectorNo]).bm_lovBuffer))
			{
				ReleaseBuffer((bmScanPos[vectorNo]).bm_lovBuffer);
				(bmScanPos[vectorNo]).bm_lovBuffer = InvalidBuffer;
			}
		}
		so->mark_pos_valid = false;
	}

	if (so->cur_pos_valid)
	{
		uint32	size = sizeof(BMScanPositionData);


		/* set the mark position */
		if (so->bm_markPos == NULL)
		{
			so->bm_markPos = (BMScanPosition) palloc(size);
		}

		bmScanPos = so->bm_currPos->posvecs;

		for (vectorNo = 0; vectorNo < so->bm_currPos->nvec; vectorNo++)
		{
			if (BufferIsValid((bmScanPos[vectorNo]).bm_lovBuffer))
				IncrBufferRefCount((bmScanPos[vectorNo]).bm_lovBuffer);
		}

		memcpy(so->bm_markPos->posvecs, bmScanPos,
			   so->bm_currPos->nvec *
			   sizeof(BMVectorData));
		memcpy(so->bm_markPos, so->bm_currPos, size);

		so->mark_pos_valid = true;
	}
}

/*
 * bmrestrpos() -- restore a scan to the last saved position.
 */
void
bmrestrpos(IndexScanDesc scan)
{
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	BMVector	bmScanPos;
	uint32 vectorNo;

	/* free space */
	if (so->cur_pos_valid)
	{
		/* release the buffers that have been stored for each related bitmap.*/
		bmScanPos = so->bm_currPos->posvecs;

		for (vectorNo=0; vectorNo<so->bm_markPos->nvec;
			 vectorNo++)
		{
			if (BufferIsValid((bmScanPos[vectorNo]).bm_lovBuffer))
			{
				ReleaseBuffer((bmScanPos[vectorNo]).bm_lovBuffer);
				(bmScanPos[vectorNo]).bm_lovBuffer = InvalidBuffer;
			}
		}
		so->cur_pos_valid = false;
	}

	if (so->mark_pos_valid)
	{
		uint32	size = sizeof(BMScanPositionData);

		/* set the current position */
		if (so->bm_currPos == NULL)
		{
			so->bm_currPos = (BMScanPosition) palloc0(size);
		}

		bmScanPos = so->bm_markPos->posvecs;

		for (vectorNo=0; vectorNo<so->bm_currPos->nvec;
			 vectorNo++)
		{
			if (BufferIsValid((bmScanPos[vectorNo]).bm_lovBuffer))
				IncrBufferRefCount((bmScanPos[vectorNo]).bm_lovBuffer);
		}		

		memcpy(so->bm_currPos->posvecs, bmScanPos,
			   so->bm_markPos->nvec *
			   sizeof(BMVectorData));
		memcpy(so->bm_currPos, so->bm_markPos, size);
		so->cur_pos_valid = true;
	}
}

/*
 * bmbulkdelete() -- bulk delete index entries
 *
 * The index is built again, as Cloudberry's reindexes it, before retrieving
 * the number of tuples indexed in it: in place, a new relfilenode and a
 * build, since REINDEX refuses an index VACUUM has open.  The table is held
 * in ShareLock and the index in AccessExclusiveLock meanwhile, as REINDEX
 * holds them.  A lazy VACUUM hides its transaction from others' snapshots,
 * which one that writes the catalog, as this does, may not: Cloudberry never
 * hides one for that reason, and this clears the flag before the catalog is
 * written (see ao_vacuum.c).
 */
IndexBulkDeleteResult *
bmbulkdelete(IndexVacuumInfo *info,
			 IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback,
			 void *callback_state)
{
	Relation	rel = info->index;
	Relation	heap = info->heaprel;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)
			palloc0(sizeof(IndexBulkDeleteResult));

	if (MyProc->statusFlags & PROC_IN_VACUUM)
	{
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags &= ~PROC_IN_VACUUM;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	LockRelationOid(RelationGetRelid(heap), ShareLock);
	LockRelationOid(RelationGetRelid(rel), AccessExclusiveLock);
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	CommandCounterIncrement();
	index_build(heap, rel, BuildIndexInfo(rel), true, false, false);

	CommandCounterIncrement();

	stats->num_pages = RelationGetNumberOfBlocks(rel);
	/* Since we re-build the index, set this to number of heap tuples. */
	stats->num_index_tuples = info->num_heap_tuples;
	stats->tuples_removed = 0;

	return stats;
}

/*
 * bmvacuumcleanup() -- post-vacuum cleanup.
 *
 * We do nothing useful here.
 */
IndexBulkDeleteResult *
bmvacuumcleanup(IndexVacuumInfo *info,
				IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* update statistics */
	stats->num_pages = RelationGetNumberOfBlocks(rel);
	stats->pages_deleted = 0;
	stats->pages_free = 0;
	/* XXX: dodgy hack to shutup index_scan() and vacuum_index() */
	stats->num_index_tuples = info->num_heap_tuples;

	return stats;
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
bmbuildCallback(Relation index, ItemPointer tupleId, Datum *attdata,
				bool *nulls, bool tupleIsAlive pg_attribute_unused(), void *state)
{
	BMBuildState *bstate = (BMBuildState *) state;

	_bitmap_buildinsert(index, *tupleId, attdata, nulls, bstate);
	bstate->ituples += 1;

	if (((int) bstate->ituples) % 1000 == 0)
		CHECK_FOR_INTERRUPTS();
}

static void
cleanup_pos(BMScanPosition pos) 
{
	if (pos->nvec == 0)
		return;
	
	/*
	 * Only cleanup bm_batchWords if we have more than one vector since
	 * _bitmap_cleanup_scanpos() will clean it up for the single vector
	 * case.
	 */
	if (pos->nvec > 1)
	{
		_bitmap_cleanup_batchwords(pos->bm_batchWords);
		if (pos->bm_batchWords != NULL)
			pfree(pos->bm_batchWords);
	}
	_bitmap_cleanup_scanpos(pos->posvecs, pos->nvec);
}


/*
 * GetBitmapIndexAuxOids - Given an open index, fetch and return the oids for
 * the bitmap subobjects (pg_bm_xxxx + pg_bm_xxxx_index).
 *
 * Note: Currently this information is not stored directly in the catalog, but
 * is hidden away inside the metadata page of the index.  Future versions should
 * move this information into the catalog.
 */
void 
GetBitmapIndexAuxOids(Relation index, Oid *heapId, Oid *indexId)
{
	Buffer     metabuf;
	BMMetaPage metapage;
	

	/* Only Bitmap Indexes have bitmap related sub-objects */
	if (!RelationIsBitmapIndex(index))
	{
		*heapId = InvalidOid;
		*indexId = InvalidOid;
		return;
	}
	
	metabuf = _bitmap_getbuf(index, BM_METAPAGE, BM_READ);
	metapage = _bitmap_get_metapage_data(index, metabuf);

	*heapId  = metapage->bm_lov_heapId;
	*indexId = metapage->bm_lov_indexId;

	_bitmap_relbuf(metabuf);
}

/*
 * bmcostestimate() -- Cloudberry's: PostgreSQL's generic index costs, which
 * it argues fit a bitmap index's vectors well enough (selfuncs.c).
 */
void
bmcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

/* Is rel a bitmap index? */
bool
RelationIsBitmapIndex(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX && rel->rd_indam != NULL &&
		rel->rd_indam->ambuild == bmbuild;
}

bytea *
bmoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(StdRdOptions, fillfactor)},
	};

	return (bytea *) build_reloptions(reloptions, validate, bm_relopt_kind,
									  sizeof(StdRdOptions), tab, lengthof(tab));
}

/*
 * Ask appropriate access method to validate the specified opclass.
 *
 * Bitmap indexes use the same opclass support functions and strategies as
 * B-tree indexes -- the list of values is a real B-tree -- so B-tree's
 * validation serves, as Cloudberry's btree_or_bitmap_validate() did.
 */
bool
bmvalidate(Oid opclassoid)
{
	return btvalidate(opclassoid);
}

/*
 * Bitmap index handler function: return IndexAmRoutine with access method
 * parameters and callbacks.  These are mostly the same as B-tree's.
 */
PG_FUNCTION_INFO_V1(bmhandler);

Datum
bmhandler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = BTMaxStrategyNumber,
		.amsupport = BTNProcs,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = true,
		.amconsistentordering = true,
		.amcanbackward = false,
		.amcanunique = true,
		.amcanmulticol = true,
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = 0,
		.amkeytype = InvalidOid,

		.ambuild = bmbuild,
		.ambuildempty = bmbuildempty,
		.aminsert = bminsert,
		.aminsertcleanup = NULL,
		.ambulkdelete = bmbulkdelete,
		.amvacuumcleanup = bmvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = bmcostestimate,
		.amgettreeheight = NULL,
		.amoptions = bmoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = bmvalidate,
		.amadjustmembers = btadjustmembers,
		.ambeginscan = bmbeginscan,
		.amrescan = bmrescan,
		.amgettuple = bmgettuple,
		.amgetbitmap = bmgetbitmap,
		.amendscan = bmendscan,
		.ammarkpos = bmmarkpos,
		.amrestrpos = bmrestrpos,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		.amtranslatestrategy = bttranslatestrategy,
		.amtranslatecmptype = bttranslatecmptype,
	};

	PG_RETURN_POINTER(&amroutine);
}

/* The index's fillfactor, its only option, as Cloudberry's has it. */
relopt_kind bm_relopt_kind;

void
bm_init(void)
{
	bm_relopt_kind = add_reloption_kind();
	add_int_reloption(bm_relopt_kind, "fillfactor",
					  "Packs bitmap index pages only to this percentage",
					  BITMAP_DEFAULT_FILLFACTOR, BITMAP_MIN_FILLFACTOR, 100,
					  ShareUpdateExclusiveLock);
	bitmap_register_rmgr();
}
