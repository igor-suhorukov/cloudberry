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
 * gp_split.c
 *	  An UPDATE that moves rows between segments: ORCA's Split.
 *
 * An UPDATE of a distribution key moves the row it changes to the segment
 * the new key hashes to.  Cloudberry does it as ORCA plans it: a Split Update
 * below a Redistribute Motion turns each row into two -- a DELETE, carrying
 * the old values, and an INSERT, carrying the new -- and the Motion hashes
 * each by the key it carries, so the DELETE goes back to the segment that
 * has the row and the INSERT to the one that will.  Cloudberry's ModifyTable
 * then does each row's action, told apart by a "DMLAction" column.
 *
 * PostgreSQL 19's ModifyTable has no per-row action, so both halves are
 * CustomScans here.  "Split Update" is Cloudberry's node.  "Update", which
 * EXPLAIN prints as ModifyTable's "Update on t", applies the rows a segment
 * receives: a DELETE by the row's ctid, an INSERT with the table's own
 * constraints, generated columns and indexes, and counts the INSERTs, which
 * are the rows the statement updated.  Triggers do not fire, as they do not
 * for Cloudberry's split update; the translator refuses a table that has any,
 * foreign keys included, since their checks are triggers.
 *
 * The planner's Split is the coordinator's (gp_explicit.c), and applies its
 * rows the same way through two functions each segment runs:
 * gp_internal.split_delete() deletes rows by their ctid and returns them,
 * and gp_internal.split_insert() inserts their new versions, routed into
 * the table's partitions as COPY routes them.  Neither fires a trigger or
 * applies a policy, as Cloudberry's Split does neither; the coordinator
 * checked the rows.  So each runs only for a connection that carries the
 * cluster secret, the coordinator's own.  A third, explicit_recheck(), says
 * why a statement of the coordinator's explicit write wrote fewer rows than
 * it was sent, failing it where another transaction changed one since the
 * coordinator read it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_motion.h"

/* ------------------------------------------------------------------------- */
/* Split Update: each row, twice                                             */
/* ------------------------------------------------------------------------- */

/*
 * The action column's values, Cloudberry's DMLAction.  Where an update
 * changes a key of the table's, ORCA sorts the rows a segment receives by
 * this column, ascending (CPhysicalDML::PosComputeRequired()), so that the
 * DELETE of a row's old version comes before any INSERT of a new one with
 * the same key -- an UPDATE that sets a unique key to the value it has, or
 * one that another row gives up.
 */
#define GP_DML_DELETE	0
#define GP_DML_INSERT	1

typedef struct SplitState
{
	CustomScanState css;
	List	   *deletecols;
	List	   *insertcols;
	AttrNumber	actioncol;
	TupleTableSlot *pending;	/* the INSERT half, after the DELETE went out */
	TupleTableSlot *deleteslot;
	TupleTableSlot *insertslot;
} SplitState;

static Node *split_create_state(CustomScan *cscan);
static void split_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *split_exec(CustomScanState *node);
static void split_end(CustomScanState *node);
static void split_rescan(CustomScanState *node);

static const CustomScanMethods split_scan_methods = {
	.CustomName = "GpSplitUpdate",
	.CreateCustomScanState = split_create_state,
};

static const CustomExecMethods split_exec_methods = {
	.CustomName = "GpSplitUpdate",
	.BeginCustomScan = split_begin,
	.ExecCustomScan = split_exec,
	.EndCustomScan = split_end,
	.ReScanCustomScan = split_rescan,
};

/* Its expressions read the child's row as the scan tuple, as a scan's do. */
static Node *
outer_to_index(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *var = (Var *) copyObject(node);

		var->varno = INDEX_VAR;
		return (Node *) var;
	}
	return expression_tree_mutator(node, outer_to_index, context);
}

/* The child's columns, as the scan tuple custom_scan_tlist describes. */
static List *
child_columns(Plan *child)
{
	List	   *tlist = NIL;
	ListCell   *lc;

	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
														 exprType((Node *) tle->expr),
														 exprTypmod((Node *) tle->expr),
														 exprCollation((Node *) tle->expr),
														 0),
										list_length(tlist) + 1,
										tle->resname, false));
	}
	return tlist;
}

Plan *
GpSplitMake(Plan *child, List *targetlist, List *deletecols, List *insertcols,
			AttrNumber actioncol)
{
	CustomScan *cscan = makeNode(CustomScan);

	Assert(list_length(deletecols) == list_length(insertcols));

	cscan->scan.plan.targetlist = (List *) outer_to_index((Node *) targetlist,
														  NULL);
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = child_columns(child);
	cscan->custom_private = list_make3(deletecols, insertcols,
									   makeInteger(actioncol));
	cscan->methods = &split_scan_methods;
	return (Plan *) cscan;
}

static Node *
split_create_state(CustomScan *cscan)
{
	SplitState *state = (SplitState *) newNode(sizeof(SplitState),
											   T_CustomScanState);

	state->css.methods = &split_exec_methods;
	return (Node *) state;
}

