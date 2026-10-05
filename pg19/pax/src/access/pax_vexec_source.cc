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
 * pax_vexec_source.cc
 *	  PAX's batch reader: a scan of a PAX table handed to vexec, the
 *	  vectorized executor, a batch of columns at a time, through the source
 *	  contract of vexec_source.h (pg_vector_executor.md §3.5.1, §3.5.3).
 *
 * The port's own file; Cloudberry has none like it.  Its VEC_BUILD adapter
 * (storage/vec/pax_porc_adpater.cc, pax_porc_vec_adpater.cc) copies a
 * group's rows into Arrow buffers for the closed engine, deleted rows left
 * out; here a group is handed as it lies wherever vexec's layouts allow,
 * deleted rows left in and marked, so that the loops are new, written from
 * the column classes they read (storage/columns/).
 *
 * vexec begins the scan itself, through table_beginscan(), and hands the
 * scan to begin() with the attributes it reads.  The reader then reads the
 * table as the scanner does (access/pax_scanner.cc), a group's columns at a
 * time where the scanner reads a row at a time:
 *
 *	files		those the scan's snapshot sees, listed from the table's aux
 *				table as PaxScanDesc::OpenReader() lists them
 *				(pax_scanner.cc:328-357): MicroPartitionIterator::New(), or
 *				NewParallelIterator() for a parallel scan; a file the sparse
 *				filter proves holds no row the quals keep is passed over;
 *	groups		each file's in order, read by OrcReader::ReadGroup() with the
 *				scan's columns as its projection; a group the sparse filter
 *				passes over, as OrcReader::ReadTuple() passes it
 *				(storage/orc/orc_reader.cc:230-238), is not read, nor one
 *				whose rows are all deleted, nor any group at all where the
 *				scan reads no column (count(*)): its rows are the file
 *				footer's count;
 *	batches		a group is sliced by offset into batches of spec->max_rows
 *				rows, a whole group where that is 0;
 *	deletes		the file's visibility map, 1 = deleted, read as
 *				TableReader::OpenFile() reads it (storage/pax.cc:517-526),
 *				inverted into the batch's visible bitmap -- never into a
 *				column's validity -- and a slice all of whose rows are
 *				deleted is not handed out;
 *	TIDs		the table's layout, PaxTidToTable(MakeCTID(file, row)), as the
 *				method hands a row's out (access/pax_access_handle.cc:118-120).
 *
 * The quals vexec gives may only prune: they reach the sparse filter, which
 * drops whole files and groups by their statistics, never rows; PAX's row
 * filter is not made.
 *
 * Layouts.  A column is handed in the layout PAX holds it in, named as
 * vexec's batch/types.c names its shapes, and copied only where vexec's
 * layout needs the bytes moved:
 *
 *					porc, PAX's default				porc_vec
 *	by-value		FIXED.  A group keeps the		FIXED: the column's buffer,
 *					values of its rows that are		a value a row, NULL rows
 *					not NULL, densely: a slice		zeroed; zero-copy
 *					without a NULL is zero-copy,
 *					one with NULLs is scattered
 *	bool			BYTE_BOOL, the same				BIT_BOOL: the bitmap's
 *													words, copied 64 rows at a
 *													time to clear the tail
 *	fixed-length	FIXED at PostgreSQL's array		FIXED: zero-copy where a
 *	by-reference	stride, copied from the			slice has no NULL, packed;
 *					values at their offsets			copied where it has
 *	varlena			DATUM: pointers at the			OFFSETS: the column's
 *					headered values, zero-copy;		offsets rebased, its bytes
 *					VIEW over the same bytes,		zero-copy; char(n) copied
 *					the Datums kept beside, where	with the trailing blanks
 *					the batch asks for views		porc_vec strips
 *	numeric			DATUM, as varlena				SCALED: PAX's 16 bytes
 *													converted to an integer at
 *													the typmod's scale; DATUM
 *													where a value has none
 *	a column a		CONST: its attmissing value,	the same
 *	file lacks		or NULL
 *
 * A PAX toast in a slice is detoasted into memory the batch holds: a
 * porc_vec string column with one is handed as DATUM.  A column of any
 * other kind is handed as the slot path reads it, the column's GetDatum()
 * row by row (OrcGroup's GetColumnDatum, orc_group.cc:48-69; OrcVecGroup's,
 * orc_vec_group.cc:36-46), so that every column has a batch and every
 * value is the one table_scan_getnextslot() returns.
 *
 * Lifetime.  A batch's columns point into the group it was sliced from and
 * into memory of the batch's own; both live until the next call of next(),
 * or until release() where vexec retained the batch.  So a group is read
 * into a buffer of its own -- ReaderOptions.reused_buffer is left empty --
 * where the scanner reads every group into one it reuses
 * (pax_scanner.cc:283-284), and the group is freed with the last batch
 * that points into it.
 *
 * Errors.  Every callback is C, runs on the backend's main thread and may
 * raise: PAX's C++ exceptions become PostgreSQL errors at this boundary, as
 * at the access method's own entry points (CBDB_TRY ... CBDB_END_TRY), and a
 * call into PostgreSQL from the C++ code is wrapped (CBDB_WRAP_START), so
 * that its error unwinds the C++ frames.  The reader is a resource PAX
 * remembers (comm/pax_resource.cc), freed where the transaction aborts
 * before end(); its PostgreSQL memory, a context under the one begin() was
 * called in, goes with that context.
 *
 *-------------------------------------------------------------------------
 */

#include "comm/cbdb_api.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "comm/cbdb_wrappers.h"
#include "comm/guc.h"
#include "comm/iterator.h"
#include "comm/pax_memory.h"
#include "comm/pax_resource.h"
#include "comm/singleton.h"
#include "comm/vec_numeric.h"
#include "exceptions/CException.h"
#include "storage/columns/pax_column.h"
#include "storage/columns/pax_columns.h"
#include "storage/columns/pax_vec_column.h"
#include "storage/filter/pax_filter.h"
#include "storage/filter/pax_sparse_filter.h"
#include "storage/local_file_system.h"
#include "storage/micro_partition_iterator.h"
#include "storage/micro_partition_metadata.h"
#include "storage/micro_partition_stats.h"
#include "storage/orc/porc.h"
#include "storage/pax_defined.h"
#include "storage/paxc_define.h"
#include "storage/toast/pax_toast.h"
#include "pax_module.h"
#include "pax_tid.h"

extern "C" {
#include "vexec_source.h"
}

