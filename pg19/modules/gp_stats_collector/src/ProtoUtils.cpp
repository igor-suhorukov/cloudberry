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
 * ProtoUtils.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/ProtoUtils.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - the query's key and the node's are gp_core's (PgUtils.cpp), and the
 *     headers it had through Cloudberry's cdb headers are included for
 *     themselves;
 *   - whether ORCA made the plan is gp_orca's mark in the PlannedStmt's
 *     extension_state, where Cloudberry's PlannedStmt has planGen;
 *   - a node's instrumentation is PostgreSQL 19's NodeInstrumentation, its
 *     times instr_time and its buffers' read and write times those of shared
 *     blocks;
 *   - what is spilled is the backend's temporary blocks written, of
 *     PostgreSQL 19's buffer usage, where Cloudberry's workfile manager
 *     counts bytes and files: no count of files;
 *   - no bytes or chunks a Motion sent or received, which Cloudberry's
 *     motion layer counts and the port's does not, and no slice's number on a
 *     segment, which the port's slices do not tell (0, the coordinator's);
 *   - the interconnect's statistics are gone with Cloudberry's
 *     IC_TEARDOWN_HOOK, which Cloudberry's core never defines.
 *-------------------------------------------------------------------------
 */

#include "ProtoUtils.h"
#include "Config.h"
#include "PgUtils.h"
#include "ProcStats.h"
#include "memory/gpdbwrappers.h"

#define typeid __typeid
#define operator __operator
extern "C" {
#include "postgres.h"
#include "access/hash.h"
#include "access/xact.h"
#include "executor/instrument.h"
#include "mb/pg_wchar.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
}
#undef typeid
#undef operator

#include <ctime>
#include <string>
#include <sys/time.h>

/* The mark gp_orca leaves in PlannedStmt.extension_state (gp_orca_planner.c) */
#define GP_ORCA_PLAN_MARK "gp_orca"

namespace
{
constexpr uint8_t UTF8_CONTINUATION_BYTE_MASK = (1 << 7) | (1 << 6);
constexpr uint8_t UTF8_CONTINUATION_BYTE = (1 << 7);
constexpr uint8_t UTF8_MAX_SYMBOL_BYTES = 4;

// Returns true if byte is the starting byte of utf8
// character, false if byte is the continuation (10xxxxxx).
inline bool
utf8_start_byte(uint8_t byte)
{
	return (byte & UTF8_CONTINUATION_BYTE_MASK) != UTF8_CONTINUATION_BYTE;
}
}  // namespace

google::protobuf::Timestamp
current_ts()
{
	google::protobuf::Timestamp current_ts;
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	current_ts.set_seconds(tv.tv_sec);
	current_ts.set_nanos(static_cast<int32_t>(tv.tv_usec * 1000));
	return current_ts;
}

void
set_query_key(gpsc::QueryKey *key)
{
	key->set_ccnt(gpsc_command_count());
	key->set_ssid(gpsc_session_id());
	key->set_tmid(gpsc_tmid());
}

void
set_segment_key(gpsc::SegmentKey *key)
{
	key->set_dbid(gpsc_dbid());
	key->set_segindex(gpsc_content_id());
}

/* Did ORCA make the plan, which gp_orca marks it with? */
static bool
planned_by_orca(PlannedStmt *plannedstmt)
{
	ListCell *lc;

	foreach (lc, plannedstmt->extension_state)
	{
		DefElem *def = lfirst_node(DefElem, lc);

		if (strcmp(def->defname, GP_ORCA_PLAN_MARK) == 0)
			return true;
	}
	return false;
}

std::string
trim_str_shrink_utf8(const char *str, size_t len, size_t lim)
{
	if (unlikely(str == nullptr))
	{
		return std::string();
	}
	if (likely(len <= lim || GetDatabaseEncoding() != PG_UTF8))
	{
		return std::string(str, std::min(len, lim));
	}

	// Handle trimming of utf8 correctly, do not cut multi-byte characters.
	size_t cut_pos = lim;
	size_t visited_bytes = 1;
	while (visited_bytes < UTF8_MAX_SYMBOL_BYTES && cut_pos > 0)
	{
		if (utf8_start_byte(static_cast<uint8_t>(str[cut_pos])))
		{
			break;
		}
		++visited_bytes;
		--cut_pos;
	}

	return std::string(str, cut_pos);
}

