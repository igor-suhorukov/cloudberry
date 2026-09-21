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
 * Portions Copyright (c) 2010-Present, VMware, Inc. or its affiliates
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * The notice above is that of Cloudberry's orca.c, which this file is
 * ported from.
 *
 * orca.c
 *	  The way in to ORCA from gp_orca's planner hook: what is done to a
 *	  query before ORCA sees it, and to the plan after.
 *
 * Ported from github/cloudberry/src/backend/optimizer/plan/orca.c, and
 * changed for PostgreSQL 19; a comment beside each change, or beside what
 * replaced it, says what and why.  In outline:
 *
 *	 * PostgreSQL 18 changed two things about the query the planner is handed
 *	   that ORCA's translator, written against PostgreSQL 16, cannot see: the
 *	   grouping step is a range table entry, and a column can be a virtual
 *	   generated one.  The first is undone here the way the planner undoes
 *	   it; the second is refused, because undoing it needs what ORCA lacks.
 *
 *	 * Cloudberry's fold_constants() is a mode of its own patched
 *	   eval_const_expressions().  PostgreSQL 19's is called instead, level by
 *	   level, with Cloudberry's one exception kept.
 *
 *	 * The plan's permission checks and dependencies are completed from the
 *	   query, because ORCA's translator knows only the tables it scans.
 *
 *	 * ShareInputScan post-processing, remove_subquery_in_RTEs and
 *	   transformGroupedWindows() are not here yet: the first and last arrive
 *	   with CTEs and window functions (T1), and the second exists to make a
 *	   plan smaller to dispatch (M2).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_orca_api.h"
#include "gp_orca_guc.h"
#include "optimizer/orca.h"
#include "optimizer/walkers.h"

static Plan *remove_redundant_results(Plan *plan);
static bool can_replace_tlist(Plan *plan);
static Node *push_down_expr_mutator(Node *node, List *child_tlist);

/*
 * An error that left ORCA without unwinding it.
 *
 * Everything ORCA calls in PostgreSQL goes through the wrapper layer, which
 * turns an error into an ORCA exception so that ORCA unwinds its own frames.
 * Something that raised past it -- a call the wrapper layer does not cover --
 * would longjmp through ORCA instead, and leave behind what those frames
 * would have undone: the worker gpos_exec registered, which makes every later
 * call in this backend fail with "Worker is already registered", and the
 * metadata cache in whatever state the query left it.  Nothing in the port is
 * known to do that, and if something does, this backend stops asking ORCA
 * rather than finding out what the state is.  Cloudberry has the same
 * exposure and resets the worker's error context instead, which with a stale
 * worker is a write through a task that no longer exists.
 */
static bool orca_state_unknown = false;

/*
 * Logging of optimization outcome
 */
static void
log_optimizer(PlannedStmt *plan, bool fUnexpectedFailure)
{
	/* optimizer logging is not enabled */
	if (!optimizer_log)
		return;

	if (plan != NULL)
	{
		elog(DEBUG1, "GPORCA produced plan");
		return;
	}

	/* optimizer failed to produce a plan, log failure */
	if ((OPTIMIZER_ALL_FAIL == optimizer_log_failure) ||
		(fUnexpectedFailure && OPTIMIZER_UNEXPECTED_FAIL == optimizer_log_failure) || 		/* unexpected fall back */
		(!fUnexpectedFailure && OPTIMIZER_EXPECTED_FAIL == optimizer_log_failure))			/* expected fall back */
	{
		if (fUnexpectedFailure)
		{
			elog(LOG, "GPORCA failed to produce plan (unexpected)");
		}
		else
		{
			elog(LOG, "GPORCA failed to produce plan");
		}
		return;
	}
}

/*
 * support_functions_check_context
 *		Context structure for checking support functions, similar to eval_const_expressions_context
 */
typedef struct
{
	PlannerInfo *root;
	bool		recurse_queries;		/* recurse into query structures */
	bool		recurse_sublink_testexpr; /* recurse into sublink test expressions */
} support_functions_check_context;

/*
 * check_support_functions_walker
 *		Walker function to check if a query tree contains functions with support functions
 *		Uses the same traversal pattern as eval_const_expressions_mutator but as a walker
 */
