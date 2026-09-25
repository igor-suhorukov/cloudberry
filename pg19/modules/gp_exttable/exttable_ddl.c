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
 * exttable_ddl.c
 *	  CREATE EXTERNAL TABLE, made the foreign table of gp_exttable_server
 *	  Cloudberry makes of it.
 *
 * The grammar (O26's rewriter, gp_desugar.c) makes the statement a CREATE
 * FOREIGN TABLE of the server, with Cloudberry's clauses as parsed carried in
 * one option, gp_exttable.spec: a List of DefElem, written out as nodes are.
 * gp_sql's ProcessUtility hook, which every backend has, hands the statement
 * here -- this module is loaded then, not preloaded -- and here the clauses
 * are checked and made the table's options, as Cloudberry's
 * DefineExternalRelation() makes them (exttablecmds.c): the format's options,
 * complete, as COPY takes them, and format_type, location_uris or command,
 * execute_on, reject_limit and its type, log_errors, encoding and
 * is_writable.  Where the table's rows are read or written it says with a
 * distribution of gp_sql's: random, over every segment, for one read there
 * and a writable one without a DISTRIBUTED clause of its own.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/commands/exttablecmds.c, parse_utilcmd.c's
 *	  transformCreateExternalStmt()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "commands/copy.h"
#include "foreign/foreign.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_exttable.h"

#define EXTTABLE_SPEC_OPTION	"gp_exttable.spec"
#define FDIST_DEF_PORT			8080

/* ------------------------------------------------------------------------- */
/* The clauses                                                               */
/* ------------------------------------------------------------------------- */

static DefElem *
spec_get(List *spec, const char *name)
{
	foreach_node(DefElem, def, spec)
		if (strcmp(def->defname, name) == 0)
			return def;
	return NULL;
}

static bool
spec_bool(List *spec, const char *name)
{
	DefElem    *def = spec_get(spec, name);

	return def != NULL && boolVal(def->arg);
}

/* Since | separates the URIs, a | or \ in one is escaped with \. */
static char *
escape_uri(const char *uri)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (const char *p = uri; *p; p++)
	{
		if (*p == '|' || *p == '\\')
			appendStringInfoChar(&buf, '\\');
		appendStringInfoChar(&buf, *p);
	}
	return buf.data;
}

static char *
transformLocationUris(List *locs, bool isweb, bool iswritable)
{
	StringInfoData buf;
	UriProtocol first_protocol = URI_FILE;
	bool		first_uri = true;

	initStringInfo(&buf);
	Assert(locs != NIL);

	foreach_node(String, v, locs)
	{
		char	   *uri_str_orig = strVal(v);
		char	   *uri_str_final;
		Uri		   *uri = ParseExternalTableUri(uri_str_orig);

		/* gpfdist's default port, written in */
		if ((uri->protocol == URI_GPFDIST || uri->protocol == URI_GPFDISTS) &&
			uri->port == -1)
		{
			char	   *at_hostname = uri_str_orig +
				strlen(uri->protocol == URI_GPFDIST ? PROTOCOL_GPFDIST : PROTOCOL_GPFDISTS);
			char	   *after_hostname = strchr(at_hostname, '/');
			char	   *hostname = pnstrdup(at_hostname, after_hostname - at_hostname);

			uri_str_final = psprintf("%s%s:%d%s",
									 (uri->protocol == URI_GPFDIST ? PROTOCOL_GPFDIST : PROTOCOL_GPFDISTS),
									 hostname, FDIST_DEF_PORT, after_hostname);
		}
		else
			uri_str_final = pstrdup(uri_str_orig);

		/* a protocol of the user's must exist, and may validate its URIs */
		if (first_uri && uri->protocol == URI_CUSTOM)
			(void) LookupExtProtocolFunction(uri->customprotocol, iswritable, true);

		if (first_uri)
			first_protocol = uri->protocol;

		if (uri->protocol != first_protocol)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("URI protocols must be the same for all data sources"),
					 errhint("Available protocols are 'http', 'file', 'gpfdist' and 'gpfdists'.")));

		if (uri->protocol != URI_HTTP && isweb)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("an EXTERNAL WEB TABLE may only use http URI's, problem in: '%s'", uri_str_final),
					 errhint("Use CREATE EXTERNAL TABLE instead.")));

		if (uri->protocol == URI_HTTP && !isweb)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("http URI's can only be used in an external web table"),
					 errhint("Use CREATE EXTERNAL WEB TABLE instead.")));

		if (iswritable && (uri->protocol == URI_HTTP || uri->protocol == URI_FILE))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported URI protocol '%s' for writable external table",
							(uri->protocol == URI_HTTP ? "http" : "file")),
					 errhint("Writable external tables may use 'gpfdist' or 'gpfdists' URIs only.")));

		if (uri->protocol != URI_CUSTOM && iswritable && strchr(uri->path, '*'))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unsupported use of wildcard in a writable external web table definition: '%s'",
							uri_str_final),
					 errhint("Specify the explicit path and file name to write into.")));

		if ((uri->protocol == URI_GPFDIST || uri->protocol == URI_GPFDISTS) &&
			iswritable && uri->path[strlen(uri->path) - 1] == '/')
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unsupported use of a directory name in a writable gpfdist(s) external table : '%s'",
							uri_str_final),
					 errhint("Specify the explicit path and file name to write into.")));

		if (!first_uri)
			appendStringInfoChar(&buf, '|');
		appendStringInfoString(&buf, escape_uri(uri_str_final));
		first_uri = false;
		FreeExternalTableUri(uri);
	}

	return buf.data;
}

