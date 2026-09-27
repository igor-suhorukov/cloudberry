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
 * rows; the counts are added up.  To the one segment its key names, when its
 * WHERE fixes the key.  What that cannot do -- join another distributed
 * table, change a partitioned table's partitions, run in a WITH query,
 * return rows, change the distribution key -- the coordinator's plan does,
 * and each row it changes is changed on its segment (gp_explicit.c); so is
 * an INSERT with RETURNING, ON CONFLICT or check options, and a MERGE.  A row whose key changes is moved by a Split:
 * deleted where it is, inserted where its new key hashes.  WHERE CURRENT OF
 * is written so too: the cursor's gather says where its row is (gp_scan.c).
 *
 * SELECT ... FOR UPDATE.  Cloudberry, without its global deadlock detector,
 * takes an ExclusiveLock on the table rather than locking rows; the port does
 * the same, which the rows it gathers, having no place on the coordinator,
 * could not be locked by anyway.
 *
 * That ExclusiveLock, and an UPDATE's or DELETE's, is taken where PostgreSQL
 * takes its own lock on the table -- as the parser opens it, or the rewriter
 * brings it in under a view -- rather than after PostgreSQL's weaker mode,
 * which it would upgrade, and two sessions upgrading deadlock (O30,
 * gp_modify_query_lockmode()).  With the deadlock detector on, whether a
 * locking clause locks rows on the segments or the table is decided as the
 * query is planned, so the parser takes AccessShareLock, as Cloudberry's
 * parser does before it decides, and planning the lock it decides on; no
 * lock taken after AccessShareLock is an upgrade that deadlocks.
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
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
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
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_segment.h"
#include "gp_settings.h"
#include "gp_subselect.h"

/* How much of a COPY's data is sent to libpq at a time. */
#define ROUTE_CHUNK		65536

static planner_hook_type prev_planner = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static query_lockmode_hook_type prev_query_lockmode = NULL;

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
	TupleDesc	storedesc;		/* theirs: the table's columns, and a line */
	TupleTableSlot *slot;
	FmgrInfo   *out;			/* each column's send or output function */
	char	   *copy_sql;
	uint64		nrows;

	/*
	 * A COPY's: each row's line in the data, kept after its columns, and
	 * the line of the one a segment failed at (router_error_context()).
	 */
	bool		lines;
	Datum	   *linevalues;
	bool	   *linenulls;
	CopyFromState cstate;
	uint64		error_lineno;

	/*
	 * An INSERT of one row reaches the segment it routes the row to, which
	 * only routing says: the statement's plan, for GpReportDtxReached(), or
	 * NULL where the statement noted its segments as it began.
	 */
	struct PlannedStmt *reached_stmt;
} GpRouter;

/*
 * lines: the rows are a COPY's, each put with its line in the data, which
 * an error a segment raises in it says.
 */
static GpRouter *
router_begin(Relation rel, GpPolicy *policy, bool lines)
{
	GpRouter   *r = (GpRouter *) palloc0(sizeof(GpRouter));
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData sql;
	bool		first = true;

	r->rel = rel;
	r->hash = GpHashMake(policy, tupdesc);
	/* the table's segments: every one, or a partial table's first so many */
	r->nsegs = policy->numsegments;
	r->replicated = GpPolicyIsReplicated(policy);
	r->binary = GpTupleDescHasBinaryIO(tupdesc);
	r->stores = palloc0_array(Tuplestorestate *, r->nsegs);
	r->lines = lines;
	r->storedesc = tupdesc;
	if (lines)
	{
		r->storedesc = CreateTemplateTupleDesc(tupdesc->natts + 1);
		for (int i = 1; i <= tupdesc->natts; i++)
			TupleDescCopyEntry(r->storedesc, i, tupdesc, i);
		TupleDescInitEntry(r->storedesc, tupdesc->natts + 1, "line",
						   INT8OID, -1, 0);
		TupleDescFinalize(r->storedesc);
		r->linevalues = palloc0_array(Datum, tupdesc->natts + 1);
		r->linenulls = palloc0_array(bool, tupdesc->natts + 1);
	}
	r->slot = MakeSingleTupleTableSlot(r->storedesc, &TTSOpsMinimalTuple);
	r->out = palloc0_array(FmgrInfo, tupdesc->natts);

	/*
	 * The columns the segment is given: all but the dropped ones and the
	 * generated ones, which the segment's own COPY computes.  A table with
	 * none of those -- CREATE TABLE t () -- is given no list: COPY takes no
	 * empty one, and its rows are each an empty line, or no fields.
	 */
	initStringInfo(&sql);
	appendStringInfo(&sql, "COPY %s",
					 GpDispatchRelationName(RelationGetRelid(rel)));
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			func;
		bool		isvarlena;

		if (att->attisdropped || att->attgenerated != '\0')
			continue;
		appendStringInfo(&sql, "%s%s", first ? " (" : ", ",
						 quote_identifier(NameStr(att->attname)));
		first = false;

		if (r->binary)
			getTypeBinaryOutputInfo(att->atttypid, &func, &isvarlena);
		else
			getTypeOutputInfo(att->atttypid, &func, &isvarlena);
		fmgr_info(func, &r->out[i]);
	}
	appendStringInfo(&sql, "%s FROM STDIN%s", first ? "" : ")",
					 r->binary ? " (FORMAT binary)" : "");
	r->copy_sql = sql.data;

	return r;
}

/* One row, whose values are the relation's columns in order; a COPY's line. */
static void
router_put(GpRouter *r, TupleTableSlot *slot, uint64 lineno)
{
	int			seg;
	int			natts = RelationGetDescr(r->rel)->natts;

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
	if (r->lines)
	{
		memcpy(r->linevalues, slot->tts_values, natts * sizeof(Datum));
		memcpy(r->linenulls, slot->tts_isnull, natts * sizeof(bool));
		r->linevalues[natts] = Int64GetDatum((int64) lineno);
		r->linenulls[natts] = false;
		tuplestore_putvalues(r->stores[seg], r->storedesc,
							 r->linevalues, r->linenulls);
	}
	else
		tuplestore_putvalues(r->stores[seg], r->storedesc,
							 slot->tts_values, slot->tts_isnull);
	r->nrows++;
}

/* The line in the data of the n-th row sent to a segment, or 0. */
static uint64
router_line(GpRouter *r, int content, uint64 n)
{
	Tuplestorestate *store;
	bool		isnull;
	Datum		d;

	if (!r->lines || n < 1 || content < 0 || content >= r->nsegs)
		return 0;
	store = r->stores[r->replicated ? 0 : content];
	if (store == NULL)
		return 0;
	tuplestore_rescan(store);
	if ((n > 1 && !tuplestore_skiptuples(store, n - 1, true)) ||
		!tuplestore_gettupleslot(store, true, false, r->slot))
		return 0;
	d = slot_getattr(r->slot, RelationGetDescr(r->rel)->natts + 1, &isnull);
	return isnull ? 0 : (uint64) DatumGetInt64(d);
}

