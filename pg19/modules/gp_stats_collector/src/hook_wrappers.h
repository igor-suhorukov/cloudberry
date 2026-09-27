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
 * hook_wrappers.h
 *
 * IDENTIFICATION
 *	  gpcontrib/gp_stats_collector/src/hook_wrappers.h
 *
 *
 * Ported to PostgreSQL 19:
 *   - no hooks_deinit(), which only _PG_fini() called, and no init_log().
 *-------------------------------------------------------------------------
 */

#ifndef HOOK_WRAPPERS_H
#define HOOK_WRAPPERS_H

#ifdef __cplusplus
extern "C" {
#endif

extern void hooks_init();
extern void gpsc_functions_reset();
extern Datum gpsc_functions_get(FunctionCallInfo fcinfo);

extern void truncate_log();

extern void test_uds_start_server(const char *path);
extern int64_t test_uds_receive(int timeout_ms);
extern void test_uds_stop_server();

#ifdef __cplusplus
}
#endif
#endif /* HOOK_WRAPPERS_H */
