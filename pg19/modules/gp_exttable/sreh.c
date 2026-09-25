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
 * sreh.c
 *	  Single row error handling -- SEGMENT REJECT LIMIT and LOG ERRORS --
 *	  for external tables and COPY, and the error logs it writes.
 *
 * An error log is a file per table under the node's data directory, as
 * Cloudberry's is: errlog/<database>_<table>, or for LOG ERRORS
 * PERSISTENTLY, which outlives the table, errlogpersistent/<database>_
 * <schema>_<name>.  A bad row is appended to it as it is found, so that it
 * stays whatever becomes of the transaction.  Each is a length, a CRC and the
 * row as a HeapTuple of the eight columns gp_read_error_log() returns.
 *
 * Cloudberry serializes the writers of a log with an LWLock of its server's.
 * gp_exttable is loaded by CREATE EXTENSION, not preloaded, and can ask for
 * no shared memory; a row is written with a single write() of the whole
 * record, in append mode, which no other writer's interleaves with, and a
 * reader stops at a record whose CRC does not hold, which is all a record
 * being written at that moment can be.
 *
 * The rows a scan rejected are counted where Cloudberry counts them, on the
 * segment, and the coordinator says how many there were, once, when the
 * statement ends: a segment tells it with a NOTICE it does not pass on
 * (SrehNoteRejected()).
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/cdb/cdbsreh.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/varlena.h"

#include "gp_core_api.h"
#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_exttable.h"

#define ErrorLogDir "errlog"
#define PersistentErrorLogDir "errlogpersistent"

#define ErrorLogNormalFileName(fname, dbId, relId) \
	snprintf(fname, MAXPGPATH, "%s/%u_%u", ErrorLogDir, dbId, relId)

#define ErrorLogPersistentFileName(fname, dbId, namespaceId, relName) \
	snprintf(fname, MAXPGPATH, "%s/%u_%u_%s", PersistentErrorLogDir, dbId, namespaceId, relName)

#define NUM_ERRORTABLE_ATTR 8
#define errtable_cmdtime 1
#define errtable_relname 2
#define errtable_filename 3
#define errtable_linenum 4
#define errtable_bytenum 5
#define errtable_errmsg 6
#define errtable_rawdata 7
#define errtable_rawbytes 8

typedef enum RejectLimitCode
{
	REJECT_NONE = 0,
	REJECT_FIRST_BAD_LIMIT,
	REJECT_LIMIT_REACHED
} RejectLimitCode;

int			gp_initial_bad_row_limit = 1000;
int			gp_reject_percent_threshold = 300;

PG_FUNCTION_INFO_V1(gp_read_error_log);
PG_FUNCTION_INFO_V1(gp_truncate_error_log);
PG_FUNCTION_INFO_V1(gp_read_persistent_error_log);
PG_FUNCTION_INFO_V1(gp_truncate_persistent_error_log);

static void
ErrorLogFileName(Oid dbid, Oid relid, bool persistent, char *fname)
{
	Assert(OidIsValid(relid) && OidIsValid(dbid));

	if (persistent)
	{
		char	   *relname = get_rel_name(relid);
		Oid			namespace = get_rel_namespace(relid);

		if (!OidIsValid(namespace))
			elog(ERROR, "relid %u does not exist for db %u", relid, dbid);
		ErrorLogPersistentFileName(fname, dbid, namespace, relname);
	}
	else
		ErrorLogNormalFileName(fname, dbid, relid);
}

CdbSreh *
makeCdbSreh(int rejectlimit, bool is_limit_in_rows, char *filename,
			char *relname, char logerrors)
{
	CdbSreh    *h = palloc0(sizeof(CdbSreh));

	h->rawdata = makeStringInfo();
	h->relname = relname;
	h->rejectlimit = rejectlimit;
	h->is_limit_in_rows = is_limit_in_rows;
	h->logerrors = logerrors;
	snprintf(h->filename, sizeof(h->filename), "%s",
			 filename ? filename : "<stdin>");
	h->badrowcontext = AllocSetContextCreate(CurrentMemoryContext,
											 "SrehMemCtxt",
											 ALLOCSET_DEFAULT_SIZES);
	return h;
}

