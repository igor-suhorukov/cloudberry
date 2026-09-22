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
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xact.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp_motion.h"

/* ------------------------------------------------------------------------- */
/* Split Update: each row, twice                                             */
/* ------------------------------------------------------------------------- */

/* The action column's values, Cloudberry's DMLAction. */
#define GP_DML_INSERT	1
#define GP_DML_DELETE	2

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
	ResultRelInfo *rri;
	TupleTableSlot *newslot;
} SplitModifyState;

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
	cscan->custom_private = list_make4(makeInteger(rti), makeInteger(natts),
									   makeInteger(actioncol),
									   makeInteger(ctidcol));
	cscan->methods = &split_modify_scan_methods;
	return (Plan *) cscan;
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

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* The result relation, as ModifyTable opens it, and its indexes. */
	state->rri = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, state->rri, state->rti);
	CheckValidResultRel(state->rri, CMD_UPDATE, ONCONFLICT_NONE, NIL);
	ExecOpenIndices(state->rri, false);
	state->newslot = ExecInitExtraTupleSlot(estate,
											RelationGetDescr(state->rri->ri_RelationDesc),
											&TTSOpsVirtual);
}

static void
split_delete(SplitModifyState *state, ItemPointer tid)
{
	EState	   *estate = state->css.ss.ps.state;
	Relation	rel = state->rri->ri_RelationDesc;
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
			ereport(ERROR,
					(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
					 errmsg("tuple to be updated was already modified by an operation triggered by the current command")));
			break;
		case TM_Updated:
		case TM_Deleted:
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to concurrent update")));
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

			if (isnull)
				elog(ERROR, "a split update's DELETE has no ctid");
			split_delete(state, DatumGetItemPointer(ctid));
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
	ExecEndNode(outerPlanState(node));
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

void
GpSplitInit(void)
{
	RegisterCustomScanMethods(&split_scan_methods);
	RegisterCustomScanMethods(&split_modify_scan_methods);
}
