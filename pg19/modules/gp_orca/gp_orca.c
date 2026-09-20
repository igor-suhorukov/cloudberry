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
 * gp_orca.c
 *	  ORCA, the cost-based optimizer, behind planner_hook.
 *
 * ORCA runs from planner_hook on the coordinator and hands back a plan whose
 * Motion nodes are CustomScans.  Before it is called, a rewrite adds explicit
 * bounding-box conditions for PostGIS's indexable functions, so those queries
 * stay on ORCA instead of the gather-everything fallback, and a counter
 * records every plan that falls back and why (decision 1).  O4 makes the
 * Motion lines read as they do on Cloudberry.
 *
 * Where the code comes from, and why it is split the way it is:
 *
 *	  ORCA's four core libraries -- libgpos, libnaucrates, libgpopt,
 *	  libgpdbcost, 920 sources and some 380k lines -- are compiled from
 *	  Cloudberry's own tree at their own paths, unmodified.  Not one file
 *	  under them includes a PostgreSQL header, so there is no PostgreSQL 16
 *	  in them to port, and taking them whole is what keeps Cloudberry's ORCA
 *	  features -- plan hints, parallel scans, the dedup-superset
 *	  preprocessor, partial aggregation below joins -- without porting a line.
 *
 *	  The translator, under pg19/orca/, is the port's own code, because it is
 *	  the only part that knows what a PostgreSQL is, and so the only part
 *	  where PostgreSQL 16 had to become PostgreSQL 19.
 *
 * At this milestone the module brings ORCA up and reports what is linked;
 * planning through it comes next.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "optimizer/walkers.h"

#include "cb_lsyscache.h"
#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_orca_api.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_orca",
					.version = GP_VERSION
);

PG_FUNCTION_INFO_V1(gp_orca_version);
PG_FUNCTION_INFO_V1(gp_orca_type_name);
PG_FUNCTION_INFO_V1(gp_orca_function_fact);
PG_FUNCTION_INFO_V1(gp_orca_find_aggregate);
PG_FUNCTION_INFO_V1(gp_orca_cast_fact);
PG_FUNCTION_INFO_V1(gp_orca_xforms);
PG_FUNCTION_INFO_V1(gp_orca_explain_refusal);

/*
 * gp_orca.version()
 *
 * What ORCA is linked in, and whether it comes up.  The xform count is the
 * number that tells one ORCA from another: Cloudberry's tree and pgorca's
 * carry different transformation rules, so the tests read it to confirm the
 * port is running the ORCA it means to.
 */
