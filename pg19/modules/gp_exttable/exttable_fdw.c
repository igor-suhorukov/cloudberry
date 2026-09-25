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
 * exttable_fdw.c
 *	  gp_exttable_fdw, the foreign data wrapper an external table is a
 *	  foreign table of, and pg_exttable(), the catalog Cloudberry had.
 *
 * Where it runs.  A readable external table that is read on the segments
 * carries the "random" distribution in its gp label (exttable_ddl.c), and so
 * is gathered as a distributed table is: by the planner, which sends each
 * segment the query of the table (gp_scan.c), whose own planning reaches
 * this wrapper there; and by ORCA, whose plan sends the ForeignScan this
 * wrapper made on the coordinator.  Which location a segment reads is chosen
 * alike wherever it is chosen (external.c).  A writable one carries the
 * distribution its DISTRIBUTED clause says, and gp_core routes an INSERT's
 * rows to the segments by it, each segment writing its share.
 *
 * Cloudberry sources this file is made of:
 *	  gpcontrib/gp_exttable_fdw/gp_exttable_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "partitioning/partdesc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "gp_exttable.h"
#include "gp_policy.h"

#define GP_EXTTABLE_ATTRNUM 12

PG_FUNCTION_INFO_V1(gp_exttable_fdw_handler);
PG_FUNCTION_INFO_V1(pg_exttable);

typedef struct exttable_fdw_state
{
	FileScanDesc ess_ScanDesc;
	ExternalSelectDesc externalSelectDesc;
	bool		hasConstraints;
	bool		isPartition;
	ExprState **constraintExprs;
	ExprState  *partitionCheckExpr;
} exttable_fdw_state;

static void cost_externalscan(ForeignPath *path, PlannerInfo *root,
							  RelOptInfo *baserel, ParamPathInfo *param_info);

static Datum
strListToArray(List *stringlist)
{
	ArrayBuildState *astate = NULL;
	ListCell   *cell;

	foreach(cell, stringlist)
		astate = accumArrayResult(astate, CStringGetTextDatum(strVal(lfirst(cell))),
								  false, TEXTOID, CurrentMemoryContext);
	if (astate)
		return makeArrayResult(astate, CurrentMemoryContext);
	return PointerGetDatum(NULL);
}

/*
 * The format options as the pg_exttable catalog's fmtopts column had them:
 * delimiter '|' null '' ...
 */
static Datum
formatOptionsToTextDatum(List *options, char formattype)
{
	StringInfoData cfbuf;
	bool		isfirst = true;
	Datum		result;

	initStringInfo(&cfbuf);
	foreach_node(DefElem, defel, options)
	{
		char	   *key = defel->defname;
		char	   *val = (char *) defGetString(defel);

		if ((fmttype_is_text(formattype) || fmttype_is_csv(formattype)) &&
			strcmp(key, "format") == 0)
			continue;

		if (isfirst)
			isfirst = false;
		else
			appendStringInfoChar(&cfbuf, ' ');

		if (fmttype_is_text(formattype) || fmttype_is_csv(formattype))
		{
			if (strcmp(key, "header") == 0)
				appendStringInfoString(&cfbuf, "header");
			else if (strcmp(key, "fill_missing_fields") == 0)
				appendStringInfoString(&cfbuf, "fill missing fields");
			else if (strcmp(key, "force_not_null") == 0)
				appendStringInfo(&cfbuf, "force not null %s", val);
			else if (strcmp(key, "force_quote") == 0)
				appendStringInfo(&cfbuf, "force quote %s", val);
			else
				appendStringInfo(&cfbuf, "%s '%s'", key, val);
		}
		else
			appendStringInfo(&cfbuf, "%s '%s'", key, val);
	}
	result = CStringGetTextDatum(cfbuf.data);
	pfree(cfbuf.data);
	return result;
}

/*
 * pg_exttable(): Cloudberry's catalog of external tables, which it keeps as
 * a view over its foreign tables since external tables became one, made
 * from their options.
 */
