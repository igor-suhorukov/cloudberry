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
 * dircopy.c
 *	  Cloudberry's COPY of a directory table's file, in and out.
 *
 * Cloudberry puts a file into a directory table with
 *
 *	  COPY BINARY t FROM {'file' | PROGRAM 'command' | STDIN} 'path' [WITH TAG 'tag']
 *
 * and takes one out with
 *
 *	  COPY BINARY DIRECTORY TABLE t 'path' TO {'file' | PROGRAM 'command' | STDOUT}
 *
 * which gpdirtableload and psql's \copy write.  O26's grammar makes each a
 * COPY PostgreSQL's parser takes, the path and the tag carried on it as
 * options in gp_sql's namespace (gp_desugar.c), and this carries it out: the
 * source read whole -- a server's file, a program's output, or the client's
 * copy stream -- and put as directory_table_put() puts it, on a cluster on
 * the segment its path hashes to; or the file got and written whole to the
 * destination.  Cloudberry's checks come first, in its words
 * (ProcessCopyDirectoryTableOptions(), copy.c; BeginCopyFromDirectoryTable(),
 * copyfrom.c; BeginCopyToDirectoryTable(), copyto.c), and PostgreSQL's own
 * of who may read or write a server's file or run a program (DoCopy()).
 *
 * A COPY of a directory table with no path, to the client or from it, moves
 * its rows, as pg_dump's COPY does and a restore's does back; to or from a
 * file or a program it is refused, as Cloudberry refuses it.
 *
 * A file is one value: 1 GB at most, where Cloudberry streams it.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/commands/copy.c, copyfrom.c and copyto.c, their directory
 *	  table parts
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "commands/defrem.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "miscadmin.h"
#include "parser/parse_node.h"
#include "port.h"
#include "storage/fd.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/memutils.h"

#include "gp_sql.h"

/* The pieces a file is sent to the client in, as COPY TO STDOUT's rows are. */
#define DIRCOPY_CHUNK	(64 * 1024)

/* What a COPY of a directory table's file says besides its source. */
typedef struct DirCopyOptions
{
	const char *path;			/* the file's relative path, or NULL */
	const char *tag;			/* its tag, or NULL */
	bool		binary;			/* FORMAT binary, which COPY BINARY says */
} DirCopyOptions;

/*
 * The options of the statement, in Cloudberry's order of checks
 * (ProcessCopyDirectoryTableOptions()): one format, binary; one tag, the
 * grammar's WITH TAG or a (tag 'x') of the option list; nothing else.  The
 * path and the tag the grammar carried are gp_sql's.
 */
static void
dircopy_options(ParseState *pstate, List *options, DirCopyOptions *opts)
{
	bool		format_specified = false;

	memset(opts, 0, sizeof(DirCopyOptions));
	foreach_node(DefElem, defel, options)
	{
		if (defel->defnamespace != NULL &&
			strcmp(defel->defnamespace, "gp_sql") == 0)
		{
			if (strcmp(defel->defname, "directory_path") == 0)
				opts->path = defGetString(defel);
			else if (strcmp(defel->defname, "directory_tag") == 0)
				opts->tag = defGetString(defel);
			continue;
		}
		if (defel->defnamespace != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s.%s\" not recognized",
							defel->defnamespace, defel->defname),
					 parser_errposition(pstate, defel->location)));

		if (strcmp(defel->defname, "format") == 0)
		{
			if (format_specified)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 parser_errposition(pstate, defel->location)));
			format_specified = true;
			if (strcmp(defGetString(defel), "binary") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("format option is not allowed in copy binary from directory table."),
						 parser_errposition(pstate, defel->location)));
			opts->binary = true;
		}
		else if (strcmp(defel->defname, "tag") == 0)
		{
			if (opts->tag != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 parser_errposition(pstate, defel->location)));
			opts->tag = defGetString(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname),
					 parser_errposition(pstate, defel->location)));
	}
}

/* PostgreSQL's rule of who may COPY from or to a server's file or program. */
static void
dircopy_check_privileges(CopyStmt *stmt)
{
	if (stmt->filename == NULL)
		return;
	if (stmt->is_program)
	{
		if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to COPY to or from an external program"),
					 errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
							   "pg_execute_server_program"),
					 errhint("Anyone can COPY to stdout or from stdin. "
							 "psql's \\copy command also works for anyone.")));
	}
	else if (stmt->is_from &&
			 !has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to COPY from a file"),
				 errdetail("Only roles with privileges of the \"%s\" role may COPY from a file.",
						   "pg_read_server_files"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));
	else if (!stmt->is_from &&
			 !has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to COPY to a file"),
				 errdetail("Only roles with privileges of the \"%s\" role may COPY to a file.",
						   "pg_write_server_files"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));
}

