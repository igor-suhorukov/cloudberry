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
 * pax_vexec_sink.cc
 *	  PAX's batch sink: an INSERT's rows handed to a PAX table by vexec, the
 *	  vectorized executor, a batch of columns at a time, through the sink
 *	  contract of vexec_sink.h (pg_vector_executor.md §3.16).
 *
 * The port's own file; Cloudberry has none like it.  vexec's VecInsert
 * finds the routine by PAX's table access method, asks which layout each
 * attribute's values come in (supports()), and hands each batch to put(),
 * where it would otherwise form rows for table_multi_insert().  The batch
 * goes into the same writer the method's inserts go into -- the
 * statement's, which access/pax_dml_state.cc makes at its first write and
 * finishes as the statement finishes -- through the port's
 * CPaxInserter::InsertBatch(), TableWriter::WriteBatch() and
 * OrcWriter::WriteBatch(): each column's values are appended into the
 * group's columns a column at a time, the group's statistics kept a column
 * at a time, and groups and files closed where a row at a time closes
 * them.
 *
 * Layouts, by the table's storage format (TableWriter::InitOptionsCaches()):
 *
 *					porc, PAX's default				porc_vec
 *	by-value		FIXED, the type's width			the same
 *	bool			BYTE_BOOL						the same
 *	fixed-length	FIXED at PostgreSQL's array		the same
 *	by-reference	stride
 *	numeric			DATUM, headered					SCALED where its typmod's
 *													precision fits PAX's 16
 *													bytes (35 digits), the
 *													integer at its scale;
 *													DATUM otherwise
 *	text, varchar,	DATUM, headered					OFFSETS: the bytes without
 *	bpchar, bytea									a header, which a porc_vec
 *													column holds
 *	other varlena	DATUM, headered					the same
 *
 * as the column holds them, so that vexec converts what its batch holds
 * otherwise, and PAX writes it as a row's would be written: a by-value
 * value's bytes, a porc_vec numeric's 16 bytes made from the integer
 * without a numeric made, a porc_vec string's bytes without a header, and
 * PAX's toast where its storage and size ask for one.
 *
 * TIDs: each row's, the table's layout -- PaxTidToTable() of the file and
 * the row's offset in it -- as the method hands a row's out after
 * tuple_insert (access/pax_access_handle.cc).
 *
 * Errors: every callback is C, runs on the backend's main thread and may
 * raise: PAX's C++ exceptions become PostgreSQL errors at this boundary, as
 * at the access method's own entry points (CBDB_TRY ... CBDB_END_TRY).  An
 * aborted statement's writer is dropped with its DML state, unwritten, as
 * an aborted INSERT's is.
 *
 *-------------------------------------------------------------------------
 */

#include "comm/cbdb_api.h"

#include "access/pax_dml_state.h"
#include "access/pax_inserter.h"
#include "access/paxc_rel_options.h"
#include "comm/cbdb_wrappers.h"
#include "comm/guc.h"
#include "comm/vec_numeric.h"
#include "exceptions/CException.h"
#include "storage/pax_defined.h"
#include "pax_module.h"
#include "pax_tid.h"

extern "C" {
#include "vexec_sink.h"
}

namespace {

// What a statement's writes into a PAX table hold between calls.
struct PaxSinkState {
  Relation rel;
  int file_bits;  // the table's TID layout (pax_tid.h)
};

// The table's storage format, as its writer reads it.
bool IsPorcVec(Relation rel) {
  return pax::StorageFormatKeyToPaxStorageFormat(RelationGetOptions(
             rel, storage_format,
             pax::pax_default_storage_format ? pax::pax_default_storage_format
                                             : STORAGE_FORMAT_TYPE_DEFAULT)) ==
         pax::PaxStorageFormat::kTypeStoragePorcVec;
}

void SetLayout(VexecSinkLayout *layout, uint8 kind, int32 width, int32 stride,
               int16 scale) {
  memset(layout, 0, sizeof(VexecSinkLayout));
  layout->layout = kind;
  layout->width = width;
  layout->stride = stride;
  layout->scale = scale;
}

}  // namespace

