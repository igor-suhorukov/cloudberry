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
 * gp_copyin.c
 *	  A segment's side of a COPY's rows the coordinator routes to it.
 *
 * The coordinator reads a COPY FROM's data and sends each segment its rows
 * by a COPY ... FROM STDIN of its own (gp_modify.c).  A row that fails on
 * the segment -- a constraint, a trigger, no partition for it -- fails
 * there as "COPY t, line n", n the segment's count of the rows it was sent,
 * and what the client is owed is the line of the data the row came from.
 * The coordinator found it in the rows it kept until its data was read; it
 * keeps none now, the rows going as they are read, so each row comes with
 * its line, as Cloudberry's does (copy_from_dispatch_row's lineno,
 * copy.c), and the segment says the line with its failure.
 *
 * PostgreSQL's COPY takes no such thing in its data, and none of its formats
 * is replaced: the segment's COPY is PostgreSQL's own, CopyFrom(), given its
 * data by a callback (copy_data_source_cb) that reads the coordinator's
 * messages as COPY itself reads a client's (CopyGetData()), keeps the lines
 * that end each of them, and hands CopyFrom() the rows.  A message is
 *
 *		rows, as COPY reads them
 *		a number for each row: its line less the line of the row before,
 *			zigzag, seven bits a byte
 *		int32	how many rows
 *		int32	how many bytes those numbers take
 *
 * the two counts last, where they are found without reading the rows.
 *
 * The lines are kept for the rows CopyFrom() has not finished with: those
 * it has read ahead, and those it holds to insert together, whose error
 * comes as they are flushed, under their own number
 * (CopyMultiInsertBufferFlush()).  How many that is PostgreSQL does not
 * say; ROUTED_KEEP_LINES behind the row being read is some sixty times the
 * thousand it holds.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_node.h"
#include "port/pg_bswap.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "gp_dispatch.h"
#include "gp_scan.h"

/* Rows behind the one being read whose lines are still kept. */
#define ROUTED_KEEP_LINES	65536

typedef struct RoutedCopy
{
	CopyFromState cstate;		/* NULL until BeginCopyFrom() returns */
	MemoryContext cxt;			/* the statement's: the lines outlive a row */
	StringInfoData msg;			/* the message being handed down */
	int			end;			/* where its rows end */
	bool		eof;			/* the coordinator ended the COPY */

	/*
	 * The lines of rows first + 1 to first + nlines, the segment's count of
	 * them, which is CopyFrom()'s line number of each.
	 */
	uint64	   *lines;
	int			nlines;
	int			maxlines;
	uint64		first;
	uint64		lastline;		/* the last row's */

	uint64		failed_row;		/* the row the last error was raised at */
} RoutedCopy;

/* The one going, a backend running one statement at a time. */
static RoutedCopy *routed = NULL;

/* The line the last one failed at, for GpCopyInRoutedFailedLine(). */
static uint64 routed_failed_line = 0;

/* The line of the segment's row "row", or 0 where it is not kept. */
static uint64
routed_line(RoutedCopy *rc, uint64 row)
{
	if (row <= rc->first || row > rc->first + rc->nlines)
		return 0;
	return rc->lines[row - rc->first - 1];
}

pg_noreturn static void
routed_bad_message(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("the coordinator's rows came without their lines")));
}

