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
 * Then they go to every segment at once, where no INSERT into the table
 * waits for another -- or else one segment after another, so that two
 * statements cannot wait for each other on different segments
 * (router_sends_at_once()).
 *
 * COPY FROM routes its rows the same way, parsed by PostgreSQL's own COPY.
 * Its data comes from the client, a file or a program, and no segment: so
 * where the rows may go to every segment at once they go as they are read,
 * each segment's COPY begun with its first rows, as Cloudberry's COPY sends
 * each row as it parses it (cdbCopySendData(), cdbcopy.c) -- and nothing of
 * the data is kept here but a piece a segment.  Each row goes with its line
 * in the data, which the segment says where the row fails there
 * (gp_copyin.c).  Where the rows must go one segment after another, the
 * first segment's go as they are read, and the others' are held until the
 * data is read and go after it, in their order, as an INSERT's do; and all
 * are held where a value the COPY computes for a row might itself run a
 * query -- a DEFAULT or a domain's CHECK that calls a function not written
 * in C -- for which the segments' connections must be free
 * (copy_may_query()).
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

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/matview.h"
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
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_log.h"
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

/*
 * A segment's rows on their way as they are read (router_stream_row()): the
 * piece of them being filled, as that segment's COPY reads them; a
 * replicated table has one of these for all its segments.
 */
typedef struct RouterSeg
{
	bool		any;			/* it has been put a row */
	bool		begun;			/* the segment's COPY is begun */
	StringInfoData buf;			/* the piece */
	StringInfoData lines;		/* its rows' lines (router_put_line()) */
	uint32		nrows;			/* rows in the piece */
	uint64		lastline;		/* the line of the last row put */
} RouterSeg;

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
	 * A COPY's: each row's line in the data, kept after its columns and
	 * sent with the row, which the segment says where a row fails there
	 * (gp_copyin.c, router_error_context()).
	 */
	bool		lines;
	Datum	   *linevalues;
	bool	   *linenulls;
	CopyFromState cstate;
	uint64		error_lineno;

	/*
	 * The segments are being talked to: an error is theirs, not one of the
	 * line the COPY is reading (router_copy_context()).
	 */
	bool		sending;

	/*
	 * A COPY's rows may go as they are read, where reading them runs
	 * nothing on the segments (copy_may_query()); whether they do is
	 * decided once it has read a piece's worth of its data
	 * (router_decide()).  Then each segment's go through its "segs" -- or
	 * the first segment's alone, where they go one segment after another
	 * ("first_only"), the others' held.
	 */
	bool		may_stream;
	bool		decided;
	bool		streaming;
	bool		first_only;
	RouterSeg  *segs;
	MemoryContext rowcxt;		/* a row's values as text or bytes, per row */

	/*
	 * An INSERT of one row reaches the segment it routes the row to, which
	 * only routing says: the statement's plan, for GpReportDtxReached(), or
	 * NULL where the statement noted its segments as it began.
	 */
	struct PlannedStmt *reached_stmt;
} GpRouter;

/*
 * May a statement's rows go to every segment at once?
 *
 * Sent one segment after another, a statement waits at a segment while it
 * holds locks only on the segments before it, so two statements never wait
 * for each other on different segments, where no segment's own deadlock
 * check finds them and only the global deadlock detector does.  Sent at
 * once they can: each first, on a different segment, with the same key of
 * a unique index, or in whatever a trigger locks.
 *
 * So at once with the detector on, which is what it is for; and without it
 * where nothing has one INSERT into the table wait for another: no unique
 * or exclusion index, and no trigger an INSERT fires, on the table or, a
 * partitioned table's rows going to its partitions on the segment, on any
 * of them.  A table of facts loaded in bulk is usually such a table; one
 * with a primary key is loaded in the order of its segments, each segment's
 * time after the last one's.
 *
 * Asked only of rows that go to more than one segment: a row of an INSERT
 * of one opens no partition here to learn it.
 */
static bool
router_sends_at_once(Relation rel)
{
	List	   *members = list_make1_oid(RelationGetRelid(rel));

	if (gp_enable_global_deadlock_detector)
		return true;
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		members = find_all_inheritors(RelationGetRelid(rel), RowExclusiveLock, NULL);

	foreach_oid(member, members)
	{
		Relation	m = table_open(member, NoLock);
		TriggerDesc *trig = m->trigdesc;
		bool		waits;

		waits = trig != NULL &&
			(trig->trig_insert_before_row || trig->trig_insert_after_row ||
			 trig->trig_insert_instead_row || trig->trig_insert_before_statement ||
			 trig->trig_insert_after_statement);
		foreach_oid(indexoid, RelationGetIndexList(m))
		{
			HeapTuple	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));

			if (!HeapTupleIsValid(tuple))
				continue;
			if (((Form_pg_index) GETSTRUCT(tuple))->indisunique ||
				((Form_pg_index) GETSTRUCT(tuple))->indisexclusion)
				waits = true;
			ReleaseSysCache(tuple);
		}
		table_close(m, NoLock);
		if (waits)
			return false;
	}
	return true;
}

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
	r->segs = palloc0_array(RouterSeg, r->nsegs);
	r->rowcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "gp_core routed row",
									  ALLOCSET_DEFAULT_SIZES);
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
	appendStringInfo(&sql, "%s FROM STDIN", first ? "" : ")");
	/* a COPY's rows come with their lines: the segment's gp_copyin.c */
	if (r->binary && lines)
		appendStringInfoString(&sql, " (FORMAT binary, " GP_COPY_LINES_OPTION ")");
	else if (r->binary)
		appendStringInfoString(&sql, " (FORMAT binary)");
	else if (lines)
		appendStringInfoString(&sql, " (" GP_COPY_LINES_OPTION ")");
	/*
	 * And the client's statement, in the comment a gather's cursor ends
	 * with, which the segment's log names and its part's phases show
	 * (gp_log.c, gp_dtx.c).
	 */
	appendStringInfoString(&sql, GpLogStatementComment());
	r->copy_sql = sql.data;

	return r;
}

