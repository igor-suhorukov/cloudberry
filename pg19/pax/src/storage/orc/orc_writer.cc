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
 * orc_writer.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/storage/orc/orc_writer.cc
 *
 *
 * Ported to PostgreSQL 19: a tuple descriptor's attributes are reached
 * through TupleDescAttr(), PostgreSQL 19's TupleDescData having no attrs[]
 * array.
 *
 * From the port: WriteBatch(), a batch's columns from vexec's sink
 * (access/pax_vexec_sink.cc, pg_vector_executor.md §3.16) written a column
 * at a time into the group's columns, each value as WriteTuple() and
 * PrepareWriteTuple() write a row's -- PAX's toasts made of the values
 * their storage and size ask one of -- and the group's statistics kept a
 * column at a time (micro_partition_stats.h).
 *-------------------------------------------------------------------------
 */

#include "comm/cbdb_api.h"

#include <algorithm>
#include <tuple>

#include "comm/cbdb_wrappers.h"
#include "comm/fmt.h"
#include "comm/guc.h"
#include "comm/log.h"
#include "comm/pax_memory.h"
#include "comm/vec_numeric.h"
#include "storage/columns/pax_column_traits.h"
#include "storage/columns/pax_vec_numeric_column.h"
#include "storage/local_file_system.h"
#include "storage/micro_partition_stats.h"
#include "storage/orc/orc_defined.h"
#include "storage/orc/orc_group.h"
#include "storage/orc/orc_type.h"
#include "storage/orc/porc.h"
#include "storage/pax_itemptr.h"
#include "storage/toast/pax_toast.h"
#include "storage/wal/pax_wal.h"
#include "storage/wal/paxc_wal.h"

extern "C" {
#include "vexec_source.h"
}