/*
 * The context of an error a segment raised in the rows routed to it.  Its
 * last line is the segment's COPY's -- "COPY t, line n", the segment's
 * count of the rows it was sent -- which is no line of the statement's: an
 * INSERT's error says none, as Cloudberry's does, whose segments insert the
 * rows themselves, and a COPY's the line of its data the row came from
 * (router_copy_context()).  The rest -- a trigger's, a function's -- stays.
 * The segment's words are PostgreSQL's untranslated; a line in another
 * language is left as it is.
 */
static char *
router_error_context(int content, const char *context, void *arg)
{
	GpRouter   *r = (GpRouter *) arg;
	const char *relname = RelationGetRelationName(r->rel);
	size_t		rlen = strlen(relname);
	const char *last = strrchr(context, '\n');
	const char *after;

	last = last ? last + 1 : context;
	after = last + 5 + rlen;
	if (strncmp(last, "COPY ", 5) != 0 || strncmp(last + 5, relname, rlen) != 0 ||
		(*after != '\0' && *after != ',' && *after != ':'))
		return pstrdup(context);
	if (strncmp(after, ", line ", 7) == 0)
		r->error_lineno = router_line(r, content,
									  strtou64(after + 7, NULL, 10));
	return last == context ? NULL : pnstrdup(context, last - context - 1);
}

/*
 * The COPY's own context, for an error a segment raised in a row routed
 * there: the line of the data the row came from, as COPY says where it
 * failed (CopyFromErrorCallback()).
 */
static void
router_copy_context(void *arg)
{
	GpRouter   *r = (GpRouter *) arg;
	CopyFromState cstate = r->cstate;

	if (r->error_lineno == 0 || cstate == NULL)
		return;
	cstate->cur_lineno = r->error_lineno;
	cstate->line_buf_valid = false;
	cstate->cur_attname = NULL;
	cstate->relname_only = false;
	CopyFromErrorCallback(cstate);
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

	GpCopyInBegin(content, r->copy_sql, router_error_context, r);

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
			uint64		took;

			if (r->reached_stmt != NULL)
				GpReportDtxReached(r->reached_stmt, &seg, 1);
			took = router_send(r, r->stores[seg], seg);

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

	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		GpModifyLockPartitions(relid, RowExclusiveLock);
	outerPlanState(node) = ExecInitNode(linitial(cscan->custom_plans),
										estate, eflags);

	state->rel = table_open(relid, NoLock);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		GpPolicy   *policy = GpScanDistributedPolicy(relid);
		Plan	   *source = linitial(cscan->custom_plans);

		state->router = router_begin(state->rel, policy, false);

		/*
		 * Cloudberry sends a single row of constants to the one segment it
		 * hashes to, and anything else to every segment, as the slice the
		 * statement is: slice 0.  A replicated table's row goes everywhere.
		 */
		GpReportDispatch(0, IsA(source, Result) && outerPlan(source) == NULL &&
						 !GpPolicyIsReplicated(policy), policy->numsegments);

		/* the segments it reaches: the single row's, as it is routed */
		if (IsA(source, Result) && outerPlan(source) == NULL &&
			!GpPolicyIsReplicated(policy))
			state->router->reached_stmt = estate->es_plannedstmt;
		else
			GpReportDtxReached(estate->es_plannedstmt, NULL,
							   policy->numsegments);
	}
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
		router_put(state->router, slot, 0);
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
	int			ncontents;		/* the segments direct dispatch sends it to */
	int		   *contents;
	int			nsegments;		/* else the table's: all, or a partial
								 * table's first so many */
	Oid			relid;			/* the table it changes */
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
	state->nsegments = intVal(lfourth(cscan->custom_private));
	state->relid = (Oid) intVal(list_nth(cscan->custom_private, 4));
	state->contents = palloc_array(int, Max(list_length((List *) lthird(cscan->custom_private)), 1));
	foreach_int(content, (List *) lthird(cscan->custom_private))
		state->contents[state->ncontents++] = content;
	return (Node *) state;
}

/*
 * Cloudberry without its global deadlock detector: an INSERT into a
 * partitioned table locks every partition, since which of them its rows go
 * to is not known until they do -- so that it never waits on one segment for
 * a writer of a partition that waits for it on another (transformTargetTable()
 * in parse_clause.c, Cloudberry's issue 13652).
 */