Datum
pg_exttable(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	ftrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			extserver;

	InitMaterializedSRF(fcinfo, 0);

	extserver = get_foreign_server_oid(GP_EXTTABLE_SERVER_NAME, true);
	if (!OidIsValid(extserver))
		return (Datum) 0;

	ftrel = table_open(ForeignTableRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_foreign_table_ftserver, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(extserver));
	scan = systable_beginscan(ftrel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_foreign_table ft = (Form_pg_foreign_table) GETSTRUCT(tuple);
		Datum		values[GP_EXTTABLE_ATTRNUM];
		bool		nulls[GP_EXTTABLE_ATTRNUM];
		List	   *ftoptions = NIL;
		ExtTableEntry *extentry;
		Datum		datum;
		bool		isnull;

		datum = heap_getattr(tuple, Anum_pg_foreign_table_ftoptions,
							 RelationGetDescr(ftrel), &isnull);
		if (!isnull)
			ftoptions = untransformRelOptions(datum);
		extentry = GetExtFromForeignTableOptions(ftoptions, ft->ftrelid);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = ObjectIdGetDatum(ft->ftrelid);
		datum = strListToArray(extentry->urilocations);
		if (DatumGetPointer(datum) != NULL)
			values[1] = datum;
		else
			nulls[1] = true;
		datum = strListToArray(extentry->execlocations);
		if (DatumGetPointer(datum) != NULL)
			values[2] = datum;
		else
			nulls[2] = true;
		values[3] = CharGetDatum(extentry->fmtcode);
		if (extentry->options)
			values[4] = formatOptionsToTextDatum(extentry->options, extentry->fmtcode);
		else
			nulls[4] = true;
		if (IS_LOG_ERRORS_PERSISTENTLY(extentry->logerrors))
			values[5] = strListToArray(list_make1(makeString("error_log_persistent=true")));
		else
			nulls[5] = true;
		if (extentry->command)
			values[6] = CStringGetTextDatum(extentry->command);
		else
			nulls[6] = true;
		values[7] = Int32GetDatum(extentry->rejectlimit);
		nulls[7] = (extentry->rejectlimit == -1);
		values[8] = CharGetDatum(extentry->rejectlimittype);
		nulls[8] = (extentry->rejectlimittype == -1);
		values[9] = BoolGetDatum(IS_LOG_TO_FILE(extentry->logerrors));
		values[10] = Int32GetDatum(extentry->encoding);
		values[11] = BoolGetDatum(extentry->iswritable);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	systable_endscan(scan);
	table_close(ftrel, AccessShareLock);

	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

static void
exttable_GetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
						   Oid foreigntableid)
{
	/* never analyzed: Cloudberry's size of one (cdb_estimate_rel_size()) */
	if (baserel->pages == 0)
	{
		baserel->pages = GP_EXTERNAL_TABLE_DEFAULT_PAGES;
		baserel->tuples = GP_EXTERNAL_TABLE_DEFAULT_TUPLES;
	}
	set_baserel_size_estimates(root, baserel);
}

static void
exttable_GetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
						 Oid foreigntableid)
{
	ForeignPath *pathnode;
	ExternalScanInfo *info;
	ExtTableEntry *extEntry = GetExtTableEntry(foreigntableid);

	info = MakeExternalScanInfo(extEntry, foreigntableid);

	pathnode = create_foreignscan_path(root, baserel, NULL, 0, 0, 0, 0, NIL,
									   NULL, NULL, NIL,
									   ExternalScanInfoToList(info));
	cost_externalscan(pathnode, root, baserel, pathnode->path.param_info);
	add_path(baserel, (Path *) pathnode);
}

static ForeignScan *
exttable_GetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
						Oid foreigntableid, ForeignPath *best_path,
						List *tlist, List *scan_clauses, Plan *outer_plan)
{
	Index		scan_relid = best_path->path.parent->relid;

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	return make_foreignscan(tlist, scan_clauses, scan_relid, NIL,
							best_path->fdw_private, NIL, NIL, NULL);
}

static void
cost_externalscan(ForeignPath *path, PlannerInfo *root,
				  RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = baserel->rows;

	run_cost += seq_page_cost * baserel->pages;
	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}

/* ------------------------------------------------------------------------- */
/* Scans                                                                     */
/* ------------------------------------------------------------------------- */

static void
exttable_BeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *scan = (ForeignScan *) node->ss.ps.plan;
	ExternalScanInfo *info = ExternalScanInfoFromList(scan->fdw_private);
	Relation	rel = node->ss.ss_currentRelation;
	exttable_fdw_state *fdw_state;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	fdw_state = palloc0(sizeof(exttable_fdw_state));
	fdw_state->ess_ScanDesc = external_beginscan(rel, info->scancounter,
												 info->uriList, info->fmtType,
												 info->isMasterOnly,
												 info->rejLimit,
												 info->rejLimitInRows,
												 info->logErrors,
												 info->encoding,
												 info->extOptions);
	fdw_state->externalSelectDesc = external_getnext_init(&node->ss.ps);
	fdw_state->hasConstraints = (rel->rd_att->constr != NULL &&
								 rel->rd_att->constr->num_check > 0);
	fdw_state->isPartition = rel->rd_rel->relispartition;
	node->fdw_state = fdw_state;
}

/* The table's CHECK constraints, which Cloudberry's scan skips a row by. */
static bool
ExternalConstraintCheck(TupleTableSlot *slot, exttable_fdw_state *st,
						Relation rel, EState *estate)
{
	TupleConstr *constr = rel->rd_att->constr;
	ExprContext *econtext;

	if (constr == NULL || constr->num_check == 0)
		return true;

	if (st->constraintExprs == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);

		st->constraintExprs = palloc(constr->num_check * sizeof(ExprState *));
		for (int i = 0; i < constr->num_check; i++)
		{
			List	   *qual = make_ands_implicit(stringToNode(constr->check[i].ccbin));

			st->constraintExprs[i] = ExecPrepareExpr((Expr *) qual, estate);
		}
		MemoryContextSwitchTo(old);
	}

	econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = slot;
	for (int i = 0; i < constr->num_check; i++)
		if (!ExecCheck(st->constraintExprs[i], econtext))
			return false;
	return true;
}

