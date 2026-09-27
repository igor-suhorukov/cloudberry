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
 * gp_segment.c
 *	  gp_segment_id: Cloudberry's system column, as a function of the row.
 *
 * Cloudberry gives every table a system column, gp_segment_id, whose value is
 * the content id of the process that read the row -- the segment that holds
 * it, or -1 on the coordinator.  An extension cannot add a system column.
 * What it can do, with O10, is give the name a meaning where nothing else
 * does: when a query names gp_segment_id, or t.gp_segment_id, and no column
 * of that name exists, columnref_fallback_hook makes it a call of
 *
 *     gp_internal.segment_of(t.*)
 *
 * and deparse_function_as_column_hook prints that call back as
 * gp_segment_id, so views, rules and EXPLAIN read as Cloudberry's do.
 *
 * What the call answers:
 *
 *   on a segment			its own content id, for any row of any relation:
 *							the row is one it holds, as in Cloudberry
 *   on the coordinator:
 *     hash-distributed		the segment the row's key hashes to, which is the
 *							one that holds it
 *     replicated			the segment the gather read it from
 *     random				an error: only the segment knows
 *     anything else		-1, a catalog or a table whose rows are here
 *
 * A condition on gp_segment_id is sent to the segments with the scan
 * (gp_scan.c), where it is answered by the first rule; so is an UPDATE or
 * DELETE, which the segments run as it is written.  The coordinator's rules
 * cover what stays here: the target list, and conditions over several
 * tables.
 *
 * gp.dist_random(NULL::t) is the one relation whose rows here come from
 * every segment, each copy with its segment: a reference to gp_segment_id
 * of it turns the call into gp_internal.dist_random_segments(NULL::t), which
 * returns the rows with their segment as one more column, left out of "*".
 * Either is printed back as gp_dist_random('t'), the call it was made of and
 * Cloudberry's, through deparse_range_function_hook (O31).
 *
 * A query that reads such a call alone and calls a function that is not
 * immutable -- gp_dist_random('gp_id'), which is how Cloudberry runs a query
 * once on each segment -- runs on every segment, as Cloudberry runs it: the
 * planner, PostgreSQL's or ORCA, is given the query as a call of
 * gp_internal.segment_query() with the query's text, printed as ruleutils
 * prints it, and each segment reads its own rows of the relation; what the
 * query does with the rows -- ORDER BY, DISTINCT, LIMIT -- is done here, as
 * Cloudberry does it above its Gather Motion.  So is what Cloudberry
 * evaluates on the coordinator for the segments: a parameter -- a PL/pgSQL
 * variable, say -- and a subquery of the query's own, which the segments'
 * text names as $1, $2, ..., and gp_internal.segment_query() is given as
 * its further arguments, so that the planner makes them initplans, as
 * Cloudberry's does, and the call sends their values.  A query that uses a
 * sequence stays on the coordinator, where the port's sequences are; so does
 * one that aggregates, and one with a column of an outer query or a
 * subquery that reads the row, which only the coordinator could answer.
 *
 * A function that runs on all segments, EXECUTE ON ALL SEGMENTS, is asked of
 * them the same way.  Called in FROM, its call becomes one of
 * gp_internal.segment_query() with the text of a query of its rows alone,
 * which each segment answers with its own, as Cloudberry's Function Scan in a
 * slice of every segment does; called in the target list of a query of no
 * relation, the query is one each segment answers once, as a query of
 * gp_dist_random('gp_id') is; and called in the target list of a query that
 * reads a relation, it is refused, as Cloudberry refuses it.  The port's own
 * such functions, which send their call to the segments themselves, answer
 * the same either way.
 *
 * pg_catalog.pg_locks has Cloudberry's three columns the same way, where
 * PostgreSQL 19's view has none of them: gp_segment_id, segment_of() of its
 * row, which is this node's content id, the node whose locks it lists; and
 * mppsessionid and mppiswriter, lock_session() and lock_writer() of its row
 * -- the coordinator session the locking process works for, -1 for none,
 * and whether it is a query's writer rather than a segment's reader, false
 * for a process no query started (gp_gdd.c keeps both).  "*" gives
 * PostgreSQL's sixteen columns, and a node's pg_locks lists its own locks:
 * the coordinator's does not gather the segments'.  And pg_stat_activity has
 * Cloudberry's sess_id the same way, activity_session() of its row, the
 * coordinator session the backend works for, as lock_session() gives it.
 * And pg_proc has Cloudberry's prodataaccess and proexeclocation the same
 * way, proc_data_access() and proc_exec_location() of its row: what the
 * function does with SQL and where it runs, which the "gp" label keeps
 * (gp_sql's funcattr.c).
 * A view its module labels a catalog ("gp" label key catalog) has
 * gp_segment_id too: it stands for a catalog table of Cloudberry's --
 * gp_ao's pg_appendonly and pg_attribute_encoding -- which has it, as every
 * table of Cloudberry's has, and its rows are the node's that reads them.
 *
 * A replicated table shows no system column on the coordinator, as
 * Cloudberry's shows none outside utility mode (scanRTEForColumn): each
 * segment's copy of a row has a ctid, an xmin and a segment of its own, and
 * which copy a session reads varies.  So gp_segment_id is not the name of
 * anything of it -- the name falls through to another relation, as in
 * Cloudberry -- and a system column PostgreSQL gave it is refused after
 * analysis, as the column that does not exist that it is in Cloudberry.
 *
 * And no column may be called gp_segment_id, of a relation that has system
 * columns -- a table, a partitioned or foreign table, a materialized view --
 * as none may be called ctid: CREATE TABLE, ALTER TABLE ... ADD COLUMN and
 * RENAME are refused in PostgreSQL's words for a system column's name
 * (CheckAttributeNamesTypes(), heap.c), which are Cloudberry's for this one.
 * A view and a composite type may have one, as they may have a ctid.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_label.h"
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_segment.h"
#include "gp_size.h"

#define GP_SEGMENT_ID	"gp_segment_id"
#define GP_MPPSESSIONID	"mppsessionid"
#define GP_MPPISWRITER	"mppiswriter"
#define GP_SESS_ID		"sess_id"
#define GP_PRODATAACCESS	"prodataaccess"
#define GP_PROEXECLOCATION	"proexeclocation"

static columnref_fallback_hook_type prev_columnref_fallback_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static deparse_function_as_column_hook_type prev_deparse_function_as_column_hook = NULL;
static deparse_range_function_hook_type prev_deparse_range_function_hook = NULL;
static object_access_hook_type prev_object_access_hook = NULL;

/* ------------------------------------------------------------------------- */
/* The functions' OIDs                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Looked up on first use and forgotten whenever pg_proc changes, so that a
 * database without the extension -- or one where it was dropped and made
 * again -- is answered correctly.  InvalidOid, once looked up, means there is
 * none.
 */
static bool func_oids_valid = false;
static Oid	segment_of_oid = InvalidOid;
static Oid	dist_random_oid = InvalidOid;
static Oid	dist_random_segments_oid = InvalidOid;
static Oid	pg_locks_oid = InvalidOid;
static Oid	lock_session_oid = InvalidOid;
static Oid	lock_writer_oid = InvalidOid;
static Oid	pg_stat_activity_oid = InvalidOid;
static Oid	activity_session_oid = InvalidOid;
static Oid	proc_data_access_oid = InvalidOid;
static Oid	proc_exec_location_oid = InvalidOid;

static char exec_location(Oid funcid);
static Oid	segment_query_oid = InvalidOid;
static Oid	segment_query_values_oid = InvalidOid;
static Oid	record_wire_oid = InvalidOid;

static void
invalidate_func_oids(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	func_oids_valid = false;
}

/*
 * By the catalog, not by LookupFuncName(): that checks the caller's USAGE on
 * the schema, and gp_core asks for these on behalf of whoever is planning --
 * ORCA's metadata asks for segment_of() for every relation it reads.
 */
static Oid
lookup_func_args(const char *schema, const char *name, const Oid *argtypes,
				 int nargs)
{
	Oid			nsp = get_namespace_oid(schema, true);

	if (!OidIsValid(nsp))
		return InvalidOid;
	return GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						   CStringGetDatum(name),
						   PointerGetDatum(buildoidvector(argtypes, nargs)),
						   ObjectIdGetDatum(nsp));
}

