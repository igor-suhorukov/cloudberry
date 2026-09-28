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
 * gp_ao.c
 *	  Append-optimized tables, by row and by column: the module.
 *
 * ao_row and ao_column are table access methods (ao_am.c) whose rows are in
 * blocks, appended to up to 127 segment files a table, each written by one
 * writer at a time (ao_dml.c), in 8K pages of the table's own relation
 * logged by this module's resource manager (ao_storage.c), with what
 * Cloudberry keeps in pg_aoseg, pg_aovisimap and pg_aoblkdir in gp_ao's
 * tables (ao_meta.c).  What the core does not ask a table access method,
 * the registry asks (O13): the options (O14), the columns a scan reads
 * (O15), a unique index's probe (O16), the columns ALTER TABLE adds to a
 * table by column (O17), BRIN's runs of blocks (O18) and UPDATE's old row
 * (O20).  What Cloudberry shows of such a table -- pg_appendonly, gp_toolkit's
 * functions of it -- is in ao_toolkit.c, a column's own options in
 * ao_encoding.c, and VACUUM in ao_vacuum.c.
 *
 * This file is what the module hooks: Cloudberry's spelling of the storage
 * options, the triggers Cloudberry refuses, the last phase of VACUUM, a
 * dropped table's rows in gp_ao's tables, ANALYZE's sample, and where
 * statements end.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_inherits.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/repack.h"
#include "commands/seclabel.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/procarray.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_policy.h"
#include "gp_ao.h"
#include "gp_encoding.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_ao",
					.version = GP_VERSION
);

/* Cloudberry's gp_appendonly_compaction_threshold: the percentage of a
 * segment file's rows deleted at which VACUUM compacts it. */
int			gp_appendonly_compaction_threshold = 10;

/* Cloudberry's gp_appendonly_compaction: whether VACUUM compacts at all. */
bool		gp_appendonly_compaction = true;

/*
 * Cloudberry's gp_appendonly_insert_files and ..._tuples_range: how many
 * segment files an insert spreads its rows over, and how many rows go to one
 * before the next (ao_dml.c).
 */
int			gp_appendonly_insert_files = 0;
int			gp_appendonly_insert_files_tuples_range = 100000;

/*
 * Cloudberry's gp_select_invisible, for an append-optimized table: a scan
 * or a fetch returns the rows deleted too, as the visibility map had none.
 */
bool		gp_select_invisible = false;

/* Cloudberry's gp_predicate_pushdown_sample_rows: accepted, and unused. */
static int	gp_predicate_pushdown_sample_rows = 10000;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static analyze_sample_rows_hook_type prev_analyze_sample_rows = NULL;
static query_lockmode_hook_type prev_query_lockmode = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;

/* ------------------------------------------------------------------------- */
/* Which tables are this module's                                            */
/* ------------------------------------------------------------------------- */

static bool
am_is_ao(Oid amoid)
{
	return OidIsValid(amoid) &&
		(amoid == get_table_am_oid("ao_row", true) ||
		 amoid == get_table_am_oid("ao_column", true));
}

static bool
relid_is_ao(Oid relid)
{
	return am_is_ao(get_rel_relam(relid));
}

/* ------------------------------------------------------------------------- */
/* Cloudberry's spelling: WITH (appendonly=true, orientation=column)         */
/* ------------------------------------------------------------------------- */

/*
 * Take appendonly, appendoptimized and orientation out of a statement's
 * options and say which access method they choose, or NULL for none, as
 * Cloudberry's grammar does (greenplumLegacyAOoptions): each once, an
 * orientation of column or row only where the table is append-optimized,
 * and appendonly=false heap, which is not the same as saying nothing -- a
 * partition that says nothing has its parent's method.  Neither is kept in
 * the table's options: they choose its access method, as they do here.
 */
static char *
take_storage_options(List **options)
{
	ListCell   *lc;
	bool		appendonly = false;
	bool		appendonly_found = false;
	bool		column = false;
	bool		orientation_found = false;

	foreach(lc, *options)
	{
		DefElem    *def = lfirst(lc);

		if (def->defnamespace != NULL)
			continue;
		if (strcmp(def->defname, "appendonly") == 0 ||
			strcmp(def->defname, "appendoptimized") == 0)
		{
			if (appendonly_found)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("parameter \"appendonly\" specified more than once")));
			appendonly = defGetBoolean(def);
			appendonly_found = true;
			*options = foreach_delete_current(*options, lc);
		}
		else if (strcmp(def->defname, "orientation") == 0)
		{
			char	   *value = defGetString(def);

			if (orientation_found)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("parameter \"orientation\" specified more than once")));
			if (strcmp(value, "column") != 0 && strcmp(value, "row") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid parameter value for \"orientation\": \"%s\"",
								value)));
			column = (strcmp(value, "column") == 0);
			orientation_found = true;
			*options = foreach_delete_current(*options, lc);
		}
	}

	if (!appendonly && orientation_found)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid option \"orientation\" for base relation"),
				 errhint("Table orientation only valid for Append Optimized relations, create an AO relation to use table orientation.")));
	if (appendonly)
		return column ? "ao_column" : "ao_row";
	if (appendonly_found)
		return "heap";
	return NULL;
}

/* USING, where the statement has one, before what the options choose. */
static void
choose_access_method(char **accessMethod, List **options)
{
	char	   *am = take_storage_options(options);

	if (am != NULL && *accessMethod == NULL)
		*accessMethod = pstrdup(am);
}

/*
 * The row UPDATE and DELETE triggers Cloudberry refuses on an
 * append-optimized table: they fetch the old row by its TID, which a table
 * whose rows are in blocks cannot give (O20 gives UPDATE its old row from
 * the plan instead).
 */
static void
check_trigger(CreateTrigStmt *stmt)
{
	Oid			relid;

	if (!stmt->row || !(stmt->events & (TRIGGER_TYPE_UPDATE | TRIGGER_TYPE_DELETE)))
		return;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid) || !relid_is_ao(relid))
		return;
	if (stmt->events & TRIGGER_TYPE_UPDATE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ON UPDATE triggers are not supported on append-only tables")));
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ON DELETE triggers are not supported on append-only tables")));
}

