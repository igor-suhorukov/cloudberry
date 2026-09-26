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
 * gp_scan.c
 *	  Reading a distributed table: the gather-all shape.
 *
 * A distributed table's rows are on the segments, and its copy on the
 * coordinator is empty.  When PostgreSQL's planner plans a query -- ORCA
 * declined it, or is off -- every scan of such a table becomes a Gather
 * Motion of the table from the segments: the plan the planner made runs on
 * the coordinator, over the rows gathered.  That is the gather-all fallback of
 * Route A in the plan (Track A §2.2, gpdb_hook.md): correct for every query
 * PostgreSQL can plan, and as slow as moving every row to one node is.  ORCA's
 * own plans, with Motions where it chooses, are the fast path, and this is
 * what a query gets when it is not on it.
 *
 * What it does not move: the columns the plan does not use; and the rows the
 * plan would throw away, when the condition that throws them away can be
 * evaluated on a segment -- one of this table's columns, its system columns,
 * constants, and nothing whose answer could differ there: no parameter, no
 * subquery, no function that is not immutable.  Such a condition is sent as
 * SQL, deparsed by PostgreSQL's own ruleutils.
 *
 * What a row brings with it besides its columns.  The node's scan tuple is
 * its own (custom_scan_tlist), not the table's: the columns the plan uses,
 * and of the table's system columns the ones it names -- ctid, xmin, xmax,
 * cmin and cmax as the segment that holds the row has them, as Cloudberry's
 * QE sends them up, and tableoid; the whole row, where the plan reads it;
 * and gp_segment_id, the segment the row came from, which is how a randomly
 * distributed table's is known here at all (gp_segment.c).  The row's ctid
 * is its scan tuple's TID too, which is what WHERE CURRENT OF reads.
 *
 * And the segments it does not ask: a hash-distributed table whose
 * conditions fix every column of its key to constants -- one each, or a few
 * through IN lists and ORs -- has all the rows they can match on the
 * segments those constants hash to, which are the only ones asked:
 * Cloudberry's direct dispatch, "PARTIAL contents" when several.  So do
 * conditions on gp_segment_id.  A replicated table's rows are all on every
 * segment, so one is asked, a different one per session.  A partial table's
 * rows are on the first so many segments its policy names, and only those
 * are asked.
 *
 * WHERE CURRENT OF a cursor, whose plan gathers the table: the cursor's
 * gather says which segment its current row came from and where it is
 * there, and the one row is read from that segment (gather_current_of()),
 * as Cloudberry's QD sends the cursor's position to the QEs.
 *
 * Cloudberry sources this file stands in for:
 *	  the Gather Motion over a scan that cdbllize.c and cdbpath.c put above a
 *	  distributed table, cdbtargeteddispatch.c and predtest_valueset.c, and
 *	  the position of a cursor's row that execCurrent.c reads
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufpage.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_segment.h"
#include "gp_settings.h"

/* What starting a gather costs before its first row: a round trip per segment. */
#define GATHER_STARTUP_COST		1000.0
/*
 * And per row, moving it: a send, a receive, and the conversion between --
 * unless gp.motion_cost_per_row says otherwise, as Cloudberry's planner lets
 * it say of a Motion.
 */
#define GATHER_ROW_COST \
	(gp_motion_cost_per_row > 0 ? gp_motion_cost_per_row : 10.0 * DEFAULT_CPU_TUPLE_COST)

/*
 * Where a column of a gather's scan tuple comes from: a column of what the
 * segments send (0 and up), or one of these, made here.
 */
#define GATHER_SRC_TABLEOID		(-1)	/* the relation's OID */
#define GATHER_SRC_WHOLEROW		(-2)	/* the row, as its table's row type */
#define GATHER_SRC_SEGMENT		(-3)	/* the segment it came from */
#define GATHER_SRC_IDENTITY		(-4)	/* its ctid as a write's plan knows it */

/* custom_private, in order */
#define GATHER_PRIVATE_SELECT		0	/* "SELECT ... FROM ONLY t" */
#define GATHER_PRIVATE_WHERE		1	/* the conditions sent, ANDed, or "" */
#define GATHER_PRIVATE_LOCKING		2	/* " FOR UPDATE ...", or "" */
#define GATHER_PRIVATE_CONTENTS		3	/* direct dispatch's segments, or NIL */
#define GATHER_PRIVATE_NSEGMENTS	4	/* the table's segments: 0 for every one */
#define GATHER_PRIVATE_TYPES		5	/* what the segments send: each type */
#define GATHER_PRIVATE_TYPMODS		6	/* and typmod */
#define GATHER_PRIVATE_SOURCES		7	/* each scan column's source */
#define GATHER_PRIVATE_ATTRS		8	/* each attribute's column sent, or -1 */
#define GATHER_PRIVATE_CTID			9	/* the column sent that is ctid, or -1 */
#define GATHER_PRIVATE_CURSOR		10	/* WHERE CURRENT OF: its cursor, or "" */
#define GATHER_PRIVATE_CURSOR_PARAM 11	/* or the parameter naming it, or 0 */
#define GATHER_PRIVATE_IDENTITY		12	/* the rows of a table being changed */
#define GATHER_PRIVATE_LIMIT		13	/* " LIMIT n" a LIMIT above sends, or "" */

static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static build_simple_rel_hook_type prev_build_simple_rel = NULL;

static Plan *gather_plan(PlannerInfo *root, RelOptInfo *rel,
						 CustomPath *best_path, List *tlist,
						 List *clauses, List *custom_plans);
static Node *gather_create_state(CustomScan *cscan);
static void gather_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *gather_exec(CustomScanState *node);
static void gather_end(CustomScanState *node);
static void gather_rescan(CustomScanState *node);
static void gather_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomPathMethods gather_path_methods = {
	.CustomName = "Gather Motion",
	.PlanCustomPath = gather_plan,
};

static const CustomScanMethods gather_scan_methods = {
	.CustomName = "Gather Motion",
	.CreateCustomScanState = gather_create_state,
};

static const CustomExecMethods gather_exec_methods = {
	.CustomName = "Gather Motion",
	.BeginCustomScan = gather_begin,
	.ExecCustomScan = gather_exec,
	.EndCustomScan = gather_end,
	.ReScanCustomScan = gather_rescan,
	.ExplainCustomScan = gather_explain,
};

typedef struct GatherScanState
{
	CustomScanState css;
	char	   *select;			/* "SELECT ... FROM ONLY t" */
	char	   *where;			/* the conditions sent, or "" */
	char	   *locking;		/* " FOR UPDATE ...", or "" */
	int			ncontents;		/* direct dispatch's segments, or 0 */
	int		   *contents;
	int			nsegments;		/* the table's: every one, or a partial
								 * table's first so many */
	TupleTableSlot *remote;		/* a row as it arrives */
	int			nsources;		/* the scan tuple's columns, and each one's */
	int		   *sources;		/* source: a column of the remote row, or
								 * GATHER_SRC_* */
	int			natts;			/* the relation's attributes, and each one's */
	int		   *attrs;			/* column in the remote row, or -1 */
	int			ctid_remote;	/* the remote row's ctid, or -1 */
	char	   *cursor_name;	/* WHERE CURRENT OF this cursor */
	int			cursor_param;	/* or the one this parameter names */
	bool		identity;		/* the rows of a table being changed */
	Datum	   *rowvalues;		/* the whole row's values, being built */
	bool	   *rownulls;
	char	   *limit;			/* " LIMIT n", or "" */
	GpGatherState *gather;
	bool		done;
	int			current_content;	/* the segment of the scan tuple's row */
	bool		external;		/* an external table's */
	Tuplestorestate *spool;		/* what it read, when it may be read again */
	TupleTableSlot *spooled;	/* a row of it, read back */
} GatherScanState;

/*
 * The planning in progress is a cursor's (see GpScanSetCursor()): its
 * gathers bring each row's ctid, for WHERE CURRENT OF to find it by.
 */
static bool planning_cursor = false;

/* ------------------------------------------------------------------------- */
/* Which relations                                                           */
/* ------------------------------------------------------------------------- */

GpPolicy *
GpScanDistributedPolicy(Oid relid)
{
	GpPolicy   *policy;

	if (GpClusterIsSingleNode())
		return NULL;

	/*
	 * A query inside a statement the coordinator dispatches whole reads and
	 * writes the coordinator's copy: each segment runs it on its own
	 * (GpDispatchIsRecording()).
	 */
	if (GpDispatchIsRecording())
		return NULL;
	if (get_rel_relkind(relid) != RELKIND_RELATION &&
		get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE &&
		get_rel_relkind(relid) != RELKIND_MATVIEW &&
		!GpPolicyIsExternalTable(relid))
		return NULL;

	policy = GpPolicyGet(relid);
	if (policy == NULL || GpPolicyIsEntry(policy))
		return NULL;
	return policy;
}

