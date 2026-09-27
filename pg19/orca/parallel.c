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
 * parallel.c
 *	  Parallelism within a segment, under ORCA: PostgreSQL's Gather in the
 *	  fragments a segment's writer runs, over their large scans.
 *
 * Cloudberry's ORCA plans a parallel scan with enable_parallel
 * (CXformGet2ParallelTableScan): a scan whose rows are spread over the
 * sibling processes of a widened gang, at random, so that each parallel
 * scan is a slice of its own below a Motion (CDistributionSpecWorkerRandom)
 * and its translator makes no Gather (TranslateDXLParallelTblScan()).  The
 * port has no sibling processes -- a slice runs in one process on each
 * segment, and a Gather of PostgreSQL's in it starts that process's workers
 * (gp_core's gp_parallel.c) -- and so leaves that transform off
 * (gpdb::IsParallelModeOK()) and works on the plan ORCA made, as lockrows.c
 * and merge.c do:
 *
 *   the fragments a segment's writer runs are those of the Gather Motions
 *   the coordinator receives, and only a writer can start workers -- a
 *   reader is a member of its writer's lock group, which cannot lead one
 *   of its own -- so it walks the coordinator's part of the plan down to
 *   each Gather Motion, and that Motion's fragment down to its Motions
 *   below, where the readers' slices are;
 *
 *   in the fragment, a Gather goes over the largest subtree its
 *   participants can split, where PostgreSQL's planner would find that
 *   pays: a sequential scan of a table whose storage shares itself among a
 *   parallel scan's participants -- heap, ao_row, ao_column, pax -- made
 *   parallel-aware, under the outer sides of hash joins whose inner sides
 *   each participant reads whole and hashes (a parallel-oblivious join, as
 *   PostgreSQL's planner makes one), all of whose expressions a worker may
 *   compute; its workers by compute_parallel_worker() of the table's pages
 *   on a segment, and its costs, in PostgreSQL's units, from the table's
 *   size there and ORCA's estimates of the rows (cost_seqscan(),
 *   cost_gather());
 *
 *   ORCA's first stage of an aggregate over such a subtree is done in
 *   three: in each participant, below the Gather, and once more above it,
 *   the participants' states combined into the segment's and passed on to
 *   the coordinator's last stage as ORCA's plan has them (three_stage());
 *
 *   not below a node that may read the Motion's rows only in part -- a
 *   Limit, the inner side of a NestLoop, a merge join -- nor below the
 *   inner side of a NestLoop in the fragment, which runs again for each
 *   outer row, nor into a Motion, a SubPlan or another CustomScan: the
 *   writer runs a fragment with a Gather whole at its first FETCH, and
 *   only nodes that read their input whole or pass it on are above one.
 *
 * Only a SELECT that may run in parallel mode, as PostgreSQL's planner asks
 * (standard_planner()): CURSOR_OPT_PARALLEL_OK, which a cursor lacks, no
 * data-modifying CTE, no row marks, and nothing parallel-unsafe anywhere --
 * the writer runs its whole fragment in parallel mode.  The coordinator
 * runs no fragment and enters no parallel mode for one (parallelModeNeeded
 * stays false); a segment keeps a fragment's Gathers only in its writer,
 * and only then runs the fragment in parallel mode (gp_parallel.c).
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/gporca/libgpopt/src/xforms/CXformGet2ParallelTableScan.cpp
 *	  and the translator's parallel scan (TranslateDXLParallelTblScan() in
 *	  src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp), as far as a
 *	  Gather within a segment's process does their work
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cb_compat.h"
#include "gp_core_api.h"
#include "gp_motion.h"
#include "gp_orca_parallel.h"

typedef struct OrcaParallel
{
	PlannedStmt *stmt;
	PlannerInfo *root;			/* for is_parallel_safe() and cost_qual_eval() */
	int			nsegments;
	int			next_node_id;	/* the plan node id of the next Gather */
} OrcaParallel;

static bool
is_motion(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		strcmp(((CustomScan *) plan)->methods->CustomName, GP_MOTION_NAME) == 0;
}

