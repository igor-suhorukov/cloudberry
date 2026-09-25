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
 * pg_pax_tables.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/catalog/pg_pax_tables.h
 *
 *
 * Ported to PostgreSQL 19: a row names the storage its aux table describes,
 * the relation's tablespace and relfilenode then, and follows that storage
 * where PostgreSQL 19 moves it to another relation without asking the table
 * access method, as a rewrite's swap_relation_files() does
 * (PaxTablesFollowStorage(); pg_pax_tables.cc); and it keeps the layout of
 * the table's row IDs, the bits of a block number its file number takes
 * (pax_tid.h).
 *-------------------------------------------------------------------------
 */

#pragma once
#include "comm/cbdb_api.h"

#define NATTS_PG_PAX_TABLES 5
#define ANUM_PG_PAX_TABLES_RELID 1
#define ANUM_PG_PAX_TABLES_AUXRELID 2
#define ANUM_PG_PAX_TABLES_RELTABLESPACE 3
#define ANUM_PG_PAX_TABLES_RELFILENODE 4
#define ANUM_PG_PAX_TABLES_FILEBITS 5

namespace paxc {

// storage: the relation's storage the aux table describes
void InsertPaxTablesEntry(Oid relid, Oid blocksrelid,
                          const RelFileLocator *storage);

// The relation relid's storage is storage now: a TRUNCATE's, whose new
// files take the row IDs' layout the rows a file may hold now say, or where
// ALTER TABLE ... SET TABLESPACE copied the files, which keep theirs.
void SetPaxTablesEntryStorage(Oid relid, const RelFileLocator *storage,
                              bool new_files);

// After ALTER of the relation relid, which may have taken another relation's
// storage: its catalog rows, the aux table's and the fast sequence's, are
// made the ones of the storage it has now.
void PaxTablesFollowStorage(Oid relid);

// The relation relid is dropped: its rows go.  Its aux table goes with it,
// by its dependency.
void DeletePaxTablesEntry(Oid relid);

void GetPaxTablesEntryAttributes(Oid relid, Oid *blocksrelid);

static inline Oid GetPaxAuxRelid(Oid pax_relid) {
  Oid aux_relid;
  GetPaxTablesEntryAttributes(pax_relid, &aux_relid);
  return aux_relid;
}

}  // namespace paxc
