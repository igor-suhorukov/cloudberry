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
 * gp_rtfilter.c
 *	  Cloudberry's runtime filters: a Bloom filter of a hash join's inner
 *	  keys, which drops the outer rows no inner row can meet before the join
 *	  reads them -- in a RuntimeFilter node above the join's outer side, and,
 *	  pushed down, in the scans below it.
 *
 * Cloudberry's planner puts a RuntimeFilter node above the outer side of a
 * hash join it expects the filter to thin (try_runtime_filter(),
 * costsize.c): an inner, right or semi join whose keys are passed by value,
 * where the rows the join keeps are fewer than 60% of its outer rows, by
 * 10,000 at least, allowing for the filter's false positives.  Its executor
 * adds each inner row's keys to a Bloom filter as the Hash node reads them,
 * and once the hash table is built the node passes on only the outer rows
 * the filter may hold.  With gp_enable_runtime_filter_pushdown, a hash join
 * whose keys are integer columns also gives each key's Bloom filter and
 * range to the sequential scans below its outer side -- through hash joins,
 * Results and Appends -- which drop the rows they rule out before anything
 * above them reads those, and EXPLAIN ANALYZE says how many.
 *
 * Here the node is a CustomScan, which a planner_shutdown_hook puts above
 * the outer side of each hash join of the planner's plan that Cloudberry's
 * rule takes, applied to the finished plan's row estimates; ORCA's plans
 * have none, as Cloudberry's ORCA makes none.  Its filter holds the join's
 * own hash values of the inner rows' keys, as the Hash node computes them,
 * and the node tests the hash value the join computes of each outer row:
 * a row whose value no inner row has cannot meet one, whatever the keys'
 * types.  PostgreSQL's Hash node reads its input itself, so the values are
 * taken by a wrapper of that input's ExecProcNode (ExecSetExecProcNode()).
 * Pushdown is set up as the executor starts, as Cloudberry's hash join sets
 * it up: the scans it reaches are sequential scans, and the planner's
 * gathers, which on the coordinator are what reads a distributed table --
 * Cloudberry's scan below its Gather Motion -- and a wrapper of each tests
 * the rows it returns, after its own conditions, where Cloudberry's scan
 * tests them before.
 *
 * A filter drops only what cannot change an answer.  It works below an
 * inner, right or semi join, whose outer rows that meet no inner row give
 * nothing, and never below an outer join's preserved side; only once the
 * Hash node has read all of its input for the hash table the join probes,
 * never while it is reading it; never on a NULL key, which the join passes
 * over itself; and pushed down only for an equality of two integer columns,
 * through nodes that pass each row on, or not, by itself.  A join with a
 * filter builds its hash table before it reads its outer side, as
 * Cloudberry's ORCA has every hash join do (prefetch_inner), so that the
 * filter is there for the first outer row.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/executor/nodeRuntimeFilter.c; the runtime filter parts of
 *	  nodeHashjoin.c (CreateRuntimeFilter(), FindTargetNodes()), nodeHash.c
 *	  (AddTupleValuesIntoRF(), PushdownRuntimeFilter()), nodeSeqscan.c
 *	  (PassByBloomFilter()) and explain.c; try_runtime_filter() of
 *	  optimizer/path/costsize.c and create_runtime_filter_path() of
 *	  pathnode.c; bloom_create_aggresive() of lib/bloomfilter.c; and the
 *	  settings of src/backend/utils/misc/guc_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "gp_rtfilter.h"
#include "gp_scan.h"

/* Cloudberry's gp_enable_runtime_filter and gp_enable_runtime_filter_pushdown */
static bool gp_enable_runtime_filter = false;
static bool gp_enable_runtime_filter_pushdown = false;

/*
 * Cloudberry's rule (costsize.c): a filter is worth it where it drops 40% of
 * the outer rows at least, and 10,000 of them.
 */
#define RTF_RATE_THRESHOLD	0.6
#define RTF_MIN_DROPPED		10000

/* What a plan with a RuntimeFilter node says of itself, in extension_state. */
#define RTF_MARK			"gp_runtime_filter"

static planner_shutdown_hook_type prev_planner_shutdown = NULL;
static ExecutorStart_hook_type prev_executor_start = NULL;
static explain_per_node_hook_type prev_explain_per_node = NULL;
static explain_node_label_hook_type prev_explain_node_label = NULL;

/* ------------------------------------------------------------------------- */
/* The Bloom filter                                                          */
/* ------------------------------------------------------------------------- */

/*
 * PostgreSQL's lib/bloomfilter.c keeps its filter's size to itself and
 * starts at 1MB; Cloudberry's runtime filters are sized as its
 * bloom_create_aggresive() sizes them, and hold 64-bit values -- a key's
 * integer, or the join's 32-bit hash value of the keys -- which are mixed
 * once, and set k bits by PostgreSQL's enhanced double hashing.
 */
typedef struct RtfBloom
{
	uint64		nbits;			/* a power of two */
	int			k;				/* the bits a value sets: 2 or 3 */
	uint64		limit;			/* the values it takes, at 1.6 bits each */
	uint64		nvalues;		/* the values added since it was cleared */
	unsigned char bits[FLEXIBLE_ARRAY_MEMBER];
} RtfBloom;