void
destroyCdbSreh(CdbSreh *cdbsreh)
{
	MemoryContextDelete(cdbsreh->badrowcontext);
	pfree(cdbsreh->rawdata->data);
	pfree(cdbsreh->rawdata);
	pfree(cdbsreh);
}

static TupleDesc
GetErrorTupleDesc(void)
{
	static TupleDesc tupdesc = NULL;

	if (tupdesc == NULL)
	{
		TupleDesc	tmp;
		MemoryContext oldcontext = MemoryContextSwitchTo(CacheMemoryContext);

		tmp = CreateTemplateTupleDesc(NUM_ERRORTABLE_ATTR);
		TupleDescInitEntry(tmp, 1, "cmdtime", TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tmp, 2, "relname", TEXTOID, -1, 0);
		TupleDescInitEntry(tmp, 3, "filename", TEXTOID, -1, 0);
		TupleDescInitEntry(tmp, 4, "linenum", INT4OID, -1, 0);
		TupleDescInitEntry(tmp, 5, "bytenum", INT4OID, -1, 0);
		TupleDescInitEntry(tmp, 6, "errmsg", TEXTOID, -1, 0);
		TupleDescInitEntry(tmp, 7, "rawdata", TEXTOID, -1, 0);
		TupleDescInitEntry(tmp, 8, "rawbytes", BYTEAOID, -1, 0);
		TupleDescFinalize(tmp);
		MemoryContextSwitchTo(oldcontext);
		tupdesc = tmp;
	}
	return tupdesc;
}

static HeapTuple
FormErrorTuple(CdbSreh *cdbsreh)
{
	bool		nulls[NUM_ERRORTABLE_ATTR];
	Datum		values[NUM_ERRORTABLE_ATTR];
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(cdbsreh->badrowcontext);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, true, sizeof(nulls));

	values[errtable_cmdtime - 1] = TimestampTzGetDatum(GetCurrentStatementStartTimestamp());
	nulls[errtable_cmdtime - 1] = false;

	if (cdbsreh->linenumber > 0)
	{
		values[errtable_linenum - 1] = Int32GetDatum((int32) cdbsreh->linenumber);
		nulls[errtable_linenum - 1] = false;
	}

	values[errtable_rawdata - 1] = CStringGetTextDatum(cdbsreh->rawdata->data);
	nulls[errtable_rawdata - 1] = false;

	values[errtable_filename - 1] = CStringGetTextDatum(cdbsreh->filename);
	nulls[errtable_filename - 1] = false;

	values[errtable_relname - 1] = CStringGetTextDatum(cdbsreh->relname);
	nulls[errtable_relname - 1] = false;

	values[errtable_errmsg - 1] = CStringGetTextDatum(cdbsreh->errmsg);
	nulls[errtable_errmsg - 1] = false;

	MemoryContextSwitchTo(oldcontext);

	return heap_form_tuple(GetErrorTupleDesc(), values, nulls);
}

/*
 * Append a bad row to its table's error log: see the file's comment for why
 * one write() of the whole record.
 */