int
GpScanReplicatedContent(const GpPolicy *policy)
{
	/*
	 * Every segment of the policy has every row; spread the sessions over
	 * them.  A partial table's are the first numsegments.
	 */
	return MyProcPid % policy->numsegments;
}

/* ------------------------------------------------------------------------- */
/* What can be sent                                                          */
/* ------------------------------------------------------------------------- */

typedef struct ShippableContext
{
	Index		relid;
} ShippableContext;

static bool
shippable_walker(Node *node, ShippableContext *cxt)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				/*
				 * This table's own columns and its system columns, which a
				 * segment has as the row it sends up has them; not the whole
				 * row, which ruleutils would name by a name the segment's
				 * statement does not give it.
				 */
				if (var->varno != cxt->relid || var->varlevelsup != 0 ||
					var->varattno == 0)
					return true;
				return false;
			}
		case T_Param:
		case T_SubLink:
		case T_SubPlan:
		case T_AlternativeSubPlan:
		case T_Aggref:
		case T_WindowFunc:
		case T_GroupingFunc:
		case T_PlaceHolderVar:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
			return true;
		default:
			break;
	}

	return expression_tree_walker(node, shippable_walker, cxt);
}

/*
 * gp_segment_id of this table, taken out of a condition before it is judged:
 * it differs between nodes, but a segment's answer is the right one -- the
 * segment that holds the row -- and a random table's is the only one.
 */
static Node *
without_segment_id(Node *node, Index *relid)
{
	if (node == NULL)
		return NULL;
	if (GpSegmentIsSegmentOf(node, *relid))
		return (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
								  Int32GetDatum(0), false, true);
	return expression_tree_mutator(node, without_segment_id, relid);
}

/*
 * Can this condition be evaluated on a segment and mean the same there?
 * Nothing whose answer could differ between nodes: only this table's columns,
 * no parameter or subquery, and no function that is not immutable -- now()
 * is a different instant on each node.  gp_segment_id is the exception.
 */
static bool
is_shippable(Expr *expr, Index relid)
{
	ShippableContext cxt = {.relid = relid};

	expr = (Expr *) without_segment_id((Node *) expr, &relid);
	if (contain_mutable_functions((Node *) expr))
		return false;
	return !shippable_walker((Node *) expr, &cxt);
}

/* ------------------------------------------------------------------------- */
/* The values a column can have: Cloudberry's PossibleValueSet               */
/* ------------------------------------------------------------------------- */

/*
 * What the conditions allow a column -- one of the key's, or gp_segment_id --
 * to be: any value, or only these constants.  Kept as Cloudberry keeps them,
 * in a hash table of the constants' bytes (predtest_valueset.c), so that the
 * segments they hash to come out in its order, which is the order its INFO
 * line names them in.
 */
typedef struct ValueSet
{
	bool		any;			/* any value is possible */
	HTAB	   *set;			/* else these, of ValueEntry */
} ValueSet;

typedef struct ValueEntry
{
	Const	   *c;
} ValueEntry;

/* The column: a key column's Var, or else gp_segment_id of varno's row. */
typedef struct ValueTarget
{
	AttrNumber	attno;			/* 0 for gp_segment_id */
	Index		varno;
	Oid			opfamily;		/* the hash family whose equality counts */
} ValueTarget;

/* The size of an array Cloudberry takes apart, as PostgreSQL's predtest.c */
#define VALUESET_MAX_ARRAY	100

static uint32
value_hash(const void *key, Size keysize)
{
	const Const *c = *((Const *const *) key);

	if (c->constisnull)
		return 0;
	if (c->constbyval)
		return hash_any((const unsigned char *) &c->constvalue, sizeof(Datum));
	return hash_any((const unsigned char *) DatumGetPointer(c->constvalue),
					datumGetSize(c->constvalue, c->constbyval, c->constlen));
}

static int
value_match(const void *key1, const void *key2, Size keysize)
{
	return equal(*((Node *const *) key1), *((Node *const *) key2)) ? 0 : 1;
}

static void
valueset_add(ValueSet *vs, Const *c)
{
	bool		found;
	ValueEntry *entry;

	if (vs->set == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Const *);
		ctl.entrysize = sizeof(ValueEntry);
		ctl.hash = value_hash;
		ctl.match = value_match;
		ctl.hcxt = CurrentMemoryContext;
		vs->set = hash_create("gp direct dispatch values", 16, &ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
							  HASH_CONTEXT);
	}
	vs->any = false;
	(void) hash_search(vs->set, &c, HASH_FIND, &found);
	if (!found)
	{
		Const	   *copy = copyObject(c);

		entry = hash_search(vs->set, &copy, HASH_ENTER, &found);
		entry->c = copy;
	}
}

static bool
valueset_contains(ValueSet *vs, Const *c)
{
	bool		found = false;

	if (vs->set != NULL)
		(void) hash_search(vs->set, &c, HASH_FIND, &found);
	return found;
}

/* The constants, in the hash table's order. */
static List *
valueset_values(ValueSet *vs)
{
	HASH_SEQ_STATUS status;
	ValueEntry *entry;
	List	   *result = NIL;

	if (vs->set == NULL)
		return NIL;
	hash_seq_init(&status, vs->set);
	while ((entry = (ValueEntry *) hash_seq_search(&status)) != NULL)
		result = lappend(result, entry->c);
	return result;
}

/* Is this the column the set is about? */
static bool
value_target_is(Node *node, ValueTarget *target)
{
	if (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (target->attno == 0)
		return GpSegmentIsSegmentOf(node, target->varno);
	return node != NULL && IsA(node, Var) &&
		((Var *) node)->varno == target->varno &&
		((Var *) node)->varattno == target->attno &&
		((Var *) node)->varlevelsup == 0;
}

/* An equality of the column and a constant: the constant it may be. */
static bool
value_atom(Node *clause, ValueTarget *target, ValueSet *result)
{
	result->any = true;
	result->set = NULL;

	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left = strip_implicit_coercions(linitial(op->args));
		Node	   *right = strip_implicit_coercions(lsecond(op->args));
		Node	   *column;
		Const	   *con;

		if (IsA(right, Const))
		{
			column = left;
			con = (Const *) right;
		}
		else if (IsA(left, Const))
		{
			column = right;
			con = (Const *) left;
		}
		else
			return false;
		if (con->constisnull || !value_target_is(column, target))
			return false;

		/*
		 * An equality the key's hash family hashes for: a constant of another
		 * of its types hashes with its own type's function as the column's
		 * value it equals would (GpHashSegmentForKey).
		 */
		if (get_op_opfamily_strategy(op->opno, target->opfamily) !=
			HTEqualStrategyNumber)
			return false;
		valueset_add(result, con);
		return true;
	}

	if (IsA(clause, NullTest) &&
		((NullTest *) clause)->nulltesttype == IS_NULL &&
		!((NullTest *) clause)->argisrow &&
		value_target_is((Node *) ((NullTest *) clause)->arg, target))
	{
		Node	   *arg = (Node *) ((NullTest *) clause)->arg;

		valueset_add(result, makeNullConst(exprType(arg), -1,
										   exprCollation(arg)));
		return true;
	}

	return false;
}

static ValueSet possible_values(Node *clause, ValueTarget *target);

/* AND: each member narrows the set; OR: each widens it. */
static ValueSet
possible_values_of(List *args, bool and, ValueTarget *target)
{
	ValueSet	result = {.any = true,.set = NULL};

	foreach_ptr(Node, arg, args)
	{
		ValueSet	child = possible_values(arg, target);

		if (and)
		{
			if (child.any)
				continue;
			if (result.any)
			{
				result = child;
				continue;
			}
			/* the intersection */
			foreach_ptr(Const, c, valueset_values(&result))
				if (!valueset_contains(&child, c))
					(void) hash_search(result.set, &c, HASH_REMOVE, NULL);
		}
		else
		{
			if (child.any)
				return child;	/* any value, whatever the rest say */
			if (result.any)
			{
				result = child;
				continue;
			}
			/* the union */
			foreach_ptr(Const, c, valueset_values(&child))
				valueset_add(&result, c);
		}
	}
	return result;
}

/* A ScalarArrayOpExpr, taken apart into its elements' comparisons. */
static ValueSet
possible_values_of_array(ScalarArrayOpExpr *saop, ValueTarget *target)
{
	ValueSet	any = {.any = true,.set = NULL};
	Node	   *array = (Node *) lsecond(saop->args);
	List	   *comparisons = NIL;

	if (IsA(array, Const) && !((Const *) array)->constisnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(((Const *) array)->constvalue);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) > VALUESET_MAX_ARRAY)
			return any;
		get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			Const	   *c = makeConst(ARR_ELEMTYPE(arr), -1,
									  ((Const *) array)->constcollid, elmlen,
									  elems[i], nulls[i], elmbyval);

			comparisons = lappend(comparisons,
								  make_opclause(saop->opno, BOOLOID, false,
												linitial(saop->args),
												(Expr *) c, InvalidOid,
												saop->inputcollid));
		}
	}
	else if (IsA(array, ArrayExpr) && !((ArrayExpr *) array)->multidims &&
			 list_length(((ArrayExpr *) array)->elements) <= VALUESET_MAX_ARRAY)
	{
		foreach_ptr(Expr, elem, ((ArrayExpr *) array)->elements)
			comparisons = lappend(comparisons,
								  make_opclause(saop->opno, BOOLOID, false,
												linitial(saop->args), elem,
												InvalidOid, saop->inputcollid));
	}
	else
		return any;

	return possible_values_of(comparisons, !saop->useOr, target);
}