void
set_query_plan(gpsc::SetQueryReq *req, QueryDesc *query_desc,
			   const Config &config)
{
	if (gpsc_coordinator() && query_desc->plannedstmt)
	{
		auto qi = req->mutable_query_info();
		qi->set_generator(planned_by_orca(query_desc->plannedstmt)
							  ? gpsc::PlanGenerator::PLAN_GENERATOR_OPTIMIZER
							  : gpsc::PlanGenerator::PLAN_GENERATOR_PLANNER);
		MemoryContext oldcxt =
			gpdb::mem_ctx_switch_to(query_desc->estate->es_query_cxt);
		ExplainState es = gpdb::get_explain_state(query_desc, true);
		if (es.str)
		{
			*qi->mutable_plan_text() = trim_str_shrink_utf8(
				es.str->data, es.str->len, config.max_plan_size());
			StringInfo norm_plan = gpdb::gen_normplan(es.str->data);
			if (norm_plan)
			{
				*qi->mutable_template_plan_text() = trim_str_shrink_utf8(
					norm_plan->data, norm_plan->len, config.max_plan_size());
				qi->set_plan_id(hash_any((unsigned char *) norm_plan->data,
										 norm_plan->len));
				gpdb::pfree(norm_plan->data);
			}
			qi->set_query_id(query_desc->plannedstmt->queryId);
			gpdb::pfree(es.str->data);
		}
		gpdb::mem_ctx_switch_to(oldcxt);
	}
}

void
set_query_text(gpsc::SetQueryReq *req, QueryDesc *query_desc,
			   const Config &config)
{
	if (gpsc_coordinator() && query_desc->sourceText)
	{
		auto qi = req->mutable_query_info();
		*qi->mutable_query_text() = trim_str_shrink_utf8(
			query_desc->sourceText, strlen(query_desc->sourceText),
			config.max_text_size());
		char *norm_query = gpdb::gen_normquery(query_desc->sourceText);
		if (norm_query)
		{
			*qi->mutable_template_query_text() = trim_str_shrink_utf8(
				norm_query, strlen(norm_query), config.max_text_size());
			gpdb::pfree(norm_query);
		}
	}
}

void
clear_big_fields(gpsc::SetQueryReq *req)
{
	if (gpsc_coordinator())
	{
		auto qi = req->mutable_query_info();
		qi->clear_plan_text();
		qi->clear_template_plan_text();
		qi->clear_query_text();
		qi->clear_template_query_text();
		qi->clear_analyze_text();
	}
}

void
set_query_info(gpsc::SetQueryReq *req)
{
	if (gpsc_coordinator())
	{
		auto qi = req->mutable_query_info();
		qi->set_username(get_user_name());
		if (IsTransactionState())
			qi->set_databasename(get_db_name());
		qi->set_rsgname(get_rg_name());
	}
}

void
set_qi_nesting_level(gpsc::SetQueryReq *req, int nesting_level)
{
	auto aqi = req->mutable_add_info();
	aqi->set_nested_level(nesting_level);
}

void
set_qi_slice_id(gpsc::SetQueryReq *req)
{
	auto aqi = req->mutable_add_info();
	aqi->set_slice_id(0);
}

void
set_qi_error_message(gpsc::SetQueryReq *req, const char *err_msg,
					 const Config &config)
{
	auto aqi = req->mutable_add_info();
	*aqi->mutable_error_message() =
		trim_str_shrink_utf8(err_msg, strlen(err_msg), config.max_text_size());
}

