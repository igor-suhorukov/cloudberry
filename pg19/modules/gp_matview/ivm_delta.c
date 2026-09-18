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
 * ivm_delta.c
 *	  Applying a delta to an incrementally maintained materialized view.
 *
 * The view is kept up to date by computing what its contents would change by,
 * rather than by computing its contents again.
 *
 * A statement that changes a base table leaves two transition tables behind:
 * the rows as they were and the rows as they are.  Running the view's own
 * query over each of those, instead of over the base table, gives the rows the
 * view loses and the rows it gains -- its old and new delta.  Applying them is
 * then arithmetic on the view:
 *
 *	- a view with GROUP BY or DISTINCT carries __ivm_count__, how many base
 *	  rows each view row stands for.  The old delta subtracts from it, and the
 *	  row goes when it reaches zero; the new delta adds to it, and a row that
 *	  matches nothing is inserted.
 *	- a view without them carries no count, and a view row corresponds to one
 *	  base row, so the old delta deletes and the new delta inserts.
 *
 * This is Cloudberry's algebra, which is in turn the IVM patch's.  Two things
 * differ.  Cloudberry cannot use a data-modifying CTE, so it writes each
 * apply step through a temporary table keyed by ctid and gp_segment_id, and
 * says in a CBDB_IVM_FIXME that the CTE form should come back when
 * multiple-write CTEs are supported.  On PostgreSQL 19 they are, so the CTE
 * form is what is here.  And the transition tables reach SQL through
 * SPI_register_trigger_data rather than through tuplestores passed by hand.
 *
 * What is not here yet is a view over more than one table.  Its delta needs
 * the other tables as they were before the statement, which is what
 * Cloudberry's rewrite_query_for_preupdate_state and ivm_visible_in_prestate
 * are for.  Until that is ported, such a view is recomputed whole, which is
 * correct but not incremental; GpIvmDeltaSupported says which is which.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tstoreReceiver.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_node.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/queryenvironment.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "gp_matview.h"

#define IVM_OLD_TRANSITION	"__ivm_oldtable"
#define IVM_NEW_TRANSITION	"__ivm_newtable"
#define IVM_OLD_DELTA		"__ivm_delta_old"
#define IVM_NEW_DELTA		"__ivm_delta_new"

/* ------------------------------------------------------------------------- */
/* The view's own query                                                      */
/* ------------------------------------------------------------------------- */

/*
 * The query a materialized view was made from, as stored in its rewrite rule.
 */
Query *
GpIvmGetViewQuery(Relation matviewRel)
{
	RewriteRule *rule;
	Query	   *query;

	if (matviewRel->rd_rules == NULL || matviewRel->rd_rules->numLocks != 1)
		elog(ERROR, "materialized view \"%s\" has no single rewrite rule",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !rule->isInstead ||
		list_length(rule->actions) != 1)
		elog(ERROR, "the rewrite rule of materialized view \"%s\" is not what a view has",
			 RelationGetRelationName(matviewRel));

	query = (Query *) linitial(rule->actions);
	return query;
}

/*
 * Whether this view's delta can be computed: for now, one base table, which
 * is what a single-table view has.  Returns the range table index of that
 * table in *rti.
 */
bool
GpIvmDeltaSupported(Query *viewQuery, Oid baseRelid, int *rti)
{
	int			found_rti = 0;
	int			nrel = 0;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, viewQuery->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		i++;
		if (rte->rtekind != RTE_RELATION)
			continue;
		nrel++;
		if (rte->relid == baseRelid)
			found_rti = i;
	}

	if (nrel != 1 || found_rti == 0)
		return false;

	*rti = found_rti;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Computing a delta                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Point the view query at a transition table instead of its base table.
 *
 * The substitution is a parsed "SELECT * FROM <transition>", so the range
 * table entry that results is whatever PostgreSQL builds for a named
 * tuplestore -- there is nothing to construct by hand.
 */