/* The method a CREATE TABLE's table will have. */
static char *
effective_am(CreateStmt *stmt, Oid *parentid)
{
	*parentid = InvalidOid;
	if (stmt->partbound != NULL && list_length(stmt->inhRelations) == 1)
		*parentid = RangeVarGetRelid(linitial(stmt->inhRelations), NoLock, true);
	if (stmt->accessMethod != NULL)
		return stmt->accessMethod;
	if (OidIsValid(*parentid) && OidIsValid(get_rel_relam(*parentid)))
		return get_am_name(get_rel_relam(*parentid));
	return default_table_access_method;
}

static bool
am_name_is_ao(const char *am)
{
	return am != NULL && (strcmp(am, "ao_row") == 0 || strcmp(am, "ao_column") == 0);
}

/*
 * What CREATE TABLE's ENCODING clauses and storage options become, taken
 * from the statement before PostgreSQL sees it and put on the table once
 * it is made.
 */
typedef struct CreatePending
{
	RangeVar   *relation;
	List	   *encodings;		/* AoColumnEncoding */
	List	   *own_opts;		/* the storage options its WITH list gave */
	List	   *parent_opts;	/* a partitioned table's, for its label */
	bool		partitioned;

	/* a method of another module's that takes the clauses (gp_encoding.h) */
	const GpEncodingMethod *method;
} CreatePending;

/*
 * The partitioned tables of this module's methods a statement is making, with
 * the options their labels will hold, until finish_create() labels them:
 * Cloudberry's classic partition clauses make the partitions inside their
 * parent's CREATE (gp_sql's partition.c), before this hook is back to label
 * the parent, and each partition takes its parent's options.  In the
 * transaction's memory, and gone with it.
 */
typedef struct PendingParent
{
	RangeVar   *relation;
	List	   *opts;
	List	   *encodings;		/* its columns', which its partitions take */
	List	   *own_opts;
	bool		labelled;		/* its columns labelled, for its partitions */
	SubTransactionId subid;		/* a CREATE that failed leaves it */
} PendingParent;

static List *pending_parents = NIL;

static void
pending_parent_push(RangeVar *relation, List *opts, List *encodings,
					List *own_opts)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	PendingParent *pp = palloc_object(PendingParent);

	pp->relation = copyObject(relation);
	pp->opts = copyObject(opts);
	/* the parent's statement's, which is done before finish_create() pops it */
	pp->encodings = encodings;
	pp->own_opts = own_opts;
	pp->labelled = false;
	pp->subid = GetCurrentSubTransactionId();
	pending_parents = lcons(pp, pending_parents);
	MemoryContextSwitchTo(old);
}

/*
 * The relations whose pg_class rows the transaction altered, for their
 * relfrozenxid: see reset_frozen_xids().
 */
static List *altered_rels = NIL;

static void
pending_parents_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PREPARE || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_PARALLEL_ABORT)
	{
		pending_parents = NIL;
		altered_rels = NIL;
	}
}

static void
pending_parents_subxact(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
	foreach(lc, pending_parents)
		if (((PendingParent *) lfirst(lc))->subid >= mySubid)
			pending_parents = foreach_delete_current(pending_parents, lc);
}

static void
pending_parent_pop(RangeVar *relation)
{
	ListCell   *lc;

	foreach(lc, pending_parents)
	{
		if (equal(((PendingParent *) lfirst(lc))->relation, relation))
		{
			pending_parents = foreach_delete_current(pending_parents, lc);
			return;
		}
	}
}

static PendingParent *
pending_parent_find(Oid parentid)
{
	foreach_ptr(PendingParent, pp, pending_parents)
		if (RangeVarGetRelid(pp->relation, NoLock, true) == parentid)
			return pp;
	return NULL;
}

/* The options a parent being made will have, or NIL. */
static List *
pending_parent_opts(Oid parentid)
{
	PendingParent *pp = pending_parent_find(parentid);

	return pp != NULL ? pp->opts : NIL;
}

/*
 * A parent being made, its columns labelled now, before its first partition
 * is made: each takes the encodings of the parent's columns (see
 * ao_encoding_apply()), which finish_create() would give them only once
 * every partition is made.  finish_create() leaves a labelled column be.
 */
static void
pending_parent_label(Oid parentid)
{
	PendingParent *pp = pending_parent_find(parentid);

	if (pp == NULL || pp->labelled)
		return;
	pp->labelled = true;
	if (get_rel_relam(parentid) == get_table_am_oid("ao_column", true))
	{
		ao_encoding_apply(parentid, pp->encodings, pp->own_opts, NIL, false);
		CommandCounterIncrement();
	}
}

/*
 * A DEFAULT COLUMN ENCODING that names an option the WITH list gives as
 * well, which Cloudberry refuses (transformColumnEncoding()'s
 * encodings_overlap()).
 */
static void
check_default_encoding(List *encodings, List *withopts)
{
	foreach_ptr(AoColumnEncoding, ce, encodings)
	{
		if (!ce->is_default)
			continue;
		foreach_node(DefElem, d, ce->opts)
			foreach_node(DefElem, w, withopts)
				if (pg_strcasecmp(d->defname, w->defname) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("DEFAULT COLUMN ENCODING clause cannot override values set in WITH clause")));
	}
}

static CreatePending *
prepare_create(CreateStmt *stmt)
{
	CreatePending *cp = palloc0_object(CreatePending);
	Oid			parentid;
	char	   *am;

	choose_access_method(&stmt->accessMethod, &stmt->options);
	ao_encoding_take(&stmt->options, &cp->encodings);
	am = effective_am(stmt, &parentid);
	cp->relation = stmt->relation;
	cp->partitioned = (stmt->partspec != NULL);
	cp->method = GpEncodingMethodOf(am);

	if (cp->encodings != NIL && cp->method == NULL &&
		(am == NULL || strcmp(am, "ao_column") != 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));
	ao_encoding_check(cp->encodings, cp->method);

	/*
	 * Another module's method takes what the clauses say, and its table's
	 * own compression options as their default -- nothing on a partitioned
	 * table, which Cloudberry keeps none for but a table by column.
	 */
	if (cp->method != NULL)
	{
		cp->own_opts = ao_compression_opts_of(stmt->options);
		check_default_encoding(cp->encodings, cp->own_opts);
		return cp;
	}
	if (!am_name_is_ao(am))
		return cp;

	cp->own_opts = ao_storage_opts_of(stmt->options);
	check_default_encoding(cp->encodings, cp->own_opts);
	if (OidIsValid(parentid))
		pending_parent_label(parentid);
	if (OidIsValid(parentid) && get_rel_relam(parentid) == get_table_am_oid(am, true))
		ao_partition_inherit(parentid, pending_parent_opts(parentid),
							 &stmt->options);
	ao_default_storage_options_add(&stmt->options);
	if (cp->partitioned)
	{
		cp->parent_opts = ao_partitioned_take(&stmt->options);
		pending_parent_push(stmt->relation, cp->parent_opts, cp->encodings,
							cp->own_opts);
	}
	return cp;
}

