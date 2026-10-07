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
 * pax_inserter.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_inserter.h
 *
 *
 * Ported to PostgreSQL 19: the micro-partition the writer writes now, whose
 * rows are in memory still until it is finished, for a fetch of one of them
 * by its TID (access/pax_dml_state.cc).
 *
 * From the port: InsertBatch(), a batch's rows from vexec's sink
 * (access/pax_vexec_sink.cc, pg_vector_executor.md §3.16).
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

#include "storage/micro_partition_metadata.h"
#include "storage/pax.h"
namespace pax {
class PartitionObject;
class CPaxInserter {
 public:
  explicit CPaxInserter(Relation rel);
  virtual ~CPaxInserter() = default;

  static void TupleInsert(Relation relation, TupleTableSlot *slot,
                          CommandId cid, int options, BulkInsertState bistate);

  static void MultiInsert(Relation relation, TupleTableSlot **slots,
                          int ntuples, CommandId cid, int options,
                          BulkInsertState bistate);

  static void FinishBulkInsert(Relation relation, int options);

  void InsertTuple(Relation relation, TupleTableSlot *slot, CommandId cid,
                   int options, BulkInsertState bistate);

  // The port's, for vexec's sink: a batch's rows, a VexecColumn an
  // attribute, written as InsertTuple() writes rows; each row's TID, PAX's
  // inside, into tids[] where it is given.
  void InsertBatch(Relation relation, const VexecColumn *columns, int nrows,
                   ItemPointerData *tids);
  void FinishInsert();

  // the micro-partition the writer is writing, InvalidBlockNumber if none
  BlockNumber WritingBlock() const {
    return writer_ ? writer_->GetBlockNumber() : InvalidBlockNumber;
  }

 private:
  Relation rel_;
  uint32 insert_count_;

  std::unique_ptr<TableWriter> writer_;
};  // class CPaxInserter

}  // namespace pax