static ValueSet
possible_values(Node *clause, ValueTarget *target)
{
	ValueSet	result = {.any = true,.set = NULL};

	if (clause == NULL)
		return result;
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (IsA(clause, List))
		return possible_values_of((List *) clause, true, target);
	if (is_andclause(clause))
		return possible_values_of(((BoolExpr *) clause)->args, true, target);
	if (is_orclause(clause))
		return possible_values_of(((BoolExpr *) clause)->args, false, target);
	if (IsA(clause, ScalarArrayOpExpr))
		return possible_values_of_array((ScalarArrayOpExpr *) clause, target);
	if (value_atom(clause, target, &result))
		return result;
	result.any = true;
	return result;
}

/* ------------------------------------------------------------------------- */
/* Which segments                                                            */
/* ------------------------------------------------------------------------- */

/* The hash family of gp_segment_id's type, int4: integer_ops. */
static Oid
segment_id_opfamily(void)
{
	Oid			opclass = GetDefaultOpClass(INT4OID, HASH_AM_OID);

	return OidIsValid(opclass) ? get_opclass_family(opclass) : InvalidOid;
}

/*
 * The segments that hold every row the conditions can match, in Cloudberry's
 * order, or NIL where that is every segment of the table -- as
 * GetContentIdsFromPlanForSingleRelation() works them out
 * (cdbtargeteddispatch.c).  First gp_segment_id, which a condition may fix
 * to a few segments of a hashed or a random table; then a hashed table's
 * key, each combination of the values its columns may take hashed to its
 * segment, as long as there are fewer than three per segment.  Conditions
 * no row can meet name segment 0 alone, where the query finds nothing, as
 * Cloudberry's cdbllize.c has it.
 */
static List *
dispatch_contents(GpPolicy *policy, List *quals, Index relid)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	List	   *result = NIL;
	ValueTarget target;
	ValueSet	vs;

	if (!gp_enable_direct_dispatch || quals == NIL ||
		!(GpPolicyIsHashPartitioned(policy) ||
		  GpPolicyIsRandomPartitioned(policy)))
		return NIL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "gp direct dispatch",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* gp_segment_id = 1, gp_segment_id IN (0, 2) */
	target.attno = 0;
	target.varno = relid;
	target.opfamily = segment_id_opfamily();
	vs = possible_values((Node *) quals, &target);
	if (!vs.any)
	{
		List	   *values = valueset_values(&vs);
		bool		ok = values != NIL && list_length(values) < policy->numsegments;
		List	   *contents = NIL;

		foreach_ptr(Const, c, values)
		{
			int64		content;

			if (c->constisnull)
				continue;
			switch (c->consttype)
			{
				case INT2OID:
					content = DatumGetInt16(c->constvalue);
					break;
				case INT4OID:
					content = DatumGetInt32(c->constvalue);
					break;
				case INT8OID:
					content = DatumGetInt64(c->constvalue);
					break;
				default:
					ok = false;
					continue;
			}
			/* a segment the table has, or none: a row of no other is there */
			if (content >= 0 && content < policy->numsegments)
				contents = list_append_unique_int(contents, (int) content);
		}
		if (ok)
		{
			MemoryContextSwitchTo(oldcxt);
			result = list_copy(contents);
			MemoryContextDelete(cxt);
			return result != NIL ? result : list_make1_int(0);
		}
	}

	if (GpPolicyIsHashPartitioned(policy))
	{
		int			nattrs = policy->nattrs;
		List	  **values = palloc0_array(List *, nattrs);
		long		combinations = 1;

		for (int k = 0; k < nattrs && combinations > 0; k++)
		{
			target.attno = policy->attrs[k];
			target.varno = relid;
			target.opfamily = get_opclass_family(policy->opclasses[k]);
			vs = possible_values((Node *) quals, &target);
			if (vs.any)
				combinations = -1;
			else
			{
				values[k] = valueset_values(&vs);
				combinations *= list_length(values[k]);
			}
		}

		if (combinations == 0)
			result = list_make1_int(-1);	/* no row can match */
		else if (combinations > 0 && combinations < policy->numsegments * 3)
		{
			Oid		   *types = palloc_array(Oid, nattrs);
			Datum	   *datums = palloc_array(Datum, nattrs);
			bool	   *isnull = palloc_array(bool, nattrs);

			for (long index = 0; index < combinations; index++)
			{
				long		cur = index;
				int			content;

				for (int k = 0; k < nattrs; k++)
				{
					int			n = list_length(values[k]);
					Const	   *c = (Const *) list_nth(values[k], cur % n);

					types[k] = c->consttype;
					datums[k] = c->constvalue;
					isnull[k] = c->constisnull;
					cur /= n;
				}
				content = GpHashSegmentForKey(policy, types, datums, isnull);
				if (content < 0)
				{
					result = NIL;	/* a type the family cannot hash */
					break;
				}
				result = list_append_unique_int(result, content);
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);
	if (result != NIL && linitial_int(result) == -1)
		result = list_make1_int(0);
	else
		result = list_copy(result);
	MemoryContextDelete(cxt);

	return result;
}

/*
 * The segments an UPDATE or DELETE sent as it stands can go to: its WHERE
 * fixes the target's key, or its gp_segment_id.  varno is the target's range
 * table index in the Query the conditions are from.
 */
List *
GpScanDirectDispatchContents(Oid relid, Node *quals, Index varno)
{
	GpPolicy   *policy = GpScanDistributedPolicy(relid);

	if (policy == NULL || quals == NULL)
		return NIL;
	return dispatch_contents(policy, make_ands_implicit((Expr *) quals), varno);
}

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The size of a distributed table.  The planner scales pg_class's reltuples
 * by the pages the table has now, and the coordinator's copy has none, so a
 * table ANALYZE has counted would be estimated at no rows at all.  What
 * ANALYZE wrote is the size across the segments (gp_analyze.c), and is taken
 * as it stands.
 *
 * A table never analyzed keeps the planner's own guess, which for a heap
 * table is PostgreSQL's for one never vacuumed: ten pages of rows as wide as
 * its columns (table_block_relation_estimate_size()).  An append-optimized
 * or PAX table's method counts its own files instead, of which the
 * coordinator has none, and says no rows: a join of two such tables would
 * be a nested loop that runs its inner scan on the segments again for each
 * outer row.  It is guessed at as a heap table is, as Cloudberry guesses at
 * every distributed table alike (cdb_estimate_rel_size()): not through
 * table_block_relation_estimate_size() itself, which reads a fillfactor
 * from options that are the method's own.
 */
static void
gp_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	HeapTuple	tuple;
	Form_pg_class classForm;

	if (prev_build_simple_rel)
		prev_build_simple_rel(root, rel, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
		return;
	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	if (!HeapTupleIsValid(tuple))
		return;
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	if (classForm->relpages > 0 && classForm->reltuples >= 0)
	{
		rel->pages = (BlockNumber) classForm->relpages;
		rel->tuples = classForm->reltuples;
		rel->allvisfrac = Min(1.0, (double) classForm->relallvisible /
							  classForm->relpages);
	}
	else if (classForm->reltuples < 0 && !classForm->relhassubclass &&
			 rel->tuples <= 0)
	{
		int32		width;

		width = get_relation_data_width(rte->relid,
										rel->attr_widths - rel->min_attr);
		width += MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData);
		rel->pages = 10;
		/* integer division, as the heap's */
		rel->tuples = rint(clamp_row_est((BLCKSZ - SizeOfPageHeaderData) / width) *
						   rel->pages);
		rel->allvisfrac = 0;
	}
	ReleaseSysCache(tuple);
}

static Node *find_segment_of(Node *tree, Index relid);

/* The relations a NOT IN's subquery was noticed of, in this planning. */
static List *noticed_relids = NIL;

/* What a condition reads of the table: its ctid, and its gp_segment_id. */
typedef struct CtidInventory
{
	Index		relid;
	bool		uses_ctid;
	bool		uses_segid;
} CtidInventory;

static bool
ctid_inventory_walker(Node *node, CtidInventory *inv)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == inv->relid && var->varlevelsup == 0 &&
			var->varattno == SelfItemPointerAttributeNumber)
			inv->uses_ctid = true;
		return false;
	}
	if (GpSegmentIsSegmentOf(node, inv->relid))
	{
		inv->uses_segid = true;
		return false;
	}
	return expression_tree_walker(node, ctid_inventory_walker, inv);
}

