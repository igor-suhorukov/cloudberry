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
 * gp_refresh.c
 *	  A materialized view on a cluster: its rows on the segments, as a
 *	  table's are.
 *
 * Cloudberry distributes a materialized view as it distributes a table --
 * by its DISTRIBUTED BY, or by the key CREATE TABLE AS would choose -- and
 * its REFRESH fills each segment's copy through one plan, whose top writes a
 * transient table on every node, swapped in at the end (matview.c's
 * transientrel_init()).  PostgreSQL 19's REFRESH runs the view's query where
 * it is, into this node's transient table, so on a cluster's coordinator it
 * is two steps, each of them something the port does already:
 *
 *   - REFRESH ... WITH NO DATA, dispatched as DDL is (gp_ddl.c): every node
 *     gives the view an empty file, with the coordinator's OIDs for the
 *     transient table it swaps in, and marks it not populated;
 *   - and then, unless the statement said WITH NO DATA, an INSERT of the
 *     view's query, run as its owner under the restrictions PostgreSQL's
 *     refresh runs it under -- a security-restricted operation, with a safe
 *     search_path -- whose rows go each to its segment by COPY, as any
 *     INSERT's do (gp_modify.c).  PostgreSQL refuses a write into a
 *     materialized view but for the view's maintenance, so the coordinator
 *     opens that (O27) for its INSERT, and each segment, told which view is
 *     being filled (gp_internal.matview_fill()), for the rows -- which it
 *     takes by that COPY into a temporary table, since PostgreSQL's COPY
 *     refuses a materialized view whatever its maintenance says, and moves
 *     into the view by an INSERT, as the view's owner.  The coordinator's
 *     INSERT is the planner's, whose rows reach the segments by COPY: ORCA's
 *     would write them on the segments in a plan of their own.
 *
 * REFRESH ... CONCURRENTLY keeps the view readable while it runs.  It is
 * checked as PostgreSQL checks it -- populated, not WITH NO DATA, a unique
 * index to match rows by -- and locked as PostgreSQL locks it, in
 * ExclusiveLock; and each segment writes only the rows that changed, as
 * PostgreSQL's refresh_by_match_merge() does: once the new rows are all
 * there, it refuses new data with two rows the same, deletes the rows of
 * its own the new data has not, and inserts the new rows it has not --
 * matched by the unique indexes' columns and the whole row's image -- so
 * that a reader sees the old rows, or once the transaction commits the new,
 * and a row that did not change is not written.
 *
 * An incremental view is distributed as any view is, and refreshed so;
 * gp_matview keeps it up to date between refreshes (ivm_cluster.c).
 *
 * Cloudberry sources this file stands in for:
 *	  the transient table and its dispatch in src/backend/commands/matview.c
 *	  (transientrel_init() and the RefreshClause a plan carries)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "rewrite/prs2lock.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_policy.h"
#include "gp_refresh.h"

/*
 * A segment: the view whose rows this transaction's COPY brings, told by
 * gp_internal.matview_fill(), and the subtransaction that was told -- a
 * rollback of it forgets the view, as the coordinator's does.
 */
static Oid	filling = InvalidOid;
static bool filling_concurrently = false;
static SubTransactionId filling_subid = InvalidSubTransactionId;
static bool callbacks_registered = false;

bool
GpRefreshIsDistributed(Oid relid)
{
	return get_rel_relkind(relid) == RELKIND_MATVIEW &&
		!GpPolicyIsEntry(GpPolicyGet(relid));
}

/* ------------------------------------------------------------------------- */
/* The coordinator                                                           */
/* ------------------------------------------------------------------------- */

bool
GpRefreshNeedsFill(RefreshMatViewStmt *stmt)
{
	Oid			relid;

	if (stmt->skipData && !stmt->concurrent)
		return false;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	return OidIsValid(relid) && GpRefreshIsDistributed(relid);
}