extern "C" {

// The layout of the table above for an attribute.
static bool PaxVexecSinkSupports(Relation rel, AttrNumber attnum,
                                 VexecSinkLayout *layout) {
  Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
  bool vec = false;

  CBDB_TRY();
  { vec = IsPorcVec(rel); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  if (att->attbyval) {
    if (att->atttypid == BOOLOID)
      SetLayout(layout, VEXEC_BYTE_BOOL, 1, 1, 0);
    else
      SetLayout(layout, VEXEC_FIXED, att->attlen, att->attlen, 0);
    return true;
  }
  if (att->attlen > 0) {
    SetLayout(layout, VEXEC_FIXED, att->attlen,
              att_align_nominal(att->attlen, att->attalign), 0);
    return true;
  }
  if (att->attlen != -1) return false;  // a cstring: no PAX column holds it

  if (vec && att->atttypid == NUMERICOID && att->atttypmod >= (int32)VARHDRSZ) {
    int32 precision = ((att->atttypmod - VARHDRSZ) >> 16) & 0xffff;
    int32 scale = (att->atttypmod - VARHDRSZ) & 0xffff;

    if (precision <= VEC_SHORT_NUMERIC_MAX_PRECISION && scale <= precision) {
      SetLayout(layout, VEXEC_SCALED, precision <= 18 ? 8 : 16,
                precision <= 18 ? 8 : 16, (int16)scale);
      return true;
    }
  }
  if (vec && (att->atttypid == TEXTOID || att->atttypid == VARCHAROID ||
              att->atttypid == BPCHAROID || att->atttypid == BYTEAOID)) {
    SetLayout(layout, VEXEC_OFFSETS, 0, 0, 0);
    return true;
  }
  SetLayout(layout, VEXEC_DATUM, 0, 0, 0);
  return true;
}

// The statement's writer into the table, made as its first row's would be.
static void *PaxVexecSinkBegin(Relation rel, const VexecSinkSpec *spec) {
  auto state = static_cast<PaxSinkState *>(palloc0(sizeof(PaxSinkState)));

  (void)spec;
  state->rel = rel;
  CBDB_TRY();
  {
    state->file_bits = cbdb::PaxTableFileBits(rel);
    (void)pax::CPaxDmlStateLocal::Instance()->GetInserter(rel);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  return state;
}

// A batch into the writer, in the memory the writer writes in, as
// CCPaxAccessMethod::MultiInsert() writes rows.
static void PaxVexecSinkPut(void *state, VexecSinkBatch *batch) {
  auto s = static_cast<PaxSinkState *>(state);

  CHECK_FOR_INTERRUPTS();
  CBDB_TRY();
  {
    auto inserter = pax::CPaxDmlStateLocal::Instance()->GetInserter(s->rel);
    MemoryContext old_ctx;

    Assert(cbdb::pax_memory_context);
    old_ctx = MemoryContextSwitchTo(cbdb::pax_memory_context);
    inserter->InsertBatch(s->rel, batch->columns, batch->nrows, batch->tids);
    MemoryContextSwitchTo(old_ctx);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  if (batch->tids)
    for (int i = 0; i < batch->nrows; i++)
      batch->tids[i] = PaxTidToTable(batch->tids[i], s->file_bits);
  pgstat_count_heap_insert(s->rel, batch->nrows);
}

// The statement is done with the table: its writer finished, as
// finish_bulk_insert finishes it after table_multi_insert().
static void PaxVexecSinkEnd(void *state) {
  auto s = static_cast<PaxSinkState *>(state);

  CBDB_TRY();
  { pax::CPaxInserter::FinishBulkInsert(s->rel, 0); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
  pfree(s);
}

static VexecSinkRoutine pax_vexec_sink;

/*
 * The routine pax.c registers from _PG_init (pax_module.h), keyed by PAX's
 * table access method, and named as EXPLAIN shows a VecInsert's write.
 */
const VexecSinkRoutine *PaxVexecSink(void) {
  if (pax_vexec_sink.size == 0) {
    pax_vexec_sink.size = sizeof(VexecSinkRoutine);
    pax_vexec_sink.minor = VEXEC_SINK_MINOR;
    pax_vexec_sink.am = PaxTableAmRoutine();
    pax_vexec_sink.name = "pax";
    pax_vexec_sink.supports = PaxVexecSinkSupports;
    pax_vexec_sink.begin = PaxVexecSinkBegin;
    pax_vexec_sink.put = PaxVexecSinkPut;
    pax_vexec_sink.end = PaxVexecSinkEnd;
  }
  return &pax_vexec_sink;
}

}  // extern "C"
