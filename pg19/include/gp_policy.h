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
 * gp_policy.h
 *	  How a table's rows are spread over the segments.
 *
 * Cloudberry keeps this in a catalog of its own, gp_distribution_policy, and
 * caches it on the relcache entry as rd_cdbpolicy.  An extension can do
 * neither, so the port keeps it in the "gp" label's distributed_by key and
 * builds the struct on demand.
 *
 * The struct is Cloudberry's, minus its NodeTag.  Cloudberry needs one
 * because a GpPolicy is embedded in an IntoClause and so travels through
 * copyObject and equal(); the port's never does, and PostgreSQL 19's NodeTag
 * enum is generated from its own sources, so an extension could not add to it
 * in any case.  Everything a reader looks at -- ptype, nattrs, attrs,
 * opclasses -- is Cloudberry's, under Cloudberry's names, because ORCA's
 * translator is written against them.
 *
 * WHY THERE IS NO CACHE.  Cloudberry pays for the policy once per relcache
 * entry; the port pays for it once per call, which is a label read plus one
 * syscache lookup per distribution column.  That is the right trade while
 * nothing asks for it in a loop.  When something does -- M2's dispatch will --
 * the place to put a cache is here, keyed by relation OID with a syscache
 * invalidation callback on pg_seclabel, and nothing above has to change.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_POLICY_H
#define GP_POLICY_H

#include "postgres.h"

#include "access/attnum.h"

/*
 * Where a relation's rows live.
 *
 * ENTRY is the coordinator alone, which is what a relation with no policy at
 * all means and what every table means on one node.  PARTITIONED with no
 * columns is random; with columns it is hashed on them.  The port keeps
 * Cloudberry's spelling of all three because ORCA's translator switches on
 * them.
 */
typedef enum GpPolicyType
{
	POLICYTYPE_PARTITIONED,		/* rows spread over the segments */
	POLICYTYPE_ENTRY,			/* rows on the coordinator */
	POLICYTYPE_REPLICATED,		/* every segment holds every row */
} GpPolicyType;

typedef struct GpPolicy
{
	GpPolicyType ptype;
	int			numsegments;	/* how many segments to spread over: the
								 * first so many, all of them but in a
								 * partial table */

	/* These apply to POLICYTYPE_PARTITIONED, and nattrs may be 0. */
	int			nattrs;
	AttrNumber *attrs;			/* the distribution key columns */
	Oid		   *opclasses;		/* and the opclass each is hashed with */
} GpPolicy;

/*
 * The policy recorded on this relation, or NULL when there is none.
 *
 * NULL is not an error and not a missing answer: it is what an unlabelled
 * relation means, which on one node is every relation nobody wrote
 * DISTRIBUTED BY for.  Callers read it as POLICYTYPE_ENTRY -- all rows in one
 * place -- which is what the predicates below do, and what ORCA's translator
 * makes of a null policy.
 *
 * Raises if the label is there but cannot be read: a column it names that the
 * relation does not have, a type that cannot be hashed, or a value in a shape
 * the port does not know.  Answering "no policy" to any of those would plan
 * the wrong distribution rather than report the problem.
 *
 * The result is palloc'd in the current context.
 */
extern GpPolicy *GpPolicyGet(Oid relid);

/*
 * The same, as it is recorded, even where GpPolicyGet() would refuse it: rows
 * spread over more segments than the cluster has, or a key column that is
 * not there (attribute number 0) or cannot be hashed (opclass InvalidOid).
 * What gp_distribution_policy shows.
 */
extern GpPolicy *GpPolicyGetRecorded(Oid relid);

/*
 * What kind of policy this is.  All five accept NULL, which is entry -- the
 * port reaches NULL constantly, where Cloudberry reaches it only for a
 * catalog table, so a predicate that dereferenced it would be wrong far more
 * often here than there.
 */
extern bool GpPolicyIsEntry(const GpPolicy *policy);
extern bool GpPolicyIsPartitioned(const GpPolicy *policy);
extern bool GpPolicyIsRandomPartitioned(const GpPolicy *policy);
extern bool GpPolicyIsHashPartitioned(const GpPolicy *policy);
extern bool GpPolicyIsReplicated(const GpPolicy *policy);

/*
 * The opclass a distribution key of this type is hashed with, as Cloudberry's
 * cdb_default_distribution_opclass_for_type() chooses it; InvalidOid when the
 * type cannot be a distribution key.
 */
extern Oid	GpPolicyDefaultOpclass(Oid typeoid);

#endif							/* GP_POLICY_H */
