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
 *	 * transformGroupedWindows() is Cloudberry's, with PostgreSQL 19's
 *	   IncrementVarSublevelsUp walker under it, which is static there.
 *
 *	 * No ShareInputScan post-processing: a CTE becomes PostgreSQL's own
 *	   CteScan and initplan, which need none (see the translator's
 *	   TranslateDXLSequence).  No remove_subquery_in_RTEs, which exists to make
 *	   a plan smaller to dispatch (M2).
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
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
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
static Node *transformGroupedWindows(Node *node, void *context);

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
 * Does a subquery or a CTE compute a volatile expression in an output column
 * nothing above it reads?
 *
 * ORCA's preprocessor prunes a computed column nothing reads
 * (CExpressionPreprocessor::PexprPruneUnusedComputedCols), keeping only
 * set-returning functions.  The planner keeps a volatile one as well
 * (allpaths.c, remove_unused_subquery_outputs), because what it does is part
 * of what the query does.  Under ORCA,
 *
 *		SELECT count(*) FROM (SELECT nextval('s'), a FROM t) x;
 *
 * counted the rows and never advanced the sequence.  So such a query is
 * refused: ORCA's core is taken unmodified, and nothing the translator hands
 * it makes it keep a column its parent does not ask for.
 *
 * A column is volatile if its expression calls a volatile function, or reads
 * a volatile column of a subquery below it, which ORCA would prune with it;
 * a set operation's column, if any branch's is.  It is read if a Var of the
 * query above names it, at any depth -- other than through a join's alias
 * list, which the parser leaves only for a FULL JOIN's merged column, and
 * counting that as unread only refuses more.  A CTE is refused if any of its
 * columns is volatile, read or not: which columns ORCA keeps of a CTE is
 * not worth working out for a case this rare.
 */
typedef struct columns_read_context
{
	Index		rtindex;		/* the range table entry whose columns count */
	int			sublevels_up;	/* how far below its query the walk is */
	Bitmapset  *read;			/* the columns some Var reads */
	bool		whole_row;		/* a whole-row Var reads them all */
} columns_read_context;

static bool
columns_read_walker(Node *node, columns_read_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->rtindex &&
			var->varlevelsup == context->sublevels_up)
		{
			if (var->varattno == InvalidAttrNumber)
				context->whole_row = true;
			else if (var->varattno > 0)
				context->read = bms_add_member(context->read, var->varattno);
		}
		return false;
	}

	if (IsA(node, Query))
	{
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, columns_read_walker,
								   (void *) context, QTW_IGNORE_JOINALIASES);
		context->sublevels_up--;
		return result;
	}

	return expression_tree_walker(node, columns_read_walker, (void *) context);
}

static Bitmapset *volatile_columns(Query *query);

/* Does the expression, of `query`, read a volatile column of a subquery? */
static bool
reads_volatile_column_walker(Node *node, Query *query)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;
		Bitmapset  *volatile_cols;

		if (var->varlevelsup != 0)
			return false;

		rte = rt_fetch(var->varno, query->rtable);
		if (rte->rtekind != RTE_SUBQUERY)
			return false;

		volatile_cols = volatile_columns(rte->subquery);
		if (var->varattno == InvalidAttrNumber)
			return !bms_is_empty(volatile_cols);
		return bms_is_member(var->varattno, volatile_cols);
	}

	/* a sublink's own query is not this query's; contain_volatile_functions() looked in it */
	if (IsA(node, Query))
		return false;

	return expression_tree_walker(node, reads_volatile_column_walker,
								  (void *) query);
}

/* The branches of a set operation, into `result`. */
static Bitmapset *
set_operation_volatile_columns(Node *setop, Query *query, Bitmapset *result)
{
	if (IsA(setop, RangeTblRef))
	{
		RangeTblEntry *rte = rt_fetch(((RangeTblRef *) setop)->rtindex,
									  query->rtable);

		return bms_add_members(result, volatile_columns(rte->subquery));
	}

	result = set_operation_volatile_columns(((SetOperationStmt *) setop)->larg,
											query, result);
	return set_operation_volatile_columns(((SetOperationStmt *) setop)->rarg,
										  query, result);
}

/* The output columns of a query ORCA would prune with what they compute. */
static Bitmapset *
volatile_columns(Query *query)
{
	Bitmapset  *result = NULL;
	ListCell   *lc;

	if (query->setOperations != NULL)
		return set_operation_volatile_columns(query->setOperations, query,
											  NULL);

	foreach(lc, query->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk)
			continue;

		if (contain_volatile_functions((Node *) tle->expr) ||
			reads_volatile_column_walker((Node *) tle->expr, query))
			result = bms_add_member(result, tle->resno);
	}

	return result;
}

