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
 *	  A segment added to the running cluster, as gpexpand adds one, and
 *	  removed, as gpshrink removes it: gpexpand's catalog lock, the tables
 *	  given the number of segments they are on, and each session taking the
 *	  new number.
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
 * Once gpexpand has added its segments (gp_segadmin.c, gp_cluster.c), a
 * session takes the new number of segments as a transaction begins, as
 * Cloudberry's takes gp_segment_configuration's rows as each transaction
 * starts (cdbcomponent_updateCdbComponents()) -- here as the transaction
 * first asks for the number, which a module can tell where it cannot tell a
 * transaction's start: parse analysis reading a table's policy, the planner,
 * the dispatcher before it uses its gang, or a catalog change
 * (GpClusterDecideSegments()).  Its gang, made to the old segments, is let
 * go of first, and everything this backend has cached of the relations and
 * of the plans made for the old number is forgotten.  A session with a
 * temporary table keeps the old number, and its gang, whose backends hold
 * the table's segments' parts: Cloudberry's rule, which then refuses the
 * session's catalog changes, as above; it takes the new number once it has
 * none left -- the port's own SET DISTRIBUTED and EXPAND TABLE make one for
 * the rows they move, so "a temporary namespace", Cloudberry's test, would
 * keep every session that ran one on the old number for good.  A plan cached
 * for the old number and run without being planned again, which asks for no
 * number, runs on the old gang, which it was made for; the change
 * invalidated every relation, so the next transaction that uses a table
 * plans it again.
 *
 * Cloudberry stores each table's number of segments with its policy, so a
 * segment added leaves every table where it was, partial until gpexpand
 * expands it.  The port's label names the number only where it is not every
 * segment (gp_policy.c), which would spread a table over a segment it has no
 * row on: so gpexpand has every table's label name it first, in each
 * database, under the catalog lock (gp.expand_pin_numsegments()).
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/misc/gpexpand.c, its calls in
 *	  src/backend/access/heap/heapam.c, and cdbcomponent_updateCdbComponents()
 *	  in src/backend/cdb/cdbutil.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_seclabel.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "storage/lock.h"
#include "tcop/utility.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_expand.h"
#include "gp_label.h"

/*
 * The catalog lock: an advisory lock of no database, of a kind of its own
 * -- the lock of the cluster's nodes is kind 3 (gp_segadmin.c).
 */
#define EXPAND_LOCK_KEY1	0x67700000	/* "gp" */
#define EXPAND_LOCK_KEY2	0x65787061	/* "expa" */
#define EXPAND_LOCK_KIND	4

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ------------------------------------------------------------------------- */
/* The number of segments, taken as a transaction begins                     */
/* ------------------------------------------------------------------------- */

/*
 * Has this session a temporary relation now?  Its temporary namespace holds
 * one: the namespace itself stays once made.
 */
static bool
has_temp_relation(void)
{
	Oid			temp_ns;
	Oid			temp_toast_ns;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key;
	bool		found;

	GetTempNamespaceState(&temp_ns, &temp_toast_ns);
	if (!OidIsValid(temp_ns))
		return false;

	ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(temp_ns));
	rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
	found = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/*
 * The decider of gp_cluster.c, the first time in a transaction that the
 * segments changed since this session last took them: taken, where the
 * session has no temporary relation -- its gang let go of first, and
 * everything it has cached of the relations and plans forgotten, ORCA's
 * metadata among them, which ORCA drops as its next optimization begins.
 */
static void
adopt_segments(void)
{
	if (has_temp_relation())
		return;
	GpDispatchResetGang();
	if (GpClusterAdoptSegments())
		InvalidateSystemCaches();
}

/* ------------------------------------------------------------------------- */
/* The catalog lock                                                          */
/* ------------------------------------------------------------------------- */

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
 * gpexpand holds it or waits for it.  And the end of a session that has not
 * taken the segments gpexpand changed -- one that kept a temporary table
 * over the change, or whose transaction began before it: what it changes in
 * its catalogs would miss the segments it does not know.
 */
static void
protect_catalog_changes(void)
{
	LOCKTAG		tag;
	uint64		old_version;
	uint64		new_version;

	expand_locktag(&tag);
	if (LockAcquire(&tag, AccessShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("gpexpand in progress, catalog changes are disallowed.")));

	old_version = GpClusterAdoptedExpandVersion();
	new_version = GpClusterExpandVersion();
	if (old_version != new_version)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cluster is expanded from version %llu to %llu, catalog changes are disallowed",
						(unsigned long long) old_version,
						(unsigned long long) new_version)));
}

static void
expand_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	GpClusterDecideSegments();
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
	GpClusterSetDecider(adopt_segments);
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

PG_FUNCTION_INFO_V1(gp_expand_pin_numsegments);

/*
 * gp.expand_pin_numsegments()
 *		Every distributed table of this database whose policy names no number
 *		of segments -- every segment, as a label says it -- given this
 *		session's number, and how many there were.
 *
 * gpexpand calls it before it copies the coordinator for the new segment,
 * whose tables then say the same.  The labels reach the segments as the
 * transaction commits, as every label does.  A table whose label names the
 * number already, or that has no distribution key -- the coordinator's own
 * -- is left as it is.
 */
Datum
gp_expand_pin_numsegments(PG_FUNCTION_ARGS)
{
	char	   *numsegments = psprintf("%d", GpClusterSegmentCount());
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	List	   *relids = NIL;
	int			pinned = 0;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp.expand_pin_numsegments() must be run on the coordinator")));

	ScanKeyInit(&key[0], Anum_pg_seclabel_classoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_seclabel_objsubid, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(0));
	rel = table_open(SecLabelRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		relids = lappend_oid(relids,
							 ((FormData_pg_seclabel *) GETSTRUCT(tuple))->objoid);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	foreach_oid(relid, relids)
	{
		ObjectAddress addr;

		ObjectAddressSet(addr, RelationRelationId, relid);
		if (GpLabelGet(&addr, GP_LABEL_distributed_by) == NULL ||
			GpLabelGet(&addr, GP_LABEL_numsegments) != NULL)
			continue;
		GpLabelSet(&addr, GP_LABEL_numsegments, numsegments);
		pinned++;
	}

	PG_RETURN_INT32(pinned);
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
