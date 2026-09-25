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
 * memprot/cdb/cdbvars.h
 *	  The include overlay of Cloudberry's memory protection: what the files
 *	  gp_resource compiles where they lie ask of Cloudberry's cdbvars.h.
 *
 * gp_resource compiles Cloudberry's vmem tracker, red zone handler, runaway
 * cleaner, idle tracker, event versions and session states
 * (src/backend/utils/mmgr/ and src/backend/utils/session_state.c)
 * unchanged, with this directory on their include path (meson.build); see
 * ../task/cron.h for why the overlay forwards rather than putting
 * Cloudberry's directory on the include path.  Every file of theirs includes
 * this one, so what they ask of Cloudberry's globals is answered here, in the
 * port's terms: Gp_role is what gp_core says this backend is, gp_session_id
 * the session it works for, and the settings, the counters and the lock of
 * the session states are gp_resource's (memprot.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_MEMPROT_CDBVARS_H
#define GP_COMPAT_MEMPROT_CDBVARS_H

#include "postgres.h"

#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "gp_core_api.h"

/* What this backend is: GP_ROLE_EXECUTE for one the coordinator dispatched to */
extern int	GpClusterBackendRole(void);
#define Gp_role (GpClusterBackendRole())

/* The session this backend works for, -1 for one that works for none */
extern int	GpMemProtSessionId(void);
#define gp_session_id (GpMemProtSessionId())

/*
 * The statement the backend runs: a count of them, and 0 on a segment
 * between two, as Cloudberry's QE resets it before it reads the next.
 */
extern PGDLLIMPORT int gp_command_count;

/* The settings, gp.* spellings of Cloudberry's (memprot.c) */
extern PGDLLIMPORT int gp_vmem_protect_limit;
extern PGDLLIMPORT int gp_vmem_limit_per_query;
extern PGDLLIMPORT bool vmem_process_interrupt;
extern PGDLLIMPORT bool coredump_on_memerror;
extern PGDLLIMPORT int gp_sessionstate_loglevel;

/* Cloudberry's lock of the session states: a tranche of gp_resource's */
extern LWLock *GpMemProtSessionStateLock(void);
#define SessionStateLock (GpMemProtSessionStateLock())

/* Cloudberry's elog.c prints the stack of an error that asks it to; not here */
static inline int
gp_compat_errprintstack(bool printstack)
{
	return 0;
}
#define errprintstack(printstack) gp_compat_errprintstack(printstack)

/* Cloudberry's ProcessInterrupts() is told where it was called from */
#define ProcessInterrupts(filename, lineno) (ProcessInterrupts)()

/* Cloudberry's c.h's */
#ifndef AssertImply
#define AssertImply(condition1, condition2) \
	Assert(!(condition1) || (condition2))
#endif

#endif							/* GP_COMPAT_MEMPROT_CDBVARS_H */
