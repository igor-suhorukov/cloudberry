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
 * aqumv.c
 *	  Answering a query from a materialized view (AQUMV).
 *
 * Cloudberry, with enable_answer_query_using_materialized_views on, asks in
 * grouping_planner(), once query_planner() has planned the query's scan, of
 * every materialized view that is up to date -- or incremental, which its
 * maintenance keeps so -- and reads the query's one table: can the query be
 * computed from the view's rows?  Where it can, the query is rewritten to
 * read the view, its scan planned, and kept if it costs less.
 *
 * PostgreSQL 19 has no hook there, so this is a planner_hook around the rest
 * of the chain (gp_orca's, gp_core's, PostgreSQL's planner): it rewrites the
 * query before planning, as it would have been rewritten there, plans the
 * query and each rewritten one through the chain, and keeps the one that
 * costs least -- the whole plan's cost, where Cloudberry compares the scans'.
 * ORCA plans the rewritten query where it plans the query, so AQUMV works
 * under ORCA too, where Cloudberry's does only when ORCA falls back;
 * gp.aqumv_under_orca off gives Cloudberry's back.  A plan ORCA made is
 * never weighed against the planner's, whose costs are another scale.
 *
 * The rewrite is Cloudberry's (aqumv.c), for a query of one table:
 *
 *	- every expression of the query is rewritten to the view's columns, the
 *	  largest of the view's expressions it contains first, and a column of
 *	  the table the view does not have fails it;
 *	- the view's conditions have to be among the query's, and the rest are
 *	  applied to the view's rows;
 *	- a view without aggregates answers any such query, its grouping,
 *	  ordering, DISTINCT and LIMIT carried over; a view that aggregates
 *	  without GROUP BY answers a query that does too, over the same rows;
 *	  and a grouped view a query grouped by the same expressions, over the
 *	  same rows.
 *
 * And, for a query of several tables, a view whose query is the query --
 * the same tables, joins, conditions and columns -- answers it whole.
 *
 * Both sides are seen here before the planner has made them its own, so the
 * conditions are made alike first, as Cloudberry's are by then: constants
 * folded, a boolean expression in its canonical form, a list of ANDed
 * conditions, and a HAVING that aggregates nothing moved into WHERE; and
 * the grouped columns PostgreSQL 19 reads from its GROUP range table entry
 * are the expressions again.
 *
 * What differs from Cloudberry, besides where it runs:
 *
 *	- the rewritten query's scan of the view is checked as the view's owner
 *	  reads it, and the query's own table, left in the plan's range table
 *	  unread, as the query's user reads it: the privileges it asked for are
 *	  the ones checked, and a plan the plan cache keeps is made again when
 *	  the table changes.  Cloudberry's rewrite checks the view's own query's
 *	  privileges, which PostgreSQL's executor refuses to take for another
 *	  relation's.
 *	- a table with row-level security is not answered for, whose policies a
 *	  view would not apply; nor a declared cursor, whose WHERE CURRENT OF
 *	  would find the view's rows rather than the table's.
 *	- a view that aggregates without GROUP BY answers only a query that does
 *	  not group either, and a grouped view only a grouped query: Cloudberry
 *	  answered one from the other, a row for a group or groups for a row.
 *	- an ungrouped aggregate's ORDER BY, which one row makes idle, is
 *	  dropped with the columns only it reads, where Cloudberry kept the
 *	  columns and failed on them.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/optimizer/plan/aqumv.c, and its call in planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_core_api.h"
#include "gp_matview.h"

/* Cloudberry's enable_answer_query_using_materialized_views, and its kin. */
bool		gp_aqumv_enabled = false;
bool		gp_aqumv_allow_foreign_table = false;
bool		gp_aqumv_under_orca = true;

static planner_hook_type prev_planner = NULL;

/* Inside CREATE ... AS and REFRESH, whose queries are the view's own. */
static int	aqumv_skip = 0;

/* How a plan says ORCA made it (gp_orca_planner.c). */
#define GP_ORCA_PLAN_MARK	"gp_orca"

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

static PlannedStmt *
plan_next(Query *parse, const char *query_string, int cursorOptions,
		  ParamListInfo boundParams, ExplainState *es)
{
	if (prev_planner)
		return prev_planner(parse, query_string, cursorOptions, boundParams, es);
	return standard_planner(parse, query_string, cursorOptions, boundParams, es);
}

static bool
planned_by_orca(PlannedStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->extension_state)
		if (strcmp(lfirst_node(DefElem, lc)->defname, GP_ORCA_PLAN_MARK) == 0)
			return true;
	return false;
}

