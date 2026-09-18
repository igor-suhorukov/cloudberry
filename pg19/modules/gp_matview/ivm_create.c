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
 * ivm_create.c
 *	  Making an incrementally maintained materialized view.
 *
 * Cloudberry spells this CREATE INCREMENTAL MATERIALIZED VIEW, which needs a
 * keyword and a field on IntoClause.  Here it is an option,
 * WITH (gp.incremental), which PostgreSQL's grammar already accepts: a
 * namespaced reloption parses, and is only rejected later in
 * transformRelOptions -- after ProcessUtility_hook has had the chance to take
 * it out.  This is how TimescaleDB reads WITH (timescaledb.continuous).
 *
 * What the option leads to is Cloudberry's own work, ported:
 *	- the query is checked against what incremental maintenance can express,
 *	- it is rewritten with the hidden columns that maintenance counts with,
 *	- the view is labelled, and
 *	- statement triggers are put on its base tables.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_inherits.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_trigger.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "parser/parse_func.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_label.h"
#include "gp_matview.h"

/*
 * Take WITH (gp.incremental) out of an option list, so that what is left is
 * something PostgreSQL will accept.  Returns whether it was there.
 */
bool
GpIvmTakeOption(List **options)
{
	ListCell   *lc;
	bool		found = false;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		char	   *name;

		if (def->defnamespace)
			name = psprintf("%s.%s", def->defnamespace, def->defname);
		else
			name = pstrdup(def->defname);

		if (strcmp(name, GP_IVM_OPTION) == 0)
		{
			/* A flag: it takes no value, and saying otherwise is a mistake. */
			if (def->arg != NULL && !defGetBoolean(def))
			{
				pfree(name);
				continue;		/* WITH (gp.incremental = false): an ordinary view */
			}
			found = true;
			*options = foreach_delete_current(*options, lc);
		}
		pfree(name);
	}

	return found;
}

/* ------------------------------------------------------------------------- */
/* What incremental maintenance can express                                  */
/* ------------------------------------------------------------------------- */

#define unsupported(what) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("%s is not supported on an incrementally maintained materialized view", \
					(what))))

static bool
check_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink) || IsA(node, SubPlan))
		unsupported("a subquery");

	if (IsA(node, Query))
	{
		Query	   *qry = (Query *) node;
		ListCell   *lc;

		if (qry->cteList != NIL)
			unsupported("a CTE");
		if (qry->groupClause != NIL && !qry->hasAggs)
			unsupported("GROUP BY without an aggregate");
		if (qry->havingQual != NULL)
			unsupported("HAVING");
		if (qry->sortClause != NIL)
			unsupported("ORDER BY");
		if (qry->limitOffset != NULL || qry->limitCount != NULL)
			unsupported("LIMIT or OFFSET");
		if (qry->hasDistinctOn)
			unsupported("DISTINCT ON");
		if (qry->hasWindowFuncs)
			unsupported("a window function");
		if (qry->groupingSets != NIL)
			unsupported("GROUPING SETS, ROLLUP or CUBE");
		if (qry->setOperations != NULL)
			unsupported("UNION, INTERSECT or EXCEPT");
		if (qry->rowMarks != NIL)
			unsupported("FOR UPDATE or FOR SHARE");
		if (qry->targetList == NIL)
			unsupported("an empty target list");

		foreach(lc, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_SUBQUERY)
				unsupported("a subquery");
			if (rte->tablesample != NULL)
				unsupported("TABLESAMPLE");
			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_PARTITIONED_TABLE)
				unsupported("a partitioned base table");
			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_FOREIGN_TABLE)
				unsupported("a foreign base table");
			if (rte->rtekind == RTE_RELATION && rte->inh &&
				has_subclass(rte->relid))
				unsupported("an inherited base table");
		}

		foreach(lc, qry->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (IsA(tle->expr, Var) && ((Var *) tle->expr)->varattno < 0)
				unsupported("a system column");
			if (tle->resname != NULL && IsIvmColumn(tle->resname))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
						 errmsg("column name \"%s\" is reserved", tle->resname),
						 errdetail("Names beginning with \"%s\" are used by incremental maintenance.",
								   GP_IVM_PREFIX)));
		}

		return query_tree_walker(qry, check_walker, context, QTW_IGNORE_RANGE_TABLE);
	}

	return expression_tree_walker(node, check_walker, context);
}

void
GpIvmCheckQuery(Query *query)
{
	check_walker((Node *) query, NULL);
}

/* ------------------------------------------------------------------------- */
/* The rewrite                                                               */
/* ------------------------------------------------------------------------- */

/*
 * What a column that goes with an aggregate column is called.  Cloudberry
 * spells it the same way, through IVM_colname(kind, resname).
 */