static void
ErrorLogWrite(CdbSreh *cdbsreh)
{
	HeapTuple	tuple;
	char		filename[MAXPGPATH];
	int			fd;
	pg_crc32c	crc;
	StringInfoData rec;
	uint32		len;

	Assert(OidIsValid(cdbsreh->relid));
	ErrorLogFileName(MyDatabaseId, cdbsreh->relid,
					 IS_LOG_ERRORS_PERSISTENTLY(cdbsreh->logerrors), filename);
	tuple = FormErrorTuple(cdbsreh);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, tuple->t_data, tuple->t_len);
	FIN_CRC32C(crc);

	/* format: 0-3 the length, 4-7 the CRC, 8- the tuple */
	initStringInfo(&rec);
	len = tuple->t_len;
	appendBinaryStringInfo(&rec, (char *) &len, sizeof(len));
	appendBinaryStringInfo(&rec, (char *) &crc, sizeof(crc));
	appendBinaryStringInfo(&rec, (char *) tuple->t_data, tuple->t_len);

	fd = OpenTransientFile(filename, O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	if (fd < 0 && errno == ENOENT)
	{
		const char *errordir = IS_LOG_ERRORS_PERSISTENTLY(cdbsreh->logerrors) ?
			PersistentErrorLogDir : ErrorLogDir;

		if (MakePGDirectory(errordir) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory for errorlog \"%s\": %m",
							errordir)));
		fd = OpenTransientFile(filename, O_WRONLY | O_CREAT | O_APPEND | PG_BINARY);
	}
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open \"%s\": %m", filename)));

	errno = 0;
	if (write(fd, rec.data, rec.len) != rec.len)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write error log \"%s\": %m", filename)));
	}
	CloseTransientFile(fd);

	pfree(rec.data);
	heap_freetuple(tuple);
}

void
HandleSingleRowError(CdbSreh *cdbsreh)
{
	cdbsreh->rejectcount++;

	if (IS_LOG_TO_FILE(cdbsreh->logerrors))
		ErrorLogWrite(cdbsreh);
}

void
ReportSrehResults(CdbSreh *cdbsreh, uint64 total_rejected)
{
	if (total_rejected > 0)
		ereport(NOTICE,
				(errmsg("found " UINT64_FORMAT " data formatting errors (" UINT64_FORMAT " or more input rows), rejected related input data",
						total_rejected, total_rejected)));
}

static RejectLimitCode
GetRejectLimitCode(CdbSreh *cdbsreh)
{
	RejectLimitCode code = REJECT_NONE;

	if (ExceedSegmentRejectHardLimit(cdbsreh))
		return REJECT_FIRST_BAD_LIMIT;

	if (cdbsreh->is_limit_in_rows)
	{
		if (cdbsreh->rejectcount >= cdbsreh->rejectlimit)
			code = REJECT_LIMIT_REACHED;
	}
	else
	{
		if (cdbsreh->processed > gp_reject_percent_threshold)
		{
			if ((cdbsreh->rejectcount * 100) / cdbsreh->processed >= cdbsreh->rejectlimit)
				code = REJECT_LIMIT_REACHED;
		}
	}

	return code;
}

void
ErrorIfRejectLimitReached(CdbSreh *cdbsreh)
{
	switch (GetRejectLimitCode(cdbsreh))
	{
		case REJECT_NONE:
			return;
		case REJECT_FIRST_BAD_LIMIT:
			ereport(ERROR,
					(errcode(ERRCODE_GP_REJECT_LIMIT_REACHED),
					 errmsg("all %d first rows in this segment were rejected",
							gp_initial_bad_row_limit),
					 errdetail("Aborting operation regardless of REJECT LIMIT value, last error was: %s",
							   cdbsreh->errmsg)));
			break;
		case REJECT_LIMIT_REACHED:
			ereport(ERROR,
					(errcode(ERRCODE_GP_REJECT_LIMIT_REACHED),
					 errmsg("segment reject limit reached, aborting operation"),
					 errdetail("Last error was: %s", cdbsreh->errmsg)));
			break;
	}
}

bool
ExceedSegmentRejectHardLimit(CdbSreh *cdbsreh)
{
	if (gp_initial_bad_row_limit == 0)
		return false;

	if (cdbsreh->processed == gp_initial_bad_row_limit &&
		cdbsreh->rejectcount >= gp_initial_bad_row_limit)
		return true;

	return false;
}

bool
IsRejectLimitReached(CdbSreh *cdbsreh)
{
	return GetRejectLimitCode(cdbsreh) != REJECT_NONE;
}

