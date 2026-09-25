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
 * memprot/utils/vmem_tracker.h
 *	  The include overlay of Cloudberry's memory protection: this reaches a
 *	  header of Cloudberry's own, and adds what Cloudberry's palloc.h gives
 *	  its memory protection -- whether it is on in this backend, and the report
 *	  of a backend's memory after a segment ran out of it -- which memprot.c
 *	  defines.  See cdb/cdbvars.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_MEMPROT_VMEM_TRACKER_H
#define GP_COMPAT_MEMPROT_VMEM_TRACKER_H

#include "../../../../src/include/utils/vmem_tracker.h"

#include "utils/memutils.h"

/* Cloudberry's palloc.h's */
typedef int64 OOMTimeType;

extern PGDLLIMPORT bool gp_mp_inited;
extern PGDLLIMPORT volatile OOMTimeType *segmentOOMTime;
extern PGDLLIMPORT volatile OOMTimeType oomTrackerStartTime;
extern PGDLLIMPORT volatile OOMTimeType alreadyReportedOOMTime;

extern void UpdateTimeAtomically(volatile OOMTimeType *time_var);

/* A backend has one thread, the one that turned memory protection on */
#define MemoryProtection_IsOwnerThread() true

#define ReportOOMConsumption() \
{ \
	if (gp_mp_inited && *segmentOOMTime >= oomTrackerStartTime && \
		*segmentOOMTime > alreadyReportedOOMTime) \
	{ \
		UpdateTimeAtomically(&alreadyReportedOOMTime); \
		write_stderr("One or more query execution processes ran out of memory on this segment. Logging memory usage."); \
		MemoryContextStats(TopMemoryContext); \
	} \
}

#endif							/* GP_COMPAT_MEMPROT_VMEM_TRACKER_H */
