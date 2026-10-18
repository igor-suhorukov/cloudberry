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
 *   parallel scan's participants -- heap, ao_row, ao_column, pax, and any
 *   other method with a vexec batch source (shares_scan()) -- made
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
 *   only nodes that read their input whole or pass it on are above one;
 *
 *   NOT IN CLOUDBERRY: a vectorized executor's nodes, which its node
 *   builders put in place of ORCA's before this pass runs (vector.c), taken
 *   for the nodes they stand for, as the engine describes each
 *   (gp_orca_vec.h, describe_node): its sequential scan a scan that can
 *   drive a split subtree, made parallel-aware as a SeqScan is, its hash
 *   join, projection, aggregation and sort as PostgreSQL's -- so that a
 *   plan of vector nodes gets the Gathers ORCA's plan of row nodes would,
 *   and the engine's scans below them share their tables among the
 *   Gather's participants (pg_vector_executor.md §3.10, V4), as many as the
 *   engine gives its scan where it says (engine_workers()).
 *
 * NOT IN CLOUDBERRY, whose ORCA makes no parallel plan on one node
 * (gpdb::IsParallelModeOK()): on one node -- gp_orca's single-node build,
 * or gp_core with no segments -- the backend runs the whole plan, as it
 * runs the planner's, and the plan is the one fragment:
 *
 *   the same Gathers, over the same subtrees, where they pay;
 *
 *   an aggregate ORCA plans in one stage, as it plans every aggregate on
 *   one node, done in two over a subtree a Gather's participants split,
 *   as PostgreSQL's planner splits it: its partial stage in each
 *   participant, below the Gather, and its final stage above it
 *   (two_stage()), each offered to a vectorized executor as the
 *   translator's nodes are (gp_orca_vector_offer()); and the partial stage
 *   of one ORCA planned in two, below a join where ORCA pushed it, inside
 *   a subtree the participants split, its final stage above the Gather
 *   (partial_stage());
 *
 *   below a Limit, through the nodes that pass its rows on, to the first
 *   that reads all its input -- a Sort, a plain or hashed Agg, a hashed
 *   SetOp, a hash join's inner side -- as the coordinator's walk goes down
 *   to a Gather Motion read whole (below_limit()); and the inner side of a
 *   NestLoop that runs once, for an outer side of one row (one_row()), or
 *   that has no parameters of the loop's and is read through a Material,
 *   which keeps its rows for the loop's next outer row;
 *
 *   each Gather with a parameter of its own that a rescan of it changes,
 *   as the planner's has (rescan_param);
 *
 *   the subplans of initplans and of CTEs, which run once, as the nodes
 *   they belong to do; an initplan's value read below a Gather is computed
 *   by the backend and passed to its workers (Gather.initParam), as the
 *   planner's are.
 *
 * Only a SELECT that may run in parallel mode, as PostgreSQL's planner asks
 * (standard_planner()): CURSOR_OPT_PARALLEL_OK, which a cursor lacks, no
 * data-modifying CTE, no row marks, nothing parallel-unsafe anywhere, and
 * not in a parallel worker -- the writer runs its whole fragment in
 * parallel mode.  The coordinator runs no fragment and enters no parallel
 * mode for one (parallelModeNeeded stays false); a segment keeps a
 * fragment's Gathers only in its writer, and only then runs the fragment in
 * parallel mode (gp_parallel.c).  On one node a plan with a Gather needs
 * parallel mode, as the planner's does.  Either way gp.enable_parallel
 * turns it on, Cloudberry's switch of ORCA's parallel plans, which
 * gp_orca's single-node build defines where gp_core does not; and where it
 * is off as a plan made with it on starts -- a cached plan -- the plan runs
 * without its Gathers, on a segment (gp_parallel.c) and on one node
 * (one_node_executor_start()).
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
#include "access/parallel.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cb_compat.h"
#include "gp_core_api.h"
#include "gp_motion.h"
#include "gp_orca_parallel.h"
#include "gp_orca_vec.h"
#include "gp_orca_vector.h"
#include "vexec_source.h"

typedef struct OrcaParallel
{
	PlannedStmt *stmt;
	PlannerInfo *root;			/* for is_parallel_safe() and cost_qual_eval();
								 * on one node, its init_plans the initplans
								 * of the nodes above the walk's */
	int			nsegments;
	int			next_node_id;	/* the plan node id of the next Gather */
	bool		one_node;		/* the whole plan the backend's */
	bool		made;			/* a Gather was made */
} OrcaParallel;

static bool
is_motion(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		strcmp(((CustomScan *) plan)->methods->CustomName, GP_MOTION_NAME) == 0;
}

/*
 * What a node of the vectorized executor registered with gp_orca stands
 * for (gp_orca_vec.h, describe_node, from its minor version 1), filled in
 * *vn; GP_ORCA_VEC_OTHER for any other node, a Motion among them.
 */