static void
finish_create(CreatePending *cp)
{
	Oid			relid = RangeVarGetRelid(cp->relation, NoLock, true);

	if (cp->method != NULL)
	{
		if (OidIsValid(relid) && !cp->partitioned &&
			get_rel_relam(relid) == get_table_am_oid(cp->method->amname, true))
			ao_encoding_apply_given(relid, cp->encodings, cp->own_opts);
		return;
	}
	if (cp->partitioned)
		pending_parent_pop(cp->relation);
	if (!OidIsValid(relid) || !relid_is_ao(relid))
		return;
	if (cp->partitioned)
		ao_partitioned_set(relid, cp->parent_opts);
	if (get_rel_relam(relid) == get_table_am_oid("ao_column", true))
		ao_encoding_apply(relid, cp->encodings, cp->own_opts, NIL, false);
}

/* ------------------------------------------------------------------------- */
/* SET ACCESS METHOD, and the options a table takes with its new method      */
/* ------------------------------------------------------------------------- */

/*
 * The WITH list the grammar rewriter carried as text (gp_desugar.c), as a
 * CREATE TABLE's WITH list would be parsed: through the rewriter again, which
 * quotes orientation=row's keyword.
 */
static List *
parse_with_text(const char *text)
{
	List	   *raw = raw_parser(psprintf("CREATE TABLE gp_ao_with () WITH (%s)", text),
								 RAW_PARSE_DEFAULT);

	return castNode(CreateStmt, linitial_node(RawStmt, raw)->stmt)->options;
}

/*
 * SET WITH (appendonly=..., orientation=..., ...), Cloudberry's legacy
 * spelling of a change of access method, and SET ACCESS METHOD m WITH
 * (...), its own: the rewriter hands each over as an option of gp_ao's in a
 * SET (...) of its own.  Each becomes PostgreSQL's SET ACCESS METHOD, with
 * the options the table is to have in its new method in the command's def,
 * where Cloudberry's grammar puts them and PostgreSQL's executor reads
 * nothing (set_access_method_options()).  As Cloudberry's grammar says
 * (greenplumLegacyAOoptions()), the method the options name has to be the
 * one SET ACCESS METHOD names.
 */
static void
prepare_set_access_method(AlterTableStmt *stmt)
{
	ListCell   *lc;
	AlterTableCmd *setam = NULL;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst(lc);
		DefElem    *def;
		List	   *opts;
		char	   *am;

		if (cmd->subtype == AT_SetAccessMethod)
			setam = cmd;
		if (cmd->subtype != AT_SetRelOptions || list_length((List *) cmd->def) != 1)
			continue;
		def = linitial((List *) cmd->def);
		if (def->defnamespace == NULL || strcmp(def->defnamespace, "gp_ao") != 0 ||
			(strcmp(def->defname, "set_with") != 0 &&
			 strcmp(def->defname, "am_with") != 0))
			continue;

		opts = parse_with_text(defGetString(def));
		am = take_storage_options(&opts);
		if (strcmp(def->defname, "set_with") == 0)
		{
			Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

			if (OidIsValid(relid) &&
				get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change access method of a partitioned table")));
			cmd->subtype = AT_SetAccessMethod;
			cmd->name = pstrdup(am);
			cmd->def = (Node *) opts;
			continue;
		}

		/* SET ACCESS METHOD m WITH (...): the command before */
		if (setam == NULL)
			elog(ERROR, "gp_ao.am_with without SET ACCESS METHOD");
		if (am != NULL && strcmp(am, setam->name ? setam->name :
								 default_table_access_method) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ACCESS METHOD is specified as \"%s\" but the WITH option indicates it to be \"%s\"",
							setam->name ? setam->name : default_table_access_method,
							am)));
		if (am != NULL)
			ereport(NOTICE,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("Redundant clauses are used to indicate the access method."),
					 errhint("Only one of these is needed to indicate access method: the SET ACCESS METHOD clause or the options in the WITH clause.")));
		setam->def = (Node *) opts;
		stmt->cmds = foreach_delete_current(stmt->cmds, lc);
	}
}

/*
 * What SET ACCESS METHOD does to a table's options, which PostgreSQL 19
 * leaves as they were, checked against nothing: a table whose method changes
 * has the options the command gives it and no others, checked against its
 * new method, as Cloudberry's ATExecSetRelOptions() clears the old ones --
 * and a heap table made append-optimized has what gp.default_storage_options
 * says of what they do not, as CREATE TABLE's would, while one that was
 * append-optimized already, given none, keeps its own (Cloudberry's
 * make_new_heap_with_colname()).  They are in pg_class before PostgreSQL's
 * rewrite makes the new table, which takes its options from there
 * (make_new_heap()), so its rows are written as they say.  A table that
 * keeps its method takes the options as SET (...) does, merged with its own,
 * and is rewritten under them if they changed, as SET (...) rewrites one
 * (rewrite_if_options_changed()).
 *
 * On the coordinator, and on a segment, whose dispatched statement carries
 * the options in the command's def.  Returns the table whose method the
 * statement changes, its method before and the options the command gave, or
 * InvalidOid.
 */