/*
 * As bloom_create_aggresive(): 128kB to 2MB, no more than work_mem, for a
 * false positive rate near 10%; and none where the rows the plan expects
 * would have fewer than 1.6 bits each.
 */
static RtfBloom *
bloom_make(double rows)
{
	double		bytes;
	uint64		nbits = 1;
	double		per_value;
	RtfBloom   *bloom;

	rows = Max(rows, 1.0);
	bytes = Min((double) work_mem * 1024.0, rows * 9.0 / 8.0);
	bytes = Max(bytes, 128.0 * 1024.0);
	bytes = Min(bytes, 2.0 * 1024.0 * 1024.0);
	while ((double) (nbits * 2) <= bytes * BITS_PER_BYTE)
		nbits *= 2;

	per_value = (double) nbits / rows;
	if (per_value < 1.6)
		return NULL;

	bloom = palloc0(offsetof(RtfBloom, bits) + nbits / BITS_PER_BYTE);
	bloom->nbits = nbits;
	bloom->k = per_value >= 3.5 ? 3 : 2;
	bloom->limit = (uint64) (nbits / 1.6);
	return bloom;
}

static void
bloom_clear(RtfBloom *bloom)
{
	if (bloom->nvalues > 0)
		memset(bloom->bits, 0, bloom->nbits / BITS_PER_BYTE);
	bloom->nvalues = 0;
}

/*
 * Past its limit a filter takes no more values, and is no longer asked: its
 * false positives would be most of what it is asked.  As Cloudberry's
 * RuntimeFilter suspends itself past its inner_threshold.
 */
static inline bool
bloom_full(const RtfBloom *bloom)
{
	return bloom->nvalues > bloom->limit;
}

/* The first bit a value sets, and the step to the next. */
static inline void
bloom_start(const RtfBloom *bloom, uint64 value, uint32 *x, uint32 *y)
{
	uint64		hash = murmurhash64(value);

	*x = (uint32) (hash & (bloom->nbits - 1));
	*y = (uint32) ((hash >> 32) & (bloom->nbits - 1));
}

static void
bloom_add(RtfBloom *bloom, uint64 value)
{
	uint32		mask = (uint32) (bloom->nbits - 1);
	uint32		x;
	uint32		y;

	if (bloom_full(bloom))
		return;
	bloom->nvalues++;
	bloom_start(bloom, value, &x, &y);
	for (int i = 0; i < bloom->k; i++)
	{
		if (i > 0)
		{
			x = (x + y) & mask;
			y = (y + i) & mask;
		}
		bloom->bits[x >> 3] |= 1 << (x & 7);
	}
}

