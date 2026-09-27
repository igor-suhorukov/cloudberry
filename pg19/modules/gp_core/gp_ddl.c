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
 * gp_ddl.c
 *	  DDL dispatch: a statement that changes the catalogs changes them on
 *	  every node, and gives every object the coordinator's OID.
 *
 * Cloudberry calls CdbDispatchUtilityStatement() from 119 places in the
 * commands it patched, each at the point where that command's work is done.
 * The port cannot patch a command, but it does not need to: every one of
 * those statements passes through ProcessUtility, and the plan (Track C §2.7)
 * collapses the 119 call sites into this hook.
 *
 * What travels is the statement's *parse tree*, as Cloudberry's dispatch
 * sends it, not its text.  The text is not always the statement: gp_sql makes
 * each partition of a classic partitioned table as a CREATE TABLE ...
 * PARTITION OF node of its own, run with the parent's text, and a module may
 * do the same with any statement it builds.  PostgreSQL 19 reads and writes
 * raw parse trees in every build (readfuncs.c), so nodeToString() carries one
 * whole; the segment's parser hands it back as parsed, through the marker
 * gp_core's raw_parser_hook recognises in a dispatched backend.
 *
 * The OIDs are R1's.  While the coordinator runs the statement, new_oid_hook
 * notes every OID it gives a catalog row, in order, with the catalog it went
 * to; the list travels with the tree, and on the segment the same hook hands
 * them out again, in the same order, checking each against the catalog that
 * asks.  R1's hook is given the catalog and not the row, so the port cannot
 * match an OID to an object by key as Cloudberry's 45 OID wrappers do; it
 * matches by order and checks the catalog, and a segment that asks for a
 * different catalog, or for more OIDs or fewer, raises rather than going its
 * own way.  Order is safe because the segment runs the same tree through the
 * same code on the same catalogs; the one exception is a session's temporary
 * namespace, which each backend makes for itself when it first needs one --
 * see new_oid().
 *
 * A catalog row a segment makes for itself -- that namespace, or anything
 * made there outside a dispatched statement -- takes an OID the coordinator
 * never gives: one from the top of the OID space down (local_oid()), where
 * Cloudberry's segment took one from its own counter and its coordinator,
 * before it made a relation, moved its counter past every segment's
 * (cdb_sync_oid_to_segments()).
 *
 * Which statements: the ones PostgreSQL itself calls "not read-only" --
 * DDL and TRUNCATE (ClassifyUtilityCommandAsReadOnly in utility.c) -- less
 * those whose effect is local to this node or which carry data, and plus
 * VACUUM, REINDEX and CLUSTER, which write nothing pg_dump sees but have to
 * reach the rows where the rows are.  See dispatch_class().
 *
 * Cloudberry sources this file stands in for:
 *	  the CdbDispatchUtilityStatement() calls in src/backend/commands/ and
 *	  src/backend/tcop/utility.c, and src/backend/catalog/oid_dispatch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "parser/parse_type.h"
#include "catalog/objectaddress.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/vacuum.h"
#include "common/relpath.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/readfuncs.h"
#include "parser/parser.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/dsm_registry.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_label.h"
#include "gp_loopback.h"
#include "gp_policy.h"
#include "gp_refresh.h"
#include "gp_scan.h"

/*
 * What a dispatched statement's text starts with.  Only a dispatched backend
 * looks for it, so a user who types it gets PostgreSQL's syntax error.  Then
 * the OIDs, "oids=<catalog>:<oid>,...", and " text=<n>" where the client's
 * statement follows the line's end in n bytes -- what the segment's
 * pg_stat_activity shows while it runs the tree, as Cloudberry's segments
 * show the statement the coordinator dispatched -- and then the tree.
 */
#define GP_TREE_MARKER		"/*gp:dispatched-tree*/"

typedef struct GpOidAssignment
{
	Oid			catalog;
	Oid			oid;
} GpOidAssignment;

typedef enum GpDispatchClass
{
	GP_DISPATCH_LOCAL,			/* this node only */
	GP_DISPATCH_IN_XACT,		/* every node, in the coordinator's transaction */
	GP_DISPATCH_OWN_XACT,		/* every node, each in a transaction of its own */
} GpDispatchClass;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static raw_parser_hook_type prev_raw_parser = NULL;
static new_oid_hook_type prev_new_oid_hook = NULL;
static tablespace_location_hook_type prev_tablespace_location_hook = NULL;
static tablespace_location_drop_hook_type prev_tablespace_location_drop_hook = NULL;

/*
 * Where the OIDs are kept, on either side.  Not a transaction's context: VACUUM
 * FULL, CREATE INDEX CONCURRENTLY and REINDEX of a database commit as they go,
 * and the list has to span the transactions of one statement.
 */
static MemoryContext ddl_cxt = NULL;

/* The coordinator: the statement being run here and dispatched after. */
static bool recording = false;
static bool recording_vacuum = false;	/* and it is a VACUUM */
static List *recorded = NIL;	/* of GpOidAssignment *, in ddl_cxt */

/* A segment: the statement the coordinator dispatched, and its OIDs. */
static Node *dispatched_tree = NULL;
static GpOidAssignment *preassigned = NULL;
static int	npreassigned = 0;
static int	next_preassigned = 0;
static bool preassigning = false;

/*
 * A segment's own catalog OIDs, from the top of the OID space down: the next
 * to try, shared by the node's backends (local_oid()).
 */
#define LOCAL_OID_TOP		((Oid) 0xFFFFFFFE)
#define LOCAL_OID_BOTTOM	((Oid) 0x80000000)

static pg_atomic_uint32 *local_oid_next = NULL;

/* ------------------------------------------------------------------------- */
/* R1: the OIDs                                                              */
/* ------------------------------------------------------------------------- */

static void
preassigned_clear(void)
{
	preassigned = NULL;			/* in ddl_cxt, which the caller resets */
	npreassigned = 0;
	next_preassigned = 0;
	preassigning = false;
	dispatched_tree = NULL;
}

static void
local_oid_init(void *ptr, void *arg)
{
	pg_atomic_init_u32((pg_atomic_uint32 *) ptr, LOCAL_OID_TOP);
}

/*
 * An OID for a catalog row a segment makes for itself: a session's temporary
 * namespace, which each backend makes when it first needs one, or anything
 * made on the segment outside a dispatched statement.
 *
 * The coordinator knows nothing of such a row, and may later give its OID to
 * an object of its own, which every node then makes with that OID: on this
 * segment the catalog's unique index would refuse it, and the statement
 * would fail.  An OID from this node's counter could well be one the
 * coordinator gives later -- the counter runs ahead of the coordinator's
 * with each TOAST value stored here -- so it is taken instead from the top
 * of the OID space down, where the coordinator's counter, which DDL moves,
 * would take some two billion OIDs to reach.  One already in the catalog is
 * passed over, as GetNewOidWithIndex() passes one over: any row, dead or
 * alive, committed or not.
 */
