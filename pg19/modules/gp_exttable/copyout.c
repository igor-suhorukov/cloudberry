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
 * copyout.c
 *	  A writable external table's rows as COPY TO writes them, in text or CSV;
 *	  and COPY TO's text with an escape of Cloudberry's.
 *
 * Cloudberry formats them with COPY TO's own routine, which it exports
 * (CopyOneRowTo()); PostgreSQL 19 keeps it static, with the routines it calls
 * (copyto.c), so they are made again here, from PostgreSQL 19's: the options
 * as COPY TO checks them (ProcessCopyOptions(), which is exported), a row's
 * fields each through its type's output function, escaped or quoted.
 *
 * COPY TO in text with ESCAPE 'OFF', or an escape of its own, is PostgreSQL's
 * COPY TO into a callback, each row made over (GpCopyToEscaped()).
 *
 * PostgreSQL sources this file is made of:
 *	  src/backend/commands/copyto.c: CopyToTextLikeOneRow(),
 *	  CopyAttributeOutText(), CopyAttributeOutCSV(), BeginCopyTo()'s
 *	  destinations, SendCopyBegin(), CopySendEndOfRow(), ClosePipeToProgram()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "commands/copy.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "mb/pg_wchar.h"
#include "storage/fd.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_exttable.h"

ExtCopyOut *
ExtCopyOutBegin(Relation rel, List *options)
{
	ExtCopyOut *co = palloc0(sizeof(ExtCopyOut));
	TupleDesc	tupDesc = RelationGetDescr(rel);
	int			num_phys_attrs = tupDesc->natts;

	ProcessCopyOptions(NULL, &co->opts, false, options);

	co->tupdesc = tupDesc;
	co->attnumlist = CopyGetAttnums(tupDesc, rel, NIL);

	/* per-column CSV FORCE QUOTE, as BeginCopyTo() sets it */
	co->opts.force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (co->opts.force_quote_all)
		MemSet(co->opts.force_quote_flags, true, num_phys_attrs * sizeof(bool));
	else if (co->opts.force_quote)
	{
		List	   *attnums = CopyGetAttnums(tupDesc, rel, co->opts.force_quote);

		foreach_int(attnum, attnums)
			co->opts.force_quote_flags[attnum - 1] = true;
	}

	if (co->opts.file_encoding < 0)
		co->file_encoding = pg_get_client_encoding();
	else
		co->file_encoding = co->opts.file_encoding;
	co->need_transcoding = !(co->file_encoding == GetDatabaseEncoding() ||
							 co->file_encoding == PG_SQL_ASCII);
	co->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(co->file_encoding);

	co->opts.null_print_client = co->opts.null_print;
	if (co->need_transcoding)
		co->opts.null_print_client = pg_server_to_any(co->opts.null_print,
													  co->opts.null_print_len,
													  co->file_encoding);

	co->out_functions = (FmgrInfo *) palloc0(num_phys_attrs * sizeof(FmgrInfo));
	foreach_int(attnum, co->attnumlist)
	{
		Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);
		Oid			func_oid;
		bool		is_varlena;

		getTypeOutputInfo(attr->atttypid, &func_oid, &is_varlena);
		fmgr_info(func_oid, &co->out_functions[attnum - 1]);
	}

	initStringInfo(&co->line);
	co->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
										   "ExtCopyOutRow",
										   ALLOCSET_DEFAULT_SIZES);
	return co;
}

void
ExtCopyOutEnd(ExtCopyOut *co)
{
	MemoryContextDelete(co->rowcontext);
}

#define DUMPSOFAR() \
	do { \
		if (ptr > start) \
			appendBinaryStringInfo(&co->line, start, ptr - start); \
	} while (0)