static int
vec_kind(Plan *plan, GpOrcaVecNode *vn)
{
	const GpOrcaVecRoutine *engine;

	memset(vn, 0, sizeof(GpOrcaVecNode));
	vn->kind = GP_ORCA_VEC_OTHER;
	if (!IsA(plan, CustomScan) || is_motion(plan))
		return vn->kind;
	engine = gp_orca_vec_find();
	if (engine == NULL || !GP_ORCA_VEC_HAS(engine, describe_node) ||
		!engine->describe_node(plan, vn))
		vn->kind = GP_ORCA_VEC_OTHER;
	return vn->kind;
}

/* A sequential scan: a SeqScan, or a vector engine's. */
static bool
is_seqscan(Plan *plan)
{
	GpOrcaVecNode vn;

	return IsA(plan, SeqScan) ||
		(IsA(plan, CustomScan) && vec_kind(plan, &vn) == GP_ORCA_VEC_SEQSCAN);
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

/*
 * A table whose access method shares a scan among its participants: heap,
 * under any name its handler is given, ao_row, ao_column and pax -- and,
 * NOT IN CLOUDBERRY, any other that has a vexec batch source
 * (vexec_source.h), as ch_table_engines' mergetree has.  vexec's planner
 * route already splits such a table's scan among a Gather's participants
 * (its paths.c, a partial VecScan), and its source reads through the
 * method's own scan, begun parallel-aware as a SeqScan's is
 * (table_beginscan_parallel()); the module that registered it is in
 * "shared_preload_libraries", which a source's registration asks.
 */
static bool
shares_scan(Relation rel)
{
	char	   *name = get_am_name(rel->rd_rel->relam);

	if (rel->rd_tableam == GetHeapamTableAmRoutine() ||
		(name != NULL &&
		 (strcmp(name, "ao_row") == 0 || strcmp(name, "ao_column") == 0 ||
		  strcmp(name, "pax") == 0)))
		return true;
	return rel->rd_tableam != NULL && vexec_find_source(rel->rd_tableam) != NULL;
}

/*
 * What a Gather's participants share: the sequential scan that drives the
 * subtree split among them, its table's share on a segment -- pages and
 * rows, as ANALYZE counted the whole -- and the workers PostgreSQL's rule
 * gives it (compute_parallel_worker()).
 */
typedef struct Share
{
	Scan	   *driver;			/* a SeqScan, or a vector engine's scan */
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

/*
 * The workers a vector engine gives its own scan of rel, the driver of a
 * split subtree, where PostgreSQL's rule gives "workers" (gp_orca_vec.h,
 * scan_workers, from its minor version 3): NOT IN CLOUDBERRY, the engine's
 * rule on PostgreSQL's planner's route, so that ORCA's plan gets as many as
 * the planner's would -- vexec gives a source's scan a worker for every
 * vexec.scan_rows_per_worker rows, where PostgreSQL's rule of a column
 * store's compressed pages gives few.  A SeqScan, and a scan of an engine
 * without the member, keep "workers".
 */
static int
engine_workers(Scan *scan, Relation rel, Share *share, int workers)
{
	const GpOrcaVecRoutine *engine;
	GpOrcaVecNode vn;
	int			answer;

	if (!IsA(scan, CustomScan) || vec_kind(&scan->plan, &vn) != GP_ORCA_VEC_SEQSCAN)
		return workers;
	engine = gp_orca_vec_find();
	if (engine == NULL || !GP_ORCA_VEC_HAS(engine, scan_workers))
		return workers;
	answer = engine->scan_workers(&scan->plan, rel, share->pages, share->tuples,
								  workers);
	if (answer < 0)
		return workers;
	return Min(answer, max_parallel_workers_per_gather);
}

/* A sequential scan that can drive a split subtree, and its share. */
static bool
scan_share(OrcaParallel *op, Scan *scan, Share *share)
{
	RangeTblEntry *rte = rt_fetch(scan->scanrelid, op->stmt->rtable);
	Relation	rel;
	RelOptInfo *dummy;

	if (!worker_reads(op, scan))
		return false;
	rel = table_open(rte->relid, AccessShareLock);
	if (!shares_scan(rel) || rel->rd_rel->reltuples < 0)
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
	share->workers = compute_parallel_worker(dummy, share->pages, -1,
											 max_parallel_workers_per_gather);
	share->workers = engine_workers(scan, rel, share, share->workers);
	table_close(rel, NoLock);
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
	GpOrcaVecNode vn;

	if (plan == NULL)
		return true;
	check_stack_depth();
	if (!node_safe(op, plan))
		return false;
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return worker_reads(op, (Scan *) plan);
		case T_CustomScan:
			/* a vector engine's: its scan, hash join or projection */
			switch (vec_kind(plan, &vn))
			{
				case GP_ORCA_VEC_SEQSCAN:
					return worker_reads(op, (Scan *) plan);
				case GP_ORCA_VEC_HASHJOIN:
				case GP_ORCA_VEC_RESULT:
					if (!is_parallel_safe(op->root, (Node *) vn.exprs))
						return false;
					break;
				default:
					return false;
			}
			break;
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
 * NOT IN CLOUDBERRY.  On one node, ORCA's partial stage of an aggregate
 * (AGGSPLIT_INITIAL_SERIAL), plain or hashed, whose final stage, above any
 * Gather over it, combines the states of a group it is handed, however
 * many participants passed one on -- below a join too, where ORCA's eager
 * aggregation pushed it (CXformPushPartialAggBelowJoin).
 */
static bool
partial_stage(OrcaParallel *op, Agg *agg)
{
	return op->one_node && agg->aggsplit == AGGSPLIT_INITIAL_SERIAL &&
		(agg->aggstrategy == AGG_PLAIN || agg->aggstrategy == AGG_HASHED) &&
		agg->groupingSets == NIL && agg->chain == NIL && agg->plan.qual == NIL;
}

/*
 * Can a subtree be split among a Gather's participants: a sequential scan
 * that shares itself among them, the driver, below the outer sides of hash
 * joins whose inner sides each participant reads whole -- an inner, left,
 * semi or anti join, where an outer row meets every inner row in the one
 * participant that reads it; not a right or full join, whose unmatched
 * inner rows every participant would return -- and projections; on one
 * node, partial stages of aggregates too (partial_stage()).
 */
static bool
splits(OrcaParallel *op, Plan *plan, Share *share)
{
	GpOrcaVecNode vn;

	if (plan == NULL || !node_safe(op, plan))
		return false;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return scan_share(op, (Scan *) plan, share);
		case T_Agg:
			return partial_stage(op, (Agg *) plan) &&
				splits(op, plan->lefttree, share);
		case T_CustomScan:
			/* a vector engine's: its scan, hash join or projection */
			switch (vec_kind(plan, &vn))
			{
				case GP_ORCA_VEC_SEQSCAN:
					return scan_share(op, (Scan *) plan, share);
				case GP_ORCA_VEC_AGG:
					return partial_stage(op, vn.agg) &&
						splits(op, plan->lefttree, share);
				case GP_ORCA_VEC_HASHJOIN:
					if ((vn.jointype != JOIN_INNER && vn.jointype != JOIN_LEFT &&
						 vn.jointype != JOIN_SEMI && vn.jointype != JOIN_ANTI) ||
						!is_parallel_safe(op->root, (Node *) vn.exprs))
						return false;
					return whole_in_each(op, plan->righttree) &&
						splits(op, plan->lefttree, share);
				case GP_ORCA_VEC_RESULT:
					return plan->lefttree != NULL &&
						is_parallel_safe(op->root, (Node *) vn.exprs) &&
						splits(op, plan->lefttree, share);
				default:
					return false;
			}
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
 * participants divide as they divide the scan's: a hash join's probes, and
 * a partial stage's transitions, an operator's cost a column.
 */
static double
split_cpu(Plan *plan)
{
	double		cpu = 0;
	GpOrcaVecNode vn;

	for (; plan != NULL && !is_seqscan(plan); plan = plan->lefttree)
	{
		if (IsA(plan, HashJoin))
			cpu += plan->lefttree->plan_rows *
				(cpu_operator_cost * list_length(((HashJoin *) plan)->hashclauses) +
				 cpu_tuple_cost);
		else if (IsA(plan, Agg))
			cpu += plan->lefttree->plan_rows * cpu_operator_cost *
				list_length(plan->targetlist);
		else if (IsA(plan, CustomScan))
			switch (vec_kind(plan, &vn))
			{
				case GP_ORCA_VEC_HASHJOIN:
					cpu += plan->lefttree->plan_rows *
						(cpu_operator_cost * vn.nhashclauses + cpu_tuple_cost);
					break;
				case GP_ORCA_VEC_AGG:
					cpu += plan->lefttree->plan_rows * cpu_operator_cost *
						list_length(plan->targetlist);
					break;
				default:
					break;
			}
	}
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

	cost_qual_eval(&qual, share->driver->plan.qual, op->root);
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
		if (plan == &share->driver->plan)
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

	/*
	 * On one node, the initplans' values read below it, which the backend
	 * computes and passes to its workers, as the planner's set_param_references()
	 * finds them
	 */
	if (op->one_node && plan->extParam != NULL)
	{
		Bitmapset  *set = NULL;

		foreach_node(SubPlan, init, op->root->init_plans)
			foreach_int(param, init->setParam)
				set = bms_add_member(set, param);
		gather->initParam = bms_intersect(plan->extParam, set);
	}

	/*
	 * On one node, where a node above may run it again, a parameter of its
	 * own that a rescan changes, which its parallel-aware scan and the nodes
	 * above that scan below it depend on, so that they are rescanned as
	 * they run again, once the scan's shared state is reset -- the
	 * planner's Gather's (assign_special_exec_param(), finalize_plan())
	 */
	if (op->one_node)
	{
		gather->rescan_param = list_length(op->stmt->paramExecTypes);
		op->stmt->paramExecTypes = lappend_oid(op->stmt->paramExecTypes, InvalidOid);
		for (Plan *below = plan; below != NULL; below = below->lefttree)
		{
			below->extParam = bms_add_member(bms_copy(below->extParam), gather->rescan_param);
			below->allParam = bms_add_member(bms_copy(below->allParam), gather->rescan_param);
			if (below->parallel_aware)
				break;
		}
	}
	op->made = true;
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
 * lower, which agg is -- or which a vector engine's node, lower, stands for,
 * as it describes it -- over a subtree a Gather's participants split, done
 * in three: that stage
 * in each participant, below the Gather, and above it one more that
 * combines theirs into the segment's, and passes it on serialized, as the
 * first stage did, to the coordinator's last (Cloudberry's three-stage
 * aggregation of a widened gang, in PostgreSQL's terms).  Its groups are
 * the lower one's columns, which it passes on; a plain or hashed aggregate
 * only, the Gather's rows being in no order.
 */
static Plan *
three_stage(OrcaParallel *op, Plan *lower, Agg *agg)
{
	Plan	   *below = lower->lefttree;
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
	lower->parallel_safe = true;
	upper->plan.lefttree = make_gather(op, lower, share.workers);
	return &upper->plan;
}

/* The Aggrefs of an expression, each once (equal()). */
static bool
collect_aggrefs(Node *node, List **aggrefs)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (!list_member(*aggrefs, node))
			*aggrefs = lappend(*aggrefs, node);
		return false;			/* no aggregate is inside another */
	}
	return expression_tree_walker(node, collect_aggrefs, aggrefs);
}

typedef struct FinalStage
{
	Agg		   *agg;			/* ORCA's, in one stage */
	List	   *aggrefs;		/* its Aggrefs, collect_aggrefs()'s */
	List	   *partial;		/* the partial stage's target list: the
								 * keys, then a state for each of aggrefs */
	bool		failed;			/* a column that is no group's key */
} FinalStage;

/*
 * An expression of the aggregate in one stage, made the final stage's over
 * the partial stage's columns: a group's key that stage's column of it, an
 * aggregate the state that stage passes on, combined and finished
 * (PostgreSQL's convert_combining_aggrefs(), and setrefs.c's references to
 * the partial stage's columns).
 */
static Node *
final_mutator(Node *node, FinalStage *fs)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		for (int i = 0; i < fs->agg->numCols; i++)
			if (((Var *) node)->varattno == fs->agg->grpColIdx[i])
				return (Node *) makeVarFromTargetEntry(OUTER_VAR,
													   list_nth_node(TargetEntry, fs->partial, i));
		fs->failed = true;
		return node;
	}
	if (IsA(node, Aggref))
	{
		int			n = 0;

		foreach_node(Aggref, aggref, fs->aggrefs)
		{
			TargetEntry *state;
			Aggref	   *final;

			if (!equal(aggref, node))
			{
				n++;
				continue;
			}
			state = list_nth_node(TargetEntry, fs->partial, fs->agg->numCols + n);
			final = makeNode(Aggref);
			memcpy(final, aggref, sizeof(Aggref));
			final->args = NIL;
			final->aggfilter = NULL;
			final = copyObject(final);
			final->args = list_make1(makeTargetEntry((Expr *) makeVarFromTargetEntry(OUTER_VAR, state),
													 1, NULL, false));
			final->aggsplit = AGGSPLIT_FINAL_DESERIAL;
			return (Node *) final;
		}
		fs->failed = true;
		return node;
	}
	return expression_tree_mutator(node, final_mutator, fs);
}

/*
 * NOT IN CLOUDBERRY.  On one node, an aggregate ORCA planned in one stage
 * (AGGSPLIT_SIMPLE), lower, which agg is -- or which a vector engine's
 * node, lower, stands for -- over a subtree a Gather's participants split,
 * done in two, as PostgreSQL's planner does it (make_partial_grouping_target(),
 * mark_partial_aggref()): a partial stage in each participant, below the
 * Gather, of the groups' keys and each aggregate's state, serialized; and
 * above it the final stage, which combines the participants' states of a
 * group and finishes them, its own target list and qual over them.  A
 * plain or hashed aggregate only, the Gather's rows being in no order.  Each
 * stage is offered to the vector engine, whose node lower may have been.
 */
static Plan *
two_stage(OrcaParallel *op, Plan *lower, Agg *agg)
{
	Plan	   *below = lower->lefttree;
	Share		share;
	FinalStage	fs;
	Agg		   *partial;
	Agg		   *final;
	Plan	   *gathered;
	int			resno = 0;

	if (agg->aggsplit != AGGSPLIT_SIMPLE ||
		(agg->aggstrategy != AGG_PLAIN && agg->aggstrategy != AGG_HASHED) ||
		agg->groupingSets != NIL || agg->chain != NIL || below == NULL ||
		!node_safe(op, &agg->plan))
		return NULL;

	memset(&fs, 0, sizeof(fs));
	fs.agg = agg;
	(void) collect_aggrefs((Node *) agg->plan.targetlist, &fs.aggrefs);
	(void) collect_aggrefs((Node *) agg->plan.qual, &fs.aggrefs);
	foreach_node(Aggref, aggref, fs.aggrefs)
		if (!OidIsValid(aggref->aggtranstype) || !combinable(aggref))
			return NULL;

	/* the partial stage's columns: the keys, then each aggregate's state */
	for (int i = 0; i < agg->numCols; i++)
	{
		TargetEntry *key = get_tle_by_resno(below->targetlist, agg->grpColIdx[i]);

		if (key == NULL)
			return NULL;
		fs.partial = lappend(fs.partial,
							 makeTargetEntry((Expr *) makeVarFromTargetEntry(OUTER_VAR, key),
											 ++resno, NULL, false));
	}
	foreach_node(Aggref, aggref, fs.aggrefs)
	{
		Aggref	   *state = copyObject(aggref);

		state->aggsplit = AGGSPLIT_INITIAL_SERIAL;
		state->aggtype = aggref->aggtranstype == INTERNALOID ? BYTEAOID :
			aggref->aggtranstype;
		fs.partial = lappend(fs.partial, makeTargetEntry((Expr *) state, ++resno, NULL, false));
	}

	final = makeNode(Agg);
	final->plan.targetlist = (List *) final_mutator((Node *) agg->plan.targetlist, &fs);
	final->plan.qual = (List *) final_mutator((Node *) agg->plan.qual, &fs);
	if (fs.failed)
		return NULL;

	/* each participant's groups, which the final stage reads */
	if (!splits(op, below, &share) ||
		!pays(op, &share,
			  split_cpu(below) + below->plan_rows * cpu_operator_cost *
			  (list_length(fs.aggrefs) + agg->numCols),
			  agg->plan.plan_rows * (share.workers + 1)))
		return NULL;
	mark_split(below, &share);

	partial = makeNode(Agg);
	partial->plan.targetlist = fs.partial;
	partial->plan.startup_cost = agg->plan.startup_cost;
	partial->plan.total_cost = agg->plan.total_cost;
	partial->plan.plan_rows = clamp_row_est(Min(agg->plan.plan_rows, below->plan_rows));
	partial->plan.plan_width = agg->plan.plan_width;
	partial->plan.plan_node_id = op->next_node_id++;
	partial->plan.parallel_safe = true;
	partial->plan.extParam = bms_copy(lower->extParam);
	partial->plan.allParam = bms_copy(lower->allParam);
	partial->plan.lefttree = below;
	partial->aggstrategy = agg->aggstrategy;
	partial->aggsplit = AGGSPLIT_INITIAL_SERIAL;
	partial->numCols = agg->numCols;
	partial->grpColIdx = agg->grpColIdx;
	partial->grpOperators = agg->grpOperators;
	partial->grpCollations = agg->grpCollations;
	partial->numGroups = agg->numGroups;
	partial->transitionSpace = agg->transitionSpace;
	partial->aggParams = bms_copy(agg->aggParams);

	final->plan.startup_cost = agg->plan.startup_cost;
	final->plan.total_cost = agg->plan.total_cost;
	final->plan.plan_rows = agg->plan.plan_rows;
	final->plan.plan_width = agg->plan.plan_width;
	final->plan.plan_node_id = lower->plan_node_id;
	final->plan.extParam = bms_copy(lower->extParam);
	final->plan.allParam = bms_copy(lower->allParam);
	final->aggstrategy = agg->aggstrategy;
	final->aggsplit = AGGSPLIT_FINAL_DESERIAL;
	final->numCols = agg->numCols;
	final->grpColIdx = palloc_array(AttrNumber, Max(agg->numCols, 1));
	for (int i = 0; i < agg->numCols; i++)
		final->grpColIdx[i] = i + 1;
	final->grpOperators = agg->grpOperators;
	final->grpCollations = agg->grpCollations;
	final->numGroups = agg->numGroups;
	final->transitionSpace = agg->transitionSpace;

	gathered = gp_orca_vector_offer(&partial->plan, op->stmt->rtable);
	gathered->parallel_safe = true;
	final->plan.lefttree = make_gather(op, gathered, share.workers);
	return gp_orca_vector_offer(&final->plan, op->stmt->rtable);
}

static Plan *fragment_walk(OrcaParallel *op, Plan *plan);
static Plan *node_walk(OrcaParallel *op, Plan *plan);

/*
 * An aggregate's stages over a subtree a Gather's participants split: in
 * three, of ORCA's first stage on a segment (three_stage()), and on one
 * node in two, of an aggregate ORCA planned in one (two_stage()); NULL for
 * neither.
 */
static Plan *
split_stages(OrcaParallel *op, Plan *lower, Agg *agg)
{
	Plan	   *upper = three_stage(op, lower, agg);

	if (upper == NULL && op->one_node)
		upper = two_stage(op, lower, agg);
	return upper;
}

/*
 * NOT IN CLOUDBERRY.  On one node, does a NestLoop's outer side return one
 * row at most, so that its inner side runs once: a plain aggregate, of no
 * grouping sets, or a vector engine's, under projections.
 */
static bool
one_row(Plan *plan)
{
	GpOrcaVecNode vn;

	for (; plan != NULL; plan = plan->lefttree)
	{
		check_stack_depth();
		if (IsA(plan, Agg))
			return ((Agg *) plan)->aggstrategy == AGG_PLAIN &&
				((Agg *) plan)->groupingSets == NIL;
		if (IsA(plan, CustomScan))
		{
			switch (vec_kind(plan, &vn))
			{
				case GP_ORCA_VEC_AGG:
					return vn.agg->aggstrategy == AGG_PLAIN &&
						vn.agg->groupingSets == NIL;
				case GP_ORCA_VEC_RESULT:
					continue;
				default:
					return false;
			}
		}
		if (!IsA(plan, Result) && !IsA(plan, Material))
			return false;
	}
	return false;
}

/*
 * NOT IN CLOUDBERRY.  On one node, below a Limit, which may read a part of
 * what is below it: through the nodes that pass their rows on as they read
 * them, down to the first that reads all its input before it returns a row
 * -- a Sort, a plain or hashed Agg, a hashed SetOp, a hash join's inner
 * side -- whose input the fragment's walk takes, as coordinator_walk() goes
 * down to a Gather Motion read whole.
 */
static Plan *
below_limit(OrcaParallel *op, Plan *plan)
{
	GpOrcaVecNode vn;

	if (plan == NULL)
		return NULL;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_Sort:
			return fragment_walk(op, plan);
		case T_Agg:
			if (((Agg *) plan)->aggstrategy == AGG_PLAIN ||
				((Agg *) plan)->aggstrategy == AGG_HASHED)
				return fragment_walk(op, plan);
			break;
		case T_SetOp:
			if (((SetOp *) plan)->strategy == SETOP_HASHED)
				return fragment_walk(op, plan);
			break;
		case T_HashJoin:
			plan->righttree = fragment_walk(op, plan->righttree);
			break;
		case T_MergeJoin:
			plan->righttree = below_limit(op, plan->righttree);
			break;
		case T_Result:
		case T_Limit:
		case T_Unique:
		case T_Group:
		case T_WindowAgg:
		case T_NestLoop:
			break;
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan =
				below_limit(op, ((SubqueryScan *) plan)->subplan);
			return plan;
		case T_CustomScan:
			/* a vector engine's, as the node it stands for */
			switch (vec_kind(plan, &vn))
			{
				case GP_ORCA_VEC_SORT:
					return fragment_walk(op, plan);
				case GP_ORCA_VEC_AGG:
					if (vn.agg->aggstrategy == AGG_PLAIN ||
						vn.agg->aggstrategy == AGG_HASHED)
						return fragment_walk(op, plan);
					break;
				case GP_ORCA_VEC_HASHJOIN:
					plan->righttree = fragment_walk(op, plan->righttree);
					break;
				case GP_ORCA_VEC_RESULT:
					break;
				default:
					return plan;
			}
			break;
		default:
			return plan;
	}
	plan->lefttree = below_limit(op, plan->lefttree);
	return plan;
}