static Oid
local_oid(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	if (local_oid_next == NULL)
	{
		bool		found;

		local_oid_next = GetNamedDSMSegment("gp_core local OIDs",
											sizeof(pg_atomic_uint32),
											local_oid_init, &found, NULL);
	}

	for (;;)
	{
		Oid			oid = pg_atomic_fetch_sub_u32(local_oid_next, 1);
		ScanKeyData key;
		SysScanDesc scan;
		bool		used;

		/* two billion of them made on this node since it started: again */
		if (oid < LOCAL_OID_BOTTOM)
		{
			pg_atomic_write_u32(local_oid_next, LOCAL_OID_TOP);
			continue;
		}

		ScanKeyInit(&key, oidcolumn, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(oid));
		scan = systable_beginscan(relation, indexId, true, SnapshotAny, 1,
								  &key);
		used = HeapTupleIsValid(systable_getnext(scan));
		systable_endscan(scan);
		if (!used)
			return oid;

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * new_oid_hook.
 *
 * Only catalog rows are the cluster's business.  A TOAST value's chunk id is
 * an OID too, and so is a user table's with OIDs, but neither names anything
 * a plan or a stored value refers to, and each node picks its own.
 */
static Oid
new_oid(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	Oid			catalog = RelationGetRelid(relation);

	if (!IsCatalogRelation(relation) || IsToastRelation(relation))
		return prev_new_oid_hook ? prev_new_oid_hook(relation, indexId, oidcolumn)
			: InvalidOid;

	if (recording)
	{
		GpOidAssignment *a;
		Oid			oid;
		MemoryContext oldcxt;

		/*
		 * Take the OID PostgreSQL would have taken, by asking it with the hook
		 * out of the way, and write it down.  Returning it through the hook
		 * marks it as preassigned, so a relation gets a relfilenumber of its
		 * own rather than its OID -- which the segments do in any case, and
		 * costs the coordinator nothing.
		 */
		new_oid_hook = prev_new_oid_hook;
		PG_TRY();
		{
			oid = GetNewOidWithIndex(relation, indexId, oidcolumn);
		}
		PG_FINALLY();
		{
			new_oid_hook = new_oid;
		}
		PG_END_TRY();

		oldcxt = MemoryContextSwitchTo(ddl_cxt);
		a = (GpOidAssignment *) palloc(sizeof(GpOidAssignment));
		a->catalog = catalog;
		a->oid = oid;
		recorded = lappend(recorded, a);
		MemoryContextSwitchTo(oldcxt);

		return oid;
	}

	if (preassigning)
	{
		GpOidAssignment *a;

		if (next_preassigned >= npreassigned ||
			preassigned[next_preassigned].catalog != catalog)
		{
			/*
			 * A session's temporary namespace is made by each backend for
			 * itself, the first time it needs one, and whether the segment's
			 * backend still has to make one says nothing about whether the
			 * coordinator's did.  The coordinator leaves its own out of the
			 * list (see drop_temp_namespaces()), and the segment takes one
			 * of its own (local_oid()).  A temporary namespace is never named
			 * by OID in anything dispatched, so nothing is lost.
			 */
			if (catalog == NamespaceRelationId)
				return local_oid(relation, indexId, oidcolumn);

			if (next_preassigned >= npreassigned)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("segment needs an OID for %s that the coordinator did not allocate",
								RelationGetRelationName(relation)),
						 errdetail("The coordinator allocated %d OIDs for this statement.",
								   npreassigned)));
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("segment and coordinator are out of step allocating OIDs"),
					 errdetail("OID %d of the statement is for catalog %u on the coordinator and for %s here.",
							   next_preassigned + 1,
							   preassigned[next_preassigned].catalog,
							   RelationGetRelationName(relation))));
		}

		a = &preassigned[next_preassigned++];
		return a->oid;
	}

	if (prev_new_oid_hook)
	{
		Oid			oid = prev_new_oid_hook(relation, indexId, oidcolumn);

		if (OidIsValid(oid))
			return oid;
	}

	/*
	 * A row a segment makes for itself.  pg_upgrade gives the OIDs it cares
	 * about itself, and leaves the rest to the counter.
	 */
	if (GpClusterContentId() >= 0 && !IsBinaryUpgrade)
		return local_oid(relation, indexId, oidcolumn);
	return InvalidOid;
}

/*
 * Leave this session's temporary namespaces out of what the segments are sent;
 * see new_oid().
 */
static void
drop_temp_namespaces(void)
{
	Oid			temp_ns;
	Oid			temp_toast_ns;
	ListCell   *lc;

	GetTempNamespaceState(&temp_ns, &temp_toast_ns);

	foreach(lc, recorded)
	{
		GpOidAssignment *a = (GpOidAssignment *) lfirst(lc);

		if (a->catalog == NamespaceRelationId &&
			(a->oid == temp_ns || a->oid == temp_toast_ns))
			recorded = foreach_delete_current(recorded, lc);
	}
}

/* ------------------------------------------------------------------------- */
/* Which statements go where                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Every statement PostgreSQL counts as changing the database -- the DDL list
 * of ClassifyUtilityCommandAsReadOnly() -- goes to every node, except:
 *
 *   - CREATE TABLE AS, SELECT INTO, CREATE MATERIALIZED VIEW and REFRESH
 *     MATERIALIZED VIEW, which carry rows: made on every node WITH NO DATA,
 *     as gp_sql makes each on a cluster, and filled by an INSERT, which puts
 *     each row where it belongs (gp_refresh.c for a materialized view);
 *   - moving a database to another tablespace, whose other connections the
 *     segments cannot see to refuse it;
 *   - publications, subscriptions and event triggers, which are about this
 *     node's own WAL and this node's own DDL -- and dropping, renaming,
 *     giving away or commenting on one.
 *
 * VACUUM, REINDEX and CLUSTER are read-only by PostgreSQL's definition -- they
 * change nothing pg_dump would show -- but they have to reach the rows, and
 * the rows are on the segments; and ALTER SYSTEM is Cloudberry's every
 * node's.  ANALYZE stays here until O3 brings the
 * segments' samples to it.
 */
/*
 * Is it an object of this node's own -- a publication, a subscription, an
 * event trigger -- which only the coordinator has, so that a statement that
 * drops, renames, gives away or comments on one is the coordinator's too?
 */
static bool
local_object(ObjectType type)
{
	switch (type)
	{
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_EVENT_TRIGGER:
			return true;
		default:
			return false;
	}
}

/* Is it a partitioned table or index?  False where there is no such relation. */
static bool
is_partitioned(Oid relid)
{
	char		relkind = OidIsValid(relid) ? get_rel_relkind(relid) : '\0';

	return relkind == RELKIND_PARTITIONED_TABLE ||
		relkind == RELKIND_PARTITIONED_INDEX;
}

