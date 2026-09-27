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
 * gpdbwrappers.h
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/memory/gpdbwrappers.h
 *
 *
 * Ported to PostgreSQL 19:
 *   - EXPLAIN's state is explain_state.h's;
 *   - no wrappers of Cloudberry's cdbexplain and resource groups, nor of
 *     the instrumentation calls that allocated and ended totaltime, which
 *     the port's collector does not make.
 *-------------------------------------------------------------------------
 */

#ifndef GPDBWRAPPERS_H
#define GPDBWRAPPERS_H

extern "C" {
#include "postgres.h"
#include "access/htup.h"
#include "commands/explain.h"
#include "commands/explain_state.h"
#include "nodes/pg_list.h"
#include "utils/elog.h"
#include "utils/memutils.h"
}

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace gpsc
{
class SetQueryReq;
}  // namespace gpsc

namespace gpdb
{

// Functions that call palloc().
// Make sure correct memory context is set.
void *palloc(Size size);
void *palloc0(Size size);
char *pstrdup(const char *str);
char *get_database_name(Oid dbid) noexcept;
bool split_identifier_string(char *rawstring, char separator,
							 List **namelist) noexcept;
ExplainState get_explain_state(QueryDesc *query_desc, bool costs) noexcept;
ExplainState get_analyze_state(QueryDesc *query_desc, bool analyze) noexcept;
HeapTuple heap_form_tuple(TupleDesc tupleDescriptor, Datum *values,
						  bool *isnull);
char *gen_normquery(const char *query) noexcept;
StringInfo gen_normplan(const char *executionPlan) noexcept;
void insert_log(const gpsc::SetQueryReq &req, bool utility);

// Palloc-free functions.
void pfree(void *pointer) noexcept;
MemoryContext mem_ctx_switch_to(MemoryContext context) noexcept;
const char *get_config_option(const char *name, bool missing_ok,
							  bool restrict_superuser) noexcept;
void list_free(List *list) noexcept;

}  // namespace gpdb

#endif /* GPDBWRAPPERS_H */