/* The lines that end the message just read; its rows end where they begin. */
static void
routed_take_lines(RoutedCopy *rc)
{
	uint32		nrows;
	uint32		nbytes;
	const unsigned char *p;
	const unsigned char *stop;

	if (rc->msg.len < 2 * (int) sizeof(uint32))
		routed_bad_message();
	memcpy(&nrows, rc->msg.data + rc->msg.len - 8, sizeof(uint32));
	memcpy(&nbytes, rc->msg.data + rc->msg.len - 4, sizeof(uint32));
	nrows = pg_ntoh32(nrows);
	nbytes = pg_ntoh32(nbytes);
	if (nbytes > (uint32) rc->msg.len - 8 || nrows > nbytes)
		routed_bad_message();
	rc->end = rc->msg.len - 8 - (int) nbytes;

	/* Those of rows long done make room for these. */
	if (rc->cstate != NULL && rc->cstate->cur_lineno > rc->first + ROUTED_KEEP_LINES)
	{
		uint64		drop = rc->cstate->cur_lineno - ROUTED_KEEP_LINES - rc->first;

		if (drop >= (uint64) rc->nlines)
			drop = rc->nlines;
		if (drop * 2 >= (uint64) rc->nlines)
		{
			memmove(rc->lines, rc->lines + drop,
					(rc->nlines - drop) * sizeof(uint64));
			rc->first += drop;
			rc->nlines -= (int) drop;
		}
	}
	if (rc->nlines + nrows > (uint32) rc->maxlines)
	{
		rc->maxlines = Max(2 * rc->maxlines, rc->nlines + (int) nrows);
		rc->maxlines = Max(rc->maxlines, 1024);
		rc->lines = rc->lines == NULL
			? MemoryContextAlloc(rc->cxt, rc->maxlines * sizeof(uint64))
			: repalloc_array(rc->lines, uint64, rc->maxlines);
	}

	p = (const unsigned char *) rc->msg.data + rc->end;
	stop = p + nbytes;
	for (uint32 i = 0; i < nrows; i++)
	{
		uint64		v = 0;
		int			shift = 0;

		for (;;)
		{
			unsigned char b;

			if (p >= stop || shift > 63)
				routed_bad_message();
			b = *p++;
			v |= (uint64) (b & 0x7F) << shift;
			if ((b & 0x80) == 0)
				break;
			shift += 7;
		}
		rc->lastline += (int64) ((v >> 1) ^ (~(v & 1) + 1));
		rc->lines[rc->nlines++] = rc->lastline;
	}
	if (p != stop)
		routed_bad_message();
}

/*
 * The coordinator's next message: its rows to hand down, or the COPY's end.
 * As CopyGetData() reads a client's, which a COPY given a callback does not
 * do itself.
 */
static void
routed_next_message(RoutedCopy *rc)
{
	for (;;)
	{
		int			mtype;
		int			maxmsglen;

		HOLD_CANCEL_INTERRUPTS();
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype == EOF)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		switch (mtype)
		{
			case PqMsg_CopyData:
				maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
				break;
			case PqMsg_CopyDone:
			case PqMsg_CopyFail:
			case PqMsg_Flush:
			case PqMsg_Sync:
				maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected message type 0x%02X during COPY from stdin",
								mtype)));
				maxmsglen = 0;	/* keep compiler quiet */
				break;
		}
		if (pq_getmessage(&rc->msg, maxmsglen))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		RESUME_CANCEL_INTERRUPTS();

		switch (mtype)
		{
			case PqMsg_CopyData:
				routed_take_lines(rc);
				return;
			case PqMsg_CopyDone:
				rc->eof = true;
				rc->end = 0;
				rc->msg.cursor = 0;
				return;
			case PqMsg_CopyFail:
				ereport(ERROR,
						(errcode(ERRCODE_QUERY_CANCELED),
						 errmsg("COPY from stdin failed: %s",
								pq_getmsgstring(&rc->msg))));
				break;
			default:
				/* Flush and Sync, which libpq may send beside a COPY */
				break;
		}
	}
}

/* CopyFrom()'s source of data: the rows of the coordinator's messages. */
static int
routed_read(void *outbuf, int minread, int maxread)
{
	RoutedCopy *rc = routed;
	int			bytesread = 0;

	while (maxread > 0 && bytesread < minread && !rc->eof)
	{
		int			avail;

		while (rc->msg.cursor >= rc->end && !rc->eof)
			routed_next_message(rc);
		if (rc->eof)
			break;
		avail = Min(rc->end - rc->msg.cursor, maxread);
		memcpy(outbuf, rc->msg.data + rc->msg.cursor, avail);
		rc->msg.cursor += avail;
		outbuf = (char *) outbuf + avail;
		maxread -= avail;
		bytesread += avail;
	}
	return bytesread;
}

/*
 * Under CopyFrom()'s own context, and so called after it for every error
 * and every notice raised in the COPY: the row it was raised at, which is
 * the failure's if the COPY fails -- the last of them, an error caught in a
 * function leaving its row here for nobody.
 */