static bool
has_unread_volatile_output_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;
		Index		rtindex = 0;

		foreach(lc, query->cteList)
		{
			CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

			if (IsA(cte->ctequery, Query) &&
				!bms_is_empty(volatile_columns((Query *) cte->ctequery)))
				return true;
		}

		/* A set operation reads every column of every branch itself. */
		if (query->setOperations == NULL)
		{
			foreach(lc, query->rtable)
			{
				RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
				Bitmapset  *volatile_cols;

				rtindex++;
				if (rte->rtekind != RTE_SUBQUERY)
					continue;

				volatile_cols = volatile_columns(rte->subquery);
				if (!bms_is_empty(volatile_cols))
				{
					columns_read_context read;

					read.rtindex = rtindex;
					read.sublevels_up = 0;
					read.read = NULL;
					read.whole_row = false;
					(void) query_tree_walker(query, columns_read_walker,
											 (void *) &read,
											 QTW_IGNORE_JOINALIASES);
					if (!read.whole_row &&
						!bms_is_subset(volatile_cols, read.read))
						return true;
				}
			}
		}

		return query_tree_walker(query, has_unread_volatile_output_walker,
								 context, 0);
	}

	return expression_tree_walker(node, has_unread_volatile_output_walker,
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
	 * A volatile expression ORCA would prune; see the walker.  After the
	 * grouping step is folded, so that a Var reads a column rather than a
	 * grouping expression.
	 */
	if (has_unread_volatile_output_walker((Node *) pqueryCopy, NULL))
	{
		failure->message = pstrdup("Falling back to Postgres-based planner because "
								   "GPORCA does not support the following feature: "
								   "a volatile function in a column nothing reads");
		return NULL;
	}

	/*
	 * Constant folding will add dependencies to functions or relations in
	 * glob->invalItems, for any functions that are inlined or eliminated
	 * away. (We will find dependencies to other objects later, after planning).
	 */
	pqueryCopy = (Query *) fold_constants_mutator((Node *) pqueryCopy, root);
	root->parse = parse;

	/*
	 * If any Query in the tree mixes window functions and aggregates, we need to
	 * transform it such that the grouped query appears as a subquery
	 */
	pqueryCopy = (Query *) transformGroupedWindows((Node *) pqueryCopy, NULL);

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
 * walkers.c does not carry; this descends every child pointer a plan of the
 * port's can have -- the two every node has, an Append's list, and the plan
 * under a SubqueryScan -- which every parent reads by position, so a child
 * that takes over a Result's target list takes over its place too.  A
 * Result it does not reach stays, which is the plan ORCA made.
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

	if (IsA(plan, Append))
	{
		ListCell   *lc;

		foreach(lc, ((Append *) plan)->appendplans)
			lfirst(lc) = remove_redundant_results((Plan *) lfirst(lc));
	}
	else if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subquery_scan = (SubqueryScan *) plan;

		subquery_scan->subplan = remove_redundant_results(subquery_scan->subplan);
	}

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

/*
 * ORCA cannot deal with window functions in the same query with
 * grouping. If a query contains both, transformGroupedWindows()
 * transforms it into a a query with a subquer to avoid that:
 *
 * If an input query (Q) mixes window functions with aggregate
 * functions or grouping, then (per SQL:2003) we need to divide
 * it into an outer query, Q', that contains no aggregate calls
 * or grouping and an inner query, Q'', that contains no window
 * calls.
 *
 * Q' will have a 1-entry range table whose entry corresponds to
 * the results of Q''.
 *
 * Q'' will have the same range as Q and will be pushed down into
 * a subquery range table entry in Q'.
 *
 * As a result, the depth of outer references in Q'' and below
 * will increase, so we need to adjust non-zero xxxlevelsup fields
 * (Var, Aggref, and WindowFunc nodes) in Q'' and below.  At the end,
 * there will be no levelsup items referring to Q'.  Prior references
 * to Q will now refer to Q''; prior references to blocks above Q will
 * refer to the same blocks above Q'.)
 *
 * We do all this by creating a new Query node, subq, for Q''.  We
 * modify the input Query node, qry, in place for Q'.  (Since qry is
 * also the input, Q, be careful not to destroy values before we're
 * done with them.
 *
 * The function is structured as a mutator, so that we can transform
 * all of the Query nodes in the entire tree, bottom-up.
 *
 * Ported from Cloudberry's orca.c.  What changed for PostgreSQL 19 is said
 * beside it: two Query fields PostgreSQL 18 added move with the grouping, and
 * the three helpers Cloudberry adds to PostgreSQL's own files are here, as
 * static functions.
 */

/* Context for transformGroupedWindows() which mutates components
 * of a query that mixes windowing and aggregation or grouping.  It
 * accumulates context for eventual construction of a subquery (the
 * grouping query) during mutation of components of the outer query
 * (the windowing query).
 */
typedef struct
{
	List	   *subtlist;		/* target list for subquery */
	List	   *subgroupClause; /* group clause for subquery */
	List	   *subgroupingSets;	/* grouping sets for subquery */
	List	   *windowClause;	/* window clause for outer query */

	/*
	 * Scratch area for init_grouped_window context and map_sgr_mutator.
	 */
	Index	   *sgr_map;
	int			sgr_map_size;

	/*
	 * Scratch area for grouped_window_mutator and var_for_grouped_window_expr.
	 */
	List	   *subrtable;
	int			call_depth;
	TargetEntry *tle;
} grouped_window_ctx;

