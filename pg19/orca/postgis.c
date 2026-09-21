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
 * postgis.c
 *	  The rewrite in front of ORCA for PostGIS's indexable functions.
 *
 * PostGIS's spatial predicates -- ST_Intersects, ST_DWithin, ST_Contains and
 * the rest, 21 signatures over geometry and geography -- are C functions
 * with a planner support function, postgis_index_supportfn.  Asked by the
 * planner, it turns a call on an indexed column into the bounding-box
 * condition the index can answer: ST_Intersects(geom, g) gives geom && g,
 * ST_DWithin(geom, g, r) gives geom && st_expand(g, r).  The condition is
 * lossy, so the call stays and is checked as well.  Without it, a spatial
 * predicate is a filter on every row, and a spatial join is a nested loop
 * over both tables.
 *
 * ORCA has no support-function logic at all, and Cloudberry's translator
 * refuses every extension function that has one (LookupFuncProps), so every
 * spatial query went to the planner.  Decision 1 keeps them on ORCA: before
 * ORCA is called, the conditions the support function would give the
 * planner are put into the query beside the call, and ORCA -- which plans a
 * GiST or BRIN scan for && as it does for any operator of an index's family
 * -- is let accept the rest.
 *
 * THE SUPPORT FUNCTION IS ASKED, NOT IMITATED.  Which strategy each function
 * wants, which argument is expanded by the radius, that a 2-D function may
 * not use a 3-D operator family, that ST_DFullyWithin is directional: all of
 * that is PostGIS's.  It changes between PostGIS releases -- 3.7 changed the
 * last of those -- and PostGIS is GPL-2.0-or-later, so it could not be
 * copied into this file even if copying it were wise.  What is here is the
 * planner's half of the exchange: find the calls and the index columns the
 * planner would ask about, and put to the support function the request
 * get_index_clause_from_support() puts (indxpath.c).  What it answers is
 * ANDed in front of the call.
 *
 * Only where the planner would ask:
 *
 *	 * A call that is a conjunct of a WHERE or JOIN ... ON qual, or of an arm
 *	   of an OR that is one: the restriction and join clauses the planner
 *	   matches to indexes, the second for a BitmapOr.  A call anywhere else is
 *	   left as it is.  Nothing could use an index there, and the condition
 *	   would only be a filter, whose answer is not quite the call's (below).
 *
 *	 * An argument that is a column with an index ORCA is told about: plain,
 *	   whole and valid, in an access method it plans (IsIndexSupported).
 *	   Nothing is asked about SP-GiST, expression or partial indexes, which
 *	   ORCA never sees.  The column may be reached through a view, a
 *	   subquery, a CTE or a join's alias: the planner pulls those up before
 *	   it asks, and ORCA pushes a condition down to the scan under them, so
 *	   the question is whether there is an index underneath.
 *
 *	 * An argument of the query the call is in.  One from a query above --
 *	   the outer side of a correlated subquery -- is not traced, so its
 *	   index is not asked about; the other argument's is.
 *
 *	 * Not through a security-barrier view, and not into a table with
 *	   row-level security.  The planner makes no index condition there out
 *	   of a function that is not leakproof (restriction_is_securely_
 *	   promotable), and none of PostGIS's is.
 *
 * WHAT THE ANSWER IS.  The conditions are PostGIS's lossy index conditions,
 * which PostGIS relies on being implied by the call: a row the call accepts
 * satisfies them.  So a query rewritten this way returns what PostGIS's
 * index scan returns -- whether ORCA then probes the index with the
 * condition or scans and filters with it.  The planner's sequential scan
 * applies only the call, and can differ from its own index scan where that
 * guarantee does not hold; PostGIS names one such case, non-finite
 * coordinates under its equality strategy (gserialized_supportfn.c).  The
 * rewrite has the index scan's answer in both.
 *
 * ORCA accepts the call itself because the translator no longer refuses
 * PostGIS's support function (CTranslatorRelcacheToDXL::LookupFuncProps).
 * gp.optimizer_postgis_rewrite off puts the refusal back, per query, in
 * orca.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/transam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "gp_orca_postgis.h"

