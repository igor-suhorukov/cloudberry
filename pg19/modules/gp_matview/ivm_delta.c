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
 *	  Bringing a materialized view up to date by what changed, not by asking
 *	  its query again.
 *
 * A statement that changes a base table leaves two transition tables behind:
 * the rows it removed and the rows it added.  Running the view's own query
 * over each of those, in place of the table, gives the rows the view loses
 * and the rows it gains.  Applying them is then arithmetic on the view:
 *
 *	- a view with GROUP BY or DISTINCT carries __ivm_count__, how many base
 *	  rows each view row stands for.  The old delta subtracts from it, and the
 *	  row goes when it reaches zero; the new delta adds to it, and a row that
 *	  matches nothing is inserted.
 *	- a view without them carries no count, and a view row corresponds to one
 *	  base row, so the old delta deletes and the new delta inserts.
 *
 * A view over more than one table needs one thing more.  Its delta is taken
 * one table at a time, and each step has to see the other tables as they were
 * *before* the statement -- otherwise a statement that changed two of them
 * would count the interaction twice.  The tables a step has already handled
 * are past that point and must be seen as they are now.  So every table this
 * statement changed starts in its pre-update state, which prestate_subquery()
 * below builds out of the table as it is now plus the rows the statement
 * deleted, and moves to its real self once its own delta has been taken.
 * ivm_state.c holds the snapshot that says which is which.
 *
 * The common case pays nothing for this.  The pre-update state of the table
 * being handled is overwritten by its own transition table before the query
 * runs, so with one table changed once -- which is what an ordinary INSERT,
 * UPDATE or DELETE does -- no prestate subquery is ever executed.
 *
 * This is Cloudberry's algebra, which is in turn the IVM patch's.  Two things
 * differ.  Cloudberry cannot use a data-modifying CTE, so it writes each
 * apply step through a temporary table keyed by ctid and gp_segment_id, and
 * says in a CBDB_IVM_FIXME that the CTE form should come back when
 * multiple-write CTEs are supported.  On PostgreSQL 19 they are, so the CTE
 * form is what is here.  And the transition tables reach SQL through
 * ephemeral named relations rather than through tuplestores passed by hand.
 *
 * What is still recomputed whole rather than maintained is listed in
 * delta_supported(): outer joins, aggregates other than count, sum and avg,
 * TRUNCATE, and a base table that has had a column dropped.  Each of those
 * leaves the view correct, just not incrementally.
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
 * Where each table this statement changed sits in the view's query.  A table
 * joined to itself sits in more than one place, and each place gets its own
 * delta.
 */
static void
locate_modified_tables(Query *viewQuery, IvmEntry *entry)
{
	ListCell   *lc;
	int			rti = 0;

	foreach(lc, viewQuery->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		IvmModifiedTable *table;

		rti++;
		if (rte->rtekind != RTE_RELATION)
			continue;

		table = GpIvmFindTable(entry, rte->relid);
		if (table != NULL)
			table->rte_indexes = lappend_int(table->rte_indexes, rti);
	}
}

/*
 * Does this table have a column that has been dropped?
 *
 * A dropped column keeps its place in the relation's descriptor but loses its
 * type, so a subquery standing in for the table cannot reproduce the
 * numbering that the view's Vars were built against: the columns after the
 * hole would shift by one.  Rather than read the wrong column, such a view is
 * recomputed.
 */
static bool
has_dropped_column(TupleDesc desc)
{
	for (int i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped)
			return true;
	}

	return false;
}

/* ------------------------------------------------------------------------- */
/* Computing a delta                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Replace the range table entry at rti with a parsed subquery.
 *
 * The substitution is whatever PostgreSQL builds for the text, so there is no
 * range table entry to construct by hand.
 */
static void
point_at_subquery(Query *query, int rti, const char *sql,
				  QueryEnvironment *queryEnv)
{
	RangeTblEntry *rte = (RangeTblEntry *) list_nth(query->rtable, rti - 1);
	ParseState *pstate = make_parsestate(NULL);
	RawStmt    *raw;
	Query	   *sub;

	pstate->p_queryEnv = queryEnv;
	pstate->p_expr_kind = EXPR_KIND_SELECT_TARGET;

	raw = (RawStmt *) linitial(raw_parser(sql, RAW_PARSE_DEFAULT));
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
}

