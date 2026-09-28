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
 * gp_rtfilter.h
 *	  Cloudberry's runtime filters: a Bloom filter of a hash join's inner
 *	  keys, above its outer side and pushed down into the scans below it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_RTFILTER_H
#define GP_RTFILTER_H

#include "fmgr.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"

extern void GpRtFilterInit(void);

/*
 * EXPLAIN ANALYZE of what the segments ran (gp_explain.c): did a runtime
 * filter work in the node -- a scan it reached, ready for a row it read --
 * as a segment reports it; and the coordinator's copy of a node the
 * segments ran, whose figures EXPLAIN shows are of one that said so.
 */
struct PlanState;
extern bool GpRtFilterWorked(struct PlanState *ps);
extern void GpRtFilterSetWorked(struct PlanState *ps);

/*
 * The table access methods whose scans take a runtime filter's range as scan
 * keys -- one of strategy BTGreaterEqualStrategyNumber at the least of the
 * inner rows' values and one of BTLessEqualStrategyNumber at the greatest, of
 * the column's type, with btree's procedures -- and skip what their
 * statistics rule out by them: Cloudberry's methods that say
 * SCAN_SUPPORT_RUNTIME_FILTER in their scan_flags (PAX's), which PostgreSQL
 * 19's TableAmRoutine has not.  A List of TableAmRoutine pointers in a
 * rendezvous variable, which a module appends its method to from its
 * _PG_init, whichever of it and gp_core loads first.
 */
#define GP_RTFILTER_KEY_METHODS	"gp_core runtime filter key methods"

struct TableAmRoutine;

static inline void
GpRtFilterRegisterKeyMethod(const struct TableAmRoutine *am)
{
	List	  **methods = (List **) find_rendezvous_variable(GP_RTFILTER_KEY_METHODS);
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	*methods = lappend(*methods, (void *) am);
	MemoryContextSwitchTo(oldcxt);
}

/* Does a scan of the method take a runtime filter's range as scan keys? */
static inline bool
GpRtFilterTakesKeys(const struct TableAmRoutine *am)
{
	List	  **methods = (List **) find_rendezvous_variable(GP_RTFILTER_KEY_METHODS);

	return list_member_ptr(*methods, am);
}

#endif							/* GP_RTFILTER_H */