void
GpAqumvSkip(bool enter)
{
	aqumv_skip += enter ? 1 : -1;
}

/* ------------------------------------------------------------------------- */
/* Making a query's and a view's expressions alike                           */
/* ------------------------------------------------------------------------- */

/*
 * One side of the comparison: a query level, and its expressions made as
 * the planner has them when Cloudberry compares -- the grouped columns the
 * expressions again, constants folded, the WHERE a list of ANDed conditions.
 */
typedef struct Side
{
	Query	   *query;
	Index		varno;			/* the one table's range table index */
	RangeTblEntry *rte;
	List	   *tlist;
	List	   *quals;
	Node	   *having;
} Side;

/*
 * A parameter a custom plan is made for, its value: as eval_const_expressions()
 * substitutes it when the planner has the parameters to hand -- which it is
 * not given here, since with them it asks a planner's state this has none of.
 */
static Node *
bind_params(Node *node, ParamListInfo params)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param) && params != NULL &&
		((Param *) node)->paramkind == PARAM_EXTERN &&
		((Param *) node)->paramid > 0 &&
		((Param *) node)->paramid <= params->numParams)
	{
		Param	   *param = (Param *) node;
		ParamExternData prmdata;
		ParamExternData *prm;

		prm = params->paramFetch != NULL
			? params->paramFetch(params, param->paramid, false, &prmdata)
			: &params->params[param->paramid - 1];
		if (OidIsValid(prm->ptype) && (prm->pflags & PARAM_FLAG_CONST) &&
			prm->ptype == param->paramtype)
		{
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(param->paramtype, &typlen, &typbyval);
			return (Node *) makeConst(param->paramtype, param->paramtypmod,
									  param->paramcollid, typlen,
									  prm->isnull ? (Datum) 0
									  : datumCopy(prm->value, typbyval, typlen),
									  prm->isnull, typbyval);
		}
	}
	return expression_tree_mutator(node, bind_params, params);
}

static Node *
folded(ParamListInfo params, Node *node)
{
	return node != NULL ? eval_const_expressions(NULL, bind_params(node, params))
		: NULL;
}

static List *
qual_list(ParamListInfo params, Node *quals)
{
	if (quals == NULL)
		return NIL;
	quals = folded(params, quals);
	quals = (Node *) canonicalize_qual((Expr *) quals, false);
	return make_ands_implicit((Expr *) quals);
}

static void
side_init(Side *side, Query *query, Index varno, ParamListInfo params,
		  bool move_having)
{
	List	   *tlist = query->targetList;
	Node	   *having = query->havingQual;
	ListCell   *lc;

	side->query = query;
	side->varno = varno;
	side->rte = rt_fetch(varno, query->rtable);

	/* PostgreSQL 19's grouped columns, the expressions again */
	if (query->hasGroupRTE)
	{
		tlist = (List *) flatten_group_exprs(NULL, query, (Node *) tlist);
		having = flatten_group_exprs(NULL, query, having);
	}

	side->tlist = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tle = flatCopyTargetEntry(lfirst_node(TargetEntry, lc));

		tle->expr = (Expr *) folded(params, (Node *) tle->expr);
		side->tlist = lappend(side->tlist, tle);
	}
	side->quals = qual_list(params, query->jointree->quals);
	side->having = folded(params, having);

	/*
	 * A HAVING condition that aggregates nothing is a WHERE condition, as
	 * subquery_planner() moves it, which is where Cloudberry's AQUMV finds
	 * it.
	 */
	if (move_having && side->having != NULL && query->groupingSets == NIL)
	{
		Expr	   *canonical = canonicalize_qual((Expr *) side->having, false);
		List	   *kept = NIL;

		foreach(lc, make_ands_implicit(canonical))
		{
			Node	   *cond = (Node *) lfirst(lc);

			if (contain_agg_clause(cond) || contain_volatile_functions(cond) ||
				contain_subplans(cond))
				kept = lappend(kept, cond);
			else
				side->quals = lappend(side->quals, cond);
		}
		side->having = kept != NIL ? (Node *) make_ands_explicit(kept) : NULL;
	}
}

