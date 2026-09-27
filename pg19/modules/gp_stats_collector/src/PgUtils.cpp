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
 * PgUtils.cpp
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/PgUtils.cpp
 *
 *
 * Ported to PostgreSQL 19:
 *   - Gp_role, gp_session_id, GpIdentity and gp_gettmid() are gp_core's
 *     (gp_cluster.h), and a node on its own is its own dispatcher;
 *   - gp_command_count, which Cloudberry's core counts on the coordinator
 *     and sends each segment, is counted here, as a statement of the client
 *     is submitted, and sent in the setting gpsc.command_count
 *     (gpsc_count_statement());
 *   - the resource group is not named: gp_resource keeps a session's group
 *     to itself (get_rg_name());
 *   - a segment tells what carries the coordinator's statement from the
 *     statement: gpsc_segment_transport(), gpsc_segment_slice().
 *-------------------------------------------------------------------------
 */

#include "PgUtils.h"
#include "Config.h"
#include "EventSender.h"
#include "memory/gpdbwrappers.h"

extern "C" {
#include "access/xact.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
}

/* The client's statements the coordinator's collector counted */
static int command_count = 0;
static TimestampTz command_start = 0;

std::string
get_user_name()
{
	// username is allocated on stack, we don't need to pfree it.
	const char *username =
		gpdb::get_config_option("session_authorization", false, false);
	return username ? std::string(username) : "";
}

std::string
get_db_name()
{
	char *dbname = gpdb::get_database_name(MyDatabaseId);
	if (dbname)
	{
		std::string result(dbname);
		gpdb::pfree(dbname);
		return result;
	}
	return "";
}

/*
 * The resource group the session runs in: none said, gp_resource keeping a
 * session's group to itself, where Cloudberry's core names it
 * (ResGroupGetGroupIdBySessionId()).
 */
std::string
get_rg_name()
{
	return "";
}

/**
 * Things get tricky with nested queries.
 * a) A nested query on master is a real query optimized and executed from
 * master. An example would be `select some_insert_function();`, where
 * some_insert_function does something like `insert into tbl values (1)`. Master
 * will create two statements. Outer select statement and inner insert statement
 * with nesting level 1.
 * For segments both statements are top-level statements with nesting level 0.
 * b) A nested query on segment is something executed as sub-statement on
 * segment. An example would be `select a from tbl where is_good_value(b);`. In
 * this case master will issue one top-level statement, but segments will change
 * contexts for UDF execution and execute  is_good_value(b) once for each tuple
 * as a nested query. Creating massive load on external agent.
 *
 * Hence, here is a decision:
 * 1) ignore all queries that are nested on segments
 * 2) record (if enabled) all queries that are nested on master
 * NODE: The truth is, we can't really ignore nested master queries, because
 * segment sees those as top-level.
 */

bool
is_top_level_query(QueryDesc *query_desc, int nesting_level)
{
	GpscQueryKey *key = gpsc_query_key(query_desc);

	if (key == NULL)
	{
		return nesting_level == 0;
	}
	return key->nesting_level == 0;
}

bool
gpsc_coordinator()
{
	return GpClusterBackendRole() == GP_ROLE_DISPATCH || GpClusterIsSingleNode();
}

bool
gpsc_segment()
{
	return GpClusterBackendRole() == GP_ROLE_EXECUTE;
}

bool
gpsc_utility_mode()
{
	return GpClusterBackendRole() == GP_ROLE_UTILITY && !GpClusterIsSingleNode();
}

/* The node's start, a time_t, as Cloudberry's gp_gettmid(); -1 past 2038 */
int
gpsc_tmid()
{
	pg_time_t t = timestamptz_to_time_t(PgStartTime);

	return (PgStartTime < 0 || t > PG_INT32_MAX) ? -1 : (int) t;
}

int
gpsc_session_id()
{
	return GpClusterSessionId();
}

int
gpsc_content_id()
{
	return GpClusterContentId();
}

int
gpsc_dbid()
{
	return GpClusterDbid();
}

/*
 * gp_command_count: the coordinator's count of its client's statements, and
 * on a segment the one the coordinator sent with the statement.
 */
int
gpsc_command_count()
{
	return gpsc_coordinator() ? command_count : Config::command_count();
}

/*
 * A statement submitted: on the coordinator a new one of the client's where
 * it began at a new time, as gp_core's log counts them (gp_log.c), and its
 * count put in gpsc.command_count, which the dispatcher sends the segments
 * with the statement (gp_dispatch.c's synced_settings).
 */
void
gpsc_count_statement()
{
	TimestampTz start = GetCurrentStatementStartTimestamp();
	char count[16];

	if (!gpsc_coordinator() || start == command_start)
	{
		return;
	}
	command_start = start;
	if (++command_count <= 0)
	{
		command_count = 1;
	}
	snprintf(count, sizeof(count), "%d", command_count);
	(void) set_config_option("gpsc.command_count", count, PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SET, true, 0, false);
}

/*
 * What the coordinator sends a segment to carry its statement rather than
 * as a statement: of the utility statements, all but the ones dispatched as
 * the coordinator's parse tree, which begin with gp_ddl.c's marker -- the
 * transaction's and the settings' commands, and a gather's cursor, declared,
 * fetched from and closed.
 */
bool
gpsc_segment_transport(const char *query_string)
{
	static const char marker[] = "/*gp:dispatched-tree*/";

	return query_string == NULL ||
		   strncmp(query_string, marker, sizeof(marker) - 1) != 0;
}

/*
 * A query of the coordinator's statement, on a segment: a gather's, the
 * planner's SQL or ORCA's fragment behind the cursor gp_dispatch.c names
 * gp_gather_N, or a slice gp_motion.c sends as a call of exec_fragment().
 */
bool
gpsc_segment_slice(const char *source_text)
{
	static const char gather[] = "DECLARE gp_gather_";
	static const char slice[] = "SELECT gp_internal.exec_fragment(";

	return source_text != NULL &&
		   (strncmp(source_text, gather, sizeof(gather) - 1) == 0 ||
			strncmp(source_text, slice, sizeof(slice) - 1) == 0);
}
