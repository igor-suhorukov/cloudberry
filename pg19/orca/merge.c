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
 * merge.c
 *	  MERGE, under ORCA.
 *
 * ORCA has no MERGE operator (CLogicalDML), and its core is taken
 * unmodified.  So a MERGE rides a SELECT, as LockRows does: ORCA plans the
 * join the planner makes of the target and the source
 * (transform_MERGE_to_join()), as a SELECT of what ModifyTable reads of
 * each joined row -- the target's ctid, which says whether the row matched,
 * and each column of the source an action or the join condition reads, as
 * the planner's preprocess_targetlist() gives its join -- and a ModifyTable
 * is put over ORCA's plan.  Its actions read the source's columns from the
 * join's row, as set_plan_refs() makes them read them (INNER_VAR), and the
 * target's from the row the ctid fetches, the scan tuple.
 *
 * RETURNING, on one node, as set_returning_clause_references() fixes it:
 * the target's Vars read the row the action wrote, the scan tuple, and the
 * source's the join's row, as OUTER_VAR -- the SELECT returns them as it
 * returns an action's -- and merge_action() the MERGE's own node answers.
 *
 * Row marks, as the planner makes them for a MERGE (preprocess_rowmarks()):
 * ROW_MARK_REFERENCE on each table of the source, its ctid carried up from
 * its scan, so that EvalPlanQual re-checks a target row another transaction
 * updated meanwhile with the source row it was joined to.
 *
 * On a cluster the MERGE is written as the planner's route writes it, by
 * the explicit write (gp_core's gp_explicit.c), from the coordinator: ORCA's
 * join comes up through its Gather with the target's segment and ctid,
 * which gp_core's Row Identity node makes the ctid the explicit write knows
 * the row by, and the target's whole row, carried up from its scan, in the
 * junk column the explicit write reads its actions' old row from.  The
 * explicit write re-checks no row through EvalPlanQual -- a row changed
 * meanwhile is refused where the segment writes it -- so the MERGE takes no
 * row marks, and its source may be anything.
 *
 * WHEN NOT MATCHED BY SOURCE: the planner tells a target row the source
 * has no row for by "src IS NOT NULL" of the source's whole row, which it
 * adds to the join condition above the join (transform_MERGE_to_join()),
 * and ORCA does not take a whole-row Var.  What stands in for the row is
 * what is null exactly where the join found none: a table's ctid, and of a
 * join any of its tables'; a subquery and a VALUES list are given a column
 * of their own that is never null, gp_present.
 *
 * A partitioned target: ModifyTable gets a result relation for each
 * partition, as the planner gives a partitioned table's (inheritance
 * planning) -- the range table entries of ORCA's scans of the partitions,
 * under its Dynamic Scan, so that EvalPlanQual re-checks a row in its
 * partition's scan as it does in the planner's Append -- each action, the
 * join condition, RETURNING and the check options taken to the partition's
 * columns by name, and the row's partition by its tableoid, carried up
 * from the scan that read its ctid; an INSERT is routed through the table.
 * On a cluster the explicit write writes it the same way, through the
 * table, the target's row carried up as its partition's.
 *
 * Not yet, and the planner's: an inherited or foreign table or a view as
 * the target, a replicated one or a coordinator's on a cluster, and on one
 * node one whose method takes the old row from the plan (O20), a
 * partition's too; RETURNING on a cluster, whose merge_action() only a
 * MERGE's own node answers, where the explicit write writes; a subquery
 * anywhere in it; and on one
 * node a source that is not plain tables, whose rows a row mark would copy
 * whole, as a ROW() the translator does not take.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableamext.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/prep.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "cb_compat.h"
#include "cb_dynamicscan.h"
#include "cb_wholerow.h"
#include "gp_core_api.h"
#include "gp_orca_lockrows.h"
#include "gp_orca_merge.h"
#include "gp_policy.h"
#include "gp_scan.h"

struct OrcaMerge
{
	Query	   *merge;			/* the MERGE, its join made */
	bool		cluster;		/* written by the explicit write */
	int			nfirst;			/* the SELECT's columns before the source's:
								 * the ctid, and on a cluster the segment */
	List	   *vars;			/* the source's Vars the SELECT returns */
	List	   *sources;		/* on one node, the source's tables */
	bool		partitioned;	/* the target is a partitioned table */
};

