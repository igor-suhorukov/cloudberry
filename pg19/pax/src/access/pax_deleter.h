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
 * pax_deleter.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_deleter.h
 *
 *
 * Ported to PostgreSQL 19: a deleter keeps its marks until its statement's
 * state is finished, and a statement of a trigger's marks rows in its outer
 * statement's deleter (access/pax_dml_state.cc), in a subtransaction of its
 * own where it has one -- so the marks a subtransaction made are known, and
 * forgotten as it aborts.
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "comm/bitmap.h"
#include "comm/pax_memory.h"
#include "storage/pax.h"

namespace pax {
class CPaxDeleter {
 public:
  explicit CPaxDeleter(Relation rel, Snapshot snapshot);
  ~CPaxDeleter() = default;
  static TM_Result DeleteTuple(Relation relation, ItemPointer tid,
                               CommandId cid, Snapshot snapshot,
                               TM_FailureData *tmfd);

  TM_Result MarkDelete(ItemPointer tid);
  bool IsMarked(ItemPointerData tid) const;
  void MarkDelete(BlockNumber pax_block_id);
  void ExecDelete();

  // The subtransaction the deleter was made in; its marks made since in a
  // subtransaction below that one, forgotten as it aborts, or its parent's
  // as it commits.
  SubTransactionId BaseSubId() const { return base_subid_; }
  void ForgetMarks(SubTransactionId subid);
  void ReparentMarks(SubTransactionId subid, SubTransactionId parent);

 private:
  // a mark a subtransaction made: a row's, or a block's first, of a block
  // the deleter had not marked before (offset kBlockMark)
  struct SubMark {
    SubTransactionId subid;
    int block_id;
    uint32 offset;
  };
  static constexpr uint32 kBlockMark = UINT32_MAX;

  std::unique_ptr<IteratorBase<MicroPartitionMetadata>> BuildDeleteIterator();
  std::map<int, std::shared_ptr<Bitmap8>> block_bitmap_map_;
  SubTransactionId base_subid_;
  std::vector<SubMark> sub_marks_;
  Relation rel_;
  Snapshot snapshot_;
  TransactionId delete_xid_;
  bool use_visimap_;
};  // class CPaxDeleter
}  // namespace pax