/* Is the value certainly not one the filter was given? */
static bool
bloom_lacks(const RtfBloom *bloom, uint64 value)
{
	uint32		mask = (uint32) (bloom->nbits - 1);
	uint32		x;
	uint32		y;

	bloom_start(bloom, value, &x, &y);
	for (int i = 0; i < bloom->k; i++)
	{
		if (i > 0)
		{
			x = (x + y) & mask;
			y = (y + i) & mask;
		}
		if (!(bloom->bits[x >> 3] & (1 << (x & 7))))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* A hash join's filters, and the scans they reach                           */
/* ------------------------------------------------------------------------- */

typedef struct RtfBuild RtfBuild;

/*
 * A key pushed down: an equality of two integer columns among the join's
 * hash conditions, Cloudberry's AttrFilter -- its inner values, and their
 * range, which the scans below the outer side test their rows' values by.
 */
typedef struct RtfKey
{
	RtfBuild   *build;
	AttrNumber	col;			/* its column of the Hash node's input */
	Oid			type;			/* of that column: int2, int4 or int8 */
	RtfBloom   *bloom;			/* NULL where the plan expects too many rows */
	int64		min;			/* the values' range; min > max for none */
	int64		max;
} RtfKey;

/*
 * A hash join's filters, and the pass of its Hash node's input that fills
 * them.  A pass starts as the Hash node asks its input for its first row and
 * ends as the input has no more: then the filters hold every inner row of
 * the hash table the join has just made, and are good for as long as the
 * join probes that one.  A rescan that keeps the table keeps them; one that
 * makes a new table empties them again.
 */
struct RtfBuild
{
	HashJoinState *join;
	HashState  *hash;
	double		rows;			/* the inner rows the plan expects */
	bool		sized;			/* have the filters been made? */
	bool		want_hashes;	/* a RuntimeFilter node reads the hash values */
	RtfBloom   *hashes;			/* the join's hash values of the inner keys */
	ExprContext *econtext;		/* where they are computed */
	List	   *keys;			/* RtfKey: the ones pushed down */
	bool		in_pass;		/* the Hash node is reading its input */
	bool		done;			/* ... has read all of it, */
	HashJoinTable table;		/* for this hash table */
};

/* A key, as a scan tests it: the key's column of the scan's rows. */
typedef struct RtfCheck
{
	RtfKey	   *key;
	AttrNumber	col;
	Oid			type;
} RtfCheck;

/* A scan the filters reach: Cloudberry's SeqScanState.filters. */
typedef struct RtfTarget
{
	List	   *checks;			/* RtfCheck */
	bool		worked;			/* a filter was ready for a row, prf_work */
} RtfTarget;

/*
 * A node whose ExecProcNode is wrapped: the Hash node input that fills a
 * join's filters, or a scan they reach.  Found by the node's address in a
 * table of the process's, which holds the nodes of every statement whose
 * executor has started and not yet ended, each statement's taken out as its
 * memory goes.
 */
typedef struct RtfNode
{
	PlanState  *ps;				/* the key */
	ExecProcNodeMtd real;		/* its own ExecProcNode */
	RtfBuild   *build;
	RtfTarget  *target;
} RtfNode;

/* A statement's wrapped nodes. */
typedef struct RtfQuery
{
	List	   *nodes;
	MemoryContextCallback forget;
} RtfQuery;

static HTAB *rtf_nodes = NULL;
static RtfNode *rtf_last = NULL;	/* the last one found */

static RtfNode *
rtf_find(PlanState *ps)
{
	if (rtf_last != NULL && rtf_last->ps == ps)
		return rtf_last;
	if (rtf_nodes == NULL)
		return NULL;
	rtf_last = hash_search(rtf_nodes, &ps, HASH_FIND, NULL);
	return rtf_last;
}

/* A statement's memory is going: its nodes with it. */
static void
rtf_forget(void *arg)
{
	RtfQuery   *query = (RtfQuery *) arg;

	foreach_ptr(PlanState, ps, query->nodes)
		(void) hash_search(rtf_nodes, &ps, HASH_REMOVE, NULL);
	rtf_last = NULL;
}

/* A node of the statement, its ExecProcNode wrapped with `wrapper`. */
static RtfNode *
rtf_register(RtfQuery *query, EState *estate, PlanState *ps,
			 ExecProcNodeMtd wrapper)
{
	RtfNode    *node;

	if (rtf_nodes == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(PlanState *);
		ctl.entrysize = sizeof(RtfNode);
		ctl.hcxt = TopMemoryContext;
		rtf_nodes = hash_create("gp_core runtime filters", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* the statement's nodes are forgotten with its memory, after an error too */
	if (query->nodes == NIL)
	{
		query->forget.func = rtf_forget;
		query->forget.arg = query;
		MemoryContextRegisterResetCallback(estate->es_query_cxt,
										   &query->forget);
	}

	node = hash_search(rtf_nodes, &ps, HASH_ENTER, NULL);
	node->ps = ps;
	node->real = ps->ExecProcNodeReal;
	node->build = NULL;
	node->target = NULL;
	query->nodes = lappend(query->nodes, ps);
	rtf_last = NULL;

	ExecSetExecProcNode(ps, wrapper);
	return node;
}

static RtfNode *
rtf_node_of(PlanState *ps)
{
	RtfNode    *node = rtf_find(ps);

	if (node == NULL)
		elog(ERROR, "a runtime filter's node is not known");
	return node;
}

/* Are the join's filters complete, for the hash table it probes now? */
static inline bool
rtf_ready(const RtfBuild *build)
{
	return build->done && build->join->hj_HashTable != NULL &&
		build->join->hj_HashTable == build->table;
}

static int64
rtf_int(Datum value, Oid type)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(value);
		case INT4OID:
			return DatumGetInt32(value);
		default:
			return DatumGetInt64(value);
	}
}

/*
 * A pass begins: the filters emptied, and made the first time, for the rows
 * the plan expects of the Hash node.
 */
static void
rtf_begin_pass(RtfBuild *build)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(build->join->js.ps.state->es_query_cxt);

	if (!build->sized)
	{
		if (build->want_hashes)
			build->hashes = bloom_make(build->rows);
		foreach_ptr(RtfKey, key, build->keys)
			key->bloom = bloom_make(build->rows);
		build->sized = true;
	}
	MemoryContextSwitchTo(oldcxt);

	if (build->hashes != NULL)
		bloom_clear(build->hashes);
	foreach_ptr(RtfKey, key, build->keys)
	{
		if (key->bloom != NULL)
			bloom_clear(key->bloom);
		key->min = PG_INT64_MAX;
		key->max = PG_INT64_MIN;
	}
	build->in_pass = true;
	build->done = false;
	build->table = build->hash->hashtable;
}

/* An inner row: its hash value, as the Hash node computes it, and its keys. */
static void
rtf_add_row(RtfBuild *build, TupleTableSlot *slot)
{
	if (build->hashes != NULL && !bloom_full(build->hashes))
	{
		ExprContext *econtext = build->econtext;
		Datum		value;
		bool		isnull;

		econtext->ecxt_outertuple = slot;
		ResetExprContext(econtext);
		value = ExecEvalExprSwitchContext(build->hash->hash_expr, econtext,
										  &isnull);
		/* a NULL key, which a strict operator's join never meets */
		if (!isnull)
			bloom_add(build->hashes, DatumGetUInt32(value));
	}

	foreach_ptr(RtfKey, key, build->keys)
	{
		bool		isnull;
		Datum		value = slot_getattr(slot, key->col, &isnull);
		int64		v;

		if (isnull)
			continue;
		v = rtf_int(value, key->type);
		key->min = Min(key->min, v);
		key->max = Max(key->max, v);
		if (key->bloom != NULL)
			bloom_add(key->bloom, (uint64) v);
	}
}

/*
 * The Hash node's input, wrapped: each row it gives the Hash node, and its
 * end, which is the end of the pass.
 */
static TupleTableSlot *
rtf_input_exec(PlanState *ps)
{
	RtfNode    *node = rtf_node_of(ps);
	RtfBuild   *build = node->build;
	TupleTableSlot *slot;

	if (!build->in_pass)
		rtf_begin_pass(build);
	slot = node->real(ps);
	if (TupIsNull(slot))
	{
		build->in_pass = false;
		build->done = true;
	}
	else
		rtf_add_row(build, slot);
	return slot;
}

/*
 * Does the row pass the filters that reach the scan?  A key's filter is
 * asked once its join has read all of its inner side, and a NULL passes.
 */
static bool
rtf_passes(RtfTarget *target, TupleTableSlot *slot)
{
	foreach_ptr(RtfCheck, check, target->checks)
	{
		RtfKey	   *key = check->key;
		Datum		value;
		bool		isnull;
		int64		v;

		if (!rtf_ready(key->build))
			continue;
		target->worked = true;

		value = slot_getattr(slot, check->col, &isnull);
		if (isnull)
			continue;
		v = rtf_int(value, check->type);
		if (v < key->min || v > key->max)
			return false;
		if (key->bloom != NULL && !bloom_full(key->bloom) &&
			bloom_lacks(key->bloom, (uint64) v))
			return false;
	}
	return true;
}

/*
 * A scan the filters reach, wrapped: the rows they rule out are counted
 * where a scan's second filter count is, which PostgreSQL's sequential scans
 * and the port's gathers leave unused -- Cloudberry counts them in a field
 * of its own, nfilteredPRF -- so that it travels with the node's other
 * counters.
 */
static TupleTableSlot *
rtf_target_exec(PlanState *ps)
{
	RtfNode    *node = rtf_node_of(ps);

	for (;;)
	{
		TupleTableSlot *slot = node->real(ps);

		if (TupIsNull(slot) || rtf_passes(node->target, slot))
			return slot;
		InstrCountFiltered2(ps, 1);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ------------------------------------------------------------------------- */
/* The RuntimeFilter node                                                    */
/* ------------------------------------------------------------------------- */

typedef struct RtfState
{
	CustomScanState css;
	RtfBuild   *build;			/* its join's; NULL, and every row passes */
} RtfState;

static Node *rtf_create_state(CustomScan *cscan);
static void rtf_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *rtf_exec(CustomScanState *node);
static void rtf_end(CustomScanState *node);
static void rtf_rescan(CustomScanState *node);
static void rtf_explain(CustomScanState *node, List *ancestors,
						ExplainState *es);

static const CustomScanMethods rtf_scan_methods = {
	.CustomName = "RuntimeFilter",
	.CreateCustomScanState = rtf_create_state,
};

static const CustomExecMethods rtf_exec_methods = {
	.CustomName = "RuntimeFilter",
	.BeginCustomScan = rtf_begin,
	.ExecCustomScan = rtf_exec,
	.EndCustomScan = rtf_end,
	.ReScanCustomScan = rtf_rescan,
	.ExplainCustomScan = rtf_explain,
};

static bool
rtf_is_node(PlanState *ps)
{
	return IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &rtf_exec_methods;
}

static Node *
rtf_create_state(CustomScan *cscan)
{
	RtfState   *state = (RtfState *) newNode(sizeof(RtfState),
											 T_CustomScanState);

	state->css.methods = &rtf_exec_methods;
	return (Node *) state;
}

static void
rtf_begin(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);

	/*
	 * Its rows are its input's, in the slots they come in, as Cloudberry's
	 * RuntimeFilter's are: the join compiles its expressions for them.
	 */
	node->ss.ps.resultops = ExecGetResultSlotOps(outerPlanState(node),
												 &node->ss.ps.resultopsfixed);
	node->ss.ps.resultopsset = true;
}

/*
 * The outer rows, less those whose hash value -- the join's own, of the
 * row's keys -- no inner row of the hash table the join probes has.  As
 * Cloudberry's ExecRuntimeFilter(), every row passes until the filter is
 * complete, and a row with a NULL key passes.
 */
static TupleTableSlot *
rtf_exec(CustomScanState *node)
{
	RtfState   *state = (RtfState *) node;
	RtfBuild   *build = state->build;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(outerPlanState(node));
		Datum		value;
		bool		isnull;

		if (TupIsNull(slot) || build == NULL || build->hashes == NULL ||
			bloom_full(build->hashes) || !rtf_ready(build))
			return slot;

		econtext->ecxt_outertuple = slot;
		ResetExprContext(econtext);
		value = ExecEvalExprSwitchContext(build->join->hj_OuterHash, econtext,
										  &isnull);
		if (isnull || !bloom_lacks(build->hashes, DatumGetUInt32(value)))
			return slot;
		CHECK_FOR_INTERRUPTS();
	}
}

static void
rtf_end(CustomScanState *node)
{
	ExecEndNode(outerPlanState(node));
}

static void
rtf_rescan(CustomScanState *node)
{
	/* a changed parameter rescans the input as it is next read */
	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

/* Cloudberry's show_runtime_filter_info(): the filter's size, as it ran. */
static void
rtf_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	RtfState   *state = (RtfState *) node;

	if (es->analyze && state->build != NULL && state->build->hashes != NULL)
		ExplainPropertyUInteger("Bloom Bits", NULL,
								state->build->hashes->nbits, es);
}

/* ------------------------------------------------------------------------- */
/* Planning: where Cloudberry's planner would put one                        */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's runtime_filter_fp_rate_estimate(): the false positive rate a
 * filter of that many inner rows will have, at most 2MB of it.
 */
static double
rtf_false_positive_rate(double inner_rows)
{
	const double max_filter_bits = 16 * 1024 * 1024;

	if (inner_rows > max_filter_bits / 1.6)
		return 1;
	if (inner_rows > max_filter_bits / 2)
		return 0.4;
	if (inner_rows > max_filter_bits / 2.5)
		return 0.3;
	return 0.1;
}

/*
 * Would Cloudberry's planner filter this join's outer side, and to how many
 * rows?  Its try_runtime_filter(), over the finished plan's estimates: the
 * rows the join keeps are the join's own, which count its other conditions
 * as well -- and a right join's unmatched inner rows, which makes it choose
 * one less often -- where Cloudberry estimates the hash conditions' alone.
 * A right semi join, PostgreSQL 19's, is a semi join whose outer rows are
 * the ones probed, and is taken as one.
 */
static bool
rtf_worth(HashJoin *join, double *rows)
{
	Plan	   *outer = outerPlan(join);
	Plan	   *inner = innerPlan(join);
	double		fp_rate;
	double		kept;
	double		passed;

	if (join->join.jointype != JOIN_INNER &&
		join->join.jointype != JOIN_RIGHT &&
		join->join.jointype != JOIN_SEMI &&
		join->join.jointype != JOIN_RIGHT_SEMI)
		return false;
	if (join->join.plan.parallel_aware || inner->parallel_aware)
		return false;

	/* by-value keys on both sides, as Cloudberry's */
	foreach_ptr(Node, clause, join->hashclauses)
	{
		Oid			left;
		Oid			right;

		if (!IsA(clause, OpExpr))
			return false;
		op_input_types(((OpExpr *) clause)->opno, &left, &right);
		if (!get_typbyval(left) || !get_typbyval(right))
			return false;
	}
	if (contain_volatile_functions((Node *) join->hashclauses))
		return false;

	fp_rate = rtf_false_positive_rate(inner->plan_rows);
	if (fp_rate > 0.5)
		return false;

	kept = Min(join->join.plan.plan_rows, outer->plan_rows);
	if (outer->plan_rows - kept < RTF_MIN_DROPPED)
		return false;
	passed = kept + (outer->plan_rows - kept) * fp_rate;
	if (!(passed < outer->plan_rows * RTF_RATE_THRESHOLD))
		return false;

	*rows = passed;
	return true;
}

/*
 * The RuntimeFilter node above `child`, as the plan has it after
 * set_plan_references(): its scan tuple is the child's row (OUTER_VAR), and
 * its target list passes it on as it is (INDEX_VAR), as a Motion's does.
 * Its costs are Cloudberry's create_runtime_filter_path()'s, the hash
 * conditions taken at an operator's cost each.
 */
static Plan *
rtf_make_node(Plan *child, PlannerGlobal *glob, double rows, int nkeys)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *plan = &cscan->scan.plan;
	List	   *scan_tlist = NIL;
	List	   *tlist = NIL;

	foreach_node(TargetEntry, tle, child->targetlist)
	{
		Oid			type = exprType((Node *) tle->expr);
		int32		typmod = exprTypmod((Node *) tle->expr);
		Oid			collation = exprCollation((Node *) tle->expr);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
															  type, typmod,
															  collation, 0),
											 tle->resno, tle->resname,
											 tle->resjunk));
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(INDEX_VAR, tle->resno,
														 type, typmod,
														 collation, 0),
										tle->resno, tle->resname,
										tle->resjunk));
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = child;
	plan->righttree = NULL;
	plan->startup_cost = child->startup_cost;
	plan->total_cost = child->total_cost +
		nkeys * cpu_operator_cost / 2 * child->plan_rows +
		cpu_tuple_cost * rows;
	plan->plan_rows = rows;
	plan->plan_width = child->plan_width;
	plan->disabled_nodes = child->disabled_nodes;
	plan->parallel_aware = false;
	plan->parallel_safe = child->parallel_safe;
	plan->async_capable = false;
	plan->plan_node_id = glob->lastPlanNodeId++;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(child->extParam);
	plan->allParam = bms_copy(child->allParam);

	cscan->scan.scanrelid = 0;
	cscan->flags = 0;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_relids = NULL;
	cscan->methods = &rtf_scan_methods;
	return (Plan *) cscan;
}