static Oid
set_access_method_options(AlterTableStmt *stmt, Oid *oldam, List **withopts)
{
	ListCell   *lc;
	Oid			relid = InvalidOid;
	Oid			changed = InvalidOid;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst(lc);
		Relation	rel;
		Oid			newam;
		List	   *opts;

		if (cmd->subtype != AT_SetAccessMethod)
			continue;
		/* The table, locked and checked as PostgreSQL's ALTER TABLE will. */
		if (!OidIsValid(relid))
		{
			relid = AlterTableLookupRelation(stmt, AccessExclusiveLock);
			if (!OidIsValid(relid))
				return InvalidOid;
		}
		newam = get_table_am_oid(cmd->name ? cmd->name : default_table_access_method,
								 false);
		opts = (List *) cmd->def;

		rel = relation_open(relid, NoLock);
		if ((rel->rd_rel->relkind != RELKIND_RELATION &&
			 rel->rd_rel->relkind != RELKIND_MATVIEW) ||
			rel->rd_rel->relam == newam)
		{
			relation_close(rel, NoLock);
			/* the table's own method, or no options of a table's: SET (...) */
			cmd->def = NULL;
			if (opts != NIL)
			{
				AlterTableCmd *set = makeNode(AlterTableCmd);

				set->subtype = AT_SetRelOptions;
				set->def = (Node *) opts;
				stmt->cmds = lappend(stmt->cmds, set);
			}
			continue;
		}
		*oldam = rel->rd_rel->relam;
		*withopts = opts;
		changed = relid;
		if (am_is_ao(newam) && !am_is_ao(rel->rd_rel->relam))
		{
			opts = list_copy(opts);
			ao_default_storage_options_add(&opts);
			/* what a segment is to give the table, the defaults with them */
			cmd->def = (Node *) opts;
		}
		if (opts != NIL || !am_is_ao(newam) || !am_is_ao(rel->rd_rel->relam))
			ao_replace_reloptions(rel, newam, opts);
		relation_close(rel, NoLock);
	}
	return changed;
}

/*
 * A table whose method a statement changed: its columns' encodings, which a
 * table by column has each of and no other has any of -- the options the
 * command gave, as a table's WITH gives its columns theirs, filled in from
 * the table's own.
 */
static void
finish_set_access_method(Oid relid, Oid oldam, List *withopts)
{
	Oid			aocol = get_table_am_oid("ao_column", true);
	Oid			newam = get_rel_relam(relid);

	if (newam == oldam)
		return;
	if (OidIsValid(aocol) && newam == aocol)
		ao_encoding_apply(relid, NIL, withopts, NIL, true);
	else if ((OidIsValid(aocol) && oldam == aocol) ||
			 (OidIsValid(oldam) && GpEncodingMethodOf(get_am_name(oldam)) != NULL))
		ao_encoding_clear(relid);
}

/*
 * ALTER TABLE ... ALTER COLUMN c SET ENCODING (...), which the grammar makes
 * SET (gp_ao.compresstype = ..., ...), and ADD COLUMN c ... ENCODING (...),
 * which it makes ADD COLUMN c ..., ALTER COLUMN c SET (gp_ao....): the
 * options taken out of their commands, and a command left with none taken
 * away.  What is left of the statement may be no command at all.
 */
typedef struct AlterPending
{
	List	   *colnames;		/* String */
	List	   *opts;			/* a List of DefElem each */
} AlterPending;

static AlterPending *
prepare_alter(AlterTableStmt *stmt)
{
	AlterPending *ap = palloc0_object(AlterPending);
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst(lc);
		List	   *mine = NIL;
		ListCell   *lc2;

		if (cmd->subtype != AT_SetOptions || cmd->def == NULL)
			continue;
		foreach(lc2, (List *) cmd->def)
		{
			DefElem    *def = lfirst(lc2);

			if (def->defnamespace == NULL || strcmp(def->defnamespace, "gp_ao") != 0)
				continue;
			mine = lappend(mine, makeDefElem(def->defname,
											 (Node *) makeString(defGetString(def)), -1));
			cmd->def = (Node *) foreach_delete_current((List *) cmd->def, lc2);
		}
		if (mine == NIL)
			continue;
		ap->colnames = lappend(ap->colnames, makeString(pstrdup(cmd->name)));
		ap->opts = lappend(ap->opts, mine);
		if (cmd->def == NULL || list_length((List *) cmd->def) == 0)
			stmt->cmds = foreach_delete_current(stmt->cmds, lc);
	}
	return ap;
}

static void
finish_alter(Oid relid, AlterPending *ap)
{
	List	   *relids;
	Oid			aocol = get_table_am_oid("ao_column", true);
	Oid			am;
	const GpEncodingMethod *method = NULL;

	if (!OidIsValid(relid))
		return;
	relids = list_make1_oid(relid);
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
		relids = find_all_inheritors(relid, NoLock, NULL);

	am = get_rel_relam(relid);
	if (OidIsValid(am) && am != aocol)
		method = GpEncodingMethodOf(get_am_name(am));
	if (ap->colnames != NIL && (!OidIsValid(aocol) || am != aocol) &&
		method == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));

	/*
	 * Another module's method: the columns named are given what the command
	 * says, and the rest nothing, as Cloudberry adds a column of such a table
	 * with no options of its own.
	 */
	if (method != NULL)
	{
		foreach_oid(r, relids)
		{
			ListCell   *lc1;
			ListCell   *lc2;

			if (get_rel_relkind(r) == RELKIND_PARTITIONED_TABLE ||
				get_rel_relam(r) != am)
				continue;
			forboth(lc1, ap->colnames, lc2, ap->opts)
				ao_encoding_set_column_given(r, strVal(lfirst(lc1)), lfirst(lc2),
											 method);
		}
		return;
	}
	if (!OidIsValid(aocol))
		return;

	foreach_oid(r, relids)
	{
		ListCell   *lc1;
		ListCell   *lc2;

		if (get_rel_relam(r) != aocol)
			continue;
		forboth(lc1, ap->colnames, lc2, ap->opts)
			ao_encoding_set_column(r, strVal(lfirst(lc1)), lfirst(lc2));
		/* and the columns it has no options for, added or all of them */
		if (get_rel_relkind(r) != RELKIND_PARTITIONED_TABLE)
			ao_encoding_apply(r, NIL, NIL, NIL, false);
	}
}

/*
 * VACUUM's last phase, as Cloudberry's post-cleanup: the segment files it
 * compacted, recycled in the transaction VACUUM leaves for its caller to
 * commit, if no snapshot still sees them -- which, when no older transaction
 * runs beside it, none does.
 */
static void
recycle_compacted(void)
{
	List	   *compacted = ao_vacuum_take_compacted();

	foreach_oid(relid, compacted)
		ao_vacuum_recycle_rel(relid);
	list_free(compacted);
}

/*
 * An append-optimized table keeps no transaction IDs in its rows, so it has
 * no relfrozenxid or relminmxid -- Cloudberry's are 0 -- and PostgreSQL
 * leaves such a table out as it advances datfrozenxid.  A rewrite that swaps
 * a table's files, ALTER TABLE's or REFRESH MATERIALIZED VIEW's, sets them
 * to RecentXmin whatever the table's access method, and no VACUUM of the
 * table would advance them after.  The tables whose pg_class rows the
 * statement altered, once it is done, have them back at 0.
 */