void
GpModifyLockPartitions(Oid relid, LOCKMODE lockmode)
{
	if (!gp_enable_global_deadlock_detector &&
		get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
		(void) find_all_inheritors(relid, lockmode, NULL);
}

static void
modify_begin(CustomScanState *node, EState *estate, int eflags)
{
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * Cloudberry without its global deadlock detector: an UPDATE or DELETE
	 * of a distributed table locks the table, so that two never wait for
	 * each other on different segments.  With it, rows (gp_gdd.c).
	 */
	if (!gp_enable_global_deadlock_detector)
		LockRelationOid(((ModifyState *) node)->relid, ExclusiveLock);
	if (((ModifyState *) node)->ncontents > 0)
	{
		GpReportDispatchContents(0, ((ModifyState *) node)->contents,
								 ((ModifyState *) node)->ncontents);
		GpReportDtxReached(estate->es_plannedstmt,
						   ((ModifyState *) node)->contents,
						   ((ModifyState *) node)->ncontents);
	}
	else
	{
		GpReportDispatch(0, false, ((ModifyState *) node)->nsegments);
		GpReportDtxReached(estate->es_plannedstmt, NULL,
						   ((ModifyState *) node)->nsegments);
	}
}

static TupleTableSlot *
modify_exec(CustomScanState *node)
{
	ModifyState *state = (ModifyState *) node;
	EState	   *estate = node->ss.ps.state;
	ParamListInfo params = estate->es_param_list_info;
	int			nparams = params ? params->numParams : 0;
	Oid		   *types = NULL;
	const char **values = NULL;
	uint64	   *counts;
	int			nsegs;

	if (state->done)
		return NULL;

	/*
	 * The statement's parameters, as text, of the types its $n had here.  A
	 * segment must be told every $n's type, as it cannot infer one the
	 * statement does not read: PL/pgSQL passes one parameter for each of the
	 * function's variables, and those the statement does not read have no
	 * type, so go as a null text.
	 */
	if (nparams > 0)
	{
		types = palloc_array(Oid, nparams);
		values = palloc0_array(const char *, nparams);
		for (int i = 0; i < nparams; i++)
		{
			ParamExternData *prm;
			ParamExternData prmdata;

			if (params->paramFetch != NULL)
				prm = params->paramFetch(params, i + 1, false, &prmdata);
			else
				prm = &params->params[i];

			types[i] = OidIsValid(prm->ptype) ? prm->ptype : TEXTOID;
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
	if (state->ncontents > 0)
	{
		GpDispatchCommandParamsOnContents(state->sql, nparams, types, values,
										  state->contents, state->ncontents,
										  counts);
		nsegs = state->ncontents;
	}
	else
		GpDispatchCommandParams(state->sql, nparams, types, values, -1,
								state->nsegments, counts);

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
static const char current_of_reason[] =
	"WHERE CURRENT OF names a row by the cursor that read it, which is here.";

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
		cxt->why = current_of_reason;
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

static CustomScan *make_custom_scan(Plan *replaced,
									const CustomScanMethods *methods);

/* each relation a query names, at any level, into *relids */
static bool
named_relations_walker(Node *node, List **relids)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_RELATION)
			*relids = list_append_unique_oid(*relids, rte->relid);
		return false;
	}
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, named_relations_walker,
								 relids, QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(node, named_relations_walker, relids);
}

/*
 * The target entries of one column's assignments the rewriter merged into
 * one (process_matched_tle()) -- two of a composite's fields, two of an
 * array's elements -- taken apart again into the parser's, an assignment
 * each, in order.  pg_get_querydef() prints a column's assignment as the
 * parser gives it: of a nest, whose input is the assignment before it, it
 * prints the last alone, and it refuses a FieldStore of several fields.
 */
static List *unmerge_assignment(TargetEntry *tle, Expr *expr, List *result);

/* The assignments in an assignment's input, where there are any. */
static List *
unmerge_input(TargetEntry *tle, Expr *input, List *result)
{
	Expr	   *e = input;

	if (IsA(e, CoerceToDomain) &&
		((CoerceToDomain *) e)->coercionformat == COERCE_IMPLICIT_CAST)
		e = ((CoerceToDomain *) e)->arg;
	if (IsA(e, FieldStore) ||
		(IsA(e, SubscriptingRef) && ((SubscriptingRef *) e)->refassgnexpr != NULL))
		return unmerge_assignment(tle, input, result);
	return result;
}

/* An entry of the parser's: tle, its expression expr, under coerce's */
static List *
unmerged_entry(TargetEntry *tle, CoerceToDomain *coerce, Expr *expr,
			   List *result)
{
	TargetEntry *one = flatCopyTargetEntry(tle);

	if (coerce != NULL)
	{
		CoerceToDomain *c = palloc_object(CoerceToDomain);

		memcpy(c, coerce, sizeof(CoerceToDomain));
		c->arg = expr;
		expr = (Expr *) c;
	}
	one->expr = expr;
	return lappend(result, one);
}

static List *
unmerge_assignment(TargetEntry *tle, Expr *expr, List *result)
{
	CoerceToDomain *coerce = NULL;

	if (IsA(expr, CoerceToDomain) &&
		((CoerceToDomain *) expr)->coercionformat == COERCE_IMPLICIT_CAST)
	{
		coerce = (CoerceToDomain *) expr;
		expr = coerce->arg;
	}
	if (IsA(expr, FieldStore))
	{
		FieldStore *fs = (FieldStore *) expr;
		ListCell   *val;
		ListCell   *num;

		result = unmerge_input(tle, fs->arg, result);
		forboth(val, fs->newvals, num, fs->fieldnums)
		{
			FieldStore *one = makeNode(FieldStore);

			one->arg = fs->arg;
			one->newvals = list_make1(lfirst(val));
			one->fieldnums = list_make1_int(lfirst_int(num));
			one->resulttype = fs->resulttype;
			result = unmerged_entry(tle, coerce, (Expr *) one, result);
		}
		return result;
	}
	if (IsA(expr, SubscriptingRef) &&
		((SubscriptingRef *) expr)->refassgnexpr != NULL)
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) expr;
		SubscriptingRef *one = palloc_object(SubscriptingRef);

		result = unmerge_input(tle, sbsref->refexpr, result);
		memcpy(one, sbsref, sizeof(SubscriptingRef));
		return unmerged_entry(tle, coerce, (Expr *) one, result);
	}
	return unmerged_entry(tle, coerce, expr, result);
}

static List *
unmerge_target_list(List *tlist)
{
	List	   *result = NIL;

	foreach_node(TargetEntry, tle, tlist)
	{
		if (tle->resjunk)
			result = lappend(result, tle);
		else
			result = unmerge_assignment(tle, tle->expr, result);
	}
	return result;
}

void
GpUnmergeAssignments(Query *query)
{
	if (query->commandType == CMD_UPDATE || query->commandType == CMD_INSERT)
		query->targetList = unmerge_target_list(query->targetList);
	if (query->onConflict != NULL)
		query->onConflict->onConflictSet =
			unmerge_target_list(query->onConflict->onConflictSet);
	foreach_node(MergeAction, action, query->mergeActionList)
		action->targetList = unmerge_target_list(action->targetList);
	foreach_node(CommonTableExpr, cte, query->cteList)
		if (IsA(cte->ctequery, Query))
			GpUnmergeAssignments((Query *) cte->ctequery);
}

/*
 * The statement, as the segments are sent it.  pg_get_querydef() takes an
 * AccessShareLock on each relation it names and keeps it, as deparsing a
 * view does (AcquireRewriteLocks()); a statement that is run holds its own
 * locks on them already, the parser's, and Cloudberry's dispatch takes no
 * more, so the ones the deparsing added go again.
 */
static char *
statement_text(Query *query)
{
	List	   *relids = NIL;
	List	   *added = NIL;
	char	   *sql;

	(void) named_relations_walker((Node *) query, &relids);
	foreach_oid(relid, relids)
		if (!CheckRelationOidLockedByMe(relid, AccessShareLock, false))
			added = lappend_oid(added, relid);
	query = copyObject(query);
	GpUnmergeAssignments(query);
	sql = pg_get_querydef(query, false);
	foreach_oid(relid, added)
		if (CheckRelationOidLockedByMe(relid, AccessShareLock, false))
			UnlockRelationOid(relid, AccessShareLock);
	return sql;
}

/*
 * The statement "original", which changes "relid", sent to the segments
 * whole in place of "mt": each changes its own rows, direct dispatch where
 * its conditions fix the key.
 */
static Plan *
pushed_modify(ModifyTable *mt, Query *original, Oid relid, GpPolicy *policy)
{
	CustomScan *cscan = make_custom_scan(&mt->plan, &modify_scan_methods);

	cscan->custom_private =
		list_make5(makeString(statement_text(original)),
				   makeBoolean(GpPolicyIsReplicated(policy)),
				   GpScanDirectDispatchContents(relid,
												original->jointree->quals,
												original->resultRelation),
				   makeInteger(policy->numsegments),
				   makeInteger((int) relid));
	return &cscan->scan.plan;
}

