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
 * pax_rel.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/comm/pax_rel.h
 *
 * Ported to PostgreSQL 19: Cloudberry's initdb wrote PAX's objects with
 * fixed OIDs into every database; the port's are CREATE EXTENSION pax's,
 * in its schema "pax" where Cloudberry's were in pg_ext_aux, a name
 * PostgreSQL keeps for its own schemas.  So each OID is looked up by name
 * (modules/pax/pax_catalog.c), and a relation is PAX's when its table access
 * method is PAX's routine.
 *
 *-------------------------------------------------------------------------
 */

#ifndef SRC_CPP_COMM_PAX_REL_H_
#define SRC_CPP_COMM_PAX_REL_H_

#include "postgres_ext.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RelationData;
struct TableAmRoutine;

/*
 * Each the OID of an object of the extension in this database, or
 * InvalidOid while it is not there.
 */
extern Oid PaxNamespaceOid(void);
extern Oid PaxAuxNamespaceOid(void);
extern Oid PaxTablesRelationId(void);
extern Oid PaxTablesRelidIndexId(void);
extern Oid PaxTablesStorageIndexId(void);
extern Oid PaxTableAmOid(void);
extern Oid PaxAmHandlerOid(void);
extern Oid PaxAuxStatsTypeOid(void);
extern Oid PaxFastSequenceOid(void);
extern Oid PaxFastSequenceIndexOid(void);

/* the routine pax_tableam_handler returns (access/pax_access_handle.cc) */
extern const struct TableAmRoutine *PaxTableAmRoutine(void);

#ifdef __cplusplus
}
#endif

// Oid of pax.pg_pax_tables
#define PAX_TABLES_RELATION_ID PaxTablesRelationId()
#define PAX_TABLES_RELID_INDEX_ID PaxTablesRelidIndexId()

#define PAX_TABLE_AM_OID PaxTableAmOid()
#define PAX_AMNAME "pax"
#define PAX_AM_HANDLER_OID PaxAmHandlerOid()
#define PAX_AM_HANDLER_NAME "pax_tableam_handler"

#define PAX_AUX_STATS_TYPE_OID PaxAuxStatsTypeOid()
#define PAX_AUX_STATS_TYPE_NAME "paxauxstats"

#define PAX_FASTSEQUENCE_OID PaxFastSequenceOid()
#define PAX_FASTSEQUENCE_INDEX_OID PaxFastSequenceIndexOid()

#define PG_PAX_FASTSEQUENCE_NAMESPACE "pax"
#define PG_PAX_FASTSEQUENCE_TABLE "pg_pax_fastsequence"
#define PG_PAX_FASTSEQUENCE_INDEX_NAME "pg_pax_fastsequence_objid_idx"

/* Cloudberry's pg_ext_aux, where each table's pg_pax_blocks_<relid> is */
#define PG_EXTAUX_NAMESPACE PaxAuxNamespaceOid()

/* Cloudberry's core named the method's OID for ORCA and the planner */
#define PAX_AM_OID PaxTableAmOid()

#define AMHandlerIsPAX(amhandler) ((amhandler) == PAX_AM_HANDLER_OID)
#define RelationIsPAX(relation) \
  ((relation)->rd_tableam == PaxTableAmRoutine())

#endif  // SRC_CPP_COMM_PAX_REL_H_
