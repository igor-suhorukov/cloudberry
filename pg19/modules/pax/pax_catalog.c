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
 * pax_catalog.c
 *	  The OIDs of CREATE EXTENSION pax's objects, by name.
 *
 * Cloudberry's initdb wrote PAX's objects into every database with fixed
 * OIDs, which PAX's code names (comm/pax_rel.h).  The port's are the
 * extension's, in its schema "pax", and each table's aux table is in
 * pg_ext_aux, which the extension's script makes, as Cloudberry's initdb
 * did -- a schema pg_dump passes over, as it should each table's aux
 * table: the restored table makes its own.  Each is looked up by name the
 * first time a backend asks and remembered until something in pg_namespace
 * changes -- DROP EXTENSION pax drops both schemas -- and not remembered
 * while one of them is missing, as it is in a database without the
 * extension, and part of the way through CREATE EXTENSION.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "pax_module.h"

typedef struct PaxCatalogOids
{
	bool		valid;
	Oid			nsp;
	Oid			aux_nsp;
	Oid			tables;
	Oid			tables_relid_index;
	Oid			tables_storage_index;
	Oid			am;
	Oid			handler;
	Oid			stats_type;
	Oid			fastseq;
	Oid			fastseq_index;
} PaxCatalogOids;

static PaxCatalogOids pax_oids;

static void
pax_catalog_reset(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	pax_oids.valid = false;
}

static const PaxCatalogOids *
pax_catalog_oids(void)
{
	PaxCatalogOids o;
	HeapTuple	tup;

	if (pax_oids.valid)
		return &pax_oids;

	memset(&o, 0, sizeof(o));
	o.nsp = get_namespace_oid(PAX_NAMESPACE_NAME, true);
	o.aux_nsp = get_namespace_oid(PAX_AUX_NAMESPACE_NAME, true);
	if (OidIsValid(o.nsp))
	{
		o.tables = get_relname_relid("pg_pax_tables", o.nsp);
		o.tables_relid_index = get_relname_relid("pg_pax_tables_relid_index", o.nsp);
		o.tables_storage_index = get_relname_relid("pg_pax_tables_storage_index", o.nsp);
		o.fastseq = get_relname_relid("pg_pax_fastsequence", o.nsp);
		o.fastseq_index = get_relname_relid("pg_pax_fastsequence_objid_idx", o.nsp);
		o.stats_type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
									   CStringGetDatum("paxauxstats"),
									   ObjectIdGetDatum(o.nsp));
		o.am = get_table_am_oid("pax", true);
		if (OidIsValid(o.am))
		{
			tup = SearchSysCache1(AMOID, ObjectIdGetDatum(o.am));
			if (HeapTupleIsValid(tup))
			{
				o.handler = ((Form_pg_am) GETSTRUCT(tup))->amhandler;
				ReleaseSysCache(tup);
			}
		}
	}
	o.valid = OidIsValid(o.nsp) && OidIsValid(o.aux_nsp) && OidIsValid(o.tables) &&
		OidIsValid(o.tables_relid_index) && OidIsValid(o.tables_storage_index) &&
		OidIsValid(o.fastseq) && OidIsValid(o.fastseq_index) &&
		OidIsValid(o.stats_type) && OidIsValid(o.am) && OidIsValid(o.handler);
	pax_oids = o;
	return &pax_oids;
}

void
PaxCatalogInit(void)
{
	CacheRegisterSyscacheCallback(NAMESPACEOID, pax_catalog_reset, (Datum) 0);
}

Oid
PaxNamespaceOid(void)
{
	return pax_catalog_oids()->nsp;
}

Oid
PaxAuxNamespaceOid(void)
{
	return pax_catalog_oids()->aux_nsp;
}

Oid
PaxTablesRelationId(void)
{
	return pax_catalog_oids()->tables;
}

Oid
PaxTablesRelidIndexId(void)
{
	return pax_catalog_oids()->tables_relid_index;
}

Oid
PaxTablesStorageIndexId(void)
{
	return pax_catalog_oids()->tables_storage_index;
}

Oid
PaxTableAmOid(void)
{
	return pax_catalog_oids()->am;
}

Oid
PaxAmHandlerOid(void)
{
	return pax_catalog_oids()->handler;
}

Oid
PaxAuxStatsTypeOid(void)
{
	return pax_catalog_oids()->stats_type;
}

Oid
PaxFastSequenceOid(void)
{
	return pax_catalog_oids()->fastseq;
}

Oid
PaxFastSequenceIndexOid(void)
{
	return pax_catalog_oids()->fastseq_index;
}

/* Is the extension there, all of it, in this database? */
bool
PaxCatalogReady(void)
{
	return pax_catalog_oids()->valid;
}
