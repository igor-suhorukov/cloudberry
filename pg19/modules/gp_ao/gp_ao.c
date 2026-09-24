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

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_inherits.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_trigger.h"
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
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/sortsupport.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_ao.h"

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
 * Cloudberry's gp_select_invisible, for an append-optimized table: a scan
 * or a fetch returns the rows deleted too, as the visibility map had none.
 */
bool		gp_select_invisible = false;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;
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
} CreatePending;

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

	if (cp->encodings != NIL && (am == NULL || strcmp(am, "ao_column") != 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));
	if (!am_name_is_ao(am))
		return cp;

	cp->own_opts = ao_storage_opts_of(stmt->options);
	if (OidIsValid(parentid) && get_rel_relam(parentid) == get_table_am_oid(am, true))
		ao_partition_inherit(parentid, &stmt->options);
	ao_default_storage_options_add(&stmt->options);
	if (cp->partitioned)
		cp->parent_opts = ao_partitioned_take(&stmt->options);
	return cp;
}

static void
finish_create(CreatePending *cp)
{
	Oid			relid = RangeVarGetRelid(cp->relation, NoLock, true);

	if (!OidIsValid(relid) || !relid_is_ao(relid))
		return;
	if (cp->partitioned)
		ao_partitioned_set(relid, cp->parent_opts);
	if (get_rel_relam(relid) == get_table_am_oid("ao_column", true))
		ao_encoding_apply(relid, cp->encodings, cp->own_opts, NIL, false);
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

	if (!OidIsValid(relid) || !OidIsValid(aocol))
		return;
	relids = list_make1_oid(relid);
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
		relids = find_all_inheritors(relid, NoLock, NULL);

	if (ap->colnames != NIL && get_rel_relam(relid) != aocol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ENCODING clause only supported with column oriented tables")));

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

static void
gp_ao_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					 bool readOnlyTree, ProcessUtilityContext context,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	CreatePending *create = NULL;
	AlterPending *alter = NULL;
	IntoClause *ctas_into = NULL;
	List	   *ctas_opts = NIL;

	if (readOnlyTree &&
		(IsA(parsetree, CreateStmt) || IsA(parsetree, CreateTableAsStmt) ||
		 IsA(parsetree, AlterTableStmt) || IsA(parsetree, SecLabelStmt)))
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
				break;
			}
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				alter = prepare_alter(stmt);
				if (stmt->cmds == NIL)
				{
					/* Nothing left for PostgreSQL: the table, as it would. */
					Oid			relid = AlterTableLookupRelation(stmt, AccessExclusiveLock);

					finish_alter(relid, alter);
					return;
				}
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
	if (ctas_into != NULL)
	{
		Oid			relid = RangeVarGetRelid(ctas_into->rel, NoLock, true);

		if (OidIsValid(relid) &&
			get_rel_relam(relid) == get_table_am_oid("ao_column", true))
			ao_encoding_apply(relid, NIL, ctas_opts, NIL, false);
	}
	if (alter != NULL)
		finish_alter(RangeVarGetRelid(((AlterTableStmt *) parsetree)->relation,
									  NoLock, true), alter);

	/*
	 * VACUUM's last phase, as Cloudberry's post-cleanup: the segment files
	 * it compacted, recycled in the transaction VACUUM leaves for its caller
	 * to commit, if no snapshot still sees them -- which, when no older
	 * transaction runs beside it, none does.
	 */
	if (IsA(parsetree, VacuumStmt))
	{
		List	   *compacted = ao_vacuum_take_compacted();

		foreach_oid(relid, compacted)
			ao_vacuum_recycle_rel(relid);
		list_free(compacted);
	}
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

static void
gp_ao_ExecutorFinish(QueryDesc *queryDesc)
{
	ao_dml_finish_query(queryDesc);

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
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

static bool
gp_ao_analyze_sample_rows(Relation relation, AnalyzeSampleRowsFunc *func,
						  BlockNumber *totalpages)
{
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

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_ao_ProcessUtility;
	prev_object_access = object_access_hook;
	object_access_hook = gp_ao_object_access;
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