static Oid
lookup_func(const char *schema, const char *name, Oid argtype)
{
	return lookup_func_args(schema, name, &argtype, 1);
}

static void
lookup_func_oids(void)
{
	if (func_oids_valid)
		return;
	segment_of_oid = lookup_func("gp_internal", "segment_of", RECORDOID);
	dist_random_oid = lookup_func("gp", "dist_random", ANYELEMENTOID);
	dist_random_segments_oid = lookup_func("gp_internal",
										   "dist_random_segments",
										   ANYELEMENTOID);
	segment_query_oid = lookup_func("gp_internal", "segment_query", TEXTOID);
	segment_query_values_oid = lookup_func_args("gp_internal", "segment_query",
												(Oid[]) {TEXTOID, ANYOID}, 2);
	record_wire_oid = lookup_func("gp_internal", "record_wire", RECORDOID);
	pg_locks_oid = get_relname_relid("pg_locks", PG_CATALOG_NAMESPACE);
	lock_session_oid = lock_writer_oid = InvalidOid;
	if (OidIsValid(pg_locks_oid))
	{
		Oid			rowtype = get_rel_type_id(pg_locks_oid);

		lock_session_oid = lookup_func("gp_internal", "lock_session", rowtype);
		lock_writer_oid = lookup_func("gp_internal", "lock_writer", rowtype);
	}
	pg_stat_activity_oid = get_relname_relid("pg_stat_activity",
											 PG_CATALOG_NAMESPACE);
	activity_session_oid = InvalidOid;
	if (OidIsValid(pg_stat_activity_oid))
		activity_session_oid = lookup_func("gp_internal", "activity_session",
										   get_rel_type_id(pg_stat_activity_oid));
	proc_data_access_oid = lookup_func("gp_internal", "proc_data_access",
									   ProcedureRelation_Rowtype_Id);
	proc_exec_location_oid = lookup_func("gp_internal", "proc_exec_location",
										 ProcedureRelation_Rowtype_Id);
	func_oids_valid = true;
}

Oid
GpSegmentOfFunction(void)
{
	lookup_func_oids();
	return segment_of_oid;
}

/*
 * GpSegmentIsSegmentOf
 *		Is this expression gp_segment_id of range table entry varno's row?
 */
bool
GpSegmentIsSegmentOf(Node *node, Index varno)
{
	FuncExpr   *fexpr;
	Node	   *arg;
	Var		   *var;

	if (node == NULL || !IsA(node, FuncExpr))
		return false;
	fexpr = (FuncExpr *) node;
	if (list_length(fexpr->args) != 1)
		return false;

	lookup_func_oids();
	if (!OidIsValid(segment_of_oid) || fexpr->funcid != segment_of_oid)
		return false;

	/* A partition's row reaches it converted to its parent's row type. */
	arg = linitial(fexpr->args);
	while (IsA(arg, ConvertRowtypeExpr))
		arg = (Node *) ((ConvertRowtypeExpr *) arg)->arg;
	if (!IsA(arg, Var))
		return false;

	var = (Var *) arg;
	return var->varno == varno && var->varattno == InvalidAttrNumber &&
		var->varlevelsup == 0;
}

/* ------------------------------------------------------------------------- */
/* Parsing: the name, where no column has it                                 */
/* ------------------------------------------------------------------------- */

/* A replicated table, read on the coordinator: it shows no system column. */
static bool
hides_system_columns(Oid relid)
{
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return false;
	return GpPolicyIsReplicated(GpScanDistributedPolicy(relid));
}

typedef struct SystemColumnsContext
{
	ParseState *pstate;
	List	   *rtables;		/* each query level's, the innermost first */
} SystemColumnsContext;

static bool
system_columns_walker(Node *node, SystemColumnsContext *cxt)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		RangeTblEntry *rte;

		if (var->varattno >= 0 || var->varlevelsup >= list_length(cxt->rtables))
			return false;
		rte = rt_fetch(var->varno, (List *) list_nth(cxt->rtables, var->varlevelsup));
		if (rte->rtekind == RTE_RELATION && hides_system_columns(rte->relid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							NameStr(SystemAttributeDefinition(var->varattno)->attname)),
					 parser_errposition(cxt->pstate, var->location)));
		return false;
	}
	if (IsA(node, Query))
	{
		bool		result;

		cxt->rtables = lcons(((Query *) node)->rtable, cxt->rtables);
		result = query_tree_walker((Query *) node, system_columns_walker, cxt, 0);
		cxt->rtables = list_delete_first(cxt->rtables);
		return result;
	}
	return expression_tree_walker(node, system_columns_walker, cxt);
}

static void
gp_post_parse_analyze(ParseState *pstate, Query *query,
					  const JumbleState *jstate)
{
	SystemColumnsContext cxt = {.pstate = pstate,.rtables = NIL};

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);
	if (GpClusterBackendRole() == GP_ROLE_DISPATCH)
		(void) system_columns_walker((Node *) query, &cxt);
}

/*
 * Does this entry have gp_segment_id?  In Cloudberry every relation that has
 * system columns has it: tables, partitioned tables, materialized views,
 * foreign tables -- an external table's row the segment's that read it, or
 * -1, the coordinator's.  Views and subqueries do not, and nor do functions
 * -- but for gp.dist_random(), whose rows are the segments'.
 */
/* Is this entry pg_catalog.pg_locks, which has Cloudberry's three columns? */
static bool
nsitem_is_pg_locks(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	lookup_func_oids();
	return rte->rtekind == RTE_RELATION && OidIsValid(pg_locks_oid) &&
		rte->relid == pg_locks_oid;
}

/* Is this entry pg_catalog.pg_stat_activity, which has Cloudberry's sess_id? */
static bool
nsitem_is_pg_proc(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	return rte->rtekind == RTE_RELATION && rte->relid == ProcedureRelationId;
}

static bool
nsitem_is_pg_stat_activity(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;
	Node	   *f;

	lookup_func_oids();
	if (!OidIsValid(pg_stat_activity_oid))
		return false;
	if (rte->rtekind == RTE_RELATION)
		return rte->relid == pg_stat_activity_oid;

	/* gp_dist_random('pg_stat_activity'): every segment's rows of it */
	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality ||
		list_length(rte->functions) != 1 || !OidIsValid(dist_random_oid))
		return false;
	f = linitial_node(RangeTblFunction, rte->functions)->funcexpr;
	return IsA(f, FuncExpr) && ((FuncExpr *) f)->funcid == dist_random_oid &&
		list_length(((FuncExpr *) f)->args) == 1 &&
		get_typ_typrelid(exprType(linitial(((FuncExpr *) f)->args))) == pg_stat_activity_oid;
}

/*
 * Is this entry a view that stands for a catalog table of Cloudberry's --
 * pg_appendonly, pg_attribute_encoding -- which its module labelled so?
 * Cloudberry's has gp_segment_id, as every table of its has.
 */
static bool
nsitem_is_catalog_view(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;
	ObjectAddress view;

	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_VIEW)
		return false;
	ObjectAddressSet(view, RelationRelationId, rte->relid);
	return GpLabelHas(&view, GP_LABEL_catalog);
}

