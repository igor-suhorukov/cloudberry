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
 * gp_metatrack.c
 *	  pg_stat_last_operation and pg_stat_last_shoperation: when an object
 *	  was last created, altered, vacuumed, analyzed or truncated, and by
 *	  whom -- Cloudberry's metadata tracking (MetaTrack*(), catalog/heap.c).
 *
 * Cloudberry keeps a row for each object and each kind of operation done on
 * it -- CREATE, ALTER, PRIVILEGE, VACUUM, ANALYZE, TRUNCATE, PARTITION --
 * with the role that did it, a subtype that says more (TABLE, OWNED BY,
 * ATTACH), and the time; the next operation of the same kind replaces the
 * row, and dropping the object removes its rows.  analyzedb reads it to find
 * the tables changed since it last ran.  A shared object's -- a database, a
 * role, a tablespace -- go to pg_stat_last_shoperation, a shared catalog.
 * Its coordinator writes them, from some eighty places in its commands, and
 * its segments none.
 *
 * Here the two are views of gp_core's tables gp_internal.stat_last_operation
 * and stat_last_shoperation, in each database, which only a cluster's
 * coordinator writes: a new object, from the object access hook as it is
 * made, and dropped, as it goes; everything else from the utility hook, once
 * the statement has done it, by what the statement says -- Cloudberry's
 * places, each by the statement that reaches it.  A server of one node
 * writes them too, as Cloudberry's single node does.  An extension's script
 * writes nothing, as Cloudberry's initdb writes nothing for its own
 * objects.  A shared object's rows are in the database the statement ran
 * in, where Cloudberry's shared catalog shows them in every database.
 *
 * VACUUM, and ANALYZE of more than one relation, commit a transaction of
 * each relation, and Cloudberry writes each one's rows in its own
 * transaction, under the lock VACUUM or ANALYZE holds on it (vacuum_rel(),
 * analyze_rel_internal()): a DROP of the relation, which removes its rows,
 * waits for them, and so does the relation's next VACUUM or ANALYZE.  Once
 * the statement is done those locks have gone, and a row written then can
 * meet a DROP's removal of it, one of the two failing -- "tuple
 * concurrently deleted" or "updated", as isolation2's lockmodes met it, an
 * ANALYZE beside two DROPs.  So what ANALYZE analyzes has its rows written
 * as Cloudberry's are, from O3's hook in the relation's transaction; what
 * VACUUM alone vacuumed after the statement, where no hook reaches, under
 * the same lock taken again.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/catalog/heap.c (MetaTrackAddObject() and the rest), and
 *	  their calls in src/backend/commands/ and src/backend/catalog/
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/tablespace.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_metatrack.h"

static object_access_hook_type prev_object_access_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static analyze_sample_rows_hook_type prev_analyze_sample_rows = NULL;

/* The last time given, so that the rows of one statement keep their order. */
static TimestampTz last_statime = 0;

/*
 * One of the tables has been dropped in this transaction -- DROP EXTENSION
 * gp_core drops them with everything else -- and nothing is written to them
 * until it ends.
 */
static bool tables_dropped = false;

/* CREATE or ALTER EXTENSION is running: what its script makes is its own. */
static int	in_extension = 0;

/*
 * The index a DROP INDEX CONCURRENTLY is dropping, whose rows go as the
 * statement ends, as Cloudberry's index_drop() forgets the index at its end.
 * The drop hook runs in the statement's first transaction, where
 * index_drop() then asks that nothing has been written yet ("must be first
 * action in transaction"); the statement commits transactions of its own
 * as it goes, and drops one index alone.
 */
static Oid	forget_concurrent = InvalidOid;

/*
 * The VACUUM or ANALYZE statement this backend runs: the rows it writes --
 * VACUUM's, of a subtype, and ANALYZE's -- and the relations whose rows it
 * wrote as it analyzed them.  In a context of its own, which the
 * transactions the statement commits leave.
 */
typedef struct VacuumRows
{
	const char *vsubtype;		/* VACUUM's rows' subtype, NULL for none */
	bool		analyze;		/* ANALYZE's rows */
	HTAB	   *written;		/* of Oid */
} VacuumRows;

static VacuumRows *vacuum_rows = NULL;

/* ------------------------------------------------------------------------- */
/* The rows                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Does this backend track?  A cluster's coordinator does, as Cloudberry's QD
 * does (Gp_role == GP_ROLE_DISPATCH), and a server of one node, as
 * Cloudberry's single node does; not while an extension's script runs --
 * gp_core's own makes the tables -- nor in a binary upgrade.
 */
