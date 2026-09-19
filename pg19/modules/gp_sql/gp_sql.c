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
 * (decision 11).  This module handles those forms -- it strips the options in
 * ProcessUtility_hook, registers the label provider, and offers the functions
 * that the grammar desugars to.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/commands/tag.c, dirtablecmds.c, storagecmds.c,
 *	  storage/file/ufile.c, parser/parse_partition_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_sql.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_sql",
					.version = GP_VERSION
);

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/*
 * Which object a tagged CREATE statement made, and whether to watch for one.
 *
 * CREATE INDEX may not name its index, so the only way to learn what was made
 * is to be told; the first relation the statement creates is the one the
 * clause was written on.  The same route is taken for CREATE TABLE, VIEW and
 * MATERIALIZED VIEW, so that there is one rule rather than one per statement.
 */
static bool pending_tags_armed = false;
static Oid	pending_relid = InvalidOid;

/* ------------------------------------------------------------------------- */
/* Where the shorthand may be written                                        */
/* ------------------------------------------------------------------------- */

/*
 * The option list of a statement that creates a relation, or NULL for a
 * statement that has none.  These are the four statements PG19 lets an
 * unknown namespace through: a namespaced reloption is rejected in
 * transformRelOptions, which runs after this hook.
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
 * The first relation a tagged CREATE statement makes is the one the clause
 * was written on.
 *
 * Only its OID is taken here.  index_create fires this hook before the
 * CommandCounterIncrement that makes the new pg_class row visible, so nothing
 * that reads the catalog for it can run yet -- not even to ask what relkind
 * it is.  The tags are put on once the statement is over.
 */
static void
gp_sql_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					 int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (classId != RelationRelationId || subId != 0)
		return;

	if (access == OAT_POST_CREATE)
	{
		if (pending_tags_armed && !OidIsValid(pending_relid))
			pending_relid = objectId;
	}
	else if (access == OAT_DROP)
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
 * CREATE TABLESPACE ... WITH (gp.server = 's') and ALTER TABLESPACE ... SET.
 *
 * Cloudberry names a library and a function in two pg_tablespace columns;
 * here the tablespace names a storage server and the handler for one
 * registers itself.  The option is taken out before tablespace_reloptions
 * would reject it, and recorded once the tablespace exists.
 */
static void
gp_sql_tablespace_options(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	List	  **options;
	List	   *opts;
	const char *spcname;

	if (IsA(parsetree, CreateTableSpaceStmt))
		options = &((CreateTableSpaceStmt *) parsetree)->options;
	else
		options = &((AlterTableSpaceOptionsStmt *) parsetree)->options;

	if (has_gp_options(*options))
	{
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
			if (IsA(parsetree, CreateTableSpaceStmt))
				options = &((CreateTableSpaceStmt *) parsetree)->options;
			else
				options = &((AlterTableSpaceOptionsStmt *) parsetree)->options;
		}
		opts = GpStorageTakeTablespaceOptions(options);
	}
	else
		opts = NIL;

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (opts == NIL)
		return;

	if (IsA(parsetree, CreateTableSpaceStmt))
		spcname = ((CreateTableSpaceStmt *) parsetree)->tablespacename;
	else
		spcname = ((AlterTableSpaceOptionsStmt *) parsetree)->tablespacename;

	GpStorageApplyToTablespace(spcname, opts);
}

static void
gp_sql_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					  bool readOnlyTree, ProcessUtilityContext context,
					  ParamListInfo params, QueryEnvironment *queryEnv,
					  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	List	  **options;
	List	   *tags = NIL;
	bool		is_alter = false;

	if (IsA(parsetree, TruncateStmt))
		GpDirTableCheckTruncate((TruncateStmt *) parsetree);

	if (IsA(parsetree, CreateTableSpaceStmt) ||
		IsA(parsetree, AlterTableSpaceOptionsStmt))
	{
		gp_sql_tablespace_options(pstmt, queryString, readOnlyTree, context,
								  params, queryEnv, dest, qc);
		return;
	}

	options = create_options_of(parsetree);

	if ((options != NULL && has_tag_options(*options)) ||
		(IsA(parsetree, AlterTableStmt) &&
		 alter_has_tag_options((AlterTableStmt *) parsetree)))
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
		}

		if (IsA(parsetree, AlterTableStmt))
		{
			tags = alter_take_tags((AlterTableStmt *) parsetree);
			is_alter = true;
		}
		else
			tags = GpTagTakeOptions(options);
	}

	if (tags == NIL)
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
	 * An undefined tag is refused before the statement runs, so that a
	 * misspelled one does not leave a table behind.
	 */
	GpTagCheckAll(tags);

	if (!is_alter)
	{
		pending_tags_armed = true;
		pending_relid = InvalidOid;
	}

	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);

		if (is_alter)
		{
			AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

			pending_relid = RangeVarGetRelid(stmt->relation, NoLock,
											 stmt->missing_ok);
		}

		if (OidIsValid(pending_relid))
			GpTagApplyToRelation(pending_relid, tags);
	}
	PG_FINALLY();
	{
		pending_tags_armed = false;
		pending_relid = InvalidOid;
	}
	PG_END_TRY();
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

	GpTagRegisterProvider();
	GpDirTableRegisterXactCallback();

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = gp_sql_ExecutorStart;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_sql_ProcessUtility;

	prev_object_access = object_access_hook;
	object_access_hook = gp_sql_object_access;
}