namespace pax {

namespace {

// char(n)'s n, which PaxVecBpCharColumn keeps among a column's attributes
// (storage/columns/pax_vec_bpchar_column.cc:32)
const char *const kBpCharNumberKey = "CHAR_N_KEY";

// What a group's column is, as the reader hands it.
enum class Kind {
  kMissing,      // the file lacks it: CONST, from attmissing
  kPorcFixed,    // porc, a by-value type: its values not NULL, densely
  kPorcVarlena,  // porc, varlena: headered values at offsets
  kPorcByRef,    // porc, fixed-length by-reference: values at offsets
  kVecFixed,     // porc_vec, a by-value type: a value a row
  kVecBits,      // porc_vec, bool: a bit a row
  kVecVarlena,   // porc_vec, varlena: payloads at offsets, a row each
  kVecBpChar,    // porc_vec, char(n): payloads without trailing blanks
  kVecByRef,     // porc_vec, fixed-length by-reference: no header
  kVecNumeric,   // porc_vec, numeric: 16 bytes a row
  kRows,         // anything else: GetDatum() row by row, as the slot path
};

// What begin() works out once of a column the scan reads.
struct ColumnPlan {
  AttrNumber attnum;  // 1..natts, or SelfItemPointerAttributeNumber
  int index;          // attnum - 1: the column's in a group
  int16 typlen;
  bool typbyval;
  uint8 alignby;
  bool is_bool;      // its base type is bool
  int32 pg_stride;   // a fixed-length by-reference value's array stride
  uint8 want;        // the layout the batch format asks for
  uint8 scaled_width;  // numeric: vexec's scaled layout's width, or 0
  int16 scale;         // numeric: and its scale
  bool missing_present;  // attmissing
  Datum missing_value;
};

// What next() keeps of a column of the group it slices.
struct GroupColumn {
  Kind kind = Kind::kMissing;
  PaxColumn *column = nullptr;
  bool vec = false;
  const uint8 *nulls = nullptr;  // the null bitmap, 1 = not NULL
  size_t nulls_len = 0;
  bool has_nulls = false;
  const char *data = nullptr;
  size_t data_len = 0;
  const int32 *offsets = nullptr;
  size_t noffsets = 0;
  size_t nonnull = 0;  // porc: the values before the next slice's
  bool has_toast = false;
  int64 bpchar_n = -1;  // porc_vec char(n): n, -1 where not known
};

// What a batch's columns point into, besides the reader's own: the group,
// and the values detoasted for it, in memory of its own.
struct BatchHold {
  MemoryContext cxt = nullptr;
  std::shared_ptr<MicroPartitionReader::Group> group;
  std::vector<std::unique_ptr<MemoryObject>> values;
  bool retained = false;
};

// vexec's scaled numeric for a typmod: the width and scale its batch/types.c
// gives a numeric column (numeric_typmod_bounds() and vexec_type_make()), so
// that a column handed SCALED needs no conversion.  The typmod is
// ((precision << 16) | (scale & 0x7ff)) + VARHDRSZ, its scale an 11-bit
// two's-complement number (PG19:src/backend/utils/adt/numeric.c:874-928).
void NumericScaled(int32 typmod, uint8 *width, int16 *scale) {
  *width = 0;
  *scale = 0;
  if (typmod < (int32)VARHDRSZ) return;

  int32 precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
  int32 tscale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
  int32 digits = precision + (tscale < 0 ? -tscale : 0);
  int32 s = Max(tscale, 0);

  *scale = (int16)s;
  if (s <= 38 && digits > 0 && digits <= 18)
    *width = 8;
  else if (s <= 38 && digits > 0 && digits <= 38)
    *width = 16;
}

inline size_t Words(size_t nbits) { return (nbits + 63) / 64; }

inline size_t CountBits(const uint64 *words, size_t nbits) {
  size_t n = 0;
  for (size_t w = 0; w < Words(nbits); w++) n += __builtin_popcountll(words[w]);
  return n;
}

inline bool BitSet(const uint64 *words, size_t i) {
  return (words[i >> 6] >> (i & 63)) & 1;
}

// The 64 bits of a bitmap of nbytes bytes from bit `bit` on, least
// significant first.  A bit past the bitmap reads as 0, as BitmapTpl::Test()
// reads it (comm/bitmap.h), where PAX stores a bitmap without its trailing
// zero words (MinimalStoredBytes()).
inline uint64 LoadWord(const uint8 *bytes, size_t nbytes, size_t bit) {
  size_t byte = bit >> 3;
  int shift = (int)(bit & 7);
  uint64 v = 0;

  for (int k = 0; k < 8; k++)
    if (byte + k < nbytes) v |= (uint64)bytes[byte + k] << (8 * k);
  if (shift != 0) {
    uint64 hi = byte + 8 < nbytes ? bytes[byte + 8] : 0;

    v = (v >> shift) | (hi << (64 - shift));
  }
  return v;
}

// The bits [start, start + nbits) of a bitmap as uint64 words, the last
// word's bits past nbits cleared, as vexec's bitmaps keep them.
void ExtractBits(const uint8 *bytes, size_t nbytes, size_t start, size_t nbits,
                 uint64 *out) {
  for (size_t w = 0; w < Words(nbits); w++)
    out[w] = LoadWord(bytes, nbytes, start + w * 64);
  if (nbits % 64 != 0)
    out[Words(nbits) - 1] &= (UINT64CONST(1) << (nbits % 64)) - 1;
}

// How many of the bits [start, start + nbits) of a bitmap are set.
size_t CountBitsAt(const uint8 *bytes, size_t nbytes, size_t start,
                   size_t nbits) {
  size_t count = 0;

  for (size_t w = 0; w < Words(nbits); w++) {
    uint64 word = LoadWord(bytes, nbytes, start + w * 64);

    if (w == Words(nbits) - 1 && nbits % 64 != 0)
      word &= (UINT64CONST(1) << (nbits % 64)) - 1;
    count += __builtin_popcountll(word);
  }
  return count;
}

// CurrentMemoryContext for a scope, given back as the scope is left, by an
// exception too.
class ContextScope final {
 public:
  explicit ContextScope(MemoryContext cxt) : old_(MemoryContextSwitchTo(cxt)) {}
  ~ContextScope() { MemoryContextSwitchTo(old_); }
  ContextScope(const ContextScope &) = delete;
  ContextScope &operator=(const ContextScope &) = delete;

 private:
  MemoryContext old_;
};

inline bool Aligned(const void *p, size_t align) {
  return align <= 1 || reinterpret_cast<uintptr_t>(p) % align == 0;
}

// An Arrow binary view, 16 bytes (arrow/docs/source/format/Columnar.rst,
// "Variable-size Binary View Layout"): the length, then up to 12 bytes
// inline, zero-padded, or a 4-byte prefix, the buffer's index and the
// offset in it.
struct BinaryView {
  int32 size;
  union {
    char inlined[12];
    struct {
      char prefix[4];
      int32 buffer_index;
      int32 offset;
    } ref;
  };
};
static_assert(sizeof(BinaryView) == 16, "an Arrow view is 16 bytes");

const char kEmpty[8] = {0};

}  // namespace

class VexecPaxReader final {
 public:
  VexecPaxReader(TableScanDesc scan, const VexecSourceSpec *spec);
  ~VexecPaxReader();

  void Begin();
  bool Next(VexecSourceBatch *out, size_t *nvisible);
  void Retain(void *owner);
  void Release(void *owner);
  void Rescan();
  void End();
  Relation GetRelation() const { return rel_; }

 private:
  bool OpenNextGroup();
  bool OpenNextFile();
  void CloseGroup();
  void CloseFile();
  void PrepareGroupColumn(int i);
  BatchHold *NewHold();
  void ResetHold();

  void *Alloc(size_t size);
  void *Alloc0(size_t size);

  size_t Visible(size_t first, size_t n, uint64 **visible);
  size_t Validity(const GroupColumn &gc, size_t s, size_t n, uint64 **validity);
  void SkipSlice(size_t s, size_t n);
  void FillColumn(int i, size_t s, size_t n, VexecColumn *out);
  void FillMissing(const ColumnPlan &plan, VexecColumn *out);
  void FillTids(size_t s, size_t n, VexecColumn *out, ItemPointerData **tids);
  void FillPorcFixed(const ColumnPlan &plan, GroupColumn &gc, size_t n,
                     const uint64 *validity, size_t nvalid, VexecColumn *out);
  void FillPorcVarlena(const ColumnPlan &plan, GroupColumn &gc, size_t s,
                       size_t n, const uint64 *validity, VexecColumn *out);
  void FillPorcByRef(const ColumnPlan &plan, GroupColumn &gc, size_t n,
                     const uint64 *validity, VexecColumn *out);
  void FillVecFixed(const ColumnPlan &plan, GroupColumn &gc, size_t s,
                    size_t n, VexecColumn *out);
  void FillVecBits(GroupColumn &gc, size_t s, size_t n, VexecColumn *out);
  bool FillVecVarlena(GroupColumn &gc, size_t s, size_t n, VexecColumn *out);
  bool FillVecBpChar(GroupColumn &gc, size_t s, size_t n,
                     const uint64 *validity, VexecColumn *out);
  void FillVecByRef(const ColumnPlan &plan, GroupColumn &gc, size_t s,
                    size_t n, const uint64 *validity, VexecColumn *out);
  void FillVecNumeric(const ColumnPlan &plan, GroupColumn &gc, size_t s,
                      size_t n, const uint64 *validity, VexecColumn *out);
  void FillRows(const ColumnPlan &plan, GroupColumn &gc, size_t s, size_t n,
                const uint64 *validity, VexecColumn *out);
  bool SliceHasToast(GroupColumn &gc, size_t s, size_t n);

  static void SetShape(VexecColumn *out, int layout, int width, int stride,
                       int scale);