static bool
tracking(void)
{
	return (GpClusterBackendRole() == GP_ROLE_DISPATCH || GpClusterIsSingleNode()) &&
		!creating_extension && in_extension == 0 && !tables_dropped &&
		!IsBinaryUpgrade &&
		!IsBootstrapProcessingMode() && IsTransactionState();
}

/* The table a class's rows go to, in this database; InvalidOid for none. */
static Oid
table_of(Oid classid)
{
	Oid			nsp = get_namespace_oid("gp_internal", true);

	if (!OidIsValid(nsp))
		return InvalidOid;
	return get_relname_relid(IsSharedRelation(classid) ?
							 "stat_last_shoperation" : "stat_last_operation",
							 nsp);
}

/* The time of a row: now, later than the last row's. */
static TimestampTz
statime(void)
{
	TimestampTz now = GetCurrentTimestamp();

	if (now <= last_statime)
		now = last_statime + 1;
	last_statime = now;
	return now;
}

/*
 * The row of this object and kind of operation: made, or its subtype, role
 * and time replaced -- MetaTrackUpdObject(); "add" makes it without looking,
 * as MetaTrackAddObject() does for a new object, which has none.
 */
static void
record(Oid classid, Oid objid, const char *action, const char *subtype,
	   bool add)
{
	Oid			relid = table_of(classid);
	Relation	rel;
	Datum		values[7];
	bool		nulls[7] = {0};
	bool		replace[7] = {0};
	NameData	aname;
	NameData	uname;
	char	   *user;
	HeapTuple	old = NULL;
	SysScanDesc scan = NULL;

	if (!OidIsValid(relid) || !OidIsValid(objid))
		return;

	rel = table_open(relid, RowExclusiveLock);

	namestrcpy(&aname, action);
	user = GetUserNameFromId(GetUserId(), true);
	if (user != NULL)
		namestrcpy(&uname, user);
	else
		snprintf(NameStr(uname), NAMEDATALEN, "%u", GetUserId());

	values[0] = ObjectIdGetDatum(classid);
	values[1] = ObjectIdGetDatum(objid);
	values[2] = NameGetDatum(&aname);
	values[3] = ObjectIdGetDatum(GetUserId());
	values[4] = NameGetDatum(&uname);
	values[5] = CStringGetTextDatum(subtype);
	values[6] = TimestampTzGetDatum(statime());
	replace[3] = replace[4] = replace[5] = replace[6] = true;

	if (!add)
	{
		List	   *indexes = RelationGetIndexList(rel);
		ScanKeyData key[3];

		ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(classid));
		ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objid));
		ScanKeyInit(&key[2], 3, BTEqualStrategyNumber, F_NAMEEQ,
					NameGetDatum(&aname));
		scan = systable_beginscan(rel,
								  indexes != NIL ? linitial_oid(indexes) : InvalidOid,
								  indexes != NIL, NULL, 3, key);
		old = systable_getnext(scan);
	}

	if (HeapTupleIsValid(old))
	{
		HeapTuple	tuple = heap_modify_tuple(old, RelationGetDescr(rel),
											  values, nulls, replace);

		CatalogTupleUpdate(rel, &old->t_self, tuple);
	}
	else
		CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel),
												values, nulls));
	if (scan != NULL)
		systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	/*
	 * The next row of the statement sees this one; a new object's row, which
	 * nothing else of its statement replaces, is written in the middle of
	 * making it, where the command is not ours to end.
	 */
	if (!add)
		CommandCounterIncrement();
}

/* An object dropped: its rows go -- MetaTrackDropObject(). */
static void
forget(Oid classid, Oid objid)
{
	Oid			relid = table_of(classid);
	Relation	rel;
	List	   *indexes;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;

	if (!OidIsValid(relid))
		return;
	rel = table_open(relid, RowExclusiveLock);
	indexes = RelationGetIndexList(rel);
	ScanKeyInit(&key[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classid));
	ScanKeyInit(&key[1], 2, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objid));
	scan = systable_beginscan(rel,
							  indexes != NIL ? linitial_oid(indexes) : InvalidOid,
							  indexes != NIL, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tuple->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

/*
 * Is a relation one whose operations are tracked: a table, an index, a
 * sequence or a view, and a partitioned table where "partitioned" says so,
 * outside a temporary schema, pg_toast, and pg_catalog while its tables are
 * being changed -- MetaTrackValidKindNsp().
 */
static bool
tracked_relation(Oid relid, bool partitioned)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Form_pg_class form;
	bool		result;

	if (!HeapTupleIsValid(tp))
		return false;
	form = (Form_pg_class) GETSTRUCT(tp);
	result = (form->relkind == RELKIND_RELATION ||
			  form->relkind == RELKIND_INDEX ||
			  form->relkind == RELKIND_SEQUENCE ||
			  form->relkind == RELKIND_VIEW ||
			  (partitioned && form->relkind == RELKIND_PARTITIONED_TABLE)) &&
		form->relnamespace != PG_TOAST_NAMESPACE &&
		!(form->relnamespace == PG_CATALOG_NAMESPACE && allowSystemTableMods) &&
		!isAnyTempNamespace(form->relnamespace);
	ReleaseSysCache(tp);
	return result;
}

