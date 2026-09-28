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
 * compat/wholerow.c
 *	  A table's row, carried up a DML's plan to its ModifyTable, whole --
 *	  and a partitioned table's row's partition, by its tableoid.
 *
 * ORCA's plan has only the columns its core keeps for its DML: an update in
 * place asks for the new values, the ctid and the segment, not the old
 * values of the columns it sets.  The target's whole row is what gp_core's
 * explicit write reads a MERGE's actions' old row from, on a cluster, where
 * ORCA's join comes up through a Gather to the coordinator (merge.c;
 * gp_core's gp_modify.c).  So it is added after ORCA: the whole row of the
 * scan that read the row's ctid, appended to that scan's target list and
 * passed up through each node on the way, as the ctid is.  Until
 * 2026-09-28 an UPDATE and a DELETE ... RETURNING of an append-optimized or
 * a PAX table carried it too, as "wholerow": O20, a core patch then, had
 * ModifyTable take such a table's old row from the plan, where it now
 * fetches it by its ctid.
 *
 * The way up is the ctid's, followed down column by column to the scan
 * whose Var it is.  A node that projects -- a Result, a join -- gets a Var
 * of its child's new column; one that does not -- a Sort, a Hash, a
 * Material, a Limit -- has a target list that is its child's, and the new
 * column is appended to both.  A Motion's columns go through its scan target
 * list (gp_motion.c, motion_make()); a partitioned table's scan through its
 * partitions, gp_orca's dynamic scan, takes the row from each partition's
 * scan at one position, which its own scan target list names (dynamicscan.c).
 * The scan's row is of the table's own row type, not an anonymous record, so
 * that it reads back in any process a Motion sends it to; a dynamic scan's
 * column is a record, each row of its partition's type.  Any other node, or a
 * scan that cannot give a whole row, ends the walk with nothing.
 *
 * A partitioned table's UPDATE and DELETE find each row's partition by its
 * tableoid, a "tableoid" column ModifyTable reads (ExecLookupResultRelByOid()),
 * and gp_core's split update its DELETE's.  ORCA's DML carries two columns of
 * the row's identity, the ctid and gp_segment_id, which an Explicit
 * Redistribute Motion routes the row back to its segment by: tableoid comes
 * the same way, from the scan that read the ctid -- through a split update's
 * Split too, which passes it to both of its rows, and a hash filter.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "cb_dynamicscan.h"
#include "cb_wholerow.h"
#include "gp_motion.h"

static bool
is_motion(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		strcmp(((CustomScan *) plan)->methods->CustomName, GP_MOTION_NAME) == 0;
}

/*
 * gp_core's nodes that pass a column of their child's on unchanged, their
 * scan tuple being its row: its Split Update (gp_split.c), and the hash
 * filter that keeps the rows of one segment's share (gp_motion.c).
 */
static bool
is_passthrough(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		(strcmp(((CustomScan *) plan)->methods->CustomName, "GpSplitUpdate") == 0 ||
		 strcmp(((CustomScan *) plan)->methods->CustomName, "GpHashFilter") == 0);
}

/* What is carried: the scan's whole row, or its row's tableoid. */
typedef enum CarryKind
{
	CARRY_WHOLE_ROW,
	CARRY_TABLEOID,
} CarryKind;

/* "expr" as a new last entry of "tlist"; the entry's resno. */
static AttrNumber
append_column(List **tlist, Expr *expr, bool junk)
{
	AttrNumber	resno = list_length(*tlist) + 1;

	*tlist = lappend(*tlist, makeTargetEntry(expr, resno, NULL, junk));
	return resno;
}

/* A Var of column "attno" of "varno", of the type of "plan"'s column "attno". */
static Var *
column_var(int varno, Plan *plan, AttrNumber attno)
{
	TargetEntry *tle = get_tle_by_resno(plan->targetlist, attno);

	return makeVar(varno, attno, exprType((Node *) tle->expr), -1, InvalidOid, 0);
}