/*
 * "SELECT * FROM <t1> UNION ALL SELECT * FROM <t2> ..." over a table's
 * transition tables.  There is more than one when a single statement wrote
 * the same table twice, which a data-modifying CTE does.
 */
static char *
transitions_subquery(List *transitions)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, transitions)
	{
		IvmTransition *tr = (IvmTransition *) lfirst(lc);

		if (buf.len > 0)
			appendStringInfoString(&buf, " UNION ALL ");
		appendStringInfo(&buf, "SELECT * FROM %s", quote_identifier(tr->name));
	}

	return buf.data;
}

/*
 * The table as the statement found it: what is there now and was already
 * there, plus what the statement deleted, which no scan can reach any more.
 */
static char *
prestate_subquery(Oid matviewOid, IvmModifiedTable *table)
{
	StringInfoData buf;
	char	   *relname;
	ListCell   *lc;

	relname = quote_qualified_identifier(get_namespace_name(get_rel_namespace(table->relid)),
										 get_rel_name(table->relid));

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "SELECT t.* FROM %s t"
					 " WHERE gp_matview.visible_in_prestate(t.tableoid, t.ctid, %u::pg_catalog.oid)",
					 relname, matviewOid);

	foreach(lc, table->old_stores)
	{
		IvmTransition *tr = (IvmTransition *) lfirst(lc);

		appendStringInfo(&buf, " UNION ALL SELECT * FROM %s",
						 quote_identifier(tr->name));
	}

	return buf.data;
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
 * Make one transition table visible to the delta queries, under the name this
 * statement's bookkeeping gave it.
 */
static void
register_transition(QueryEnvironment *queryEnv, IvmModifiedTable *table,
					IvmTransition *tr)
{
	EphemeralNamedRelation enr;

	enr = palloc0(sizeof(EphemeralNamedRelationData));
	enr->md.name = tr->name;
	enr->md.reliddesc = table->relid;
	enr->md.tupdesc = NULL;
	enr->md.enrtype = ENR_NAMED_TUPLESTORE;
	enr->md.enrtuples = tuplestore_tuple_count(tr->store);
	enr->reldata = tr->store;

	register_ENR(queryEnv, enr);
}

/*
 * Compute one delta and hand it to SPI under a name the apply statements use.
 * Returns false when nothing came out, so there is nothing to apply.
 */
static bool
make_delta(Relation matviewRel, Query *working, int rti, List *transitions,
		   const char *deltaname, QueryEnvironment *queryEnv,
		   Tuplestorestate **ts_out)
{
	Query	   *delta_query;
	Tuplestorestate *ts;
	TupleDesc	tupdesc;
	double		ntuples;
	EphemeralNamedRelation enr;
	char	   *sql;

	*ts_out = NULL;
	if (transitions == NIL)
		return false;

	/*
	 * The working query keeps the range table's state from step to step, so
	 * what is planned is a copy of it: planning scribbles on its input.
	 */
	sql = transitions_subquery(transitions);
	delta_query = copyObject(working);
	point_at_subquery(delta_query, rti, sql, queryEnv);
	pfree(sql);

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

	*ts_out = ts;
	return true;
}

static void
drop_delta(const char *deltaname, Tuplestorestate *ts)
{
	if (ts == NULL)
		return;

	SPI_unregister_relation(deltaname);
	tuplestore_end(ts);
}

/* ------------------------------------------------------------------------- */
/* Applying a delta                                                          */
/* ------------------------------------------------------------------------- */

/*
 * An avg() column and the two columns that make it maintainable: the sum of
 * its inputs and how many of them were not null.
 */
typedef struct ViewAvg
{
	Form_pg_attribute col;
	Form_pg_attribute sum;
	Form_pg_attribute count;
} ViewAvg;

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
	List	   *avgs;			/* ViewAvg *: avg() columns */
	Form_pg_attribute count_col;	/* __ivm_count__, or NULL */
	bool		supported;		/* can this view's delta be applied? */
} ViewShape;

/*
 * The column a rewritten view query put beside an aggregate, by name.
 */
static Form_pg_attribute
find_companion(TupleDesc desc, const char *kind, const char *resname)
{
	char	   *want = ivm_companion_name(kind, resname);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && strcmp(NameStr(att->attname), want) == 0)
			return att;
	}

	return NULL;
}