static void
record_relation(Oid relid, const char *action, const char *subtype)
{
	if (tracked_relation(relid, true))
		record(RelationRelationId, relid, action, subtype, false);
}

/*
 * A relation VACUUM, ANALYZE or TRUNCATE did something to: a table, which
 * each takes, and not a view it skips -- Cloudberry's rows are written by
 * the relation it processed (vacuum_rel(), do_analyze_rel()).
 */
static void
record_processed(Oid relid, const char *action, const char *subtype)
{
	char		relkind = get_rel_relkind(relid);

	if (relkind == RELKIND_RELATION || relkind == RELKIND_PARTITIONED_TABLE)
		record_relation(relid, action, subtype);
}

/*
 * May VACUUM's row of a relation be written after the statement, whose
 * transaction of the relation has committed and taken its lock with it?
 * The lock VACUUM holds as Cloudberry writes the row,
 * ShareUpdateExclusiveLock, taken again and held till this transaction
 * ends: a DROP of the relation then waits for the row, or has removed the
 * relation already, which record_processed() finds; and so does the
 * relation's next VACUUM or ANALYZE, whose rows are written under the same
 * lock.  Taken without waiting -- the statement holds the lock of each
 * relation whose row it wrote before, which a DROP of both could be
 * waiting for -- so where another session holds or waits for a lock that
 * conflicts, the row is not written: a DROP's, which would remove it, a
 * VACUUM's or ANALYZE's of the same relation, which writes its own, or an
 * autovacuum worker's, which writes none.
 */
static bool
lock_for_row(Oid relid)
{
	return CheckRelationOidLockedByMe(relid, ShareUpdateExclusiveLock, true) ||
		ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock);
}

/* ------------------------------------------------------------------------- */
/* What is made, and what goes                                               */
/* ------------------------------------------------------------------------- */

/*
 * A new relation's kind as its CREATE row says it (heap_create_with_catalog()
 * and index_create()); NULL for one not tracked.  Its pg_class row is not
 * one the command sees yet, and its relation cache entry, which it was made
 * with, says what the row does.
 */
static const char *
created_relation(Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);
	Form_pg_class form;
	const char *subtype = NULL;

	if (!RelationIsValid(rel))
		return NULL;
	form = rel->rd_rel;
	switch (form->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			subtype = "TABLE";
			break;
		case RELKIND_INDEX:
			subtype = "INDEX";
			break;
		case RELKIND_SEQUENCE:
			subtype = "SEQUENCE";
			break;
		case RELKIND_VIEW:
			subtype = "VIEW";
			break;
		case RELKIND_MATVIEW:
			subtype = "MATVIEW";
			break;
		default:
			break;
	}
	if (form->relnamespace == PG_TOAST_NAMESPACE ||
		(form->relnamespace == PG_CATALOG_NAMESPACE && allowSystemTableMods) ||
		isAnyTempNamespace(form->relnamespace))
		subtype = NULL;
	RelationClose(rel);
	return subtype;
}

/* A new schema's name, from its row, which only the command itself sees. */
static char *
created_namespace(Oid nspoid)
{
	Relation	rel = table_open(NamespaceRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	char	   *name = NULL;

	ScanKeyInit(&key, Anum_pg_namespace_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(nspoid));
	scan = systable_beginscan(rel, NamespaceOidIndexId, true, SnapshotSelf, 1, &key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		name = pstrdup(NameStr(((Form_pg_namespace) GETSTRUCT(tuple))->nspname));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return name;
}

static void
metatrack_object_access(ObjectAccessType access, Oid classId, Oid objectId,
						int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (subId != 0 || !tracking())
		return;

	if (access == OAT_POST_CREATE)
	{
		ObjectAccessPostCreate *pc = (ObjectAccessPostCreate *) arg;
		const char *subtype = NULL;

		if (pc != NULL && pc->is_internal)
			return;
		switch (classId)
		{
			case RelationRelationId:
				subtype = created_relation(objectId);
				break;
			case NamespaceRelationId:
				{
					char	   *name = created_namespace(objectId);

					if (name != NULL && strncmp(name, "pg_temp", 7) != 0 &&
						strncmp(name, "pg_toast", 8) != 0)
						subtype = "SCHEMA";
				}
				break;
			case PolicyRelationId:
				subtype = "POLICY";
				break;
			case PublicationRelationId:
				subtype = "PUBLICATION";
				break;
			case SubscriptionRelationId:
				subtype = "SUBSCRIPTION";
				break;
			case TransformRelationId:
				subtype = "TRANSFORM";
				break;
			case DatabaseRelationId:
				subtype = "DATABASE";
				break;
			case AuthIdRelationId:
				subtype = "ROLE";
				break;
			case TableSpaceRelationId:
				subtype = "TABLESPACE";
				break;
			default:
				break;
		}
		if (subtype != NULL)
			record(classId, objectId, "CREATE", subtype, true);
	}
	else if (access == OAT_DROP)
	{
		ObjectAccessDrop *drop = (ObjectAccessDrop *) arg;

		if (classId == RelationRelationId &&
			(objectId == table_of(RelationRelationId) ||
			 objectId == table_of(DatabaseRelationId)))
			tables_dropped = true;
		else if (drop != NULL &&
				 (drop->dropflags & PERFORM_DELETION_CONCURRENTLY) != 0)
			forget_concurrent = objectId;
		else
			forget(classId, objectId);
	}
}

static void
metatrack_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		tables_dropped = false;
		in_extension = 0;
	}
	/* not at a commit, which a concurrent drop makes on its way */
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
		forget_concurrent = InvalidOid;
}

