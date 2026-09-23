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

#include "access/attnum.h"

/*
 * Bump the minor when something is added, the major when anything already
 * here changes meaning or moves.
 */
#define GP_CORE_API_VERSION_MAJOR	1
#define GP_CORE_API_VERSION_MINOR	6

/*
 * What the rendezvous variable points at.  It is the first thing a module
 * sees of gp_core, so it never grows a field in the middle.
 */
typedef struct GpCoreApi
{
	int			version_major;
	int			version_minor;

	/*
	 * What this backend is: a connection the dispatcher opened executes, the
	 * coordinator dispatches, and anything else is a utility session -- which
	 * is what a psql opened on a segment is, here as in Cloudberry.  It is not
	 * always the node's role; see gp.role for that one.
	 */
	int			(*get_role) (void);

	/*
	 * How many primary segments to compute with.  Never 0: it is a divisor,
	 * not a flag -- ORCA's cost model asserts 0 < segments and divides by it,
	 * and Cloudberry's own getgpsegmentCount() answers 1 for a singleton for
	 * the same reason.  Ask is_single_node() for the question this is not.
	 */
	int			(*get_segment_count) (void);

	/* This node's content id: -1 on the coordinator, 0..n-1 on segments. */
	int			(*get_content_id) (void);

	/*
	 * Is this a single-node server -- the extension loaded, with no segments
	 * configured?  It is a flag of its own and not a segment count of zero,
	 * as Cloudberry's gp_internal_is_singlenode is.  Added in API 1.1.
	 */
	bool		(*is_single_node) (void);

	/*
	 * Which node of the cluster this is: the dbid of its line in the cluster
	 * configuration.  The coordinator keeps 1.  Added in API 1.2.
	 */
	int			(*get_dbid) (void);

	/*
	 * ORCA's Motions, carried out by gp_core (gp_motion.h says what each
	 * does).  Added in API 1.3.
	 */
	bool		(*motion_can_dispatch) (void);
	struct Plan *(*motion_make_gather) (struct Plan *fragment,
										struct List *targetlist,
										struct List *qual,
										int content, int slice, int nkeys,
										const AttrNumber *keys,
										const Oid *sortops,
										const Oid *collations,
										const bool *nullsfirst);
	bool		(*motion_is) (struct Plan *plan);
	int			(*motion_segment) (struct Plan *plan);
	void		(*motion_set_segment) (struct Plan *plan, int content);
	int			(*direct_dispatch_segment) (Oid relid, int nvalues,
											const Oid *types,
											const Datum *values,
											const bool *isnull);

	/* The Motions between segments.  Added in API 1.4. */
	struct Plan *(*motion_make_send) (int type, struct Plan *fragment,
									  struct List *targetlist,
									  struct List *qual, int content,
									  int slice, struct List *hashexprs,
									  struct List *hashfuncs);
	int			(*motion_type) (struct Plan *plan);
	int			(*motion_slice) (struct Plan *plan);
	void		(*motion_set_prepare) (struct Plan *plan, struct List *slices);
	struct Plan *(*motion_make_hash_filter) (struct Plan *child,
											 struct List *targetlist,
											 struct List *qual, int nkeys,
											 const AttrNumber *cols,
											 const Oid *hashfuncs,
											 int segment);
	struct Plan *(*motion_make_dml) (struct Plan *modify, int content,
									 int slice);
	struct Plan *(*split_make) (struct Plan *child, struct List *targetlist,
								struct List *deletecols,
								struct List *insertcols,
								AttrNumber actioncol);
	struct Plan *(*split_modify_make) (struct Plan *child, Index rti,
									   int natts, AttrNumber actioncol,
									   AttrNumber ctidcol);

	/* Since 1.5: the slice that receives a Motion between segments. */
	void		(*motion_set_parent) (struct Plan *plan, int parent);

	/*
	 * Since 1.6: the parameters a Motion's fragment is sent with, as lists of
	 * ids -- PARAM_EXEC ones the coordinator sets, and PARAM_EXTERN ones.
	 */
	void		(*motion_set_params) (struct Plan *plan, struct List *exec_params,
									  struct List *extern_params);
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