static void
point_at_transition(Query *query, int rti, const char *enrname,
					QueryEnvironment *queryEnv)
{
	RangeTblEntry *rte = (RangeTblEntry *) list_nth(query->rtable, rti - 1);
	ParseState *pstate = make_parsestate(NULL);
	StringInfoData buf;
	RawStmt    *raw;
	Query	   *sub;

	pstate->p_queryEnv = queryEnv;
	pstate->p_expr_kind = EXPR_KIND_SELECT_TARGET;

	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT * FROM %s", quote_identifier(enrname));

	raw = (RawStmt *) linitial(raw_parser(buf.data, RAW_PARSE_DEFAULT));
	sub = transformStmt(pstate, raw->stmt);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = sub;
	rte->security_barrier = false;

	/* Fields a subquery RTE must not carry. */
	rte->relid = InvalidOid;
	rte->relkind = 0;
	rte->rellockmode = 0;
	rte->tablesample = NULL;
	rte->perminfoindex = 0;
	rte->inh = false;

	free_parsestate(pstate);
	pfree(buf.data);
}

/*
 * Run a query into a tuplestore, and describe what came out.
 */
static Tuplestorestate *
run_into_tuplestore(Query *query, QueryEnvironment *queryEnv,
					TupleDesc *tupdesc_out, double *ntuples_out)
{
	PlannedStmt *plan;
	QueryDesc  *qd;
	DestReceiver *dest;
	Tuplestorestate *ts;

	ts = tuplestore_begin_heap(false, false, work_mem);
	dest = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(dest, ts, CurrentMemoryContext, false,
									NULL, NULL);

	plan = pg_plan_query(query, NULL, CURSOR_OPT_PARALLEL_OK, NULL, NULL);

	qd = CreateQueryDesc(plan, "gp_matview delta", GetActiveSnapshot(),
						 InvalidSnapshot, dest, NULL, queryEnv, 0);
	ExecutorStart(qd, 0);
	ExecutorRun(qd, ForwardScanDirection, 0);

	*tupdesc_out = CreateTupleDescCopy(qd->tupDesc);
	*ntuples_out = (double) qd->estate->es_processed;

	ExecutorFinish(qd);
	ExecutorEnd(qd);
	FreeQueryDesc(qd);
	dest->rDestroy(dest);

	return ts;
}

/*
 * Make one transition table visible to the delta queries, under the name its
 * trigger gave it.  This is what SPI_register_trigger_data does for SPI.
 */
static void
register_transition(QueryEnvironment *queryEnv, TriggerData *trigdata,
					Tuplestorestate *ts, const char *name)
{
	EphemeralNamedRelation enr;

	if (ts == NULL || name == NULL)
		return;

	enr = palloc0(sizeof(EphemeralNamedRelationData));
	enr->md.name = pstrdup(name);
	enr->md.reliddesc = RelationGetRelid(trigdata->tg_relation);
	enr->md.tupdesc = NULL;
	enr->md.enrtype = ENR_NAMED_TUPLESTORE;
	enr->md.enrtuples = tuplestore_tuple_count(ts);
	enr->reldata = ts;

	register_ENR(queryEnv, enr);
}

/*
 * Compute one delta and hand it to SPI under a name the apply statements use.
 * Returns false when the transition table was empty, so there is nothing to
 * apply.
 */