/* Is the relation a plain table -- whose method fetches a row by its ctid? */
static bool
plain_table(RangeTblEntry *rte, bool fetchable)
{
	Relation	rel;
	bool		from_plan;

	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION ||
		has_subclass(rte->relid))
		return false;
	if (!fetchable)
		return true;
	rel = table_open(rte->relid, NoLock);
	from_plan = table_old_row_from_plan(rel);
	table_close(rel, NoLock);
	return !from_plan;
}

static bool
fetchable_table(RangeTblEntry *rte)
{
	return plain_table(rte, true);
}

/*
 * Is the relation a partitioned table whose partitions are all plain
 * tables -- and, "fetchable", each one's method fetches a row by its ctid?
 * Its partitions are locked as the table is, as the planner's inheritance
 * planning locks them (expand_inherited_rtentry()).
 */
static bool
partitioned_table(RangeTblEntry *rte, bool fetchable)
{
	List	   *tree;
	bool		ok = true;

	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_PARTITIONED_TABLE)
		return false;
	tree = find_all_inheritors(rte->relid, rte->rellockmode, NULL);
	foreach_oid(relid, tree)
	{
		char		relkind = get_rel_relkind(relid);
		Relation	rel;

		if (relkind == RELKIND_PARTITIONED_TABLE)
			continue;
		if (relkind != RELKIND_RELATION)
		{
			ok = false;
			break;
		}
		if (!fetchable)
			continue;
		rel = table_open(relid, NoLock);
		ok = !table_old_row_from_plan(rel);
		table_close(rel, NoLock);
		if (!ok)
			break;
	}
	list_free(tree);
	return ok;
}

/* The source's tables into *sources; false where it reads anything else. */
static bool
source_tables(Node *node, Query *query, List **sources)
{
	if (IsA(node, RangeTblRef))
	{
		Index		rti = ((RangeTblRef *) node)->rtindex;

		if (!fetchable_table(rt_fetch(rti, query->rtable)))
			return false;
		*sources = lappend_int(*sources, rti);
		return true;
	}
	if (IsA(node, JoinExpr))
		return source_tables(((JoinExpr *) node)->larg, query, sources) &&
			source_tables(((JoinExpr *) node)->rarg, query, sources);
	return false;
}

/* The Vars of the source "node" reads, added to *vars once each. */
static void
add_source_vars(Node *node, Index target, List **vars)
{
	List	   *found = pull_var_clause(node, 0);

	foreach_node(Var, var, found)
	{
		if (var->varno == target || var->varlevelsup != 0)
			continue;
		if (!list_member(*vars, var))
			*vars = lappend(*vars, copyObject(var));
	}
}

/*
 * What is null in the join's row exactly where the join found no row of
 * the source "jtnode": a table's ctid; of a join, any of its sides'; a
 * subquery's or a VALUES list's column gp_present, added to it here, true
 * in every row.  NULL where there is nothing to stand in -- a function, a
 * CTE, a subquery of a set operation or DISTINCT.  "nulling" is the
 * whole-row Var's varnullingrels, as the Vars above the join have them.
 */
static Expr *
source_present(Node *jtnode, Query *query, Bitmapset *nulling)
{
	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;
		Expr	   *l = source_present(join->larg, query, nulling);
		Expr	   *r = l != NULL ? source_present(join->rarg, query, nulling) : NULL;

		return r != NULL ? make_orclause(list_make2(l, r)) : NULL;
	}
	if (IsA(jtnode, RangeTblRef))
	{
		Index		rti = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, query->rtable);
		Var		   *var = NULL;
		NullTest   *ntest;

		if (rte->rtekind == RTE_RELATION &&
			(rte->relkind == RELKIND_RELATION ||
			 rte->relkind == RELKIND_PARTITIONED_TABLE ||
			 rte->relkind == RELKIND_MATVIEW))
			var = makeVar(rti, SelfItemPointerAttributeNumber, TIDOID, -1,
						  InvalidOid, 0);
		else if (rte->rtekind == RTE_SUBQUERY &&
				 rte->subquery->setOperations == NULL &&
				 rte->subquery->distinctClause == NIL)
		{
			Query	   *sub = rte->subquery;
			AttrNumber	attno = list_length(sub->targetList) + 1;

			sub->targetList = lappend(sub->targetList,
									  makeTargetEntry((Expr *) makeBoolConst(true, false),
													  attno, pstrdup("gp_present"), false));
			rte->eref->colnames = lappend(rte->eref->colnames,
										  makeString(pstrdup("gp_present")));
			var = makeVar(rti, attno, BOOLOID, -1, InvalidOid, 0);
		}
		else if (rte->rtekind == RTE_VALUES)
		{
			AttrNumber	attno = list_length(rte->coltypes) + 1;
			List	   *rows = NIL;

			foreach_ptr(List, row, rte->values_lists)
				rows = lappend(rows, lappend(list_copy(row),
											 makeBoolConst(true, false)));
			rte->values_lists = rows;
			rte->coltypes = lappend_oid(rte->coltypes, BOOLOID);
			rte->coltypmods = lappend_int(rte->coltypmods, -1);
			rte->colcollations = lappend_oid(rte->colcollations, InvalidOid);
			rte->eref->colnames = lappend(rte->eref->colnames,
										  makeString(pstrdup("gp_present")));
			var = makeVar(rti, attno, BOOLOID, -1, InvalidOid, 0);
		}
		if (var == NULL)
			return NULL;
		var->varnullingrels = bms_copy(nulling);
		ntest = makeNode(NullTest);
		ntest->arg = (Expr *) var;
		ntest->nulltesttype = IS_NOT_NULL;
		ntest->argisrow = false;
		ntest->location = -1;
		return (Expr *) ntest;
	}
	return NULL;
}

