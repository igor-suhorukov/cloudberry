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
 * pax_updater.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_updater.cc
 *
 *
 * Ported to PostgreSQL 19: a row this command has updated already -- a
 * join's second row for it -- says so as PAX's delete of it says, the
 * command in tmfd->cmax, which the executor compares with its own to pass
 * the row over, where it read what the struct held before.
 *-------------------------------------------------------------------------
 */

#include "access/pax_updater.h"

#include "access/pax_deleter.h"
#include "access/pax_dml_state.h"
#include "access/pax_inserter.h"

namespace pax {
TM_Result CPaxUpdater::UpdateTuple(
    const Relation relation, const ItemPointer otid, TupleTableSlot *slot,
    const CommandId cid, const Snapshot snapshot, const Snapshot /*crosscheck*/,
    const bool /*wait*/, TM_FailureData * tmfd,
    LockTupleMode * lockmode, TU_UpdateIndexes *update_indexes) {
  TM_Result result;

  auto dml_state = CPaxDmlStateLocal::Instance();
  auto deleter = dml_state->GetDeleter(relation, snapshot);
  auto inserter = dml_state->GetInserter(relation);

  Assert(deleter != nullptr);
  Assert(inserter != nullptr);

  *lockmode = LockTupleExclusive;
  result = deleter->MarkDelete(otid);

  if (result == TM_Ok) {
    inserter->InsertTuple(relation, slot, cid, 0, nullptr);
    *update_indexes = TU_All;
  } else {
    // FYI, ao ignores both tmfd and lockmode
    tmfd->ctid = *otid;
    if (result == TM_SelfModified) {
      tmfd->xmax = GetCurrentTransactionId();
      tmfd->cmax = cid;
    }
    *update_indexes = TU_None;
  }
  // TODO(gongxun): update pgstat info
  return result;
}
}  // namespace pax