/*
 * NOT IN CLOUDBERRY.  On one node, the subplans of a node's initplans --
 * a value's, or a CTE's, which every CteScan of it reads -- which run once
 * as the node does, and so are walked as the fragment is; and the node's
 * own walk, with the initplans whose values the backend can compute for a
 * Gather below, all but a CTE's, among those is_parallel_safe() takes for
 * parallel-safe and make_gather() passes on.
 */
static Plan *
initplans_walk(OrcaParallel *op, Plan *plan)
{
	List	   *outer = op->root->init_plans;
	List	   *values = NIL;

	foreach_node(SubPlan, init, plan->initPlan)
	{
		ListCell   *cell;

		if (init->plan_id < 1 || init->plan_id > list_length(op->stmt->subplans))
			continue;
		cell = list_nth_cell(op->stmt->subplans, init->plan_id - 1);
		lfirst(cell) = fragment_walk(op, (Plan *) lfirst(cell));
		if (init->subLinkType != CTE_SUBLINK)
			values = lappend(values, init);
	}
	op->root->init_plans = list_concat_copy(outer, values);
	plan = node_walk(op, plan);
	op->root->init_plans = outer;
	return plan;
}

/*
 * The fragment a writer runs: its scans that a Gather pays over, through
 * the nodes that run once each time it runs -- not a NestLoop's inner side,
 * which runs again for each outer row, not below a Limit, which may read a
 * part of what is below, not into a Motion, whose fragment another process
 * runs, nor any other CustomScan.  On one node, the whole plan, a Limit's
 * input and a NestLoop's inner side as far as they run once too.
 */