/*
 * The context of an error a segment raised in the rows routed to it.  Its
 * last line is the segment's COPY's -- "COPY t, line n", the segment's
 * count of the rows it was sent -- which is no line of the statement's: an
 * INSERT's error says none, as Cloudberry's does, whose segments insert the
 * rows themselves, and a COPY's the line of its data the row came from
 * (router_copy_context()), which the segment said as it failed.  The rest --
 * a trigger's, a function's -- stays.  The segment's words are PostgreSQL's
 * untranslated; a line in another language is left as it is.
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
	if (r->lines && strncmp(after, ", line ", 7) == 0)
		r->error_lineno = GpCopyInFailedLine(content);
	return last == context ? NULL : pnstrdup(context, last - context - 1);
}

/*
 * The COPY's own context: where it is in its data, as COPY says where it
 * failed (CopyFromErrorCallback()) -- or, for an error a segment raised in
 * a row routed there, the line of the data that row came from.  While the
 * segments are talked to an error is theirs or of the way to them, and no
 * line's but that one.
 */
static void
router_copy_context(void *arg)
{
	GpRouter   *r = (GpRouter *) arg;
	CopyFromState cstate = r->cstate;

	if (cstate == NULL)
		return;
	if (r->sending)
	{
		if (r->error_lineno == 0)
			return;
		cstate->cur_lineno = r->error_lineno;
		cstate->line_buf_valid = false;
		cstate->cur_attname = NULL;
		cstate->relname_only = false;
	}
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
				bytea	   *out = GpSendFunctionCall(&r->out[i], slot->tts_values[i]);

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

/*
 * Does a piece of the rows as they are read go to every segment: a
 * replicated table's, unless the first segment's alone go so?
 */
static inline bool
router_stream_all(GpRouter *r)
{
	return r->replicated && !r->first_only;
}

/* What a segment's rows as they are read are in: one, where they go to all. */
static inline RouterSeg *
router_seg(GpRouter *r, int seg)
{
	return &r->segs[router_stream_all(r) ? 0 : seg];
}

/* A row's line onto a piece's: the difference, zigzag, seven bits a byte. */
static void
router_put_line(StringInfo lines, uint64 *lastline, uint64 lineno)
{
	int64		delta = (int64) (lineno - *lastline);
	uint64		v = ((uint64) delta << 1) ^ (uint64) (delta >> 63);

	do
	{
		unsigned char b = v & 0x7F;

		v >>= 7;
		appendStringInfoChar(lines, v != 0 ? (b | 0x80) : b);
	} while (v != 0);
	*lastline = lineno;
}

/* The start of a segment's data: a binary COPY's signature. */
static void
router_open_data(GpRouter *r, StringInfo buf)
{
	if (!r->binary)
		return;
	/* PGCOPY\n\377\r\n\0, no flags, no header extension */
	appendBinaryStringInfo(buf, "PGCOPY\n\377\r\n\0", 11);
	pq_sendint32(buf, 0);
	pq_sendint32(buf, 0);
}

/*
 * A piece, made: a COPY's ends with its rows' lines, how many rows they
 * are, and how many bytes the lines take, as gp_copyin.c reads them.
 */
static void
router_close_piece(GpRouter *r, StringInfo buf, StringInfo lines, uint32 nrows)
{
	if (!r->lines)
		return;
	appendBinaryStringInfo(buf, lines->data, lines->len);
	pq_sendint32(buf, nrows);
	pq_sendint32(buf, lines->len);
}

/*
 * A COPY whose rows go as they are read has met a segment whose COPY
 * failed: nothing more is read or sent.  The COPYs are ended, every other
 * segment's as stopped short of its rows, and the end raises the error.
 */
pg_noreturn static void
router_stream_fail(GpRouter *r)
{
	uint64	   *counts = palloc0_array(uint64, r->nsegs);

	for (int seg = 0; seg < r->nsegs; seg++)
		if (router_seg(r, seg)->begun)
			GpCopyInStop(seg);
	GpCopyInEnd(counts);
	elog(ERROR, "a segment said the rows sent to it had failed, and raised no error");
}

/*
 * Send a segment's piece now -- a replicated table's to every segment, where
 * they all take it as it is read -- and begin the next: the segment's COPY
 * is begun with its first piece, and the piece waits until the last one
 * sent there has left, so that no more is held here than a piece a
 * segment, and a segment that reads slowly slows the reading of the data
 * to what it takes, as Cloudberry's COPY is slowed by a segment it cannot
 * send to (cdbCopySendData(), cdbcopy.c).
 */
static void
router_stream_piece(GpRouter *r, int seg)
{
	RouterSeg  *rs = router_seg(r, seg);
	int			first = router_stream_all(r) ? 0 : seg;
	int			last = router_stream_all(r) ? r->nsegs - 1 : seg;
	bool		was_sending = r->sending;
	bool		ready;

	router_close_piece(r, &rs->buf, &rs->lines, rs->nrows);

	r->sending = true;
	/* not one more segment's COPY for rows that have failed on another */
	if (GpCopyInFailed())
		router_stream_fail(r);
	if (!rs->begun)
	{
		int		   *segs = palloc_array(int, last - first + 1);

		for (int s = first; s <= last; s++)
			segs[s - first] = s;
		GpCopyInBegin(segs, last - first + 1, r->copy_sql,
					  router_error_context, r);
		pfree(segs);
		rs->begun = true;
	}

	for (;;)
	{
		if (GpCopyInFailed())
			router_stream_fail(r);
		ready = true;
		for (int s = first; s <= last && ready; s++)
			ready = GpCopyInReady(s);
		if (ready)
			break;
		GpCopyInWait();
	}
	for (int s = first; s <= last; s++)
		GpCopyInData(s, rs->buf.data, rs->buf.len);
	r->sending = was_sending;

	resetStringInfo(&rs->buf);
	resetStringInfo(&rs->lines);
	rs->nrows = 0;
}

/*
 * One row on its way now: onto its segment's piece, as COPY reads it, and
 * the piece sent when it has filled.  The slot is the table's columns, and
 * after them, of a row that was held (router_decide()), its line.
 */
static void
router_stream_row(GpRouter *r, int seg, TupleTableSlot *slot, uint64 lineno)
{
	RouterSeg  *rs = router_seg(r, seg);
	MemoryContext oldcxt;

	if (!rs->any)
	{
		oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(r));
		initStringInfo(&rs->buf);
		initStringInfo(&rs->lines);
		MemoryContextSwitchTo(oldcxt);
		router_open_data(r, &rs->buf);
		rs->any = true;
	}

	/* what its columns' output functions leave goes with the row */
	oldcxt = MemoryContextSwitchTo(r->rowcxt);
	router_encode(r, slot, &rs->buf);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(r->rowcxt);

	if (r->lines)
		router_put_line(&rs->lines, &rs->lastline, lineno);
	rs->nrows++;

	if (rs->buf.len >= ROUTE_CHUNK)
		router_stream_piece(r, seg);
}