static void
CopyAttributeOutText(ExtCopyOut *co, const char *string)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = co->opts.delim[0];

	if (co->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), co->file_encoding);
	else
		ptr = string;

	start = ptr;
	while ((c = *ptr) != '\0')
	{
		if ((unsigned char) c < (unsigned char) 0x20)
		{
			switch (c)
			{
				case '\b':
					c = 'b';
					break;
				case '\f':
					c = 'f';
					break;
				case '\n':
					c = 'n';
					break;
				case '\r':
					c = 'r';
					break;
				case '\t':
					c = 't';
					break;
				case '\v':
					c = 'v';
					break;
				default:
					if (c == delimc)
						break;
					ptr++;
					continue;
			}
			DUMPSOFAR();
			appendStringInfoChar(&co->line, '\\');
			appendStringInfoChar(&co->line, c);
			start = ++ptr;
		}
		else if (c == '\\' || c == delimc)
		{
			DUMPSOFAR();
			appendStringInfoChar(&co->line, '\\');
			start = ptr++;
		}
		else if (IS_HIGHBIT_SET(c) && co->encoding_embeds_ascii)
			ptr += pg_encoding_mblen(co->file_encoding, ptr);
		else
			ptr++;
	}

	DUMPSOFAR();
}

static void
CopyAttributeOutCSV(ExtCopyOut *co, const char *string, bool use_quote)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = co->opts.delim[0];
	char		quotec = co->opts.quote[0];
	char		escapec = co->opts.escape[0];
	bool		single_attr = (list_length(co->attnumlist) == 1);

	if (!use_quote && strcmp(string, co->opts.null_print) == 0)
		use_quote = true;

	if (co->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), co->file_encoding);
	else
		ptr = string;

	if (!use_quote)
	{
		if (single_attr && strcmp(ptr, "\\.") == 0)
			use_quote = true;
		else
		{
			const char *tptr = ptr;

			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && co->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(co->file_encoding, tptr);
				else
					tptr++;
			}
		}
	}

	if (use_quote)
	{
		appendStringInfoChar(&co->line, quotec);
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if (c == quotec || c == escapec)
			{
				DUMPSOFAR();
				appendStringInfoChar(&co->line, escapec);
				start = ptr;
			}
			if (IS_HIGHBIT_SET(c) && co->encoding_embeds_ascii)
				ptr += pg_encoding_mblen(co->file_encoding, ptr);
			else
				ptr++;
		}
		DUMPSOFAR();
		appendStringInfoChar(&co->line, quotec);
	}
	else
		appendStringInfoString(&co->line, ptr);
}

/* A row, appended to co->line with its newline. */
void
ExtCopyOutRow(ExtCopyOut *co, TupleTableSlot *slot)
{
	bool		need_delim = false;
	bool		is_csv = (co->opts.format == COPY_FORMAT_CSV);
	MemoryContext oldcontext;

	MemoryContextReset(co->rowcontext);
	oldcontext = MemoryContextSwitchTo(co->rowcontext);

	slot_getallattrs(slot);

	foreach_int(attnum, co->attnumlist)
	{
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (need_delim)
			appendStringInfoChar(&co->line, co->opts.delim[0]);
		need_delim = true;

		if (isnull)
			appendStringInfoString(&co->line, co->opts.null_print_client);
		else
		{
			char	   *string = OutputFunctionCall(&co->out_functions[attnum - 1],
													value);

			if (is_csv)
				CopyAttributeOutCSV(co, string,
									co->opts.force_quote_flags[attnum - 1]);
			else
				CopyAttributeOutText(co, string);
		}
	}

	appendStringInfoChar(&co->line, '\n');
	MemoryContextSwitchTo(oldcontext);
}