static const char *
cannot_push_reason(Query *query, Oid target, GpPolicy *policy)
{
	PushContext cxt = {.target = target,.why = NULL};

	if (query->returningList != NIL)
		return "RETURNING from a distributed table waits for the rows to come back through a Motion.";

	/* a view's WITH CHECK OPTION, which the statement sent would not carry */
	if (query->withCheckOptions != NIL)
		return "It is written through a view WITH CHECK OPTION or under row-level security, whose checks are the coordinator's.";

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
 *
 * With the global deadlock detector on, Cloudberry's planner locks rows
 * instead for the query it can (checkCanOptSelectLockingClause, analyze.c):
 * one table in FROM, no subquery, no set operation -- a table whose rows
 * are each on one segment.  The segments lock them, the gather sending its
 * locking clause with its query (gp_scan.c).
 */
static bool
segments_lock_rows(Query *q)
{
	RowMarkClause *rc;
	RangeTblEntry *rte;
	GpPolicy   *policy;
	const char *gdd = GetConfigOption("gp.enable_global_deadlock_detector",
									  true, false);

	if (gdd == NULL || strcmp(gdd, "on") != 0 ||
		list_length(q->rowMarks) != 1 || q->setOperations != NULL ||
		q->hasSubLinks || q->jointree == NULL ||
		list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return false;
	rc = linitial_node(RowMarkClause, q->rowMarks);
	if (rc->pushedDown ||
		((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex != (int) rc->rti)
		return false;
	rte = rt_fetch(rc->rti, q->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION ||
		has_subclass(rte->relid))
		return false;
	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL ||
		!(GpPolicyIsHashPartitioned(policy) || GpPolicyIsRandomPartitioned(policy)))
		return false;

	GpScanSetLocking(rte->relid, rc->strength, rc->waitPolicy);
	q->rowMarks = NIL;

	/*
	 * The table's own lock, RowShareLock, which the parser left to planning
	 * (gp_modify_query_lockmode()): in the range table, for a cached plan to
	 * take too.
	 */
	if (rte->rellockmode < RowShareLock)
	{
		LockRelationOid(rte->relid, RowShareLock);
		rte->rellockmode = RowShareLock;
	}
	return true;
}

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

			/* in the range table too, for a cached plan to take */
			if (rte->rtekind == RTE_RELATION &&
				GpScanDistributedPolicy(rte->relid) != NULL)
			{
				LockRelationOid(rte->relid, ExclusiveLock);
				rte->rellockmode = ExclusiveLock;
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
	/* A scalar subquery in what is written is an initplan of the node replaced. */
	cscan->scan.plan.initPlan = replaced->initPlan;
	cscan->scan.plan.extParam = replaced->extParam;
	cscan->scan.plan.allParam = replaced->allParam;
	cscan->scan.plan.targetlist = NIL;
	cscan->scan.scanrelid = 0;
	cscan->methods = methods;
	return cscan;
}

static PlannedStmt *gp_modify_planner_routed(Query *parse,
											 const char *query_string,
											 int cursorOptions,
											 ParamListInfo boundParams,
											 ExplainState *es);

/*
 * The policy of "root", which a partitioned table's -- or an inheritance
 * tree's -- statement names, if every table it writes is distributed as
 * "root" is: the same key columns, by name, hashed alike, or all random, or
 * all replicated, over as many segments.  Then a row a segment moves into
 * another partition is still on the segment it belongs on.  NULL if not.
 */
static GpPolicy *
written_alike(PlannedStmt *stmt, ModifyTable *mt, Oid root)
{
	GpPolicy   *policy = GpScanDistributedPolicy(root);

	if (policy == NULL)
		return NULL;
	foreach_int(rti, mt->resultRelations)
	{
		Oid			relid = rt_fetch(rti, stmt->rtable)->relid;
		GpPolicy   *leaf = GpScanDistributedPolicy(relid);

		if (leaf == NULL || leaf->ptype != policy->ptype ||
			leaf->numsegments != policy->numsegments ||
			leaf->nattrs != policy->nattrs)
			return NULL;
		for (int k = 0; k < policy->nattrs; k++)
			if (leaf->opclasses[k] != policy->opclasses[k] ||
				strcmp(get_attname(relid, leaf->attrs[k], false),
					   get_attname(root, policy->attrs[k], false)) != 0)
				return NULL;
	}
	return policy;
}

/* Does it write a table whose rows are on the segments? */
static bool
writes_distributed(PlannedStmt *stmt, ModifyTable *mt)
{
	ListCell   *lc;

	foreach(lc, mt->resultRelations)
		if (GpScanDistributedPolicy(rt_fetch(lfirst_int(lc), stmt->rtable)->relid) != NULL)
			return true;
	return false;
}

static const char *
operation_words(CmdType operation)
{
	return operation == CMD_INSERT ? "INSERT INTO" :
		operation == CMD_UPDATE ? "UPDATE" :
		operation == CMD_DELETE ? "DELETE FROM" : "MERGE INTO";
}

/*
 * The write, by Cloudberry's Explicit Redistribute Motion (gp_explicit.c):
 * the plan runs here, and each row it writes is written on its segment.
 * Refused, with the reason, where that cannot be done.
 */
static Plan *
write_explicitly(PlannedStmt *stmt, ModifyTable *mt, const char *on_conflict)
{
	const char *why = GpExplicitCannot(stmt, mt, on_conflict);

	if (why != NULL)
	{
		Index		rti = mt->rootRelation != 0 ? mt->rootRelation
			: linitial_int(mt->resultRelations);

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot %s distributed table \"%s\" this way yet",
						operation_words(mt->operation),
						get_rel_name(rt_fetch(rti, stmt->rtable)->relid)),
				 errdetail("%s", why)));
	}
	return GpExplicitMake(mt, on_conflict);
}

/* The explicit write for a ModifyTable ORCA's translator made (merge.c). */
Plan *
GpModifyWriteExplicitly(PlannedStmt *stmt, Plan *modify)
{
	return write_explicitly(stmt, castNode(ModifyTable, modify), NULL);
}

/*
 * A ModifyTable left in a plan that writes a distributed table, where
 * nothing above took it: it would run on the coordinator, against its empty
 * copy, with rows gathered from the segments whose ctid means nothing here.
 * Refused, by name, rather than run.
 */
static void
refuse_local_write(Plan *plan, PlannedStmt *stmt)
{
	ModifyTable *mt;
	ListCell   *lc;

	if (plan == NULL || !IsA(plan, ModifyTable))
		return;
	mt = (ModifyTable *) plan;

	foreach(lc, mt->resultRelations)
	{
		RangeTblEntry *rte = rt_fetch(lfirst_int(lc), stmt->rtable);

		if (GpScanDistributedPolicy(rte->relid) == NULL)
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot %s distributed table \"%s\" this way yet",
						operation_words(mt->operation),
						get_rel_name(rte->relid)),
				 errdetail("The coordinator's plan would change the coordinator's copy, which has no rows.")));
	}
}

/*
 * A MERGE into a distributed table: the target's row, as a junk column the
 * plan carries up with each row it joins, where an action may read it.
 * PostgreSQL's MERGE fetches the row by its ctid as it acts; the explicit
 * write, on the coordinator, has only what the gathers brought
 * (gp_explicit.c).  A whole-row Var of the target, which for a partition
 * is its row as the root's.
 */
static void
merge_target_junk(Query *q)
{
	RangeTblEntry *rte;
	bool		reads = false;

	if (q->commandType != CMD_MERGE)
		return;
	rte = rt_fetch(q->resultRelation, q->rtable);
	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;
	foreach_node(MergeAction, action, q->mergeActionList)
		if (action->matchKind != MERGE_WHEN_NOT_MATCHED_BY_TARGET)
			reads = true;
	if (!reads)
		return;

	q->targetList = lappend(q->targetList,
							makeTargetEntry((Expr *) makeWholeRowVar(rte, q->resultRelation,
																	 0, false),
											list_length(q->targetList) + 1,
											pstrdup(GP_MERGE_TARGET_JUNK), true));
}