static void
split_begin(CustomScanState *node, EState *estate, int eflags)
{
	SplitState *state = (SplitState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	TupleDesc	tupdesc = ExecTypeFromTL(cscan->scan.plan.targetlist);

	state->deletecols = (List *) linitial(cscan->custom_private);
	state->insertcols = (List *) lsecond(cscan->custom_private);
	state->actioncol = (AttrNumber) intVal(lthird(cscan->custom_private));
	state->deleteslot = ExecInitExtraTupleSlot(estate, tupdesc, &TTSOpsVirtual);
	state->insertslot = ExecInitExtraTupleSlot(estate, tupdesc, &TTSOpsVirtual);

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
}

/*
 * Cloudberry's SplitTupleTableSlot(): each column of the output is the old
 * value in the DELETE and the new in the INSERT, where the Split says which
 * input columns those are; the action column says which is which; anything
 * else passes through to both.
 */
static TupleTableSlot *
split_exec(CustomScanState *node)
{
	SplitState *state = (SplitState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	TupleTableSlot *child;
	TupleTableSlot *del = state->deleteslot;
	TupleTableSlot *ins = state->insertslot;
	ListCell   *lc;
	int			k = 0;

	if (state->pending != NULL)
	{
		TupleTableSlot *out = state->pending;

		state->pending = NULL;
		return out;
	}

	child = ExecProcNode(outerPlanState(node));
	if (TupIsNull(child))
		return NULL;
	slot_getallattrs(child);

	ExecClearTuple(del);
	ExecClearTuple(ins);
	foreach(lc, cscan->scan.plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		int			att = tle->resno - 1;

		if (tle->resno == state->actioncol)
		{
			del->tts_values[att] = Int32GetDatum(GP_DML_DELETE);
			del->tts_isnull[att] = false;
			ins->tts_values[att] = Int32GetDatum(GP_DML_INSERT);
			ins->tts_isnull[att] = false;
		}
		else if (tle->resno <= list_length(state->insertcols))
		{
			int			delatt = list_nth_int(state->deletecols, k);
			int			insatt = list_nth_int(state->insertcols, k);

			if (delatt == -1)
			{
				del->tts_values[att] = (Datum) 0;
				del->tts_isnull[att] = true;
			}
			else
			{
				del->tts_values[att] = child->tts_values[delatt - 1];
				del->tts_isnull[att] = child->tts_isnull[delatt - 1];
			}
			ins->tts_values[att] = child->tts_values[insatt - 1];
			ins->tts_isnull[att] = child->tts_isnull[insatt - 1];
			k++;
		}
		else if (IsA(tle->expr, Var))
		{
			AttrNumber	from = ((Var *) tle->expr)->varattno;

			del->tts_values[att] = child->tts_values[from - 1];
			del->tts_isnull[att] = child->tts_isnull[from - 1];
			ins->tts_values[att] = child->tts_values[from - 1];
			ins->tts_isnull[att] = child->tts_isnull[from - 1];
		}
		else
		{
			del->tts_values[att] = ins->tts_values[att] = (Datum) 0;
			del->tts_isnull[att] = ins->tts_isnull[att] = true;
		}
	}
	ExecStoreVirtualTuple(del);
	ExecStoreVirtualTuple(ins);

	/* the DELETE first, so that a key that stays unique stays so */
	state->pending = ins;
	return del;
}

static void
split_end(CustomScanState *node)
{
	ExecEndNode(outerPlanState(node));
}

static void
split_rescan(CustomScanState *node)
{
	((SplitState *) node)->pending = NULL;
	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

/* ------------------------------------------------------------------------- */
/* Update: what a segment receives, applied                                  */
/* ------------------------------------------------------------------------- */

typedef struct SplitModifyState
{
	CustomScanState css;
	Index		rti;
	int			natts;			/* the table's attributes, first in each row */
	AttrNumber	actioncol;
	AttrNumber	ctidcol;
	AttrNumber	tableoidcol;	/* a partitioned table's: the row's partition */
	ResultRelInfo *rri;
	TupleTableSlot *newslot;

	/*
	 * A partitioned table's: each DELETE's partition, opened as it comes,
	 * and the routing of each INSERT into its partition, as an INSERT into
	 * the table routes it.
	 */
	List	   *partitions;		/* SplitPartition */
	List	   *tree;			/* the table and its partitions' OIDs */
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute;
} SplitModifyState;

typedef struct SplitPartition
{
	Oid			relid;
	Relation	rel;
} SplitPartition;

static Node *split_modify_create_state(CustomScan *cscan);
static void split_modify_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *split_modify_exec(CustomScanState *node);
static void split_modify_end(CustomScanState *node);
static void split_modify_rescan(CustomScanState *node);

static const CustomScanMethods split_modify_scan_methods = {
	.CustomName = "GpSplitModify",
	.CreateCustomScanState = split_modify_create_state,
};

static const CustomExecMethods split_modify_exec_methods = {
	.CustomName = "GpSplitModify",
	.BeginCustomScan = split_modify_begin,
	.ExecCustomScan = split_modify_exec,
	.EndCustomScan = split_modify_end,
	.ReScanCustomScan = split_modify_rescan,
};

Plan *
GpSplitModifyMake(Plan *child, Index rti, int natts, AttrNumber actioncol,
				  AttrNumber ctidcol)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = NIL;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = child_columns(child);
	cscan->custom_private = list_make5(makeInteger(rti), makeInteger(natts),
									   makeInteger(actioncol),
									   makeInteger(ctidcol),
									   makeInteger(InvalidAttrNumber));
	cscan->methods = &split_modify_scan_methods;
	return (Plan *) cscan;
}

/*
 * A partitioned table's split update: the column that says which partition
 * each DELETE's row is in, by its tableoid.  The INSERTs are routed.
 */
void
GpSplitModifySetTableOid(Plan *plan, AttrNumber tableoidcol)
{
	if (!GpSplitModifyIs(plan, NULL))
		elog(ERROR, "not a split update's node");
	lfirst(list_nth_cell(((CustomScan *) plan)->custom_private, 4)) =
		makeInteger(tableoidcol);
}

bool
GpSplitModifyIs(Plan *plan, Index *rti)
{
	if (plan == NULL || !IsA(plan, CustomScan) ||
		((CustomScan *) plan)->methods != &split_modify_scan_methods)
		return false;
	if (rti != NULL)
		*rti = (Index) intVal(linitial(((CustomScan *) plan)->custom_private));
	return true;
}

static Node *
split_modify_create_state(CustomScan *cscan)
{
	SplitModifyState *state = (SplitModifyState *) newNode(sizeof(SplitModifyState),
														   T_CustomScanState);

	state->css.methods = &split_modify_exec_methods;
	return (Node *) state;
}

static void
split_modify_begin(CustomScanState *node, EState *estate, int eflags)
{
	SplitModifyState *state = (SplitModifyState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;

	state->rti = (Index) intVal(linitial(priv));
	state->natts = intVal(lsecond(priv));
	state->actioncol = (AttrNumber) intVal(lthird(priv));
	state->ctidcol = (AttrNumber) intVal(lfourth(priv));
	state->tableoidcol = (AttrNumber) intVal(list_nth(priv, 4));

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* The result relation, as ModifyTable opens it, and its indexes. */
	state->rri = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, state->rri, state->rti);
	CheckValidResultRel(state->rri, CMD_UPDATE, ONCONFLICT_NONE, NIL);
	state->newslot = ExecInitExtraTupleSlot(estate,
											RelationGetDescr(state->rri->ri_RelationDesc),
											&TTSOpsVirtual);
	if (state->rri->ri_RelationDesc->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		ExecOpenIndices(state->rri, false);
		return;
	}

	/*
	 * A partitioned table's rows are its partitions': a DELETE's is found in
	 * the one its tableoid names, and an INSERT goes where the table's
	 * routing sends it, as an INSERT into the table would -- set up as
	 * gp_internal.split_insert() sets it up, for a ModifyTable that is not
	 * there.
	 */
	if (state->tableoidcol == InvalidAttrNumber)
		elog(ERROR, "a partitioned table's split update has no tableoid");
	state->tree = find_all_inheritors(RelationGetRelid(state->rri->ri_RelationDesc),
									  NoLock, NULL);
	state->mtstate = makeNode(ModifyTableState);
	state->mtstate->ps.plan = NULL;
	state->mtstate->ps.state = estate;
	state->mtstate->operation = CMD_INSERT;
	state->mtstate->mt_nrels = 1;
	state->mtstate->resultRelInfo = state->rri;
	state->mtstate->rootResultRelInfo = state->rri;
	state->proute = ExecSetupPartitionTupleRouting(estate,
												   state->rri->ri_RelationDesc);
}

/* The partition a partitioned table's DELETE finds its row in, opened once. */
static Relation
split_partition(SplitModifyState *state, Oid relid)
{
	SplitPartition *part;

	foreach_ptr(SplitPartition, p, state->partitions)
		if (p->relid == relid)
			return p->rel;
	if (!list_member_oid(state->tree, relid))
		elog(ERROR, "relation %u is not \"%s\" or one of its partitions",
			 relid, RelationGetRelationName(state->rri->ri_RelationDesc));
	part = palloc0_object(SplitPartition);
	part->relid = relid;
	part->rel = table_open(relid, RowExclusiveLock);
	state->partitions = lappend(state->partitions, part);
	return part->rel;
}

/*
 * A row the statement's join found twice, and moved once already: which of
 * its new versions would win is the join's order, so Cloudberry refuses it
 * (nodeModifyTable.c, ExecDelete() under a split update), in these words and
 * with this code, and so does this.  PostgreSQL's UPDATE, which changes the
 * row where it is, changes it once and passes the second over.
 */
static void
split_multiple_updates(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
			 errmsg("multiple updates to a row by the same query is not allowed")));
}