static bool
check_support_functions_walker(Node *node, support_functions_check_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;

		if (context->recurse_queries)
		{
			/* Recurse into Query structures like fold_constants does */
			return query_tree_walker(query, check_support_functions_walker, context, 0);
		}
		return false;
	}

	/* Handle SubLink like eval_const_expressions_mutator does */
	if (IsA(node, SubLink) && !context->recurse_sublink_testexpr)
	{
		SubLink *sublink = (SubLink *) node;

		/*
		 * Also invoke the walker on the sublink's Query node, so it
		 * can recurse into the sub-query if it wants to.
		 */
		return query_tree_walker((Query *) sublink->subselect, check_support_functions_walker, context, 0);
	}

	/* Check both FuncExpr and OpExpr nodes for prosupport functions */
	if (IsA(node, FuncExpr) || IsA(node, OpExpr))
	{
		HeapTuple	proctup;
		Form_pg_proc procform;
		bool		has_support = false;
		Oid			funcid;
		const char	*exprtype;

		/* Extract function OID from either FuncExpr or OpExpr */
		if (IsA(node, FuncExpr))
		{
			FuncExpr *funcexpr = (FuncExpr *) node;
			funcid = funcexpr->funcid;
			exprtype = "FuncExpr";
		}
		else /* OpExpr */
		{
			OpExpr *opexpr = (OpExpr *) node;
			funcid = opexpr->opfuncid;
			exprtype = "OpExpr";
		}

		proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
		if (HeapTupleIsValid(proctup))
		{
			procform = (Form_pg_proc) GETSTRUCT(proctup);
			has_support = OidIsValid(procform->prosupport);
			/* Skip pg_catalog namespace support functions - they are safe */
			if (has_support && procform->pronamespace != PG_CATALOG_NAMESPACE)
			{
				elog(DEBUG1, "Found %s with non-pg_catalog prosupport function, falling back to standard planner", exprtype);
				ReleaseSysCache(proctup);
				return true;
			}
			ReleaseSysCache(proctup);
		}
	}

	return expression_tree_walker(node, check_support_functions_walker, context);
}

/*
 * query_contains_support_functions
 *		Check if a query contains function calls that have support functions
 */
static bool
query_contains_support_functions(Query *query)
{
	support_functions_check_context context;

	context.root = NULL;  /* We don't need PlannerInfo for this check */
	context.recurse_queries = true; /* recurse into query structures */
	context.recurse_sublink_testexpr = false; /* do not recurse into sublink test expressions */

	/* Use the same traversal pattern as fold_constants but with a walker */
	return query_or_expression_tree_walker((Node *) query, check_support_functions_walker, &context, 0);
}

/*
 * Does any relation in the query have a virtual generated column?
 *
 * PostgreSQL 18 added them, and the planner expands every reference to one
 * into its generation expression before planning (prepjointree.c,
 * expand_virtual_generated_columns); the column has no storage, so a scan
 * that read it would read nothing.  ORCA's translator predates them and
 * would do exactly that -- a plan that runs and returns NULL for the column.
 *
 * The query is refused rather than expanded here, because expanding it is
 * not always a substitution: a reference on the nullable side of an outer
 * join, or under grouping sets, must go to NULL when its row does, and the
 * planner gets that from wrapping the expression in a PlaceHolderVar, which
 * ORCA cannot represent.  Doing the easy half would plan some of these
 * queries and get others wrong.
 *
 * NoLock, as the planner opens them: every relation in the query is locked
 * already, by the parser or the rewriter, or by the plan cache.
 */
static bool
has_virtual_generated_columns_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node,
								 has_virtual_generated_columns_walker,
								 context, QTW_EXAMINE_RTES_BEFORE);

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;
		Relation	rel;
		bool		found;

		if (rte->rtekind != RTE_RELATION)
			return false;

		rel = table_open(rte->relid, NoLock);
		found = (RelationGetDescr(rel)->constr != NULL &&
				 RelationGetDescr(rel)->constr->has_generated_virtual);
		table_close(rel, NoLock);

		return found;
	}

	return expression_tree_walker(node, has_virtual_generated_columns_walker,
								  context);
}