static Plan *
fragment_walk(OrcaParallel *op, Plan *plan)
{
	if (plan == NULL)
		return NULL;
	if (op->one_node && plan->initPlan != NIL)
		return initplans_walk(op, plan);
	return node_walk(op, plan);
}

/* A node of the fragment, and what is below it. */
static Plan *
node_walk(OrcaParallel *op, Plan *plan)
{
	Share		share;

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
				Plan	   *upper = split_stages(op, plan, (Agg *) plan);

				if (upper != NULL)
					return upper;
				plan->lefttree = fragment_walk(op, plan->lefttree);
				break;
			}
		case T_CustomScan:
			{
				GpOrcaVecNode vn;

				/* a vector engine's, as the node it stands for */
				switch (vec_kind(plan, &vn))
				{
					case GP_ORCA_VEC_AGG:
						{
							Plan	   *upper = split_stages(op, plan, vn.agg);

							if (upper != NULL)
								return upper;
							plan->lefttree = fragment_walk(op, plan->lefttree);
							break;
						}
					case GP_ORCA_VEC_SORT:
						{
							GpOrcaVecNode child;

							/*
							 * not between the sort and a scan right below
							 * it, whose relation the engine's sort may fetch
							 * the columns past its keys from, by TID, and
							 * finds through it (vexec's late columns)
							 */
							if (plan->lefttree != NULL &&
								vec_kind(plan->lefttree, &child) == GP_ORCA_VEC_SEQSCAN)
								break;
							plan->lefttree = fragment_walk(op, plan->lefttree);
							break;
						}
					case GP_ORCA_VEC_HASHJOIN:
					case GP_ORCA_VEC_RESULT:
						plan->lefttree = fragment_walk(op, plan->lefttree);
						plan->righttree = fragment_walk(op, plan->righttree);
						break;
					default:
						break;
				}
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
			{
				bool		once = op->one_node && one_row(plan->lefttree);

				plan->lefttree = fragment_walk(op, plan->lefttree);

				/*
				 * On one node, an inner side that runs once, for the one row
				 * of the outer side -- ORCA's plan of a comparison with a
				 * scalar subquery's aggregate -- or that has no parameters of
				 * the loop's and is read through a Material, which reads
				 * what is below it once and keeps it for every outer row
				 * after the first (EXEC_FLAG_REWIND, ExecReScanMaterial())
				 */
				if (once)
					plan->righttree = fragment_walk(op, plan->righttree);
				else if (op->one_node && ((NestLoop *) plan)->nestParams == NIL &&
						 plan->righttree != NULL && IsA(plan->righttree, Material) &&
						 plan->righttree->initPlan == NIL)
					plan->righttree->lefttree =
						fragment_walk(op, plan->righttree->lefttree);
				break;
			}
		case T_Limit:
			if (op->one_node)
				plan->lefttree = below_limit(op, plan->lefttree);
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
			{
				GpOrcaVecNode vn;

				/* a vector engine's, as the node it stands for */
				switch (vec_kind(plan, &vn))
				{
					case GP_ORCA_VEC_SORT:
						whole = true;
						break;
					case GP_ORCA_VEC_AGG:
						if (vn.agg->aggstrategy == AGG_PLAIN ||
							vn.agg->aggstrategy == AGG_HASHED)
							whole = true;
						break;
					case GP_ORCA_VEC_HASHJOIN:
					case GP_ORCA_VEC_RESULT:
						break;
					default:
						return;
				}
				break;
			}
		default:
			break;
	}
	coordinator_walk(op, plan->lefttree, whole);
	coordinator_walk(op, plan->righttree, whole);
}