void
VerifyRejectLimit(char rejectlimittype, int rejectlimit)
{
	if (rejectlimittype == 'r')
	{
		if (rejectlimit < 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("segment reject limit in ROWS must be 2 or larger (got %d)",
							rejectlimit)));
	}
	else
	{
		Assert(rejectlimittype == 'p');
		if (rejectlimit < 1 || rejectlimit > 100)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("segment reject limit in PERCENT must be between 1 and 100 (got %d)",
							rejectlimit)));
	}
}

/* ------------------------------------------------------------------------- */
/* The count of rejected rows, for the coordinator to report                 */
/* ------------------------------------------------------------------------- */

/*
 * A segment's scan, as it ends, tells the coordinator how many rows it
 * rejected -- Cloudberry's QE sends its QD a 'j' message (SendNumRows()) --
 * and the coordinator adds them up and says how many there were once the
 * statement is done.  Here the segment raises a NOTICE with a SQLSTATE of
 * gp_exttable's, which gp_core's relay hands to gp_exttable rather than to
 * the client; where the scan ran on the coordinator itself, it adds its own.
 */
#define ERRCODE_GP_SREH_COUNT	MAKE_SQLSTATE('X','X','G','S','R')

static uint64 rejected_total = 0;

void
SrehNoteRejected(int64 rejected)
{
	int			role = GpClusterBackendRole();

	if (rejected <= 0)
		return;
	if (role == GP_ROLE_EXECUTE)
		ereport(NOTICE,
				(errcode(ERRCODE_GP_SREH_COUNT),
				 errmsg("gp_exttable rejected " INT64_FORMAT, rejected)));
	else
		rejected_total += rejected;
}

/* gp_core's relay: a segment's NOTICE, which may be ours */
bool		SrehRelayNotice(const char *sqlstate, const char *message);

bool
SrehRelayNotice(const char *sqlstate, const char *message)
{
	int64		n;

	if (sqlstate == NULL || strcmp(sqlstate, "XXGSR") != 0)
		return false;
	if (sscanf(message, "gp_exttable rejected " INT64_FORMAT, &n) == 1 && n > 0)
		rejected_total += n;
	return true;
}

/* What a statement that failed rejected: never said. */
void
SrehForgetRejected(void)
{
	rejected_total = 0;
}

/* As a statement ends on the coordinator, or on one node. */
void
SrehReportRejected(void)
{
	uint64		n = rejected_total;

	rejected_total = 0;
	if (n > 0)
		ReportSrehResults(NULL, n);
}

/* ------------------------------------------------------------------------- */
/* Reading and truncating the logs                                           */
/* ------------------------------------------------------------------------- */

/* One record of fp, or NULL at its end or at a record that does not hold. */
static HeapTuple
ErrorLogRead(FILE *fp, const char *fname)
{
	uint32		t_len;
	HeapTuple	tuple;
	pg_crc32c	crc,
				written_crc;

	if (fread(&t_len, 1, sizeof(uint32), fp) != sizeof(uint32))
		return NULL;
	if (t_len > MaxAllocSize - HEAPTUPLESIZE)
		return NULL;

	tuple = palloc(HEAPTUPLESIZE + t_len);
	tuple->t_len = t_len;
	ItemPointerSetInvalid(&tuple->t_self);
	tuple->t_tableOid = InvalidOid;
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	if (fread(&written_crc, 1, sizeof(pg_crc32c), fp) != sizeof(pg_crc32c) ||
		fread(tuple->t_data, 1, tuple->t_len, fp) != tuple->t_len)
	{
		pfree(tuple);
		return NULL;
	}

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, tuple->t_data, tuple->t_len);
	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, written_crc))
	{
		elog(LOG, "incorrect checksum in error log %s", fname);
		pfree(tuple);
		return NULL;
	}
	return tuple;
}