/*
 * A row a Split would move that another transaction changed since the
 * statement's snapshot, refused as Cloudberry's ExecDelete() refuses it
 * under a split update: where the transaction's snapshot is its first, as
 * PostgreSQL refuses it; updated, as the recheck that meets the Motion below
 * the write refuses it (GpMotionRefuseRecheck()), a split's new version
 * being one no recheck could take back; deleted, in words of its own.
 */
static void
split_changed_concurrently(TM_Result result)
{
	if (IsolationUsesXactSnapshot())
	{
		if (result == TM_Deleted)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to concurrent delete")));
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to concurrent update")));
	}
	if (result == TM_Deleted)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("could not split update tuple which has been deleted by other transaction")));
	GpMotionRefuseRecheck();
}

static void
split_delete(SplitModifyState *state, Relation rel, ItemPointer tid)
{
	EState	   *estate = state->css.ss.ps.state;
	TM_FailureData tmfd;
	TM_Result	result;

	result = table_tuple_delete(rel, tid, estate->es_output_cid, 0,
								estate->es_snapshot, estate->es_crosscheck_snapshot,
								true, &tmfd);
	switch (result)
	{
		case TM_Ok:
			break;
		case TM_SelfModified:
			/* this statement's own, as another row of its join moved it */
			if (tmfd.cmax == estate->es_output_cid)
				split_multiple_updates();
			ereport(ERROR,
					(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
					 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
			break;
		case TM_Updated:
		case TM_Deleted:
			split_changed_concurrently(result);
			break;
		default:
			elog(ERROR, "unexpected table_tuple_delete status: %u", result);
	}
}

static void
split_insert(SplitModifyState *state, TupleTableSlot *row)
{
	EState	   *estate = state->css.ss.ps.state;
	ResultRelInfo *rri = state->rri;
	Relation	rel = rri->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TupleTableSlot *slot = state->newslot;

	ExecClearTuple(slot);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped || i >= state->natts)
		{
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
		}
		else
		{
			slot->tts_values[i] = row->tts_values[i];
			slot->tts_isnull[i] = row->tts_isnull[i];
		}
	}
	ExecStoreVirtualTuple(slot);

	/*
	 * A partitioned table's new row goes to the partition the table's
	 * routing chooses, in that partition's row shape.
	 */
	if (state->proute != NULL)
	{
		TupleConversionMap *map;

		rri = ExecFindPartition(state->mtstate, state->rri, state->proute,
								slot, estate);
		map = ExecGetRootToChildMap(rri, estate);
		if (map != NULL)
			slot = execute_attr_map_slot(map->attrMap, slot,
										 rri->ri_PartitionTupleSlot);
		rel = rri->ri_RelationDesc;
		tupdesc = RelationGetDescr(rel);
		slot->tts_tableOid = RelationGetRelid(rel);
	}
	ExecMaterializeSlot(slot);

	/* a new row: every generated column computed, every index given it */
	if (tupdesc->constr && tupdesc->constr->has_generated_stored)
		ExecComputeStoredGenerated(rri, estate, slot, CMD_INSERT);
	if (tupdesc->constr)
		ExecConstraints(rri, slot, estate);

	table_tuple_insert(rel, slot, estate->es_output_cid, 0, NULL);
	if (rri->ri_NumIndices > 0)
		(void) ExecInsertIndexTuples(rri, estate, 0, slot, NIL, NULL);
}