static GpDispatchClass
dispatch_class(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreatedbStmt:
		case T_DropdbStmt:
			return GP_DISPATCH_OWN_XACT;

		/*
		 * ALTER SYSTEM, read-only by PostgreSQL's definition, writes each
		 * node's postgresql.auto.conf in Cloudberry, which dispatches it after
		 * its own (utility.c); outside a transaction, as it runs.
		 */
		case T_AlterSystemStmt:
			return GP_DISPATCH_OWN_XACT;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->is_vacuumcmd)
				return GP_DISPATCH_OWN_XACT;
			return GP_DISPATCH_LOCAL;

		case T_IndexStmt:
			return ((IndexStmt *) parsetree)->concurrent
				? GP_DISPATCH_OWN_XACT : GP_DISPATCH_IN_XACT;

		case T_DropStmt:
			if (local_object(((DropStmt *) parsetree)->removeType))
				return GP_DISPATCH_LOCAL;
			return ((DropStmt *) parsetree)->concurrent
				? GP_DISPATCH_OWN_XACT : GP_DISPATCH_IN_XACT;

		case T_RenameStmt:
			return local_object(((RenameStmt *) parsetree)->renameType)
				? GP_DISPATCH_LOCAL : GP_DISPATCH_IN_XACT;
		case T_AlterOwnerStmt:
			return local_object(((AlterOwnerStmt *) parsetree)->objectType)
				? GP_DISPATCH_LOCAL : GP_DISPATCH_IN_XACT;
		case T_CommentStmt:
			return local_object(((CommentStmt *) parsetree)->objtype)
				? GP_DISPATCH_LOCAL : GP_DISPATCH_IN_XACT;
		case T_SecLabelStmt:
			return local_object(((SecLabelStmt *) parsetree)->objtype)
				? GP_DISPATCH_LOCAL : GP_DISPATCH_IN_XACT;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;
				ListCell   *lc;

				foreach(lc, stmt->params)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "concurrently") == 0 &&
						defGetBoolean(opt))
						return GP_DISPATCH_OWN_XACT;
				}

				/*
				 * REINDEX SCHEMA, SYSTEM and DATABASE commit as they go, a
				 * transaction a table, and so does one of a partitioned
				 * table or index, a transaction a partition
				 * (ReindexPartitions()): PostgreSQL runs none inside a
				 * transaction block.
				 */
				if (stmt->kind == REINDEX_OBJECT_SCHEMA ||
					stmt->kind == REINDEX_OBJECT_DATABASE ||
					stmt->kind == REINDEX_OBJECT_SYSTEM ||
					(stmt->relation != NULL &&
					 is_partitioned(RangeVarGetRelid(stmt->relation, NoLock, true))))
					return GP_DISPATCH_OWN_XACT;
				return GP_DISPATCH_IN_XACT;
			}

		/*
		 * CLUSTER and REPACK of every table, or of a partitioned one, commit
		 * as they go, a transaction a table, and with CONCURRENTLY or
		 * ANALYZE run outside a transaction block too (ExecRepack()); of one
		 * table, in the transaction that asks.
		 */
		case T_RepackStmt:
			{
				RepackStmt *stmt = (RepackStmt *) parsetree;
				ListCell   *lc;

				if (stmt->relation == NULL ||
					is_partitioned(RangeVarGetRelid(stmt->relation->relation,
													NoLock, true)))
					return GP_DISPATCH_OWN_XACT;
				foreach(lc, stmt->params)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if ((strcmp(opt->defname, "concurrently") == 0 ||
						 strcmp(opt->defname, "analyze") == 0) &&
						defGetBoolean(opt))
						return GP_DISPATCH_OWN_XACT;
				}
				return GP_DISPATCH_IN_XACT;
			}

		case T_AlterDatabaseStmt:
			{
				ListCell   *lc;

				foreach(lc, ((AlterDatabaseStmt *) parsetree)->options)
				{
					if (strcmp(((DefElem *) lfirst(lc))->defname, "tablespace") == 0)
						return GP_DISPATCH_LOCAL;
				}
				return GP_DISPATCH_IN_XACT;
			}

		/*
		 * A tablespace is a directory on each machine, and each node's is the
		 * directory of its dbid under it (node_tablespace_location()).
		 */
		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
			return GP_DISPATCH_OWN_XACT;
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableMoveAllStmt:
			return GP_DISPATCH_IN_XACT;

		/*
		 * A table made from a query WITH NO DATA is made on every node, as
		 * the CREATE TABLE PostgreSQL makes of it (ctas_as_create); gp_sql
		 * turns every CREATE TABLE AS on a cluster into one, and fills the
		 * table with an INSERT, which puts each row where it belongs.
		 */
		case T_CreateTableAsStmt:
			{
				CreateTableAsStmt *ctas = (CreateTableAsStmt *) parsetree;

				if ((ctas->objtype == OBJECT_TABLE ||
					 ctas->objtype == OBJECT_MATVIEW) && ctas->into->skipData)
					return GP_DISPATCH_IN_XACT;
				return GP_DISPATCH_LOCAL;
			}

		/*
		 * A materialized view whose rows are on the segments is emptied on
		 * every node, as gp_refresh.c's REFRESH begins; one of the
		 * coordinator's alone is refreshed here.
		 */
		case T_RefreshMatViewStmt:
			{
				RefreshMatViewStmt *stmt = (RefreshMatViewStmt *) parsetree;
				Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

				if (stmt->skipData && !stmt->concurrent &&
					OidIsValid(relid) && GpRefreshIsDistributed(relid))
					return GP_DISPATCH_IN_XACT;
				return GP_DISPATCH_LOCAL;
			}

		case T_CreatePublicationStmt:
		case T_AlterPublicationStmt:
		case T_CreateSubscriptionStmt:
		case T_AlterSubscriptionStmt:
		case T_DropSubscriptionStmt:
		case T_CreateEventTrigStmt:
		case T_AlterEventTrigStmt:
			return GP_DISPATCH_LOCAL;

		case T_AlterCollationStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_AlterDomainStmt:
		case T_AlterEnumStmt:
		case T_AlterExtensionContentsStmt:
		case T_AlterExtensionStmt:
		case T_AlterFdwStmt:
		case T_AlterForeignServerStmt:
		case T_AlterFunctionStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOpFamilyStmt:
		case T_AlterOperatorStmt:
		case T_AlterPolicyStmt:
		case T_AlterRoleSetStmt:
		case T_AlterRoleStmt:
		case T_AlterSeqStmt:
		case T_AlterStatsStmt:
		case T_AlterTSConfigurationStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTableStmt:
		case T_AlterTypeStmt:
		case T_AlterUserMappingStmt:
		case T_CompositeTypeStmt:
		case T_CreateAmStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateExtensionStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateForeignTableStmt:
		case T_CreateFunctionStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreatePLangStmt:
		case T_CreatePolicyStmt:
		case T_CreateRangeStmt:
		case T_CreateRoleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStatsStmt:
		case T_CreateStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CreateUserMappingStmt:
		case T_DefineStmt:
		case T_DropOwnedStmt:
		case T_DropRoleStmt:
		case T_DropUserMappingStmt:
		case T_GrantRoleStmt:
		case T_GrantStmt:
		case T_ImportForeignSchemaStmt:
		case T_ReassignOwnedStmt:
		case T_RuleStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
			return GP_DISPATCH_IN_XACT;

		default:
			return GP_DISPATCH_LOCAL;
	}
}

