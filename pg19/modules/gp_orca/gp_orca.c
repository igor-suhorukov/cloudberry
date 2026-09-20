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
#include "access/relation.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/tuplestore.h"

#include "optimizer/walkers.h"

#include "cb_clauses.h"
#include "cb_lsyscache.h"
#include "cb_module.h"
#include "cb_plancat.h"
#include "cb_selfuncs.h"
#include "cb_subselect.h"
#include "cb_tlist.h"
#include "gp_core_api.h"
#include "gp_orca_api.h"
#include "gp_orca_guc.h"
#include "gp_orca_planner.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_orca",
					.version = GP_VERSION
);

PG_FUNCTION_INFO_V1(gp_orca_version);
PG_FUNCTION_INFO_V1(gp_orca_type_name);
PG_FUNCTION_INFO_V1(gp_orca_function_fact);
PG_FUNCTION_INFO_V1(gp_orca_find_aggregate);
PG_FUNCTION_INFO_V1(gp_orca_aggregate_fact);
PG_FUNCTION_INFO_V1(gp_orca_exec_location);
PG_FUNCTION_INFO_V1(gp_orca_cast_fact);
PG_FUNCTION_INFO_V1(gp_orca_operator_fact);
PG_FUNCTION_INFO_V1(gp_orca_comparison_operator);
PG_FUNCTION_INFO_V1(gp_orca_index_opfamilies);
PG_FUNCTION_INFO_V1(gp_orca_default_partition_opfamily);
PG_FUNCTION_INFO_V1(gp_orca_relation_fact);
PG_FUNCTION_INFO_V1(gp_orca_relation_policy);
PG_FUNCTION_INFO_V1(gp_orca_constraint_fact);
PG_FUNCTION_INFO_V1(gp_orca_att_stats_kinds);
PG_FUNCTION_INFO_V1(gp_orca_ext_stats);
PG_FUNCTION_INFO_V1(gp_orca_ext_stats_kinds);
PG_FUNCTION_INFO_V1(gp_orca_partitioned_size);
PG_FUNCTION_INFO_V1(gp_orca_tlist_members);
PG_FUNCTION_INFO_V1(gp_orca_flatten_join_aliases);
PG_FUNCTION_INFO_V1(gp_orca_array_const_to_expr);
PG_FUNCTION_INFO_V1(gp_orca_testexpr_is_hashable);
PG_FUNCTION_INFO_V1(gp_orca_timevalue_scalar);
PG_FUNCTION_INFO_V1(gp_orca_numeric_scalar);
PG_FUNCTION_INFO_V1(gp_orca_fallbacks);
PG_FUNCTION_INFO_V1(gp_orca_reset_fallbacks);
PG_FUNCTION_INFO_V1(gp_orca_traceflags);
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
 * gp_orca.fallbacks()
 *
 * How many statements ORCA planned, and how many it did not and why.
 *
 * Decision 1 asks for this from the first milestone: whether to build Route B
 * as well as Route A is to be decided at M7 from how often the fallback fires
 * on real workloads, and a number that only starts being collected once
 * everything works would not answer that.
 *
 * The counts are the server's, not the session's, and survive a backend.
 */