typedef struct source_row_context
{
	Index		source;			/* the source's range table index */
	Node	   *jtnode;			/* and the source */
	Query	   *query;
	bool		failed;			/* nothing stands in for its row */
} source_row_context;

/* "src IS NOT NULL" of the source's whole row, made source_present()'s */
static Node *
source_row_mutator(Node *node, source_row_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, NullTest) && IsA(((NullTest *) node)->arg, Var) &&
		((NullTest *) node)->nulltesttype == IS_NOT_NULL)
	{
		Var		   *var = (Var *) ((NullTest *) node)->arg;

		if (var->varno == context->source && var->varattno == InvalidAttrNumber &&
			var->varlevelsup == 0)
		{
			Expr	   *present = source_present(context->jtnode, context->query,
												 var->varnullingrels);

			if (present == NULL)
			{
				context->failed = true;
				return node;
			}
			return (Node *) present;
		}
	}
	return expression_tree_mutator(node, source_row_mutator, context);
}

bool
GpOrcaPrepareMerge(Query *query, Query **select, OrcaMerge **statep,
				   const char **why)
{
	Query	   *merge;
	Query	   *sel;
	RangeTblEntry *target;
	OrcaMerge  *state;
	JoinExpr   *join;
	List	   *vars = NIL;
	List	   *sources = NIL;
	AttrNumber	resno = 1;
	bool		cluster = !IS_SINGLENODE();
	const GpCoreApi *api = cb_core_api();

	*select = NULL;
	*statep = NULL;
	*why = NULL;
	if (query->commandType != CMD_MERGE)
		return true;

	if (cluster &&
		(api == NULL || api->version_major != GP_CORE_API_VERSION_MAJOR ||
		 api->version_minor < 11 || !OidIsValid(api->segment_of_function())))
	{
		*why = "a MERGE on a cluster, with a gp_core before 1.11";
		return false;
	}
	if (query->returningList != NIL && cluster)
	{
		*why = "a MERGE's RETURNING on a cluster";
		return false;
	}
	if (query->hasSubLinks)
	{
		*why = "a MERGE with a subquery";
		return false;
	}
	target = rt_fetch(query->resultRelation, query->rtable);
	if (query->mergeTargetRelation != query->resultRelation ||
		!(plain_table(target, !cluster) || partitioned_table(target, !cluster)))
	{
		*why = "a MERGE into a view, or a table that is not a plain one fetched by its ctid";
		return false;
	}
	if (cluster)
	{
		GpPolicy   *policy = GpPolicyGet(target->relid);

		if (policy == NULL ||
			!(GpPolicyIsHashPartitioned(policy) || GpPolicyIsRandomPartitioned(policy)))
		{
			*why = "a MERGE into a replicated table, or a coordinator's on a cluster";
			return false;
		}
	}
	merge = copyObject(query);
	transform_MERGE_to_join(merge);
	join = linitial_node(JoinExpr, merge->jointree->fromlist);

	/* WHEN NOT MATCHED BY SOURCE's test that the source has a row */
	if (merge->mergeJoinCondition != NULL)
	{
		source_row_context context = {0};

		context.jtnode = join->rarg;
		context.source = IsA(join->rarg, RangeTblRef)
			? ((RangeTblRef *) join->rarg)->rtindex
			: IsA(join->rarg, JoinExpr) ? ((JoinExpr *) join->rarg)->rtindex : 0;
		context.query = merge;
		merge->mergeJoinCondition = source_row_mutator(merge->mergeJoinCondition,
													   &context);
		if (context.failed)
		{
			*why = "a MERGE's WHEN NOT MATCHED BY SOURCE, from a source nothing stands in for the row of";
			return false;
		}
	}

	/*
	 * The target's side of the join is a FROM list of it alone, for a view's
	 * conditions; a table has none, and ORCA's translator takes a join of
	 * tables and joins.
	 */
	if (IsA(join->larg, FromExpr) && ((FromExpr *) join->larg)->quals == NULL &&
		list_length(((FromExpr *) join->larg)->fromlist) == 1)
		join->larg = linitial(((FromExpr *) join->larg)->fromlist);

	/*
	 * An automatically updatable view's conditions, which the rewriter put
	 * on the target's side of the join (rewriteTargetView()): the view's
	 * MERGE is the planner's.  So is a source the translator would not
	 * know how to make the join's columns of.
	 */
	if (!IsA(join->larg, RangeTblRef) ||
		!(IsA(join->rarg, RangeTblRef) || IsA(join->rarg, JoinExpr)))
	{
		*why = "a MERGE into a view";
		return false;
	}

	/*
	 * The join's range table entry has no columns (a MERGE's join is the
	 * planner's, which reads none of them); ORCA's translator makes the
	 * join's output of them, so they are the target's and then the
	 * source's, as the parser gives a join of its own.
	 */
	{
		RangeTblEntry *joinrte = rt_fetch(join->rtindex, merge->rtable);
		Node	   *sides[2] = {join->larg, join->rarg};

		joinrte->joinaliasvars = NIL;
		joinrte->joinleftcols = NIL;
		joinrte->joinrightcols = NIL;
		joinrte->eref->colnames = NIL;
		for (int side = 0; side < 2; side++)
		{
			int			rtindex = IsA(sides[side], RangeTblRef)
				? ((RangeTblRef *) sides[side])->rtindex
				: ((JoinExpr *) sides[side])->rtindex;
			List	   *names;
			List	   *colvars;

			expandRTE(rt_fetch(rtindex, merge->rtable), rtindex, 0,
					  VAR_RETURNING_DEFAULT, -1, false, &names, &colvars);
			joinrte->eref->colnames = list_concat(joinrte->eref->colnames, names);
			joinrte->joinaliasvars = list_concat(joinrte->joinaliasvars, colvars);
			foreach_node(Var, var, colvars)
			{
				if (side == 0)
					joinrte->joinleftcols = lappend_int(joinrte->joinleftcols,
														var->varattno);
				else
					joinrte->joinrightcols = lappend_int(joinrte->joinrightcols,
														 var->varattno);
			}
		}
	}
	if (!cluster && !source_tables(join->rarg, merge, &sources))
	{
		*why = "a MERGE whose source is not tables, whose rows a row mark would copy";
		return false;
	}

	foreach_node(MergeAction, action, merge->mergeActionList)
	{
		add_source_vars(action->qual, merge->resultRelation, &vars);
		add_source_vars((Node *) action->targetList, merge->resultRelation, &vars);
	}
	add_source_vars(merge->mergeJoinCondition, merge->resultRelation, &vars);
	add_source_vars((Node *) merge->returningList, merge->resultRelation, &vars);

	/* the SELECT ORCA plans: the join, and what ModifyTable reads of it */
	sel = copyObject(merge);
	sel->commandType = CMD_SELECT;
	sel->resultRelation = 0;
	sel->mergeActionList = NIL;
	sel->mergeJoinCondition = NULL;
	sel->mergeTargetRelation = 0;
	sel->returningList = NIL;
	sel->withCheckOptions = NIL;
	sel->canSetTag = true;
	sel->targetList =
		list_make1(makeTargetEntry((Expr *) makeVar(merge->resultRelation,
													SelfItemPointerAttributeNumber,
													TIDOID, -1, InvalidOid, 0),
								   resno++, pstrdup("ctid"), false));

	/* on a cluster, the segment the target's row is on: gp_segment_id */
	if (cluster)
	{
		Var		   *row = makeVar(merge->resultRelation, InvalidAttrNumber,
								  get_rel_type_id(target->relid), -1,
								  InvalidOid, 0);

		sel->targetList =
			lappend(sel->targetList,
					makeTargetEntry((Expr *) makeFuncExpr(api->segment_of_function(),
														  INT4OID, list_make1(row),
														  InvalidOid, InvalidOid,
														  COERCE_EXPLICIT_CALL),
									resno++, pstrdup("gp_segment_id"), false));
	}
	foreach_node(Var, var, vars)
		sel->targetList = lappend(sel->targetList,
								  makeTargetEntry((Expr *) copyObject(var),
												  resno++, NULL, false));

	state = palloc0(sizeof(OrcaMerge));
	state->merge = merge;
	state->cluster = cluster;
	state->nfirst = cluster ? 2 : 1;
	state->vars = vars;
	state->sources = sources;
	state->partitioned = target->relkind == RELKIND_PARTITIONED_TABLE;
	*select = sel;
	*statep = state;
	return true;
}

