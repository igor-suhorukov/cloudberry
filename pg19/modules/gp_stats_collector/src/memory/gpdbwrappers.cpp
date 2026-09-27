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
 * gpdbwrappers.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/memory/gpdbwrappers.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - EXPLAIN's state and output are PostgreSQL 19's (explain_state.h,
 *     explain_format.h), and EXPLAIN ANALYZE's text has no Cloudberry
 *     statistics of the slices (ExplainPrintExecStatsEnd());
 *   - get_database_name() and heap_form_tuple() are declared where
 *     PostgreSQL 19 declares them (lsyscache.h, htup_details.h);
 *   - no wrappers of Cloudberry's cdbexplain and resource groups, nor of
 *     the instrumentation calls that allocated and ended totaltime, which
 *     the port's collector does not make.
 *-------------------------------------------------------------------------
 */

#include "gpdbwrappers.h"
#include "log/LogOps.h"

extern "C" {
#include "postgres.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "commands/dbcommands.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "nodes/pg_list.h"
#include "stat_statements_parser/pg_stat_statements_parser.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"
}

namespace
{

template <bool Throws, typename Func, typename... Args>
auto
wrap(Func &&func, Args &&...args) noexcept(!Throws)
	-> decltype(func(std::forward<Args>(args)...))
{
	using RetType = decltype(func(std::forward<Args>(args)...));

	// Empty struct for void return type.
	struct VoidResult
	{
	};
	using ResultHolder = std::conditional_t<std::is_void_v<RetType>, VoidResult,
											std::optional<RetType>>;

	bool success;
	ErrorData *edata;
	ResultHolder result_holder;

	PG_TRY();
	{
		if constexpr (!std::is_void_v<RetType>)
		{
			result_holder.emplace(func(std::forward<Args>(args)...));
		}
		else
		{
			func(std::forward<Args>(args)...);
		}
		edata = NULL;
		success = true;
	}
	PG_CATCH();
	{
		MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);
		edata = CopyErrorData();
		MemoryContextSwitchTo(oldctx);
		FlushErrorState();
		success = false;
	}
	PG_END_TRY();

	if (!success)
	{
		std::string err;
		if (edata && edata->message)
		{
			err = std::string(edata->message);
		}
		else
		{
			err = "Unknown error occurred";
		}

		if (edata)
		{
			FreeErrorData(edata);
		}

		if constexpr (Throws)
		{
			throw std::runtime_error(err);
		}

		if constexpr (!std::is_void_v<RetType>)
		{
			return RetType{};
		}
		else
		{
			return;
		}
	}

	if constexpr (!std::is_void_v<RetType>)
	{
		return *std::move(result_holder);
	}
	else
	{
		return;
	}
}

template <typename Func, typename... Args>
auto
wrap_throw(Func &&func, Args &&...args)
	-> decltype(func(std::forward<Args>(args)...))
{
	return wrap<true>(std::forward<Func>(func), std::forward<Args>(args)...);
}

template <typename Func, typename... Args>
auto
wrap_noexcept(Func &&func, Args &&...args) noexcept
	-> decltype(func(std::forward<Args>(args)...))
{
	return wrap<false>(std::forward<Func>(func), std::forward<Args>(args)...);
}
}  // namespace

void *
gpdb::palloc(Size size)
{
	return wrap_throw(::palloc, size);
}

void *
gpdb::palloc0(Size size)
{
	return wrap_throw(::palloc0, size);
}

char *
gpdb::pstrdup(const char *str)
{
	return wrap_throw(::pstrdup, str);
}

char *
gpdb::get_database_name(Oid dbid) noexcept
{
	return wrap_noexcept(::get_database_name, dbid);
}

bool
gpdb::split_identifier_string(char *rawstring, char separator,
							  List **namelist) noexcept
{
	return wrap_noexcept(SplitIdentifierString, rawstring, separator, namelist);
}

ExplainState
gpdb::get_explain_state(QueryDesc *query_desc, bool costs) noexcept
{
	return wrap_noexcept([&]() {
		ExplainState *es = NewExplainState();
		es->costs = costs;
		es->verbose = true;
		es->format = EXPLAIN_FORMAT_TEXT;
		ExplainBeginOutput(es);
		ExplainPrintPlan(es, query_desc);
		ExplainEndOutput(es);
		return *es;
	});
}

ExplainState
gpdb::get_analyze_state(QueryDesc *query_desc, bool analyze) noexcept
{
	return wrap_noexcept([&]() {
		ExplainState *es = NewExplainState();
		es->analyze = analyze;
		es->verbose = true;
		es->buffers = es->analyze;
		es->timing = es->analyze;
		es->summary = es->analyze;
		es->format = EXPLAIN_FORMAT_TEXT;
		ExplainBeginOutput(es);
		if (analyze)
		{
			ExplainPrintPlan(es, query_desc);
		}
		ExplainEndOutput(es);
		return *es;
	});
}

HeapTuple
gpdb::heap_form_tuple(TupleDesc tupleDescriptor, Datum *values, bool *isnull)
{
	if (!tupleDescriptor || !values || !isnull)
		throw std::runtime_error(
			"Invalid input parameters for heap tuple formation");

	return wrap_throw(::heap_form_tuple, tupleDescriptor, values, isnull);
}

void
gpdb::pfree(void *pointer) noexcept
{
	// Note that ::pfree asserts that pointer != NULL.
	if (!pointer)
		return;

	wrap_noexcept(::pfree, pointer);
}

MemoryContext
gpdb::mem_ctx_switch_to(MemoryContext context) noexcept
{
	return MemoryContextSwitchTo(context);
}

const char *
gpdb::get_config_option(const char *name, bool missing_ok,
						bool restrict_superuser) noexcept
{
	if (!name)
		return nullptr;

	return wrap_noexcept(GetConfigOption, name, missing_ok, restrict_superuser);
}

void
gpdb::list_free(List *list) noexcept
{
	if (!list)
		return;

	wrap_noexcept(::list_free, list);
}

char *
gpdb::gen_normquery(const char *query) noexcept
{
	return wrap_noexcept(::gen_normquery, query);
}

StringInfo
gpdb::gen_normplan(const char *exec_plan) noexcept
{
	return wrap_noexcept(::gen_normplan, exec_plan);
}

void
gpdb::insert_log(const gpsc::SetQueryReq &req, bool utility)
{
	return wrap_throw(::insert_log, req, utility);
}