static TupleTableSlot *
split_modify_exec(CustomScanState *node)
{
	SplitModifyState *state = (SplitModifyState *) node;
	EState	   *estate = node->ss.ps.state;

	for (;;)
	{
		TupleTableSlot *row = ExecProcNode(outerPlanState(node));
		bool		isnull;
		int			action;

		if (TupIsNull(row))
			return NULL;
		slot_getallattrs(row);

		action = DatumGetInt32(slot_getattr(row, state->actioncol, &isnull));
		if (isnull)
			elog(ERROR, "a split update's row has no action");

		if (action == GP_DML_DELETE)
		{
			Datum		ctid = slot_getattr(row, state->ctidcol, &isnull);
			Relation	rel = state->rri->ri_RelationDesc;

			if (isnull)
				elog(ERROR, "a split update's DELETE has no ctid");
			if (state->proute != NULL)
			{
				Datum		tableoid = slot_getattr(row, state->tableoidcol,
													&isnull);

				if (isnull)
					elog(ERROR, "a split update's DELETE has no tableoid");
				rel = split_partition(state, DatumGetObjectId(tableoid));
			}
			split_delete(state, rel, DatumGetItemPointer(ctid));
		}
		else if (action == GP_DML_INSERT)
		{
			split_insert(state, row);
			estate->es_processed++;
		}
		else
			elog(ERROR, "a split update's row has action %d", action);

		ResetExprContext(node->ss.ps.ps_ExprContext);
	}
}

