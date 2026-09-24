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
 * lockrows.c
 *	  SELECT ... FOR UPDATE, FOR SHARE and the rest, under ORCA.
 *
 * Cloudberry's ORCA reads no row marks: Cloudberry answers a locking clause
 * with a lock on the whole table, ExclusiveLock, taken when the statement is
 * analysed, and only its planner, with the global deadlock detector on,
 * ever locks rows -- for a SELECT of one table with no subquery in its WHERE
 * (checkCanOptSelectLockingClause, analyze.c), whose rows the segments lock
 * below the Gather.  PostgreSQL 19 locks rows in the LockRows node its plan
 * has to have.  The port's ORCA plans such a query, as decided in
 * cloudberry.md ("Row locks under ORCA"):
 *
 *   on one node, as PostgreSQL does: the rows of every table the query
 *   locks, with LockRows at the top of the plan -- below a LIMIT, so that
 *   the limit counts locked rows -- NOWAIT, SKIP LOCKED and the re-check of
 *   a row updated meanwhile being PostgreSQL's own;
 *
 *   on a cluster with the deadlock detector on, Cloudberry's shape: for the
 *   query its planner locks the rows of, the segments lock them, LockRows
 *   running in the Gather's fragment, above its sort so the Gather's merge
 *   keeps the order (greenplum-db/gpdb#9724);
 *
 *   on a cluster otherwise, Cloudberry's table lock, ExclusiveLock -- put in
 *   the plan's range table, so that a cached plan takes it too -- and no
 *   LockRows.
 *
 * What ORCA plans cannot take: a table whose rows it does not lock in a
 * query that locks some -- PostgreSQL copies such a table's whole row
 * (ROW_MARK_COPY) -- a table with inheritance children, the same table
 * twice, and a locking clause inside a subquery.  Those go to the planner.
 *
 * What LockRows needs of the plan below it is each locked table's ctid,
 * under the name "ctid<n>" (ExecBuildAuxRowMark).  ORCA keeps only what a
 * query outputs, so the ctid is added to the query as an output column,
 * which the plan then marks junk -- in the fragment below LockRows too,
 * where it is followed down from the query's output.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "cb_compat.h"
#include "gp_core_api.h"
#include "gp_motion.h"
#include "gp_orca_lockrows.h"
#include "gp_policy.h"

/* A table whose rows the plan locks. */
typedef struct OrcaRowMark
{
	Index		rti;			/* in the query */
	Oid			relid;
	int			id;				/* rowmarkId: its ctid's column is "ctid<id>" */
	AttrNumber	resno;			/* the query's output column holding it */
	LockClauseStrength strength;
	LockWaitPolicy waitPolicy;
} OrcaRowMark;

/* ------------------------------------------------------------------------- */
/* The query                                                                 */
/* ------------------------------------------------------------------------- */

static bool
nested_rowmarks_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		if (((Query *) node)->rowMarks != NIL)
			return true;
		return query_tree_walker((Query *) node, nested_rowmarks_walker,
								 context, 0);
	}
	return expression_tree_walker(node, nested_rowmarks_walker, context);
}

/* Is the global deadlock detector on?  gp_core's setting. */
static bool
gdd_enabled(void)
{
	const char *value = GetConfigOption("gp.enable_global_deadlock_detector",
										true, false);

	return value != NULL && strcmp(value, "on") == 0;
}

/*
 * The query Cloudberry's planner locks the rows of: one table in FROM, no
 * subquery, no set operation (checkCanOptSelectLockingClause), and a table
 * whose rows are each on one segment -- a replicated table's are on all.
 */
static bool
cluster_locks_rows(Query *query, Index rti)
{
	RangeTblEntry *rte = rt_fetch(rti, query->rtable);
	GpPolicy   *policy;

	if (query->setOperations != NULL || query->hasSubLinks ||
		query->jointree == NULL || list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef) ||
		((RangeTblRef *) linitial(query->jointree->fromlist))->rtindex != (int) rti)
		return false;
	policy = GpPolicyGet(rte->relid);
	return policy != NULL &&
		(GpPolicyIsHashPartitioned(policy) || GpPolicyIsRandomPartitioned(policy));
}