char *
ivm_companion_name(const char *kind, const char *resname)
{
	return makeObjectName(psprintf("%s%s", GP_IVM_PREFIX, kind), resname, "_");
}

/*
 * A count() or sum() over the same argument as an aggregate already in the
 * target list, named after it.
 *
 * Built through the parser, so that the transition type and everything else
 * an Aggref carries are what they would be had the user written it.
 */
static TargetEntry *
make_companion(ParseState *pstate, Aggref *aggref, char *kind,
			   const char *resname, AttrNumber resno)
{
	FuncCall   *fn = makeFuncCall(SystemFuncName(kind), NIL,
								  COERCE_EXPLICIT_CALL, -1);
	Node	   *node;
	List	   *args;

	args = list_make1(copyObject(((TargetEntry *) linitial(aggref->args))->expr));
	node = ParseFuncOrColumn(pstate, fn->funcname, args, NULL, fn, false, -1);

	return makeTargetEntry((Expr *) node, resno,
						   ivm_companion_name(kind, resname), false);
}

Query *
GpIvmRewriteQuery(Query *query, List *colNames)
{
	Query	   *rewritten = copyObject(query);
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_expr_kind = EXPR_KIND_SELECT_TARGET;

	if (rewritten->groupClause)
	{
		ListCell   *lc;

		foreach(lc, rewritten->groupClause)
		{
			SortGroupClause *scl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(scl, rewritten->targetList);

			if (tle->resjunk)
				unsupported("a GROUP BY expression that is not in the select list");
		}
	}
	else if (!rewritten->hasAggs && rewritten->distinctClause)
	{
		/* DISTINCT becomes GROUP BY; the count below then does the work. */
		rewritten->groupClause = transformDistinctClause(NULL,
														 &rewritten->targetList,
														 rewritten->sortClause,
														 false);
	}

	/*
	 * sum() cannot be maintained on its own: when the last row of a group
	 * goes, its sum is NULL rather than zero, and only a count of the
	 * non-null inputs says which.  So each sum() gains a count() of the same
	 * expression, named after it.  avg() is that sum divided by that count,
	 * so it gains both and is read off them.  This is Cloudberry's
	 * makeIvmAggColumn, for the aggregates the delta path handles.
	 */
	if (rewritten->hasAggs)
	{
		List	   *extra = NIL;
		ListCell   *lc;
		AttrNumber	next_resno = list_length(rewritten->targetList) + 1;

		foreach(lc, rewritten->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Aggref	   *aggref;
			char	   *aggname;

			if (tle->resjunk || !IsA(tle->expr, Aggref))
				continue;
			aggref = (Aggref *) tle->expr;
			aggname = get_func_name(aggref->aggfnoid);
			if (aggname == NULL || list_length(aggref->args) != 1)
				continue;

			if (strcmp(aggname, "sum") == 0)
				extra = lappend(extra,
								make_companion(pstate, aggref, "count",
											   tle->resname, next_resno++));
			else if (strcmp(aggname, "avg") == 0)
			{
				extra = lappend(extra,
								make_companion(pstate, aggref, "sum",
											   tle->resname, next_resno++));
				extra = lappend(extra,
								make_companion(pstate, aggref, "count",
											   tle->resname, next_resno++));
			}
		}

		rewritten->targetList = list_concat(rewritten->targetList, extra);
	}

	if (rewritten->distinctClause || rewritten->hasAggs)
	{
		FuncCall   *fn = makeFuncCall(SystemFuncName("count"), NIL,
									  COERCE_EXPLICIT_CALL, -1);
		Node	   *node;
		TargetEntry *tle;

		fn->agg_star = true;
		node = ParseFuncOrColumn(pstate, fn->funcname, NIL, NULL, fn, false, -1);

		tle = makeTargetEntry((Expr *) node,
							  list_length(rewritten->targetList) + 1,
							  pstrdup(GP_IVM_COUNT_COL),
							  false);
		rewritten->targetList = lappend(rewritten->targetList, tle);
		rewritten->hasAggs = true;
	}

	free_parsestate(pstate);
	return rewritten;
}

/* ------------------------------------------------------------------------- */
/* Marking the view, and the triggers that maintain it                       */
/* ------------------------------------------------------------------------- */

static ObjectAddress
matview_address(Oid matviewOid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, matviewOid);
	return addr;
}

bool
GpIvmIsIncremental(Oid matviewOid)
{
	ObjectAddress addr = matview_address(matviewOid);

	return GpLabelHas(&addr, GP_LABEL_incremental);
}