static bool
nsitem_has_segment_id(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	if (rte->rtekind == RTE_RELATION)
		return ((rte->relkind == RELKIND_RELATION ||
				 rte->relkind == RELKIND_PARTITIONED_TABLE ||
				 rte->relkind == RELKIND_MATVIEW ||
				 rte->relkind == RELKIND_FOREIGN_TABLE) &&
				!hides_system_columns(rte->relid)) ||
			nsitem_is_pg_locks(nsitem) || nsitem_is_catalog_view(nsitem);

	if (rte->rtekind == RTE_FUNCTION && list_length(rte->functions) == 1)
	{
		RangeTblFunction *rtfunc = linitial_node(RangeTblFunction,
												 rte->functions);

		lookup_func_oids();
		return IsA(rtfunc->funcexpr, FuncExpr) &&
			OidIsValid(dist_random_oid) &&
			((FuncExpr *) rtfunc->funcexpr)->funcid == dist_random_oid;
	}

	return false;
}

/*
 * Give gp.dist_random(NULL::t)'s entry the column gp_segment_id, by calling
 * dist_random_segments() in its place with t's columns and that one as its
 * column definition list.  The column comes last and "*" leaves it out, so
 * every Var already made for the entry keeps its meaning.
 *
 * The entry's column names and its namespace columns are made anew, not
 * changed where they are: a JOIN's ON clause may name gp_segment_id while
 * the parser is building the join's own columns from the entry's, the lists
 * and the array it read before the clause (transformFromClauseItem()), whose
 * length it sized its own by.
 */
static void
add_dist_random_segment_column(ParseState *pstate, ParseNamespaceItem *nsitem,
							   int location)
{
	RangeTblEntry *rte = nsitem->p_rte;
	RangeTblFunction *rtfunc = linitial_node(RangeTblFunction, rte->functions);
	FuncExpr   *fexpr = (FuncExpr *) rtfunc->funcexpr;
	Oid			relid = get_typ_typrelid(exprType(linitial(fexpr->args)));
	Relation	rel;
	TupleDesc	tupdesc;
	int			ncols;
	List	   *colnames;
	ParseNamespaceColumn *nscolumns;
	ParseNamespaceColumn *nscol;

	if (rte->funcordinality || !OidIsValid(dist_random_segments_oid) ||
		!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp_segment_id of gp.dist_random() is available only for a relation's rows without ORDINALITY"),
				 parser_errposition(pstate, location)));

	rel = table_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	rtfunc->funccolnames = NIL;
	rtfunc->funccoltypes = NIL;
	rtfunc->funccoltypmods = NIL;
	rtfunc->funccolcollations = NIL;
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/*
		 * A dropped column keeps its place, so that no Var already made
		 * moves: an integer, always null, under the name PostgreSQL gives a
		 * dropped column -- which ruleutils prints, and which neither "*" nor
		 * a reference reaches, the entry's own name for it staying empty.
		 */
		if (att->attisdropped)
		{
			rtfunc->funccolnames = lappend(rtfunc->funccolnames,
										   makeString(psprintf("........pg.dropped.%d........",
															   att->attnum)));
			rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes, INT4OID);
			rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods, -1);
			rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
													InvalidOid);
			continue;
		}

		rtfunc->funccolnames = lappend(rtfunc->funccolnames,
									   makeString(pstrdup(NameStr(att->attname))));
		rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes, att->atttypid);
		rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods,
											 att->atttypmod);
		rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
												att->attcollation);
	}
	ncols = tupdesc->natts + 1;
	table_close(rel, NoLock);

	if (rtfunc->funccolcount != ncols - 1 ||
		list_length(rte->eref->colnames) != ncols - 1)
		elog(ERROR, "gp.dist_random() entry does not match its relation");

	colnames = list_copy(rte->eref->colnames);
	nscolumns = palloc_array(ParseNamespaceColumn, ncols);
	memcpy(nscolumns, nsitem->p_nscolumns,
		   (ncols - 1) * sizeof(ParseNamespaceColumn));

	/*
	 * A column definition list's names are the entry's too, which ruleutils
	 * prints from: a dropped column's is the placeholder's, its namespace
	 * column the placeholder, which "*" does not expand.
	 */
	foreach_node(String, name, colnames)
	{
		int			i = foreach_current_index(name);

		if (strVal(name)[0] != '\0')
			continue;
		lfirst(list_nth_cell(colnames, i)) =
			makeString(pstrdup(strVal(list_nth(rtfunc->funccolnames, i))));
		nscol = &nscolumns[i];
		memset(nscol, 0, sizeof(*nscol));
		nscol->p_varno = nsitem->p_rtindex;
		nscol->p_varattno = i + 1;
		nscol->p_vartype = INT4OID;
		nscol->p_vartypmod = -1;
		nscol->p_varcollid = InvalidOid;
		nscol->p_varreturningtype = nsitem->p_returning_type;
		nscol->p_varnosyn = nsitem->p_rtindex;
		nscol->p_varattnosyn = i + 1;
		nscol->p_dontexpand = true;
	}

	rtfunc->funccolnames = lappend(rtfunc->funccolnames,
								   makeString(pstrdup(GP_SEGMENT_ID)));
	rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes, INT4OID);
	rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods, -1);
	rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
											InvalidOid);
	rtfunc->funccolcount = ncols;

	fexpr = copyObject(fexpr);
	fexpr->funcid = dist_random_segments_oid;
	fexpr->funcresulttype = RECORDOID;
	rtfunc->funcexpr = (Node *) fexpr;

	rte->eref->colnames = lappend(colnames, makeString(pstrdup(GP_SEGMENT_ID)));

	nsitem->p_nscolumns = nscolumns;
	nscol = &nscolumns[ncols - 1];
	memset(nscol, 0, sizeof(*nscol));
	nscol->p_varno = nsitem->p_rtindex;
	nscol->p_varattno = ncols;
	nscol->p_vartype = INT4OID;
	nscol->p_vartypmod = -1;
	nscol->p_varcollid = InvalidOid;
	nscol->p_varreturningtype = nsitem->p_returning_type;
	nscol->p_varnosyn = nsitem->p_rtindex;
	nscol->p_varattnosyn = ncols;
	nscol->p_dontexpand = true;
}

/* A call of this function of this entry's row. */
static Node *
make_row_call(ParseState *pstate, ParseNamespaceItem *nsitem,
			  int sublevels_up, int location, Oid funcid, Oid rettype)
{
	Var		   *var;
	FuncExpr   *fexpr;

	/* The row, as transformWholeRowRef() makes it for "t.*" */
	var = makeWholeRowVar(nsitem->p_rte, nsitem->p_rtindex, sublevels_up, true);
	var->location = location;
	markNullableIfNeeded(pstate, var);
	markVarForSelectPriv(pstate, var);

	fexpr = makeFuncExpr(funcid, rettype, list_make1(var),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	fexpr->location = location;
	return (Node *) fexpr;
}

/* gp_segment_id of this entry's row. */
static Node *
make_segment_id(ParseState *pstate, ParseNamespaceItem *nsitem,
				int sublevels_up, int location)
{
	if (nsitem->p_rte->rtekind == RTE_FUNCTION)
	{
		add_dist_random_segment_column(pstate, nsitem, location);
		return scanNSItemForColumn(pstate, nsitem, sublevels_up,
								   GP_SEGMENT_ID, location);
	}
	return make_row_call(pstate, nsitem, sublevels_up, location,
						 segment_of_oid, INT4OID);
}

/*
 * The one entry an unqualified name means: the nearest query level with a
 * relation that has it, where it is an error for there to be two, as it is
 * for any column two relations have.
 */
static ParseNamespaceItem *
find_unqualified(ParseState *pstate, const char *name,
				 bool (*has) (ParseNamespaceItem *),
				 int location, int *sublevels_up)
{
	int			levels_up = 0;

	for (ParseState *ps = pstate; ps != NULL; ps = ps->parentParseState)
	{
		ParseNamespaceItem *found = NULL;

		foreach_ptr(ParseNamespaceItem, nsitem, ps->p_namespace)
		{
			if (!nsitem->p_cols_visible)
				continue;
			if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
				continue;
			if (!has(nsitem))
				continue;
			if (found != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous", name),
						 parser_errposition(pstate, location)));
			found = nsitem;
		}
		if (found != NULL)
		{
			*sublevels_up = levels_up;
			return found;
		}
		levels_up++;
	}
	return NULL;
}

