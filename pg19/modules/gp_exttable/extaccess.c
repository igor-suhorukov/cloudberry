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
 * extaccess.c
 *	  Reading an external table's rows from its location, and writing a
 *	  writable one's.
 *
 * A location's data is parsed by PostgreSQL's COPY FROM, fed by a callback
 * that reads the location.  Cloudberry's COPY handles single row errors
 * itself; PostgreSQL 19's handles only errors of a type's input function,
 * and only by skipping the row.  So a line is split into its fields by COPY
 * (NextCopyFromRawFields(), which PostgreSQL 19 exports for this), and the
 * fields are made values here, as NextCopyFrom() makes them: which lets a
 * missing field be NULL, for FILL MISSING FIELDS, and a row that will not
 * parse be set aside, for SEGMENT REJECT LIMIT, with the message it failed
 * with -- a type's input function reporting softly, a line's format error
 * caught as Cloudberry catches it, as a data error.
 *
 * A writable table's rows are formatted by copyout.c, since PostgreSQL 19
 * keeps COPY TO's per-row routine static.
 *
 * Cloudberry sources this file is made of:
 *	  gpcontrib/gp_exttable_fdw/extaccess.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_exttable.h"

static HeapTuple externalgettup(FileScanDesc scan);
static void open_external_readable_source(FileScanDesc scan, ExternalSelectDesc desc);
static void open_external_writable_source(ExternalInsertDesc extInsertDesc);
static int	external_getdata_callback(void *outbuf, int minread, int maxread);
static void external_scan_error_callback(void *arg);
static Oid	lookupCustomFormatter(List **options, bool iswritable);

/*
 * PostgreSQL 19's data source callback takes no argument of the caller's:
 * the scan being read is the one whose row NextCopyFromRawFields() is
 * reading now, one at a time.
 */
static FileScanDesc reading_scan = NULL;

/* The options of the port's and Cloudberry's that COPY does not take. */
static bool
is_copy_option(const char *name)
{
	static const char *const not_copy[] = {
		"fill_missing_fields", "newline", "line_delim", "formatter",
		"format_type", "reject_limit", "reject_limit_type", "log_errors",
		"is_writable", "execute_on", "location_uris", "command", NULL
	};

	for (int i = 0; not_copy[i] != NULL; i++)
		if (pg_strcasecmp(name, not_copy[i]) == 0)
			return false;
	return true;
}

static bool
has_option(List *options, const char *name)
{
	foreach_node(DefElem, def, options)
		if (pg_strcasecmp(def->defname, name) == 0)
			return defGetBoolean(def);
	return false;
}

/*
 * FORCE QUOTE, FORCE NOT NULL and FORCE NULL as the table keeps them -- its
 * columns' names joined by commas, or "*" -- as COPY takes them: a list of
 * names, or all of them.  Cloudberry's ProcessCopyOptions() reads the string
 * itself (parse_joined_option_list()).
 */
static DefElem *
column_list_option(DefElem *def)
{
	char	   *value = defGetString(def);
	List	   *names = NIL;
	char	   *tok;
	char	   *save;

	if (strcmp(value, "*") == 0)
		return makeDefElem(def->defname, (Node *) makeNode(A_Star), def->location);
	for (tok = strtok_r(pstrdup(value), ",", &save); tok != NULL;
		 tok = strtok_r(NULL, ",", &save))
		names = lappend(names, makeString(tok));
	return makeDefElem(def->defname, (Node *) names, def->location);
}

/*
 * The table's options COPY takes.  Text's escape and delimiter as
 * Cloudberry's COPY has them, which PostgreSQL's has not: an ESCAPE, which
 * it takes in CSV alone and whose text escapes with the backslash always,
 * is left out where it is the backslash; ESCAPE 'OFF', every backslash a
 * character, and DELIMITER 'OFF', the whole line one column, are made of
 * the data as it is read (external_getdata_callback()), and left out too.
 * Any other escape of text's is refused.
 */
static List *
copy_options_ext(List *options, bool *escape_off, char *escape_char,
				 bool *delim_off)
{
	List	   *result = NIL;
	bool		csv = false;

	*escape_off = *delim_off = false;
	*escape_char = '\0';
	foreach_node(DefElem, def, options)
		if (pg_strcasecmp(def->defname, "format") == 0 &&
			pg_strcasecmp(defGetString(def), "csv") == 0)
			csv = true;

	foreach_node(DefElem, def, options)
	{
		if (!is_copy_option(def->defname))
			continue;
		if (!csv && pg_strcasecmp(def->defname, "escape") == 0)
		{
			char	   *esc = defGetString(def);

			if (pg_strcasecmp(esc, "off") == 0)
				*escape_off = true;
			else if (strcmp(esc, "\\") != 0)
				*escape_char = esc[0];
			continue;
		}
		if (!csv && pg_strcasecmp(def->defname, "delimiter") == 0 &&
			pg_strcasecmp(defGetString(def), "off") == 0)
		{
			*delim_off = true;
			continue;
		}
		if ((pg_strcasecmp(def->defname, "force_quote") == 0 ||
			 pg_strcasecmp(def->defname, "force_not_null") == 0 ||
			 pg_strcasecmp(def->defname, "force_null") == 0) &&
			def->arg != NULL && IsA(def->arg, String))
			def = column_list_option(def);
		result = lappend(result, def);
	}
	return result;
}

