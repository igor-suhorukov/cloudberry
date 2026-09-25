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
 * pax_access_handle.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_access_handle.h
 *
 *
 * Ported to PostgreSQL 19: the callbacks take PostgreSQL 19's arguments;
 * Cloudberry's own members of TableAmRoutine are gone, the ones PAX needs
 * becoming the TableAmExtRoutine the module registers (O13 to O20); and a
 * TID crosses the table access method's boundary translated between PAX's
 * layout and the table's (see pax_access_handle.cc).
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

namespace paxc {

class PaxAccessMethod final {
 private:
  PaxAccessMethod() = default;

 public:
  static const TupleTableSlotOps *SlotCallbacks(Relation rel) noexcept;

  /* Parallel table scan related functions. */
  static Size ParallelscanEstimate(Relation rel);
  static Size ParallelscanInitialize(Relation rel, ParallelTableScanDesc pscan);
  static void ParallelscanReinitialize(Relation rel,
                                       ParallelTableScanDesc pscan);

  /* Callbacks for non-modifying operations on individual tuples */
  static bool TupleFetchRowVersion(Relation relation, ItemPointer tid,
                                   Snapshot snapshot, TupleTableSlot *slot);
  static bool TupleTidValid(TableScanDesc scan, ItemPointer tid);
  static void TupleGetLatestTid(TableScanDesc sscan, ItemPointer tid);
  static bool TupleSatisfiesSnapshot(Relation rel, TupleTableSlot *slot,
                                     Snapshot snapshot);
  static TransactionId IndexDeleteTuples(Relation rel,
                                         TM_IndexDeleteOp *delstate);

  static bool RelationNeedsToastTable(Relation rel);
  static uint64 RelationSize(Relation rel, ForkNumber fork_number);
  static void EstimateRelSize(Relation rel, int32 *attr_widths,
                              BlockNumber *pages, double *tuples,
                              double *allvisfrac);

  /* unsupported DML now, may move to CCPaxAccessMethod */
  static void TupleInsertSpeculative(Relation relation, TupleTableSlot *slot,
                                     CommandId cid, uint32 options,
                                     BulkInsertState bistate,
                                     uint32 spec_token);
  static void TupleCompleteSpeculative(Relation relation, TupleTableSlot *slot,
                                       uint32 spec_token, bool succeeded);
  static TM_Result TupleLock(Relation relation, ItemPointer tid,
                             Snapshot snapshot, TupleTableSlot *slot,
                             CommandId cid, LockTupleMode mode,
                             LockWaitPolicy wait_policy, uint8 flags,
                             TM_FailureData *tmfd);

  static void RelationVacuum(Relation onerel, const VacuumParams *params,
                             BufferAccessStrategy bstrategy);
  static double IndexBuildRangeScan(
      Relation heap_relation, Relation index_relation, IndexInfo *index_info,
      bool allow_sync, bool anyvisible, bool progress,
      BlockNumber start_blockno, BlockNumber numblocks,
      IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
  static void IndexValidateScan(Relation heap_relation, Relation index_relation,
                                IndexInfo *index_info, Snapshot snapshot,
                                ValidateIndexState *state);

  static bytea *AmOptions(Datum reloptions, char relkind, bool validate);
};

}  // namespace paxc

namespace pax {
class CCPaxAccessMethod final {
 private:
  CCPaxAccessMethod() = default;

 public:
  static TableScanDesc ScanBegin(Relation rel, Snapshot snapshot, int nkeys,
                                 struct ScanKeyData *key,
                                 ParallelTableScanDesc pscan, uint32 flags);
  static void ScanEnd(TableScanDesc scan);
  static void ScanRescan(TableScanDesc scan, struct ScanKeyData *key,
                         bool set_params, bool allow_strat, bool allow_sync,
                         bool allow_pagemode);
  static bool ScanGetNextSlot(TableScanDesc scan, ScanDirection direction,
                              TupleTableSlot *slot);

  // O15: the plan node a scan just begun serves
  static void ScanExtractColumns(TableScanDesc scan, struct PlanState *ps);

  static bool IndexUniqueCheck(Relation rel, ItemPointer tid, Snapshot snapshot,
                               bool *all_dead);

  /* Index Scan Callbacks */
  static struct IndexFetchTableData *IndexFetchBegin(Relation rel,
                                                     uint32 flags);
  static void IndexFetchEnd(struct IndexFetchTableData *scan);
  static void IndexFetchReset(struct IndexFetchTableData *scan);
  static bool IndexFetchTuple(struct IndexFetchTableData *scan, ItemPointer tid,
                              Snapshot snapshot, TupleTableSlot *slot,
                              bool *call_again, bool *all_dead);

  /* Manipulations of physical tuples. */
  static void TupleInsert(Relation relation, TupleTableSlot *slot,
                          CommandId cid, uint32 options,
                          BulkInsertState bistate);
  static TM_Result TupleDelete(Relation relation, ItemPointer tid,
                               CommandId cid, uint32 options,
                               Snapshot snapshot, Snapshot crosscheck,
                               bool wait, TM_FailureData *tmfd);
  static TM_Result TupleUpdate(Relation relation, ItemPointer otid,
                               TupleTableSlot *slot, CommandId cid,
                               uint32 options, Snapshot snapshot,
                               Snapshot crosscheck, bool wait,
                               TM_FailureData *tmfd, LockTupleMode *lockmode,
                               TU_UpdateIndexes *update_indexes);

  static void RelationCopyData(Relation rel, const RelFileLocator *newrnode);

  static void RelationCopyForCluster(Relation old_heap, Relation new_heap,
                                     Relation old_index, bool use_sort,
                                     TransactionId oldest_xmin,
                                     Snapshot snapshot,
                                     TransactionId *xid_cutoff,
                                     MultiXactId *multi_cutoff,
                                     double *num_tuples, double *tups_vacuumed,
                                     double *tups_recently_dead);

  static void RelationSetNewFilenode(Relation rel,
                                     const RelFileLocator *newrlocator,
                                     char persistence,
                                     TransactionId *freeze_xid,
                                     MultiXactId *minmulti);

  static void RelationNontransactionalTruncate(Relation rel);

  static bool ScanAnalyzeNextBlock(TableScanDesc scan, ReadStream *stream);
  static bool ScanAnalyzeNextTuple(TableScanDesc scan, double *liverows,
                                   double *deadrows, TupleTableSlot *slot);
  static bool ScanBitmapNextTuple(TableScanDesc scan, TupleTableSlot *slot,
                                  bool *recheck, uint64 *lossy_pages,
                                  uint64 *exact_pages);
  static bool ScanSampleNextBlock(TableScanDesc scan,
                                  SampleScanState *scanstate);
  static bool ScanSampleNextTuple(TableScanDesc scan,
                                  SampleScanState *scanstate,
                                  TupleTableSlot *slot);

  static void MultiInsert(Relation relation, TupleTableSlot **slots,
                          int ntuples, CommandId cid, uint32 options,
                          BulkInsertState bistate);

  static void FinishBulkInsert(Relation relation, uint32 options);

  // DML init/fini, which Cloudberry's executor called and the port's module
  // does as a statement ends (pax_access_handle.cc)
  static void ExtDmlInit(Relation rel, CmdType operation);
  static void ExtDmlFini(Relation rel, CmdType operation);
};

}  // namespace pax