/* The view's query, as the rule it keeps it in has it (matview.c). */
static Query *
view_query(Relation matviewRel)
{
	RuleLock   *rules = matviewRel->rd_rules;
	List	   *actions;

	if (!matviewRel->rd_rel->relhasrules || rules == NULL ||
		rules->numLocks != 1 || rules->rules[0]->event != CMD_SELECT)
		elog(ERROR, "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));
	actions = rules->rules[0]->actions;
	if (list_length(actions) != 1)
		elog(ERROR, "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));
	return copyObject(linitial_node(Query, actions));
}

/* A unique index CONCURRENTLY can match rows by (matview.c). */
static bool
has_usable_unique_index(Relation matviewRel)
{
	List	   *indexes = RelationGetIndexList(matviewRel);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexes)
	{
		Relation	indexRel = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index index = indexRel->rd_index;

		if (index->indisunique && index->indimmediate && index->indisvalid &&
			RelationGetIndexPredicate(indexRel) == NIL && index->indnatts > 0)
		{
			found = true;
			for (int i = 0; i < index->indnatts; i++)
				if (index->indkey.values[i] <= 0)
					found = false;
		}
		index_close(indexRel, AccessShareLock);
		if (found)
			break;
	}
	list_free(indexes);
	return found;
}

/*
 * A statement planned and run here as PostgreSQL's refresh runs the view's
 * query (refresh_matview_datafill()), with the statement's snapshot -- not
 * through SPI, whose context line would name the INSERT in an error the
 * view's query raises.  Returns the rows.
 */
static uint64
run_statement(const char *sql)
{
	RawStmt    *raw = linitial_node(RawStmt, pg_parse_query(sql));
	List	   *queries;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	uint64		processed;

	queries = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);
	if (list_length(queries) != 1)
		elog(ERROR, "unexpected rewrite result for REFRESH MATERIALIZED VIEW");
	plan = pg_plan_query(linitial_node(Query, queries), sql,
						 CURSOR_OPT_PARALLEL_OK, NULL, NULL);

	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();
	queryDesc = CreateQueryDesc(plan, sql, GetActiveSnapshot(), InvalidSnapshot,
								None_Receiver, NULL, NULL, 0);
	ExecutorStart(queryDesc, 0);
	ExecutorRun(queryDesc, ForwardScanDirection, 0);
	processed = queryDesc->estate->es_processed;
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);
	FreeQueryDesc(queryDesc);
	PopActiveSnapshot();
	return processed;
}

/*
 * The view's query, inserted into it: the planner's INSERT, as the view's
 * owner, as PostgreSQL's refresh runs the query.  Returns the rows.
 */
static uint64
fill(Relation matviewRel)
{
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	volatile bool opened = false;
	char	   *sql;
	uint64		processed;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(matviewRel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();
	if (GetConfigOption("gp.optimizer", true, false) != NULL)
		(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0,
								 false);

	/* after the search_path is the safe one, so every name is qualified */
	sql = psprintf("INSERT INTO %s %s",
				   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
											  RelationGetRelationName(matviewRel)),
				   pg_get_querydef(view_query(matviewRel), false));

	PG_TRY();
	{
		OpenMatViewIncrementalMaintenanceExternal();
		opened = true;
		processed = run_statement(sql);
		CloseMatViewIncrementalMaintenanceExternal();
		opened = false;
	}
	PG_CATCH();
	{
		if (opened)
			CloseMatViewIncrementalMaintenanceExternal();
		PG_RE_THROW();
	}
	PG_END_TRY();

	AtEOXact_GUC(false, save_nestlevel);
	SetUserIdAndSecContext(save_userid, save_sec_context);
	return processed;
}

static void
tell_segments(Oid relid, const char *phase)
{
	GpDispatchCommand(psprintf("SELECT gp_internal.matview_fill(%u, %s)",
							   relid, quote_literal_cstr(phase)));
}

