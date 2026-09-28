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
 * gp_size.h
 *	  The size functions, the cluster's (gp_size.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SIZE_H
#define GP_SIZE_H

#include "postgres.h"

#include "nodes/parsenodes.h"

/*
 * The statement's calls of pg_relation_size() and the other size functions,
 * made calls of gp_internal's, which add the segments' sizes, before it is
 * planned on a cluster's coordinator.
 */
extern void GpSizeRewrite(Query *parse);

/*
 * A table access method whose tables' files are not their relfilenumber's:
 * the size functions measure its tables by its relation_size.  Only while
 * the postmaster loads the module that provides it; through gp_core's API.
 */
struct TableAmRoutine;
extern void GpSizeFromAmRegister(const struct TableAmRoutine *am);

#endif							/* GP_SIZE_H */
