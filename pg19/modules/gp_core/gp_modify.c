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
 * gp_modify.c
 *	  Writing a distributed table when PostgreSQL's planner plans.
 *
 * INSERT.  The coordinator runs the INSERT's own plan -- the VALUES, or the
 * SELECT, with every default and every nextval() already evaluated, so that
 * a serial column has one sequence -- and hashes each row it produces to the
 * segment its distribution key names: Cloudberry's cdbhash, kept exactly
 * (gp_hash.c).  The rows travel by COPY ... FROM STDIN, in binary where every
 * column can, and each segment's own COPY checks the constraints and fires
 * the row triggers where the row lands, as Cloudberry's segments do.  They are
 * held on the coordinator until the plan that made them is finished, because
 * that plan may be reading from the same segments over the same connections.
 * COPY FROM routes its rows the same way, parsed by PostgreSQL's own COPY.
 *
 * UPDATE and DELETE.  A statement whose rows are all on the segment where the
 * row it changes is -- it reads the table it changes, and nothing else but
 * replicated tables and values -- is sent to every segment as it stands,
 * deparsed by PostgreSQL's own ruleutils, and each segment changes its own
 * rows; the counts are added up.  What that cannot do -- move a row to another
 * segment because its key changed, join another distributed table -- needs a
 * Motion between segments, which is ORCA's distributed plans, and is refused
 * until then with the reason.
 *
 * SELECT ... FOR UPDATE.  Cloudberry, without its global deadlock detector,
 * takes an ExclusiveLock on the table rather than locking rows; the port does
 * the same, which the rows it gathers, having no place on the coordinator,
 * could not be locked by anyway.
 *
 * Cloudberry sources this file stands in for:
 *	  the Redistribute Motion under an INSERT (cdbpath.c,
 *	  cdbpath_motion_for_insert), cdbcopy.c's COPY dispatch, and the UPDATE
 *	  and DELETE plans cdbllize.c makes for a single distributed table
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_policy.h"
#include "gp_scan.h"

/* How much of a COPY's data is sent to libpq at a time. */
#define ROUTE_CHUNK		65536

static planner_hook_type prev_planner = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ------------------------------------------------------------------------- */
/* The router: rows to the segments their keys name                          */
/* ------------------------------------------------------------------------- */

typedef struct GpRouter
{
	Relation	rel;
	GpHash	   *hash;
	int			nsegs;
	bool		replicated;
	bool		binary;
	Tuplestorestate **stores;	/* one per segment; one only when replicated */
	TupleTableSlot *slot;
	FmgrInfo   *out;			/* each column's send or output function */
	char	   *copy_sql;
	uint64		nrows;
} GpRouter;

static GpRouter *
router_begin(Relation rel, GpPolicy *policy)
{
	GpRouter   *r = (GpRouter *) palloc0(sizeof(GpRouter));
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData sql;
	bool		first = true;

	r->rel = rel;
	r->hash = GpHashMake(policy, tupdesc);
	GpClusterSegments(&r->nsegs);
	r->replicated = GpPolicyIsReplicated(policy);
	r->binary = GpTupleDescHasBinaryIO(tupdesc);
	r->stores = palloc0_array(Tuplestorestate *, r->nsegs);
	r->slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);
	r->out = palloc0_array(FmgrInfo, tupdesc->natts);

	/*
	 * The columns the segment is given: all but the dropped ones and the
	 * generated ones, which the segment's own COPY computes.
	 */
	initStringInfo(&sql);
	appendStringInfo(&sql, "COPY %s (",
					 GpDispatchRelationName(RelationGetRelid(rel)));
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			func;
		bool		isvarlena;

		if (att->attisdropped || att->attgenerated != '\0')
			continue;
		appendStringInfo(&sql, "%s%s", first ? "" : ", ",
						 quote_identifier(NameStr(att->attname)));
		first = false;

		if (r->binary)
			getTypeBinaryOutputInfo(att->atttypid, &func, &isvarlena);
		else
			getTypeOutputInfo(att->atttypid, &func, &isvarlena);
		fmgr_info(func, &r->out[i]);
	}
	appendStringInfo(&sql, ") FROM STDIN%s", r->binary ? " (FORMAT binary)" : "");
	r->copy_sql = sql.data;

	return r;
}

