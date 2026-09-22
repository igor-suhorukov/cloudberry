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
 * gp_sql.c
 *	  Cloudberry-only SQL surface: tags, directory tables, storage servers.
 *
 * Cloudberry's DDL keeps working through O26, whose grammar emits only PG19
 * parse nodes: namespaced options, security labels and function calls
 * (decision 11), one statement for each of the user's.  This module handles
 * those forms -- it takes the options, and what a statement carries on its
 * parse node, out again in ProcessUtility_hook and applies them once the
 * statement has run, registers the label provider, and offers the procedures
 * the grammar desugars a statement to.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/commands/tag.c, dirtablecmds.c, storagecmds.c,
 *	  tablecmds_gp.c, storage/file/ufile.c, parser/parse_partition_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_grammar.h"
#include "gp_label.h"
#include "gp_partition.h"
#include "gp_sql.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_sql",
					.version = GP_VERSION
);

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/*
 * What a statement the hook is watching has made, while it runs: the class
 * and OID of each object, in the order they were made (gp_sql_object_access).
 * A statement can run another inside it -- CREATE SCHEMA runs its elements --
 * so arming saves what was there and disarming puts it back.  The lists are
 * in TopTransactionContext, since the hook that adds to them runs in whatever
 * context the statement is in at the time.
 */
static bool pending_armed = false;
static List *pending_classes = NIL;
static List *pending_objects = NIL;

static void check_distribution_policy(const char *policy);

void
GpSqlPendingArm(GpSqlPending *save)
{
	save->armed = pending_armed;
	save->classes = pending_classes;
	save->objects = pending_objects;
	pending_armed = true;
	pending_classes = NIL;
	pending_objects = NIL;
}

void
GpSqlPendingRestore(const GpSqlPending *save)
{
	pending_armed = save->armed;
	pending_classes = save->classes;
	pending_objects = save->objects;
}

/* The first object of this class the statement made, or InvalidOid. */
Oid
GpSqlPendingFirst(Oid classId)
{
	ListCell   *c;
	ListCell   *o;

	forboth(c, pending_classes, o, pending_objects)
	{
		if (lfirst_oid(c) == classId)
			return lfirst_oid(o);
	}
	return InvalidOid;
}

/* Did the statement make this object? */
static bool
pending_has(Oid classId, Oid objectId)
{
	ListCell   *c;
	ListCell   *o;

	forboth(c, pending_classes, o, pending_objects)
	{
		if (lfirst_oid(c) == classId && lfirst_oid(o) == objectId)
			return true;
	}
	return false;
}

/* The ProcessUtility hook after this one, or PostgreSQL's own. */
void
GpSqlProcessUtilityNext(PlannedStmt *pstmt, const char *queryString,
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

/* ------------------------------------------------------------------------- */
/* Where the shorthand may be written                                        */
/* ------------------------------------------------------------------------- */

/*
 * The option list of a statement that creates a relation, or NULL for a
 * statement that has none.  These are the four statements PG19 lets an
 * unknown namespace through: a namespaced reloption is rejected in
 * transformRelOptions, which runs after this hook.  A foreign table's are in
 * its OPTIONS list instead, which has no namespaces (fdw_options_of).
 */
static List **
create_options_of(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			return &((CreateStmt *) parsetree)->options;
		case T_ViewStmt:
			return &((ViewStmt *) parsetree)->options;
		case T_IndexStmt:
			return &((IndexStmt *) parsetree)->options;
		case T_CreateTableAsStmt:
			{
				CreateTableAsStmt *ctas = (CreateTableAsStmt *) parsetree;

				if (ctas->into != NULL)
					return &ctas->into->options;
				return NULL;
			}
		default:
			return NULL;
	}
}

/*
 * CREATE FOREIGN TABLE ... OPTIONS ("gp_tag.env" 'prod', "gp.distributed_by"
 * '(a)'): where the TAG and DISTRIBUTED BY of a foreign table go
 * (gp_desugar.c, rw_add_fdw_option).  A generic option's name is one
 * identifier, so they are told by that name's prefix rather than a namespace,
 * and taken out before the wrapper's validator would refuse them.
 */
static List **
fdw_options_of(Node *parsetree)
{
	if (IsA(parsetree, CreateForeignTableStmt))
		return &((CreateForeignTableStmt *) parsetree)->options;
	return NULL;
}

