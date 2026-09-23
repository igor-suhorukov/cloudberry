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
 * gp_parseloc.c
 *	  The locations in the parse tree of a rewritten statement, put back in
 *	  the text the user wrote.
 *
 * Every raw parse node that parse analysis may report an error about keeps
 * the byte offset of its token in the text the grammar read.  For a statement
 * O26 rewrote, that is the rewrite, and the error would be reported at the
 * same offset of the user's text -- somewhere else.  So once the grammar has
 * built the tree, every location in it is mapped back through the rewrite's
 * position map (gp_desugar.c), and so are each statement's start and length,
 * which is what pg_stat_statements cuts the statement's text out by.
 *
 * PostgreSQL has no walker that reaches every node, so this is one:
 * raw_expression_tree_walker for expressions and the statements that hold
 * queries, and the utility statements that hold expressions -- a column's
 * default, a CHECK, an index's WHERE, a function's body -- by hand.  A node
 * it does not know keeps the rewrite's locations, which puts an error in it
 * where it was reported before this file existed, and is no worse -- unless
 * the location is one query jumbling records, which pg_stat_statements cuts
 * out of the user's text: a constant, a SET's value.  Left the rewrite's, it
 * cuts the wrong bytes, or bytes past its statement's end, which stops an
 * assert-enabled server.  So every statement that can hold one is walked.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"

#include "gp_grammar.h"

static bool remap_walker(Node *node, void *context);

#define REMAP(field) \
	((field) = ((field) < 0 ? (field) : GpPosMapSource(map, (field))))

/*
 * The locations query jumbling records, which pg_stat_statements reads back
 * in the user's text: a constant's, an IN or ARRAY list's ends, and those of
 * the few statements it normalizes.  One the rewrite wrote is not in that
 * text, so it becomes unknown (GpPosMapCopied).
 */
#define REMAP_COPIED(field) \
	((field) = ((field) < 0 ? (field) : GpPosMapCopied(map, (field))))
#define WALK(n)	remap_walker((Node *) (n), context)