static void init_grouped_window_context(grouped_window_ctx * ctx, Query *qry);
static Var *var_for_grouped_window_expr(grouped_window_ctx * ctx, Node *expr, bool force);
static void discard_grouped_window_context(grouped_window_ctx * ctx);
static Node *map_sgr_mutator(Node *node, void *context);
static Node *grouped_window_mutator(Node *node, void *context);
static Alias *make_replacement_alias(Query *qry, const char *aname);
static char *generate_positional_name(AttrNumber attrno);
static List *generate_alternate_vars(Var *var, grouped_window_ctx * ctx);
static void IncrementVarSublevelsUpInTransformGroupedWindows(Node *node,
															 int delta_sublevels_up,
															 int min_sublevels_up);
static void get_sortgroupclauses_tles(List *clauses, List *targetList,
									  List **tles, List **sortops, List **eqops);
static Index maxSortGroupRef(List *targetlist, bool include_orderedagg);

static Node *
transformGroupedWindows(Node *node, void *context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Query))
	{
		// do a depth-first recursion into any subqueries
		Query *qry = (Query *) query_tree_mutator((Query *) node, transformGroupedWindows, context, 0);
		Query	   *subq;
		RangeTblEntry *rte;
		RangeTblRef *ref;
		Alias	   *alias;
		bool		hadSubLinks;

		grouped_window_ctx ctx;

		Assert(IsA(qry, Query));

		/*
		 * we are done if this query doesn't have both window functions and group by/aggregates
		 */
		if (!qry->hasWindowFuncs ||
			!(qry->groupClause || qry->groupingSets || qry->hasAggs))
			return (Node *) qry;

		hadSubLinks = qry->hasSubLinks;

		Assert(qry->commandType == CMD_SELECT);
		Assert(qry->utilityStmt == NULL);
		Assert(qry->returningList == NIL);

		/*
		 * Make the new subquery (Q'').  Note that (per SQL:2003) there can't be
		 * any window functions called in the WHERE, GROUP BY, or HAVING clauses.
		 */
		subq = makeNode(Query);
		subq->commandType = CMD_SELECT;
		subq->querySource = QSRC_PARSER;
		subq->canSetTag = true;
		subq->utilityStmt = NULL;
		subq->resultRelation = 0;
		subq->hasAggs = qry->hasAggs;
		subq->hasWindowFuncs = false;	/* reevaluate later */
		subq->hasSubLinks = qry->hasSubLinks;	/* reevaluate later */

		/* Core of subquery input table expression: */
		subq->rtable = qry->rtable; /* before windowing */
		subq->rteperminfos = qry->rteperminfos; /* before windowing */
		subq->jointree = qry->jointree; /* before windowing */
		subq->targetList = NIL;		/* fill in later */

		subq->returningList = NIL;
		subq->groupClause = qry->groupClause;	/* before windowing */
		subq->groupingSets = qry->groupingSets; /* before windowing */
		subq->havingQual = qry->havingQual; /* before windowing */
		subq->windowClause = NIL;	/* by construction */
		subq->distinctClause = NIL; /* after windowing */
		subq->sortClause = NIL;		/* after windowing */
		subq->limitOffset = NULL;	/* after windowing */
		subq->limitCount = NULL;	/* after windowing */
		subq->rowMarks = NIL;
		subq->setOperations = NULL;

		/*
		 * Two fields PostgreSQL 18 added go with the grouping, which
		 * Cloudberry's PostgreSQL 16 does not have: GROUP BY DISTINCT, and
		 * whether the range table holds the grouping step's entry.  The
		 * entry moves with the range table, and its Vars are gone already
		 * -- flatten_group_rtes_walker() ran first.
		 */
		subq->groupDistinct = qry->groupDistinct;
		subq->hasGroupRTE = qry->hasGroupRTE;
		qry->groupDistinct = false;
		qry->hasGroupRTE = false;

		/*
		 * Check if there is a window function in the join tree. If so we must
		 * mark hasWindowFuncs in the sub query as well.
		 */
		if (contain_window_function((Node *) subq->jointree))
			subq->hasWindowFuncs = true;

		/*
		 * Make the single range table entry for the outer query Q' as a wrapper
		 * for the subquery (Q'') currently under construction.
		 */
		rte = makeNode(RangeTblEntry);
		rte->rtekind = RTE_SUBQUERY;
		rte->subquery = subq;
		rte->alias = NULL;			/* fill in later */
		rte->eref = NULL;			/* fill in later */
		rte->inFromCl = true;

		/*
		 * Subquery RTEs do not need RTEPermissionInfo.  Permission checks
		 * are performed on the base tables within the subquery itself.
		 */

		/*
		 * Make a reference to the new range table entry .
		 */
		ref = makeNode(RangeTblRef);
		ref->rtindex = 1;

		/*
		 * Set up context for mutating the target list.  Careful. This is trickier
		 * than it looks.  The context will be "primed" with grouping targets.
		 */
		init_grouped_window_context(&ctx, qry);

		/*
		 * Begin rewriting the outer query in place.
		 */
		qry->hasAggs = false;		/* by construction */
		/* qry->hasSubLinks -- reevaluate later. */

		/* Core of outer query input table expression: */
		qry->rtable = list_make1(rte);
		qry->rteperminfos = NIL;
		qry->jointree = (FromExpr *) makeNode(FromExpr);
		qry->jointree->fromlist = list_make1(ref);
		qry->jointree->quals = NULL;
		/* qry->targetList -- to be mutated from Q to Q' below */

		qry->groupClause = NIL;		/* by construction */
		qry->groupingSets = NIL;	/* by construction */
		qry->havingQual = NULL;		/* by construction */

		/*
		 * Mutate the Q target list and windowClauses for use in Q' and, at the
		 * same time, update state with info needed to assemble the target list
		 * for the subquery (Q'').
		 */
		qry->targetList = (List *) grouped_window_mutator((Node *) qry->targetList, &ctx);
		qry->windowClause = (List *) grouped_window_mutator((Node *) qry->windowClause, &ctx);
		qry->hasSubLinks = checkExprHasSubLink((Node *) qry->targetList);

		/*
		 * New subquery fields
		 */
		subq->targetList = ctx.subtlist;
		subq->groupClause = ctx.subgroupClause;
		subq->groupingSets = ctx.subgroupingSets;

		/*
		 * A set-returning function in a grouping expression is computed by
		 * the grouping query now, and one elsewhere in the target list still
		 * by the outer; Cloudberry leaves hasTargetSRFs as the input query had
		 * it, on both.
		 */
		subq->hasTargetSRFs = expression_returns_set((Node *) subq->targetList);
		qry->hasTargetSRFs = expression_returns_set((Node *) qry->targetList);

		/*
		 * We always need an eref, but we shouldn't really need a filled in alias.
		 * However, view deparse (or at least the fix for MPP-2189) wants one.
		 */
		alias = make_replacement_alias(subq, "Window");
		rte->eref = copyObject(alias);
		rte->alias = alias;

		/*
		 * Accommodate depth change in new subquery, Q''.
		 */
		IncrementVarSublevelsUpInTransformGroupedWindows((Node *) subq, 1, 1);

		/* Might have changed. */
		subq->hasSubLinks = checkExprHasSubLink((Node *) subq);

		Assert(qry->targetList != NIL);
		Assert(IsA(qry->targetList, List));

		/*
		 * Use error instead of assertion to "use" hadSubLinks and keep compiler
		 * happy.
		 */
		if (hadSubLinks != (qry->hasSubLinks || subq->hasSubLinks))
			elog(ERROR, "inconsistency detected in internal grouped windows transformation");

		discard_grouped_window_context(&ctx);

		return (Node *) qry;
	}

	/*
	 * for all other node types, just keep walking the tree
	 */
	return expression_tree_mutator(node, transformGroupedWindows, context);
}