static bool
has_prefixed_option(List *options, const char *prefix)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strncmp(def->defname, prefix, strlen(prefix)) == 0)
			return true;
	}

	return false;
}

/*
 * Does this statement carry the shorthand at all?  Asked before the parse
 * tree is copied, so that a statement without it is not copied for nothing.
 */
static bool
has_tag_options(List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, GP_TAG_OPTION_NS) == 0)
			return true;
	}

	return false;
}

/* The same for the port's own namespace, which tablespaces use. */
static bool
has_gp_options(List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, GP_OPTION_NS) == 0)
			return true;
	}

	return false;
}

/*
 * One of the port's own options that gp_sql takes: WITH (gp.distributed_by
 * = '(a,b)'), which is what DISTRIBUTED BY becomes, or WITH
 * (gp.directory_table = true), which is what CREATE DIRECTORY TABLE does.
 * Other options of the namespace are other modules' to take.
 */
static bool
is_gp_option(DefElem *def, const char *name)
{
	return def->defnamespace != NULL &&
		strcmp(def->defnamespace, GP_OPTION_NS) == 0 &&
		strcmp(def->defname, name) == 0;
}

static bool
has_gp_option(List *options, const char *name)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		if (is_gp_option((DefElem *) lfirst(lc), name))
			return true;
	}

	return false;
}

/*
 * Take one of them out of the option list, before the statement would refuse
 * it.  With prefixed, it is a foreign table's "gp.distributed_by".
 */
static DefElem *
take_gp_option(List **options, const char *name, bool prefixed)
{
	DefElem    *found = NULL;
	char	   *fullname = psprintf("%s.%s", GP_OPTION_NS, name);
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (prefixed ? strcmp(def->defname, fullname) != 0 : !is_gp_option(def, name))
			continue;
		if (found != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant options"),
					 errdetail("%s is given more than once.", fullname)));
		found = def;
		*options = foreach_delete_current(*options, lc);
	}

	return found;
}

/* The same, over the SET/RESET subcommands of an ALTER TABLE. */
static bool
alter_has_tag_options(AlterTableStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

		if ((cmd->subtype == AT_SetRelOptions || cmd->subtype == AT_ResetRelOptions) &&
			has_tag_options((List *) cmd->def))
			return true;
	}

	return false;
}

/*
 * Take the tags out of an ALTER TABLE ... SET (...) / RESET (...).
 *
 * SET gives a tag its value, RESET takes it away, which is what Cloudberry
 * writes as ALTER ... TAG (...) and UNSET TAG (...).
 */
static List *
alter_take_tags(AlterTableStmt *stmt)
{
	List	   *tags = NIL;
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);
		List	   *opts;

		if (cmd->subtype != AT_SetRelOptions && cmd->subtype != AT_ResetRelOptions)
			continue;

		opts = (List *) cmd->def;
		if (cmd->subtype == AT_ResetRelOptions)
			tags = list_concat(tags, GpTagTakeResetOptions(&opts));
		else
			tags = list_concat(tags, GpTagTakeOptions(&opts));
		cmd->def = (Node *) opts;

		/*
		 * A subcommand whose whole option list was tags would otherwise reach
		 * ALTER TABLE as an empty SET (), which it rejects.
		 */
		if (opts == NIL)
			stmt->cmds = foreach_delete_current(stmt->cmds, lc);
	}

	return tags;
}

/* ------------------------------------------------------------------------- */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * What a statement the hook is watching makes.
 *
 * Only the OIDs are taken here.  index_create fires this hook before the
 * CommandCounterIncrement that makes the new pg_class row visible, so nothing
 * that reads the catalog for it can run yet -- not even to ask what relkind
 * it is.  Which of them the clauses were written on is worked out once the
 * statement is over (created_relation).
 */
static void
gp_sql_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					 int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (subId != 0)
		return;

	if (access == OAT_POST_CREATE && pending_armed)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);

		pending_classes = lappend_oid(pending_classes, classId);
		pending_objects = lappend_oid(pending_objects, objectId);
		MemoryContextSwitchTo(oldcxt);
	}
	else if (access == OAT_DROP && classId == RelationRelationId)
	{
		char		relkind = get_rel_relkind(objectId);

		if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
			GpTagIndexDropped(objectId);
		else if (relkind == RELKIND_RELATION)
			GpDirTableDropped(objectId);
	}
}

