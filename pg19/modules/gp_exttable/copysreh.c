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
 * copysreh.c
 *	  COPY FROM ... [LOG ERRORS] SEGMENT REJECT LIMIT n [ROWS | PERCENT]:
 *	  the data, each line that would not load left out and logged.
 *
 * Cloudberry's COPY catches a line's data error -- its format, a field too
 * many or too few, a value its column's type refuses -- logs the line and
 * goes on, until the reject limit is reached (copyfrom.c's error handling,
 * cdbsreh.c).  PostgreSQL 19's COPY has no place for that: its ON_ERROR
 * catches a type's refusal alone.  So the data is read twice.  Here, by
 * PostgreSQL's COPY parser, a line at a time, each line's fields tried as
 * its columns' values, as an external table's scan tries them
 * (extaccess.c); and the lines that pass, as they were read, by PostgreSQL's
 * own COPY into the table, which gp_core's COPY runs (gp_modify.c) with
 * GpSrehCopyRead() as its data source.  A constraint's refusal is no data
 * error, and fails the COPY there, as it fails Cloudberry's.
 *
 * Cloudberry sources this file stands for:
 *	  src/backend/commands/copyfrom.c (HandleCopyError), cdb/cdbsreh.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_exttable.h"

typedef struct SrehCopy
{
	CopyFromState cstate;		/* the data, as it is read */
	CdbSreh    *sreh;
	bool		fill_missing;
	StringInfoData ready;		/* lines that passed, not yet handed on */
	int			ready_off;
	bool		eof;
	Datum	   *values;
	bool	   *nulls;
	MemoryContext linecxt;		/* a line's values, reset at the next */
} SrehCopy;

/*
 * The data source callback takes no argument of its caller's (PostgreSQL
 * 19's copy_data_source_cb), and a COPY reads one source at a time: the
 * one being read.
 */
static SrehCopy *reading = NULL;

PGDLLEXPORT void *GpSrehCopyBegin(ParseState *pstate, Relation rel,
								  const char *filename, bool is_program,
								  List *attlist, List *options,
								  int reject_limit, bool limit_in_rows,
								  char log_errors, List **load_options);
PGDLLEXPORT int GpSrehCopyRead(void *outbuf, int minread, int maxread);
PGDLLEXPORT uint64 GpSrehCopyEnd(void *state);

/*
 * A line's fields as its columns' values, as NextCopyFrom() makes them, with
 * FILL MISSING FIELDS; false, and the message, for a field its type refuses
 * or a field too many or too few.  The values are only tried: the COPY that
 * loads the line makes them again, defaults and all.
 */
static bool
fields_load(SrehCopy *sc, char **fields, int nfields, char **message)
{
	CopyFromState cstate = sc->cstate;
	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	int			fieldno = 0;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	if (nfields > list_length(cstate->attnumlist))
	{
		*message = pstrdup("extra data after last expected column");
		return false;
	}

	foreach_int(attnum, cstate->attnumlist)
	{
		int			m = attnum - 1;
		Form_pg_attribute att = TupleDescAttr(tupDesc, m);
		char	   *string;

		if (fieldno >= nfields)
		{
			if (sc->fill_missing && nfields > 0)
				continue;
			*message = psprintf("missing data for column \"%s\"", NameStr(att->attname));
			return false;
		}
		string = fields[fieldno++];

		if (cstate->opts.format == COPY_FORMAT_CSV)
		{
			if (string == NULL && cstate->opts.force_notnull_flags[m])
				string = cstate->opts.null_print;
			else if (string != NULL && cstate->opts.force_null_flags[m] &&
					 strcmp(string, cstate->opts.null_print) == 0)
				string = NULL;
		}

		cstate->cur_attname = NameStr(att->attname);
		cstate->cur_attval = string;
		if (!InputFunctionCallSafe(&cstate->in_functions[m], string,
								   cstate->typioparams[m], att->atttypmod,
								   (Node *) &escontext, &sc->values[m]))
		{
			*message = escontext.error_data ?
				psprintf("%s, column %s", escontext.error_data->message,
						 NameStr(att->attname)) :
				pstrdup("invalid input");
			return false;
		}
	}
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
	return true;
}

/* "COPY t, line n", as COPY says where it failed (CopyFromErrorCallback()) */
static void
copy_line_context(void *arg)
{
	SrehCopy   *sc = (SrehCopy *) arg;

	errcontext("COPY %s, line %" PRIu64,
			   RelationGetRelationName(sc->cstate->rel), sc->cstate->cur_lineno);
}

/* A line refused: logged, if the COPY logs errors, and counted. */
static void
reject_line(SrehCopy *sc, const char *message)
{
	CdbSreh    *sreh = sc->sreh;
	CopyFromState cstate = sc->cstate;
	MemoryContext old = MemoryContextSwitchTo(sreh->badrowcontext);

	MemoryContextReset(sreh->badrowcontext);
	sreh->errmsg = pstrdup(message);
	resetStringInfo(sreh->rawdata);
	appendBinaryStringInfo(sreh->rawdata, cstate->line_buf.data, cstate->line_buf.len);
	while (sreh->rawdata->len > 0 &&
		   (sreh->rawdata->data[sreh->rawdata->len - 1] == '\n' ||
			sreh->rawdata->data[sreh->rawdata->len - 1] == '\r'))
		sreh->rawdata->data[--sreh->rawdata->len] = '\0';
	sreh->linenumber = cstate->cur_lineno;
	MemoryContextSwitchTo(old);

	HandleSingleRowError(sreh);
	{
		ErrorContextCallback errcallback;

		errcallback.callback = copy_line_context;
		errcallback.arg = sc;
		errcallback.previous = error_context_stack;
		error_context_stack = &errcallback;
		ErrorIfRejectLimitReached(sreh);
		error_context_stack = errcallback.previous;
	}
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
}