static bool
make_delta(Relation matviewRel, Query *viewQuery, int rti,
		   const char *transition, const char *deltaname,
		   QueryEnvironment *queryEnv)
{
	Query	   *delta_query;
	Tuplestorestate *ts;
	TupleDesc	tupdesc;
	double		ntuples;
	EphemeralNamedRelation enr;

	if (get_visible_ENR_metadata(queryEnv, transition) == NULL)
		return false;

	delta_query = copyObject(viewQuery);
	point_at_transition(delta_query, rti, transition, queryEnv);

	ts = run_into_tuplestore(delta_query, queryEnv, &tupdesc, &ntuples);

	if (ntuples == 0)
	{
		tuplestore_end(ts);
		return false;
	}

	enr = palloc0(sizeof(EphemeralNamedRelationData));
	enr->md.name = pstrdup(deltaname);
	enr->md.reliddesc = InvalidOid;
	enr->md.tupdesc = tupdesc;
	enr->md.enrtype = ENR_NAMED_TUPLESTORE;
	enr->md.enrtuples = ntuples;
	enr->reldata = ts;

	if (SPI_register_relation(enr) != SPI_OK_REL_REGISTER)
		elog(ERROR, "could not register the %s delta of \"%s\"",
			 deltaname, RelationGetRelationName(matviewRel));

	return true;
}

/* ------------------------------------------------------------------------- */
/* Applying a delta                                                          */
/* ------------------------------------------------------------------------- */

/*
 * What a view's columns are for.
 *
 * A view row is identified by the columns the query groups by -- not by every
 * visible column, because an aggregate's value is a consequence of the row
 * rather than part of its identity.  A view with neither GROUP BY nor DISTINCT
 * has no count and no aggregates, and there every column is part of the
 * identity.
 */
typedef struct ViewShape
{
	List	   *keys;			/* Form_pg_attribute: identify a view row */
	List	   *counts;			/* Form_pg_attribute: count() columns */
	List	   *sums;			/* Form_pg_attribute: sum() columns */
	List	   *companions;		/* the count that goes with each sum */
	Form_pg_attribute count_col;	/* __ivm_count__, or NULL */
	bool		supported;		/* can this view's delta be applied? */
} ViewShape;

static void
describe_view(Relation matviewRel, Query *viewQuery, ViewShape *shape)
{
	TupleDesc	desc = RelationGetDescr(matviewRel);
	ListCell   *lc;

	shape->keys = NIL;
	shape->counts = NIL;
	shape->sums = NIL;
	shape->companions = NIL;
	shape->count_col = NULL;
	shape->supported = true;

	foreach(lc, viewQuery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Form_pg_attribute att;

		if (tle->resjunk || tle->resno > desc->natts)
			continue;
		att = TupleDescAttr(desc, tle->resno - 1);
		if (att->attisdropped)
			continue;

		if (strcmp(NameStr(att->attname), GP_IVM_COUNT_COL) == 0)
		{
			shape->count_col = att;
			continue;
		}

		/* A companion count travels with its sum, not on its own. */
		if (IsIvmColumn(NameStr(att->attname)))
			continue;

		if (IsA(tle->expr, Aggref))
		{
			char	   *aggname = get_func_name(((Aggref *) tle->expr)->aggfnoid);

			/*
			 * count() is maintainable on its own.  sum() is too, given the
			 * count of its non-null inputs that the rewrite added beside it:
			 * without that, an empty group could not be told from one that
			 * sums to nothing.  min(), max() and avg() need more than that,
			 * so a view using them is recomputed rather than maintained.
			 */
			if (aggname == NULL)
				shape->supported = false;
			else if (strcmp(aggname, "count") == 0)
				shape->counts = lappend(shape->counts, att);
			else if (strcmp(aggname, "sum") == 0)
				shape->sums = lappend(shape->sums, att);
			else
				shape->supported = false;
			continue;
		}

		shape->keys = lappend(shape->keys, att);
	}

	/*
	 * Each sum needs the count that was added beside it.  A view made before
	 * that column existed has none, and is recomputed rather than maintained.
	 */
	foreach(lc, shape->sums)
	{
		Form_pg_attribute sum = (Form_pg_attribute) lfirst(lc);
		char	   *want = ivm_companion_name(NameStr(sum->attname));
		Form_pg_attribute found = NULL;

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped && strcmp(NameStr(att->attname), want) == 0)
			{
				found = att;
				break;
			}
		}

		if (found == NULL)
		{
			shape->supported = false;
			return;
		}
		shape->companions = lappend(shape->companions, found);
	}

	/* Without a count column, a view row stands for exactly one base row. */
	if (shape->count_col == NULL && shape->counts == NIL && shape->sums == NIL)
	{
		shape->keys = NIL;
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped && !IsIvmColumn(NameStr(att->attname)))
				shape->keys = lappend(shape->keys, att);
		}
	}
}