  Relation rel_;
  Snapshot snapshot_;
  ParallelTableScanDesc parallel_;
  Snapshot aux_snapshot_ = nullptr;  // registered here, for SnapshotAny
  int file_bits_;
  MemoryContext cxt_;

  std::vector<ColumnPlan> plans_;
  std::vector<GroupColumn> gcols_;
  size_t ncolumns_;
  size_t nfetch_;  // columns read from the files: those not ctid
  size_t max_rows_;
  bool want_tids_;

  std::shared_ptr<PaxFilter> filter_;
  std::unique_ptr<IteratorBase<MicroPartitionMetadata>> iterator_;

  // the file being read
  std::unique_ptr<OrcReader> reader_;
  int file_id_ = -1;
  std::shared_ptr<Bitmap8> visimap_;
  size_t ngroups_ = 0;
  size_t next_group_ = 0;
  size_t next_group_row_ = 0;  // the file's row the next group starts at

  // the group being sliced
  bool group_open_ = false;
  std::shared_ptr<MicroPartitionReader::Group> group_;
  size_t group_rows_ = 0;
  size_t group_row_ = 0;  // the file's row the group starts at
  size_t slice_ = 0;      // the group's row the next slice starts at

  // the memory of the batch handed out last, and of those vexec retains
  BatchHold *hold_ = nullptr;
  std::vector<BatchHold *> retained_;
};

VexecPaxReader::VexecPaxReader(TableScanDesc scan, const VexecSourceSpec *spec)
    : rel_(scan->rs_rd),
      snapshot_(scan->rs_snapshot),
      parallel_(scan->rs_parallel) {
  TupleDesc desc = RelationGetDescr(rel_);
  std::vector<bool> projection(desc->natts, false);

  file_bits_ = cbdb::PaxTableFileBits(rel_);
  cxt_ = cbdb::AllocSetCtxCreate(CurrentMemoryContext, "PAX vexec source",
                                 ALLOCSET_DEFAULT_SIZES);

  ncolumns_ = spec->ncolumns;
  nfetch_ = 0;
  want_tids_ = (spec->flags & VEXEC_SRC_TIDS) != 0;
  max_rows_ = spec->max_rows > 0 ? (size_t)spec->max_rows : 0;

  plans_.resize(ncolumns_);
  gcols_.resize(ncolumns_);
  for (size_t i = 0; i < ncolumns_; i++) {
    ColumnPlan &p = plans_[i];
    AttrNumber attnum = spec->attnums[i];

    memset(&p, 0, sizeof(p));
    p.attnum = attnum;
    p.want = spec->layouts ? spec->layouts[i] : VEXEC_DATUM;
    if (attnum == SelfItemPointerAttributeNumber) {
      want_tids_ = true;
      continue;
    }
    CBDB_CHECK(attnum >= 1 && attnum <= desc->natts &&
                   !TupleDescAttr(desc, attnum - 1)->attisdropped,
               cbdb::CException::ExType::kExTypeInvalid,
               fmt("vexec asked a PAX scan for attribute %d", attnum));

    Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
    int32 basetypmod = att->atttypmod;
    Oid basetype;

    // the type's own properties, a domain's its base type's
    CBDB_WRAP_START;
    { basetype = getBaseTypeAndTypmod(att->atttypid, &basetypmod); }
    CBDB_WRAP_END;

    p.index = attnum - 1;
    p.typlen = att->attlen;
    p.typbyval = att->attbyval;
    p.alignby = (uint8)typalign_to_alignby(att->attalign);
    p.is_bool = basetype == BOOLOID;
    p.pg_stride = p.typlen > 0 ? (int32)TYPEALIGN(p.alignby, p.typlen) : 0;
    if (basetype == NUMERICOID)
      NumericScaled(basetypmod, &p.scaled_width, &p.scale);

    // attmissing, as slot_getmissingattrs() reads it
    // (PG19:src/backend/executor/execTuples.c:2151-2180)
    if (desc->constr && desc->constr->missing &&
        desc->constr->missing[attnum - 1].am_present) {
      p.missing_present = true;
      p.missing_value = desc->constr->missing[attnum - 1].am_value;
    }

    projection[attnum - 1] = true;
    nfetch_++;
  }

  // The projection: only the scan's columns are read.  The sparse filter,
  // where PAX's is on, as PaxScanDesc::ExtractColumns() makes it
  // (pax_scanner.cc:426-427), from the quals vexec says it may prune by and
  // its runtime keys; never the row filter, which would drop rows.
  filter_ = std::make_shared<PaxFilter>();
  filter_->SetColumnProjection(std::move(projection));
  if (pax_enable_sparse_filter && (spec->quals != NIL || spec->nkeys > 0)) {
    ContextScope scope(cxt_);

    filter_->InitSparseFilter(rel_, spec->quals, spec->keys, spec->nkeys);
  }
}

// What End() has not freed: the C++ objects alone, for a reader freed by
// PAX's resource callback where a transaction aborts, when its PostgreSQL
// memory may be gone already.
VexecPaxReader::~VexecPaxReader() {
  for (auto h : retained_) PAX_DELETE(h);
  if (hold_) PAX_DELETE(hold_);
}

// The files under the scan's snapshot, as PaxScanDesc::OpenReader() lists
// them (pax_scanner.cc:328-357): the catalog snapshot a scan of SnapshotAny
// reads the aux table with is registered while the scan runs.
void VexecPaxReader::Begin() {
  ContextScope scope(cxt_);
  Snapshot aux_snapshot = snapshot_;

  if (snapshot_ && snapshot_->snapshot_type == SNAPSHOT_ANY) {
    CBDB_WRAP_START;
    { aux_snapshot_ = RegisterSnapshot(GetCatalogSnapshot(InvalidOid)); }
    CBDB_WRAP_END;
    aux_snapshot = aux_snapshot_;
  }

  std::unique_ptr<IteratorBase<MicroPartitionMetadata>> iter;
  if (parallel_)
    iter = MicroPartitionIterator::NewParallelIterator(
        rel_, aux_snapshot, (ParallelBlockTableScanDesc)parallel_);
  else
    iter = MicroPartitionIterator::New(rel_, aux_snapshot);

  if (filter_->SparseFilterEnabled()) {
    auto filter = filter_;
    Relation rel = rel_;

    iter = std::make_unique<FilterIterator<MicroPartitionMetadata>>(
        std::move(iter), [filter, rel](const auto &x) {
          MicroPartitionStatsProvider provider(x.GetStats());
          return filter->ExecSparseFilter(
              provider, RelationGetDescr(rel),
              PaxSparseFilter::StatisticsKind::kFile);
        });
  }
  iterator_ = std::move(iter);
  hold_ = NewHold();
}

BatchHold *VexecPaxReader::NewHold() {
  auto hold = PAX_NEW<BatchHold>();

  hold->cxt = cbdb::AllocSetCtxCreate(cxt_, "PAX vexec batch",
                                      ALLOCSET_DEFAULT_SIZES);
  return hold;
}

// The batch handed out last is done with, unless vexec retains it: its
// memory is reused, and what it held of the group and its values let go.
void VexecPaxReader::ResetHold() {
  if (hold_->retained) {
    retained_.push_back(hold_);
    hold_ = NewHold();
    return;
  }
  hold_->group.reset();
  hold_->values.clear();
  MemoryContextReset(hold_->cxt);
}

void *VexecPaxReader::Alloc(size_t size) {
  return cbdb::MemCtxAlloc(hold_->cxt, Max(size, (size_t)8));
}

void *VexecPaxReader::Alloc0(size_t size) {
  void *p = Alloc(size);

  memset(p, 0, Max(size, (size_t)8));
  return p;
}

bool VexecPaxReader::OpenNextFile() {
  auto file_system = Singleton<LocalFileSystem>::GetInstance();

  if (!iterator_->HasNext()) return false;

  auto meta = iterator_->Next();
  file_id_ = meta.GetMicroPartitionId();

  // the file's deletes, as TableReader::OpenFile() reads them
  // (storage/pax.cc:517-526)
  visimap_ = nullptr;
  const std::string &visimap_file = meta.GetVisibilityBitmapFile();
  if (!visimap_file.empty()) {
    auto file = file_system->Open(visimap_file, fs::kReadMode);
    auto file_length = file->FileLength();
    auto bm = std::make_shared<Bitmap8>(file_length * 8);

    file->ReadN(bm->Raw().bitmap, file_length);
    file->Close();
    visimap_ = std::move(bm);
  }

  std::unique_ptr<File> toast_file;
  if (meta.GetExistToast())
    toast_file = file_system->Open(meta.GetFileName() + TOAST_FILE_SUFFIX,
                                   fs::kReadMode);

  auto reader = std::make_unique<OrcReader>(
      file_system->Open(meta.GetFileName(), fs::kReadMode),
      std::move(toast_file));
  MicroPartitionReader::ReaderOptions options;

  // no reused buffer: each group is read into a buffer of its own, which
  // the batches sliced from it point into
  options.filter = filter_;
  options.visibility_bitmap = visimap_;
  reader->Open(options);

  ngroups_ = reader->GetGroupNums();
  next_group_ = 0;
  next_group_row_ = 0;
  reader_ = std::move(reader);
  return true;
}

void VexecPaxReader::CloseFile() {
  CloseGroup();
  if (reader_) {
    reader_->Close();
    reader_ = nullptr;
  }
  visimap_ = nullptr;
  file_id_ = -1;
}

void VexecPaxReader::CloseGroup() {
  group_open_ = false;
  group_ = nullptr;
  group_rows_ = 0;
  slice_ = 0;
  for (auto &gc : gcols_) gc = GroupColumn();
}

// The next group with rows to hand out, read; false when the files are done.
bool VexecPaxReader::OpenNextGroup() {
  for (;;) {
    if (!reader_ && !OpenNextFile()) return false;
    if (next_group_ >= ngroups_) {
      CloseFile();
      continue;
    }

    size_t g = next_group_++;
    size_t rows = reader_->GetTupleCountsInGroup(g);
    size_t first = next_group_row_;

    next_group_row_ += rows;
    if (rows == 0) continue;

    // the group's statistics, as OrcReader::ReadTuple() prunes by them
    // (orc_reader.cc:230-238)
    if (filter_->SparseFilterEnabled()) {
      auto info = reader_->GetGroupStatsInfo(g);

      if (!filter_->ExecSparseFilter(*info, RelationGetDescr(rel_),
                                     PaxSparseFilter::StatisticsKind::kGroup))
        continue;
    }

    // a group whose rows are all deleted
    if (visimap_ && CountBitsAt(visimap_->Raw().bitmap, visimap_->Raw().size,
                                first, rows) == rows)
      continue;

    group_rows_ = rows;
    group_row_ = first;
    slice_ = 0;
    if (nfetch_ > 0) {
      std::shared_ptr<MicroPartitionReader::Group> group(
          reader_->ReadGroup(g).release());

      CBDB_CHECK(group->GetRows() == rows && group->GetRowOffset() == first,
                 cbdb::CException::ExType::kExTypeLogicError,
                 fmt("PAX group %lu of file %d: %lu rows at %lu, the footer "
                     "says %lu at %lu",
                     g, file_id_, group->GetRows(), group->GetRowOffset(),
                     rows, first));
      group_ = std::move(group);
      for (size_t i = 0; i < ncolumns_; i++) PrepareGroupColumn(i);
    }
    group_open_ = true;
    return true;
  }
}

// What a slice of a column needs of the group's column, found once a
// group: its kind, its buffers, its null bitmap.
void VexecPaxReader::PrepareGroupColumn(int i) {
  const ColumnPlan &plan = plans_[i];
  GroupColumn &gc = gcols_[i];
  const auto &columns = group_->GetAllColumns();

  gc = GroupColumn();
  if (plan.attnum == SelfItemPointerAttributeNumber) return;

  // a column added since the file was written
  if ((size_t)plan.index >= columns->GetColumns()) {
    gc.kind = Kind::kMissing;
    return;
  }

  PaxColumn *column = (*columns)[plan.index].get();
  CBDB_CHECK(column != nullptr, cbdb::CException::ExType::kExTypeLogicError,
             fmt("PAX read no column %d of the group, which the scan reads",
                 plan.index));
  gc.column = column;
  gc.vec = column->GetStorageFormat() == PaxStorageFormat::kTypeStoragePorcVec;
  gc.has_nulls = column->HasNull();
  if (gc.has_nulls) {
    const auto &raw = column->GetBitmap()->Raw();

    gc.nulls = raw.bitmap;
    gc.nulls_len = raw.size;
  }
  gc.has_toast = column->ToastCounts() > 0;
  std::tie(gc.data, gc.data_len) = column->GetBuffer();
  gc.kind = Kind::kRows;

  switch (column->GetPaxColumnTypeInMem()) {
    case PaxColumnTypeInMem::kTypeFixed:
    case PaxColumnTypeInMem::kTypeBitPacked:
      // by-value types; bool in porc, a byte a value
      if (plan.typbyval && column->GetTypeLength() == plan.typlen)
        gc.kind = gc.vec ? Kind::kVecFixed : Kind::kPorcFixed;
      break;
    case PaxColumnTypeInMem::kTypeVecBitPacked:
      if (plan.is_bool && gc.vec) gc.kind = Kind::kVecBits;
      break;
    case PaxColumnTypeInMem::kTypeNonFixed:
    case PaxColumnTypeInMem::kTypeBpChar:
    case PaxColumnTypeInMem::kTypeDecimal:
    case PaxColumnTypeInMem::kTypeVecBpChar:
    case PaxColumnTypeInMem::kTypeVecNoHeader: {
      std::pair<char *, size_t> offsets{nullptr, 0};

      if (gc.vec) {
        auto c = dynamic_cast<PaxVecNonFixedColumn *>(column);

        if (c) offsets = c->GetOffsetBuffer(false);
      } else {
        auto c = dynamic_cast<PaxNonFixedColumn *>(column);

        if (c) offsets = c->GetOffsetBuffer(false);
      }
      gc.offsets = reinterpret_cast<const int32 *>(offsets.first);
      gc.noffsets = offsets.second / sizeof(int32);

      switch (column->GetPaxColumnTypeInMem()) {
        case PaxColumnTypeInMem::kTypeVecBpChar: {
          const auto &attrs = column->GetAttributes();
          auto it = attrs.find(kBpCharNumberKey);

          if (it != attrs.end()) gc.bpchar_n = atoll(it->second.c_str());
          if (plan.typlen == -1 && gc.vec) gc.kind = Kind::kVecBpChar;
          break;
        }
        case PaxColumnTypeInMem::kTypeVecNoHeader:
          if (plan.typlen > 0 && !plan.typbyval && gc.vec)
            gc.kind = Kind::kVecByRef;
          break;
        default:
          if (plan.typlen == -1)
            gc.kind = gc.vec ? Kind::kVecVarlena : Kind::kPorcVarlena;
          else if (plan.typlen > 0 && !plan.typbyval && !gc.vec)
            gc.kind = Kind::kPorcByRef;
          break;
      }
      break;
    }
    case PaxColumnTypeInMem::kTypeVecDecimal:
      if (gc.vec && column->GetTypeLength() == VEC_SHORT_NUMERIC_STORE_BYTES)
        gc.kind = Kind::kVecNumeric;
      break;
    default:
      break;
  }
}

// The rows of a slice that are not deleted, 1 = visible, into *visible, or
// NULL where every row is; returns how many.
size_t VexecPaxReader::Visible(size_t first, size_t n, uint64 **visible) {
  *visible = nullptr;
  if (!visimap_) return n;

  const auto &raw = visimap_->Raw();
  size_t deleted = CountBitsAt(raw.bitmap, raw.size, first, n);

  if (deleted == 0 || deleted == n) return n - deleted;

  uint64 *words = static_cast<uint64 *>(Alloc(Words(n) * 8));

  ExtractBits(raw.bitmap, raw.size, first, n, words);
  for (size_t w = 0; w < Words(n); w++) words[w] = ~words[w];
  if (n % 64 != 0) words[Words(n) - 1] &= (UINT64CONST(1) << (n % 64)) - 1;
  *visible = words;
  return n - deleted;
}

// A slice of a column's null bitmap as validity, 1 = valid, into
// *validity, or NULL where no row of the slice is NULL; returns how many
// rows are not NULL.
size_t VexecPaxReader::Validity(const GroupColumn &gc, size_t s, size_t n,
                                uint64 **validity) {
  *validity = nullptr;
  if (!gc.has_nulls) return n;

  uint64 *words = static_cast<uint64 *>(Alloc(Words(n) * 8));

  ExtractBits(gc.nulls, gc.nulls_len, s, n, words);

  size_t count = CountBits(words, n);
  if (count < n) *validity = words;
  return count;
}

// A slice whose rows are all deleted: porc's columns move past its values.
void VexecPaxReader::SkipSlice(size_t s, size_t n) {
  for (auto &gc : gcols_) {
    if (!gc.column || gc.vec) continue;
    gc.nonnull +=
        gc.has_nulls ? CountBitsAt(gc.nulls, gc.nulls_len, s, n) : n;
  }
}

bool VexecPaxReader::Next(VexecSourceBatch *out, size_t *nvisible) {
  ContextScope scope(cxt_);

  ResetHold();
  for (;;) {
    if (!group_open_ && !OpenNextGroup()) return false;
    if (slice_ >= group_rows_) {
      CloseGroup();
      continue;
    }

    size_t s = slice_;
    size_t n = group_rows_ - s;
    uint64 *visible;

    if (max_rows_ > 0 && n > max_rows_) n = max_rows_;
    slice_ += n;

    *nvisible = Visible(group_row_ + s, n, &visible);
    if (*nvisible == 0) {
      SkipSlice(s, n);
      continue;
    }

    VexecColumn *columns =
        static_cast<VexecColumn *>(Alloc0(sizeof(VexecColumn) * ncolumns_));
    ItemPointerData *tids = nullptr;

    hold_->group = group_;
    for (size_t i = 0; i < ncolumns_; i++) {
      if (plans_[i].attnum == SelfItemPointerAttributeNumber)
        FillTids(s, n, &columns[i], &tids);
      else
        FillColumn(i, s, n, &columns[i]);
    }
    if (want_tids_ && !tids) FillTids(s, n, nullptr, &tids);

    out->nrows = (int)n;
    out->visible = visible;
    out->tids = tids;
    out->columns = columns;
    out->owner = hold_;
    return true;
  }
}

void VexecPaxReader::SetShape(VexecColumn *out, int layout, int width,
                              int stride, int scale) {
  out->layout = (uint8)layout;
  out->encoding = VEXEC_FLAT;
  out->arrow_values = false;
  out->width = width;
  out->stride = stride;
  out->scale = (int16)scale;
}

// The rows' TIDs, of the table's layout (pax_tid.h); as a column too where
// vexec asks for ctid among the columns.
void VexecPaxReader::FillTids(size_t s, size_t n, VexecColumn *out,
                              ItemPointerData **tids) {
  if (!*tids) {
    auto t = static_cast<ItemPointerData *>(Alloc(sizeof(ItemPointerData) * n));

    for (size_t i = 0; i < n; i++)
      t[i] = PaxTidToTable(MakeCTID(file_id_, group_row_ + s + i), file_bits_);
    *tids = t;
  }
  if (out) {
    SetShape(out, VEXEC_FIXED, sizeof(ItemPointerData),
             sizeof(ItemPointerData), 0);
    out->values = *tids;
  }
}

void VexecPaxReader::FillColumn(int i, size_t s, size_t n, VexecColumn *out) {
  const ColumnPlan &plan = plans_[i];
  GroupColumn &gc = gcols_[i];
  uint64 *validity;
  size_t nvalid;

  if (gc.kind == Kind::kMissing) {
    FillMissing(plan, out);
    return;
  }

  nvalid = Validity(gc, s, n, &validity);
  out->validity = validity;
  switch (gc.kind) {
    case Kind::kPorcFixed:
      FillPorcFixed(plan, gc, n, validity, nvalid, out);
      break;
    case Kind::kPorcVarlena:
      FillPorcVarlena(plan, gc, s, n, validity, out);
      break;
    case Kind::kPorcByRef:
      FillPorcByRef(plan, gc, n, validity, out);
      break;
    case Kind::kVecFixed:
      FillVecFixed(plan, gc, s, n, out);
      break;
    case Kind::kVecBits:
      FillVecBits(gc, s, n, out);
      break;
    case Kind::kVecVarlena:
      if (!FillVecVarlena(gc, s, n, out))
        FillRows(plan, gc, s, n, validity, out);
      break;
    case Kind::kVecBpChar:
      if (!FillVecBpChar(gc, s, n, validity, out))
        FillRows(plan, gc, s, n, validity, out);
      break;
    case Kind::kVecByRef:
      FillVecByRef(plan, gc, s, n, validity, out);
      break;
    case Kind::kVecNumeric:
      FillVecNumeric(plan, gc, s, n, validity, out);
      break;
    default:
      FillRows(plan, gc, s, n, validity, out);
      break;
  }
}

// A column the file lacks: its attmissing value for every row, or NULL, as
// the slot path fills it (OrcGroup::ReadTuple() calls
// slot_getmissingattrs(), orc_group.cc:155-158).
void VexecPaxReader::FillMissing(const ColumnPlan &plan, VexecColumn *out) {
  if (plan.typlen == -1) {
    auto datums = static_cast<Datum *>(Alloc0(sizeof(Datum)));

    SetShape(out, VEXEC_DATUM, 0, 0, 0);
    datums[0] = plan.missing_present ? plan.missing_value : (Datum)0;
    out->values = datums;
  } else if (plan.is_bool) {
    auto bytes = static_cast<uint8 *>(Alloc0(8));

    SetShape(out, VEXEC_BYTE_BOOL, 1, 1, 0);
    bytes[0] = plan.missing_present && DatumGetBool(plan.missing_value);
    out->values = bytes;
  } else if (plan.typbyval) {
    auto value = static_cast<char *>(Alloc0(8));

    SetShape(out, VEXEC_FIXED, plan.typlen, plan.typlen, 0);
    if (plan.missing_present)
      store_att_byval(value, plan.missing_value, plan.typlen);
    out->values = value;
  } else {
    auto value = static_cast<char *>(Alloc0(plan.pg_stride));

    SetShape(out, VEXEC_FIXED, plan.typlen, plan.pg_stride, 0);
    if (plan.missing_present)
      memcpy(value, DatumGetPointer(plan.missing_value), plan.typlen);
    out->values = value;
  }
  out->encoding = VEXEC_CONST;
  if (!plan.missing_present) out->validity = static_cast<uint64 *>(Alloc0(8));
}

// porc, a by-value type: the values of the rows that are not NULL, densely
// (the writer appends none for a NULL, orc_writer.cc:452-455).  A slice
// without a NULL is zero-copy where its values are aligned; one with NULLs
// is scattered, its NULL rows zeroed.  bool is a byte a value, 0 or 1.
void VexecPaxReader::FillPorcFixed(const ColumnPlan &plan, GroupColumn &gc,
                                   size_t n, const uint64 *validity,
                                   size_t nvalid, VexecColumn *out) {
  size_t width = plan.typlen;
  const char *src = gc.data + gc.nonnull * width;

  CBDB_CHECK((gc.nonnull + nvalid) * width <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column %d has %lu bytes, not the %lu its rows need",
                 plan.index, gc.data_len, (gc.nonnull + nvalid) * width));
  if (plan.is_bool)
    SetShape(out, VEXEC_BYTE_BOOL, 1, 1, 0);
  else
    SetShape(out, VEXEC_FIXED, width, width, 0);