void
GpRefreshMatView(PlannedStmt *pstmt, const char *queryString,
				 ProcessUtilityContext context, ParamListInfo params,
				 QueryEnvironment *queryEnv, QueryCompletion *qc)
{
	RefreshMatViewStmt *stmt = castNode(RefreshMatViewStmt, pstmt->utilityStmt);
	Relation	matviewRel;
	Oid			relid;
	uint64		processed;

	if (!stmt->concurrent)
	{
		PlannedStmt *empty = copyObject(pstmt);

		/* every node's copy emptied, and marked not populated */
		castNode(RefreshMatViewStmt, empty->utilityStmt)->skipData = true;
		ProcessUtility(empty, queryString, false, PROCESS_UTILITY_SUBCOMMAND,
					   params, queryEnv, None_Receiver, NULL);
		CommandCounterIncrement();

		/* the statement's lock is held; the name is the one it refreshed */
		relid = RangeVarGetRelid(stmt->relation, NoLock, false);
		matviewRel = table_open(relid, NoLock);
		SetMatViewPopulatedState(matviewRel, true);
		CommandCounterIncrement();
		tell_segments(relid, "plain");
	}
	else
	{
		/* PostgreSQL's lock and checks of a concurrent refresh (matview.c) */
		relid = RangeVarGetRelidExtended(stmt->relation, ExclusiveLock, 0,
										 RangeVarCallbackMaintainsTable, NULL);
		matviewRel = table_open(relid, NoLock);
		if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a materialized view",
							RelationGetRelationName(matviewRel))));
		if (!RelationIsPopulated(matviewRel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s cannot be used when the materialized view is not populated",
							"CONCURRENTLY")));
		if (stmt->skipData)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s options cannot be used together",
							"CONCURRENTLY", "WITH NO DATA")));
		if (!has_usable_unique_index(matviewRel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently",
							quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
													   RelationGetRelationName(matviewRel))),
					 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));
		tell_segments(relid, "concurrent");
	}

	processed = fill(matviewRel);
	tell_segments(relid, "done");
	table_close(matviewRel, NoLock);

	if (qc)
		SetQueryCompletion(qc, CMDTAG_REFRESH_MATERIALIZED_VIEW, processed);
}

/*
 * As a plan starts on the coordinator: a view read from the segments that is
 * not populated is refused, as PostgreSQL refuses a scan of it
 * (ExecOpenScanRelation()), and not by each segment in words of its own.
 */
void
GpRefreshCheckScannable(PlannedStmt *stmt, int eflags)
{
	ListCell   *lc;

	if ((eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA)) != 0)
		return;
	foreach(lc, stmt->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		Relation	rel;

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_MATVIEW ||
			!GpRefreshIsDistributed(rte->relid))
			continue;
		rel = table_open(rte->relid, NoLock);
		if (!RelationIsScannable(rel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("materialized view \"%s\" has not been populated",
							RelationGetRelationName(rel)),
					 errhint("Use the REFRESH MATERIALIZED VIEW command.")));
		table_close(rel, NoLock);
	}
}

/* ------------------------------------------------------------------------- */
/* A segment                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * PostgreSQL's COPY refuses a materialized view whatever its maintenance
 * says, so a segment takes the view's rows into a temporary table of the
 * view's owner, made for the refresh -- the COPY the coordinator sends is
 * pointed at it -- and moves them into the view once they are all there, by
 * an INSERT under the view's maintenance.
 */
static char *
staging_name(Oid relid)
{
	return psprintf("gp_matview_fill_%u", relid);
}

/*
 * Run a statement as the view's owner, as PostgreSQL's refresh writes the
 * view -- with its maintenance open, where it writes the view.
 */
static void
run_as_owner(Relation rel, const char *sql, int expected, bool maintenance)
{
	Oid			save_userid;
	int			save_sec_context;
	volatile bool opened = false;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (maintenance)
		{
			OpenMatViewIncrementalMaintenanceExternal();
			opened = true;
		}
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql, false, 0) != expected)
			elog(ERROR, "could not refresh materialized view \"%s\": %s",
				 RelationGetRelationName(rel), sql);
		SPI_finish();
		if (maintenance)
		{
			CloseMatViewIncrementalMaintenanceExternal();
			opened = false;
		}
	}
	PG_CATCH();
	{
		if (opened)
			CloseMatViewIncrementalMaintenanceExternal();
		SetUserIdAndSecContext(save_userid, save_sec_context);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SetUserIdAndSecContext(save_userid, save_sec_context);
}

