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
 * gp_utilmode.c
 *	  A session of a segment's own, as Cloudberry's utility mode names its
 *	  temporary schema.
 *
 * Cloudberry names a session's temporary schema by its session id,
 * pg_temp_<gp_session_id>, on the coordinator and on each segment it
 * dispatches to, and a utility-mode session's by its backend id, which could
 * be another session's id: so a utility-mode session's is pg_temp_0<backend
 * id>, and its TOAST schema pg_toast_temp_0<backend id>, a name no session id
 * gives (InitTempTableNamespace(), namespace.c).  A session of a segment's
 * own is the port's utility mode (gp_cluster.c), and PostgreSQL names every
 * session's temporary schema by its process number, pg_temp_<number>, a
 * dispatched one's too, which no other live backend of the node has.  So the
 * two kinds cannot clash on the port; but a utility session's schema is
 * named as Cloudberry names it, pg_temp_0<number>, so that what Cloudberry's
 * utility mode shows of it, and a tool that tells the two kinds apart by
 * name, finds it.  PostgreSQL makes the schema under its own name, or takes
 * the one of that name a dead session of the same number left, as the
 * session first needs it, with no hook; the name is given once the statement
 * that made it is done -- every statement that makes a temporary object is a
 * utility statement, or runs one, a function's included -- in the same
 * transaction, which takes the name back with the schema if it aborts.
 * Everything else knows the schema by its OID, and the number in its name
 * is read the same either way (GetTempNamespaceProcNumber(): atoi("017") is
 * 17).  A schema of that name a dead utility session of the same number left
 * is dropped with whatever it holds, as PostgreSQL would have emptied it to
 * reuse it.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/catalog/namespace.c, InitTempTableNamespace()'s names in
 *	  utility mode
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "miscadmin.h"
#include "storage/procnumber.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_utilmode.h"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*
 * Give the schema the name, dropping a schema of that name a dead session
 * of this process number left.  Only this backend makes or takes a schema
 * of its number's names while it lives.
 */
static void
utilmode_rename_schema(Oid nsp, const char *name)
{
	Relation	rel;
	HeapTuple	tuple;
	Oid			stale = get_namespace_oid(name, true);

	if (OidIsValid(stale))
	{
		ObjectAddress addr;

		ObjectAddressSet(addr, NamespaceRelationId, stale);
		performDeletion(&addr, DROP_CASCADE,
						PERFORM_DELETION_INTERNAL |
						PERFORM_DELETION_QUIETLY |
						PERFORM_DELETION_SKIP_EXTENSIONS);
		CommandCounterIncrement();
	}

	rel = table_open(NamespaceRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(NAMESPACEOID, ObjectIdGetDatum(nsp));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for namespace %u", nsp);
	namestrcpy(&((Form_pg_namespace) GETSTRUCT(tuple))->nspname, name);
	CatalogTupleUpdate(rel, &tuple->t_self, tuple);
	InvokeObjectPostAlterHook(NamespaceRelationId, nsp, 0);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * The session's temporary schemas, if it has them and they have
 * PostgreSQL's names still: Cloudberry's utility mode's names.
 */
static void
utilmode_name_temp_schemas(void)
{
	Oid			nsp;
	Oid			toast;
	char	   *name;
	char		postgres_name[NAMEDATALEN];
	char		cloudberry_name[NAMEDATALEN];

	GetTempNamespaceState(&nsp, &toast);
	if (!OidIsValid(nsp))
		return;
	snprintf(postgres_name, sizeof(postgres_name), "pg_temp_%d", MyProcNumber);
	name = get_namespace_name(nsp);
	if (name == NULL || strcmp(name, postgres_name) != 0)
		return;

	snprintf(cloudberry_name, sizeof(cloudberry_name), "pg_temp_0%d",
			 MyProcNumber);
	utilmode_rename_schema(nsp, cloudberry_name);
	if (OidIsValid(toast))
	{
		snprintf(cloudberry_name, sizeof(cloudberry_name), "pg_toast_temp_0%d",
				 MyProcNumber);
		utilmode_rename_schema(toast, cloudberry_name);
	}
	CommandCounterIncrement();
}

static void
utilmode_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						bool readOnlyTree, ProcessUtilityContext context,
						ParamListInfo params, QueryEnvironment *queryEnv,
						DestReceiver *dest, QueryCompletion *qc)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	/*
	 * A session of a segment's own, in a transaction that may still write:
	 * not one a statement such as COMMIT ended.
	 */
	if (GpClusterContentId() >= 0 &&
		GpClusterBackendRole() == GP_ROLE_UTILITY &&
		IsTransactionState())
		utilmode_name_temp_schemas();
}

void
GpUtilmodeInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = utilmode_ProcessUtility;
}
