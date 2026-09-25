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
 * pg_pax_tables.cc
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/catalog/pg_pax_tables.cc
 *
 *
 * Ported to PostgreSQL 19, whose rewrites -- VACUUM FULL, CLUSTER, ALTER
 * TABLE's, REFRESH MATERIALIZED VIEW -- fill a new relation and swap the two
 * relations' storage without asking the table access method, where
 * Cloudberry's asked PAX to swap its catalog rows (its swap_relation_files
 * callback, CPaxAuxSwapRelationFiles()).  So a row of pg_pax_tables names
 * the storage its aux table describes -- the tablespace and relfilenode --
 * and the catalog follows the storage: swap_relation_files() fires the
 * object access hook's OAT_POST_ALTER for each relation once it has swapped
 * them, and PaxTablesFollowStorage() then finds the row describing the
 * storage the relation has now.  Where that is another table's, the two
 * trade their aux tables' rows, their storage and their fast sequences; where
 * the relation was not PAX's before -- ALTER TABLE ... SET ACCESS METHOD pax
 * -- the other table's aux table becomes its own.  The swap is a change of
 * this command's, which the catalog snapshot does not see yet: what it
 * reads, it reads with SnapshotSelf.  A table's rows go as it is dropped,
 * from the object access hook, where Cloudberry's went through its custom
 * object classes, which PostgreSQL 19's dependencies do not have.
 *-------------------------------------------------------------------------
 */

#include "catalog/pg_pax_tables.h"

#include "comm/cbdb_api.h"

#include "catalog/pax_aux_table.h"
#include "catalog/pax_fastsequence.h"
#include "comm/cbdb_wrappers.h"
#include "comm/guc.h"
#include "pax_tid.h"

namespace paxc {

// pg_class's reltablespace for the storage: InvalidOid for the database's
// default tablespace.
static Oid StorageTablespace(const RelFileLocator *storage) {
  return storage->spcOid == MyDatabaseTableSpace ? InvalidOid
                                                 : storage->spcOid;
}

void InsertPaxTablesEntry(Oid relid, Oid blocksrelid,
                          const RelFileLocator *storage) {
  Relation rel;
  TupleDesc desc;
  HeapTuple tuple;
  bool nulls[NATTS_PG_PAX_TABLES];
  Datum values[NATTS_PG_PAX_TABLES];

  rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);
  desc = RelationGetDescr(rel);
  Assert(desc->natts == NATTS_PG_PAX_TABLES);

  values[ANUM_PG_PAX_TABLES_RELID - 1] = ObjectIdGetDatum(relid);
  values[ANUM_PG_PAX_TABLES_AUXRELID - 1] = ObjectIdGetDatum(blocksrelid);
  values[ANUM_PG_PAX_TABLES_RELTABLESPACE - 1] =
      ObjectIdGetDatum(StorageTablespace(storage));
  values[ANUM_PG_PAX_TABLES_RELFILENODE - 1] =
      ObjectIdGetDatum(storage->relNumber);
  values[ANUM_PG_PAX_TABLES_FILEBITS - 1] =
      Int16GetDatum(PaxTidFileBitsFor(pax::pax_max_tuples_per_file));
  memset(nulls, false, sizeof(nulls));

  tuple = heap_form_tuple(desc, values, nulls);

  /* insert a new tuple */
  CatalogTupleInsert(rel, tuple);

  table_close(rel, NoLock);
}

void GetPaxTablesEntryAttributes(Oid relid, Oid *blocksrelid) {
  Relation rel;
  ScanKeyData key[1];
  SysScanDesc scan;
  HeapTuple tuple;
  bool isnull;

  rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);

  ScanKeyInit(&key[0], ANUM_PG_PAX_TABLES_RELID, BTEqualStrategyNumber, F_OIDEQ,
              ObjectIdGetDatum(relid));

  scan = systable_beginscan(rel, PAX_TABLES_RELID_INDEX_ID, true, NULL, 1, key);
  tuple = systable_getnext(scan);
  if (!HeapTupleIsValid(tuple))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("pax table relid \"%d\" does not exist in "
                           "pg_pax_tables",
                           relid)));

  if (blocksrelid) {
    *blocksrelid = heap_getattr(tuple, ANUM_PG_PAX_TABLES_AUXRELID,
                                RelationGetDescr(rel), &isnull);
    if (isnull) ereport(ERROR, (errmsg("pg_pax_tables.auxrelid is null")));
  }

  /* Finish up scan and close pg_pax_tables catalog. */
  systable_endscan(scan);
  table_close(rel, NoLock);
}

