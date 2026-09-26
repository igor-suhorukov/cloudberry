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
 * compat/notin.c
 *	  NOT IN as PostgreSQL's planner makes it: a hashed SubPlan.
 *
 * ORCA makes NOT IN an anti-join that treats a NULL on either side as NOT IN
 * does -- Cloudberry's JOIN_LASJ_NOTIN, a hash join whose executor knows
 * those NULLs.  PostgreSQL 19 has no such join, and its planner never makes
 * NOT IN one: it filters the outer rows by a hashed SubPlan,
 * NOT (x = ANY (SELECT y ...)), whose hash table of the inner rows also
 * knows whether one of them was NULL (nodeSubplan.c, ExecHashSubPlan()) --
 * NOT IN's semantics, the planner's own.  So the translator makes ORCA's
 * anti-join that: its outer side under a Result that filters it by the
 * SubPlan, and its inner side, as ORCA planned it -- broadcast to each
 * segment, or gathered -- the SubPlan's plan, under a Result that gives the
 * inner expressions of the join's conditions, one column each, where the
 * SubPlan's parameters take them.
 *
 * The SubPlan hashes where the planner's would: a strict operator it can
 * hash by, and inner rows whose hash tables fit in hash_mem (subselect.c,
 * hash_ok_operator() and subplan_is_hashable()).  One that could not hash
 * would scan the inner side again for each outer row, a Motion in it too,
 * which a Motion cannot do; the translator refuses it instead.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "utils/lsyscache.h"

#include "cb_notin.h"

/* Is "node" a Var of "varno" alone -- no other relation's, no parameter? */
static bool
other_than_walker(Node *node, int *varno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varno != *varno;
	if (IsA(node, Param) || IsA(node, SubPlan) || IsA(node, Aggref))
		return true;
	return expression_tree_walker(node, other_than_walker, varno);
}

static bool
only_of(Node *node, int varno)
{
	return !other_than_walker(node, &varno);
}

/* The inner side's Vars, as its Result's child's. */
static Node *
inner_to_outer_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INNER_VAR)
	{
		Var		   *var = (Var *) copyObject(node);

		var->varno = OUTER_VAR;
		return (Node *) var;
	}
	return expression_tree_mutator(node, inner_to_outer_mutator, context);
}

Plan *
gp_orca_not_in_subplan(List *clauses, Plan *inner, List *paramids,
					   Expr **testexpr, bool *hashable)
{
	Result	   *result = makeNode(Result);
	List	   *tests = NIL;
	AttrNumber	resno = 0;
	ListCell   *lc;
	ListCell   *lp;

	if (clauses == NIL || list_length(clauses) != list_length(paramids))
		return NULL;

	*hashable = true;
	forboth(lc, clauses, lp, paramids)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Expr	   *outer;
		Expr	   *innerexpr;
		Param	   *param;
		OpExpr	   *test;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2 || op->opretset)
			return NULL;
		outer = (Expr *) linitial(op->args);
		innerexpr = (Expr *) lsecond(op->args);
		if (!only_of((Node *) outer, OUTER_VAR) ||
			!only_of((Node *) innerexpr, INNER_VAR))
			return NULL;

		result->plan.targetlist =
			lappend(result->plan.targetlist,
					makeTargetEntry((Expr *) inner_to_outer_mutator((Node *) innerexpr,
																	NULL),
									++resno, NULL, false));

		param = makeNode(Param);
		param->paramkind = PARAM_EXEC;
		param->paramid = lfirst_int(lp);
		param->paramtype = exprType((Node *) innerexpr);
		param->paramtypmod = exprTypmod((Node *) innerexpr);
		param->paramcollid = exprCollation((Node *) innerexpr);
		param->location = -1;

		test = (OpExpr *) make_opclause(op->opno, op->opresulttype, false,
										outer, (Expr *) param,
										op->opcollid, op->inputcollid);
		test->opfuncid = op->opfuncid;
		tests = lappend(tests, test);

		if (!op_hashjoinable(op->opno, exprType((Node *) outer)) ||
			!func_strict(get_opcode(op->opno)))
			*hashable = false;
	}

	result->plan.lefttree = inner;
	result->plan.startup_cost = inner->startup_cost;
	result->plan.total_cost = inner->total_cost;
	result->plan.plan_rows = inner->plan_rows;
	result->plan.plan_width = inner->plan_width;

	/*
	 * What the planner's hashed SubPlan may hold (subplan_is_hashable()): its
	 * table of the inner rows, and NOT IN's of the rows with a NULL.
	 */
	if (EstimateSubplanHashTableSpace(inner->plan_rows, inner->plan_width,
									  false) >= get_hash_memory_limit())
		*hashable = false;

	*testexpr = list_length(tests) == 1 ? (Expr *) linitial(tests) :
		make_andclause(tests);
	return (Plan *) result;
}