static PlannedStmt *
gp_modify_planner(Query *parse, const char *query_string, int cursorOptions,
				  ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;
	List	   *conflicts = NIL;
	ListCell   *conflict;
	ListCell   *lc;

	/*
	 * On every node: what of it needs the coordinator -- the segments to
	 * send a query to, the cluster's sizes -- asks for the coordinator
	 * itself (gp_segment.c, gp_size.c).
	 */
	GpPrepareQuery(parse);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return gp_modify_planner_routed(parse, query_string, cursorOptions,
										boundParams, es);

	/*
	 * An INSERT ... ON CONFLICT in a WITH query: its clause printed as the
	 * statement's own is, before the planner changes it -- one for each, in
	 * the WITH's order, which is the order the planner makes their subplans
	 * in (SS_process_ctes()); none for a table on the coordinator.  A write
	 * in a WITH query is at the statement's top level, as PostgreSQL's
	 * parser requires.
	 */
	foreach(lc, parse->cteList)
	{
		Query	   *q = castNode(Query, lfirst_node(CommonTableExpr, lc)->ctequery);
		GpPolicy   *target;

		merge_target_junk(q);
		if (q->commandType != CMD_INSERT || q->onConflict == NULL)
			continue;
		target = GpScanDistributedPolicy(rt_fetch(q->resultRelation, q->rtable)->relid);
		conflicts = lappend(conflicts,
							target != NULL ? GpExplicitOnConflict(q, target) : NULL);
	}

	merge_target_junk(parse);
	stmt = gp_modify_planner_routed(parse, query_string, cursorOptions,
									boundParams, es);

	/* a write in a WITH query is a subplan; it is written explicitly */
	conflict = list_head(conflicts);
	foreach(lc, stmt->subplans)
	{
		Plan	   *sub = (Plan *) lfirst(lc);
		const char *on_conflict = NULL;

		if (sub == NULL || !IsA(sub, ModifyTable))
			continue;
		if (((ModifyTable *) sub)->onConflictAction != ONCONFLICT_NONE &&
			conflict != NULL)
		{
			on_conflict = (const char *) lfirst(conflict);
			conflict = lnext(conflicts, conflict);
		}
		if (writes_distributed(stmt, (ModifyTable *) sub))
			lfirst(lc) = write_explicitly(stmt, (ModifyTable *) sub, on_conflict);
	}
	refuse_local_write(stmt->planTree, stmt);
	return stmt;
}

static PlannedStmt *
gp_modify_planner_routed(Query *parse, const char *query_string, int cursorOptions,
						 ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;
	Query	   *original = NULL;
	char	   *on_conflict = NULL;
	ModifyTable *mt;
	RangeTblEntry *rte;
	GpPolicy   *policy;
	bool		was_cursor;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return prev_planner ? prev_planner(parse, query_string, cursorOptions,
										   boundParams, es)
			: standard_planner(parse, query_string, cursorOptions, boundParams, es);

	if (!segments_lock_rows(parse))
		(void) lock_instead_of_row_marks((Node *) parse, NULL);

	/* a NOT IN's subquery that reads a ctid alone, as Cloudberry notices it */
	GpScanNoticeSublinkCtid(parse);

	/* The planner changes the Query; an UPDATE or DELETE may be sent as it was. */
	if (parse->commandType == CMD_UPDATE || parse->commandType == CMD_DELETE)
		original = copyObject(parse);

	/* and an INSERT's ON CONFLICT is sent as it was written */
	if (parse->commandType == CMD_INSERT && parse->onConflict != NULL)
	{
		GpPolicy   *target = GpScanDistributedPolicy(rt_fetch(parse->resultRelation,
															  parse->rtable)->relid);

		if (target != NULL)
			on_conflict = GpExplicitOnConflict(parse, target);
	}

	/*
	 * A correlated scalar subquery of an aggregate, whose every run would
	 * gather a distributed table again, made a join (gp_subselect.c), as
	 * Cloudberry's planner makes one.
	 */
	GpSubselectDecorrelate(parse);

	/* A cursor's gathers bring each row's ctid, for WHERE CURRENT OF. */
	was_cursor = GpScanSetCursor((cursorOptions & CURSOR_OPT_FAST_PLAN) != 0);
	PG_TRY();
	{
		stmt = prev_planner ? prev_planner(parse, query_string, cursorOptions,
										   boundParams, es)
			: standard_planner(parse, query_string, cursorOptions, boundParams, es);
	}
	PG_FINALLY();
	{
		GpScanClearLocking();
		(void) GpScanSetCursor(was_cursor);
	}
	PG_END_TRY();
	GpScanBoundGathers(stmt);

	if (!IsA(stmt->planTree, ModifyTable))
		return stmt;
	mt = (ModifyTable *) stmt->planTree;

	/*
	 * A partitioned table's partitions, or an inheritance tree -- one of
	 * them alone, where the planner pruned the rest: sent to the segments
	 * whole, as a plain table's statement is, where it reads only its target
	 * and each table it writes is distributed as the one it names; each
	 * segment's executor finds each row's partition, and moves a row whose
	 * partition key changes into another, there.
	 */
	if (list_length(mt->resultRelations) != 1 ||
		(original != NULL &&
		 rt_fetch(original->resultRelation, original->rtable)->relid !=
		 rt_fetch(linitial_int(mt->resultRelations), stmt->rtable)->relid))
	{
		if (!writes_distributed(stmt, mt))
			return stmt;
		if (original != NULL)
		{
			Oid			root = rt_fetch(original->resultRelation,
										original->rtable)->relid;

			policy = written_alike(stmt, mt, root);
			if (policy != NULL &&
				cannot_push_reason(original, root, policy) == NULL)
			{
				stmt->planTree = pushed_modify(mt, original, root, policy);
				return stmt;
			}
		}
		stmt->planTree = write_explicitly(stmt, mt, NULL);
		return stmt;
	}
	rte = rt_fetch(linitial_int(mt->resultRelations), stmt->rtable);
	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL)
		return stmt;

	if (mt->operation == CMD_INSERT)
	{
		CustomScan *cscan;
		Relation	rel;

		/*
		 * What it wrote comes back from the segments, for RETURNING and for
		 * a view's check options; ON CONFLICT is each segment's, after its
		 * rows' VALUES, which COPY has no place for; and a table's policies
		 * a segment's COPY refuses to apply.
		 */
		if (mt->returningLists != NIL || mt->onConflictAction != ONCONFLICT_NONE ||
			mt->withCheckOptionLists != NIL)
		{
			stmt->planTree = write_explicitly(stmt, mt, on_conflict);
			return stmt;
		}

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
		const char *why;

		/*
		 * What the segments cannot do as it is written, the plan does here,
		 * and each row it changes is changed where it is -- WHERE CURRENT OF
		 * among it, whose cursor's gather says where its row is (gp_scan.c).
		 */
		why = cannot_push_reason(original, rte->relid, policy);

		/*
		 * A replicated table's row has a ctid on each segment, and the
		 * cursor read one of them: Cloudberry's words for it
		 * (isSimplyUpdatableRelation).
		 */
		if (why == current_of_reason && GpPolicyIsReplicated(policy))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" is not simply updatable",
							get_rel_name(rte->relid))));
		if (why != NULL)
		{
			stmt->planTree = write_explicitly(stmt, mt, NULL);
			return stmt;
		}

		stmt->planTree = pushed_modify(mt, original, rte->relid, policy);
		return stmt;
	}

	/*
	 * MERGE: the plan joins source and target here, and each action is
	 * written where its row is -- an UPDATE's, a DELETE's, an INSERT's.
	 */
	if (mt->operation == CMD_MERGE)
		stmt->planTree = write_explicitly(stmt, mt, NULL);

	return stmt;
}