Datum
gp_orca_version(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	GpOrcaEnsureInitialized();

	values[0] = CStringGetTextDatum("Apache Cloudberry");
	values[1] = Int32GetDatum(GpOrcaXformCount());
	values[2] = BoolGetDatum(GpOrcaIsInitialized());

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.xforms()
 *
 * Every transformation rule this ORCA carries, by name.
 *
 * A rule is what ORCA searches with, so this is the honest answer to "what
 * can this optimizer do".  It is also how to see which ORCA is linked in:
 * Cloudberry's tree carries ExfGet2ParallelTableScan,
 * ExfPushPartialAggBelowJoin and ExfImplementHashSequenceProject, which the
 * single-node fork of ORCA does not.
 */
Datum
gp_orca_xforms(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		GpOrcaEnsureInitialized();
		funcctx->max_calls = GpOrcaXformIdLimit();

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	/*
	 * Walk the id space rather than the rules, and skip the holes in it:
	 * rules retired over ORCA's life keep their ids so that the rules around
	 * them do not move, and those ids have no name.
	 */
	while (funcctx->call_cntr < funcctx->max_calls)
	{
		const char *name = GpOrcaXformName((int) funcctx->call_cntr);

		if (name != NULL)
			SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(name));

		funcctx->call_cntr++;
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * gp_orca.explain_refusal(sql)
 *
 * What the server-side checks say about a query, before ORCA is asked to plan
 * it.  ORCA declines some queries on the answers, and this is how to see why
 * without reading a log: ORDER BY over an ordering operator on a plain column
 * is the KNN shape PostgreSQL's planner turns into a GiST index scan and ORCA
 * cannot, so ORCA leaves those alone.
 *
 * It is also how the port's own copies of those checks are tested.  They are
 * re-implementations of functions Cloudberry adds to PostgreSQL's
 * lsyscache.c and walkers.c, and the port does not build those files, so
 * something has to say that the copies answer the same way.
 */
Datum
gp_orca_explain_refusal(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *raw;
	Query	   *query;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	raw = pg_parse_query(sql);
	if (list_length(raw) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("expected exactly one statement, got %d",
						list_length(raw))));

	query = parse_analyze_fixedparams(linitial_node(RawStmt, raw), sql,
									  NULL, 0, NULL);

	values[0] = BoolGetDatum(has_orderby_ordering_op(query));
	/*
	 * check_collation() answers 1 or -1, not an OID: it says only whether
	 * something in the query carries a collation other than the default.
	 * Cloudberry's copy still carries the merge marker that says so.
	 */
	values[1] = BoolGetDatum(check_collation((Node *) query) == 1);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * The compat layer, seen from SQL.
 *
 * Everything below reports what pg19/orca/compat/ answers.  Those functions
 * are the port's re-implementations of what Cloudberry adds to PostgreSQL's
 * own files, which the port does not build, and they are called from C++ by
 * the gpdb:: wrapper layer -- so without a surface like this, the only thing
 * that could say whether a re-implementation is right would be the
 * translator, which is several milestones of work away.  They are diagnostic
 * as well as testable: "what does ORCA see about this object" is a question
 * worth being able to ask of a live server.
 */

/*
 * A List of OIDs as an oid[], for the probes below.  An empty list is an
 * empty array rather than NULL: ORCA is told "no output arguments", which is
 * not the same as "unknown".
 */
static Datum
oid_list_to_array(List *oids)
{
	Datum	   *elems = palloc(sizeof(Datum) * list_length(oids));
	int			i = 0;
	ListCell   *lc;

	foreach(lc, oids)
		elems[i++] = ObjectIdGetDatum(lfirst_oid(lc));

	return PointerGetDatum(construct_array_builtin(elems, i, OIDOID));
}

/*
 * gp_orca.type_name(oid)
 *
 * pg_type.typname, which is not format_type_be(): no schema qualification and
 * no "[]" on an array type.  NULL for an OID that is not a type, which is the
 * contract ORCA's metadata cache relies on.
 */
Datum
gp_orca_type_name(PG_FUNCTION_ARGS)
{
	char	   *name = get_type_name(PG_GETARG_OID(0));

	if (name == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(name));
}

/*
 * gp_orca.function_fact(oid)
 *
 * What ORCA asks about a function before it can build metadata for it.
 */
Datum
gp_orca_function_fact(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	HeapTuple	tuple;
	bool		exists = function_exists(funcid);
	bool		is_agg = aggregate_exists(funcid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = BoolGetDatum(exists);
	values[1] = BoolGetDatum(is_agg);

	/*
	 * The list accessors raise on an OID that is not a function, so ask them
	 * only once function_exists() has said there is one.  That asymmetry is
	 * Cloudberry's and ORCA depends on it: the existence checks are how it
	 * decides whether to go on.
	 */
	if (exists)
	{
		values[2] = oid_list_to_array(get_func_arg_types(funcid));
		values[3] = oid_list_to_array(get_func_output_arg_types(funcid));
	}
	else
		nulls[2] = nulls[3] = true;

	if (is_agg)
		values[4] = ObjectIdGetDatum(get_agg_transtype(funcid));
	else
		nulls[4] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.find_aggregate(name, argtype)
 *
 * The one-argument aggregate of this name over this type, in any schema.
 * InvalidOid -- reported as NULL -- when there is none.
 */
Datum
gp_orca_find_aggregate(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			result = get_aggregate(name, PG_GETARG_OID(1));

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * gp_orca.cast_fact(src, dst)
 *
 * Whether an implicit cast exists between two types, what performs it, and
 * whether it costs anything at run time.
 */
Datum
gp_orca_cast_fact(PG_FUNCTION_ARGS)
{
	Oid			src = PG_GETARG_OID(0);
	Oid			dst = PG_GETARG_OID(1);
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tuple;
	bool		binary_coercible = false;
	Oid			castfunc = InvalidOid;
	CoercionPathType pathtype = COERCION_PATH_NONE;
	bool		exists;
	const char *pathname;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	exists = get_cast_func(src, dst, &binary_coercible, &castfunc, &pathtype);

	switch (pathtype)
	{
		case COERCION_PATH_NONE:
			pathname = "none";
			break;
		case COERCION_PATH_FUNC:
			pathname = "func";
			break;
		case COERCION_PATH_RELABELTYPE:
			pathname = "relabel";
			break;
		case COERCION_PATH_COERCEVIAIO:
			pathname = "io";
			break;
		case COERCION_PATH_ARRAYCOERCE:
			pathname = "arraycoerce";
			break;
		default:
			pathname = "unknown";
			break;
	}

	values[0] = BoolGetDatum(exists);
	values[1] = BoolGetDatum(binary_coercible);
	if (OidIsValid(castfunc))
		values[2] = ObjectIdGetDatum(castfunc);
	else
		nulls[2] = true;
	values[3] = CStringGetTextDatum(pathname);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("gp_orca");
	CB_REQUIRE_CORE("gp_orca");

	/*
	 * ORCA is not brought up here.  Its libraries build process-local state
	 * that a postmaster has no use for, and that every backend would then
	 * inherit through fork; a backend that never plans with ORCA should pay
	 * nothing for it.  GpOrcaEnsureInitialized() does it on first use.
	 */
}
