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
 * pax_access_method_internal.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_access_method_internal.cc
 *
 *
 * Ported to PostgreSQL 19:
 *   - a table's row of pg_pax_tables names its storage, which a TRUNCATE
 *     and ALTER TABLE ... SET TABLESPACE change (catalog/pg_pax_tables.cc);
 *   - relation_copy_for_cluster takes the rewrite's snapshot;
 *   - the size is the table's for all its forks, as O19's size functions ask
 *     with InvalidForkNumber, as for its main fork: PAX's files are all it
 *     has;
 *   - Cloudberry's swap_relation_files callback is gone, the catalog
 *     following the storage instead, and so are its custom object classes,
 *     which PostgreSQL 19's dependencies do not have: the module's object
 *     access hook deletes a dropped table's rows.
 *-------------------------------------------------------------------------
 */

#include "access/pax_access_handle.h"

#include "comm/cbdb_api.h"

#include "access/pax_table_cluster.h"
#include "catalog/pax_catalog.h"
#include "comm/cbdb_wrappers.h"
#include "comm/paxc_wrappers.h"
#include "comm/singleton.h"
#include "exceptions/CException.h"
#include "storage/wal/paxc_wal.h"

#define RELATION_IS_PAX(rel) \
  (OidIsValid((rel)->rd_rel->relam) && RelationIsPAX(rel))

namespace paxc {
static void pax_disallow_dfs_tablespace(Oid reltablespace) {
  if (!OidIsValid(reltablespace))
    reltablespace = MyDatabaseTableSpace;

  if (paxc::IsDfsTablespaceById(reltablespace))
    ereport(ERROR, (errmsg("pax unsupport dfs tablespace:%u", reltablespace)));
}
}

