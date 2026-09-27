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
 * gp_monitor.c
 *	  Cloudberry's functions for looking into a query and into the
 *	  cluster's backends: gp_dump_query_oids(), what a query depends on;
 *	  gp_log_backend_memory_contexts(), each segment's backends of a session
 *	  told to log their memory contexts; and gp_get_suboverflowed_backends(),
 *	  the backends of a node whose subtransactions have overflowed their
 *	  cache, which gp_suboverflowed_backend reads on every node.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/utils/adt/gp_dump_oids.c
 *	  src/backend/utils/adt/mcxtfuncs.c: gp_log_backend_memory_contexts()
 *	  src/backend/cdb/cdbutil.c: gp_get_suboverflowed_backends()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/indexing.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_gdd.h"

/* ------------------------------------------------------------------------- */
/* gp_dump_query_oids()                                                      */
/* ------------------------------------------------------------------------- */

/*
 * The functions a query calls, as the planner would record its plan's
 * dependence on them (setrefs.c's fix_expr_common(), which Cloudberry's
 * record_plan_function_dependency() copies into its list for this
 * function): an aggregate, a window function, a function, an operator's
 * function, those of an array's operator; the built-in ones not, whose
 * OIDs are below FirstUnpinnedObjectId.  In the order met, each once.
 */
static bool
query_funcs_walker(Node *node, List **funcs)
{
	Oid			funcs_met[3] = {InvalidOid, InvalidOid, InvalidOid};

	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;

		if (query->commandType == CMD_UTILITY)
		{
			/* CALL's call, and the query EXPLAIN and the like hold */
			if (IsA(query->utilityStmt, CallStmt))
			{
				CallStmt   *call = (CallStmt *) query->utilityStmt;

				(void) query_funcs_walker((Node *) call->funcexpr, funcs);
				(void) query_funcs_walker((Node *) call->outargs, funcs);
				return false;
			}
			query = UtilityContainsQuery(query->utilityStmt);
			if (query == NULL)
				return false;
		}
		return query_tree_walker(query, query_funcs_walker, funcs, 0);
	}

	if (IsA(node, Aggref))
		funcs_met[0] = ((Aggref *) node)->aggfnoid;
	else if (IsA(node, WindowFunc))
		funcs_met[0] = ((WindowFunc *) node)->winfnoid;
	else if (IsA(node, FuncExpr))
		funcs_met[0] = ((FuncExpr *) node)->funcid;
	else if (IsA(node, OpExpr) || IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		/* the three are one struct */
		set_opfuncid((OpExpr *) node);
		funcs_met[0] = ((OpExpr *) node)->opfuncid;
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

		set_sa_opfuncid(saop);
		funcs_met[0] = saop->opfuncid;
		funcs_met[1] = saop->hashfuncid;
		funcs_met[2] = saop->negfuncid;
	}

	for (int i = 0; i < lengthof(funcs_met); i++)
		if (funcs_met[i] >= (Oid) FirstUnpinnedObjectId)
			*funcs = list_append_unique_oid(*funcs, funcs_met[i]);

	return expression_tree_walker(node, query_funcs_walker, funcs);
}

/*
 * The sequences a table's column defaults call nextval() of: a default
 * (pg_attrdef) depends on the column it is of and on each relation it
 * names -- Cloudberry's getRefedSequences().
 */
static List *
table_default_sequences(Oid relid)
{
	List	   *defaults = NIL;
	List	   *sequences = NIL;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	/* the table's column defaults */
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

		if (dep->classid == AttrDefaultRelationId && dep->objsubid == 0 &&
			dep->refobjsubid != 0)
			defaults = lappend_oid(defaults, dep->objid);
	}
	systable_endscan(scan);

	/* and the sequences each of them depends on */
	foreach_oid(def, defaults)
	{
		ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(AttrDefaultRelationId));
		ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(def));
		scan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

			if (dep->refclassid == RelationRelationId && dep->refobjsubid == 0 &&
				get_rel_relkind(dep->refobjid) == RELKIND_SEQUENCE)
				sequences = lappend_oid(sequences, dep->refobjid);
		}
		systable_endscan(scan);
	}

	table_close(depRel, AccessShareLock);
	return sequences;
}