/*
 * CREATE EXTENSION and ALTER EXTENSION UPDATE run the extension's script
 * here and then on every segment, and while it runs here what its queries
 * write is the coordinator's copy alone (GpDispatchIsRecording()); each
 * segment writes its own as it runs the script.  So a table the script
 * gave a distribution -- PostGIS's spatial_ref_sys, which gp_sql replicates
 * (decision 14b) -- keeps its rows here only until the script is done: the
 * coordinator keeps none of a distributed table's rows, as Cloudberry's does
 * not, and a copy kept here would never see a later write.  A table the
 * script made is emptied at no cost (ExecuteTruncateGuts() truncates one
 * made in this transaction in place); one an update script wrote is given
 * an empty file, as TRUNCATE gives it one.
 */
static void
empty_script_tables(Node *parsetree)
{
	const char *extname;
	Oid			extoid;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *lc;

	if (IsA(parsetree, CreateExtensionStmt))
		extname = ((CreateExtensionStmt *) parsetree)->extname;
	else if (IsA(parsetree, AlterExtensionStmt))
		extname = ((AlterExtensionStmt *) parsetree)->extname;
	else
		return;
	extoid = get_extension_oid(extname, true);
	if (!OidIsValid(extoid))
		return;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(extoid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL,
							  2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
		GpPolicy   *policy;
		Relation	rel;

		if (dep->classid != RelationRelationId ||
			dep->deptype != DEPENDENCY_EXTENSION ||
			get_rel_relkind(dep->objid) != RELKIND_RELATION)
			continue;
		policy = GpPolicyGet(dep->objid);
		if (GpPolicyIsEntry(policy))
			continue;

		/* the copy is empty here but where the script wrote it */
		rel = table_open(dep->objid, AccessShareLock);
		if (RelationGetNumberOfBlocks(rel) == 0)
		{
			table_close(rel, AccessShareLock);
			continue;
		}
		table_close(rel, NoLock);
		rel = table_open(dep->objid, AccessExclusiveLock);
		rels = lappend(rels, rel);
		relids = lappend_oid(relids, dep->objid);
		if (RelationIsLogicallyLogged(rel))
			relids_logged = lappend_oid(relids_logged, dep->objid);
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	if (rels == NIL)
		return;
	ExecuteTruncateGuts(rels, relids, relids_logged, DROP_RESTRICT, false,
						false);
	foreach(lc, rels)
		table_close((Relation) lfirst(lc), NoLock);
}

/*
 * The same extension on every node ("PostGIS on the hook-based Cloudberry",
 * what the Cloudberry extension must add, item 5).  CREATE EXTENSION with no
 * VERSION installs each node's own default version, and a node's library is
 * whatever its host has installed; so once the statement has run everywhere,
 * each segment's version of the extension is compared with the
 * coordinator's -- and for PostGIS's, the libraries it reports it was built
 * with, GEOS, PROJ and GDAL, since two hosts' PostGIS of one version may
 * link different ones.  A difference is an error, which undoes the
 * statement on every node.  PostGIS's full version string is not compared
 * whole: it names PROJ's directories, which are a host's.
 */
static const struct
{
	const char *extname;
	const char *query;			/* %1$s: the extension's schema, quoted */
}			extension_libraries[] = {
	{"postgis",
		"SELECT %1$s.postgis_lib_version() || ', GEOS ' || %1$s.postgis_geos_version()"
		" || ', PROJ ' || pg_catalog.split_part(%1$s.postgis_proj_version(), ' ', 1)"},
	{"postgis_raster", "SELECT %1$s.postgis_gdal_version()"},
	{"postgis_sfcgal", "SELECT %1$s.postgis_sfcgal_version()"},
};

/*
 * The first column of the query's first row here, as text, or NULL.  Not
 * read-only: that would read with the statement's snapshot, taken before
 * the extension was made.
 */
static char *
coordinator_value(const char *sql)
{
	MemoryContext cxt = CurrentMemoryContext;
	char	   *result = NULL;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 1) == SPI_OK_SELECT && SPI_processed > 0)
	{
		char	   *value = SPI_getvalue(SPI_tuptable->vals[0],
										 SPI_tuptable->tupdesc, 1);

		if (value != NULL)
			result = MemoryContextStrdup(cxt, value);
	}
	SPI_finish();
	return result;
}

static void
check_same_everywhere(const char *extname, const char *sql)
{
	int			nsegments = GpClusterSegmentCount();
	char	  **values = palloc0_array(char *, nsegments);
	char	   *mine = coordinator_value(sql);

	GpDispatchQueryFirstValues(sql, -1, values);
	for (int i = 0; i < nsegments; i++)
	{
		if (values[i] != NULL && mine != NULL && strcmp(values[i], mine) == 0)
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("extension \"%s\" is not the same on segment %d as on the coordinator",
						extname, i),
				 errdetail("The segment has \"%s\", the coordinator \"%s\".",
						   values[i] != NULL ? values[i] : "",
						   mine != NULL ? mine : ""),
				 errhint("Install the same version of the extension, and of the libraries it uses, on every host.")));
	}
}

/*
 * A database a superuser makes on a cluster's coordinator gets gp_core's
 * extension, as gpinitsystem gives it template1 and postgres
 * (CREATE_GPEXTENSIONS): one made from template0, which has none, would
 * otherwise lack the functions the cluster runs through there --
 * gp_internal.exec_fragment(), by which a segment runs a slice of ORCA's
 * plan, the interconnect's, a split update's -- and every plan of ORCA's
 * with a Motion would be refused in it.  Cloudberry's are its catalog's, in
 * every database.  The other modules' extensions stay the database's own
 * affair, as they are in one made from template1.
 *
 * CREATE DATABASE runs in a transaction of its own, and its database is
 * another backend's to connect to only once that has committed: so it is
 * committed here, as VACUUM commits its own, and the statement goes on in
 * a new one.  The extension is made over a connection to the new database,
 * whose CREATE EXTENSION is dispatched to the segments from there as any
 * is -- quietly, where the template had it already, and planned by the
 * planner.  If it fails, the database is left as it is, and a WARNING
 * says why: the database was made.
 */