  if (!validity && Aligned(src, width)) {
    out->values = src;
  } else if (!validity) {
    char *dst = static_cast<char *>(Alloc(n * width));

    memcpy(dst, src, n * width);
    out->values = dst;
  } else {
    char *dst = static_cast<char *>(Alloc0(n * width));
    size_t k = 0;

    for (size_t i = 0; i < n; i++) {
      if (!BitSet(validity, i)) continue;
      switch (width) {
        case 1:
          dst[i] = src[k];
          break;
        case 2:
          memcpy(dst + i * 2, src + k * 2, 2);
          break;
        case 4:
          memcpy(dst + i * 4, src + k * 4, 4);
          break;
        case 8:
          memcpy(dst + i * 8, src + k * 8, 8);
          break;
        default:
          memcpy(dst + i * width, src + k * width, width);
          break;
      }
      k++;
    }
    out->values = dst;
  }
  gc.nonnull += nvalid;
}

// porc, varlena: the values are kept with their headers, so a row's Datum
// points at its value where it lies, as the slot path's does
// (PaxNonFixedColumn::GetDatum(), storage/columns/pax_column.cc).  A PAX
// toast -- compressed, or in the file's .toast -- is detoasted into memory
// the batch holds.  The column's toast map is by row: GetDatum() is given
// the value's index among those not NULL (orc_group.cc:63), which is not
// the row's where a NULL comes before it in the group, so the row's is
// asked here.
//
// Where the batch asks for views, they are built over the same bytes,
// past each value's header, the Datums kept beside them (§3.4.2): no byte
// is copied but the 12 a view holds inline.
void VexecPaxReader::FillPorcVarlena(const ColumnPlan &plan, GroupColumn &gc,
                                     size_t s, size_t n,
                                     const uint64 *validity, VexecColumn *out) {
  auto datums = static_cast<Datum *>(Alloc0(sizeof(Datum) * n));
  size_t k = gc.nonnull;
  bool detoasted = false;
  std::shared_ptr<DataBuffer<char>> external;

  if (gc.has_toast) external = gc.column->GetExternalToastDataBuffer();
  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;
    CBDB_CHECK(k + 1 < gc.noffsets && (size_t)gc.offsets[k] < gc.data_len,
               cbdb::CException::ExType::kExTypeOutOfRange,
               fmt("PAX column %d has no value %lu", plan.index, k));

    Datum d = PointerGetDatum(gc.data + gc.offsets[k]);
    if (gc.has_toast && gc.column->IsToast(s + i)) {
      auto detoast =
          pax_detoast(d, external ? external->Start() : nullptr,
                      external ? external->Used() : 0);

      d = detoast.first;
      if (detoast.second) {
        hold_->values.emplace_back(std::move(detoast.second));
        detoasted = true;
      }
    }
    datums[i] = d;
    k++;
  }
  gc.nonnull = k;

  SetShape(out, VEXEC_DATUM, 0, 0, 0);
  out->values = datums;
  if (plan.want != VEXEC_VIEW || gc.data_len > (size_t)PG_INT32_MAX) return;

  // Views: buffer 0 the group's values, then each value detoasted for the
  // batch, a buffer of its own.
  int nbuffers = 1 + (detoasted ? (int)hold_->values.size() : 0);
  auto buffers =
      static_cast<const void **>(Alloc0(sizeof(void *) * nbuffers));
  auto sizes = static_cast<int64 *>(Alloc0(sizeof(int64) * nbuffers));
  auto views = static_cast<BinaryView *>(Alloc0(sizeof(BinaryView) * n));
  int nextra = 1;

  buffers[0] = gc.data ? gc.data : kEmpty;
  sizes[0] = gc.data_len;
  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;

    struct varlena *vl = (struct varlena *)DatumGetPointer(datums[i]);
    const char *p = VARDATA_ANY(vl);
    size_t len = VARSIZE_ANY_EXHDR(vl);
    BinaryView *v = &views[i];

    v->size = (int32)len;
    if (len <= sizeof(v->inlined)) {
      memcpy(v->inlined, p, len);
      continue;
    }
    memcpy(v->ref.prefix, p, 4);
    if ((const char *)vl >= gc.data && (const char *)vl < gc.data + gc.data_len) {
      v->ref.buffer_index = 0;
      v->ref.offset = (int32)(p - gc.data);
    } else {
      // a value detoasted for the batch: the buffer is the value's own
      v->ref.buffer_index = nextra;
      v->ref.offset = 0;
      buffers[nextra] = p;
      sizes[nextra] = len;
      nextra++;
    }
  }
  SetShape(out, VEXEC_VIEW, 0, 0, 0);
  out->values = views;
  out->buffers = buffers;
  out->buffer_sizes = sizes;
  out->nbuffers = nextra;
  out->datums = datums;
}