/* Helper for transformGroupedWindows:
 *
 * Prime the subquery target list in the context with the grouping
 * and windowing attributes from the given query and adjust the
 * subquery group clauses in the context to agree.
 *
 * Note that we arrange dense sortgroupref values and stash the
 * referents on the front of the subquery target list.  This may
 * be over-kill, but the grouping extension code seems to like it
 * this way.
 *
 * Note that we only transfer sortgroupref values associated with
 * grouping and windowing to the subquery context.  The subquery
 * shouldn't care about ordering, etc. XXX
 */
static void
init_grouped_window_context(grouped_window_ctx * ctx, Query *qry)
{
	List	   *grp_tles;
	List	   *grp_sortops;
	List	   *grp_eqops;
	ListCell   *lc = NULL;
	Index		maxsgr = 0;

	get_sortgroupclauses_tles(qry->groupClause, qry->targetList,
							  &grp_tles, &grp_sortops, &grp_eqops);
	list_free(grp_sortops);
	maxsgr = maxSortGroupRef(grp_tles, true);

	ctx->subtlist = NIL;
	ctx->subgroupClause = NIL;
	ctx->subgroupingSets = NIL;

	/*
	 * Set up scratch space.
	 */

	ctx->subrtable = qry->rtable;

	/*
	 * Map input = outer query sortgroupref values to subquery values while
	 * building the subquery target list prefix.
	 */
	ctx->sgr_map = palloc0((maxsgr + 1) * sizeof(ctx->sgr_map[0]));
	ctx->sgr_map_size = maxsgr + 1;
	foreach(lc, grp_tles)
	{
		TargetEntry *tle;
		Index		old_sgr;

		tle = (TargetEntry *) copyObject(lfirst(lc));
		old_sgr = tle->ressortgroupref;

		ctx->subtlist = lappend(ctx->subtlist, tle);
		tle->resno = list_length(ctx->subtlist);
		tle->ressortgroupref = tle->resno;
		tle->resjunk = false;

		ctx->sgr_map[old_sgr] = tle->ressortgroupref;
	}

	/* Miscellaneous scratch area. */
	ctx->call_depth = 0;
	ctx->tle = NULL;

	/* Revise grouping into ctx->subgroupClause */
	ctx->subgroupClause = (List *) map_sgr_mutator((Node *) qry->groupClause, ctx);
	ctx->subgroupingSets = (List *) map_sgr_mutator((Node *) qry->groupingSets, ctx);
}


