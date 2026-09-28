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
 * gp_subselect.c
 *	  The planner's route: a correlated scalar subquery of an aggregate made a
 *	  join with its rows grouped by the correlation's columns.
 *
 * PostgreSQL plans
 *
 *	   SELECT ... FROM partsupp
 *		WHERE ps_availqty > (SELECT 0.5 * sum(l_quantity) FROM lineitem
 *							  WHERE l_partkey = ps_partkey
 *								AND l_suppkey = ps_suppkey AND ...)
 *
 * -- TPC-H's query 20 -- as a SubPlan run for each row of partsupp.  On the
 * planner's route each run gathers lineitem from the segments again, its
 * correlation's parameters not sent (gp_scan.c): some twenty seconds for each
 * of rpt_tpch's three tables.  Cloudberry's planner makes a join of such a
 * subquery (convert_EXPR_to_join(), cdbsubselect.c):
 *
 *	   SELECT ... FROM partsupp,
 *			  (SELECT l_partkey, l_suppkey, 0.5 * sum(l_quantity)
 *				 FROM lineitem WHERE ... GROUP BY l_partkey, l_suppkey) gp_s
 *		WHERE ps_availqty > gp_s.agg
 *		  AND gp_s.l_partkey = ps_partkey AND gp_s.l_suppkey = ps_suppkey
 *
 * and so does this file, on the Query, as the planner's route is asked to
 * plan it, for a subquery that reads a distributed table -- one gather of
 * lineitem, grouped once.
 *
 * Cloudberry makes the join whatever the aggregate, and a row of partsupp
 * with no lineitem row is then dropped where the subquery would have given
 * its aggregate over no rows: NULL for sum(), which the comparison drops too,
 * but 0 for count(), which it may not.  Here the join is made only where it
 * gives the subquery's answer: the comparison is strict and a top-level
 * condition of the WHERE, so a NULL drops the row; and the subquery's value
 * is NULL over no rows -- an expression strict in its aggregates, one of
 * which is NULL over no rows (sum(), avg(), min(), max() and their kin, not
 * count()).  A correlation is an equality of the outer query's expression and
 * the subquery's own, at its WHERE's top level; the subquery has no GROUP BY,
 * HAVING, window, LIMIT, DISTINCT or set operation, one column, and no other
 * reference to the outer query.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/cdbsubselect.c (convert_EXPR_to_join() and
 *	  safe_to_convert_EXPR()), and its call in pull_up_sublinks_qual_recurse()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_scan.h"
#include "gp_subselect.h"

/*
 * The built-in aggregates whose value over no rows is NULL -- count() and
 * regr_count(), 0 over no rows, are not among them.
 */
static const char *const null_on_empty_aggs[] = {
	"sum", "avg", "min", "max", "stddev", "stddev_pop", "stddev_samp",
	"variance", "var_pop", "var_samp", "bit_and", "bit_or", "bit_xor",
	"bool_and", "bool_or", "every", "string_agg", "array_agg", "json_agg",
	"jsonb_agg", "xmlagg", "corr", "covar_pop", "covar_samp", "regr_avgx",
	"regr_avgy", "regr_intercept", "regr_r2", "regr_slope", "regr_sxx",
	"regr_sxy", "regr_syy", "range_agg", "range_intersect_agg", "any_value",
};

static bool
agg_null_on_empty(Oid aggfnoid)
{
	HeapTuple	tp;
	Form_pg_proc proc;
	bool		result = false;

	if (aggfnoid >= FirstNormalObjectId)
		return false;
	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tp))
		return false;
	proc = (Form_pg_proc) GETSTRUCT(tp);
	if (proc->pronamespace == PG_CATALOG_NAMESPACE)
	{
		for (int i = 0; i < lengthof(null_on_empty_aggs); i++)
			if (strcmp(NameStr(proc->proname), null_on_empty_aggs[i]) == 0)
				result = true;
	}
	ReleaseSysCache(tp);
	return result;
}

/*
 * Is the subquery's value NULL over no rows?  Its expression is strict all
 * the way down to its aggregates and constants, and one of the aggregates is
 * NULL over no rows, so the expression is NULL too.  *found says whether one
 * was met.
 */