/*
 * The rule Cloudberry applies in ExecMain: a directory table's rows are
 * written by its own functions, not by DML.
 */
static void
gp_sql_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	GpDirTableCheckDML(queryDesc);

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * CREATE TABLESPACE ... WITH (gp.server = 's') and ALTER TABLESPACE ... SET,
 * and the tags of either.
 *
 * Cloudberry names a library and a function in two pg_tablespace columns;
 * here the tablespace names a storage server and the handler for one
 * registers itself.  The option is taken out before tablespace_reloptions
 * would reject it, and recorded once the tablespace exists.
 *
 * CREATE TABLESPACE ... TAG (...) is in the WITH list, gp_tag.env = 'prod',
 * because the statement may not run in a transaction block and so cannot be
 * followed by anything that would set them; ALTER TABLESPACE ... TAG (...)
 * and UNSET TAG (...) are ALTER TABLESPACE ... SET (gp_tag.env = 'prod') and
 * RESET (gp_tag.env), which is where the shorthand works on any table.
 */
static void
gp_sql_tablespace_options(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	List	  **options;
	List	   *opts = NIL;
	List	   *tags = NIL;
	bool		creating = IsA(parsetree, CreateTableSpaceStmt);
	const char *spcname;

	if (creating)
		options = &((CreateTableSpaceStmt *) parsetree)->options;
	else
		options = &((AlterTableSpaceOptionsStmt *) parsetree)->options;

	if (has_gp_options(*options) || has_tag_options(*options))
	{
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
			if (creating)
				options = &((CreateTableSpaceStmt *) parsetree)->options;
			else
				options = &((AlterTableSpaceOptionsStmt *) parsetree)->options;
		}
		opts = GpStorageTakeTablespaceOptions(options);

		if (!creating && ((AlterTableSpaceOptionsStmt *) parsetree)->isReset)
			tags = GpTagTakeResetOptions(options);
		else
			tags = GpTagTakeOptions(options);
	}

	/* An undefined tag is refused before the tablespace is made. */
	GpTagCheckAll(tags);

	GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);

	if (creating)
		spcname = ((CreateTableSpaceStmt *) parsetree)->tablespacename;
	else
		spcname = ((AlterTableSpaceOptionsStmt *) parsetree)->tablespacename;

	if (opts != NIL)
		GpStorageApplyToTablespace(spcname, opts);

	if (tags != NIL)
	{
		CommandCounterIncrement();
		GpTagApplyToObject(TableSpaceRelationId,
						   get_tablespace_oid(spcname, false), tags);
	}
}

/*
 * CREATE DATABASE ... TAG (...) and ALTER DATABASE ... TAG (...), which the
 * desugarer writes as options named "gp_tag.<name>" -- = DEFAULT for UNSET
 * TAG -- because neither statement has a namespaced option, and CREATE
 * DATABASE may not run in a transaction block, so cannot be followed by the
 * call that would set them.  They are taken out before createdb() or
 * AlterDatabase() would refuse them, and the database is labelled once the
 * statement has run, in its own transaction.  AlterDatabase() with nothing
 * left to do still checks that the database is the user's.
 */
static void
gp_sql_database_tags(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree;
	List	   *tags;
	const char *dbname;

	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
		readOnlyTree = false;
	}
	parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, CreatedbStmt))
	{
		tags = GpTagTakePrefixedOptions(&((CreatedbStmt *) parsetree)->options);
		dbname = ((CreatedbStmt *) parsetree)->dbname;
	}
	else
	{
		tags = GpTagTakePrefixedOptions(&((AlterDatabaseStmt *) parsetree)->options);
		dbname = ((AlterDatabaseStmt *) parsetree)->dbname;
	}

	/* An undefined tag is refused before the database is made. */
	GpTagCheckAll(tags);

	GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);

	CommandCounterIncrement();
	GpTagApplyToObject(DatabaseRelationId, get_database_oid(dbname, false), tags);
}