/* Append what is left of a stream to buf, a value's worth at most. */
static void
dircopy_read_stream(FILE *fp, const char *name, StringInfo buf)
{
	char		chunk[DIRCOPY_CHUNK];
	size_t		n;

	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
	{
		if ((Size) buf->len + n > MaxAllocSize - VARHDRSZ - 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the file of \"%s\" is too large to put in a directory table", name),
					 errdetail("A directory table's file is one value, 1 GB at most.")));
		appendBinaryStringInfo(buf, chunk, n);
	}
	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from COPY file: %m")));
}

/*
 * The client's copy stream, whole, as CopyGetData() reads a COPY FROM
 * STDIN's (copyfromparse.c): CopyInResponse, then CopyData messages to a
 * CopyDone.
 */
static void
dircopy_read_client(StringInfo buf)
{
	StringInfoData msg;

	if (whereToSendOutput != DestRemote)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY FROM STDIN of a directory table's file needs a client")));

	pq_beginmessage(&msg, PqMsg_CopyInResponse);
	pq_sendbyte(&msg, 1);		/* binary */
	pq_sendint16(&msg, 1);
	pq_sendint16(&msg, 1);
	pq_endmessage(&msg);
	pq_flush();

	initStringInfo(&msg);
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
		resetStringInfo(&msg);
		if (pq_getmessage(&msg, maxmsglen))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		RESUME_CANCEL_INTERRUPTS();

		if (mtype == PqMsg_CopyDone)
			break;
		if (mtype == PqMsg_CopyFail)
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("COPY from stdin failed: %s",
							pq_getmsgstring(&msg))));
		if (mtype != PqMsg_CopyData)
			continue;			/* Flush and Sync, as COPY ignores them */
		if ((Size) buf->len + msg.len > MaxAllocSize - VARHDRSZ - 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the file from stdin is too large to put in a directory table"),
					 errdetail("A directory table's file is one value, 1 GB at most.")));
		appendBinaryStringInfo(buf, msg.data, msg.len);
	}
	pfree(msg.data);
}

/* The content COPY FROM names, as one value. */
static bytea *
dircopy_read_source(CopyStmt *stmt)
{
	StringInfoData buf;
	bytea	   *content;

	initStringInfo(&buf);
	if (stmt->filename == NULL)
		dircopy_read_client(&buf);
	else if (stmt->is_program)
	{
		FILE	   *fp = OpenPipeStream(stmt->filename, PG_BINARY_R);
		int			rc;

		if (fp == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							stmt->filename)));
		dircopy_read_stream(fp, stmt->filename, &buf);
		rc = ClosePipeStream(fp);
		if (rc == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close pipe to external command: %m")));
		else if (rc != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("program \"%s\" failed", stmt->filename),
					 errdetail_internal("%s", wait_result_to_str(rc))));
	}
	else
	{
		FILE	   *fp = AllocateFile(stmt->filename, PG_BINARY_R);
		struct stat st;

		if (fp == NULL)
		{
			int			save_errno = errno;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							stmt->filename),
					 (save_errno == ENOENT || save_errno == EACCES) ?
					 errhint("COPY FROM instructs the PostgreSQL server process to read a file. "
							 "You may want a client-side facility such as psql's \\copy.") : 0));
		}
		if (fstat(fileno(fp), &st))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", stmt->filename)));
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", stmt->filename)));
		dircopy_read_stream(fp, stmt->filename, &buf);
		FreeFile(fp);
	}

	content = (bytea *) palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(content, VARHDRSZ + buf.len);
	memcpy(VARDATA(content), buf.data, buf.len);
	pfree(buf.data);
	return content;
}