/*
 * A distributed table's ctid is a place on one segment, and names a row only
 * with gp_segment_id beside it.  A query whose conditions -- its own or a
 * join's, not what it returns -- read a ctid without it is told so, and an
 * UPDATE or DELETE is refused: Cloudberry's
 * cdbmutate_warn_ctid_without_segid(), in its words.
 */
static void
warn_ctid_without_segment_id(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte)
{
	CtidInventory inv = {.relid = rel->relid};
	Relids		needed;
	const char *cmd;
	int			elevel;

	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;

	/* ctid needed above the scan, but not only by the final target list */
	needed = rel->attr_needed[SelfItemPointerAttributeNumber - rel->min_attr];
	if (bms_nonempty_difference(needed, bms_make_singleton(0)))
		inv.uses_ctid = true;
	foreach_node(RestrictInfo, ri, rel->joininfo)
		(void) ctid_inventory_walker((Node *) ri->clause, &inv);
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		(void) ctid_inventory_walker((Node *) ri->clause, &inv);

	/* a join's equality is an equivalence class, not a condition of either */
	foreach_node(EquivalenceClass, ec, root->eq_classes)
		foreach_node(EquivalenceMember, em, ec->ec_members)
			if (find_segment_of((Node *) em->em_expr, rel->relid) != NULL)
				inv.uses_segid = true;

	if (!inv.uses_ctid || inv.uses_segid ||
		list_member_oid(noticed_relids, rte->relid))
		return;

	switch (root->parse->commandType)
	{
		case CMD_UPDATE:
			cmd = "UPDATE";
			elevel = ERROR;
			break;
		case CMD_DELETE:
			cmd = "DELETE";
			elevel = ERROR;
			break;
		case CMD_MERGE:
			cmd = "MERGE";
			elevel = ERROR;
			break;
		default:
			cmd = "SELECT";
			elevel = NOTICE;
	}
	ereport(elevel,
			(errmsg("%s uses system-defined column \"%s.ctid\" without the necessary companion column \"%s.gp_segment_id\"",
					cmd, rte->eref->aliasname, rte->eref->aliasname),
			 errhint("To uniquely identify a row within a distributed table, use the \"gp_segment_id\" column together with the \"ctid\" column.")));
}

/*
 * NOT IN (SELECT ctid FROM t), and <> ALL: Cloudberry makes it an anti-join
 * whose condition compares t's ctid, and notices it there; PostgreSQL's
 * planner keeps it a subplan, whose ctid is only what the subquery returns.
 * So such a subquery is noticed as it stands, once for its relation, before
 * the statement is planned.
 */
typedef struct SublinkNoticeContext
{
	CmdType		cmd;			/* of the query the SubLink is in */
	bool		under_not;
} SublinkNoticeContext;

static bool
sublink_notice_walker(Node *node, SublinkNoticeContext *cxt)
{
	if (node == NULL)
		return false;

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == NOT_EXPR)
	{
		bool		save = cxt->under_not;
		bool		result;

		cxt->under_not = true;
		result = expression_tree_walker(node, sublink_notice_walker, cxt);
		cxt->under_not = save;
		return result;
	}

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *sub = (Query *) sublink->subselect;
		bool		save = cxt->under_not;

		if ((sublink->subLinkType == ALL_SUBLINK ||
			 (sublink->subLinkType == ANY_SUBLINK && cxt->under_not)) &&
			sub->commandType == CMD_SELECT)
		{
			foreach_node(TargetEntry, tle, sub->targetList)
			{
				Var		   *var = (Var *) tle->expr;
				RangeTblEntry *rte;
				const char *cmd;

				if (tle->resjunk || !IsA(var, Var) || var->varlevelsup != 0 ||
					var->varattno != SelfItemPointerAttributeNumber)
					continue;
				rte = rt_fetch(var->varno, sub->rtable);
				if (rte->rtekind != RTE_RELATION ||
					GpScanDistributedPolicy(rte->relid) == NULL ||
					find_segment_of((Node *) sub->targetList, var->varno) != NULL ||
					list_member_oid(noticed_relids, rte->relid))
					continue;
				cmd = cxt->cmd == CMD_UPDATE ? "UPDATE" :
					cxt->cmd == CMD_DELETE ? "DELETE" :
					cxt->cmd == CMD_MERGE ? "MERGE" : "SELECT";
				ereport(cxt->cmd == CMD_UPDATE || cxt->cmd == CMD_DELETE ||
						cxt->cmd == CMD_MERGE ? ERROR : NOTICE,
						(errmsg("%s uses system-defined column \"%s.ctid\" without the necessary companion column \"%s.gp_segment_id\"",
								cmd, rte->eref->aliasname, rte->eref->aliasname),
						 errhint("To uniquely identify a row within a distributed table, use the \"gp_segment_id\" column together with the \"ctid\" column.")));
				noticed_relids = lappend_oid(noticed_relids, rte->relid);
			}
		}

		/* the test expression, and the subquery's own */
		cxt->under_not = false;
		(void) sublink_notice_walker(sublink->testexpr, cxt);
		(void) query_tree_walker(sub, sublink_notice_walker, cxt, 0);
		cxt->under_not = save;
		return false;
	}

	if (IsA(node, Query))
	{
		CmdType		save = cxt->cmd;
		bool		result;

		cxt->cmd = ((Query *) node)->commandType;
		result = query_tree_walker((Query *) node, sublink_notice_walker, cxt, 0);
		cxt->cmd = save;
		return result;
	}

	cxt->under_not = false;
	return expression_tree_walker(node, sublink_notice_walker, cxt);
}

void
GpScanNoticeSublinkCtid(Query *parse)
{
	SublinkNoticeContext cxt = {.cmd = parse->commandType,.under_not = false};

	/* the last planning's list went with its memory */
	noticed_relids = NIL;
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	(void) query_tree_walker(parse, sublink_notice_walker, &cxt, 0);
}

static void
gp_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					RangeTblEntry *rte)
{
	CustomPath *cp;
	GpPolicy   *policy;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	if (rel->reloptkind == RELOPT_BASEREL && rte->rtekind == RTE_RELATION)
		warn_ctid_without_segment_id(root, rel, rte);
	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	/* A parent whose children are scanned instead has no scan of its own. */
	if (rte->rtekind != RTE_RELATION || rte->inh)
		return;
	if (IS_DUMMY_REL(rel))
		return;
	/*
	 * a table's, a materialized view's or an external table's, which the
	 * segments read
	 */
	if (get_rel_relkind(rte->relid) != RELKIND_RELATION &&
		get_rel_relkind(rte->relid) != RELKIND_MATVIEW &&
		get_rel_relkind(rte->relid) != RELKIND_FOREIGN_TABLE)
		return;

	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL)
		return;

	/*
	 * Every other way of reading it reads the coordinator's empty copy, so
	 * this is the only path left: not the cheapest of several, the one that
	 * gives the right answer.
	 */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rel->rows;
	cp->path.startup_cost = GATHER_STARTUP_COST;
	cp->path.total_cost = GATHER_STARTUP_COST +
		rel->rows * (GATHER_ROW_COST + cpu_tuple_cost);
	cp->path.pathkeys = NIL;

	/*
	 * It projects: a query of this table alone computes what it returns in
	 * the gather itself, gp_segment_id among it, which only the gather knows
	 * of a random table's row.
	 */
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &gather_path_methods;

	add_path(rel, &cp->path);
}

/* The locking clause a gather sends the segments; see GpScanSetLocking(). */
static Oid	locking_relid = InvalidOid;
static LockClauseStrength locking_strength;
static LockWaitPolicy locking_wait;

void
GpScanSetLocking(Oid relid, LockClauseStrength strength,
				 LockWaitPolicy waitPolicy)
{
	locking_relid = relid;
	locking_strength = strength;
	locking_wait = waitPolicy;
}

void
GpScanClearLocking(void)
{
	locking_relid = InvalidOid;
}

bool
GpScanSetCursor(bool cursor)
{
	bool		was = planning_cursor;

	planning_cursor = cursor;
	return was;
}

/* The first expression in a tree that is gp_segment_id of varno's row. */
static bool
find_segment_of_walker(Node *node, void *context)
{
	void	  **cxt = (void **) context;

	if (node == NULL)
		return false;
	if (GpSegmentIsSegmentOf(node, *(Index *) cxt[0]))
	{
		cxt[1] = node;
		return true;
	}
	return expression_tree_walker(node, find_segment_of_walker, context);
}

static Node *
find_segment_of(Node *tree, Index relid)
{
	void	   *cxt[2] = {&relid, NULL};

	(void) find_segment_of_walker(tree, cxt);
	return (Node *) cxt[1];
}

