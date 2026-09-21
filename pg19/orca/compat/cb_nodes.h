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
 * Portions Copyright (c) 2005-2009, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 * The definition below is Cloudberry's, from its src/include/nodes/nodes.h.
 *
 * compat/cb_nodes.h
 *	  What Cloudberry adds to PostgreSQL's nodes.h that the translator uses.
 *
 * One name.  Cloudberry's AggSplit has a fourth mode, the middle stage of a
 * three-stage aggregate: it combines partial states, as a final stage does,
 * and passes them on still partial, as a first stage does.  It is not a new
 * primitive.  It is PostgreSQL's own four options at once, and PostgreSQL
 * 19's executor tests each option by itself (DO_AGGSPLIT_COMBINE and the
 * other three, nodeAgg.c), so an Agg in this mode runs there as it is.
 * PostgreSQL 19's enum names only the combinations its own planner makes,
 * and ORCA makes one more, so the port names it here.
 *
 * Named cb_nodes.h rather than nodes/nodes.h for the reason
 * compat/cb_lsyscache.h gives: a header here named after a PostgreSQL header
 * would shadow it for the whole module.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_NODES_H
#define GP_ORCA_COMPAT_CB_NODES_H

#include "nodes/nodes.h"

/*
 * A macro rather than an enum member: the enum is PostgreSQL's.  The cast
 * makes it an AggSplit where one is assigned, as Cloudberry's member is.
 */
#define AGGSPLIT_INTERMEDIATE \
	((AggSplit) (AGGSPLITOP_SKIPFINAL | AGGSPLITOP_SERIALIZE | \
				 AGGSPLITOP_COMBINE | AGGSPLITOP_DESERIALIZE))

#endif							/* GP_ORCA_COMPAT_CB_NODES_H */