/* ------------------------------------------------------------------------- */
/* Rewriting a query's expressions to a view's columns                       */
/* ------------------------------------------------------------------------- */

typedef struct MapContext
{
	List	   *targets;		/* the view's columns' expressions, largest first */
	TupleDesc	desc;			/* the view's */
	bool		unmatched;		/* a column the view does not have */
} MapContext;

/* Is there a column of the level, or count(*), in the expression? */
static bool
has_var_or_count_star(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref) && ((Aggref *) node)->aggstar)
		return true;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup == 0;
	if (IsA(node, PlaceHolderVar) && ((PlaceHolderVar *) node)->phlevelsup == 0)
		return true;
	return expression_tree_walker(node, has_var_or_count_star, context);
}

/* How many nodes an expression has: the larger are tried first. */
static bool
count_nodes(Node *node, void *context)
{
	if (node == NULL)
		return false;
	(*(int *) context)++;
	return expression_tree_walker(node, count_nodes, context);
}

static int
larger_first(const ListCell *a, const ListCell *b)
{
	int			na = 0;
	int			nb = 0;

	(void) count_nodes((Node *) lfirst_node(TargetEntry, a)->expr, &na);
	(void) count_nodes((Node *) lfirst_node(TargetEntry, b)->expr, &nb);
	return nb - na;
}

/*
 * The view's columns an expression may be rewritten to: every one computed
 * from the table -- a constant's is not needed, the query computes it
 * itself -- and none of incremental maintenance's hidden ones.
 */
static void
map_init(MapContext *cx, Side *view, TupleDesc desc)
{
	ListCell   *lc;

	cx->targets = NIL;
	foreach(lc, view->tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk || tle->resno > desc->natts ||
			IsIvmColumn(NameStr(TupleDescAttr(desc, tle->resno - 1)->attname)) ||
			!has_var_or_count_star((Node *) tle->expr, NULL))
			continue;
		cx->targets = lappend(cx->targets, tle);
	}
	list_sort(cx->targets, larger_first);
	cx->desc = desc;
	cx->unmatched = false;
}

/*
 * An expression of the query, with each part the view computes replaced by
 * the view's column: Cloudberry's aqumv_adjust_sub_matched_expr_mutator().
 */
static Node *
map_mutator(Node *node, MapContext *cx)
{
	ListCell   *lc;

	if (node == NULL || cx->unmatched)
		return node;
	if (IsA(node, TargetEntry))
	{
		TargetEntry *tle = flatCopyTargetEntry((TargetEntry *) node);

		tle->expr = (Expr *) map_mutator((Node *) tle->expr, cx);
		return (Node *) tle;
	}
	if (IsA(node, Const))
		return node;

	foreach(lc, cx->targets)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (equal(node, tle->expr))
		{
			Form_pg_attribute att = TupleDescAttr(cx->desc, tle->resno - 1);

			return (Node *) makeVar(1, att->attnum, att->atttypid,
									att->atttypmod, att->attcollation, 0);
		}
	}

	if (!has_var_or_count_star(node, NULL))
		return node;
	if (IsA(node, Var))
	{
		cx->unmatched = true;
		return node;
	}
	return expression_tree_mutator(node, map_mutator, cx);
}

static Node *
map_expr(Node *node, MapContext *cx)
{
	return map_mutator(copyObject(node), cx);
}

/*
 * The view's conditions have to be among the query's: the rest, into
 * *post, are the ones its rows are filtered by.  Cloudberry's
 * aqumv_process_from_quals().
 */
static bool
split_quals(List *query_quals, List *view_quals, List **post)
{
	ListCell   *lc;

	foreach(lc, view_quals)
		if (!list_member(query_quals, lfirst(lc)))
			return false;
	*post = NIL;
	foreach(lc, query_quals)
		if (!list_member(view_quals, lfirst(lc)))
			*post = lappend(*post, lfirst(lc));
	return true;
}

/* The expressions a query groups by, as a list. */
static List *
grouping_exprs(Side *side)
{
	List	   *exprs = NIL;
	ListCell   *lc;

	foreach(lc, side->query->groupClause)
		exprs = lappend(exprs,
						get_sortgroupclause_expr(lfirst_node(SortGroupClause, lc),
												 side->tlist));
	return exprs;
}

static bool
same_members(List *a, List *b)
{
	ListCell   *lc;

	foreach(lc, a)
		if (!list_member(b, lfirst(lc)))
			return false;
	foreach(lc, b)
		if (!list_member(a, lfirst(lc)))
			return false;
	return true;
}