static List *
copy_options(List *options)
{
	bool		escape_off;
	char		escape_char;
	bool		delim_off;

	return copy_options_ext(options, &escape_off, &escape_char, &delim_off);
}

List *
appendCopyEncodingOption(List *copyFmtOpts, int encoding)
{
	return lappend(copyFmtOpts,
				   makeDefElem("encoding",
							   (Node *) makeString((char *) pg_encoding_to_char(encoding)),
							   -1));
}

/* ------------------------------------------------------------------------- */
/* Scans                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A scan of uri, NULL where this node reads nothing, or of the data a
 * caller's function reads (ExtScanSourceBegin()); an encoding of -1 is
 * COPY's own, or the one the options give.
 */
static FileScanDesc
begin_scan(Relation relation, uint32 scancounter, char *uri,
		   ExtSourceRead source_read, void *source_arg, char fmtType,
		   int rejLimit, bool rejLimitInRows, char logErrors, int encoding,
		   List *extOptions)
{
	FileScanDesc scan;
	TupleDesc	tupDesc;
	int			role = GpClusterBackendRole();
	List	   *copyOpts;

	RelationIncrementReferenceCount(relation);

	scan = (FileScanDesc) palloc0(sizeof(FileScanDescData));
	ItemPointerSetInvalid(&scan->fs_ctup.t_self);
	scan->fs_rd = relation;
	scan->fs_scancounter = scancounter;
	scan->fs_fmttype = fmtType;
	scan->fs_options = extOptions;
	scan->fs_encoding = encoding;
	scan->fs_source_read = source_read;
	scan->fs_source_arg = source_arg;

	scan->fs_uri = uri;
	scan->fs_noop = (uri == NULL);

	tupDesc = RelationGetDescr(relation);
	scan->fs_tupDesc = tupDesc;
	scan->num_phys_attrs = tupDesc->natts;
	scan->values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	scan->nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));
	scan->fs_rowcontext = AllocSetContextCreate(CurrentMemoryContext,
												"ExternalScanRow",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * The COPY options: the table's, less the ones of its own, with its
	 * encoding.  A custom format's options are its formatter's.
	 */
	copyOpts = fmttype_is_custom(fmtType) ? NIL :
		copy_options_ext(extOptions, &scan->fs_escape_off, &scan->fs_escape_char,
						 &scan->fs_delim_off);
	if (encoding >= 0)
		copyOpts = appendCopyEncodingOption(list_copy(copyOpts), encoding);
	if ((scan->fs_escape_off || scan->fs_escape_char || scan->fs_delim_off) &&
		PG_ENCODING_IS_CLIENT_ONLY(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ESCAPE 'OFF' and DELIMITER 'OFF' are not supported in encoding \"%s\"",
						pg_encoding_to_char(encoding))));

	reading_scan = scan;
	scan->fs_pstate = BeginCopyFrom(NULL, relation, NULL, NULL, false,
									external_getdata_callback, NIL, copyOpts);
	reading_scan = NULL;

	if (scan->fs_pstate->opts.header_line != COPY_HEADER_FALSE &&
		source_read == NULL &&
		role == GP_ROLE_DISPATCH && !GpClusterIsSingleNode())
		ereport(NOTICE,
				(errmsg("HEADER means that each one of the data files has a header row")));

	scan->fs_csv = (scan->fs_pstate->opts.format == COPY_FORMAT_CSV);
	scan->fs_escape = scan->fs_pstate->opts.escape;
	scan->fs_quote = scan->fs_pstate->opts.quote;
	scan->fs_header = (scan->fs_pstate->opts.header_line != COPY_HEADER_FALSE);
	scan->fs_eol_type = EOL_UNKNOWN;

	if (rejLimit != -1)
		scan->fs_sreh = makeCdbSreh(rejLimit, rejLimitInRows,
									uri ? uri : "<none>",
									pstrdup(RelationGetRelationName(relation)),
									logErrors);
	if (scan->fs_sreh)
		scan->fs_sreh->relid = RelationGetRelid(relation);

	if (fmttype_is_custom(fmtType))
	{
		List	   *params = list_copy(extOptions);
		Oid			procOid = lookupCustomFormatter(&params, false);
		Form_pg_attribute att;

		scan->fs_custom_formatter_func = palloc(sizeof(FmgrInfo));
		fmgr_info(procOid, scan->fs_custom_formatter_func);
		scan->fs_custom_formatter_params = params;

		scan->in_functions = (FmgrInfo *) palloc(tupDesc->natts * sizeof(FmgrInfo));
		scan->typioparams = (Oid *) palloc(tupDesc->natts * sizeof(Oid));
		for (int i = 0; i < tupDesc->natts; i++)
		{
			att = TupleDescAttr(tupDesc, i);
			if (att->attisdropped)
				continue;
			getTypeInputInfo(att->atttypid, &scan->in_func_oid,
							 &scan->typioparams[i]);
			fmgr_info(scan->in_func_oid, &scan->in_functions[i]);
		}

		scan->fs_formatter = (FormatterData *) palloc0(sizeof(FormatterData));
		scan->fs_formatter->type = T_FormatterData;
		initStringInfo(&scan->fs_formatter->fmt_databuf);
		scan->fs_formatter->fmt_perrow_ctx =
			AllocSetContextCreate(CurrentMemoryContext, "ExtFormatterRow",
								  ALLOCSET_DEFAULT_SIZES);
	}

	return scan;
}