static bool
null_over_no_rows(Node *node, bool *found)
{
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Aggref:
			{
				Aggref	   *agg = (Aggref *) node;

				if (agg->agglevelsup != 0 || agg->aggkind != AGGKIND_NORMAL)
					return false;
				if (agg_null_on_empty(agg->aggfnoid))
					*found = true;
				return true;
			}
		case T_Const:
			return true;
		case T_RelabelType:
			return null_over_no_rows((Node *) ((RelabelType *) node)->arg, found);
		case T_CoerceViaIO:
			return null_over_no_rows((Node *) ((CoerceViaIO *) node)->arg, found);
		case T_FuncExpr:
			if (!func_strict(((FuncExpr *) node)->funcid))
				return false;
			foreach_ptr(Node, arg, ((FuncExpr *) node)->args)
				if (!null_over_no_rows(arg, found))
					return false;
			return true;
		case T_OpExpr:
			set_opfuncid((OpExpr *) node);
			if (!func_strict(((OpExpr *) node)->opfuncid))
				return false;
			foreach_ptr(Node, arg, ((OpExpr *) node)->args)
				if (!null_over_no_rows(arg, found))
					return false;
			return true;
		default:
			return false;
	}
}

/*
 * Does the expression read a query level "levelsup" above it, or one above
 * that -- a Var, a placeholder or an aggregate of one -- counting the levels
 * of the queries nested in it?
 */
static bool
reads_above_walker(Node *node, int *sublevels_up)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup >= *sublevels_up;
	if (IsA(node, PlaceHolderVar) &&
		((PlaceHolderVar *) node)->phlevelsup >= *sublevels_up)
		return true;
	if (IsA(node, Aggref) && ((Aggref *) node)->agglevelsup >= *sublevels_up)
		return true;
	if (IsA(node, GroupingFunc) &&
		((GroupingFunc *) node)->agglevelsup >= *sublevels_up)
		return true;
	if (IsA(node, Query))
	{
		bool		result;

		(*sublevels_up)++;
		result = query_tree_walker((Query *) node, reads_above_walker,
								   sublevels_up, 0);
		(*sublevels_up)--;
		return result;
	}
	return expression_tree_walker(node, reads_above_walker, sublevels_up);
}

static bool
reads_level_or_above(Node *node, int levelsup)
{
	int			sublevels_up = levelsup;

	return query_or_expression_tree_walker(node, reads_above_walker,
										   &sublevels_up, 0);
}

/* Does the query read a distributed table, a gather to each of its runs? */
static bool
reads_distributed(Query *q)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		if (rte->rtekind == RTE_RELATION &&
			GpScanDistributedPolicy(rte->relid) != NULL)
			return true;
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			reads_distributed(rte->subquery))
			return true;
	}
	return false;
}

/* One correlation: the outer query's expression = the subquery's own. */
typedef struct Correlation
{
	OpExpr	   *op;				/* as the subquery has it */
	int			inner_arg;		/* which of its arguments is the subquery's */
	Oid			eqop;			/* the subquery's side grouped by these */
	Oid			sortop;
	bool		hashable;
} Correlation;

/*
 * A condition of the subquery's WHERE that reads the outer query: an
 * equality -- a merge-joinable operator -- of an expression of the outer
 * query's columns alone and one of the subquery's own alone.  NULL for
 * anything else.
 */