static void
fill_xact_callback(XactEvent event, void *arg)
{
	filling = InvalidOid;
	filling_subid = InvalidSubTransactionId;
}

static void
fill_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					  SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && filling_subid == mySubid)
	{
		filling = InvalidOid;
		filling_subid = InvalidSubTransactionId;
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB && filling_subid == mySubid)
		filling_subid = parentSubid;
}

/*
 * The rows this segment has of the view made the new ones, writing only
 * those that changed: PostgreSQL's refresh_by_match_merge() (matview.c), on
 * the rows the coordinator sent into the staging table.  New data with two
 * rows the same, NULLs apart, is refused, in PostgreSQL's words; then the
 * rows of the view the new data has not are deleted, and the new rows the
 * view has not inserted -- the same rows, by the columns of the view's
 * unique indexes, with their opclasses' equality, and by the whole row's
 * image, *=, as PostgreSQL matches them.
 */
static void
merge_changed_rows(Relation rel, const char *view, const char *staging)
{
	StringInfoData match;
	List	   *indexes = RelationGetIndexList(rel);
	TupleDesc	desc = RelationGetDescr(rel);
	Oid		   *used = palloc0_array(Oid, desc->natts);
	Oid			save_userid;
	int			save_sec_context;
	volatile bool opened = false;

	/* the columns of every unique index a row can be matched by */
	initStringInfo(&match);
	foreach_oid(indexoid, indexes)
	{
		Relation	indexRel = index_open(indexoid, AccessShareLock);
		Form_pg_index index = indexRel->rd_index;
		bool		usable = index->indisunique && index->indimmediate &&
			index->indisvalid && RelationGetIndexPredicate(indexRel) == NIL &&
			index->indnkeyatts > 0;

		for (int i = 0; usable && i < index->indnkeyatts; i++)
			usable = index->indkey.values[i] > 0;
		for (int i = 0; usable && i < index->indnkeyatts; i++)
		{
			AttrNumber	attnum = index->indkey.values[i];
			Form_pg_attribute attr = TupleDescAttr(desc, attnum - 1);
			Oid			opfamily = indexRel->rd_opfamily[i];
			Oid			opcintype = indexRel->rd_opcintype[i];
			Oid			op = get_opfamily_member_for_cmptype(opfamily, opcintype,
															 opcintype, COMPARE_EQ);

			if (!OidIsValid(op) || used[attnum - 1] == op)
				continue;
			used[attnum - 1] = op;
			generate_operator_clause(&match,
									 quote_qualified_identifier("newdata", NameStr(attr->attname)),
									 attr->atttypid, op,
									 quote_qualified_identifier("mv", NameStr(attr->attname)),
									 attr->atttypid);
			appendStringInfoString(&match, " AND ");
		}
		index_close(indexRel, AccessShareLock);
	}
	list_free(indexes);
	appendStringInfoString(&match, "newdata.* OPERATOR(pg_catalog.*=) mv.*");

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(psprintf("ANALYZE %s", staging), false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not analyze the new rows of \"%s\"",
				 RelationGetRelationName(rel));
		if (SPI_execute(psprintf("SELECT newdata.*::%s FROM %s newdata"
								 " WHERE newdata.* IS NOT NULL AND EXISTS"
								 " (SELECT 1 FROM %s newdata2 WHERE newdata2.* IS NOT NULL"
								 " AND newdata2.* OPERATOR(pg_catalog.*=) newdata.*"
								 " AND newdata2.ctid OPERATOR(pg_catalog.<>) newdata.ctid)",
								 staging, staging, staging),
						false, 1) != SPI_OK_SELECT)
			elog(ERROR, "could not check the new rows of \"%s\"",
				 RelationGetRelationName(rel));
		if (SPI_processed > 0)
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
							RelationGetRelationName(rel)),
					 errdetail("Row: %s",
							   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));

		OpenMatViewIncrementalMaintenanceExternal();
		opened = true;
		if (SPI_execute(psprintf("DELETE FROM %s mv WHERE NOT EXISTS"
								 " (SELECT 1 FROM %s newdata WHERE %s)",
								 view, staging, match.data),
						false, 0) != SPI_OK_DELETE ||
			SPI_execute(psprintf("INSERT INTO %s SELECT newdata.* FROM %s newdata"
								 " WHERE NOT EXISTS (SELECT 1 FROM %s mv WHERE %s)",
								 view, staging, view, match.data),
						false, 0) != SPI_OK_INSERT)
			elog(ERROR, "could not refresh materialized view \"%s\" concurrently",
				 RelationGetRelationName(rel));
		CloseMatViewIncrementalMaintenanceExternal();
		opened = false;
		SPI_finish();
	}
	PG_FINALLY();
	{
		if (opened)
			CloseMatViewIncrementalMaintenanceExternal();
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
}