/*
 * The whole-row Var of varno's row the plan reads, first in the target the
 * rel gives the plan above it: of the table's row type, or of RECORD, as the
 * planner makes the one a foreign table's UPDATE reads, and O20's for a
 * table whose access method takes its old row from the plan.  The scan
 * tuple's column has the type the plan's Var expects, or the executor
 * refuses it; and one column serves every whole-row Var of the rel, which
 * setrefs.c matches by column number alone.  So RECORD's, where the plan
 * has one: the other is gp_segment_id's argument, segment_of(t.*), whose
 * call is a column of the scan tuple of its own and reads no whole row.
 */
static bool
find_whole_row_walker(Node *node, void *context)
{
	void	  **cxt = (void **) context;

	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == *(Index *) cxt[0] &&
		((Var *) node)->varattno == InvalidAttrNumber &&
		((Var *) node)->varlevelsup == 0)
	{
		if (cxt[1] == NULL || ((Var *) node)->vartype == RECORDOID)
			cxt[1] = node;
		return ((Var *) node)->vartype == RECORDOID;
	}
	return expression_tree_walker(node, find_whole_row_walker, context);
}

static Var *
find_whole_row(Node *tree, Index relid)
{
	void	   *cxt[2] = {&relid, NULL};

	(void) find_whole_row_walker(tree, cxt);
	return (Var *) cxt[1];
}

/* One more column of what the segments send. */
static int
remote_column(StringInfo sql, List **types, List **typmods,
			  const char *column, Oid type, int32 typmod)
{
	int			n = list_length(*types);

	if (n > 0)
		appendStringInfoString(sql, ", ");
	GpAppendTransferColumn(sql, column, type);
	*types = lappend_oid(*types, type);
	*typmods = lappend_int(*typmods, typmod);
	return n;
}

static Plan *
gather_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation;
	TupleDesc	tupdesc;
	GpPolicy   *policy;
	List	   *pushed = NIL;
	List	   *local = NIL;
	CurrentOfExpr *current_of = NULL;
	List	   *dpcontext;
	Bitmapset  *needed = NULL;
	StringInfoData select;
	StringInfoData where;
	StringInfoData locking;
	List	   *types = NIL;
	List	   *typmods = NIL;
	List	   *scan_tlist = NIL;
	List	   *sources = NIL;
	List	   *attrs = NIL;
	List	   *contents = NIL;
	int			ctid_remote = -1;
	bool		whole_row;
	bool		identity;
	Node	   *segment_of = NULL;
	static const AttrNumber sysattrs[] = {
		MinTransactionIdAttributeNumber, MinCommandIdAttributeNumber,
		MaxTransactionIdAttributeNumber, MaxCommandIdAttributeNumber
	};

#define NEEDED(attno) \
	bms_is_member((attno) - FirstLowInvalidHeapAttributeNumber, needed)
#define SCAN_COLUMN(expr, name, source) \
	do { \
		scan_tlist = lappend(scan_tlist, \
							 makeTargetEntry((Expr *) (expr), \
											 list_length(scan_tlist) + 1, \
											 pstrdup(name), false)); \
		sources = lappend_int(sources, (source)); \
	} while (0)

	relation = table_open(rte->relid, NoLock);
	tupdesc = RelationGetDescr(relation);
	policy = GpScanDistributedPolicy(rte->relid);

	/*
	 * Which conditions go to the segments, and which stay here; and the
	 * cursor WHERE CURRENT OF names, whose position the gather reads itself.
	 */
	foreach_node(RestrictInfo, ri, clauses)
	{
		if (IsA(ri->clause, CurrentOfExpr) &&
			((CurrentOfExpr *) ri->clause)->cvarno == rel->relid)
			current_of = (CurrentOfExpr *) ri->clause;
		else if (!ri->pseudoconstant && is_shippable(ri->clause, rel->relid))
			pushed = lappend(pushed, ri->clause);
		else
			local = lappend(local, ri);
	}

	/*
	 * Which columns anything here reads: what the rel gives the plan above
	 * it, which is all a projection put in this node's place may compute
	 * from -- the planner hands a node that projects no target list of its
	 * own (CP_IGNORE_TLIST) -- and the conditions left here.
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &needed);
	pull_varattnos((Node *) tlist, rel->relid, &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, false), rel->relid,
				   &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, true), rel->relid,
				   &needed);
	whole_row = NEEDED(InvalidAttrNumber);

	/*
	 * A table an UPDATE, DELETE or MERGE changes: each row with its ctid,
	 * which with the segment it came from is the row's identity to the write
	 * that changes it on that segment (gp_explicit.c).
	 */
	identity = (root->parse->commandType == CMD_UPDATE ||
				root->parse->commandType == CMD_DELETE ||
				root->parse->commandType == CMD_MERGE) &&
		bms_is_member(rel->relid, root->all_result_relids);

	/* What the segments send, and the scan tuple made of it. */
	initStringInfo(&select);
	appendStringInfoString(&select, "SELECT ");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		int			remote = -1;

		if (!att->attisdropped && (whole_row || NEEDED(att->attnum)))
		{
			remote = remote_column(&select, &types, &typmods,
								   quote_identifier(NameStr(att->attname)),
								   att->atttypid, att->atttypmod);
			if (NEEDED(att->attnum))
				SCAN_COLUMN(makeVar(rel->relid, att->attnum, att->atttypid,
									att->atttypmod, att->attcollation, 0),
							NameStr(att->attname), remote);
		}
		attrs = lappend_int(attrs, remote);
	}

	/*
	 * ctid, where the plan reads it, where it is a write's identity, and in a
	 * cursor, whose rows WHERE CURRENT OF finds by it.
	 */
	if (NEEDED(SelfItemPointerAttributeNumber) || identity || planning_cursor ||
		current_of != NULL)
	{
		ctid_remote = remote_column(&select, &types, &typmods, "ctid",
									TIDOID, -1);
		if (NEEDED(SelfItemPointerAttributeNumber))
			SCAN_COLUMN(makeVar(rel->relid, SelfItemPointerAttributeNumber,
								TIDOID, -1, InvalidOid, 0),
						"ctid", identity ? GATHER_SRC_IDENTITY : ctid_remote);
	}
	for (int i = 0; i < lengthof(sysattrs); i++)
	{
		const FormData_pg_attribute *att = SystemAttributeDefinition(sysattrs[i]);

		if (NEEDED(sysattrs[i]))
			SCAN_COLUMN(makeVar(rel->relid, sysattrs[i], att->atttypid, -1,
								InvalidOid, 0),
						NameStr(att->attname),
						remote_column(&select, &types, &typmods,
									  NameStr(att->attname), att->atttypid,
									  -1));
	}
	if (NEEDED(TableOidAttributeNumber))
		SCAN_COLUMN(makeVar(rel->relid, TableOidAttributeNumber, OIDOID, -1,
							InvalidOid, 0),
					"tableoid", GATHER_SRC_TABLEOID);
	if (whole_row)
	{
		Var		   *wholerow = find_whole_row((Node *) rel->reltarget->exprs,
											  rel->relid);

		if (wholerow == NULL || wholerow->vartype != RECORDOID)
		{
			Var		   *other = find_whole_row((Node *) tlist, rel->relid);

			if (other != NULL && (wholerow == NULL || other->vartype == RECORDOID))
				wholerow = other;
		}
		SCAN_COLUMN(wholerow != NULL ? copyObject(wholerow)
					: makeWholeRowVar(rte, rel->relid, 0, false),
					RelationGetRelationName(relation), GATHER_SRC_WHOLEROW);
	}

	/*
	 * gp_segment_id of a hashed or random table, where the query names it:
	 * the segment the row came from, which is the one that holds it.  Only a
	 * random table's needs this -- nothing in the row says -- and a hashed
	 * table's comes free.  It is in the scan tuple as the call the query
	 * makes, so that the planner's own references to that call, where this
	 * node computes them, read it (setrefs.c).
	 */
	if (policy != NULL && !GpPolicyIsReplicated(policy))
	{
		/* a partition's, in its own target, of its row as its parent's */
		segment_of = find_segment_of((Node *) rel->reltarget->exprs, rel->relid);
		if (segment_of == NULL)
			segment_of = find_segment_of((Node *) root->processed_tlist, rel->relid);
		if (segment_of == NULL)
			segment_of = find_segment_of((Node *) extract_actual_clauses(local, false),
										 rel->relid);
		if (segment_of != NULL)
			SCAN_COLUMN(copyObject(segment_of), "gp_segment_id",
						GATHER_SRC_SEGMENT);
	}

	/* A row with no column still has to be a row. */
	if (types == NIL)
		(void) remote_column(&select, &types, &typmods, "NULL::pg_catalog.bool",
							 BOOLOID, -1);

	appendStringInfo(&select, " FROM ONLY %s",
					 GpDispatchRelationName(RelationGetRelid(relation)));

	dpcontext = deparse_context_for(RelationGetRelationName(relation),
									rte->relid);
	initStringInfo(&where);
	foreach_ptr(Node, clause, pushed)
	{
		Node	   *qual = copyObject(clause);

		/* deparse_context_for() knows this relation as range table entry 1 */
		ChangeVarNodes(qual, rel->relid, 1, 0);
		/* ruleutils brackets an operator's operands itself */
		appendStringInfo(&where, "%s%s", where.len > 0 ? " AND " : "",
						 deparse_expression(qual, dpcontext, false, true));
	}

	/* the rows a SELECT ... FOR UPDATE locks, locked where they are */
	initStringInfo(&locking);
	if (OidIsValid(locking_relid) && locking_relid == rte->relid)
		appendStringInfo(&locking, " FOR %s%s",
						 locking_strength == LCS_FORKEYSHARE ? "KEY SHARE" :
						 locking_strength == LCS_FORSHARE ? "SHARE" :
						 locking_strength == LCS_FORNOKEYUPDATE ? "NO KEY UPDATE" :
						 "UPDATE",
						 locking_wait == LockWaitSkip ? " SKIP LOCKED" :
						 locking_wait == LockWaitError ? " NOWAIT" : "");

	if (policy != NULL && GpPolicyIsReplicated(policy))
		contents = list_make1_int(GpScanReplicatedContent(policy));
	else if (policy != NULL && current_of == NULL)
		contents = dispatch_contents(policy, pushed, rel->relid);

	table_close(relation, NoLock);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(local, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = list_make5(makeString(select.data),
									   makeString(where.data),
									   makeString(locking.data),
									   contents,
									   makeInteger(policy != NULL ? policy->numsegments : 0));
	cscan->custom_private = lappend(cscan->custom_private, types);
	cscan->custom_private = lappend(cscan->custom_private, typmods);
	cscan->custom_private = lappend(cscan->custom_private, sources);
	cscan->custom_private = lappend(cscan->custom_private, attrs);
	cscan->custom_private = lappend(cscan->custom_private, makeInteger(ctid_remote));
	cscan->custom_private = lappend(cscan->custom_private,
									makeString(current_of != NULL && current_of->cursor_name != NULL
											   ? pstrdup(current_of->cursor_name) : ""));
	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(current_of != NULL ? current_of->cursor_param : 0));
	cscan->custom_private = lappend(cscan->custom_private, makeBoolean(identity));
	cscan->custom_private = lappend(cscan->custom_private, makeString(""));
	cscan->methods = &gather_scan_methods;

	return &cscan->scan.plan;