#ifdef USE_MANIFEST_API
namespace pax {

void CCPaxAccessMethod::RelationSetNewFilenode(Relation rel,
                                               const RelFileNode *newrnode,
                                               char persistence,
                                               TransactionId *freeze_xid,
                                               MultiXactId *minmulti) {
  *freeze_xid = *minmulti = InvalidTransactionId;

  paxc::pax_disallow_dfs_tablespace(rel->rd_rel->reltablespace);

  manifest_create(rel, *newrnode);
}

void CCPaxAccessMethod::RelationNontransactionalTruncate(Relation rel) {
  manifest_truncate(rel);
}

void CCPaxAccessMethod::RelationCopyData(Relation rel,
                                         const RelFileNode *newrnode) {
  CBDB_TRY();
  {
    cbdb::RelOpenSmgr(rel);
    cbdb::PaxRelationCreateStorage(*newrnode, rel);
    pax::PaxCopyAllDataFiles(rel, newrnode, true);
    cbdb::RelDropStorage(rel);
    cbdb::RelCloseSmgr(rel);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_END_TRY();
}

void CCPaxAccessMethod::RelationCopyForCluster(
    Relation old_rel, Relation new_rel, Relation old_index, bool use_sort,
    TransactionId /*oldest_xmin*/, TransactionId * /*xid_cutoff*/,
    MultiXactId * /*multi_cutoff*/, double * /*num_tuples*/,
    double * /*tups_vacuumed*/, double * /* tups_recently_dead*/) {
  Assert(RelationIsPAX(old_rel));
  Assert(RelationIsPAX(new_rel));
  if (!use_sort && old_index == nullptr) {
    pax::CCPaxAccessMethod::ExtDmlInit(new_rel, CMD_INSERT);
    paxc::CPaxCopyAllTuples(old_rel, new_rel, nullptr);
    pax::CCPaxAccessMethod::ExtDmlFini(new_rel, CMD_INSERT);
    return;
  }

  // like aocs, pax tables can only be clustered against a btree index
  if (!(old_index && old_index->rd_rel->relam == BTREE_AM_OID))
    elog(ERROR, "only btree index is supported for pax table clustering");

  CBDB_TRY();
  {
    pax::IndexCluster(old_rel, new_rel, old_index, GetActiveSnapshot());
  }
  CBDB_CATCH_DEFAULT();
  CBDB_END_TRY();
}

} // namespace pax

namespace paxc {
uint64 PaxAccessMethod::RelationSize(Relation rel, ForkNumber fork_number) {
  Assert(RelationIsPAX(rel));
  if (fork_number != MAIN_FORKNUM) return 0;

  ManifestRelation mrel;
  ManifestScan mscan;
  ManifestTuple mtuple;
  uint64 pax_size = 0;

  mrel = manifest_open(rel);
  mscan = manifest_beginscan(mrel, nullptr);
  while ((mtuple = manifest_getnext(mscan, nullptr))) {
    Datum datum;
    bool isnull;
    datum = get_manifesttuple_value(mtuple, mrel, PAX_AUX_PTBLOCKSIZE, &isnull);
    Assert(!isnull);
    pax_size += DatumGetUInt32(datum);
  }
  manifest_endscan(mscan);
  manifest_close(mrel);

  return Int64GetDatum(pax_size);
}

void PaxAccessMethod::EstimateRelSize(Relation rel, int32 * /*attr_widths*/,
                                      BlockNumber *pages, double *tuples,
                                      double *allvisfrac) {
  Assert(RelationIsPAX(rel));

  ManifestRelation mrel;
  ManifestScan mscan;
  ManifestTuple mtuple;
  uint64 pax_size = 0;
  uint64 total_tuples = 0;

  // Even an empty table takes at least one page,
  // but number of tuples for an empty table could be 0.
  *tuples = 0;
  *pages = 1;
  // index-only scan is not supported in PAX
  *allvisfrac = 0;

  mrel = manifest_open(rel);
  mscan = manifest_beginscan(mrel, nullptr);
  while ((mtuple = manifest_getnext(mscan, nullptr))) {
    Datum datum;
    bool isnull;

    datum = get_manifesttuple_value(mtuple, mrel, PAX_AUX_PTTUPCOUNT, &isnull);
    Assert(!isnull);
    total_tuples += DatumGetUInt32(datum);

    datum = get_manifesttuple_value(mtuple, mrel, PAX_AUX_PTBLOCKSIZE, &isnull);
    Assert(!isnull);
    pax_size += DatumGetUInt32(datum);
  }
  manifest_endscan(mscan);
  manifest_close(mrel);

  *tuples = static_cast<double>(total_tuples);
  *pages = RelationGuessNumberOfBlocksFromSize(pax_size);
}

void PaxAccessMethod::SwapRelationFiles(Oid relid1, Oid relid2,
                                        TransactionId frozen_xid,
                                        MultiXactId cutoff_multi) {
  manifest_swap_table(relid1, relid2, frozen_xid, cutoff_multi);
}
} // namespace paxc
#else

#include "storage/file_system.h"
#include "storage/local_file_system.h"

namespace pax {

void CCPaxAccessMethod::RelationSetNewFilenode(Relation rel,
                                               const RelFileLocator *newrlocator,
                                               char persistence,
                                               TransactionId *freeze_xid,
                                               MultiXactId *minmulti) {
  Relation pax_tables_rel;
  ScanKeyData scan_key[1];
  SysScanDesc scan;
  HeapTuple tuple;
  Oid pax_relid;
  bool exists;

  *freeze_xid = *minmulti = InvalidTransactionId;

  paxc::pax_disallow_dfs_tablespace(rel->rd_rel->reltablespace);

  pax_tables_rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);
  pax_relid = RelationGetRelid(rel);

  ScanKeyInit(&scan_key[0], ANUM_PG_PAX_TABLES_RELID, BTEqualStrategyNumber,
              F_OIDEQ, ObjectIdGetDatum(pax_relid));
  scan = systable_beginscan(pax_tables_rel, PAX_TABLES_RELID_INDEX_ID, true,
                            NULL, 1, scan_key);
  tuple = systable_getnext(scan);
  exists = HeapTupleIsValid(tuple);
  if (exists) {
    Oid aux_relid;

    // set new filenode, not create new table
    //
    // 1. truncate aux table by new relfilenode
    aux_relid = ::paxc::GetPaxAuxRelid(pax_relid);
    Assert(OidIsValid(aux_relid));
    paxc::PaxAuxRelationSetNewFilenode(aux_relid);
    // 2. the row describes the new, empty, storage, whose files start the
    // layout of row IDs afresh
    paxc::SetPaxTablesEntryStorage(pax_relid, newrlocator, true);
  } else {
    // create new table
    //
    // 1. create aux table
    // 2. initialize fast sequence in pg_pax_fastsequence
    // 3. setup dependency
    paxc::CPaxCreateMicroPartitionTable(rel);
  }

  // initialize or reset the fast sequence number
  CPaxInitializeFastSequenceEntry(
      pax_relid,
      exists ? FASTSEQUENCE_INIT_TYPE_UPDATE : FASTSEQUENCE_INIT_TYPE_CREATE,
      0);