/* The largest plan node id of a tree, with its fragments and subqueries. */
static int
max_node_id(Plan *plan)
{
	int			max;
	int			below;
	List	   *children = NIL;

	if (plan == NULL)
		return -1;
	check_stack_depth();
	max = plan->plan_node_id;
	below = max_node_id(plan->lefttree);
	max = Max(max, below);
	below = max_node_id(plan->righttree);
	max = Max(max, below);
	switch (nodeTag(plan))
	{
		case T_Append:
			children = ((Append *) plan)->appendplans;
			break;
		case T_MergeAppend:
			children = ((MergeAppend *) plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = ((BitmapAnd *) plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = ((BitmapOr *) plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = ((CustomScan *) plan)->custom_plans;
			break;
		case T_SubqueryScan:
			children = list_make1(((SubqueryScan *) plan)->subplan);
			break;
		default:
			break;
	}
	foreach_ptr(Plan, child, children)
	{
		below = max_node_id(child);
		max = Max(max, below);
	}
	return max;
}

/* A table access method that shares a scan among its participants. */
static bool
shares_scan(Oid amoid)
{
	char	   *name = get_am_name(amoid);

	return name != NULL &&
		(strcmp(name, "heap") == 0 || strcmp(name, "ao_row") == 0 ||
		 strcmp(name, "ao_column") == 0 || strcmp(name, "pax") == 0);
}

/*
 * What a Gather's participants share: the sequential scan that drives the
 * subtree split among them, its table's share on a segment -- pages and
 * rows, as ANALYZE counted the whole -- and the workers PostgreSQL's rule
 * gives it (compute_parallel_worker()).
 */
typedef struct Share
{
	SeqScan    *driver;
	double		pages;
	double		tuples;
	int			workers;
	double		divisor;		/* get_parallel_divisor()'s */
} Share;

/* May a worker compute a node's own expressions? */
static bool
node_safe(OrcaParallel *op, Plan *plan)
{
	return plan->initPlan == NIL &&
		is_parallel_safe(op->root, (Node *) plan->qual) &&
		is_parallel_safe(op->root, (Node *) plan->targetlist);
}

/* A table a worker may read: a plain one, not a temporary one. */
static bool
worker_reads(OrcaParallel *op, Scan *scan)
{
	RangeTblEntry *rte = rt_fetch(scan->scanrelid, op->stmt->rtable);

	return rte->rtekind == RTE_RELATION &&
		get_rel_relkind(rte->relid) == RELKIND_RELATION &&
		get_rel_persistence(rte->relid) != RELPERSISTENCE_TEMP;
}

/* A sequential scan that can drive a split subtree, and its share. */
static bool
scan_share(OrcaParallel *op, SeqScan *scan, Share *share)
{
	RangeTblEntry *rte = rt_fetch(scan->scan.scanrelid, op->stmt->rtable);
	Relation	rel;
	RelOptInfo *dummy;

	if (!worker_reads(op, &scan->scan))
		return false;
	rel = table_open(rte->relid, AccessShareLock);
	if (!shares_scan(rel->rd_rel->relam) || rel->rd_rel->reltuples < 0)
	{
		table_close(rel, NoLock);
		return false;
	}
	share->driver = scan;
	share->pages = (double) rel->rd_rel->relpages / op->nsegments;
	share->tuples = rel->rd_rel->reltuples / op->nsegments;
	dummy = makeNode(RelOptInfo);
	dummy->reloptkind = RELOPT_BASEREL;
	dummy->rel_parallel_workers = RelationGetParallelWorkers(rel, -1);
	table_close(rel, NoLock);
	share->workers = compute_parallel_worker(dummy, share->pages, -1,
											 max_parallel_workers_per_gather);
	share->divisor = share->workers;
	if (parallel_leader_participation && 1.0 - 0.3 * share->workers > 0)
		share->divisor += 1.0 - 0.3 * share->workers;
	return share->workers > 0;
}

/*
 * Can a subtree run whole in each participant, as the inner side of a join
 * split among them: sequential scans, hash joins and their hashes, and
 * projections, of expressions a worker may compute.
 */
static bool
whole_in_each(OrcaParallel *op, Plan *plan)
{
	if (plan == NULL)
		return true;
	check_stack_depth();
	if (!node_safe(op, plan))
		return false;
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return worker_reads(op, (Scan *) plan);
		case T_Hash:
			if (!is_parallel_safe(op->root, (Node *) ((Hash *) plan)->hashkeys))
				return false;
			break;
		case T_HashJoin:
			if (!is_parallel_safe(op->root, (Node *) ((Join *) plan)->joinqual) ||
				!is_parallel_safe(op->root, (Node *) ((HashJoin *) plan)->hashclauses) ||
				!is_parallel_safe(op->root, (Node *) ((HashJoin *) plan)->hashkeys))
				return false;
			break;
		case T_Result:
			if (!is_parallel_safe(op->root, ((Result *) plan)->resconstantqual))
				return false;
			break;
		default:
			return false;
	}
	return whole_in_each(op, plan->lefttree) && whole_in_each(op, plan->righttree);
}

/*
 * Can a subtree be split among a Gather's participants: a sequential scan
 * that shares itself among them, the driver, below the outer sides of hash
 * joins whose inner sides each participant reads whole -- an inner, left,
 * semi or anti join, where an outer row meets every inner row in the one
 * participant that reads it; not a right or full join, whose unmatched
 * inner rows every participant would return -- and projections.
 */
static bool
splits(OrcaParallel *op, Plan *plan, Share *share)
{
	if (plan == NULL || !node_safe(op, plan))
		return false;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return scan_share(op, (SeqScan *) plan, share);
		case T_HashJoin:
			{
				HashJoin   *hj = (HashJoin *) plan;
				JoinType	jointype = hj->join.jointype;

				if ((jointype != JOIN_INNER && jointype != JOIN_LEFT &&
					 jointype != JOIN_SEMI && jointype != JOIN_ANTI) ||
					!is_parallel_safe(op->root, (Node *) hj->join.joinqual) ||
					!is_parallel_safe(op->root, (Node *) hj->hashclauses) ||
					!is_parallel_safe(op->root, (Node *) hj->hashkeys))
					return false;
				return whole_in_each(op, plan->righttree) &&
					splits(op, plan->lefttree, share);
			}
		case T_Result:
			return plan->lefttree != NULL &&
				is_parallel_safe(op->root, ((Result *) plan)->resconstantqual) &&
				splits(op, plan->lefttree, share);
		default:
			return false;
	}
}

/*
 * The CPU a split subtree spends above its driver on each row, which the
 * participants divide as they divide the scan's: a hash join's probes.
 */
static double
split_cpu(Plan *plan)
{
	double		cpu = 0;

	for (; plan != NULL && !IsA(plan, SeqScan); plan = plan->lefttree)
		if (IsA(plan, HashJoin))
			cpu += plan->lefttree->plan_rows *
				(cpu_operator_cost * list_length(((HashJoin *) plan)->hashclauses) +
				 cpu_tuple_cost);
	return cpu;
}

/*
 * Does a Gather pay, in PostgreSQL's units (cost_seqscan(), cost_gather()):
 * the driver's CPU and "cpu" more, divided among the participants, against
 * the Gather's start and the "rows" it passes on.
 */
static bool
pays(OrcaParallel *op, Share *share, double cpu, double rows)
{
	QualCost	qual;
	double		disk = share->pages * seq_page_cost;
	double		serial;
	double		parallel;

	cost_qual_eval(&qual, share->driver->scan.plan.qual, op->root);
	cpu += share->tuples * (cpu_tuple_cost + qual.per_tuple);
	serial = disk + cpu;
	parallel = disk + cpu / share->divisor + parallel_setup_cost +
		rows * parallel_tuple_cost;
	return parallel < serial;
}

/*
 * A subtree split among the participants: its driver parallel-aware, and
 * each node's rows those of one participant, as the planner's partial paths
 * estimate them; what each reads whole is left as it is.
 */
static void
mark_split(Plan *plan, Share *share)
{
	for (; plan != NULL; plan = plan->lefttree)
	{
		plan->parallel_safe = true;
		plan->plan_rows = clamp_row_est(plan->plan_rows / share->divisor);
		if (plan->righttree != NULL)
			plan->righttree->parallel_safe = true;
		if (plan == &share->driver->scan.plan)
		{
			plan->parallel_aware = true;
			break;
		}
	}
}

/* A Gather over a plan, passing on its columns as they are. */
static Plan *
make_gather(OrcaParallel *op, Plan *plan, int workers)
{
	Gather	   *gather = makeNode(Gather);

	foreach_node(TargetEntry, tle, plan->targetlist)
		gather->plan.targetlist =
			lappend(gather->plan.targetlist,
					makeTargetEntry((Expr *) makeVarFromTargetEntry(OUTER_VAR, tle),
									tle->resno, tle->resname, tle->resjunk));
	gather->plan.startup_cost = plan->startup_cost;
	gather->plan.total_cost = plan->total_cost;
	gather->plan.plan_rows = plan->plan_rows;
	gather->plan.plan_width = plan->plan_width;
	gather->plan.plan_node_id = op->next_node_id++;
	gather->plan.extParam = bms_copy(plan->extParam);
	gather->plan.allParam = bms_copy(plan->allParam);
	gather->plan.lefttree = plan;
	gather->num_workers = workers;
	gather->rescan_param = -1;
	gather->single_copy = false;
	gather->invisible = false;
	gather->initParam = NULL;
	return &gather->plan;
}

/* Can an aggregate's states be combined, and passed on between processes? */
static bool
combinable(Aggref *aggref)
{
	HeapTuple	tuple;
	Form_pg_aggregate agg;
	bool		result;

	if (aggref->aggkind != AGGKIND_NORMAL || aggref->aggdistinct != NIL ||
		aggref->aggorder != NIL || aggref->aggdirectargs != NIL)
		return false;
	tuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(tuple))
		return false;
	agg = (Form_pg_aggregate) GETSTRUCT(tuple);
	result = OidIsValid(agg->aggcombinefn) &&
		(aggref->aggtranstype != INTERNALOID ||
		 (OidIsValid(agg->aggserialfn) && OidIsValid(agg->aggdeserialfn)));
	ReleaseSysCache(tuple);
	return result;
}