FileScanDesc
external_beginscan(Relation relation, uint32 scancounter, List *uriList,
				   char fmtType, bool isMasterOnly, int rejLimit,
				   bool rejLimitInRows, char logErrors, int encoding,
				   List *extOptions)
{
	int			segindex = GpClusterIsSingleNode() ? 0 : GpClusterContentId();
	char	   *uri = NULL;

	/*
	 * The URI this node reads: a segment its content's, the coordinator the
	 * first where the table is read ON COORDINATOR, one node the one there is.
	 */
	if (GpClusterIsSingleNode())
		segindex = 0;
	else if (GpClusterBackendRole() == GP_ROLE_DISPATCH)
		segindex = isMasterOnly ? 0 : -1;
	else if (isMasterOnly)
		segindex = -1;			/* the coordinator's to read */

	if (segindex >= 0 && segindex < list_length(uriList))
	{
		String	   *v = list_nth(uriList, segindex);

		if (strlen(strVal(v)) > 0)
			uri = strVal(v);
	}

	return begin_scan(relation, scancounter, uri, NULL, NULL, fmtType,
					  rejLimit, rejLimitInRows, logErrors, encoding, extOptions);
}

void
external_rescan(FileScanDesc scan)
{
	List	   *copyOpts;

	external_stopscan(scan);

	/* a new COPY state, from the beginning of the location */
	if (scan->fs_pstate != NULL)
		EndCopyFrom(scan->fs_pstate);
	copyOpts = fmttype_is_custom(scan->fs_fmttype) ? NIL : copy_options(scan->fs_options);
	if (scan->fs_encoding >= 0)
		copyOpts = appendCopyEncodingOption(list_copy(copyOpts), scan->fs_encoding);
	reading_scan = scan;
	scan->fs_pstate = BeginCopyFrom(NULL, scan->fs_rd, NULL, NULL, false,
									external_getdata_callback, NIL, copyOpts);
	reading_scan = NULL;
}

void
external_stopscan(FileScanDesc scan)
{
	if (!scan->fs_noop && scan->fs_file)
	{
		url_fclose(scan->fs_file, false, RelationGetRelationName(scan->fs_rd));
		scan->fs_file = NULL;
	}
}

void
external_endscan(FileScanDesc scan)
{
	char	   *relname = pstrdup(RelationGetRelationName(scan->fs_rd));

	/* how many rows were rejected, for the coordinator to report */
	if (scan->fs_sreh)
	{
		SrehNoteRejected(scan->fs_sreh->rejectcount);
		destroyCdbSreh(scan->fs_sreh);
		scan->fs_sreh = NULL;
	}

	if (scan->fs_formatter)
	{
		if (scan->fs_formatter->fmt_databuf.data)
			pfree(scan->fs_formatter->fmt_databuf.data);
		MemoryContextDelete(scan->fs_formatter->fmt_perrow_ctx);
		pfree(scan->fs_formatter);
		scan->fs_formatter = NULL;
	}

	if (scan->fs_pstate != NULL)
	{
		EndCopyFrom(scan->fs_pstate);
		scan->fs_pstate = NULL;
	}
	MemoryContextDelete(scan->fs_rowcontext);

	/*
	 * Close the location, if the scan stopped before its end -- a LIMIT, or
	 * an error: what closing it says then -- a command killed by the pipe
	 * closed under it -- is no error of the query's.  At its end the scan
	 * closed it already, and a command's failure was the query's
	 * (external_getnext()).
	 */
	if (!scan->fs_noop && scan->fs_file)
	{
		url_fclose(scan->fs_file, false, relname);
		scan->fs_file = NULL;
	}

	RelationDecrementReferenceCount(scan->fs_rd);
	pfree(relname);
}

ExternalSelectDesc
external_getnext_init(PlanState *state)
{
	ExternalSelectDesc desc = (ExternalSelectDesc) palloc0(sizeof(ExternalSelectDescData));

	if (state != NULL)
		desc->projInfo = state->ps_ProjInfo;
	return desc;
}

HeapTuple
external_getnext(FileScanDesc scan, ScanDirection direction,
				 ExternalSelectDesc desc)
{
	HeapTuple	tuple;
	ErrorContextCallback errcallback;

	if (scan->fs_noop)
		return NULL;

	/*
	 * The location is opened here, on the first row, and not as the scan
	 * begins: a plan's nodes are all begun, but not all run (MPP-1261).
	 */
	if (!scan->fs_file)
		open_external_readable_source(scan, desc);

	errcallback.callback = external_scan_error_callback;
	errcallback.arg = (void *) scan;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	tuple = externalgettup(scan);

	error_context_stack = errcallback.previous;

	if (tuple == NULL)
	{
		/* a command's failure shows only as it is closed */
		if (scan->fs_file->type == CFTYPE_EXEC)
		{
			url_fclose(scan->fs_file, true, RelationGetRelationName(scan->fs_rd));
			scan->fs_file = NULL;
		}
		return NULL;
	}

	pgstat_count_heap_getnext(scan->fs_rd);
	return tuple;
}