/* Helper for transformGroupedWindows */
static void
discard_grouped_window_context(grouped_window_ctx * ctx)
{
	ctx->subtlist = NIL;
	ctx->subgroupClause = NIL;
	ctx->subgroupingSets = NIL;
	ctx->tle = NULL;
	if (ctx->sgr_map)
		pfree(ctx->sgr_map);
	ctx->sgr_map = NULL;
	ctx->subrtable = NULL;
}


/* Helper for transformGroupedWindows:
 *
 * Look for the given expression in the context's subtlist.  If
 * none is found and the force argument is true, add a target
 * for it.  Make and return a variable referring to the target
 * with the matching expression, or return NULL, if no target
 * was found/added.
 */
static Var *
var_for_grouped_window_expr(grouped_window_ctx * ctx, Node *expr, bool force)
{
	Var		   *var = NULL;
	TargetEntry *tle = tlist_member((Expr *) expr, ctx->subtlist);

	if (tle == NULL && force)
	{
		tle = makeNode(TargetEntry);
		ctx->subtlist = lappend(ctx->subtlist, tle);
		tle->expr = (Expr *) expr;
		tle->resno = list_length(ctx->subtlist);

		/*
		 * See comment in grouped_window_mutator for why level 3 is
		 * appropriate.
		 */
		if (ctx->call_depth == 3 && ctx->tle != NULL && ctx->tle->resname != NULL)
		{
			tle->resname = pstrdup(ctx->tle->resname);
		}
		else
		{
			tle->resname = generate_positional_name(tle->resno);
		}
		tle->ressortgroupref = 0;
		tle->resorigtbl = 0;
		tle->resorigcol = 0;
		tle->resjunk = false;
	}

	if (tle != NULL)
	{
		var = makeNode(Var);
		var->varno = 1;			/* one and only */
		var->varattno = tle->resno; /* by construction */
		var->vartype = exprType((Node *) tle->expr);
		var->vartypmod = exprTypmod((Node *) tle->expr);
		var->varcollid = exprCollation((Node *) tle->expr);
		var->varlevelsup = 0;
		var->varnosyn = 1;
		var->varattnosyn = tle->resno;
		var->location = 0;
	}

	return var;
}


/* Helper for transformGroupedWindows:
 *
 * Mutator for subquery groupingClause to adjust sortgroupref values
 * based on map developed while priming context target list.
 */
static Node *
map_sgr_mutator(Node *node, void *context)
{
	grouped_window_ctx *ctx = (grouped_window_ctx *) context;

	if (!node)
		return NULL;

	if (IsA(node, List))
	{
		ListCell   *lc;
		List	   *new_lst = NIL;

		foreach(lc, (List *) node)
		{
			Node	   *newnode = lfirst(lc);

			newnode = map_sgr_mutator(newnode, ctx);
			new_lst = lappend(new_lst, newnode);
		}
		return (Node *) new_lst;
	}
	else if (IsA(node, IntList))
	{
		ListCell   *lc;
		List	   *new_lst = NIL;

		foreach(lc, (List *) node)
		{
			int			sortgroupref = lfirst_int(lc);

			if (sortgroupref < 0 || sortgroupref >= ctx->sgr_map_size)
				elog(ERROR, "sortgroupref %d out of bounds", sortgroupref);

			sortgroupref = ctx->sgr_map[sortgroupref];

			new_lst = lappend_int(new_lst, sortgroupref);
		}
		return (Node *) new_lst;
	}
	else if (IsA(node, SortGroupClause))
	{
		SortGroupClause *g = (SortGroupClause *) node;
		SortGroupClause *new_g = makeNode(SortGroupClause);

		memcpy(new_g, g, sizeof(SortGroupClause));
		new_g->tleSortGroupRef = ctx->sgr_map[g->tleSortGroupRef];
		return (Node *) new_g;
	}
	else if (IsA(node, GroupingSet))
	{
		GroupingSet *gset = (GroupingSet *) node;
		GroupingSet *newgset = (GroupingSet *) node;

		newgset = makeNode(GroupingSet);
		newgset->kind = gset->kind;
		newgset->content = (List *) map_sgr_mutator((Node *) gset->content, context);
		newgset->location = gset->location;

		return (Node *) newgset;
	}
	else
		elog(ERROR, "unexpected node type %d", nodeTag(node));
}