/*
 * Fold the grouping step back into the queries above it.
 *
 * PostgreSQL 18 gave a query with GROUP BY an RTE_GROUP range table entry,
 * and made every grouped expression in its target list and HAVING a Var of
 * that entry.  The planner replaces those Vars with the expressions again
 * early in subquery_planner(), with flatten_group_exprs(); ORCA's translator
 * has never heard of the entry, and would look for the column in a relation
 * that is not one.  So this does what the planner does, with the planner's
 * function, for every query in the tree.
 *
 * With no PlannerInfo, flatten_group_exprs() drops the grouping-set nulling
 * marks it would otherwise add (see its header comment).  Nothing downstream
 * reads them: ORCA does not, and set_plan_references(), which checks them,
 * never runs on an ORCA plan.
 */
static bool
flatten_group_rtes_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		if (query->hasGroupRTE)
		{
			query->targetList = (List *)
				flatten_group_exprs(NULL, query, (Node *) query->targetList);
			query->havingQual =
				flatten_group_exprs(NULL, query, query->havingQual);
		}

		return query_tree_walker(query, flatten_group_rtes_walker, context, 0);
	}

	return expression_tree_walker(node, flatten_group_rtes_walker, context);
}

/*
 * Constant folding, over the whole query tree.
 *
 * Cloudberry's fold_constants() is eval_const_expressions() in a mode of its
 * own, which PostgreSQL 19's does not have: it descends into subqueries, and
 * it leaves a SubLink's test expression as it found it.  The second matters.
 * ORCA's translator reads the test expression of ANY and ALL as the OpExpr
 * the parser made, and folding can make it something else -- a SQL-language
 * operator function inlines into whatever its body is -- which the translator
 * would take for an OpExpr all the same.
 *
 * So PostgreSQL 19's eval_const_expressions() is called on each expression,
 * level by level, and a SubLink's test expression is taken out before and
 * put back after.  They are matched by the subquery each SubLink points to,
 * which eval_const_expressions() never copies: it does not descend into a
 * Query, and returns the one it was given.
 *
 * The PlannerInfo follows the level being folded.  eval_const_expressions()
 * binds parameters from root->glob->boundParams, and records the functions
 * it inlines in root->glob->invalItems, which the plan has to depend on --
 * both reasons Cloudberry passes one too.  PostgreSQL 19 also asks root->parse
 * about the relation a Var belongs to, when deciding whether it can be NULL,
 * so root->parse has to be the query that Var is in.  With no NOT NULL facts
 * collected (glob->rel_notnullatts_hash), the answer is always "maybe", which
 * is the safe one.
 *
 * Not Cloudberry's max_size: its eval_const_expressions() declines to fold an
 * expression whose result would be a constant larger than
 * GPOPT_MAX_FOLDED_CONSTANT_SIZE, so that DXL does not carry it.
 * PostgreSQL 19's has no such limit, and ORCA plans a large constant as it
 * plans one the query wrote out.
 */
typedef struct fold_constants_context
{
	PlannerInfo *root;
	List	   *subselects;		/* SubLink subqueries, in step with ... */
	List	   *testexprs;		/* ... the test expressions taken from them */
} fold_constants_context;

static Node *fold_constants_mutator(Node *node, void *context);

static bool
detach_testexprs_walker(Node *node, void *context)
{
	fold_constants_context *fcontext = (fold_constants_context *) context;

	if (node == NULL)
		return false;

	/* A subquery is folded on its own, level by level. */
	if (IsA(node, Query))
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		fcontext->subselects = lappend(fcontext->subselects, sublink->subselect);
		fcontext->testexprs = lappend(fcontext->testexprs, sublink->testexpr);
		sublink->testexpr = NULL;

		return false;
	}

	return expression_tree_walker(node, detach_testexprs_walker, context);
}

static Node *
reattach_testexprs_mutator(Node *node, void *context)
{
	fold_constants_context *fcontext = (fold_constants_context *) context;

	if (node == NULL)
		return NULL;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		SubLink    *newnode = makeNode(SubLink);
		ListCell   *lcs;
		ListCell   *lct;

		memcpy(newnode, sublink, sizeof(SubLink));
		forboth(lcs, fcontext->subselects, lct, fcontext->testexprs)
		{
			if (lfirst(lcs) == (void *) sublink->subselect)
			{
				newnode->testexpr = (Node *) lfirst(lct);
				break;
			}
		}
		newnode->subselect = fold_constants_mutator(sublink->subselect,
													fcontext->root);
		return (Node *) newnode;
	}

	/* A CTE's query, which eval_const_expressions() does not descend into. */
	if (IsA(node, Query))
		return fold_constants_mutator(node, fcontext->root);

	return expression_tree_mutator(node, reattach_testexprs_mutator, context);
}