static void
reset_frozen_xids(void)
{
	List	   *rels = altered_rels;
	Relation	pg_class = NULL;

	altered_rels = NIL;
	foreach_oid(relid, rels)
	{
		HeapTuple	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		Form_pg_class form;

		if (!HeapTupleIsValid(tup))
			continue;			/* a rewrite's transient table, dropped */
		form = (Form_pg_class) GETSTRUCT(tup);
		if (am_is_ao(form->relam) &&
			(TransactionIdIsValid(form->relfrozenxid) ||
			 MultiXactIdIsValid(form->relminmxid)))
		{
			if (pg_class == NULL)
				pg_class = table_open(RelationRelationId, RowExclusiveLock);
			form->relfrozenxid = InvalidTransactionId;
			form->relminmxid = InvalidMultiXactId;
			CatalogTupleUpdate(pg_class, &tup->t_self, tup);
		}
		heap_freetuple(tup);
	}
	list_free(rels);
	if (pg_class != NULL)
	{
		table_close(pg_class, RowExclusiveLock);
		CommandCounterIncrement();
	}
}

/*
 * ALTER TABLE ... SET (...) and RESET (...) of an append-optimized table's
 * options: Cloudberry rewrites the table when they change what its rows are
 * stored with, so that the rows it has are stored as the new options say
 * too, and not only those written after (ATExecSetRelOptions()'s
 * aoopt_changed, relOptionsEquals()).  The same values in another order, or
 * a value that is the default already, change nothing: what is compared is
 * the options resolved, before the statement and after.
 */
typedef struct OptionsWatch
{
	Oid			relid;
	AoOptions	before;
} OptionsWatch;

static OptionsWatch *
watch_options(AlterTableStmt *stmt)
{
	OptionsWatch *w;
	bool		found = false;
	Oid			relid;
	char		relkind;
	Relation	rel;

	foreach_node(AlterTableCmd, cmd, stmt->cmds)
		if (cmd->subtype == AT_SetRelOptions || cmd->subtype == AT_ResetRelOptions ||
			cmd->subtype == AT_ReplaceRelOptions)
			found = true;
	if (!found || stmt->objtype != OBJECT_TABLE)
		return NULL;
	/* The table, locked and checked as PostgreSQL's ALTER TABLE will. */
	relid = AlterTableLookupRelation(stmt, AlterTableGetLockLevel(stmt->cmds));
	if (!OidIsValid(relid) || !relid_is_ao(relid))
		return NULL;
	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
		return NULL;

	w = palloc(sizeof(OptionsWatch));
	w->relid = relid;
	rel = relation_open(relid, NoLock);
	ao_get_options(rel, &w->before);
	relation_close(rel, NoLock);
	return w;
}

/*
 * The table rewritten if its options changed: its rows copied into new files
 * under them, as CLUSTER copies a table (table_relation_copy_for_cluster(),
 * gp_ao's), and the files swapped, its indexes rebuilt.
 */
static void
rewrite_if_options_changed(OptionsWatch *w)
{
	Relation	rel;
	Relation	newrel;
	Oid			newrelid;
	char		persistence;
	AoOptions	after;
	TransactionId xid_cutoff = InvalidTransactionId;
	MultiXactId multi_cutoff = InvalidMultiXactId;
	double		num_tuples;
	double		tups_vacuumed;
	double		tups_recently_dead;

	if (w == NULL || !relid_is_ao(w->relid))
		return;

	rel = table_open(w->relid, AccessExclusiveLock);
	ao_get_options(rel, &after);
	if (after.blocksize == w->before.blocksize &&
		after.compresstype == w->before.compresstype &&
		after.compresslevel == w->before.compresslevel &&
		after.checksum == w->before.checksum)
	{
		table_close(rel, NoLock);
		return;
	}

	persistence = rel->rd_rel->relpersistence;
	newrelid = make_new_heap(w->relid, rel->rd_rel->reltablespace,
							 rel->rd_rel->relam, persistence, AccessExclusiveLock);
	newrel = table_open(newrelid, AccessExclusiveLock);
	table_relation_copy_for_cluster(rel, newrel, NULL, false,
									GetOldestNonRemovableTransactionId(rel),
									GetActiveSnapshot(),
									&xid_cutoff, &multi_cutoff, &num_tuples,
									&tups_vacuumed, &tups_recently_dead);
	table_close(newrel, NoLock);
	table_close(rel, NoLock);
	finish_heap_swap(w->relid, newrelid, false, false, false, true, true,
					 InvalidTransactionId, InvalidMultiXactId, persistence);
}