/* Does a CREATE or ALTER DATABASE carry the desugarer's tag options? */
static bool
database_has_tags(Node *parsetree)
{
	List	   *options;

	if (IsA(parsetree, CreatedbStmt))
		options = ((CreatedbStmt *) parsetree)->options;
	else if (IsA(parsetree, AlterDatabaseStmt))
		options = ((AlterDatabaseStmt *) parsetree)->options;
	else
		return false;

	return has_prefixed_option(options, GP_TAG_OPTION_NS ".");
}

/*
 * The list a statement's carried tags are on (gp_desugar.c,
 * GpAttachCarriers): TAG on CREATE SCHEMA, CREATE USER and CREATE SEQUENCE,
 * and TAG and UNSET TAG on ALTER USER, whose statements have no list a
 * namespaced option can be written in.  A schema has no options at all, so
 * its tags are among its elements.
 */
static List **
carried_list_of(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateSchemaStmt:
			return &((CreateSchemaStmt *) parsetree)->schemaElts;
		case T_CreateRoleStmt:
			return &((CreateRoleStmt *) parsetree)->options;
		case T_AlterRoleStmt:
			return &((AlterRoleStmt *) parsetree)->options;
		case T_CreateSeqStmt:
			return &((CreateSeqStmt *) parsetree)->options;
		default:
			return NULL;
	}
}

/*
 * The relation a CREATE statement carrying tags or a distribution made, or
 * InvalidOid if it made none -- CREATE TABLE IF NOT EXISTS of one that was
 * there already.  The statement names it, all but CREATE INDEX, which need
 * not, and it is the one of that name among the relations the statement
 * made.  It used to be the first relation the statement made, which for a
 * table with a serial column is the column's sequence: the tags of
 * CREATE TABLE t (id serial) WITH (gp_tag.env = 'prod') went on t_id_seq.
 * CREATE OR REPLACE VIEW of a view that was there already made nothing, and
 * is the view it replaced.  CREATE INDEX makes one index, and child indexes
 * after it on a partitioned table; the first index is the one.
 */
static bool
on_cluster_coordinator(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && !core->is_single_node() &&
		core->get_role() == GP_ROLE_DISPATCH;
}

static Oid
created_relation(Node *parsetree)
{
	RangeVar   *rv;
	Oid			relid;
	ListCell   *c;
	ListCell   *o;

	/* what the statement made, visible */
	CommandCounterIncrement();

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			rv = ((CreateStmt *) parsetree)->relation;
			break;
		case T_CreateForeignTableStmt:
			rv = ((CreateForeignTableStmt *) parsetree)->base.relation;
			break;
		case T_CreateSeqStmt:
			rv = ((CreateSeqStmt *) parsetree)->sequence;
			break;
		case T_CreateTableAsStmt:
			rv = ((CreateTableAsStmt *) parsetree)->into->rel;
			break;
		case T_ViewStmt:
			rv = ((ViewStmt *) parsetree)->view;
			if (((ViewStmt *) parsetree)->replace)
				return RangeVarGetRelid(rv, NoLock, true);
			break;
		case T_IndexStmt:
			forboth(c, pending_classes, o, pending_objects)
			{
				char		relkind;

				if (lfirst_oid(c) != RelationRelationId)
					continue;
				relkind = get_rel_relkind(lfirst_oid(o));
				if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
					return lfirst_oid(o);
			}
			return InvalidOid;
		default:
			return InvalidOid;
	}

	relid = RangeVarGetRelid(rv, NoLock, true);
	return pending_has(RelationRelationId, relid) ? relid : InvalidOid;
}

/*
 * TAG on CREATE SCHEMA, CREATE USER, CREATE SEQUENCE and ALTER USER, carried
 * to the statement's parse node because its grammar has no place for them.
 *
 * They are taken out before PostgreSQL reads the list -- CREATE ROLE would
 * refuse an option it does not know, and CREATE SCHEMA an element that is not
 * a statement -- checked before the statement runs, so that a misspelled tag
 * does not leave a schema behind, and put on the object once it exists.
 *
 * What the tags need is what SECURITY LABEL would need of the object.  An
 * object the statement made is the user's.  ALTER USER with nothing left in
 * it checks next to nothing, so the check SECURITY LABEL makes of a role --
 * CREATEROLE and the ADMIN option on it, or superuser for a superuser -- is
 * made here, before it runs.
 */
