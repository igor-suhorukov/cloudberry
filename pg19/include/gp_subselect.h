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
 * gp_subselect.h
 *	  The planner's route: a correlated scalar subquery of an aggregate made a
 *	  join (gp_subselect.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SUBSELECT_H
#define GP_SUBSELECT_H

#include "nodes/parsenodes.h"

/*
 * Each correlated scalar subquery of an aggregate that reads a distributed
 * table, at every level of the query, made a join with its rows grouped by
 * the correlation, where that gives its answer.  On a cluster's coordinator.
 */
extern void GpSubselectDecorrelate(Query *parse);

#endif							/* GP_SUBSELECT_H */