static AttrNumber
carry(Plan *plan, AttrNumber resno, List *rtable, CarryKind kind)
{
	TargetEntry *tle = get_tle_by_resno(plan->targetlist, resno);
	Expr	   *expr;
	Var		   *var;
	Plan	   *child;
	AttrNumber	childno;

	if (tle == NULL)
		return InvalidAttrNumber;
	expr = tle->expr;
	while (expr != NULL && IsA(expr, RelabelType))
		expr = ((RelabelType *) expr)->arg;

	/*
	 * A plan that reads no row has a NULL for the ctid -- ORCA's Result in
	 * the place of a scan all of whose partitions a condition prunes -- and
	 * a NULL beside it, then.
	 */
	if (expr != NULL && IsA(expr, Const) && ((Const *) expr)->constisnull)
		return append_column(&plan->targetlist,
							 (Expr *) makeNullConst(kind == CARRY_TABLEOID
													? OIDOID : RECORDOID,
													-1, InvalidOid),
							 true);
	if (expr == NULL || !IsA(expr, Var))
		return InvalidAttrNumber;
	var = (Var *) expr;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
			{
				/* the scan that read the row: the row, whole */
				Index		scanrelid = ((Scan *) plan)->scanrelid;
				Oid			rowtype;

				if (var->varno != scanrelid ||
					var->varattno != SelfItemPointerAttributeNumber)
					return InvalidAttrNumber;
				if (kind == CARRY_TABLEOID)
					return append_column(&plan->targetlist,
										 (Expr *) makeVar(scanrelid,
														  TableOidAttributeNumber,
														  OIDOID, -1,
														  InvalidOid, 0),
										 true);
				rowtype = get_rel_type_id(rt_fetch(scanrelid, rtable)->relid);
				if (!OidIsValid(rowtype))
					return InvalidAttrNumber;
				return append_column(&plan->targetlist,
									 (Expr *) makeVar(scanrelid,
													  InvalidAttrNumber,
													  rowtype, -1,
													  InvalidOid, 0),
									 true);
			}

		case T_Result:
		case T_Sort:
		case T_IncrementalSort:
		case T_Material:
		case T_Limit:
		case T_Unique:
		case T_Hash:
			if (var->varno != OUTER_VAR || plan->lefttree == NULL)
				return InvalidAttrNumber;
			child = plan->lefttree;
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			if (var->varno == OUTER_VAR)
				child = plan->lefttree;
			else if (var->varno == INNER_VAR)
				child = plan->righttree;
			else
				return InvalidAttrNumber;
			break;

		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;
				TargetEntry *stle;
				AttrNumber	scanno;

				if (var->varno != INDEX_VAR)
					return InvalidAttrNumber;
				stle = get_tle_by_resno(cscan->custom_scan_tlist, var->varattno);
				if (stle == NULL || !IsA(stle->expr, Var))
					return InvalidAttrNumber;

				if ((is_motion(plan) || is_passthrough(plan)) &&
					plan->lefttree != NULL &&
					((Var *) stle->expr)->varno == OUTER_VAR)
				{
					/*
					 * The fragment's column, through the scan target list; a
					 * Split passes a column that is neither its DELETE's nor
					 * its INSERT's to both.
					 */
					childno = carry(plan->lefttree,
									((Var *) stle->expr)->varattno, rtable,
									kind);
					if (childno == InvalidAttrNumber)
						return InvalidAttrNumber;
					scanno = append_column(&cscan->custom_scan_tlist,
										   (Expr *) column_var(OUTER_VAR,
															   plan->lefttree,
															   childno),
										   false);
				}
				else if (cscan->methods == &gp_orca_dynamic_scan_methods)
				{
					/*
					 * Each partition's scan returns the row at the position
					 * the node's scan target list gives it, which names it
					 * in the table's terms, as a record: the partitions'
					 * row types are their own.
					 */
					ListCell   *lc;

					scanno = list_length(cscan->custom_scan_tlist) + 1;
					foreach(lc, cscan->custom_plans)
					{
						if (carry((Plan *) lfirst(lc), var->varattno, rtable,
								  kind) != scanno)
							return InvalidAttrNumber;
					}
					(void) append_column(&cscan->custom_scan_tlist,
										 (Expr *) makeVar(((Var *) stle->expr)->varno,
														  kind == CARRY_TABLEOID
														  ? TableOidAttributeNumber
														  : InvalidAttrNumber,
														  kind == CARRY_TABLEOID
														  ? OIDOID : RECORDOID,
														  -1, InvalidOid, 0),
										 false);
				}
				else
					return InvalidAttrNumber;

				return append_column(&plan->targetlist,
									 (Expr *) makeVar(INDEX_VAR, scanno,
													  exprType((Node *) get_tle_by_resno(cscan->custom_scan_tlist, scanno)->expr),
													  -1, InvalidOid, 0),
									 true);
			}

		default:
			return InvalidAttrNumber;
	}

	childno = carry(child, var->varattno, rtable, kind);
	if (childno == InvalidAttrNumber)
		return InvalidAttrNumber;
	return append_column(&plan->targetlist,
						 (Expr *) column_var(var->varno, child, childno), true);
}

