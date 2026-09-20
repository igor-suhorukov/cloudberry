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
 * compat/clauses.c
 *	  What Cloudberry adds to PostgreSQL's clauses.c for ORCA.
 *
 * Ported from github/cloudberry/src/backend/optimizer/util/clauses.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "cb_clauses.h"

/*
 * flatten_join_alias_var_optimizer
 *		Replace Vars that reference a JOIN's output with references to the
 *		columns underneath, everywhere in a Query that ORCA's normalization
 *		may later move.
 *
 * WHY ORCA NEEDS ITS OWN.  PostgreSQL flattens join alias Vars inside quals,
 * so that a predicate over a join output can be pushed down; it does not
 * flatten the target list, because the planner keeps the range table beside
 * the plan and can resolve an alias Var whenever it meets one.  ORCA cannot:
 * CQueryMutators rewrites a Query into a derived table before translating
 * it, and an expression that moves out of the query that owns the JOIN has
 * nothing left to resolve against.  So the expressions that can move are
 * flattened first, and the quals -- which do not move -- are left alone and
 * resolved during translation through ORCA's <query level, varno, varattno>
 * mapping.  That reasoning is Cloudberry's, and it still holds on
 * PostgreSQL 19.
 *
 * WHERE IT ACTUALLY DOES ANYTHING, measured rather than assumed.  A USING
 * column only becomes a join alias Var that needs flattening when the join is
 * a FULL OUTER one.  For an inner or one-sided outer join the merged column
 * is exactly one of the inputs, and the parser resolves the reference to that
 * input as it analyzes it -- so "SELECT x FROM a JOIN b USING (x)" arrives
 * here already naming a base relation and nothing happens.  Under FULL JOIN
 * the merged value is COALESCE(a.x, b.x), which is not a Var, so the
 * reference stays pointed at the join and one Var becomes two.  A whole-row
 * reference to any join is the other case: it expands into a RowExpr naming
 * the inputs.
 *
 * WHAT CHANGED FOR POSTGRESQL 19.  Two things, and neither is in the shape
 * of the function.
 *
 * Cloudberry flattens query->scatterClause, which PostgreSQL 19 has no field
 * for: SCATTER BY is a Cloudberry-only clause on a table function's argument
 * and Track F records it as lost, along with the anytable type and the
 * describe callbacks it goes with.  There is nothing to flatten.
 *
 * Cloudberry frees each list or node it replaced.  The port does not, and
 * the difference is not observable: the tree being edited is a copy this
 * function just made, so it belongs to the caller's memory context and goes
 * when that does.  Freeing bought back one list header per clause, and for
 * havingQual and limitOffset it pfree'd only the top node of a tree whose
 * children it left behind -- so it was never reclaiming the tree, only its
 * root.  Taking it out removes a pointer that has to stay unreachable.
 *
 * queryLevel is unused, here as in Cloudberry.  It stays in the signature
 * because gpdb::FlattenJoinAliasVar passes ORCA's query level through and
 * the port does not edit ORCA's core.
 */