static void
routed_error_context(void *arg)
{
	RoutedCopy *rc = (RoutedCopy *) arg;

	rc->failed_row = (rc->cstate == NULL || rc->cstate->relname_only) ? 0
		: rc->cstate->cur_lineno;
}

/* Is this a COPY of rows the coordinator sends with their lines? */
bool
GpCopyInIsRouted(Node *parsetree)
{
	CopyStmt   *stmt = (CopyStmt *) parsetree;

	if (!IsA(parsetree, CopyStmt) || !stmt->is_from || stmt->filename != NULL ||
		stmt->relation == NULL)
		return false;
	foreach_node(DefElem, def, stmt->options)
		if (strcmp(def->defname, GP_COPY_LINES_OPTION) == 0)
			return true;
	return false;
}

/*
 * That COPY: DoCopy()'s checks and its lock, the answer that tells the
 * coordinator to send, which a COPY read by a callback does not give, and
 * PostgreSQL's COPY FROM over the rows.  How many it took.
 */
uint64
GpCopyInRouted(PlannedStmt *pstmt, const char *queryString,
			   QueryEnvironment *queryEnv)
{
	CopyStmt   *stmt = (CopyStmt *) pstmt->utilityStmt;
	ParseState *pstate = make_parsestate(NULL);
	List	   *options = NIL;
	int16		format = 0;
	int			natts;
	Relation	rel;
	RoutedCopy	rc;
	StringInfoData buf;
	uint64		processed = 0;

	foreach_node(DefElem, def, stmt->options)
	{
		if (strcmp(def->defname, GP_COPY_LINES_OPTION) == 0)
			continue;
		if (strcmp(def->defname, "format") == 0 &&
			strcmp(defGetString(def), "binary") == 0)
			format = 1;
		options = lappend(options, def);
	}
	pstate->p_sourcetext = queryString;
	pstate->p_queryEnv = queryEnv;

	rel = table_openrv(stmt->relation, RowExclusiveLock);
	GpCopyFromCheckPermissions(pstate, stmt, rel);
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY FROM not supported with row-level security"),
				 errhint("Use INSERT statements instead.")));
	if (XactReadOnly && !rel->rd_islocaltemp)
		PreventCommandIfReadOnly("COPY FROM");

	memset(&rc, 0, sizeof(rc));
	rc.cxt = CurrentMemoryContext;
	initStringInfo(&rc.msg);
	routed_failed_line = 0;

	PG_TRY();
	{
		CopyFromState cstate;
		ErrorContextCallback errcallback;

		routed = &rc;

		/* as ReceiveCopyBegin() answers, before the first read */
		natts = list_length(CopyGetAttnums(RelationGetDescr(rel), rel, stmt->attlist));
		pq_beginmessage(&buf, PqMsg_CopyInResponse);
		pq_sendbyte(&buf, format);
		pq_sendint16(&buf, natts);
		for (int i = 0; i < natts; i++)
			pq_sendint16(&buf, format);
		pq_endmessage(&buf);
		pq_flush();

		cstate = BeginCopyFrom(pstate, rel, NULL, NULL, false, routed_read,
							   stmt->attlist, options);
		rc.cstate = cstate;

		errcallback.callback = routed_error_context;
		errcallback.arg = &rc;
		errcallback.previous = error_context_stack;
		error_context_stack = &errcallback;
		processed = CopyFrom(cstate);
		error_context_stack = errcallback.previous;

		EndCopyFrom(cstate);
	}
	PG_CATCH();
	{
		routed_failed_line = routed_line(&rc, rc.failed_row);
		routed = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();
	routed = NULL;

	if (rc.lines != NULL)
		pfree(rc.lines);
	pfree(rc.msg.data);
	table_close(rel, NoLock);
	free_parsestate(pstate);
	return processed;
}

/*
 * The line of the coordinator's data the last such COPY failed at, or 0;
 * said once.
 */
uint64
GpCopyInRoutedFailedLine(void)
{
	uint64		line = routed_failed_line;

	routed_failed_line = 0;
	return line;
}