/* Is a LIMIT or an OFFSET, where there is one, a constant? */
static bool
constant_limits(Query *query)
{
	return (query->limitCount == NULL || IsA(query->limitCount, Const)) &&
		(query->limitOffset == NULL || IsA(query->limitOffset, Const));
}

/* ------------------------------------------------------------------------- */
/* The rewritten query                                                       */
/* ------------------------------------------------------------------------- */

/*
 * A query of the view alone, in the query's place: its range table the
 * view's entry, read as the view's owner reads it -- the query's user's own
 * privileges are checked on its table, which patch_up() puts back beside
 * the plan (see the file's header).
 */
static Query *
view_query(Query *query, Relation matviewRel, List *tlist, List *quals)
{
	Query	   *rw = makeNode(Query);
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RTEPermissionInfo *perminfo = makeNode(RTEPermissionInfo);
	RangeTblRef *rtr = makeNode(RangeTblRef);
	TupleDesc	desc = RelationGetDescr(matviewRel);
	List	   *colnames = NIL;

	for (int i = 0; i < desc->natts; i++)
		colnames = lappend(colnames,
						   makeString(pstrdup(NameStr(TupleDescAttr(desc, i)->attname))));

	/* as the parser makes a FROM item that is not ONLY, which ORCA plans */
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(matviewRel);
	rte->relkind = RELKIND_MATVIEW;
	rte->rellockmode = AccessShareLock;
	rte->inh = true;
	rte->inFromCl = true;
	rte->eref = makeAlias(RelationGetRelationName(matviewRel), colnames);
	rte->perminfoindex = 1;

	perminfo->relid = rte->relid;
	perminfo->inh = true;
	perminfo->requiredPerms = ACL_SELECT;
	perminfo->checkAsUser = matviewRel->rd_rel->relowner;

	rtr->rtindex = 1;

	rw->commandType = CMD_SELECT;
	rw->querySource = query->querySource;
	rw->queryId = query->queryId;
	rw->canSetTag = query->canSetTag;
	rw->rtable = list_make1(rte);
	rw->rteperminfos = list_make1(perminfo);
	rw->jointree = makeFromExpr(list_make1(rtr),
								quals != NIL ? (Node *) make_ands_explicit(quals) : NULL);
	rw->targetList = tlist;
	rw->hasTargetSRFs = expression_returns_set((Node *) tlist);
	rw->stmt_location = query->stmt_location;
	rw->stmt_len = query->stmt_len;
	return rw;
}

/*
 * A query of one table, rewritten to read the view -- whose query is
 * "view", of the same table -- or NULL where the view cannot answer it.
 * Cloudberry's answer_query_using_materialized_views(), for one view.
 */