static void
create_core_extension(const char *dbname)
{
	char	   *failure;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		GpClusterIsSingleNode() || !superuser() ||
		!extension_file_exists("gp_core"))
		return;

	/* the portal's snapshot goes with the transaction, as VACUUM's does */
	if (ActiveSnapshotSet())
		PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();

	failure = GpLoopbackRunApart(dbname,
								 "SET client_min_messages = warning; "
								 "SET gp.optimizer = off; "
								 "CREATE EXTENSION IF NOT EXISTS gp_core");
	if (failure != NULL)
		ereport(WARNING,
				(errmsg("extension \"gp_core\" was not created in database \"%s\"",
						dbname),
				 errdetail_internal("%s", failure)));
}

/*
 * The check's queries are the planner's: ORCA would take one of the
 * catalogs only to fall back from it, and say so to a session that traces
 * its fallbacks, in lines Cloudberry's CREATE EXTENSION never prints.
 */
static void
check_extension_everywhere(Node *parsetree)
{
	const char *extname;
	Oid			extoid;
	int			save_nestlevel;

	if (IsA(parsetree, CreateExtensionStmt))
		extname = ((CreateExtensionStmt *) parsetree)->extname;
	else if (IsA(parsetree, AlterExtensionStmt))
		extname = ((AlterExtensionStmt *) parsetree)->extname;
	else
		return;
	extoid = get_extension_oid(extname, true);
	if (!OidIsValid(extoid))
		return;

	save_nestlevel = NewGUCNestLevel();
	if (GetConfigOption("gp.optimizer", true, false) != NULL)
		(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0,
								 false);
	check_same_everywhere(extname,
						  psprintf("SELECT extversion FROM pg_catalog.pg_extension WHERE extname = %s",
								   quote_literal_cstr(extname)));
	for (int i = 0; i < lengthof(extension_libraries); i++)
	{
		char	   *schema;

		if (strcmp(extension_libraries[i].extname, extname) != 0)
			continue;
		schema = get_namespace_name(get_extension_schema(extoid));
		check_same_everywhere(extname,
							  psprintf(extension_libraries[i].query,
									   quote_identifier(schema)));
	}
	AtEOXact_GUC(false, save_nestlevel);
}

/* ------------------------------------------------------------------------- */
/* Tablespaces                                                               */
/* ------------------------------------------------------------------------- */

static void next_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree, ProcessUtilityContext context,
								ParamListInfo params, QueryEnvironment *queryEnv,
								DestReceiver *dest, QueryCompletion *qc);

/* The widest a node's dbid, gp.dbid, is printed: INT_MAX's ten digits. */
#define DBID_CHARS	10

/*
 * A tablespace is a directory, and the nodes of a cluster may share a
 * machine: each node's is the directory named for its dbid under the one
 * CREATE TABLESPACE gives, made where it is not there yet, as Cloudberry's
 * create_tablespace_directories() makes it.  PostgreSQL asks for it through
 * O32 wherever it links pg_tblspc to a tablespace: as a node runs the
 * statement, which each node is sent with the location it was given, and
 * as a mirror or a standby replays the statement's WAL record, which
 * carries that location too -- so each node makes a directory of its own,
 * whichever node wrote the record.  A location that is not there is left
 * for PostgreSQL to report, in its words; an in-place tablespace, each
 * node's own already, is not asked about.
 */
static const char *
node_tablespace_location(const char *location, Oid tablespaceoid)
{
	struct stat st;
	char	   *dir;

	if (prev_tablespace_location_hook)
		location = prev_tablespace_location_hook(location, tablespaceoid);
	if (stat(location, &st) < 0 || !S_ISDIR(st.st_mode))
		return location;

	/*
	 * CREATE TABLESPACE checked the location's length for the files under
	 * it, and this is longer by a dbid: the widest one's, so that a node the
	 * record reaches later finds it no longer than its writer did.
	 */
	if (strlen(location) + 1 + DBID_CHARS + 1 +
		strlen(TABLESPACE_VERSION_DIRECTORY) + 1 + OIDCHARS + 1 + OIDCHARS +
		1 + FORKNAMECHARS + 1 + OIDCHARS > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long", location)));

	dir = psprintf("%s/%d", location, GpClusterDbid());
	if (mkdir(dir, S_IRWXU) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));
	return dir;
}

/*
 * The other half of it: as DROP TABLESPACE removes this node's link to a
 * tablespace, the directory node_tablespace_location() made -- the one
 * named for this node's dbid -- goes too, emptied, as Cloudberry's
 * destroy_tablespace_directories() removes it.  PostgreSQL calls it through
 * O32 wherever it removes the link: as a node runs the statement, and as a
 * mirror or a standby replays its record -- so no node keeps its directory.
 * A link to anything else, a tablespace made before gp_core was loaded, is
 * PostgreSQL's alone, and a directory with anything else in it is left;
 * one that cannot be removed is said in a WARNING, or in redo a LOG, where
 * Cloudberry fails the DROP: the tablespace is gone either way.
 */
static void
node_tablespace_location_drop(const char *linkloc, Oid tablespaceoid, bool redo)
{
	char		target[MAXPGPATH];
	char		suffix[32];
	ssize_t		len;

	if (prev_tablespace_location_drop_hook)
		prev_tablespace_location_drop_hook(linkloc, tablespaceoid, redo);

	len = readlink(linkloc, target, sizeof(target) - 1);
	if (len < 0)
		return;					/* the link's own trouble is PostgreSQL's */
	target[len] = '\0';
	snprintf(suffix, sizeof(suffix), "/%d", GpClusterDbid());
	if (len <= strlen(suffix) || strcmp(target + len - strlen(suffix), suffix) != 0)
		return;
	if (rmdir(target) < 0 && errno != ENOENT && errno != ENOTEMPTY &&
		errno != EEXIST)
		ereport(redo ? LOG : WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m", target)));
}

/*
 * Run a tablespace's statement here: CREATE TABLESPACE in this node's
 * directory, and, on a segment, an in-place one as the coordinator allowed
 * it.
 */
static void
run_tablespace_statement(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc,
						 bool dispatched)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, CreateTableSpaceStmt))
	{
		CreateTableSpaceStmt *stmt = (CreateTableSpaceStmt *) parsetree;
		int			nestlevel = -1;

		if (dispatched && stmt->location != NULL && stmt->location[0] == '\0')
		{
			nestlevel = NewGUCNestLevel();
			(void) set_config_option("allow_in_place_tablespaces", "on",
									 PGC_SUSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);
		}
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
		if (nestlevel >= 0)
			AtEOXact_GUC(true, nestlevel);
	}
	else
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
}

/* A statement's arguments, for a callback that runs it (GpRefreshRunCopy). */
typedef struct RunArgs
{
	PlannedStmt *pstmt;
	const char *queryString;
	bool		readOnlyTree;
	ProcessUtilityContext context;
	ParamListInfo params;
	QueryEnvironment *queryEnv;
	DestReceiver *dest;
	QueryCompletion *qc;
} RunArgs;

