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
 * gp_orca_postgis.h
 *	  The rewrite in front of ORCA for PostGIS's indexable functions.
 *
 * See postgis.c.  Called from orca.c, before ORCA is handed a query; the
 * support-function check is also reached from the translator, through
 * gpdb::IsPostgisIndexSupport, and from orca.c's check for a query with no
 * range table.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_POSTGIS_H
#define GP_ORCA_POSTGIS_H

#include "nodes/parsenodes.h"

/* gp.optimizer_postgis_rewrite */
extern bool gp_optimizer_postgis_rewrite;

/* Is `supportfn` PostGIS's index support function? */
extern bool GpOrcaIsPostgisIndexSupport(Oid supportfn);

/* Does anything in `query`, at any level, call a function that has it? */
extern bool GpOrcaQueryCallsPostgisIndexable(Query *query);

/*
 * Add to `query`, in place, the index conditions PostGIS's support function
 * gives for the calls the planner would ask it about.
 */
extern void GpOrcaPostgisRewrite(Query *query);

#endif							/* GP_ORCA_POSTGIS_H */
