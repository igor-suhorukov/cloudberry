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
 * compat/dynamicscan.c
 *	  Dynamic Scan and Partition Selector, as CustomScans.
 *
 * What Cloudberry's nodes do, with PostgreSQL 19's scans and partition
 * pruning underneath (see cb_dynamicscan.h for the plan nodes):
 *
 *	- Cloudberry's DynamicSeqScan opens each partition in turn and remaps the
 *	  scan's columns to it while the query runs, which keeps a plan small
 *	  when a table has thousands of partitions.  The port's Dynamic Scan is
 *	  handed its partitions' scans ready-made, as the planner's Append is:
 *	  each is an ordinary scan, so the executor's own SeqScan, IndexScan,
 *	  IndexOnlyScan and BitmapHeapScan run it, EXPLAIN shows it, and nothing
 *	  here reimplements them.  What it adds to an Append is what Cloudberry's
 *	  adds: it runs only the partitions a Partition Selector chose.
 *	- Cloudberry's PartitionSelector evaluates its pruning steps through
 *	  ExecAddMatchingSubPlans(), which it added so that a step can read the
 *	  selector's row.  PostgreSQL 19's get_matching_partitions() evaluates a
 *	  step's expressions in whatever expression context it is handed, so the
 *	  port's selector hands it one whose outer tuple is the row.
 *
 * A Dynamic Scan that starts before its selector has finished -- a hash
 * join may fetch its first outer row before it builds its hash table --
 * reads the parameter as unset and scans every partition it has, which is
 * the answer without pruning.  CTranslatorDXLToPlStmt makes that rare by
 * giving the hash join's outer side the start-up cost that stops the fetch.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/partition.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "cb_dynamicscan.h"

/* ------------------------------------------------------------------------- */
/* The scan of one partition                                                 */
/* ------------------------------------------------------------------------- */

typedef struct remap_context
{
	Index		root_rti;
	Index		part_rti;
	Relation	root;
	Relation	part;
	int			failure;
} remap_context;

/*
 * Expressions over the table's columns: the partition's column of the same
 * name, which need not be at the same number -- a partition can have been
 * created with its columns in another order, or have dropped ones.
 */
static List *
remap_table_exprs(List *exprs, remap_context *context)
{
	exprs = map_partition_varattnos(exprs, context->root_rti, context->part,
									context->root);
	ChangeVarNodes((Node *) exprs, context->root_rti, context->part_rti, 0);
	return exprs;
}

/*
 * Expressions over an index's columns, numbered by position in the index: a
 * partition's index of the table's index has its columns in the same order.
 */
static List *
remap_index_exprs(List *exprs, remap_context *context)
{
	ChangeVarNodes((Node *) exprs, context->root_rti, context->part_rti, 0);
	return exprs;
}

/* May this transaction's snapshots use the index?  As get_relation_info(). */
static bool
index_usable_by_snapshots(Oid index_oid)
{
	HeapTuple	tup;
	Form_pg_index index;
	bool		usable;

	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u", index_oid);
	index = (Form_pg_index) GETSTRUCT(tup);
	usable = !(index->indcheckxmin &&
			   !TransactionIdPrecedes(HeapTupleHeaderGetXmin(tup->t_data),
									  TransactionXmin));
	ReleaseSysCache(tup);

	return usable;
}

/* The partition's index of the table's index. */
static Oid
partition_index(Oid index_oid, remap_context *context)
{
	Oid			part_index = index_get_partition(context->part, index_oid);

	if (!OidIsValid(part_index))
		context->failure = GP_ORCA_PARTITION_NO_INDEX;
	else if (!index_usable_by_snapshots(part_index))
		context->failure = GP_ORCA_PARTITION_INDEX_TOO_NEW;
	else
		return part_index;

	return InvalidOid;
}