/* One row, whose values are the relation's columns in order. */
static void
router_put(GpRouter *r, TupleTableSlot *slot)
{
	int			seg;

	slot_getallattrs(slot);
	seg = GpHashSegment(r->hash, slot->tts_values, slot->tts_isnull);
	if (seg == GP_HASH_ALL_SEGMENTS)
		seg = 0;				/* one copy, sent to every segment */

	if (r->stores[seg] == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(r));

		r->stores[seg] = tuplestore_begin_heap(false, false, work_mem);
		MemoryContextSwitchTo(oldcxt);
	}
	tuplestore_putvalues(r->stores[seg], RelationGetDescr(r->rel),
						 slot->tts_values, slot->tts_isnull);
	r->nrows++;
}

/* COPY's text format for one value: backslash and the separators escaped. */
static void
copy_text_escape(StringInfo buf, const char *s)
{
	for (; *s; s++)
	{
		switch (*s)
		{
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			default:
				appendStringInfoChar(buf, *s);
				break;
		}
	}
}

/* One row, as COPY reads it, onto the end of buf. */
static void
router_encode(GpRouter *r, TupleTableSlot *slot, StringInfo buf)
{
	TupleDesc	tupdesc = RelationGetDescr(r->rel);
	int			nfields = 0;
	bool		first = true;

	slot_getallattrs(slot);

	if (r->binary)
	{
		for (int i = 0; i < tupdesc->natts; i++)
			if (!TupleDescAttr(tupdesc, i)->attisdropped &&
				TupleDescAttr(tupdesc, i)->attgenerated == '\0')
				nfields++;
		pq_sendint16(buf, nfields);
	}

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped || att->attgenerated != '\0')
			continue;

		if (r->binary)
		{
			if (slot->tts_isnull[i])
				pq_sendint32(buf, -1);
			else
			{
				bytea	   *out = SendFunctionCall(&r->out[i], slot->tts_values[i]);

				pq_sendint32(buf, VARSIZE(out) - VARHDRSZ);
				appendBinaryStringInfo(buf, VARDATA(out), VARSIZE(out) - VARHDRSZ);
			}
		}
		else
		{
			if (!first)
				appendStringInfoChar(buf, '\t');
			if (slot->tts_isnull[i])
				appendStringInfoString(buf, "\\N");
			else
				copy_text_escape(buf, OutputFunctionCall(&r->out[i],
														 slot->tts_values[i]));
		}
		first = false;
	}

	if (!r->binary)
		appendStringInfoChar(buf, '\n');
}

/* Send one segment's rows, and answer how many it took. */
static uint64
router_send(GpRouter *r, Tuplestorestate *store, int content)
{
	StringInfoData buf;
	MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext,
												 "gp_core routed row",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldcxt;

	GpCopyInBegin(content, r->copy_sql);

	initStringInfo(&buf);
	if (r->binary)
	{
		/* PGCOPY\n\377\r\n\0, no flags, no header extension */
		appendBinaryStringInfo(&buf, "PGCOPY\n\377\r\n\0", 11);
		pq_sendint32(&buf, 0);
		pq_sendint32(&buf, 0);
	}

	tuplestore_rescan(store);
	while (tuplestore_gettupleslot(store, true, false, r->slot))
	{
		oldcxt = MemoryContextSwitchTo(rowcxt);
		router_encode(r, r->slot, &buf);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(rowcxt);

		if (buf.len >= ROUTE_CHUNK)
		{
			GpCopyInData(buf.data, buf.len);
			resetStringInfo(&buf);
		}
		CHECK_FOR_INTERRUPTS();
	}

	if (r->binary)
		pq_sendint16(&buf, -1);
	if (buf.len > 0)
		GpCopyInData(buf.data, buf.len);

	pfree(buf.data);
	MemoryContextDelete(rowcxt);
	return GpCopyInEnd();
}