/* The location fields of a node, whatever else it holds. */
static void
remap_node_locations(Node *node, const GpPosMap *map)
{
	switch (nodeTag(node))
	{
		case T_A_ArrayExpr:
			REMAP(((A_ArrayExpr *) node)->location);
			REMAP_COPIED(((A_ArrayExpr *) node)->list_start);
			REMAP_COPIED(((A_ArrayExpr *) node)->list_end);
			break;
		case T_A_Const:
			REMAP_COPIED(((A_Const *) node)->location);
			break;
		case T_A_Expr:
			REMAP(((A_Expr *) node)->location);
			REMAP_COPIED(((A_Expr *) node)->rexpr_list_start);
			REMAP_COPIED(((A_Expr *) node)->rexpr_list_end);
			break;
		case T_BooleanTest:
			REMAP(((BooleanTest *) node)->location);
			break;
		case T_BoolExpr:
			REMAP(((BoolExpr *) node)->location);
			break;
		case T_CaseExpr:
			{
				CaseExpr   *c = (CaseExpr *) node;
				ListCell   *lc;

				REMAP(c->location);
				/* raw_expression_tree_walker never visits a CaseWhen */
				foreach(lc, c->args)
					REMAP(lfirst_node(CaseWhen, lc)->location);
			}
			break;
		case T_CoalesceExpr:
			REMAP(((CoalesceExpr *) node)->location);
			break;
		case T_CollateClause:
			REMAP(((CollateClause *) node)->location);
			break;
		case T_ColumnDef:
			REMAP(((ColumnDef *) node)->location);
			break;
		case T_ColumnRef:
			REMAP(((ColumnRef *) node)->location);
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				REMAP(cte->location);
				/* nor these */
				if (cte->search_clause != NULL)
					REMAP(cte->search_clause->location);
				if (cte->cycle_clause != NULL)
					REMAP(cte->cycle_clause->location);
			}
			break;
		case T_Constraint:
			REMAP(((Constraint *) node)->location);
			break;
		case T_DeallocateStmt:
			REMAP_COPIED(((DeallocateStmt *) node)->location);
			break;
		case T_DefElem:
			REMAP(((DefElem *) node)->location);
			break;
		case T_FetchStmt:
			REMAP_COPIED(((FetchStmt *) node)->location);
			break;
		case T_FuncCall:
			REMAP(((FuncCall *) node)->location);
			break;
		case T_FunctionParameter:
			REMAP(((FunctionParameter *) node)->location);
			break;
		case T_GroupingFunc:
			REMAP(((GroupingFunc *) node)->location);
			break;
		case T_GroupingSet:
			REMAP(((GroupingSet *) node)->location);
			break;
		case T_IndexElem:
			REMAP(((IndexElem *) node)->location);
			break;
		case T_InferClause:
			REMAP(((InferClause *) node)->location);
			break;
		case T_JsonAggConstructor:
			REMAP(((JsonAggConstructor *) node)->location);
			break;
		case T_JsonArrayConstructor:
			REMAP(((JsonArrayConstructor *) node)->location);
			break;
		case T_JsonArrayQueryConstructor:
			REMAP(((JsonArrayQueryConstructor *) node)->location);
			break;
		case T_JsonBehavior:
			REMAP(((JsonBehavior *) node)->location);
			break;
		case T_JsonConstructorExpr:
			REMAP(((JsonConstructorExpr *) node)->location);
			break;
		case T_JsonFormat:
			REMAP(((JsonFormat *) node)->location);
			break;
		case T_JsonFuncExpr:
			REMAP(((JsonFuncExpr *) node)->location);
			break;
		case T_JsonIsPredicate:
			REMAP(((JsonIsPredicate *) node)->location);
			break;
		case T_JsonObjectConstructor:
			REMAP(((JsonObjectConstructor *) node)->location);
			break;
		case T_JsonParseExpr:
			REMAP(((JsonParseExpr *) node)->location);
			break;
		case T_JsonScalarExpr:
			REMAP(((JsonScalarExpr *) node)->location);
			break;
		case T_JsonSerializeExpr:
			REMAP(((JsonSerializeExpr *) node)->location);
			break;
		case T_JsonTable:
			REMAP(((JsonTable *) node)->location);
			break;
		case T_JsonTableColumn:
			REMAP(((JsonTableColumn *) node)->location);
			break;
		case T_JsonTablePathSpec:
			REMAP(((JsonTablePathSpec *) node)->location);
			REMAP(((JsonTablePathSpec *) node)->name_location);
			break;
		case T_MergeSupportFunc:
			REMAP(((MergeSupportFunc *) node)->location);
			break;
		case T_MinMaxExpr:
			REMAP(((MinMaxExpr *) node)->location);
			break;
		case T_NamedArgExpr:
			REMAP(((NamedArgExpr *) node)->location);
			break;
		case T_NullTest:
			REMAP(((NullTest *) node)->location);
			break;
		case T_OnConflictClause:
			REMAP(((OnConflictClause *) node)->location);
			break;
		case T_ParamRef:
			REMAP(((ParamRef *) node)->location);
			break;
		case T_PartitionBoundSpec:
			REMAP(((PartitionBoundSpec *) node)->location);
			break;
		case T_PartitionElem:
			REMAP(((PartitionElem *) node)->location);
			break;
		case T_PartitionRangeDatum:
			REMAP(((PartitionRangeDatum *) node)->location);
			break;
		case T_PartitionSpec:
			REMAP(((PartitionSpec *) node)->location);
			break;
		case T_PLAssignStmt:
			REMAP(((PLAssignStmt *) node)->location);
			break;
		case T_PublicationAllObjSpec:
			REMAP(((PublicationAllObjSpec *) node)->location);
			break;
		case T_PublicationObjSpec:
			REMAP(((PublicationObjSpec *) node)->location);
			break;
		case T_RangeTableFunc:
			REMAP(((RangeTableFunc *) node)->location);
			break;
		case T_RangeTableFuncCol:
			REMAP(((RangeTableFuncCol *) node)->location);
			break;
		case T_RangeTableSample:
			REMAP(((RangeTableSample *) node)->location);
			break;
		case T_RangeVar:
			REMAP(((RangeVar *) node)->location);
			break;
		case T_ResTarget:
			REMAP(((ResTarget *) node)->location);
			break;
		case T_ReturningOption:
			REMAP(((ReturningOption *) node)->location);
			break;
		case T_RoleSpec:
			REMAP(((RoleSpec *) node)->location);
			break;
		case T_RowExpr:
			REMAP(((RowExpr *) node)->location);
			break;
		case T_SetToDefault:
			REMAP(((SetToDefault *) node)->location);
			break;
		case T_SortBy:
			REMAP(((SortBy *) node)->location);
			break;
		case T_SQLValueFunction:
			REMAP(((SQLValueFunction *) node)->location);
			break;
		case T_SubLink:
			REMAP(((SubLink *) node)->location);
			break;
		case T_TransactionStmt:
			REMAP_COPIED(((TransactionStmt *) node)->location);
			break;
		case T_TypeCast:
			REMAP(((TypeCast *) node)->location);
			break;
		case T_TypeName:
			REMAP(((TypeName *) node)->location);
			break;
		case T_VariableSetStmt:
			REMAP_COPIED(((VariableSetStmt *) node)->location);
			break;
		case T_WaitStmt:
			REMAP_COPIED(((WaitStmt *) node)->lsn_location);
			break;
		case T_WindowDef:
			REMAP(((WindowDef *) node)->location);
			break;
		case T_WithClause:
			REMAP(((WithClause *) node)->location);
			break;
		case T_XmlExpr:
			REMAP(((XmlExpr *) node)->location);
			break;
		case T_XmlSerialize:
			REMAP(((XmlSerialize *) node)->location);
			break;
		default:
			break;
	}
}