static Node *
fold_constants_mutator(Node *node, void *context)
{
	PlannerInfo *root = (PlannerInfo *) context;
	fold_constants_context fcontext;

	if (node == NULL)
		return NULL;

	if (IsA(node, Query))
	{
		Query	   *saved = root->parse;
		Query	   *result;

		root->parse = (Query *) node;
		result = query_tree_mutator((Query *) node, fold_constants_mutator,
									context, 0);
		root->parse = saved;

		return (Node *) result;
	}

	/* An expression of the current level. */
	fcontext.root = root;
	fcontext.subselects = NIL;
	fcontext.testexprs = NIL;

	(void) detach_testexprs_walker(node, &fcontext);
	node = eval_const_expressions(root, node);

	return reattach_testexprs_mutator(node, &fcontext);
}

/*
 * The permission checks the executor makes, completed from the query.
 *
 * ORCA's translator builds a range table entry, and a permission entry for
 * it, for each table it scans, and for nothing else.  The planner's range
 * table is the whole query's, flattened.  The difference is checks that do
 * not happen: a view's own SELECT privilege -- the rewriter keeps the view in
 * the range table for exactly that, as a subquery entry, and ORCA scans the
 * tables under it -- and any table ORCA plans no scan of at all, such as one
 * behind WHERE false or one a join it could remove.  PostgreSQL checks every
 * one of them when the plan starts; without this, a user who may not read a
 * view could read it through ORCA.
 *
 * So every permission entry in the query, at every level, is added to the
 * plan's, each with a range table entry of its own that no plan node reads --
 * the planner's flattened range table has such entries too.  An entry for a
 * table ORCA does scan is checked twice, which asks nothing new.  It also
 * puts every relation the query names in the range table, so that the plan
 * cache locks it before reusing the plan, as it would a planner's.
 *
 * The copies have what ExecCheckPermissions() asserts of an entry pointing at
 * a permission entry (execMain.c): a relation or a view, one entry for each
 * permission entry, and the same relid in both.
 */
static bool
add_query_permissions_walker(Node *node, void *context)
{
	PlannedStmt *stmt = (PlannedStmt *) context;

	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			RTEPermissionInfo *perminfo;
			RangeTblEntry *newrte;

			if (rte->perminfoindex == 0)
				continue;

			perminfo = getRTEPermissionInfo(query->rteperminfos, rte);

			newrte = makeNode(RangeTblEntry);
			newrte->rtekind = rte->rtekind;
			newrte->relid = rte->relid;
			newrte->relkind = rte->relkind;
			newrte->rellockmode = rte->rellockmode;
			newrte->inh = false;
			newrte->alias = copyObject(rte->alias);
			newrte->eref = copyObject(rte->eref);
			newrte->inFromCl = false;

			stmt->permInfos = lappend(stmt->permInfos, copyObject(perminfo));
			newrte->perminfoindex = list_length(stmt->permInfos);
			stmt->rtable = lappend(stmt->rtable, newrte);
			stmt->unprunableRelids = bms_add_member(stmt->unprunableRelids,
													list_length(stmt->rtable));
		}

		return query_tree_walker(query, add_query_permissions_walker, context, 0);
	}

	return expression_tree_walker(node, add_query_permissions_walker, context);
}

/*
 * The functions a plan calls, as plan-cache dependencies.
 *
 * Cloudberry's cdb_extract_plan_dependencies() runs setrefs.c's
 * fix_expr_common() over every node of the plan; PostgreSQL 19's
 * fix_expr_common() is static.  What it records is recorded here the same
 * way, through the two functions PostgreSQL does export: each function an
 * expression calls, and each relation a regclass constant names.  The
 * query's own dependencies are collected separately, by
 * extract_query_dependencies(); this catches the functions ORCA adds.
 */
typedef struct plan_dependencies_context
{
	plan_tree_base_prefix base; /* the PlannedStmt, for SubPlans */
	PlannerInfo *root;
} plan_dependencies_context;

