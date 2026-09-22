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
 * gp_scan.h
 *	  Reading and writing distributed tables when PostgreSQL's planner plans.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SCAN_H
#define GP_SCAN_H

#include "postgres.h"

#include "gp_policy.h"

/*
 * The policy of a table whose rows are on the segments, or NULL for one whose
 * rows are here: on a single node every table, on a cluster a table with no
 * policy or an entry one.
 */
extern GpPolicy *GpScanDistributedPolicy(Oid relid);

/* The scan hooks, where there is a cluster; see gp_scan.c. */
extern void GpScanInit(void);

/* The write path, where there is a cluster; see gp_modify.c. */
extern void GpModifyInit(void);

#endif							/* GP_SCAN_H */