// A row of pg_pax_tables.
struct PaxTablesRow {
  bool found = false;
  ItemPointerData tid;
  Oid relid = InvalidOid;
  Oid auxrelid = InvalidOid;
  Oid spc = InvalidOid;
  Oid filenode = InvalidOid;
  int filebits = 0;
};

static PaxTablesRow FetchPaxTablesRow(Relation rel, Oid indexid, int nkeys,
                                      ScanKey keys, Oid exclude_relid) {
  PaxTablesRow row;
  SysScanDesc scan;
  HeapTuple tuple;
  TupleDesc desc = RelationGetDescr(rel);

  scan = systable_beginscan(rel, indexid, true, SnapshotSelf, nkeys, keys);
  while ((tuple = systable_getnext(scan)) != NULL) {
    Datum values[NATTS_PG_PAX_TABLES];
    bool nulls[NATTS_PG_PAX_TABLES];

    heap_deform_tuple(tuple, desc, values, nulls);
    if (DatumGetObjectId(values[ANUM_PG_PAX_TABLES_RELID - 1]) ==
        exclude_relid)
      continue;
    row.found = true;
    row.tid = tuple->t_self;
    row.relid = DatumGetObjectId(values[ANUM_PG_PAX_TABLES_RELID - 1]);
    row.auxrelid = DatumGetObjectId(values[ANUM_PG_PAX_TABLES_AUXRELID - 1]);
    row.spc = DatumGetObjectId(values[ANUM_PG_PAX_TABLES_RELTABLESPACE - 1]);
    row.filenode = DatumGetObjectId(values[ANUM_PG_PAX_TABLES_RELFILENODE - 1]);
    row.filebits = DatumGetInt16(values[ANUM_PG_PAX_TABLES_FILEBITS - 1]);
    break;
  }
  systable_endscan(scan);
  return row;
}

static PaxTablesRow FetchPaxTablesRowByRelid(Relation rel, Oid relid) {
  ScanKeyData key[1];

  ScanKeyInit(&key[0], ANUM_PG_PAX_TABLES_RELID, BTEqualStrategyNumber,
              F_OIDEQ, ObjectIdGetDatum(relid));
  return FetchPaxTablesRow(rel, PAX_TABLES_RELID_INDEX_ID, 1, key,
                           InvalidOid);
}

static PaxTablesRow FetchPaxTablesRowByStorage(Relation rel, Oid spc,
                                               Oid filenode,
                                               Oid exclude_relid) {
  ScanKeyData key[2];

  ScanKeyInit(&key[0], ANUM_PG_PAX_TABLES_RELTABLESPACE,
              BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(spc));
  ScanKeyInit(&key[1], ANUM_PG_PAX_TABLES_RELFILENODE, BTEqualStrategyNumber,
              F_OIDEQ, ObjectIdGetDatum(filenode));
  return FetchPaxTablesRow(rel, PaxTablesStorageIndexId(), 2, key,
                           exclude_relid);
}

// Replace the columns of the row at tid that repl says with values.
static void UpdatePaxTablesRow(Relation rel, ItemPointer tid, Datum *values,
                               bool *repl) {
  HeapTupleData oldtup;
  Buffer buffer;
  HeapTuple newtup;
  bool nulls[NATTS_PG_PAX_TABLES];

  memset(nulls, false, sizeof(nulls));
  oldtup.t_self = *tid;
  if (!heap_fetch(rel, SnapshotSelf, &oldtup, &buffer, false))
    elog(ERROR, "could not fetch pg_pax_tables row (%u,%u)",
         ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
  newtup = heap_modify_tuple(&oldtup, RelationGetDescr(rel), values, nulls,
                             repl);
  ReleaseBuffer(buffer);
  CatalogTupleUpdate(rel, &newtup->t_self, newtup);
  heap_freetuple(newtup);
}

// The storage the row describes, and the layout of its row IDs, unless
// filebits is 0.
static void SetRowStorage(Relation rel, ItemPointer tid, Oid spc,
                          Oid filenode, int filebits) {
  Datum values[NATTS_PG_PAX_TABLES];
  bool repl[NATTS_PG_PAX_TABLES];

  memset(repl, false, sizeof(repl));
  values[ANUM_PG_PAX_TABLES_RELTABLESPACE - 1] = ObjectIdGetDatum(spc);
  values[ANUM_PG_PAX_TABLES_RELFILENODE - 1] = ObjectIdGetDatum(filenode);
  values[ANUM_PG_PAX_TABLES_FILEBITS - 1] = Int16GetDatum(filebits);
  repl[ANUM_PG_PAX_TABLES_RELTABLESPACE - 1] = true;
  repl[ANUM_PG_PAX_TABLES_RELFILENODE - 1] = true;
  repl[ANUM_PG_PAX_TABLES_FILEBITS - 1] = (filebits != 0);
  UpdatePaxTablesRow(rel, tid, values, repl);
}

void SetPaxTablesEntryStorage(Oid relid, const RelFileLocator *storage,
                              bool new_files) {
  Relation rel;
  PaxTablesRow row;

  rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);
  row = FetchPaxTablesRowByRelid(rel, relid);
  if (!row.found)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("pax table relid \"%d\" does not exist in "
                           "pg_pax_tables",
                           relid)));
  SetRowStorage(rel, &row.tid, StorageTablespace(storage),
                storage->relNumber,
                new_files ? PaxTidFileBitsFor(pax::pax_max_tuples_per_file)
                          : 0);
  table_close(rel, NoLock);
}