/* A partition's bound, which the rows of a partition read are held to. */
static bool
ExternalPartitionCheck(TupleTableSlot *slot, exttable_fdw_state *st,
					   Relation rel, EState *estate)
{
	ExprContext *econtext;

	if (st->partitionCheckExpr == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);

		st->partitionCheckExpr = ExecPrepareCheck(RelationGetPartitionQual(rel),
												  estate);
		MemoryContextSwitchTo(old);
	}
	econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = slot;
	return ExecCheck(st->partitionCheckExpr, econtext);
}

static TupleTableSlot *
exttable_IterateForeignScan(ForeignScanState *node)
{
	EState	   *estate = node->ss.ps.state;
	exttable_fdw_state *st = (exttable_fdw_state *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	Relation	rel = node->ss.ss_currentRelation;
	MemoryContext oldcxt;

	/*
	 * What the scan keeps between rows lives in the query's context; the row
	 * itself is the slot's.
	 */
	oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	for (;;)
	{
		HeapTuple	tuple = external_getnext(st->ess_ScanDesc,
											 ForwardScanDirection,
											 st->externalSelectDesc);

		if (!tuple)
		{
			ExecClearTuple(slot);
			break;
		}
		ExecStoreHeapTuple(tuple, slot, false);

		if (st->isPartition && !ExternalPartitionCheck(slot, st, rel, estate))
			continue;
		if (st->hasConstraints && !ExternalConstraintCheck(slot, st, rel, estate))
			continue;
		break;
	}
	MemoryContextSwitchTo(oldcxt);

	return slot;
}

static void
exttable_ReScanForeignScan(ForeignScanState *node)
{
	exttable_fdw_state *st = (exttable_fdw_state *) node->fdw_state;

	external_rescan(st->ess_ScanDesc);
}

static void
exttable_EndForeignScan(ForeignScanState *node)
{
	exttable_fdw_state *st = (exttable_fdw_state *) node->fdw_state;

	if (st == NULL)
		return;
	external_endscan(st->ess_ScanDesc);
}

/* ------------------------------------------------------------------------- */
/* Writes                                                                    */
/* ------------------------------------------------------------------------- */

static int
exttable_IsForeignRelUpdatable(Relation rel)
{
	ExtTableEntry *extentry = GetExtTableEntry(RelationGetRelid(rel));

	return extentry->iswritable ? (1 << CMD_INSERT) : 0;
}

static void
exttable_BeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
							List *fdw_private, int subplan_index, int eflags)
{
	/*
	 * The location is opened with the first row, so that a node that writes
	 * none -- the coordinator, whose rows go to the segments -- opens none.
	 */
}

static TupleTableSlot *
exttable_ExecForeignInsert(EState *estate, ResultRelInfo *rinfo,
						   TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	ExternalInsertDesc extInsertDesc = (ExternalInsertDesc) rinfo->ri_FdwState;

	if (!extInsertDesc)
	{
		MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);

		extInsertDesc = external_insert_init(rinfo->ri_RelationDesc);
		rinfo->ri_FdwState = extInsertDesc;
		MemoryContextSwitchTo(old);
	}

	external_insert(extInsertDesc, slot);
	return slot;
}

static void
exttable_EndForeignModify(EState *estate, ResultRelInfo *rinfo)
{
	ExternalInsertDesc extInsertDesc = (ExternalInsertDesc) rinfo->ri_FdwState;

	if (extInsertDesc != NULL)
		external_insert_finish(extInsertDesc);
}

static void
exttable_BeginForeignInsert(ModifyTableState *mtstate, ResultRelInfo *rinfo)
{
	/* as BeginForeignModify: the location with the first row */
}

static void
exttable_EndForeignInsert(EState *estate, ResultRelInfo *rinfo)
{
	ExternalInsertDesc extInsertDesc = (ExternalInsertDesc) rinfo->ri_FdwState;

	if (extInsertDesc != NULL)
		external_insert_finish(extInsertDesc);
}

Datum
gp_exttable_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = exttable_GetForeignRelSize;
	routine->GetForeignPaths = exttable_GetForeignPaths;
	routine->GetForeignPlan = exttable_GetForeignPlan;
	routine->BeginForeignScan = exttable_BeginForeignScan;
	routine->IterateForeignScan = exttable_IterateForeignScan;
	routine->ReScanForeignScan = exttable_ReScanForeignScan;
	routine->EndForeignScan = exttable_EndForeignScan;

	routine->IsForeignRelUpdatable = exttable_IsForeignRelUpdatable;
	routine->BeginForeignModify = exttable_BeginForeignModify;
	routine->ExecForeignInsert = exttable_ExecForeignInsert;
	routine->EndForeignModify = exttable_EndForeignModify;
	routine->BeginForeignInsert = exttable_BeginForeignInsert;
	routine->EndForeignInsert = exttable_EndForeignInsert;

	PG_RETURN_POINTER(routine);
}
