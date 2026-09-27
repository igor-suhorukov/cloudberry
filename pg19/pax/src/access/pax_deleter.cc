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
 * pax_deleter.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_deleter.cc
 *
 *
 * Ported to PostgreSQL 19: the marks a subtransaction below the deleter's
 * own makes are logged, and forgotten as it aborts (access/pax_deleter.h).
 *-------------------------------------------------------------------------
 */

#include "access/pax_deleter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "access/pax_dml_state.h"
#include "access/paxc_rel_options.h"
#include "catalog/pax_catalog.h"
#include "comm/singleton.h"
#include "storage/pax_itemptr.h"
namespace pax {
CPaxDeleter::CPaxDeleter(Relation rel, Snapshot snapshot)
    : rel_(rel), snapshot_(snapshot) {
  delete_xid_ = GetCurrentTransactionId();
  Assert(TransactionIdIsValid(delete_xid_));

  use_visimap_ = true;
  base_subid_ = GetCurrentSubTransactionId();
}

TM_Result CPaxDeleter::DeleteTuple(Relation relation, ItemPointer tid,
                                   CommandId cid, Snapshot snapshot,
                                   TM_FailureData *tmfd) {
  TM_Result result;

  auto deleter =
      CPaxDmlStateLocal::Instance()->GetDeleter(relation, snapshot);
  Assert(deleter != nullptr);
  result = deleter->MarkDelete(tid);
  if (result == TM_SelfModified) {
    tmfd->cmax = cid;
  }
  return result;
}

// used for delete tuples
TM_Result CPaxDeleter::MarkDelete(ItemPointer tid) {
  uint32 tuple_offset = pax::GetTupleOffset(*tid);

  int block_id = MapToBlockNumber(rel_, *tid);
  SubTransactionId subid = GetCurrentSubTransactionId();

  if (block_bitmap_map_.find(block_id) == block_bitmap_map_.end()) {
    block_bitmap_map_[block_id] = std::make_shared<Bitmap8>();
    if (subid != base_subid_)
      sub_marks_.push_back({subid, block_id, kBlockMark});
    if (!use_visimap_) {
      cbdb::DeleteMicroPartitionEntry(RelationGetRelid(rel_), snapshot_,
                                      block_id);
    }
  }
  auto bitmap = block_bitmap_map_[block_id].get();
  if (bitmap->Test(tuple_offset)) {
    return TM_SelfModified;
  }
  bitmap->Set(tuple_offset);
  if (subid != base_subid_)
    sub_marks_.push_back({subid, block_id, tuple_offset});
  return TM_Ok;
}

// An aborted subtransaction's marks, cleared, the last first: a block its
// first mark brought in goes again.  A block deleted whole, without the
// visibility map, is undone with the catalog's row the abort brings back.
void CPaxDeleter::ForgetMarks(SubTransactionId subid) {
  for (auto it = sub_marks_.rbegin(); it != sub_marks_.rend(); ++it) {
    if (it->subid != subid) continue;
    auto entry = block_bitmap_map_.find(it->block_id);
    if (entry == block_bitmap_map_.end()) continue;
    if (it->offset == kBlockMark)
      block_bitmap_map_.erase(entry);
    else
      entry->second->Clear(it->offset);
  }
  sub_marks_.erase(std::remove_if(sub_marks_.begin(), sub_marks_.end(),
                                  [subid](const SubMark &m) {
                                    return m.subid == subid;
                                  }),
                   sub_marks_.end());
}

// A committed subtransaction's marks are its parent's; the deleter's own
// subtransaction's, once they reach it, need no log.
void CPaxDeleter::ReparentMarks(SubTransactionId subid,
                                SubTransactionId parent) {
  if (base_subid_ == subid) base_subid_ = parent;
  for (auto &m : sub_marks_)
    if (m.subid == subid) m.subid = parent;
  sub_marks_.erase(std::remove_if(sub_marks_.begin(), sub_marks_.end(),
                                  [this](const SubMark &m) {
                                    return m.subid == base_subid_;
                                  }),
                   sub_marks_.end());
}

bool CPaxDeleter::IsMarked(ItemPointerData tid) const {
  int block_id = MapToBlockNumber(rel_, tid);
  auto it = block_bitmap_map_.find(block_id);

  if (it == block_bitmap_map_.end()) return false;

  const auto &bitmap = it->second;
  uint32 tuple_offset = pax::GetTupleOffset(tid);
  return bitmap->Test(tuple_offset);
}

// used for merge remaining partition files, no tuple needs to delete
void CPaxDeleter::MarkDelete(BlockNumber pax_block_id) {
  int block_id = int(pax_block_id);

  if (block_bitmap_map_.find(block_id) == block_bitmap_map_.end()) {
    block_bitmap_map_[block_id] = std::make_shared<Bitmap8>();
    cbdb::DeleteMicroPartitionEntry(RelationGetRelid(rel_), snapshot_,
                                    block_id);
  }
}

void CPaxDeleter::ExecDelete() {
  if (block_bitmap_map_.empty()) return;

  TableDeleter table_deleter(rel_, block_bitmap_map_,
                             snapshot_);
  if (use_visimap_) {
    table_deleter.DeleteWithVisibilityMap(BuildDeleteIterator(), delete_xid_);
  } else {
    table_deleter.Delete(BuildDeleteIterator());
  }
}

std::unique_ptr<IteratorBase<MicroPartitionMetadata>>
CPaxDeleter::BuildDeleteIterator() {
  std::vector<pax::MicroPartitionMetadata> micro_partitions;
  auto rel_path = cbdb::BuildPaxDirectoryPath(
      rel_->rd_locator, rel_->rd_backend);
  for (auto &it : block_bitmap_map_) {
    std::string block_id = std::to_string(it.first);
    {
      pax::MicroPartitionMetadata meta_info;

      meta_info.SetFileName(cbdb::BuildPaxFilePath(rel_path, block_id));
      meta_info.SetMicroPartitionId(it.first);
      micro_partitions.push_back(std::move(meta_info));
    }
  }
  return std::make_unique<VectorIterator<MicroPartitionMetadata>>(
          std::move(micro_partitions));
}

}  // namespace pax
