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
 * cb_compat.h
 *	  Cloudberry's own spellings, over the port's state.
 *
 * Cloudberry asks about its role in the cluster in 1,355 places across 207
 * backend files.  Those call sites are not worth rewriting, so the macros keep
 * their names and read gp_core's state instead of the global variables the
 * fork declares.
 *
 * "Single node" means what it means on Cloudberry: the extension is loaded and
 * there are no segments.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_COMPAT_H
#define CB_COMPAT_H

#include "fmgr.h"

#include "cb_module.h"
#include "gp_core_api.h"

/*
 * gp_core, found through the rendezvous variable it publishes and remembered
 * per translation unit.
 *
 * An accessor rather than a variable somebody has to define: a module that
 * uses one of the role macros below would otherwise have to remember to
 * define the variable and to assign it in _PG_init, and forgetting either is
 * an undefined symbol at load time rather than a compile error.  A module may
 * only ask after gp_core is loaded, which CB_REQUIRE_CORE has checked by
 * then; find_rendezvous_variable() creates an empty slot if it has not been,
 * so the lookup is retried rather than cached as NULL.
 */
static inline const GpCoreApi *
cb_core_api(void)
{
	static const GpCoreApi *cached = NULL;

	if (cached == NULL)
	{
		void	  **rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS);

		cached = (const GpCoreApi *) *rv;
	}

	return cached;
}

#define Gp_role					(cb_core_api()->get_role())
#define GpIdentity_segindex		(cb_core_api()->get_content_id())
#define getgpsegmentCount()		(cb_core_api()->get_segment_count())

#define IS_QUERY_DISPATCHER() \
	(Gp_role == GP_ROLE_DISPATCH)
#define IS_QUERY_EXECUTOR_BACKEND() \
	(Gp_role == GP_ROLE_EXECUTE)
/*
 * Not "the segment count is zero".  The count is a divisor -- ORCA asserts
 * 0 < segments and divides by it -- so it is never zero, and this question is
 * asked of a flag instead, as Cloudberry asks it of gp_internal_is_singlenode.
 */
#define IS_SINGLENODE() \
	(cb_core_api()->is_single_node())
#define IS_QD_OR_SINGLENODE() \
	(IS_QUERY_DISPATCHER() || IS_SINGLENODE())
#define IS_UTILITY_OR_SINGLENODE() \
	(Gp_role == GP_ROLE_UTILITY || IS_SINGLENODE())
#define IS_UTILITY_BUT_NOT_SINGLENODE() \
	(Gp_role == GP_ROLE_UTILITY && !IS_SINGLENODE())

#endif							/* CB_COMPAT_H */
