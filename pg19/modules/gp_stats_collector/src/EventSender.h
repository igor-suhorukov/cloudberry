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
 * EventSender.h
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/EventSender.h
 *
 *
 * Ported to PostgreSQL 19:
 *   - a query's key is kept in a table of the module's by the QueryDesc's
 *     address (GpscQueryKey), PostgreSQL 19's QueryDesc having no
 *     gpsc_query_key, with the transaction and subtransaction it was given
 *     in, for an abort to forget (EventSender::abort_queries());
 *   - QueryMetricsStatus is gp_query_info.h's, the port's spelling of
 *     Cloudberry's utils/metrics_utils.h;
 *   - an error the log saw is kept for the queries an abort ends
 *     (EventSender::error_seen());
 *   - the interconnect's statistics are gone with Cloudberry's
 *     IC_TEARDOWN_HOOK, which Cloudberry's core never defines.
 *-------------------------------------------------------------------------
 */

#ifndef EVENTSENDER_H
#define EVENTSENDER_H

#include <memory>
#include <tuple>
#include <unordered_map>

#define typeid __typeid
extern "C" {
#include "postgres.h"
#include "utils/elog.h"

#include "gp_query_info.h"
}
#undef typeid

#include "Config.h"
#include "PgUtils.h"
#include "memory/gpdbwrappers.h"

class UDSConnector;
struct QueryDesc;
namespace gpsc
{
class SetQueryReq;
}

#include <cstdint>

/*
 * What Cloudberry keeps in QueryDesc.gpsc_query_key: the key a query is
 * reported under, which a query the collector passes over has too, so that
 * its end is passed over as well.  Kept by the QueryDesc's address, with the
 * top-level transaction and the subtransaction it was given in: an abort
 * ends the queries its transaction left in flight, and not one of an
 * earlier transaction a procedure's COMMIT ended under a CALL still
 * running, nor one a hook of the collector's is running -- a CALL whose
 * procedure rolls back -- which that hook sees end.
 */
struct GpscQueryKey
{
	int tmid;
	int ssid;
	int ccnt;
	int nesting_level;
	uintptr_t query_desc_addr;
	uint64_t xact;				/* the transaction's, gpsc_transaction() */
	SubTransactionId subid;
	int active;					/* the hooks running it: an abort then is theirs */
};

/* The query's key, or NULL where it has none */
GpscQueryKey *gpsc_query_key(const QueryDesc *query_desc);
/* A key for the query, which it did not have */
GpscQueryKey *gpsc_query_key_register(const QueryDesc *query_desc);
/* The query's key, gone with the query */
void gpsc_query_key_forget(const QueryDesc *query_desc);
/* A hook of the collector's begins or ends running the query */
void gpsc_query_enter(const QueryDesc *query_desc);
void gpsc_query_leave(const QueryDesc *query_desc);
/* The number of the top-level transaction, and its end */
uint64_t gpsc_transaction();
void gpsc_transaction_end();

struct QueryKey
{
	int tmid;
	int ssid;
	int ccnt;
	int nesting_level;
	uintptr_t query_desc_addr;

	bool
	operator==(const QueryKey &other) const
	{
		return std::tie(tmid, ssid, ccnt, nesting_level, query_desc_addr) ==
			   std::tie(other.tmid, other.ssid, other.ccnt, other.nesting_level,
						other.query_desc_addr);
	}

	static void
	register_qkey(QueryDesc *query_desc, size_t nesting_level)
	{
		GpscQueryKey *key = gpsc_query_key_register(query_desc);

		key->tmid = gpsc_tmid();
		key->ssid = gpsc_session_id();
		key->ccnt = gpsc_command_count();
		key->nesting_level = nesting_level;
		key->query_desc_addr = (uintptr_t) query_desc;
	}

	static QueryKey
	from_qdesc(QueryDesc *query_desc)
	{
		return from_key(gpsc_query_key(query_desc));
	}