/*
 * Does the session ask for ORCA's parallel plans: gp.enable_parallel --
 * gp_core's, or on one node gp_orca's own -- and PostgreSQL's
 * max_parallel_workers_per_gather above 0, as gp_core's GpParallelEnabled()
 * answers.
 */
static bool
parallel_enabled(void)
{
	const char *enabled = GetConfigOption("gp.enable_parallel", true, false);
	bool		on = false;

	return enabled != NULL && parse_bool(enabled, &on) && on &&
		max_parallel_workers_per_gather > 0;
}

/* ------------------------------------------------------------------------- */
/* On one node, a plan made with Gathers, run without them                   */
/* ------------------------------------------------------------------------- */

/*
 * NOT IN CLOUDBERRY.  On a PlannedStmt, in its extension_state: the pass
 * put Gathers into the plan on one node.
 */
#define GP_ORCA_GATHERS_MARK	"gp_orca_one_node_gathers"

static ExecutorStart_hook_type prev_executor_start = NULL;

static bool
gathers_marked(PlannedStmt *stmt)
{
	foreach_node(DefElem, def, stmt->extension_state)
		if (strcmp(def->defname, GP_ORCA_GATHERS_MARK) == 0)
			return true;
	return false;
}

/*
 * A plan's Gathers taken out: each replaced by what is below it, whose
 * columns a Gather passes on as they are (make_gather()), run whole by the
 * one process -- the scan that drove it no longer parallel-aware, and an
 * aggregate's final stage combining the one partial state below it -- as
 * gp_core's strip_gathers() runs a segment's fragment (gp_parallel.c).
 */
