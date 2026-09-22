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

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/readfuncs.h"
#include "parser/parser.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"

/*
 * What a dispatched statement's text starts with.  Only a dispatched backend
 * looks for it, so a user who types it gets PostgreSQL's syntax error.
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

/*
 * Where the OIDs are kept, on either side.  Not a transaction's context: VACUUM
 * FULL, CREATE INDEX CONCURRENTLY and REINDEX of a database commit as they go,
 * and the list has to span the transactions of one statement.
 */
static MemoryContext ddl_cxt = NULL;

/* The coordinator: the statement being run here and dispatched after. */
static bool recording = false;
static List *recorded = NIL;	/* of GpOidAssignment *, in ddl_cxt */

/* A segment: the statement the coordinator dispatched, and its OIDs. */
static Node *dispatched_tree = NULL;
static GpOidAssignment *preassigned = NULL;
static int	npreassigned = 0;
static int	next_preassigned = 0;
static bool preassigning = false;

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
			 * from its own counter.  A temporary namespace is never named by
			 * OID in anything dispatched, so nothing is lost.
			 */
			if (catalog == NamespaceRelationId)
				return InvalidOid;

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

	return prev_new_oid_hook ? prev_new_oid_hook(relation, indexId, oidcolumn)
		: InvalidOid;
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
 *   - CREATE TABLE AS, SELECT INTO and REFRESH MATERIALIZED VIEW, which carry
 *     rows: a table filled from a query is the coordinator's until the
 *     distributed INSERT exists to fill it everywhere, and it has no
 *     distribution label, so everything that reads it reads it here;
 *   - tablespaces: a tablespace is a directory on each machine, and nodes that
 *     share a machine would share the directory, which PostgreSQL's layout
 *     under it has no room to tell apart (Cloudberry puts the dbid in the
 *     path, in code the port cannot change);
 *   - publications, subscriptions and event triggers, which are about this
 *     node's own WAL and this node's own DDL.
 *
 * VACUUM, REINDEX and CLUSTER are read-only by PostgreSQL's definition -- they
 * change nothing pg_dump would show -- but they have to reach the rows, and
 * the rows are on the segments.  ANALYZE stays here until O3 brings the
 * segments' samples to it.
 */
static GpDispatchClass
dispatch_class(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreatedbStmt:
		case T_DropdbStmt:
			return GP_DISPATCH_OWN_XACT;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->is_vacuumcmd)
				return GP_DISPATCH_OWN_XACT;
			return GP_DISPATCH_LOCAL;

		case T_IndexStmt:
			return ((IndexStmt *) parsetree)->concurrent
				? GP_DISPATCH_OWN_XACT : GP_DISPATCH_IN_XACT;

		case T_DropStmt:
			return ((DropStmt *) parsetree)->concurrent
				? GP_DISPATCH_OWN_XACT : GP_DISPATCH_IN_XACT;

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

				/* REINDEX DATABASE and SYSTEM commit as they go. */
				if (stmt->kind == REINDEX_OBJECT_DATABASE ||
					stmt->kind == REINDEX_OBJECT_SYSTEM)
					return GP_DISPATCH_OWN_XACT;
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
		 * A table made from a query WITH NO DATA is made on every node, as
		 * the CREATE TABLE PostgreSQL makes of it (ctas_as_create); gp_sql
		 * turns every CREATE TABLE AS on a cluster into one, and fills the
		 * table with an INSERT, which puts each row where it belongs.
		 */
		case T_CreateTableAsStmt:
			{
				CreateTableAsStmt *ctas = (CreateTableAsStmt *) parsetree;

				if (ctas->objtype == OBJECT_TABLE && ctas->into->skipData)
					return GP_DISPATCH_IN_XACT;
				return GP_DISPATCH_LOCAL;
			}

		case T_RefreshMatViewStmt:
		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableMoveAllStmt:
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
		case T_AlterOwnerStmt:
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
		case T_CommentStmt:
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
		case T_RenameStmt:
		case T_RuleStmt:
		case T_SecLabelStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
		case T_RepackStmt:
			return GP_DISPATCH_IN_XACT;

		default:
			return GP_DISPATCH_LOCAL;
	}
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
 * What the segments are sent: the marker, the OIDs, and the tree.
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
	appendStringInfoChar(&buf, '\n');
	appendStringInfoString(&buf, tree);

	return buf.data;
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
	 * A segment, running what the coordinator sent.  It is run as the
	 * coordinator ran it, and then every OID the coordinator sent has to have
	 * been used: fewer means the segment made fewer objects, which is as much
	 * a divergence as making different ones.
	 */
	if (preassigning && parsetree == dispatched_tree)
	{
		PG_TRY();
		{
			next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

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
	 * statement, and do what it does inside, themselves.
	 */
	if (recording || GpClusterBackendRole() != GP_ROLE_DISPATCH)
	{
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
		return;
	}

	class = dispatch_class(parsetree);
	if (class == GP_DISPATCH_LOCAL)
	{
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
		return;
	}

	/* Before running it: PostgreSQL may change a tree it is given. */
	tree = nodeToString(parsetree);

	MemoryContextReset(ddl_cxt);
	recorded = NIL;
	recording = true;
	PG_TRY();
	{
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		recording = false;
	}
	PG_END_TRY();

	/*
	 * It ran here, so it will run there: most of what could make it fail --
	 * the syntax, a name taken, a privilege missing -- has been checked on
	 * the coordinator already.  What the segments say goes into this
	 * transaction, which they share (see gp_dispatch.c), so a failure there
	 * undoes it here too.
	 */
	if (IsA(parsetree, CreateTableAsStmt))
	{
		tree = ctas_as_create((CreateTableAsStmt *) parsetree);
		if (tree == NULL)
		{
			recorded = NIL;
			MemoryContextReset(ddl_cxt);
			return;
		}
	}

	drop_temp_namespaces();
	GpDispatchUtility(build_payload(tree), class == GP_DISPATCH_OWN_XACT);

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
		RawStmt    *raw;
		Node	   *stmt;
		MemoryContext oldcxt;
		int			n = 0;

		if (strncmp(p, "oids=", 5) != 0 || (nl = strchr(p, '\n')) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("malformed dispatched statement")));
		p += 5;

		preassigned_clear();
		MemoryContextReset(ddl_cxt);

		/* The OIDs outlive this parse: they are used when the statement runs. */
		oldcxt = MemoryContextSwitchTo(ddl_cxt);
		for (const char *q = p; q < nl; q++)
			if (*q == ':')
				n++;
		preassigned = n > 0 ? palloc_array(GpOidAssignment, n) : NULL;
		MemoryContextSwitchTo(oldcxt);

		while (p < nl)
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
			if (*end != ',' && end != nl)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("malformed OID list in dispatched statement")));
			preassigned[npreassigned].catalog = (Oid) catalog;
			preassigned[npreassigned].oid = (Oid) oid;
			npreassigned++;
			p = (*end == ',') ? end + 1 : end;
		}

		stmt = (Node *) stringToNode(nl + 1);

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
		recorded = NIL;
		MemoryContextReset(ddl_cxt);
	}
}

bool
GpDispatchIsTreeText(const char *str)
{
	return strncmp(str, GP_TREE_MARKER, strlen(GP_TREE_MARKER)) == 0;
}

bool
GpDispatchIsDispatchedStatement(Node *utilityStmt)
{
	return dispatched_tree != NULL && utilityStmt == dispatched_tree;
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

	RegisterXactCallback(gp_ddl_xact_callback, NULL);
}