static Node *
gp_columnref_fallback(ParseState *pstate, ColumnRef *cref)
{
	int			nfields = list_length(cref->fields);
	Node	   *last = (Node *) llast(cref->fields);
	ParseNamespaceItem *nsitem = NULL;
	int			sublevels_up = 0;
	const char *name;
	bool		(*has) (ParseNamespaceItem *);
	Oid			funcid = InvalidOid;
	Oid			rettype = InvalidOid;

	if (prev_columnref_fallback_hook)
	{
		Node	   *node = prev_columnref_fallback_hook(pstate, cref);

		if (node != NULL)
			return node;
	}

	if (nfields > 3 || !IsA(last, String))
		return NULL;
	name = strVal(last);

	/* Without the extension in this database the names mean nothing. */
	lookup_func_oids();
	if (strcmp(name, GP_SEGMENT_ID) == 0 && OidIsValid(segment_of_oid))
		has = nsitem_has_segment_id;
	else if (strcmp(name, GP_MPPSESSIONID) == 0 && OidIsValid(lock_session_oid))
	{
		has = nsitem_is_pg_locks;
		funcid = lock_session_oid;
		rettype = INT4OID;
	}
	else if (strcmp(name, GP_MPPISWRITER) == 0 && OidIsValid(lock_writer_oid))
	{
		has = nsitem_is_pg_locks;
		funcid = lock_writer_oid;
		rettype = BOOLOID;
	}
	else if (strcmp(name, GP_SESS_ID) == 0 && OidIsValid(activity_session_oid))
	{
		has = nsitem_is_pg_stat_activity;
		funcid = activity_session_oid;
		rettype = INT4OID;
	}
	else if (strcmp(name, GP_PRODATAACCESS) == 0 && OidIsValid(proc_data_access_oid))
	{
		has = nsitem_is_pg_proc;
		funcid = proc_data_access_oid;
		rettype = CHAROID;
	}
	else if (strcmp(name, GP_PROEXECLOCATION) == 0 && OidIsValid(proc_exec_location_oid))
	{
		has = nsitem_is_pg_proc;
		funcid = proc_exec_location_oid;
		rettype = CHAROID;
	}
	else
		return NULL;

	if (nfields == 1)
		nsitem = find_unqualified(pstate, name, has, cref->location,
								  &sublevels_up);
	else
	{
		char	   *nspname = NULL;
		char	   *relname;

		if (nfields == 3)
			nspname = strVal(linitial(cref->fields));
		relname = strVal(list_nth(cref->fields, nfields - 2));
		nsitem = refnameNamespaceItem(pstate, nspname, relname,
									  cref->location, &sublevels_up);
		if (nsitem != NULL && !has(nsitem))
			nsitem = NULL;
	}

	if (nsitem == NULL)
		return NULL;
	if (has == nsitem_has_segment_id)
		return make_segment_id(pstate, nsitem, sublevels_up, cref->location);
	return make_row_call(pstate, nsitem, sublevels_up, cref->location,
						 funcid, rettype);
}

/* ------------------------------------------------------------------------- */
/* Deparsing: the call, printed as the column                                */
/* ------------------------------------------------------------------------- */

static const char *
gp_deparse_function_as_column(FuncExpr *expr)
{
	lookup_func_oids();
	if (OidIsValid(segment_of_oid) && expr->funcid == segment_of_oid)
		return GP_SEGMENT_ID;
	if (OidIsValid(lock_session_oid) && expr->funcid == lock_session_oid)
		return GP_MPPSESSIONID;
	if (OidIsValid(lock_writer_oid) && expr->funcid == lock_writer_oid)
		return GP_MPPISWRITER;
	if (OidIsValid(activity_session_oid) && expr->funcid == activity_session_oid)
		return GP_SESS_ID;
	if (OidIsValid(proc_data_access_oid) && expr->funcid == proc_data_access_oid)
		return GP_PRODATAACCESS;
	if (OidIsValid(proc_exec_location_oid) && expr->funcid == proc_exec_location_oid)
		return GP_PROEXECLOCATION;
	if (prev_deparse_function_as_column_hook)
		return prev_deparse_function_as_column_hook(expr);
	return NULL;
}

/*
 * gp.dist_random(NULL::t), or dist_random_segments() in its place, printed as
 * the gp_dist_random('t') O26 makes it of (gp_sql's gp_desugar.c), which
 * Cloudberry prints: the relation's name qualified where the search path
 * does not find it, and no column definition list, which the parser makes
 * again.  Its alias is printed where it is not the relation's name, which
 * O26 gives it where none was written.  (O31)
 */
static const char *
gp_deparse_range_function(RangeTblEntry *rte, const char *refname,
						  bool *print_alias)
{
	RangeTblFunction *rtfunc = linitial_node(RangeTblFunction, rte->functions);
	FuncExpr   *fexpr = (FuncExpr *) rtfunc->funcexpr;
	Oid			relid = InvalidOid;
	char	   *relname = NULL;

	lookup_func_oids();
	if (IsA(fexpr, FuncExpr) && list_length(fexpr->args) == 1 &&
		((OidIsValid(dist_random_oid) && fexpr->funcid == dist_random_oid) ||
		 (OidIsValid(dist_random_segments_oid) &&
		  fexpr->funcid == dist_random_segments_oid)) &&
		IsA(linitial(fexpr->args), Const) &&
		((Const *) linitial(fexpr->args))->constisnull)
	{
		relid = get_typ_typrelid(((Const *) linitial(fexpr->args))->consttype);
		relname = OidIsValid(relid) ? get_rel_name(relid) : NULL;
	}

	if (relname != NULL)
	{
		const char *name = RelationIsVisible(relid) ? quote_identifier(relname)
			: quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
										 relname);

		*print_alias = strcmp(refname, relname) != 0;
		return psprintf("gp_dist_random(%s)", quote_literal_cstr(name));
	}
	if (prev_deparse_range_function_hook)
		return prev_deparse_range_function_hook(rte, refname, print_alias);
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* The value                                                                 */
/* ------------------------------------------------------------------------- */

/* What segment_of() keeps between calls in one expression. */
typedef struct SegmentOfCache
{
	Oid			typid;			/* the row type it was set up for */
	bool		hashed;			/* hash-distributed: compute from the key */
	int			answer;			/* otherwise the answer, whatever the row */
	TupleDesc	tupdesc;
	GpHash	   *hash;
	Datum	   *values;
	bool	   *isnull;
} SegmentOfCache;

static SegmentOfCache *
segment_of_setup(FunctionCallInfo fcinfo, Oid typid)
{
	SegmentOfCache *cache = (SegmentOfCache *) fcinfo->flinfo->fn_extra;
	MemoryContext oldcxt;
	Oid			relid;
	GpPolicy   *policy;

	if (cache != NULL && cache->typid == typid)
		return cache;

	oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
	cache = palloc0_object(SegmentOfCache);
	cache->typid = typid;
	cache->answer = -1;

	relid = get_typ_typrelid(typid);
	policy = OidIsValid(relid) ? GpScanDistributedPolicy(relid) : NULL;

	if (policy == NULL)
		cache->answer = -1;
	else if (GpPolicyIsReplicated(policy))
		cache->answer = GpScanReplicatedContent(policy);
	else if (GpPolicyIsRandomPartitioned(policy))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp_segment_id of randomly distributed table \"%s\" is known only on its segments",
						get_rel_name(relid)),
				 errdetail("The query computes it above the gather that read the table, and nothing in a row says which segment held it."),
				 errhint("Name it in a query of that table alone, whose gather gives it, or in a condition on that table, which the segments evaluate.")));
	else
	{
		TupleDesc	tupdesc = lookup_rowtype_tupdesc(typid, -1);

		cache->tupdesc = CreateTupleDescCopy(tupdesc);
		ReleaseTupleDesc(tupdesc);
		cache->hash = GpHashMake(policy, cache->tupdesc);
		cache->values = palloc_array(Datum, cache->tupdesc->natts);
		cache->isnull = palloc_array(bool, cache->tupdesc->natts);
		cache->hashed = true;
	}

	MemoryContextSwitchTo(oldcxt);
	fcinfo->flinfo->fn_extra = cache;
	return cache;
}