#undef NEEDED
#undef SCAN_COLUMN
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

static int *
int_array(List *list, int *n)
{
	int		   *result = palloc_array(int, Max(list_length(list), 1));

	*n = 0;
	foreach_int(i, list)
		result[(*n)++] = i;
	return result;
}

static Node *
gather_create_state(CustomScan *cscan)
{
	GatherScanState *state = (GatherScanState *) newNode(sizeof(GatherScanState),
														 T_CustomScanState);
	List	   *priv = cscan->custom_private;

	state->css.methods = &gather_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	state->select = strVal(list_nth(priv, GATHER_PRIVATE_SELECT));
	state->where = strVal(list_nth(priv, GATHER_PRIVATE_WHERE));
	state->locking = strVal(list_nth(priv, GATHER_PRIVATE_LOCKING));
	state->contents = int_array((List *) list_nth(priv, GATHER_PRIVATE_CONTENTS),
								&state->ncontents);
	state->nsegments = intVal(list_nth(priv, GATHER_PRIVATE_NSEGMENTS));
	state->sources = int_array((List *) list_nth(priv, GATHER_PRIVATE_SOURCES),
							   &state->nsources);
	state->attrs = int_array((List *) list_nth(priv, GATHER_PRIVATE_ATTRS),
							 &state->natts);
	state->ctid_remote = intVal(list_nth(priv, GATHER_PRIVATE_CTID));
	state->cursor_name = strVal(list_nth(priv, GATHER_PRIVATE_CURSOR));
	if (state->cursor_name[0] == '\0')
		state->cursor_name = NULL;
	state->cursor_param = intVal(list_nth(priv, GATHER_PRIVATE_CURSOR_PARAM));
	state->identity = boolVal(list_nth(priv, GATHER_PRIVATE_IDENTITY));
	state->limit = strVal(list_nth(priv, GATHER_PRIVATE_LIMIT));
	return (Node *) state;
}

static bool
gather_is_current_of(GatherScanState *state)
{
	return state->cursor_name != NULL || state->cursor_param > 0;
}

static void
gather_begin(CustomScanState *node, EState *estate, int eflags)
{
	GatherScanState *state = (GatherScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *types = (List *) list_nth(cscan->custom_private, GATHER_PRIVATE_TYPES);
	List	   *typmods = (List *) list_nth(cscan->custom_private, GATHER_PRIVATE_TYPMODS);
	TupleDesc	remote = CreateTemplateTupleDesc(list_length(types));
	int			i = 0;
	ListCell   *lt,
			   *lm;

	forboth(lt, types, lm, typmods)
	{
		i++;
		TupleDescInitEntry(remote, i, NULL, lfirst_oid(lt), lfirst_int(lm), 0);
	}
	TupleDescFinalize(remote);
	state->remote = ExecInitExtraTupleSlot(estate, remote, &TTSOpsVirtual);
	state->rowvalues = palloc_array(Datum, Max(state->natts, 1));
	state->rownulls = palloc_array(bool, Max(state->natts, 1));
	state->current_content = -1;
	state->external = !gather_is_current_of(state) &&
		GpPolicyIsExternalTable(RelationGetRelid(node->ss.ss_currentRelation));

	/* each gather is a slice of its own, numbered as the executor meets it */
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		int			slice = GpNextGatherSlice();

		if (gather_is_current_of(state))
			GpReportDispatch(slice, true, state->nsegments);
		else if (state->ncontents > 0)
			GpReportDispatchContents(slice, state->contents, state->ncontents);
		else
			GpReportDispatch(slice, false, state->nsegments);
	}
}

/* The name WHERE CURRENT OF gives the cursor by: its own, or a parameter's. */
static char *
cursor_name_of(GatherScanState *state)
{
	ParamListInfo params = state->css.ss.ps.ps_ExprContext->ecxt_param_list_info;
	int			id = state->cursor_param;

	if (state->cursor_name != NULL)
		return state->cursor_name;

	if (params != NULL && id > 0 && id <= params->numParams)
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, id, false, &prmdata);
		else
			prm = &params->params[id - 1];

		if (OidIsValid(prm->ptype) && !prm->isnull)
		{
			if (prm->ptype != REFCURSOROID)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								id, format_type_be(prm->ptype),
								format_type_be(REFCURSOROID))));
			return TextDatumGetCString(prm->value);
		}
	}
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", id)));
	return NULL;
}

/*
 * The gather of relid in a cursor's plan, as execCurrent.c's
 * search_plan_tree() finds a scan: through an Append -- one of whose inputs
 * only can be on a row -- a Result, a Limit and a subquery; not through
 * anything that holds rows back.
 */
static GatherScanState *
find_cursor_gather(PlanState *node, Oid relid, bool *pending_rescan)
{
	GatherScanState *result = NULL;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_CustomScanState:
			if (((CustomScanState *) node)->methods == &gather_exec_methods &&
				((ScanState *) node)->ss_currentRelation != NULL &&
				RelationGetRelid(((ScanState *) node)->ss_currentRelation) == relid)
				result = (GatherScanState *) node;
			break;
		case T_AppendState:
			{
				AppendState *astate = (AppendState *) node;

				for (int i = 0; i < astate->as_nplans; i++)
				{
					GatherScanState *elem = find_cursor_gather(astate->appendplans[i],
															   relid,
															   pending_rescan);

					if (elem == NULL)
						continue;
					if (result != NULL)
						return NULL;	/* more than one */
					result = elem;
				}
			}
			break;
		case T_ResultState:
		case T_LimitState:
			result = find_cursor_gather(outerPlanState(node), relid,
										pending_rescan);
			break;
		case T_SubqueryScanState:
			result = find_cursor_gather(((SubqueryScanState *) node)->subplan,
										relid, pending_rescan);
			break;
		default:
			break;
	}

	if (result != NULL && node->chgParam != NULL)
		*pending_rescan = true;
	return result;
}

/*
 * WHERE CURRENT OF: where the cursor's current row of this relation is -- its
 * segment and its ctid there -- or false if another relation's scan made it,
 * as execCurrentOf() answers, with its errors.
 */