static void
split_modify_end(CustomScanState *node)
{
	SplitModifyState *state = (SplitModifyState *) node;

	ExecEndNode(outerPlanState(node));
	if (state->proute != NULL)
		ExecCleanupTupleRouting(state->mtstate, state->proute);
	foreach_ptr(SplitPartition, p, state->partitions)
		table_close(p->rel, NoLock);
}

static void
split_modify_rescan(CustomScanState *node)
{
	elog(ERROR, "a split update cannot be run again");
}

/*
 * EXPLAIN's names for them, through O4: Cloudberry's "Split Update", and
 * "Update on t" for what applies it, as ModifyTable is printed.
 */
bool
GpSplitExplainLabel(PlanState *planstate, ExplainState *es,
					const char **pname, const char **suffix)
{
	CustomScanState *css = (CustomScanState *) planstate;

	if (!IsA(planstate, CustomScanState))
		return false;
	if (css->methods == &split_exec_methods)
	{
		*pname = "Split Update";
		return true;
	}
	if (css->methods == &split_modify_exec_methods)
	{
		SplitModifyState *state = (SplitModifyState *) css;
		RangeTblEntry *rte = rt_fetch(state->rti, es->rtable);

		*pname = "Update";
		*suffix = psprintf(" on %s", quote_identifier(get_rel_name(rte->relid)));
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* The planner's Split, on a segment                                         */
/* ------------------------------------------------------------------------- */

/*
 * The table a split function is called for -- NULL::t says which -- once it
 * is known that the coordinator called it, for a user who may update the
 * table, with arrays, none of them NULL, which are read after this.
 */
static Oid
split_target(FunctionCallInfo fcinfo, const char *name)
{
	Oid			rowtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(rowtype) ? typeidTypeRelid(rowtype) : InvalidOid;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s() moves rows only for the coordinator", name),
				 errdetail("The connection does not carry this cluster's secret."),
				 errhint("Set \"gp.cluster_secret\" to the same value on every node.")));
	if (!OidIsValid(relid))
		elog(ERROR, "%s() is not given a table's row type", name);
	if (pg_class_aclcheck(relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)),
					   get_rel_name(relid));
	for (int i = 1; i < PG_NARGS(); i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("%s()'s arrays must not be null", name)));
	return relid;
}

/* The arrays a split function is given, each as long as the first. */
static void
split_array(ArrayType *array, Oid elemtype, Datum **elems, int *n)
{
	int16		typlen;
	bool		typbyval;
	char		typalign;
	bool	   *nulls;
	int			count;

	get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);
	deconstruct_array(array, elemtype, typlen, typbyval, typalign,
					  elems, &nulls, &count);
	for (int i = 0; i < count; i++)
		if (nulls[i])
			elog(ERROR, "a split's array holds a null");
	if (*n >= 0 && count != *n)
		elog(ERROR, "a split's arrays differ in length");
	*n = count;
}

/* A table of the tree a split writes, opened once, and its row's shape. */
typedef struct SplitTable
{
	Oid			relid;
	Relation	rel;
	TupleTableSlot *slot;		/* the table's own */
	TupleConversionMap *toroot; /* its row to the root's, or NULL */
	ResultRelInfo *rri;			/* an INSERT's */
	TupleConversionMap *fromroot;	/* the root's row to its, or NULL */
	TupleTableSlot *inslot;		/* and a slot for that */
} SplitTable;

static SplitTable *
split_table(List **parts, Relation root, Oid relid, List *tree)
{
	SplitTable *part;

	foreach_ptr(SplitTable, p, *parts)
		if (p->relid == relid)
			return p;
	if (!list_member_oid(tree, relid))
		elog(ERROR, "relation %u is not \"%s\" or one of its partitions",
			 relid, RelationGetRelationName(root));

	part = palloc0_object(SplitTable);
	part->relid = relid;
	part->rel = relid == RelationGetRelid(root) ? root
		: table_open(relid, RowExclusiveLock);
	part->slot = table_slot_create(part->rel, NULL);
	part->toroot = convert_tuples_by_name(RelationGetDescr(part->rel),
										  RelationGetDescr(root));
	*parts = lappend(*parts, part);
	return part;
}

static void
split_tables_close(List *parts, Relation root)
{
	foreach_ptr(SplitTable, p, parts)
	{
		ExecDropSingleTupleTableSlot(p->slot);
		if (p->inslot != NULL)
			ExecDropSingleTupleTableSlot(p->inslot);
		if (p->rel != root)
			table_close(p->rel, NoLock);
	}
}

