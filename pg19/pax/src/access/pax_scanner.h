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
 * pax_scanner.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_scanner.h
 *
 *
 * Ported to PostgreSQL 19:
 *   - the plan node a scan serves arrives once the scan has begun, through
 *     O15's scan_extractcolumns, where Cloudberry's began the scan with it
 *     (scan_begin_extractcolumns): the reader is made as the scan first reads,
 *     so that the projection and the filters the node gives it are its own;
 *   - a bitmap scan walks PostgreSQL 19's TID bitmap iterator itself, where
 *     Cloudberry's executor handed it a page at a time, each TID of the
 *     table's layout translated to PAX's (pax_tid.h);
 *   - gp_enable_predicate_pushdown is the port's gp.enable_predicate_pushdown;
 *   - the catalog snapshot a scan of SnapshotAny reads the aux table with is
 *     registered while the scan runs, as PostgreSQL 19 checks;
 *   - an index fetch's descriptor may be kept between the fetches of one
 *     query, a row fetched by its TID (Rebind()).
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

#include <unordered_set>

#include "comm/pax_memory.h"
#include "storage/filter/pax_filter.h"
#include "storage/pax.h"
#ifdef VEC_BUILD
#include "storage/vec/pax_vec_adapter.h"
#endif

namespace paxc {
bool IndexUniqueCheck(Relation rel, ItemPointer tid, Snapshot snapshot,
                      bool *all_dead);
}

namespace pax {
class PaxIndexScanDesc final {
 public:
  explicit PaxIndexScanDesc(Relation rel);
  ~PaxIndexScanDesc();
  bool FetchTuple(ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot,
                  bool *call_again, bool *all_dead);

  // release internal reader
  void Release();
  // the table's relation as the caller has it open now, for a descriptor
  // kept between its fetches
  inline void Rebind(Relation rel) { base_.rel = rel; }
  inline IndexFetchTableData *ToBase() { return &base_; }
  inline Relation GetRelation() { return base_.rel; }
  static inline PaxIndexScanDesc *FromBase(IndexFetchTableData *base) {
    return reinterpret_cast<PaxIndexScanDesc *>(base);
  }

 private:
  bool OpenMicroPartition(BlockNumber block, Snapshot snapshot);

  IndexFetchTableData base_;
  BlockNumber current_block_ = InvalidBlockNumber;
  std::unique_ptr<MicroPartitionReader> reader_;
  std::string rel_path_;
};

class PaxScanDesc {
 public:
  PaxScanDesc() = default;
  TableScanDesc BeginScan(Relation relation, Snapshot snapshot, int nkeys,
                          struct ScanKeyData *key, ParallelTableScanDesc pscan,
                          uint32 flags, std::shared_ptr<PaxFilter> &&pax_filter,
                          bool build_bitmap);

  // O15: the plan node the scan serves, before the scan reads
  void ExtractColumns(struct PlanState *ps);

  void EndScan();
  void ReScan(ScanKey key, bool set_params, bool allow_strat, bool allow_sync,
              bool allow_pagemode);

  bool GetNextSlot(TupleTableSlot *slot);

  bool ScanAnalyzeNextBlock(BlockNumber blockno,
                            BufferAccessStrategy bstrategy);
  bool ScanAnalyzeNextTuple(TransactionId oldest_xmin, double *liverows,
                            double *deadrows, TupleTableSlot *slot);

  bool ScanSampleNextBlock(SampleScanState *scanstate);

  bool ScanSampleNextTuple(SampleScanState *scanstate, TupleTableSlot *slot);

  bool BitmapNextTuple(TupleTableSlot *slot, bool *recheck,
                       uint64 *lossy_pages, uint64 *exact_pages);

  ~PaxScanDesc();

  static inline PaxScanDesc *ToDesc(TableScanDesc scan) {
    auto desc = reinterpret_cast<PaxScanDesc *>(scan);
    return desc;
  }

  inline Relation GetRelation() { return rs_base_.rs_rd; }

 private:
  // the reader, made as the scan first reads
  void OpenReader();

  TableScanDescData rs_base_{};

  // what OpenReader() needs of BeginScan()
  bool build_bitmap_ = false;
  ParallelTableScanDesc pscan_ = nullptr;
  // the catalog snapshot a scan of SnapshotAny reads the aux table with
  Snapshot aux_snapshot_ = nullptr;

  std::unique_ptr<TableReader> reader_;

  std::shared_ptr<DataBuffer<char>> reused_buffer_;

  MemoryContext memory_context_ = nullptr;

  // Only used by `scan analyze` and `scan sample`
  uint64 next_tuple_id_ = 0;
  // Only used by `scan analyze`
  uint64 prev_target_tuple_id_ = 0;
  // Only used by `scan analyze`
  uint64 target_tuple_id_ = 0;
  // Only used by `scan sample`
  uint64 fetch_tuple_id_ = 0;
  uint64 total_tuples_ = 0;

  // filter used to do column projection
  std::shared_ptr<PaxFilter> filter_ = nullptr;
#ifdef VEC_BUILD
  std::unique_ptr<VecAdapter> vec_adapter_;
#endif

  // used only by bitmap index scan: the TIDs of the bitmap's page
  std::unique_ptr<PaxIndexScanDesc> index_desc_;
  int cindex_ = 0;
  BlockNumber bm_block_ = InvalidBlockNumber;
  int bm_noffsets_ = 0;
  OffsetNumber bm_offsets_[TBM_MAX_TUPLES_PER_PAGE];
};  // class PaxScanDesc

}  // namespace pax
