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
 * compat/optimizer/orcaopt.h
 *	  What kind of plan the caller is asking ORCA for.
 *
 * Ported from github/cloudberry/src/include/optimizer/orcaopt.h, which is
 * forty lines and two booleans.  Cloudberry's planner() fills one of these and
 * passes it as a fifth argument to planner_hook; PostgreSQL 19's planner_hook
 * has no such argument and the port may not add one, so gp_orca's hook decides
 * both fields for itself and hands the struct down from there.
 *
 * Who decides what, in the port:
 *
 *	 create_parallel_plan		 always false.  Decision 2 defers intra-segment
 *								 parallelism until after M7, and gpdb::
 *								 IsParallelModeOK -- the one wrapper ORCA's own
 *								 core calls -- answers no, which is what keeps
 *								 the parallel xforms from firing.
 *
 *	 create_vectorization_plan	 always false, as Cloudberry's own planner()
 *								 sets it: the vectorized executor that reads it
 *								 is not in the open-source tree.  A future
 *								 engine would ask for it through a function
 *								 gp_orca exports rather than a hook argument.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_ORCAOPT_H
#define GP_ORCA_COMPAT_ORCAOPT_H

typedef struct OptimizerOptions
{
	bool		create_vectorization_plan;
	bool		create_parallel_plan;
} OptimizerOptions;

#endif							/* GP_ORCA_COMPAT_ORCAOPT_H */