/* ------------------------------------------------------------------------- */
/* What statements do                                                        */
/* ------------------------------------------------------------------------- */

/* ALTER TABLE's subcommand, as its ALTER row says it (tablecmds.c). */
static const char *
alter_subtype(AlterTableType type)
{
	switch (type)
	{
		case AT_AddColumn:
			return "ADD COLUMN";
		case AT_DropNotNull:
			return "ALTER COLUMN DROP NOT NULL";
		case AT_SetNotNull:
			return "ALTER COLUMN SET NOT NULL";
		case AT_ColumnDefault:
			return "ALTER COLUMN DEFAULT";
		case AT_SetStatistics:
			return "ALTER COLUMN SET STATISTICS";
		case AT_SetStorage:
			return "ALTER COLUMN SET STORAGE";
		case AT_DropColumn:
			return "DROP COLUMN";
		case AT_AddIndex:
			return "ADD INDEX";
		case AT_AddConstraint:
			return "ADD CONSTRAINT";
		case AT_DropConstraint:
			return "DROP CONSTRAINT";
		case AT_AlterColumnType:
			return "ALTER COLUMN TYPE";
		case AT_ChangeOwner:
			return "OWNER";
		case AT_ClusterOn:
			return "CLUSTER ON";
		case AT_DropCluster:
			return "SET WITHOUT CLUSTER";
		case AT_SetRelOptions:
			return "SET";
		case AT_ResetRelOptions:
			return "RESET";
		case AT_SetTableSpace:
			return "SET TABLESPACE";
		case AT_AddInherit:
			return "INHERIT";
		case AT_DropInherit:
			return "NO INHERIT";
		default:
			return NULL;
	}
}

static void
alter_table(AlterTableStmt *stmt)
{
	Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

	if (!OidIsValid(relid))
		return;
	foreach_node(AlterTableCmd, cmd, stmt->cmds)
	{
		const char *subtype;

		/* a partition attached or detached, the partition's row */
		if (cmd->subtype == AT_AttachPartition || cmd->subtype == AT_DetachPartition)
		{
			PartitionCmd *pc = (PartitionCmd *) cmd->def;
			Oid			part = pc != NULL && pc->name != NULL ?
				RangeVarGetRelid(pc->name, NoLock, true) : InvalidOid;

			if (OidIsValid(part) && tracked_relation(part, true))
				record(RelationRelationId, part, "PARTITION",
					   cmd->subtype == AT_AttachPartition ? "ATTACH" : "DETACH",
					   false);
			continue;
		}
		subtype = alter_subtype(cmd->subtype);
		if (subtype != NULL && tracked_relation(relid, false))
			record(RelationRelationId, relid, "ALTER", subtype, false);
	}
}

/* A relation and, where it has them, its partitions and children. */
static List *
with_children(Oid relid)
{
	return find_all_inheritors(relid, NoLock, NULL);
}

/*
 * The relations a VACUUM or ANALYZE statement processed, as PostgreSQL's
 * vacuum() finds them: one given by its OID as it is -- which is how
 * gp_partanalyze.c hands on the list Cloudberry's rules make -- one named
 * with its partitions and children unless ONLY says not, and where none is
 * named, each table of the database the user may maintain
 * (get_all_vacuum_rels()), whose rows Cloudberry writes one by one.
 */