static void
gp_ao_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	CreatePending *create = NULL;
	AlterPending *alter = NULL;
	List	   *type_encoding = NIL;
	IntoClause *ctas_into = NULL;
	List	   *ctas_opts = NIL;
	const GpEncodingMethod *ctas_method = NULL;
	Oid			am_changed = InvalidOid;
	Oid			am_before = InvalidOid;
	List	   *am_withopts = NIL;
	OptionsWatch *watch = NULL;

	/*
	 * A fetch's descriptor kept between rows (ao_am.c) is a query's: gone as
	 * a utility statement begins, and as it ends -- COPY's AFTER triggers
	 * fetch their rows by TID, in no query.
	 */
	ao_fetch_cache_reset();

	/*
	 * On a segment, the statement the coordinator dispatched: what this hook
	 * made of it there -- the access method, the options -- it carries, and
	 * the labels it wrote there follow it (GpDispatchNoteLabelOf()).  What
	 * is left is VACUUM's, whose segment files are here.
	 */
	if (GpDispatchIsDispatchedStatement(parsetree))
	{
		if (IsA(parsetree, VacuumStmt))
			list_free(ao_vacuum_take_compacted());
		if (IsA(parsetree, AlterTableStmt))
		{
			Oid			oldam;
			List	   *withopts;

			if (readOnlyTree)
			{
				pstmt = copyObject(pstmt);
				parsetree = pstmt->utilityStmt;
				readOnlyTree = false;
			}
			if (!OidIsValid(set_access_method_options((AlterTableStmt *) parsetree,
													  &oldam, &withopts)))
				watch = watch_options((AlterTableStmt *) parsetree);
		}
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		rewrite_if_options_changed(watch);
		if (IsA(parsetree, VacuumStmt))
			recycle_compacted();
		reset_frozen_xids();
		ao_fetch_cache_reset();
		return;
	}

	if (readOnlyTree &&
		(IsA(parsetree, CreateStmt) || IsA(parsetree, CreateTableAsStmt) ||
		 IsA(parsetree, AlterTableStmt) || IsA(parsetree, SecLabelStmt) ||
		 IsA(parsetree, DefineStmt)))
	{
		pstmt = copyObject(pstmt);
		parsetree = pstmt->utilityStmt;
		readOnlyTree = false;
	}

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:
			create = prepare_create((CreateStmt *) parsetree);
			break;
		case T_CreateTableAsStmt:
			{
				IntoClause *into = ((CreateTableAsStmt *) parsetree)->into;

				choose_access_method(&into->accessMethod, &into->options);
				if (am_name_is_ao(into->accessMethod ? into->accessMethod :
								  default_table_access_method))
				{
					ctas_opts = ao_storage_opts_of(into->options);
					ao_default_storage_options_add(&into->options);
					ctas_into = into;
				}
				else if ((ctas_method = GpEncodingMethodOf(into->accessMethod ?
														   into->accessMethod :
														   default_table_access_method)) != NULL)
				{
					/* its columns take its compression options, as a CREATE's */
					ctas_opts = ao_compression_opts_of(into->options);
					ctas_into = into;
				}
				break;
			}
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				prepare_set_access_method(stmt);
				alter = prepare_alter(stmt);
				if (stmt->cmds == NIL)
				{
					/* Nothing left for PostgreSQL: the table, as it would. */
					Oid			relid = AlterTableLookupRelation(stmt, AccessExclusiveLock);

					finish_alter(relid, alter);
					return;
				}
				am_changed = set_access_method_options(stmt, &am_before,
													   &am_withopts);
				if (!OidIsValid(am_changed))
					watch = watch_options(stmt);
				break;
			}
		case T_SecLabelStmt:
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parsetree;

				/* ALTER TYPE ... SET DEFAULT ENCODING, filled in */
				if (stmt->provider != NULL && strcmp(stmt->provider, "gp_ao") == 0 &&
					stmt->objtype == OBJECT_TYPE && stmt->label != NULL)
					stmt->label = ao_encoding_type_label(stmt->label);
				break;
			}
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;
				ListCell   *lc;

				/*
				 * CREATE TYPE ... (..., compresstype=..., blocksize=...):
				 * Cloudberry's default encoding of the type's columns, which
				 * gp_ao keeps as the type's label once it is made.
				 */
				if (stmt->kind != OBJECT_TYPE)
					break;
				foreach(lc, stmt->definition)
				{
					DefElem    *def = lfirst(lc);

					if (pg_strcasecmp(def->defname, "compresstype") == 0 ||
						pg_strcasecmp(def->defname, "compresslevel") == 0 ||
						pg_strcasecmp(def->defname, "blocksize") == 0)
					{
						type_encoding = lappend(type_encoding,
												makeDefElem(downcase_identifier(def->defname,
																				strlen(def->defname),
																				false, false),
															(Node *) makeString(defGetString(def)),
															-1));
						stmt->definition = foreach_delete_current(stmt->definition, lc);
					}
				}
				break;
			}
		case T_CreateTrigStmt:
			check_trigger((CreateTrigStmt *) parsetree);
			break;
		case T_VacuumStmt:
			/* What a VACUUM that failed compacted, the next one recycles. */
			list_free(ao_vacuum_take_compacted());
			break;
		default:
			break;
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (create != NULL)
		finish_create(create);
	if (type_encoding != NIL)
	{
		DefineStmt *stmt = (DefineStmt *) parsetree;
		ObjectAddress addr;

		addr.classId = TypeRelationId;
		addr.objectId = typenameTypeId(NULL, makeTypeNameFromNameList(stmt->defnames));
		addr.objectSubId = 0;
		SetSecurityLabel(&addr, "gp_ao",
						 ao_encoding_type_label(ao_enc_format(type_encoding)));
		GpDispatchNoteLabelOf(&addr, "gp_ao");
	}
	if (ctas_into != NULL)
	{
		Oid			relid = RangeVarGetRelid(ctas_into->rel, NoLock, true);

		if (OidIsValid(relid) && ctas_method != NULL)
			ao_encoding_apply_given(relid, NIL, ctas_opts);
		else if (OidIsValid(relid) &&
				 get_rel_relam(relid) == get_table_am_oid("ao_column", true))
			ao_encoding_apply(relid, NIL, ctas_opts, NIL, false);
	}
	if (OidIsValid(am_changed))
		finish_set_access_method(am_changed, am_before, am_withopts);
	rewrite_if_options_changed(watch);
	if (alter != NULL)
		finish_alter(RangeVarGetRelid(((AlterTableStmt *) parsetree)->relation,
									  NoLock, true), alter);

	if (IsA(parsetree, VacuumStmt))
		recycle_compacted();
	reset_frozen_xids();
	ao_fetch_cache_reset();
}

/* ------------------------------------------------------------------------- */
/* A dropped table's rows in gp_ao's tables                                  */
/* ------------------------------------------------------------------------- */

/*
 * A table dropped -- or the old files of one a rewrite replaced, which go
 * with the transient relation the rewrite drops -- takes its rows in gp_ao's
 * tables with it, in the same transaction.
 */
static void
gp_ao_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	/* A rewrite's swap among them, which the hook comes before it shows */
	if (access == OAT_POST_ALTER && classId == RelationRelationId && subId == 0)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		altered_rels = list_append_unique_oid(altered_rels, objectId);
		MemoryContextSwitchTo(old);
	}

	if (access == OAT_DROP && classId == RelationRelationId && subId == 0 &&
		relid_is_ao(objectId) &&
		OidIsValid(ao_meta_relid("segfile", true)))
	{
		Relation	rel = relation_open(objectId, NoLock);

		ao_dml_forget_rel(objectId);
		if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind) &&
			smgrexists(RelationGetSmgr(rel), MAIN_FORKNUM) &&
			smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM) > 0)
			ao_meta_delete_storage(ao_storage_id(rel));
		relation_close(rel, NoLock);
	}
}

/* ------------------------------------------------------------------------- */
/* Where statements end                                                      */
/* ------------------------------------------------------------------------- */