/* ------------------------------------------------------------------------- */
/* The plan                                                                  */
/* ------------------------------------------------------------------------- */

/* ORCA's range table index of the Query's table "qrte", or 0 */
static Index
plan_rti_of(PlannedStmt *stmt, RangeTblEntry *qrte)
{
	Index		rti = 0;
	int			matches = 0;
	int			i = 0;

	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		i++;
		if (rte->rtekind == RTE_RELATION && rte->relid == qrte->relid &&
			rte->eref != NULL &&
			strcmp(rte->eref->aliasname, qrte->eref->aliasname) == 0)
		{
			rti = i;
			matches++;
		}
	}
	return matches == 1 ? rti : 0;
}

typedef struct merge_vars_context
{
	OrcaMerge  *state;
	Index		rti;			/* the target's, in ORCA's range table */
	int			source;			/* the join's row: INNER_VAR or OUTER_VAR */
} merge_vars_context;

/*
 * An action's expression over ORCA's plan: the target's Vars read the scan
 * tuple, as they are, at the target's index in ORCA's range table; the
 * source's read the join's row, as INNER_VAR -- RETURNING's as OUTER_VAR,
 * as ExecProcessReturning() hands it the row -- by their place in it.
 */
static Node *
merge_vars_mutator(Node *node, merge_vars_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varlevelsup == 0)
	{
		Var		   *var = (Var *) copyObject(node);
		int			i;

		if (var->varno == context->state->merge->resultRelation)
		{
			var->varno = context->rti;
			var->varnosyn = context->rti;
			var->varnullingrels = NULL;
			return (Node *) var;
		}
		var->varnullingrels = NULL;
		i = -1;
		foreach_node(Var, v, context->state->vars)
		{
			if (v->varno == var->varno && v->varattno == var->varattno)
			{
				i = foreach_current_index(v);
				break;
			}
		}
		if (i < 0)
			elog(ERROR, "a MERGE's column the plan does not return");
		var->varno = context->source;
		var->varattno = i + 1 + context->state->nfirst;
		return (Node *) var;
	}
	return expression_tree_mutator(node, merge_vars_mutator, context);
}