static bool
extract_plan_dependencies_walker(Node *node, void *context)
{
	plan_dependencies_context *pcontext = (plan_dependencies_context *) context;
	PlannerInfo *root = pcontext->root;

	if (node == NULL)
		return false;

	if (IsA(node, Aggref))
		record_plan_function_dependency(root, ((Aggref *) node)->aggfnoid);
	else if (IsA(node, WindowFunc))
		record_plan_function_dependency(root, ((WindowFunc *) node)->winfnoid);
	else if (IsA(node, FuncExpr))
		record_plan_function_dependency(root, ((FuncExpr *) node)->funcid);
	else if (IsA(node, OpExpr) || IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		/* the three share OpExpr's layout, as setrefs.c relies on too */
		set_opfuncid((OpExpr *) node);
		record_plan_function_dependency(root, ((OpExpr *) node)->opfuncid);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(saop);
		record_plan_function_dependency(root, saop->opfuncid);
		if (OidIsValid(saop->hashfuncid))
			record_plan_function_dependency(root, saop->hashfuncid);
		if (OidIsValid(saop->negfuncid))
			record_plan_function_dependency(root, saop->negfuncid);
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		if ((con->consttype == REGCLASSOID || con->consttype == OIDOID) &&
			!con->constisnull)
			root->glob->relationOids =
				lappend_oid(root->glob->relationOids,
							DatumGetObjectId(con->constvalue));
	}

	return plan_tree_walker(node, extract_plan_dependencies_walker, context,
							true);
}

/*
 * optimize_query
 *		Plan the query using the GPORCA planner
 *
 * This is the main entrypoint for invoking Orca.
 *
 * NULL means ORCA made no plan, and *failure says why.  Cloudberry's has no
 * failure argument: it reports a fallback itself and returns.  gp_orca's
 * planner hook counts every fallback by reason, and the reason is here.
 */
