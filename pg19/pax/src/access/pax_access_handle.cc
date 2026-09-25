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
 * pax_access_handle.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_access_handle.cc
 *
 * Ported to PostgreSQL 19:
 *   - the callbacks take PostgreSQL 19's arguments, and Cloudberry's own
 *     members of TableAmRoutine are gone: what PAX needs of them is the
 *     TableAmExtRoutine the module registers (O13) -- its options (O14), the
 *     columns a scan reads (O15, where Cloudberry began the scan with the
 *     plan node), a unique index's probe (O16), its size (O19) and UPDATE's
 *     old row from the plan (O20);
 *   - a row is fetched by its TID, as gp_core's split update fetches the old
 *     row, where Cloudberry's Split took it from the plan;
 *   - a TID crosses the method's boundary translated between PAX's layout,
 *     which its own code keeps, and the table's (pax_tid.h);
 *   - Cloudberry's executor called dml_init and dml_fini, and PostgreSQL 19's
 *     does not: a writer is made as a statement first writes a table, and
 *     finished as the query or the utility statement that made it finishes,
 *     before its AFTER triggers, or before a commit (access/pax_dml_state.cc);
 *   - ANALYZE samples through the module's hook (modules/pax/pax.c), the
 *     table's blocks being no pages ANALYZE could read;
 *   - the object access hook keeps a table's catalog with its storage through
 *     a rewrite, and deletes it as the table is dropped, where Cloudberry had
 *     its swap_relation_files callback and custom object classes
 *     (catalog/pg_pax_tables.cc);
 *   - CLUSTER, a RepackStmt in PostgreSQL 19, of a table clustered by its
 *     cluster_columns is PAX's on each node, sent to the segments through
 *     gp_core; there are no DFS tablespaces to refuse;
 *   - the module's magic block and _PG_init are modules/pax/pax.c's, which
 *     calls pax_init() here.
 *
 *-------------------------------------------------------------------------
 */

#include "access/pax_access_handle.h"

#include "comm/cbdb_api.h"

#include "access/pax_dml_state.h"
#include "access/pax_scanner.h"
#include "access/pax_table_cluster.h"
#include "access/pax_updater.h"
#include "access/paxc_rel_options.h"
#include "catalog/pax_catalog.h"
#include "clustering/zorder_utils.h"
#include "comm/guc.h"
#include "comm/pax_memory.h"
#include "comm/pax_resource.h"
#include "comm/paxc_wrappers.h"
#include "comm/vec_numeric.h"
#include "exceptions/CException.h"
#include "storage/local_file_system.h"
#include "storage/paxc_smgr.h"
#include "storage/wal/pax_wal.h"
#include "storage/wal/paxc_wal.h"
#include "pax_module.h"
#include "pax_tid.h"

extern "C" {
#include "gp_dispatch.h"
#include "gp_dtx.h"
}

#define NOT_IMPLEMENTED_YET                        \
  ereport(ERROR,                                   \
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
           errmsg("not implemented yet on pax relations: %s", __func__)))

#define NOT_SUPPORTED_YET                                 \
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
                  errmsg("not supported on pax relations: %s", __func__)))

#define RELATION_IS_PAX(rel) \
  (OidIsValid((rel)->rd_rel->relam) && RelationIsPAX(rel))