/*
 * A scan of data a caller reads itself -- pxf_fdw's, from its server --
 * parsed and its bad rows handled as an external table's are, where
 * Cloudberry's COPY FROM took the caller's callback and its single row error
 * handling (external.h).  The node reading it is whichever begins it: the
 * caller decides where it runs.
 */
FileScanDesc
ExtScanSourceBegin(Relation rel, const char *source, char fmtType,
				   List *options, int rejLimit, bool rejLimitInRows,
				   char logErrors, ExtSourceRead read, void *arg)
{
	if (!fmttype_is_text(fmtType) && !fmttype_is_csv(fmtType))
		elog(ERROR, "a scan of a source reads text or CSV, not format '%c'",
			 fmtType);

	return begin_scan(rel, 0, pstrdup(source), read, arg, fmtType, rejLimit,
					  rejLimitInRows, logErrors, -1, options);
}

/*
 * What the scan keeps between rows -- the source it opens with the first --
 * lives in the context it was begun in, whatever the caller's is now: an
 * executor's per-row context, for a foreign scan's IterateForeignScan.
 */
HeapTuple
ExtScanSourceNext(FileScanDesc scan)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(scan));
	HeapTuple	tuple = external_getnext(scan, ForwardScanDirection, NULL);

	MemoryContextSwitchTo(old);
	return tuple;
}

void
ExtScanSourceRescan(FileScanDesc scan)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(scan));

	external_rescan(scan);
	MemoryContextSwitchTo(old);
}

void
ExtScanSourceEnd(FileScanDesc scan)
{
	external_endscan(scan);
}

/*
 * A row that would not parse, under SEGMENT REJECT LIMIT: counted, logged if
 * LOG ERRORS says so, and the query failed once too many are.
 */
static void
reject_row(FileScanDesc scan, const char *message)
{
	CdbSreh    *sreh = scan->fs_sreh;
	CopyFromState cstate = scan->fs_pstate;
	MemoryContext old = MemoryContextSwitchTo(sreh->badrowcontext);

	MemoryContextReset(sreh->badrowcontext);
	if (cstate->cur_attname)
		sreh->errmsg = psprintf("%s, column %s", message, cstate->cur_attname);
	else
		sreh->errmsg = pstrdup(message);

	resetStringInfo(sreh->rawdata);
	appendBinaryStringInfo(sreh->rawdata, cstate->line_buf.data, cstate->line_buf.len);
	while (sreh->rawdata->len > 0 &&
		   (sreh->rawdata->data[sreh->rawdata->len - 1] == '\n' ||
			sreh->rawdata->data[sreh->rawdata->len - 1] == '\r'))
		sreh->rawdata->data[--sreh->rawdata->len] = '\0';
	sreh->linenumber = cstate->cur_lineno;
	if (scan->fs_file != NULL && scan->fs_file->current[0] != '\0')
		strlcpy(sreh->filename, scan->fs_file->current, sizeof(sreh->filename));
	MemoryContextSwitchTo(old);

	HandleSingleRowError(sreh);
	ErrorIfRejectLimitReached(sreh);

	/*
	 * The column's name is left as it is, as Cloudberry leaves it: a line
	 * whose error names no column of its own -- extra data after the last --
	 * is logged with the name of the one the line before failed at, which
	 * is what Cloudberry's error logs say and its tests expect.
	 */
}

/*
 * The values of a line's fields, as NextCopyFrom() makes them (copyfromparse.c's
 * CopyFromTextLikeOneRow()), with FILL MISSING FIELDS; false, the message in
 * *message, for a field its type would not take under SEGMENT REJECT LIMIT.
 */
static bool
fields_to_values(FileScanDesc scan, char **fields, int nfields, char **message)
{
	CopyFromState cstate = scan->fs_pstate;
	TupleDesc	tupDesc = scan->fs_tupDesc;
	int			fieldno = 0;
	bool		fill = has_option(scan->fs_options, "fill_missing_fields");
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	if (nfields > list_length(cstate->attnumlist))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("extra data after last expected column")));

	MemSet(scan->nulls, true, tupDesc->natts * sizeof(bool));
	MemSet(scan->values, 0, tupDesc->natts * sizeof(Datum));

	foreach_int(attnum, cstate->attnumlist)
	{
		int			m = attnum - 1;
		Form_pg_attribute att = TupleDescAttr(tupDesc, m);
		char	   *string;

		if (fieldno >= nfields)
		{
			/*
			 * Cloudberry's FILL MISSING FIELDS: the columns a short line has
			 * no field for are NULL -- but not a line with none at all.
			 */
			if (fill && nfields > 0)
				continue;
			/* the message names the column; no other is being read */
			cstate->cur_attname = NULL;
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("missing data for column \"%s\"",
							NameStr(att->attname))));
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

		if (scan->fs_sreh == NULL)
			scan->values[m] = InputFunctionCall(&cstate->in_functions[m], string,
												cstate->typioparams[m],
												att->atttypmod);
		else if (!InputFunctionCallSafe(&cstate->in_functions[m], string,
										cstate->typioparams[m], att->atttypmod,
										(Node *) &escontext, &scan->values[m]))
		{
			*message = escontext.error_data ? escontext.error_data->message :
				pstrdup("invalid input");
			return false;
		}
		if (string != NULL)
			scan->nulls[m] = false;
	}
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
	return true;
}