/*
 * Helper for transformGroupedWindows:
 *
 * Transform targets from Q into targets for Q' and place information
 * needed to eventually construct the target list for the subquery Q''
 * in the context structure.
 *
 * The general idea is to add expressions that must be evaluated in the
 * subquery to the subquery target list (in the context) and to replace
 * them with Var nodes in the outer query.
 *
 * If there are any Agg nodes in the Q'' target list, arrange
 * to set hasAggs to true in the subquery. (This should already be
 * done, though).
 *
 * If we're pushing down an entire TLE that has a resname, use
 * it as an alias in the upper TLE, too.  Facilitate this by copying
 * down the resname from an immediately enclosing TargetEntry, if any.
 *
 * The algorithm repeatedly searches the subquery target list under
 * construction (quadric), however we don't expect many targets so
 * we don't optimize this.  (Could, for example, use a hash or divide
 * the target list into var, expr, and group/aggregate function lists.)
 */

static Node *
grouped_window_mutator(Node *node, void *context)
{
	Node	   *result = NULL;

	grouped_window_ctx *ctx = (grouped_window_ctx *) context;

	if (!node)
		return result;

	ctx->call_depth++;

	if (IsA(node, TargetEntry))
	{
		TargetEntry *tle = (TargetEntry *) node;
		TargetEntry *new_tle = makeNode(TargetEntry);

		/* Copy the target entry. */
		new_tle->resno = tle->resno;
		if (tle->resname == NULL)
		{
			new_tle->resname = generate_positional_name(new_tle->resno);
		}
		else
		{
			new_tle->resname = pstrdup(tle->resname);
		}
		new_tle->ressortgroupref = tle->ressortgroupref;
		new_tle->resorigtbl = InvalidOid;
		new_tle->resorigcol = 0;
		new_tle->resjunk = tle->resjunk;

		/*
		 * This is pretty shady, but we know our call pattern.  The target
		 * list is at level 1, so we're interested in target entries at level
		 * 2.  We record them in context so var_for_grouped_window_expr can maybe make a
		 * better than default choice of alias.
		 */
		if (ctx->call_depth == 2)
		{
			ctx->tle = tle;
		}
		else
		{
			ctx->tle = NULL;
		}

		new_tle->expr = (Expr *) grouped_window_mutator((Node *) tle->expr, ctx);

		ctx->tle = NULL;
		result = (Node *) new_tle;
	}
	else if (IsA(node, Aggref))
	{
		/* Aggregation expression */
		result = (Node *) var_for_grouped_window_expr(ctx, node, true);
	}
	else if (IsA(node, GroupingFunc))
	{
		GroupingFunc *gfunc = (GroupingFunc *) node;
		GroupingFunc *newgfunc;

		newgfunc = (GroupingFunc *) copyObject((Node *) gfunc);

		newgfunc->refs = (List *) map_sgr_mutator((Node *) newgfunc->refs, ctx);

		result = (Node *) var_for_grouped_window_expr(ctx, (Node *) newgfunc, true);
	}
	else if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/*
		 * Since this is a Var (leaf node), we must be able to mutate it, else
		 * we can't finish the transformation and must give up.
		 */
		result = (Node *) var_for_grouped_window_expr(ctx, node, false);

		if (!result)
		{
			List	   *altvars = generate_alternate_vars(var, ctx);
			ListCell   *lc;

			foreach(lc, altvars)
			{
				result = (Node *) var_for_grouped_window_expr(ctx, lfirst(lc), false);
				if (result)
					break;
			}
		}

		if (!result)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unresolved grouping key in window query"),
					 errhint("You might need to use explicit aliases and/or to refer to grouping keys in the same way throughout the query, or turn gp.optimizer=off.")));
		}
	}
	else if (IsA(node, SubLink))
	{
		/* put the subquery into Q'' */
		result = (Node *) var_for_grouped_window_expr(ctx, node, true /* force */);
	}
	else
	{
		/* Grouping expression; may not find one. */
		result = (Node *) var_for_grouped_window_expr(ctx, node, false /* force */);
	}


	if (!result)
	{
		result = expression_tree_mutator(node, grouped_window_mutator, ctx);
	}

	ctx->call_depth--;
	return result;
}

/*
 * Helper for transformGroupedWindows:
 *
 * Build an Alias for a subquery RTE representing the given Query.
 * The input string aname is the name for the overall Alias. The
 * attribute names are all found or made up.
 */
static Alias *
make_replacement_alias(Query *qry, const char *aname)
{
	ListCell   *lc = NULL;
	char	   *name = NULL;
	Alias	   *alias = makeNode(Alias);
	AttrNumber	attrno = 0;

	alias->aliasname = pstrdup(aname);
	alias->colnames = NIL;

	foreach(lc, qry->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		attrno++;

		if (tle->resname)
		{
			/* Prefer the target's resname. */
			name = pstrdup(tle->resname);
		}
		else if (IsA(tle->expr, Var))
		{
			/*
			 * If the target expression is a Var, use the name of the
			 * attribute in the query's range table.
			 */
			Var		   *var = (Var *) tle->expr;
			RangeTblEntry *rte = rt_fetch(var->varno, qry->rtable);

			name = pstrdup(get_rte_attribute_name(rte, var->varattno));
		}
		else
		{
			/* If all else, fails, generate a name based on position. */
			name = generate_positional_name(attrno);
		}

		alias->colnames = lappend(alias->colnames, makeString(name));
	}
	return alias;
}