bool
GpOrcaPrepareRowMarks(Query *query, List **marks, const char **why)
{
	List	   *keep = NIL;
	ListCell   *lc;
	int			nrels = 0;
	bool		cluster = !IS_SINGLENODE();
	int			id = 0;

	*marks = NIL;
	*why = NULL;

	if (query_tree_walker(query, nested_rowmarks_walker, NULL, 0))
	{
		*why = "a locking clause inside a subquery";
		return false;
	}
	if (query->rowMarks == NIL)
		return true;

	/* every table the query reads, each once */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind != RTE_RELATION || rte->relkind == RELKIND_VIEW)
			continue;
		nrels++;
		foreach_node(RangeTblEntry, other, query->rtable)
		{
			if (other != rte && other->rtekind == RTE_RELATION &&
				other->relid == rte->relid)
			{
				*why = "a locking clause on a query that reads a table twice";
				return false;
			}
		}
	}

	foreach(lc, query->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, lc);
		RangeTblEntry *rte = rt_fetch(rc->rti, query->rtable);

		if (rc->pushedDown || rte->rtekind != RTE_RELATION)
		{
			*why = "a locking clause on a view or a subquery";
			return false;
		}
		if (rte->relkind != RELKIND_RELATION)
		{
			*why = "a locking clause on a foreign or partitioned table";
			return false;
		}
		if (has_subclass(rte->relid))
		{
			*why = "a locking clause on a table with inheritance children";
			return false;
		}
	}

	/* a table read and not locked would need its whole row copied */
	if (list_length(query->rowMarks) != nrels)
	{
		*why = "a locking clause on some of the tables a query reads";
		return false;
	}

	foreach(lc, query->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, lc);
		RangeTblEntry *rte = rt_fetch(rc->rti, query->rtable);

		/*
		 * A cluster where rows are not locked: Cloudberry's table lock, in
		 * the range table and so in every execution of a cached plan, and
		 * now, as Cloudberry takes it when the statement is analysed.  A
		 * table the coordinator holds the rows of keeps its row lock.
		 */
		if (cluster && GpPolicyGet(rte->relid) != NULL &&
			!(gdd_enabled() && list_length(query->rowMarks) == 1 &&
			  cluster_locks_rows(query, rc->rti)))
		{
			rte->rellockmode = ExclusiveLock;
			LockRelationOid(rte->relid, ExclusiveLock);
			continue;
		}
		keep = lappend(keep, rc);
	}

	/* the coordinator's own table among a cluster's is the planner's */
	if (cluster && keep != NIL && list_length(keep) != list_length(query->rowMarks))
	{
		*why = "a locking clause on a coordinator's table beside a distributed one";
		return false;
	}

	foreach(lc, keep)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, lc);
		RangeTblEntry *rte = rt_fetch(rc->rti, query->rtable);
		OrcaRowMark *mark = palloc0(sizeof(OrcaRowMark));
		Var		   *ctid = makeVar(rc->rti, SelfItemPointerAttributeNumber,
								   TIDOID, -1, InvalidOid, 0);

		mark->rti = rc->rti;
		mark->relid = rte->relid;
		mark->id = ++id;
		mark->resno = list_length(query->targetList) + 1;
		mark->strength = rc->strength;
		mark->waitPolicy = rc->waitPolicy;
		query->targetList = lappend(query->targetList,
									makeTargetEntry((Expr *) ctid, mark->resno,
													psprintf("ctid%d", mark->id),
													false));
		*marks = lappend(*marks, mark);
	}

	/* the plan takes them now, not the translator, which refuses them */
	query->rowMarks = NIL;
	return true;
}

/* ------------------------------------------------------------------------- */
/* The plan                                                                  */
/* ------------------------------------------------------------------------- */

static bool
is_motion(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		strcmp(((CustomScan *) plan)->methods->CustomName, GP_MOTION_NAME) == 0;
}