bool		gp_optimizer_postgis_rewrite = true;

/*
 * How deep a column is followed through views, subqueries, CTEs and joins
 * before the rewrite stops looking.  None of those can refer to itself, so
 * this bounds work rather than preventing a loop.
 */
#define TRACE_DEPTH_LIMIT	32

/*
 * An index column a traced argument is, with what an IndexOptInfo needs to
 * describe the index to the support function.
 */
typedef struct IndexColumn
{
	Index		varno;			/* the argument's range table entry */
	Oid			indexoid;
	Oid			relam;
	int			ncolumns;
	int			nkeycolumns;
	int			indexcol;		/* the column the argument is, 0-based */
	int		   *indexkeys;
	Oid		   *opfamily;
	Oid		   *opcintype;
	Oid		   *indexcollations;
} IndexColumn;

typedef struct RewriteContext
{
	List	   *levels;			/* the queries being rewritten, innermost first */
	PlannerInfo *root;			/* a stand-in for the planner's; see ask() */
} RewriteContext;

static void rewrite_query(Query *query, RewriteContext *cxt);
static void trace_column(Var *var, List *levels, int depth, Index varno,
						 List **columns);
static void trace_setop(Query *query, Node *setop, AttrNumber attno,
						List *levels, int depth, Index varno, List **columns);

/*
 * Is `supportfn` PostGIS's postgis_index_supportfn?
 *
 * By what it runs rather than by what it is called: a C function whose
 * symbol is postgis_index_supportfn, in a library of PostGIS's.  Anything
 * that runs that code is PostGIS's support function whatever it is named,
 * and a function of that name that runs anything else is not.
 */
bool
GpOrcaIsPostgisIndexSupport(Oid supportfn)
{
	HeapTuple	tup;
	bool		result = false;

	if (!OidIsValid(supportfn))
		return false;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(supportfn));
	if (!HeapTupleIsValid(tup))
		return false;

	if (((Form_pg_proc) GETSTRUCT(tup))->prolang == ClanguageId)
	{
		Datum		prosrc;
		Datum		probin;
		bool		srcnull;
		bool		binnull;

		prosrc = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosrc, &srcnull);
		probin = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &binnull);
		if (!srcnull && !binnull)
		{
			char	   *symbol = TextDatumGetCString(prosrc);
			char	   *library = TextDatumGetCString(probin);
			char	   *base = strrchr(library, '/');

			/* "$libdir/postgis-3", however PostGIS was installed */
			base = base ? base + 1 : library;
			result = strcmp(symbol, "postgis_index_supportfn") == 0 &&
				strncmp(base, "postgis-", strlen("postgis-")) == 0;
		}
	}
	ReleaseSysCache(tup);

	return result;
}

static bool
calls_postgis_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, calls_postgis_walker,
								 context, 0);

	if (IsA(node, FuncExpr) &&
		GpOrcaIsPostgisIndexSupport(get_func_support(((FuncExpr *) node)->funcid)))
		return true;

	if (IsA(node, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) node;

		set_opfuncid(opexpr);
		if (GpOrcaIsPostgisIndexSupport(get_func_support(opexpr->opfuncid)))
			return true;
	}

	return expression_tree_walker(node, calls_postgis_walker, context);
}

/*
 * Does anything in `query` call a function whose support function is
 * PostGIS's?  FuncExpr and OpExpr, as orca.c's check for a query with no
 * range table looks at both; PostGIS's operators have no support function,
 * so in practice it is the first.
 */
bool
GpOrcaQueryCallsPostgisIndexable(Query *query)
{
	return calls_postgis_walker((Node *) query, NULL);
}

