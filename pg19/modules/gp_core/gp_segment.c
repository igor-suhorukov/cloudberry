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
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
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
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_segment.h"

#define GP_SEGMENT_ID	"gp_segment_id"
#define GP_MPPSESSIONID	"mppsessionid"
#define GP_MPPISWRITER	"mppiswriter"
#define GP_SESS_ID		"sess_id"

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
lookup_func(const char *schema, const char *name, Oid argtype)
{
	Oid			nsp = get_namespace_oid(schema, true);

	if (!OidIsValid(nsp))
		return InvalidOid;
	return GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						   CStringGetDatum(name),
						   PointerGetDatum(buildoidvector(&argtype, 1)),
						   ObjectIdGetDatum(nsp));
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
 * system columns has it: tables, partitioned tables, materialized views.
 * Views and subqueries do not, and nor do functions -- but for
 * gp.dist_random(), whose rows are the segments'.
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
nsitem_is_pg_stat_activity(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	lookup_func_oids();
	return rte->rtekind == RTE_RELATION && OidIsValid(pg_stat_activity_oid) &&
		rte->relid == pg_stat_activity_oid;
}

static bool
nsitem_has_segment_id(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	if (rte->rtekind == RTE_RELATION)
		return ((rte->relkind == RELKIND_RELATION ||
				 rte->relkind == RELKIND_PARTITIONED_TABLE ||
				 rte->relkind == RELKIND_MATVIEW) &&
				!hides_system_columns(rte->relid)) ||
			nsitem_is_pg_locks(nsitem);

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

	/*
	 * A column definition list's names are the entry's too, which ruleutils
	 * prints from: a dropped column's is the placeholder's, its namespace
	 * column the placeholder, which "*" does not expand.
	 */
	foreach_node(String, name, rte->eref->colnames)
	{
		int			i = foreach_current_index(name);

		if (strVal(name)[0] != '\0')
			continue;
		lfirst(list_nth_cell(rte->eref->colnames, i)) =
			makeString(pstrdup(strVal(list_nth(rtfunc->funccolnames, i))));
		nscol = &nsitem->p_nscolumns[i];
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

	rte->eref->colnames = lappend(rte->eref->colnames,
								  makeString(pstrdup(GP_SEGMENT_ID)));

	nsitem->p_nscolumns = repalloc_array(nsitem->p_nscolumns,
										 ParseNamespaceColumn, ncols);
	nscol = &nsitem->p_nscolumns[ncols - 1];
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
	int			session;
	bool		reader;

	if (!GpGddBackendIdentity(lock_row_pid(PG_GETARG_HEAPTUPLEHEADER(0)),
							  &session, &reader))
		PG_RETURN_INT32(-1);
	PG_RETURN_INT32(session);
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