static bool
gather_current_of(GatherScanState *state, int *content, ItemPointer tid)
{
	Oid			relid = RelationGetRelid(state->css.ss.ss_currentRelation);
	char	   *cursor_name = cursor_name_of(state);
	char	   *table_name = get_rel_name(relid);
	Portal		portal;
	QueryDesc  *queryDesc;
	GatherScanState *cursor;
	bool		pending_rescan = false;
	TupleTableSlot *slot;

	portal = GetPortalByName(cursor_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor_name)));
	if (portal->strategy != PORTAL_ONE_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a SELECT query", cursor_name)));
	queryDesc = portal->queryDesc;
	if (queryDesc == NULL || queryDesc->estate == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is held from a previous transaction",
						cursor_name)));

	cursor = find_cursor_gather(queryDesc->planstate, relid, &pending_rescan);
	if (cursor == NULL || cursor->ctid_remote < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
						cursor_name, table_name)));
	if (portal->atStart || portal->atEnd)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not positioned on a row",
						cursor_name)));

	slot = cursor->css.ss.ss_ScanTupleSlot;
	if (TupIsNull(slot) || pending_rescan || cursor->current_content < 0)
		return false;
	*content = cursor->current_content;
	ItemPointerCopy(&slot->tts_tid, tid);
	return true;
}

/*
 * A plan's cost less what the planner charges its gathers for starting --
 * the round trip to each segment, which Cloudberry's cost model has no
 * counterpart of -- for what a cost is compared with that was set in
 * Cloudberry's units: a resource group's min_cost, under which a query runs
 * without a slot (gp_resource's resgroup.c).  ORCA's plans have no gathers
 * of the planner's.
 */
static void
gather_startup_walker(Plan *plan, int *ngathers)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gather_scan_methods)
		(*ngathers)++;
	gather_startup_walker(plan->lefttree, ngathers);
	gather_startup_walker(plan->righttree, ngathers);
	if (IsA(plan, CustomScan))
	{
		foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
			gather_startup_walker(child, ngathers);
	}
	else if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			gather_startup_walker(child, ngathers);
	}
	else if (IsA(plan, SubqueryScan))
		gather_startup_walker(((SubqueryScan *) plan)->subplan, ngathers);
}

double
GpPlanCostLessGathers(PlannedStmt *stmt)
{
	int			ngathers = 0;
	double		cost;

	gather_startup_walker(stmt->planTree, &ngathers);
	foreach_ptr(Plan, sub, stmt->subplans)
		gather_startup_walker(sub, &ngathers);
	cost = stmt->planTree->total_cost - ngathers * GATHER_STARTUP_COST;
	return cost > 0 ? cost : 0;
}

/*
 * Start reading: from the segments the plan names, or the cursor's one.
 *
 * The coordinator checked the statement's privileges before it ran any of
 * it, the table's among them, as the user each is checked as -- a view's
 * owner for a table the view reads -- and a segment knows only the session's
 * role.  So the query says it was checked (GP_CHECKED_MARKER), and a segment
 * checks none of its privileges, for the coordinator's connection alone, as
 * Cloudberry's segments check none of a SELECT's (InitPlan()).
 */
static bool
gather_start(GatherScanState *state)
{
	TupleDesc	desc = state->remote->tts_tupleDescriptor;
	StringInfoData sql;

	initStringInfo(&sql);
	appendStringInfoString(&sql, GP_CHECKED_MARKER);
	appendStringInfoString(&sql, state->select);

	if (gather_is_current_of(state))
	{
		int			content;
		ItemPointerData tid;

		if (!gather_current_of(state, &content, &tid))
			return false;
		appendStringInfo(&sql, " WHERE ctid = '(%u,%u)'::pg_catalog.tid%s%s%s",
						 ItemPointerGetBlockNumber(&tid),
						 ItemPointerGetOffsetNumber(&tid),
						 state->where[0] != '\0' ? " AND " : "",
						 state->where, state->locking);
		state->gather = GpGatherStartOn(sql.data, desc, content);
		return true;
	}

	if (state->where[0] != '\0')
		appendStringInfo(&sql, " WHERE %s", state->where);
	appendStringInfoString(&sql, state->limit);
	appendStringInfoString(&sql, state->locking);

	/* the segments direct dispatch named, or the table's: all, or the first */
	if (state->ncontents > 0)
		state->gather = GpGatherStartOnContents(sql.data, desc, state->contents,
												state->ncontents);
	else
		state->gather = GpGatherStartOnSegments(sql.data, desc,
												state->nsegments);
	return true;
}

/* The scan tuple, from a row as it arrived from segment "content". */
static void
gather_store(GatherScanState *state, TupleTableSlot *slot, int content)
{
	TupleTableSlot *remote = state->remote;
	Relation	rel = state->css.ss.ss_currentRelation;
	EState	   *estate = state->css.ss.ps.state;

	ExecClearTuple(slot);
	for (int i = 0; i < state->nsources; i++)
	{
		int			src = state->sources[i];

		slot->tts_isnull[i] = false;
		switch (src)
		{
			case GATHER_SRC_TABLEOID:
				slot->tts_values[i] = ObjectIdGetDatum(RelationGetRelid(rel));
				break;
			case GATHER_SRC_SEGMENT:
				slot->tts_values[i] = Int32GetDatum(content);
				break;
			case GATHER_SRC_WHOLEROW:
				{
					TupleDesc	reldesc = RelationGetDescr(rel);
					HeapTuple	tuple;

					for (int a = 0; a < state->natts; a++)
					{
						int			r = state->attrs[a];

						state->rowvalues[a] = r >= 0 ? remote->tts_values[r] : (Datum) 0;
						state->rownulls[a] = r >= 0 ? remote->tts_isnull[r] : true;
					}
					tuple = heap_form_tuple(reldesc, state->rowvalues,
											state->rownulls);
					slot->tts_values[i] = heap_copy_tuple_as_datum(tuple, reldesc);
				}
				break;
			case GATHER_SRC_IDENTITY:
				{
					ItemPointer synthetic = palloc_object(ItemPointerData);

					GpRowIdentityMake(estate, content,
									  (ItemPointer) DatumGetPointer(remote->tts_values[state->ctid_remote]),
									  synthetic);
					slot->tts_values[i] = PointerGetDatum(synthetic);
				}
				break;
			default:
				slot->tts_values[i] = remote->tts_values[src];
				slot->tts_isnull[i] = remote->tts_isnull[src];
				break;
		}
	}
	ExecStoreVirtualTuple(slot);

	if (state->ctid_remote >= 0 && !remote->tts_isnull[state->ctid_remote])
		ItemPointerCopy((ItemPointer) DatumGetPointer(remote->tts_values[state->ctid_remote]),
						&slot->tts_tid);
	else
		ItemPointerSetInvalid(&slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	state->current_content = content;
}

/* The segments' next row, into the scan slot; false when they have no more. */
static bool
gather_fetch(GatherScanState *state, TupleTableSlot *slot)
{
	ScanState  *ss = &state->css.ss;
	MemoryContext oldcxt;
	int			content;
	bool		got;

	if (state->done)
		return false;

	if (state->gather == NULL && !gather_start(state))
	{
		state->done = true;
		state->current_content = -1;
		return false;
	}

	/*
	 * The row's values live in the per-tuple context, which ExecScan resets
	 * before asking for the next one; the query's own context would keep every
	 * row the scan ever read.
	 */
	oldcxt = MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	got = GpGatherNext(state->gather, state->remote, &content);
	if (got)
	{
		slot_getallattrs(state->remote);
		gather_store(state, slot, content);
		if (state->spool != NULL)
			tuplestore_puttupleslot(state->spool, slot);
	}
	MemoryContextSwitchTo(oldcxt);

	if (got)
		return true;

	GpGatherEnd(state->gather);
	state->gather = NULL;
	state->done = true;
	state->current_content = -1;
	return false;
}

static TupleTableSlot *
gather_next(ScanState *ss)
{
	GatherScanState *state = (GatherScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	/*
	 * Read again, what was kept is read first; the segments are read further
	 * only past its end, as a Materialize reads its child.
	 */
	if (state->spool != NULL && !tuplestore_ateof(state->spool) &&
		tuplestore_gettupleslot(state->spool, true, false, state->spooled))
	{
		ExecCopySlot(slot, state->spooled);
		slot->tts_tableOid = RelationGetRelid(ss->ss_currentRelation);
		return slot;
	}

	if (gather_fetch(state, slot))
		return slot;
	return ExecClearTuple(slot);
}

static bool
gather_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gather_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gather_next, gather_recheck);
}

bool
GpGatherScanStartEarly(PlanState *ps)
{
	GatherScanState *state = (GatherScanState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &gather_exec_methods)
		return false;

	/* WHERE CURRENT OF reads the row its cursor is on when it runs */
	if (!gather_is_current_of(state) && state->gather == NULL && !state->done)
		(void) gather_start(state);
	return true;
}

static void
gather_end(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
	if (state->spool != NULL)
		tuplestore_end(state->spool);
	state->spool = NULL;
}