/* Each hash join of the tree that Cloudberry's rule takes, filtered. */
static bool
rtf_add_filters(Plan *plan, PlannerGlobal *glob)
{
	bool		added = false;
	double		rows;

	if (plan == NULL)
		return false;
	check_stack_depth();

	added |= rtf_add_filters(plan->lefttree, glob);
	added |= rtf_add_filters(plan->righttree, glob);
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				added |= rtf_add_filters(child, glob);
			break;
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				added |= rtf_add_filters(child, glob);
			break;
		case T_SubqueryScan:
			added |= rtf_add_filters(((SubqueryScan *) plan)->subplan, glob);
			break;
		case T_CustomScan:
			foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
				added |= rtf_add_filters(child, glob);
			break;
		default:
			break;
	}

	if (IsA(plan, HashJoin) && rtf_worth((HashJoin *) plan, &rows))
	{
		plan->lefttree = rtf_make_node(plan->lefttree, glob, rows,
									   list_length(((HashJoin *) plan)->hashclauses));
		added = true;
	}
	return added;
}

/*
 * The planner's plan, finished: with gp.enable_runtime_filter on, a
 * RuntimeFilter node above the outer side of each hash join Cloudberry's
 * planner would give one (hash_inner_and_outer(), joinpath.c), and the plan
 * marked as having them, which tells the executor to look for them.
 */