static Correlation *
correlation_of(Node *node)
{
	OpExpr	   *op;
	Correlation *c;
	Node	   *left;
	Node	   *right;
	Node	   *inner;
	bool		left_outer;
	bool		right_outer;

	if (!IsA(node, OpExpr) || list_length(((OpExpr *) node)->args) != 2)
		return NULL;
	op = (OpExpr *) node;
	left = linitial(op->args);
	right = lsecond(op->args);
	left_outer = reads_level_or_above(left, 1);
	right_outer = reads_level_or_above(right, 1);
	if (left_outer == right_outer)
		return NULL;

	c = palloc0_object(Correlation);
	c->op = op;
	c->inner_arg = left_outer ? 1 : 0;
	inner = c->inner_arg == 0 ? left : right;

	/*
	 * The outer side reads the query just above alone, and its own level's
	 * columns not at all; the inner side the subquery's; neither changes
	 * from row to row of itself nor reads a subquery.
	 */
	if (reads_level_or_above(c->inner_arg == 0 ? right : left, 2) ||
		contain_var_clause(c->inner_arg == 0 ? right : left) ||
		contain_volatile_functions(node) || contain_subplans(node) ||
		checkExprHasSubLink(node) || !contain_var_clause(inner))
		return NULL;

	/* grouped by its type's own equality, sorted or hashed */
	if (!op_mergejoinable(op->opno, exprType(left)))
		return NULL;
	get_sort_group_operators(exprType(inner), false, false, false,
							 &c->sortop, &c->eqop, NULL, &c->hashable);
	if (!OidIsValid(c->eqop) || (!OidIsValid(c->sortop) && !c->hashable))
		return NULL;
	return c;
}

/*
 * The scalar subquery of "x op (SELECT ...)" at the query's WHERE's top
 * level, made a join with its rows grouped by its correlation, if that gives
 * its answer: true where it was.
 */
static bool
convert_expr_sublink(Query *q, OpExpr *cmp, int sublink_arg, List **conjuncts)
{
	SubLink    *sublink = (SubLink *) list_nth(cmp->args, sublink_arg);
	Query	   *sub = (Query *) sublink->subselect;
	Query	   *probe;
	TargetEntry *value;
	List	   *correlations = NIL;
	List	   *plain = NIL;
	Query	   *grouped;
	List	   *tlist = NIL;
	List	   *groups = NIL;
	List	   *colnames = NIL;
	RangeTblEntry *rte;
	int			rti;
	int			k = 0;
	bool		found = false;
	Var		   *valvar;

	if (sublink->subLinkType != EXPR_SUBLINK || !IsA(sub, Query) ||
		sub->commandType != CMD_SELECT)
		return false;
	if (sub->setOperations != NULL || sub->groupClause != NIL ||
		sub->groupingSets != NIL || sub->havingQual != NULL ||
		sub->windowClause != NIL || sub->hasWindowFuncs ||
		sub->limitOffset != NULL || sub->limitCount != NULL ||
		sub->distinctClause != NIL || sub->cteList != NIL ||
		sub->rowMarks != NIL || !sub->hasAggs || sub->hasTargetSRFs ||
		sub->jointree == NULL || sub->jointree->fromlist == NIL)
		return false;
	if (list_length(sub->targetList) != 1)
		return false;
	value = linitial_node(TargetEntry, sub->targetList);
	if (value->resjunk || exprType((Node *) value->expr) == RECORDOID)
		return false;

	/* the comparison drops a row whose subquery gave NULL */
	set_opfuncid(cmp);
	if (!func_strict(cmp->opfuncid) || cmp->opresulttype != BOOLOID)
		return false;
	if (!null_over_no_rows((Node *) value->expr, &found) || !found)
		return false;

	/* only a subquery whose runs are each a gather of a distributed table */
	if (!reads_distributed(sub))
		return false;

	/* nothing reads the outer query but the WHERE's correlations */
	probe = copyObject(sub);
	probe->jointree->quals = NULL;
	if (reads_level_or_above((Node *) probe, 1))
		return false;

	foreach_ptr(Node, cond, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		Correlation *c;

		if (!reads_level_or_above(cond, 1))
		{
			plain = lappend(plain, cond);
			continue;
		}
		if ((c = correlation_of(cond)) == NULL)
			return false;
		correlations = lappend(correlations, c);
	}
	if (correlations == NIL)
		return false;

	/* the subquery, grouped by the correlation's own expressions */
	grouped = copyObject(sub);
	foreach_ptr(Correlation, c, correlations)
	{
		TargetEntry *tle;
		SortGroupClause *sgc = makeNode(SortGroupClause);

		k++;
		tle = makeTargetEntry((Expr *) copyObject(list_nth(c->op->args, c->inner_arg)),
							  k, psprintf("gp_key%d", k), false);
		tle->ressortgroupref = k;
		tlist = lappend(tlist, tle);
		sgc->tleSortGroupRef = k;
		sgc->eqop = c->eqop;
		sgc->sortop = c->sortop;
		sgc->reverse_sort = false;
		sgc->nulls_first = false;
		sgc->hashable = c->hashable;
		groups = lappend(groups, sgc);
		colnames = lappend(colnames, makeString(psprintf("gp_key%d", k)));
	}
	tlist = lappend(tlist, makeTargetEntry((Expr *) copyObject(value->expr),
										   k + 1, pstrdup("gp_value"), false));
	colnames = lappend(colnames, makeString(pstrdup("gp_value")));
	grouped->targetList = tlist;
	grouped->groupClause = groups;
	grouped->sortClause = NIL;
	grouped->jointree->quals = (Node *) make_ands_explicit(plain);

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = grouped;
	rte->alias = makeAlias("gp_scalar_subquery", NIL);
	rte->eref = makeAlias("gp_scalar_subquery", colnames);
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	q->rtable = lappend(q->rtable, rte);
	rti = list_length(q->rtable);
	q->jointree->fromlist = lappend(q->jointree->fromlist, makeNode(RangeTblRef));
	((RangeTblRef *) llast(q->jointree->fromlist))->rtindex = rti;

	/* the comparison reads the group's value */
	valvar = makeVar(rti, k + 1, exprType((Node *) value->expr),
					 exprTypmod((Node *) value->expr),
					 exprCollation((Node *) value->expr), 0);
	list_nth_cell(cmp->args, sublink_arg)->ptr_value = valvar;

	/* and each correlation joins the outer row to its group */
	k = 0;
	foreach_ptr(Correlation, c, correlations)
	{
		OpExpr	   *join = copyObject(c->op);
		Node	   *outer = list_nth(join->args, 1 - c->inner_arg);
		Node	   *inner = list_nth(join->args, c->inner_arg);

		k++;
		IncrementVarSublevelsUp(outer, -1, 1);
		list_nth_cell(join->args, c->inner_arg)->ptr_value =
			makeVar(rti, k, exprType(inner), exprTypmod(inner),
					exprCollation(inner), 0);
		*conjuncts = lappend(*conjuncts, join);
	}
	return true;
}