static Node *
merge_vars(Node *node, OrcaMerge *state, Index rti)
{
	merge_vars_context context = {.state = state,.rti = rti,.source = INNER_VAR};

	return merge_vars_mutator(node, &context);
}

static List *
merge_returning(List *returning, OrcaMerge *state, Index rti)
{
	merge_vars_context context = {.state = state,.rti = rti,.source = OUTER_VAR};

	return (List *) merge_vars_mutator((Node *) returning, &context);
}

/* A WHEN condition or the join condition, as the executor takes it */
static List *
implicit_qual(Node *qual)
{
	if (qual == NULL)
		return NIL;
	return make_ands_implicit(canonicalize_qual((Expr *) qual, false));
}

/*
 * An INSERT action's target list, every column of the table in order, as
 * the planner completes it (preptlist.c, expand_insert_targetlist()): what
 * the action leaves out is NULL.
 */
static List *
insert_targetlist(List *tlist, Relation rel)
{
	List	   *result = NIL;
	ListCell   *item = list_head(tlist);
	int			natts = RelationGetNumberOfAttributes(rel);

	for (int attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attno - 1);
		TargetEntry *tle = NULL;

		if (item != NULL)
		{
			TargetEntry *old = lfirst_node(TargetEntry, item);

			if (!old->resjunk && old->resno == attno)
			{
				tle = old;
				item = lnext(tlist, item);
			}
		}
		if (tle == NULL)
		{
			Node	   *expr;

			if (att->attisdropped)
				expr = (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
										  (Datum) 0, true, true);
			else if (att->attgenerated)
			{
				Oid			type = att->atttypid;
				int32		typmod = att->atttypmod;

				type = getBaseTypeAndTypmod(type, &typmod);
				expr = (Node *) makeConst(type, typmod, att->attcollation,
										  att->attlen, (Datum) 0, true,
										  att->attbyval);
			}
			else
			{
				expr = coerce_null_to_domain(att->atttypid, att->atttypmod,
											 att->attcollation, att->attlen,
											 att->attbyval);
				if (!IsA(expr, Const))
					expr = eval_const_expressions(NULL, expr);
			}
			tle = makeTargetEntry((Expr *) expr, attno,
								  pstrdup(NameStr(att->attname)), false);
		}
		result = lappend(result, tle);
	}
	return result;
}

