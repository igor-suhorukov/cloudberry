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
 * At this milestone it carries the incremental views.  Dynamic tables and the
 * AQUMV bookkeeping follow; see cloudberry.md, "Milestones".
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/createas.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_matview.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_matview",
					.version = GP_VERSION
);

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static star_expansion_filter_hook_type prev_star_filter = NULL;

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
/* The create path                                                           */
/* ------------------------------------------------------------------------- */

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
	Query	   *rewritten = NULL;

	if (IsA(parsetree, CreateTableAsStmt))
	{
		ctas = (CreateTableAsStmt *) parsetree;

		if (ctas->objtype == OBJECT_MATVIEW && ctas->into != NULL)
			incremental = GpIvmTakeOption(&ctas->into->options);
	}

	if (!incremental)
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		return;
	}

	/* What the view is made of has to be something maintenance can follow. */
	GpIvmCheckQuery((Query *) ctas->query);

	/*
	 * Both copies of the query have to gain the hidden columns: ctas->query
	 * is what fills the view, and into->viewQuery is what becomes its rule.
	 * Rewriting only one leaves the rule and the relation disagreeing on how
	 * many columns there are.
	 */
	rewritten = GpIvmRewriteQuery((Query *) ctas->query, ctas->into->colNames);
	ctas->query = (Node *) rewritten;
	if (ctas->into->viewQuery != NULL)
		ctas->into->viewQuery =
			GpIvmRewriteQuery((Query *) ctas->into->viewQuery, ctas->into->colNames);

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

		GpIvmAfterCreate(matviewOid, rewritten);
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
}