static void
rtf_planner_shutdown(PlannerGlobal *glob, Query *parse,
					 const char *query_string, PlannedStmt *pstmt)
{
	bool		added;

	if (prev_planner_shutdown)
		prev_planner_shutdown(glob, parse, query_string, pstmt);

	if (!gp_enable_runtime_filter || pstmt->commandType == CMD_UTILITY)
		return;

	added = rtf_add_filters(pstmt->planTree, glob);
	foreach_ptr(Plan, sub, pstmt->subplans)
		added |= rtf_add_filters(sub, glob);
	if (added)
		pstmt->extension_state = lappend(pstmt->extension_state,
										 makeDefElem(RTF_MARK, NULL, -1));
}

/* ------------------------------------------------------------------------- */
/* The executor's start: filters and the scans they reach                    */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's IsEqualOp(): the equality of two integers, the ones whose
 * values its filters hold as they are.
 */
static bool
rtf_int_equality(OpExpr *op)
{
	Oid			func = OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno);

	switch (func)
	{
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_INT24EQ:
		case F_INT42EQ:
		case F_INT28EQ:
		case F_INT82EQ:
		case F_INT48EQ:
		case F_INT84EQ:
			return true;
		default:
			return false;
	}
}

/* The Var of the node's output column `col`, if it is one of `varno`'s. */
static Var *
rtf_column_var(List *tlist, AttrNumber col, int varno)
{
	TargetEntry *tle = get_tle_by_resno(tlist, col);

	if (tle == NULL || !IsA(tle->expr, Var) ||
		((Var *) tle->expr)->varno != varno ||
		((Var *) tle->expr)->varattno < 1)
		return NULL;
	return (Var *) tle->expr;
}

