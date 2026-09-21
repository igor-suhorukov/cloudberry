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
 * gp_orca_planner.h
 *	  ORCA behind planner_hook, and the count of what it would not plan.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_PLANNER_H
#define GP_ORCA_PLANNER_H

#include "postgres.h"

/*
 * Why a query did not get an ORCA plan.
 *
 * Decision 1 asks for these from the first milestone, "on real workloads":
 * the choice between Route A alone and also building Route B is to be made at
 * M7 from these numbers, so they have to have been collected before then.
 * A reason is only useful if it says what to do about it, so they are the
 * reasons a person could act on rather than a single "fell back" tally.
 *
 * GP_FALLBACK_KEYS(X) -- X(key, what it means)
 */
#define GP_FALLBACK_KEYS(X) \
	X(disabled,       "gp.optimizer is off") \
	X(not_dispatcher, "this backend does not plan for the cluster") \
	X(cursor_option,  "a cursor ORCA does not plan") \
	X(utility,        "not a query ORCA plans") \
	X(declined,       "ORCA looked and would not plan it") \
	X(error,          "ORCA raised while planning")

typedef enum GpFallbackReason
{
#define X(key, doc)		GP_FALLBACK_##key,
	GP_FALLBACK_KEYS(X)
#undef X
	GP_FALLBACK_NREASONS
} GpFallbackReason;

/* Installed during preload; removed is not supported, as for every hook. */
extern void GpOrcaInstallPlannerHook(void);

/* What the counters say, and putting them back to zero. */
extern uint64 GpOrcaFallbackCount(GpFallbackReason reason);
extern uint64 GpOrcaPlanCount(void);
extern void GpOrcaResetCounters(void);
extern const char *GpOrcaFallbackReasonName(GpFallbackReason reason);
extern const char *GpOrcaFallbackReasonDoc(GpFallbackReason reason);

#endif							/* GP_ORCA_PLANNER_H */