static void
gp_sql_carried_tags(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree;
	List	   *tags;
	Oid			roleid = InvalidOid;
	GpSqlPending save;

	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
		readOnlyTree = false;
	}
	parsetree = pstmt->utilityStmt;

	tags = GpTagTakeCarried(carried_list_of(parsetree));
	GpTagCheckAll(tags);

	if (IsA(parsetree, AlterRoleStmt))
	{
		RoleSpec   *role = ((AlterRoleStmt *) parsetree)->role;
		ObjectAddress addr;

		roleid = get_rolespec_oid(role, false);
		ObjectAddressSet(addr, AuthIdRelationId, roleid);
		check_object_ownership(GetUserId(), OBJECT_ROLE, addr,
							   (Node *) makeString(get_rolespec_name(role)), NULL);
	}

	GpSqlPendingArm(&save);
	PG_TRY();
	{
		GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

		CommandCounterIncrement();
		switch (nodeTag(parsetree))
		{
			case T_CreateSchemaStmt:
				{
					/* IF NOT EXISTS of one that was there made nothing */
					Oid			nspid = GpSqlPendingFirst(NamespaceRelationId);

					if (OidIsValid(nspid))
						GpTagApplyToObject(NamespaceRelationId, nspid, tags);
				}
				break;
			case T_CreateRoleStmt:
				GpTagApplyToObject(AuthIdRelationId,
								   GpSqlPendingFirst(AuthIdRelationId), tags);
				break;
			case T_AlterRoleStmt:
				GpTagApplyToObject(AuthIdRelationId, roleid, tags);
				break;
			case T_CreateSeqStmt:
				{
					Oid			relid = created_relation(parsetree);

					if (OidIsValid(relid))
						GpTagApplyToRelation(relid, tags);
				}
				break;
			default:
				break;
		}
	}
	PG_FINALLY();
	{
		GpSqlPendingRestore(&save);
	}
	PG_END_TRY();
}

/* A CREATE TABLE AS on a cluster is being made, and this is it again. */
static bool in_cluster_ctas = false;

static void gp_sql_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								  bool readOnlyTree, ProcessUtilityContext context,
								  ParamListInfo params, QueryEnvironment *queryEnv,
								  DestReceiver *dest, QueryCompletion *qc);

/*
 * CREATE TABLE AS, and SELECT INTO, on a cluster's coordinator: see where it
 * is called.  The rows are the query's, deparsed as ruleutils deparses a
 * view: the query was analyzed here, and the INSERT is analyzed again from
 * its text, against the same catalogs, in the same transaction.
 */
static void
gp_sql_cluster_ctas(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	CreateTableAsStmt *ctas;
	Query	   *query;
	bool		existed;
	bool		fill;
	Oid			relid;
	char	   *sql;
	uint64		processed;

	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
		readOnlyTree = false;
	}
	ctas = (CreateTableAsStmt *) pstmt->utilityStmt;
	query = (Query *) ctas->query;

	existed = OidIsValid(RangeVarGetRelid(ctas->into->rel, NoLock, true));
	fill = !ctas->into->skipData;
	ctas->into->skipData = true;

	in_cluster_ctas = true;
	PG_TRY();
	{
		gp_sql_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							  params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		in_cluster_ctas = false;
	}
	PG_END_TRY();

	/* IF NOT EXISTS, and it was there: nothing was made, nothing to fill */
	if (existed)
		return;

	/* it was not there before, so the one there now is the one it made */
	CommandCounterIncrement();
	relid = RangeVarGetRelid(ctas->into->rel, NoLock, false);

	GpDistributionApplyDefault(NULL, relid);
	if (!fill)
		return;

	sql = psprintf("INSERT INTO %s %s",
				   quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
											  get_rel_name(relid)),
				   pg_get_querydef(query, false));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not fill the table CREATE TABLE AS made");
	processed = SPI_processed;
	SPI_finish();

	if (qc)
		SetQueryCompletion(qc, CMDTAG_SELECT, processed);
}