Query *
flatten_join_alias_var_optimizer(Query *query, int queryLevel)
{
	Query	   *queryNew = (Query *) copyObject(query);
	ListCell   *lc;

	(void) queryLevel;

	if (queryNew->targetList != NIL)
		queryNew->targetList = (List *)
			flatten_join_alias_vars(NULL, queryNew,
									(Node *) queryNew->targetList);

	if (queryNew->returningList != NIL)
		queryNew->returningList = (List *)
			flatten_join_alias_vars(NULL, queryNew,
									(Node *) queryNew->returningList);

	if (queryNew->havingQual != NULL)
		queryNew->havingQual =
			flatten_join_alias_vars(NULL, queryNew, queryNew->havingQual);

	if (queryNew->limitOffset != NULL)
		queryNew->limitOffset =
			flatten_join_alias_vars(NULL, queryNew, queryNew->limitOffset);

	if (queryNew->limitCount != NULL)
		queryNew->limitCount =
			flatten_join_alias_vars(NULL, queryNew, queryNew->limitCount);

	/*
	 * A window frame's bounds are expressions, and a WindowClause is not, so
	 * Cloudberry walks them by hand rather than handing them to the mutator.
	 *
	 * It cannot fire.  PostgreSQL requires a frame offset to be free of
	 * variables at the query's own level -- transformFrameOffset calls
	 * checkExprIsVarFree, and "ROWS BETWEEN x PRECEDING" is rejected with
	 * "argument of ROWS must not contain variables" -- and a Var at a deeper
	 * level is not this call's to substitute.  So there is never a join alias
	 * Var here to flatten, on PostgreSQL 19 or on Cloudberry's PostgreSQL 16.
	 *
	 * The loop stays because it is Cloudberry's and costs nothing, and
	 * because the reason it is unreachable is a rule of the parser's that
	 * could be relaxed.  The test beside it pins that rule rather than the
	 * loop, so if it ever changes the loop is what gets looked at.
	 */
	foreach(lc, queryNew->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		if (wc == NULL)
			continue;

		if (wc->startOffset)
			wc->startOffset =
				flatten_join_alias_vars(NULL, queryNew, wc->startOffset);

		if (wc->endOffset)
			wc->endOffset =
				flatten_join_alias_vars(NULL, queryNew, wc->endOffset);
	}

	return queryNew;
}

/*
 * transform_array_Const_to_ArrayExpr
 *		An array constant rewritten as the ArrayExpr that builds it, so that
 *		ORCA can see the elements.
 *
 * ORCA derives constraints from an IN list by putting the ArrayExpr into
 * disjunctive normal form and reading the elements out.  It has no way to
 * look inside an array Const -- the bytes are a datum of a type it was never
 * compiled against -- so the elements are handed to it as separate Consts
 * instead.  Anything that is not an array constant comes back unchanged, so
 * the caller can apply this blindly.
 *
 * WHAT CHANGED FOR POSTGRESQL 19, and it is a real difference rather than a
 * rename.  ArrayExpr has gained array_collid since PostgreSQL 16, and
 * Cloudberry's version does not set it -- makeNode zeroes it, so the
 * rewritten expression comes out with no collation where the Const it
 * replaced had one.  exprCollation() reads array_collid for an ArrayExpr and
 * constcollid for a Const, so on Cloudberry those two disagree for any
 * collatable element type, and a text[] constant loses its collation on the
 * way into the optimizer.  The port carries the Const's collation across,
 * which is what "the same value, written differently" has to mean.
 *
 * list_start and list_end are new in PostgreSQL 19 too, but they are only
 * error positions, and there was no literal element list in the source text
 * to point at.  They are set to -1, "unknown", rather than left at the 0
 * that makeNode would give them, which would point at the first character of
 * the statement.
 */
Expr *
transform_array_Const_to_ArrayExpr(Const *c)
{
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	int			nelems;
	Datum	   *elems;
	bool	   *nulls;
	ArrayType  *ac;
	ArrayExpr  *aexpr;

	Assert(IsA(c, Const));

	if (c->constisnull)
		return (Expr *) c;		/* a NULL of array type has no elements */

	elemtype = get_element_type(c->consttype);
	if (elemtype == InvalidOid)
		return (Expr *) c;		/* not an array */

	ac = DatumGetArrayTypeP(c->constvalue);
	nelems = ArrayGetNItems(ARR_NDIM(ac), ARR_DIMS(ac));

	get_typlenbyvalalign(elemtype, &elemlen, &elembyval, &elemalign);
	deconstruct_array(ac, elemtype, elemlen, elembyval, elemalign,
					  &elems, &nulls, &nelems);

	aexpr = makeNode(ArrayExpr);
	aexpr->array_typeid = c->consttype;
	aexpr->array_collid = c->constcollid;
	aexpr->element_typeid = elemtype;
	aexpr->multidims = false;
	aexpr->list_start = -1;
	aexpr->list_end = -1;
	aexpr->location = c->location;

	for (int i = 0; i < nelems; i++)
		aexpr->elements = lappend(aexpr->elements,
								  makeConst(elemtype,
											-1,
											c->constcollid,
											elemlen,
											elems[i],
											nulls[i],
											elembyval));

	return (Expr *) aexpr;
}