Datum
gp_orca_fallbacks(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * "planned" is a row like the others so that a reader can take the whole
	 * table and work out a rate without a second call, which would see a
	 * different moment.
	 */
	values[0] = CStringGetTextDatum("planned");
	values[1] = CStringGetTextDatum("ORCA produced the plan");
	values[2] = Int64GetDatum((int64) GpOrcaPlanCount());
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	for (int i = 0; i < GP_FALLBACK_NREASONS; i++)
	{
		values[0] = CStringGetTextDatum(GpOrcaFallbackReasonName(i));
		values[1] = CStringGetTextDatum(GpOrcaFallbackReasonDoc(i));
		values[2] = Int64GetDatum((int64) GpOrcaFallbackCount(i));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * gp_orca.reset_fallbacks()
 *
 * Start counting again.  Useful between the runs of a workload; restricted,
 * because one session resetting them loses another's numbers.
 */
Datum
gp_orca_reset_fallbacks(PG_FUNCTION_ARGS)
{
	GpOrcaResetCounters();
	PG_RETURN_VOID();
}

/*
 * gp_orca.traceflags()
 *
 * The trace flags the current settings ask for.
 *
 * ORCA has no settings of its own.  Everything a person can turn on or off in
 * it is a bit in a set handed to the optimizer when a query is planned, and
 * config/CConfigParamMapping.cpp is where the settings on the outside become
 * the bits on the inside.  This reads that answer, which is otherwise
 * invisible until there is an optimizer to hand it to.
 *
 * The ids are ORCA's own, and the ones above EopttraceDisableXformBase name a
 * transformation rule that is switched off.
 */
Datum
gp_orca_traceflags(PG_FUNCTION_ARGS)
{
	int		   *flags = NULL;
	int			n = GpOrcaTraceFlags(&flags);
	Datum	   *elems = palloc(sizeof(Datum) * (n > 0 ? n : 1));

	for (int i = 0; i < n; i++)
		elems[i] = Int32GetDatum(flags[i]);

	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, n, INT4OID));
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
 * gp_orca.aggregate_fact(oid)
 *
 * What ORCA records about an aggregate when it builds its metadata object.
 * NULL for an OID that is not an aggregate: the compat functions raise on
 * one, as Cloudberry's do, and ORCA asks only after aggregate_exists().
 */
Datum
gp_orca_aggregate_fact(PG_FUNCTION_ARGS)
{
	Oid			aggid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tuple;
	bool		is_ordered;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	if (!aggregate_exists(aggid))
		PG_RETURN_NULL();

	is_ordered = is_agg_ordered(aggid);

	values[0] = BoolGetDatum(is_ordered);
	values[1] = BoolGetDatum(is_agg_partial_capable(aggid));
	values[2] = BoolGetDatum(is_agg_repsafe(aggid));

	/*
	 * The answer ORCA actually acts on, and it is a conjunction rather than
	 * either column: it splits an aggregate, and hashes one, only when the
	 * aggregate is not ordered and has the functions to combine two
	 * transition values (CTranslatorRelcacheToDXL.cpp:1628,1633).
	 */
	values[3] = BoolGetDatum(!is_ordered && is_agg_partial_capable(aggid));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.exec_location(oid)
 *
 * Where a function may run, as the single character ORCA compares against.
 * 'a' is the answer for every function nothing has labelled, and the only one
 * ORCA will plan a call of.
 */
Datum
gp_orca_exec_location(PG_FUNCTION_ARGS)
{
	char		location = func_exec_location(PG_GETARG_OID(0));

	PG_RETURN_TEXT_P(cstring_to_text_with_len(&location, 1));
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

/*
 * The CmpType spellings, shared by the two probes below.  These are the names
 * a test reads, so they are ORCA's own rather than PostgreSQL's COMPARE_*.
 */
static const struct
{
	const char *name;
	CmpType		cmpt;
}			cmptype_names[] =
{
	{"eq", CmptEq},
	{"neq", CmptNEq},
	{"lt", CmptLT},
	{"leq", CmptLEq},
	{"gt", CmptGT},
	{"geq", CmptGEq},
	{"other", CmptOther},
};

/*
 * gp_orca.operator_fact(oid)
 *
 * What an operator means, and which families say so.
 */
Datum
gp_orca_operator_fact(PG_FUNCTION_ARGS)
{
	Oid			opno = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tuple;
	CmpType		cmpt = get_comparison_type(opno);
	const char *name = "other";

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	for (size_t i = 0; i < lengthof(cmptype_names); i++)
	{
		if (cmptype_names[i].cmpt == cmpt)
		{
			name = cmptype_names[i].name;
			break;
		}
	}

	values[0] = CStringGetTextDatum(name);
	values[1] = oid_list_to_array(get_operator_opfamilies(opno));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.comparison_operator(lefttype, righttype, cmptype)
 *
 * The inverse: build the operator of a given meaning over two types.
 */
Datum
gp_orca_comparison_operator(PG_FUNCTION_ARGS)
{
	Oid			lefttype = PG_GETARG_OID(0);
	Oid			righttype = PG_GETARG_OID(1);
	char	   *want = text_to_cstring(PG_GETARG_TEXT_PP(2));
	CmpType		cmpt = CmptOther;
	bool		found = false;
	Oid			result;

	for (size_t i = 0; i < lengthof(cmptype_names); i++)
	{
		if (strcmp(cmptype_names[i].name, want) == 0)
		{
			cmpt = cmptype_names[i].cmpt;
			found = true;
			break;
		}
	}

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown comparison type \"%s\"", want),
				 errhint("Use eq, neq, lt, leq, gt, geq or other.")));

	result = get_comparison_operator(lefttype, righttype, cmpt);

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * gp_orca.index_opfamilies(oid)
 *
 * The operator family of each key column of an index, in order.
 */
Datum
gp_orca_index_opfamilies(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(oid_list_to_array(get_index_opfamilies(PG_GETARG_OID(0))));
}

/*
 * gp_orca.default_partition_opfamily(oid)
 *
 * The btree family a range partition key of this type would use.
 */
Datum
gp_orca_default_partition_opfamily(PG_FUNCTION_ARGS)
{
	Oid			result = default_partition_opfamily_for_type(PG_GETARG_OID(0));

	if (!OidIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * gp_orca.relation_fact(oid)
 *
 * What ORCA asks about a table it is considering.
 */
Datum
gp_orca_relation_fact(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};
	HeapTuple	tuple;
	List	   *keys = get_relation_keys(relid);
	Datum	   *keyarrays;
	int			nkeys = 0;
	ListCell   *lc;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	/*
	 * A key is a list of attribute numbers and a relation has several, so
	 * this is an array of arrays.  It is reported as text, because a
	 * two-dimensional SQL array would have to be rectangular and these are
	 * not: a table can have a one-column key and a three-column one.
	 */
	keyarrays = palloc(sizeof(Datum) * list_length(keys));
	foreach(lc, keys)
	{
		List	   *key = (List *) lfirst(lc);
		StringInfoData buf;
		ListCell   *kc;
		bool		first = true;

		initStringInfo(&buf);
		appendStringInfoChar(&buf, '{');
		foreach(kc, key)
		{
			appendStringInfo(&buf, "%s%d", first ? "" : ",", lfirst_int(kc));
			first = false;
		}
		appendStringInfoChar(&buf, '}');

		keyarrays[nkeys++] = CStringGetTextDatum(buf.data);
	}

	values[0] = PointerGetDatum(construct_array_builtin(keyarrays, nkeys,
													   TEXTOID));
	values[1] = oid_list_to_array(get_check_constraint_oids(relid));
	values[2] = BoolGetDatum(has_subclass_slow(relid));
	values[3] = BoolGetDatum(has_update_triggers(relid, false));
	values[4] = BoolGetDatum(has_update_triggers(relid, true));

	{
		Relation	rel = relation_open(relid, AccessShareLock);

		values[5] = BoolGetDatum(child_distribution_mismatch(rel));
		relation_close(rel, AccessShareLock);
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.relation_policy(regclass)
 *
 * How ORCA is told this relation's rows are spread.  gp.policy() answers the
 * same question from gp_core; this reaches it the way ORCA does, through
 * relation_policy(Relation), so that the entry point the wrapper layer calls
 * is the one under test.
 *
 * NULL when the relation has no policy, which is what ORCA's translator makes
 * EreldistrMasterOnly of -- all rows in one place, which is what one node
 * means.
 */
Datum
gp_orca_relation_policy(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = relation_open(relid, AccessShareLock);
	GpPolicy   *policy = relation_policy(rel);
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;
	const char *kind;
	Datum	   *cols;

	relation_close(rel, AccessShareLock);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	if (policy == NULL)
		PG_RETURN_NULL();

	/*
	 * The four names are the Ereldistrpolicy values the translator derives
	 * from these fields, so that a test reads what ORCA would be told rather
	 * than the struct it is told it with.
	 */
	if (GpPolicyIsReplicated(policy))
		kind = "replicated";
	else if (GpPolicyIsHashPartitioned(policy))
		kind = "hash";
	else if (GpPolicyIsRandomPartitioned(policy))
		kind = "random";
	else
		kind = "masteronly";

	cols = (Datum *) palloc(sizeof(Datum) * Max(policy->nattrs, 1));
	for (int i = 0; i < policy->nattrs; i++)
		cols[i] = Int32GetDatum((int32) policy->attrs[i]);

	values[0] = CStringGetTextDatum(kind);
	values[1] = PointerGetDatum(construct_array_builtin(cols, policy->nattrs,
													   INT4OID));
	values[2] = Int32GetDatum(policy->numsegments);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.constraint_fact(oid)
 *
 * What ORCA reads off a check constraint before turning it into a predicate.
 */
Datum
gp_orca_constraint_fact(PG_FUNCTION_ARGS)
{
	Oid			conoid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;
	char	   *name = get_check_constraint_name(conoid);
	Oid			relid = get_check_constraint_relid(conoid);
	Node	   *expr = get_check_constraint_expr_tree(conoid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	if (name != NULL)
		values[0] = CStringGetTextDatum(name);
	else
		nulls[0] = true;

	if (OidIsValid(relid))
		values[1] = ObjectIdGetDatum(relid);
	else
		nulls[1] = true;

	/*
	 * The expression round-trips through nodeToString rather than being
	 * deparsed: what matters here is that a tree came back at all and that
	 * it is the tree pg_constraint holds, not how it reads.
	 */
	if (expr != NULL)
		values[2] = CStringGetTextDatum(nodeToString(expr));
	else
		nulls[2] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.att_stats_kinds(relid, attnum)
 *
 * The statistic kinds in the pg_statistic row ORCA would read for a column,
 * and whether that row is the inherited one.  NULL when there are no
 * statistics at all.
 *
 * The kinds are what makes this worth asking: ORCA unpacks the slots itself,
 * so what it can do with a column depends on which slots are filled.
 */
Datum
gp_orca_att_stats_kinds(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	AttrNumber	attnum = (AttrNumber) PG_GETARG_INT32(1);
	HeapTuple	stats = get_att_stats(relid, attnum);
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tuple;
	Form_pg_statistic form;
	Datum		kinds[STATISTIC_NUM_SLOTS];
	int			nkinds = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	if (!HeapTupleIsValid(stats))
		PG_RETURN_NULL();

	form = (Form_pg_statistic) GETSTRUCT(stats);

	values[0] = BoolGetDatum(form->stainherit);

	kinds[nkinds++] = Int32GetDatum(form->stakind1);
	kinds[nkinds++] = Int32GetDatum(form->stakind2);
	kinds[nkinds++] = Int32GetDatum(form->stakind3);
	kinds[nkinds++] = Int32GetDatum(form->stakind4);
	kinds[nkinds++] = Int32GetDatum(form->stakind5);

	values[1] = PointerGetDatum(construct_array_builtin(kinds, nkinds,
													   INT4OID));

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/* get_att_stats hands back a copy, and the caller owns it. */
	heap_freetuple(stats);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.ext_stats(regclass)
 *
 * The extended statistics objects ORCA is told a relation has.  One row per
 * (object, kind, stxdinherit) triple, which is the shape
 * GetRelationExtStatistics() returns and the shape the translator walks.
 *
 * A relation analyzed both ways therefore reports each kind twice, differing
 * only in "inherit" -- see compat/plancat.c for why the port keeps that
 * rather than deduplicating.
 */
Datum
gp_orca_ext_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = relation_open(relid, AccessShareLock);
	List	   *infos;
	ListCell   *lc;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	infos = GetRelationExtStatistics(rel);

	foreach(lc, infos)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};
		Datum		keys[INDEX_MAX_KEYS];
		int			nkeys = 0;
		int			attno = -1;
		char	   *name;

		while ((attno = bms_next_member(info->keys, attno)) >= 0 &&
			   nkeys < INDEX_MAX_KEYS)
			keys[nkeys++] = Int32GetDatum(attno);

		name = GetExtStatisticsName(info->statOid);

		values[0] = ObjectIdGetDatum(info->statOid);
		values[1] = CStringGetTextDatum(name);
		values[2] = CharGetDatum(info->kind);
		values[3] = PointerGetDatum(construct_array_builtin(keys, nkeys,
															INT4OID));
		values[4] = BoolGetDatum(info->inherit);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	relation_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * gp_orca.ext_stats_kinds(oid)
 *
 * The kinds pg_statistic_ext.stxkind records for one object -- what CREATE
 * STATISTICS asked for, which is not the same as what ANALYZE has built.
 */
Datum
gp_orca_ext_stats_kinds(PG_FUNCTION_ARGS)
{
	Oid			statOid = PG_GETARG_OID(0);
	List	   *kinds = GetExtStatisticsKinds(statOid);
	Datum	   *elems = palloc(sizeof(Datum) * list_length(kinds));
	int			i = 0;
	ListCell   *lc;

	foreach(lc, kinds)
		elems[i++] = CharGetDatum((char) lfirst_int(lc));

	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, i, CHAROID));
}

/*
 * gp_orca.partitioned_size(regclass)
 *
 * How big ORCA is told a partitioned table is: the rows and pages of the
 * whole thing, summed over its leaves.  PostgreSQL's planner never asks this,
 * because it plans each partition on its own; ORCA costs the table as one
 * object.
 *
 * Worth reading on an ordinary table too, where the answer is that table's
 * own numbers and nothing is summed.
 */
Datum
gp_orca_partitioned_size(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = relation_open(relid, AccessShareLock);
	double		numtuples;
	PageEstimate pages;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	numtuples = cdb_estimate_partitioned_numtuples(rel);
	pages = cdb_estimate_partitioned_numpages(rel);

	relation_close(rel, AccessShareLock);

	values[0] = Float8GetDatum(numtuples);
	values[1] = Int64GetDatum((int64) pages.totalpages);
	values[2] = Int64GetDatum((int64) pages.totalallvisiblepages);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * Parse and analyze one statement, for the probes that need a Query.
 *
 * The same shape as gp_orca.explain_refusal(), and for the same reason:
 * these compat functions take a parse tree, so a test can only reach them
 * through one.
 */
static Query *
probe_parse_one(const char *sql)
{
	List	   *raw = pg_parse_query(sql);

	if (list_length(raw) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("expected exactly one statement, got %d",
						list_length(raw))));

	return parse_analyze_fixedparams(linitial_node(RawStmt, raw), sql,
									 NULL, 0, NULL);
}

/*
 * gp_orca.tlist_members(sql, resno)
 *
 * The resnos of every target entry computing the same expression as the one
 * at "resno".  This is the difference from PostgreSQL's tlist_member(), which
 * answers with the first match and stops: for "SELECT a, a, b" the answer
 * here is {1,2}, and tlist_member() would say 1.
 */
Datum
gp_orca_tlist_members(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			resno = PG_GETARG_INT32(1);
	Query	   *query = probe_parse_one(sql);
	TargetEntry *probe = NULL;
	List	   *matches;
	Datum	   *elems;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resno == resno)
		{
			probe = tle;
			break;
		}
	}

	if (probe == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("no target entry with resno %d", resno)));

	matches = tlist_members((Node *) probe->expr, query->targetList);

	elems = palloc(sizeof(Datum) * list_length(matches));
	foreach(lc, matches)
		elems[i++] = Int32GetDatum(((TargetEntry *) lfirst(lc))->resno);

	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, i, INT4OID));
}

/*
 * Every distinct varno a node tree references, sorted, as an int[].
 */
static Datum
varnos_of(Node *node)
{
	List	   *vars = pull_var_clause(node,
									   PVC_RECURSE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_RECURSE_PLACEHOLDERS);
	Bitmapset  *seen = NULL;
	Datum	   *elems;
	int			n = 0;
	int			varno = -1;
	ListCell   *lc;

	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (IsA(var, Var))
			seen = bms_add_member(seen, (int) var->varno);
	}

	elems = palloc(sizeof(Datum) * Max(bms_num_members(seen), 1));
	while ((varno = bms_next_member(seen, varno)) >= 0)
		elems[n++] = Int32GetDatum(varno);

	return PointerGetDatum(construct_array_builtin(elems, n, INT4OID));
}