/*
 * ORCA's first stage of an aggregate on a segment (AGGSPLIT_INITIAL_SERIAL),
 * over a subtree a Gather's participants split, done in three: that stage
 * in each participant, below the Gather, and above it one more that
 * combines theirs into the segment's, and passes it on serialized, as the
 * first stage did, to the coordinator's last (Cloudberry's three-stage
 * aggregation of a widened gang, in PostgreSQL's terms).  Its groups are
 * the lower one's columns, which it passes on; a plain or hashed aggregate
 * only, the Gather's rows being in no order.
 */
static Plan *
three_stage(OrcaParallel *op, Agg *agg)
{
	Plan	   *below = agg->plan.lefttree;
	Share		share;
	Agg		   *upper;
	int			naggs = 0;

	if (agg->aggsplit != AGGSPLIT_INITIAL_SERIAL ||
		(agg->aggstrategy != AGG_PLAIN && agg->aggstrategy != AGG_HASHED) ||
		agg->plan.qual != NIL || agg->groupingSets != NIL || agg->chain != NIL ||
		!node_safe(op, &agg->plan))
		return NULL;
	foreach_node(TargetEntry, tle, agg->plan.targetlist)
	{
		if (IsA(tle->expr, Aggref))
		{
			if (!combinable((Aggref *) tle->expr))
				return NULL;
			naggs++;
		}
		else if (!IsA(tle->expr, Var))
			return NULL;
	}

	upper = makeNode(Agg);
	upper->aggstrategy = agg->aggstrategy;
	upper->aggsplit = AGGSPLITOP_COMBINE | AGGSPLITOP_DESERIALIZE |
		AGGSPLITOP_SKIPFINAL | AGGSPLITOP_SERIALIZE;
	upper->numCols = agg->numCols;
	upper->grpColIdx = palloc_array(AttrNumber, Max(agg->numCols, 1));
	for (int i = 0; i < agg->numCols; i++)
	{
		upper->grpColIdx[i] = 0;
		foreach_node(TargetEntry, tle, agg->plan.targetlist)
			if (IsA(tle->expr, Var) && ((Var *) tle->expr)->varno == OUTER_VAR &&
				((Var *) tle->expr)->varattno == agg->grpColIdx[i])
				upper->grpColIdx[i] = tle->resno;
		if (upper->grpColIdx[i] == 0)
			return NULL;
	}
	upper->grpOperators = agg->numCols > 0 ?
		palloc_array(Oid, agg->numCols) : NULL;
	upper->grpCollations = agg->numCols > 0 ?
		palloc_array(Oid, agg->numCols) : NULL;
	if (agg->numCols > 0)
	{
		memcpy(upper->grpOperators, agg->grpOperators, agg->numCols * sizeof(Oid));
		memcpy(upper->grpCollations, agg->grpCollations, agg->numCols * sizeof(Oid));
	}
	upper->numGroups = agg->numGroups;
	upper->transitionSpace = agg->transitionSpace;

	/* each participant's groups, which the combining stage reads */
	if (!splits(op, below, &share) ||
		!pays(op, &share,
			  split_cpu(below) + below->plan_rows * cpu_operator_cost * (naggs + agg->numCols),
			  agg->plan.plan_rows * (share.workers + 1)))
		return NULL;

	/* its columns: the lower one's, each state combined */
	foreach_node(TargetEntry, tle, agg->plan.targetlist)
	{
		Expr	   *expr = (Expr *) makeVarFromTargetEntry(OUTER_VAR, tle);

		if (IsA(tle->expr, Aggref))
		{
			Aggref	   *lower = (Aggref *) tle->expr;
			Aggref	   *combine = makeNode(Aggref);

			memcpy(combine, lower, sizeof(Aggref));
			combine->args = NIL;
			combine->aggfilter = NULL;
			combine = copyObject(combine);
			combine->args = list_make1(makeTargetEntry(expr, 1, NULL, false));
			combine->aggsplit = upper->aggsplit;
			expr = (Expr *) combine;
		}
		upper->plan.targetlist =
			lappend(upper->plan.targetlist,
					makeTargetEntry(expr, tle->resno, tle->resname, tle->resjunk));
	}
	upper->plan.startup_cost = agg->plan.startup_cost;
	upper->plan.total_cost = agg->plan.total_cost;
	upper->plan.plan_rows = agg->plan.plan_rows;
	upper->plan.plan_width = agg->plan.plan_width;
	upper->plan.plan_node_id = op->next_node_id++;

	mark_split(below, &share);
	agg->plan.parallel_safe = true;
	upper->plan.lefttree = make_gather(op, &agg->plan, share.workers);
	return &upper->plan;
}