/* ------------------------------------------------------------------------- */
/* COPY TO in text, with an escape of Cloudberry's                            */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's COPY TO writes text with the escape the statement gives it
 * in place of the backslash, or with ESCAPE 'OFF' each value as it is
 * (copyto.c's CopyAttributeOutText()); PostgreSQL 19's takes an ESCAPE in
 * CSV alone.  So PostgreSQL's COPY TO writes each row in text, into
 * escaped_row(), which makes it over -- each escape the backslash begins
 * made the statement's, or with OFF undone, a NULL left as it is -- and
 * writes it where COPY TO would have: the file, the program or the client,
 * opened and written as copyto.c opens and writes them.
 */
typedef struct EscapedCopyTo
{
	FILE	   *file;			/* the file, the program's pipe, or stdout */
	const char *filename;
	bool		is_program;
	bool		to_client;
	bool		escape_off;
	char		escape_char;
	char		delim;
	char	   *null_print;		/* as COPY TO writes it, in the data's encoding */
	int			null_len;
	StringInfoData row;
} EscapedCopyTo;

/* The data destination callback takes no argument of its caller's. */
static EscapedCopyTo *writing = NULL;

PGDLLEXPORT uint64 GpCopyToEscaped(ParseState *pstate, Relation rel,
								   RawStmt *query, List *attlist,
								   const char *filename, bool is_program,
								   List *options, bool escape_off,
								   char escape_char);

static void
escaped_write(EscapedCopyTo *et, const char *data, int len)
{
	if (et->to_client)
	{
		(void) pq_putmessage(PqMsg_CopyData, data, len);
		return;
	}
	if (fwrite(data, len, 1, et->file) != 1 || ferror(et->file))
	{
		if (et->is_program)
		{
			if (errno == EPIPE)
			{
				int			pclose_rc = ClosePipeStream(et->file);

				/* the program's own failure says more than a broken pipe */
				et->file = NULL;
				if (pclose_rc != -1 && pclose_rc != 0)
					ereport(ERROR,
							(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							 errmsg("program \"%s\" failed", et->filename),
							 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
				errno = EPIPE;
			}
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to COPY program: %m")));
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to COPY file: %m")));
	}
}

/*
 * A row as PostgreSQL's COPY TO writes it in text, without its end: its
 * fields, split where a delimiter is not escaped, each written anew.
 */
static void
escaped_row(void *data, int len)
{
	EscapedCopyTo *et = writing;
	const char *p = (const char *) data;
	const char *end = p + len;
	StringInfo	row = &et->row;

	Assert(et != NULL);
	resetStringInfo(row);
	for (;;)
	{
		const char *field = p;

		while (p < end && *p != et->delim)
			p += (*p == '\\' && p + 1 < end) ? 2 : 1;

		if (p - field == et->null_len &&
			memcmp(field, et->null_print, et->null_len) == 0)
			appendBinaryStringInfo(row, field, et->null_len);
		else
		{
			for (const char *q = field; q < p; q++)
			{
				if (*q != '\\' || q + 1 >= p)
				{
					appendStringInfoChar(row, *q);
					continue;
				}
				q++;
				if (!et->escape_off)
				{
					appendStringInfoChar(row, et->escape_char);
					appendStringInfoChar(row, *q);
					continue;
				}
				switch (*q)
				{
					case 'b':
						appendStringInfoChar(row, '\b');
						break;
					case 'f':
						appendStringInfoChar(row, '\f');
						break;
					case 'n':
						appendStringInfoChar(row, '\n');
						break;
					case 'r':
						appendStringInfoChar(row, '\r');
						break;
					case 't':
						appendStringInfoChar(row, '\t');
						break;
					case 'v':
						appendStringInfoChar(row, '\v');
						break;
					default:
						appendStringInfoChar(row, *q);
						break;
				}
			}
		}
		if (p >= end)
			break;
		appendStringInfoChar(row, et->delim);
		p++;
	}
	appendStringInfoChar(row, '\n');
	escaped_write(et, row->data, row->len);
}

/* Open where the rows go: the file, the program, or the client. */
static void
escaped_open(EscapedCopyTo *et, int natts)
{
	if (et->filename == NULL)
	{
		if (whereToSendOutput == DestRemote)
		{
			StringInfoData buf;

			/* CopyOutResponse, text, as SendCopyBegin() sends it */
			pq_beginmessage(&buf, PqMsg_CopyOutResponse);
			pq_sendbyte(&buf, 0);
			pq_sendint16(&buf, natts);
			for (int i = 0; i < natts; i++)
				pq_sendint16(&buf, 0);
			pq_endmessage(&buf);
			et->to_client = true;
		}
		else
			et->file = stdout;
	}
	else if (et->is_program)
	{
		et->file = OpenPipeStream(et->filename, PG_BINARY_W);
		if (et->file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							et->filename)));
	}
	else
	{
		mode_t		oumask;
		struct stat st;

		if (!is_absolute_path(et->filename))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("relative path not allowed for COPY to file")));
		oumask = umask(S_IWGRP | S_IWOTH);
		PG_TRY();
		{
			et->file = AllocateFile(et->filename, PG_BINARY_W);
		}
		PG_FINALLY();
		{
			umask(oumask);
		}
		PG_END_TRY();
		if (et->file == NULL)
		{
			int			save_errno = errno;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for writing: %m",
							et->filename),
					 (save_errno == ENOENT || save_errno == EACCES) ?
					 errhint("COPY TO instructs the PostgreSQL server process to write a file. "
							 "You may want a client-side facility such as psql's \\copy.") : 0));
		}
		if (fstat(fileno(et->file), &st))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", et->filename)));
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", et->filename)));
	}
}

