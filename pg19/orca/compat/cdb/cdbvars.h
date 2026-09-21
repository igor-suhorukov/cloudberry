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
 * compat/cdb/cdbvars.h
 *	  ORCA asks for the cluster and for its own settings by Cloudberry's name
 *	  for the file that holds both.
 *
 * Cloudberry's cdbvars.h is one header for two unrelated things: what this
 * backend's role in the cluster is, and the 438 settings of guc_gp.c.  The
 * port keeps them apart --
 *
 *	 pg19/compat/cb_compat.h		 the cluster, over gp_core's rendezvous API
 *	 pg19/orca/config/gp_orca_guc.h	 the optimizer's own settings, as gp.*
 *
 * -- and this header is the seam, so that a translator file Cloudberry wrote
 * still finds what it asks for.  Nothing is declared here that is not declared
 * in one of those two.
 *
 * One name is deliberately absent.  Cloudberry's gp_internal_is_singlenode is
 * a setting; the port's IS_SINGLENODE() reads gp_core's is_single_node() flag
 * instead, for the reason under "ORCA is told there are no segments" in
 * cloudberry.md: the segment count is a number ORCA does arithmetic with and
 * must be at least 1, so single-node mode cannot be spelled as a count of
 * zero.  A file that reads the setting directly should fail to compile here
 * and be changed to ask the macro.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CDBVARS_H
#define GP_ORCA_COMPAT_CDBVARS_H

/* Gp_role, GpIdentity, getgpsegmentCount(), IS_SINGLENODE() and friends. */
#include "cb_compat.h"

/* optimizer_*, which is every setting ORCA reads. */
#include "gp_orca_guc.h"

#endif							/* GP_ORCA_COMPAT_CDBVARS_H */