// access methods that are implemented in C++
namespace pax {

// A row PAX names, as the table gives it out.
static inline void PaxSlotTidToTable(Relation rel, TupleTableSlot *slot) {
  slot->tts_tid = PaxTidToTable(slot->tts_tid, cbdb::PaxTableFileBits(rel));
}

TableScanDesc CCPaxAccessMethod::ScanBegin(Relation relation, Snapshot snapshot,
                                           int nkeys, struct ScanKeyData *key,
                                           ParallelTableScanDesc pscan,
                                           uint32 flags) {
  CBDB_TRY();
  {
    auto desc = PAX_NEW<PaxScanDesc>();
    pax::common::RememberResourceCallback(pax::ReleaseTopObject<PaxScanDesc>,
                                          cbdb::PointerToDatum(desc));

    return desc->BeginScan(relation, snapshot, nkeys, key, pscan, flags,
                           nullptr, true);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_END_TRY();

  pg_unreachable();
}

void CCPaxAccessMethod::ScanEnd(TableScanDesc scan) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    desc->EndScan();

    pax::common::ForgetResourceCallback(pax::ReleaseTopObject<PaxScanDesc>,
                                        cbdb::PointerToDatum(desc));
    PAX_DELETE<PaxScanDesc>(desc);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

void CCPaxAccessMethod::ScanExtractColumns(TableScanDesc scan,
                                           struct PlanState *ps) {
  CBDB_TRY();
  { PaxScanDesc::ToDesc(scan)->ExtractColumns(ps); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

bool CCPaxAccessMethod::IndexUniqueCheck(Relation rel, ItemPointer tid,
                                         Snapshot snapshot, bool *all_dead) {
  ItemPointerData internal;

  CBDB_TRY();
  {
    internal = PaxTidFromTable(*tid, cbdb::PaxTableFileBits(rel));

    auto dsl = pax::CPaxDmlStateLocal::Instance();
    if (dsl->IsInitialized()) {
      auto del = dsl->GetDeleter(rel, snapshot, true);

      if (del && del->IsMarked(internal)) return false;
    }
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  return paxc::IndexUniqueCheck(rel, &internal, snapshot, all_dead);
}

struct IndexFetchTableData *CCPaxAccessMethod::IndexFetchBegin(
    Relation rel, uint32 /*flags*/) {
  CBDB_TRY();
  {
    Assert(RELATION_IS_PAX(rel));
    auto desc = PAX_NEW<PaxIndexScanDesc>(rel);
    pax::common::RememberResourceCallback(
        pax::ReleaseTopObject<PaxIndexScanDesc>, PointerGetDatum(desc));
    return desc->ToBase();
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  return nullptr;  // keep compiler quiet
}

void CCPaxAccessMethod::IndexFetchEnd(IndexFetchTableData *scan) {
  CBDB_TRY();
  {
    auto desc = PaxIndexScanDesc::FromBase(scan);
    desc->Release();

    pax::common::ForgetResourceCallback(pax::ReleaseTopObject<PaxIndexScanDesc>,
                                        PointerGetDatum(desc));
    pax::PAX_DELETE(desc);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

bool CCPaxAccessMethod::IndexFetchTuple(struct IndexFetchTableData *scan,
                                        ItemPointer tid, Snapshot snapshot,
                                        TupleTableSlot *slot, bool *call_again,
                                        bool *all_dead) {
  CBDB_TRY();
  {
    auto desc = PaxIndexScanDesc::FromBase(scan);
    ItemPointerData internal =
        PaxTidFromTable(*tid, cbdb::PaxTableFileBits(desc->GetRelation()));

    if (call_again) *call_again = false;
    if (all_dead) *all_dead = false;
    if (desc->FetchTuple(&internal, snapshot, slot, call_again, all_dead)) {
      slot->tts_tid = *tid;
      return true;
    }
    return false;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  return false;  // keep compiler quiet
}

void CCPaxAccessMethod::IndexFetchReset(IndexFetchTableData * /*scan*/) {}

void CCPaxAccessMethod::ScanRescan(TableScanDesc scan, ScanKey key,
                                   bool set_params, bool allow_strat,
                                   bool allow_sync, bool allow_pagemode) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    desc->ReScan(key, set_params, allow_strat, allow_sync, allow_pagemode);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

bool CCPaxAccessMethod::ScanGetNextSlot(TableScanDesc scan,
                                        ScanDirection /*direction*/,
                                        TupleTableSlot *slot) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    bool result;

    result = desc->GetNextSlot(slot);
    if (result) {
      PaxSlotTidToTable(desc->GetRelation(), slot);
      pgstat_count_heap_getnext(desc->GetRelation());
    }
    return result;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  pg_unreachable();
}

void CCPaxAccessMethod::TupleInsert(Relation relation, TupleTableSlot *slot,
                                    CommandId cid, uint32 options,
                                    BulkInsertState bistate) {
  CBDB_TRY();
  {
    MemoryContext old_ctx;

#ifdef FAULT_INJECTOR
    FaultInjector_InjectFaultIfSet("pax_insert", DDLNotSpecified, "",
                                   RelationGetRelationName(relation));
#endif

    // the writer, made here if this is the statement's first row, comes
    // with the memory context it writes in
    auto inserter = CPaxDmlStateLocal::Instance()->GetInserter(relation);
    Assert(cbdb::pax_memory_context);

    old_ctx = MemoryContextSwitchTo(cbdb::pax_memory_context);
    inserter->InsertTuple(relation, slot, cid, (int)options, bistate);
    MemoryContextSwitchTo(old_ctx);

    PaxSlotTidToTable(relation, slot);
    pgstat_count_heap_insert(relation, 1);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({
      // FIXME: destroy CPaxInserter?
  });
  CBDB_END_TRY();
}

TM_Result CCPaxAccessMethod::TupleDelete(Relation relation, ItemPointer tid,
                                         CommandId cid, uint32 /*options*/,
                                         Snapshot snapshot,
                                         Snapshot /*crosscheck*/, bool /*wait*/,
                                         TM_FailureData *tmfd) {
  CBDB_TRY();
  {
    ItemPointerData internal =
        PaxTidFromTable(*tid, cbdb::PaxTableFileBits(relation));
    auto result =
        CPaxDeleter::DeleteTuple(relation, &internal, cid, snapshot, tmfd);
    if (result == TM_Ok) pgstat_count_heap_delete(relation);
    return result;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pg_unreachable();
}

TM_Result CCPaxAccessMethod::TupleUpdate(Relation relation, ItemPointer otid,
                                         TupleTableSlot *slot, CommandId cid,
                                         uint32 /*options*/, Snapshot snapshot,
                                         Snapshot crosscheck, bool wait,
                                         TM_FailureData *tmfd,
                                         LockTupleMode *lockmode,
                                         TU_UpdateIndexes *update_indexes) {
  CBDB_TRY();
  {
    MemoryContext old_ctx;
    TM_Result result;
    ItemPointerData internal =
        PaxTidFromTable(*otid, cbdb::PaxTableFileBits(relation));

    // the writer and the deleter, made here if this is the statement's first
    // row, come with the memory context they write in
    (void)CPaxDmlStateLocal::Instance()->GetInserter(relation);
    Assert(cbdb::pax_memory_context);
    old_ctx = MemoryContextSwitchTo(cbdb::pax_memory_context);
    result = CPaxUpdater::UpdateTuple(relation, &internal, slot, cid, snapshot,
                                      crosscheck, wait, tmfd, lockmode,
                                      update_indexes);
    MemoryContextSwitchTo(old_ctx);
    if (result == TM_Ok) {
      PaxSlotTidToTable(relation, slot);
      pgstat_count_heap_update(relation, false, false);
    }
    return result;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pg_unreachable();
}

// ANALYZE samples a PAX table through the module's hook (modules/pax/pax.c):
// its block numbers, which ANALYZE's read stream would walk, are no pages.
bool CCPaxAccessMethod::ScanAnalyzeNextBlock(TableScanDesc /*scan*/,
                                             ReadStream * /*stream*/) {
  return false;
}

bool CCPaxAccessMethod::ScanAnalyzeNextTuple(TableScanDesc /*scan*/,
                                             double * /*liverows*/,
                                             double * /*deadrows*/,
                                             TupleTableSlot * /*slot*/) {
  return false;
}

bool CCPaxAccessMethod::ScanBitmapNextTuple(TableScanDesc scan,
                                            TupleTableSlot *slot,
                                            bool *recheck,
                                            uint64 *lossy_pages,
                                            uint64 *exact_pages) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    bool result;
    result = desc->BitmapNextTuple(slot, recheck, lossy_pages, exact_pages);
    if (result) {
      pgstat_count_heap_fetch(desc->GetRelation());
    }
    return result;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pg_unreachable();
}

bool CCPaxAccessMethod::ScanSampleNextBlock(TableScanDesc scan,
                                            SampleScanState *scanstate) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    return desc->ScanSampleNextBlock(scanstate);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pg_unreachable();
}

bool CCPaxAccessMethod::ScanSampleNextTuple(TableScanDesc scan,
                                            SampleScanState *scanstate,
                                            TupleTableSlot *slot) {
  CBDB_TRY();
  {
    auto desc = PaxScanDesc::ToDesc(scan);
    bool result = desc->ScanSampleNextTuple(scanstate, slot);
    if (result) PaxSlotTidToTable(desc->GetRelation(), slot);
    return result;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pg_unreachable();
}

void CCPaxAccessMethod::MultiInsert(Relation relation, TupleTableSlot **slots,
                                    int ntuples, CommandId cid, uint32 options,
                                    BulkInsertState bistate) {
  CBDB_TRY();
  {
    MemoryContext old_ctx;

    (void)CPaxDmlStateLocal::Instance()->GetInserter(relation);
    Assert(cbdb::pax_memory_context);

    old_ctx = MemoryContextSwitchTo(cbdb::pax_memory_context);
    CPaxInserter::MultiInsert(relation, slots, ntuples, cid, (int)options,
                              bistate);
    MemoryContextSwitchTo(old_ctx);

    for (int i = 0; i < ntuples; i++) PaxSlotTidToTable(relation, slots[i]);
    pgstat_count_heap_insert(relation, ntuples);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({
      // FIXME: destroy CPaxInserter?
  });
  CBDB_END_TRY();
}

void CCPaxAccessMethod::FinishBulkInsert(Relation relation, uint32 options) {
  // Implement Pax dml cleanup for case "create table xxx1 as select * from
  // xxx2", which would not call ExtDmlFini callback function and relies on
  // FinishBulkInsert callback function to cleanup its dml state.
  CBDB_TRY();
  {
    // no need switch memory context
    // cause it just call dml finish
    pax::CPaxInserter::FinishBulkInsert(relation, (int)options);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({
      // FIXME: destroy CPaxInserter?
  });
  CBDB_END_TRY();
}

void CCPaxAccessMethod::ExtDmlInit(Relation rel, CmdType operation) {
  CBDB_TRY();
  { pax::CPaxDmlStateLocal::Instance()->InitDmlState(rel, operation); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

void CCPaxAccessMethod::ExtDmlFini(Relation rel, CmdType operation) {
  CBDB_TRY();
  { pax::CPaxDmlStateLocal::Instance()->FinishDmlState(rel, operation); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

}  // namespace pax
// END of C++ implementation

// access methods that are implemented in C
namespace paxc {
const TupleTableSlotOps *PaxAccessMethod::SlotCallbacks(
    Relation /*rel*/) noexcept {
  return &TTSOpsVirtual;
}

Size PaxAccessMethod::ParallelscanEstimate(Relation /*rel*/) {
  return sizeof(ParallelBlockTableScanDescData);
}

Size PaxAccessMethod::ParallelscanInitialize(Relation rel,
                                             ParallelTableScanDesc pscan) {
  ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc)pscan;
  bpscan->base.phs_locator = rel->rd_locator;
  bpscan->base.phs_syncscan = false;
  pg_atomic_init_u64(&bpscan->phs_nallocated, 0);
  // Like ao/aocs, we don't need phs_mutex and phs_startblock, though, init
  // them.
  SpinLockInit(&bpscan->phs_mutex);
  bpscan->phs_startblock = InvalidBlockNumber;
  bpscan->phs_numblock = InvalidBlockNumber;
  bpscan->phs_nblocks = 0;
  return sizeof(ParallelBlockTableScanDescData);
}

void PaxAccessMethod::ParallelscanReinitialize(Relation rel,
                                               ParallelTableScanDesc pscan) {
  ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc)pscan;

  pg_atomic_write_u64(&bpscan->phs_nallocated, 0);
}

void PaxAccessMethod::TupleInsertSpeculative(Relation /*relation*/,
                                             TupleTableSlot * /*slot*/,
                                             CommandId /*cid*/,
                                             uint32 /*options*/,
                                             BulkInsertState /*bistate*/,
                                             uint32 /*spec_token*/) {
  NOT_IMPLEMENTED_YET;
}

void PaxAccessMethod::TupleCompleteSpeculative(Relation /*relation*/,
                                               TupleTableSlot * /*slot*/,
                                               uint32 /*spec_token*/,
                                               bool /*succeeded*/) {
  NOT_IMPLEMENTED_YET;
}

TM_Result PaxAccessMethod::TupleLock(Relation /*relation*/, ItemPointer /*tid*/,
                                     Snapshot /*snapshot*/,
                                     TupleTableSlot * /*slot*/,
                                     CommandId /*cid*/, LockTupleMode /*mode*/,
                                     LockWaitPolicy /*wait_policy*/,
                                     uint8 /*flags*/,
                                     TM_FailureData * /*tmfd*/) {
  NOT_IMPLEMENTED_YET;
  return TM_Ok;
}

// A row by its TID, as the DELETE of a split update fetches the old row it
// sends back to the coordinator (gp_core's gp_split_delete()), where
// Cloudberry's Split took it from the plan: through the index fetch's path,
// its descriptor kept while one query fetches from one table with one
// snapshot, so that a file is opened once for the rows it has.
struct PaxFetchCache {
  Oid relid = InvalidOid;
  Snapshot snapshot = nullptr;
  pax::PaxIndexScanDesc *desc = nullptr;
};
static PaxFetchCache fetch_cache;

void PaxFetchCacheReset() {
  auto desc = fetch_cache.desc;

  fetch_cache = PaxFetchCache();
  if (desc) {
    desc->Release();
    pax::PAX_DELETE(desc);
  }
}

bool PaxAccessMethod::TupleFetchRowVersion(Relation relation, ItemPointer tid,
                                           Snapshot snapshot,
                                           TupleTableSlot *slot) {
  CBDB_TRY();
  {
    ItemPointerData internal =
        PaxTidFromTable(*tid, cbdb::PaxTableFileBits(relation));

    if (fetch_cache.desc == nullptr ||
        fetch_cache.relid != RelationGetRelid(relation) ||
        fetch_cache.snapshot != snapshot) {
      PaxFetchCacheReset();
      fetch_cache.desc = pax::PAX_NEW<pax::PaxIndexScanDesc>(relation);
      fetch_cache.relid = RelationGetRelid(relation);
      fetch_cache.snapshot = snapshot;
    }
    fetch_cache.desc->Rebind(relation);
    if (fetch_cache.desc->FetchTuple(&internal, snapshot, slot, nullptr,
                                     nullptr)) {
      slot->tts_tid = *tid;
      slot->tts_tableOid = RelationGetRelid(relation);
      return true;
    }
    return false;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  return false;
}

bool PaxAccessMethod::TupleTidValid(TableScanDesc /*scan*/,
                                    ItemPointer /*tid*/) {
  NOT_IMPLEMENTED_YET;
  return false;
}

void PaxAccessMethod::TupleGetLatestTid(TableScanDesc /*sscan*/,
                                        ItemPointer /*tid*/) {
  NOT_SUPPORTED_YET;
}

bool PaxAccessMethod::TupleSatisfiesSnapshot(Relation /*rel*/,
                                             TupleTableSlot * /*slot*/,
                                             Snapshot /*snapshot*/) {
  NOT_IMPLEMENTED_YET;
  return true;
}

TransactionId PaxAccessMethod::IndexDeleteTuples(
    Relation /*rel*/, TM_IndexDeleteOp * /*delstate*/) {
  NOT_SUPPORTED_YET;
  return 0;
}

void PaxAccessMethod::RelationVacuum(Relation /*onerel*/,
                                     const VacuumParams * /*params*/,
                                     BufferAccessStrategy /*bstrategy*/) {
  /* PAX: micro-partitions have no dead tuples, so vacuum is empty */
}

bool PaxAccessMethod::RelationNeedsToastTable(Relation /*rel*/) {
  // PAX never used the toasting, don't create the toast table from Cloudberry 7

  return false;
}

double PaxAccessMethod::IndexBuildRangeScan(
    Relation heap_relation, Relation index_relation, IndexInfo *index_info,
    bool /*allow_sync*/, bool anyvisible, bool progress,
    BlockNumber start_blockno, BlockNumber numblocks,
    IndexBuildCallback callback, void *callback_state, TableScanDesc scan) {
  Datum values[INDEX_MAX_KEYS];
  bool isnull[INDEX_MAX_KEYS];
  double reltuples = 0;
  ExprState *predicate;
  TupleTableSlot *slot;
  EState *estate;
  ExprContext *econtext;
  Snapshot snapshot;
  TransactionId OldestXmin;
  Oid bitmap_am_oid = get_index_am_oid("bitmap", true);

  bool checking_uniqueness pg_attribute_unused();
  bool need_unregister_snapshot = false;
  BlockNumber previous_blkno = InvalidBlockNumber;

  Assert(OidIsValid(index_relation->rd_rel->relam));
  Assert(!IsSystemRelation(heap_relation));

  // gp_ao's bitmap index, where it is there
  if (index_relation->rd_rel->relam != BTREE_AM_OID &&
      index_relation->rd_rel->relam != HASH_AM_OID &&
      index_relation->rd_rel->relam != GIN_AM_OID &&
      (!OidIsValid(bitmap_am_oid) ||
       index_relation->rd_rel->relam != bitmap_am_oid))
    elog(ERROR, "pax only support btree/hash/gin/bitmap indexes");

  checking_uniqueness =
      (index_info->ii_Unique || index_info->ii_ExclusionOps != NULL);
  // "Any visible" mode is not compatible with uniqueness checks; make sure
  // only one of those is requested.
  (void)anyvisible;  // keep compiler quiet for release version
  Assert(!(anyvisible && checking_uniqueness));

  slot = table_slot_create(heap_relation, NULL);
  estate = CreateExecutorState();
  econtext = GetPerTupleExprContext(estate);
  econtext->ecxt_scantuple = slot;
  predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

  /*
   * Prepare for scan of the base relation.  In a normal index build, we use
   * SnapshotAny because we must retrieve all tuples and do our own time
   * qual checks (because we have to index RECENTLY_DEAD tuples). In a
   * concurrent build, or during bootstrap, we take a regular MVCC snapshot
   * and index whatever's live according to that.
   */
  OldestXmin = InvalidTransactionId;

  /* okay to ignore lazy VACUUMs here */
  if (!IsBootstrapProcessingMode() && !index_info->ii_Concurrent)
    OldestXmin = GetOldestNonRemovableTransactionId(heap_relation);

  if (!scan) {
    if (!TransactionIdIsValid(OldestXmin)) {
      snapshot = RegisterSnapshot(GetTransactionSnapshot());
      need_unregister_snapshot = true;
    } else {
      need_unregister_snapshot = false;
      snapshot = SnapshotAny;
    }
    scan = table_beginscan(heap_relation, snapshot, 0, NULL, 0);
  } else {
    snapshot = scan->rs_snapshot;
    need_unregister_snapshot = false;
  }

  // FIXME: Only brin index uses partial index now. setup start_blockno
  // and numblocks is too late after beginscan is called now, because
  // the current micro partition is opened. The workaround is ugly to
  // check and close the current micro partition and open another one.
  if (start_blockno != 0 || numblocks != InvalidBlockNumber)
    elog(ERROR, "PAX doesn't support partial index scan now");

  while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
    CHECK_FOR_INTERRUPTS();

    if (progress) {
      // the table's TID, of which the scan gives the block
      BlockNumber blkno = ItemPointerGetBlockNumber(&slot->tts_tid);
      if (previous_blkno == InvalidBlockNumber)
        previous_blkno = blkno;
      else if (previous_blkno != blkno) {
        pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
                                     blkno - start_blockno);
        previous_blkno = blkno;
      }
    }
    reltuples += 1;

    MemoryContextReset(econtext->ecxt_per_tuple_memory);

    /*
     * In a partial index, discard tuples that don't satisfy the
     * predicate.
     */
    if (predicate && !ExecQual(predicate, econtext)) continue;

    /*
     * For the current heap tuple, extract all the attributes we use in
     * this index, and note which are null.  This also performs evaluation
     * of any expressions needed.
     */
    FormIndexDatum(index_info, slot, estate, values, isnull);

    /*
     * You'd think we should go ahead and build the index tuple here, but
     * some index AMs want to do further processing on the data first.  So
     * pass the values[] and isnull[] arrays, instead.
     */
    callback(index_relation, &slot->tts_tid, values, isnull, true,
             callback_state);
  }

  /* Report scan progress one last time. */
  if (progress && previous_blkno != InvalidBlockNumber)
    pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
                                 previous_blkno + 1 - start_blockno);

  table_endscan(scan);
  if (need_unregister_snapshot) UnregisterSnapshot(snapshot);

  ExecDropSingleTupleTableSlot(slot);
  FreeExecutorState(estate);

  /* These may have been pointing to the now-gone estate */
  index_info->ii_ExpressionsState = NIL;
  index_info->ii_PredicateState = NULL;

  return reltuples;
}

void PaxAccessMethod::IndexValidateScan(Relation /*heap_relation*/,
                                        Relation /*index_relation*/,
                                        IndexInfo * /*index_info*/,
                                        Snapshot /*snapshot*/,
                                        ValidateIndexState * /*state*/) {
  NOT_IMPLEMENTED_YET;
}

bytea *PaxAccessMethod::AmOptions(Datum reloptions, char relkind,
                                  bool validate) {
  return paxc_default_rel_options(reloptions, relkind, validate);
}

}  // namespace paxc
// END of C implementation

extern "C" {

static const TableAmRoutine kPaxColumnMethods = {
    .type = T_TableAmRoutine,
    .slot_callbacks = paxc::PaxAccessMethod::SlotCallbacks,
    .scan_begin = pax::CCPaxAccessMethod::ScanBegin,
    .scan_end = pax::CCPaxAccessMethod::ScanEnd,
    .scan_rescan = pax::CCPaxAccessMethod::ScanRescan,
    .scan_getnextslot = pax::CCPaxAccessMethod::ScanGetNextSlot,

    .parallelscan_estimate = paxc::PaxAccessMethod::ParallelscanEstimate,
    .parallelscan_initialize = paxc::PaxAccessMethod::ParallelscanInitialize,
    .parallelscan_reinitialize =
        paxc::PaxAccessMethod::ParallelscanReinitialize,

    .index_fetch_begin = pax::CCPaxAccessMethod::IndexFetchBegin,
    .index_fetch_reset = pax::CCPaxAccessMethod::IndexFetchReset,
    .index_fetch_end = pax::CCPaxAccessMethod::IndexFetchEnd,
    .index_fetch_tuple = pax::CCPaxAccessMethod::IndexFetchTuple,

    .tuple_fetch_row_version = paxc::PaxAccessMethod::TupleFetchRowVersion,
    .tuple_tid_valid = paxc::PaxAccessMethod::TupleTidValid,
    .tuple_get_latest_tid = paxc::PaxAccessMethod::TupleGetLatestTid,
    .tuple_satisfies_snapshot = paxc::PaxAccessMethod::TupleSatisfiesSnapshot,
    .index_delete_tuples = paxc::PaxAccessMethod::IndexDeleteTuples,

    .tuple_insert = pax::CCPaxAccessMethod::TupleInsert,
    .tuple_insert_speculative = paxc::PaxAccessMethod::TupleInsertSpeculative,
    .tuple_complete_speculative =
        paxc::PaxAccessMethod::TupleCompleteSpeculative,
    .multi_insert = pax::CCPaxAccessMethod::MultiInsert,
    .tuple_delete = pax::CCPaxAccessMethod::TupleDelete,
    .tuple_update = pax::CCPaxAccessMethod::TupleUpdate,
    .tuple_lock = paxc::PaxAccessMethod::TupleLock,
    .finish_bulk_insert = pax::CCPaxAccessMethod::FinishBulkInsert,

    .relation_set_new_filelocator =
        pax::CCPaxAccessMethod::RelationSetNewFilenode,
    .relation_nontransactional_truncate =
        pax::CCPaxAccessMethod::RelationNontransactionalTruncate,
    .relation_copy_data = pax::CCPaxAccessMethod::RelationCopyData,
    .relation_copy_for_cluster = pax::CCPaxAccessMethod::RelationCopyForCluster,
    .relation_vacuum = paxc::PaxAccessMethod::RelationVacuum,
    .scan_analyze_next_block = pax::CCPaxAccessMethod::ScanAnalyzeNextBlock,
    .scan_analyze_next_tuple = pax::CCPaxAccessMethod::ScanAnalyzeNextTuple,
    .index_build_range_scan = paxc::PaxAccessMethod::IndexBuildRangeScan,
    .index_validate_scan = paxc::PaxAccessMethod::IndexValidateScan,

    .relation_size = paxc::PaxAccessMethod::RelationSize,
    .relation_needs_toast_table =
        paxc::PaxAccessMethod::RelationNeedsToastTable,

    .relation_estimate_size = paxc::PaxAccessMethod::EstimateRelSize,
    .scan_bitmap_next_tuple = pax::CCPaxAccessMethod::ScanBitmapNextTuple,
    .scan_sample_next_block = pax::CCPaxAccessMethod::ScanSampleNextBlock,
    .scan_sample_next_tuple = pax::CCPaxAccessMethod::ScanSampleNextTuple,
};

/*
 * What PostgreSQL 19's core asks of the registry (O13), where Cloudberry's
 * asked its own members of TableAmRoutine.
 */
static const TableAmExtRoutine kPaxExtMethods = {
    .size = sizeof(TableAmExtRoutine),
    .reloptions = paxc::PaxAccessMethod::AmOptions,
    .scan_extractcolumns = pax::CCPaxAccessMethod::ScanExtractColumns,
    .scan_by_column = true,
    .index_unique_check = pax::CCPaxAccessMethod::IndexUniqueCheck,
    .relation_add_columns = NULL,
    .relation_get_block_sequences = NULL,
    .size_from_am = true,
    .old_row_from_plan = true,
};

const TableAmRoutine *PaxTableAmRoutine(void) { return &kPaxColumnMethods; }

PG_FUNCTION_INFO_V1(pax_tableam_handler);
Datum pax_tableam_handler(PG_FUNCTION_ARGS) {  // NOLINT
  PG_RETURN_POINTER(&kPaxColumnMethods);
}

static object_access_hook_type prev_object_access_hook = NULL;

static ProcessUtility_hook_type prev_ProcessUtilit_hook = NULL;

static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;

static ExecutorFinish_hook_type prev_ExecutorFinish_hook = NULL;

static bool relation_has_cluster_columns_options(Relation rel) {
  auto *options = (paxc::PaxOptions *)(rel->rd_options);

  if (options == nullptr) {
    return false;
  }

  // if not zorder and lexical cluster type, return false
  if (strcasecmp(options->cluster_type, PAX_LEXICAL_CLUSTER_TYPE) != 0 &&
      strcasecmp(options->cluster_type, PAX_ZORDER_CLUSTER_TYPE) != 0) {
    return false;
  }

  // no need to check zorder cluster type, because it has been checked in
  // PaxObjectAccessHook
  Bitmapset *bms = paxc::paxc_get_columns_index_by_options(
      rel, options->cluster_columns(), nullptr, false);

  if (bms) {
    bms_free(bms);
  }
  return bms != nullptr;
}

static bool relation_has_clustered_index(Relation rel) {
  // We need to find the index that has indisclustered set.
  Oid indexOid = InvalidOid;
  ListCell *index;
  foreach (index, RelationGetIndexList(rel)) {
    indexOid = lfirst_oid(index);
    if (get_index_isclustered(indexOid)) break;
    indexOid = InvalidOid;
  }
  return OidIsValid(indexOid);
}

static bool table_can_be_clustered(Relation rel) {
  return relation_has_cluster_columns_options(rel) ||
         relation_has_clustered_index(rel);
}

static void cluster_pax_rel(Relation rel, Snapshot snapshot) {
  CBDB_TRY();
  { pax::Cluster(rel, snapshot, false); }
  CBDB_CATCH_DEFAULT();
  CBDB_END_TRY();
}

// CLUSTER of a table clustered by its cluster_columns: PAX's own, on each
// node.  The coordinator sends the statement to the segments itself, where
// gp_core would send it to be run as PostgreSQL's CLUSTER; a segment, or a
// node on its own, clusters its rows.
static void cluster_pax_rel_on_nodes(Relation rel) {
  if (IS_QUERY_DISPATCHER()) {
    char *sql = psprintf(
        "CLUSTER %s",
        quote_qualified_identifier(
            get_namespace_name(RelationGetNamespace(rel)),
            RelationGetRelationName(rel)));

    GpDispatchCommand(sql);
    pfree(sql);
    return;
  }

  bool pushed = !ActiveSnapshotSet();
  if (pushed) PushActiveSnapshot(GetTransactionSnapshot());
  cluster_pax_rel(rel, GetActiveSnapshot());
  if (pushed) PopActiveSnapshot();

  // gp_core's hook, which this statement does not reach, tells the
  // coordinator whether this segment's part wrote
  GpDtxReportXid();
}

static void paxProcessUtility(PlannedStmt *pstmt, const char *queryString,
                              bool readOnlyTree, ProcessUtilityContext context,
                              ParamListInfo params, QueryEnvironment *queryEnv,
                              DestReceiver *dest,
                              QueryCompletion *completionTag) {
  bool isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
  // if is pax table, do something
  switch (nodeTag(pstmt->utilityStmt)) {
    case T_RepackStmt: {
      RepackStmt *stmt = (RepackStmt *)pstmt->utilityStmt;

      // Cloudberry's CLUSTER; REPACK and VACUUM FULL rewrite as
      // PostgreSQL's do
      if (stmt->command != REPACK_COMMAND_CLUSTER ||
          GpDispatchIsDispatchedStatement(pstmt->utilityStmt))
        break;
      if (stmt->relation && stmt->relation->relation) {
        RangeVar *relation = stmt->relation->relation;
        Relation rel = table_openrv(relation, AccessShareLock);
        if (RelationIsPAX(rel)) {
          bool has_cluster_columns;
          if (stmt->indexname == NULL && !table_can_be_clustered(rel)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("there is no previously clustered index or "
                            "cluster_columns reloptions for table \"%s\"",
                            relation->relname)));
          }

          has_cluster_columns = relation_has_cluster_columns_options(rel);

          // cluster table using indexname,we should check if
          // relation_has_zorder_clusted
          if (stmt->indexname && has_cluster_columns) {
            // without the space Cloudberry's elog took off the message's
            // end as it sent it (cdb_tidy_message())
            elog(ERROR,
                 "cannot using index to cluster table which has "
                 "cluster-columns");
          }

          if (has_cluster_columns) {
            cluster_pax_rel_on_nodes(rel);
            table_close(rel, NoLock);
            return;
          }
        }

        table_close(rel, NoLock);
      } else if (IS_QUERY_DISPATCHER() && PaxCatalogReady()) {
        // only cluster pax zorder clustered table
        List *relids = NULL;
        SysScanDesc scan;
        HeapTuple tuple;

        // We cannot run this form of CLUSTER inside a user transaction block;
        // we'd be holding locks way too long.
        PreventInTransactionBlock(isTopLevel, "CLUSTER");

        auto pax_aux_rel = table_open(PAX_TABLES_RELATION_ID, AccessShareLock);
        scan = systable_beginscan(pax_aux_rel, InvalidOid, false,
                                  GetActiveSnapshot(), 0, NULL);

        while ((tuple = systable_getnext(scan)) != NULL) {
          Datum datum;
          bool isnull;
          datum = heap_getattr(tuple, ANUM_PG_PAX_TABLES_RELID,
                               pax_aux_rel->rd_att, &isnull);
          Assert(!isnull);
          Oid relid = DatumGetObjectId(datum);
          relids = lappend_oid(relids, relid);
        }
        systable_endscan(scan);
        table_close(pax_aux_rel, AccessShareLock);

        ListCell *lc = NULL;
        foreach (lc, relids) {
          Oid pax_rel_id = Oid(lfirst_oid(lc));

          Relation rel = table_open(pax_rel_id, RowExclusiveLock);
          if (relation_has_cluster_columns_options(rel))
            cluster_pax_rel_on_nodes(rel);
          table_close(rel, RowExclusiveLock);
        }

        list_free(relids);
      }
      break;
    }
    default:
      break;
  }

  // What the statement writes to a PAX table outside a query -- COPY FROM --
  // is its own, finished as it ends.
  pax::CPaxDmlStateLocal::PushOwner(pstmt);
  PG_TRY();
  {
    if (prev_ProcessUtilit_hook)
      prev_ProcessUtilit_hook(pstmt, queryString, readOnlyTree, context,
                              params, queryEnv, dest, completionTag);
    else
      standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
                              params, queryEnv, dest, completionTag);
  }
  PG_FINALLY();
  { pax::CPaxDmlStateLocal::PopOwner(pstmt); }
  PG_END_TRY();

  CBDB_TRY();
  {
    paxc::PaxFetchCacheReset();
    pax::CPaxDmlStateLocal::Instance()->FinishOwned(pstmt);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

// What a query writes to a PAX table is its own, finished as the query
// finishes, before its AFTER triggers fire.
static void PaxExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                           uint64 count) {
  pax::CPaxDmlStateLocal::PushOwner(queryDesc);
  PG_TRY();
  {
    if (prev_ExecutorRun_hook)
      prev_ExecutorRun_hook(queryDesc, direction, count);
    else
      standard_ExecutorRun(queryDesc, direction, count);
  }
  PG_FINALLY();
  { pax::CPaxDmlStateLocal::PopOwner(queryDesc); }
  PG_END_TRY();
}

static void PaxExecutorFinish(QueryDesc *queryDesc) {
  CBDB_TRY();
  {
    paxc::PaxFetchCacheReset();
    pax::CPaxDmlStateLocal::Instance()->FinishOwned(queryDesc);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  if (prev_ExecutorFinish_hook)
    prev_ExecutorFinish_hook(queryDesc);
  else
    standard_ExecutorFinish(queryDesc);
}

// A transaction's writers: finished before it commits, forgotten, unwritten,
// where it or a subtransaction of it aborts.
static void PaxXactCallback(XactEvent event, void * /*arg*/) {
  switch (event) {
    case XACT_EVENT_PRE_COMMIT:
    case XACT_EVENT_PARALLEL_PRE_COMMIT:
    case XACT_EVENT_PRE_PREPARE: {
      bool pushed = !ActiveSnapshotSet();

      // the catalog rows a writer writes as it finishes need a snapshot,
      // which a transaction about to commit has none of
      if (pushed) PushActiveSnapshot(GetTransactionSnapshot());
      CBDB_TRY();
      { pax::CPaxDmlStateLocal::Instance()->FinishAll(); }
      CBDB_CATCH_DEFAULT();
      CBDB_FINALLY({});
      CBDB_END_TRY();
      if (pushed) PopActiveSnapshot();
      break;
    }
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_ABORT:
    case XACT_EVENT_PREPARE:
      try {
        paxc::PaxFetchCacheReset();
        pax::CPaxDmlStateLocal::Instance()->Forget(InvalidSubTransactionId);
      } catch (...) {
      }
      break;
    default:
      break;
  }
}

static void PaxSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
                               SubTransactionId parentSubid, void * /*arg*/) {
  try {
    if (event == SUBXACT_EVENT_ABORT_SUB) {
      paxc::PaxFetchCacheReset();
      pax::CPaxDmlStateLocal::Instance()->Forget(mySubid);
    }
    else if (event == SUBXACT_EVENT_COMMIT_SUB)
      pax::CPaxDmlStateLocal::Instance()->Reparent(mySubid, parentSubid);
  } catch (...) {
  }
}

static void PaxCheckMinMaxColumns(Relation rel, const char *minmax_columns) {
  if (!minmax_columns) return;

  // already check the options exists
  auto bms = paxc::paxc_get_columns_index_by_options(rel, minmax_columns,
                                                     nullptr, true);
  bms_free(bms);
}

static void PaxCheckBloomFilterColumns(Relation rel, const char *bf_columns) {
  if (!bf_columns) return;

  // already check the options exists
  auto bms =
      paxc::paxc_get_columns_index_by_options(rel, bf_columns, nullptr, true);
  bms_free(bms);
}

static void PaxCheckClusterColumns(Relation rel, const char *cluster_columns) {
  if (!cluster_columns || strlen(cluster_columns) == 0) return;

  Oid indexOid = InvalidOid;
  ListCell *index;
  foreach (index, RelationGetIndexList(rel)) {
    indexOid = lfirst_oid(index);
    if (get_index_isclustered(indexOid)) {
      elog(ERROR,
           "pax table has previously clustered index, can't set "
           "cluster_columns reloptions");
    }
  }

  auto bms = paxc::paxc_get_columns_index_by_options(
      rel, cluster_columns,
      [](Form_pg_attribute attr) {
        // this callback in the paxc env
        if (!paxc::support_zorder_type(attr->atttypid)) {
          elog(ERROR, "the type of column %s does not support zorder cluster",
               attr->attname.data);
        }
      },
      true);
  bms_free(bms);
}

static void PaxCheckNumericOption(Relation rel, char *storage_format) {
  auto relnatts = RelationGetNumberOfAttributes(rel);
  auto tupdesc = RelationGetDescr(rel);

  if (strcmp(storage_format, STORAGE_FORMAT_TYPE_PORC_VEC) != 0) {
    return;
  }

#ifndef HAVE_INT128
  elog(ERROR, "option 'storage_format=porc_vec' must be enable INT128 build");
#endif

  for (int attno = 0; attno < relnatts; attno++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, attno);

    if (attr->atttypid != NUMERICOID) continue;

    if (attr->atttypmod < 0) {
      elog(ERROR,
           "column '%s' created with not support precision(-1) and scale(-1).",
           NameStr(attr->attname));
    }

    int64 precision = ((attr->atttypmod - VARHDRSZ) >> 16) & 0xffff;

    // no need check scale
    if (precision > VEC_SHORT_NUMERIC_MAX_PRECISION) {
      elog(ERROR,
           "column '%s' precision(%ld) out of range, precision should be (0, "
           "%d]",
           NameStr(attr->attname), precision, VEC_SHORT_NUMERIC_MAX_PRECISION);
    }
  }
}

static void PaxObjectAccessHook(ObjectAccessType access, Oid class_id,
                                Oid object_id, int sub_id, void *arg) {
  Relation rel;
  paxc::PaxOptions *options;
  bool ok;

  if (prev_object_access_hook)
    prev_object_access_hook(access, class_id, object_id, sub_id, arg);

  if (class_id != RelationRelationId) return;

  // if not (OAT_POST_CREATE or OAT_TRUNCATE or OAT_DROP)
  if (!(access == OAT_POST_CREATE || access == OAT_TRUNCATE ||
        access == OAT_POST_ALTER || access == OAT_DROP))
    return;

  // A table dropped, or the transient one a rewrite drops: its rows in PAX's
  // catalog, and what it had yet to write, go with it.  Its aux table goes
  // by its dependency.
  if (access == OAT_DROP) {
    if (sub_id == 0 && PaxCatalogReady()) {
      paxc::DeletePaxTablesEntry(object_id);
      pax::CPaxDmlStateLocal::Instance()->ForgetRelation(object_id);
    }
    return;
  }

  // A relation that may have just taken another's storage, as a rewrite
  // swaps them: its catalog follows the storage.
  if (access == OAT_POST_ALTER && sub_id == 0 && PaxCatalogReady())
    paxc::PaxTablesFollowStorage(object_id);

  if (access == OAT_POST_ALTER) {
    ObjectAccessPostAlter *pa_arg = (ObjectAccessPostAlter *)arg;
    bool is_internal = pa_arg->is_internal;
    if (is_internal) {
      return;
    }
  }

  // Nothing of a TRUNCATE's is checked.
  if (access == OAT_TRUNCATE) return;

  // Only a PAX table's options are checked, once the statement's change
  // of it is seen.
  CommandCounterIncrement();
  if (!OidIsValid(PaxTableAmOid()) || get_rel_relam(object_id) != PaxTableAmOid())
    return;

  rel = relation_open(object_id, RowExclusiveLock);
  ok = ((rel->rd_rel->relkind == RELKIND_RELATION ||
         rel->rd_rel->relkind == RELKIND_MATVIEW) &&
        RelationIsPAX(rel));
  if (!ok) goto out;

  switch (access) {
    case OAT_POST_CREATE:
    case OAT_POST_ALTER: {
      options = reinterpret_cast<paxc::PaxOptions *>(rel->rd_options);
      if (options == NULL) goto out;

      PaxCheckMinMaxColumns(rel, options->minmax_columns());
      PaxCheckBloomFilterColumns(rel, options->bloomfilter_columns());
      PaxCheckClusterColumns(rel, options->cluster_columns());
      PaxCheckNumericOption(rel, options->storage_format);
    } break;

    default:
      break;
  }

out:
  relation_close(rel, RowExclusiveLock);
}

// Cloudberry's _PG_init, as modules/pax/pax.c's calls it, once the module
// is known to be preloaded with gp_core.
void pax_init(void) {  // NOLINT
  prev_object_access_hook = object_access_hook;
  object_access_hook = PaxObjectAccessHook;

  prev_ProcessUtilit_hook = ProcessUtility_hook;
  ProcessUtility_hook = paxProcessUtility;

  prev_ExecutorRun_hook = ExecutorRun_hook;
  ExecutorRun_hook = PaxExecutorRun;

  prev_ExecutorFinish_hook = ExecutorFinish_hook;
  ExecutorFinish_hook = PaxExecutorFinish;

  RegisterXactCallback(PaxXactCallback, NULL);
  RegisterSubXactCallback(PaxSubXactCallback, NULL);

  paxc::DefineGUCs();

  pax::common::InitResourceCallback();

  paxc::paxc_reg_rel_options();

  paxc::RegisterPaxRmgr();

  paxc::RegisterPaxSmgr();

  RegisterTableAmExtension(&kPaxColumnMethods, &kPaxExtMethods);
}
}  // extern "C"
