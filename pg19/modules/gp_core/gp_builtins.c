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
 * gp_builtins.c
 *	  PostgreSQL's built-in functions that Cloudberry runs on its segments
 *	  too: a BRIN index's summarization, and the operating system's
 *	  collations imported.
 *
 * Cloudberry's brin_summarize_new_values() and brin_summarize_range() add up
 * what each segment's call summarized, where the index's rows are: SQL
 * functions of its system_functions.sql over internal ones that run on all
 * segments.  Its pg_import_system_collations() makes each collation on the
 * coordinator and dispatches a CREATE COLLATION of it, whose
 * DefineCollation() on a segment checks that the locale loads there
 * (collationcmds.c, DispatchCollationCreate()).  PostgreSQL's three run
 * where they are called, on the coordinator alone, whose distributed tables
 * are empty and whose collations would be no segment's.
 *
 * So a call of one of them on a cluster's coordinator is made a call of
 * gp_internal's of the same name and arguments before the statement is
 * planned, as a size function's is (gp_size.c), and these do what
 * Cloudberry's do: PostgreSQL's function here, and then each segment's, its
 * count added; or the CREATE COLLATION of each collation it made, with the
 * OID it was given here (GpDdlDispatchDone()), in the coordinator's
 * transaction, which a segment that cannot load the locale fails whole.
 * Cloudberry's import dispatches the aliases and the ICU collations it
 * makes; this, every one it makes that a database of this encoding can use
 * -- one of another encoding is no collation of this database's, and its
 * CREATE COLLATION would fail on a segment -- the ones named as "locale -a"
 * names them too, which Cloudberry's leaves on the coordinator.
 *
 * Cloudberry sources this file stands in for:
 *	  brin_summarize_new_values() and brin_summarize_range() in
 *	  src/backend/catalog/system_functions.sql, and
 *	  pg_import_system_collations() and DispatchCollationCreate() in
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"

/* Is this a cluster's coordinator, which has segments to ask? */
static bool
coordinator_of_cluster(void)
{
	return !GpClusterIsSingleNode() &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH;
}

/* What the segments answered to the query, added up. */
static int32
segments_sum(char *sql)
{
	char	  **values;
	int32		sum = 0;

	if (!coordinator_of_cluster())
		return 0;
	values = palloc0_array(char *, GpClusterSegmentCount());
	GpDispatchQueryFirstValues(sql, -1, values);
	for (int i = 0; i < GpClusterSegmentCount(); i++)
		if (values[i] != NULL)
			sum += pg_strtoint32(values[i]);
	return sum;
}

PG_FUNCTION_INFO_V1(gp_brin_summarize_new_values);

/* gp_internal.brin_summarize_new_values(regclass) */
Datum
gp_brin_summarize_new_values(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	int32		n;

	n = DatumGetInt32(DirectFunctionCall1(brin_summarize_new_values,
										  ObjectIdGetDatum(indexoid)));
	n += segments_sum(psprintf("SELECT pg_catalog.brin_summarize_new_values(%u::pg_catalog.regclass)",
							   indexoid));
	PG_RETURN_INT32(n);
}

PG_FUNCTION_INFO_V1(gp_brin_summarize_range);

/* gp_internal.brin_summarize_range(regclass, bigint) */
Datum
gp_brin_summarize_range(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	int64		heapBlk = PG_GETARG_INT64(1);
	int32		n;

	n = DatumGetInt32(DirectFunctionCall2(brin_summarize_range,
										  ObjectIdGetDatum(indexoid),
										  Int64GetDatum(heapBlk)));
	n += segments_sum(psprintf("SELECT pg_catalog.brin_summarize_range(%u::pg_catalog.regclass, " INT64_FORMAT ")",
							   indexoid, heapBlk));
	PG_RETURN_INT32(n);
}

/* The collations of the namespace, as the transaction now sees them. */
static List *
namespace_collations(Oid nspid)
{
	Relation	rel = table_open(CollationRelationId, AccessShareLock);
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *oids = NIL;

	ScanKeyInit(&key, Anum_pg_collation_collnamespace, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(nspid));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		oids = lappend_oid(oids, ((Form_pg_collation) GETSTRUCT(tuple))->oid);
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);
	return oids;
}

/*
 * The CREATE COLLATION of a collation the import made, sent to the
 * segments, as Cloudberry's DispatchCollationCreate() writes it: its name,
 * its provider and its locale; nothing for one of another encoding, or of a
 * provider PostgreSQL's import makes none of.
 */
static void
dispatch_collation(Oid collid)
{
	HeapTuple	tuple = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	Form_pg_collation coll;
	DefineStmt *stmt;
	List	   *definition;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for collation %u", collid);
	coll = (Form_pg_collation) GETSTRUCT(tuple);

	if (coll->collencoding != -1 && coll->collencoding != GetDatabaseEncoding())
	{
		ReleaseSysCache(tuple);
		return;
	}
	if (coll->collprovider == COLLPROVIDER_ICU)
	{
		char	   *locale = TextDatumGetCString(SysCacheGetAttrNotNull(COLLOID, tuple,
																		  Anum_pg_collation_colllocale));

		definition = list_make2(makeDefElem("provider", (Node *) makeString("icu"), -1),
								makeDefElem("locale", (Node *) makeString(locale), -1));
	}
	else if (coll->collprovider == COLLPROVIDER_LIBC)
	{
		char	   *collate = TextDatumGetCString(SysCacheGetAttrNotNull(COLLOID, tuple,
																		   Anum_pg_collation_collcollate));
		char	   *ctype = TextDatumGetCString(SysCacheGetAttrNotNull(COLLOID, tuple,
																		 Anum_pg_collation_collctype));

		definition = list_make1(makeDefElem("provider", (Node *) makeString("libc"), -1));
		if (strcmp(collate, ctype) == 0)
			definition = lappend(definition,
								 makeDefElem("locale", (Node *) makeString(collate), -1));
		else
			definition = lappend(lappend(definition,
										 makeDefElem("lc_collate", (Node *) makeString(collate), -1)),
								 makeDefElem("lc_ctype", (Node *) makeString(ctype), -1));
	}
	else
	{
		ReleaseSysCache(tuple);
		return;
	}
	if (!coll->collisdeterministic)
		definition = lappend(definition,
							 makeDefElem("deterministic", (Node *) makeBoolean(false), -1));

	stmt = makeNode(DefineStmt);
	stmt->kind = OBJECT_COLLATION;
	stmt->oldstyle = false;
	stmt->defnames = list_make2(makeString(get_namespace_name(coll->collnamespace)),
								makeString(pstrdup(NameStr(coll->collname))));
	stmt->args = NIL;
	stmt->definition = definition;
	ReleaseSysCache(tuple);

	GpDdlDispatchDone((Node *) stmt, CollationRelationId, collid);
}

PG_FUNCTION_INFO_V1(gp_import_system_collations);

/* gp_internal.pg_import_system_collations(regnamespace) */
Datum
gp_import_system_collations(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	List	   *before = NIL;
	int32		created;

	if (coordinator_of_cluster())
		before = namespace_collations(nspid);
	created = DatumGetInt32(DirectFunctionCall1(pg_import_system_collations,
												ObjectIdGetDatum(nspid)));
	if (coordinator_of_cluster() && created > 0)
	{
		foreach_oid(collid, namespace_collations(nspid))
			if (!list_member_oid(before, collid))
				dispatch_collation(collid);
	}
	PG_RETURN_INT32(created);
}