  systable_endscan(scan);
  table_close(pax_tables_rel, NoLock);

  // create relfilenode file for pax table
  auto srel = paxc::PaxRelationCreateStorage(*newrlocator, rel);
  smgrclose(srel);

  // create data directory
  CBDB_TRY();
  {
    FileSystem *fs = pax::Singleton<LocalFileSystem>::GetInstance();
    auto path = cbdb::BuildPaxDirectoryPath(*newrlocator, rel->rd_backend);
    Assert(!path.empty());
    CBDB_CHECK(
        (fs->CreateDirectory(path) == 0),
        cbdb::CException::ExType::kExTypeIOError,
        fmt("Create directory failed [path=%s, errno=%d], "
            "relfilenode [spcNode=%u, dbNode=%u, relNode=%u, backend=%d]",
			path.c_str(), errno, newrlocator->spcOid, newrlocator->dbOid,
			newrlocator->relNumber, rel->rd_backend));
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

// * non transactional truncate table case:
// 1. create table inside transactional block, and then truncate table inside
// transactional block.
// 2.create table outside transactional block, insert data
// and truncate table inside transactional block.
void CCPaxAccessMethod::RelationNontransactionalTruncate(Relation rel) {
  CBDB_TRY();
  { pax::CCPaxAuxTable::PaxAuxRelationNontransactionalTruncate(rel); }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

void CCPaxAccessMethod::RelationCopyData(Relation rel,
                                         const RelFileLocator *newrnode) {
  CBDB_TRY();
  {
    cbdb::RelOpenSmgr(rel);
    cbdb::PaxRelationCreateStorage(*newrnode, rel);
    pax::CCPaxAuxTable::PaxAuxRelationCopyData(rel, newrnode, true);
    cbdb::RelDropStorage(rel);
    cbdb::RelCloseSmgr(rel);
    // the files, copied, keep their row IDs' layout
    CBDB_WRAP_FUNCTION(paxc::SetPaxTablesEntryStorage, RelationGetRelid(rel),
                       newrnode, false);
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

//  Copy data from `OldTable` into `NewTable`, as part of a CLUSTER or VACUUM
//  FULL.

//  PAX does not have dead tuples, but the core framework requires
//  to implement this callback to do CLUSTER/VACUUM FULL/etc.
//  PAX may have re-organize semantics for this function.

//  Additional Input parameters:
//  - use_sort - if true, the table contents are sorted appropriate for
//    `OldIndex`; if false and OldIndex is not InvalidOid, the data is copied
//    in that index's order; if false and OldIndex is InvalidOid, no sorting is
//    performed
//  - OldIndex - see use_sort
//  - OldestXmin - computed by vacuum_set_xid_limits(), even when
//    not needed for the relation's AM
//  - *xid_cutoff - ditto
//  - *multi_cutoff - ditto

//  Output parameters:
//  - *xid_cutoff - rel's new relfrozenxid value, may be invalid
//  - *multi_cutoff - rel's new relminmxid value, may be invalid
//  - *tups_vacuumed - stats, for logging, if appropriate for AM
//  - *tups_recently_dead - stats, for logging, if appropriate for AM

void CCPaxAccessMethod::RelationCopyForCluster(
    Relation old_rel, Relation new_rel, Relation old_index, bool use_sort,
    TransactionId /*oldest_xmin*/, Snapshot /*snapshot*/,
    TransactionId * /*xid_cutoff*/, MultiXactId * /*multi_cutoff*/,
    double * /*num_tuples*/, double * /*tups_vacuumed*/,
    double * /* tups_recently_dead*/) {
  Assert(RELATION_IS_PAX(old_rel));
  Assert(RELATION_IS_PAX(new_rel));
  CBDB_TRY();
  {
    //  if false and OldIndex is InvalidOid, no sorting is performed, just copy
    if (!use_sort && old_index == NULL) {
      pax::CCPaxAuxTable::PaxAuxRelationCopyDataForCluster(old_rel, new_rel);
      return;
    }

    // TODO(gongxun): should we support index's order to cluster table? ao/aocs
    // does not support.

    // like aocs, pax tables can only be clustered against a btree index
    CBDB_CHECK(old_index && (old_index->rd_rel->relam == BTREE_AM_OID),
               cbdb::CException::kExTypeInvalidIndexType,
               "PAX tables can only be clustered against a btree index");
    pax::IndexCluster(old_rel, new_rel, old_index, GetActiveSnapshot());
  }
  CBDB_CATCH_DEFAULT();
  CBDB_FINALLY({});
  CBDB_END_TRY();
}

} // namespace pax

namespace paxc {
uint64 PaxAccessMethod::RelationSize(Relation rel, ForkNumber fork_number) {
  Oid pax_aux_oid;
  Relation pax_aux_rel;
  TupleDesc aux_tup_desc;
  HeapTuple aux_tup;
  SysScanDesc aux_scan;
  uint64 pax_size = 0;

  if (fork_number != MAIN_FORKNUM && fork_number != InvalidForkNumber)
    return 0;

  // Get the oid of pg_pax_blocks_xxx from pg_pax_tables
  pax_aux_oid = ::paxc::GetPaxAuxRelid(rel->rd_id);

  // Scan pg_pax_blocks_xxx to calculate size of micro partition
  pax_aux_rel = table_open(pax_aux_oid, AccessShareLock);
  aux_tup_desc = RelationGetDescr(pax_aux_rel);

  aux_scan = systable_beginscan(pax_aux_rel, InvalidOid, false, NULL, 0, NULL);
  while (HeapTupleIsValid(aux_tup = systable_getnext(aux_scan))) {
    bool isnull = false;
    // TODO(chenhongjie): Exactly what is needed and being obtained is
    // compressed size. Later, when the aux table supports size attributes
    // before/after compression, we need to distinguish two attributes by names.
    Datum tup_datum = heap_getattr(
        aux_tup, ANUM_PG_PAX_BLOCK_TABLES_PTBLOCKSIZE, aux_tup_desc, &isnull);

    Assert(!isnull);
    pax_size += DatumGetUInt32(tup_datum);
  }

  systable_endscan(aux_scan);
  table_close(pax_aux_rel, AccessShareLock);

  return pax_size;
}

// Similar to the case of AO and AOCS tables, PAX table has auxiliary tables,
// size can be read directly from the auxiliary table, and there is not much
// space for optimization in estimating relsize. So this function is implemented
// in the same way as pax_relation_size().
void PaxAccessMethod::EstimateRelSize(Relation rel, int32 * /*attr_widths*/,
                                      BlockNumber *pages, double *tuples,
                                      double *allvisfrac) {
  Oid pax_aux_oid;
  Relation pax_aux_rel;
  TupleDesc aux_tup_desc;
  HeapTuple aux_tup;
  SysScanDesc aux_scan;
  uint64 total_tuples = 0;
  uint64 pax_size = 0;

  // Even an empty table takes at least one page,
  // but number of tuples for an empty table could be 0.
  *tuples = 0;
  *pages = 1;
  // index-only scan is not supported in PAX
  *allvisfrac = 0;

  // Get the oid of pg_pax_blocks_xxx from pg_pax_tables
  pax_aux_oid = ::paxc::GetPaxAuxRelid(rel->rd_id);

  // Scan pg_pax_blocks_xxx to get attributes
  pax_aux_rel = table_open(pax_aux_oid, AccessShareLock);
  aux_tup_desc = RelationGetDescr(pax_aux_rel);

  aux_scan = systable_beginscan(pax_aux_rel, InvalidOid, false, NULL, 0, NULL);
  while (HeapTupleIsValid(aux_tup = systable_getnext(aux_scan))) {
    Datum pttupcount_datum;
    Datum ptblocksize_datum;
    bool isnull = false;

    pttupcount_datum = heap_getattr(
        aux_tup, ANUM_PG_PAX_BLOCK_TABLES_PTTUPCOUNT, aux_tup_desc, &isnull);
    Assert(!isnull);
    total_tuples += DatumGetUInt32(pttupcount_datum);

    isnull = false;
    // TODO(chenhongjie): Exactly what we want to get here is uncompressed size,
    // but what we're getting is compressed size. Later, when the aux table
    // supports size attributes before/after compression, this needs to
    // be corrected.
    ptblocksize_datum = heap_getattr(
        aux_tup, ANUM_PG_PAX_BLOCK_TABLES_PTBLOCKSIZE, aux_tup_desc, &isnull);

    Assert(!isnull);
    pax_size += DatumGetUInt32(ptblocksize_datum);
  }

  systable_endscan(aux_scan);
  table_close(pax_aux_rel, AccessShareLock);

  *tuples = static_cast<double>(total_tuples);
  *pages = RelationGuessNumberOfBlocksFromSize(pax_size);
}

} // namespace paxc
#endif