/* Every row of a log file into the tuplestore. */
static void
ErrorLogReadAll(const char *fname, ReturnSetInfo *rsinfo)
{
	FILE	   *fp = AllocateFile(fname, "r");
	HeapTuple	tuple;
	TupleDesc	tupdesc = GetErrorTupleDesc();

	if (fp == NULL)
		return;
	while ((tuple = ErrorLogRead(fp, fname)) != NULL)
	{
		Datum		values[NUM_ERRORTABLE_ATTR];
		bool		nulls[NUM_ERRORTABLE_ATTR];

		heap_deform_tuple(tuple, tupdesc, values, nulls);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		pfree(tuple);
	}
	FreeFile(fp);
}

/*
 * Where the error log of a table dropped since, persistently kept, is: by
 * the schema its name says, or the first on the search path that has one.
 */
static bool
RetrievePersistentErrorLogFromRangeVar(RangeVar *relrv, AclMode mode, char *fname)
{
	AclResult	aclresult;
	bool		findfile = false;
	Oid			namespaceId = InvalidOid;
	char	   *schemaname = NULL;
	Oid			relid;

	relid = RangeVarGetRelid(relrv, NoLock, true);
	if (OidIsValid(relid))
	{
		aclresult = pg_class_aclcheck(relid, GetUserId(), mode);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLE, relrv->relname);

		ErrorLogFileName(MyDatabaseId, relid, true, fname);
		return true;
	}

	if (relrv->schemaname)
	{
		namespaceId = LookupExplicitNamespace(relrv->schemaname, true);
		if (OidIsValid(namespaceId))
		{
			schemaname = relrv->schemaname;
			findfile = true;
			ErrorLogPersistentFileName(fname, MyDatabaseId, namespaceId, relrv->relname);
		}
	}
	else
	{
		SearchPathMatcher *path = GetSearchPathMatcher(CurrentMemoryContext);
		ListCell   *cell;
		char		filename[MAXPGPATH];

		foreach(cell, path->schemas)
		{
			namespaceId = lfirst_oid(cell);
			ErrorLogPersistentFileName(filename, MyDatabaseId, namespaceId, relrv->relname);
			if (access(filename, R_OK) == 0)
			{
				schemaname = get_namespace_name(namespaceId);
				strlcpy(fname, filename, MAXPGPATH);
				findfile = true;
				break;
			}
		}
	}
	if (findfile)
	{
		aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), mode);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA, schemaname);
		return true;
	}
	return false;
}

/*
 * gp_read_error_log(text): the rows every node's log of the table has --
 * the segments', which external tables write, and the coordinator's, which
 * COPY writes, it parsing COPY's rows here where Cloudberry's segments do.
 */
Datum
gp_read_error_log(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	text	   *relname = PG_GETARG_TEXT_PP(0);
	RangeVar   *relrv;
	Oid			relid;
	char		fname[MAXPGPATH];

	if (!GpDispatchFunctionToSegments(fcinfo))
		InitMaterializedSRF(fcinfo, 0);

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	relid = RangeVarGetRelid(relrv, NoLock, true);

	/* If the relation has gone, no rows. */
	if (OidIsValid(relid))
	{
		AclResult	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLE, relrv->relname);
		ErrorLogNormalFileName(fname, MyDatabaseId, relid);
		ErrorLogReadAll(fname, rsinfo);
	}
	return (Datum) 0;
}

Datum
gp_read_persistent_error_log(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	text	   *relname = PG_GETARG_TEXT_PP(0);
	RangeVar   *relrv;
	char		fname[MAXPGPATH];

	if (!GpDispatchFunctionToSegments(fcinfo))
		InitMaterializedSRF(fcinfo, 0);

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	if (RetrievePersistentErrorLogFromRangeVar(relrv, ACL_SELECT, fname))
		ErrorLogReadAll(fname, rsinfo);
	return (Datum) 0;
}