/* ------------------------------------------------------------------------- */
/* COPY                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's options of COPY that PostgreSQL's COPY has not: [LOG ERRORS]
 * SEGMENT REJECT LIMIT n [ROWS | PERCENT] and FILL MISSING FIELDS and
 * NEWLINE, which the grammar carries on the statement's options
 * (gp_desugar.c, rw_copy_options()) or its option list names, and in text
 * an ESCAPE, which PostgreSQL's COPY takes in CSV alone.  A COPY FROM with
 * any reads its data through gp_exttable's filter, which takes them, and
 * logs and leaves out each line that would not load under SEGMENT REJECT
 * LIMIT (copysreh.c); the rest is loaded by PostgreSQL's COPY, from it.  A
 * COPY TO takes an ESCAPE alone (copy_to_escaped()).
 */
typedef struct CopySreh
{
	int			limit;			/* SEGMENT REJECT LIMIT, or -1 */
	bool		rows;
	char		log_errors;
	const char *name;			/* the first of the options, as said */
	void	   *state;			/* gp_exttable's, while the data is read */
} CopySreh;

typedef void *(*SrehCopyBegin_fn) (ParseState *pstate, Relation rel,
								   const char *filename, bool is_program,
								   List *attlist, List *options,
								   int reject_limit, bool limit_in_rows,
								   char log_errors, CopyFromState *loader);
typedef uint64 (*SrehCopyEnd_fn) (void *state);
typedef uint64 (*CopyToEscaped_fn) (ParseState *pstate, Relation rel,
									RawStmt *query, List *attlist,
									const char *filename, bool is_program,
									List *options, bool escape_off,
									char escape_char);

/* A COPY's format, as its options give it: "text", "csv" or "binary". */
static const char *
copy_format(CopyStmt *stmt)
{
	foreach_node(DefElem, def, stmt->options)
		if (def->defnamespace == NULL && strcmp(def->defname, "format") == 0)
			return defGetString(def);
	return "text";
}

/* The name of the first of Cloudberry's options the COPY has, or NULL. */
static const char *
copy_cloudberry_option(CopyStmt *stmt)
{
	bool		text = strcmp(copy_format(stmt), "text") == 0;

	foreach_node(DefElem, def, stmt->options)
	{
		if (def->defnamespace != NULL &&
			strcmp(def->defnamespace, "gp_exttable") == 0 &&
			strcmp(def->defname, "reject_limit") == 0)
			return "SEGMENT REJECT LIMIT";
		if (strcmp(def->defname, "fill_missing_fields") == 0)
			return "FILL MISSING FIELDS";
		if (strcmp(def->defname, "newline") == 0)
			return "NEWLINE";
		if (text && def->defnamespace == NULL &&
			strcmp(def->defname, "escape") == 0)
			return "ESCAPE";
	}
	return NULL;
}

/*
 * The carried options, taken out before PostgreSQL's COPY reads the rest:
 * SEGMENT REJECT LIMIT's into sreh, and FILL MISSING FIELDS and NEWLINE left
 * as options of their own names, which gp_exttable's filter reads.
 */
static void
copy_take_sreh(CopyStmt *stmt, CopySreh *sreh)
{
	ListCell   *lc;

	sreh->limit = -1;
	sreh->rows = true;
	sreh->log_errors = 'f';
	sreh->name = copy_cloudberry_option(stmt);
	sreh->state = NULL;
	foreach(lc, stmt->options)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace == NULL || strcmp(def->defnamespace, "gp_exttable") != 0)
			continue;
		if (strcmp(def->defname, "reject_limit") == 0)
			sreh->limit = atoi(defGetString(def));
		else if (strcmp(def->defname, "reject_limit_type") == 0)
			sreh->rows = (defGetString(def)[0] == 'r');
		else if (strcmp(def->defname, "log_errors") == 0)
			sreh->log_errors = defGetString(def)[0];
		else
		{
			lfirst(lc) = makeDefElem(def->defname, def->arg, def->location);
			continue;
		}
		stmt->options = foreach_delete_current(stmt->options, lc);
	}
}

static bool
copy_has_option(CopyStmt *stmt, const char *name)
{
	foreach_node(DefElem, def, stmt->options)
		if (strcmp(def->defname, name) == 0)
			return true;
	return false;
}

/*
 * The COPY's data, through the filter: the COPY FROM that loads the lines
 * it passes, which gp_exttable makes.
 */
static CopyFromState
copy_begin_sreh(ParseState *pstate, CopyStmt *stmt, Relation rel,
				CopySreh *sreh)
{
	SrehCopyBegin_fn begin;
	CopyFromState loader;

	begin = (SrehCopyBegin_fn)
		load_external_function("$libdir/gp_exttable", "GpSrehCopyBegin", true, NULL);
	sreh->state = begin(pstate, rel, stmt->filename, stmt->is_program,
						stmt->attlist, stmt->options, sreh->limit, sreh->rows,
						sreh->log_errors, &loader);
	return loader;
}

static void
copy_end_sreh(CopySreh *sreh)
{
	SrehCopyEnd_fn end = (SrehCopyEnd_fn)
		load_external_function("$libdir/gp_exttable", "GpSrehCopyEnd", true, NULL);

	(void) end(sreh->state);
	sreh->state = NULL;
}

/*
 * COPY FROM's privileges, checked as DoCopy() checks them: INSERT on the
 * table or its columns, through ExecCheckPermissions() and so through its
 * hook, where diskquota refuses a load into a table over its quota.
 */
static void
copy_from_check_permissions(ParseState *pstate, CopyStmt *stmt, Relation rel)
{
	ParseNamespaceItem *nsitem;
	RTEPermissionInfo *perminfo;

	nsitem = addRangeTableEntryForRelation(pstate, rel, RowExclusiveLock,
										   NULL, false, false);
	perminfo = nsitem->p_perminfo;
	perminfo->requiredPerms = ACL_INSERT;
	foreach_int(attnum, CopyGetAttnums(RelationGetDescr(rel), rel, stmt->attlist))
		perminfo->insertedCols = bms_add_member(perminfo->insertedCols,
												attnum - FirstLowInvalidHeapAttributeNumber);
	ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);
}

/*
 * COPY t FROM, into a table whose rows are here -- one node's, or one the
 * coordinator keeps -- with Cloudberry's options: DoCopy()'s checks, and
 * PostgreSQL's COPY, reading through the filter.
 */