/*
 * UPDATE and DELETE of an append-optimized table under a transaction
 * snapshot, which Cloudberry refuses as it starts them
 * (ExecInitModifyTable()): a row has no xmax to say that a transaction the
 * snapshot does not see deleted it, and it would be deleted, or updated,
 * again.
 */
static void
gp_ao_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	PlannedStmt *pstmt = queryDesc->plannedstmt;

	if ((pstmt->commandType == CMD_UPDATE || pstmt->commandType == CMD_DELETE) &&
		IsolationUsesXactSnapshot())
	{
		int			rti = -1;

		while ((rti = bms_next_member(pstmt->resultRelationRelids, rti)) >= 0)
		{
			RangeTblEntry *rte = rt_fetch(rti, pstmt->rtable);

			if (rte->rtekind != RTE_RELATION || !relid_is_ao(rte->relid))
				continue;
			if (pstmt->commandType == CMD_UPDATE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("updates on append-only tables are not supported in serializable transactions")));
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("deletes on append-only tables are not supported in serializable transactions")));
		}
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
gp_ao_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	ao_dml_run_begin(queryDesc);
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		ao_dml_run_end(queryDesc);
	}
	PG_END_TRY();
}

/*
 * A query's ExecutorFinish: what it wrote, finished before its AFTER
 * triggers fire; and the descriptor its fetches by TID kept, dropped as it
 * finishes, and the one its AFTER triggers' kept after (ao_am.c).
 */
static void
gp_ao_ExecutorFinish(QueryDesc *queryDesc)
{
	ao_fetch_cache_reset();
	ao_dml_finish_query(queryDesc);

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
	ao_fetch_cache_reset();
}

/*
 * O30: UPDATE and DELETE hold an append-optimized table in ExclusiveLock,
 * as Cloudberry's do even with the global deadlock detector on, taken as the
 * parser opens the table rather than after its RowExclusiveLock, which two
 * writers would deadlock upgrading.
 */
static LOCKMODE
gp_ao_query_lockmode(Oid relid, LOCKMODE lockmode, AclMode requiredPerms)
{
	if (prev_query_lockmode)
		lockmode = prev_query_lockmode(relid, lockmode, requiredPerms);

	if (lockmode == RowExclusiveLock &&
		(requiredPerms & (ACL_UPDATE | ACL_DELETE)) != 0 &&
		relid_is_ao(relid))
		return ExclusiveLock;
	return lockmode;
}

/*
 * UPDATE or DELETE ... WHERE CURRENT OF a cursor, of an append-optimized
 * table: refused as Cloudberry refuses it, as the statement is analysed,
 * with its words.  A cursor's row of such a table is in no page to find
 * again by its TID as the core's execCurrentOf() would.
 */
static bool
contains_current_of(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, CurrentOfExpr))
		return true;
	return expression_tree_walker(node, contains_current_of, context);
}

static void
gp_ao_post_parse_analyze(ParseState *pstate, Query *query, const JumbleState *jstate)
{
	if (prev_post_parse_analyze)
		prev_post_parse_analyze(pstate, query, jstate);

	/* as Cloudberry's transformInsertStmt() refuses it */
	if (query->commandType == CMD_INSERT && query->onConflict != NULL &&
		query->resultRelation > 0 &&
		relid_is_ao(rt_fetch(query->resultRelation, query->rtable)->relid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("INSERT ON CONFLICT is not supported for appendoptimized relations")));

	if ((query->commandType == CMD_UPDATE || query->commandType == CMD_DELETE) &&
		query->resultRelation > 0 && query->jointree != NULL &&
		contains_current_of(query->jointree->quals, NULL))
	{
		RangeTblEntry *rte = rt_fetch(query->resultRelation, query->rtable);

		if (rte->rtekind == RTE_RELATION && relid_is_ao(rte->relid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" is not simply updatable",
							get_rel_name(rte->relid))));
	}
}

/* ------------------------------------------------------------------------- */
/* ANALYZE's sample                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The rows of an append-optimized table are in no page ANALYZE can pick, so
 * it is sampled here: every live row read, and a reservoir kept by Vitter's
 * algorithm, as acquire_sample_rows() keeps one of the rows of the pages it
 * reads, then sorted by TID, as it sorts its sample for the correlation.
 */
static int
compare_rows(const void *a, const void *b, void *arg)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;

	return ItemPointerCompare(&ha->t_self, &hb->t_self);
}

static int
ao_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows,
					   int targrows, double *totalrows, double *totaldeadrows)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	ReservoirStateData rstate;
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	int			numrows = 0;
	double		liverows = 0;
	double		rowstoskip = -1;
	AoSegfile  *segfiles;
	int			nsegfiles;
	int64		storage_id = ao_storage_id(rel);

	reservoir_init_selection_state(&rstate, targrows);
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		vacuum_delay_point(true);
		if (numrows < targrows)
		{
			rows[numrows] = ExecCopySlotHeapTuple(slot);
			rows[numrows]->t_self = slot->tts_tid;
			numrows++;
		}
		else
		{
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, liverows, targrows);
			if (rowstoskip <= 0)
			{
				int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = ExecCopySlotHeapTuple(slot);
				rows[k]->t_self = slot->tts_tid;
			}
			rowstoskip -= 1;
		}
		liverows += 1;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);

	if (numrows == targrows)
		qsort_interruptible(rows, numrows, sizeof(HeapTuple), compare_rows, NULL);

	*totalrows = liverows;
	*totaldeadrows = 0;
	segfiles = ao_segfiles_read(storage_id, snapshot, &nsegfiles);
	for (int i = 0; i < nsegfiles; i++)
	{
		AoVisimap  *vm = ao_visimap_load(storage_id, segfiles[i].segno, snapshot);

		*totaldeadrows += ao_visimap_count(vm);
	}
	UnregisterSnapshot(snapshot);

	ereport(elevel,
			(errmsg("\"%s\": %.0f live rows and %.0f dead rows; %d rows in sample",
					RelationGetRelationName(rel), liverows, *totaldeadrows,
					numrows)));
	return numrows;
}

/*
 * pg_appendonly.segfilecount, as Cloudberry's ANALYZE counts it
 * (AcquireCountOfSegmentFile()): on a cluster's coordinator, the segments'
 * segment files of a distributed table together, over the number of
 * segments, as each segment counts its own; elsewhere this node's.  Not on a
 * segment sampling for the coordinator's ANALYZE, which asks for the count
 * apart.
 */