void
set_metric_instrumentation(gpsc::MetricInstrumentation *metrics,
						   QueryDesc *query_desc, int nested_calls,
						   double nested_time)
{
	auto instrument = query_desc->planstate->instrument;
	if (instrument)
	{
		metrics->set_ntuples(instrument->ntuples);
		metrics->set_nloops(instrument->nloops);
		metrics->set_tuplecount(instrument->tuplecount);
		metrics->set_firsttuple(INSTR_TIME_GET_DOUBLE(instrument->firsttuple));
		metrics->set_startup(INSTR_TIME_GET_DOUBLE(instrument->startup));
		metrics->set_total(INSTR_TIME_GET_DOUBLE(instrument->instr.total));
		auto &buffusage = instrument->instr.bufusage;
		metrics->set_shared_blks_hit(buffusage.shared_blks_hit);
		metrics->set_shared_blks_read(buffusage.shared_blks_read);
		metrics->set_shared_blks_dirtied(buffusage.shared_blks_dirtied);
		metrics->set_shared_blks_written(buffusage.shared_blks_written);
		metrics->set_local_blks_hit(buffusage.local_blks_hit);
		metrics->set_local_blks_read(buffusage.local_blks_read);
		metrics->set_local_blks_dirtied(buffusage.local_blks_dirtied);
		metrics->set_local_blks_written(buffusage.local_blks_written);
		metrics->set_temp_blks_read(buffusage.temp_blks_read);
		metrics->set_temp_blks_written(buffusage.temp_blks_written);
		metrics->set_blk_read_time(
			INSTR_TIME_GET_DOUBLE(buffusage.shared_blk_read_time));
		metrics->set_blk_write_time(
			INSTR_TIME_GET_DOUBLE(buffusage.shared_blk_write_time));
	}
	metrics->set_inherited_calls(nested_calls);
	metrics->set_inherited_time(nested_time);
}

void
set_gp_metrics(gpsc::GPMetrics *metrics, QueryDesc *query_desc,
			   int nested_calls, double nested_time)
{
	if (query_desc->planstate && query_desc->planstate->instrument)
	{
		set_metric_instrumentation(metrics->mutable_instrumentation(),
								   query_desc, nested_calls, nested_time);
	}
	fill_self_stats(metrics->mutable_systemstat());
	metrics->mutable_systemstat()->set_runningtimeseconds(
		time(NULL) - metrics->mutable_systemstat()->runningtimeseconds());
	metrics->mutable_spill()->set_totalbytes(
		pgBufferUsage.temp_blks_written * BLCKSZ -
		metrics->mutable_spill()->totalbytes());
}

gpsc::SetQueryReq
create_query_req(gpsc::QueryStatus status)
{
	gpsc::SetQueryReq req;
	req.set_query_status(status);
	*req.mutable_datetime() = current_ts();
	set_query_key(req.mutable_query_key());
	set_segment_key(req.mutable_segment_key());
	return req;
}

double
protots_to_double(const google::protobuf::Timestamp &ts)
{
	return double(ts.seconds()) + double(ts.nanos()) / 1000000000.0;
}

void
set_analyze_plan_text(QueryDesc *query_desc, gpsc::SetQueryReq *req,
					  const Config &config)
{
	// Make sure it is a valid txn and it is not an utility
	// statement for ExplainPrintPlan() later.
	if (!IsTransactionState() || !query_desc->plannedstmt)
	{
		return;
	}
	MemoryContext oldcxt =
		gpdb::mem_ctx_switch_to(query_desc->estate->es_query_cxt);
	ExplainState es = gpdb::get_analyze_state(
		query_desc, query_desc->instrument_options && config.enable_analyze());
	gpdb::mem_ctx_switch_to(oldcxt);
	if (es.str)
	{
		// Remove last line break.
		if (es.str->len > 0 && es.str->data[es.str->len - 1] == '\n')
		{
			es.str->data[--es.str->len] = '\0';
		}
		auto trimmed_analyze = trim_str_shrink_utf8(es.str->data, es.str->len,
													config.max_plan_size());
		req->mutable_query_info()->set_analyze_text(trimmed_analyze);
		gpdb::pfree(es.str->data);
	}
}
