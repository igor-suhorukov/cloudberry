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

/*
 * Where a view is maintained from; see GpIvmSite().  On a cluster the
 * triggers keep what they are handed, and the coordinator maintains the view
 * once the statement is over (ivm_cluster.c).
 */
typedef enum IvmSite
{
	IVM_SITE_ONE_NODE,			/* here, by the statement's triggers */
	IVM_SITE_CLUSTER,			/* by the coordinator, from what each node kept */
	IVM_SITE_NONE,				/* not at all: a utility session on a node */
} IvmSite;

/* What GpIvmAsOwnerBegin() changed, for GpIvmAsOwnerEnd() to put back. */
typedef struct GpIvmOwnerState
{
	Oid			userid;
	int			sec_context;
	int			nestlevel;
} GpIvmOwnerState;

/*
 * One step of a view's maintenance: the rows it loses and the rows it gains
 * from one place's change, either NULL where there are none, in the view's
 * columns, handed to whoever applies them (GpIvmComputeDeltas()).
 */
typedef void (*IvmDeltaApplier) (Relation matviewRel,
								 struct Tuplestorestate *old_rows,
								 struct Tuplestorestate *new_rows,
								 TupleDesc desc, void *arg);

/* ivm_create.c */
extern bool GpIvmTakeOption(List **options);
extern void GpIvmCheckQuery(Query *query);
extern Query *GpIvmRewriteQuery(Query *query, List *colNames);
extern void GpIvmAfterCreate(Oid matviewOid, Query *rewritten);
extern void GpIvmRestored(Oid matviewOid);
extern bool GpIvmIsIncremental(Oid matviewOid);
extern char *ivm_companion_name(const char *kind, const char *resname);

/* dynamic.c */
extern bool GpDynTakeOption(List **options, char **schedule);
extern void GpDynAfterCreate(Oid matviewOid, const char *schedule);
extern void GpDynRestored(Oid matviewOid);
extern void GpDynDropped(Oid matviewOid);

/* ivm_state.c */
extern void GpIvmEntryBefore(Oid matviewOid, bool kept);
extern IvmEntry *GpIvmEntryAfter(Oid matviewOid, struct TriggerData *trigdata,
								 bool kept, bool *is_last);
extern IvmEntry *GpIvmEntryMake(Oid matviewOid);
extern IvmModifiedTable *GpIvmEntryTable(IvmEntry *entry, Relation rel);
extern IvmTransition *GpIvmEntryAddTransition(IvmEntry *entry,
											  IvmModifiedTable *table,
											  struct Tuplestorestate *store,
											  bool old, bool owned);
extern IvmEntry *GpIvmEntryTake(Oid matviewOid);
extern List *GpIvmEntryViews(void);
extern IvmModifiedTable *GpIvmFindTable(IvmEntry *entry, Oid relid);
extern void GpIvmEntryForget(IvmEntry *entry);

/* ivm_maintain.c */
extern void GpIvmRefresh(Oid matviewOid);
extern IvmSite GpIvmSite(void);
extern void GpIvmCount(bool by_delta);
extern void GpIvmAsOwnerBegin(Oid matviewOid, GpIvmOwnerState *state);
extern void GpIvmAsOwnerEnd(GpIvmOwnerState *state);

/* ivm_delta.c */
extern Query *GpIvmGetViewQuery(Relation matviewRel);
extern bool GpIvmApplyDelta(IvmEntry *entry);
extern bool GpIvmDeltaSupported(IvmEntry *entry, Relation matviewRel,
								bool cluster);
extern bool GpIvmComputeDeltas(IvmEntry *entry, Relation matviewRel,
							   bool cluster, IvmDeltaApplier apply, void *arg);
extern void GpIvmApplyStaged(Relation matviewRel,
							 struct Tuplestorestate *old_rows,
							 struct Tuplestorestate *new_rows, bool replace);
extern struct Tuplestorestate *GpIvmRunQuery(Query *query,
											 struct QueryEnvironment *queryEnv,
											 TupleDesc *tupdesc_out,
											 double *ntuples_out);

/* ivm_cluster.c */
struct PlannedStmt;
extern void GpIvmClusterStatementEnd(struct PlannedStmt *stmt);
extern void GpIvmClusterUtilityEnd(Oid relid);
extern bool GpIvmClusterMaintaining(void);
extern char *GpIvmClusterDefaultDistribution(Query *rewritten, List *colNames);
extern void GpIvmClusterCheckDistribution(Oid matviewOid, Query *rewritten);
extern void GpIvmClusterMakeTriggers(Oid matviewOid);

#endif							/* GP_MATVIEW_H */