/*
 * The relations and functions a text of statements depends on, added to
 * the lists: each statement's, and, for a materialized view a statement
 * reads at its own level, its definition's -- Cloudberry's
 * gp_dump_query_oids() has the rewriter expand such a view as it expands a
 * view (its Query's expandMatViews, which PostgreSQL 19's has not), and
 * one in a statement EXPLAIN holds, or in a subquery, is not expanded,
 * there as here.
 */
static void
text_dependencies(const char *sql, List **relids, List **funcs, bool top)
{
	List	   *queries = NIL;
	bool		hasRowSecurity = false;

	foreach_node(RawStmt, raw, pg_parse_query(sql))
		queries = list_concat(queries,
							  pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL));

	foreach_node(Query, q, queries)
	{
		List	   *q_relids = NIL;
		List	   *q_invalitems = NIL;

		extract_query_dependencies((Node *) q, &q_relids, &q_invalitems,
								   &hasRowSecurity);
		*relids = list_concat(*relids, q_relids);
		(void) query_funcs_walker((Node *) q, funcs);

		if (!top || q->commandType == CMD_UTILITY)
			continue;
		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_MATVIEW)
				text_dependencies(TextDatumGetCString(DirectFunctionCall1(pg_get_viewdef,
																		  ObjectIdGetDatum(rte->relid))),
								  relids, funcs, false);
	}
}

static void
append_oids(StringInfo buf, List *oids)
{
	bool		first = true;

	foreach_oid(oid, oids)
	{
		if (!first)
			appendStringInfoChar(buf, ',');
		appendStringInfo(buf, "%u", oid);
		first = false;
	}
}

/*
 * An error in the query is reported as the query's, an internal query, at
 * its place there -- Cloudberry's sql_query_parse_error_callback().
 */
static void
dump_query_error_callback(void *arg)
{
	int			pos = geterrposition();

	errposition(0);
	internalerrposition(pos);
	internalerrquery((const char *) arg);
}

PG_FUNCTION_INFO_V1(gp_dump_query_oids);

/*
 * gp_dump_query_oids(text): the relations and the functions the statements
 * of the text depend on, as a JSON object of two lists -- {"relids": "...",
 * "funcids": "..."} -- which Cloudberry's minirepro reads to dump what a
 * query needs.  The relations are those the planner would record
 * (extract_query_dependencies()), the sequences their defaults call, and
 * the partitions and inheritors of each; each once, in the order met.
 */
Datum
gp_dump_query_oids(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ErrorContextCallback errcallback;
	List	   *relids = NIL;
	List	   *funcs = NIL;
	List	   *result = NIL;
	StringInfoData buf;

	/* the statements, analyzed and rewritten as they would be planned */
	errcallback.callback = dump_query_error_callback;
	errcallback.arg = sql;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	text_dependencies(sql, &relids, &funcs, true);
	error_context_stack = errcallback.previous;

	/* the sequences of the relations' defaults */
	foreach_oid(relid, list_copy(relids))
		relids = list_concat(relids, table_default_sequences(relid));

	/* each relation once, and the relations that inherit from it after it */
	foreach_oid(relid, relids)
	{
		if (list_member_oid(result, relid))
			continue;
		result = lappend_oid(result, relid);
		result = list_concat(result,
							 list_delete_first(find_all_inheritors(relid, NoLock, NULL)));
	}

	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"relids\": \"");
	append_oids(&buf, result);
	appendStringInfoString(&buf, "\", \"funcids\": \"");
	append_oids(&buf, funcs);
	appendStringInfoString(&buf, "\"}");

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/* ------------------------------------------------------------------------- */
/* gp_log_backend_memory_contexts()                                          */
/* ------------------------------------------------------------------------- */

/* A segment's answer that it logged nothing, as Cloudberry's says it. */
#define LOG_MEMORY_CONTEXTS_FAILED	(-99)

/*
 * On a segment: every backend of this node that works for the session is
 * told to log its memory contexts (pg_log_backend_memory_contexts()).  The
 * segment's content id if one was and none failed; otherwise
 * LOG_MEMORY_CONTEXTS_FAILED.
 */
