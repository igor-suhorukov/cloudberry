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
 * compat/prepagg.c
 *	  find_compatible_agg() and find_compatible_trans(), for a caller that
 *	  has no PlannerInfo.
 *
 * Both bodies are PostgreSQL 19's, from
 * pg19/src/backend/optimizer/prep/prepagg.c, with two changes and no others:
 *
 *	1. `static` is gone, because ORCA calls them.
 *	2. The lists come in as parameters rather than through `root`.  ORCA
 *	   builds an Agg node without ever building a PlannerInfo, so there is no
 *	   `root` to reach them through; it keeps the two lists itself, in
 *	   CContextDXLToPlStmt.  Cloudberry makes the same change.
 *
 * `find_compatible_trans` also loses PostgreSQL's `newagg` parameter, which
 * its body does not use.
 *
 * Keeping the bodies verbatim is deliberate: what these two functions decide
 * is which aggregates may share a transition state, and a difference between
 * the planner's answer and ORCA's would be a wrong plan rather than a
 * compile error.  When PostgreSQL changes them, this file is re-copied.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"

#include "cb_prepagg.h"

/*
 * find_compatible_agg - search for a previously initialized per-Agg struct
 *
 * Searches the previously looked at aggregates to find one which is compatible
 * with this one, with the same input parameters.  If no compatible aggregate
 * can be found, returns -1.
 *
 * As a side-effect, this also collects a list of existing, shareable per-Trans
 * structs with matching inputs.  If no identical Aggref is found, the list is
 * passed later to find_compatible_trans, to see if we can at least reuse
 * the state value of another aggregate.
 */
int
find_compatible_agg(List *agginfos, Aggref *newagg,
					List **same_input_transnos)
{
	ListCell   *lc;
	int			aggno;

	*same_input_transnos = NIL;

	/* we mustn't reuse the aggref if it contains volatile function calls */
	if (contain_volatile_functions((Node *) newagg))
		return -1;

	/*
	 * Search through the list of already seen aggregates.  If we find an
	 * existing identical aggregate call, then we can re-use that one.  While
	 * searching, we'll also collect a list of Aggrefs with the same input
	 * parameters.  If no matching Aggref is found, the caller can potentially
	 * still re-use the transition state of one of them.  (At this stage we
	 * just compare the parsetrees; whether different aggregates share the
	 * same transition function will be checked later.)
	 */
	aggno = -1;
	foreach(lc, agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *existingRef;

		aggno++;

		existingRef = linitial_node(Aggref, agginfo->aggrefs);

		/* all of the following must be the same or it's no match */
		if (newagg->inputcollid != existingRef->inputcollid ||
			newagg->aggtranstype != existingRef->aggtranstype ||
			newagg->aggstar != existingRef->aggstar ||
			newagg->aggvariadic != existingRef->aggvariadic ||
			newagg->aggkind != existingRef->aggkind ||
			!equal(newagg->args, existingRef->args) ||
			!equal(newagg->aggorder, existingRef->aggorder) ||
			!equal(newagg->aggdistinct, existingRef->aggdistinct) ||
			!equal(newagg->aggfilter, existingRef->aggfilter))
			continue;

		/* if it's the same aggregate function then report exact match */
		if (newagg->aggfnoid == existingRef->aggfnoid &&
			newagg->aggtype == existingRef->aggtype &&
			newagg->aggcollid == existingRef->aggcollid &&
			equal(newagg->aggdirectargs, existingRef->aggdirectargs))
		{
			list_free(*same_input_transnos);
			*same_input_transnos = NIL;
			return aggno;
		}

		/*
		 * Not identical, but it had the same inputs.  If the final function
		 * permits sharing, return its transno to the caller, in case we can
		 * re-use its per-trans state.  (If there's already sharing going on,
		 * we might report a transno more than once.  find_compatible_trans is
		 * cheap enough that it's not worth spending cycles to avoid that.)
		 */
		if (agginfo->shareable)
			*same_input_transnos = lappend_int(*same_input_transnos,
											   agginfo->transno);
	}

	return -1;
}

/*
 * find_compatible_trans - search for a previously initialized per-Trans
 * struct
 *
 * Searches the list of transnos for a per-Trans struct with the same
 * transition function and initial condition. (The inputs have already been
 * verified to match.)
 */
int
find_compatible_trans(List *aggtransinfos, bool shareable,
					  Oid aggtransfn, Oid aggtranstype,
					  int transtypeLen, bool transtypeByVal,
					  Oid aggcombinefn,
					  Oid aggserialfn, Oid aggdeserialfn,
					  Datum initValue, bool initValueIsNull,
					  List *transnos)
{
	ListCell   *lc;

	/* If this aggregate can't share transition states, give up */
	if (!shareable)
		return -1;

	foreach(lc, transnos)
	{
		int			transno = lfirst_int(lc);
		AggTransInfo *pertrans = list_nth_node(AggTransInfo,
											   aggtransinfos,
											   transno);

		/*
		 * if the transfns or transition state types are not the same then the
		 * state can't be shared.
		 */
		if (aggtransfn != pertrans->transfn_oid ||
			aggtranstype != pertrans->aggtranstype)
			continue;

		/*
		 * The serialization and deserialization functions must match, if
		 * present, as we're unable to share the trans state for aggregates
		 * which will serialize or deserialize into different formats.
		 * Remember that these will be InvalidOid if they're not required for
		 * this agg node.
		 */
		if (aggserialfn != pertrans->serialfn_oid ||
			aggdeserialfn != pertrans->deserialfn_oid)
			continue;

		/*
		 * Combine function must also match.  We only care about the combine
		 * function with partial aggregates, but it's too early in the
		 * planning to know if we will do partial aggregation, so be
		 * conservative.
		 */
		if (aggcombinefn != pertrans->combinefn_oid)
			continue;

		/*
		 * Check that the initial condition matches, too.
		 */
		if (initValueIsNull && pertrans->initValueIsNull)
			return transno;

		if (!initValueIsNull && !pertrans->initValueIsNull &&
			datumIsEqual(initValue, pertrans->initValue,
						 transtypeByVal, transtypeLen))
			return transno;
	}
	return -1;
}