/*
 * Would ORCA be told about this index?  The translator's IsIndexSupported
 * (CTranslatorRelcacheToDXL.cpp), in C: no expressions, no predicate, valid,
 * and an access method ORCA plans -- less Cloudberry's bitmap index, which
 * no module of the port provides yet.
 *
 * And not an index the planner would skip because this transaction's
 * snapshots may not read through it yet (get_relation_info, indcheckxmin).
 * ORCA's metadata admits one, but a plan that uses it is refused at
 * translation, so a condition that made ORCA choose it would only turn a
 * plan into a fallback.
 */
static bool
index_is_orcas(Relation index)
{
	Form_pg_index form = index->rd_index;
	Oid			am = index->rd_rel->relam;

	if (!form->indisvalid ||
		!heap_attisnull(index->rd_indextuple, Anum_pg_index_indexprs, NULL) ||
		!heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL))
		return false;

	if (am != BTREE_AM_OID && am != HASH_AM_OID && am != GIST_AM_OID &&
		am != GIN_AM_OID && am != BRIN_AM_OID)
		return false;

	if (form->indcheckxmin &&
		!TransactionIdPrecedes(HeapTupleHeaderGetXmin(index->rd_indextuple->t_data),
							   TransactionXmin))
		return false;

	return true;
}

/*
 * The index columns ORCA is told about that are column `attno` of relation
 * `relid`.  The relation is locked already: the parser or the rewriter locked
 * every relation the query reads, views' included.
 */
static void
column_indexes(Oid relid, AttrNumber attno, Index varno, List **columns)
{
	Relation	rel = relation_open(relid, NoLock);
	List	   *indexoids = RelationGetIndexList(rel);
	ListCell   *lc;

	foreach(lc, indexoids)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	index = index_open(indexoid, AccessShareLock);

		if (index_is_orcas(index))
		{
			Form_pg_index form = index->rd_index;
			int			ncolumns = form->indnatts;
			int			nkeycolumns = form->indnkeyatts;

			for (int col = 0; col < nkeycolumns; col++)
			{
				IndexColumn *ic;
				ListCell   *lc2;
				bool		seen = false;

				if (form->indkey.values[col] != attno)
					continue;

				/* the same table twice under one UNION ALL */
				foreach(lc2, *columns)
				{
					IndexColumn *other = (IndexColumn *) lfirst(lc2);

					if (other->indexoid == indexoid && other->indexcol == col)
						seen = true;
				}
				if (seen)
					continue;

				ic = palloc0_object(IndexColumn);
				ic->varno = varno;
				ic->indexoid = indexoid;
				ic->relam = index->rd_rel->relam;
				ic->ncolumns = ncolumns;
				ic->nkeycolumns = nkeycolumns;
				ic->indexcol = col;
				ic->indexkeys = palloc_array(int, ncolumns);
				ic->opfamily = palloc_array(Oid, nkeycolumns);
				ic->opcintype = palloc_array(Oid, nkeycolumns);
				ic->indexcollations = palloc_array(Oid, nkeycolumns);
				for (int i = 0; i < ncolumns; i++)
					ic->indexkeys[i] = form->indkey.values[i];
				for (int i = 0; i < nkeycolumns; i++)
				{
					ic->opfamily[i] = index->rd_opfamily[i];
					ic->opcintype[i] = index->rd_opcintype[i];
					ic->indexcollations[i] = index->rd_indcollation[i];
				}
				*columns = lappend(*columns, ic);
			}
		}
		index_close(index, NoLock);
	}
	list_free(indexoids);
	relation_close(rel, NoLock);
}

