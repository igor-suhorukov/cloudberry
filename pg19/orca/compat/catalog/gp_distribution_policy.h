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
 * compat/catalog/gp_distribution_policy.h
 *	  ORCA asks for the distribution policy by Cloudberry's name for it.
 *
 * In Cloudberry this is a catalog header: it defines the gp_distribution_policy
 * relation with CATALOG(), and the GpPolicy struct beside it.  The port has no
 * such catalog -- an extension cannot add one -- and keeps the policy in a `gp`
 * security label instead, read into the same struct by gp_core.  So this header
 * is a forwarding header and nothing else, and everything it forwards to is in
 * pg19/include/gp_policy.h.
 *
 * What the translator does not get, and why it does not need it:
 *
 *	 GpPolicy.type (NodeTag)   Cloudberry embeds a GpPolicy in an IntoClause,
 *							   so its copy travels through copyObject() and
 *							   equal() and needs a tag.  The port's never does,
 *							   and PostgreSQL 19 generates its NodeTag enum
 *							   from its own sources, so an extension could not
 *							   add to it in any case.
 *
 *	 GpPolicyRelationId, the    Catalog OIDs and the Form_ struct.  There is no
 *	 Form_ struct, the index    catalog.
 *	 OID, SYM_POLICYTYPE_*
 *
 *	 GP_POLICY_DEFAULT_NUMSEGMENTS()  Picking a default segment count when a
 *							   table is created.  That is DDL, which is
 *							   gp_sql's, not the optimizer's.
 *
 * GpPolicyFetch() is the one function ORCA calls from this header, and it is
 * spelled here over the port's GpPolicyGet().  The difference worth knowing is
 * that Cloudberry's never returns NULL -- it builds an entry policy on the fly
 * for a relation with no catalog row -- while the port's returns NULL for the
 * unlabelled relation, which on one node is nearly every relation.  ORCA's
 * relcache translator already reads a null policy as EreldistrMasterOnly, so
 * the port hands NULL straight through rather than manufacturing a struct that
 * says the same thing.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_GP_DISTRIBUTION_POLICY_H
#define GP_ORCA_COMPAT_GP_DISTRIBUTION_POLICY_H

#include "gp_policy.h"

/* Cloudberry's spelling of the reader. */
#define GpPolicyFetch(relid)	GpPolicyGet(relid)

#endif							/* GP_ORCA_COMPAT_GP_DISTRIBUTION_POLICY_H */