	static QueryKey
	from_key(const GpscQueryKey *key)
	{
		return {
			.tmid = key->tmid,
			.ssid = key->ssid,
			.ccnt = key->ccnt,
			.nesting_level = key->nesting_level,
			.query_desc_addr = key->query_desc_addr,
		};
	}
};

// https://www.boost.org/doc/libs/1_35_0/doc/html/boost/hash_combine_id241013.html
template <class T>
inline void
hash_combine(std::size_t &seed, const T &v)
{
	std::hash<T> hasher;
	seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

namespace std
{
template <>
struct hash<QueryKey>
{
	size_t
	operator()(const QueryKey &k) const noexcept
	{
		size_t seed = hash<uint32_t>{}(k.tmid);
		hash_combine(seed, k.ssid);
		hash_combine(seed, k.ccnt);
		hash_combine(seed, k.nesting_level);
		uintptr_t addr = k.query_desc_addr;
		if constexpr (SIZE_MAX < UINTPTR_MAX)
		{
			addr %= SIZE_MAX;
		}
		hash_combine(seed, addr);
		return seed;
	}
};
}  // namespace std

class EventSender
{
public:
	void executor_before_start(QueryDesc *query_desc, int eflags);
	void executor_after_start(QueryDesc *query_desc, int eflags);
	void executor_end(QueryDesc *query_desc);
	void query_metrics_collect(QueryMetricsStatus status, void *arg,
							   bool utility, ErrorData *edata = NULL);
	void analyze_stats_collect(QueryDesc *query_desc);
	void error_seen(const ErrorData *edata);
	void abort_queries(SubTransactionId subid);
	void
	incr_depth()
	{
		nesting_level++;
	}
	void
	decr_depth()
	{
		nesting_level--;
	}
	EventSender();
	~EventSender();

private:
	enum QueryState
	{
		SUBMIT,
		START,
		END,
		DONE
	};

	struct QueryItem
	{
		std::unique_ptr<gpsc::SetQueryReq> message;
		QueryState state;
		uint64_t errors_before = 0;	/* errors_seen as it was submitted */

		explicit QueryItem(QueryState st);
	};

	bool log_query_req(const gpsc::SetQueryReq &req, const std::string &event,
					   bool utility);
	bool verify_query(QueryDesc *query_desc, QueryState state, bool utility);
	void update_query_state(QueryItem &query, QueryState new_state,
							bool utility, bool success = true);
	QueryItem &get_query(QueryDesc *query_desc);
	void submit_query(QueryDesc *query_desc);
	void collect_query_submit(QueryDesc *query_desc, bool utility);
	void report_query_done(QueryDesc *query_desc, QueryItem &query,
						   QueryMetricsStatus status, bool utility,
						   ErrorData *edata = NULL);
	void report_aborted(QueryItem &query, bool utility);
	void collect_query_done(QueryDesc *query_desc, bool utility,
							QueryMetricsStatus status, ErrorData *edata = NULL);
	void update_nested_counters(QueryDesc *query_desc);
	bool qdesc_submitted(QueryDesc *query_desc);
	bool nesting_is_valid(QueryDesc *query_desc, int nesting_level);
	bool need_report_nested_query();
	bool filter_query(QueryDesc *query_desc);
	bool segment_statement(QueryDesc *query_desc, bool utility);

	bool proto_verified = false;
	int nesting_level = 0;
	int64_t nested_calls = 0;
	double nested_timing = 0;
	std::unordered_map<QueryKey, QueryItem> queries;

	/*
	 * The last error the log saw, which ends the queries an abort finds in
	 * flight, and how many had been seen by then, so that a query is told
	 * only of one seen since it was submitted.
	 */
	std::string last_error;
	bool last_error_canceled = false;
	uint64_t errors_seen = 0;

	Config config;
};
#endif /* EVENTSENDER_H */