static Query *
rewrite_single(Side *q, Side *view, Relation matviewRel)
{
	Query	   *query = q->query;
	Query	   *vq = view->query;
	MapContext	cx;
	List	   *post;
	List	   *tlist = NIL;
	Query	   *rw;
	ListCell   *lc;

	if (!query->hasAggs && vq->hasAggs)
		return NULL;

	map_init(&cx, view, RelationGetDescr(matviewRel));

	if (query->groupClause != NIL && vq->groupClause != NIL)
	{
		/*
		 * Both grouped: by the same expressions, over the same rows -- a row
		 * of the view is a group of the query.
		 */
		if (query->distinctClause != NIL || query->hasDistinctOn ||
			query->groupingSets != NIL || query->groupDistinct ||
			query->sortClause != NIL || query->limitCount != NULL ||
			query->limitOffset != NULL || q->having != NULL ||
			query->hasTargetSRFs)
			return NULL;
		if (vq->groupingSets != NIL || vq->groupDistinct)
			return NULL;
		if (!split_quals(q->quals, view->quals, &post) || post != NIL)
			return NULL;
		if (!same_members(grouping_exprs(q), grouping_exprs(view)))
			return NULL;
		tlist = (List *) map_expr((Node *) q->tlist, &cx);
		if (cx.unmatched)
			return NULL;
		rw = view_query(query, matviewRel, tlist, NIL);
		return rw;
	}

	if (vq->hasAggs)
	{
		/*
		 * Both aggregate without GROUP BY: the view is one row, the query's
		 * one row over the same rows.  Its ORDER BY is idle over one row, and
		 * goes with the columns only it reads; its HAVING filters the one
		 * row.
		 */
		List	   *sortrefs = NIL;

		if (query->groupClause != NIL || vq->groupClause != NIL)
			return NULL;
		if (query->distinctClause != NIL || query->hasDistinctOn ||
			query->groupingSets != NIL || query->groupDistinct ||
			query->hasTargetSRFs || !constant_limits(query))
			return NULL;
		if (!split_quals(q->quals, view->quals, &post) || post != NIL)
			return NULL;

		foreach(lc, query->sortClause)
			sortrefs = lappend_int(sortrefs,
								   lfirst_node(SortGroupClause, lc)->tleSortGroupRef);
		foreach(lc, q->tlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (tle->resjunk && list_member_int(sortrefs, tle->ressortgroupref))
				continue;
			tle = (TargetEntry *) map_expr((Node *) tle, &cx);
			tle->ressortgroupref = 0;
			tle->resno = list_length(tlist) + 1;
			tlist = lappend(tlist, tle);
		}
		post = q->having != NULL
			? make_ands_implicit((Expr *) map_expr(q->having, &cx)) : NIL;
		if (cx.unmatched)
			return NULL;
		rw = view_query(query, matviewRel, tlist, post);
		rw->limitCount = copyObject(query->limitCount);
		rw->limitOffset = copyObject(query->limitOffset);
		rw->limitOption = query->limitOption;
		return rw;
	}

	/*
	 * A view without aggregates: a row of it is a row of the table, so the
	 * query is the view's rows filtered by the conditions it has besides the
	 * view's, grouped, ordered and limited as the query is.
	 */
	if (vq->groupClause != NIL)
		return NULL;
	if (!split_quals(q->quals, view->quals, &post))
		return NULL;
	tlist = (List *) map_expr((Node *) q->tlist, &cx);
	post = (List *) map_expr((Node *) post, &cx);
	rw = view_query(query, matviewRel, tlist, post);
	rw->havingQual = map_expr(q->having, &cx);
	if (cx.unmatched)
		return NULL;
	rw->hasAggs = query->hasAggs;
	rw->groupClause = copyObject(query->groupClause);
	rw->groupingSets = copyObject(query->groupingSets);
	rw->groupDistinct = query->groupDistinct;
	rw->sortClause = copyObject(query->sortClause);
	rw->distinctClause = copyObject(query->distinctClause);
	rw->hasDistinctOn = query->hasDistinctOn;
	rw->limitCount = copyObject(query->limitCount);
	rw->limitOffset = copyObject(query->limitOffset);
	rw->limitOption = query->limitOption;
	return rw;
}

/*
 * A query of several tables that is the view's own query, rewritten to read
 * the view's columns, or NULL: Cloudberry's aqumv_query_is_exact_match() and
 * answer_query_using_materialized_views_for_join().
 */
static Query *
rewrite_join(Query *query, Query *vq, Relation matviewRel)
{
	TupleDesc	desc = RelationGetDescr(matviewRel);
	List	   *tlist = NIL;
	ListCell   *lc1;
	ListCell   *lc2;
	Query	   *rw;
	int			attnum = 0;

	if (!equal(query->rtable, vq->rtable) ||
		!equal(query->jointree, vq->jointree) ||
		list_length(query->targetList) != list_length(vq->targetList) ||
		!equal(query->groupClause, vq->groupClause) ||
		query->groupDistinct != vq->groupDistinct ||
		!equal(query->havingQual, vq->havingQual) ||
		!equal(query->sortClause, vq->sortClause) ||
		!equal(query->distinctClause, vq->distinctClause) ||
		!equal(query->limitCount, vq->limitCount) ||
		!equal(query->limitOffset, vq->limitOffset) ||
		query->limitOption != vq->limitOption ||
		query->hasAggs != vq->hasAggs ||
		query->hasWindowFuncs != vq->hasWindowFuncs ||
		query->hasDistinctOn != vq->hasDistinctOn)
		return NULL;

	forboth(lc1, query->targetList, lc2, vq->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc1);
		TargetEntry *vtle = lfirst_node(TargetEntry, lc2);
		Form_pg_attribute att;
		TargetEntry *ntle;

		/* the view has no column an ORDER BY of something else could read */
		if (!equal(tle->expr, vtle->expr) || tle->resjunk || vtle->resjunk ||
			tle->ressortgroupref != vtle->ressortgroupref)
			return NULL;
		att = TupleDescAttr(desc, attnum++);
		ntle = makeTargetEntry((Expr *) makeVar(1, att->attnum, att->atttypid,
												att->atttypmod, att->attcollation, 0),
							   (AttrNumber) attnum, tle->resname, false);
		ntle->ressortgroupref = tle->ressortgroupref;
		tlist = lappend(tlist, ntle);
	}

	rw = view_query(query, matviewRel, tlist, NIL);
	rw->sortClause = copyObject(query->sortClause);
	rw->limitCount = copyObject(query->limitCount);
	rw->limitOffset = copyObject(query->limitOffset);
	rw->limitOption = query->limitOption;
	return rw;
}