/* Send every segment its rows; answer how many rows the statement wrote. */
static uint64
router_finish(GpRouter *r)
{
	if (r->replicated)
	{
		/* Every segment takes every row; the statement wrote each once. */
		if (r->stores[0] != NULL)
			for (int seg = 0; seg < r->nsegs; seg++)
				(void) router_send(r, r->stores[0], seg);
		return r->nrows;
	}

	for (int seg = 0; seg < r->nsegs; seg++)
	{
		if (r->stores[seg] != NULL)
		{
			uint64		took = router_send(r, r->stores[seg], seg);

			if (took != (uint64) tuplestore_tuple_count(r->stores[seg]))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("segment %d took %llu of the %lld rows sent to it",
								seg, (unsigned long long) took,
								(long long) tuplestore_tuple_count(r->stores[seg]))));
		}
	}
	return r->nrows;
}

static void
router_end(GpRouter *r)
{
	for (int seg = 0; seg < r->nsegs; seg++)
		if (r->stores[seg] != NULL)
			tuplestore_end(r->stores[seg]);
	ExecDropSingleTupleTableSlot(r->slot);
}

/* ------------------------------------------------------------------------- */
/* INSERT                                                                    */
/* ------------------------------------------------------------------------- */

static Node *insert_create_state(CustomScan *cscan);
static void insert_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *insert_exec(CustomScanState *node);
static void insert_end(CustomScanState *node);
static void insert_rescan(CustomScanState *node);

static const CustomScanMethods insert_scan_methods = {
	.CustomName = "Redistribute Motion",
	.CreateCustomScanState = insert_create_state,
};

static const CustomExecMethods insert_exec_methods = {
	.CustomName = "Redistribute Motion",
	.BeginCustomScan = insert_begin,
	.ExecCustomScan = insert_exec,
	.EndCustomScan = insert_end,
	.ReScanCustomScan = insert_rescan,
};

typedef struct InsertState
{
	CustomScanState css;
	Index		rti;
	Relation	rel;
	GpRouter   *router;
	bool		done;
} InsertState;

static Node *
insert_create_state(CustomScan *cscan)
{
	InsertState *state = (InsertState *) newNode(sizeof(InsertState),
												 T_CustomScanState);

	state->css.methods = &insert_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	state->rti = intVal(linitial(cscan->custom_private));
	return (Node *) state;
}

static void
insert_begin(CustomScanState *node, EState *estate, int eflags)
{
	InsertState *state = (InsertState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Oid			relid = exec_rt_fetch(state->rti, estate)->relid;

	outerPlanState(node) = ExecInitNode(linitial(cscan->custom_plans),
										estate, eflags);

	state->rel = table_open(relid, NoLock);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		state->router = router_begin(state->rel,
									 GpScanDistributedPolicy(relid));
}

static TupleTableSlot *
insert_exec(CustomScanState *node)
{
	InsertState *state = (InsertState *) node;
	PlanState  *child = outerPlanState(node);
	EState	   *estate = node->ss.ps.state;

	if (state->done)
		return NULL;

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);

		if (TupIsNull(slot))
			break;
		router_put(state->router, slot);
		ResetPerTupleExprContext(estate);
	}

	/*
	 * The plan above is finished with the segments now, so the connections
	 * are free for the rows it made.
	 */
	estate->es_processed += router_finish(state->router);
	state->done = true;
	return NULL;
}

static void
insert_end(CustomScanState *node)
{
	InsertState *state = (InsertState *) node;

	ExecEndNode(outerPlanState(node));
	if (state->router != NULL)
		router_end(state->router);
	table_close(state->rel, NoLock);
}

static void
insert_rescan(CustomScanState *node)
{
	elog(ERROR, "a distributed INSERT cannot be rescanned");
}

/* ------------------------------------------------------------------------- */
/* UPDATE and DELETE                                                         */
/* ------------------------------------------------------------------------- */