static void
run_copy(void *arg)
{
	RunArgs    *a = (RunArgs *) arg;

	run_tablespace_statement(a->pstmt, a->queryString, a->readOnlyTree,
							 a->context, a->params, a->queryEnv, a->dest,
							 a->qc, false);
}

/* ------------------------------------------------------------------------- */
/* The hooks                                                                 */
/* ------------------------------------------------------------------------- */

static void
next_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/*
 * The end of the first line of what the segments are sent, and the client's
 * statement after it: what pg_stat_activity shows of it here, as much of it
 * as a segment's shows, and what a segment's log names as its statement
 * (gp_log.c) -- sent whether activities are tracked or not.
 */
static void
payload_text(StringInfo buf)
{
	int			len = 0;

	if (debug_query_string != NULL)
		len = pg_mbcliplen(debug_query_string, strlen(debug_query_string),
						   pgstat_track_activity_query_size - 1);
	if (len > 0)
		appendStringInfo(buf, " text=%d", len);
	appendStringInfoChar(buf, '\n');
	if (len > 0)
		appendBinaryStringInfo(buf, debug_query_string, len);
}

/*
 * What the segments are sent: the marker, the OIDs, the client's statement
 * and the tree.
 */
static char *
build_payload(const char *tree)
{
	StringInfoData buf;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&buf);
	appendStringInfoString(&buf, GP_TREE_MARKER "oids=");
	foreach(lc, recorded)
	{
		GpOidAssignment *a = (GpOidAssignment *) lfirst(lc);

		appendStringInfo(&buf, "%s%u:%u", first ? "" : ",", a->catalog, a->oid);
		first = false;
	}
	payload_text(&buf);
	appendStringInfoString(&buf, tree);

	return buf.data;
}

/*
 * A label as the coordinator has it, as the SECURITY LABEL statement a
 * segment is sent to write it -- a parse tree, as every dispatched statement
 * is, so that it needs nothing installed there and is checked there as
 * SECURITY LABEL checks one: the provider's own check, and that the user
 * owns the object.  NULL for an object that is gone, or of a kind no label
 * is sent for.  "gp"'s, or another provider's.
 */
char *
GpDdlLabelPayload(const ObjectAddress *object, const char *label)
{
	return GpDdlLabelPayloadOf(object, GP_LABEL_PROVIDER, label);
}

char *
GpDdlLabelPayloadOf(const ObjectAddress *object, const char *provider,
					const char *label)
{
	SecLabelStmt *stmt;
	List	   *objname = NIL;
	List	   *objargs = NIL;
	List	   *names = NIL;
	ListCell   *lc;

	if (getObjectIdentityParts(object, &objname, &objargs, true) == NULL)
		return NULL;
	foreach(lc, objname)
		names = lappend(names, makeString((char *) lfirst(lc)));

	stmt = makeNode(SecLabelStmt);
	stmt->objtype = object->objectSubId != 0 ? OBJECT_COLUMN
		: get_object_type(object->classId, object->objectId);
	switch (stmt->objtype)
	{
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_COLUMN:
			stmt->object = (Node *) names;
			break;
		case OBJECT_SCHEMA:
			stmt->object = (Node *) linitial(names);
			break;
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_AGGREGATE:
			{
				ObjectWithArgs *owa = makeNode(ObjectWithArgs);

				owa->objname = names;
				foreach(lc, objargs)
					owa->objargs = lappend(owa->objargs,
										   typeStringToTypeName((char *) lfirst(lc),
																NULL));
				stmt->object = (Node *) owa;
				break;
			}
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			/* a type's identity is one string, its name as SQL writes it */
			stmt->object = (Node *) typeStringToTypeName((char *) linitial(objname),
														 NULL);
			break;
		default:
			return NULL;
	}
	stmt->provider = pstrdup(provider);
	stmt->label = label != NULL ? pstrdup(label) : NULL;

	{
		StringInfoData buf;

		initStringInfo(&buf);
		appendStringInfoString(&buf, GP_TREE_MARKER "oids=");
		payload_text(&buf);
		appendStringInfoString(&buf, nodeToString(stmt));
		return buf.data;
	}
}

/*
 * What a segment is sent for a CREATE TABLE AS: the CREATE TABLE PostgreSQL
 * made of it here (createas.c, create_ctas_internal), from the table as it
 * now is.  The statement itself cannot travel: by now its query has been
 * analyzed, and a segment would analyze it again.  NULL when it made nothing
 * -- IF NOT EXISTS, and the table was there.
 */
static char *
ctas_as_create(CreateTableAsStmt *ctas)
{
	IntoClause *into = ctas->into;
	CreateStmt *create;
	RangeVar   *rv;
	Relation	rel;
	TupleDesc	tupdesc;
	Oid			relid;

	if (recorded == NIL)
		return NULL;
	relid = RangeVarGetRelid(into->rel, NoLock, false);

	rv = copyObject(into->rel);
	if (rv->relpersistence != RELPERSISTENCE_TEMP)
		rv->schemaname = get_namespace_name(get_rel_namespace(relid));

	create = makeNode(CreateStmt);
	create->relation = rv;
	create->options = into->options;
	create->oncommit = into->onCommit;
	create->tablespacename = into->tableSpaceName;
	create->accessMethod = into->accessMethod;
	create->if_not_exists = false;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		create->tableElts = lappend(create->tableElts,
									makeColumnDef(NameStr(att->attname),
												  att->atttypid,
												  att->atttypmod,
												  att->attcollation));
	}
	relation_close(rel, AccessShareLock);

	return nodeToString(create);
}

/*
 * What a segment is sent for a CREATE MATERIALIZED VIEW: the statement
 * itself, WITH NO DATA -- a view's rule is its query, so the segment has to
 * make the view from one -- its query printed here as ruleutils prints a
 * view's, and parsed again, to be analyzed there against the same catalogs,
 * in the same search_path, as it was here.  Its rows come after, by REFRESH
 * (gp_refresh.c).  NULL when it made nothing.
 */
static char *
matview_as_create(CreateTableAsStmt *ctas)
{
	CreateTableAsStmt *create;
	RawStmt    *raw;
	Oid			relid;

	if (recorded == NIL)
		return NULL;
	relid = RangeVarGetRelid(ctas->into->rel, NoLock, false);

	raw = linitial_node(RawStmt,
						raw_parser(pg_get_querydef(castNode(Query, ctas->query), false),
								   RAW_PARSE_DEFAULT));
	create = makeNode(CreateTableAsStmt);
	create->query = raw->stmt;
	create->into = copyObject(ctas->into);
	create->into->viewQuery = NULL;
	create->into->skipData = true;
	if (create->into->rel->relpersistence != RELPERSISTENCE_TEMP)
		create->into->rel->schemaname = get_namespace_name(get_rel_namespace(relid));
	create->objtype = OBJECT_MATVIEW;
	create->is_select_into = false;
	create->if_not_exists = false;
	return nodeToString(create);
}