static void
describe_view(Relation matviewRel, Query *viewQuery, ViewShape *shape)
{
	TupleDesc	desc = RelationGetDescr(matviewRel);
	ListCell   *lc;

	shape->keys = NIL;
	shape->counts = NIL;
	shape->sums = NIL;
	shape->companions = NIL;
	shape->avgs = NIL;
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

		/* A companion column travels with its aggregate, not on its own. */
		if (IsIvmColumn(NameStr(att->attname)))
			continue;

		if (IsA(tle->expr, Aggref))
		{
			char	   *aggname = get_func_name(((Aggref *) tle->expr)->aggfnoid);

			/*
			 * count() is maintainable on its own.  sum() is too, given the
			 * count of its non-null inputs that the rewrite added beside it:
			 * without that, an empty group could not be told from one that
			 * sums to nothing.  avg() is that sum and that count divided.
			 *
			 * min() and max() are not: when the row holding the extreme goes,
			 * the new extreme is somewhere in the group and only the base
			 * table knows where.  Cloudberry refuses such a view outright;
			 * here it is made, and recomputed whole on every change.
			 */
			if (aggname == NULL)
				shape->supported = false;
			else if (strcmp(aggname, "count") == 0)
				shape->counts = lappend(shape->counts, att);
			else if (strcmp(aggname, "sum") == 0)
				shape->sums = lappend(shape->sums, att);
			else if (strcmp(aggname, "avg") == 0)
			{
				ViewAvg    *avg = palloc0(sizeof(ViewAvg));

				avg->col = att;
				avg->sum = find_companion(desc, "sum", NameStr(att->attname));
				avg->count = find_companion(desc, "count", NameStr(att->attname));
				if (avg->sum == NULL || avg->count == NULL)
					shape->supported = false;
				else
					shape->avgs = lappend(shape->avgs, avg);
			}
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
		Form_pg_attribute found = find_companion(desc, "count",
												 NameStr(sum->attname));

		if (found == NULL)
		{
			shape->supported = false;
			return;
		}
		shape->companions = lappend(shape->companions, found);
	}

	/* Without a count column, a view row stands for exactly one base row. */
	if (shape->count_col == NULL && shape->counts == NIL &&
		shape->sums == NIL && shape->avgs == NIL)
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

/* What a count column becomes: the view's, plus or minus the delta's. */
static char *
new_count_expr(const char *op, const char *src, const char *cnt)
{
	return psprintf("(mv.%s OPERATOR(pg_catalog.%s) %s.%s)", cnt, op, src, cnt);
}

/*
 * What a sum column becomes.
 *
 * A sum is NULL when it has no non-null inputs left, which is what its
 * companion count says.  Otherwise a NULL on either side means that side
 * contributed nothing.
 */
static char *
new_sum_expr(const char *op, const char *src, const char *sum, const char *cnt)
{
	return psprintf("(CASE WHEN %s OPERATOR(pg_catalog.=) 0 THEN NULL"
					" WHEN mv.%s IS NULL THEN %s.%s"
					" WHEN %s.%s IS NULL THEN mv.%s"
					" ELSE mv.%s OPERATOR(pg_catalog.%s) %s.%s END)",
					new_count_expr(op, src, cnt),
					sum, src, sum,
					src, sum, sum,
					sum, op, src, sum);
}

/*
 * The SET clauses that carry the aggregate columns across, "+" for a new
 * delta and "-" for an old one.  <src> is what the delta row is called.
 */
static char *
aggregate_set_clauses(ViewShape *shape, const char *op, const char *src)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);

	foreach(lc, shape->counts)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);
		const char *name = quote_identifier(NameStr(att->attname));

		appendStringInfo(&buf, ", %s = %s", name,
						 new_count_expr(op, src, name));
	}

	foreach(lc, shape->sums)
	{
		Form_pg_attribute att = (Form_pg_attribute) lfirst(lc);
		const char *name = quote_identifier(NameStr(att->attname));
		const char *cnt = quote_identifier(ivm_companion_name("count", NameStr(att->attname)));

		appendStringInfo(&buf, ", %s = %s", name,
						 new_sum_expr(op, src, name, cnt));
		/* and the companion count moves with it */
		appendStringInfo(&buf, ", %s = %s", cnt, new_count_expr(op, src, cnt));
	}

	foreach(lc, shape->avgs)
	{
		ViewAvg    *avg = (ViewAvg *) lfirst(lc);
		const char *name = quote_identifier(NameStr(avg->col->attname));
		const char *sum = quote_identifier(NameStr(avg->sum->attname));
		const char *cnt = quote_identifier(NameStr(avg->count->attname));
		char	   *count_expr = new_count_expr(op, src, cnt);
		char	   *sum_expr = new_sum_expr(op, src, sum, cnt);

		/*
		 * The average is not carried across; it is the sum and the count that
		 * are, and it is read off them.  Every SET clause of one UPDATE sees
		 * the row as it was, so what the other two clauses will store has to
		 * be written out again here rather than referred to.
		 */
		appendStringInfo(&buf,
						 ", %s = (CASE WHEN %s OPERATOR(pg_catalog.=) 0 THEN NULL"
						 " ELSE CAST(%s AS %s) OPERATOR(pg_catalog./) %s END)",
						 name, count_expr,
						 sum_expr, format_type_be(avg->col->atttypid),
						 count_expr);
		appendStringInfo(&buf, ", %s = %s", sum, sum_expr);
		appendStringInfo(&buf, ", %s = %s", cnt, count_expr);
	}

	return buf.data;
}