/* Is the node's output column `col` of type `type`? */
static bool
rtf_column_is(PlanState *ps, AttrNumber col, Oid type)
{
	TupleDesc	desc = ps->ps_ResultTupleDesc;

	return desc != NULL && col >= 1 && col <= desc->natts &&
		TupleDescAttr(desc, col - 1)->atttypid == type;
}

/* A key the scan tests, at its column `col`. */
static void
rtf_add_check(RtfQuery *query, PlanState *ps, RtfKey *key, AttrNumber col,
			  Oid type, int *ntargets)
{
	RtfNode    *node = rtf_find(ps);
	RtfCheck   *check;

	if (!rtf_column_is(ps, col, type))
		return;
	if (node == NULL)
	{
		node = rtf_register(query, ps->state, ps, rtf_target_exec);
		node->target = palloc0_object(RtfTarget);
	}
	else if (node->target == NULL)
		return;

	check = palloc_object(RtfCheck);
	check->key = key;
	check->col = col;
	check->type = type;
	node->target->checks = lappend(node->target->checks, check);
	(*ntargets)++;
}

/*
 * The scans below a join's outer side that its key reaches: Cloudberry's
 * FindTargetNodes(), which follows the key's column down through hash
 * joins' outer sides, Results and Appends to sequential scans -- and here
 * also through the port's own nodes that pass a row on, or not, by itself:
 * a RuntimeFilter, ORCA's Dynamic Scan, which is an Append of the scans of
 * a table's partitions, and the Result with hash filters ORCA puts above a
 * replicated table.  A gather is a scan here, as its label says.  A row
 * such a node passes on keeps its key's value; one it drops gives nothing,
 * so a row the scan drops is one the join would have met no inner row
 * with, and a lower join's row made of it, NULL where the key is, meets
 * none either.  A Motion is where the key's rows leave for another slice,
 * and the search stops there, as Cloudberry's does.
 */