PG_FUNCTION_INFO_V1(gp_segment_of);

/*
 * gp_internal.segment_of(t.*)
 *		gp_segment_id of a row of t; see the head of this file.
 */
Datum
gp_segment_of(PG_FUNCTION_ARGS)
{
	HeapTupleHeader row;
	HeapTupleData tuple;
	SegmentOfCache *cache;

	/* A segment answers for the rows it holds, which are all it reads. */
	if (GpClusterContentId() >= 0)
		PG_RETURN_INT32(GpClusterContentId());

	row = PG_GETARG_HEAPTUPLEHEADER(0);
	cache = segment_of_setup(fcinfo, HeapTupleHeaderGetTypeId(row));
	if (!cache->hashed)
		PG_RETURN_INT32(cache->answer);

	tuple.t_len = HeapTupleHeaderGetDatumLength(row);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = row;
	heap_deform_tuple(&tuple, cache->tupdesc, cache->values, cache->isnull);

	PG_RETURN_INT32(GpHashSegment(cache->hash, cache->values, cache->isnull));
}

/* The process id of a pg_locks or pg_stat_activity row, 0 where it has none. */
static int
lock_row_pid(HeapTupleHeader row)
{
	bool		isnull;
	Datum		pid = GetAttributeByName(row, "pid", &isnull);

	return isnull ? 0 : DatumGetInt32(pid);
}

PG_FUNCTION_INFO_V1(gp_lock_session);

/*
 * gp_internal.lock_session(pg_locks): mppsessionid, the coordinator session
 * the process holding or awaiting the lock works for, as Cloudberry's
 * pg_locks gives it; -1 for one that works for none.
 */
Datum
gp_lock_session(PG_FUNCTION_ARGS)
{
	int			session;
	bool		reader;

	if (!GpGddBackendIdentity(lock_row_pid(PG_GETARG_HEAPTUPLEHEADER(0)),
							  &session, &reader))
		PG_RETURN_INT32(-1);
	PG_RETURN_INT32(session);
}

PG_FUNCTION_INFO_V1(gp_lock_writer);

/*
 * gp_internal.lock_writer(pg_locks): mppiswriter.  True for a coordinator's
 * backend and a segment's writer, false for a segment's reader, and false
 * for a process no query started -- a session of a segment's own, a process
 * of the server's -- as Cloudberry's InitProcess sets it.
 */
Datum
gp_lock_writer(PG_FUNCTION_ARGS)
{
	int			session;
	bool		reader;

	if (!GpGddBackendIdentity(lock_row_pid(PG_GETARG_HEAPTUPLEHEADER(0)),
							  &session, &reader))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(!reader);
}

PG_FUNCTION_INFO_V1(gp_activity_session);

/*
 * gp_internal.activity_session(pg_stat_activity): sess_id, the coordinator
 * session the backend works for, as Cloudberry's pg_stat_activity gives it;
 * -1 for one that works for none.
 */
Datum
gp_activity_session(PG_FUNCTION_ARGS)
{
	int			pid = lock_row_pid(PG_GETARG_HEAPTUPLEHEADER(0));
	int			session;
	bool		reader;

	if (!GpGddBackendIdentity(pid, &session, &reader))
	{
		/*
		 * One node's backends tell no deadlock detector their session, there
		 * being none to tell; a client's session is its own, its pid, as
		 * GpClusterSessionId() says -- as in Cloudberry's single-node mode,
		 * where every client backend has a gp_session_id.
		 */
		PGPROC	   *proc = pid != 0 && GpClusterIsSingleNode() ?
			BackendPidGetProc(pid) : NULL;

		if (proc != NULL && proc->backendType == B_BACKEND)
			PG_RETURN_INT32(pid);
		PG_RETURN_INT32(-1);
	}
	PG_RETURN_INT32(session);
}

PG_FUNCTION_INFO_V1(gp_proc_data_access);
PG_FUNCTION_INFO_V1(gp_proc_exec_location);

/* The function a pg_proc row is. */
static Oid
proc_row_oid(HeapTupleHeader row)
{
	bool		isnull;
	Datum		oid = GetAttributeByNum(row, Anum_pg_proc_oid, &isnull);

	return isnull ? InvalidOid : DatumGetObjectId(oid);
}

/*
 * gp_internal.proc_data_access(pg_proc): prodataaccess, what the function
 * does with SQL, as Cloudberry's pg_proc says it -- 'n'o SQL, 'c'ontains
 * SQL, 'r'eads SQL data or 'm'odifies it -- by the "gp" label's data_access
 * key, and where it has none by Cloudberry's default: CONTAINS SQL for a
 * SQL function, NO SQL for any other.
 */
Datum
gp_proc_data_access(PG_FUNCTION_ARGS)
{
	HeapTupleHeader row = PG_GETARG_HEAPTUPLEHEADER(0);
	ObjectAddress addr;
	char	   *value;
	bool		isnull;
	Datum		lang;

	ObjectAddressSet(addr, ProcedureRelationId, proc_row_oid(row));
	value = GpLabelGet(&addr, GP_LABEL_data_access);
	if (value == NULL)
	{
		lang = GetAttributeByNum(row, Anum_pg_proc_prolang, &isnull);
		PG_RETURN_CHAR(!isnull && DatumGetObjectId(lang) == SQLlanguageId ? 'c' : 'n');
	}
	if (strcmp(value, "none") == 0)
		PG_RETURN_CHAR('n');
	if (strcmp(value, "contains") == 0)
		PG_RETURN_CHAR('c');
	if (strcmp(value, "reads") == 0)
		PG_RETURN_CHAR('r');
	if (strcmp(value, "modifies") == 0)
		PG_RETURN_CHAR('m');
	PG_RETURN_NULL();
}

/*
 * gp_internal.proc_exec_location(pg_proc): proexeclocation, where the
 * function runs, as Cloudberry's pg_proc says it -- 'a'ny node, the
 * 'c'oordinator, an 'i'nitplan, all 's'egments -- by the label's execute_on
 * key.
 */
Datum
gp_proc_exec_location(PG_FUNCTION_ARGS)
{
	PG_RETURN_CHAR(exec_location(proc_row_oid(PG_GETARG_HEAPTUPLEHEADER(0))));
}

PG_FUNCTION_INFO_V1(gp_dist_random_segments);

/*
 * gp_internal.dist_random_segments(NULL::t)
 *		gp.dist_random(NULL::t), each row with the segment it came from.
 *
 * Called only in gp.dist_random()'s place, by the parser, which gives it t's
 * columns and gp_segment_id as its column definition list.
 */
