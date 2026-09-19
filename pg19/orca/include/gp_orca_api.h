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
 * gp_orca_api.h
 *	  What gp_orca's C code may call in ORCA's C++.
 *
 * ORCA is C++ and PostgreSQL is C, so everything the module needs crosses
 * here, and only here.  Cloudberry has the same boundary in
 * src/include/gpopt/CGPOptimizer.h; this is its PostgreSQL 19 form.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_API_H
#define GP_ORCA_API_H

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Bring ORCA up in this backend, if it is not up already.
 *
 * ORCA is initialised per backend and on demand, never in _PG_init: its
 * libraries build process-local state (memory pool manager, xform factory,
 * DXL token table) that a postmaster has no use for and that every backend
 * would then inherit through fork.  pgorca does the same.
 */
extern void GpOrcaEnsureInitialized(void);

/* Is ORCA up in this backend? */
extern bool GpOrcaIsInitialized(void);

/*
 * There is no counterpart that takes ORCA down.  Outside an assert-enabled
 * build gpopt_terminate() is an empty function, ORCA's pools are process
 * memory that exit reclaims, and nothing in the port has a reason to plan
 * with ORCA and then stop being able to.  One goes in when something needs it.
 */

/*
 * How many transformation rules this ORCA actually has.
 *
 * Retired rules are not counted; see GpOrcaXformName() for why the id space
 * is larger than this.  It is the number that says which ORCA is linked in:
 * Cloudberry's tree and the single-node fork differ here, because each
 * carries rules the other does not.
 */
extern int	GpOrcaXformCount(void);

/* One past the largest transformation-rule id, for walking the id space. */
extern int	GpOrcaXformIdLimit(void);

/*
 * The name of one transformation rule, or NULL if that id is not a rule.
 *
 * Ids run from 0 to GpOrcaXformIdLimit() - 1, but the space has holes: rules
 * retired over ORCA's life keep their ids so that the rules around them do
 * not move.  A caller walks the whole space and skips the NULLs.  ORCA must
 * be up, because the names live in the xform factory.
 */
extern const char *GpOrcaXformName(int xform_id);

#ifdef __cplusplus
}
#endif

#endif							/* GP_ORCA_API_H */
