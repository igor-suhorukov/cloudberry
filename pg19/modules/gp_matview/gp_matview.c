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
 * gp_matview.c
 *	  Incremental materialized views, dynamic tables and AQUMV bookkeeping.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/optimizer/plan/aqumv.c, catalog/gp_matview_aux.c,
 *	  and the incremental-view code of matview.c and createas.c
 *
 * It carries the incremental views -- on one node, and on a cluster, where
 * the coordinator keeps them up to date (ivm_cluster.c) -- and dynamic
 * tables.  The AQUMV bookkeeping follows; see cloudberry.md, "Milestones".
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "commands/createas.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_matview.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_matview",
					.version = GP_VERSION
);

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static star_expansion_filter_hook_type prev_star_filter = NULL;
static object_access_hook_type prev_object_access = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

/*
 * Was gp_sql loaded before this module, so that its ProcessUtility hook runs
 * inside this one's and reads a CREATE's options after this one has added
 * to them?  See add_cluster_distribution().
 */
static bool gp_sql_inside = false;

/* ------------------------------------------------------------------------- */
/* O28: the hidden columns stay out of "*"                                   */
/* ------------------------------------------------------------------------- */

static Bitmapset *
gp_matview_star_filter(Oid relid)
{
	Bitmapset  *omitted = NULL;
	Relation	rel;

	if (prev_star_filter)
		omitted = prev_star_filter(relid);

	if (get_rel_relkind(relid) != RELKIND_MATVIEW)
		return omitted;

	rel = RelationIdGetRelation(relid);
	if (!RelationIsValid(rel))
		return omitted;

	for (int i = 0; i < RelationGetNumberOfAttributes(rel); i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);

		if (att->attisdropped)
			continue;
		if (IsIvmColumn(NameStr(att->attname)))
			omitted = bms_add_member(omitted, att->attnum);
	}

	RelationClose(rel);
	return omitted;
}

/* ------------------------------------------------------------------------- */
/* Dropping one                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry makes a dynamic table's refresh task an internal dependency of
 * the view, so that dropping one drops the other.  A job here is a row in
 * gp_task's table rather than a catalog object, so this stands in for that
 * dependency.  The triggers and the label of an incremental view need nothing
 * here: those are real dependencies and PostgreSQL drops them itself.
 */
static void
gp_matview_object_access(ObjectAccessType access, Oid classId, Oid objectId,
						 int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (access != OAT_DROP || classId != RelationRelationId || subId != 0)
		return;
	if (get_rel_relkind(objectId) != RELKIND_MATVIEW)
		return;

	GpDynDropped(objectId);
}

/* ------------------------------------------------------------------------- */
/* A cluster's coordinator keeps its incremental views up to date            */
/* ------------------------------------------------------------------------- */

/*
 * Once a statement's executor has finished, the incremental views over the
 * tables it wrote are brought up to date (ivm_cluster.c): on a cluster the
 * rows were written on the segments, whose triggers kept what changed for
 * the coordinator.  On one node the triggers did it themselves, and this
 * does nothing.
 */
static void
gp_matview_ExecutorFinish(QueryDesc *queryDesc)
{
	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);

	if (!(queryDesc->estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY))
		GpIvmClusterStatementEnd(queryDesc->plannedstmt);
}

/*
 * The run of the rest of the chain, and then, on a cluster's coordinator,
 * the incremental views a utility statement changed brought up to date:
 * those over the table a COPY FROM wrote, and those a TRUNCATE's triggers,
 * which fire here too, kept something for.
 */
static void
run_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
			ProcessUtilityContext context, ParamListInfo params,
			QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	Oid			copied = InvalidOid;

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (IsA(parsetree, CopyStmt) && ((CopyStmt *) parsetree)->is_from &&
		((CopyStmt *) parsetree)->relation != NULL)
		copied = RangeVarGetRelid(((CopyStmt *) parsetree)->relation, NoLock, true);
	GpIvmClusterUtilityEnd(copied);
}