static List *
vacuumed_relations(VacuumStmt *stmt)
{
	List	   *result = NIL;

	if (stmt->rels == NIL)
	{
		Relation	pgclass = table_open(RelationRelationId, AccessShareLock);
		TableScanDesc scan = table_beginscan_catalog(pgclass, 0, NULL);
		HeapTuple	tuple;

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

			if ((form->relkind == RELKIND_RELATION ||
				 form->relkind == RELKIND_MATVIEW ||
				 form->relkind == RELKIND_PARTITIONED_TABLE) &&
				!(form->relpersistence == RELPERSISTENCE_TEMP &&
				  !isTempOrTempToastNamespace(form->relnamespace)) &&
				((object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
				  !form->relisshared) ||
				 pg_class_aclcheck(form->oid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK))
				result = lappend_oid(result, form->oid);
		}
		table_endscan(scan);
		table_close(pgclass, AccessShareLock);
		return result;
	}

	foreach_node(VacuumRelation, vr, stmt->rels)
	{
		Oid			relid = OidIsValid(vr->oid) ? vr->oid :
			(vr->relation != NULL ? RangeVarGetRelid(vr->relation, NoLock, true) : InvalidOid);

		if (!OidIsValid(relid))
			continue;
		if (!OidIsValid(vr->oid) && vr->relation->inh)
			result = list_concat(result, with_children(relid));
		else
			result = lappend_oid(result, relid);
	}
	return result;
}

/*
 * An option's Boolean value, as defGetBoolean() reads it, before PostgreSQL
 * has checked the statement: a value it refuses is false here, and its
 * error comes as the statement runs.
 */
static bool
option_is_true(DefElem *opt)
{
	if (opt->arg == NULL)
		return true;
	if (IsA(opt->arg, Integer))
		return intVal(opt->arg) == 1;
	return pg_strcasecmp(defGetString(opt), "true") == 0 ||
		pg_strcasecmp(defGetString(opt), "on") == 0;
}

/*
 * The rows a VACUUM or ANALYZE statement is to write, as its options say, in
 * "cxt"; NULL for none -- VACUUM (ONLY_DATABASE_STATS), which takes no
 * relation, or a database without gp_core's tables.
 */
static VacuumRows *
vacuum_rows_of(VacuumStmt *stmt, MemoryContext cxt)
{
	bool		full = false;
	bool		freeze = false;
	bool		analyze = !stmt->is_vacuumcmd;
	VacuumRows *rows;
	HASHCTL		ctl;

	if (!OidIsValid(table_of(RelationRelationId)))
		return NULL;
	foreach_node(DefElem, opt, stmt->options)
	{
		if (strcmp(opt->defname, "full") == 0)
			full = option_is_true(opt);
		else if (strcmp(opt->defname, "freeze") == 0)
			freeze = option_is_true(opt);
		else if (strcmp(opt->defname, "analyze") == 0)
			analyze = option_is_true(opt);
		else if (strcmp(opt->defname, "only_database_stats") == 0 && option_is_true(opt))
			return NULL;
	}

	rows = MemoryContextAllocZero(cxt, sizeof(VacuumRows));
	if (stmt->is_vacuumcmd)
		rows->vsubtype = full && freeze ? "FULL FREEZE" : full ? "FULL" : freeze ? "FREEZE" : "";
	rows->analyze = analyze;
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Oid);
	ctl.hcxt = cxt;
	rows->written = hash_create("gp_core VACUUM and ANALYZE rows", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	return rows;
}

/*
 * O3's hook, as analyze_rel() asks it of the relation it analyzes, which it
 * holds in ShareUpdateExclusiveLock till its transaction ends: the
 * relation's rows written in that transaction, where Cloudberry's
 * analyze_rel_internal() writes its ANALYZE row -- a VACUUM ANALYZE's
 * VACUUM row first, which vacuum_rel()'s transaction before it has no hook
 * to write, so that the two keep their order.  ANALYZE's row of each
 * relation it analyzed, a partitioned table's too; VACUUM's but a
 * partitioned table's, which has nothing to vacuum and none in Cloudberry
 * (vacuum_rel()).  Not where acquire_inherited_sample_rows() asks it of each
 * member of the tree it samples, as the table's owner, in a
 * security-restricted operation.
 */