/*
 * After CREATE INDEX: indcheckxmin here where a segment set it, as
 * Cloudberry's cdb_sync_indcheckxmin_with_segments() sets it (indexcmds.c).
 * An index built over a heap with HOT chains the build found broken is not
 * for snapshots older than its own transaction, and says so in indcheckxmin;
 * the coordinator plans for the segments, and holds none of a distributed
 * table's rows to find such a chain in.  So each index the statement made
 * whose indcheckxmin is off here is asked about, all in one query to each
 * segment.  That query reads pg_index there under a lock the segment keeps
 * to the end of the transaction, so the coordinator takes the same lock
 * first and keeps it too, as Cloudberry's does: a VACUUM FULL of pg_index
 * then waits for this transaction here, rather than deadlocking with it
 * between here and a segment.
 */
static void
sync_indcheckxmin(List *assigned)
{
	StringInfoData oids;
	ListCell   *lc;
	char	  **values;
	int			nsegs;

	initStringInfo(&oids);
	foreach(lc, assigned)
	{
		GpOidAssignment *a = (GpOidAssignment *) lfirst(lc);
		HeapTuple	tup;

		if (a->catalog != RelationRelationId ||
			get_rel_relkind(a->oid) != RELKIND_INDEX)
			continue;
		tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(a->oid));
		if (!HeapTupleIsValid(tup))
			continue;
		if (!((Form_pg_index) GETSTRUCT(tup))->indcheckxmin)
			appendStringInfo(&oids, "%s%u", oids.len > 0 ? "," : "", a->oid);
		ReleaseSysCache(tup);
	}
	if (oids.len == 0)
		return;

	LockRelationOid(IndexRelationId, AccessShareLock);

	(void) GpClusterSegments(&nsegs);
	values = palloc0_array(char *, Max(nsegs, 1));
	GpDispatchQueryFirstValues(psprintf("SELECT pg_catalog.string_agg(indexrelid::pg_catalog.text, ',')"
										"  FROM pg_catalog.pg_index"
										" WHERE indcheckxmin AND indexrelid IN (%s)",
										oids.data),
							   -1, values);

	for (int i = 0; i < nsegs; i++)
	{
		char	   *list = values[i];
		char	   *tok;
		char	   *save = NULL;

		if (list == NULL)
			continue;
		for (tok = strtok_r(list, ",", &save); tok != NULL;
			 tok = strtok_r(NULL, ",", &save))
		{
			Oid			indexoid = (Oid) strtoul(tok, NULL, 10);
			Relation	pg_index = table_open(IndexRelationId, RowExclusiveLock);
			HeapTuple	tup = SearchSysCacheCopy1(INDEXRELID,
												  ObjectIdGetDatum(indexoid));

			if (HeapTupleIsValid(tup) &&
				!((Form_pg_index) GETSTRUCT(tup))->indcheckxmin)
			{
				((Form_pg_index) GETSTRUCT(tup))->indcheckxmin = true;
				CatalogTupleUpdate(pg_index, &tup->t_self, tup);
				CommandCounterIncrement();
			}
			if (HeapTupleIsValid(tup))
				heap_freetuple(tup);
			table_close(pg_index, RowExclusiveLock);
		}
	}
}

static void
gp_ddl_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	GpDispatchClass class;
	char	   *tree;

	/*
	 * Cloudberry's fault at the start of CreateFunction(), on whichever node
	 * runs it: a CREATE FUNCTION of an extension's script too, which a
	 * segment runs as it runs the CREATE EXTENSION it was sent.
	 */
	if (IsA(parsetree, CreateFunctionStmt))
		(void) GP_FAULT("create_function_fail");

	/*
	 * A segment, running what the coordinator sent.  It is run as the
	 * coordinator ran it, and then every OID the coordinator sent has to have
	 * been used: fewer means the segment made fewer objects, which is as much
	 * a divergence as making different ones.
	 */
	if (preassigning && parsetree == dispatched_tree)
	{
		PG_TRY();
		{
			run_tablespace_statement(pstmt, queryString, readOnlyTree, context,
									 params, queryEnv, dest, qc, true);

			if (next_preassigned < npreassigned)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("segment used %d of the %d OIDs the coordinator allocated for this statement",
								next_preassigned, npreassigned)));
		}
		PG_FINALLY();
		{
			preassigned_clear();
			MemoryContextReset(ddl_cxt);
		}
		PG_END_TRY();
		return;
	}

	/*
	 * Only the coordinator dispatches, and a statement run while another one
	 * is being recorded is part of that one: the segments run the outer
	 * statement, and do what it does inside, themselves.  A segment's COPY
	 * that brings a materialized view its rows runs as gp_refresh.c says.
	 */
	if (recording || GpClusterBackendRole() != GP_ROLE_DISPATCH)
	{
		RangeVar   *staging;

		if (GpClusterBackendRole() == GP_ROLE_EXECUTE &&
			IsA(parsetree, CopyStmt) &&
			(staging = GpRefreshFillTarget((CopyStmt *) parsetree)) != NULL)
		{
			PlannedStmt *copy = copyObject(pstmt);
			RunArgs		args = {copy, queryString, false, context,
			params, queryEnv, dest, qc};

			castNode(CopyStmt, copy->utilityStmt)->relation = staging;
			GpRefreshRunCopy(run_copy, &args);
			return;
		}
		run_tablespace_statement(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc, false);
		return;
	}

	/* REFRESH of a materialized view on the segments, with its rows */
	if (IsA(parsetree, RefreshMatViewStmt) &&
		GpRefreshNeedsFill((RefreshMatViewStmt *) parsetree))
	{
		GpRefreshMatView(pstmt, queryString, context, params, queryEnv, qc);
		return;
	}

	class = dispatch_class(parsetree);
	if (class == GP_DISPATCH_LOCAL)
	{
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);

		/* ANALYZE: the all-visible pages are the segments' (gp_analyze.c) */
		if (IsA(parsetree, VacuumStmt))
			GpAnalyzeSegmentCounts((VacuumStmt *) parsetree);
		return;
	}

	/* Before running it: PostgreSQL may change a tree it is given. */
	tree = nodeToString(parsetree);

	MemoryContextReset(ddl_cxt);
	recorded = NIL;
	recording = true;
	recording_vacuum = IsA(parsetree, VacuumStmt);
	PG_TRY();
	{
		run_tablespace_statement(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc, false);
	}
	PG_FINALLY();
	{
		recording = false;
		recording_vacuum = false;
	}
	PG_END_TRY();

	empty_script_tables(parsetree);

	/*
	 * It ran here, so it will run there: most of what could make it fail --
	 * the syntax, a name taken, a privilege missing -- has been checked on
	 * the coordinator already.  What the segments say goes into this
	 * transaction, which they share (see gp_dispatch.c), so a failure there
	 * undoes it here too.
	 */
	if (IsA(parsetree, CreateTableAsStmt))
	{
		tree = ((CreateTableAsStmt *) parsetree)->objtype == OBJECT_MATVIEW
			? matview_as_create((CreateTableAsStmt *) parsetree)
			: ctas_as_create((CreateTableAsStmt *) parsetree);
		if (tree == NULL)
		{
			recorded = NIL;
			MemoryContextReset(ddl_cxt);
			return;
		}
	}

	drop_temp_namespaces();
	GpDispatchUtility(build_payload(tree), class == GP_DISPATCH_OWN_XACT);
	check_extension_everywhere(parsetree);
	if (IsA(parsetree, CreatedbStmt))
		create_core_extension(((CreatedbStmt *) parsetree)->dbname);

	if (IsA(parsetree, IndexStmt) && !((IndexStmt *) parsetree)->concurrent)
		sync_indcheckxmin(recorded);

	/* VACUUM: what it counted is the segments' (gp_analyze.c) */
	if (IsA(parsetree, VacuumStmt))
		GpAnalyzeSegmentCounts((VacuumStmt *) parsetree);

	recorded = NIL;
	MemoryContextReset(ddl_cxt);
}