/*
 * Read again: the segments run the query again -- or, for an external table
 * that keeps what it read, that is read again (GpGatherScanMarkRescans()).
 * The query the segments run has no parameter in it, which is what makes
 * what it read the answer whatever the parameters now are.
 */
static void
gather_rescan(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->spool != NULL)
	{
		tuplestore_rescan(state->spool);
		return;
	}

	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
	state->done = false;
	state->current_content = -1;
}

/*
 * An external table's gather that the plan may read more than once -- the
 * inner side of a nested loop, a subquery run for each row, the recursive
 * part of WITH RECURSIVE -- keeps the rows it read and reads them again: its
 * source is read once, as Cloudberry's planner makes sure of by putting a
 * Materialize above an external scan it would rescan (the path is not
 * "rescannable").  A command runs once, a file's rejected rows are counted
 * once, and gpfdist serves a scan once.  A Materialize that already keeps
 * the rows, above a subtree with no parameter to change, reads them once.
 */
static void mark_rescans(PlanState *ps, bool again);

static void
mark_rescans_list(List *subplans, bool again)
{
	foreach_node(SubPlanState, sps, subplans)
		mark_rescans(sps->planstate, again);
}

static void
mark_rescans_array(PlanState **planstates, int n, bool again)
{
	for (int i = 0; i < n; i++)
		mark_rescans(planstates[i], again);
}

static void
mark_rescans(PlanState *ps, bool again)
{
	if (ps == NULL)
		return;
	check_stack_depth();

	if (again && IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) ps;

		if (state->external && state->spool == NULL)
		{
			EState	   *estate = ps->state;
			MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

			state->spool = tuplestore_begin_heap(false, false, work_mem);
			state->spooled =
				ExecInitExtraTupleSlot(estate,
									   state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor,
									   &TTSOpsMinimalTuple);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	/* an initplan runs again as what it belongs to does; a subplan per row */
	mark_rescans_list(ps->initPlan, again);
	mark_rescans_list(ps->subPlan, true);

	switch (nodeTag(ps))
	{
		case T_NestLoopState:
		case T_RecursiveUnionState:
			mark_rescans(outerPlanState(ps), again);
			mark_rescans(innerPlanState(ps), true);
			return;
		case T_MaterialState:
			if ((((MaterialState *) ps)->eflags & EXEC_FLAG_REWIND) &&
				bms_is_empty(outerPlanState(ps)->plan->allParam))
				again = false;
			break;
		case T_AppendState:
			mark_rescans_array(((AppendState *) ps)->appendplans,
							   ((AppendState *) ps)->as_nplans, again);
			break;
		case T_MergeAppendState:
			mark_rescans_array(((MergeAppendState *) ps)->mergeplans,
							   ((MergeAppendState *) ps)->ms_nplans, again);
			break;
		case T_SubqueryScanState:
			mark_rescans(((SubqueryScanState *) ps)->subplan, again);
			break;
		case T_CustomScanState:
			foreach_ptr(PlanState, child, ((CustomScanState *) ps)->custom_ps)
				mark_rescans(child, again);
			break;
		default:
			break;
	}
	mark_rescans(outerPlanState(ps), again);
	mark_rescans(innerPlanState(ps), again);
}

void
GpGatherScanMarkRescans(PlanState *root)
{
	mark_rescans(root, false);
}

/*
 * A LIMIT above a gather, with nothing between them that drops a row: the
 * segments need send no more than its rows and its OFFSET's, and the query
 * sent them says so, as Cloudberry's planner puts a Limit below its Gather
 * Motion.  A segment's scan stops where the query does -- an external
 * table's, before the rows further on it would have rejected.
 */
static bool
limit_value(Node *expr, int64 *value)
{
	if (expr == NULL)
	{
		*value = 0;
		return true;
	}
	if (!IsA(expr, Const) || ((Const *) expr)->constisnull ||
		((Const *) expr)->consttype != INT8OID)
		return false;
	*value = DatumGetInt64(((Const *) expr)->constvalue);
	return *value >= 0;
}

static void
bound_gathers(Plan *plan, int64 bound)
{
	if (plan == NULL)
		return;
	check_stack_depth();

	switch (nodeTag(plan))
	{
		case T_Limit:
			{
				Limit	   *limit = (Limit *) plan;
				int64		count;
				int64		offset;

				if (limit->limitCount != NULL &&
					limit->limitOption == LIMIT_OPTION_COUNT &&
					limit_value(limit->limitCount, &count) &&
					limit_value(limit->limitOffset, &offset) &&
					count <= PG_INT64_MAX - offset)
					bound_gathers(plan->lefttree, count + offset);
				else
					bound_gathers(plan->lefttree, -1);
				return;
			}
		case T_Result:
			/* a projection, or a condition on no row in particular */
			bound_gathers(plan->lefttree, plan->qual == NIL ? bound : -1);
			return;
		case T_SubqueryScan:
			bound_gathers(((SubqueryScan *) plan)->subplan,
						  plan->qual == NIL ? bound : -1);
			return;
		case T_Append:
			/* UNION ALL: no branch needs to give more than the whole */
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				bound_gathers(child, plan->qual == NIL ? bound : -1);
			return;
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods == &gather_scan_methods)
				{
					if (bound >= 0 && plan->qual == NIL &&
						list_length(cscan->custom_private) > GATHER_PRIVATE_LIMIT)
						list_nth_cell(cscan->custom_private,
									  GATHER_PRIVATE_LIMIT)->ptr_value =
							makeString(psprintf(" LIMIT " INT64_FORMAT, bound));
					return;
				}
				foreach_ptr(Plan, child, cscan->custom_plans)
					bound_gathers(child, -1);
				break;
			}
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				bound_gathers(child, -1);
			break;
		case T_BitmapAnd:
			foreach_ptr(Plan, child, ((BitmapAnd *) plan)->bitmapplans)
				bound_gathers(child, -1);
			break;
		case T_BitmapOr:
			foreach_ptr(Plan, child, ((BitmapOr *) plan)->bitmapplans)
				bound_gathers(child, -1);
			break;
		default:
			break;
	}
	bound_gathers(plan->lefttree, -1);
	bound_gathers(plan->righttree, -1);
}

void
GpScanBoundGathers(PlannedStmt *stmt)
{
	bound_gathers(stmt->planTree, -1);
	foreach_ptr(Plan, sub, stmt->subplans)
		bound_gathers(sub, -1);
}

static void
gather_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GatherScanState *state = (GatherScanState *) node;

	if (gather_is_current_of(state))
		ExplainPropertyInteger("Segments", NULL, 1, es);
	else if (state->ncontents == 1)
		ExplainPropertyInteger("Segment", NULL, state->contents[0], es);
	else if (state->ncontents > 1)
	{
		List	   *contents = NIL;

		for (int i = 0; i < state->ncontents; i++)
			contents = lappend(contents, psprintf("%d", state->contents[i]));
		ExplainPropertyList("Segments", contents, es);
	}
	else
		ExplainPropertyInteger("Segments", NULL, state->nsegments, es);

	if (es->verbose)
	{
		StringInfoData sql;

		initStringInfo(&sql);
		appendStringInfoString(&sql, state->select);
		if (gather_is_current_of(state))
			appendStringInfo(&sql, " WHERE CURRENT OF %s%s%s",
							 state->cursor_name != NULL ? quote_identifier(state->cursor_name)
							 : psprintf("$%d", state->cursor_param),
							 state->where[0] != '\0' ? " AND " : "",
							 state->where);
		else if (state->where[0] != '\0')
			appendStringInfo(&sql, " WHERE %s", state->where);
		if (!gather_is_current_of(state))
			appendStringInfoString(&sql, state->limit);
		appendStringInfoString(&sql, state->locking);
		ExplainPropertyText("Remote SQL", sql.data, es);
	}
}

/*
 * What EXPLAIN calls the node, through O4.  Cloudberry prints its Gather
 * Motion over the scan as two lines, "Gather Motion 2:1  (slice1; segments:
 * 2)" and the scan beneath; the port's node is both, so it is one line that
 * says both -- the motion, the table it reads, and how many segments.
 */
static explain_node_label_hook_type prev_explain_node_label = NULL;

static void
gp_scan_explain_label(PlanState *planstate, ExplainState *es,
					  const char **pname, const char **suffix)
{
	CustomScanState *node = (CustomScanState *) planstate;

	if (IsA(planstate, CustomScanState) && node->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) node;
		int			nsegs = gather_is_current_of(state) ? 1
			: state->ncontents > 0 ? state->ncontents : state->nsegments;

		*pname = psprintf("Gather Motion %d:1", nsegs);
		*suffix = psprintf("  (slice1; segments: %d)", nsegs);
		return;
	}

	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

void
GpScanInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&gather_scan_methods);

	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = gp_scan_explain_label;

	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gp_set_rel_pathlist;

	prev_build_simple_rel = build_simple_rel_hook;
	build_simple_rel_hook = gp_build_simple_rel;
}