Datum
gp_dist_random_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = get_typ_typrelid(get_fn_expr_argtype(fcinfo->flinfo, 0));
	Relation	rel;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	GpGatherState *gather;
	StringInfoData sql;
	int			natts;
	int			content;

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine which relation to read")));

	rel = table_open(relid, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	natts = tupdesc->natts;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (rsinfo->setDesc->natts != natts + 1)
		elog(ERROR, "gp_internal.dist_random_segments() called with %d columns for a relation of %d",
			 rsinfo->setDesc->natts, natts);

	/* One node, or a utility session: this node's rows, and its id */
	if (GpDistRandomIsLocal())
	{
		GpDistRandomLocal(relid, rsinfo->setResult, rsinfo->setDesc, true);
		table_close(rel, AccessShareLock);
		return (Datum) 0;
	}

	initStringInfo(&sql);
	appendStringInfo(&sql, "%s FROM %s", GpTransferSelectList(tupdesc),
					 GpDispatchRelationName(RelationGetRelid(rel)));

	slot = MakeSingleTupleTableSlot(GpTransferDesc(tupdesc), &TTSOpsVirtual);
	gather = GpGatherStart(sql.data, slot->tts_tupleDescriptor);
	while (GpGatherNext(gather, slot, &content))
		GpTransferPut(tupdesc, slot, rsinfo->setResult, rsinfo->setDesc, content);
	GpGatherEnd(gather);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* No column of that name                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Refuse a column called gp_segment_id of this relation, as a system
 * column's name is refused: read with SnapshotSelf, since the statement that
 * made or renamed it has not made its rows visible yet -- sepgsql reads a new
 * relation's columns so (relation.c).  A view, a composite type and an index
 * have no system columns, and may.
 */
static void
check_segment_id_column(Oid relid)
{
	Relation	rel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;
	char		relkind = '\0';

	ScanKeyInit(&key[0], Anum_pg_attribute_attrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1], Anum_pg_attribute_attname, BTEqualStrategyNumber,
				F_NAMEEQ, CStringGetDatum(GP_SEGMENT_ID));
	rel = table_open(AttributeRelationId, AccessShareLock);
	scan = systable_beginscan(rel, AttributeRelidNameIndexId, true,
							  SnapshotSelf, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tuple);

		if (att->attnum > 0 && !att->attisdropped)
			found = true;
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (!found)
		return;

	/* what kind of relation, from pg_class as the statement has it */
	ScanKeyInit(&key[0], Anum_pg_class_oid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(rel, ClassOidIndexId, true, SnapshotSelf, 1, key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		relkind = ((Form_pg_class) GETSTRUCT(tuple))->relkind;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (relkind == RELKIND_VIEW || relkind == RELKIND_COMPOSITE_TYPE ||
		relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
		return;
	ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_COLUMN),
			 errmsg("column name \"%s\" conflicts with a system column name",
					GP_SEGMENT_ID)));
}

static void
gp_segment_object_access(ObjectAccessType access, Oid classId, Oid objectId,
						 int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/*
	 * A relation made, a column added (subId its number), or a column
	 * altered -- renamed among it -- and not by gp_core's own script, whose
	 * catalogs name no such column anyway.
	 */
	if (classId == RelationRelationId &&
		(access == OAT_POST_CREATE || (access == OAT_POST_ALTER && subId > 0)))
		check_segment_id_column(objectId);
}

/* ------------------------------------------------------------------------- */
/* A query of gp_dist_random() alone, run on every segment                   */
/* ------------------------------------------------------------------------- */

/*
 * A value the coordinator evaluates for the segments: a parameter, or a
 * subquery of a value's kind -- EXPR or ARRAY, whose test reads nothing of
 * the row -- that reads nothing of the query it is in, nor of one outside.
 */
static bool
carried_value(Node *node)
{
	if (IsA(node, Param))
		return ((Param *) node)->paramkind == PARAM_EXTERN;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Oid			type = exprType(node);

		return (sublink->subLinkType == EXPR_SUBLINK ||
				sublink->subLinkType == ARRAY_SUBLINK) &&
			!contain_vars_of_level(sublink->subselect, 1) &&
			type != RECORDOID && get_typtype(type) != TYPTYPE_PSEUDO;
	}
	return false;
}

/*
 * An x IN (SELECT ...) that reads nothing of the query it is in -- an ANY
 * subquery of one column, whose test is one operator between x and the
 * subquery's value -- as x op ANY (ARRAY(SELECT ...)): the same answer,
 * null where one is, with the subquery a value of ARRAY's kind, which the
 * coordinator evaluates for the segments.  NULL where it is not that.
 */
static ScalarArrayOpExpr *
any_as_array(SubLink *sublink)
{
	OpExpr	   *op;
	Param	   *value;
	SubLink    *array;
	ScalarArrayOpExpr *saop;
	int			ncols = 0;

	if (sublink->subLinkType != ANY_SUBLINK || sublink->testexpr == NULL ||
		!IsA(sublink->testexpr, OpExpr) ||
		contain_vars_of_level(sublink->subselect, 1))
		return NULL;
	op = (OpExpr *) sublink->testexpr;
	if (list_length(op->args) != 2 || !IsA(lsecond(op->args), Param))
		return NULL;
	value = (Param *) lsecond(op->args);
	if (value->paramkind != PARAM_SUBLINK || value->paramid != 1 ||
		value->paramtype == RECORDOID ||
		!OidIsValid(get_array_type(value->paramtype)))
		return NULL;
	foreach_node(TargetEntry, tle, ((Query *) sublink->subselect)->targetList)
		if (!tle->resjunk)
			ncols++;
	if (ncols != 1)
		return NULL;

	array = makeNode(SubLink);
	array->subLinkType = ARRAY_SUBLINK;
	array->subselect = sublink->subselect;
	array->location = sublink->location;
	saop = makeNode(ScalarArrayOpExpr);
	saop->opno = op->opno;
	saop->opfuncid = op->opfuncid;
	saop->useOr = true;
	saop->inputcollid = op->inputcollid;
	saop->args = list_make2(linitial(op->args), array);
	saop->location = op->location;
	return saop;
}

/*
 * Is there something in the expression that only the coordinator can
 * answer: a sequence, which the port keeps there; a column of an outer
 * query, or a subquery that is not a value to carry (above) nor an IN of
 * one; or an aggregate, which a query of this shape does not have anyway?
 */
static bool
coordinator_only_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup > 0;
	if (IsA(node, SubLink) && any_as_array((SubLink *) node) != NULL)
		return coordinator_only_walker(linitial(((OpExpr *) ((SubLink *) node)->testexpr)->args),
									   context);
	if (IsA(node, Param) || IsA(node, SubLink))
		return !carried_value(node);
	if (IsA(node, Aggref) || IsA(node, GroupingFunc) || IsA(node, WindowFunc) ||
		IsA(node, NextValueExpr))
		return true;
	if (IsA(node, FuncExpr))
	{
		switch (((FuncExpr *) node)->funcid)
		{
			case F_NEXTVAL:
			case F_CURRVAL:
			case F_SETVAL_REGCLASS_INT8:
			case F_SETVAL_REGCLASS_INT8_BOOL:
			case F_LASTVAL:
				return true;
			default:
				break;
		}
	}
	return expression_tree_walker(node, coordinator_only_walker, context);
}

/*
 * Can a segment send every column of the target list?  void, what a function
 * called for what it does answers, travels too, and so does a record of no
 * declared type, described (gp_internal.record_wire).
 */
static bool
columns_travel(List *tlist)
{
	foreach_node(TargetEntry, tle, tlist)
	{
		Oid			type = exprType((Node *) tle->expr);

		if (type == RECORDOID && OidIsValid(record_wire_oid))
			continue;
		if ((get_typtype(type) == TYPTYPE_PSEUDO && type != VOIDOID) ||
			GpTransferType(type) != type)
			return false;
	}
	return true;
}

/*
 * Is q a SELECT whose rows each segment can make apart, the coordinator only
 * gathering, sorting, making distinct and limiting them: one with no set
 * operation, CTE, aggregate, window or grouping, and nothing locked?
 */
static bool
select_alone(Query *q)
{
	return q->commandType == CMD_SELECT && q->utilityStmt == NULL &&
		q->setOperations == NULL && q->cteList == NIL && !q->hasRecursive &&
		!q->hasModifyingCTE && !q->hasAggs && !q->hasWindowFuncs &&
		!q->hasForUpdate && q->rowMarks == NIL &&
		q->groupClause == NIL && q->groupingSets == NIL &&
		q->havingQual == NULL && q->windowClause == NIL &&
		q->limitOption != LIMIT_OPTION_WITH_TIES;
}