static Plan *
strip_gathers(Plan *plan)
{
	if (plan == NULL)
		return NULL;
	check_stack_depth();
	if (IsA(plan, Gather))
	{
		for (Plan *below = plan->lefttree; below != NULL; below = below->lefttree)
			below->parallel_aware = false;
		return strip_gathers(plan->lefttree);
	}
	plan->lefttree = strip_gathers(plan->lefttree);
	plan->righttree = strip_gathers(plan->righttree);
	switch (nodeTag(plan))
	{
		case T_Append:
			{
				ListCell   *lc;

				foreach(lc, ((Append *) plan)->appendplans)
					lfirst(lc) = strip_gathers((Plan *) lfirst(lc));
				break;
			}
		case T_SubqueryScan:
			((SubqueryScan *) plan)->subplan =
				strip_gathers(((SubqueryScan *) plan)->subplan);
			break;
		default:
			break;
	}
	return plan;
}

/*
 * NOT IN CLOUDBERRY.  A plan the pass gave Gathers on one node, starting
 * where the session asks for no parallel plan -- a prepared statement's or
 * a function's cached plan, gp.enable_parallel turned off since -- runs a
 * copy of it without them and outside parallel mode, as a segment's writer
 * takes a fragment's Gathers out (gp_parallel.c): the setting is the one
 * that turns ORCA's parallel plans off on a cluster.  The cached plan stays
 * as it was made, for when the setting is on again; a parallel worker runs
 * the part it is sent.
 */
