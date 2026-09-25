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
 *	  A writable external table's rows as COPY TO writes them, in text or CSV.
 *
 * Cloudberry formats them with COPY TO's own routine, which it exports
 * (CopyOneRowTo()); PostgreSQL 19 keeps it static, with the routines it calls
 * (copyto.c), so they are made again here, from PostgreSQL 19's: the options
 * as COPY TO checks them (ProcessCopyOptions(), which is exported), a row's
 * fields each through its type's output function, escaped or quoted.
 *
 * PostgreSQL sources this file is made of:
 *	  src/backend/commands/copyto.c: CopyToTextLikeOneRow(),
 *	  CopyAttributeOutText(), CopyAttributeOutCSV()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/copy.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
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