/*
 * The distribution an incremental view on a cluster is made with, where its
 * statement names none: added to the statement's options as a DISTRIBUTED
 * BY would have been, which gp_sql takes out and records once the view is
 * made (GpIvmClusterDefaultDistribution() says which).  Only where gp_sql
 * reads the options after this module has, which it does when it was loaded
 * first; otherwise the view gets gp_sql's own choice, and is refused after
 * it is made if its rows could not be maintained by it.
 */
static void
add_cluster_distribution(IntoClause *into, Query *rewritten)
{
	const GpCoreApi *core = GpCoreApiLookup();
	ListCell   *lc;

	if (!gp_sql_inside || core == NULL || core->is_single_node() ||
		core->get_role() != GP_ROLE_DISPATCH)
		return;
	foreach(lc, into->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace != NULL && strcmp(def->defnamespace, "gp") == 0 &&
			strcmp(def->defname, "distributed_by") == 0)
			return;
	}
	into->options = lappend(into->options,
							makeDefElemExtended("gp", "distributed_by",
												(Node *) makeString(GpIvmClusterDefaultDistribution(rewritten,
																								  into->colNames)),
												DEFELEM_UNSPEC, -1));
}

/*
 * On a cluster's coordinator, an incremental view just made or restored:
 * its distribution is one its deltas can be sent by, and the segments' base
 * tables get its triggers too.
 */
static void
cluster_incremental(Oid matviewOid, Query *rewritten)
{
	const GpCoreApi *core = GpCoreApiLookup();

	if (core == NULL || core->is_single_node() ||
		core->get_role() != GP_ROLE_DISPATCH)
		return;
	GpIvmClusterCheckDistribution(matviewOid, rewritten);
	GpIvmClusterMakeTriggers(matviewOid);
}

/* ------------------------------------------------------------------------- */
/* The create path                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Is one of this module's options in the list?
 *
 * Asked before anything is taken out of it, because taking one out is a
 * change to the statement, and a statement that arrives with readOnlyTree may
 * not be changed.  It builds the dotted name the same way the two take
 * functions do, because the parser splits "gp.incremental" into a namespace
 * and a name and only the pair means anything.
 */
static bool
has_matview_option(List *options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		char	   *name;
		bool		ours;

		name = def->defnamespace != NULL
			? psprintf("%s.%s", def->defnamespace, def->defname)
			: pstrdup(def->defname);

		ours = (strcmp(name, GP_IVM_OPTION) == 0 ||
				strcmp(name, GP_DYN_OPTION) == 0);
		pfree(name);

		if (ours)
			return true;
	}

	return false;
}

/*
 * CREATE MATERIALIZED VIEW ... WITH (gp.incremental) AS <query>
 *
 * The option is taken out here, before transformRelOptions would reject it,
 * and the query is checked and rewritten before the view is created, so that
 * the view is made with the hidden columns already in it.
 */