static HeapTuple
externalgettup_defined(FileScanDesc scan)
{
	CopyFromState cstate = scan->fs_pstate;

	for (;;)
	{
		char	  **fields;
		int			nfields;
		bool		got;
		char	   *message = NULL;
		MemoryContext old;

		CHECK_FOR_INTERRUPTS();

		reading_scan = scan;
		if (scan->fs_sreh == NULL)
		{
			got = NextCopyFromRawFields(cstate, &fields, &nfields);
			reading_scan = NULL;
			if (!got)
				return NULL;
			(void) fields_to_values(scan, fields, nfields, &message);
			return heap_form_tuple(scan->fs_tupDesc, scan->values, scan->nulls);
		}

		/*
		 * Under SEGMENT REJECT LIMIT, a data error of the line's -- its
		 * format, its encoding, a field too many or too few -- is caught, as
		 * Cloudberry's FILEAM_HANDLE_ERROR catches it; any other kind is the
		 * query's.
		 */
		old = CurrentMemoryContext;
		PG_TRY();
		{
			got = NextCopyFromRawFields(cstate, &fields, &nfields);
			if (got)
				(void) fields_to_values(scan, fields, nfields, &message);
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			MemoryContextSwitchTo(old);
			edata = CopyErrorData();
			if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
				PG_RE_THROW();
			FlushErrorState();
			message = pstrdup(edata->message);
			FreeErrorData(edata);
			got = true;
		}
		PG_END_TRY();
		reading_scan = NULL;

		if (!got)
			return NULL;

		scan->fs_sreh->processed++;
		if (message != NULL)
		{
			reject_row(scan, message);
			continue;
		}
		return heap_form_tuple(scan->fs_tupDesc, scan->values, scan->nulls);
	}
}

/*
 * A custom format: the data handed to the user's formatter function a
 * buffer at a time, which returns a row or asks for more.
 */
static HeapTuple
externalgettup_custom(FileScanDesc scan)
{
	FormatterData *formatter = scan->fs_formatter;
	char		buf[65536];
	bool		eof = false;

	for (;;)
	{
		HeapTuple	tuple = NULL;
		bool		error_caught = false;
		MemoryContext oldctx = CurrentMemoryContext;

		/* more data, if the formatter has none to work on */
		if (formatter->fmt_databuf.cursor >= formatter->fmt_databuf.len && !eof)
		{
			int			n;

			reading_scan = scan;
			n = external_getdata_callback(buf, 1, sizeof(buf));
			reading_scan = NULL;
			if (formatter->fmt_databuf.cursor > 0)
			{
				resetStringInfo(&formatter->fmt_databuf);
				formatter->fmt_databuf.cursor = 0;
			}
			if (n > 0)
				appendBinaryStringInfo(&formatter->fmt_databuf, buf, n);
			else
				eof = true;
		}
		if (formatter->fmt_databuf.cursor >= formatter->fmt_databuf.len && eof)
			return NULL;

		PG_TRY();
		{
			LOCAL_FCINFO(fcinfo, 0);

			formatter->fmt_relation = scan->fs_rd;
			formatter->fmt_tupDesc = scan->fs_tupDesc;
			formatter->fmt_notification = FMT_NONE;
			formatter->fmt_saw_eof = eof;
			formatter->fmt_args = scan->fs_custom_formatter_params;
			formatter->fmt_conv_funcs = scan->in_functions;
			formatter->fmt_typioparams = scan->typioparams;
			formatter->fmt_external_encoding = scan->fs_encoding;
			formatter->fmt_needs_transcoding = (scan->fs_encoding != GetDatabaseEncoding());
			MemoryContextReset(formatter->fmt_perrow_ctx);

			InitFunctionCallInfoData(*fcinfo, scan->fs_custom_formatter_func, 0,
									 InvalidOid, (Node *) formatter, NULL);

			/*
			 * In the scan's context, as Cloudberry calls it: what the
			 * formatter keeps from call to call (its user context) is
			 * allocated there, and outlives the row.  Its row is made there
			 * too, and is the caller's to free, as Cloudberry's slot frees
			 * it: it is copied into the row's context.
			 */
			MemoryContextSwitchTo(GetMemoryChunkContext(scan));
			(void) FunctionCallInvoke(fcinfo);
			MemoryContextSwitchTo(oldctx);
			if (formatter->fmt_notification == FMT_NONE &&
				formatter->fmt_tuple != NULL)
			{
				tuple = heap_copytuple(formatter->fmt_tuple);
				heap_freetuple(formatter->fmt_tuple);
			}
			formatter->fmt_tuple = NULL;
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			if (scan->fs_sreh == NULL)
				PG_RE_THROW();
			MemoryContextSwitchTo(oldctx);
			edata = CopyErrorData();
			if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
				PG_RE_THROW();
			FlushErrorState();
			error_caught = true;

			resetStringInfo(&scan->fs_pstate->line_buf);
			if (formatter->fmt_badrow_len > 0 && formatter->fmt_badrow_data)
				appendBinaryStringInfo(&scan->fs_pstate->line_buf,
									   formatter->fmt_badrow_data,
									   formatter->fmt_badrow_len);
			scan->fs_pstate->cur_lineno = formatter->fmt_badrow_num;
			scan->fs_sreh->processed++;
			reject_row(scan, edata->message);
			FreeErrorData(edata);
			formatter->fmt_badrow_len = 0;
		}
		PG_END_TRY();

		if (error_caught)
			continue;
		if (formatter->fmt_notification == FMT_NEED_MORE_DATA)
		{
			/* the rest of the data too short to be a row: said, and dropped */
			if (eof)
			{
				if (formatter->fmt_databuf.cursor < formatter->fmt_databuf.len)
					ereport(WARNING,
							(errcode(ERRCODE_DATA_EXCEPTION),
							 errmsg("unexpected end of file")));
				return NULL;
			}
			/* keep what is left, and read more after it */
			if (formatter->fmt_databuf.cursor > 0)
			{
				StringInfo	b = &formatter->fmt_databuf;
				int			remaining = b->len - b->cursor;

				memmove(b->data, b->data + b->cursor, remaining);
				b->len = remaining;
				b->data[b->len] = '\0';
				b->cursor = 0;
			}
			reading_scan = scan;
			{
				int			n = external_getdata_callback(buf, 1, sizeof(buf));

				if (n > 0)
					appendBinaryStringInfo(&formatter->fmt_databuf, buf, n);
				else
					eof = true;
			}
			reading_scan = NULL;
			continue;
		}
		if (tuple != NULL)
		{
			if (scan->fs_sreh)
				scan->fs_sreh->processed++;
			return tuple;
		}
	}
}