/*
 * A COPY has read a piece's worth of its data, and its rows may go as they
 * are read (copy_may_query()): which do?  Every segment's, where they may go
 * to every segment at once (router_sends_at_once()), or there is one
 * segment.  Otherwise the first segment's: the others' are held to the
 * data's end and go after it, one segment after another in their order, as
 * an INSERT's do once its plan has finished with the segments.  The first
 * segment's COPY is still the first begun, and ends before the next is, so
 * the order that keeps two such statements from waiting for each other on
 * different segments is kept; and that segment loads while the data is
 * read, its share of the rows never held.  A replicated table's rows are
 * every segment's: held for the others as they go to the first.  The rows
 * held so far for a segment whose rows go now are sent on, and every row
 * after them as it is put.
 *
 * A COPY of less data than that decides nothing: its rows go as it ends,
 * as they went, to the segments it has rows for and no other.
 */
static void
router_decide(GpRouter *r)
{
	int			natts = RelationGetDescr(r->rel)->natts;

	r->decided = true;
	r->streaming = true;
	r->first_only = r->nsegs > 1 && !router_sends_at_once(r->rel);

	for (int seg = 0; seg < (r->first_only ? 1 : r->nsegs); seg++)
	{
		Tuplestorestate *store = r->stores[seg];

		if (store == NULL)
			continue;
		tuplestore_rescan(store);
		while (tuplestore_gettupleslot(store, true, false, r->slot))
		{
			bool		isnull;
			uint64		line = 0;

			slot_getallattrs(r->slot);
			if (r->lines)
				line = (uint64) DatumGetInt64(slot_getattr(r->slot, natts + 1, &isnull));
			router_stream_row(r, seg, r->slot, line);
		}
		if (r->first_only && r->replicated)
			continue;
		tuplestore_end(store);
		r->stores[seg] = NULL;
	}
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
	r->nrows++;

	if (r->streaming && (!r->first_only || seg == 0))
	{
		router_stream_row(r, seg, slot, lineno);
		/* a replicated table's row is the segments' after the first too */
		if (!r->first_only || !r->replicated)
			return;
	}

	if (r->stores[seg] == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(r));

		/*
		 * work_mem is the statement's for its rows on their way, not each
		 * segment's: a store has its part of it, and a replicated table's
		 * one store the whole.  64 kB is the least work_mem itself can be.
		 */
		r->stores[seg] = tuplestore_begin_heap(false, false,
											   r->replicated ? work_mem
											   : Max(work_mem / r->nsegs, 64));
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

	/* its data is past a piece: router_decide() */
	if (r->may_stream && !r->decided &&
		r->cstate->bytes_processed > ROUTE_CHUNK)
		router_decide(r);
}