static Node *
strip_relabel(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

/*
 * What output column `attno` of `query` reads, followed down.  Under a set
 * operation, every branch: ORCA pushes a condition into each, and one with
 * an index can use it.
 */
static void
trace_output(Query *query, AttrNumber attno, List *levels, int depth,
			 Index varno, List **columns)
{
	TargetEntry *tle;
	Node	   *expr;

	if (query->setOperations != NULL)
	{
		trace_setop(query, query->setOperations, attno, levels, depth, varno,
					columns);
		return;
	}

	tle = get_tle_by_resno(query->targetList, attno);
	if (tle == NULL || tle->resjunk)
		return;

	expr = strip_relabel((Node *) tle->expr);
	if (IsA(expr, Var))
		trace_column((Var *) expr, levels, depth + 1, varno, columns);
}

static void
trace_setop(Query *query, Node *setop, AttrNumber attno, List *levels,
			int depth, Index varno, List **columns)
{
	if (IsA(setop, RangeTblRef))
	{
		RangeTblEntry *rte = rt_fetch(((RangeTblRef *) setop)->rtindex,
									  query->rtable);

		if (rte->rtekind == RTE_SUBQUERY)
			trace_output(rte->subquery, attno, lcons(rte->subquery, levels),
						 depth + 1, varno, columns);
	}
	else if (IsA(setop, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setop;

		trace_setop(query, op->larg, attno, levels, depth, varno, columns);
		trace_setop(query, op->rarg, attno, levels, depth, varno, columns);
	}
}

/*
 * The index columns the column `var` reads is, followed through views,
 * subqueries, CTEs and join aliases to the table under them.  `levels` are
 * the queries around `var`, the one it is in first; `varno` is the range
 * table entry at the call's own level that the trace started from, which is
 * what the support function is told the index is on.
 */
static void
trace_column(Var *var, List *levels, int depth, Index varno, List **columns)
{
	Query	   *query;
	RangeTblEntry *rte;

	if (depth > TRACE_DEPTH_LIMIT || var->varattno <= 0 ||
		var->varlevelsup >= list_length(levels))
		return;

	levels = list_copy_tail(levels, var->varlevelsup);
	query = (Query *) linitial(levels);
	rte = rt_fetch(var->varno, query->rtable);

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			if (rte->securityQuals == NIL)
				column_indexes(rte->relid, var->varattno, varno, columns);
			break;

		case RTE_SUBQUERY:
			if (!rte->security_barrier)
				trace_output(rte->subquery, var->varattno,
							 lcons(rte->subquery, levels), depth + 1, varno,
							 columns);
			break;

		case RTE_JOIN:
			if (var->varattno <= list_length(rte->joinaliasvars))
			{
				Node	   *alias;

				/*
				 * A merged column of a FULL JOIN is a COALESCE, which no
				 * index is; every other alias is a Var of this level.
				 */
				alias = strip_relabel(list_nth(rte->joinaliasvars,
											   var->varattno - 1));
				if (alias && IsA(alias, Var))
					trace_column((Var *) alias, levels, depth + 1, varno,
								 columns);
			}
			break;

		case RTE_CTE:
			if (rte->ctelevelsup < list_length(levels) && !rte->self_reference)
			{
				List	   *owner = list_copy_tail(levels, rte->ctelevelsup);
				ListCell   *lc;

				foreach(lc, ((Query *) linitial(owner))->cteList)
				{
					CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

					if (strcmp(cte->ctename, rte->ctename) != 0)
						continue;
					if (!cte->cterecursive && IsA(cte->ctequery, Query))
						trace_output((Query *) cte->ctequery, var->varattno,
									 lcons(cte->ctequery, owner), depth + 1,
									 varno, columns);
					break;
				}
			}
			break;

		default:
			/* functions, VALUES, tuplestores: nothing is indexed */
			break;
	}
}

/*
 * Ask the support function about one index column, as the planner does
 * (indxpath.c, get_index_clause_from_support).
 *
 * The request carries a PlannerInfo and an IndexOptInfo, and neither exists
 * yet: the query has not been planned, and will not be by the planner.  The
 * stand-ins carry what a support function reads of them.  PostGIS's reads
 * the index's relation, to check that the other side of the call does not
 * refer to it -- is_pseudo_constant_for_index(), whose pull_varnos() uses
 * the PlannerInfo only for PlaceHolderVars, which a query from the parser
 * does not have.  So the IndexOptInfo's relation is the argument's range
 * table entry at the call's level, which is what "the other side refers to
 * the indexed table" means there, and the rest of it is filled from the
 * index as get_relation_info would fill it.
 */
static List *
ask(Oid supportfn, FuncExpr *call, int indexarg, IndexColumn *ic,
	RewriteContext *cxt)
{
	SupportRequestIndexCondition req;
	IndexOptInfo *index = makeNode(IndexOptInfo);
	RelOptInfo *rel = makeNode(RelOptInfo);

	rel->reloptkind = RELOPT_BASEREL;
	rel->relid = ic->varno;
	rel->relids = bms_make_singleton(ic->varno);

	index->indexoid = ic->indexoid;
	index->rel = rel;
	index->relam = ic->relam;
	index->ncolumns = ic->ncolumns;
	index->nkeycolumns = ic->nkeycolumns;
	index->indexkeys = ic->indexkeys;
	index->opfamily = ic->opfamily;
	index->opcintype = ic->opcintype;
	index->indexcollations = ic->indexcollations;

	cxt->root->parse = (Query *) linitial(cxt->levels);

	req.type = T_SupportRequestIndexCondition;
	req.root = cxt->root;
	req.funcid = call->funcid;
	req.node = (Node *) call;
	req.indexarg = indexarg;
	req.index = index;
	req.indexcol = ic->indexcol;
	req.opfamily = ic->opfamily[ic->indexcol];
	req.indexcollation = ic->indexcollations[ic->indexcol];
	req.lossy = true;

	return (List *) DatumGetPointer(OidFunctionCall1(supportfn,
													 PointerGetDatum(&req)));
}

/* Is `cond` in `clauses` already, as written or with its sides swapped? */
static bool
has_condition(List *clauses, Node *cond)
{
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (equal(clause, cond))
			return true;

		if (IsA(clause, OpExpr) && IsA(cond, OpExpr))
		{
			OpExpr	   *a = (OpExpr *) clause;
			OpExpr	   *b = (OpExpr *) cond;

			if (list_length(a->args) == 2 && list_length(b->args) == 2 &&
				OidIsValid(b->opno) && b->opno == get_commutator(a->opno) &&
				equal(linitial(a->args), lsecond(b->args)) &&
				equal(lsecond(a->args), linitial(b->args)))
				return true;
		}
	}
	return false;
}

