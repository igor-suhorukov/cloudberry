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
 * LogOps.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/log/LogOps.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - no init_log(): the extension's script makes gpsc.__log, on every node
 *     with the coordinator's OID, as every table of an extension is made
 *     there, where init_log()'s heap_create_with_catalog() gave each node's
 *     an OID of its own; a table of other columns than LogSchema.h's is not
 *     written;
 *   - a row is written with gpsc_frozen_insert(), the port's
 *     frozen_heap_insert(), which leaves the command where it was: no
 *     CommandCounterIncrement() after it;
 *   - heap_open() and heap_close() are table_open() and table_close().
 *-------------------------------------------------------------------------
 */

#include "protos/gpsc_set_service.pb.h"

#include "LogOps.h"
#include "LogSchema.h"

extern "C" {
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "gpsc_frozen.h"
}

void
insert_log(const gpsc::SetQueryReq &req, bool utility)
{
	Oid namespaceId;
	Oid relationId;
	Relation rel;

	/* Return if xact is not valid (needed for catalog lookups). */
	if (!IsTransactionState())
	{
		return;
	}

	/* Return if extension was not loaded */
	namespaceId = get_namespace_oid(schema_name.data(), true /* missing_ok */);
	if (!OidIsValid(namespaceId))
	{
		return;
	}

	/* Return if the table was not created yet */
	relationId = get_relname_relid(log_relname.data(), namespaceId);
	if (!OidIsValid(relationId))
	{
		return;
	}

	bool nulls[natts_gpsc_log];
	Datum values[natts_gpsc_log];

	memset(nulls, true, sizeof(nulls));
	memset(values, 0, sizeof(values));

	extract_query_req(req, "", values, nulls);
	nulls[attnum_gpsc_log_utility] = false;
	values[attnum_gpsc_log_utility] = BoolGetDatum(utility);

	rel = table_open(relationId, RowExclusiveLock);

	/* A table the script did not make as LogSchema.h describes the row */
	if (RelationGetDescr(rel)->natts != (int) natts_gpsc_log)
	{
		table_close(rel, RowExclusiveLock);
		return;
	}

	/* Insert the tuple as a frozen one to ensure it is logged even if txn rolls
   * back or aborts */
	gpsc_frozen_insert(rel, values, nulls);

	/* Keep lock on rel until end of xact */
	table_close(rel, NoLock);
}

void
truncate_log()
{
	Oid namespaceId;
	Oid relationId;
	Relation relation;

	namespaceId = get_namespace_oid(schema_name.data(), false /* missing_ok */);
	relationId = get_relname_relid(log_relname.data(), namespaceId);

	relation = table_open(relationId, AccessExclusiveLock);

	/* Truncate the main table */
	heap_truncate_one_rel(relation);

	/* Keep lock on rel until end of xact */
	table_close(relation, NoLock);

	/* Make changes visible */
	CommandCounterIncrement();
}