static char *
transformExecOnClause(List *on_clause)
{
	char	   *exec_location_str = NULL;

	/* one node runs everything, as the coordinator would */
	if (GpClusterIsSingleNode())
		return "COORDINATOR_ONLY";
	if (on_clause == NIL)
		return "ALL_SEGMENTS";

	foreach_node(DefElem, defel, on_clause)
	{
		if (exec_location_str)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("ON clause must not have more than one element")));

		if (strcmp(defel->defname, "all") == 0)
			exec_location_str = "ALL_SEGMENTS";
		else if (strcmp(defel->defname, "hostname") == 0)
			exec_location_str = psprintf("HOST:%s", strVal(defel->arg));
		else if (strcmp(defel->defname, "eachhost") == 0)
			exec_location_str = "PER_HOST";
		else if (strcmp(defel->defname, "coordinator") == 0)
			exec_location_str = "COORDINATOR_ONLY";
		else if (strcmp(defel->defname, "segment") == 0)
			exec_location_str = psprintf("SEGMENT_ID:%d", (int) intVal(defel->arg));
		else if (strcmp(defel->defname, "random") == 0)
			exec_location_str = psprintf("TOTAL_SEGS:%d", (int) intVal(defel->arg));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("unknown location code for EXECUTE in tablecmds")));
	}

	return exec_location_str;
}

static char
transformFormatType(const char *formatname)
{
	if (pg_strcasecmp(formatname, "text") == 0)
		return 't';
	else if (pg_strcasecmp(formatname, "csv") == 0)
		return 'c';
	else if (pg_strcasecmp(formatname, "custom") == 0)
		return 'b';
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("unsupported format '%s'", formatname),
			 errhint("Available formats for external tables are \"text\", \"csv\" and \"custom\".")));
	return 't';
}

static char *
list_join(List *list, char delimiter)
{
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_node(String, s, list)
	{
		if (buf.len > 0)
			appendStringInfoChar(&buf, delimiter);
		appendStringInfoString(&buf, strVal(s));
	}
	return buf.data;
}

/*
 * The FORMAT options, checked as COPY checks them, made complete as
 * Cloudberry stores them: format, delimiter, null, escape, quote for CSV,
 * then header, fill_missing_fields, force_not_null, force_quote, newline.
 * FILL MISSING FIELDS, NEWLINE and DELIMITER 'OFF' are Cloudberry's, and
 * taken out before PostgreSQL's ProcessCopyOptions() sees the rest.
 */