/* The Dynamic Scan of the partitioned table at "rti" below "plan", or NULL */
static CustomScan *
find_dynamic_scan(Plan *plan, Index rti)
{
	CustomScan *found;

	if (plan == NULL)
		return NULL;
	if (IsA(plan, CustomScan))
	{
		CustomScan *cscan = (CustomScan *) plan;

		if (cscan->methods == &gp_orca_dynamic_scan_methods)
			return bms_is_member(rti, cscan->custom_relids) ? cscan : NULL;
	}
	found = find_dynamic_scan(plan->lefttree, rti);
	return found != NULL ? found : find_dynamic_scan(plan->righttree, rti);
}

/*
 * A list of a partitioned target's -- actions, their conditions, RETURNING,
 * check options -- as the partition at "part_rti" has it: each column of the
 * table by its name in the partition (adjust_appendrel_attrs()).
 */
static List *
partition_exprs(List *exprs, Index rti, Oid root_oid, Index part_rti, Oid part_oid)
{
	if (exprs == NIL)
		return NIL;
	return gp_orca_partition_exprs(exprs, rti, root_oid, part_rti, part_oid);
}

bool
GpOrcaFinishMerge(PlannedStmt *stmt, OrcaMerge *state, const char **why)
{
	Query	   *merge = state->merge;
	RangeTblEntry *qtarget = rt_fetch(merge->resultRelation, merge->rtable);
	Index		rti = plan_rti_of(stmt, qtarget);
	Plan	   *sub = stmt->planTree;
	ModifyTable *mt;
	RangeTblEntry *rte;
	RTEPermissionInfo *perminfo;
	RTEPermissionInfo *qperminfo;
	List	   *actions = NIL;
	List	   *marks = NIL;
	List	   *part_rtis = NIL;
	List	   *wcos = NIL;
	List	   *returning = NIL;
	List	   *joincond;
	Relation	rel;
	int			epq;

	*why = NULL;
	if (rti == 0)
	{
		*why = "a MERGE whose target the plan does not scan once";
		return false;
	}

	/* the join's columns are junk to ModifyTable, the ctid named as it finds it */
	foreach_node(TargetEntry, tle, sub->targetlist)
		tle->resjunk = true;
	linitial_node(TargetEntry, sub->targetlist)->resname = pstrdup("ctid");

	/*
	 * A partitioned target: the partitions ORCA's Dynamic Scan reads it
	 * through, each a result relation, and each row's partition, its
	 * tableoid, carried up from the scan that read its ctid.
	 */
	if (state->partitioned)
	{
		CustomScan *dscan = find_dynamic_scan(sub, rti);
		AttrNumber	resno;
		TargetEntry *tle;

		if (dscan != NULL)
			foreach_ptr(Plan, child, dscan->custom_plans)
				part_rtis = lappend_int(part_rtis, ((Scan *) child)->scanrelid);
		if (part_rtis == NIL)
		{
			*why = "a MERGE into a partitioned table the plan does not scan through its partitions";
			return false;
		}
		resno = gp_orca_carry_tableoid(sub, 1, stmt->rtable);
		if (resno == InvalidAttrNumber)
		{
			*why = "a MERGE into a partitioned table whose rows' partitions the plan does not carry";
			return false;
		}
		tle = list_nth_node(TargetEntry, sub->targetlist, resno - 1);
		tle->resname = pstrdup("tableoid");
		tle->resjunk = true;
	}

	/*
	 * On a cluster: the target's whole row, carried up from the scan that
	 * read its ctid, in the junk column the explicit write reads an action's
	 * old row from (gp_modify.c, merge_target_junk()); and over ORCA's plan,
	 * gp_core's node that makes the ctid of a row on a segment the ctid the
	 * explicit write knows it by.
	 */
	if (state->cluster)
	{
		AttrNumber	resno = gp_orca_carry_whole_row(sub, 1, stmt->rtable);
		TargetEntry *tle;

		if (resno == InvalidAttrNumber)
		{
			*why = "a MERGE whose target's rows the plan does not carry";
			return false;
		}
		tle = list_nth_node(TargetEntry, sub->targetlist, resno - 1);
		tle->resname = pstrdup(GP_MERGE_TARGET_JUNK);
		tle->resjunk = true;
		sub = cb_core_api()->row_identity_make(sub, 2, 1);
		sub->plan_node_id = GpOrcaMaxPlanNodeId(stmt->planTree) + 1;
	}

	/* the source's row marks, each table's ctid carried up to them */
	foreach_int(qrti, state->sources)
	{
		RangeTblEntry *qrte = rt_fetch(qrti, merge->rtable);
		Index		prti = plan_rti_of(stmt, qrte);
		Var		   *ctid = makeVar(qrti, SelfItemPointerAttributeNumber, TIDOID,
								   -1, InvalidOid, 0);
		AttrNumber	resno = prti != 0 ? gp_orca_carry_rte_column(sub, prti, ctid)
			: InvalidAttrNumber;
		PlanRowMark *prm;
		TargetEntry *tle;

		if (resno == InvalidAttrNumber)
		{
			*why = "a MERGE whose source's rows the plan does not carry";
			return false;
		}
		prm = makeNode(PlanRowMark);
		prm->rti = prti;
		prm->prti = prti;
		prm->rowmarkId = list_length(marks) + 1;
		prm->markType = ROW_MARK_REFERENCE;
		prm->allMarkTypes = (1 << ROW_MARK_REFERENCE);
		prm->strength = LCS_NONE;
		prm->waitPolicy = LockWaitBlock;
		prm->isParent = false;
		marks = lappend(marks, prm);
		tle = list_nth_node(TargetEntry, sub->targetlist, resno - 1);
		tle->resname = psprintf("ctid%u", prm->rowmarkId);
		tle->resjunk = true;
	}

	/* the actions, as the planner prepares and set_plan_refs() fixes them */
	rel = table_open(qtarget->relid, NoLock);
	foreach_node(MergeAction, action, merge->mergeActionList)
	{
		MergeAction *a = copyObject(action);

		if (a->commandType == CMD_INSERT)
			a->targetList = insert_targetlist(a->targetList, rel);
		else if (a->commandType == CMD_UPDATE)
			a->updateColnos = extract_update_targetlist_colnos(a->targetList);
		a->targetList = (List *) merge_vars((Node *) a->targetList, state, rti);
		a->qual = merge_vars((Node *) implicit_qual(a->qual), state, rti);
		actions = lappend(actions, a);
	}
	table_close(rel, NoLock);

	mt = makeNode(ModifyTable);
	mt->plan.lefttree = sub;
	mt->plan.targetlist = NIL;
	mt->plan.startup_cost = sub->startup_cost;
	mt->plan.total_cost = sub->total_cost;
	mt->plan.plan_rows = sub->plan_rows;
	mt->plan.plan_width = 0;
	mt->plan.plan_node_id = GpOrcaMaxPlanNodeId(sub) + 1;
	mt->operation = CMD_MERGE;
	mt->canSetTag = merge->canSetTag;
	mt->nominalRelation = rti;
	mt->rootRelation = 0;
	mt->resultRelations = list_make1_int(rti);
	if (merge->withCheckOptions != NIL)
	{
		wcos = copyObject(merge->withCheckOptions);
		foreach_node(WithCheckOption, wco, wcos)
			wco->qual = merge_vars((Node *) implicit_qual(wco->qual), state, rti);
		mt->withCheckOptionLists = list_make1(wcos);
	}
	mt->fdwPrivLists = list_make1(NIL);
	mt->rowMarks = marks;
	mt->onConflictAction = ONCONFLICT_NONE;
	mt->mergeActionLists = list_make1(actions);
	joincond = (List *) merge_vars((Node *) implicit_qual(merge->mergeJoinCondition),
								   state, rti);
	mt->mergeJoinConditions = list_make1(joincond);
	if (merge->returningList != NIL)
	{
		returning = merge_returning(merge->returningList, state, rti);
		mt->returningLists = list_make1(returning);
		mt->returningOldAlias = merge->returningOldAlias;
		mt->returningNewAlias = merge->returningNewAlias;
		mt->plan.targetlist = copyObject(returning);
	}

	/*
	 * A partitioned target's partitions, each a result relation with the
	 * table's lists in its own columns -- an INSERT's, which is routed
	 * through the table, in the table's -- locked as the table is, as the
	 * planner's inheritance planning locks them; the table the root.
	 */
	if (state->partitioned)
	{
		Oid			root_oid = qtarget->relid;

		mt->rootRelation = rti;
		mt->resultRelations = part_rtis;
		mt->withCheckOptionLists = NIL;
		mt->fdwPrivLists = NIL;
		mt->mergeActionLists = NIL;
		mt->mergeJoinConditions = NIL;
		mt->returningLists = NIL;
		foreach_int(part_rti, part_rtis)
		{
			RangeTblEntry *prte = rt_fetch(part_rti, stmt->rtable);
			Oid			part_oid = prte->relid;
			List	   *pactions = NIL;

			foreach_node(MergeAction, a, actions)
			{
				MergeAction *pa = copyObject(a);

				if (pa->commandType != CMD_INSERT)
					pa->targetList = partition_exprs(pa->targetList, rti, root_oid,
													 part_rti, part_oid);
				if (pa->commandType == CMD_UPDATE)
					pa->updateColnos = gp_orca_partition_colnos(root_oid, part_oid,
																pa->updateColnos);
				pa->qual = (Node *) partition_exprs((List *) pa->qual, rti, root_oid,
													part_rti, part_oid);
				pactions = lappend(pactions, pa);
			}
			mt->mergeActionLists = lappend(mt->mergeActionLists, pactions);
			mt->mergeJoinConditions =
				lappend(mt->mergeJoinConditions,
						partition_exprs(joincond, rti, root_oid, part_rti, part_oid));
			if (wcos != NIL)
				mt->withCheckOptionLists =
					lappend(mt->withCheckOptionLists,
							partition_exprs(wcos, rti, root_oid, part_rti, part_oid));
			if (returning != NIL)
				mt->returningLists =
					lappend(mt->returningLists,
							partition_exprs(returning, rti, root_oid, part_rti, part_oid));
			mt->fdwPrivLists = lappend(mt->fdwPrivLists, NIL);
			prte->rellockmode = qtarget->rellockmode;
			LockRelationOid(part_oid, qtarget->rellockmode);
		}
		if (returning != NIL)
			mt->plan.targetlist = copyObject(linitial(mt->returningLists));
	}

	/* EvalPlanQual's parameter, which every node below depends on */
	epq = list_length(stmt->paramExecTypes);
	stmt->paramExecTypes = lappend_oid(stmt->paramExecTypes, InvalidOid);
	mt->epqParam = epq;
	GpOrcaAddParamToTree(sub, epq);

	stmt->planTree = &mt->plan;
	if (state->cluster)
		stmt->planTree = cb_core_api()->explicit_write(stmt, &mt->plan);
	stmt->commandType = CMD_MERGE;
	stmt->canSetTag = merge->canSetTag;
	stmt->hasReturning = merge->returningList != NIL;
	stmt->resultRelationRelids = bms_make_singleton(rti);
	foreach_int(part_rti, part_rtis)
		stmt->resultRelationRelids = bms_add_member(stmt->resultRelationRelids,
													part_rti);
	stmt->rowMarks = marks;
	foreach_node(PlanRowMark, prm, marks)
		stmt->rowMarkRelids = bms_add_member(stmt->rowMarkRelids, prm->rti);

	/*
	 * The result relation's permission entry, completed from the MERGE's:
	 * what ORCA's scan of it asked is SELECT, and the executor reads the
	 * columns an UPDATE sets and an INSERT gives (ExecGetUpdatedCols()).
	 */
	rte = rt_fetch(rti, stmt->rtable);
	perminfo = getRTEPermissionInfo(stmt->permInfos, rte);
	qperminfo = getRTEPermissionInfo(merge->rteperminfos, qtarget);
	perminfo->requiredPerms |= qperminfo->requiredPerms;
	perminfo->checkAsUser = qperminfo->checkAsUser;
	perminfo->selectedCols = bms_union(perminfo->selectedCols, qperminfo->selectedCols);
	perminfo->insertedCols = bms_union(perminfo->insertedCols, qperminfo->insertedCols);
	perminfo->updatedCols = bms_union(perminfo->updatedCols, qperminfo->updatedCols);
	rte->rellockmode = qtarget->rellockmode;
	return true;
}
