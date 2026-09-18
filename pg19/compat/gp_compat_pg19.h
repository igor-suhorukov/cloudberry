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
 * gp_compat_pg19.h
 *	  PostgreSQL 16 -> 19 shims for Cloudberry sources built by this port.
 *
 * Cloudberry's sources are written against PostgreSQL 16.9.  Where PostgreSQL
 * renamed something or changed an argument list, and the change is mechanical,
 * it is bridged here so that the Cloudberry source can be compiled unchanged.
 * Anything that changed in substance is rewritten in the module instead; see
 * cloudberry.md, the "Macros and compat layer" section of each track.
 *
 * This header is included last, after the PostgreSQL headers.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_PG19_H
#define GP_COMPAT_PG19_H

#include "postgres.h"

#include "access/htup_details.h"
#include "storage/procnumber.h"
#include "storage/proc.h"

/*
 * Backend ids became process numbers (PG17).
 */
#define BackendId			ProcNumber
#define InvalidBackendId	INVALID_PROC_NUMBER
#define GP_PROC_LXID(p)		((p)->vxid.lxid)

/*
 * MinimalTuple builders take a trailing amount of extra space (PG18).
 */
#define gp_heap_form_minimal_tuple(desc, values, isnull) \
	heap_form_minimal_tuple((desc), (values), (isnull), 0)

/*
 * ExecutorRun lost execute_once (PG18).
 */
#define GP_EXECUTOR_RUN(qd, dir, count) \
	ExecutorRun((qd), (dir), (count))

/*
 * A wait event set is now owned by a ResourceOwner rather than allocated in a
 * memory context, and NULL means "for the life of the session" (PG17).  There
 * is no ResetWaitEventSet any more, so callers free and create instead; the
 * transports do this once per gang, not per row.
 */
#define GP_CREATE_WAIT_EVENT_SET(nevents) \
	CreateWaitEventSet(NULL, (nevents))

/*
 * Relation paths are returned by value in a fixed-size struct rather than in
 * palloc'd memory (PG18), so callers read .str and must not pfree.
 */
#define GP_RELPATH_STR(rlocator, forknum) \
	(relpath((rlocator), (forknum)).str)

/*
 * Cloudberry's fault injection becomes PostgreSQL's injection points, which
 * are compiled in only where the build asked for them.
 */
#ifndef SIMPLE_FAULT_INJECTOR
#define SIMPLE_FAULT_INJECTOR(name)		INJECTION_POINT(name, NULL)
#endif

#endif							/* GP_COMPAT_PG19_H */
