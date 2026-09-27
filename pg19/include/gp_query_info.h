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
 * gp_query_info.h
 *	  Cloudberry's query_info_collect_hook, between the port's modules.
 *
 * Cloudberry's core calls query_info_collect_hook as a query is submitted,
 * starts, is done, fails or is cancelled, and as each node of its plan runs;
 * gp_stats_collector is what sets it.  PostgreSQL 19 has no such hook, and
 * its executor hooks tell a module most of a query's life
 * (gp_stats_collector's hook_wrappers.cpp).  What they cannot tell is a
 * query's wait for a slot of its resource queue, which gp_resource keeps, a
 * module as the collector is.  So the hook is a rendezvous variable, which
 * the collector sets while it is preloaded and gp_resource calls where
 * Cloudberry's ResLockPortal() calls the hook, whichever of the two modules
 * is loaded first.
 *
 * A call with METRICS_QUERY_ERROR or METRICS_QUERY_CANCELED is made while
 * the error is being handled, in a PG_CATCH block, so that the hook can copy
 * it (CopyErrorData()), where Cloudberry's collector read it with
 * Cloudberry's elog_message().
 *
 * Cloudberry source this file stands in for:
 *	  src/include/utils/metrics_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_QUERY_INFO_H
#define GP_QUERY_INFO_H

#include "fmgr.h"

#include "cb_module.h"

/* What happened to the query, or to a node of its plan: Cloudberry's values */
typedef enum
{
	METRICS_PLAN_NODE_INITIALIZE = 100,
	METRICS_PLAN_NODE_EXECUTING,
	METRICS_PLAN_NODE_FINISHED,

	METRICS_QUERY_SUBMIT = 200,
	METRICS_QUERY_START,
	METRICS_QUERY_DONE,
	METRICS_QUERY_ERROR,
	METRICS_QUERY_CANCELING,
	METRICS_QUERY_CANCELED,

	METRICS_INNER_QUERY_DONE = 300
} QueryMetricsStatus;

/* The query's QueryDesc, or the plan node's PlanState, is arg */
typedef void (*query_info_collect_hook_type) (QueryMetricsStatus status,
											   void *arg);

/* Where the hook is kept: NULL until a module sets it. */
static inline query_info_collect_hook_type *
gp_query_info_collect_hook(void)
{
	static void **slot = NULL;

	if (slot == NULL)
		slot = find_rendezvous_variable(CB_QUERY_INFO_RENDEZVOUS);
	return (query_info_collect_hook_type *) slot;
}

/* Tell the hook, if a module set it, as Cloudberry's core does. */
static inline void
gp_query_info_collect(QueryMetricsStatus status, void *arg)
{
	query_info_collect_hook_type hook = *gp_query_info_collect_hook();

	if (hook != NULL)
		hook(status, arg);
}

#endif							/* GP_QUERY_INFO_H */