static List *
transformFormatOpts(char formattype, List *formatOpts, int numcols, bool iswritable)
{
	List	   *cslist = NIL;
	List	   *copyopts = NIL;
	CopyFormatOptions opts;
	bool		fill_missing = false;
	bool		delim_off = false;
	char	   *text_escape = NULL;
	char	   *eol_str = NULL;

	memset(&opts, 0, sizeof(opts));

	if (fmttype_is_custom(formattype))
	{
		bool		found = false;

		foreach_node(DefElem, defel, formatOpts)
		{
			if (strcmp(defel->defname, "formatter") == 0)
			{
				if (found)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("redundant formatter option")));
				found = true;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("no formatter function specified")));

		cslist = list_copy(formatOpts);
		cslist = lappend(cslist, makeDefElem("format", (Node *) makeString("custom"), -1));
		return cslist;
	}

	foreach_node(DefElem, defel, formatOpts)
	{
		if (strcmp(defel->defname, "fill_missing_fields") == 0)
			fill_missing = true;
		else if (strcmp(defel->defname, "newline") == 0)
		{
			eol_str = defGetString(defel);
			if (pg_strcasecmp(eol_str, "lf") != 0 && pg_strcasecmp(eol_str, "cr") != 0 &&
				pg_strcasecmp(eol_str, "crlf") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for NEWLINE \"%s\"", eol_str),
						 errhint("Valid options are: 'LF', 'CRLF' and 'CR'.")));
		}
		else if (strcmp(defel->defname, "delimiter") == 0 &&
				 pg_strcasecmp(defGetString(defel), "off") == 0)
			delim_off = true;

		/* Cloudberry's COPY says what else its delimiter may be */
		else if (strcmp(defel->defname, "delimiter") == 0 &&
				 strlen(defGetString(defel)) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY delimiter must be a single one-byte character, or 'off'")));

		/*
		 * Text's escape, any one character or OFF, which PostgreSQL's COPY
		 * takes in CSV alone: kept as it is written, and made of the data as
		 * a scan reads it (extaccess.c, copy_options_ext()).
		 */
		else if (strcmp(defel->defname, "escape") == 0 &&
				 !fmttype_is_csv(formattype))
		{
			text_escape = defGetString(defel);
			if (pg_strcasecmp(text_escape, "off") != 0 && strlen(text_escape) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY escape must be a single one-byte character")));
		}
		else if (strcmp(defel->defname, "delimiter") == 0 ||
				 strcmp(defel->defname, "null") == 0 ||
				 strcmp(defel->defname, "header") == 0 ||
				 strcmp(defel->defname, "quote") == 0 ||
				 strcmp(defel->defname, "escape") == 0 ||
				 strcmp(defel->defname, "force_not_null") == 0 ||
				 strcmp(defel->defname, "force_quote") == 0)
			copyopts = lappend(copyopts, defel);
		else if (strcmp(defel->defname, "formatter") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("formatter option only valid for custom formatters")));
		else
			elog(ERROR, "option \"%s\" not recognized", defel->defname);
	}

	if (fmttype_is_csv(formattype))
	{
		copyopts = lappend(list_copy(copyopts),
						   makeDefElem("format", (Node *) makeString("csv"), -1));
		cslist = lappend(cslist, makeDefElem("format", (Node *) makeString("csv"), -1));
	}
	else
		cslist = lappend(cslist, makeDefElem("format", (Node *) makeString("text"), -1));

	ProcessCopyOptions(NULL, &opts, !iswritable, copyopts);

	if (delim_off && numcols != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("using no delimiter is only possible for a single column table")));

	if (opts.header_line != COPY_HEADER_FALSE && iswritable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("HEADER is not yet supported for writable external tables")));

	cslist = lappend(cslist, makeDefElem("delimiter",
										 (Node *) makeString(delim_off ? "off" : opts.delim), -1));
	cslist = lappend(cslist, makeDefElem("null", (Node *) makeString(opts.null_print), -1));
	cslist = lappend(cslist, makeDefElem("escape",
										 (Node *) makeString(text_escape ? text_escape :
															 opts.escape ? opts.escape : "\\"),
										 -1));
	if (fmttype_is_csv(formattype))
		cslist = lappend(cslist, makeDefElem("quote", (Node *) makeString(opts.quote), -1));
	if (opts.header_line != COPY_HEADER_FALSE)
		cslist = lappend(cslist, makeDefElem("header", (Node *) makeString("true"), -1));
	if (fill_missing)
		cslist = lappend(cslist, makeDefElem("fill_missing_fields", (Node *) makeString("true"), -1));
	if (opts.force_notnull)
		cslist = lappend(cslist, makeDefElem("force_not_null",
											 (Node *) makeString(list_join(opts.force_notnull, ',')), -1));
	if (opts.force_quote)
		cslist = lappend(cslist, makeDefElem("force_quote",
											 (Node *) makeString(list_join(opts.force_quote, ',')), -1));
	else if (opts.force_quote_all)
		cslist = lappend(cslist, makeDefElem("force_quote", (Node *) makeString("*"), -1));
	if (eol_str)
		cslist = lappend(cslist, makeDefElem("newline", (Node *) makeString(eol_str), -1));

	return cslist;
}

/*
 * CREATE EXTERNAL TABLE ... (LIKE t): PostgreSQL refuses LIKE in a foreign
 * table, so its columns are written in here -- names, types and collations,
 * all an external table's column has (transformCreateExternalStmt()).  LIKE
 * INCLUDING is refused, as Cloudberry refuses it (transformTableLikeClause()).
 */