static bool
metatrack_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
							  BlockNumber *totalpages)
{
	Oid			relid = RelationGetRelid(relation);
	bool		found;

	if (vacuum_rows != NULL && vacuum_rows->analyze && tracking() &&
		!InSecurityRestrictedOperation() &&
		CheckRelationOidLockedByMe(relid, ShareUpdateExclusiveLock, true))
	{
		(void) hash_search(vacuum_rows->written, &relid, HASH_ENTER, &found);
		if (!found)
		{
			if (vacuum_rows->vsubtype != NULL &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				record_processed(relid, "VACUUM", vacuum_rows->vsubtype);
			record_processed(relid, "ANALYZE", "");
		}
	}
	return prev_analyze_sample_rows
		? prev_analyze_sample_rows(relation, func, totalpages) : false;
}

/*
 * After a VACUUM, its row of each table it vacuumed that has none yet -- a
 * VACUUM ANALYZE's are written as it analyzes -- under the table's lock
 * taken again (lock_for_row()).
 */
static void
vacuum_(VacuumStmt *stmt)
{
	if (vacuum_rows == NULL || vacuum_rows->vsubtype == NULL)
		return;
	foreach_oid(relid, vacuumed_relations(stmt))
	{
		if (get_rel_relkind(relid) != RELKIND_RELATION ||
			hash_search(vacuum_rows->written, &relid, HASH_FIND, NULL) != NULL ||
			!lock_for_row(relid))
			continue;
		record_processed(relid, "VACUUM", vacuum_rows->vsubtype);
	}
}

static void
truncate_(TruncateStmt *stmt)
{
	foreach_node(RangeVar, rv, stmt->relations)
	{
		Oid			relid = RangeVarGetRelid(rv, NoLock, true);

		if (!OidIsValid(relid))
			continue;
		foreach_oid(child, rv->inh ? with_children(relid) : list_make1_oid(relid))
		{
			record_processed(child, "VACUUM", "TRUNCATE");
			record_processed(child, "TRUNCATE", "");
		}
	}
}

/*
 * The tables REINDEX SCHEMA, DATABASE or SYSTEM goes through, as PostgreSQL
 * 19's ReindexMultipleTables() chooses them: the tables and materialized
 * views of the schema, of the database less its catalogs, or the catalogs --
 * not another session's temporary ones, nor a shared one the user may not
 * maintain, nor a catalog with CONCURRENTLY.
 */
static List *
reindexed_tables(ReindexStmt *stmt)
{
	List	   *result = NIL;
	bool		concurrently = false;
	ScanKeyData key[1];
	int			nkeys = 0;
	Relation	pg_class;
	TableScanDesc scan;
	HeapTuple	tuple;

	foreach_node(DefElem, opt, stmt->params)
		if (strcmp(opt->defname, "concurrently") == 0)
			concurrently = defGetBoolean(opt);

	if (stmt->kind == REINDEX_OBJECT_SCHEMA)
	{
		Oid			nspid = get_namespace_oid(stmt->name, true);

		if (!OidIsValid(nspid))
			return NIL;
		ScanKeyInit(&key[0], Anum_pg_class_relnamespace, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(nspid));
		nkeys = 1;
	}

	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(pg_class, nkeys, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);
		bool		catalog = IsCatalogRelationOid(form->oid);

		if ((form->relkind != RELKIND_RELATION && form->relkind != RELKIND_MATVIEW) ||
			(form->relpersistence == RELPERSISTENCE_TEMP &&
			 !isTempNamespace(form->relnamespace)) ||
			(stmt->kind == REINDEX_OBJECT_SYSTEM && !catalog) ||
			(stmt->kind == REINDEX_OBJECT_DATABASE && catalog) ||
			(form->relisshared &&
			 pg_class_aclcheck(form->oid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK) ||
			(concurrently && catalog))
			continue;
		result = lappend_oid(result, form->oid);
	}
	table_endscan(scan);
	table_close(pg_class, AccessShareLock);
	return result;
}

/*
 * Each index a REINDEX rebuilt, as Cloudberry's reindex_index() records it:
 * of the index, of the table's, and of every table REINDEX SCHEMA, DATABASE
 * or SYSTEM went through.
 */
static void
reindex(ReindexStmt *stmt)
{
	Oid			relid;

	if (stmt->kind == REINDEX_OBJECT_SCHEMA ||
		stmt->kind == REINDEX_OBJECT_SYSTEM ||
		stmt->kind == REINDEX_OBJECT_DATABASE)
	{
		foreach_oid(table, reindexed_tables(stmt))
		{
			Relation	rel = try_relation_open(table, AccessShareLock);
			List	   *indexes;

			if (rel == NULL)
				continue;
			indexes = RelationGetIndexList(rel);
			relation_close(rel, AccessShareLock);
			foreach_oid(index, indexes)
				record_relation(index, "VACUUM", "REINDEX");
		}
		return;
	}

	if (stmt->relation == NULL)
		return;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
		return;
	if (stmt->kind == REINDEX_OBJECT_INDEX)
		record_relation(relid, "VACUUM", "REINDEX");
	else if (stmt->kind == REINDEX_OBJECT_TABLE)
	{
		Relation	rel = relation_open(relid, AccessShareLock);
		List	   *indexes = RelationGetIndexList(rel);

		relation_close(rel, AccessShareLock);
		foreach_oid(index, indexes)
			record_relation(index, "VACUUM", "REINDEX");
	}
}

static void
grant(GrantStmt *stmt)
{
	if (stmt->targtype != ACL_TARGET_OBJECT ||
		(stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_SEQUENCE))
		return;
	foreach_node(RangeVar, rv, stmt->objects)
	{
		Oid			relid = RangeVarGetRelid(rv, NoLock, true);

		if (OidIsValid(relid))
			record_relation(relid, "PRIVILEGE", stmt->is_grant ? "GRANT" : "REVOKE");
	}
}

/* ALTER ROLE's options, as its ALTER row says them (AlterRole(), user.c). */
static const char *
role_subtype(AlterRoleStmt *stmt)
{
	DefElem    *opt;

	if (list_length(stmt->options) == 0)
		return "0 OPTIONS";
	if (list_length(stmt->options) > 1)
		return psprintf("%d OPTIONS", list_length(stmt->options));
	opt = linitial_node(DefElem, stmt->options);
	if (strcmp(opt->defname, "canlogin") == 0)
		return "LOGIN";
	if (strcmp(opt->defname, "connectionlimit") == 0)
		return "CONNECTION LIMIT";
	if (strcmp(opt->defname, "validUntil") == 0)
		return "VALID UNTIL";
	if (strcmp(opt->defname, "rolemembers") == 0)
		return "ROLE";
	return asc_toupper(opt->defname, strlen(opt->defname));
}

/* A SET of ALTER DATABASE or ALTER ROLE, as its row says it (pg_db_role_setting.c). */
static const char *
set_subtype(VariableSetStmt *set)
{
	return set->kind == VAR_RESET_ALL ? "RESET ALL" :
		set->kind == VAR_RESET ? "RESET" : "SET";
}

static void
statement_done(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_AlterTableStmt:
			alter_table((AlterTableStmt *) parsetree);
			break;
		case T_CreateStmt:
			{
				CreateStmt *stmt = (CreateStmt *) parsetree;
				Oid			relid;

				/* CREATE TABLE ... PARTITION OF: the partition attached */
				if (stmt->partbound == NULL)
					break;
				relid = RangeVarGetRelid(stmt->relation, NoLock, true);
				if (OidIsValid(relid) && tracked_relation(relid, true))
					record(RelationRelationId, relid, "PARTITION", "ATTACH", false);
			}
			break;
		case T_AlterSeqStmt:
			{
				AlterSeqStmt *stmt = (AlterSeqStmt *) parsetree;
				Oid			relid = RangeVarGetRelid(stmt->sequence, NoLock, true);

				foreach_node(DefElem, opt, stmt->options)
				{
					const char *subtype = strcmp(opt->defname, "owned_by") == 0 ?
						"OWNED BY" : asc_toupper(opt->defname, strlen(opt->defname));

					if (OidIsValid(relid))
						record_relation(relid, "ALTER", subtype);
				}
			}
			break;
		case T_GrantStmt:
			grant((GrantStmt *) parsetree);
			break;
		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				foreach_node(AccessPriv, priv, stmt->granted_roles)
				{
					Oid			roleid = get_role_oid(priv->priv_name, true);

					if (OidIsValid(roleid))
						record(AuthIdRelationId, roleid, "PRIVILEGE",
							   stmt->is_grant ? "GRANT" : "REVOKE", false);
				}
			}
			break;
		case T_VacuumStmt:
			vacuum_((VacuumStmt *) parsetree);
			break;
		case T_TruncateStmt:
			truncate_((TruncateStmt *) parsetree);
			break;
		case T_ReindexStmt:
			reindex((ReindexStmt *) parsetree);
			break;
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;
				Oid			relid;

				switch (stmt->renameType)
				{
					case OBJECT_TABLE:
					case OBJECT_INDEX:
					case OBJECT_SEQUENCE:
					case OBJECT_VIEW:
					case OBJECT_COLUMN:
						relid = RangeVarGetRelid(stmt->relation, NoLock, true);
						if (OidIsValid(relid) && tracked_relation(relid, false))
							record(RelationRelationId, relid, "ALTER",
								   stmt->renameType == OBJECT_COLUMN ? "RENAME COLUMN" : "RENAME",
								   false);
						break;
					case OBJECT_SCHEMA:
						record(NamespaceRelationId,
							   get_namespace_oid(stmt->newname, true), "ALTER", "RENAME", false);
						break;
					case OBJECT_DATABASE:
						record(DatabaseRelationId,
							   get_database_oid(stmt->newname, true), "ALTER", "RENAME", false);
						break;
					case OBJECT_ROLE:
						record(AuthIdRelationId,
							   get_role_oid(stmt->newname, true), "ALTER", "RENAME", false);
						break;
					case OBJECT_TABLESPACE:
						record(TableSpaceRelationId,
							   get_tablespace_oid(stmt->newname, true), "ALTER", "RENAME", false);
						break;
					default:
						break;
				}
			}
			break;
		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (stmt->objectType == OBJECT_SCHEMA)
					record(NamespaceRelationId,
						   get_namespace_oid(strVal(stmt->object), true), "ALTER", "OWNER", false);
				else if (stmt->objectType == OBJECT_DATABASE)
					record(DatabaseRelationId,
						   get_database_oid(strVal(stmt->object), true), "ALTER", "OWNER", false);
			}
			break;
		case T_AlterDatabaseSetStmt:
			{
				AlterDatabaseSetStmt *stmt = (AlterDatabaseSetStmt *) parsetree;

				record(DatabaseRelationId, get_database_oid(stmt->dbname, true),
					   "ALTER", set_subtype(stmt->setstmt), false);
			}
			break;
		case T_AlterDatabaseStmt:
			{
				AlterDatabaseStmt *stmt = (AlterDatabaseStmt *) parsetree;

				foreach_node(DefElem, opt, stmt->options)
					if (strcmp(opt->defname, "connection_limit") == 0)
						record(DatabaseRelationId, get_database_oid(stmt->dbname, true),
							   "ALTER", "CONNECTION LIMIT", false);
			}
			break;
		case T_AlterRoleSetStmt:
			{
				AlterRoleSetStmt *stmt = (AlterRoleSetStmt *) parsetree;

				/*
				 * Cloudberry's row of a role's setting is under pg_database's
				 * class, the role's OID its object (AlterSetting()).
				 */
				if (stmt->role != NULL && stmt->role->roletype == ROLESPEC_CSTRING)
					record(DatabaseRelationId, get_role_oid(stmt->role->rolename, true),
						   "ALTER", set_subtype(stmt->setstmt), false);
			}
			break;
		case T_AlterRoleStmt:
			{
				AlterRoleStmt *stmt = (AlterRoleStmt *) parsetree;

				if (stmt->role != NULL && stmt->role->roletype == ROLESPEC_CSTRING)
					record(AuthIdRelationId, get_role_oid(stmt->role->rolename, true),
						   "ALTER", role_subtype(stmt), false);
			}
			break;
		default:
			break;
	}
}

