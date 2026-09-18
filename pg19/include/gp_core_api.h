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
 * gp_core_api.h
 *	  The surface gp_core offers the other modules of the port.
 *
 * gp_core is preloaded first, and PostgreSQL opens libraries with RTLD_GLOBAL,
 * so the other modules resolve these at load time.  The rendezvous variable
 * carries a version, so that a module built against an older gp_core says so
 * instead of reading a struct that has moved.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_CORE_API_H
#define GP_CORE_API_H

#include "postgres.h"

/*
 * Bump the minor when something is added, the major when anything already
 * here changes meaning or moves.
 */
#define GP_CORE_API_VERSION_MAJOR	1
#define GP_CORE_API_VERSION_MINOR	0

/*
 * What the rendezvous variable points at.  It is the first thing a module
 * sees of gp_core, so it never grows a field in the middle.
 */
typedef struct GpCoreApi
{
	int			version_major;
	int			version_minor;

	/* The role this server was started in; see gp.role. */
	int			(*get_role) (void);

	/* Number of primary segments the extension knows about, 0 in single node. */
	int			(*get_segment_count) (void);

	/* This node's content id: -1 on the coordinator, 0..n-1 on segments. */
	int			(*get_content_id) (void);
} GpCoreApi;

/*
 * The values get_role() returns.  They are the port's spelling of Cloudberry's
 * Gp_role, and the compatibility header maps the old names onto them.
 */
typedef enum GpRole
{
	GP_ROLE_UTILITY = 0,		/* an ordinary local session */
	GP_ROLE_DISPATCH,			/* the coordinator, which dispatches */
	GP_ROLE_EXECUTE,			/* a segment process, which is dispatched to */
} GpRole;

/*
 * Look gp_core up.  Returns NULL when it is not loaded, so a caller that can
 * work without it may check; modules that cannot use CB_REQUIRE_CORE().
 */
extern const GpCoreApi *GpCoreApiLookup(void);

#endif							/* GP_CORE_API_H */
