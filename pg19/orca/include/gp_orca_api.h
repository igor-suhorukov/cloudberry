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

/*
 * The trace flags the current settings ask for.
 *
 * ORCA has no settings of its own -- everything that can be turned on or off
 * in it is a bit in a set the optimizer is handed -- so this is what
 * gp.optimizer_* adds up to.  The array is palloc'd; the count is returned.
 */
extern int	GpOrcaTraceFlags(int **flags);

/*
 * The probes below reach the gpdb:: wrapper layer, which is C++ and so
 * otherwise unreachable from the module until the translator exists.  Each
 * sets *raised when ORCA raised instead of answering; see orca_probe.cpp for
 * why the layer needs testing before it has a caller.
 */

/* Is this operator NDV-preserving?  Tests compat/cb_operator_oids.h. */
extern bool GpOrcaOpNDVPreserving(Oid opno, bool *raised);

/* The access method a relation is stored with. */
extern char *GpOrcaRelAmName(Oid reloid, bool *raised);

/* Does this index access method handler resolve? */
extern bool GpOrcaIndexAmRoutineExists(Oid am_handler, bool *raised);

/*
 * 1 when this extended statistics object has functional dependencies, 0 when
 * it has none.  *raised would mean the port had dropped Cloudberry's
 * allow_null, which turns every unbuilt statistics object into a failed plan.
 */
extern int	GpOrcaMVDependencyState(Oid stat_oid, bool *raised);

/* Has the catalog changed since this backend last asked? */
extern bool GpOrcaMDCacheNeedsReset(bool *raised);

/* The distribution policy ORCA sees, or NULL when the relation has none. */
extern char *GpOrcaPolicyKind(Oid relid, bool *raised);

/*
 * Call a wrapper that belongs to a later milestone, and return the ORCA
 * exception it raised as "major/minor", or NULL if it did not raise.
 */
extern char *GpOrcaUnportedRaise(void);

/*
 * Replay the planner's aggregate bookkeeping over the port's copies of
 * find_compatible_agg() and find_compatible_trans(), for a list of Aggrefs.
 *
 * Returns the number of aggregates, and sets *aggnos and *transnos to what
 * the port's matchers decided.  The caller compares them against what the
 * planner decided, which PostgreSQL leaves on Aggref.aggno and
 * Aggref.aggtransno.
 */
extern int	GpOrcaReplayAggrefs(struct List *aggrefs, int **aggnos,
								int **transnos, bool *raised);

/*
 * What ORCA's metadata accessor says about one catalog object, as DXL: the
 * relcache translator's answer, through the metadata cache, exactly as the
 * optimizer will ask for it.
 *
 * kind is one of relation, index, check_constraint, type, operator,
 * function, aggregate, relation_stats and column_stats, and the caller checks
 * it; attno is read by column_stats alone.  On a raise *message holds what
 * ORCA logged on the way out, or NULL, and the caller must raise a
 * PostgreSQL error (see orca_probe.cpp for why) -- re-throwing the original
 * when *from_postgres says it is still on the error stack.  The DXL is
 * palloc'd.
 */
extern char *GpOrcaMDDxl(const char *kind, Oid oid, int attno, bool *raised,
						 bool *from_postgres, char **message);

#ifdef __cplusplus
}
#endif

#endif							/* GP_ORCA_API_H */