/*
 * The index conditions PostGIS's support function gives for `call`.
 *
 * For each index column an argument is, the first argument that is it, as
 * match_funcclause_to_indexcol() asks: ST_Intersects(t.geom, t.geom) is
 * asked about t's index once, with the first argument.  The same table
 * under two range table entries -- a self-join -- is two indexes to ask
 * about, as it is two scans to the planner.
 */
static List *
index_conditions(FuncExpr *call, RewriteContext *cxt)
{
	Oid			supportfn;
	List	   *asked = NIL;
	List	   *conds = NIL;
	int			indexarg = 0;
	ListCell   *lc;

	if (call->funcresulttype != BOOLOID)
		return NIL;

	supportfn = get_func_support(call->funcid);
	if (!GpOrcaIsPostgisIndexSupport(supportfn))
		return NIL;

	foreach(lc, call->args)
	{
		Node	   *arg = strip_relabel((Node *) lfirst(lc));
		List	   *columns = NIL;
		ListCell   *lc2;

		if (arg && IsA(arg, Var) && ((Var *) arg)->varlevelsup == 0)
			trace_column((Var *) arg, cxt->levels, 0, ((Var *) arg)->varno,
						 &columns);

		foreach(lc2, columns)
		{
			IndexColumn *ic = (IndexColumn *) lfirst(lc2);
			ListCell   *lc3;
			bool		seen = false;

			foreach(lc3, asked)
			{
				IndexColumn *other = (IndexColumn *) lfirst(lc3);

				if (other->varno == ic->varno &&
					other->indexoid == ic->indexoid &&
					other->indexcol == ic->indexcol)
					seen = true;
			}
			if (seen)
				continue;
			asked = lappend(asked, ic);

			foreach(lc3, ask(supportfn, call, indexarg, ic, cxt))
			{
				Node	   *cond = (Node *) lfirst(lc3);

				if (IsA(cond, OpExpr))
					set_opfuncid((OpExpr *) cond);
				if (!has_condition(conds, cond))
					conds = lappend(conds, cond);
			}
		}
		indexarg++;
	}

	return conds;
}