int PaxTableFileBits(Relation rel) {
  struct PaxRelCache {
    int filebits;
  };
  PaxRelCache *cache = (PaxRelCache *)rel->rd_amcache;

  if (cache == NULL) {
    Relation pax_tables;
    PaxTablesRow row;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;

    // the catalog as the latest snapshot sees it, as a relcache entry is
    pax_tables = table_open(PAX_TABLES_RELATION_ID, AccessShareLock);
    ScanKeyInit(&key[0], ANUM_PG_PAX_TABLES_RELID, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(pax_tables, PAX_TABLES_RELID_INDEX_ID, true,
                              NULL, 1, key);
    tuple = systable_getnext(scan);
    if (!HeapTupleIsValid(tuple))
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                      errmsg("pax table relid \"%d\" does not exist in "
                             "pg_pax_tables",
                             RelationGetRelid(rel))));
    {
      bool isnull;
      Datum d = heap_getattr(tuple, ANUM_PG_PAX_TABLES_FILEBITS,
                             RelationGetDescr(pax_tables), &isnull);

      row.filebits = isnull ? 0 : DatumGetInt16(d);
    }
    systable_endscan(scan);
    table_close(pax_tables, AccessShareLock);
    if (row.filebits < PAX_TID_MIN_FILE_BITS ||
        row.filebits > PAX_TID_MAX_FILE_BITS)
      elog(ERROR, "pax table \"%s\" has an invalid row ID layout: %d",
           RelationGetRelationName(rel), row.filebits);

    cache = (PaxRelCache *)MemoryContextAlloc(CacheMemoryContext,
                                              sizeof(PaxRelCache));
    cache->filebits = row.filebits;
    rel->rd_amcache = cache;
  }
  return cache->filebits;
}

void PaxTableCheckFileNumber(Relation rel, uint32 file_number) {
  int file_bits = PaxTableFileBits(rel);

  if (file_number > PaxTidMaxFileNumber(file_bits))
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("PAX table \"%s\" has used all %u of its file numbers",
                    RelationGetRelationName(rel),
                    PaxTidMaxFileNumber(file_bits) + 1),
             errhint("TRUNCATE the table, or rewrite it with VACUUM FULL, to "
                     "start its file numbers again.")));
}

// The relation's pg_class row as this command has left it.
static bool CurrentClassRow(Oid relid, Oid *relam, Oid *spc, Oid *filenode) {
  Relation rel;
  ScanKeyData key[1];
  SysScanDesc scan;
  HeapTuple tuple;
  bool found = false;

  rel = table_open(RelationRelationId, AccessShareLock);
  ScanKeyInit(&key[0], Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
              ObjectIdGetDatum(relid));
  scan = systable_beginscan(rel, ClassOidIndexId, true, SnapshotSelf, 1, key);
  tuple = systable_getnext(scan);
  if (HeapTupleIsValid(tuple)) {
    Form_pg_class form = (Form_pg_class)GETSTRUCT(tuple);

    *relam = form->relam;
    *spc = form->reltablespace;
    *filenode = form->relfilenode;
    found = true;
  }
  systable_endscan(scan);
  table_close(rel, AccessShareLock);
  return found;
}