static Node *modify_create_state(CustomScan *cscan);
static void modify_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *modify_exec(CustomScanState *node);
static void modify_end(CustomScanState *node);
static void modify_rescan(CustomScanState *node);
static void modify_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomScanMethods modify_scan_methods = {
	.CustomName = "Dispatch",
	.CreateCustomScanState = modify_create_state,
};

static const CustomExecMethods modify_exec_methods = {
	.CustomName = "Dispatch",
	.BeginCustomScan = modify_begin,
	.ExecCustomScan = modify_exec,
	.EndCustomScan = modify_end,
	.ReScanCustomScan = modify_rescan,
	.ExplainCustomScan = modify_explain,
};

typedef struct ModifyState
{
	CustomScanState css;
	char	   *sql;
	bool		replicated;
	bool		done;
} ModifyState;

static Node *
modify_create_state(CustomScan *cscan)
{
	ModifyState *state = (ModifyState *) newNode(sizeof(ModifyState),
												 T_CustomScanState);

	state->css.methods = &modify_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
	return (Node *) state;
}

static void
modify_begin(CustomScanState *node, EState *estate, int eflags)
{
}

static TupleTableSlot *
modify_exec(CustomScanState *node)
{
	ModifyState *state = (ModifyState *) node;
	EState	   *estate = node->ss.ps.state;
	ParamListInfo params = estate->es_param_list_info;
	int			nparams = params ? params->numParams : 0;
	const char **values = NULL;
	uint64	   *counts;
	int			nsegs;

	if (state->done)
		return NULL;

	/* The statement's parameters, as text, as the segments' $n read them. */
	if (nparams > 0)
	{
		values = palloc0_array(const char *, nparams);
		for (int i = 0; i < nparams; i++)
		{
			ParamExternData *prm;
			ParamExternData prmdata;

			if (params->paramFetch != NULL)
				prm = params->paramFetch(params, i + 1, false, &prmdata);
			else
				prm = &params->params[i];

			if (!prm->isnull && OidIsValid(prm->ptype))
			{
				Oid			typout;
				bool		isvarlena;

				getTypeOutputInfo(prm->ptype, &typout, &isvarlena);
				values[i] = OidOutputFunctionCall(typout, prm->value);
			}
		}
	}

	GpClusterSegments(&nsegs);
	counts = palloc0_array(uint64, nsegs);
	GpDispatchCommandParams(state->sql, nparams, values, -1, counts);

	/* Every segment changed its own rows; a replicated table's once each. */
	if (state->replicated)
		estate->es_processed += counts[0];
	else
		for (int i = 0; i < nsegs; i++)
			estate->es_processed += counts[i];

	state->done = true;
	return NULL;
}

static void
modify_end(CustomScanState *node)
{
}

static void
modify_rescan(CustomScanState *node)
{
	((ModifyState *) node)->done = false;
}

static void
modify_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	if (es->verbose)
		ExplainPropertyText("Remote SQL", ((ModifyState *) node)->sql, es);
}

/*
 * Can every row this statement reads be found on the segment of the row it
 * changes?  Only if every table it reads is the one it changes or one that
 * every segment holds whole.  Returns NULL if so, or why not.
 */
typedef struct PushContext
{
	Oid			target;
	const char *why;
} PushContext;