/*
 * How a view row is matched to a delta row.  NULL is a value here, not the
 * absence of one: two rows that group NULL together are the same view row.
 */
static char *
matching_condition(List *keys)
{
	StringInfoData buf;
	ListCell   *lc;

	if (keys == NIL)
		return "true";

	initStringInfo(&buf);
	foreach(lc, keys)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);

		appendStringInfo(&buf, "%s%s IS NOT DISTINCT FROM %s",
						 buf.len > 0 ? " AND " : "",
						 quote_qualified_identifier("mv", NameStr(att->attname)),
						 quote_qualified_identifier("diff", NameStr(att->attname)));
	}

	return buf.data;
}

static char *
column_list(List *cols, const char *prefix)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, cols)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);

		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf,
							   prefix
							   ? quote_qualified_identifier(prefix, NameStr(att->attname))
							   : quote_identifier(NameStr(att->attname)));
	}

	return buf.data;
}

/*
 * The SET clauses that carry the count columns across, "+" for a new delta
 * and "-" for an old one.  <src> is where the delta row is called.
 */
static char *
count_set_clauses(ViewShape *shape, const char *op, const char *src)
{
	StringInfoData buf;
	ListCell   *lc;
	bool		adding = (op[0] == '+');

	initStringInfo(&buf);

	foreach(lc, shape->counts)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);
		char	   *name = quote_identifier(NameStr(att->attname));

		appendStringInfo(&buf, ", %s = mv.%s OPERATOR(pg_catalog.%s) %s.%s",
						 name, name, op, src, name);
	}

	foreach(lc, shape->sums)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);
		char	   *name = quote_identifier(NameStr(att->attname));
		char	   *cnt = quote_identifier(ivm_companion_name(NameStr(att->attname)));

		/*
		 * A sum is NULL when it has no non-null inputs left, which the
		 * companion count is what says.  Otherwise a NULL on either side
		 * means that side contributed nothing.
		 */
		appendStringInfo(&buf,
						 ", %s = (CASE WHEN %s THEN NULL"
						 " WHEN mv.%s IS NULL THEN %s.%s"
						 " WHEN %s.%s IS NULL THEN mv.%s"
						 " ELSE mv.%s OPERATOR(pg_catalog.%s) %s.%s END)",
						 name,
						 adding
						 ? psprintf("mv.%s OPERATOR(pg_catalog.=) 0 AND %s.%s OPERATOR(pg_catalog.=) 0",
									cnt, src, cnt)
						 : psprintf("mv.%s OPERATOR(pg_catalog.=) %s.%s",
									cnt, src, cnt),
						 name, src, name,
						 src, name, name,
						 name, op, src, name);

		/* and the companion count moves with it */
		appendStringInfo(&buf, ", %s = mv.%s OPERATOR(pg_catalog.%s) %s.%s",
						 cnt, cnt, op, src, cnt);
	}

	return buf.data;
}

/* Everything a delta row carries into the view. */
static List *
all_view_columns(ViewShape *shape)
{
	List	   *all = list_concat_copy(shape->keys, shape->counts);

	all = list_concat(all, list_copy(shape->sums));
	all = list_concat(all, list_copy(shape->companions));
	return all;
}

static void
run(const char *sql, int expected)
{
	int			ret = SPI_exec(sql, 0);

	if (ret != expected)
		elog(ERROR, "gp_matview: applying a delta failed (%d): %s", ret, sql);
}

/*
 * The old delta: rows the view loses.
 *
 * Each view row's count falls by the delta's, and the row goes when nothing
 * is left of it.  Both happen in one statement, so the view is read once.
 */