static uint64
copy_from_local_sreh(ParseState *pstate, CopyStmt *stmt, Relation rel,
					 CopySreh *sreh)
{
	CopyFromState cstate;
	uint64		processed;

	copy_from_check_permissions(pstate, stmt, rel);
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY FROM not supported with row-level security"),
				 errhint("Use INSERT statements instead.")));
	if (XactReadOnly && !rel->rd_islocaltemp)
		PreventCommandIfReadOnly("COPY FROM");

	cstate = copy_begin_sreh(pstate, stmt, rel, sreh);
	processed = CopyFrom(cstate);
	EndCopyFrom(cstate);
	copy_end_sreh(sreh);
	return processed;
}

/*
 * COPY t FROM: parsed by PostgreSQL's own COPY, as it would be into a local
 * table, and routed.  The coordinator evaluates the defaults, as for INSERT,
 * so that a serial column has one sequence.  With Cloudberry's options, read
 * through gp_exttable's filter (above).  An error in the data says where, as
 * COPY's own does, and so does one a segment raises in a row routed there
 * (router_error_context()): the line of the data the row came from.
 */
static uint64
copy_from_distributed(ParseState *pstate, CopyStmt *stmt, Relation rel,
					  GpPolicy *policy, CopySreh *sreh)
{
	CopyFromState cstate;
	GpRouter   *router;
	TupleTableSlot *slot;
	ExprContext *econtext;
	EState	   *estate = CreateExecutorState();
	ErrorContextCallback errcallback;
	uint64		processed;

	if (sreh != NULL)
		cstate = copy_begin_sreh(pstate, stmt, rel, sreh);
	else
		cstate = BeginCopyFrom(pstate, rel, NULL, stmt->filename, stmt->is_program,
							   NULL, stmt->attlist, stmt->options);
	router = router_begin(rel, policy, true);
	router->cstate = cstate;
	/* Cloudberry's COPY is sent to every segment of the table */
	GpReportDtxReached(NULL, NULL, policy->numsegments);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	econtext = GetPerTupleExprContext(estate);

	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		ExecClearTuple(slot);

		if (!NextCopyFrom(cstate, econtext, slot->tts_values, slot->tts_isnull))
			break;
		ExecStoreVirtualTuple(slot);
		router_put(router, slot, cstate->cur_lineno);
	}
	error_context_stack = errcallback.previous;

	errcallback.callback = router_copy_context;
	errcallback.arg = router;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	processed = router_finish(router);
	error_context_stack = errcallback.previous;

	EndCopyFrom(cstate);
	router_end(router);

	/*
	 * What the filter refused is said once the rows are where they go: an
	 * error a segment raises of them fails the COPY first, as Cloudberry's
	 * says nothing of its rejects then.
	 */
	if (sreh != NULL)
		copy_end_sreh(sreh);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	return processed;
}

/*
 * COPY t TO as the query that reads t: SELECT its columns FROM ONLY t, as
 * COPY of a table reads no child of it; a partitioned table's query reads
 * the partitions.  A distributed table's COPY TO, whose rows the query
 * gathers like any other, and, with Cloudberry's ESCAPE, a table's with row
 * security, as DoCopy() reads it.
 */
static Node *
copy_to_query(CopyStmt *stmt, Oid relid)
{
	StringInfoData sql;
	List	   *raw;

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	if (stmt->attlist == NIL)
		appendStringInfoString(&sql, "*");
	else
	{
		ListCell   *lc;

		foreach(lc, stmt->attlist)
			appendStringInfo(&sql, "%s%s", lc == list_head(stmt->attlist) ? "" : ", ",
							 quote_identifier(strVal(lfirst(lc))));
	}
	appendStringInfo(&sql, " FROM %s%s",
					 get_rel_relkind(relid) == RELKIND_RELATION ? "ONLY " : "",
					 GpDispatchRelationName(relid));
	raw = raw_parser(sql.data, RAW_PARSE_DEFAULT);
	return linitial_node(RawStmt, raw)->stmt;
}

/*
 * COPY ... TO in text with an ESCAPE of Cloudberry's -- 'OFF', or one byte
 * of its own -- which PostgreSQL's COPY TO refuses outside CSV: gp_exttable
 * writes it (copyout.c, GpCopyToEscaped()), after DoCopy()'s checks, of a
 * file's or a program's privileges and of the table's.  An ESCAPE of the
 * backslash, text's own, is only left out.
 */
static uint64
copy_to_escaped(ParseState *pstate, CopyStmt *stmt, int stmt_location,
				int stmt_len, bool *done)
{
	char	   *esc = NULL;
	List	   *options = NIL;
	Relation	rel = NULL;
	RawStmt    *query = NULL;
	CopyToEscaped_fn copy_to;
	uint64		processed;

	foreach_node(DefElem, def, stmt->options)
	{
		if (def->defnamespace == NULL && strcmp(def->defname, "escape") == 0)
		{
			if (esc != NULL)
				errorConflictingDefElem(def, pstate);
			esc = defGetString(def);
		}
		else
			options = lappend(options, def);
	}
	stmt->options = options;
	*done = false;
	if (esc == NULL || strcmp(esc, "\\") == 0)
		return 0;
	if (pg_strcasecmp(esc, "off") != 0 && (strlen(esc) != 1 || IS_HIGHBIT_SET(esc[0])))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape must be a single one-byte character")));

	if (stmt->filename != NULL)
	{
		if (stmt->is_program &&
			!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to COPY to or from an external program"),
					 errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
							   "pg_execute_server_program"),
					 errhint("Anyone can COPY to stdout or from stdin. "
							 "psql's \\copy command also works for anyone.")));
		if (!stmt->is_program &&
			!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to COPY to a file"),
					 errdetail("Only roles with privileges of the \"%s\" role may COPY to a file.",
							   "pg_write_server_files"),
					 errhint("Anyone can COPY to stdout or from stdin. "
							 "psql's \\copy command also works for anyone.")));
	}

	if (stmt->relation != NULL)
	{
		Oid			relid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);
		GpPolicy   *policy = GpClusterBackendRole() == GP_ROLE_DISPATCH ?
			GpScanDistributedPolicy(relid) : NULL;

		if (policy != NULL ||
			check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		{
			query = makeNode(RawStmt);
			query->stmt = copy_to_query(stmt, relid);
		}
		else
		{
			ParseNamespaceItem *nsitem;
			RTEPermissionInfo *perminfo;

			/* SELECT on the table or its columns, as DoCopy() checks it */
			rel = table_open(relid, NoLock);
			nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
												   NULL, false, false);
			perminfo = nsitem->p_perminfo;
			perminfo->requiredPerms = ACL_SELECT;
			foreach_int(attnum, CopyGetAttnums(RelationGetDescr(rel), rel, stmt->attlist))
				perminfo->selectedCols = bms_add_member(perminfo->selectedCols,
														attnum - FirstLowInvalidHeapAttributeNumber);
			ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);
		}
	}
	else
	{
		query = makeNode(RawStmt);
		query->stmt = stmt->query;
		query->stmt_location = stmt_location;
		query->stmt_len = stmt_len;
	}

	copy_to = (CopyToEscaped_fn)
		load_external_function("$libdir/gp_exttable", "GpCopyToEscaped", true, NULL);
	processed = copy_to(pstate, rel, query, rel ? stmt->attlist : NIL,
						stmt->filename, stmt->is_program, options,
						pg_strcasecmp(esc, "off") == 0,
						pg_strcasecmp(esc, "off") == 0 ? '\0' : esc[0]);
	if (rel != NULL)
		table_close(rel, NoLock);
	*done = true;
	return processed;
}