/* Write it all, or say why not. */
static void
dircopy_write_stream(FILE *fp, const char *data, int len)
{
	if (len > 0 && fwrite(data, 1, len, fp) != (size_t) len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to COPY file: %m")));
}

/*
 * A file's content to where COPY TO names: a server's file, which only an
 * absolute path may name, a program's input, or the client, as COPY TO
 * STDOUT sends its rows (SendCopyBegin(), copyto.c).
 */
static void
dircopy_write_destination(CopyStmt *stmt, const char *data, int len)
{
	if (stmt->filename == NULL)
	{
		StringInfoData msg;

		if (whereToSendOutput != DestRemote)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY TO STDOUT of a directory table's file needs a client")));
		pq_beginmessage(&msg, PqMsg_CopyOutResponse);
		pq_sendbyte(&msg, 1);	/* binary */
		pq_sendint16(&msg, 1);
		pq_sendint16(&msg, 1);
		pq_endmessage(&msg);
		for (int off = 0; off < len; off += DIRCOPY_CHUNK)
			(void) pq_putmessage(PqMsg_CopyData, data + off,
								 Min(len - off, DIRCOPY_CHUNK));
		pq_putemptymessage(PqMsg_CopyDone);
	}
	else if (stmt->is_program)
	{
		FILE	   *fp = OpenPipeStream(stmt->filename, PG_BINARY_W);
		int			rc;

		if (fp == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							stmt->filename)));
		dircopy_write_stream(fp, data, len);
		rc = ClosePipeStream(fp);
		if (rc == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close pipe to external command: %m")));
		else if (rc != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("program \"%s\" failed", stmt->filename),
					 errdetail_internal("%s", wait_result_to_str(rc))));
	}
	else
	{
		FILE	   *fp;
		mode_t		oumask;

		if (!is_absolute_path(stmt->filename))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("relative path not allowed for COPY to file")));
		oumask = umask(S_IWGRP | S_IWOTH);
		PG_TRY();
		{
			fp = AllocateFile(stmt->filename, PG_BINARY_W);
		}
		PG_FINALLY();
		{
			umask(oumask);
		}
		PG_END_TRY();
		if (fp == NULL)
		{
			int			save_errno = errno;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for writing: %m",
							stmt->filename),
					 (save_errno == ENOENT || save_errno == EACCES) ?
					 errhint("COPY TO instructs the PostgreSQL server process to write a file. "
							 "You may want a client-side facility such as psql's \\copy.") : 0));
		}
		dircopy_write_stream(fp, data, len);
		if (FreeFile(fp))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", stmt->filename)));
	}
}

bool
GpDirTableCopy(CopyStmt *stmt, const char *queryString, QueryCompletion *qc)
{
	ParseState *pstate;
	DirCopyOptions opts;
	Oid			relid;
	uint64		processed;

	if (stmt->relation == NULL)
		return false;

	memset(&opts, 0, sizeof(opts));
	foreach_node(DefElem, defel, stmt->options)
	{
		if (defel->defnamespace != NULL && strcmp(defel->defnamespace, "gp_sql") == 0 &&
			strcmp(defel->defname, "directory_path") == 0)
			opts.path = defGetString(defel);
	}

	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid) || GpDirTableLocation(relid) == NULL)
	{
		if (opts.path != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a directory table",
							stmt->relation->relname)));
		return false;
	}

	/* no path: its rows, to or from the client, as pg_dump moves them */
	if (opts.path == NULL)
	{
		if (stmt->filename == NULL && !stmt->is_program)
			return false;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 stmt->is_from ?
				 errmsg("Copy from directory table file name can't be null.") :
				 errmsg("COPY to directory table must specify the relative_path name.")));
	}

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	dircopy_options(pstate, stmt->options, &opts);
	if (!opts.binary)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 stmt->is_from ?
				 errmsg("Only support copy binary from directory table.") :
				 errmsg("Only support copy binary directory table to.")));
	if (stmt->attlist != NIL || stmt->whereClause != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("a COPY of a directory table's file takes no column list and no WHERE clause")));
	dircopy_check_privileges(stmt);

	if (stmt->is_from)
	{
		GpDirTablePut(relid, opts.path, dircopy_read_source(stmt), opts.tag);
		processed = 1;
	}
	else
	{
		bytea	   *content = GpDirTableGet(relid, opts.path);

		dircopy_write_destination(stmt, content ? VARDATA_ANY(content) : NULL,
								  content ? VARSIZE_ANY_EXHDR(content) : 0);
		processed = content != NULL ? 1 : 0;
	}

	if (qc != NULL)
		SetQueryCompletion(qc, CMDTAG_COPY, processed);
	return true;
}