/*
 * raw_parser_hook: on a segment, a dispatched statement is already parsed.
 */
static List *
gp_ddl_raw_parser(const char *str, RawParseMode mode)
{
	if (mode == RAW_PARSE_DEFAULT && GpDispatchIsTreeText(str) &&
		GpClusterIsDispatched())
	{
		const char *p = str + strlen(GP_TREE_MARKER);
		const char *nl;
		const char *oids_end;
		const char *tree;
		RawStmt    *raw;
		Node	   *stmt;
		MemoryContext oldcxt;
		int			n = 0;

		if (strncmp(p, "oids=", 5) != 0 || (nl = strchr(p, '\n')) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed dispatched statement")));
		p += 5;

		/* the client's statement, which this backend shows while it runs */
		tree = nl + 1;
		oids_end = memchr(p, ' ', nl - p);
		if (oids_end == NULL)
			oids_end = nl;
		else
		{
			char	   *end;
			long		len;

			if (strncmp(oids_end, " text=", 6) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed dispatched statement")));
			len = strtol(oids_end + 6, &end, 10);
			if (end != nl || len < 0 || len > (long) strlen(tree))
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed dispatched statement")));
			pgstat_report_activity(STATE_RUNNING, pnstrdup(tree, len));
			tree += len;
		}

		preassigned_clear();
		MemoryContextReset(ddl_cxt);

		/* The OIDs outlive this parse: they are used when the statement runs. */
		oldcxt = MemoryContextSwitchTo(ddl_cxt);
		for (const char *q = p; q < oids_end; q++)
			if (*q == ':')
				n++;
		preassigned = n > 0 ? palloc_array(GpOidAssignment, n) : NULL;
		MemoryContextSwitchTo(oldcxt);

		while (p < oids_end)
		{
			char	   *end;
			unsigned long catalog;
			unsigned long oid;

			catalog = strtoul(p, &end, 10);
			if (*end != ':')
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OID list in dispatched statement")));
			oid = strtoul(end + 1, &end, 10);
			if (*end != ',' && end != oids_end)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OID list in dispatched statement")));
			preassigned[npreassigned].catalog = (Oid) catalog;
			preassigned[npreassigned].oid = (Oid) oid;
			npreassigned++;
			p = (*end == ',') ? end + 1 : end;
		}

		stmt = (Node *) stringToNode(tree);

		raw = makeNode(RawStmt);
		raw->stmt = stmt;
		raw->stmt_location = 0;
		raw->stmt_len = 0;

		dispatched_tree = stmt;
		preassigning = true;

		return list_make1(raw);
	}

	if (prev_raw_parser)
		return prev_raw_parser(str, mode);
	return standard_raw_parser(str, mode);
}

/*
 * A statement that fails leaves its OIDs behind; the next one must not start
 * with them.  Only an abort: a commit may be one of the several a single
 * VACUUM or CREATE INDEX CONCURRENTLY makes, with the statement still going.
 */
static void
gp_ddl_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		preassigned_clear();
		recording = false;
		recording_vacuum = false;
		recorded = NIL;
		MemoryContextReset(ddl_cxt);
	}
}

bool
GpDispatchIsTreeText(const char *str)
{
	return strncmp(str, GP_TREE_MARKER, strlen(GP_TREE_MARKER)) == 0;
}

/*
 * The client's statement a dispatched statement's text carries, the n bytes
 * after its first line's " text=<n>" (payload_text()), into *len; NULL where
 * it carries none.  Its first line has no space before that.
 */
const char *
GpDispatchTreeStatement(const char *str, int *len)
{
	const char *nl;
	const char *p;
	char	   *end;
	long		n;

	if (!GpDispatchIsTreeText(str) || (nl = strchr(str, '\n')) == NULL ||
		(p = memchr(str, ' ', nl - str)) == NULL || strncmp(p, " text=", 6) != 0)
		return NULL;
	n = strtol(p + 6, &end, 10);
	if (end != nl || n <= 0 || n > (long) strlen(nl + 1))
		return NULL;
	*len = (int) n;
	return nl + 1;
}

bool
GpDispatchIsDispatchedStatement(Node *utilityStmt)
{
	return dispatched_tree != NULL && utilityStmt == dispatched_tree;
}

/*
 * A VACUUM is dispatched whole too -- a VACUUM FULL's new files have to be
 * recorded -- but nothing it runs inside is a query of the kind: what its
 * ANALYZE samples is the segments' rows, the coordinator's statistics of a
 * distributed table (gp_analyze.c), as a plain ANALYZE's are.
 */
bool
GpDispatchIsRecording(void)
{
	return recording && !recording_vacuum;
}

bool
GpDispatchIsRunningDispatched(void)
{
	return preassigning;
}

void
GpDdlInit(void)
{
	/*
	 * A single node dispatches nothing, and its server stays what it was:
	 * none of these hooks is installed on one, so the singlenode suites see
	 * exactly the server they saw before M2.
	 */
	if (GpClusterIsSingleNode())
		return;

	ddl_cxt = AllocSetContextCreate(TopMemoryContext, "gp_core DDL dispatch",
									ALLOCSET_SMALL_SIZES);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_ddl_ProcessUtility;

	prev_raw_parser = raw_parser_hook;
	raw_parser_hook = gp_ddl_raw_parser;

	prev_new_oid_hook = new_oid_hook;
	new_oid_hook = new_oid;

	prev_tablespace_location_hook = tablespace_location_hook;
	tablespace_location_hook = node_tablespace_location;
	prev_tablespace_location_drop_hook = tablespace_location_drop_hook;
	tablespace_location_drop_hook = node_tablespace_location_drop;

	RegisterXactCallback(gp_ddl_xact_callback, NULL);
}
