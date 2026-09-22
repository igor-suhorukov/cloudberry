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

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
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

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_grammar.h"
#include "gp_label.h"
#include "gp_sql.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_sql",
					.version = GP_VERSION
);

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/*
 * The relations a CREATE statement carrying tags or a distribution has made,
 * while it runs, and whether to watch for them; see created_relation.  The
 * list is in TopTransactionContext, since the hook that adds to it runs in
 * whatever context the statement is in at the time.
 */
static bool pending_armed = false;
static List *pending_created = NIL;

static void check_distribution_policy(const char *policy);

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

/*
 * WITH (gp.distributed_by = '(a,b)'): what DISTRIBUTED BY on CREATE TABLE,
 * CREATE TABLE AS and CREATE MATERIALIZED VIEW becomes (gp_desugar.c,
 * rw_distribution).  Other options of the port's namespace are other
 * modules' to take.
 */
static bool
is_distribution_option(DefElem *def)
{
	return def->defnamespace != NULL &&
		strcmp(def->defnamespace, GP_OPTION_NS) == 0 &&
		strcmp(def->defname, "distributed_by") == 0;
}

static bool
has_distribution_option(List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		if (is_distribution_option((DefElem *) lfirst(lc)))
			return true;
	}

	return false;
}

/* Take it out of the option list, before the statement would refuse it. */
static char *
take_distribution_option(List **options)
{
	char	   *policy = NULL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (!is_distribution_option(def))
			continue;
		if (policy != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("a table can be given one distribution")));
		policy = defGetString(def);
		*options = foreach_delete_current(*options, lc);
	}

	return policy;
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
 * The relations a CREATE statement carrying tags or a distribution makes.
 *
 * Only their OIDs are taken here.  index_create fires this hook before the
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

	if (classId != RelationRelationId || subId != 0)
		return;

	if (access == OAT_POST_CREATE)
	{
		if (pending_armed)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);

			pending_created = lappend_oid(pending_created, objectId);
			MemoryContextSwitchTo(oldcxt);
		}
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
	List	   *opts = NIL;
	List	   *tags = NIL;
	bool		creating = IsA(parsetree, CreateTableSpaceStmt);
	const char *spcname;

	if (creating)
		options = &((CreateTableSpaceStmt *) parsetree)->options;
	else
		options = &((AlterTableSpaceOptionsStmt *) parsetree)->options;

	if (has_gp_options(*options) || (creating && has_tag_options(*options)))
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

		/*
		 * CREATE TABLESPACE ... TAG (...), which the desugarer writes into the
		 * WITH list: the statement may not run in a transaction block, so a
		 * call after it could not be how its tags are set.
		 */
		if (creating)
			tags = GpTagTakeOptions(options);
	}

	/* An undefined tag is refused before the tablespace is made. */
	GpTagCheckAll(tags);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
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
 * CREATE DATABASE ... TAG (...), which the desugarer writes as options named
 * "gp_tag.<name>", because the statement may not run in a transaction block
 * and so cannot be followed by the call that would set them.  They are taken
 * out before createdb() would refuse them, and the database is labelled once
 * it exists, in the statement's own transaction.
 */
static void
gp_sql_createdb_tags(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	CreatedbStmt *stmt;
	List	   *tags;

	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
		readOnlyTree = false;
	}
	stmt = (CreatedbStmt *) pstmt->utilityStmt;
	tags = GpTagTakeDatabaseOptions(&stmt->options);

	/* An undefined tag is refused before the database is made. */
	GpTagCheckAll(tags);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	CommandCounterIncrement();
	GpTagApplyToObject(DatabaseRelationId, get_database_oid(stmt->dbname, false),
					   tags);
}

/* Does CREATE DATABASE carry the desugarer's tag options? */
static bool
createdb_has_tags(CreatedbStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strncmp(def->defname, GP_TAG_OPTION_NS ".",
					strlen(GP_TAG_OPTION_NS ".")) == 0)
			return true;
	}

	return false;
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
static Oid
created_relation(Node *parsetree)
{
	RangeVar   *rv;
	Oid			relid;
	ListCell   *lc;

	/* what the statement made, visible */
	CommandCounterIncrement();

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			rv = ((CreateStmt *) parsetree)->relation;
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
			foreach(lc, pending_created)
			{
				char		relkind = get_rel_relkind(lfirst_oid(lc));

				if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
					return lfirst_oid(lc);
			}
			return InvalidOid;
		default:
			return InvalidOid;
	}

	relid = RangeVarGetRelid(rv, NoLock, true);
	return list_member_oid(pending_created, relid) ? relid : InvalidOid;
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
	char	   *policy = NULL;
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

	if (IsA(parsetree, CreatedbStmt) &&
		createdb_has_tags((CreatedbStmt *) parsetree))
	{
		gp_sql_createdb_tags(pstmt, queryString, readOnlyTree, context,
							 params, queryEnv, dest, qc);
		return;
	}

	options = create_options_of(parsetree);

	if ((options != NULL &&
		 (has_tag_options(*options) || has_distribution_option(*options))) ||
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
		{
			tags = GpTagTakeOptions(options);
			policy = take_distribution_option(options);
		}
	}

	if (tags == NIL && policy == NULL)
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
	 * An undefined tag, or a policy that is not one, is refused before the
	 * statement runs, so that a misspelled one does not leave a table behind.
	 */
	GpTagCheckAll(tags);
	if (policy != NULL)
		check_distribution_policy(policy);

	if (!is_alter)
	{
		pending_armed = true;
		pending_created = NIL;
	}

	PG_TRY();
	{
		Oid			relid;

		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
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
			GpTagApplyToRelation(relid, tags);
		}
	}
	PG_FINALLY();
	{
		pending_armed = false;
		pending_created = NIL;
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