static void
apply_old_delta_with_count(const char *mvname, ViewShape *shape)
{
	StringInfoData buf;
	char	   *match = matching_condition(shape->keys);
	const char *count = quote_identifier(NameStr(shape->count_col->attname));
	bool		no_group_by = (shape->keys == NIL);
	char	   *sets = count_set_clauses(shape, "-", "t");
	List	   *aggcols = list_concat_copy(shape->counts, shape->sums);
	char	   *carried;

	aggcols = list_concat(aggcols, list_copy(shape->companions));
	carried = column_list(aggcols, "diff");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "WITH t AS ("
					 "  SELECT diff.%s AS __cnt,"
					 "         (diff.%s OPERATOR(pg_catalog.=) mv.%s AND %s) AS __for_dlt,"
					 "         mv.ctid AS __tid%s%s"
					 "  FROM %s AS mv, %s AS diff WHERE %s"
					 "), updt AS ("
					 "  UPDATE %s AS mv SET %s = mv.%s OPERATOR(pg_catalog.-) t.__cnt%s"
					 "  FROM t WHERE mv.ctid OPERATOR(pg_catalog.=) t.__tid AND NOT t.__for_dlt"
					 ")"
					 "DELETE FROM %s AS mv USING t"
					 " WHERE mv.ctid OPERATOR(pg_catalog.=) t.__tid AND t.__for_dlt",
					 count,
					 count, count, no_group_by ? "false" : "true",
					 carried[0] ? ", " : "", carried,
					 mvname, IVM_OLD_DELTA, match,
					 mvname, count, count, sets,
					 mvname);
	run(buf.data, SPI_OK_DELETE);
	pfree(buf.data);
}

/*
 * Without a count, a view row stands for one base row, so exactly as many
 * view rows go as the delta holds -- which is what the row number is for when
 * the view has duplicates.
 */
static void
apply_old_delta_no_count(const char *mvname, ViewShape *shape)
{
	StringInfoData buf;
	char	   *match = matching_condition(shape->keys);

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "DELETE FROM %s WHERE ctid IN ("
					 "  SELECT __tid FROM ("
					 "    SELECT pg_catalog.row_number() OVER (PARTITION BY %s) AS __rn,"
					 "           mv.ctid AS __tid, diff.__cnt"
					 "    FROM %s AS mv,"
					 "         (SELECT *, pg_catalog.count(*) OVER (PARTITION BY %s) AS __cnt"
					 "          FROM %s) AS diff"
					 "    WHERE %s) v"
					 "  WHERE v.__rn OPERATOR(pg_catalog.<=) v.__cnt)",
					 mvname,
					 column_list(shape->keys, "mv"),
					 mvname,
					 column_list(shape->keys, NULL),
					 IVM_OLD_DELTA,
					 match);
	run(buf.data, SPI_OK_DELETE);
	pfree(buf.data);
}

/*
 * The new delta: rows the view gains.  A row that matches one already there
 * adds to its counts; one that matches nothing is inserted whole.
 */
static void
apply_new_delta_with_count(const char *mvname, ViewShape *shape)
{
	StringInfoData buf;
	char	   *match = matching_condition(shape->keys);
	const char *count = quote_identifier(NameStr(shape->count_col->attname));
	char	   *sets = count_set_clauses(shape, "+", "diff");
	List	   *all = all_view_columns(shape);
	char	   *returning = column_list(shape->keys, "mv");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "WITH updt AS ("
					 "  UPDATE %s AS mv SET %s = mv.%s OPERATOR(pg_catalog.+) diff.%s%s"
					 "  FROM %s AS diff WHERE %s"
					 "  RETURNING %s"
					 ")"
					 "INSERT INTO %s (%s, %s)"
					 " SELECT %s, diff.%s FROM %s AS diff"
					 " WHERE NOT EXISTS (SELECT 1 FROM updt AS mv WHERE %s)",
					 mvname, count, count, count, sets,
					 IVM_NEW_DELTA, match,
					 returning[0] ? returning : "mv.ctid",
					 mvname, column_list(all, NULL), count,
					 column_list(all, "diff"), count, IVM_NEW_DELTA,
					 match);
	run(buf.data, SPI_OK_INSERT);
	pfree(buf.data);
}