/*
 * gp_orca.flatten_join_aliases(sql)
 *
 * Which range table entries the target list refers to, before and after the
 * join alias Vars in it are flattened.
 *
 * That is the whole of what the function is for: a Var that names a JOIN's
 * output column resolves only against the query that owns the JOIN, and
 * ORCA's normalization moves the target list out of that query.  After
 * flattening, the target list names the base relations instead -- two of
 * them, for a USING column, because the merged column is a COALESCE of both
 * sides.
 *
 * "where_after" is the other half of the contract, and the half that would
 * fail silently: the WHERE clause is deliberately *not* flattened, because
 * it does not move, and ORCA resolves its alias Vars during translation
 * through its own <query level, varno, varattno> mapping.  A future version
 * that flattened everything would look like an improvement and would be a
 * behaviour change.
 */
Datum
gp_orca_flatten_join_aliases(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Query	   *query = probe_parse_one(sql);
	Query	   *flat;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tuple;
	List	   *bounds = NIL;
	ListCell   *lc;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = varnos_of((Node *) query->targetList);

	flat = flatten_join_alias_var_optimizer(query, 0);
	values[1] = varnos_of((Node *) flat->targetList);
	values[2] = varnos_of(flat->jointree ? flat->jointree->quals : NULL);

	/*
	 * The window frame bounds, which are the one part the function walks by
	 * hand rather than handing to the mutator: a WindowClause is not an
	 * expression, so expression_tree_mutator would not reach inside it.
	 */
	foreach(lc, flat->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		if (wc == NULL)
			continue;
		if (wc->startOffset)
			bounds = lappend(bounds, wc->startOffset);
		if (wc->endOffset)
			bounds = lappend(bounds, wc->endOffset);
	}
	values[3] = varnos_of((Node *) bounds);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.testexpr_is_hashable(sql)
 *
 * Would ORCA be allowed to hash this WHERE clause, if it were an ANY
 * SubLink's test expression?
 *
 * The question decides whether a subplan builds its subquery into a hash
 * table once or re-runs the comparison per outer row, and PostgreSQL keeps
 * the answer to itself -- see compat/subselect.c.
 *
 * The probe passes an empty list of subquery Param ids, so what it exercises
 * is the operator half of the rule: hashable, strict, binary, and with no
 * Var of the outer query on the right.  The Param half needs a subplan,
 * which is the translator's to build.
 */
Datum
gp_orca_testexpr_is_hashable(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Query	   *query = probe_parse_one(sql);
	Node	   *qual;

	if (query->jointree == NULL || query->jointree->quals == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the statement has no WHERE clause to look at")));

	qual = query->jointree->quals;

	PG_RETURN_BOOL(testexpr_is_hashable(qual, NIL));
}

/*
 * gp_orca.timevalue_scalar(expr)
 *
 * A time-shaped constant on the one scale ORCA compares such values on.
 *
 * "ok" is false for a type the conversion does not know, which the caller has
 * to look at: 0 is a perfectly good timestamp, so the value alone cannot say.
 */
Datum
gp_orca_timevalue_scalar(PG_FUNCTION_ARGS)
{
	char	   *expr = text_to_cstring(PG_GETARG_TEXT_PP(0));
	StringInfoData buf;
	Query	   *query;
	TargetEntry *tle;
	Const	   *c;
	bool		failure = false;
	double		scalar;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT %s", expr);
	query = probe_parse_one(buf.data);

	tle = (TargetEntry *) linitial(query->targetList);
	if (!IsA(tle->expr, Const))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("expression is not a constant")));

	c = (Const *) tle->expr;
	if (c->constisnull)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a null has no scalar value")));

	scalar = convert_timevalue_to_scalar(c->constvalue, c->consttype,
										 &failure);

	values[0] = BoolGetDatum(!failure);
	values[1] = Float8GetDatum(scalar);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * gp_orca.numeric_scalar(numeric)
 *
 * A numeric as the double ORCA holds a histogram bound in.  "No overflow" is
 * the point: a numeric holds values no double can, and a bound that is out of
 * range is still a usable bound once it becomes an infinity.  Raising here
 * would lose the whole histogram over one bucket.
 */
Datum
gp_orca_numeric_scalar(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	PG_RETURN_FLOAT8(numeric_to_double_no_overflow(num));
}

/*
 * gp_orca.array_const_to_expr(expr)
 *
 * What an array constant becomes when ORCA is given it: an ArrayExpr whose
 * elements it can look inside, or the Const unchanged if it was not an array.
 *
 * "in_collation" and "out_collation" are the point.  ArrayExpr grew an
 * array_collid field after Cloudberry forked, and Cloudberry's version of
 * this rewrite does not set it, so on Cloudberry the two differ for any
 * collatable element type and the optimizer is handed an expression whose
 * collation is InvalidOid.  Here they are equal, which is what "the same
 * value, written differently" has to mean.
 */
Datum
gp_orca_array_const_to_expr(PG_FUNCTION_ARGS)
{
	char	   *expr = text_to_cstring(PG_GETARG_TEXT_PP(0));
	StringInfoData buf;
	Query	   *query;
	TargetEntry *tle;
	Const	   *c;
	Expr	   *result;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT %s", expr);
	query = probe_parse_one(buf.data);

	tle = (TargetEntry *) linitial(query->targetList);
	if (!IsA(tle->expr, Const))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("expression is a %s, not a Const",
						nodeToString(tle->expr))));

	c = (Const *) tle->expr;
	result = transform_array_Const_to_ArrayExpr(c);

	values[0] = CStringGetTextDatum(IsA(result, ArrayExpr) ? "ArrayExpr"
								    : "Const");
	values[1] = Int32GetDatum(IsA(result, ArrayExpr) ?
							  list_length(((ArrayExpr *) result)->elements) : 0);
	values[2] = ObjectIdGetDatum(exprCollation((Node *) c));
	values[3] = ObjectIdGetDatum(exprCollation((Node *) result));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("gp_orca");
	CB_REQUIRE_CORE("gp_orca");

	/*
	 * Cloudberry's gp_enable_relsize_collection, which on a cluster means
	 * "ask the segments how big the table really is, instead of trusting
	 * what ANALYZE left in pg_class".  The name gains a dot because
	 * PostgreSQL 19 will not define a custom setting without one
	 * (pg19/src/backend/utils/misc/guc.c:951-957) -- which is true of all
	 * 438 of Cloudberry's settings, not only the ones written to files.
	 *
	 * It is read by compat/plancat.c, where the comment says what it does on
	 * one node and what M2 turns it into.
	 */
	DefineCustomBoolVariable("gp.enable_relsize_collection",
							 "Ask the segments for a relation's real size.",
							 "When off, a relation that has never been "
							 "analyzed is estimated from pg_class alone.",
							 &gp_enable_relsize_collection,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * No MarkGUCPrefixReserved here, for the reason gp_core gives: the "gp"
	 * prefix is shared by every module, and reserving it would drop the
	 * placeholders belonging to modules that have not loaded yet.
	 */

	GpOrcaDefineSettings();
	GpOrcaInstallPlannerHook();

	/*
	 * ORCA is not brought up here.  Its libraries build process-local state
	 * that a postmaster has no use for, and that every backend would then
	 * inherit through fork; a backend that never plans with ORCA should pay
	 * nothing for it.  GpOrcaEnsureInitialized() does it on first use.
	 */
}