// porc, a fixed-length by-reference type -- uuid, interval, name: its bytes
// without a header at the value's offset (orc_writer.cc:524-527), copied at
// PostgreSQL's array stride.
void VexecPaxReader::FillPorcByRef(const ColumnPlan &plan, GroupColumn &gc,
                                   size_t n, const uint64 *validity,
                                   VexecColumn *out) {
  char *dst = static_cast<char *>(Alloc0((size_t)plan.pg_stride * n));
  size_t k = gc.nonnull;

  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;
    CBDB_CHECK(k + 1 < gc.noffsets &&
                   (size_t)gc.offsets[k] + plan.typlen <= gc.data_len,
               cbdb::CException::ExType::kExTypeOutOfRange,
               fmt("PAX column %d has no value %lu", plan.index, k));
    memcpy(dst + (size_t)plan.pg_stride * i, gc.data + gc.offsets[k],
           plan.typlen);
    k++;
  }
  gc.nonnull = k;
  SetShape(out, VEXEC_FIXED, plan.typlen, plan.pg_stride, 0);
  out->values = dst;
}

// porc_vec, a by-value type: a value a row, NULL rows zeroed
// (PaxVecCommColumn::AppendNull(), pax_vec_column.cc): zero-copy where the
// slice is aligned.
void VexecPaxReader::FillVecFixed(const ColumnPlan &plan, GroupColumn &gc,
                                  size_t s, size_t n, VexecColumn *out) {
  size_t width = plan.typlen;
  const char *src = gc.data + s * width;

  CBDB_CHECK((s + n) * width <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column %d has %lu bytes, not the %lu its rows need",
                 plan.index, gc.data_len, (s + n) * width));
  if (plan.is_bool)
    SetShape(out, VEXEC_BYTE_BOOL, 1, 1, 0);
  else
    SetShape(out, VEXEC_FIXED, width, width, 0);
  if (Aligned(src, width)) {
    out->values = src;
  } else {
    char *dst = static_cast<char *>(Alloc(n * width));

    memcpy(dst, src, n * width);
    out->values = dst;
  }
}

