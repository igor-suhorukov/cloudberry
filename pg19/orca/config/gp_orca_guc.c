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
 * config/gp_orca_guc.c
 *	  ORCA's settings: defined once, from the list in gp_orca_guc.h.
 *
 * Every name gains a "gp." prefix, because PostgreSQL 19 will not define a
 * custom variable without a dot in its name.  See the header for what that
 * corrects in the plan.
 *
 * The defaults and descriptions are Cloudberry's, read off
 * github/cloudberry/src/backend/utils/misc/guc_gp.c.  Nothing here changes
 * what a setting means; the only difference is its spelling and the fact
 * that an extension rather than the server defines it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc.h"
#include "utils/memutils.h"

#include "gp_orca_api.h"
#include "gp_orca_guc.h"

/* One definition per row of the list. */
#define X(var, dflt, doc)	bool var = dflt;
GP_ORCA_BOOL_GUCS(X)
#undef X

#define X(var, dflt, lo, hi, doc)	int var = dflt;
GP_ORCA_INT_GUCS(X)
#undef X

#define X(var, dflt, lo, hi, doc)	double var = dflt;
GP_ORCA_REAL_GUCS(X)
#undef X

/*
 * DefineCustomStringVariable writes through this pointer and frees what it
 * finds, so the initial value has to be one free() may be called on -- NULL,
 * never a string literal.  It is set to the empty string when the variable is
 * defined.
 */
char	   *optimizer_search_strategy_path = NULL;

bool		optimizer_use_gpdb_allocators = true;

int			optimizer_minidump = OPTIMIZER_MINIDUMP_FAIL;
int			optimizer_cost_model = OPTIMIZER_GPDB_CALIBRATED;
int			optimizer_join_order = JOIN_ORDER_EXHAUSTIVE2_SEARCH;
int			optimizer_agg_pds_strategy = OPTIMIZER_AGG_PDS_ALL_KEY;
bool	   *optimizer_xforms = NULL;

static const struct config_enum_entry optimizer_minidump_options[] = {
	{"onerror", OPTIMIZER_MINIDUMP_FAIL, false},
	{"always", OPTIMIZER_MINIDUMP_ALWAYS, false},
	{NULL, 0, false}
};

static const struct config_enum_entry optimizer_cost_model_options[] = {
	{"legacy", OPTIMIZER_GPDB_LEGACY, false},
	{"calibrated", OPTIMIZER_GPDB_CALIBRATED, false},
	{"experimental", OPTIMIZER_GPDB_EXPERIMENTAL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry optimizer_join_order_options[] = {
	{"query", JOIN_ORDER_IN_QUERY, false},
	{"greedy", JOIN_ORDER_GREEDY_SEARCH, false},
	{"exhaustive", JOIN_ORDER_EXHAUSTIVE_SEARCH, false},
	{"exhaustive2", JOIN_ORDER_EXHAUSTIVE2_SEARCH, false},
	{NULL, 0, false}
};

void
GpOrcaDefineSettings(void)
{
	/*
	 * The booleans, one DefineCustomBoolVariable per row.  "gp." is glued on
	 * here rather than written into the list, so that the list stays a list
	 * of Cloudberry's names and a reader can match it against guc_gp.c.
	 */
#define X(var, dflt, doc) \
	DefineCustomBoolVariable("gp." #var, doc, NULL, &var, dflt, \
							 PGC_USERSET, 0, NULL, NULL, NULL);
	GP_ORCA_BOOL_GUCS(X)
#undef X

	/* The numbers, from the same two lists. */
#define X(var, dflt, lo, hi, doc) \
	DefineCustomIntVariable("gp." #var, doc, NULL, &var, dflt, lo, hi, \
							PGC_USERSET, 0, NULL, NULL, NULL);
	GP_ORCA_INT_GUCS(X)
#undef X

#define X(var, dflt, lo, hi, doc) \
	DefineCustomRealVariable("gp." #var, doc, NULL, &var, dflt, lo, hi, \
							 PGC_USERSET, 0, NULL, NULL, NULL);
	GP_ORCA_REAL_GUCS(X)
#undef X

	DefineCustomStringVariable("gp.optimizer_search_strategy_path",
							   "Sets the search strategy used by the optimizer.",
							   NULL,
							   &optimizer_search_strategy_path,
							   "",
							   PGC_USERSET, 0, NULL, NULL, NULL);

	/*
	 * Postmaster context, as in Cloudberry: the allocator is chosen when
	 * ORCA's memory pool manager is built, and every pool made afterwards
	 * inherits the choice, so changing it in a session would mean nothing.
	 */
	DefineCustomBoolVariable("gp.optimizer_use_gpdb_allocators",
							 "Have the optimizer allocate through PostgreSQL memory contexts.",
							 NULL,
							 &optimizer_use_gpdb_allocators,
							 true,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.optimizer_minidump",
							 "Generate optimizer minidump.",
							 NULL,
							 &optimizer_minidump,
							 OPTIMIZER_MINIDUMP_FAIL,
							 optimizer_minidump_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.optimizer_cost_model",
							 "Set optimizer cost model.",
							 NULL,
							 &optimizer_cost_model,
							 OPTIMIZER_GPDB_CALIBRATED,
							 optimizer_cost_model_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.optimizer_join_order",
							 "Set optimizer join heuristic model.",
							 NULL,
							 &optimizer_join_order,
							 JOIN_ORDER_EXHAUSTIVE2_SEARCH,
							 optimizer_join_order_options,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	/*
	 * An int with a range rather than an enum, as in Cloudberry: the values
	 * are named by #define there and the setting is written as a number.
	 */
	DefineCustomIntVariable("gp.optimizer_agg_pds_strategy",
							"Set the strategy of agg required distribution.",
							NULL,
							&optimizer_agg_pds_strategy,
							OPTIMIZER_AGG_PDS_ALL_KEY,
							OPTIMIZER_AGG_PDS_ALL_KEY,
							OPTIMIZER_AGG_PDS_EXCLUDE_NON_FIXED,
							PGC_USERSET, 0, NULL, NULL, NULL);

	/*
	 * The disabled-xform array.  ORCA's sentinel is asked for rather than
	 * hardcoded, because the id space grows with every rule added upstream
	 * and a fixed size here would be a buffer overrun on the next merge.
	 */
	optimizer_xforms = (bool *) MemoryContextAllocZero(TopMemoryContext,
													   sizeof(bool) *
													   GpOrcaXformIdLimit());
}