static void
rtf_find_targets(RtfQuery *query, RtfKey *key, PlanState *ps, AttrNumber col,
				 Oid type, int *ntargets)
{
	int			slice;
	int			nsegs;

	for (;;)
	{
		Var		   *var;

		check_stack_depth();
		if (ps == NULL)
			return;

		if (IsA(ps, SeqScanState) || GpGatherScanSlice(ps, &slice, &nsegs))
		{
			rtf_add_check(query, ps, key, col, type, ntargets);
			return;
		}

		switch (nodeTag(ps))
		{
			case T_HashJoinState:
			case T_ResultState:
				var = rtf_column_var(ps->plan->targetlist, col, OUTER_VAR);
				if (var == NULL)
					return;
				col = var->varattno;
				ps = outerPlanState(ps);
				continue;

			case T_AppendState:
				{
					AppendState *append = (AppendState *) ps;

					var = rtf_column_var(ps->plan->targetlist, col, OUTER_VAR);
					if (var == NULL)
						return;
					for (int i = 0; i < append->as_nplans; i++)
						rtf_find_targets(query, key, append->appendplans[i],
										 var->varattno, type, ntargets);
					return;
				}

			case T_CustomScanState:
				{
					CustomScanState *css = (CustomScanState *) ps;
					const char *name = css->methods->CustomName;

					var = rtf_column_var(ps->plan->targetlist, col, INDEX_VAR);
					if (var == NULL)
						return;

					/* its rows are its scans', as they come */
					if (strcmp(name, "Dynamic Scan") == 0)
					{
						if (var->varattno != col)
							return;
						foreach_ptr(PlanState, child, css->custom_ps)
							rtf_find_targets(query, key, child, col, type,
											 ntargets);
						return;
					}

					/* its scan tuple is its input's row */
					if (rtf_is_node(ps) || strcmp(name, "GpHashFilter") == 0)
					{
						var = rtf_column_var(((CustomScan *) ps->plan)->custom_scan_tlist,
											 var->varattno, OUTER_VAR);
						if (var == NULL)
							return;
						col = var->varattno;
						ps = outerPlanState(ps);
						continue;
					}
					return;
				}

			default:
				return;
		}
	}
}

/*
 * Cloudberry's CreateRuntimeFilter(): each of the join's hash conditions
 * that is an equality of two integer columns, a key, with the scans it
 * reaches below the outer side; a key that reaches none is dropped.
 */
static void
rtf_pushdown_keys(RtfQuery *query, RtfBuild *build)
{
	HashJoin   *plan = (HashJoin *) build->join->js.ps.plan;
	PlanState  *input = outerPlanState(build->hash);

	foreach_ptr(Node, clause, plan->hashclauses)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Var		   *outer_var = NULL;
		Var		   *inner_var = NULL;
		RtfKey	   *key;
		int			ntargets = 0;

		if (!IsA(clause, OpExpr) || list_length(op->args) != 2 ||
			!rtf_int_equality(op))
			continue;
		foreach_ptr(Node, arg, op->args)
		{
			if (!IsA(arg, Var) || ((Var *) arg)->varattno < 1)
				break;
			if (((Var *) arg)->varno == OUTER_VAR)
				outer_var = (Var *) arg;
			else if (((Var *) arg)->varno == INNER_VAR)
				inner_var = (Var *) arg;
		}
		if (outer_var == NULL || inner_var == NULL ||
			!rtf_column_is(input, inner_var->varattno, inner_var->vartype))
			continue;

		key = palloc0_object(RtfKey);
		key->build = build;
		key->col = inner_var->varattno;
		key->type = inner_var->vartype;
		key->min = PG_INT64_MAX;
		key->max = PG_INT64_MIN;
		rtf_find_targets(query, key, outerPlanState(build->join),
						 outer_var->varattno, outer_var->vartype, &ntargets);
		if (ntargets > 0)
			build->keys = lappend(build->keys, key);
		else
			pfree(key);
	}
}

/*
 * A hash join's filters: the RuntimeFilter node's, where the planner put
 * one above its outer side, and the keys pushed down, where
 * gp.enable_runtime_filter_pushdown is on; the Hash node's input wrapped to
 * fill them.  An inner, right or semi join's only, and not a parallel one's,
 * whose hash table is filled by several processes, each seeing some of its
 * rows -- which Cloudberry's filters are suspended for.
 */