static bool
ErrorLogPrefixDelete(Oid databaseId, Oid namespaceId, bool persistent)
{
	char		filename[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;
	const char *dirpath = persistent ? PersistentErrorLogDir : ErrorLogDir;
	char		prefix[MAXPGPATH];

	prefix[0] = '\0';
	if (OidIsValid(databaseId))
	{
		if (OidIsValid(namespaceId))
			snprintf(prefix, sizeof(prefix), "%u_%u_", databaseId, namespaceId);
		else
			snprintf(prefix, sizeof(prefix), "%u_", databaseId);
	}

	dir = AllocateDir(dirpath);
	if (dir == NULL)
		return false;

	while ((de = ReadDirExtended(dir, dirpath, LOG)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		if (!OidIsValid(databaseId) ||
			strncmp(de->d_name, prefix, strlen(prefix)) == 0)
		{
			if (snprintf(filename, MAXPGPATH, "%s/%s", dirpath, de->d_name) >= MAXPGPATH - 1)
			{
				ereport(WARNING,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("log filename truncation on \"%s\", unable to delete error log",
								de->d_name)));
				continue;
			}
			unlink(filename);
		}
	}

	FreeDir(dir);
	return true;
}

bool
PersistentErrorLogDelete(Oid databaseId, Oid namespaceId, const char *fname)
{
	if (fname == NULL)
		return ErrorLogPrefixDelete(databaseId, namespaceId, true);
	return unlink(fname) == 0;
}

bool
ErrorLogDelete(Oid databaseId, Oid relationId)
{
	char		filename[MAXPGPATH];

	if (!OidIsValid(relationId))
		return ErrorLogPrefixDelete(databaseId, InvalidOid, false);

	ErrorLogNormalFileName(filename, databaseId, relationId);
	return unlink(filename) == 0;
}

static bool
TruncateErrorLog(text *relname, bool persistent)
{
	char	   *relname_str = text_to_cstring(relname);
	bool		allResults = true;

	if (strcmp(relname_str, "*.*") == 0)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to delete all error log files")));
		ErrorLogPrefixDelete(InvalidOid, InvalidOid, persistent);
	}
	else if (strcmp(relname_str, "*") == 0)
	{
		if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));
		ErrorLogPrefixDelete(MyDatabaseId, InvalidOid, persistent);
	}
	else
	{
		RangeVar   *relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));

		if (persistent)
		{
			char		filename[MAXPGPATH];

			if (RetrievePersistentErrorLogFromRangeVar(relrv, ACL_TRUNCATE, filename))
				PersistentErrorLogDelete(MyDatabaseId, InvalidOid, filename);
		}
		else
		{
			Oid			relid = RangeVarGetRelid(relrv, NoLock, true);
			AclResult	aclresult;

			if (!OidIsValid(relid))
				return false;

			aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_TRUNCATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLE, relrv->relname);

			ErrorLogDelete(MyDatabaseId, relid);
		}
	}

	/* And on the segments, each of which has to find the table too. */
	if (!GpClusterIsSingleNode() && GpClusterBackendRole() == GP_ROLE_DISPATCH)
	{
		char	   *sql = psprintf("SELECT pg_catalog.%s(%s)",
								   persistent ? "gp_truncate_persistent_error_log" : "gp_truncate_error_log",
								   quote_literal_cstr(relname_str));
		GpGatherState *gather;
		TupleDesc	desc = CreateTemplateTupleDesc(1);
		TupleTableSlot *slot;

		TupleDescInitEntry(desc, 1, "ok", BOOLOID, -1, 0);
		TupleDescFinalize(desc);
		gather = GpGatherStart(sql, desc);
		slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		while (GpGatherNext(gather, slot, NULL))
		{
			bool		isnull;
			Datum		v = slot_getattr(slot, 1, &isnull);

			allResults &= (!isnull && DatumGetBool(v));
		}
		GpGatherEnd(gather);
		ExecDropSingleTupleTableSlot(slot);
	}

	return allResults;
}

Datum
gp_truncate_error_log(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(TruncateErrorLog(PG_GETARG_TEXT_PP(0), false));
}

Datum
gp_truncate_persistent_error_log(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(TruncateErrorLog(PG_GETARG_TEXT_PP(0), true));
}