static void
ao_note_segfilecount(Relation rel)
{
	const GpCoreApi *core = GpCoreApiLookup();
	int64		storage_id = ao_storage_id(rel);
	int			count = 0;

	if (core != NULL && core->get_role() == GP_ROLE_EXECUTE)
		return;
	if (core != NULL && core->get_role() == GP_ROLE_DISPATCH &&
		!core->is_single_node() &&
		!GpPolicyIsEntry(GpPolicyGet(RelationGetRelid(rel))))
	{
		int			nsegs = core->get_segment_count();
		char	  **values = palloc0_array(char *, nsegs);

		GpDispatchQueryFirstValues(psprintf("SELECT gp_ao.segfile_count(%u)",
											RelationGetRelid(rel)),
								   -1, values);
		for (int i = 0; i < nsegs; i++)
			if (values[i] != NULL)
				count += pg_strtoint32(values[i]);
		count /= nsegs;
	}
	else
		pfree(ao_segfiles_read(storage_id, GetLatestSnapshot(), &count));
	ao_segfilecount_set(storage_id, count);
}

static bool
gp_ao_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
						  BlockNumber *totalpages)
{
	if (ao_is_ao_table(relation) &&
		RELKIND_HAS_STORAGE(relation->rd_rel->relkind))
		ao_note_segfilecount(relation);

	/* A table whose rows are on the segments: gp_core samples them there. */
	if (prev_analyze_sample_rows &&
		prev_analyze_sample_rows(relation, func, totalpages))
		return true;
	if (!ao_is_ao_table(relation))
		return false;
	*func = ao_acquire_sample_rows;
	*totalpages = RelationGetNumberOfBlocks(relation);
	return true;
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_ao_storage_id);
PG_FUNCTION_INFO_V1(gp_ao_options);

Datum
gp_ao_storage_id(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	int64		storage_id = 0;
	bool		isnull = true;

	if (ao_is_ao_table(rel))
	{
		storage_id = ao_storage_id(rel);
		isnull = false;
	}
	relation_close(rel, AccessShareLock);
	if (isnull)
		PG_RETURN_NULL();
	PG_RETURN_INT64(storage_id);
}

Datum
gp_ao_options(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {0};
	AoOptions	opts;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (!ao_is_ao_table(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an append-optimized table",
						RelationGetRelationName(rel))));
	ao_get_options(rel, &opts);
	values[0] = Int32GetDatum(opts.blocksize);
	values[1] = CStringGetTextDatum(ao_compresstype_name(opts.compresstype));
	values[2] = Int32GetDatum(opts.compresslevel);
	values[3] = BoolGetDatum(opts.checksum);
	values[4] = BoolGetDatum(ao_storage_is_columnar(rel));
	relation_close(rel, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

void
_PG_init(void)
{
	/*
	 * A custom WAL resource manager and table access methods' extension
	 * routines can only be registered while the postmaster loads libraries.
	 */
	CB_REQUIRE_PRELOAD("gp_ao");
	CB_REQUIRE_CORE("gp_ao");

	DefineCustomIntVariable("gp.appendonly_compaction_threshold",
							"Percentage of a segment file's rows deleted at which VACUUM compacts it.",
							"Cloudberry calls this gp_appendonly_compaction_threshold.",
							&gp_appendonly_compaction_threshold,
							10, 0, 100,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.appendonly_insert_files",
							"Number of segment files to insert for appendonly table within a transaction.",
							"Cloudberry calls this gp_appendonly_insert_files: an insert spreads its rows over as many segment files, gp.appendonly_insert_files_tuples_range rows at a time, where more than one.",
							&gp_appendonly_insert_files,
							0, 0, 127,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.appendonly_insert_files_tuples_range",
							"Number of rows an insert writes to one segment file before the next, when it writes several.",
							"Cloudberry calls this gp_appendonly_insert_files_tuples_range.",
							&gp_appendonly_insert_files_tuples_range,
							100000, 0, INT_MAX,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomStringVariable("gp.default_storage_options",
							   "Storage options a new append-optimized table has where its statement gives none.",
							   "Cloudberry calls this gp_default_storage_options; it takes blocksize, "
							   "compresstype, compresslevel and checksum, as Cloudberry 7's does.",
							   &gp_default_storage_options,
							   "",
							   PGC_USERSET, 0,
							   ao_default_storage_options_check, NULL, NULL);
	DefineCustomBoolVariable("gp.appendonly_compaction",
							 "Enables compacting segment files during VACUUM commands.",
							 "Cloudberry calls this gp_appendonly_compaction.",
							 &gp_appendonly_compaction,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.predicate_pushdown_sample_rows",
							"Max sample rows during predicate pushdown.",
							"Cloudberry calls this gp_predicate_pushdown_sample_rows: its column "
							"scan tries a scan's quals column by column on a sample of rows, to "
							"order the columns it reads.  Accepted for Cloudberry's scripts: "
							"gp_ao's reads the columns the quals name and leaves them to the "
							"executor, so it samples nothing.",
							&gp_predicate_pushdown_sample_rows,
							10000, 0, INT_MAX,
							PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.select_invisible",
							 "Lets a scan of an append-optimized table return the rows deleted from it.",
							 "Cloudberry calls this gp_select_invisible.  It is for debugging.",
							 &gp_select_invisible,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);
	MarkGUCPrefixReserved("gp_ao");

	ao_options_init();
	ao_encoding_init();
	ao_register_rmgr();
	ao_register_table_ams();
	ao_dml_init();
	bm_init();
	RegisterXactCallback(pending_parents_xact, NULL);
	RegisterSubXactCallback(pending_parents_subxact, NULL);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_ao_ProcessUtility;
	prev_object_access = object_access_hook;
	object_access_hook = gp_ao_object_access;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = gp_ao_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = gp_ao_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = gp_ao_ExecutorFinish;
	prev_analyze_sample_rows = analyze_sample_rows_hook;
	analyze_sample_rows_hook = gp_ao_analyze_sample_rows;
	prev_query_lockmode = query_lockmode_hook;
	query_lockmode_hook = gp_ao_query_lockmode;
	prev_post_parse_analyze = post_parse_analyze_hook;
	post_parse_analyze_hook = gp_ao_post_parse_analyze;
}