/*
 * The fragment a writer runs: its scans that a Gather pays over, through
 * the nodes that run once each time it runs -- not a NestLoop's inner side,
 * which runs again for each outer row, not below a Limit, which may read a
 * part of what is below, not into a Motion, whose fragment another process
 * runs, nor any other CustomScan.
 */
static Plan *
fragment_walk(OrcaParallel *op, Plan *plan)
{
	Share		share;

	if (plan == NULL)
		return NULL;
	check_stack_depth();

	/* the largest subtree the participants can split, if a Gather pays */
	if (splits(op, plan, &share) &&
		pays(op, &share, split_cpu(plan), plan->plan_rows))
	{
		mark_split(plan, &share);
		return make_gather(op, plan, share.workers);
	}

	switch (nodeTag(plan))
	{
		case T_Agg:
			{
				Plan	   *upper = three_stage(op, (Agg *) plan);

				if (upper != NULL)
					return upper;
				plan->lefttree = fragment_walk(op, plan->lefttree);
				break;
			}
		case T_HashJoin:
		case T_MergeJoin:
		case T_Hash:
		case T_Sort:
		case T_Result:
		case T_Unique:
		case T_Group:
		case T_WindowAgg:
		case T_SetOp:
		case T_Material:
		case T_ProjectSet:
			plan->lefttree = fragment_walk(op, plan->lefttree);
			plan->righttree = fragment_walk(op, plan->righttree);
			break;
		case T_NestLoop:
			plan->lefttree = fragment_walk(op, plan->lefttree);
			break;
		case T_Append:
			{
				ListCell   *lc;

				foreach(lc, ((Append *) plan)->appendplans)
					lfirst(lc) = fragment_walk(op, (Plan *) lfirst(lc));
				break;
			}
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan =
				fragment_walk(op, ((SubqueryScan *) plan)->subplan);
			break;
		default:
			break;
	}
	return plan;
}