AttrNumber
gp_orca_carry_whole_row(Plan *plan, AttrNumber resno, List *rtable)
{
	return carry(plan, resno, rtable, CARRY_WHOLE_ROW);
}

/* A Var of "varno", column "attno", of "proto"'s type, typmod and collation. */
static Var *
typed_var(int varno, AttrNumber attno, Var *proto)
{
	return makeVar(varno, attno, proto->vartype, proto->vartypmod,
				   proto->varcollid, 0);
}

/*
 * A column of another relation than the one a ctid names: the scan of range
 * table entry "rti" is looked for below "plan", and the column "proto" names
 * is appended to its target list and passed up through each node on the way,
 * as the ctid is -- the RETURNING of an UPDATE ... FROM or a DELETE ...
 * USING reads the other relation's row the statement joined.  Through the
 * inner side of a semi-join too, which returns the first inner row each
 * outer row matched as the one that matched it -- the planner's semi-join
 * carries a row mark's ctid so (an UPDATE or DELETE whose WHERE has an
 * EXISTS or an IN).  Not through an anti-join's, whose rows match nothing,
 * nor through any node but those that pass a column on.
 */
AttrNumber
gp_orca_carry_rte_column(Plan *plan, Index rti, Var *proto)
{
	AttrNumber	childno;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
			if (((Scan *) plan)->scanrelid != rti)
				return InvalidAttrNumber;
			return append_column(&plan->targetlist,
								 (Expr *) typed_var(rti, proto->varattno, proto),
								 true);

		case T_Result:
		case T_Sort:
		case T_IncrementalSort:
		case T_Material:
		case T_Limit:
		case T_Unique:
		case T_Hash:
			if (plan->lefttree == NULL)
				return InvalidAttrNumber;
			childno = gp_orca_carry_rte_column(plan->lefttree, rti, proto);
			if (childno == InvalidAttrNumber)
				return InvalidAttrNumber;
			return append_column(&plan->targetlist,
								 (Expr *) typed_var(OUTER_VAR, childno, proto),
								 true);

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			{
				JoinType	jointype = ((Join *) plan)->jointype;

				if (jointype != JOIN_RIGHT_SEMI && jointype != JOIN_RIGHT_ANTI)
				{
					childno = gp_orca_carry_rte_column(plan->lefttree, rti, proto);
					if (childno != InvalidAttrNumber)
						return append_column(&plan->targetlist,
											 (Expr *) typed_var(OUTER_VAR, childno, proto),
											 true);
				}
				if (jointype == JOIN_ANTI)
					return InvalidAttrNumber;
				childno = gp_orca_carry_rte_column(plan->righttree, rti, proto);
				if (childno == InvalidAttrNumber)
					return InvalidAttrNumber;
				return append_column(&plan->targetlist,
									 (Expr *) typed_var(INNER_VAR, childno, proto),
									 true);
			}

		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;
				AttrNumber	scanno;

				if (!(is_motion(plan) || is_passthrough(plan)) ||
					plan->lefttree == NULL)
					return InvalidAttrNumber;
				childno = gp_orca_carry_rte_column(plan->lefttree, rti, proto);
				if (childno == InvalidAttrNumber)
					return InvalidAttrNumber;
				scanno = append_column(&cscan->custom_scan_tlist,
									   (Expr *) typed_var(OUTER_VAR, childno, proto),
									   false);
				return append_column(&plan->targetlist,
									 (Expr *) typed_var(INDEX_VAR, scanno, proto),
									 true);
			}

		default:
			return InvalidAttrNumber;
	}
}

AttrNumber
gp_orca_carry_tableoid(Plan *plan, AttrNumber resno, List *rtable)
{
	return carry(plan, resno, rtable, CARRY_TABLEOID);
}
