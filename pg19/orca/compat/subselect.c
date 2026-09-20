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
 * compat/subselect.c
 *	  What Cloudberry exports from PostgreSQL's subselect.c for ORCA.
 *
 * PostgreSQL 19's subselect.c, unchanged apart from the word "static".  The
 * two helpers come with it because they are static there too and nothing
 * else needs them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cb_subselect.h"

static bool test_opexpr_is_hashable(OpExpr *testexpr, List *param_ids);
static bool hash_ok_operator(OpExpr *expr);

/*
 * testexpr_is_hashable: is an ANY SubLink's test expression hashable?
 *
 * To identify LHS vs RHS of the hash expression, we must be given the
 * list of output Param IDs of the SubLink's subquery.
 */
bool
testexpr_is_hashable(Node *testexpr, List *param_ids)
{
	/*
	 * The testexpr must be a single OpExpr, or an AND-clause containing only
	 * OpExprs, each of which satisfy test_opexpr_is_hashable().
	 */
	if (testexpr && IsA(testexpr, OpExpr))
	{
		if (test_opexpr_is_hashable((OpExpr *) testexpr, param_ids))
			return true;
	}
	else if (is_andclause(testexpr))
	{
		ListCell   *l;

		foreach(l, ((BoolExpr *) testexpr)->args)
		{
			Node	   *andarg = (Node *) lfirst(l);

			if (!IsA(andarg, OpExpr))
				return false;
			if (!test_opexpr_is_hashable((OpExpr *) andarg, param_ids))
				return false;
		}
		return true;
	}

	return false;
}

static bool
test_opexpr_is_hashable(OpExpr *testexpr, List *param_ids)
{
	/*
	 * The combining operator must be hashable and strict.  The need for
	 * hashability is obvious, since we want to use hashing.  Without
	 * strictness, behavior in the presence of nulls is too unpredictable.  We
	 * actually must assume even more than plain strictness: it can't yield
	 * NULL for non-null inputs, either (see nodeSubplan.c).  However, hash
	 * indexes and hash joins assume that too.
	 */
	if (!hash_ok_operator(testexpr))
		return false;

	/*
	 * The left and right inputs must belong to the outer and inner queries
	 * respectively; hence Params that will be supplied by the subquery must
	 * not appear in the LHS, and Vars of the outer query must not appear in
	 * the RHS.  (Ordinarily, this must be true because of the way that the
	 * parser builds an ANY SubLink's testexpr ... but inlining of functions
	 * could have changed the expression's structure, so we have to check.
	 * Such cases do not occur often enough to be worth trying to optimize, so
	 * we don't worry about trying to commute the clause or anything like
	 * that; we just need to be sure not to build an invalid plan.)
	 */
	if (list_length(testexpr->args) != 2)
		return false;
	if (contain_exec_param((Node *) linitial(testexpr->args), param_ids))
		return false;
	if (contain_var_clause((Node *) lsecond(testexpr->args)))
		return false;
	return true;
}

/*
 * Check expression is hashable + strict
 *
 * We could use op_hashjoinable() and op_strict(), but do it like this to
 * avoid a redundant cache lookup.
 */
static bool
hash_ok_operator(OpExpr *expr)
{
	Oid			opid = expr->opno;

	/* quick out if not a binary operator */
	if (list_length(expr->args) != 2)
		return false;
	if (opid == ARRAY_EQ_OP ||
		opid == RECORD_EQ_OP ||
		opid == RANGE_EQ_OP ||
		opid == MULTIRANGE_EQ_OP)
	{
		/* these are strict, but must check input type to ensure hashable */
		Node	   *leftarg = linitial(expr->args);

		return op_hashjoinable(opid, exprType(leftarg));
	}
	else
	{
		/* else must look up the operator properties */
		HeapTuple	tup;
		Form_pg_operator optup;

		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", opid);
		optup = (Form_pg_operator) GETSTRUCT(tup);
		if (!optup->oprcanhash || !func_strict(optup->oprcode))
		{
			ReleaseSysCache(tup);
			return false;
		}
		ReleaseSysCache(tup);
		return true;
	}
}