/*
 * A piece more of a store's rows, as COPY reads them, in buf; false once the
 * store has no more after it.  "first" is its first piece.  A COPY's rows'
 * lines end the piece (router_close_piece()), counted on from *lastline.
 */
static bool
router_next_piece(GpRouter *r, Tuplestorestate *store, bool first,
				  StringInfo buf, StringInfo lines, uint64 *lastline,
				  MemoryContext rowcxt)
{
	int			natts = RelationGetDescr(r->rel)->natts;
	uint32		nrows = 0;
	bool		more = true;

	resetStringInfo(buf);
	resetStringInfo(lines);
	if (first)
	{
		tuplestore_rescan(store);
		*lastline = 0;
		router_open_data(r, buf);
	}

	while (buf->len < ROUTE_CHUNK)
	{
		MemoryContext oldcxt;

		if (!tuplestore_gettupleslot(store, true, false, r->slot))
		{
			if (r->binary)
				pq_sendint16(buf, -1);
			more = false;
			break;
		}
		oldcxt = MemoryContextSwitchTo(rowcxt);
		router_encode(r, r->slot, buf);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(rowcxt);
		if (r->lines)
		{
			bool		isnull;

			router_put_line(lines, lastline,
							(uint64) DatumGetInt64(slot_getattr(r->slot, natts + 1, &isnull)));
		}
		nrows++;
	}
	if (buf->len > 0)
		router_close_piece(r, buf, lines, nrows);
	return more;
}

/* Can every segment a store's rows go to be given a piece more? */
static bool
router_ready(GpRouter *r, const int *segs, int nsegs, int i)
{
	if (!r->replicated)
		return GpCopyInReady(segs[i]);
	for (int j = 0; j < nsegs; j++)
		if (!GpCopyInReady(segs[j]))
			return false;
	return true;
}

/*
 * Send the segments listed the rows held for them, all of them at once: a
 * piece of each segment's as that segment can take one, so that each loads
 * while the others are sent theirs, and the statement takes its slowest
 * segment's time, not their times together -- nor does a segment that reads
 * slowly hold back the rows of the others.  A replicated table's rows are
 * one store's, each piece of which goes to every segment listed, as the
 * slowest of them takes it.  Answer how many rows the segments took; of a
 * replicated table, how many one of them did.
 *
 * Once a segment says its COPY has failed, nothing more is sent: the COPYs
 * are ended, those not yet sent all their rows as failed, and the end
 * raises the error -- the first segment's in their order of those that had
 * one by then.  A statement of few rows has sent them all before any
 * segment answers, and says what it said when they went in that order.
 */
static uint64
router_send(GpRouter *r, const int *segs, int nsegs)
{
	int			nstores = r->replicated ? 1 : nsegs;
	bool	   *more = palloc_array(bool, nstores);
	bool	   *begun = palloc0_array(bool, nstores);
	uint64	   *lastline = palloc0_array(uint64, nstores);
	uint64	   *counts = palloc0_array(uint64, nsegs);
	uint64		total = 0;
	bool		any;
	bool		failed = false;
	StringInfoData buf;
	StringInfoData lines;

	GpCopyInBegin(segs, nsegs, r->copy_sql, router_error_context, r);

	initStringInfo(&buf);
	initStringInfo(&lines);
	memset(more, true, nstores * sizeof(bool));
	do
	{
		bool		sent = false;

		any = false;
		for (int i = 0; i < nstores && !failed; i++)
		{
			if (!more[i])
				continue;
			any = true;
			if (!router_ready(r, segs, nsegs, i))
				continue;
			more[i] = router_next_piece(r, r->stores[r->replicated ? 0 : segs[i]],
										!begun[i], &buf, &lines, &lastline[i],
										r->rowcxt);
			begun[i] = true;
			sent = true;
			if (buf.len > 0)
			{
				if (r->replicated)
				{
					for (int j = 0; j < nsegs; j++)
						GpCopyInData(segs[j], buf.data, buf.len);
				}
				else
					GpCopyInData(segs[i], buf.data, buf.len);
			}
			CHECK_FOR_INTERRUPTS();
			failed = GpCopyInFailed();
		}
		if (any && !sent && !failed)
		{
			GpCopyInWait();
			failed = GpCopyInFailed();
		}
	} while (any && !failed);

	if (failed)
	{
		for (int i = 0; i < nsegs; i++)
			if (more[r->replicated ? 0 : i])
				GpCopyInStop(segs[i]);
	}

	pfree(buf.data);
	pfree(lines.data);
	GpCopyInEnd(counts);

	for (int i = 0; i < nsegs; i++)
		total = r->replicated ? counts[i] : total + counts[i];
	pfree(counts);
	pfree(more);
	pfree(begun);
	pfree(lastline);
	return total;
}