/*
 * The coordinator's part of the plan, down to each Gather Motion that it
 * reads to its end: a node that reads all its input before it returns a row
 * -- a Sort, a Hash, a plain or hashed Agg, a hashed SetOp -- reads the
 * Motions below it to their end whenever it runs; a Limit, a NestLoop's
 * inner side and a merge join may not; the rest read as far as what reads
 * them does.
 */
static void
coordinator_walk(OrcaParallel *op, Plan *plan, bool whole)
{
	if (plan == NULL)
		return;
	check_stack_depth();

	if (is_motion(plan))
	{
		if (whole && cb_core_api()->motion_type(plan) == GP_MOTION_GATHER)
			plan->lefttree = fragment_walk(op, plan->lefttree);
		return;
	}

	switch (nodeTag(plan))
	{
		case T_Sort:
		case T_Hash:
			whole = true;
			break;
		case T_Agg:
			if (((Agg *) plan)->aggstrategy == AGG_PLAIN ||
				((Agg *) plan)->aggstrategy == AGG_HASHED)
				whole = true;
			break;
		case T_SetOp:
			if (((SetOp *) plan)->strategy == SETOP_HASHED)
				whole = true;
			break;
		case T_Limit:
		case T_MergeJoin:
			whole = false;
			break;
		case T_NestLoop:
			coordinator_walk(op, plan->lefttree, whole);
			coordinator_walk(op, plan->righttree, false);
			return;
		case T_Append:
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				coordinator_walk(op, child, whole);
			break;
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				coordinator_walk(op, child, whole);
			break;
		case T_SubqueryScan:
			coordinator_walk(op, ((SubqueryScan *) plan)->subplan, whole);
			break;
		case T_CustomScan:
			return;
		default:
			break;
	}
	coordinator_walk(op, plan->lefttree, whole);
	coordinator_walk(op, plan->righttree, whole);
}