namespace pax {

std::vector<pax::porc::proto::Type_Kind> OrcWriter::BuildSchema(TupleDesc desc,
                                                                bool is_vec) {
  std::vector<pax::porc::proto::Type_Kind> type_kinds;
  for (int i = 0; i < desc->natts; i++) {
    auto attr = TupleDescAttr(desc, i);
    type_kinds.emplace_back(ConvertPgTypeToPorcType(attr, is_vec));
  }

  return type_kinds;
}

static std::unique_ptr<PaxColumn> CreateDecimalColumn(
    bool is_vec, const PaxEncoder::EncodingOption &opts) {
  if (is_vec) {
    return traits::ColumnOptCreateTraits2<
        PaxShortNumericColumn>::create_encoding(DEFAULT_CAPACITY, opts);
  } else {
    return traits::ColumnOptCreateTraits2<PaxPgNumericColumn>::create_encoding(
        DEFAULT_CAPACITY, DEFAULT_CAPACITY, opts);
  }
}

static std::unique_ptr<PaxColumn> CreateBpCharColumn(
    bool is_vec, const PaxEncoder::EncodingOption &opts) {
  if (is_vec)
    return traits::ColumnOptCreateTraits2<PaxVecBpCharColumn>::create_encoding(
        DEFAULT_CAPACITY, DEFAULT_CAPACITY, opts);

  return traits::ColumnOptCreateTraits2<PaxBpCharColumn>::create_encoding(
      DEFAULT_CAPACITY, DEFAULT_CAPACITY, opts);
}

static std::unique_ptr<PaxColumn> CreateBitPackedColumn(
    bool is_vec, const PaxEncoder::EncodingOption &opts) {
  if (is_vec)
    return traits::ColumnOptCreateTraits2<
        PaxVecBitPackedColumn>::create_encoding(DEFAULT_CAPACITY, opts);

  return traits::ColumnOptCreateTraits2<PaxBitPackedColumn>::create_encoding(
      DEFAULT_CAPACITY, opts);
}

template <typename N>
static std::unique_ptr<PaxColumn> CreateCommColumn(
    bool is_vec, const PaxEncoder::EncodingOption &opts) {
  if (is_vec)
    return traits::ColumnOptCreateTraits<PaxVecEncodingColumn,
                                         N>::create_encoding(DEFAULT_CAPACITY,
                                                             opts);

  return traits::ColumnOptCreateTraits<PaxEncodingColumn, N>::create_encoding(
      DEFAULT_CAPACITY, opts);
}

static std::unique_ptr<PaxColumns> BuildColumns(
    const std::vector<pax::porc::proto::Type_Kind> &types, const TupleDesc desc,
    const std::vector<std::tuple<ColumnEncoding_Kind, int>>
        &column_encoding_types,
    const PaxStorageFormat &storage_format) {
  std::unique_ptr<PaxColumns> columns;
  bool is_vec;

  columns = std::make_unique<PaxColumns>();
  is_vec = (storage_format == PaxStorageFormat::kTypeStoragePorcVec);
  columns->SetStorageFormat(storage_format);

  for (size_t i = 0; i < types.size();
       i++) {  // already checked types.size() == desc->nattrs
    auto type = types[i];
    auto attr = TupleDescAttr(desc, i);
    std::unique_ptr<PaxColumn> column = nullptr;
    size_t align_size;

    PaxEncoder::EncodingOption encoding_option;
    encoding_option.column_encode_type = std::get<0>(column_encoding_types[i]);
    encoding_option.is_sign = true;
    encoding_option.compress_level = std::get<1>(column_encoding_types[i]);

    encoding_option.offsets_encode_type = ColumnEncoding_Kind_DIRECT_DELTA;

    switch (type) {
      case (pax::porc::proto::Type_Kind::Type_Kind_STRING): {
        encoding_option.is_sign = false;
        if (is_vec) {
          column =
              traits::ColumnOptCreateTraits2<PaxVecNonFixedEncodingColumn>::
                  create_encoding(DEFAULT_CAPACITY, DEFAULT_CAPACITY,
                                  std::move(encoding_option));

        } else {
          column = traits::ColumnOptCreateTraits2<
              PaxNonFixedEncodingColumn>::create_encoding(DEFAULT_CAPACITY,
                                                          DEFAULT_CAPACITY,
                                                          std::move(
                                                              encoding_option));
        }

        break;
      }
      case (pax::porc::proto::Type_Kind::Type_Kind_VECNOHEADER): {
        Assert(is_vec);
        column =
            traits::ColumnOptCreateTraits2<PaxVecNoHdrColumn>::create_encoding(
                DEFAULT_CAPACITY, DEFAULT_CAPACITY, std::move(encoding_option));
        break;
      }
      case (pax::porc::proto::Type_Kind::Type_Kind_VECBPCHAR):
      case (pax::porc::proto::Type_Kind::Type_Kind_BPCHAR): {
        column = CreateBpCharColumn(is_vec, std::move(encoding_option));
        break;
      }
      case (pax::porc::proto::Type_Kind::Type_Kind_VECDECIMAL):
      case (pax::porc::proto::Type_Kind::Type_Kind_DECIMAL): {
        AssertImply(is_vec,
                    type == pax::porc::proto::Type_Kind::Type_Kind_VECDECIMAL);
        AssertImply(!is_vec,
                    type == pax::porc::proto::Type_Kind::Type_Kind_DECIMAL);
        column = CreateDecimalColumn(is_vec, std::move(encoding_option));
        break;
      }
      case (pax::porc::proto::Type_Kind::Type_Kind_BOOLEAN):
        column = CreateBitPackedColumn(is_vec, std::move(encoding_option));
        break;
      case (pax::porc::proto::Type_Kind::Type_Kind_BYTE):  // len 1 integer
        column = CreateCommColumn<int8>(is_vec, std::move(encoding_option));
        break;
      case (pax::porc::proto::Type_Kind::Type_Kind_SHORT):  // len 2 integer
        column = CreateCommColumn<int16>(is_vec, std::move(encoding_option));
        break;
      case (pax::porc::proto::Type_Kind::Type_Kind_INT):  // len 4 integer
        column = CreateCommColumn<int32>(is_vec, std::move(encoding_option));
        break;
      case (pax::porc::proto::Type_Kind::Type_Kind_LONG):  // len 8 integer
        column = CreateCommColumn<int64>(is_vec, std::move(encoding_option));
        break;
      default:
        CBDB_RAISE(cbdb::CException::ExType::kExTypeLogicError,
                   fmt("Invalid PORC PB [type=%d]", type));
        break;
    }

    Assert(column);

    switch (attr->attalign) {
      case TYPALIGN_SHORT:
        align_size = ALIGNOF_SHORT;
        break;
      case TYPALIGN_INT:
        align_size = ALIGNOF_INT;
        break;
      case TYPALIGN_DOUBLE:
        align_size = ALIGNOF_DOUBLE;
        break;
      case TYPALIGN_CHAR:
        align_size = PAX_DATA_NO_ALIGN;
        break;
      default:
        CBDB_RAISE(cbdb::CException::ExType::kExTypeLogicError,
                   fmt("Invalid attribute [attalign=%c]", attr->attalign));
    }

    column->SetAlignSize(align_size);
    columns->Append(std::move(column));
  }

  return columns;
}

OrcWriter::OrcWriter(
    const MicroPartitionWriter::WriterOptions &writer_options,
    const std::vector<pax::porc::proto::Type_Kind> &column_types,
    std::unique_ptr<File> file, std::unique_ptr<File> toast_file)
    : MicroPartitionWriter(writer_options),
      is_closed_(false),
      column_types_(column_types),
      file_(std::move(file)),
      toast_file_(std::move(toast_file)),
      current_written_phy_size_(0),
      row_index_(0),
      total_rows_(0),
      current_offset_(0),
      current_toast_file_offset_(0),
      group_stats_(writer_options.rel_tuple_desc) {
  Assert(writer_options.rel_tuple_desc);
  Assert(writer_options.rel_tuple_desc->natts ==
         static_cast<int>(column_types.size()));

  pax_columns_ =
      BuildColumns(column_types_, writer_options.rel_tuple_desc,
                   writer_options.encoding_opts, writer_options.storage_format);

  summary_.rel_oid = writer_options.rel_oid;
  summary_.block_id = writer_options.block_id;
  summary_.file_name = writer_options.file_name;

  post_script_.set_footerlength(0);
  post_script_.set_majorversion(PAX_MAJOR_VERSION);
  post_script_.set_minorversion(PAX_MINOR_VERSION);
  post_script_.set_writer(PORC_WRITER_ID);
  post_script_.set_magic(PORC_MAGIC_ID);

  group_stats_.Initialize(writer_options.enable_min_max_col_idxs,
                          writer_options.enable_bf_col_idxs);

  // Precompute slowpath indices for varlena columns (non-byval and typlen == -1)
  varlena_slowpath_indices_.clear();
  varlena_slowpath_indices_.reserve(writer_options.rel_tuple_desc->natts);
  for (int i = 0; i < writer_options.rel_tuple_desc->natts; ++i) {
    auto attrs = TupleDescAttr(writer_options.rel_tuple_desc, i);
    if (!attrs->attbyval && attrs->attlen == -1) {
      varlena_slowpath_indices_.push_back(i);
    }
  }
}

OrcWriter::~OrcWriter() {}

void OrcWriter::Flush() {
  BufferedOutputStream buffer_mem_stream(2048);
  DataBuffer<char> toast_mem(nullptr, 0, true, false);

  if (WriteStripe(&buffer_mem_stream, &toast_mem)) {
    std::unique_ptr<PaxColumns> new_columns;
    current_written_phy_size_ += pax_columns_->PhysicalSize();
    Assert(current_offset_ >= buffer_mem_stream.GetDataBuffer()->Used());
    summary_.file_size += buffer_mem_stream.GetDataBuffer()->Used();
    file_->PWriteN(buffer_mem_stream.GetDataBuffer()->GetBuffer(),
                   buffer_mem_stream.GetDataBuffer()->Used(),
                   current_offset_ - buffer_mem_stream.GetDataBuffer()->Used());
    // TODO(gongxun): only pax.so and needWAL(temp table and unlogger table
    // should not write wal) should write wal log, paxformat.so should not write
    // wal
    if (writer_options_.need_wal) {
      cbdb::XLogPaxInsert(
          writer_options_.node, writer_options_.block_id.c_str(),
          current_offset_ - buffer_mem_stream.GetDataBuffer()->Used(),
          buffer_mem_stream.GetDataBuffer()->GetBuffer(),
          buffer_mem_stream.GetDataBuffer()->Used());
    }
    if (toast_mem.GetBuffer()) {
      Assert(toast_file_);
      Assert(current_toast_file_offset_ >= toast_mem.Used());
      toast_file_->PWriteN(toast_mem.GetBuffer(), toast_mem.Used(),
                           current_toast_file_offset_ - toast_mem.Used());
      if (writer_options_.need_wal) {
        std::string toast_file_name =
            writer_options_.block_id + TOAST_FILE_SUFFIX;
        cbdb::XLogPaxInsert(writer_options_.node, toast_file_name.c_str(),
                            current_toast_file_offset_ - toast_mem.Used(),
                            toast_mem.GetBuffer(), toast_mem.Used());
      }
    }

    new_columns = BuildColumns(column_types_, writer_options_.rel_tuple_desc,
                               writer_options_.encoding_opts,
                               writer_options_.storage_format);

    for (size_t i = 0; i < column_types_.size(); ++i) {
      auto old_column = (*pax_columns_)[i].get();
      auto new_column = (*new_columns)[i].get();

      Assert(old_column && new_column);
      if (old_column->HasAttributes()) {
        // TODO(jiaqizho): consider use std::map::swap here
        new_column->SetAttributes(old_column->GetAttributes());
      }
    }

    pax_columns_ = std::move(new_columns);
  }
}

std::vector<std::pair<int, Datum>> OrcWriter::PrepareWriteTuple(
    TupleTableSlot *table_slot) {
  TupleDesc tuple_desc;
  bool is_null;
  Datum tts_value;
  char type_storage;
  struct varlena *tts_value_vl = nullptr, *detoast_vl = nullptr;
  std::vector<std::pair<int, Datum>> detoast_map;

  tuple_desc = writer_options_.rel_tuple_desc;
  Assert(tuple_desc);
  const auto &required_stats_cols = group_stats_.GetRequiredStatsColsMask();

  for (int i : varlena_slowpath_indices_) {
    bool save_origin_datum;
    auto attrs = TupleDescAttr(tuple_desc, i);
    is_null = table_slot->tts_isnull[i];
    tts_value = table_slot->tts_values[i];
    type_storage = attrs->attstorage;

    AssertImply(attrs->attisdropped, is_null);

    if (is_null) {
      continue;
    }

    // prepare toast
    tts_value_vl = (struct varlena *)DatumGetPointer(tts_value);

    // Once passin toast is compress toast and datum is within the range
    // allowed by PAX, then PAX will direct store it
    if (VARATT_IS_COMPRESSED(tts_value_vl)) {
      auto compress_toast_extsize =
          VARDATA_COMPRESSED_GET_EXTSIZE(tts_value_vl);

      if (type_storage != TYPSTORAGE_PLAIN &&
          !VARATT_CAN_MAKE_PAX_EXTERNAL_TOAST_BY_SIZE(compress_toast_extsize) &&
          VARATT_CAN_MAKE_PAX_COMPRESSED_TOAST_BY_SIZE(
              compress_toast_extsize)) {
        continue;
      }
    }

    save_origin_datum = false;

    // if not in required_stats_cols, then we allow datum with short header
    // Numeric always need ensure that with the 4B header, otherwise it will
    // be converted twice in the vectorization path.
    if (required_stats_cols[i] || VARATT_IS_COMPRESSED(tts_value_vl) ||
        VARATT_IS_EXTERNAL(tts_value_vl)
#ifdef VEC_BUILD
        || attrs->atttypid == NUMERICOID
#endif
    ) {
      // still detoast the origin toast
      detoast_vl = cbdb::PgDeToastDatum(tts_value_vl);
      Assert(detoast_vl != nullptr);
    } else {
      detoast_vl = tts_value_vl;
    }

    if (tts_value_vl != detoast_vl) {
      table_slot->tts_values[i] = PointerGetDatum(detoast_vl);
      detoast_memory_holder_.emplace_back(detoast_vl);
      save_origin_datum = true;
      detoast_map.emplace_back(
          std::pair<int, Datum>{i, PointerGetDatum(detoast_vl)});
    }

    if (pax_enable_toast && type_storage != TYPSTORAGE_PLAIN) {
      Datum pax_toast_datum;
      // only make toast here
      std::shared_ptr<MemoryObject> mobj = nullptr;

      std::tie(pax_toast_datum, mobj) =
          pax_make_toast(PointerGetDatum(detoast_vl), type_storage);

      if (mobj) {
        Assert(pax_toast_datum != 0);
        toast_memory_holder_.emplace_back(std::move(mobj));
        table_slot->tts_values[i] = pax_toast_datum;
        save_origin_datum = true;
      }
    }

    if (save_origin_datum)
      origin_datum_holder_.emplace_back(std::pair<int, Datum>{i, tts_value});
  }

  return detoast_map;
}

void OrcWriter::WriteTuple(TupleTableSlot *table_slot) {
  int natts;
  TupleDesc tuple_desc;
  int16 type_len;
  bool type_by_val;
  bool is_null;
  Datum tts_value;
  struct varlena *tts_value_vl = nullptr;

  SIMPLE_FAULT_INJECTOR("orc_writer_write_tuple");

  auto detoast_map = PrepareWriteTuple(table_slot);

  // The reason why
  tuple_desc = writer_options_.rel_tuple_desc;
  Assert(tuple_desc);

  SetTupleOffset(&table_slot->tts_tid, row_index_++);
  natts = tuple_desc->natts;

  CBDB_CHECK(
      pax_columns_->GetColumns() == static_cast<size_t>(natts),
      cbdb::CException::ExType::kExTypeSchemaNotMatch,
      fmt("The number of column in memory not match the in TupleDesc, "
          "[in mem=%lu, in desc=%d], \n %s",
          pax_columns_->GetColumns(), natts, file_->DebugString().c_str()));

  for (int i = 0; i < natts; i++) {
    type_len = TupleDescAttr(tuple_desc, i)->attlen;
    type_by_val = TupleDescAttr(tuple_desc, i)->attbyval;
    is_null = table_slot->tts_isnull[i];
    tts_value = table_slot->tts_values[i];

    AssertImply(TupleDescAttr(tuple_desc, i)->attisdropped, is_null);

    if (is_null) {
      (*pax_columns_)[i]->AppendNull();
      continue;
    }

    if (type_by_val) {
      switch (type_len) {
        case 1: {
          auto value = cbdb::DatumToInt8(tts_value);
          (*pax_columns_)[i]->Append(reinterpret_cast<char *>(&value),
                                     type_len);
          break;
        }
        case 2: {
          auto value = cbdb::DatumToInt16(tts_value);
          (*pax_columns_)[i]->Append(reinterpret_cast<char *>(&value),
                                     type_len);
          break;
        }
        case 4: {
          auto value = cbdb::DatumToInt32(tts_value);
          (*pax_columns_)[i]->Append(reinterpret_cast<char *>(&value),
                                     type_len);
          break;
        }
        case 8: {
          auto value = cbdb::DatumToInt64(tts_value);
          (*pax_columns_)[i]->Append(reinterpret_cast<char *>(&value),
                                     type_len);
          break;
        }
        default:
          Assert(!"should not be here! pg_type which attbyval=true only have typlen of "
                  "1, 2, 4, or 8 ");
      }
    } else {
      switch (type_len) {
        case -1: {
          tts_value_vl = (struct varlena *)DatumGetPointer(tts_value);
          if (COLUMN_STORAGE_FORMAT_IS_VEC(pax_columns_)) {
            // NUMERIC requires a complete Datum
            // It won't get a toast
            if (TupleDescAttr(tuple_desc, i)->atttypid == NUMERICOID) {
              Assert((*pax_columns_)[i]->GetPaxColumnTypeInMem() ==
                     PaxColumnTypeInMem::kTypeVecDecimal);
              Assert(!VARATT_IS_PAX_SUPPORT_TOAST(tts_value_vl));
              (*pax_columns_)[i]->Append(reinterpret_cast<char *>(tts_value_vl),
                                         VARSIZE_ANY(tts_value_vl));

            } else {
              if (VARATT_IS_PAX_SUPPORT_TOAST(tts_value_vl)) {
                (*pax_columns_)[i]->AppendToast(
                    reinterpret_cast<char *>(tts_value_vl),
                    PAX_VARSIZE_ANY(tts_value_vl));
              } else {
                (*pax_columns_)[i]->Append(VARDATA_ANY(tts_value_vl),
                                           VARSIZE_ANY_EXHDR(tts_value_vl));
              }
            }
          } else {
            if (VARATT_IS_PAX_SUPPORT_TOAST(tts_value_vl)) {
              (*pax_columns_)[i]->AppendToast(
                  reinterpret_cast<char *>(tts_value_vl),
                  PAX_VARSIZE_ANY(tts_value_vl));
            } else {
              (*pax_columns_)[i]->Append(reinterpret_cast<char *>(tts_value_vl),
                                         VARSIZE_ANY(tts_value_vl));
            }
          }

          break;
        }
        default:
          Assert(type_len > 0);
          (*pax_columns_)[i]->Append(
              static_cast<char *>(cbdb::DatumToPointer(tts_value)), type_len);
          break;
      }
    }
  }

  summary_.num_tuples++;
  pax_columns_->AddRows(1);
  for (const auto &pair : detoast_map)
    table_slot->tts_values[pair.first] = pair.second;

  group_stats_.AddRow(table_slot);

  EndWriteTuple(table_slot);
}

// A batch's rows, a group's room at a time: each column's values appended
// into the group's columns, then the rows counted, numbered and flushed
// with the group where it fills, as EndWriteTuple() flushes it.
void OrcWriter::WriteBatch(const VexecColumn *columns, size_t start,
                           size_t nrows, ItemPointerData *tids) {
  TupleDesc tuple_desc = writer_options_.rel_tuple_desc;
  size_t done = 0;

  CBDB_CHECK(
      pax_columns_->GetColumns() == static_cast<size_t>(tuple_desc->natts),
      cbdb::CException::ExType::kExTypeSchemaNotMatch,
      fmt("The number of column in memory not match the in TupleDesc, "
          "[in mem=%lu, in desc=%d], \n %s",
          pax_columns_->GetColumns(), tuple_desc->natts,
          file_->DebugString().c_str()));

  while (done < nrows) {
    size_t in_group = pax_columns_->GetRows();
    size_t n;

    if (in_group >= writer_options_.group_limit) {
      Flush();
      continue;
    }
    n = std::min(writer_options_.group_limit - in_group, nrows - done);
    for (int i = 0; i < tuple_desc->natts; i++)
      WriteBatchColumn(i, columns[i], start + done, n);
    for (size_t k = 0; k < n; k++) {
      if (tids) SetTupleOffset(&tids[done + k], row_index_);
      row_index_++;
    }
    summary_.num_tuples += n;
    pax_columns_->AddRows(n);
    done += n;
    if (pax_columns_->GetRows() >= writer_options_.group_limit) Flush();
  }
}

// A varlena value, with its header, as PrepareWriteTuple() and WriteTuple()
// write a row's: PAX's toast made of it where its storage asks for one and
// it is large enough, else the value -- without its header in porc_vec,
// with it in porc.  The sink's values are never external or compressed.
void OrcWriter::AppendBatchVarlena(int column_index, PaxColumn *column,
                                   varlena *value, bool header_stripped) {
  auto attr = TupleDescAttr(writer_options_.rel_tuple_desc, column_index);

  if (pax_enable_toast && attr->attstorage != TYPSTORAGE_PLAIN) {
    Datum toast;
    std::shared_ptr<MemoryObject> mobj;

    std::tie(toast, mobj) =
        pax_make_toast(PointerGetDatum(value), attr->attstorage);
    if (mobj) {
      // the toast's bytes, and an external one's data, are copied in
      auto toast_vl = reinterpret_cast<varlena *>(DatumGetPointer(toast));
      column->AppendToast(reinterpret_cast<char *>(toast_vl),
                          PAX_VARSIZE_ANY(toast_vl));
      return;
    }
  }
  if (header_stripped)
    column->Append(VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
  else
    column->Append(reinterpret_cast<char *>(value), VARSIZE_ANY(value));
}

static inline Datum BatchFixedDatum(const char *p, int16 typlen) {
  switch (typlen) {
    case 1:
      return CharGetDatum(*p);
    case 2: {
      int16 v;
      memcpy(&v, p, sizeof(v));
      return Int16GetDatum(v);
    }
    case 4: {
      int32 v;
      memcpy(&v, p, sizeof(v));
      return Int32GetDatum(v);
    }
    default: {
      int64 v;
      memcpy(&v, p, sizeof(v));
      return Int64GetDatum(v);
    }
  }
}

// One column's rows [start, start + nrows) of a batch, in the layout the
// sink asked for (access/pax_vexec_sink.cc, SinkLayout()):
//
//   FIXED      a by-value type's values, or a fixed-length by-reference
//              type's at PostgreSQL's array stride: Append() of the type's
//              bytes, as WriteTuple() appends a row's
//   BYTE_BOOL  bool, a byte a row
//   SCALED     porc_vec's numeric of a typmod: the integer at the typmod's
//              scale, written as the 16 bytes the column holds
//              (pg_short_numeric_to_vec_short_numeric(), comm/vec_numeric.cc)
//   OFFSETS    porc_vec's text, varchar, bpchar and bytea: the bytes
//              without a header, which a porc_vec column holds; a value
//              large enough for a PAX toast is made a varlena first
//   DATUM      any other varlena, with its header
//
// A NULL is AppendNull().  The statistics are counts alone for a column
// that keeps no values; else each value goes to them as a Datum, a
// payload's made a varlena.
void OrcWriter::WriteBatchColumn(int column_index, const VexecColumn &column,
                                 size_t start, size_t nrows) {
  auto attr = TupleDescAttr(writer_options_.rel_tuple_desc, column_index);
  PaxColumn *pax_column = (*pax_columns_)[column_index].get();
  bool is_vec = COLUMN_STORAGE_FORMAT_IS_VEC(pax_columns_);
  bool keeps = group_stats_.KeepsValues(column_index);
  size_t nulls = 0;
  std::vector<char> made;  // a payload made a varlena

  for (size_t r = start; r < start + nrows; r++) {
    Datum value = 0;

    if (attr->attisdropped ||
        (column.validity && !((column.validity[r >> 6] >> (r & 63)) & 1))) {
      pax_column->AppendNull();
      nulls++;
      continue;
    }
    switch (column.layout) {
      case VEXEC_FIXED: {
        auto p = static_cast<const char *>(column.values) +
                 r * static_cast<size_t>(column.stride);

        pax_column->Append(const_cast<char *>(p), attr->attlen);
        if (keeps)
          value = attr->attbyval ? BatchFixedDatum(p, attr->attlen)
                                 : PointerGetDatum(p);
        break;
      }
      case VEXEC_BYTE_BOOL: {
        char b = static_cast<const uint8 *>(column.values)[r] != 0;

        pax_column->Append(&b, 1);
        value = BoolGetDatum(b);
        break;
      }
      case VEXEC_SCALED: {
        auto numeric_column = static_cast<PaxShortNumericColumn *>(pax_column);
        auto data = numeric_column->GetDataBuffer();
        int128 v;
        int64 lo, hi;

        if (column.width == 8) {
          int64 v64;

          memcpy(&v64, static_cast<const char *>(column.values) + r * 8,
                 sizeof(v64));
          v = v64;
        } else {
          memcpy(&v, static_cast<const char *>(column.values) + r * 16,
                 sizeof(v));
        }
        lo = static_cast<int64>(static_cast<uint64>(v));
        hi = static_cast<int64>(
            (static_cast<uint64>(column.scale)
             << VEC_SHORT_NUMERIC_HIGH_DATA_WIDTH) |
            (static_cast<uint64>(static_cast<int64>(v >> 64)) &
             ((1ULL << VEC_SHORT_NUMERIC_HIGH_DATA_WIDTH) - 1)));
        if (data->Available() < VEC_SHORT_NUMERIC_STORE_BYTES)
          data->ReSize(data->Used() + VEC_SHORT_NUMERIC_STORE_BYTES, 2);
        memcpy(data->GetAvailableBuffer(), &lo, sizeof(lo));
        memcpy(data->GetAvailableBuffer() + sizeof(lo), &hi, sizeof(hi));
        data->Brush(VEC_SHORT_NUMERIC_STORE_BYTES);
        // the counts PaxShortNumericColumn::Append() keeps
        pax_column->PaxColumn::Append(nullptr, 0);
        if (keeps) value = vec_short_numeric_to_datum(&lo, &hi);
        break;
      }
      case VEXEC_OFFSETS: {
        auto offsets = static_cast<const int32 *>(column.values);
        auto p = static_cast<const char *>(column.buffers[0]) + offsets[r];
        size_t len = offsets[r + 1] - offsets[r];
        bool toast = VARATT_CAN_MAKE_PAX_COMPRESSED_TOAST_BY_SIZE(len) ||
                     VARATT_CAN_MAKE_PAX_EXTERNAL_TOAST_BY_SIZE(len);

        if (!keeps && !toast) {
          pax_column->Append(const_cast<char *>(p), len);
          break;
        }
        made.resize(len + VARHDRSZ);
        SET_VARSIZE(made.data(), len + VARHDRSZ);
        memcpy(made.data() + VARHDRSZ, p, len);
        if (toast)
          AppendBatchVarlena(column_index, pax_column,
                             reinterpret_cast<varlena *>(made.data()), true);
        else
          pax_column->Append(const_cast<char *>(p), len);
        value = PointerGetDatum(made.data());
        break;
      }
      case VEXEC_DATUM: {
        auto vl = reinterpret_cast<varlena *>(
            DatumGetPointer(static_cast<const Datum *>(column.values)[r]));

        if (attr->attlen > 0)
          pax_column->Append(reinterpret_cast<char *>(vl), attr->attlen);
        else if (is_vec && attr->atttypid == NUMERICOID)
          pax_column->Append(reinterpret_cast<char *>(vl), VARSIZE_ANY(vl));
        else
          AppendBatchVarlena(column_index, pax_column, vl, is_vec);
        value = PointerGetDatum(vl);
        break;
      }
      default:
        CBDB_RAISE(cbdb::CException::ExType::kExTypeLogicError,
                   fmt("PAX's sink was given a layout it did not ask for "
                       "[column=%d, layout=%d]",
                       column_index, column.layout));
    }
    if (keeps) group_stats_.AddColumnValue(column_index, value);
  }
  if (!keeps)
    group_stats_.AddColumnCounts(column_index, nrows - nulls, nulls);
  else if (nulls > 0)
    group_stats_.AddColumnCounts(column_index, 0, nulls);
}

void OrcWriter::EndWriteTuple(TupleTableSlot *table_slot) {
  // restore original datum value
  for (auto const &pair : origin_datum_holder_) {
    table_slot->tts_values[pair.first] = pair.second;
  }
  for (auto p : detoast_memory_holder_) {
    cbdb::Pfree(p);
  }
  origin_datum_holder_.clear();
  toast_memory_holder_.clear();
  detoast_memory_holder_.clear();

  if (pax_columns_->GetRows() >= writer_options_.group_limit) {
    Flush();
  }
}

void OrcWriter::MergeTo(MicroPartitionWriter *writer) {
  auto orc_writer = dynamic_cast<OrcWriter *>(writer);
  Assert(orc_writer);
  Assert(!is_closed_ && !(orc_writer->is_closed_));
  Assert(this != writer);
  Assert(writer_options_.rel_oid == orc_writer->writer_options_.rel_oid);

  // merge the groups which in disk
  MergeGroups(orc_writer);

  // clear the unstate file in disk.
  orc_writer->DeleteUnstateFile();

  // merge the memory
  MergePaxColumns(orc_writer);

  // Update summary
  summary_.num_tuples += orc_writer->summary_.num_tuples;

  if (mp_stats_) {
    mp_stats_->MergeTo(orc_writer->mp_stats_.get());
  }
}

void OrcWriter::MergePaxColumns(OrcWriter *writer) {
  PaxColumns *columns = writer->pax_columns_.get();
  bool ok pg_attribute_unused();
  Assert(columns->GetColumns() == pax_columns_->GetColumns());
  Assert(columns->GetRows() < writer_options_.group_limit);
  if (columns->GetRows() == 0) {
    return;
  }

  BufferedOutputStream buffer_mem_stream(2048);
  DataBuffer<char> toast_mem(nullptr, 0, true, false);
  ok = WriteStripe(&buffer_mem_stream, &toast_mem, columns,
                   &(writer->group_stats_), writer->mp_stats_.get());

  // must be ok
  Assert(ok);

  file_->PWriteN(buffer_mem_stream.GetDataBuffer()->GetBuffer(),
                 buffer_mem_stream.GetDataBuffer()->Used(),
                 current_offset_ - buffer_mem_stream.GetDataBuffer()->Used());

  if (writer_options_.need_wal) {
    cbdb::XLogPaxInsert(
        writer_options_.node, writer_options_.block_id.c_str(),
        current_offset_ - buffer_mem_stream.GetDataBuffer()->Used(),
        buffer_mem_stream.GetDataBuffer()->GetBuffer(),
        buffer_mem_stream.GetDataBuffer()->Used());
  }

  // direct write the toast
  if (toast_mem.GetBuffer()) {
    Assert(toast_file_);
    Assert(current_toast_file_offset_ >= toast_mem.Used());
    toast_file_->PWriteN(toast_mem.GetBuffer(), toast_mem.Used(),
                         current_toast_file_offset_ - toast_mem.Used());
    if (writer_options_.need_wal) {
      std::string toast_file_name =
          writer_options_.block_id + TOAST_FILE_SUFFIX;
      cbdb::XLogPaxInsert(writer_options_.node, toast_file_name.c_str(),
                          current_toast_file_offset_ - toast_mem.Used(),
                          toast_mem.GetBuffer(), toast_mem.Used());
    }
  }

  // Not do memory merge
}

void OrcWriter::MergeGroups(OrcWriter *orc_writer) {
  auto merge_buffer = std::make_shared<DataBuffer<char>>(0);

  for (int index = 0; index < orc_writer->file_footer_.stripes_size();
       index++) {
    MergeGroup(orc_writer, index, merge_buffer);
  }
}

void OrcWriter::MergeGroup(OrcWriter *orc_writer, int group_index,
                           std::shared_ptr<DataBuffer<char>> &merge_buffer) {
  const auto &stripe_info = orc_writer->file_footer_.stripes(group_index);
  auto total_len = stripe_info.footerlength();
  auto stripe_data_len = stripe_info.datalength();
  auto number_of_rows = stripe_info.numberofrows();
  auto number_of_toast = stripe_info.numberoftoast();
  auto toast_off = stripe_info.toastoffset();
  auto toast_len = stripe_info.toastlength();

  // will not flush empty group in disk
  Assert(stripe_data_len);

  if (!merge_buffer->GetBuffer() || merge_buffer->Capacity() < total_len) {
    merge_buffer = std::make_shared<DataBuffer<char>>(total_len);
  }

  orc_writer->file_->Flush();
  orc_writer->file_->PReadN(merge_buffer->GetBuffer(), total_len,
                            stripe_info.offset());

  summary_.file_size += total_len;
  file_->PWriteN(merge_buffer->GetBuffer(), total_len, current_offset_);

  if (writer_options_.need_wal) {
    cbdb::XLogPaxInsert(writer_options_.node, writer_options_.block_id.c_str(),
                        current_offset_, merge_buffer->GetBuffer(), total_len);
  }

  // merge the toast file content
  // We could merge a single toast file directly into another file,
  // but this would make assumptions about the toast file

  // check the external toast exist
  if (toast_len > 0) {
    // must exist
    Assert(toast_file_);
    Assert(merge_buffer->GetBuffer());
    if (merge_buffer->Capacity() < toast_len) {
      merge_buffer = std::make_shared<DataBuffer<char>>(toast_len);
    }
    orc_writer->toast_file_->Flush();
    orc_writer->toast_file_->PReadN(merge_buffer->GetBuffer(), toast_len,
                                    toast_off);
    toast_file_->PWriteN(merge_buffer->GetBuffer(), toast_len,
                         current_toast_file_offset_);
    if (writer_options_.need_wal) {
      std::string toast_file_name =
          writer_options_.block_id + TOAST_FILE_SUFFIX;
      cbdb::XLogPaxInsert(writer_options_.node, toast_file_name.c_str(),
                          current_toast_file_offset_, merge_buffer->GetBuffer(),
                          toast_len);
    }
  }

  auto stripe_info_write = file_footer_.add_stripes();

  stripe_info_write->set_offset(current_offset_);
  stripe_info_write->set_datalength(stripe_data_len);
  stripe_info_write->set_footerlength(total_len);
  stripe_info_write->set_numberofrows(number_of_rows);

  stripe_info_write->set_numberoftoast(number_of_toast);
  stripe_info_write->set_toastoffset(current_toast_file_offset_);
  stripe_info_write->set_toastlength(toast_len);

  current_toast_file_offset_ += toast_len;
  current_offset_ += total_len;
  total_rows_ += number_of_rows;

  Assert((size_t)stripe_info.colstats_size() == pax_columns_->GetColumns());

  for (int stats_index = 0; stats_index < stripe_info.colstats_size();
       stats_index++) {
    auto col_stats = stripe_info.colstats(stats_index);
    auto col_stats_write = stripe_info_write->add_colstats();
    col_stats_write->CopyFrom(col_stats);

    stripe_info_write->add_exttoastlength(
        stripe_info.exttoastlength(stats_index));
  }
}

void OrcWriter::DeleteUnstateFile() {
  file_->Close();
  // FIXME(gongxun): refactor into `filesystem::Delete(file);` to support
  // multiple filesystem.
  file_->Delete();

  if (toast_file_) {
    toast_file_->Close();
    toast_file_->Delete();
  }

  is_closed_ = true;
}

bool OrcWriter::WriteStripe(BufferedOutputStream *buffer_mem_stream,
                            DataBuffer<char> *toast_mem) {
  return WriteStripe(buffer_mem_stream, toast_mem, pax_columns_.get(),
                     &group_stats_, mp_stats_.get());
}

bool OrcWriter::WriteStripe(BufferedOutputStream *buffer_mem_stream,
                            DataBuffer<char> *toast_mem,
                            PaxColumns *pax_columns,
                            MicroPartitionStats *stripe_stats,
                            MicroPartitionStats *file_stats) {
  std::vector<pax::porc::proto::Stream> streams;
  std::vector<ColumnEncoding> encoding_kinds;
  pax::porc::proto::StripeFooter stripe_footer;
  pax::porc::proto::StripeInformation *stripe_info;
  bool pb_serialize_failed;

  size_t data_len = 0;
  size_t number_of_row = pax_columns->GetRows();
  size_t toast_len = 0;
  size_t number_of_toast = pax_columns->ToastCounts();

  // No need add stripe if nothing in memeory
  if (number_of_row == 0) {
    return false;
  }

  PaxColumns::ColumnStreamsFunc column_streams_func =
      [&streams](const pax::porc::proto::Stream_Kind &kind, size_t column,
                 size_t length, size_t padding) {
        Assert(padding < MEMORY_ALIGN_SIZE);
        pax::porc::proto::Stream stream;
        stream.set_kind(kind);
        stream.set_column(static_cast<uint32>(column));
        stream.set_length(length);
        stream.set_padding(padding);

        streams.push_back(std::move(stream));
      };

  PaxColumns::ColumnEncodingFunc column_encoding_func =
      [&encoding_kinds](const ColumnEncoding_Kind &encoding_kind,
                        const uint64 compress_lvl, const int64 origin_len,
                        const ColumnEncoding_Kind &offset_stream_encoding_kind,
                        const uint64 offset_stream_compress_lvl,
                        const int64 offset_stream_origin_len) {
        ColumnEncoding column_encoding;
        Assert(encoding_kind !=
               ColumnEncoding_Kind::ColumnEncoding_Kind_DEF_ENCODED);
        column_encoding.set_kind(encoding_kind);
        column_encoding.set_compress_lvl(compress_lvl);
        column_encoding.set_length(origin_len);

        column_encoding.set_offset_stream_kind(offset_stream_encoding_kind);
        column_encoding.set_offset_stream_compress_lvl(
            offset_stream_compress_lvl);
        column_encoding.set_offset_stream_length(offset_stream_origin_len);

        encoding_kinds.push_back(std::move(column_encoding));
      };

  auto data_buffer =
      pax_columns->GetDataBuffer(column_streams_func, column_encoding_func);

  Assert(data_buffer->Used() == data_buffer->Capacity());

  for (const auto &stream : streams) {
    *stripe_footer.add_streams() = stream;
    data_len += stream.length();
  }

  if (file_stats) {
    file_stats->MergeTo(stripe_stats);
  }

  auto stats_info = stripe_stats->Serialize();
  Assert(stats_info);

  stripe_info = file_footer_.add_stripes();

  for (size_t i = 0; i < pax_columns->GetColumns(); i++) {
    auto pb_stats = stripe_info->add_colstats();
    auto col_stats = stats_info->columnstats(static_cast<int>(i));
    auto pax_column = (*pax_columns)[i].get();

    Assert(col_stats.hasnull() == pax_column->HasNull());
    Assert(col_stats.allnull() == pax_column->AllNull());

    *stripe_footer.add_pax_col_encodings() = encoding_kinds[i];

    pb_stats->set_hastoast(pax_column->ToastCounts() > 0);
    pb_stats->set_hasnull(col_stats.hasnull());
    pb_stats->set_allnull(col_stats.allnull());
    pb_stats->set_nonnullrows(col_stats.nonnullrows());
    if (col_stats.has_bloomfilterinfo())
      *pb_stats->mutable_bloomfilterinfo() = col_stats.bloomfilterinfo();
    if (col_stats.has_columnbfstats())
      *pb_stats->mutable_columnbfstats() = col_stats.columnbfstats();
    *pb_stats->mutable_coldatastats() = col_stats.datastats();
    PAX_LOG_IF(pax_enable_debug,
               "write group[%lu](allnull=%s, hasnull=%s, nonnullrows=%lu, "
               "hastoast=%s, nrows=%lu)",
               i, BOOL_TOSTRING(col_stats.allnull()),
               BOOL_TOSTRING(col_stats.hasnull()), col_stats.nonnullrows(),
               BOOL_TOSTRING(pax_column->ToastCounts() > 0),
               pax_column->GetRows());
  }

  stripe_stats->Reset();
  buffer_mem_stream->Set(data_buffer);

  // check memory io with protobuf
  pb_serialize_failed =
      stripe_footer.SerializeToZeroCopyStream(buffer_mem_stream);
  if (unlikely(!pb_serialize_failed)) {
    // current pb strucature is too large
    PAX_LOG_IF(pax_enable_debug, "Invalid STRIPE FOOTER PB %s",
               stripe_footer.DebugString().c_str());
    CBDB_RAISE(cbdb::CException::ExType::kExTypeIOError,
               fmt("Fail to serialize the STRIPE FOOTER pb structure into mem "
                   "stream. [mem used=%ld, mem avail=%ld], \n %s",
                   buffer_mem_stream->ByteCount(),
                   buffer_mem_stream->GetDataBuffer()->Available(),
                   file_->DebugString().c_str()));
  }

  // Begin deal the toast memory
  if (number_of_toast > 0) {
    auto external_data_buffer = pax_columns->GetExternalToastDataBuffer();
    toast_len = external_data_buffer->Used();
    if (toast_len > 0) {
      Assert(!toast_mem->GetBuffer());
      toast_mem->Set(external_data_buffer->GetBuffer(), toast_len);
      toast_mem->BrushAll();
    }
  }

  stripe_info->set_offset(current_offset_);
  stripe_info->set_datalength(data_len);
  stripe_info->set_footerlength(buffer_mem_stream->GetSize());
  stripe_info->set_numberofrows(number_of_row);
  stripe_info->set_toastoffset(current_toast_file_offset_);
  stripe_info->set_toastlength(toast_len);
  stripe_info->set_numberoftoast(number_of_toast);
  for (size_t i = 0; i < pax_columns->GetColumns(); i++) {
    auto pax_column = (*pax_columns)[i].get();
    auto ext_buffer = pax_column->GetExternalToastDataBuffer();
    stripe_info->add_exttoastlength(ext_buffer ? ext_buffer->Used() : 0);
  }

  current_offset_ += buffer_mem_stream->GetSize();
  current_toast_file_offset_ += toast_len;
  total_rows_ += number_of_row;

  return true;
}

void OrcWriter::Close() {
  if (is_closed_) {
    return;
  }
  BufferedOutputStream buffer_mem_stream(2048);
  size_t file_offset = current_offset_;
  bool empty_stripe = false;
  std::shared_ptr<DataBuffer<char>> data_buffer;
  DataBuffer<char> toast_mem(nullptr, 0, true, false);

  empty_stripe = !WriteStripe(&buffer_mem_stream, &toast_mem);
  if (empty_stripe) {
    data_buffer = std::make_shared<DataBuffer<char>>(2048);
    buffer_mem_stream.Set(data_buffer);
  }

  WriteFileFooter(&buffer_mem_stream);
  WritePostscript(&buffer_mem_stream);

  summary_.file_size += buffer_mem_stream.GetDataBuffer()->Used();

  file_->PWriteN(buffer_mem_stream.GetDataBuffer()->GetBuffer(),
                 buffer_mem_stream.GetDataBuffer()->Used(), file_offset);

  if (writer_options_.need_wal) {
    cbdb::XLogPaxInsert(writer_options_.node, writer_options_.block_id.c_str(),
                        file_offset,
                        buffer_mem_stream.GetDataBuffer()->GetBuffer(),
                        buffer_mem_stream.GetDataBuffer()->Used());
  }

  if (toast_mem.GetBuffer()) {
    Assert(toast_file_);
    Assert(current_toast_file_offset_ >= toast_mem.Used());
    toast_file_->PWriteN(toast_mem.GetBuffer(), toast_mem.Used(),
                         current_toast_file_offset_ - toast_mem.Used());
    if (writer_options_.need_wal) {
      std::string toast_file_name =
          writer_options_.block_id + TOAST_FILE_SUFFIX;
      cbdb::XLogPaxInsert(writer_options_.node, toast_file_name.c_str(),
                          current_toast_file_offset_ - toast_mem.Used(),
                          toast_mem.GetBuffer(), toast_mem.Used());
    }
  }

  summary_.exist_ext_toast = toast_file_ && current_toast_file_offset_ != 0;

  // Close toast_file before origin file
  if (toast_file_) {
    toast_file_->Flush();
    toast_file_->Close();
    // no toast happend
    if (current_toast_file_offset_ == 0) {
      toast_file_->Delete();
    }
  }

  file_->Flush();
  file_->Close();

  if (summary_callback_) {
    summary_.mp_stats = !mp_stats_ ? nullptr : mp_stats_->Serialize();
    summary_callback_(summary_);
  }

  is_closed_ = true;
}

size_t OrcWriter::PhysicalSize() const {
  return current_written_phy_size_ + pax_columns_->PhysicalSize();
}

void OrcWriter::WriteFileFooter(BufferedOutputStream *buffer_mem_stream) {
  bool pb_serialize_failed;
  Assert(writer_options_.storage_format == kTypeStoragePorcNonVec ||
         writer_options_.storage_format == kTypeStoragePorcVec);
  file_footer_.set_contentlength(current_offset_);
  file_footer_.set_numberofrows(total_rows_);
  file_footer_.set_storageformat(writer_options_.storage_format);

  // build types and column attributes
  auto proto_type = file_footer_.add_types();
  proto_type->set_kind(::pax::porc::proto::Type_Kind_STRUCT);

  for (size_t i = 0; i < column_types_.size(); ++i) {
    auto orc_type = column_types_[i];

    auto sub_proto_type = file_footer_.add_types();
    sub_proto_type->set_kind(orc_type);
    auto pax_column = (*pax_columns_)[i].get();
    if (pax_column && pax_column->HasAttributes()) {
      const auto &column_attrs = pax_column->GetAttributes();
      for (const auto &kv : column_attrs) {
        auto attr_pair = sub_proto_type->add_attributes();
        attr_pair->set_key(kv.first);
        attr_pair->set_value(kv.second);
      }
    }
    file_footer_.mutable_types(0)->add_subtypes(i);
  }

  Assert(file_footer_.colinfo_size() == 0);
  for (size_t i = 0; i < pax_columns_->GetColumns(); i++) {
    auto pb_colinfo = file_footer_.add_colinfo();
    *pb_colinfo = *group_stats_.GetColumnBasicInfo(static_cast<int>(i));
  }

  buffer_mem_stream->StartBufferOutRecord();

  pb_serialize_failed =
      file_footer_.SerializeToZeroCopyStream(buffer_mem_stream);
  if (unlikely(!pb_serialize_failed)) {
    PAX_LOG_IF(pax_enable_debug, "Invalid FOOTER PB %s",
               file_footer_.DebugString().c_str());
    CBDB_RAISE(cbdb::CException::ExType::kExTypeIOError,
               fmt("Fail to serialize the FOOTER pb structure into mem stream. "
                   "[mem used=%ld, mem avail=%ld], \n %s",
                   buffer_mem_stream->ByteCount(),
                   buffer_mem_stream->GetDataBuffer()->Available(),
                   file_->DebugString().c_str()));
  }

  post_script_.set_footerlength(buffer_mem_stream->EndBufferOutRecord());
}

void OrcWriter::WritePostscript(BufferedOutputStream *buffer_mem_stream) {
  bool pb_serialize_failed;
  buffer_mem_stream->StartBufferOutRecord();
  pb_serialize_failed =
      post_script_.SerializeToZeroCopyStream(buffer_mem_stream);
  if (unlikely(!pb_serialize_failed)) {
    PAX_LOG_IF(pax_enable_debug, "Invalid POSTSCRIPT PB %s",
               post_script_.DebugString().c_str());
    CBDB_RAISE(cbdb::CException::ExType::kExTypeIOError,
               fmt("Fail to serialize the POSTSCRIPT pb structure into mem "
                   "stream. [mem used=%ld, mem avail=%ld], \n %s",
                   buffer_mem_stream->ByteCount(),
                   buffer_mem_stream->GetDataBuffer()->Available(),
                   file_->DebugString().c_str()));
  }

  auto ps_len = (uint64)buffer_mem_stream->EndBufferOutRecord();
  Assert(ps_len > 0);
  static_assert(sizeof(ps_len) == PORC_POST_SCRIPT_SIZE,
                "post script type len not match.");
  buffer_mem_stream->DirectWrite((char *)&ps_len, PORC_POST_SCRIPT_SIZE);
}

}  // namespace pax
