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
 * micro_partition_stats_updater.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/storage/micro_partition_stats_updater.cc
 *
 *
 * The port's copy, for a fix of Cloudberry's own: the statistics a DELETE
 * writes for a file it deletes from.  A group without deleted rows gave the
 * statistics its file keeps of it in place of its rows, which
 * MicroPartitionStats::MergeRawInfo() takes for what they may not be: a
 * column of each of the table's columns -- a file written before ALTER
 * TABLE ... ADD COLUMN has none of the columns added since, and the DELETE
 * failed (a protobuf index out of range, "ERROR: PaxExecutorFinish"); a
 * minimum and a maximum of each minmax column -- a group whose column is all
 * null keeps none, nor one written before the column became a minmax
 * column, and the DELETE crashed or failed an assertion; and with no bloom
 * filter, which it merges none of, so that an IN of a bloom filter column
 * found none of the group's values afterwards.  Here a group gives its
 * statistics only where they are what its rows make, and is read otherwise,
 * as a group with deleted rows is -- every column of it, which TableDeleter
 * reads now (pax.cc): its statistics are every column's, and the columns it
 * left unread were counted not null in each row, so that an IS NULL found
 * none of the file's rows afterwards.
 *-------------------------------------------------------------------------
 */

#include "storage/micro_partition_stats_updater.h"

#include "comm/pax_memory.h"
#include "storage/filter/pax_filter.h"
#include "storage/micro_partition_stats.h"
#include "storage/pax_defined.h"
#include "storage/pax_itemptr.h"

namespace pax {

// Whether the statistics a file keeps of a group are what its rows make of
// each of the table's columns as it is now, as MergeRawInfo() takes them:
// a column for each, a minimum and a maximum of each minmax column, kept for
// its type and collation, and no bloom filter column, whose filter it does
// not merge.
static bool GroupStatsMergeable(const ColumnStatsProvider &group_stats,
                                TupleDesc desc,
                                const std::vector<int> &minmax_columns,
                                const std::vector<int> &bf_columns) {
  if (!bf_columns.empty() || group_stats.ColumnSize() != desc->natts)
    return false;

  for (auto column_index : minmax_columns) {
    auto att = TupleDescAttr(desc, column_index);
    const auto &data_stats = group_stats.DataStats(column_index);
    const auto &info = group_stats.ColumnInfo(column_index);

    if (att->attisdropped) continue;
    if (group_stats.AllNull(column_index) || !data_stats.has_minimal() ||
        !data_stats.has_maximum() || info.typid() != att->atttypid ||
        info.collation() != att->attcollation)
      return false;
  }
  return true;
}

MicroPartitionStatsUpdater::MicroPartitionStatsUpdater(
    MicroPartitionReader *reader, std::shared_ptr<Bitmap8> visibility_bitmap)
    : reader_(reader) {
  Assert(reader);
  Assert(visibility_bitmap);

  // O(n) here
  size_t group_tup_offset = 0;
  for (size_t i = 0; i < reader_->GetGroupNums(); i++) {
    size_t group_tup_end = reader_->GetTupleCountsInGroup(i) + group_tup_offset;
    bool exist_invisible_tuple = false;
    for (size_t j = group_tup_offset; j < group_tup_end; j++) {
      if (visibility_bitmap->Test(j)) {
        exist_invisible_tuple = true;
        break;
      }
    }
    exist_invisible_tuples_.emplace_back(exist_invisible_tuple);
    group_tup_offset += reader_->GetTupleCountsInGroup(i);
  }
}

std::shared_ptr<MicroPartitionStats> MicroPartitionStatsUpdater::Update(
    TupleTableSlot *slot, const std::vector<int> &minmax_columns,
    const std::vector<int> &bf_columns) {
  TupleDesc desc;
  std::shared_ptr<MicroPartitionStats> mp_stats;
  std::shared_ptr<MicroPartitionStats> group_stats;

  Assert(slot);
  desc = slot->tts_tupleDescriptor;
  mp_stats = std::make_shared<MicroPartitionStats>(desc);
  mp_stats->Initialize(minmax_columns, bf_columns);

  Assert(exist_invisible_tuples_.size() == reader_->GetGroupNums());

  for (size_t group_index = 0; group_index < exist_invisible_tuples_.size();
       group_index++) {
    auto column_group_stats = reader_->GetGroupStatsInfo(group_index);

    if (exist_invisible_tuples_[group_index] ||
        !GroupStatsMergeable(*column_group_stats, desc, minmax_columns,
                             bf_columns)) {
      if (!group_stats) {
        group_stats = std::make_shared<MicroPartitionStats>(desc);
        group_stats->Initialize(minmax_columns, bf_columns);
      }

      // already setup the visible map
      auto group = reader_->ReadGroup(group_index);
#ifdef ENABLE_DEBUG
      size_t read_count = 0;
#endif
      while (group->ReadTuple(slot).first) {
        group_stats->AddRow(slot);
#ifdef ENABLE_DEBUG
        ++read_count;
#endif
      }

#ifdef ENABLE_DEBUG
      // fewer rows than the group's where it has deleted ones, and all of
      // them where it is read for its statistics
      if (exist_invisible_tuples_[group_index])
        Assert(read_count < group->GetRows());
      else
        Assert(read_count == group->GetRows());
#endif

    } else {
      ::pax::stats::MicroPartitionStatisticsInfo stat_info;
      for (int column_index = 0; column_index < desc->natts; column_index++) {
        auto new_col_stats = stat_info.add_columnstats();

        new_col_stats->mutable_datastats()->CopyFrom(
            column_group_stats->DataStats(column_index));
        new_col_stats->set_allnull(column_group_stats->AllNull(column_index));
        new_col_stats->set_hasnull(column_group_stats->HasNull(column_index));
        new_col_stats->set_nonnullrows(
            column_group_stats->NonNullRows(column_index));
        new_col_stats->mutable_info()->CopyFrom(
            column_group_stats->ColumnInfo(column_index));
      }
      mp_stats->MergeRawInfo(&stat_info);
    }
  }

  if (group_stats) {
    mp_stats->MergeTo(group_stats.get());
  }

  return mp_stats;
}

}  // namespace pax