static void
gp_sql_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	List	  **options;
	List	  **fdw_options;
	List	   *tags = NIL;
	char	   *policy = NULL;
	bool		directory_table = false;
	DefElem    *partition_by = NULL;
	List	   *partition_cmds = NIL;
	bool		is_alter = false;
	List	  **carried;
	GpSqlPending save;

	/*
	 * On a segment, the statement the coordinator dispatched: whatever this
	 * hook does besides it was done on the coordinator, and dispatched on its
	 * own if it was a statement.  See GpDispatchIsDispatchedStatement().
	 */
	if (GpDispatchIsDispatchedStatement(pstmt->utilityStmt))
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		return;
	}

	/*
	 * CREATE TABLE AS on a cluster: the table first, with no rows, on every
	 * node -- gp_core dispatches a CREATE TABLE AS WITH NO DATA -- then
	 * distributed as the statement or the defaults say, and only then
	 * filled, by an INSERT, which puts each row on the segment its key names.
	 */
	if (IsA(parsetree, CreateTableAsStmt) && on_cluster_coordinator() &&
		((CreateTableAsStmt *) parsetree)->objtype == OBJECT_TABLE &&
		IsA(((CreateTableAsStmt *) parsetree)->query, Query) && params == NULL &&
		!in_cluster_ctas)
	{
		gp_sql_cluster_ctas(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
		return;
	}

	if (IsA(parsetree, TruncateStmt))
		GpDirTableCheckTruncate((TruncateStmt *) parsetree);

	if (IsA(parsetree, CreateTableSpaceStmt) ||
		IsA(parsetree, AlterTableSpaceOptionsStmt))
	{
		gp_sql_tablespace_options(pstmt, queryString, readOnlyTree, context,
								  params, queryEnv, dest, qc);
		return;
	}

	if (database_has_tags(parsetree))
	{
		gp_sql_database_tags(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	if (IsA(parsetree, CreateFunctionStmt) || IsA(parsetree, AlterFunctionStmt))
	{
		GpFuncAttrProcessUtility(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
		return;
	}

	/*
	 * GRANT and REVOKE on a partitioned table of Cloudberry's reach its
	 * partitions, as they do in Cloudberry (partition.c).
	 */
	if (IsA(parsetree, GrantStmt))
	{
		List	   *objects = GpPartitionGrantObjects((GrantStmt *) parsetree);

		if (objects != NIL)
		{
			if (readOnlyTree)
			{
				pstmt = copyObject(pstmt);
				parsetree = pstmt->utilityStmt;
				readOnlyTree = false;
			}
			((GrantStmt *) parsetree)->objects = objects;
		}
	}

	/*
	 * ALTER TABLE t RENAME TO t2 renames t's partitions too, t_1_prt_a to
	 * t2_1_prt_a, as Cloudberry's does (partition.c, GpPartitionRenamed).
	 */
	if (IsA(parsetree, RenameStmt) &&
		((RenameStmt *) parsetree)->renameType == OBJECT_TABLE &&
		((RenameStmt *) parsetree)->relation != NULL)
	{
		RenameStmt *rs = (RenameStmt *) parsetree;
		Oid			relid = RangeVarGetRelid(rs->relation, NoLock, true);
		char	   *oldname = OidIsValid(relid) ? get_rel_name(relid) : NULL;

		GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		if (oldname != NULL)
		{
			CommandCounterIncrement();
			GpPartitionRenamed(relid, oldname, rs->newname);
		}
		return;
	}

	carried = carried_list_of(parsetree);
	if (carried != NULL && GpTagHasCarried(*carried))
	{
		gp_sql_carried_tags(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
		return;
	}

	options = create_options_of(parsetree);
	fdw_options = fdw_options_of(parsetree);

	if ((options != NULL &&
		 (has_tag_options(*options) || has_gp_option(*options, "distributed_by") ||
		  has_gp_option(*options, "directory_table") ||
		  has_gp_option(*options, GP_PARTITION_BY_OPTION))) ||
		(fdw_options != NULL &&
		 (has_prefixed_option(*fdw_options, GP_TAG_OPTION_NS ".") ||
		  has_prefixed_option(*fdw_options, GP_OPTION_NS ".distributed_by"))) ||
		(IsA(parsetree, AlterTableStmt) &&
		 (alter_has_tag_options((AlterTableStmt *) parsetree) ||
		  GpPartitionHasCmds((AlterTableStmt *) parsetree))))
	{
		/*
		 * The tree is about to be changed, so it must be ours to change.  A
		 * read-only tree belongs to a cached plan that may be run again.
		 */
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
			options = create_options_of(parsetree);
			fdw_options = fdw_options_of(parsetree);
		}

		if (IsA(parsetree, AlterTableStmt))
		{
			tags = alter_take_tags((AlterTableStmt *) parsetree);
			partition_cmds = GpPartitionTakeCmds((AlterTableStmt *) parsetree);
			is_alter = true;
		}
		else if (fdw_options != NULL)
		{
			DefElem    *def = take_gp_option(fdw_options, "distributed_by", true);

			tags = GpTagTakePrefixedOptions(fdw_options);
			if (def != NULL)
				policy = defGetString(def);
		}
		else
		{
			DefElem    *def;

			tags = GpTagTakeOptions(options);
			def = take_gp_option(options, "distributed_by", false);
			if (def != NULL)
				policy = defGetString(def);
			def = take_gp_option(options, "directory_table", false);
			if (def != NULL)
			{
				if (!IsA(parsetree, CreateStmt))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("only CREATE TABLE makes a directory table")));
				directory_table = (def->arg == NULL || defGetBoolean(def));
			}
			partition_by = take_gp_option(options, GP_PARTITION_BY_OPTION, false);
			if (partition_by != NULL && !IsA(parsetree, CreateStmt))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("only CREATE TABLE takes a partition clause")));

			/*
			 * The table's WITH (appendonly = ...) chooses its storage, which
			 * its partitions then have: a partitioned table has no storage
			 * parameters of its own in PostgreSQL.
			 */
			if (partition_by != NULL)
				((CreateStmt *) parsetree)->accessMethod =
					GpPartitionLegacyAccessMethod(((CreateStmt *) parsetree)->accessMethod,
												  options);
		}
	}

	if (tags == NIL && policy == NULL && !directory_table &&
		partition_by == NULL && partition_cmds == NIL)
	{
		/*
		 * On a cluster, a table nobody distributed is distributed anyway.
		 * Which relation the statement made is known only while the pending
		 * list is armed, so it is armed for this too.
		 */
		if (IsA(parsetree, CreateStmt) && on_cluster_coordinator())
		{
			GpSqlPendingArm(&save);
			PG_TRY();
			{
				Oid			relid;

				GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree,
										context, params, queryEnv, dest, qc);
				relid = created_relation(parsetree);
				if (OidIsValid(relid))
					GpDistributionApplyDefault((CreateStmt *) parsetree, relid);
			}
			PG_FINALLY();
			{
				GpSqlPendingRestore(&save);
			}
			PG_END_TRY();
		}
		else
			GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);

		if (IsA(parsetree, CreateStmt))
			GpPartitionMade((CreateStmt *) parsetree);
		return;
	}

	/*
	 * An undefined tag, or a policy that is not one, is refused before the
	 * statement runs, so that a misspelled one does not leave a table behind.
	 */
	GpTagCheckAll(tags);
	if (policy != NULL)
		check_distribution_policy(policy);

	if (!is_alter)
		GpSqlPendingArm(&save);

	PG_TRY();
	{
		Oid			relid;

		GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

		if (is_alter)
		{
			AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

			relid = RangeVarGetRelid(stmt->relation, NoLock, stmt->missing_ok);
		}
		else
			relid = created_relation(parsetree);

		if (OidIsValid(relid))
		{
			if (policy != NULL)
			{
				ObjectAddress addr;

				/* the statement made it, so the user running it owns it */
				ObjectAddressSet(addr, RelationRelationId, relid);
				GpLabelSet(&addr, GP_LABEL_distributed_by, policy);
			}
			else if (!is_alter && IsA(parsetree, CreateStmt) &&
					 on_cluster_coordinator())
				GpDistributionApplyDefault((CreateStmt *) parsetree, relid);
			GpTagApplyToRelation(relid, tags);
			if (directory_table)
				GpDirTableClaim(relid);

			/* the partitions, once the table is distributed and tagged */
			if (partition_by != NULL)
				GpPartitionCreate(relid, partition_by, queryString, queryEnv);
		}

		if (partition_cmds != NIL)
			GpPartitionAlter((AlterTableStmt *) parsetree, partition_cmds,
							 queryString, queryEnv);
	}
	PG_FINALLY();
	{
		if (!is_alter)
			GpSqlPendingRestore(&save);
	}
	PG_END_TRY();
}