static void
gp_matview_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	CreateTableAsStmt *ctas = NULL;
	bool		incremental = false;
	bool		dynamic = false;
	char	   *schedule = NULL;
	Query	   *rewritten = NULL;

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
	 * SECURITY LABEL on a materialized view, as a restore of pg_dump's output
	 * labels one: what the label says the view is, it is made -- an
	 * incremental view's triggers, on a cluster's segments too, a dynamic
	 * table's job -- where it is not yet.
	 */
	if (IsA(parsetree, SecLabelStmt) &&
		((SecLabelStmt *) parsetree)->objtype == OBJECT_MATVIEW &&
		((SecLabelStmt *) parsetree)->provider != NULL &&
		strcmp(((SecLabelStmt *) parsetree)->provider, "gp") == 0)
	{
		Oid			relid;

		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		CommandCounterIncrement();
		relid = RangeVarGetRelid(makeRangeVarFromNameList(castNode(List, ((SecLabelStmt *) parsetree)->object)),
								 NoLock, false);
		GpIvmRestored(relid);
		if (GpIvmIsIncremental(relid))
		{
			Relation	rel = table_open(relid, AccessShareLock);

			cluster_incremental(relid, GpIvmGetViewQuery(rel));
			table_close(rel, AccessShareLock);
		}
		GpDynRestored(relid);
		return;
	}

	if (IsA(parsetree, CreateTableAsStmt))
	{
		ctas = (CreateTableAsStmt *) parsetree;

		if (ctas->objtype == OBJECT_MATVIEW && ctas->into != NULL &&
			has_matview_option(ctas->into->options))
		{
			/*
			 * Everything below this point changes the statement: the option
			 * comes out of the option list, and for an incremental view both
			 * copies of the query are replaced by rewritten ones.  When
			 * readOnlyTree says the tree is not ours to change -- it belongs
			 * to a plan cache -- change a copy instead, and tell the rest of
			 * the chain that the copy is writable.  gp_sql does the same.
			 *
			 * Nothing observable depends on this today, and it is worth
			 * saying why rather than leaving someone to find out: for a
			 * saved plan BuildCachedPlan copies query_list before planning
			 * (pg19/src/backend/utils/cache/plancache.c:1072-1082), and DDL
			 * invalidates cached plans in any case, so the tree that reaches
			 * this hook is freshly parsed every time.  What is being kept
			 * here is the contract, not that accident.
			 */
			if (readOnlyTree)
			{
				pstmt = copyObject(pstmt);
				parsetree = pstmt->utilityStmt;
				ctas = (CreateTableAsStmt *) parsetree;
				readOnlyTree = false;
			}

			incremental = GpIvmTakeOption(&ctas->into->options);
			dynamic = GpDynTakeOption(&ctas->into->options, &schedule);
		}
	}

	if (!incremental && !dynamic)
	{
		run_utility(pstmt, queryString, readOnlyTree, context, params,
					queryEnv, dest, qc);
		return;
	}

	if (incremental)
	{
		/* What the view is made of has to be something maintenance can follow. */
		GpIvmCheckQuery((Query *) ctas->query);

		/*
		 * Both copies of the query have to gain the hidden columns:
		 * ctas->query is what fills the view, and into->viewQuery is what
		 * becomes its rule.  Rewriting only one leaves the rule and the
		 * relation disagreeing on how many columns there are.
		 */
		rewritten = GpIvmRewriteQuery((Query *) ctas->query, ctas->into->colNames);
		ctas->query = (Node *) rewritten;
		if (ctas->into->viewQuery != NULL)
			ctas->into->viewQuery =
				GpIvmRewriteQuery((Query *) ctas->into->viewQuery, ctas->into->colNames);

		/* on a cluster, rows its deltas can be sent to their segments by */
		add_cluster_distribution(ctas->into, rewritten);
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	/*
	 * The view exists now, so it can be labelled and its base tables given
	 * the triggers that keep it up to date.
	 */
	{
		Oid			matviewOid = RangeVarGetRelid(ctas->into->rel, NoLock, false);

		if (incremental)
		{
			GpIvmAfterCreate(matviewOid, rewritten);
			cluster_incremental(matviewOid, rewritten);
		}
		if (dynamic)
			GpDynAfterCreate(matviewOid, schedule);
	}
}

void
_PG_init(void)
{
	/*
	 * This module reads CREATE MATERIALIZED VIEW before PostgreSQL does, so
	 * its ProcessUtility hook has to be in place before any statement runs.
	 * A module loaded on demand is loaded when one of its functions is first
	 * called, which is far too late: the first CREATE ... WITH (gp.incremental)
	 * of a session would be rejected by transformRelOptions instead.
	 */
	CB_REQUIRE_PRELOAD("gp_matview");
	CB_REQUIRE_CORE("gp_matview");

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_matview_ProcessUtility;

	prev_star_filter = star_expansion_filter_hook;
	star_expansion_filter_hook = gp_matview_star_filter;

	prev_object_access = object_access_hook;
	object_access_hook = gp_matview_object_access;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = gp_matview_ExecutorFinish;

	/* gp_sql defines its settings as it loads */
	gp_sql_inside = GetConfigOption("gp.create_table_random_default_distribution",
									true, false) != NULL;
}
