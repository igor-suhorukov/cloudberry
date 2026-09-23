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
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_scan.h"
#include "gp_segment.h"

#define GP_SEGMENT_ID	"gp_segment_id"

static columnref_fallback_hook_type prev_columnref_fallback_hook = NULL;
static deparse_function_as_column_hook_type prev_deparse_function_as_column_hook = NULL;

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

static void
invalidate_func_oids(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	func_oids_valid = false;
}

static Oid
lookup_func(const char *schema, const char *name, Oid argtype)
{
	return LookupFuncName(list_make2(makeString(unconstify(char *, schema)),
									 makeString(unconstify(char *, name))),
						  1, &argtype, true);
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
	func_oids_valid = true;
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

/*
 * Does this entry have gp_segment_id?  In Cloudberry every relation that has
 * system columns has it: tables, partitioned tables, materialized views.
 * Views and subqueries do not, and nor do functions -- but for
 * gp.dist_random(), whose rows are the segments'.
 */
static bool
nsitem_has_segment_id(ParseNamespaceItem *nsitem)
{
	RangeTblEntry *rte = nsitem->p_rte;

	if (rte->rtekind == RTE_RELATION)
		return rte->relkind == RELKIND_RELATION ||
			rte->relkind == RELKIND_PARTITIONED_TABLE ||
			rte->relkind == RELKIND_MATVIEW;

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

		/* A column definition list has no place for a dropped column. */
		if (att->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gp_segment_id of gp.dist_random() is not available for \"%s\", which has dropped columns",
							RelationGetRelationName(rel)),
					 parser_errposition(pstate, location)));

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

/* gp_segment_id of this entry's row. */
static Node *
make_segment_id(ParseState *pstate, ParseNamespaceItem *nsitem,
				int sublevels_up, int location)
{
	Var		   *var;
	FuncExpr   *fexpr;

	if (nsitem->p_rte->rtekind == RTE_FUNCTION)
	{
		add_dist_random_segment_column(pstate, nsitem, location);
		return scanNSItemForColumn(pstate, nsitem, sublevels_up,
								   GP_SEGMENT_ID, location);
	}

	/* The row, as transformWholeRowRef() makes it for "t.*" */
	var = makeWholeRowVar(nsitem->p_rte, nsitem->p_rtindex, sublevels_up, true);
	var->location = location;
	markNullableIfNeeded(pstate, var);
	markVarForSelectPriv(pstate, var);

	fexpr = makeFuncExpr(segment_of_oid, INT4OID, list_make1(var),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	fexpr->location = location;
	return (Node *) fexpr;
}

/*
 * The one entry an unqualified gp_segment_id means: the nearest query level
 * with a relation that has it, where it is an error for there to be two, as
 * it is for any column two relations have.
 */
static ParseNamespaceItem *
find_unqualified(ParseState *pstate, int location, int *sublevels_up)
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
			if (!nsitem_has_segment_id(nsitem))
				continue;
			if (found != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous",
								GP_SEGMENT_ID),
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

	if (prev_columnref_fallback_hook)
	{
		Node	   *node = prev_columnref_fallback_hook(pstate, cref);

		if (node != NULL)
			return node;
	}

	if (nfields > 3 || !IsA(last, String) ||
		strcmp(strVal(last), GP_SEGMENT_ID) != 0)
		return NULL;

	/* Without the extension in this database the name means nothing. */
	lookup_func_oids();
	if (!OidIsValid(segment_of_oid))
		return NULL;

	if (nfields == 1)
		nsitem = find_unqualified(pstate, cref->location, &sublevels_up);
	else
	{
		char	   *nspname = NULL;
		char	   *relname;

		if (nfields == 3)
			nspname = strVal(linitial(cref->fields));
		relname = strVal(list_nth(cref->fields, nfields - 2));
		nsitem = refnameNamespaceItem(pstate, nspname, relname,
									  cref->location, &sublevels_up);
		if (nsitem != NULL && !nsitem_has_segment_id(nsitem))
			nsitem = NULL;
	}

	if (nsitem == NULL)
		return NULL;
	return make_segment_id(pstate, nsitem, sublevels_up, cref->location);
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
	if (prev_deparse_function_as_column_hook)
		return prev_deparse_function_as_column_hook(expr);
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
				 errdetail("Its rows were read here, and nothing in a row says which segment held it."),
				 errhint("Compare gp_segment_id in a condition on that table alone, which the segments evaluate, or read it through gp.dist_random().")));
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
	Datum	   *values;
	bool	   *nulls;
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

	values = palloc_array(Datum, natts + 1);
	nulls = palloc_array(bool, natts + 1);

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	gather = GpGatherStart(sql.data, tupdesc);
	while (GpGatherNext(gather, slot, &content))
	{
		slot_getallattrs(slot);
		memcpy(values, slot->tts_values, natts * sizeof(Datum));
		memcpy(nulls, slot->tts_isnull, natts * sizeof(bool));
		values[natts] = Int32GetDatum(content);
		nulls[natts] = false;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	GpGatherEnd(gather);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
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

	CacheRegisterSyscacheCallback(PROCOID, invalidate_func_oids, (Datum) 0);
}
