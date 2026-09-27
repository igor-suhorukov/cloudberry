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
 * PgUtils.h
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/PgUtils.h
 *
 *
 * Ported to PostgreSQL 19:
 *   - what Cloudberry's core keeps in globals of its own -- Gp_role,
 *     gp_session_id, gp_command_count, GpIdentity, gp_gettmid() -- is asked
 *     of gp_core, or kept here (PgUtils.cpp);
 *   - a segment tells the statements the coordinator's has it run from the
 *     ones that carry them, which Cloudberry's segment never sees
 *     (gpsc_segment_transport(), gpsc_segment_slice());
 *   - the header is guarded against a second inclusion.
 *-------------------------------------------------------------------------
 */

#ifndef PGUTILS_H
#define PGUTILS_H

extern "C" {
#include "postgres.h"
#include "commands/explain.h"
}

#include <string>

std::string get_user_name();
std::string get_db_name();
std::string get_rg_name();
bool is_top_level_query(QueryDesc *query_desc, int nesting_level);

/* Cloudberry's dispatcher: the coordinator, or a node on its own */
bool gpsc_coordinator();
/* Cloudberry's executor: a segment's backend the coordinator dispatched to */
bool gpsc_segment();
/* Cloudberry's utility mode: a session of one node of a cluster */
bool gpsc_utility_mode();

/* gp_gettmid(): the node's start, as a time_t */
int gpsc_tmid();
/* gp_session_id, the coordinator's backend's on every node */
int gpsc_session_id();
/* GpIdentity: this node's content id and dbid */
int gpsc_content_id();
int gpsc_dbid();

/* gp_command_count, and its count of a client's statement on the coordinator */
int gpsc_command_count();
void gpsc_count_statement();

/* On a segment: a utility statement that carries the coordinator's */
bool gpsc_segment_transport(const char *query_string);
/* On a segment: a query of the coordinator's statement, by its text */
bool gpsc_segment_slice(const char *source_text);

#endif /* PGUTILS_H */
