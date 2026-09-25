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
 * pax_dml_state.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_dml_state.h
 *
 *
 * Ported to PostgreSQL 19, whose executor has no dml_init and dml_fini: a
 * table's writer state is made as a statement first writes the table, and
 * belongs to the query running then, or to no query where a utility
 * statement writes -- COPY FROM; the module finishes it as that query
 * finishes, before its AFTER triggers, as a utility statement ends, at
 * finish_bulk_insert, and before a commit, and drops it, unwritten, where
 * its subtransaction aborts (access/pax_access_handle.cc).
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

#include <memory>
#include <vector>

#include "access/pax_deleter.h"
#include "access/pax_inserter.h"
#include "comm/cbdb_wrappers.h"
#include "comm/singleton.h"

namespace pax {
struct PaxDmlState {
  Oid oid;
  CPaxInserter *inserter;
  CPaxDeleter *deleter;
};

class CPaxDmlStateLocal final {
  friend class Singleton<CPaxDmlStateLocal>;

 public:
  static CPaxDmlStateLocal *Instance() {
    return Singleton<CPaxDmlStateLocal>::GetInstance();
  }

  ~CPaxDmlStateLocal() = default;

  void InitDmlState(Relation rel, CmdType operation);
  void FinishDmlState(Relation rel, CmdType operation);

  // The query ExecutorRun is running now, whose statement the states made
  // meanwhile are, or NULL outside one.
  static void PushOwner(const void *owner);
  static void PopOwner(const void *owner);

  // Finish the states owner's statement made, in this subtransaction.
  void FinishOwned(const void *owner);
  // Finish every state: a transaction about to commit.
  void FinishAll();
  // Drop, unwritten, the states subid made, or every state with
  // InvalidSubTransactionId: an abort.
  void Forget(SubTransactionId subid);
  // A subtransaction committed: its states are its parent's.
  void Reparent(SubTransactionId subid, SubTransactionId parent);
  // A table being dropped: its states go, unwritten.
  void ForgetRelation(Oid relid);

  bool IsInitialized() const { return cbdb::pax_memory_context != nullptr; }
  CPaxInserter *GetInserter(Relation rel);
  CPaxDeleter *GetDeleter(Relation rel, Snapshot snapshot,
                          bool missing_null = false);

  void Reset();

  CPaxDmlStateLocal(const CPaxDmlStateLocal &) = delete;
  CPaxDmlStateLocal &operator=(const CPaxDmlStateLocal &) = delete;

 private:
  struct DmlStateValue {
    std::unique_ptr<CPaxInserter> inserter;
    std::unique_ptr<CPaxDeleter> deleter;
    const void *owner = nullptr;
    SubTransactionId subid = InvalidSubTransactionId;
  };

  void FinishState(Oid oid, std::shared_ptr<DmlStateValue> state);

  CPaxDmlStateLocal();
  static void DmlStateResetCallback(void * /*arg*/);

  std::shared_ptr<DmlStateValue> FindDmlState(const Oid &oid);
  std::shared_ptr<DmlStateValue> RemoveDmlState(const Oid &oid);

 private:
  std::unordered_map<Oid, std::shared_ptr<DmlStateValue>> dml_descriptor_tab_;
  static std::vector<const void *> owners_;
  Oid last_oid_;
  std::shared_ptr<DmlStateValue> last_state_;

  MemoryContextCallback cb_;
};

}  // namespace pax