static void
one_node_executor_start(QueryDesc *queryDesc, int eflags)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;

	if (stmt != NULL && stmt->parallelModeNeeded && !IsParallelWorker() &&
		gathers_marked(stmt) && !parallel_enabled())
	{
		PlannedStmt *copy = palloc_object(PlannedStmt);
		ListCell   *lc;

		memcpy(copy, stmt, sizeof(PlannedStmt));
		copy->planTree = strip_gathers(copyObject(stmt->planTree));
		copy->subplans = NIL;
		foreach(lc, stmt->subplans)
			copy->subplans = lappend(copy->subplans,
									 strip_gathers(copyObject((Plan *) lfirst(lc))));
		copy->parallelModeNeeded = false;
		queryDesc->plannedstmt = copy;
	}

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
GpOrcaParallelInit(void)
{
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = one_node_executor_start;
}

void
GpOrcaParallelize(PlannedStmt *stmt, Query *query, int cursorOptions)
{
	OrcaParallel op;

	if (!parallel_enabled() ||
		!(cursorOptions & CURSOR_OPT_PARALLEL_OK) || !IsUnderPostmaster ||
		IsParallelWorker() ||
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
	op.one_node = IS_SINGLENODE();
	op.made = false;

	if (!op.one_node)
	{
		coordinator_walk(&op, stmt->planTree, true);
		return;
	}

	/*
	 * NOT IN CLOUDBERRY.  On one node, the whole plan, which the backend runs
	 * in parallel mode where it has a Gather, as the planner's plan; a worker
	 * opens each relation with its range table's lock mode, and asserts that
	 * it holds that lock (execUtils.c), so a relation ORCA left without one
	 * is read in AccessShareLock, which the backend takes, as gp_parallel.c
	 * has a segment's writer take it.
	 */
	stmt->planTree = fragment_walk(&op, stmt->planTree);
	if (op.made)
	{
		foreach_node(RangeTblEntry, rte, stmt->rtable)
			if (rte->rtekind == RTE_RELATION && rte->rellockmode == NoLock)
			{
				rte->rellockmode = AccessShareLock;
				LockRelationOid(rte->relid, AccessShareLock);
			}
		stmt->parallelModeNeeded = true;
		stmt->extension_state =
			lappend(stmt->extension_state,
					makeDefElem(pstrdup(GP_ORCA_GATHERS_MARK),
								(Node *) makeBoolean(true), -1));
	}
}