// Take an aux table's rows out of it: copies with their TOASTed values
// inline, as they are about to be deleted.
static List *TakeAuxRows(Relation aux) {
  List *rows = NIL;
  SysScanDesc scan;
  HeapTuple tuple;

  scan = systable_beginscan(aux, InvalidOid, false, SnapshotSelf, 0, NULL);
  while ((tuple = systable_getnext(scan)) != NULL) {
    rows = lappend(rows, toast_flatten_tuple(tuple, RelationGetDescr(aux)));
    CatalogTupleDelete(aux, &tuple->t_self);
  }
  systable_endscan(scan);
  return rows;
}

static void PutAuxRows(Relation aux, List *rows) {
  ListCell *lc;

  foreach (lc, rows) {
    HeapTuple tuple = (HeapTuple)lfirst(lc);

    CatalogTupleInsert(aux, tuple);
    heap_freetuple(tuple);
  }
  list_free(rows);
}

// Two tables' aux tables trade their rows.
static void SwapAuxRows(Oid aux1, Oid aux2) {
  Relation rel1 = table_open(aux1, AccessExclusiveLock);
  Relation rel2 = table_open(aux2, AccessExclusiveLock);
  List *rows1 = TakeAuxRows(rel1);
  List *rows2 = TakeAuxRows(rel2);

  PutAuxRows(rel1, rows2);
  PutAuxRows(rel2, rows1);
  table_close(rel2, NoLock);
  table_close(rel1, NoLock);
}

// The fast sequence row of objid, as this command has left it.
static HeapTuple FetchFastSequence(Relation rel, Oid objid) {
  ScanKeyData key[1];
  SysScanDesc scan;
  HeapTuple tuple;

  ScanKeyInit(&key[0], ANUM_PG_PAX_FAST_SEQUENCE_OBJID, BTEqualStrategyNumber,
              F_OIDEQ, ObjectIdGetDatum(objid));
  scan = systable_beginscan(rel, PAX_FASTSEQUENCE_INDEX_OID, true,
                            SnapshotSelf, 1, key);
  tuple = systable_getnext(scan);
  if (HeapTupleIsValid(tuple)) tuple = heap_copytuple(tuple);
  systable_endscan(scan);
  return tuple;
}

static void SetFastSequence(Relation rel, HeapTuple tuple, Oid objid,
                            Datum seq) {
  Datum values[NATTS_PG_PAX_FAST_SEQUENCE_TABLES];
  bool nulls[NATTS_PG_PAX_FAST_SEQUENCE_TABLES];
  bool repl[NATTS_PG_PAX_FAST_SEQUENCE_TABLES];
  HeapTuple newtup;

  memset(nulls, false, sizeof(nulls));
  memset(repl, true, sizeof(repl));
  values[ANUM_PG_PAX_FAST_SEQUENCE_OBJID - 1] = ObjectIdGetDatum(objid);
  values[ANUM_PG_PAX_FAST_SEQUENCE_LASTSEQUENCE - 1] = seq;
  newtup = heap_modify_tuple(tuple, RelationGetDescr(rel), values, nulls,
                             repl);
  CatalogTupleUpdate(rel, &newtup->t_self, newtup);
  heap_freetuple(newtup);
}

static Datum FastSequenceValue(Relation rel, HeapTuple tuple) {
  bool isnull;

  return heap_getattr(tuple, ANUM_PG_PAX_FAST_SEQUENCE_LASTSEQUENCE,
                      RelationGetDescr(rel), &isnull);
}

// The fast sequences of two tables trade values, or the one of from becomes
// to's where to has none.
static void MoveFastSequence(Oid from, Oid to, bool swap) {
  Relation rel = table_open(PAX_FASTSEQUENCE_OID, RowExclusiveLock);
  HeapTuple from_tuple = FetchFastSequence(rel, from);
  HeapTuple to_tuple = FetchFastSequence(rel, to);

  if (!HeapTupleIsValid(from_tuple))
    elog(ERROR, "no tuple found in pg_pax_fastsequence for pax table %u",
         from);
  if (swap) {
    if (!HeapTupleIsValid(to_tuple))
      elog(ERROR, "no tuple found in pg_pax_fastsequence for pax table %u",
           to);
    SetFastSequence(rel, from_tuple, from, FastSequenceValue(rel, to_tuple));
    SetFastSequence(rel, to_tuple, to, FastSequenceValue(rel, from_tuple));
  } else {
    if (HeapTupleIsValid(to_tuple)) CatalogTupleDelete(rel, &to_tuple->t_self);
    SetFastSequence(rel, from_tuple, to, FastSequenceValue(rel, from_tuple));
  }
  table_close(rel, NoLock);
}