static bool
push_walker(Node *node, PushContext *cxt)
{
	if (node == NULL || cxt->why != NULL)
		return false;

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/*
		 * Row-level security and security-barrier views reach the planner as
		 * conditions on the range table entry, which the deparsed statement
		 * would not carry: the segment would change rows the user may not.
		 */
		if (rte->securityQuals != NIL)
		{
			cxt->why = "It reads through row-level security or a security-barrier view, whose conditions do not travel with the statement.";
			return false;
		}

		if (rte->rtekind == RTE_RELATION && rte->relid != cxt->target)
		{
			GpPolicy   *policy = GpScanDistributedPolicy(rte->relid);

			if (policy == NULL)
				cxt->why = psprintf("It reads \"%s\", whose rows are on the coordinator.",
									get_rel_name(rte->relid));
			else if (!GpPolicyIsReplicated(policy))
				cxt->why = psprintf("It reads \"%s\", another distributed table.",
									get_rel_name(rte->relid));
		}
		return false;
	}

	if (IsA(node, NextValueExpr))
	{
		cxt->why = "It takes a sequence's next value, and the sequence is the coordinator's.";
		return true;
	}
	if (IsA(node, FuncExpr))
	{
		Oid			f = ((FuncExpr *) node)->funcid;

		if (f == F_NEXTVAL || f == F_CURRVAL || f == F_SETVAL_REGCLASS_INT8 ||
			f == F_SETVAL_REGCLASS_INT8_BOOL || f == F_LASTVAL)
		{
			cxt->why = "It uses a sequence, and the sequence is the coordinator's.";
			return true;
		}
	}
	if (IsA(node, CurrentOfExpr))
	{
		cxt->why = "WHERE CURRENT OF names a row by the cursor that read it, which is here.";
		return true;
	}

	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;

		if (q->hasModifyingCTE)
		{
			cxt->why = "It has a data-modifying WITH query.";
			return true;
		}
		return query_tree_walker(q, push_walker, cxt, QTW_EXAMINE_RTES_BEFORE);
	}

	return expression_tree_walker(node, push_walker, cxt);
}

static const char *
cannot_push_reason(Query *query, Oid target, GpPolicy *policy)
{
	PushContext cxt = {.target = target,.why = NULL};

	if (query->returningList != NIL)
		return "RETURNING from a distributed table waits for the rows to come back through a Motion.";

	/* A row whose key changes belongs on another segment. */
	if (query->commandType == CMD_UPDATE && GpPolicyIsHashPartitioned(policy))
	{
		ListCell   *lc;

		foreach(lc, query->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (tle->resjunk)
				continue;
			for (int k = 0; k < policy->nattrs; k++)
				if (policy->attrs[k] == tle->resno)
					return psprintf("It changes \"%s\", a column of the distribution key, which would move the row to another segment.",
									get_attname(target, tle->resno, false));
		}
	}

	(void) push_walker((Node *) query, &cxt);
	return cxt.why;
}

/* ------------------------------------------------------------------------- */
/* The planner                                                               */
/* ------------------------------------------------------------------------- */

/*
 * SELECT ... FOR UPDATE of a distributed table: Cloudberry's table lock, and
 * no row marks, which would have a LockRows node lock rows on the coordinator
 * that are not there -- a gathered row has no place in its empty copy.  Every
 * level of the query, because FOR UPDATE may be written in a subquery or a
 * WITH query, and each keeps its own.
 */
static bool
lock_instead_of_row_marks(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;
		ListCell   *lc;

		foreach(lc, q->rowMarks)
		{
			RowMarkClause *rc = lfirst_node(RowMarkClause, lc);
			RangeTblEntry *rte = rt_fetch(rc->rti, q->rtable);

			if (rte->rtekind == RTE_RELATION &&
				GpScanDistributedPolicy(rte->relid) != NULL)
			{
				LockRelationOid(rte->relid, ExclusiveLock);
				q->rowMarks = foreach_delete_current(q->rowMarks, lc);
			}
		}
		return query_tree_walker(q, lock_instead_of_row_marks, context, 0);
	}

	return expression_tree_walker(node, lock_instead_of_row_marks, context);
}

static CustomScan *
make_custom_scan(Plan *replaced, const CustomScanMethods *methods)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.startup_cost = replaced->startup_cost;
	cscan->scan.plan.total_cost = replaced->total_cost;
	cscan->scan.plan.plan_rows = replaced->plan_rows;
	cscan->scan.plan.plan_width = replaced->plan_width;
	cscan->scan.plan.plan_node_id = replaced->plan_node_id;
	cscan->scan.plan.targetlist = NIL;
	cscan->scan.scanrelid = 0;
	cscan->methods = methods;
	return cscan;
}