/*
 * Helper for transformGroupedWindows:
 *
 * Make a palloc'd C-string named for the input attribute number.
 */
static char *
generate_positional_name(AttrNumber attrno)
{
	int			rc = 0;
	char		buf[NAMEDATALEN];

	rc = snprintf(buf, sizeof(buf),
				  "att_%d", attrno);
	if (rc == EOF || rc < 0 || rc >= sizeof(buf))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("can't generate internal attribute name")));
	}
	return pstrdup(buf);
}

/*
 * Helper for transformGroupedWindows:
 *
 * Find alternate Vars on the range of the input query that are aliases
 * (modulo ANSI join) of the input Var on the range and that occur in the
 * target list of the input query.
 *
 * If the input Var references a join result, there will be a single
 * alias.  If not, we need to search the range table for occurrences
 * of the input Var in some join result's RTE and add a Var referring
 * to the appropriate attribute of the join RTE to the list.
 *
 * This is not efficient, but the need is rare (MPP-12082) so we don't
 * bother to precompute this.
 */
static List *
generate_alternate_vars(Var *invar, grouped_window_ctx * ctx)
{
	List	   *rtable = ctx->subrtable;
	RangeTblEntry *inrte;
	List	   *alternates = NIL;

	Assert(IsA(invar, Var));

	inrte = rt_fetch(invar->varno, rtable);

	if (inrte->rtekind == RTE_JOIN)
	{
		Node	   *ja = list_nth(inrte->joinaliasvars, invar->varattno - 1);

		/*
		 * Though Node types other than Var (e.g., CoalesceExpr or Const) may
		 * occur as joinaliasvars, we ignore them.
		 */
		if (IsA(ja, Var))
		{
			alternates = lappend(alternates, copyObject(ja));
		}
	}
	else
	{
		ListCell   *jlc;
		Index		varno = 0;

		foreach(jlc, rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(jlc);

			varno++;			/* This RTE's varno */

			if (rte->rtekind == RTE_JOIN)
			{
				ListCell   *alc;
				AttrNumber	attno = 0;

				foreach(alc, rte->joinaliasvars)
				{
					ListCell   *tlc;
					Node	   *altnode = lfirst(alc);
					Var		   *altvar = (Var *) altnode;

					attno++;	/* This attribute's attno in its join RTE */

					if (!IsA(altvar, Var) || !equal(invar, altvar))
						continue;

					/* Look for a matching Var in the target list. */

					foreach(tlc, ctx->subtlist)
					{
						TargetEntry *tle = (TargetEntry *) lfirst(tlc);
						Var		   *v = (Var *) tle->expr;

						if (IsA(v, Var) && v->varno == varno && v->varattno == attno)
						{
							alternates = lappend(alternates, tle->expr);
						}
					}
				}
			}
		}
	}
	return alternates;
}

/*
 * IncrementVarSublevelsUp, the way transformGroupedWindows() needs it.
 *
 * Copied from PostgreSQL 19's rewriteManip.c, where the walker is static,
 * with the one change Cloudberry makes to it there (rewriteManip.c,
 * "Fix for MPP-19436"): a reference to a CTE of the query being split stays
 * pointed at it, and the CTE list stays with the outer query, so the
 * reference now has one more level to climb -- whatever level the reference
 * is at, not only those at or below min_sublevels_up.
 */
typedef struct
{
	int			delta_sublevels_up;
	int			min_sublevels_up;
} IncrementVarSublevelsUp_context;

static bool
IncrementVarSublevelsUp_walker(Node *node,
							   IncrementVarSublevelsUp_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup >= context->min_sublevels_up)
			var->varlevelsup += context->delta_sublevels_up;
		return false;			/* done here */
	}
	if (IsA(node, CurrentOfExpr))
	{
		/* this should not happen */
		if (context->min_sublevels_up == 0)
			elog(ERROR, "cannot push down CurrentOfExpr");
		return false;
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if (agg->agglevelsup >= context->min_sublevels_up)
			agg->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, GroupingFunc))
	{
		GroupingFunc *grp = (GroupingFunc *) node;

		if (grp->agglevelsup >= context->min_sublevels_up)
			grp->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup >= context->min_sublevels_up)
			phv->phlevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, ReturningExpr))
	{
		ReturningExpr *rexpr = (ReturningExpr *) node;

		if (rexpr->retlevelsup >= context->min_sublevels_up)
			rexpr->retlevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE)
		{
			if (rte->ctelevelsup >= context->min_sublevels_up)
				rte->ctelevelsup += context->delta_sublevels_up;

			/* Cloudberry's change; see above */
			else if (rte->ctelevelsup == context->min_sublevels_up - 1)
				rte->ctelevelsup += context->delta_sublevels_up;
		}
		return false;			/* allow range_table_walker to continue */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->min_sublevels_up++;
		result = query_tree_walker((Query *) node,
								   IncrementVarSublevelsUp_walker,
								   context,
								   QTW_EXAMINE_RTES_BEFORE);
		context->min_sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, IncrementVarSublevelsUp_walker, context);
}