/*
 * The nodes raw_expression_tree_walker descends into.  It raises on any other,
 * so it is handed nothing that is not one of these.
 */
static bool
raw_walker_knows(Node *node)
{
	switch (nodeTag(node))
	{
		case T_JsonFormat:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_SQLValueFunction:
		case T_Integer:
		case T_Float:
		case T_Boolean:
		case T_String:
		case T_BitString:
		case T_ParamRef:
		case T_A_Const:
		case T_A_Star:
		case T_MergeSupportFunc:
		case T_ReturningOption:
		case T_Alias:
		case T_RangeVar:
		case T_GroupingFunc:
		case T_SubLink:
		case T_CaseExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_XmlExpr:
		case T_JsonReturning:
		case T_JsonValueExpr:
		case T_JsonParseExpr:
		case T_JsonScalarExpr:
		case T_JsonSerializeExpr:
		case T_JsonConstructorExpr:
		case T_JsonIsPredicate:
		case T_JsonArgument:
		case T_JsonFuncExpr:
		case T_JsonBehavior:
		case T_JsonTable:
		case T_JsonTableColumn:
		case T_JsonTablePathSpec:
		case T_NullTest:
		case T_BooleanTest:
		case T_JoinExpr:
		case T_List:
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
		case T_MergeWhenClause:
		case T_ReturningClause:
		case T_SelectStmt:
		case T_PLAssignStmt:
		case T_A_Expr:
		case T_BoolExpr:
		case T_ColumnRef:
		case T_FuncCall:
		case T_NamedArgExpr:
		case T_A_Indices:
		case T_A_Indirection:
		case T_A_ArrayExpr:
		case T_ResTarget:
		case T_MultiAssignRef:
		case T_TypeCast:
		case T_CollateClause:
		case T_SortBy:
		case T_WindowDef:
		case T_RangeSubselect:
		case T_RangeFunction:
		case T_RangeTableSample:
		case T_RangeTableFunc:
		case T_RangeTableFuncCol:
		case T_TypeName:
		case T_IndexElem:
		case T_GroupingSet:
		case T_LockingClause:
		case T_XmlSerialize:
		case T_WithClause:
		case T_InferClause:
		case T_OnConflictClause:
		case T_CommonTableExpr:
		case T_JsonOutput:
		case T_JsonKeyValue:
		case T_JsonObjectConstructor:
		case T_JsonArrayConstructor:
		case T_JsonAggConstructor:
		case T_JsonObjectAgg:
		case T_JsonArrayAgg:
		case T_JsonArrayQueryConstructor:
			return true;
		default:
			return false;
	}
}