static PlannedStmt *
gp_modify_planner(Query *parse, const char *query_string, int cursorOptions,
				  ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;
	Query	   *original = NULL;
	ModifyTable *mt;
	RangeTblEntry *rte;
	GpPolicy   *policy;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return prev_planner ? prev_planner(parse, query_string, cursorOptions,
										   boundParams, es)
			: standard_planner(parse, query_string, cursorOptions, boundParams, es);

	(void) lock_instead_of_row_marks((Node *) parse, NULL);

	/* The planner changes the Query; an UPDATE or DELETE may be sent as it was. */
	if (parse->commandType == CMD_UPDATE || parse->commandType == CMD_DELETE)
		original = copyObject(parse);

	stmt = prev_planner ? prev_planner(parse, query_string, cursorOptions,
									   boundParams, es)
		: standard_planner(parse, query_string, cursorOptions, boundParams, es);

	if (!IsA(stmt->planTree, ModifyTable))
		return stmt;
	mt = (ModifyTable *) stmt->planTree;
	if (list_length(mt->resultRelations) != 1)
		return stmt;
	rte = rt_fetch(linitial_int(mt->resultRelations), stmt->rtable);
	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL)
		return stmt;

	if (mt->operation == CMD_INSERT)
	{
		CustomScan *cscan;
		Relation	rel;

		if (mt->onConflictAction != ONCONFLICT_NONE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT ... ON CONFLICT into distributed table \"%s\" is not supported yet",
							get_rel_name(rte->relid))));
		if (mt->returningLists != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT ... RETURNING into distributed table \"%s\" is not supported yet",
							get_rel_name(rte->relid))));
		if (mt->withCheckOptionLists != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT through a view WITH CHECK OPTION or row-level security into distributed table \"%s\" is not supported yet",
							get_rel_name(rte->relid))));

		/*
		 * The segments fire the row triggers, each for its own rows, as
		 * Cloudberry's do.  A statement trigger has no segment to be fired
		 * on once, and firing it on each would fire it once per segment.
		 */
		rel = table_open(rte->relid, NoLock);
		if (rel->trigdesc != NULL &&
			(rel->trigdesc->trig_insert_before_statement ||
			 rel->trigdesc->trig_insert_after_statement))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("statement-level triggers on distributed table \"%s\" are not supported",
							RelationGetRelationName(rel))));
		table_close(rel, NoLock);

		cscan = make_custom_scan(&mt->plan, &insert_scan_methods);
		cscan->custom_plans = list_make1(outerPlan(mt));
		cscan->custom_private = list_make1(makeInteger(linitial_int(mt->resultRelations)));
		stmt->planTree = &cscan->scan.plan;
		return stmt;
	}

	if (mt->operation == CMD_UPDATE || mt->operation == CMD_DELETE)
	{
		CustomScan *cscan;
		const char *why;

		why = cannot_push_reason(original, rte->relid, policy);
		if (why != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot %s distributed table \"%s\" this way yet",
							mt->operation == CMD_UPDATE ? "UPDATE" : "DELETE FROM",
							get_rel_name(rte->relid)),
					 errdetail("%s", why)));

		cscan = make_custom_scan(&mt->plan, &modify_scan_methods);
		cscan->custom_private = list_make2(makeString(pg_get_querydef(original, false)),
										   makeBoolean(GpPolicyIsReplicated(policy)));
		stmt->planTree = &cscan->scan.plan;
		return stmt;
	}

	if (mt->operation == CMD_MERGE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE into distributed table \"%s\" is not supported yet",
						get_rel_name(rte->relid))));

	return stmt;
}

/* ------------------------------------------------------------------------- */
/* COPY                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * COPY t FROM: parsed by PostgreSQL's own COPY, as it would be into a local
 * table, and routed.  The coordinator evaluates the defaults, as for INSERT,
 * so that a serial column has one sequence.
 */