PlannedStmt *
optimize_query(Query *parse, int cursorOptions, ParamListInfo boundParams,
			   OptimizerOptions *options, GpOrcaFailure *failure)
{
	PlannerInfo *root;
	PlannerGlobal *glob;
	Query	   *pqueryCopy;
	PlannedStmt *result = NULL;
	List	   *relationOids;
	List	   *invalItems;
	bool		hasRowSecurity;
	ListCell   *lp;

	failure->unexpected = false;
	failure->from_postgres = false;
	failure->message = NULL;

	/*
	 * Not Cloudberry's "fall back for updatable cursor": CURSOR_OPT_UPDATABLE
	 * is Cloudberry's bit, and PostgreSQL 19 has no such option.  A cursor
	 * declared FOR UPDATE carries row marks, which the translator refuses;
	 * one that is only used by WHERE CURRENT OF finds the scan under its plan
	 * the way it would under the planner's.
	 */

	if (orca_state_unknown)
	{
		failure->unexpected = true;
		failure->message = pstrdup("an earlier error left ORCA without unwinding it; "
								   "this backend no longer asks ORCA");
		return NULL;
	}

	/* Cloudberry brings ORCA up on the planner's first call, too. */
	GpOrcaEnsureInitialized();

	/*
	 * Initialize a dummy PlannerGlobal struct. ORCA doesn't use it, but the
	 * pre- and post-processing steps do.
	 */
	glob = makeNode(PlannerGlobal);
	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->transientPlan = false;
	glob->dependsOnRole = false;
	/* these will be filled in below, in the pre- and post-processing steps */
	glob->finalrtable = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;

	root = makeNode(PlannerInfo);
	root->parse = parse;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;

	/* create a local copy to hand to the optimizer */
	pqueryCopy = (Query *) copyObject(parse);

	/*
	 * Pre-process the Query tree before calling optimizer.
	 *
	 * Check if rtable is NULL and query contains support functions.
	 * If both conditions are true, fall back to standard planner to avoid
	 * crashes in extension support functions that expect valid rtable.
	 *
	 * Performance note: This check does not significantly impact performance
	 * since pqueryCopy->rtable is non-NULL for most queries. The rtable is
	 * typically only NULL when the query contains sublinks, which is uncommon.
	 * The query_contains_support_functions() traversal only occurs in these
	 * rare cases.
	 */
	if (pqueryCopy->rtable == NULL && query_contains_support_functions(pqueryCopy))
	{
		elog(DEBUG1, "Query rtable is NULL and contains support functions, falling back to standard planner");
		failure->message = pstrdup("Falling back to Postgres-based planner because "
								   "GPORCA does not support the following feature: "
								   "a support function in a query with no range table");
		return NULL;
	}

	/* PostgreSQL 18's virtual generated columns; see the walker. */
	if (query_tree_walker(pqueryCopy, has_virtual_generated_columns_walker,
						  NULL, QTW_EXAMINE_RTES_BEFORE))
	{
		failure->message = pstrdup("Falling back to Postgres-based planner because "
								   "GPORCA does not support the following feature: "
								   "virtual generated columns");
		return NULL;
	}

	/* PostgreSQL 18's grouping step, folded back first, as the planner does. */
	(void) flatten_group_rtes_walker((Node *) pqueryCopy, NULL);

	/*
	 * Constant folding will add dependencies to functions or relations in
	 * glob->invalItems, for any functions that are inlined or eliminated
	 * away. (We will find dependencies to other objects later, after planning).
	 */
	pqueryCopy = (Query *) fold_constants_mutator((Node *) pqueryCopy, root);
	root->parse = parse;

	/*
	 * Not Cloudberry's transformGroupedWindows(), which splits a query that
	 * mixes window functions and aggregates in two: window functions are
	 * T1's, and until then the translator refuses them.
	 */

	/*
	 * Ok, invoke ORCA.
	 *
	 * An error that reaches here by longjmp went through ORCA without
	 * unwinding it; see orca_state_unknown.  An error ORCA caught comes back
	 * as failure->from_postgres instead, and is re-thrown below, once no C++
	 * frame is left between here and it.
	 */
	PG_TRY();
	{
		result = GpOrcaOptimize(pqueryCopy, options, failure);
	}
	PG_CATCH();
	{
		orca_state_unknown = true;
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (failure->from_postgres)
		PG_RE_THROW();

	log_optimizer(result, failure->unexpected);

	CHECK_FOR_INTERRUPTS();

	/*
	 * If ORCA didn't produce a plan, bail out and fall back to the Postgres
	 * planner.
	 */
	if (!result)
		return NULL;

	/*
	 * Post-process the plan.
	 */

	result->planTree = remove_redundant_results(result->planTree);
	foreach(lp, result->subplans)
		lfirst(lp) = remove_redundant_results((Plan *) lfirst(lp));

	/*
	 * For plan cache invalidation purposes, extract the OIDs of all
	 * relations in the final range table, and of all functions used in
	 * expressions in the plan tree. (In the regular planner, this is done
	 * in set_plan_references, see that for more comments.)
	 *
	 * The translator has put the range table's relations in
	 * result->relationOids already; the functions are collected here.
	 */
	{
		plan_dependencies_context pcontext;

		/* A subplan is reached through the SubPlan that runs it. */
		exec_init_plan_tree_base(&pcontext.base, result);
		pcontext.root = root;
		(void) extract_plan_dependencies_walker((Node *) result->planTree,
												&pcontext);
	}

	/*
	 * Also extract dependencies from the original Query tree. This is needed
	 * to capture dependencies to e.g. views, which have been expanded at
	 * planning to the underlying tables, and don't appear anywhere in the
	 * resulting plan.
	 *
	 * Its last output, whether any query in the tree has row security, is
	 * the plan cache's to act on (plansource->dependsOnRLS), and it does; the
	 * planner does not make a plan role-dependent for it.
	 */
	extract_query_dependencies((Node *) pqueryCopy,
							   &relationOids,
							   &invalItems,
							   &hasRowSecurity);
	result->relationOids = list_concat(result->relationOids,
									   glob->relationOids);
	result->relationOids = list_concat(result->relationOids, relationOids);
	result->invalItems = list_concat(glob->invalItems, invalItems);
	result->transientPlan = glob->transientPlan;
	result->dependsOnRole = glob->dependsOnRole;

	/*
	 * The permission checks the translator cannot know about; see above.
	 * From the query as ORCA was handed it, after folding, which is the query
	 * the planner would have taken its range table from: a subquery that
	 * folding removed is not checked by either.
	 */
	(void) add_query_permissions_walker((Node *) pqueryCopy, result);

	/*
	 * Like standard_planner: a scrollable cursor over a plan that cannot run
	 * backwards gets a Material on top.  Cloudberry does not need this,
	 * because it does not offer scrollable cursors over a distributed plan.
	 */
	if ((cursorOptions & CURSOR_OPT_SCROLL) &&
		!ExecSupportsBackwardScan(result->planTree))
		result->planTree = materialize_finished_plan(result->planTree);

	result->queryId = parse->queryId;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;
	result->utilityStmt = parse->utilityStmt;

	return result;
}

/*
 * ORCA tends to generate gratuitous Result nodes for various reasons. We
 * try to clean it up here, as much as we can, by eliminating the Results
 * that are not really needed.
 *
 * Cloudberry walks the plan with its plan_tree_mutator(), which the port's
 * walkers.c does not carry; this descends the two child pointers, which is
 * every child a plan has until Append and the joins arrive (T1), and they
 * will bring the mutator with them.  A Result it does not reach stays, which
 * is the plan ORCA made.
 */
static Plan *
remove_redundant_results(Plan *plan)
{
	if (!plan)
		return NULL;

	if (IsA(plan, Result))
	{
		Result	   *result_plan = (Result *) plan;
		Plan	   *child_plan = result_plan->plan.lefttree;

		/*
		 * If this Result doesn't contain quals, hash filter or anything else
		 * funny, and the child node is projection capable, we can let the
		 * child node do the projection, and eliminate this Result.
		 *
		 * (We could probably push down quals and some other stuff to the child
		 * node if we worked a bit harder.)
		 *
		 * Not Cloudberry's numHashFilterCols test: a hash filter is a Result
		 * field Cloudberry adds, for Motion, and PostgreSQL 19's Result has
		 * none.
		 */
		if (result_plan->resconstantqual == NULL &&
			result_plan->plan.initPlan == NIL &&
			result_plan->plan.qual == NIL &&
			!expression_returns_set((Node *) result_plan->plan.targetlist) &&
			can_replace_tlist(child_plan))
		{
			List	   *tlist = result_plan->plan.targetlist;
			ListCell   *lc;

			child_plan = remove_redundant_results(child_plan);

			foreach(lc, tlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				tle->expr = (Expr *) push_down_expr_mutator((Node *) tle->expr,
															child_plan->targetlist);
			}

			/* Not Cloudberry's plan.flow, which PostgreSQL 19's Plan has not. */
			child_plan->targetlist = tlist;

			return child_plan;
		}
	}

	plan->lefttree = remove_redundant_results(plan->lefttree);
	plan->righttree = remove_redundant_results(plan->righttree);

	return plan;
}

/*
 * Can the target list of a Plan node safely be replaced?
 */
static bool
can_replace_tlist(Plan *plan)
{
	if (!plan)
		return false;

	/*
	 * SRFs in targetlists are quite funky. Don't mess with them.
	 * We could probably be smarter about them, but doesn't seem
	 * worth the trouble.
	 */
	if (expression_returns_set((Node *) plan->targetlist))
		return false;

	if (!is_projection_capable_plan(plan))
		return false;

	/*
	 * Not Cloudberry's two refusals, a Result with a hash filter and a Split
	 * Update: both compute a hash over their output columns by position, and
	 * both are Cloudberry's nodes, for Motion, which arrive with M2.
	 */

	return true;
}

/*
 * Fix up a target list, by replacing outer-Vars with the exprs from
 * the child target list, when we're stripping off a Result node.
 */
static Node *
push_down_expr_mutator(Node *node, List *child_tlist)
{
	if (!node)
		return NULL;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == OUTER_VAR && var->varattno > 0)
		{
			TargetEntry *child_tle = (TargetEntry *)
				list_nth(child_tlist, var->varattno - 1);
			// The const expr pertatining to a column in a child result node
			// has const.consttypmod set as default value.
			// correct typmod can be found at var.vartypmod.
			// const.consttypmod value needs to be fixed before replacing var with const.
			if (IsA(child_tle->expr, Const))
			{
				((Const *) child_tle->expr)->consttypmod = ((Var *) node)->vartypmod;
			}
			else if (IsA(child_tle->expr, Var))
			{
				((Var *) child_tle->expr)->vartypmod = ((Var *) node)->vartypmod;
			}

			return (Node *) child_tle->expr;
		}
	}
	return expression_tree_mutator(node, push_down_expr_mutator, child_tlist);
}