// porc_vec, bool: a bit a row, least significant first, as Arrow's
// (PaxVecBitPackedColumn, pax_vec_bitpacked_column.cc): the slice's words,
// copied so that they start at a word and end with their tail cleared.
void VexecPaxReader::FillVecBits(GroupColumn &gc, size_t s, size_t n,
                                 VexecColumn *out) {
  auto bits = static_cast<uint64 *>(Alloc(Words(n) * 8));

  ExtractBits(reinterpret_cast<const uint8 *>(gc.data), gc.data_len, s, n,
              bits);
  SetShape(out, VEXEC_BIT_BOOL, 1, 1, 0);
  out->values = bits;
}

// Whether a row of a slice holds a PAX toast.
bool VexecPaxReader::SliceHasToast(GroupColumn &gc, size_t s, size_t n) {
  if (!gc.has_toast) return false;
  for (size_t i = s; i < s + n; i++)
    if (gc.column->IsToast(i)) return true;
  return false;
}

// porc_vec, varlena: the payloads without their headers, at offsets a row
// each, an empty one for a NULL (PaxVecNonFixedColumn, pax_vec_column.cc):
// Arrow's binary, handed zero-copy, its offsets rebased to start at 0.  A
// slice with a PAX toast, whose bytes are the toast's, is not.
bool VexecPaxReader::FillVecVarlena(GroupColumn &gc, size_t s, size_t n,
                                    VexecColumn *out) {
  if (SliceHasToast(gc, s, n)) return false;
  CBDB_CHECK(s + n < gc.noffsets &&
                 (size_t)gc.offsets[s + n] <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column's offsets end before row %lu", s + n));

  int32 base = gc.offsets[s];
  auto offsets = static_cast<int32 *>(Alloc(sizeof(int32) * (n + 1)));
  auto buffers = static_cast<const void **>(Alloc(sizeof(void *)));
  auto sizes = static_cast<int64 *>(Alloc(sizeof(int64)));

  for (size_t i = 0; i <= n; i++) offsets[i] = gc.offsets[s + i] - base;
  buffers[0] = gc.data ? gc.data + base : kEmpty;
  sizes[0] = offsets[n];
  SetShape(out, VEXEC_OFFSETS, 0, 0, 0);
  out->values = offsets;
  out->buffers = buffers;
  out->buffer_sizes = sizes;
  out->nbuffers = 1;
  return true;
}

// porc_vec, char(n): stored without its trailing blanks, which the slot
// path pads back to the n bytes of the column's first value
// (PaxVecBpCharColumn::GetBuffer(), pax_vec_bpchar_column.cc:72-104), as
// here, into a copy.  A slice with a PAX toast, or one whose file did not
// record n, is read as the slot path reads it.
bool VexecPaxReader::FillVecBpChar(GroupColumn &gc, size_t s, size_t n,
                                   const uint64 *validity, VexecColumn *out) {
  if (SliceHasToast(gc, s, n)) return false;
  CBDB_CHECK(s + n < gc.noffsets &&
                 (size_t)gc.offsets[s + n] <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column's offsets end before row %lu", s + n));

  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;
    if (gc.bpchar_n <= 0) return false;

    size_t len = gc.offsets[s + i + 1] - gc.offsets[s + i];
    total += Max(len, (size_t)gc.bpchar_n);
  }
  CBDB_CHECK(total <= (size_t)PG_INT32_MAX,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("char(n) values of %lu bytes in a batch", total));

  auto offsets = static_cast<int32 *>(Alloc(sizeof(int32) * (n + 1)));
  auto bytes = static_cast<char *>(Alloc(total));
  auto buffers = static_cast<const void **>(Alloc(sizeof(void *)));
  auto sizes = static_cast<int64 *>(Alloc(sizeof(int64)));
  size_t pos = 0;

  for (size_t i = 0; i < n; i++) {
    offsets[i] = (int32)pos;
    if (validity && !BitSet(validity, i)) continue;

    size_t len = gc.offsets[s + i + 1] - gc.offsets[s + i];
    memcpy(bytes + pos, gc.data + gc.offsets[s + i], len);
    if (len < (size_t)gc.bpchar_n) {
      memset(bytes + pos + len, ' ', gc.bpchar_n - len);
      len = gc.bpchar_n;
    }
    pos += len;
  }
  offsets[n] = (int32)pos;
  buffers[0] = bytes;
  sizes[0] = pos;
  SetShape(out, VEXEC_OFFSETS, 0, 0, 0);
  out->values = offsets;
  out->buffers = buffers;
  out->buffer_sizes = sizes;
  out->nbuffers = 1;
  return true;
}

// porc_vec, a fixed-length by-reference type: its bytes without a header
// at offsets, none for a NULL (PaxVecNoHdrColumn,
// pax_vec_no_hdr_column.h).  A slice without a NULL lies packed: zero-copy
// where aligned; any other is copied at PostgreSQL's array stride.
void VexecPaxReader::FillVecByRef(const ColumnPlan &plan, GroupColumn &gc,
                                  size_t s, size_t n, const uint64 *validity,
                                  VexecColumn *out) {
  CBDB_CHECK(s + n < gc.noffsets &&
                 (size_t)gc.offsets[s + n] <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column's offsets end before row %lu", s + n));

  const char *src = gc.data + gc.offsets[s];
  if (!validity &&
      (size_t)(gc.offsets[s + n] - gc.offsets[s]) == n * plan.typlen &&
      Aligned(src, plan.alignby)) {
    SetShape(out, VEXEC_FIXED, plan.typlen, plan.typlen, 0);
    out->values = src;
    return;
  }

  char *dst = static_cast<char *>(Alloc0((size_t)plan.pg_stride * n));
  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;
    CBDB_CHECK(gc.offsets[s + i + 1] - gc.offsets[s + i] == plan.typlen,
               cbdb::CException::ExType::kExTypeOutOfRange,
               fmt("PAX column %d's value of row %lu is not %d bytes",
                   plan.index, s + i, plan.typlen));
    memcpy(dst + (size_t)plan.pg_stride * i, gc.data + gc.offsets[s + i],
           plan.typlen);
  }
  SetShape(out, VEXEC_FIXED, plan.typlen, plan.pg_stride, 0);
  out->values = dst;
}

// porc_vec, numeric: 16 bytes a row (comm/vec_numeric.cc).  The first 8 are
// the low bits of a 120-bit two's-complement integer; of the next 8, the
// top 2 say finite (0) or NaN and the infinities, the next 6 are the
// value's display scale, and the low 56 are the integer's high bits:
// pg_short_numeric_to_vec_short_numeric() writes the value times 10 to its
// display scale.  A typmod gives every finite value its scale, so where
// vexec scales the column (§3.4.1) the integer is its scaled form, read
// here without a numeric made.  A slice with a value that is not finite or
// not at that scale -- or at one vec_short_numeric_to_datum() reads as NaN
// -- is handed as the Datums the slot path makes
// (PaxShortNumericColumn::GetDatum(), pax_vec_numeric_column.cc).
void VexecPaxReader::FillVecNumeric(const ColumnPlan &plan, GroupColumn &gc,
                                    size_t s, size_t n, const uint64 *validity,
                                    VexecColumn *out) {
  const size_t width = VEC_SHORT_NUMERIC_STORE_BYTES;
  const char *src = gc.data + s * width;

  CBDB_CHECK((s + n) * width <= gc.data_len,
             cbdb::CException::ExType::kExTypeOutOfRange,
             fmt("PAX column %d has %lu bytes, not the %lu its rows need",
                 plan.index, gc.data_len, (s + n) * width));

  bool scaled = plan.scaled_width > 0 &&
                plan.scale <= VEC_SHORT_NUMERIC_MAX_SCALE;
  char *dst = nullptr;

  if (scaled) dst = static_cast<char *>(Alloc0((size_t)plan.scaled_width * n));
  for (size_t i = 0; scaled && i < n; i++) {
    uint64 lo, hi;

    if (validity && !BitSet(validity, i)) continue;
    memcpy(&lo, src + i * width, sizeof(uint64));
    memcpy(&hi, src + i * width + sizeof(uint64), sizeof(uint64));
    if ((hi >> 62) != 0 || (int16)((hi >> 56) & 0x3f) != plan.scale) {
      scaled = false;
      break;
    }

    // the 56 high bits, sign-extended from their top
    int64 high = (int64)(hi << 8) >> 8;
    int128 value = (int128)(((uint128)(uint64)high << 64) | lo);

    if (plan.scaled_width == 8) {
      if (value < PG_INT64_MIN || value > PG_INT64_MAX) {
        scaled = false;
        break;
      }
      int64 v64 = (int64)value;
      memcpy(dst + i * 8, &v64, sizeof(int64));
    } else {
      memcpy(dst + i * 16, &value, sizeof(int128));
    }
  }
  if (scaled) {
    SetShape(out, VEXEC_SCALED, plan.scaled_width, plan.scaled_width,
             plan.scale);
    out->values = dst;
    return;
  }

  auto datums = static_cast<Datum *>(Alloc0(sizeof(Datum) * n));
  ContextScope scope(hold_->cxt);

  CBDB_WRAP_START;
  {
    for (size_t i = 0; i < n; i++) {
      if (validity && !BitSet(validity, i)) continue;
      datums[i] = vec_short_numeric_to_datum(
          reinterpret_cast<const int64 *>(src + i * width),
          reinterpret_cast<const int64 *>(src + i * width + sizeof(int64)));
    }
  }
  CBDB_WRAP_END;
  SetShape(out, VEXEC_DATUM, 0, 0, 0);
  out->values = datums;
}

// Any other column: its values as the slot path reads them, GetDatum() of
// the value's index among those not NULL in porc (OrcGroup's
// GetColumnDatum(), orc_group.cc:48-69), of the row in porc_vec
// (OrcVecGroup's, orc_vec_group.cc:36-46), into the layout its type has.
void VexecPaxReader::FillRows(const ColumnPlan &plan, GroupColumn &gc,
                              size_t s, size_t n, const uint64 *validity,
                              VexecColumn *out) {
  void *values;
  size_t k = gc.nonnull;
  ContextScope scope(hold_->cxt);

  if (plan.typlen == -1) {
    values = Alloc0(sizeof(Datum) * n);
    SetShape(out, VEXEC_DATUM, 0, 0, 0);
  } else if (plan.is_bool) {
    values = Alloc0(n);
    SetShape(out, VEXEC_BYTE_BOOL, 1, 1, 0);
  } else if (plan.typbyval) {
    values = Alloc0((size_t)plan.typlen * n);
    SetShape(out, VEXEC_FIXED, plan.typlen, plan.typlen, 0);
  } else {
    values = Alloc0((size_t)plan.pg_stride * n);
    SetShape(out, VEXEC_FIXED, plan.typlen, plan.pg_stride, 0);
  }

  for (size_t i = 0; i < n; i++) {
    if (validity && !BitSet(validity, i)) continue;

    Datum d = gc.column->GetDatum(gc.vec ? s + i : k++);

    if (plan.typlen == -1)
      static_cast<Datum *>(values)[i] = d;
    else if (plan.is_bool)
      static_cast<uint8 *>(values)[i] = DatumGetBool(d);
    else if (plan.typbyval)
      store_att_byval(static_cast<char *>(values) + (size_t)plan.typlen * i,
                      d, plan.typlen);
    else
      memcpy(static_cast<char *>(values) + (size_t)plan.pg_stride * i,
             DatumGetPointer(d), plan.typlen);
  }
  if (!gc.vec) gc.nonnull = k;
  out->values = values;
}

void VexecPaxReader::Retain(void *owner) {
  if (owner == hold_) {
    hold_->retained = true;
    return;
  }
  for (auto h : retained_)
    if (h == owner) h->retained = true;
}

// A retained batch let go: the one handed out last is reused by the next
// call; an older one is freed.
void VexecPaxReader::Release(void *owner) {
  if (owner == hold_) {
    hold_->retained = false;
    return;
  }
  for (auto it = retained_.begin(); it != retained_.end(); ++it) {
    if (*it != owner) continue;

    BatchHold *h = *it;
    retained_.erase(it);
    cbdb::MemoryCtxDelete(h->cxt);
    PAX_DELETE(h);
    return;
  }
}

// From the first file again, after table_rescan(), which leaves PAX's own
// reader alone: vexec reads none.
void VexecPaxReader::Rescan() {
  ContextScope scope(cxt_);

  CloseFile();
  ResetHold();
  iterator_->Rewind();
}

// The files closed, the aux table's scan ended, the snapshot unregistered,
// the memory freed; the sparse filter's counts logged under pax.enable_debug,
// as PaxScanDesc::EndScan() logs them (pax_scanner.cc:366-368).
void VexecPaxReader::End() {
  if (pax_enable_debug && filter_) filter_->LogStatistics();
  CloseFile();
  if (iterator_) {
    iterator_->Release();
    iterator_ = nullptr;
  }
  if (aux_snapshot_) {
    CBDB_WRAP_START;
    { UnregisterSnapshot(aux_snapshot_); }
    CBDB_WRAP_END;
    aux_snapshot_ = nullptr;
  }
  for (auto h : retained_) PAX_DELETE(h);
  retained_.clear();
  if (hold_) {
    PAX_DELETE(hold_);
    hold_ = nullptr;
  }
  filter_ = nullptr;
  cbdb::MemoryCtxDelete(cxt_);
  cxt_ = nullptr;
}

}  // namespace pax