/* ------------------------------------------------------------------------- */
/* Which queries, and which views                                            */
/* ------------------------------------------------------------------------- */

/*
 * The level of a statement AQUMV may answer: a SELECT, or an INSERT's
 * SELECT, as Cloudberry answers each query level its planner plans.  Its
 * place, where it is a subquery, in *rti.
 */
static Query *
answerable_level(Query *parse, Index *rti)
{
	Node	   *jt;

	*rti = 0;
	if (parse->commandType == CMD_SELECT)
		return parse;
	if (parse->commandType != CMD_INSERT || parse->onConflict != NULL ||
		list_length(parse->jointree->fromlist) != 1)
		return NULL;
	jt = (Node *) linitial(parse->jointree->fromlist);
	if (!IsA(jt, RangeTblRef) ||
		rt_fetch(((RangeTblRef *) jt)->rtindex,
				 parse->rtable)->rtekind != RTE_SUBQUERY)
		return NULL;
	*rti = ((RangeTblRef *) jt)->rtindex;
	return rt_fetch(*rti, parse->rtable)->subquery;
}

/*
 * Does the table have row-level security, whose policies a view would not
 * apply?
 */
static bool
has_row_security(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		result;

	if (!HeapTupleIsValid(tup))
		return false;
	result = ((Form_pg_class) GETSTRUCT(tup))->relrowsecurity;
	ReleaseSysCache(tup);
	return result;
}

/*
 * Can the level be answered from a view at all, and of which tables is it?
 * The tables into *relids; for a query of one table, its place in *varno.
 */