static void
remap_plan(Plan *plan, remap_context *context)
{
	ListCell   *lc;

	plan->targetlist = remap_table_exprs(plan->targetlist, context);
	plan->qual = remap_table_exprs(plan->qual, context);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
			((Scan *) plan)->scanrelid = context->part_rti;
			break;

		case T_IndexScan:
			{
				IndexScan  *scan = (IndexScan *) plan;

				scan->scan.scanrelid = context->part_rti;
				scan->indexid = partition_index(scan->indexid, context);
				scan->indexqual = remap_index_exprs(scan->indexqual, context);
				scan->indexqualorig = remap_table_exprs(scan->indexqualorig,
														context);
				scan->indexorderby = remap_index_exprs(scan->indexorderby,
													   context);
				scan->indexorderbyorig =
					remap_table_exprs(scan->indexorderbyorig, context);
				break;
			}

		case T_IndexOnlyScan:
			{
				IndexOnlyScan *scan = (IndexOnlyScan *) plan;

				/* its target list and filter read the index, by INDEX_VAR */
				scan->scan.scanrelid = context->part_rti;
				scan->indexid = partition_index(scan->indexid, context);
				scan->indexqual = remap_index_exprs(scan->indexqual, context);
				scan->recheckqual = remap_index_exprs(scan->recheckqual,
													  context);
				scan->indexorderby = remap_index_exprs(scan->indexorderby,
													   context);
				scan->indextlist = remap_table_exprs(scan->indextlist, context);
				break;
			}

		case T_BitmapHeapScan:
			{
				BitmapHeapScan *scan = (BitmapHeapScan *) plan;

				scan->scan.scanrelid = context->part_rti;
				scan->bitmapqualorig = remap_table_exprs(scan->bitmapqualorig,
														 context);
				remap_plan(plan->lefttree, context);
				break;
			}

		case T_BitmapIndexScan:
			{
				BitmapIndexScan *scan = (BitmapIndexScan *) plan;

				scan->scan.scanrelid = context->part_rti;
				scan->indexid = partition_index(scan->indexid, context);
				scan->indexqual = remap_index_exprs(scan->indexqual, context);
				scan->indexqualorig = remap_table_exprs(scan->indexqualorig,
														context);
				break;
			}

		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				remap_plan((Plan *) lfirst(lc), context);
			break;

		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				remap_plan((Plan *) lfirst(lc), context);
			break;

		default:
			elog(ERROR, "unexpected node in the scan of a partitioned table: %d",
				 (int) nodeTag(plan));
	}
}

Plan *
gp_orca_plan_for_partition(Plan *scan, Index root_rti, Index part_rti,
						   Oid root_relid, Oid part_relid, int *failure)
{
	remap_context context;
	Plan	   *plan;

	/* Both are locked already, the partition by the translator. */
	context.root_rti = root_rti;
	context.part_rti = part_rti;
	context.root = table_open(root_relid, NoLock);
	context.part = table_open(part_relid, NoLock);
	context.failure = GP_ORCA_PARTITION_OK;

	plan = (Plan *) copyObject(scan);
	remap_plan(plan, &context);

	table_close(context.part, NoLock);
	table_close(context.root, NoLock);

	*failure = context.failure;
	return context.failure == GP_ORCA_PARTITION_OK ? plan : NULL;
}

RangeTblEntry *
gp_orca_partition_rte(const RangeTblEntry *root, Oid part_relid)
{
	Relation	part = table_open(part_relid, NoLock);
	TupleDesc	desc = RelationGetDescr(part);
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	List	   *colnames = NIL;

	rte->rtekind = RTE_RELATION;
	rte->relid = part_relid;
	rte->relkind = part->rd_rel->relkind;
	rte->rellockmode = root->rellockmode;
	rte->inh = false;
	rte->inFromCl = root->inFromCl;
	rte->perminfoindex = 0;

	/*
	 * Each column under the table's alias for the column of the same name,
	 * as the planner's child entries have them, so that EXPLAIN names the
	 * partition's columns as the query did.
	 */
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		const char *name = "";

		if (!att->attisdropped)
		{
			AttrNumber	root_attno = get_attnum(root->relid,
												NameStr(att->attname));

			name = NameStr(att->attname);
			if (root_attno > 0 &&
				root_attno <= list_length(root->eref->colnames))
				name = strVal(list_nth(root->eref->colnames, root_attno - 1));
		}
		colnames = lappend(colnames, makeString(pstrdup(name)));
	}
	rte->alias = rte->eref = makeAlias(root->eref->aliasname, colnames);

	table_close(part, NoLock);
	return rte;
}

