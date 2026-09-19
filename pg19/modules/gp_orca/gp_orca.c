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
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"

#include "optimizer/walkers.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_orca_api.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_orca",
					.version = GP_VERSION
);

PG_FUNCTION_INFO_V1(gp_orca_version);
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