/*
 * The next row, made in the scan's row context, which the next call resets:
 * the slot is given it not to free.
 */
static HeapTuple
externalgettup(FileScanDesc scan)
{
	HeapTuple	tuple;
	MemoryContext old;

	MemoryContextReset(scan->fs_rowcontext);
	old = MemoryContextSwitchTo(scan->fs_rowcontext);
	if (scan->fs_custom_formatter_func == NULL)
		tuple = externalgettup_defined(scan);
	else
		tuple = externalgettup_custom(scan);
	MemoryContextSwitchTo(old);
	return tuple;
}

/* The user's formatter function: its "formatter" option, taken out. */
static Oid
lookupCustomFormatter(List **options, bool iswritable)
{
	ListCell   *lc;
	char	   *formatter_name = NULL;
	List	   *funcname;
	Oid			argList[1];
	Oid			returnOid;
	Oid			procOid;

	foreach(lc, *options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "formatter") == 0)
		{
			formatter_name = defGetString(defel);
			*options = foreach_delete_current(*options, lc);
			break;
		}
	}
	if (formatter_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("formatter function not found in table options")));

	funcname = list_make1(makeString(formatter_name));
	if (iswritable)
	{
		argList[0] = RECORDOID;
		returnOid = BYTEAOID;
	}
	else
		returnOid = RECORDOID;

	procOid = LookupFuncName(funcname, iswritable ? 1 : 0, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("formatter function \"%s\" of type %s was not found",
						formatter_name, iswritable ? "writable" : "readable"),
				 errhint("Create it with CREATE FUNCTION.")));

	if (get_func_rettype(procOid) != returnOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("formatter function \"%s\" of type %s has an incorrect return type",
						formatter_name, iswritable ? "writable" : "readable")));

	if (object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for formatter function \"%s\"",
						formatter_name)));

	return procOid;
}

/* ------------------------------------------------------------------------- */
/* The data                                                                  */
/* ------------------------------------------------------------------------- */

static void
open_external_readable_source(FileScanDesc scan, ExternalSelectDesc desc)
{
	extvar_t	extvar;

	if (scan->fs_source_read != NULL)
	{
		scan->fs_file = url_source_fopen(scan->fs_uri, scan->fs_source_read,
										 scan->fs_source_arg);
		return;
	}

	memset(&extvar, 0, sizeof(extvar));
	external_set_env_vars_ext(&extvar, scan->fs_uri, scan->fs_csv,
							  scan->fs_escape, scan->fs_quote,
							  scan->fs_eol_type, scan->fs_header,
							  scan->fs_scancounter,
							  scan->fs_custom_formatter_params);

	scan->fs_file = url_fopen(scan->fs_uri, false, &extvar,
							  &scan->fs_pstate->opts, desc, scan->fs_rd);
}