static Node *
index_var_mutator(Node *node, List *indextlist)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		TargetEntry *tle = get_tle_by_resno(indextlist,
											((Var *) node)->varattno);

		if (tle == NULL)
			elog(ERROR, "no index column %d in an index-only scan",
				 ((Var *) node)->varattno);
		return (Node *) copyObject(tle->expr);
	}

	return expression_tree_mutator(node, index_var_mutator, indextlist);
}

List *
gp_orca_dynamic_scan_tlist(Plan *scan)
{
	if (IsA(scan, IndexOnlyScan))
		return (List *) index_var_mutator((Node *) scan->targetlist,
										  ((IndexOnlyScan *) scan)->indextlist);

	return (List *) copyObject(scan->targetlist);
}

int
gp_orca_top_partition_index(Oid root_relid, Oid leaf_relid)
{
	Relation	root = table_open(root_relid, NoLock);
	PartitionDesc partdesc = RelationGetPartitionDesc(root, true);
	List	   *ancestors = get_partition_ancestors(leaf_relid);
	Oid			top = leaf_relid;
	ListCell   *lc;
	int			result = -1;

	/* the ancestor just below the table, if the leaf is not a partition of it */
	foreach(lc, ancestors)
	{
		if (lfirst_oid(lc) == root_relid)
			break;
		top = lfirst_oid(lc);
	}

	for (int i = 0; i < partdesc->nparts; i++)
	{
		if (partdesc->oids[i] == top)
		{
			result = i;
			break;
		}
	}

	list_free(ancestors);
	table_close(root, NoLock);
	return result;
}

/* ------------------------------------------------------------------------- */
/* Dynamic Scan                                                              */
/* ------------------------------------------------------------------------- */

/*
 * A Partition Selector's state, which is what its parameter holds once it
 * has finished: a pointer to it, so that "chose no partition" -- an empty
 * set, which a Bitmapset spells NULL -- is not read as "has not finished".
 */
typedef struct PartitionSelectorState
{
	CustomScanState css;
	int			paramid;
	List	   *steps;
	Relation	rel;
	PartitionPruneContext context;
	Bitmapset  *selected;		/* in the query's memory; NULL is empty */
	bool		done;
} PartitionSelectorState;

typedef struct DynamicScanState
{
	CustomScanState css;
	int			nchildren;
	PlanState **children;
	int		   *part_index;		/* each child's partition, at the top */
	List	   *paramids;		/* the Partition Selectors' parameters */
	bool		chosen;			/* has `valid` been worked out? */
	Bitmapset  *valid;			/* the children to run */
	int			current;		/* the child being run, or -1 */
	int			nscanned;		/* children run, for EXPLAIN ANALYZE */
} DynamicScanState;

static Node *create_dynamic_scan_state(CustomScan *cscan);
static void begin_dynamic_scan(CustomScanState *node, EState *estate,
							   int eflags);
static TupleTableSlot *exec_dynamic_scan(CustomScanState *node);
static void end_dynamic_scan(CustomScanState *node);
static void rescan_dynamic_scan(CustomScanState *node);
static void explain_dynamic_scan(CustomScanState *node, List *ancestors,
								 ExplainState *es);

const CustomScanMethods gp_orca_dynamic_scan_methods = {
	.CustomName = "Dynamic Scan",
	.CreateCustomScanState = create_dynamic_scan_state,
};

static const CustomExecMethods dynamic_scan_exec_methods = {
	.CustomName = "Dynamic Scan",
	.BeginCustomScan = begin_dynamic_scan,
	.ExecCustomScan = exec_dynamic_scan,
	.EndCustomScan = end_dynamic_scan,
	.ReScanCustomScan = rescan_dynamic_scan,
	.ExplainCustomScan = explain_dynamic_scan,
};

static Node *
create_dynamic_scan_state(CustomScan *cscan)
{
	DynamicScanState *state = palloc0_object(DynamicScanState);

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &dynamic_scan_exec_methods;

	return (Node *) state;
}