PG_FUNCTION_INFO_V1(gp_sql_set_distribution);
PG_FUNCTION_INFO_V1(gp_sql_distribution);

/*
 * The three shapes a recorded distribution policy may have.
 *
 *	   random			 no key; a row may be on any segment
 *	   replicated		 every segment holds every row
 *	   (a,b)			 hashed on those columns, each quoted where it needs
 *						 to be
 *
 * The parentheses are what tells a one-column list from a policy word.
 * Without them DISTRIBUTED BY (random) recorded "random", exactly as
 * DISTRIBUTED RANDOMLY did, and a reader could not tell a table hashed on a
 * column called "random" from a randomly distributed one.
 *
 * Refused when it is set rather than ignored when it is read, which is the
 * rule the "gp" label already follows for its keys: a value this function
 * does not understand is far more likely to be a caller that has not been
 * told about the parentheses than a message from the future.  The columns
 * themselves are not resolved here -- that is the reader's work, and it has
 * to be done against the relation as it stands when the plan is made, not as
 * it stood when the label was written.
 */
static void
check_distribution_policy(const char *policy)
{
	size_t		len = strlen(policy);

	if (strcmp(policy, "random") == 0 || strcmp(policy, "replicated") == 0)
		return;

	if (len >= 3 && policy[0] == '(' && policy[len - 1] == ')')
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized distribution policy \"%s\"", policy),
			 errhint("Use \"random\", \"replicated\", or a parenthesised "
					 "column list such as \"(a,b)\".")));
}