void
ExtTableExpandLike(CreateStmt *stmt, const char *queryString)
{
	List	   *elts = NIL;

	foreach_ptr(Node, elt, stmt->tableElts)
	{
		TableLikeClause *like;
		Relation	rel;
		TupleDesc	desc;

		if (!IsA(elt, TableLikeClause))
		{
			elts = lappend(elts, elt);
			continue;
		}
		like = (TableLikeClause *) elt;
		if (like->options != 0)
		{
			ParseState *pstate = make_parsestate(NULL);

			pstate->p_sourcetext = queryString;
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("LIKE INCLUDING may not be used with this kind of relation"),
					 parser_errposition(pstate, like->relation->location)));
		}
		rel = relation_openrv(like->relation, AccessShareLock);
		desc = RelationGetDescr(rel);
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);
			ColumnDef  *def;

			if (att->attisdropped)
				continue;
			def = makeColumnDef(NameStr(att->attname), att->atttypid,
								att->atttypmod, att->attcollation);
			elts = lappend(elts, def);
		}
		relation_close(rel, NoLock);
	}
	stmt->tableElts = elts;
}

/* Is this CREATE FOREIGN TABLE what the grammar made of CREATE EXTERNAL TABLE? */
bool
ExtTableIsCreate(CreateForeignTableStmt *stmt)
{
	foreach_node(DefElem, def, stmt->options)
		if (strcmp(def->defname, EXTTABLE_SPEC_OPTION) == 0)
			return true;
	return false;
}

PGDLLEXPORT void GpExtTableTransformCreate(CreateForeignTableStmt *stmt,
										   const char *queryString);

void
GpExtTableTransformCreate(CreateForeignTableStmt *stmt, const char *queryString)
{
	ExtTableTransformCreate(stmt, queryString);
}