static int64
log_session_memory_contexts(int session)
{
	List	   *pids = NIL;
	int			logged = 0;
	bool		failed = false;

	/* the backends of the session, found before any is signalled */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (int i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];

		if (proc->pid != 0 && proc->backendType == B_BACKEND)
			pids = lappend_int(pids, proc->pid);
	}
	LWLockRelease(ProcArrayLock);

	foreach_int(pid, pids)
	{
		int			its_session;
		bool		reader;

		if (session <= 0 ||
			!GpGddBackendIdentity(pid, &its_session, &reader) ||
			its_session != session)
			continue;
		if (DatumGetBool(DirectFunctionCall1(pg_log_backend_memory_contexts,
											 Int32GetDatum(pid))))
			logged++;
		else
			failed = true;
	}

	if (failed || logged == 0)
		return LOG_MEMORY_CONTEXTS_FAILED;
	return GpClusterContentId();
}

PG_FUNCTION_INFO_V1(gp_log_backend_memory_contexts);

/*
 * gp_log_backend_memory_contexts(session [, content]): on the coordinator,
 * the session's backends of every segment, or of the one given, told to
 * log their memory contexts, and how many segments did; a WARNING for each
 * that could not, and for a content that is none.  On a segment, its own
 * backends of the session.  Not in utility mode, as in Cloudberry: a node
 * of no cluster has no segments to ask.
 */
Datum
gp_log_backend_memory_contexts(PG_FUNCTION_ARGS)
{
	int64		session = PG_GETARG_INT64(0);
	int			nsegments = GpClusterSegmentCount();
	int			content = -1;
	int			nasked;
	char	  **values;
	int64		logged = 0;

	if (GpClusterContentId() >= 0)
		PG_RETURN_INT64(log_session_memory_contexts((int) session));

	if (GpClusterIsSingleNode())
		ereport(ERROR,
				(errmsg("this function does not work in utility mode")));

	if (PG_NARGS() > 1)
	{
		content = (int) PG_GETARG_INT64(1);
		if (content < 0 || content >= nsegments)
		{
			ereport(WARNING,
					(errmsg("\"%i\" is not a valid content ID", content)));
			PG_RETURN_INT64(0);
		}
	}

	nasked = (content >= 0) ? 1 : nsegments;
	values = palloc0_array(char *, nasked);
	GpDispatchQueryFirstValues(content >= 0 ?
							   psprintf("SELECT pg_catalog.gp_log_backend_memory_contexts(" INT64_FORMAT ", %d)",
										session, content) :
							   psprintf("SELECT pg_catalog.gp_log_backend_memory_contexts(" INT64_FORMAT ")",
										session),
							   content, values);

	for (int i = 0; i < nasked; i++)
	{
		int			asked = (content >= 0) ? content : i;

		if (values[i] != NULL && atoi(values[i]) == asked)
			logged++;
		else
			ereport(WARNING,
					(errmsg("unable to log memory contexts for session: \"%i\", on contentID: \"%i\"",
							(int) session, asked)));
	}

	PG_RETURN_INT64(logged);
}

/* ------------------------------------------------------------------------- */
/* gp_get_suboverflowed_backends()                                           */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_get_suboverflowed_backends);

/*
 * gp_get_suboverflowed_backends(): the processes of this node whose
 * transaction has more subtransactions with an XID than its PGPROC caches,
 * whose snapshots then have to look in pg_subtrans; NULL for none.  Each
 * PGPROC's own flag is read, which PostgreSQL 19 keeps beside the dense
 * array of the ProcArray's that Cloudberry reads by the PGPROC's index.
 */
Datum
gp_get_suboverflowed_backends(PG_FUNCTION_ARGS)
{
	ArrayBuildState *astate = NULL;

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (int i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];

		if (proc->pid != 0 && proc->subxidStatus.overflowed)
			astate = accumArrayResult(astate, Int32GetDatum(proc->pid), false,
									  INT4OID, CurrentMemoryContext);
	}
	LWLockRelease(ProcArrayLock);

	if (astate == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}