static void
apply_new_delta_no_count(const char *mvname, ViewShape *shape)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "INSERT INTO %s (%s) SELECT %s FROM %s",
					 mvname, column_list(shape->keys, NULL),
					 column_list(shape->keys, NULL), IVM_NEW_DELTA);
	run(buf.data, SPI_OK_INSERT);
	pfree(buf.data);
}

/* ------------------------------------------------------------------------- */

/*
 * Bring a view up to date from the transition tables of the statement that
 * changed one of its base tables.  Returns false when this view's delta
 * cannot be computed yet, so the caller recomputes it whole instead.
 */
bool
GpIvmApplyDelta(Oid matviewOid, Oid baseRelid, TriggerData *trigdata)
{
	Relation	matviewRel;
	Query	   *viewQuery;
	QueryEnvironment *queryEnv;
	ViewShape	shape;
	int			rti;
	char	   *mvname;
	bool		old_delta,
				new_delta;

	/*
	 * TRUNCATE leaves no transition tables, so there is no delta to compute:
	 * what the view loses is everything that came from that table.  The
	 * caller recomputes instead.
	 */
	if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		return false;

	/*
	 * The apply statements find view rows by ctid and then write them, so no
	 * one else may be maintaining this view at the same time.  This is the
	 * lock Cloudberry takes, and for the same reason.
	 */
	matviewRel = table_open(matviewOid, ExclusiveLock);
	viewQuery = GpIvmGetViewQuery(matviewRel);

	describe_view(matviewRel, viewQuery, &shape);

	if (!shape.supported || !GpIvmDeltaSupported(viewQuery, baseRelid, &rti))
	{
		table_close(matviewRel, NoLock);
		return false;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * The transition tables have to be visible twice over: to the queries
	 * that compute the deltas, which are planned and run here, and to the
	 * statements that apply them, which go through SPI.  SPI keeps its own
	 * environment private, so it is told separately.
	 */
	queryEnv = create_queryEnv();
	register_transition(queryEnv, trigdata, trigdata->tg_oldtable,
						trigdata->tg_trigger->tgoldtable);
	register_transition(queryEnv, trigdata, trigdata->tg_newtable,
						trigdata->tg_trigger->tgnewtable);

	if (SPI_register_trigger_data(trigdata) != SPI_OK_TD_REGISTER)
		elog(ERROR, "could not register the transition tables");

	mvname = quote_qualified_identifier(
										get_namespace_name(RelationGetNamespace(matviewRel)),
										RelationGetRelationName(matviewRel));

	/* The view query is copied per delta, so this one is never scribbled on. */
	old_delta = trigdata->tg_oldtable != NULL &&
		make_delta(matviewRel, viewQuery, rti,
				   trigdata->tg_trigger->tgoldtable, IVM_OLD_DELTA, queryEnv);
	new_delta = trigdata->tg_newtable != NULL &&
		make_delta(matviewRel, viewQuery, rti,
				   trigdata->tg_trigger->tgnewtable, IVM_NEW_DELTA, queryEnv);

	/* Old before new: a row that is both removed and added must not be lost. */
	if (old_delta)
	{
		if (shape.count_col != NULL)
			apply_old_delta_with_count(mvname, &shape);
		else
			apply_old_delta_no_count(mvname, &shape);
	}
	if (new_delta)
	{
		if (shape.count_col != NULL)
			apply_new_delta_with_count(mvname, &shape);
		else
			apply_new_delta_no_count(mvname, &shape);
	}

	SPI_finish();
	table_close(matviewRel, NoLock);

	return true;
}
