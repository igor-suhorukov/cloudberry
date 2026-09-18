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
 * gp_matview.h
 *	  Incremental materialized views, shared between this module's files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_MATVIEW_H
#define GP_MATVIEW_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "utils/rel.h"
#include "tcop/utility.h"

/*
 * The columns the rewrite adds carry this prefix, and O28 keeps them out of
 * "*".  Cloudberry uses the same names, so a view created there and one
 * created here have the same shape.
 */
#define GP_IVM_PREFIX		"__ivm_"
#define GP_IVM_COUNT_COL	"__ivm_count__"

#define IsIvmColumn(name)  (strncmp((name), GP_IVM_PREFIX, strlen(GP_IVM_PREFIX)) == 0)

/* The option that asks for one: CREATE MATERIALIZED VIEW ... WITH (gp.incremental) */
#define GP_IVM_OPTION		"gp.incremental"

/* ivm_create.c */
extern bool GpIvmTakeOption(List **options);
extern void GpIvmCheckQuery(Query *query);
extern Query *GpIvmRewriteQuery(Query *query, List *colNames);
extern void GpIvmAfterCreate(Oid matviewOid, Query *rewritten);
extern bool GpIvmIsIncremental(Oid matviewOid);
extern char *ivm_companion_name(const char *resname);

/* ivm_maintain.c */
extern void GpIvmRefresh(Oid matviewOid);

/* ivm_delta.c */
struct TriggerData;
extern Query *GpIvmGetViewQuery(Relation matviewRel);
extern bool GpIvmDeltaSupported(Query *viewQuery, Oid baseRelid, int *rti);
extern bool GpIvmApplyDelta(Oid matviewOid, Oid baseRelid,
							struct TriggerData *trigdata);

#endif							/* GP_MATVIEW_H */