static void
metatrack_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		extension = IsA(parsetree, CreateExtensionStmt) ||
		IsA(parsetree, AlterExtensionStmt);
	VacuumRows *outer_rows = vacuum_rows;
	MemoryContext rows_cxt = NULL;

	/*
	 * An extension's schema, which CREATE EXTENSION makes before its script
	 * runs, is the extension's as much as what the script makes.
	 */
	if (extension)
		in_extension++;

	/* A VACUUM or ANALYZE statement writes rows as it goes. */
	if (IsA(parsetree, VacuumStmt) && tracking())
		rows_cxt = AllocSetContextCreate(TopMemoryContext, "gp_core VACUUM and ANALYZE rows",
										 ALLOCSET_SMALL_SIZES);

	PG_TRY();
	{
		if (rows_cxt != NULL)
			vacuum_rows = vacuum_rows_of((VacuumStmt *) parsetree, rows_cxt);
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		if (extension)
			in_extension--;

		if (OidIsValid(forget_concurrent))
		{
			Oid			relid = forget_concurrent;

			forget_concurrent = InvalidOid;
			if (tracking())
				forget(RelationRelationId, relid);
		}

		if (tracking())
		{
			/* what the statement made, seen */
			CommandCounterIncrement();
			statement_done(parsetree);
		}
	}
	PG_FINALLY();
	{
		vacuum_rows = outer_rows;
		if (rows_cxt != NULL)
			MemoryContextDelete(rows_cxt);
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* What the other modules ask                                                */
/* ------------------------------------------------------------------------- */

/*
 * A partitioned table's partitions changed by Cloudberry's own partition
 * commands, which gp_sql carries out: the row of the table, as
 * GpAlterPartMetaTrackUpdObject() writes it (tablecmds_gp.c).
 */
void
GpMetaTrackPartition(Oid relid, const char *subtype)
{
	if (tracking())
		record(RelationRelationId, relid, "PARTITION", subtype, false);
}

void
GpMetaTrackInit(void)
{
	prev_object_access_hook = object_access_hook;
	object_access_hook = metatrack_object_access;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = metatrack_ProcessUtility;
	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = metatrack_analyze_sample_rows;
	RegisterXactCallback(metatrack_xact_callback, NULL);
}