/*
 * gp_sql.set_distribution(regclass, text)
 *
 * What DISTRIBUTED BY says, recorded on the table.  What reads it is ORCA's
 * relcache translator, which asks every relation what it is distributed by,
 * and the dispatch of M2; on one node every table is on the one node, so
 * recording it is all there is to do here.
 */
Datum
gp_sql_set_distribution(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ObjectAddress addr;

	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));

	ObjectAddressSet(addr, RelationRelationId, relid);

	if (PG_ARGISNULL(1))
		GpLabelSet(&addr, GP_LABEL_distributed_by, NULL);
	else
	{
		char	   *policy = text_to_cstring(PG_GETARG_TEXT_PP(1));

		check_distribution_policy(policy);
		GpLabelSet(&addr, GP_LABEL_distributed_by, policy);
	}

	PG_RETURN_VOID();
}

/* gp_sql.distribution(regclass) -> text */
Datum
gp_sql_distribution(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ObjectAddress addr;
	char	   *policy;

	ObjectAddressSet(addr, RelationRelationId, relid);
	policy = GpLabelGet(&addr, GP_LABEL_distributed_by);

	if (policy == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(policy));
}

void
_PG_init(void)
{
	/*
	 * This module reads CREATE TABLE and its friends before PostgreSQL does,
	 * so its ProcessUtility hook has to be in place before any statement
	 * runs.  A module loaded on demand is loaded when one of its functions is
	 * first called, and that is after the statement that needed it: the first
	 * CREATE TABLE ... WITH (gp_tag.x = 'y') of a session would be rejected
	 * by transformRelOptions instead.  It also registers a security label
	 * provider, which every backend has to have registered to read a label
	 * the provider owns.
	 */
	CB_REQUIRE_PRELOAD("gp_sql");
	CB_REQUIRE_CORE("gp_sql");

	DefineCustomIntVariable("gp.max_partition_level",
							"Sets the maximum number of levels allowed when creating a partitioned table using Greenplum classic syntax.",
							"0, the default, is no limit.  Cloudberry calls "
							"this gp_max_partition_level.",
							&gp_max_partition_level,
							0, 0, INT_MAX,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.allow_dml_directory_table",
							 "Allow ordinary DML on a directory table.",
							 "Its rows describe files on disk, so writing them "
							 "by hand makes the two disagree.  Cloudberry calls "
							 "this allow_dml_directory_table.",
							 &gp_allow_dml_directory_table,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	GpDistributionDefineSettings();

	GpTagRegisterProvider();
	GpDirTableRegisterXactCallback();

	/*
	 * O26: Cloudberry's own spelling of a statement is rewritten into
	 * PostgreSQL's before the grammar sees it.  See pg19/grammar/.
	 */
	GpGrammarInstallHook();

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = gp_sql_ExecutorStart;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_sql_ProcessUtility;

	prev_object_access = object_access_hook;
	object_access_hook = gp_sql_object_access;
}