/*
 * The source's callbacks, C, as vexec calls them: C++ exceptions become
 * errors here.
 */
extern "C" {

static bool PaxVexecSupports(Relation rel, AttrNumber attnum);
static void *PaxVexecBegin(TableScanDesc scan, const VexecSourceSpec *spec);
static bool PaxVexecNext(void *state, VexecSourceBatch *out);
static void PaxVexecRetain(void *state, void *owner);
static void PaxVexecRelease(void *state, void *owner);
static void PaxVexecRescan(void *state);
static void PaxVexecEnd(void *state);

/*
 * Every attribute PAX stores: by-value, fixed-length by-reference and
 * varlena -- the kinds BuildSchema() gives a column type
 * (storage/orc/orc_type.cc:37-78) -- and ctid.  Not a dropped attribute,
 * nor another system one, which vexec makes itself.
 */
static bool PaxVexecSupports(Relation rel, AttrNumber attnum) {
  TupleDesc desc = RelationGetDescr(rel);
  Form_pg_attribute att;

  if (attnum == SelfItemPointerAttributeNumber) return true;
  if (attnum < 1 || attnum > desc->natts) return false;
  att = TupleDescAttr(desc, attnum - 1);
  if (att->attisdropped) return false;
  return att->attlen == -1 || att->attlen > 0;
}

static void *PaxVexecBegin(TableScanDesc scan, const VexecSourceSpec *spec) {
  /*
   * A slice's deleted rows are handed with the rest, marked in its visible
   * bitmap; vexec compacts a batch itself where it pays.  A scan that asked
   * for them left out would read them, so it is refused.
   */
  if (spec->flags & VEXEC_SRC_COMPACT)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("PAX's batch reader does not compact its batches"),
             errdetail("A scan asked for VEXEC_SRC_COMPACT; PAX marks the "
                       "rows it has deleted in a batch's visible bitmap.")));

  CBDB_TRY();
  {
    auto reader = pax::PAX_NEW<pax::VexecPaxReader>(scan, spec);

    pax::common::RememberResourceCallback(
        pax::ReleaseTopObject<pax::VexecPaxReader>,
        cbdb::PointerToDatum(reader));
    reader->Begin();
    return reader;
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  pg_unreachable();
}