static bool
remap_walker(Node *node, void *context)
{
	const GpPosMap *map = (const GpPosMap *) context;

	if (node == NULL)
		return false;

	check_stack_depth();

	remap_node_locations(node, map);

	switch (nodeTag(node))
	{
		case T_RawStmt:
			{
				RawStmt    *rs = (RawStmt *) node;

				/*
				 * A length of 0 is "to the end of the string", and stays
				 * that; so does one whose end the rewrite wrote in place of
				 * the statement's own, a statement it added.
				 */
				if (rs->stmt_location >= 0)
				{
					int			start = GpPosMapSource(map, rs->stmt_location);

					if (rs->stmt_len > 0)
						rs->stmt_len = Max(GpPosMapSource(map, rs->stmt_location +
														  rs->stmt_len) - start, 0);
					rs->stmt_location = start;
				}
				return WALK(rs->stmt);
			}

			/* columns: raw_expression_tree_walker leaves out constraints */
		case T_ColumnDef:
			{
				ColumnDef  *cd = (ColumnDef *) node;

				return WALK(cd->typeName) || WALK(cd->raw_default) ||
					WALK(cd->collClause) || WALK(cd->constraints);
			}
		case T_Constraint:
			{
				Constraint *c = (Constraint *) node;

				return WALK(c->raw_expr) || WALK(c->where_clause) ||
					WALK(c->pktable) || WALK(c->exclusions) || WALK(c->options);
			}
		case T_DefElem:
			return WALK(((DefElem *) node)->arg);
		case T_IntoClause:
			{
				IntoClause *into = (IntoClause *) node;

				/*
				 * raw_expression_tree_walker leaves out its options, where
				 * a CREATE TABLE AS carries its DISTRIBUTED BY: an error in
				 * one is reported where the user wrote it.
				 */
				return WALK(into->rel) || WALK(into->options) ||
					WALK(into->viewQuery);
			}
		case T_TableLikeClause:
			return WALK(((TableLikeClause *) node)->relation);
		case T_PartitionSpec:
			return WALK(((PartitionSpec *) node)->partParams);
		case T_PartitionElem:
			return WALK(((PartitionElem *) node)->expr);
		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *b = (PartitionBoundSpec *) node;

				return WALK(b->listdatums) || WALK(b->lowerdatums) ||
					WALK(b->upperdatums);
			}
		case T_PartitionRangeDatum:
			return WALK(((PartitionRangeDatum *) node)->value);
		case T_PartitionCmd:
			return WALK(((PartitionCmd *) node)->name) ||
				WALK(((PartitionCmd *) node)->bound);
		case T_StatsElem:
			return WALK(((StatsElem *) node)->expr);
		case T_FunctionParameter:
			return WALK(((FunctionParameter *) node)->argType) ||
				WALK(((FunctionParameter *) node)->defexpr);

			/* the statements that hold a query, or an expression */
		case T_ViewStmt:
			return WALK(((ViewStmt *) node)->view) ||
				WALK(((ViewStmt *) node)->query) ||
				WALK(((ViewStmt *) node)->options);
		case T_CreateTableAsStmt:
			return WALK(((CreateTableAsStmt *) node)->query) ||
				WALK(((CreateTableAsStmt *) node)->into);
		case T_ExplainStmt:
			return WALK(((ExplainStmt *) node)->query) ||
				WALK(((ExplainStmt *) node)->options);
		case T_DeclareCursorStmt:
			return WALK(((DeclareCursorStmt *) node)->query);
		case T_PrepareStmt:
			return WALK(((PrepareStmt *) node)->argtypes) ||
				WALK(((PrepareStmt *) node)->query);
		case T_ExecuteStmt:
			return WALK(((ExecuteStmt *) node)->params);
		case T_CopyStmt:
			{
				CopyStmt   *c = (CopyStmt *) node;

				return WALK(c->relation) || WALK(c->query) ||
					WALK(c->options) || WALK(c->whereClause);
			}
		case T_RuleStmt:
			return WALK(((RuleStmt *) node)->relation) ||
				WALK(((RuleStmt *) node)->whereClause) ||
				WALK(((RuleStmt *) node)->actions);
		case T_CreateStmt:
			{
				CreateStmt *c = (CreateStmt *) node;

				return WALK(c->relation) || WALK(c->tableElts) ||
					WALK(c->inhRelations) || WALK(c->partbound) ||
					WALK(c->partspec) || WALK(c->ofTypename) ||
					WALK(c->constraints) || WALK(c->options);
			}
		case T_AlterTableStmt:
			return WALK(((AlterTableStmt *) node)->relation) ||
				WALK(((AlterTableStmt *) node)->cmds);
		case T_AlterTableCmd:
			return WALK(((AlterTableCmd *) node)->newowner) ||
				WALK(((AlterTableCmd *) node)->def);
		case T_IndexStmt:
			{
				IndexStmt  *i = (IndexStmt *) node;

				return WALK(i->relation) || WALK(i->indexParams) ||
					WALK(i->indexIncludingParams) || WALK(i->options) ||
					WALK(i->whereClause);
			}
		case T_CreateFunctionStmt:
			{
				CreateFunctionStmt *f = (CreateFunctionStmt *) node;

				return WALK(f->parameters) || WALK(f->returnType) ||
					WALK(f->options) || WALK(f->sql_body);
			}

			/*
			 * ALTER FUNCTION f(int) EXECUTE ON ANY is SET gp.execute_on =
			 * 'any' once rewritten, and a SET's value is a constant query
			 * jumbling records: left the rewrite's, pg_stat_statements
			 * would cut it out of the user's text at a place past the
			 * statement's end.
			 */
		case T_AlterFunctionStmt:
			return WALK(((AlterFunctionStmt *) node)->func) ||
				WALK(((AlterFunctionStmt *) node)->actions);
		case T_ObjectWithArgs:
			return WALK(((ObjectWithArgs *) node)->objargs) ||
				WALK(((ObjectWithArgs *) node)->objfuncargs);
		case T_ReturnStmt:
			return WALK(((ReturnStmt *) node)->returnval);
		case T_CallStmt:
			return WALK(((CallStmt *) node)->funccall);
		case T_DoStmt:
			return WALK(((DoStmt *) node)->args);
		case T_CreateTrigStmt:
			return WALK(((CreateTrigStmt *) node)->relation) ||
				WALK(((CreateTrigStmt *) node)->whenClause);
		case T_CreatePolicyStmt:
			return WALK(((CreatePolicyStmt *) node)->table) ||
				WALK(((CreatePolicyStmt *) node)->roles) ||
				WALK(((CreatePolicyStmt *) node)->qual) ||
				WALK(((CreatePolicyStmt *) node)->with_check);
		case T_AlterPolicyStmt:
			return WALK(((AlterPolicyStmt *) node)->table) ||
				WALK(((AlterPolicyStmt *) node)->roles) ||
				WALK(((AlterPolicyStmt *) node)->qual) ||
				WALK(((AlterPolicyStmt *) node)->with_check);
		case T_CreateDomainStmt:
			return WALK(((CreateDomainStmt *) node)->typeName) ||
				WALK(((CreateDomainStmt *) node)->collClause) ||
				WALK(((CreateDomainStmt *) node)->constraints);
		case T_AlterDomainStmt:
			return WALK(((AlterDomainStmt *) node)->def);
		case T_CreateStatsStmt:
			return WALK(((CreateStatsStmt *) node)->exprs) ||
				WALK(((CreateStatsStmt *) node)->relations);
		case T_VariableSetStmt:
			return WALK(((VariableSetStmt *) node)->args);

			/*
			 * and the other statements a SET is in: none is rewritten, but
			 * one after a statement that was is at the rewrite's positions
			 * too.
			 */
		case T_AlterRoleSetStmt:
			return WALK(((AlterRoleSetStmt *) node)->role) ||
				WALK(((AlterRoleSetStmt *) node)->setstmt);
		case T_AlterDatabaseSetStmt:
			return WALK(((AlterDatabaseSetStmt *) node)->setstmt);
		case T_AlterSystemStmt:
			return WALK(((AlterSystemStmt *) node)->setstmt);
		case T_TransactionStmt:
			return WALK(((TransactionStmt *) node)->options);

		default:
			if (raw_walker_knows(node))
				return raw_expression_tree_walker(node, remap_walker, context);
			return false;
	}
}

void
GpRemapParseLocations(List *parsetree, const GpPosMap *map)
{
	(void) remap_walker((Node *) parsetree, (void *) map);
}
