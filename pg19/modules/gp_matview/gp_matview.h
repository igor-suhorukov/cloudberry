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

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "utils/rel.h"
#include "utils/snapshot.h"
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

/* And the one that asks for a dynamic table; see dynamic.c. */
#define GP_DYN_OPTION		"gp.dynamic_schedule"

/*
 * What one statement leaves for the maintenance that follows it.  ivm_state.c
 * says why this has to be kept, and ivm_delta.c is what reads it.
 */
struct Tuplestorestate;
struct TupleTableSlot;
struct TriggerData;

/* One transition table, under a name of its own. */
typedef struct IvmTransition
{
	struct Tuplestorestate *store;
	char	   *name;			/* what the delta queries call it */
	bool		owned;			/* copied out of the query that made it */
} IvmTransition;

/* One base table this statement changed. */
typedef struct IvmModifiedTable
{
	Oid			relid;
	List	   *old_stores;		/* IvmTransition *: rows it lost */
	List	   *new_stores;		/* IvmTransition *: rows it gained */
	TupleDesc	tupdesc;		/* the table's, copied */
	List	   *rte_indexes;	/* int: where it sits in the view query */
	Relation	rel;			/* opened by the first prestate probe */
	struct TupleTableSlot *slot;
} IvmModifiedTable;

/* One view, between its first BEFORE trigger and its last AFTER trigger. */
typedef struct IvmEntry
{
	Oid			matviewOid;
	Snapshot	snapshot;		/* the tables as the statement found them */
	List	   *tables;			/* IvmModifiedTable * */
	int			before_count;
	int			after_count;
	bool		truncated;		/* a TRUNCATE leaves nothing to delta */
	SubTransactionId subid;		/* the statement's, so an abort drops it */
	MemoryContext cxt;
} IvmEntry;

/* ivm_create.c */
extern bool GpIvmTakeOption(List **options);
extern void GpIvmCheckQuery(Query *query);
extern Query *GpIvmRewriteQuery(Query *query, List *colNames);
extern void GpIvmAfterCreate(Oid matviewOid, Query *rewritten);
extern bool GpIvmIsIncremental(Oid matviewOid);
extern char *ivm_companion_name(const char *kind, const char *resname);

/* dynamic.c */
extern bool GpDynTakeOption(List **options, char **schedule);
extern void GpDynAfterCreate(Oid matviewOid, const char *schedule);
extern void GpDynDropped(Oid matviewOid);

/* ivm_state.c */
extern void GpIvmEntryBefore(Oid matviewOid);
extern IvmEntry *GpIvmEntryAfter(Oid matviewOid, struct TriggerData *trigdata,
								 bool *is_last);
extern IvmModifiedTable *GpIvmFindTable(IvmEntry *entry, Oid relid);
extern void GpIvmEntryForget(IvmEntry *entry);

/* ivm_maintain.c */
extern void GpIvmRefresh(Oid matviewOid);

/* ivm_delta.c */
extern Query *GpIvmGetViewQuery(Relation matviewRel);
extern bool GpIvmApplyDelta(IvmEntry *entry);

#endif							/* GP_MATVIEW_H */