/*
 * One statement trigger.  The matview's OID travels in the trigger's
 * arguments, which is how a trigger function has always been told what it is
 * working on.  Cloudberry instead added a field to CreateTrigStmt, but that
 * field is only read when the statement is dispatched to segments, so a
 * single node does not need it -- and this file records the dependency
 * itself, which is what that field was for.
 */
static void
create_ivm_trigger(Oid relOid, Oid matviewOid, int16 events, int16 timing)
{
	CreateTrigStmt *stmt = makeNode(CreateTrigStmt);
	ObjectAddress address;
	ObjectAddress refaddr = matview_address(matviewOid);
	List	   *transitions = NIL;
	char		trigname[NAMEDATALEN];
	const char *base;

	switch (events)
	{
		case TRIGGER_TYPE_INSERT:
			base = "gp_ivm_ins";
			break;
		case TRIGGER_TYPE_DELETE:
			base = "gp_ivm_del";
			break;
		case TRIGGER_TYPE_UPDATE:
			base = "gp_ivm_upd";
			break;
		case TRIGGER_TYPE_TRUNCATE:
			base = "gp_ivm_trunc";
			break;
		default:
			elog(ERROR, "unsupported trigger event %d", events);
	}
	snprintf(trigname, sizeof(trigname), "%s_%s_%u", base,
			 timing == TRIGGER_TYPE_BEFORE ? "before" : "after", matviewOid);

	if (timing == TRIGGER_TYPE_AFTER)
	{
		if (events == TRIGGER_TYPE_INSERT || events == TRIGGER_TYPE_UPDATE)
		{
			TriggerTransition *n = makeNode(TriggerTransition);

			n->name = "__ivm_newtable";
			n->isNew = true;
			n->isTable = true;
			transitions = lappend(transitions, n);
		}
		if (events == TRIGGER_TYPE_DELETE || events == TRIGGER_TYPE_UPDATE)
		{
			TriggerTransition *n = makeNode(TriggerTransition);

			n->name = "__ivm_oldtable";
			n->isNew = false;
			n->isTable = true;
			transitions = lappend(transitions, n);
		}
	}

	stmt->trigname = pstrdup(trigname);
	stmt->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relOid)),
								  get_rel_name(relOid), -1);
	stmt->funcname = list_make2(makeString("gp_matview"),
								makeString(timing == TRIGGER_TYPE_BEFORE
										   ? "ivm_immediate_before"
										   : "ivm_immediate_maintenance"));
	stmt->args = list_make1(makeString(psprintf("%u", matviewOid)));
	stmt->row = false;
	stmt->timing = timing;
	stmt->events = events;
	stmt->columns = NIL;
	stmt->whenClause = NULL;
	stmt->transitionRels = transitions;
	stmt->isconstraint = false;
	stmt->deferrable = false;
	stmt->initdeferred = false;
	stmt->constrrel = NULL;

	address = CreateTrigger(stmt, NULL, relOid, InvalidOid, InvalidOid,
							InvalidOid, InvalidOid, InvalidOid, NULL,
							false, false);

	/* So that dropping the view takes its triggers with it. */
	recordDependencyOn(&address, &refaddr, DEPENDENCY_AUTO);

	CommandCounterIncrement();
}

static void
create_triggers_on_base_tables(Query *qry, Oid matviewOid)
{
	List	   *seen = NIL;
	ListCell   *lc;

	foreach(lc, qry->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
			continue;
		/* A table joined to itself needs only one set of triggers. */
		if (list_member_oid(seen, rte->relid))
			continue;
		seen = lappend_oid(seen, rte->relid);

		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_INSERT, TRIGGER_TYPE_BEFORE);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_DELETE, TRIGGER_TYPE_BEFORE);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_UPDATE, TRIGGER_TYPE_BEFORE);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_TRUNCATE, TRIGGER_TYPE_BEFORE);

		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_INSERT, TRIGGER_TYPE_AFTER);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_DELETE, TRIGGER_TYPE_AFTER);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_UPDATE, TRIGGER_TYPE_AFTER);
		create_ivm_trigger(rte->relid, matviewOid, TRIGGER_TYPE_TRUNCATE, TRIGGER_TYPE_AFTER);
	}

	list_free(seen);
}

void
GpIvmAfterCreate(Oid matviewOid, Query *rewritten)
{
	ObjectAddress addr = matview_address(matviewOid);

	/*
	 * Cloudberry sets pg_class.relisivm.  A label says the same thing, is
	 * dropped with the view, and pg_dump writes it, so the view still
	 * round-trips.
	 */
	GpLabelSet(&addr, GP_LABEL_incremental, "");
	CommandCounterIncrement();

	create_triggers_on_base_tables(rewritten, matviewOid);
}