static RowMarkType
mark_type(LockClauseStrength strength)
{
	switch (strength)
	{
		case LCS_FORKEYSHARE:
			return ROW_MARK_KEYSHARE;
		case LCS_FORSHARE:
			return ROW_MARK_SHARE;
		case LCS_FORNOKEYUPDATE:
			return ROW_MARK_NOKEYEXCLUSIVE;
		default:
			return ROW_MARK_EXCLUSIVE;
	}
}

/* The largest plan node id in a tree, the fragments below Motions included. */
static int
max_plan_node_id(Plan *plan)
{
	int			max;
	List	   *subs = NIL;
	ListCell   *lc;

	if (plan == NULL)
		return -1;

	max = plan->plan_node_id;
	max = Max(max, max_plan_node_id(plan->lefttree));
	max = Max(max, max_plan_node_id(plan->righttree));
	if (IsA(plan, Append))
		subs = ((Append *) plan)->appendplans;
	else if (IsA(plan, BitmapAnd))
		subs = ((BitmapAnd *) plan)->bitmapplans;
	else if (IsA(plan, BitmapOr))
		subs = ((BitmapOr *) plan)->bitmapplans;
	else if (IsA(plan, CustomScan))
		subs = ((CustomScan *) plan)->custom_plans;
	foreach(lc, subs)
		max = Max(max, max_plan_node_id((Plan *) lfirst(lc)));
	return max;
}

/*
 * Every node below LockRows depends on its EvalPlanQual parameter, as the
 * planner makes them (finalize_plan); the translator does the same for
 * ModifyTable's (AddParamToPlanTree).
 */
static void
add_param_to_tree(Plan *plan, int paramid)
{
	if (plan == NULL)
		return;
	plan->extParam = bms_add_member(plan->extParam, paramid);
	plan->allParam = bms_add_member(plan->allParam, paramid);
	add_param_to_tree(plan->lefttree, paramid);
	add_param_to_tree(plan->righttree, paramid);
	if (IsA(plan, BitmapAnd) || IsA(plan, BitmapOr))
	{
		ListCell   *lc;

		foreach(lc, IsA(plan, BitmapAnd) ? ((BitmapAnd *) plan)->bitmapplans
				: ((BitmapOr *) plan)->bitmapplans)
			add_param_to_tree((Plan *) lfirst(lc), paramid);
	}
}

/*
 * The output column `resno` of `plan`, followed down to the node `target`
 * whose own column it is: through each node that passes its child's column
 * on, and through a Motion to its fragment.  0 if it is computed on the way.
 */
static AttrNumber
follow_column(Plan *plan, AttrNumber resno, Plan *target)
{
	while (plan != target)
	{
		TargetEntry *tle;
		Var		   *var;

		if (plan == NULL || resno < 1 || resno > list_length(plan->targetlist))
			return 0;
		tle = list_nth_node(TargetEntry, plan->targetlist, resno - 1);
		if (!IsA(tle->expr, Var))
			return 0;
		var = (Var *) tle->expr;

		if (is_motion(plan))
		{
			CustomScan *cscan = (CustomScan *) plan;
			TargetEntry *scan_tle;

			/* the Motion's columns are its scan tuple's, the fragment's */
			if (var->varno != INDEX_VAR ||
				var->varattno < 1 || var->varattno > list_length(cscan->custom_scan_tlist))
				return 0;
			scan_tle = list_nth_node(TargetEntry, cscan->custom_scan_tlist,
									 var->varattno - 1);
			if (!IsA(scan_tle->expr, Var) || ((Var *) scan_tle->expr)->varno != OUTER_VAR)
				return 0;
			resno = ((Var *) scan_tle->expr)->varattno;
		}
		else
		{
			if (var->varno != OUTER_VAR)
				return 0;
			resno = var->varattno;
		}
		plan = plan->lefttree;
	}
	return resno;
}