static bool
level_eligible(Query *q, List **relids, Index *varno)
{
	ListCell   *lc;
	int			rti = 0;
	int			nrels = 0;

	*relids = NIL;
	*varno = 0;
	if (q->commandType != CMD_SELECT || q->rowMarks != NIL ||
		q->cteList != NIL || q->setOperations != NULL || q->hasWindowFuncs ||
		q->hasModifyingCTE || q->hasSubLinks || q->hasRecursive)
		return false;
	if (contain_mutable_functions((Node *) q))
		return false;

	foreach(lc, q->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		char		relkind;

		rti++;
		if (rte->rtekind == RTE_GROUP || rte->rtekind == RTE_JOIN)
			continue;
		if (rte->rtekind != RTE_RELATION || rte->tablesample != NULL ||
			rte->securityQuals != NIL || IsCatalogRelationOid(rte->relid))
			return false;
		relkind = get_rel_relkind(rte->relid);
		if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
			relkind != RELKIND_FOREIGN_TABLE)
			return false;
		if (relkind == RELKIND_FOREIGN_TABLE && !gp_aqumv_allow_foreign_table)
			return false;
		if (relkind != RELKIND_PARTITIONED_TABLE &&
			!get_rel_relispartition(rte->relid) &&
			(has_superclass(rte->relid) || has_subclass(rte->relid)))
			return false;
		if (has_row_security(rte->relid))
			return false;
		*relids = list_append_unique_oid(*relids, rte->relid);
		*varno = rti;
		nrels++;
	}

	/* one table, the one FROM item; or a join, a self-join's too */
	if (nrels == 1 && list_length(q->jointree->fromlist) == 1 &&
		IsA(linitial(q->jointree->fromlist), RangeTblRef) &&
		((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex == (int) *varno)
		return true;
	*varno = 0;
	return nrels > 1;
}

/*
 * A view that may answer: populated, up to date -- or incremental, which
 * its maintenance keeps so -- not unlogged, whose rows a crash empties, and
 * readable by its owner, as whom the rewritten query reads it; a foreign
 * table's only where gp.aqumv_allow_foreign_table says so.
 */
static bool
view_usable(Relation matviewRel)
{
	bool		has_foreign;
	char		status;

	if (!RelationIsPopulated(matviewRel) ||
		matviewRel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED ||
		pg_class_aclcheck(RelationGetRelid(matviewRel),
						  matviewRel->rd_rel->relowner, ACL_SELECT) != ACLCHECK_OK)
		return false;
	status = GpMvauxStatus(RelationGetRelid(matviewRel), &has_foreign);
	if (has_foreign && !gp_aqumv_allow_foreign_table)
		return false;
	return GpIvmIsIncremental(RelationGetRelid(matviewRel)) ||
		status == 'u' || status == 'r';
}

/*
 * A view's query, for comparing with a query's: its rule's, without the
 * columns incremental maintenance counts with.  Cloudberry's refusals of a
 * view query it cannot answer from.
 */
static Query *
comparable_view_query(Relation matviewRel)
{
	Query	   *vq = copyObject(GpIvmGetViewQuery(matviewRel));

	if (vq->hasWindowFuncs || vq->hasDistinctOn || vq->hasModifyingCTE ||
		vq->hasSubLinks || vq->limitCount != NULL || vq->limitOffset != NULL ||
		vq->rowMarks != NIL || vq->distinctClause != NIL || vq->cteList != NIL ||
		vq->setOperations != NULL ||
		(!vq->hasAggs && vq->groupClause != NIL) ||
		(vq->havingQual != NULL && vq->groupClause == NIL))
		return NULL;
	return vq;
}

/* The view's one table, the query's, where it has one; its place in *varno. */
static bool
view_reads(Query *vq, Oid relid, Index *varno)
{
	Node	   *jt;
	RangeTblEntry *rte;

	if (list_length(vq->jointree->fromlist) != 1)
		return false;
	jt = (Node *) linitial(vq->jointree->fromlist);
	if (!IsA(jt, RangeTblRef))
		return false;
	*varno = ((RangeTblRef *) jt)->rtindex;
	rte = rt_fetch(*varno, vq->rtable);
	return rte->rtekind == RTE_RELATION && rte->relid == relid;
}

/* ------------------------------------------------------------------------- */
/* The plan chosen                                                           */
/* ------------------------------------------------------------------------- */

/*
 * A plan's cost, for weighing a plan of a view against the statement's.  On
 * a cluster the planner's route costs a gather by the rows it brings, and
 * not by what the segments read to find them, where Cloudberry's plans cost
 * the segments' scans too: so there the segments' reading of each table the
 * plan reads is added, as a sequential scan of it costs.  ORCA's costs have
 * it already, and so has one node's plan.
 */
static Cost
comparable_cost(PlannedStmt *stmt)
{
	const GpCoreApi *core = GpCoreApiLookup();
	Cost		cost = stmt->planTree->total_cost;
	ListCell   *lc;

	if (core == NULL || core->is_single_node() || planned_by_orca(stmt))
		return cost;
	foreach(lc, stmt->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		HeapTuple	tup;
		Form_pg_class form;

		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
			continue;
		tup = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
		if (!HeapTupleIsValid(tup))
			continue;
		form = (Form_pg_class) GETSTRUCT(tup);
		cost += form->relpages * seq_page_cost +
			Max(form->reltuples, 0) * cpu_tuple_cost;
		ReleaseSysCache(tup);
	}
	return cost;
}

/*
 * The query's own tables, beside a plan that reads a view instead: in its
 * range table, unread, with the privileges the query's user asked for of
 * them, which the executor checks; and in the relations a cached plan is
 * made again for.
 */
static void
patch_up(PlannedStmt *stmt, Query *level)
{
	ListCell   *lc;

	foreach(lc, level->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		RangeTblEntry *copy;

		if (rte->rtekind != RTE_RELATION)
			continue;
		copy = copyObject(rte);
		copy->rellockmode = AccessShareLock;
		copy->securityQuals = NIL;
		if (rte->perminfoindex != 0)
		{
			stmt->permInfos = lappend(stmt->permInfos,
									  copyObject(getRTEPermissionInfo(level->rteperminfos, rte)));
			copy->perminfoindex = list_length(stmt->permInfos);
		}
		stmt->rtable = lappend(stmt->rtable, copy);
		stmt->relationOids = lappend_oid(stmt->relationOids, rte->relid);
	}
}

/* The statement with its answerable level replaced by "rw". */
static Query *
with_level(Query *parse, Index rti, Query *rw)
{
	Query	   *copy;

	if (rti == 0)
		return rw;
	copy = copyObject(parse);
	rt_fetch(rti, copy->rtable)->subquery = rw;
	return copy;
}

/*
 * planner_hook: plan the statement, and each rewrite of it that reads a
 * view instead, and keep the cheapest.
 */
static PlannedStmt *
gp_aqumv_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams, ExplainState *es)
{
	const GpCoreApi *core = GpCoreApiLookup();
	Query	   *level;
	Index		rti;
	Index		varno;
	List	   *relids;
	List	   *views;
	PlannedStmt *best;
	bool		best_orca;
	Cost		best_cost;
	bool		answered = false;
	Side		q;
	ListCell   *lc;
	int			nestlevel;

	if (!gp_aqumv_enabled || aqumv_skip > 0 || GpMvauxBusy() ||
		GpIvmClusterMaintaining() || InSecurityRestrictedOperation() ||
		(cursorOptions & CURSOR_OPT_FAST_PLAN) != 0 ||
		(core != NULL && core->get_role() == GP_ROLE_EXECUTE))
		return plan_next(parse, query_string, cursorOptions, boundParams, es);

	level = answerable_level(parse, &rti);
	if (level == NULL || !level_eligible(level, &relids, &varno) ||
		(views = GpMvauxViewsOver(relids)) == NIL)
		return plan_next(parse, query_string, cursorOptions, boundParams, es);

	/* The statement as it is, planned as it always is. */
	best = plan_next(copyObject(parse), query_string, cursorOptions,
					 boundParams, es);
	best_orca = planned_by_orca(best);
	best_cost = comparable_cost(best);

	/*
	 * Cloudberry's ORCA does not answer from views; the planner does, when
	 * ORCA falls back.  gp.aqumv_under_orca off is that: a statement ORCA
	 * planned is left as it is, and the rewritten ones are the planner's.
	 */
	nestlevel = NewGUCNestLevel();
	if (!gp_aqumv_under_orca)
	{
		if (best_orca)
		{
			AtEOXact_GUC(false, nestlevel);
			return best;
		}
		if (GetConfigOption("gp.optimizer", true, false) != NULL)
			(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
									 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0,
									 false);
	}
	/* one fallback's INFO line for the statement, not one for each view */
	if (GetConfigOption("gp.optimizer_trace_fallback", true, false) != NULL)
		(void) set_config_option("gp.optimizer_trace_fallback", "off",
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE,
								 true, 0, false);

	if (varno != 0)
		side_init(&q, level, varno, boundParams, true);

	foreach(lc, views)
	{
		Relation	matviewRel = table_open(lfirst_oid(lc), AccessShareLock);
		Query	   *vq = NULL;
		Query	   *rw = NULL;
		PlannedStmt *plan;

		if (view_usable(matviewRel))
			vq = comparable_view_query(matviewRel);

		if (vq != NULL && varno != 0)
		{
			Index		vvarno;
			Side		view;

			if (view_reads(vq, linitial_oid(relids), &vvarno))
			{
				if (vvarno != varno)
					ChangeVarNodes((Node *) vq, vvarno, varno, 0);
				side_init(&view, vq, varno, NULL, false);
				rw = rewrite_single(&q, &view, matviewRel);
			}
		}
		else if (vq != NULL)
			rw = rewrite_join(level, vq, matviewRel);

		if (rw == NULL)
		{
			table_close(matviewRel, AccessShareLock);
			continue;
		}

		plan = plan_next(with_level(parse, rti, rw), query_string,
						 cursorOptions, boundParams, es);
		if (planned_by_orca(plan) == best_orca &&
			comparable_cost(plan) < best_cost)
		{
			best = plan;
			best_cost = comparable_cost(plan);
			answered = true;
			/* the lock the plan's scan of the view needs is kept */
			table_close(matviewRel, NoLock);
		}
		else
			table_close(matviewRel, AccessShareLock);
	}
	AtEOXact_GUC(false, nestlevel);

	if (answered)
		patch_up(best, level);
	return best;
}

void
GpAqumvInit(void)
{
	prev_planner = planner_hook;
	planner_hook = gp_aqumv_planner;
}