static void
rtf_setup_join(RtfQuery *query, HashJoinState *join)
{
	JoinType	type = join->js.jointype;
	HashState  *hash = (HashState *) innerPlanState(join);
	PlanState  *input;
	RtfNode    *node;
	RtfBuild   *build;
	RtfState   *filter = NULL;

	if (type != JOIN_INNER && type != JOIN_RIGHT && type != JOIN_SEMI &&
		type != JOIN_RIGHT_SEMI)
		return;
	if (hash == NULL || !IsA(hash, HashState) ||
		join->js.ps.plan->parallel_aware || hash->ps.plan->parallel_aware)
		return;
	input = outerPlanState(hash);
	/* a subplan's tree is met once for each place that runs it */
	if (input == NULL || rtf_find(input) != NULL)
		return;

	build = palloc0_object(RtfBuild);
	build->join = join;
	build->hash = hash;
	build->rows = hash->ps.plan->plan_rows;

	if (rtf_is_node(outerPlanState(join)))
		filter = (RtfState *) outerPlanState(join);
	if (gp_enable_runtime_filter_pushdown)
		rtf_pushdown_keys(query, build);
	if (filter == NULL && build->keys == NIL)
	{
		pfree(build);
		return;
	}

	if (filter != NULL)
	{
		build->want_hashes = true;
		build->econtext = CreateExprContext(join->js.ps.state);
		filter->build = build;
	}
	node = rtf_register(query, join->js.ps.state, input, rtf_input_exec);
	node->build = build;

	/*
	 * The hash table first, then the outer side, as Cloudberry's ORCA has
	 * every hash join do: PostgreSQL's reads an outer row first where the
	 * outer side's start-up cost is below the Hash node's total, to skip
	 * the hash table if the outer side is empty -- unless it already knows
	 * it is not, which only this heuristic reads.
	 */
	join->hj_OuterNotEmpty = true;
}

static bool
rtf_walker(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;
	if (IsA(ps, HashJoinState))
		rtf_setup_join((RtfQuery *) context, (HashJoinState *) ps);
	return planstate_tree_walker(ps, rtf_walker, context);
}

static bool
rtf_marked(PlannedStmt *stmt)
{
	foreach_node(DefElem, def, stmt->extension_state)
	{
		if (strcmp(def->defname, RTF_MARK) == 0)
			return true;
	}
	return false;
}

/*
 * A statement's filters, as its executor has made its nodes: where the plan
 * has RuntimeFilter nodes, or keys can be pushed down.  The subplans are
 * walked on their own too, a CTE's among them, which no node of the tree
 * leads to.  EXPLAIN without ANALYZE runs nothing, and needs none.
 */
static void
rtf_executor_start(QueryDesc *queryDesc, int eflags)
{
	EState	   *estate;
	MemoryContext oldcxt;
	RtfQuery   *query;

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) || queryDesc->planstate == NULL)
		return;
	if (!gp_enable_runtime_filter_pushdown &&
		!rtf_marked(queryDesc->plannedstmt))
		return;

	estate = queryDesc->estate;
	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	query = palloc0_object(RtfQuery);
	(void) rtf_walker(queryDesc->planstate, query);
	foreach_ptr(PlanState, sub, estate->es_subplanstates)
		(void) rtf_walker(sub, query);
	MemoryContextSwitchTo(oldcxt);
}

/* ------------------------------------------------------------------------- */
/* EXPLAIN                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's show_pushdown_runtime_filter_info(): under ANALYZE, with
 * pushdown on, the rows the filters removed from a scan they reached and
 * worked in, 0 among them -- or that a scan's counters say they removed,
 * where they worked in another process.  After the node's own lines, which
 * is where this hook prints, where Cloudberry's line comes before its
 * Filter.
 */
static void
rtf_explain_per_node(PlanState *planstate, List *ancestors,
					 const char *relationship, const char *plan_name,
					 ExplainState *es)
{
	RtfNode    *node;
	int			slice;
	int			nsegs;

	if (prev_explain_per_node)
		prev_explain_per_node(planstate, ancestors, relationship, plan_name,
							  es);

	if (!es->analyze || !gp_enable_runtime_filter_pushdown ||
		planstate->instrument == NULL)
		return;
	if (!IsA(planstate, SeqScanState) &&
		!GpGatherScanSlice(planstate, &slice, &nsegs))
		return;

	node = rtf_find(planstate);
	if ((node != NULL && node->target != NULL && node->target->worked) ||
		planstate->instrument->nfiltered2 > 0)
		ExplainPropertyFloat("Rows Removed by Pushdown Runtime Filter", NULL,
							 planstate->instrument->nfiltered2, 0, es);
}

/* The node's name in EXPLAIN, Cloudberry's, through O4. */
static void
rtf_explain_label(PlanState *planstate, ExplainState *es, const char **pname,
				  const char **suffix)
{
	if (rtf_is_node(planstate))
	{
		*pname = "RuntimeFilter";
		return;
	}
	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpRtFilterInit(void)
{
	/*
	 * Cloudberry's gp_enable_runtime_filter has no description; this says
	 * what it does.
	 */
	DefineCustomBoolVariable("gp.enable_runtime_filter",
							 "Enables the planner's use of runtime filters in hash join plans.",
							 "Cloudberry calls this gp_enable_runtime_filter.",
							 &gp_enable_runtime_filter,
							 false, PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.enable_runtime_filter_pushdown",
							 "Try to push the hash table of hash join to the seqscan or AM as bloom filter.",
							 "Cloudberry calls this gp_enable_runtime_filter_pushdown.",
							 &gp_enable_runtime_filter_pushdown,
							 false, PGC_USERSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* a plan with one travels to a parallel worker */
	RegisterCustomScanMethods(&rtf_scan_methods);

	prev_planner_shutdown = planner_shutdown_hook;
	planner_shutdown_hook = rtf_planner_shutdown;
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = rtf_executor_start;
	prev_explain_per_node = explain_per_node_hook;
	explain_per_node_hook = rtf_explain_per_node;
	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = rtf_explain_label;
}