/* Everything a delta row carries into the view. */
static List *
all_view_columns(ViewShape *shape)
{
	List	   *all = list_concat_copy(shape->keys, shape->counts);
	ListCell   *lc;

	all = list_concat(all, list_copy(shape->sums));
	all = list_concat(all, list_copy(shape->companions));

	foreach(lc, shape->avgs)
	{
		ViewAvg    *avg = (ViewAvg *) lfirst(lc);

		all = lappend(all, avg->col);
		all = lappend(all, avg->sum);
		all = lappend(all, avg->count);
	}

	return all;
}

/* The aggregate columns a delta row carries, for the old delta's CTE. */
static List *
all_aggregate_columns(ViewShape *shape)
{
	List	   *all = list_concat_copy(shape->counts, shape->sums);
	ListCell   *lc;

	all = list_concat(all, list_copy(shape->companions));

	foreach(lc, shape->avgs)
	{
		ViewAvg    *avg = (ViewAvg *) lfirst(lc);

		all = lappend(all, avg->col);
		all = lappend(all, avg->sum);
		all = lappend(all, avg->count);
	}

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
	char	   *sets = aggregate_set_clauses(shape, "-", "t");
	char	   *carried = column_list(all_aggregate_columns(shape), "diff");

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
 *
 * The delta is folded to one row per distinct value first.  Left as it is, a
 * delta that itself holds duplicates would join to every matching view row
 * once per copy, and the row numbering would then run over a view row more
 * than once and delete fewer rows than it should.  A join view is where both
 * sides hold duplicates at once.
 */
static void
apply_old_delta_no_count(const char *mvname, ViewShape *shape)
{
	StringInfoData buf;
	char	   *match = matching_condition(shape->keys);
	char	   *keys = column_list(shape->keys, NULL);

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "DELETE FROM %s WHERE ctid IN ("
					 "  SELECT __tid FROM ("
					 "    SELECT pg_catalog.row_number() OVER (PARTITION BY %s) AS __rn,"
					 "           mv.ctid AS __tid, diff.__cnt"
					 "    FROM %s AS mv,"
					 "         (SELECT %s, pg_catalog.count(*) AS __cnt"
					 "          FROM %s GROUP BY %s) AS diff"
					 "    WHERE %s) v"
					 "  WHERE v.__rn OPERATOR(pg_catalog.<=) v.__cnt)",
					 mvname,
					 column_list(shape->keys, "mv"),
					 mvname,
					 keys, IVM_OLD_DELTA, keys,
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
	char	   *sets = aggregate_set_clauses(shape, "+", "diff");
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
 * Can this statement's effect on this view be expressed as a delta?
 */
static bool
delta_supported(Query *viewQuery, IvmEntry *entry, ViewShape *shape)
{
	ListCell   *lc;

	/*
	 * TRUNCATE leaves no transition tables, so there is nothing to compute a
	 * delta from: what the view loses is everything that came from that
	 * table.
	 */
	if (entry->truncated)
		return false;

	if (!shape->supported)
		return false;

	/*
	 * An outer join's delta is not this arithmetic.  Deleting the last row on
	 * the inner side does not remove a view row, it turns the inner columns
	 * to null, and knowing which needs a count per outer row that the rewrite
	 * does not add.  Such a view is recomputed.
	 */
	foreach(lc, viewQuery->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_JOIN && rte->jointype != JOIN_INNER)
			return false;
	}

	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);

		/* A table the view does not read cannot have changed it. */
		if (table->rte_indexes == NIL)
			return false;

		if (has_dropped_column(table->tupdesc))
			return false;
	}

	return true;
}

