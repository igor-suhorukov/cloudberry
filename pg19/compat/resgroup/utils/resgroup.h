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
 * resgroup/utils/resgroup.h
 *	  The include overlay of Cloudberry's cgroup code: what it reads of
 *	  Cloudberry's resource groups, from gp_resource's.
 *
 * gp_resource compiles Cloudberry's cgroup code where it lies --
 * src/backend/utils/resgroup/cgroup.c, cgroup-ops-linux-v1.c,
 * cgroup-ops-linux-v2.c, cgroup_io_limit.c and the io_limit grammar -- and
 * drives it from its own resgroup.c through the operations table
 * cgroup.h declares, as Cloudberry's resgroup.c does.  Those files ask for
 * Cloudberry's headers by Cloudberry's names; the directory that holds them
 * cannot go on the include path, since it also holds Cloudberry's copies of
 * PostgreSQL 16 headers (see task/cron.h).  So this directory holds one
 * header per name they ask for: a forward to Cloudberry's own where that
 * header is only the cgroup code's (utils/cgroup.h and the like), and where
 * it is Cloudberry's resource manager's, what the code reads of it, from
 * gp_resource.h -- which spells the settings gp.*, and the policy
 * gp_resource_manager_policy.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_RESGROUP_UTILS_RESGROUP_H
#define GP_COMPAT_RESGROUP_UTILS_RESGROUP_H

#include "miscadmin.h"

#include "gp_resource.h"
#include "utils/cgroup.h"

/* guc_gp.c's policy variable, which is gp_resource's by another name */
#define Gp_resource_manager_policy gp_resource_manager_policy

/* resgroup.h's defaults of a group's cpuset and io_limit */
#define DefaultCpuset	"-1"
#define DefaultIOLimit	"-1"
#define DefaultCPUWeight 100

/* guc_gp.c's, which the cgroup code sets to 0: every process nice 0 */
extern PGDLLIMPORT int gp_segworker_relative_priority;

#endif							/* GP_COMPAT_RESGROUP_UTILS_RESGROUP_H */