/*
 * The statement has no more rows.  Send every segment its own -- what is
 * left of them, where they went as they were read; answer how many rows the
 * statement wrote: those the segments took, which a BEFORE row trigger that
 * answers NULL leaves fewer than were sent, as PostgreSQL's INSERT and COPY
 * count them.  Every segment takes a replicated table's every row, and the
 * statement wrote each once.
 */
static uint64
router_finish(GpRouter *r)
{
	int		   *segs = palloc_array(int, r->nsegs);
	int			nsegs = 0;
	uint64		total = 0;

	r->sending = true;

	/*
	 * Gone as they were read: each segment's last piece, and the end -- the
	 * first segment's alone, where the others' are held to go after it.
	 */
	if (r->streaming)
	{
		uint64	   *counts = palloc0_array(uint64, r->nsegs);

		for (int seg = 0; seg < (router_stream_all(r) ? 1 : r->nsegs); seg++)
		{
			RouterSeg  *rs = &r->segs[seg];

			if (!rs->any)
				continue;
			if (r->binary)
				pq_sendint16(&rs->buf, -1);
			if (rs->buf.len > 0)
				router_stream_piece(r, seg);
		}
		for (int seg = 0; seg < r->nsegs; seg++)
			if (router_seg(r, seg)->any)
				nsegs++;
		if (GpCopyInFailed())
			router_stream_fail(r);
		/* none, where the first segment had no rows to go as they were read */
		if (nsegs > 0)
			GpCopyInEnd(counts);
		for (int i = 0; i < nsegs; i++)
			total = r->replicated ? counts[i] : total + counts[i];
		pfree(counts);
		if (!r->first_only)
			return total;
		nsegs = 0;
	}

	for (int seg = r->first_only ? 1 : 0; seg < r->nsegs; seg++)
		if (r->stores[r->replicated ? 0 : seg] != NULL)
			segs[nsegs++] = seg;

	if (!r->first_only && nsegs > 1 && router_sends_at_once(r->rel))
	{
		if (r->reached_stmt != NULL && !r->replicated)
			GpReportDtxReached(r->reached_stmt, segs, nsegs);
		return router_send(r, segs, nsegs);
	}

	/* One segment after another, in their order */
	for (int i = 0; i < nsegs; i++)
	{
		uint64		took;

		if (r->reached_stmt != NULL && !r->replicated)
			GpReportDtxReached(r->reached_stmt, &segs[i], 1);
		took = router_send(r, &segs[i], 1);
		total = r->replicated ? took : total + took;
	}
	return total;
}

static void
router_end(GpRouter *r)
{
	for (int seg = 0; seg < r->nsegs; seg++)
		if (r->stores[seg] != NULL)
			tuplestore_end(r->stores[seg]);
	ExecDropSingleTupleTableSlot(r->slot);
	MemoryContextDelete(r->rowcxt);
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

	GpModifyCheckTarget(relid);
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

/*
 * A write's target, as PostgreSQL's CheckValidResultRel() checks it where a
 * ModifyTable starts, before the plan below it makes a row: a materialized
 * view is written under its maintenance alone -- a REFRESH's, or an
 * incremental view's own -- and refused in PostgreSQL's words otherwise, and
 * under EXPLAIN too.  The writes here stand in for the ModifyTable; without
 * this the segments' COPY refused the view in its own words, once the rows
 * were made and routed.
 */
void
GpModifyCheckTarget(Oid relid)
{
	if (get_rel_relkind(relid) == RELKIND_MATVIEW &&
		!MatViewIncrementalMaintenanceIsEnabled())
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change materialized view \"%s\"",
						get_rel_name(relid))));
}

static void
modify_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpModifyCheckTarget(((ModifyState *) node)->relid);
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
 * changes?  Only if every table it reads is the one it changes, as the row
 * it changes, or one that every segment holds whole.  Returns NULL if so, or
 * why not.
 */
static const char current_of_reason[] =
	"WHERE CURRENT OF names a row by the cursor that read it, which is here.";

typedef struct PushContext
{
	Oid			target;
	RangeTblEntry *target_rte;	/* the statement's own entry of it */
	bool		target_whole;	/* replicated: every segment holds it whole */
	const char *why;
} PushContext;