void
GpOrcaParallelize(PlannedStmt *stmt, Query *query, int cursorOptions)
{
	const char *enabled = GetConfigOption("gp.enable_parallel", true, false);
	bool		on = false;
	OrcaParallel op;

	if (enabled == NULL || !parse_bool(enabled, &on) || !on ||
		max_parallel_workers_per_gather <= 0 || IS_SINGLENODE() ||
		!(cursorOptions & CURSOR_OPT_PARALLEL_OK) || !IsUnderPostmaster ||
		stmt->commandType != CMD_SELECT || stmt->hasModifyingCTE ||
		stmt->rowMarks != NIL || max_parallel_hazard(query) == PROPARALLEL_UNSAFE)
		return;

	op.stmt = stmt;
	op.root = makeNode(PlannerInfo);
	op.root->glob = makeNode(PlannerGlobal);
	op.root->glob->maxParallelHazard = PROPARALLEL_RESTRICTED;
	op.root->query_level = 1;
	op.nsegments = getgpsegmentCount();
	if (op.nsegments < 1)
		op.nsegments = 1;
	op.next_node_id = max_node_id(stmt->planTree) + 1;
	foreach_ptr(Plan, sub, stmt->subplans)
	{
		int			below = max_node_id(sub) + 1;

		op.next_node_id = Max(op.next_node_id, below);
	}

	coordinator_walk(&op, stmt->planTree, true);
}