static void
IncrementVarSublevelsUpInTransformGroupedWindows(Node *node,
												 int delta_sublevels_up,
												 int min_sublevels_up)
{
	IncrementVarSublevelsUp_context context;

	context.delta_sublevels_up = delta_sublevels_up;
	context.min_sublevels_up = min_sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									IncrementVarSublevelsUp_walker,
									&context,
									QTW_EXAMINE_RTES_BEFORE);
}

/*
 * get_sortgroupclauses_tles
 *      Find a list of unique targetlist entries matching the given list of
 *      SortGroupClauses, or GroupingClauses.
 *
 * The unique targetlist entries are returned in *tles, and the sort
 * and equality operators associated with each tle are returned in
 * *sortops and *eqops.
 *
 * Cloudberry's, from its tlist.c, which PostgreSQL's does not have.  Its
 * second half, which put the entries of Greenplum's old GroupingClauses after
 * the rest, is not here: nothing adds to the lists it reads, in Cloudberry
 * either, since PostgreSQL's grouping sets replaced GroupingClause.
 */
static void
get_sortgroupclauses_tles_recurse(List *clauses, List *targetList,
								  List **tles, List **sortops, List **eqops)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Node *node = lfirst(lc);

		if (node == NULL)
			continue;

		if (IsA(node, SortGroupClause))
		{
			SortGroupClause *sgc = (SortGroupClause *) node;
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   targetList);

			if (!list_member(*tles, tle))
			{
				*tles = lappend(*tles, tle);
				*sortops = lappend_oid(*sortops, sgc->sortop);
				*eqops = lappend_oid(*eqops, sgc->eqop);
			}
		}
		else if (IsA(node, List))
		{
			get_sortgroupclauses_tles_recurse((List *) node, targetList,
											  tles, sortops, eqops);
		}
		else
			elog(ERROR, "unrecognized node type in list of sort/group clauses: %d",
				 (int) nodeTag(node));
	}
}

static void
get_sortgroupclauses_tles(List *clauses, List *targetList,
						  List **tles, List **sortops, List **eqops)
{
	*tles = NIL;
	*sortops = NIL;
	*eqops = NIL;

	get_sortgroupclauses_tles_recurse(clauses, targetList,
									  tles, sortops, eqops);
}

/*
 * Return the largest sortgroupref value in use in the given
 * target list.
 *
 * If include_orderedagg is false, consider only the top-level
 * entries in the target list, i.e., those that might be occur
 * in a groupClause, distinctClause, or sortClause of the Query
 * node that immediately contains the target list.
 *
 * If include_orderedagg is true, also consider AggOrder entries
 * embedded in Aggref nodes within the target list.  Though
 * such entries will only occur in the aggregation sub_tlist
 * (input) they affect sortgroupref numbering for both sub_tlist
 * and tlist (aggregate).
 *
 * Cloudberry's, from its tlist.c, which PostgreSQL's does not have.
 */
typedef struct maxSortGroupRef_context
{
	Index		maxsgr;
	bool		include_orderedagg;
} maxSortGroupRef_context;

static bool
maxSortGroupRef_walker(Node *node, maxSortGroupRef_context *cxt)
{
	if ( node == NULL )
		return false;

	if ( IsA(node, TargetEntry) )
	{
		TargetEntry *tle = (TargetEntry*)node;
		if ( tle->ressortgroupref > cxt->maxsgr )
			cxt->maxsgr = tle->ressortgroupref;

		return maxSortGroupRef_walker((Node*)tle->expr, cxt);
	}

	/* Aggref nodes don't nest, so we can treat them here without recurring
	 * further.
	 */

	if ( IsA(node, Aggref) )
	{
		Aggref *ref = (Aggref*)node;

		if ( cxt->include_orderedagg )
		{
			ListCell *lc;

			foreach (lc, ref->aggorder)
			{
				SortGroupClause *sort = (SortGroupClause *)lfirst(lc);
				Assert(IsA(sort, SortGroupClause));
				Assert( sort->tleSortGroupRef != 0 );
				if (sort->tleSortGroupRef > cxt->maxsgr )
					cxt->maxsgr = sort->tleSortGroupRef;
			}

		}
		return false;
	}

	return expression_tree_walker(node, maxSortGroupRef_walker, cxt);
}

static Index
maxSortGroupRef(List *targetlist, bool include_orderedagg)
{
	maxSortGroupRef_context context;
	context.maxsgr = 0;
	context.include_orderedagg = include_orderedagg;

	if (targetlist != NIL)
	{
		if ( !IsA(targetlist, List) || !IsA(linitial(targetlist), TargetEntry ) )
			elog(ERROR, "non-targetlist argument supplied");

		maxSortGroupRef_walker((Node*)targetlist, &context);
	}

	return context.maxsgr;
}
