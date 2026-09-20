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
 * compat/cb_prepagg.h
 *	  Matching one aggregate against the aggregates already seen.
 *
 * PostgreSQL 19 has both of these in optimizer/prep/prepagg.c and keeps them
 * `static`; Cloudberry's whole change to the file is to un-static them and to
 * take the lists as parameters instead of reaching them through a
 * `PlannerInfo`.  ORCA has no `PlannerInfo` -- that is the planner's state
 * and ORCA is not the planner -- so the second half of that change is not
 * cosmetic.
 *
 * The plan's list of compat files did not include prepagg.c at all.  See
 * "Corrections to the plan" in cloudberry.md: a function counts as "exists in
 * PostgreSQL 19" only if a header declares it, and the check that built the
 * list looked in the sources.
 *
 * Called from CTranslatorDXLToPlStmt, when a DXL aggregate becomes an Agg
 * node and its transition states have to be shared the way the planner would
 * have shared them.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_PREPAGG_H
#define CB_PREPAGG_H

#include "nodes/pathnodes.h"
#include "nodes/primnodes.h"

/*
 * Find an aggregate among `agginfos` with the same inputs as `newagg`, and
 * return its index, or -1.  As a side effect `*same_input_transnos` is set to
 * the transnos of the aggregates that matched on inputs but not exactly, so
 * that a caller which finds no exact match can still try to share a
 * transition state.
 */
extern int	find_compatible_agg(List *agginfos, Aggref *newagg,
								List **same_input_transnos);

/*
 * Find a transition state among `transnos` -- indexes into `aggtransinfos` --
 * that this aggregate can share, or -1.
 *
 * PostgreSQL's signature also takes the Aggref, and does not use it; it is
 * left out here rather than carried as an unused parameter, which is what
 * Cloudberry does too.
 */
extern int	find_compatible_trans(List *aggtransinfos, bool shareable,
								  Oid aggtransfn, Oid aggtranstype,
								  int transtypeLen, bool transtypeByVal,
								  Oid aggcombinefn,
								  Oid aggserialfn, Oid aggdeserialfn,
								  Datum initValue, bool initValueIsNull,
								  List *transnos);

#endif							/* CB_PREPAGG_H */
