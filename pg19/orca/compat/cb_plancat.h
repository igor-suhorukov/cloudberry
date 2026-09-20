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
 * compat/cb_plancat.h
 *	  What Cloudberry adds to PostgreSQL's plancat.c for ORCA.
 *
 * Five functions, and they answer two questions ORCA asks about a relation
 * that PostgreSQL's own planner never asks in this form:
 *
 *	  what extended statistics does it have, asked without a RelOptInfo,
 *	  because ORCA builds metadata before it has built anything planner-ish;
 *
 *	  how big is a partitioned table as a whole, which PostgreSQL answers by
 *	  planning the children separately and ORCA answers by summing first.
 *
 * Named cb_plancat.h rather than optimizer/plancat.h for the reason
 * compat/cb_lsyscache.h gives at length: compat/ comes before the server's
 * include directory, so a file named after a PostgreSQL header would shadow
 * it for every translation unit in the module.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_CB_PLANCAT_H
#define GP_ORCA_COMPAT_CB_PLANCAT_H

#include "nodes/pg_list.h"
#include "storage/block.h"
#include "utils/relcache.h"

/*
 * Ask this node how big a relation really is, rather than trusting the
 * numbers ANALYZE left in pg_class.
 *
 * Cloudberry's gp_enable_relsize_collection, off by default, and it means
 * what its name says on a cluster: go and ask the segments.  On one node
 * there is nobody to ask but this backend, and this backend can read the
 * file -- so the port answers it locally, which is the same answer.  M2 is
 * where it becomes a dispatch; see compat/plancat.c.
 */
extern PGDLLIMPORT bool gp_enable_relsize_collection;

/*
 * Extended statistics.
 *
 * ORCA asks for the objects on a relation (kinds and the columns each
 * covers), and then for one object's name.  It never asks for the data: it
 * reads the ndistinct and dependency values itself, out of pg_statistic_ext_data.
 */
extern List *GetRelationExtStatistics(Relation relation);
extern char *GetExtStatisticsName(Oid statOid);
extern List *GetExtStatisticsKinds(Oid statOid);

/*
 * The size of a partitioned table, summed over its leaves.
 *
 * PostgreSQL has no such function, because its planner never needs one: it
 * plans each partition as a relation of its own and adds the costs up at the
 * Append.  ORCA costs the partitioned table as one object, so it needs the
 * total before it has looked at any leaf.
 */
typedef struct PageEstimate
{
	BlockNumber totalpages;
	BlockNumber totalallvisiblepages;
} PageEstimate;

extern double cdb_estimate_partitioned_numtuples(Relation rel);
extern PageEstimate cdb_estimate_partitioned_numpages(Relation rel);

#endif							/* GP_ORCA_COMPAT_CB_PLANCAT_H */
