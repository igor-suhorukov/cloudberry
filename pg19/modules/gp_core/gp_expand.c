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
 * gp_expand.c
 *	  gpexpand's catalog lock: no catalog changes while a segment is added.
 *
 * gpexpand makes a new segment from a copy of the coordinator's catalogs, and
 * then adds it to the cluster.  A catalog change made between the two would
 * be missing from the new segment, so gpexpand holds a lock of the whole
 * cluster's catalogs meanwhile -- gp_expand_lock_catalog(), in a transaction
 * of its own -- and every statement that changes a catalog on the
 * coordinator takes the same lock, shared, without waiting: one that finds
 * gpexpand holding it, or waiting for it, fails at once, rather than waiting
 * for an expansion that will leave it on a cluster it was not planned for.
 * Two sessions changing their catalogs do not wait for each other, and
 * statements that change no catalog, DML and reads, never take the lock.
 *
 * Cloudberry takes it in heap_insert(), heap_update() and heap_delete() of a
 * catalog of pg_catalog (gp_expand_protect_catalog_changes()), which a
 * module cannot reach: here it is taken in ProcessUtility_hook, by every
 * statement that PostgreSQL logs as DDL, and by TRUNCATE, REINDEX and VACUUM
 * FULL, which change pg_class as Cloudberry's check sees -- whether a client
 * sent it or a function ran it.  A catalog written by a C function that a
 * SELECT calls is not covered.  COMMENT, which writes pg_description, is
 * not, as Cloudberry's list of the coordinator's own catalogs leaves it out;
 * nor are PREPARE, whose EXECUTE is, and ALTER SYSTEM, which writes a file.
 *
 * The lock is an advisory lock of no database -- which no user's advisory
 * lock is -- of a kind of its own, as the lock of the cluster's nodes is
 * (gp_segadmin.c): taken with PostgreSQL's lock manager, it is a
 * transaction's, waited for and released as Cloudberry's is.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/misc/gpexpand.c, and its calls in
 *	  src/backend/access/heap/heapam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "storage/lock.h"
#include "tcop/utility.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_expand.h"

/*
 * The catalog lock: an advisory lock of no database, of a kind of its own
 * -- the lock of the cluster's nodes is kind 3 (gp_segadmin.c).
 */
#define EXPAND_LOCK_KEY1	0x67700000	/* "gp" */
#define EXPAND_LOCK_KEY2	0x65787061	/* "expa" */
#define EXPAND_LOCK_KIND	4

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

static void
expand_locktag(LOCKTAG *tag)
{
	SET_LOCKTAG_ADVISORY(*tag, InvalidOid, EXPAND_LOCK_KEY1, EXPAND_LOCK_KEY2,
						 EXPAND_LOCK_KIND);
}

/*
 * Does the statement change a catalog of the coordinator's that a new
 * segment's copy would miss?  What PostgreSQL logs as DDL, but COMMENT,
 * PREPARE and ALTER SYSTEM; and TRUNCATE, REINDEX and VACUUM FULL, which give
 * a relation a new file in pg_class.
 */
static bool
changes_catalog(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CommentStmt:
		case T_PrepareStmt:
		case T_AlterSystemStmt:
			return false;
		case T_TruncateStmt:
		case T_ReindexStmt:
			return true;
		case T_VacuumStmt:
			foreach_node(DefElem, opt, ((VacuumStmt *) parsetree)->options)
				if (strcmp(opt->defname, "full") == 0 && defGetBoolean(opt))
					return true;
			return false;
		default:
			return GetCommandLogLevel(parsetree) == LOGSTMT_DDL;
	}
}

/*
 * Cloudberry's gp_expand_protect_catalog_changes(): on the coordinator, the
 * catalog lock, shared, for the rest of the transaction, or an error where
 * gpexpand holds it or waits for it.
 */
static void
protect_catalog_changes(void)
{
	LOCKTAG		tag;

	expand_locktag(&tag);
	if (LockAcquire(&tag, AccessShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("gpexpand in progress, catalog changes are disallowed.")));
}

static void
expand_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	if (GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		changes_catalog(pstmt->utilityStmt))
		protect_catalog_changes();

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

void
GpExpandInit(void)
{
	/* A server of no cluster has no segment to add. */
	if (GpClusterIsSingleNode())
		return;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = expand_ProcessUtility;
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_expand_lock_catalog);

/*
 * gp_expand_lock_catalog()
 *		The catalog lock, for the rest of the transaction, once every
 *		transaction that has changed a catalog has ended.  gpexpand's.
 */
Datum
gp_expand_lock_catalog(PG_FUNCTION_ARGS)
{
	LOCKTAG		tag;

	expand_locktag(&tag);
	(void) LockAcquire(&tag, AccessExclusiveLock, false, false);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_expand_bump_version);

/*
 * gp_expand_bump_version()
 *		gpexpand's word that the cluster's segments have changed, which it
 *		says once it has changed them: a session that has not taken the
 *		change since may change no catalog (gp_cluster.c).
 */
Datum
gp_expand_bump_version(PG_FUNCTION_ARGS)
{
	GpClusterBumpExpandVersion();

	PG_RETURN_VOID();
}