static uint64
copy_from_distributed(ParseState *pstate, CopyStmt *stmt, Relation rel,
					  GpPolicy *policy)
{
	CopyFromState cstate;
	GpRouter   *router;
	TupleTableSlot *slot;
	ExprContext *econtext;
	EState	   *estate = CreateExecutorState();
	uint64		processed;

	cstate = BeginCopyFrom(pstate, rel, NULL, stmt->filename, stmt->is_program,
						   NULL, stmt->attlist, stmt->options);
	router = router_begin(rel, policy);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	econtext = GetPerTupleExprContext(estate);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		ExecClearTuple(slot);

		if (!NextCopyFrom(cstate, econtext, slot->tts_values, slot->tts_isnull))
			break;
		ExecStoreVirtualTuple(slot);
		router_put(router, slot);
	}

	EndCopyFrom(cstate);
	processed = router_finish(router);
	router_end(router);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	return processed;
}

static void
gp_modify_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, CopyStmt) && ((CopyStmt *) parsetree)->relation != NULL &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		CopyStmt   *stmt = (CopyStmt *) parsetree;
		Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);
		GpPolicy   *policy = OidIsValid(relid) ? GpScanDistributedPolicy(relid) : NULL;

		if (policy != NULL && stmt->is_from)
		{
			ParseState *pstate = make_parsestate(NULL);
			Relation	rel;
			uint64		processed;

			pstate->p_sourcetext = queryString;
			pstate->p_queryEnv = queryEnv;

			if (stmt->whereClause != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY FROM ... WHERE into distributed table \"%s\" is not supported yet",
								stmt->relation->relname)));

			/*
			 * DoCopy() would check these; this path does not go through it.
			 * The lock and the privilege are the ones COPY FROM takes.
			 */
			rel = table_open(relid, RowExclusiveLock);
			if (pg_class_aclcheck(relid, GetUserId(), ACL_INSERT) != ACLCHECK_OK)
				aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind),
							   RelationGetRelationName(rel));
			if (stmt->filename != NULL && !has_privs_of_role(GetUserId(),
															 stmt->is_program ? ROLE_PG_EXECUTE_SERVER_PROGRAM : ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file or program")));

			processed = copy_from_distributed(pstate, stmt, rel, policy);
			table_close(rel, NoLock);

			if (qc)
				SetQueryCompletion(qc, CMDTAG_COPY, processed);
			return;
		}

		if (policy != NULL && !stmt->is_from)
		{
			/*
			 * COPY t TO is COPY (SELECT ... FROM t) TO, whose query gathers
			 * the rows like any other.
			 */
			CopyStmt   *copy = copyObject(stmt);
			StringInfoData sql;
			List	   *raw;

			initStringInfo(&sql);
			appendStringInfoString(&sql, "SELECT ");
			if (copy->attlist == NIL)
				appendStringInfoString(&sql, "*");
			else
			{
				ListCell   *lc;

				foreach(lc, copy->attlist)
					appendStringInfo(&sql, "%s%s", lc == list_head(copy->attlist) ? "" : ", ",
									 quote_identifier(strVal(lfirst(lc))));
			}
			/* ONLY, as COPY of a table reads no child of it; a partitioned
			 * table COPY refuses, and its query reads the partitions. */
			appendStringInfo(&sql, " FROM %s%s",
							 get_rel_relkind(relid) == RELKIND_RELATION ? "ONLY " : "",
							 GpDispatchRelationName(relid));

			raw = raw_parser(sql.data, RAW_PARSE_DEFAULT);
			copy->query = linitial_node(RawStmt, raw)->stmt;
			copy->relation = NULL;
			copy->attlist = NIL;

			pstmt = copyObject(pstmt);
			pstmt->utilityStmt = (Node *) copy;
			readOnlyTree = false;
		}
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

void
GpModifyInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&insert_scan_methods);
	RegisterCustomScanMethods(&modify_scan_methods);

	prev_planner = planner_hook;
	planner_hook = gp_modify_planner;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_modify_ProcessUtility;
}