static const char *push_reads_target_again(Oid relid);

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

		/*
		 * The table it changes, read again -- in a subquery, a join, a
		 * scalar subquery's max() -- is each segment's share of it there,
		 * not the table: SET d = (SELECT max(d) FROM t) would be each
		 * segment's own maximum (push_reads_target_again()).
		 */
		if (rte->rtekind == RTE_RELATION && rte->relid == cxt->target &&
			rte != cxt->target_rte && !cxt->target_whole)
		{
			cxt->why = push_reads_target_again(rte->relid);
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

	cxt.target_rte = query->resultRelation > 0
		? rt_fetch(query->resultRelation, query->rtable) : NULL;
	cxt.target_whole = GpPolicyIsReplicated(policy);

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
 * Is the table append-optimized -- of gp_ao's access methods, which gp_core
 * knows by name, as it loads without gp_ao?
 */
static bool
relation_is_append_optimized(Oid relid)
{
	Oid			relam = get_rel_relam(relid);
	char	   *amname;
	bool		result;

	if (!OidIsValid(relam))
		return false;
	amname = get_am_name(relam);
	result = amname != NULL &&
		(strcmp(amname, "ao_row") == 0 || strcmp(amname, "ao_column") == 0);
	if (amname != NULL)
		pfree(amname);
	return result;
}

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
 * are each on one segment, and no append-optimized table, whose rows no
 * one can lock (parse_relation.c, RelationIsAppendOptimized()).  The
 * segments lock them, the gather sending its locking clause with its query
 * (gp_scan.c).
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
		has_subclass(rte->relid) || relation_is_append_optimized(rte->relid))
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

static void refuse_utility_key_update(Query *parse);
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
 * Refused, with the reason, where that cannot be done.  "planned": the plan
 * is PostgreSQL's planner's, which a row another transaction updated can be
 * rechecked by, as its ModifyTable rechecks one; not ORCA's.
 */
static Plan *
write_explicitly(PlannedStmt *stmt, ModifyTable *mt, const char *on_conflict,
				 bool planned)
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
	return GpExplicitMake(mt, on_conflict, planned);
}

/* The explicit write for a ModifyTable ORCA's translator made (merge.c). */
Plan *
GpModifyWriteExplicitly(PlannedStmt *stmt, Plan *modify)
{
	return write_explicitly(stmt, castNode(ModifyTable, modify), NULL, false);
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

	refuse_utility_key_update(parse);

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
			lfirst(lc) = write_explicitly(stmt, (ModifyTable *) sub, on_conflict,
										  true);
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
	 * gather a distributed table again, made a join, and a NOT IN an
	 * anti-join (gp_subselect.c), as Cloudberry's planner makes them.
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
		stmt->planTree = write_explicitly(stmt, mt, NULL, true);
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
			stmt->planTree = write_explicitly(stmt, mt, on_conflict, true);
			return stmt;
		}

		/*
		 * The segments fire the row triggers, each for its own rows, as
		 * Cloudberry's do.  A statement trigger has no segment to be fired
		 * on once, and firing it on each would fire it once per segment --
		 * but for an internal one, gp_matview's, which keeps each segment's
		 * rows for the coordinator's maintenance of an incremental view.
		 */
		rel = table_open(rte->relid, NoLock);
		for (int i = 0; rel->trigdesc != NULL && i < rel->trigdesc->numtriggers; i++)
		{
			Trigger    *trig = &rel->trigdesc->triggers[i];

			if (!trig->tgisinternal && !TRIGGER_FOR_ROW(trig->tgtype) &&
				TRIGGER_FOR_INSERT(trig->tgtype))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statement-level triggers on distributed table \"%s\" are not supported",
								RelationGetRelationName(rel))));
		}
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
			stmt->planTree = write_explicitly(stmt, mt, NULL, true);
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
		stmt->planTree = write_explicitly(stmt, mt, NULL, true);

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
void
GpCopyFromCheckPermissions(ParseState *pstate, CopyStmt *stmt, Relation rel)
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

	GpCopyFromCheckPermissions(pstate, stmt, rel);
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
 * Might calling this function run a query?  One of a procedural language
 * might, and one of SQL's that the planner left a call.  One built in or
 * written in C is taken not to -- but for PostgreSQL's own that run a query
 * they are given, or read a table they are named: the functions that map
 * tables and queries to XML, and the two of text search.  An extension's
 * function in C that runs a query on the segments is not known here, and
 * is refused if it does it while a COPY's rows go there (conn_still_busy(),
 * gp_dispatch.c).
 */
static bool
copy_function_may_query(Oid funcid, void *context)
{
	HeapTuple	tuple;
	Oid			lang;

	switch (funcid)
	{
		case F_TABLE_TO_XML:
		case F_QUERY_TO_XML:
		case F_CURSOR_TO_XML:
		case F_TABLE_TO_XMLSCHEMA:
		case F_QUERY_TO_XMLSCHEMA:
		case F_CURSOR_TO_XMLSCHEMA:
		case F_TABLE_TO_XML_AND_XMLSCHEMA:
		case F_QUERY_TO_XML_AND_XMLSCHEMA:
		case F_SCHEMA_TO_XML:
		case F_SCHEMA_TO_XMLSCHEMA:
		case F_SCHEMA_TO_XML_AND_XMLSCHEMA:
		case F_DATABASE_TO_XML:
		case F_DATABASE_TO_XMLSCHEMA:
		case F_DATABASE_TO_XML_AND_XMLSCHEMA:
		case F_TS_REWRITE_TSQUERY_TEXT:
		case F_TS_STAT_TEXT:
		case F_TS_STAT_TEXT_TEXT:
			return true;
		default:
			break;
	}

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		return true;
	lang = ((Form_pg_proc) GETSTRUCT(tuple))->prolang;
	ReleaseSysCache(tuple);
	return lang != INTERNALlanguageId && lang != ClanguageId;
}