static int
external_getdata_callback(void *outbuf, int minread, int maxread)
{
	FileScanDesc scan = reading_scan;
	int			bytesread;

	Assert(scan != NULL);

	/* opened late, where the first row is asked for; see external_getnext() */
	if (scan->fs_file == NULL)
		return 0;

	/*
	 * Text's ESCAPE 'OFF', an escape of its own and DELIMITER 'OFF', made of
	 * the data (see copy_options_ext()): each backslash doubled, the escape
	 * made the backslash, each tab written \t -- so that COPY's text reads a
	 * backslash as itself, the escape as its own and a tab as part of the one
	 * column.  Each byte becomes at most two, so half as many are read.  The
	 * table's encoding embeds no ASCII in a character's other bytes, which
	 * was checked as the scan began.
	 */
	if (scan->fs_escape_off || scan->fs_escape_char || scan->fs_delim_off)
	{
		char	   *raw = palloc(Max(maxread / 2, 1));
		char	   *out = (char *) outbuf;
		int			n = 0;

		bytesread = url_fread(raw, Max(maxread / 2, 1), scan->fs_file, scan->fs_pstate);
		for (int i = 0; i < bytesread; i++)
		{
			if (raw[i] == '\\' && (scan->fs_escape_off || scan->fs_escape_char))
			{
				out[n++] = '\\';
				out[n++] = '\\';
			}
			else if (scan->fs_escape_char && raw[i] == scan->fs_escape_char)
				out[n++] = '\\';
			else if (raw[i] == '\t' && scan->fs_delim_off)
			{
				out[n++] = '\\';
				out[n++] = 't';
			}
			else
				out[n++] = raw[i];
		}
		pfree(raw);
		if (bytesread <= 0 && url_ferror(scan->fs_file, bytesread, NULL, 0))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from external file: %m")));
		return bytesread > 0 ? n : 0;
	}

	bytesread = url_fread(outbuf, maxread, scan->fs_file, scan->fs_pstate);

	if (bytesread <= 0 && url_ferror(scan->fs_file, bytesread, NULL, 0))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from external file: %m")));

	return bytesread > 0 ? bytesread : 0;
}

static char *
linenumber_atoi(char *buffer, size_t bufsz, int64 linenumber)
{
	if (linenumber < 0)
		snprintf(buffer, bufsz, "%s", "N/A");
	else
		snprintf(buffer, bufsz, INT64_FORMAT, linenumber);
	return buffer;
}

static void
external_scan_error_callback(void *arg)
{
	FileScanDesc scan = (FileScanDesc) arg;
	CopyFromState cstate = scan->fs_pstate;
	char		buffer[20];
	const char *relname = RelationGetRelationName(scan->fs_rd);

	if (scan->fs_custom_formatter_func)
	{
		errcontext("External table %s", relname);
		return;
	}

	if (cstate->cur_attname)
		errcontext("External table %s, line %s of %s, column %s",
				   relname,
				   linenumber_atoi(buffer, sizeof(buffer), cstate->cur_lineno),
				   scan->fs_uri, cstate->cur_attname);
	else if (!cstate->need_transcoding && cstate->line_buf_valid)
	{
		char	   *line_buf = CopyLimitPrintoutLength(cstate->line_buf.data);
		int			len = strlen(line_buf);

		while (len > 0 && (line_buf[len - 1] == '\n' || line_buf[len - 1] == '\r'))
			line_buf[--len] = '\0';
		errcontext("External table %s, line %s of %s: \"%s\"",
				   relname,
				   linenumber_atoi(buffer, sizeof(buffer), cstate->cur_lineno),
				   scan->fs_uri, line_buf);
		pfree(line_buf);
	}
	else if (cstate->cur_lineno > 0)
		errcontext("External table %s, line %s of file %s",
				   relname,
				   linenumber_atoi(buffer, sizeof(buffer), cstate->cur_lineno),
				   scan->fs_uri);
	else
		errcontext("External table %s, file %s", relname, scan->fs_uri);
}

/* ------------------------------------------------------------------------- */
/* Writes                                                                    */
/* ------------------------------------------------------------------------- */

/* how much of a writer's rows it keeps before it sends them */
#define EXT_WRITE_BUFSIZE	(64 * 1024)

ExternalInsertDesc
external_insert_init(Relation rel)
{
	ExternalInsertDesc extInsertDesc;
	ExtTableEntry *extentry = GetExtTableEntry(RelationGetRelid(rel));
	List	   *copyFmtOpts;

	extInsertDesc = (ExternalInsertDesc) palloc0(sizeof(ExternalInsertDescData));
	extInsertDesc->ext_rel = rel;

	/*
	 * The coordinator writes nothing of a table its segments write, whose
	 * rows go to them; one ON COORDINATOR, which has no policy, is its own.
	 */
	extInsertDesc->ext_noop = (!GpClusterIsSingleNode() &&
							   GpClusterBackendRole() == GP_ROLE_DISPATCH &&
							   strcmp(strVal(linitial(extentry->execlocations)),
									  "COORDINATOR_ONLY") != 0);
	extInsertDesc->ext_tupDesc = RelationGetDescr(rel);

	if (extentry->command)
		extInsertDesc->ext_uri = psprintf("execute:%s", extentry->command);
	else
	{
		int			segindex = GpClusterIsSingleNode() ? 0 : GpClusterContentId();
		int			num_segs = GpClusterIsSingleNode() ? 1 : GpClusterSegmentCount();
		int			num_urls = list_length(extentry->urilocations);

		if (num_urls > num_segs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("external table has more URLs than available primary segments that can write into them")));
		if (segindex < 0)
			segindex = 0;
		extInsertDesc->ext_uri =
			pstrdup(strVal(list_nth(extentry->urilocations, segindex % num_urls)));
	}

	copyFmtOpts = fmttype_is_custom(extentry->fmtcode) ? NIL :
		copy_options(extentry->options);
	copyFmtOpts = appendCopyEncodingOption(list_copy(copyFmtOpts), extentry->encoding);

	if (fmttype_is_custom(extentry->fmtcode))
	{
		List	   *params = list_copy(extentry->options);
		Oid			procOid = lookupCustomFormatter(&params, true);

		extInsertDesc->ext_custom_formatter_func = palloc(sizeof(FmgrInfo));
		fmgr_info(procOid, extInsertDesc->ext_custom_formatter_func);
		extInsertDesc->ext_custom_formatter_params = params;
		extInsertDesc->ext_formatter_data = (FormatterData *) palloc0(sizeof(FormatterData));
		extInsertDesc->ext_formatter_data->type = T_FormatterData;
		extInsertDesc->ext_rowcontext = AllocSetContextCreate(CurrentMemoryContext,
															  "ExtFormatterRow",
															  ALLOCSET_DEFAULT_SIZES);
		extInsertDesc->ext_formatter_data->fmt_perrow_ctx = extInsertDesc->ext_rowcontext;
		/* the custom path still needs the encoding and the output functions */
		extInsertDesc->ext_out = ExtCopyOutBegin(rel, appendCopyEncodingOption(NIL, extentry->encoding));
	}
	else
		extInsertDesc->ext_out = ExtCopyOutBegin(rel, copyFmtOpts);

	return extInsertDesc;
}