/* A row of the root's, as the composite value a split function returns. */
static Datum
split_root_row(TupleTableSlot *slot, TupleConversionMap *toroot,
			   TupleTableSlot *rootslot)
{
	TupleDesc	rootdesc = rootslot->tts_tupleDescriptor;
	HeapTuple	tuple;

	slot_getallattrs(slot);
	if (toroot != NULL)
		slot = execute_attr_map_slot(toroot->attrMap, slot, rootslot);
	tuple = heap_form_tuple(rootdesc, slot->tts_values, slot->tts_isnull);
	return heap_copy_tuple_as_datum(tuple, rootdesc);
}

PG_FUNCTION_INFO_V1(gp_split_delete);

/*
 * gp_internal.split_delete(NULL::t, ctids, tables, numbers)
 *		A Split's DELETE half on this segment: each row the coordinator names,
 *		by its table and ctid, deleted as ORCA's Split deletes it, and returned
 *		with its number, its table and its ctid, as t's row.  A row this
 *		statement deleted already is passed over, as ExecDelete() passes it
 *		over.
 */
Datum
gp_split_delete(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = split_target(fcinfo, "gp_internal.split_delete");
	Datum	   *tids;
	Datum	   *toids;
	Datum	   *numbers;
	int			n = -1;
	Relation	root;
	List	   *tree;
	List	   *parts = NIL;
	TupleTableSlot *rootslot;
	Snapshot	snapshot = GetActiveSnapshot();
	CommandId	cid = GetCurrentCommandId(true);

	split_array(PG_GETARG_ARRAYTYPE_P(1), TIDOID, &tids, &n);
	split_array(PG_GETARG_ARRAYTYPE_P(2), OIDOID, &toids, &n);
	split_array(PG_GETARG_ARRAYTYPE_P(3), INT8OID, &numbers, &n);
	InitMaterializedSRF(fcinfo, 0);

	root = table_open(relid, RowExclusiveLock);
	tree = find_all_inheritors(relid, NoLock, NULL);
	rootslot = MakeSingleTupleTableSlot(RelationGetDescr(root), &TTSOpsVirtual);

	for (int i = 0; i < n; i++)
	{
		SplitTable *part = split_table(&parts, root, DatumGetObjectId(toids[i]), tree);
		ItemPointer tid = DatumGetItemPointer(tids[i]);
		TM_FailureData tmfd;
		TM_Result	result;
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		CHECK_FOR_INTERRUPTS();
		if (!table_tuple_fetch_row_version(part->rel, tid, snapshot, part->slot))
			continue;
		result = table_tuple_delete(part->rel, tid, cid, 0, snapshot,
									InvalidSnapshot, true, &tmfd);
		switch (result)
		{
			case TM_Ok:
				break;
			case TM_SelfModified:
				if (tmfd.cmax != cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
				split_multiple_updates();
				break;
			case TM_Updated:
			case TM_Deleted:
				split_changed_concurrently(result);
				break;
			default:
				elog(ERROR, "unexpected table_tuple_delete status: %u", result);
		}

		values[0] = numbers[i];
		values[1] = toids[i];
		values[2] = tids[i];
		values[3] = split_root_row(part->slot, part->toroot, rootslot);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	split_tables_close(parts, root);
	ExecDropSingleTupleTableSlot(rootslot);
	table_close(root, NoLock);
	return (Datum) 0;
}

/*
 * What heap_update() and heap_delete() would find at a row's version now;
 * one that is no longer there, what they find at a version another
 * transaction updated.
 */
static TM_Result
recheck_row(Relation rel, ItemPointer tid, CommandId cid)
{
	HeapTupleData tuple;
	Buffer		buffer;
	TM_Result	result;

	if (!ItemPointerIsValid(tid) ||
		ItemPointerGetBlockNumber(tid) >= RelationGetNumberOfBlocks(rel))
		return TM_Updated;
	tuple.t_self = *tid;
	if (!heap_fetch(rel, SnapshotAny, &tuple, &buffer, false))
		return TM_Updated;
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	result = HeapTupleSatisfiesUpdate(&tuple, cid, buffer);
	UnlockReleaseBuffer(buffer);
	return result;
}

PG_FUNCTION_INFO_V1(gp_explicit_recheck);

/*
 * gp_internal.explicit_recheck(NULL::t, ctids, tables, deleted)
 *		Why a statement of the explicit write (gp_explicit.c) wrote fewer of
 *		the rows it sent this segment than it sent.  The statement found
 *		each row by the ctid the coordinator read it at, under the
 *		coordinator's snapshot; at READ COMMITTED a row another transaction
 *		changed since is rechecked in its new version, which that ctid never
 *		matches, and passed over -- and the new values the coordinator
 *		computed from the version it read are lost.  So each row's version is
 *		looked at as heap_update() finds it.  One another transaction
 *		updated, or moved to another partition, fails the statement, as the
 *		recheck below Cloudberry's Explicit Redistribute Motion fails it.  One
 *		another transaction deleted is passed over, as PostgreSQL passes it
 *		over, but for a Split's ("deleted" 's'), which Cloudberry refuses,
 *		and a MERGE's that would have tried its NOT MATCHED actions for it
 *		('m'), a serialization failure the client may retry.  One this
 *		transaction wrote, or a trigger or a policy kept from being written,
 *		is passed over.  A version that is no longer there was pruned once
 *		the statement's snapshot was let go, so another transaction updated
 *		or deleted it, which cannot be told apart: it fails the statement as
 *		an update does.  A table whose rows are not heap's is not looked at,
 *		and fails it so.
 */
Datum
gp_explicit_recheck(PG_FUNCTION_ARGS)
{
	Oid			rowtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(rowtype) ? typeidTypeRelid(rowtype) : InvalidOid;
	CommandId	cid = GetCurrentCommandId(false);
	Datum	   *tids;
	Datum	   *toids;
	int			n = -1;
	char		deleted;
	List	   *tree;
	List	   *rels = NIL;

	if (!OidIsValid(relid))
		elog(ERROR, "gp_internal.explicit_recheck() is not given a table's row type");
	for (int i = 1; i < PG_NARGS(); i++)
		if (PG_ARGISNULL(i))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("gp_internal.explicit_recheck()'s arguments must not be null")));
	/* who may change the table's rows may ask what became of them */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_UPDATE | ACL_DELETE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)),
					   get_rel_name(relid));
	split_array(PG_GETARG_ARRAYTYPE_P(1), TIDOID, &tids, &n);
	split_array(PG_GETARG_ARRAYTYPE_P(2), OIDOID, &toids, &n);
	deleted = PG_GETARG_CHAR(3);

	tree = find_all_inheritors(relid, NoLock, NULL);
	for (int i = 0; i < n; i++)
	{
		Oid			toid = DatumGetObjectId(toids[i]);
		Relation	rel = NULL;
		TM_Result	result;

		CHECK_FOR_INTERRUPTS();
		foreach_ptr(RelationData, r, rels)
			if (RelationGetRelid(r) == toid)
				rel = r;
		if (rel == NULL)
		{
			if (!list_member_oid(tree, toid))
				elog(ERROR, "relation %u is not \"%s\" or one of its partitions",
					 toid, get_rel_name(relid));
			rel = table_open(toid, AccessShareLock);
			rels = lappend(rels, rel);
		}

		result = rel->rd_rel->relam == HEAP_TABLE_AM_OID
			? recheck_row(rel, DatumGetItemPointer(tids[i]), cid)
			: TM_Updated;
		if (result == TM_Updated || (result == TM_Deleted && deleted == 's'))
		{
			if (deleted == 's')
				split_changed_concurrently(result);
			GpMotionRefuseRecheck();
		}
		if (result == TM_Deleted && deleted == 'm')
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to concurrent delete")));
	}

	foreach_ptr(RelationData, r, rels)
		table_close(r, NoLock);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_split_insert);