static void
begin_dynamic_scan(CustomScanState *node, EState *estate, int eflags)
{
	DynamicScanState *state = (DynamicScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *part_index = (List *) linitial(cscan->custom_private);
	ListCell   *lc;
	int			i = 0;

	Assert(list_length(cscan->custom_private) == 4);
	Assert(list_length(part_index) == list_length(cscan->custom_plans));

	state->nchildren = list_length(cscan->custom_plans);
	state->children = palloc0_array(PlanState *, state->nchildren);
	state->part_index = palloc0_array(int, state->nchildren);
	state->paramids = (List *) lsecond(cscan->custom_private);
	state->current = -1;

	foreach(lc, cscan->custom_plans)
	{
		state->children[i] = ExecInitNode((Plan *) lfirst(lc), estate, eflags);
		node->custom_ps = lappend(node->custom_ps, state->children[i]);
		state->part_index[i] = list_nth_int(part_index, i);
		i++;
	}

	/*
	 * The rows are the children's, in whatever slots they come in, as an
	 * Append's are.
	 */
	node->ss.ps.resultopsset = true;
	node->ss.ps.resultopsfixed = false;
}

/*
 * The children to run: all of them, less those whose partition a Partition
 * Selector that has finished did not choose.
 */
static Bitmapset *
choose_children(DynamicScanState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	Bitmapset  *valid = NULL;
	ListCell   *lc;

	for (int i = 0; i < state->nchildren; i++)
		valid = bms_add_member(valid, i);

	foreach(lc, state->paramids)
	{
		ParamExecData *prm = &estate->es_param_exec_vals[lfirst_int(lc)];
		PartitionSelectorState *selector;

		/* unset: the selector has not finished, and rules nothing out yet */
		if (prm->isnull || DatumGetPointer(prm->value) == NULL)
			continue;

		selector = (PartitionSelectorState *) DatumGetPointer(prm->value);
		for (int i = 0; i < state->nchildren; i++)
		{
			if (!bms_is_member(state->part_index[i], selector->selected))
				valid = bms_del_member(valid, i);
		}
	}

	return valid;
}

static TupleTableSlot *
exec_dynamic_scan(CustomScanState *node)
{
	DynamicScanState *state = (DynamicScanState *) node;

	if (!state->chosen)
	{
		state->valid = choose_children(state);
		state->chosen = true;
		state->current = bms_next_member(state->valid, -1);
		if (state->current >= 0)
			state->nscanned++;
	}

	while (state->current >= 0)
	{
		TupleTableSlot *slot;

		CHECK_FOR_INTERRUPTS();

		slot = ExecProcNode(state->children[state->current]);
		if (!TupIsNull(slot))
			return slot;

		state->current = bms_next_member(state->valid, state->current);
		if (state->current >= 0)
			state->nscanned++;
	}

	return NULL;
}

static void
end_dynamic_scan(CustomScanState *node)
{
	DynamicScanState *state = (DynamicScanState *) node;

	for (int i = 0; i < state->nchildren; i++)
		ExecEndNode(state->children[i]);
}

static void
rescan_dynamic_scan(CustomScanState *node)
{
	DynamicScanState *state = (DynamicScanState *) node;

	/*
	 * As ExecReScanAppend: a child a changed parameter reaches rescans
	 * itself when next asked for a row.  A selector may have chosen again,
	 * so the choice is made again too.
	 */
	for (int i = 0; i < state->nchildren; i++)
	{
		PlanState  *child = state->children[i];

		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
		if (child->chgParam == NULL)
			ExecReScan(child);
	}

	state->chosen = false;
	state->valid = NULL;
	state->current = -1;
}

/* What EXPLAIN calls a Dynamic Scan, by the scan ORCA planned for the table. */
static const char *
dynamic_scan_name(CustomScan *cscan)
{
	switch ((NodeTag) intVal(lthird(cscan->custom_private)))
	{
		case T_SeqScan:
			return "Dynamic Seq Scan";
		case T_IndexScan:
			return "Dynamic Index Scan";
		case T_IndexOnlyScan:
			return "Dynamic Index Only Scan";
		case T_BitmapHeapScan:
			return "Dynamic Bitmap Heap Scan";
		default:
			return NULL;
	}
}

/* The index a Dynamic Scan's index or index-only scans are of, or 0. */
static Oid
dynamic_scan_index(CustomScan *cscan)
{
	return (Oid) intVal(lfourth(cscan->custom_private));
}

/* The table a Dynamic Scan reads: its range table index. */
static Index
dynamic_scan_table(CustomScan *cscan)
{
	return (Index) bms_singleton_member(cscan->custom_relids);
}

/* As explain.c's explain_get_index_name(), which is static. */
static const char *
index_name(Oid index_oid)
{
	const char *result = NULL;

	if (explain_get_index_name_hook)
		result = explain_get_index_name_hook(index_oid);
	if (result == NULL)
	{
		result = get_rel_name(index_oid);
		if (result == NULL)
			elog(ERROR, "cache lookup failed for index %u", index_oid);
	}
	return result;
}

/*
 * The index and the table a Dynamic Scan reads, as EXPLAIN names the index
 * and the target of a scan (explain.c, ExplainIndexScanDetails() and
 * ExplainTargetRel()): in text, appended to `text`; otherwise as properties,
 * the ones those functions write.
 */
static void
explain_dynamic_scan_target(CustomScan *cscan, ExplainState *es,
							StringInfo text)
{
	Index		rti = dynamic_scan_table(cscan);
	RangeTblEntry *rte = rt_fetch(rti, es->rtable);
	char	   *refname = (char *) list_nth(es->rtable_names, rti - 1);
	char	   *objectname = get_rel_name(rte->relid);
	char	   *namespace = NULL;
	Oid			index_oid = dynamic_scan_index(cscan);

	Assert(rte->rtekind == RTE_RELATION);
	if (refname == NULL)
		refname = rte->eref->aliasname;
	if (es->verbose)
		namespace = get_namespace_name_or_temp(get_rel_namespace(rte->relid));

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		/*
		 * The index first, as Cloudberry's "Dynamic Index Scan on i on t"
		 * puts it; quoted, which Cloudberry's is not, as every other name
		 * EXPLAIN prints is.
		 */
		if (OidIsValid(index_oid))
			appendStringInfo(text, " on %s",
							 quote_identifier(index_name(index_oid)));
		appendStringInfoString(text, " on");
		if (namespace != NULL)
			appendStringInfo(text, " %s.%s", quote_identifier(namespace),
							 quote_identifier(objectname));
		else
			appendStringInfo(text, " %s", quote_identifier(objectname));
		if (strcmp(refname, objectname) != 0)
			appendStringInfo(text, " %s", quote_identifier(refname));
	}
	else
	{
		if (OidIsValid(index_oid))
			ExplainPropertyText("Index Name", index_name(index_oid), es);
		ExplainPropertyText("Relation Name", objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", refname, es);
	}
}

/* The parameters in `paramids`, spelled as EXPLAIN spells a parameter. */
static List *
param_names(List *paramids)
{
	List	   *names = NIL;
	ListCell   *lc;

	foreach(lc, paramids)
		names = lappend(names, psprintf("$%d", lfirst_int(lc)));
	return names;
}

static void
explain_dynamic_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	DynamicScanState *state = (DynamicScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	RangeTblEntry *rte = rt_fetch(dynamic_scan_table(cscan), es->rtable);
	List	   *paramids = (List *) lsecond(cscan->custom_private);

	/* in text, the node's name says this, through gp_orca_label_dynamic_scans */
	if (es->format != EXPLAIN_FORMAT_TEXT)
		explain_dynamic_scan_target(cscan, es, NULL);

	/*
	 * Cloudberry's words: the partitions static pruning left, out of the
	 * table's partitions, counted as its countLeafPartTables() counts them
	 * (github/cloudberry/src/backend/commands/explain.c:2546-2565,6258-6268).
	 */
	ExplainPropertyInteger("Number of partitions to scan",
						   psprintf("(out of %d)",
									list_length(find_all_inheritors(rte->relid,
																	NoLock,
																	NULL)) - 1),
						   list_length(cscan->custom_plans), es);

	/*
	 * Which Partition Selectors choose among them, by the parameter each
	 * hands its choice over in, as Cloudberry's EXPLAIN names the selectors
	 * of an Append (the same file, 4527-4546).  Its dynamic scans say nothing
	 * of their selectors; this one prints its partitions' scans too, so it
	 * reads differently from Cloudberry's in any case.
	 */
	if (paramids != NIL)
		ExplainPropertyList("Partition Selectors", param_names(paramids), es);
	if (es->analyze)
		ExplainPropertyInteger("Partitions Scanned", NULL, state->nscanned, es);
}

/* ------------------------------------------------------------------------- */
/* Partition Selector                                                        */
/* ------------------------------------------------------------------------- */


static Node *create_partition_selector_state(CustomScan *cscan);
static void begin_partition_selector(CustomScanState *node, EState *estate,
									 int eflags);
static TupleTableSlot *exec_partition_selector(CustomScanState *node);
static void end_partition_selector(CustomScanState *node);
static void rescan_partition_selector(CustomScanState *node);
static void explain_partition_selector(CustomScanState *node, List *ancestors,
									   ExplainState *es);

const CustomScanMethods gp_orca_partition_selector_methods = {
	.CustomName = "Partition Selector",
	.CreateCustomScanState = create_partition_selector_state,
};

static const CustomExecMethods partition_selector_exec_methods = {
	.CustomName = "Partition Selector",
	.BeginCustomScan = begin_partition_selector,
	.ExecCustomScan = exec_partition_selector,
	.EndCustomScan = end_partition_selector,
	.ReScanCustomScan = rescan_partition_selector,
	.ExplainCustomScan = explain_partition_selector,
};

static Node *
create_partition_selector_state(CustomScan *cscan)
{
	PartitionSelectorState *state = palloc0_object(PartitionSelectorState);

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &partition_selector_exec_methods;

	return (Node *) state;
}

/*
 * The pruning context for the table, as execPartition.c's
 * InitPartitionPruneContext() makes one for an Append, with each step's
 * expressions compiled against this node, so that a Var of the row it is
 * handed reads the row.
 */
static void
init_prune_context(PartitionSelectorState *state, PartitionDesc partdesc,
				   PartitionKey partkey)
{
	PartitionPruneContext *context = &state->context;
	int			n_steps = list_length(state->steps);
	int			partnatts = partkey->partnatts;
	ListCell   *lc;

	context->strategy = partkey->strategy;
	context->partnatts = partnatts;
	context->nparts = partdesc->nparts;
	context->boundinfo = partdesc->boundinfo;
	context->partcollation = partkey->partcollation;
	context->partsupfunc = partkey->partsupfunc;
	context->stepcmpfuncs = palloc0_array(FmgrInfo, n_steps * partnatts);
	context->ppccontext = CurrentMemoryContext;
	context->planstate = &state->css.ss.ps;
	context->exprcontext = state->css.ss.ps.ps_ExprContext;
	context->exprstates = palloc0_array(ExprState *, n_steps * partnatts);

	foreach(lc, state->steps)
	{
		PartitionPruneStepOp *step = (PartitionPruneStepOp *) lfirst(lc);
		ListCell   *lc2;
		int			keyno = 0;

		if (!IsA(step, PartitionPruneStepOp))
			continue;

		foreach(lc2, step->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc2);

			while (bms_is_member(keyno, step->nullkeys))
				keyno++;

			if (!IsA(expr, Const))
				context->exprstates[PruneCxtStateIdx(partnatts,
													 step->step.step_id,
													 keyno)] =
					ExecInitExpr(expr, context->planstate);
			keyno++;
		}
	}
}