static void
escaped_close(EscapedCopyTo *et)
{
	if (et->to_client)
		pq_putemptymessage(PqMsg_CopyDone);
	else if (et->is_program)
	{
		int			pclose_rc = ClosePipeStream(et->file);

		if (pclose_rc == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close pipe to external command: %m")));
		else if (pclose_rc != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("program \"%s\" failed", et->filename),
					 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
	else if (et->file != stdout && FreeFile(et->file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", et->filename)));
}

/*
 * COPY rel TO, or COPY (query) TO, in text, with ESCAPE 'OFF' (escape_off)
 * or an escape of its own (escape_char): the rows it wrote.  The caller has
 * checked what DoCopy() checks -- the table's privileges and row security,
 * a file's or a program's -- and "options" are the statement's less its
 * ESCAPE.
 */
uint64
GpCopyToEscaped(ParseState *pstate, Relation rel, RawStmt *query,
				List *attlist, const char *filename, bool is_program,
				List *options, bool escape_off, char escape_char)
{
	EscapedCopyTo *et = palloc0_object(EscapedCopyTo);
	EscapedCopyTo *outer = writing;
	CopyFormatOptions opts = {0};
	CopyToState cstate;
	int			file_encoding;
	int			natts;
	uint64		processed;

	ProcessCopyOptions(pstate, &opts, false, options);
	if (opts.format != COPY_FORMAT_TEXT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY ESCAPE requires CSV mode")));
	file_encoding = opts.file_encoding >= 0 ? opts.file_encoding :
		pg_get_client_encoding();
	if (PG_ENCODING_IS_CLIENT_ONLY(file_encoding))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY's text ESCAPE other than the backslash is not supported in encoding \"%s\"",
						pg_encoding_to_char(file_encoding))));

	et->filename = filename;
	et->is_program = is_program;
	et->escape_off = escape_off;
	et->escape_char = escape_char;
	et->delim = opts.delim[0];
	et->null_print = opts.null_print;
	if (file_encoding != GetDatabaseEncoding() && file_encoding != PG_SQL_ASCII)
		et->null_print = pg_server_to_any(opts.null_print, opts.null_print_len,
										  file_encoding);
	et->null_len = strlen(et->null_print);
	initStringInfo(&et->row);

	/* the columns it writes, which the client is told before the rows */
	if (rel != NULL)
		natts = list_length(CopyGetAttnums(RelationGetDescr(rel), rel, attlist));
	else
	{
		List	   *stmts = pg_analyze_and_rewrite_fixedparams(copyObject(query),
															   pstate->p_sourcetext,
															   NULL, 0, NULL);
		Query	   *q = linitial_node(Query, stmts);

		natts = ExecCleanTargetListLength(q->commandType == CMD_SELECT ?
										  q->targetList : q->returningList);
	}

	cstate = BeginCopyTo(pstate, rel, rel ? NULL : query,
						 rel ? RelationGetRelid(rel) : InvalidOid,
						 NULL, false, escaped_row, attlist, options);
	escaped_open(et, natts);
	writing = et;
	PG_TRY();
	{
		processed = DoCopyTo(cstate);
	}
	PG_FINALLY();
	{
		writing = outer;
	}
	PG_END_TRY();
	EndCopyTo(cstate);
	escaped_close(et);
	return processed;
}