/*
 * gp_internal.split_insert(NULL::t, rows, tables, numbers)
 *		A Split's INSERT half on this segment: each new version, t's row,
 *		inserted as ORCA's Split inserts it -- its generated columns computed,
 *		its constraints checked, its index entries made -- into the partition
 *		t's routing gives it, or where the old version was, for a table of an
 *		inheritance tree; and returned with its number, its table and its
 *		ctid, as t's row.
 */
Datum
gp_split_insert(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = split_target(fcinfo, "gp_internal.split_insert");
	ArrayType  *rowarray = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *rows;
	Datum	   *toids;
	Datum	   *numbers;
	int			n = -1;
	EState	   *estate;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ResultRelInfo *rootrri;
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute = NULL;
	Relation	root;
	TupleDesc	rootdesc;
	TupleTableSlot *rootslot;
	TupleTableSlot *outslot;
	List	   *tree;
	List	   *parts = NIL;

	split_array(rowarray, ARR_ELEMTYPE(rowarray), &rows, &n);
	split_array(PG_GETARG_ARRAYTYPE_P(2), OIDOID, &toids, &n);
	split_array(PG_GETARG_ARRAYTYPE_P(3), INT8OID, &numbers, &n);
	InitMaterializedSRF(fcinfo, 0);

	/* the table as COPY FROM sets it up: a range table of it, its indexes */
	root = table_open(relid, RowExclusiveLock);
	rootdesc = RelationGetDescr(root);
	if (ARR_ELEMTYPE(rowarray) != rootdesc->tdtypeid)
		elog(ERROR, "gp_internal.split_insert() is given rows of another type");
	estate = CreateExecutorState();
	estate->es_output_cid = GetCurrentCommandId(true);
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, root, RowExclusiveLock,
										   NULL, false, false);
	nsitem->p_perminfo->requiredPerms = ACL_INSERT;
	ExecInitRangeTable(estate, pstate->p_rtable, pstate->p_rteperminfos,
					   bms_make_singleton(1));
	rootrri = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, rootrri, 1);
	ExecOpenIndices(rootrri, false);

	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = CMD_INSERT;
	mtstate->mt_nrels = 1;
	mtstate->resultRelInfo = rootrri;
	mtstate->rootResultRelInfo = rootrri;
	if (root->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		proute = ExecSetupPartitionTupleRouting(estate, root);
	tree = find_all_inheritors(relid, NoLock, NULL);

	rootslot = ExecInitExtraTupleSlot(estate, rootdesc, &TTSOpsVirtual);
	outslot = MakeSingleTupleTableSlot(rootdesc, &TTSOpsVirtual);

	for (int i = 0; i < n; i++)
	{
		HeapTupleHeader td = DatumGetHeapTupleHeader(rows[i]);
		HeapTupleData tuple;
		ResultRelInfo *rri = rootrri;
		TupleConversionMap *map = NULL;
		TupleTableSlot *slot = rootslot;
		SplitTable *part = NULL;
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);

		/* the new version, as the root has it, a virtual column null */
		tuple.t_len = HeapTupleHeaderGetDatumLength(td);
		ItemPointerSetInvalid(&tuple.t_self);
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = td;
		ExecClearTuple(rootslot);
		heap_deform_tuple(&tuple, rootdesc, rootslot->tts_values,
						  rootslot->tts_isnull);
		for (int k = 0; k < rootdesc->natts; k++)
			if (TupleDescAttr(rootdesc, k)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			{
				rootslot->tts_values[k] = (Datum) 0;
				rootslot->tts_isnull[k] = true;
			}
		ExecStoreVirtualTuple(rootslot);

		/* where it goes: the partition routing gives it, or the old one's table */
		if (proute != NULL)
		{
			rri = ExecFindPartition(mtstate, rootrri, proute, rootslot, estate);
			map = ExecGetRootToChildMap(rri, estate);
			if (map != NULL)
				slot = execute_attr_map_slot(map->attrMap, rootslot,
											 rri->ri_PartitionTupleSlot);
		}
		else if (DatumGetObjectId(toids[i]) != relid)
		{
			part = split_table(&parts, root, DatumGetObjectId(toids[i]), tree);
			if (part->rri == NULL)
			{
				part->rri = makeNode(ResultRelInfo);
				InitResultRelInfo(part->rri, part->rel, 0, rootrri, 0);
				ExecOpenIndices(part->rri, false);
				part->fromroot = convert_tuples_by_name(rootdesc,
														RelationGetDescr(part->rel));
				part->inslot = MakeSingleTupleTableSlot(RelationGetDescr(part->rel),
														&TTSOpsVirtual);
			}
			rri = part->rri;
			if (part->fromroot != NULL)
				slot = execute_attr_map_slot(part->fromroot->attrMap, rootslot,
											 part->inslot);
			else
				slot = ExecCopySlot(part->inslot, rootslot);
		}
		ExecMaterializeSlot(slot);
		slot->tts_tableOid = RelationGetRelid(rri->ri_RelationDesc);

		/* a new row: every generated column computed, every index given it */
		if (RelationGetDescr(rri->ri_RelationDesc)->constr != NULL)
		{
			TupleConstr *constr = RelationGetDescr(rri->ri_RelationDesc)->constr;

			if (constr->has_generated_stored)
				ExecComputeStoredGenerated(rri, estate, slot, CMD_INSERT);
			ExecConstraints(rri, slot, estate);
		}
		if (proute == NULL && rri->ri_RelationDesc->rd_rel->relispartition)
			ExecPartitionCheck(rri, slot, estate, true);
		table_tuple_insert(rri->ri_RelationDesc, slot, estate->es_output_cid,
						   0, NULL);
		if (rri->ri_NumIndices > 0)
			(void) ExecInsertIndexTuples(rri, estate, 0, slot, NIL, NULL);

		/* the row as it was written, where, and as the root has it */
		values[0] = numbers[i];
		values[1] = ObjectIdGetDatum(RelationGetRelid(rri->ri_RelationDesc));
		values[2] = ItemPointerGetDatum(&slot->tts_tid);
		if (slot == rootslot)
			values[3] = split_root_row(slot, NULL, outslot);
		else if (part != NULL)
			values[3] = split_root_row(slot, part->toroot, outslot);
		else
		{
			TupleConversionMap *toroot = ExecGetChildToRootMap(rri);

			values[3] = split_root_row(slot, toroot, outslot);
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	ExecDropSingleTupleTableSlot(outslot);
	foreach_ptr(SplitTable, p, parts)
		if (p->rri != NULL)
			ExecCloseIndices(p->rri);
	split_tables_close(parts, root);
	if (proute != NULL)
		ExecCleanupTupleRouting(mtstate, proute);
	ExecResetTupleTable(estate->es_tupleTable, false);
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);
	FreeExecutorState(estate);
	table_close(root, NoLock);
	return (Datum) 0;
}

void
GpSplitInit(void)
{
	RegisterCustomScanMethods(&split_scan_methods);
	RegisterCustomScanMethods(&split_modify_scan_methods);
}