bool
GpOrcaAddLockRows(PlannedStmt *stmt, List *marks, const char **why)
{
	List	   *rowmarks = NIL;
	Plan	   *parent = NULL;
	Plan	   *below;
	LockRows   *lockrows;
	List	   *tlist = NIL;
	int			epq;
	ListCell   *lc;

	*why = NULL;
	if (marks == NIL)
		return true;

	/* each locked table's entry in the plan's range table */
	foreach(lc, marks)
	{
		OrcaRowMark *mark = (OrcaRowMark *) lfirst(lc);
		PlanRowMark *prm = makeNode(PlanRowMark);
		Index		rti = 0;
		int			i = 0;

		foreach_node(RangeTblEntry, rte, stmt->rtable)
		{
			i++;
			if (rte->rtekind != RTE_RELATION || rte->relid != mark->relid)
				continue;
			if (rti != 0)
			{
				*why = "a locked table the plan scans twice";
				return false;
			}
			rti = i;
		}
		if (rti == 0)
		{
			*why = "a locked table the plan does not scan";
			return false;
		}
		prm->rti = rti;
		prm->prti = rti;
		prm->rowmarkId = mark->id;
		prm->markType = mark_type(mark->strength);
		prm->allMarkTypes = (1 << prm->markType);
		prm->strength = mark->strength;
		prm->waitPolicy = mark->waitPolicy;
		prm->isParent = false;
		rowmarks = lappend(rowmarks, prm);
	}

	/*
	 * Where LockRows goes: at the top of the plan on one node, below the
	 * Gather in its fragment on a cluster -- through what the coordinator
	 * does above the Gather -- and in either, below a LIMIT.
	 */
	below = stmt->planTree;
	if (!IS_SINGLENODE())
	{
		while (below != NULL && !is_motion(below))
		{
			if (below->righttree != NULL)
			{
				*why = "a locking clause on a plan that joins on the coordinator";
				return false;
			}
			parent = below;
			below = below->lefttree;
		}
		if (below == NULL ||
			cb_core_api()->motion_type(below) != GP_MOTION_GATHER)
		{
			*why = "a locking clause on a plan with no Gather at the top";
			return false;
		}
		parent = below;
		below = below->lefttree;
	}
	if (IsA(below, Limit))
	{
		parent = below;
		below = below->lefttree;
	}

	/* the ctids, in the node LockRows reads, by the names it looks for */
	foreach(lc, marks)
	{
		OrcaRowMark *mark = (OrcaRowMark *) lfirst(lc);
		AttrNumber	resno = follow_column(stmt->planTree, mark->resno, below);
		TargetEntry *tle;

		if (resno == 0)
		{
			*why = "a locked table's ctid computed on the way up the plan";
			return false;
		}
		tle = list_nth_node(TargetEntry, below->targetlist, resno - 1);
		tle->resname = psprintf("ctid%d", mark->id);
		tle->resjunk = true;
	}

	/* LockRows passes on its child's columns, as setrefs.c makes it */
	foreach_node(TargetEntry, tle, below->targetlist)
	{
		Var		   *var = makeVar(OUTER_VAR, tle->resno,
								  exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);

		tlist = lappend(tlist, makeTargetEntry((Expr *) var, tle->resno,
											   tle->resname, tle->resjunk));
	}

	epq = list_length(stmt->paramExecTypes);
	stmt->paramExecTypes = lappend_oid(stmt->paramExecTypes, InvalidOid);

	lockrows = makeNode(LockRows);
	lockrows->plan.targetlist = tlist;
	lockrows->plan.lefttree = below;
	lockrows->plan.startup_cost = below->startup_cost;
	lockrows->plan.total_cost = below->total_cost;
	lockrows->plan.plan_rows = below->plan_rows;
	lockrows->plan.plan_width = below->plan_width;
	lockrows->plan.plan_node_id = max_plan_node_id(stmt->planTree) + 1;
	lockrows->rowMarks = rowmarks;
	lockrows->epqParam = epq;
	add_param_to_tree(below, epq);

	if (parent == NULL)
		stmt->planTree = &lockrows->plan;
	else
		parent->lefttree = &lockrows->plan;
	stmt->rowMarks = rowmarks;

	/* and the query's own output leaves the ctids out */
	foreach(lc, marks)
	{
		OrcaRowMark *mark = (OrcaRowMark *) lfirst(lc);

		if (mark->resno <= list_length(stmt->planTree->targetlist))
			list_nth_node(TargetEntry, stmt->planTree->targetlist,
						  mark->resno - 1)->resjunk = true;
	}
	return true;
}