static void
begin_partition_selector(CustomScanState *node, EState *estate, int eflags)
{
	PartitionSelectorState *state = (PartitionSelectorState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	PartitionDesc partdesc;

	Assert(outerPlan(cscan) != NULL);
	Assert(list_length(cscan->custom_private) == 3);

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);

	state->paramid = intVal(linitial(cscan->custom_private));
	state->steps = (List *) lthird(cscan->custom_private);

	/* Locked by the plan's range table, as the table's own entry is there. */
	state->rel = table_open((Oid) intVal(lsecond(cscan->custom_private)),
							NoLock);
	if (estate->es_partition_directory == NULL)
		estate->es_partition_directory =
			CreatePartitionDirectory(estate->es_query_cxt, false);
	partdesc = PartitionDirectoryLookup(estate->es_partition_directory,
										state->rel);
	init_prune_context(state, partdesc, RelationGetPartitionKey(state->rel));

	/* nothing chosen yet */
	estate->es_param_exec_vals[state->paramid].value = (Datum) 0;
	estate->es_param_exec_vals[state->paramid].isnull = false;
}

static TupleTableSlot *
exec_partition_selector(CustomScanState *node)
{
	PartitionSelectorState *state = (PartitionSelectorState *) node;
	EState	   *estate = node->ss.ps.state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *slot;
	Bitmapset  *matched;
	MemoryContext oldcontext;

	slot = ExecProcNode(outerPlanState(node));
	if (TupIsNull(slot))
	{
		/*
		 * The child is done, so the set is complete: hand it to the Dynamic
		 * Scan.  A selector that saw no rows chose no partition.
		 */
		if (!state->done)
		{
			estate->es_param_exec_vals[state->paramid].value =
				PointerGetDatum(state);
			state->done = true;
		}
		return NULL;
	}

	ResetExprContext(econtext);
	econtext->ecxt_outertuple = slot;

	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	matched = get_matching_partitions(&state->context, state->steps);
	MemoryContextSwitchTo(estate->es_query_cxt);
	state->selected = bms_add_members(state->selected, matched);
	MemoryContextSwitchTo(oldcontext);

	if (node->ss.ps.ps_ProjInfo != NULL)
		return ExecProject(node->ss.ps.ps_ProjInfo);
	return slot;
}