/*
 * Bring a view up to date from the transition tables the statement left.
 * Returns false when this view's delta cannot be computed, so the caller
 * recomputes it whole instead.
 */
bool
GpIvmApplyDelta(IvmEntry *entry)
{
	Relation	matviewRel;
	Query	   *viewQuery;
	Query	   *working;
	QueryEnvironment *queryEnv;
	ViewShape	shape;
	char	   *mvname;
	RangeTblEntry **original;
	int			nrtable;
	ListCell   *lc;

	/*
	 * The apply statements find view rows by ctid and then write them, so no
	 * one else may be maintaining this view at the same time.  This is the
	 * lock Cloudberry takes, and for the same reason.
	 */
	matviewRel = table_open(entry->matviewOid, ExclusiveLock);
	viewQuery = GpIvmGetViewQuery(matviewRel);

	describe_view(matviewRel, viewQuery, &shape);
	locate_modified_tables(viewQuery, entry);

	if (!delta_supported(viewQuery, entry, &shape))
	{
		table_close(matviewRel, NoLock);
		return false;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * The transition tables are what the delta queries read, so they are put
	 * in an environment of their own; the apply statements read the deltas
	 * instead, which SPI is told about as each one is computed.
	 */
	queryEnv = create_queryEnv();
	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, table->old_stores)
			register_transition(queryEnv, table, (IvmTransition *) lfirst(lc2));
		foreach(lc2, table->new_stores)
			register_transition(queryEnv, table, (IvmTransition *) lfirst(lc2));
	}

	mvname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
										RelationGetRelationName(matviewRel));

	/*
	 * The working copy carries the range table from step to step.  Its RTEs
	 * come from a stored rule, so they are locked here rather than left to
	 * the planner.
	 */
	working = copyObject(viewQuery);
	AcquireRewriteLocks(working, true, false);

	nrtable = list_length(working->rtable);
	original = (RangeTblEntry **) palloc0((nrtable + 1) * sizeof(RangeTblEntry *));

	/* Every table this statement changed starts as the statement found it. */
	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, table->rte_indexes)
		{
			int			rti = lfirst_int(lc2);
			char	   *sql = prestate_subquery(entry->matviewOid, table);

			/*
			 * Kept whole, because pointing the entry at the pre-update state
			 * overwrites it in place, and it is what the table goes back to
			 * once its own delta has been taken.
			 */
			original[rti] = copyObject((RangeTblEntry *) list_nth(working->rtable, rti - 1));
			point_at_subquery(working, rti, sql, queryEnv);
			pfree(sql);
		}
	}

	/*
	 * One table at a time, and one place in the query at a time: take that
	 * place's delta, then leave the table as it is now, so the next step sees
	 * a change that has already been accounted for.
	 */
	foreach(lc, entry->tables)
	{
		IvmModifiedTable *table = (IvmModifiedTable *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, table->rte_indexes)
		{
			int			rti = lfirst_int(lc2);
			Tuplestorestate *old_ts;
			Tuplestorestate *new_ts;
			bool		old_delta;
			bool		new_delta;

			old_delta = make_delta(matviewRel, working, rti, table->old_stores,
								   IVM_OLD_DELTA, queryEnv, &old_ts);
			new_delta = make_delta(matviewRel, working, rti, table->new_stores,
								   IVM_NEW_DELTA, queryEnv, &new_ts);

			/* This place is now past its change. */
			lfirst(list_nth_cell(working->rtable, rti - 1)) = original[rti];

			/* Old before new: a row that is both removed and added stays. */
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

			drop_delta(IVM_OLD_DELTA, old_ts);
			drop_delta(IVM_NEW_DELTA, new_ts);
		}
	}

	SPI_finish();
	table_close(matviewRel, NoLock);

	return true;
}