static Node *rewrite_qual(Node *qual, RewriteContext *cxt);

/*
 * A conjunction -- the clauses of an AND, or a clause standing alone.  Each
 * call in it gains its conditions in front of it, less any the conjunction
 * has already: written by hand, as PostGIS 2 taught people to, or given for
 * a call before it.  A condition counted twice would halve ORCA's estimate
 * of the rows for nothing.
 */
static List *
rewrite_conjuncts(List *clauses, RewriteContext *cxt)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (is_orclause(clause))
		{
			ListCell   *arm;

			foreach(arm, ((BoolExpr *) clause)->args)
				lfirst(arm) = rewrite_qual((Node *) lfirst(arm), cxt);
		}
		else if (IsA(clause, FuncExpr))
		{
			ListCell   *lc2;

			foreach(lc2, index_conditions((FuncExpr *) clause, cxt))
			{
				Node	   *cond = (Node *) lfirst(lc2);

				if (!has_condition(clauses, cond) &&
					!has_condition(result, cond))
					result = lappend(result, cond);
			}
		}
		result = lappend(result, clause);
	}

	return result;
}

static Node *
rewrite_qual(Node *qual, RewriteContext *cxt)
{
	List	   *clauses;

	if (qual == NULL)
		return NULL;

	/* eval_const_expressions() has flattened an AND of ANDs already */
	if (is_andclause(qual))
	{
		((BoolExpr *) qual)->args =
			rewrite_conjuncts(((BoolExpr *) qual)->args, cxt);
		return qual;
	}

	clauses = rewrite_conjuncts(list_make1(qual), cxt);
	if (list_length(clauses) == 1)
		return (Node *) linitial(clauses);
	return (Node *) make_andclause(clauses);
}

/* The quals of a FROM list and of the joins in it, at one level. */
static void
rewrite_jointree(Node *jtnode, RewriteContext *cxt)
{
	if (jtnode == NULL)
		return;

	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *from = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, from->fromlist)
			rewrite_jointree((Node *) lfirst(lc), cxt);
		from->quals = rewrite_qual(from->quals, cxt);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;

		rewrite_jointree(join->larg, cxt);
		rewrite_jointree(join->rarg, cxt);
		join->quals = rewrite_qual(join->quals, cxt);
	}
}

static bool
rewrite_sublevels_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		rewrite_query((Query *) node, (RewriteContext *) context);
		return false;
	}

	return expression_tree_walker(node, rewrite_sublevels_walker, context);
}

/*
 * One query: the levels below it first -- subqueries in FROM, CTEs and the
 * subqueries of SubLinks, wherever they are -- and then its own quals.
 */
static void
rewrite_query(Query *query, RewriteContext *cxt)
{
	cxt->levels = lcons(query, cxt->levels);

	(void) query_tree_walker(query, rewrite_sublevels_walker, cxt, 0);
	rewrite_jointree((Node *) query->jointree, cxt);

	cxt->levels = list_delete_first(cxt->levels);
}

void
GpOrcaPostgisRewrite(Query *query)
{
	RewriteContext cxt;

	cxt.levels = NIL;
	cxt.root = makeNode(PlannerInfo);
	cxt.root->glob = makeNode(PlannerGlobal);
	cxt.root->query_level = 1;
	cxt.root->planner_cxt = CurrentMemoryContext;

	rewrite_query(query, &cxt);
}