static void
end_partition_selector(CustomScanState *node)
{
	PartitionSelectorState *state = (PartitionSelectorState *) node;

	ExecEndNode(outerPlanState(node));
	table_close(state->rel, NoLock);
}

static void
rescan_partition_selector(CustomScanState *node)
{
	PartitionSelectorState *state = (PartitionSelectorState *) node;
	EState	   *estate = node->ss.ps.state;

	state->selected = NULL;
	state->done = false;
	estate->es_param_exec_vals[state->paramid].value = (Datum) 0;

	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

static void
explain_partition_selector(CustomScanState *node, List *ancestors,
						   ExplainState *es)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	/*
	 * In text, the node's name says this, through
	 * gp_orca_label_dynamic_scans; otherwise it is the property Cloudberry
	 * writes.
	 */
	if (es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyInteger("Selector ID", NULL,
							   intVal(linitial(cscan->custom_private)), es);
}

/* ------------------------------------------------------------------------- */

void
gp_orca_register_dynamic_scans(void)
{
	RegisterCustomScanMethods(&gp_orca_dynamic_scan_methods);
	RegisterCustomScanMethods(&gp_orca_partition_selector_methods);
}

bool
gp_orca_label_dynamic_scans(PlanState *planstate, ExplainState *es,
							const char **pname, const char **suffix)
{
	CustomScan *cscan = (CustomScan *) planstate->plan;

	/*
	 * Only text output prints a node's name.  Both nodes are claimed all the
	 * same, so that nobody else names them.
	 */
	if (cscan->methods == &gp_orca_partition_selector_methods)
	{
		/* github/cloudberry/src/backend/commands/explain.c:1972-1973,2254-2267 */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			*pname = "Partition Selector";
			*suffix = psprintf(" (selector id: $%d)",
							   intVal(linitial(cscan->custom_private)));
		}
		return true;
	}

	if (cscan->methods == &gp_orca_dynamic_scan_methods)
	{
		const char *name = dynamic_scan_name(cscan);

		/* the same file, 1690-1729 and 2111-2140 */
		if (es->format == EXPLAIN_FORMAT_TEXT && name != NULL)
		{
			StringInfoData target;

			initStringInfo(&target);
			explain_dynamic_scan_target(cscan, es, &target);
			*pname = name;
			*suffix = target.data;
		}
		return true;
	}

	return false;
}