// An aux table goes from one table to another: its dependency, and its name
// and its index's, which say the table's OID.
static void MoveAuxTable(Oid auxrelid, Oid from, Oid to) {
  char name[NAMEDATALEN];
  char index_name[NAMEDATALEN];
  Oid index_oid = FindAuxIndexOid(auxrelid, NULL);

  if (changeDependencyFor(RelationRelationId, auxrelid, RelationRelationId,
                          from, to) != 1)
    elog(ERROR, "could not move the dependency of aux table %u to %u",
         auxrelid, to);
  snprintf(name, sizeof(name), "pg_pax_blocks_%u", to);
  snprintf(index_name, sizeof(index_name), "%s_idx", name);
  RenameRelationInternal(auxrelid, name, true, false);
  RenameRelationInternal(index_oid, index_name, true, true);
}

void PaxTablesFollowStorage(Oid relid) {
  Relation rel;
  PaxTablesRow row;
  PaxTablesRow owner;
  Oid relam;
  Oid spc;
  Oid filenode;

  if (!CurrentClassRow(relid, &relam, &spc, &filenode) ||
      relam != PAX_TABLE_AM_OID)
    return;

  rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);
  row = FetchPaxTablesRowByRelid(rel, relid);
  if (row.found && row.spc == spc && row.filenode == filenode) {
    table_close(rel, RowExclusiveLock);
    return;
  }

  // Whose rows describe the storage the relation has now
  owner = FetchPaxTablesRowByStorage(rel, spc, filenode, relid);
  if (!owner.found) {
    table_close(rel, RowExclusiveLock);
    return;
  }

  if (row.found) {
    // Two PAX tables swapped their storage: their catalog does too.
    SwapAuxRows(row.auxrelid, owner.auxrelid);
    SetRowStorage(rel, &row.tid, owner.spc, owner.filenode, owner.filebits);
    SetRowStorage(rel, &owner.tid, row.spc, row.filenode, row.filebits);
    MoveFastSequence(owner.relid, relid, true);
    // where Cloudberry's swap has swapped the fast sequences
    SIMPLE_FAULT_INJECTOR("pax_finish_swap_fast_fastsequence");
  } else {
    // The relation became a PAX table with the other's storage, which is not
    // one any more: the other's catalog becomes the relation's.
    Datum values[NATTS_PG_PAX_TABLES];
    bool repl[NATTS_PG_PAX_TABLES];

    memset(repl, false, sizeof(repl));
    values[ANUM_PG_PAX_TABLES_RELID - 1] = ObjectIdGetDatum(relid);
    repl[ANUM_PG_PAX_TABLES_RELID - 1] = true;
    UpdatePaxTablesRow(rel, &owner.tid, values, repl);
    MoveAuxTable(owner.auxrelid, owner.relid, relid);
    MoveFastSequence(owner.relid, relid, false);
  }
  table_close(rel, RowExclusiveLock);
}

void DeletePaxTablesEntry(Oid relid) {
  Relation rel;
  PaxTablesRow row;
  HeapTuple tuple;

  rel = table_open(PAX_TABLES_RELATION_ID, RowExclusiveLock);
  row = FetchPaxTablesRowByRelid(rel, relid);
  if (row.found) CatalogTupleDelete(rel, &row.tid);
  table_close(rel, RowExclusiveLock);

  rel = table_open(PAX_FASTSEQUENCE_OID, RowExclusiveLock);
  tuple = FetchFastSequence(rel, relid);
  if (HeapTupleIsValid(tuple)) CatalogTupleDelete(rel, &tuple->t_self);
  table_close(rel, RowExclusiveLock);
}

}  // namespace paxc

namespace cbdb {
int PaxTableFileBits(Relation rel) {
  CBDB_WRAP_START;
  { return paxc::PaxTableFileBits(rel); }
  CBDB_WRAP_END;
  return 0;
}

void PaxTableCheckFileNumber(Relation rel, uint32 file_number) {
  CBDB_WRAP_FUNCTION(paxc::PaxTableCheckFileNumber, rel, file_number);
}
}  // namespace cbdb