static void
gp_modify_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	CopySreh	sreh_data;
	CopySreh   *sreh = NULL;

	/* Cloudberry's options, carried by the grammar or named: copy_take_sreh() */
	if (IsA(parsetree, CopyStmt) &&
		copy_cloudberry_option((CopyStmt *) parsetree) != NULL)
	{
		CopyStmt   *stmt;

		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
		}
		stmt = (CopyStmt *) parsetree;
		copy_take_sreh(stmt, &sreh_data);
		sreh = &sreh_data;
		if (!stmt->is_from && sreh->limit >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling only available using COPY FROM")));
		if (!stmt->is_from && copy_has_option(stmt, "fill_missing_fields"))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("fill missing fields only available for data loading, not unloading")));
		if (!stmt->is_from && copy_has_option(stmt, "newline"))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("newline currently available for data loading only, not unloading")));
		if (!stmt->is_from)
		{
			ParseState *pstate = make_parsestate(NULL);
			bool		done;
			uint64		processed;

			pstate->p_sourcetext = queryString;
			pstate->p_queryEnv = queryEnv;
			processed = copy_to_escaped(pstate, stmt, pstmt->stmt_location,
										pstmt->stmt_len, &done);
			if (done)
			{
				if (qc)
					SetQueryCompletion(qc, CMDTAG_COPY, processed);
				return;
			}
			/* an ESCAPE of the backslash, left out: PostgreSQL's COPY TO */
			sreh = NULL;
		}
		else if (stmt->relation == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling only available for distributed user tables")));
		else if (stmt->whereClause != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY FROM ... WHERE is not supported with %s", sreh->name)));
	}

	if (IsA(parsetree, CopyStmt) && ((CopyStmt *) parsetree)->relation != NULL &&
		(GpClusterBackendRole() == GP_ROLE_DISPATCH || sreh != NULL))
	{
		CopyStmt   *stmt = (CopyStmt *) parsetree;
		Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);
		GpPolicy   *policy = OidIsValid(relid) && GpClusterBackendRole() == GP_ROLE_DISPATCH ?
			GpScanDistributedPolicy(relid) : NULL;

		if (sreh != NULL && policy == NULL)
		{
			ParseState *pstate = make_parsestate(NULL);
			Relation	rel;
			uint64		processed;

			pstate->p_sourcetext = queryString;
			pstate->p_queryEnv = queryEnv;
			rel = table_openrv(stmt->relation, RowExclusiveLock);
			if (stmt->filename != NULL && !has_privs_of_role(GetUserId(),
															 stmt->is_program ? ROLE_PG_EXECUTE_SERVER_PROGRAM : ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file or program")));
			processed = copy_from_local_sreh(pstate, stmt, rel, sreh);
			table_close(rel, NoLock);
			if (qc)
				SetQueryCompletion(qc, CMDTAG_COPY, processed);
			return;
		}

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
			 * The lock and the privileges are the ones COPY FROM takes.
			 */
			rel = table_open(relid, RowExclusiveLock);
			copy_from_check_permissions(pstate, stmt, rel);
			if (stmt->filename != NULL && !has_privs_of_role(GetUserId(),
															 stmt->is_program ? ROLE_PG_EXECUTE_SERVER_PROGRAM : ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file or program")));

			processed = copy_from_distributed(pstate, stmt, rel, policy, sreh);
			table_close(rel, NoLock);

			if (qc)
				SetQueryCompletion(qc, CMDTAG_COPY, processed);
			GpAutoStats(CMD_INSERT, relid, processed,
						context != PROCESS_UTILITY_TOPLEVEL);
			return;
		}

		if (policy != NULL && !stmt->is_from)
		{
			/*
			 * COPY t TO is COPY (SELECT ... FROM t) TO, whose query gathers
			 * the rows like any other.
			 */
			CopyStmt   *copy = copyObject(stmt);

			copy->query = copy_to_query(stmt, relid);
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

/*
 * O30: the lock a query takes on a distributed table, where PostgreSQL takes
 * its own -- as the parser opens the table, or the rewriter brings it in
 * under a view or a rule -- and first, so that none taken on it later is an
 * upgrade two sessions deadlock on.  The planner and the executor ask for
 * theirs again (here, gp_explicit.c, gp_motion.c, lockrows.c), and find it
 * held.
 *
 * Without the global deadlock detector, Cloudberry's ExclusiveLock, as its
 * parser takes it (CdbTryOpenTable, addRangeTableEntry): on a table an
 * UPDATE, a DELETE or an INSERT ... ON CONFLICT DO UPDATE writes, and on one
 * a locking clause locks.  With it, a write keeps PostgreSQL's mode and
 * locks rows; a locking clause locks the rows on the segments for a query of
 * that one table, and the table otherwise -- which is known only once the
 * query is planned, so until then AccessShareLock, as Cloudberry's parser
 * holds before it decides.  A replicated table's rows are on every segment,
 * and a partitioned table's in its partitions, neither locked row by row:
 * their ExclusiveLock now.
 */
static LOCKMODE
gp_modify_query_lockmode(Oid relid, LOCKMODE lockmode, AclMode requiredPerms)
{
	GpPolicy   *policy;
	bool		writes;

	if (prev_query_lockmode)
		lockmode = prev_query_lockmode(relid, lockmode, requiredPerms);

	writes = lockmode == RowExclusiveLock &&
		(requiredPerms & (ACL_UPDATE | ACL_DELETE)) != 0;
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		!(writes || lockmode == RowShareLock) ||
		(policy = GpScanDistributedPolicy(relid)) == NULL)
		return lockmode;

	if (!gp_enable_global_deadlock_detector)
		return ExclusiveLock;
	if (writes)
		return lockmode;
	if (GpPolicyIsReplicated(policy) ||
		get_rel_relkind(relid) != RELKIND_RELATION || has_subclass(relid))
		return ExclusiveLock;
	return AccessShareLock;
}

void
GpModifyInit(void)
{
	/*
	 * COPY's single-row error handling, which one node carries out too
	 * (copy_from_local_sreh()): the grammar carries it on the statement for
	 * this hook to take off, cluster or none.  On one node no table has a
	 * policy, and the hook does nothing else.
	 */
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_modify_ProcessUtility;

	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&insert_scan_methods);
	RegisterCustomScanMethods(&modify_scan_methods);
	GpExplicitInit();

	prev_planner = planner_hook;
	planner_hook = gp_modify_planner;

	prev_query_lockmode = query_lockmode_hook;
	query_lockmode_hook = gp_modify_query_lockmode;
}