/*
 * Is q a SELECT of one gp.dist_random() call, with no aggregate, whose
 * target list or condition calls a function that is not immutable, and
 * whose every column a segment can send?
 */
static bool
dist_random_pushable(Query *q)
{
	RangeTblEntry *rte;
	RangeTblFunction *rtfunc;
	Oid			funcid;

	if (!select_alone(q))
		return false;
	if (list_length(q->rtable) != 1 || q->jointree == NULL ||
		list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return false;
	rte = linitial_node(RangeTblEntry, q->rtable);
	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality ||
		rte->lateral || list_length(rte->functions) != 1)
		return false;
	rtfunc = linitial_node(RangeTblFunction, rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return false;
	funcid = ((FuncExpr *) rtfunc->funcexpr)->funcid;
	if (funcid != dist_random_oid && funcid != dist_random_segments_oid)
		return false;

	if (!contain_mutable_functions((Node *) q->targetList) &&
		!contain_mutable_functions(q->jointree->quals))
		return false;
	if (coordinator_only_walker((Node *) q->targetList, NULL) ||
		coordinator_only_walker(q->jointree->quals, NULL))
		return false;
	return columns_travel(q->targetList);
}

/*
 * The values the coordinator evaluates for the segments, taken out of the
 * query the segments are sent, each made the parameter $n of it, in order.
 */
typedef struct CarryContext
{
	List	   *values;			/* of Node *, the expressions */
} CarryContext;

static Node *
carry_mutator(Node *node, CarryContext *context)
{
	ScalarArrayOpExpr *saop;

	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink) && (saop = any_as_array((SubLink *) node)) != NULL)
		return carry_mutator((Node *) saop, context);
	if ((IsA(node, Param) || IsA(node, SubLink)) && carried_value(node))
	{
		Param	   *param = makeNode(Param);

		context->values = lappend(context->values, copyObject(node));
		param->paramkind = PARAM_EXTERN;
		param->paramid = list_length(context->values);
		param->paramtype = exprType(node);
		param->paramtypmod = exprTypmod(node);
		param->paramcollid = exprCollation(node);
		param->location = -1;
		return (Node *) param;
	}
	return expression_tree_mutator(node, carry_mutator, context);
}

/*
 * Make q, which dist_random_pushable() took, a query of the rows its text
 * gives on every segment: gp_internal.segment_query(text, ...) with a column
 * definition list of q's target list, and the same target list over it,
 * ordered, made distinct and limited here.  What the segments are sent is
 * the target list whole -- what ORDER BY alone names too -- and the
 * condition, with the values carried (carry_mutator()) the call's further
 * arguments.
 */
static void
dist_random_push(Query *q)
{
	RangeTblEntry *old = q->rtable != NIL ? linitial_node(RangeTblEntry, q->rtable) : NULL;
	Query	   *sent = copyObject(q);
	char	   *sql;
	RangeTblFunction *rtfunc = makeNode(RangeTblFunction);
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RangeTblRef *rtr = makeNode(RangeTblRef);
	FuncExpr   *call;
	List	   *names = NIL;
	List	   *tlist = NIL;
	int			n = 0;

	CarryContext carry = {NIL};

	sent->targetList = (List *) carry_mutator((Node *) sent->targetList, &carry);
	sent->jointree->quals = carry_mutator(sent->jointree->quals, &carry);
	sent->hasSubLinks = false;
	sent->sortClause = NIL;
	sent->distinctClause = NIL;
	sent->hasDistinctOn = false;
	sent->limitCount = NULL;
	sent->limitOffset = NULL;
	sent->limitOption = LIMIT_OPTION_COUNT;
	foreach_node(TargetEntry, tle, sent->targetList)
	{
		tle->resjunk = false;
		tle->ressortgroupref = 0;
		if (tle->resname == NULL)
			tle->resname = psprintf("gp_c%d", tle->resno);
		/* a record of no declared type is sent with its row type described */
		if (exprType((Node *) tle->expr) == RECORDOID)
			tle->expr = (Expr *) makeFuncExpr(record_wire_oid, GpRecordWireType(),
											  list_make1(tle->expr), InvalidOid,
											  InvalidOid, COERCE_EXPLICIT_CALL);
	}
	sql = pg_get_querydef(sent, false);

	foreach_node(TargetEntry, tle, q->targetList)
	{
		Node	   *expr = (Node *) tle->expr;
		Var		   *var;
		TargetEntry *copy;

		n++;
		names = lappend(names, makeString(pstrdup(tle->resname != NULL
												  ? tle->resname
												  : "?column?")));
		rtfunc->funccoltypes = lappend_oid(rtfunc->funccoltypes, exprType(expr));
		rtfunc->funccoltypmods = lappend_int(rtfunc->funccoltypmods, exprTypmod(expr));
		rtfunc->funccolcollations = lappend_oid(rtfunc->funccolcollations,
												exprCollation(expr));
		var = makeVar(1, n, exprType(expr), exprTypmod(expr),
					  exprCollation(expr), 0);
		copy = flatCopyTargetEntry(tle);
		copy->expr = (Expr *) var;
		tlist = lappend(tlist, copy);
	}

	/* the text alone, or the text and the values its $n are */
	call = makeFuncExpr(carry.values != NIL ?
						segment_query_values_oid : segment_query_oid,
						RECORDOID,
						lcons(makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID,
										-1, CStringGetTextDatum(sql),
										false, false),
							  carry.values),
						InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	call->funcretset = true;
	rtfunc->funcexpr = (Node *) call;
	rtfunc->funccolcount = n;
	rtfunc->funccolnames = names;

	rte->rtekind = RTE_FUNCTION;
	rte->functions = list_make1(rtfunc);
	rte->eref = makeAlias(old != NULL ? old->eref->aliasname : "gp_segments",
						  copyObject(names));
	rte->inFromCl = true;

	q->rtable = list_make1(rte);
	q->rteperminfos = NIL;
	rtr->rtindex = 1;
	q->jointree = makeFromExpr(list_make1(rtr), NULL);
	q->targetList = tlist;
	q->hasTargetSRFs = false;
	/* a subquery carried is the call's argument now, planned as an initplan */
	q->hasSubLinks = false;
	foreach_ptr(Node, value, carry.values)
		if (IsA(value, SubLink))
			q->hasSubLinks = true;
}

/* ------------------------------------------------------------------------- */
/* A function that runs on all segments: EXECUTE ON ALL SEGMENTS              */
/* ------------------------------------------------------------------------- */

/*
 * Where a set-returning function runs, by the execute_on key of its "gp"
 * label, which gp_sql writes for EXECUTE ON (funcattr.c): 'a'ny node, the
 * 'c'oordinator, an 'i'nitplan, or all 's'egments -- Cloudberry's
 * pg_proc.proexeclocation.  Only a set-returning function has any but ANY,
 * as Cloudberry takes it on no other.
 */
static char
exec_location(Oid funcid)
{
	ObjectAddress addr;
	char	   *value;

	ObjectAddressSet(addr, ProcedureRelationId, funcid);
	value = GpLabelGet(&addr, GP_LABEL_execute_on);
	if (value == NULL || pg_strcasecmp(value, "any") == 0)
		return 'a';
	if (pg_strcasecmp(value, "all_segments") == 0)
		return 's';
	if (pg_strcasecmp(value, "coordinator") == 0 ||
		pg_strcasecmp(value, "master") == 0)
		return 'c';
	if (pg_strcasecmp(value, "initplan") == 0)
		return 'i';
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized \"execute_on\" value \"%s\" on function %s",
					value, format_procedure(funcid))));
	pg_unreachable();
}

/* Does the expression, at its own level, call a function that runs on all segments? */
static bool
all_segments_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
		return false;
	if (IsA(node, FuncExpr) && ((FuncExpr *) node)->funcretset &&
		exec_location(((FuncExpr *) node)->funcid) == 's')
		return true;
	return expression_tree_walker(node, all_segments_walker, context);
}