static void
open_external_writable_source(ExternalInsertDesc extInsertDesc)
{
	extvar_t	extvar;
	ExtCopyOut *co = extInsertDesc->ext_out;

	memset(&extvar, 0, sizeof(extvar));
	external_set_env_vars_ext(&extvar, extInsertDesc->ext_uri,
							  co->opts.format == COPY_FORMAT_CSV,
							  co->opts.escape, co->opts.quote, EOL_UNKNOWN,
							  co->opts.header_line != COPY_HEADER_FALSE, 0,
							  extInsertDesc->ext_custom_formatter_params);

	extInsertDesc->ext_file = url_fopen(extInsertDesc->ext_uri, true, &extvar,
										&co->opts, NULL, extInsertDesc->ext_rel);
}

static void
external_senddata(ExternalInsertDesc extInsertDesc, const char *data, int len)
{
	char		ebuf[512] = {0};
	size_t		nwrote;

	if (len == 0)
		return;
	nwrote = url_fwrite((void *) data, len, extInsertDesc->ext_file);
	if (url_ferror(extInsertDesc->ext_file, (int) nwrote, ebuf, sizeof(ebuf)))
	{
		if (*ebuf)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to external resource: %s", ebuf)));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to external resource: %m")));
	}
}

void
external_insert(ExternalInsertDesc extInsertDesc, TupleTableSlot *slot)
{
	ExtCopyOut *co = extInsertDesc->ext_out;

	if (extInsertDesc->ext_noop)
		return;

	if (extInsertDesc->ext_file == NULL)
		open_external_writable_source(extInsertDesc);

	if (extInsertDesc->ext_custom_formatter_func)
	{
		FormatterData *formatter = extInsertDesc->ext_formatter_data;
		HeapTuple	tuple;
		Datum		d;
		bytea	   *b;
		MemoryContext old;
		LOCAL_FCINFO(fcinfo, 1);

		MemoryContextReset(extInsertDesc->ext_rowcontext);
		old = MemoryContextSwitchTo(extInsertDesc->ext_rowcontext);
		tuple = ExecCopySlotHeapTuple(slot);
		formatter->fmt_relation = extInsertDesc->ext_rel;
		formatter->fmt_tupDesc = extInsertDesc->ext_tupDesc;
		formatter->fmt_args = extInsertDesc->ext_custom_formatter_params;
		formatter->fmt_conv_funcs = co->out_functions;
		formatter->fmt_external_encoding = co->file_encoding;
		formatter->fmt_needs_transcoding = co->need_transcoding;

		InitFunctionCallInfoData(*fcinfo, extInsertDesc->ext_custom_formatter_func,
								 1, InvalidOid, (Node *) formatter, NULL);
		fcinfo->args[0].value = HeapTupleGetDatum(tuple);
		fcinfo->args[0].isnull = false;
		d = FunctionCallInvoke(fcinfo);
		if (!fcinfo->isnull)
		{
			b = DatumGetByteaPP(d);
			appendBinaryStringInfo(&co->line, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
		}
		MemoryContextSwitchTo(old);
	}
	else
		ExtCopyOutRow(co, slot);

	if (co->line.len >= EXT_WRITE_BUFSIZE)
	{
		external_senddata(extInsertDesc, co->line.data, co->line.len);
		resetStringInfo(&co->line);
	}
}

void
external_insert_finish(ExternalInsertDesc extInsertDesc)
{
	ExtCopyOut *co = extInsertDesc->ext_out;

	if (!extInsertDesc->ext_noop && extInsertDesc->ext_file != NULL)
	{
		external_senddata(extInsertDesc, co->line.data, co->line.len);
		resetStringInfo(&co->line);
		url_fflush(extInsertDesc->ext_file);
		url_fclose(extInsertDesc->ext_file, true,
				   RelationGetRelationName(extInsertDesc->ext_rel));
		extInsertDesc->ext_file = NULL;
	}
	ExtCopyOutEnd(co);
}