/*
 * The next line, into sc->ready if it passes: false at the end of the data.
 * A data error of the line's is caught, as Cloudberry's COPY catches it
 * (its class, 22); any other kind is the statement's.
 */
static bool
next_line(SrehCopy *sc)
{
	CopyFromState cstate = sc->cstate;
	char	  **fields;
	int			nfields;
	bool		got;
	char	   *message = NULL;
	MemoryContext old = MemoryContextSwitchTo(sc->linecxt);

	MemoryContextReset(sc->linecxt);
	PG_TRY();
	{
		got = NextCopyFromRawFields(cstate, &fields, &nfields);
		if (got)
			(void) fields_load(sc, fields, nfields, &message);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(sc->linecxt);
		edata = CopyErrorData();
		if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
			PG_RE_THROW();
		FlushErrorState();
		message = pstrdup(edata->message);
		got = true;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);

	if (!got)
		return false;

	sc->sreh->processed++;
	if (message != NULL)
		reject_line(sc, message);
	else
	{
		/* the line as it was read, in the server's encoding, without its end */
		appendBinaryStringInfo(&sc->ready, cstate->line_buf.data, cstate->line_buf.len);
		while (sc->ready.len > 0 &&
			   (sc->ready.data[sc->ready.len - 1] == '\n' ||
				sc->ready.data[sc->ready.len - 1] == '\r'))
			sc->ready.data[--sc->ready.len] = '\0';
		appendStringInfoChar(&sc->ready, '\n');
	}
	return true;
}

/*
 * Begin: the data opened, as COPY FROM opens it -- the file, the program, or
 * the client -- and the options the COPY that loads the lines is to take:
 * the statement's, less HEADER, which was read here, and in the server's
 * encoding, which the lines are in once read here.
 */
void *
GpSrehCopyBegin(ParseState *pstate, Relation rel, const char *filename,
				bool is_program, List *attlist, List *options,
				int reject_limit, bool limit_in_rows, char log_errors,
				List **load_options)
{
	SrehCopy   *sc = palloc0_object(SrehCopy);
	List	   *copyopts = NIL;
	TupleDesc	tupDesc = RelationGetDescr(rel);

	*load_options = NIL;
	foreach_node(DefElem, def, options)
	{
		if (strcmp(def->defname, "fill_missing_fields") == 0)
		{
			sc->fill_missing = defGetBoolean(def);
			continue;
		}
		copyopts = lappend(copyopts, def);
		if (strcmp(def->defname, "header") == 0 ||
			strcmp(def->defname, "encoding") == 0)
			continue;
		*load_options = lappend(*load_options, def);
	}
	*load_options = lappend(*load_options,
							makeDefElem("encoding",
										(Node *) makeString((char *) GetDatabaseEncodingName()),
										-1));

	sc->cstate = BeginCopyFrom(pstate, rel, NULL, filename, is_program, NULL,
							   attlist, copyopts);
	if (sc->cstate->opts.format == COPY_FORMAT_BINARY)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SEGMENT REJECT LIMIT is not supported with binary COPY")));
	sc->sreh = makeCdbSreh(reject_limit, limit_in_rows,
						   filename ? (char *) filename : "<stdin>",
						   pstrdup(RelationGetRelationName(rel)), log_errors);
	sc->sreh->relid = RelationGetRelid(rel);
	initStringInfo(&sc->ready);
	sc->values = palloc0_array(Datum, Max(tupDesc->natts, 1));
	sc->nulls = palloc0_array(bool, Max(tupDesc->natts, 1));
	sc->linecxt = AllocSetContextCreate(CurrentMemoryContext, "SrehCopyLine",
										ALLOCSET_DEFAULT_SIZES);
	reading = sc;
	return sc;
}

/* The loading COPY's data: the lines that passed, as many as it asks for. */
int
GpSrehCopyRead(void *outbuf, int minread, int maxread)
{
	SrehCopy   *sc = reading;
	int			n;

	Assert(sc != NULL);
	while (sc->ready.len - sc->ready_off < minread && !sc->eof)
	{
		CHECK_FOR_INTERRUPTS();
		if (!next_line(sc))
			sc->eof = true;
	}
	n = Min(maxread, sc->ready.len - sc->ready_off);
	memcpy(outbuf, sc->ready.data + sc->ready_off, n);
	sc->ready_off += n;
	if (sc->ready_off == sc->ready.len)
	{
		resetStringInfo(&sc->ready);
		sc->ready_off = 0;
	}
	return n;
}

/*
 * End: the data closed, and how many lines it refused, said as Cloudberry's
 * COPY says it.
 */
uint64
GpSrehCopyEnd(void *state)
{
	SrehCopy   *sc = (SrehCopy *) state;
	uint64		rejected = sc->sreh->rejectcount;

	EndCopyFrom(sc->cstate);
	ReportSrehResults(sc->sreh, rejected);
	destroyCdbSreh(sc->sreh);
	MemoryContextDelete(sc->linecxt);
	reading = NULL;
	return rejected;
}