static bool copy_type_may_query(Oid typid, int depth);

static bool
copy_expr_may_query(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (check_functions_in_node(node, copy_function_may_query, context))
		return true;
	if (IsA(node, SubLink) || IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, CoerceToDomain) &&
		copy_type_may_query(((CoerceToDomain *) node)->resulttype, 0))
		return true;
	if (IsA(node, CoerceViaIO) &&
		copy_type_may_query(((CoerceViaIO *) node)->resulttype, 0))
		return true;
	return expression_tree_walker(node, copy_expr_may_query, context);
}

/*
 * Might reading a value of this type run a query: a domain whose CHECK
 * might, or a type with one inside -- an array's elements, a composite's
 * columns, a range's bounds.
 */
static bool
copy_type_may_query(Oid typid, int depth)
{
	Oid			elem;

	if (depth > 16)
		return true;
	switch (get_typtype(typid))
	{
		case TYPTYPE_DOMAIN:
			{
				DomainConstraintRef *ref = palloc_object(DomainConstraintRef);

				/* its own constraints and those of the domains under it */
				InitDomainConstraintRef(typid, ref, CurrentMemoryContext, false);
				foreach_node(DomainConstraintState, con, ref->constraints)
					if (con->check_expr != NULL &&
						copy_expr_may_query((Node *) con->check_expr, NULL))
						return true;
				return copy_type_may_query(getBaseType(typid), depth + 1);
			}
		case TYPTYPE_COMPOSITE:
			{
				TupleDesc	tupdesc = lookup_rowtype_tupdesc(typid, -1);
				bool		may = false;

				for (int i = 0; i < tupdesc->natts && !may; i++)
					may = !TupleDescAttr(tupdesc, i)->attisdropped &&
						copy_type_may_query(TupleDescAttr(tupdesc, i)->atttypid,
											depth + 1);
				ReleaseTupleDesc(tupdesc);
				return may;
			}
		case TYPTYPE_RANGE:
			return copy_type_may_query(get_range_subtype(typid), depth + 1);
		case TYPTYPE_MULTIRANGE:
			return copy_type_may_query(get_multirange_range(typid), depth + 1);
		default:
			elem = get_element_type(typid);
			return OidIsValid(elem) && copy_type_may_query(elem, depth + 1);
	}
}

/*
 * Might reading the COPY's data run a query?  Its rows go to the segments
 * as they are read only if not: a query of a distributed table needs the
 * connections the rows are going over, and a connection in COPY mode takes
 * nothing else.  What is computed for a row here is the DEFAULT of each
 * column the data leaves out -- or of any, with COPY's DEFAULT option --
 * and each value read, by its type's input function, which for a domain
 * checks its constraints.  A function of a procedural language, or in SQL
 * and not inlined, might read a table; nextval() takes the sequence here.
 */
static bool
copy_may_query(CopyFromState cstate, Relation rel)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (cstate->defexprs[i] != NULL &&
			copy_expr_may_query((Node *) cstate->defexprs[i]->expr, NULL))
			return true;
		if (list_member_int(cstate->attnumlist, i + 1) &&
			copy_type_may_query(att->atttypid, 0))
			return true;
	}
	return false;
}