/* One query level: each top-level condition of its WHERE, one at a time. */
static void
decorrelate_level(Query *q)
{
	List	   *conjuncts;
	bool		changed = false;

	if (q->commandType != CMD_SELECT || q->jointree == NULL ||
		q->jointree->quals == NULL || !q->hasSubLinks)
		return;

	conjuncts = make_ands_implicit((Expr *) q->jointree->quals);
	foreach_ptr(Node, cond, conjuncts)
	{
		OpExpr	   *cmp;

		if (!IsA(cond, OpExpr) || list_length(((OpExpr *) cond)->args) != 2)
			continue;
		cmp = (OpExpr *) cond;
		for (int arg = 1; arg >= 0; arg--)
		{
			if (IsA(list_nth(cmp->args, arg), SubLink) &&
				!IsA(list_nth(cmp->args, 1 - arg), SubLink) &&
				convert_expr_sublink(q, cmp, arg, &conjuncts))
			{
				changed = true;
				break;
			}
		}
	}
	if (changed)
		q->jointree->quals = (Node *) make_ands_explicit(conjuncts);
}

static bool
decorrelate_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;
		bool		result;

		/* its subqueries first, each a level of its own */
		result = query_tree_walker(q, decorrelate_walker, context, 0);
		decorrelate_level(q);
		return result;
	}
	return expression_tree_walker(node, decorrelate_walker, context);
}

/*
 * The planner's route, on the coordinator of a cluster: each correlated
 * scalar subquery of an aggregate that reads a distributed table, and can be
 * a join, made one, at every level of the query.
 */
void
GpSubselectDecorrelate(Query *parse)
{
	if (GpClusterIsSingleNode() || GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	(void) decorrelate_walker((Node *) parse, NULL);
}