PG_FUNCTION_INFO_V1(gp_matview_fill);

/*
 * gp_internal.matview_fill(view, phase)
 *
 * The coordinator, telling a segment that the COPY it sends next brings a
 * materialized view its rows: "plain" after REFRESH ... WITH NO DATA has
 * emptied it, which marks it populated, and "concurrent", which deletes its
 * rows instead; and "done", after the rows, which puts them in.  Only from a
 * connection that carries the cluster secret, and for a role that may
 * refresh the view.
 */
Datum
gp_matview_fill(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *phase = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		concurrent = strcmp(phase, "concurrent") == 0;
	bool		done = strcmp(phase, "done") == 0;
	char	   *view;
	char	   *staging;
	Relation	rel;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only the coordinator fills a materialized view on a segment")));
	if (!concurrent && !done && strcmp(phase, "plain") != 0)
		elog(ERROR, "unrecognized phase \"%s\"", phase);
	if (done && filling != relid)
		elog(ERROR, "materialized view %u is not being filled", relid);

	/* the statement's own lock, as the coordinator holds it */
	rel = table_open(relid, concurrent ? ExclusiveLock : AccessExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(rel))));
	if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_MATVIEW,
					   RelationGetRelationName(rel));
	view = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
									  RelationGetRelationName(rel));
	staging = quote_qualified_identifier("pg_temp", staging_name(relid));

	if (done)
	{
		if (filling_concurrently)
			merge_changed_rows(rel, view, staging);
		else
			run_as_owner(rel, psprintf("INSERT INTO %s SELECT * FROM %s", view, staging),
						 SPI_OK_INSERT, true);
		run_as_owner(rel, psprintf("DROP TABLE %s", staging), SPI_OK_UTILITY, false);
		filling = InvalidOid;
		filling_subid = InvalidSubTransactionId;
		table_close(rel, NoLock);
		PG_RETURN_VOID();
	}

	if (!concurrent)
		SetMatViewPopulatedState(rel, true);
	run_as_owner(rel, psprintf("CREATE TEMP TABLE %s (LIKE %s) USING heap",
							   staging, view),
				 SPI_OK_UTILITY, false);
	table_close(rel, NoLock);

	if (!callbacks_registered)
	{
		RegisterXactCallback(fill_xact_callback, NULL);
		RegisterSubXactCallback(fill_subxact_callback, NULL);
		callbacks_registered = true;
	}
	filling = relid;
	filling_concurrently = concurrent;
	filling_subid = GetCurrentSubTransactionId();
	PG_RETURN_VOID();
}

RangeVar *
GpRefreshFillTarget(CopyStmt *stmt)
{
	if (!OidIsValid(filling) || !stmt->is_from || stmt->relation == NULL ||
		stmt->filename != NULL ||
		RangeVarGetRelid(stmt->relation, NoLock, true) != filling ||
		!GpClusterDispatchTrusted())
		return NULL;
	return makeRangeVar("pg_temp", staging_name(filling), -1);
}

void
GpRefreshRunCopy(void (*run) (void *arg), void *arg)
{
	Oid			save_userid;
	int			save_sec_context;
	Relation	rel = table_open(filling, NoLock);
	Oid			owner = rel->rd_rel->relowner;

	table_close(rel, NoLock);
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(owner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		run(arg);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
}