/*
 * COPY t FROM: parsed by PostgreSQL's own COPY, as it would be into a local
 * table, and routed -- each row as it is read, where the router sends them
 * so (router_decide()).  The coordinator evaluates the defaults, as for
 * INSERT, so that a serial column has one sequence.  With Cloudberry's
 * options, read through gp_exttable's filter (above).  An error in the data
 * says where, as COPY's own does, and so does one a segment raises in a row
 * routed there (router_copy_context()): the line of the data the row came
 * from.
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
	ErrorContextCallback readcallback;
	ErrorContextCallback routecallback;
	uint64		processed;

	if (sreh != NULL)
		cstate = copy_begin_sreh(pstate, stmt, rel, sreh);
	else
		cstate = BeginCopyFrom(pstate, rel, NULL, stmt->filename, stmt->is_program,
							   NULL, stmt->attlist, stmt->options);
	router = router_begin(rel, policy, true);
	router->cstate = cstate;
	router->may_stream = !copy_may_query(cstate, rel);
	/* Cloudberry's COPY is sent to every segment of the table */
	GpReportDtxReached(NULL, NULL, policy->numsegments);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	econtext = GetPerTupleExprContext(estate);

	/*
	 * A row is read under COPY's own context, by its own name: the filter
	 * knows it by that, and leaves it out of an error of its own reading
	 * (loader_read(), copysreh.c).  And routed under the router's, which is
	 * that one until the row is on its way to a segment.
	 */
	readcallback.callback = CopyFromErrorCallback;
	readcallback.arg = cstate;
	readcallback.previous = error_context_stack;
	routecallback.callback = router_copy_context;
	routecallback.arg = router;
	routecallback.previous = error_context_stack;
	error_context_stack = &routecallback;
	for (;;)
	{
		MemoryContext oldcxt;
		bool		got;

		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		ExecClearTuple(slot);

		/*
		 * The row's values in the per-tuple context, where NextCopyFrom()
		 * computes a column's DEFAULT, as CopyFrom() calls it: gone with
		 * the next row, once the router has sent this one on.
		 */
		error_context_stack = &readcallback;
		oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		got = NextCopyFrom(cstate, econtext, slot->tts_values, slot->tts_isnull);
		MemoryContextSwitchTo(oldcxt);
		error_context_stack = &routecallback;
		if (!got)
			break;
		ExecStoreVirtualTuple(slot);
		router_put(router, slot, cstate->cur_lineno);
	}
	processed = router_finish(router);
	error_context_stack = routecallback.previous;

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

	/* a segment's COPY of rows the coordinator sends with their lines */
	if (GpClusterIsDispatched() && GpCopyInIsRouted(parsetree))
	{
		uint64		processed = GpCopyInRouted(pstmt, queryString, queryEnv);

		if (qc)
			SetQueryCompletion(qc, CMDTAG_COPY, processed);
		return;
	}

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
			GpCopyFromCheckPermissions(pstate, stmt, rel);
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

/*
 * The hook as it is set: the mode above, and then Cloudberry's fault at the
 * moment its parser or rewriter has locked a relation it opens, in that mode
 * (CdbTryOpenTable(), table.c, which parserOpenTable() and
 * AcquireRewriteLocks() call): a test sleeps there, holding an UPDATE's
 * RowExclusiveLock with the global deadlock detector on, while another
 * session's UPDATE of the table goes on beside it (gdd/avoid-qd-deadlock).
 * The core takes the lock as the hook returns, so it is taken here first,
 * where a fault is set, in the mode the core will take (queryLockMode(),
 * parse_relation.c): the parser's own where the hook chose a weaker one for
 * a relation the query writes.
 */
static LOCKMODE
gp_modify_query_lockmode_fault(Oid relid, LOCKMODE lockmode,
							   AclMode requiredPerms)
{
	LOCKMODE	chosen = gp_modify_query_lockmode(relid, lockmode, requiredPerms);

	if (gp_fault_active != NULL && *gp_fault_active > 0)
	{
		LockRelationOid(relid, lockmode >= RowExclusiveLock && chosen < lockmode
						? lockmode : chosen);
		(void) GP_FAULT("upgrade_row_lock");
	}
	return chosen;
}

/*
 * An UPDATE of a hash-distributed table's distribution key, in a session of
 * a segment's own: refused, as Cloudberry's utility mode refuses it
 * (create_modifytable_path(), pathnode.c) -- a row whose key changes may
 * belong on another segment, and only the coordinator's Split moves it
 * there.  A key column changes, as Cloudberry's check_splitupdate()
 * (preptlist.c) decides, where its new value is anything but the column
 * itself.  Cloudberry decides it once the UPDATE is planned, and lets one
 * whose every path is a dummy -- of no row, WHERE false -- go; here it is
 * decided before planning, and such an UPDATE is refused too.
 */
static void
refuse_utility_key_update(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy   *policy;
	ListCell   *lc;

	if (parse->commandType != CMD_UPDATE || parse->resultRelation <= 0 ||
		GpClusterContentId() < 0 ||
		GpClusterBackendRole() != GP_ROLE_UTILITY)
		return;
	rte = rt_fetch(parse->resultRelation, parse->rtable);
	policy = GpPolicyGet(rte->relid);
	if (!GpPolicyIsHashPartitioned(policy))
		return;

	foreach(lc, parse->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *var = (Var *) tle->expr;

		if (tle->resjunk)
			continue;
		if (IsA(var, Var) && var->varno == parse->resultRelation &&
			var->varattno == tle->resno && var->varlevelsup == 0)
			continue;
		for (int i = 0; i < policy->nattrs; i++)
			if (policy->attrs[i] == tle->resno)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot update distribution key columns in utility mode")));
	}
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
	query_lockmode_hook = gp_modify_query_lockmode_fault;
}

/*
 * Why a statement that reads the table it changes again, elsewhere than as
 * the row it changes, is not sent to the segments as it stands: what each
 * segment would read of it there is its own share.  Cloudberry's plan reads
 * the whole table there, through a Motion, as the coordinator's explicit
 * write does here (gp_explicit.c).  A replicated table is whole on every
 * segment, and is sent so still.
 */
static const char *
push_reads_target_again(Oid relid)
{
	return psprintf("It reads \"%s\", the table it changes, again, of which a segment holds its share.",
					get_rel_name(relid));
}