/*
 * A call in FROM of a function that runs on all segments -- one function,
 * with arguments that read no column of the query -- made a call of
 * gp_internal.segment_query() with the text of a query of the function's
 * rows alone, which each segment answers with its own, as Cloudberry's Function
 * Scan in a slice of every segment does.  An argument the coordinator
 * evaluates for the segments -- a parameter, a subquery of its own -- is
 * carried as a query of gp_dist_random() alone carries one.  WITH ORDINALITY
 * numbers the rows here, gathered.  A call whose arguments read another
 * relation's columns is left where it was, on the coordinator, as are the
 * functions of the port's own that send their call to the segments
 * themselves (GpDispatchFunctionToSegments()).
 */
static void
all_segments_rte(RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc = linitial_node(RangeTblFunction, rte->functions);
	RangeTblFunction *call = makeNode(RangeTblFunction);
	RangeTblEntry *srte;
	RangeTblRef *rtr = makeNode(RangeTblRef);
	Query	   *sent = makeNode(Query);
	CarryContext carry = {NIL};
	List	   *colnames;
	List	   *colvars;
	ListCell   *ln;
	ListCell   *lv;
	FuncExpr   *fexpr;
	int			n = 0;

	if (contain_vars_of_level(rtfunc->funcexpr, 0) ||
		coordinator_only_walker(rtfunc->funcexpr, NULL))
		return;

	/* the query the segments answer: the function's rows, each column */
	srte = copyObject(rte);
	srte->funcordinality = false;
	srte->lateral = false;
	expandRTE(srte, 1, 0, VAR_RETURNING_DEFAULT, -1, true, &colnames, &colvars);
	foreach_ptr(Node, expr, colvars)
	{
		Oid			type = exprType(expr);

		if (type == RECORDOID ? !OidIsValid(record_wire_oid)
			: (get_typtype(type) == TYPTYPE_PSEUDO && type != VOIDOID) ||
			GpTransferType(type) != type)
			return;
	}
	linitial_node(RangeTblFunction, srte->functions)->funcexpr =
		carry_mutator(rtfunc->funcexpr, &carry);
	sent->commandType = CMD_SELECT;
	sent->querySource = QSRC_ORIGINAL;
	sent->canSetTag = true;
	sent->rtable = list_make1(srte);
	rtr->rtindex = 1;
	sent->jointree = makeFromExpr(list_make1(rtr), NULL);
	forboth(ln, colnames, lv, colvars)
	{
		Node	   *expr = (Node *) lfirst(lv);
		Node	   *col = expr;

		n++;
		/* a record of no declared type is sent with its row type described */
		if (exprType(expr) == RECORDOID)
			col = (Node *) makeFuncExpr(record_wire_oid, GpRecordWireType(),
										list_make1(expr), InvalidOid,
										InvalidOid, COERCE_EXPLICIT_CALL);
		sent->targetList = lappend(sent->targetList,
								   makeTargetEntry((Expr *) col, n,
												   psprintf("gp_c%d", n), false));
		call->funccolnames = lappend(call->funccolnames,
									 makeString(pstrdup(strVal(lfirst(ln))[0] != '\0'
														? strVal(lfirst(ln))
														: psprintf("gp_c%d", n))));
		call->funccoltypes = lappend_oid(call->funccoltypes, exprType(expr));
		call->funccoltypmods = lappend_int(call->funccoltypmods, exprTypmod(expr));
		call->funccolcollations = lappend_oid(call->funccolcollations,
											  exprCollation(expr));
	}

	/* and the call that asks them for it */
	fexpr = makeFuncExpr(carry.values != NIL ?
						 segment_query_values_oid : segment_query_oid,
						 RECORDOID,
						 lcons(makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID,
										 -1,
										 CStringGetTextDatum(pg_get_querydef(sent, false)),
										 false, false),
							   carry.values),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	fexpr->funcretset = true;
	call->funcexpr = (Node *) fexpr;
	call->funccolcount = n;
	rte->functions = list_make1(call);
}

/*
 * The functions of q that run on all segments.  One called in FROM is asked
 * of them (all_segments_rte()); one in the target list of a query of no
 * relation makes the query one the segments answer, as a query of
 * gp_dist_random() alone is (dist_random_push()), where each runs it once,
 * as Cloudberry runs it; and one in the target list of a query that reads a
 * relation is refused, as Cloudberry refuses it (preprocess_expression()),
 * for want of a place to run it that is the segments' and the relation's
 * rows' both.
 */
static void
all_segments_push(Query *q)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		FuncExpr   *f;

		if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1 ||
			!IsA(linitial_node(RangeTblFunction, rte->functions)->funcexpr, FuncExpr))
			continue;
		f = (FuncExpr *) linitial_node(RangeTblFunction, rte->functions)->funcexpr;
		if (f->funcretset && exec_location(f->funcid) == 's')
			all_segments_rte(rte);
	}

	if (q->commandType != CMD_SELECT ||
		!all_segments_walker((Node *) q->targetList, NULL))
		return;
	if (q->rtable != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function with EXECUTE ON restrictions cannot be used in the SELECT list of a query with FROM")));
	if (select_alone(q) && q->jointree != NULL &&
		!coordinator_only_walker((Node *) q->targetList, NULL) &&
		!coordinator_only_walker(q->jointree->quals, NULL) &&
		columns_travel(q->targetList))
		dist_random_push(q);
}

static bool
push_dist_random_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;

		(void) query_tree_walker(q, push_dist_random_walker, context, 0);
		if (dist_random_pushable(q))
			dist_random_push(q);
		else
			all_segments_push(q);
		return false;
	}
	return expression_tree_walker(node, push_dist_random_walker, context);
}

/*
 * GpSegmentPushDistRandom
 *		Each query of the statement that reads gp_dist_random() alone and
 *		calls a function that is not immutable, made one the segments run.
 *
 * Before the planner, PostgreSQL's (gp_modify.c) or ORCA (through gp_core's
 * API), by GpPrepareQuery(): on the coordinator of a cluster, which has
 * segments to run it on.
 */
void
GpSegmentPushDistRandom(Query *parse)
{
	if (GpDistRandomIsLocal())
		return;
	lookup_func_oids();
	if (!OidIsValid(segment_query_oid) ||
		!OidIsValid(segment_query_values_oid) ||
		(!OidIsValid(dist_random_oid) && !OidIsValid(dist_random_segments_oid)))
		return;
	(void) push_dist_random_walker((Node *) parse, NULL);
}

/*
 * GpPrepareQuery
 *		A statement as gp_core has it planned, by whichever planner: on a
 *		cluster's coordinator a query of gp_dist_random() alone that calls a
 *		function which is not immutable made one the segments run, and the
 *		size functions made the cluster's (gp_size.c), in that order -- a
 *		size function the segments run is each segment's own; and on every
 *		node pg_tablespace_location() made the location's.
 */
void
GpPrepareQuery(Query *parse)
{
	GpSegmentPushDistRandom(parse);
	GpSizeRewrite(parse);
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * On every node, one or many: a segment parses what the coordinator sends,
 * and a single node answers -1, as Cloudberry's single-node mode does.
 */
void
GpSegmentInit(void)
{
	prev_columnref_fallback_hook = columnref_fallback_hook;
	columnref_fallback_hook = gp_columnref_fallback;
	prev_deparse_function_as_column_hook = deparse_function_as_column_hook;
	deparse_function_as_column_hook = gp_deparse_function_as_column;
	prev_deparse_range_function_hook = deparse_range_function_hook;
	deparse_range_function_hook = gp_deparse_range_function;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = gp_post_parse_analyze;
	prev_object_access_hook = object_access_hook;
	object_access_hook = gp_segment_object_access;

	CacheRegisterSyscacheCallback(PROCOID, invalidate_func_oids, (Datum) 0);
}