static bool PaxVexecNext(void *state, VexecSourceBatch *out) {
  auto reader = static_cast<pax::VexecPaxReader *>(state);
  size_t nvisible = 0;
  bool found = false;

  CHECK_FOR_INTERRUPTS();
  CBDB_TRY();
  { found = reader->Next(out, &nvisible); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();

  /*
   * The rows a scan returns, as the method counts them a row at a time
   * (pgstat_count_heap_getnext(), access/pax_access_handle.cc:268).
   */
  if (found && nvisible > 0) {
    Relation rel = reader->GetRelation();

    if (pgstat_should_count_relation(rel))
      rel->pgstat_info->counts.tuples_returned += nvisible;
  }
  return found;
}

static void PaxVexecRetain(void *state, void *owner) {
  CBDB_TRY();
  { static_cast<pax::VexecPaxReader *>(state)->Retain(owner); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

static void PaxVexecRelease(void *state, void *owner) {
  CBDB_TRY();
  { static_cast<pax::VexecPaxReader *>(state)->Release(owner); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

static void PaxVexecRescan(void *state) {
  CBDB_TRY();
  { static_cast<pax::VexecPaxReader *>(state)->Rescan(); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

static void PaxVexecEnd(void *state) {
  CBDB_TRY();
  {
    auto reader = static_cast<pax::VexecPaxReader *>(state);

    reader->End();
    pax::common::ForgetResourceCallback(
        pax::ReleaseTopObject<pax::VexecPaxReader>,
        cbdb::PointerToDatum(reader));
    pax::PAX_DELETE(reader);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

static VexecSourceRoutine pax_vexec_source;

/*
 * The routine pax.c registers from _PG_init (pax_module.h), keyed by PAX's
 * table access method, and named as EXPLAIN shows a VecScan's source.
 */
const VexecSourceRoutine *PaxVexecSource(void) {
  if (pax_vexec_source.size == 0) {
    pax_vexec_source.size = sizeof(VexecSourceRoutine);
    pax_vexec_source.minor = VEXEC_SOURCE_MINOR;
    pax_vexec_source.am = PaxTableAmRoutine();
    pax_vexec_source.name = "pax";
    pax_vexec_source.supports = PaxVexecSupports;
    pax_vexec_source.begin = PaxVexecBegin;
    pax_vexec_source.next = PaxVexecNext;
    pax_vexec_source.retain = PaxVexecRetain;
    pax_vexec_source.release = PaxVexecRelease;
    pax_vexec_source.rescan = PaxVexecRescan;
    pax_vexec_source.end = PaxVexecEnd;
    pax_vexec_source.estimate = nullptr;
  }
  return &pax_vexec_source;
}

}  // extern "C"