void
ExtTableTransformCreate(CreateForeignTableStmt *stmt, const char *queryString)
{
	List	   *spec = NIL;
	List	   *rest = NIL;
	List	   *formatOpts;
	List	   *extOptions = NIL;
	List	   *entryOptions = NIL;
	DefElem    *d;
	bool		iswritable;
	bool		isweb;
	char		formattype;
	char	   *locationUris = NULL;
	char	   *locationExec = NULL;
	char	   *command = NULL;
	char		logerrors = LOG_ERRORS_DISABLE;
	int			rejectlimit = -1;
	char		rejectlimittype = '\0';
	int			encoding = -1;
	bool		log_persistent = false;
	List	   *locs = NIL;
	int			ncols;

	foreach_node(DefElem, def, stmt->options)
	{
		if (strcmp(def->defname, EXTTABLE_SPEC_OPTION) == 0)
			spec = (List *) stringToNode(defGetString(def));
		else
			rest = lappend(rest, def);	/* gp.distributed_by and the tags */
	}

	iswritable = spec_bool(spec, "writable");
	isweb = spec_bool(spec, "web");

	/* the server must be there, which CREATE EXTENSION gp_exttable made */
	if (GetForeignServerByName(GP_EXTTABLE_SERVER_NAME, true) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("external tables need the gp_exttable extension"),
				 errhint("Run CREATE EXTENSION gp_exttable in this database.")));

	ExtTableExpandLike(&stmt->base, queryString);
	ncols = list_length(stmt->base.tableElts);

	if ((d = spec_get(spec, "command")) != NULL)
	{
		command = strVal(d->arg);
		if (strlen(command) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid EXECUTE clause, command string is empty")));
		if (!isweb)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("EXECUTE may not be used with a regular external table"),
					 errhint("Use CREATE EXTERNAL WEB TABLE instead.")));
		d = spec_get(spec, "on");
		if (iswritable && d != NULL && (List *) d->arg != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("ON clause may not be used with a writable external table")));
		locationExec = transformExecOnClause(d ? (List *) d->arg : NIL);
	}
	else
	{
		d = spec_get(spec, "location");
		locs = (List *) d->arg;
		locationExec = transformExecOnClause((d = spec_get(spec, "on")) ? (List *) d->arg : NIL);
		locationUris = transformLocationUris(locs, isweb, iswritable);
	}

	formattype = transformFormatType(strVal(spec_get(spec, "format")->arg));
	d = spec_get(spec, "format_opts");
	formatOpts = transformFormatOpts(formattype, d ? (List *) d->arg : NIL, ncols,
									 iswritable);

	/* OPTIONS (...), less error_log_persistent, which LOG ERRORS takes */
	if ((d = spec_get(spec, "options")) != NULL)
	{
		foreach_node(DefElem, o, (List *) d->arg)
		{
			if (pg_strcasecmp(o->defname, "error_log_persistent") == 0)
				log_persistent = defGetBoolean(o);
			else
				extOptions = lappend(extOptions, o);
		}
	}

	if ((d = spec_get(spec, "reject_limit")) != NULL)
	{
		if (iswritable)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("single row error handling may not be used with a writable external table")));
		rejectlimit = intVal(d->arg);
		rejectlimittype = spec_bool(spec, "reject_rows") ? 'r' : 'p';
		d = spec_get(spec, "log_errors");
		logerrors = d ? strVal(d->arg)[0] : LOG_ERRORS_DISABLE;
		if (IS_LOG_ERRORS_ENABLE(logerrors) && log_persistent)
			logerrors = LOG_ERRORS_PERSISTENTLY;
		VerifyRejectLimit(rejectlimittype, rejectlimit);
	}

	if ((d = spec_get(spec, "encoding")) != NULL)
	{
		const char *encoding_name;

		if (IsA(d->arg, Integer))
		{
			encoding = intVal(d->arg);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 || pg_valid_client_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code", encoding)));
		}
		else
		{
			encoding_name = strVal(d->arg);
			if (pg_valid_client_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name", encoding_name)));
			encoding = pg_char_to_encoding(encoding_name);
		}
	}
	if (encoding < 0)
		encoding = GetDatabaseEncoding();

	/* more locations than segments: made, but unreadable until expanded */
	if (locs != NIL && !GpClusterIsSingleNode() &&
		GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		Uri		   *uri = ParseExternalTableUri(strVal(linitial(locs)));

		if ((uri->protocol == URI_FILE || uri->protocol == URI_HTTP) &&
			GpClusterSegmentCount() < list_length(locs))
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("number of locations (%d) exceeds the number of segments (%d)",
							list_length(locs), GpClusterSegmentCount()),
					 errhint("The table cannot be queried until cluster is expanded so that there are at least as many segments as locations.")));
	}

	/* GenerateExtTableEntryOptions() */
	entryOptions = lappend(entryOptions, makeDefElem("format_type",
													 (Node *) makeString(psprintf("%c", formattype)), -1));
	if (command)
		entryOptions = lappend(entryOptions, makeDefElem("command", (Node *) makeString(command), -1));
	else
		entryOptions = lappend(entryOptions, makeDefElem("location_uris", (Node *) makeString(locationUris), -1));
	entryOptions = lappend(entryOptions, makeDefElem("execute_on", (Node *) makeString(locationExec), -1));
	if (rejectlimit != -1)
	{
		entryOptions = lappend(entryOptions, makeDefElem("reject_limit",
														 (Node *) makeString(psprintf("%d", rejectlimit)), -1));
		entryOptions = lappend(entryOptions, makeDefElem("reject_limit_type",
														 (Node *) makeString(psprintf("%c", rejectlimittype)), -1));
	}
	entryOptions = lappend(entryOptions, makeDefElem("log_errors",
													 (Node *) makeString(psprintf("%c", logerrors)), -1));
	entryOptions = lappend(entryOptions, makeDefElem("encoding",
													 (Node *) makeString(psprintf("%d", encoding)), -1));
	entryOptions = lappend(entryOptions, makeDefElem("is_writable",
													 (Node *) makeString(iswritable ? "true" : "false"), -1));

	/*
	 * Where the rows are: read on the segments, or written there without a
	 * distribution of the statement's own, randomly; read on the coordinator,
	 * the coordinator's, which a table without a distribution is.
	 */
	{
		bool		has_policy = false;
		bool		replicated = false;

		foreach_node(DefElem, def, rest)
			if (strcmp(def->defname, "gp.distributed_by") == 0)
			{
				has_policy = true;
				replicated = pg_strcasecmp(defGetString(def), "replicated") == 0;
			}

		if (has_policy && !iswritable)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("readable external tables can't specify a DISTRIBUTED BY clause")));
		if (replicated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("external tables can't have DISTRIBUTED REPLICATED clause")));
		if (!has_policy && !GpClusterIsSingleNode() &&
			strcmp(locationExec, "COORDINATOR_ONLY") != 0)
			rest = lappend(rest, makeDefElem("gp.distributed_by",
											 (Node *) makeString("random"), -1));
	}

	stmt->servername = GP_EXTTABLE_SERVER_NAME;
	stmt->options = list_concat(list_concat(list_concat(formatOpts, extOptions),
											entryOptions),
								rest);
}
