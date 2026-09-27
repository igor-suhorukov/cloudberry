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
 * external.h
 *	  Cloudberry's external tables, as gp_exttable carries them: what a
 *	  foreign table of gp_exttable_server says, and what a module of another
 *	  library reads and writes COPY data with.
 *
 * Cloudberry's own header, src/include/access/external.h, was its server's,
 * and the modules of gpcontrib built against it: gpcloud's s3:// protocol
 * asks it for the table it reads or writes.  Here it is gp_exttable's.
 *
 * The rest is the port's.  Cloudberry's COPY takes single row error handling
 * and a callback of its caller's, and exports its per-row writer
 * (CopyOneRowTo()), which pxf_fdw reads and writes its server's data with;
 * PostgreSQL 19's COPY does neither, and gp_exttable does both for its own
 * scans and writes (extaccess.c, copyout.c).  So a scan of data a caller
 * reads itself -- ExtScanSourceBegin() -- is an external table's scan whose
 * location is the caller's read function, and a row is formatted by
 * ExtCopyOutRow().
 *
 * Inside gp_exttable these are its functions.  A module of another library
 * finds them with load_external_function(), once each, as gp_sql finds
 * gp_exttable's and as PostgreSQL's transform modules find hstore's: the
 * server opens a library with RTLD_NOW, so a library that named them would
 * not load in a backend that had not loaded gp_exttable, which nothing
 * preloads.  Finding them loads it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTERNAL_H
#define EXTERNAL_H

#include "access/htup.h"
#include "commands/copy.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

#define fmttype_is_custom(c) ((c) == 'b')
#define fmttype_is_text(c)   ((c) == 't')
#define fmttype_is_csv(c)    ((c) == 'c')

typedef struct ExtTableEntry
{
	List	   *urilocations;	/* String */
	List	   *execlocations;	/* one String: ALL_SEGMENTS, HOST:h, ... */
	char		fmtcode;		/* 't', 'c' or 'b' */
	List	   *options;		/* the rest, for COPY or a formatter */
	char	   *command;		/* EXECUTE's */
	int			rejectlimit;	/* -1 for none */
	char		rejectlimittype;	/* 'r' rows, 'p' percent */
	char		logerrors;		/* LOG_ERRORS_* */
	int			encoding;
	bool		iswritable;
} ExtTableEntry;

/* LOG ERRORS, as a table or a scan says it */
#define LOG_ERRORS_ENABLE			't'
#define LOG_ERRORS_PERSISTENTLY		'p'
#define LOG_ERRORS_DISABLE			'f'

/* A scan of gp_exttable's (extaccess.c), whose data is a caller's. */
typedef struct FileScanDescData *FileScanDesc;

/*
 * Where such a scan's data comes from: the caller's function, which fills
 * buf with up to len bytes and says how many, 0 at the end of the data.
 */
typedef int (*ExtSourceRead) (void *arg, char *buf, int len);

/*
 * A writer's rows as COPY TO writes them, in text or CSV, each appended to
 * line with its newline (copyout.c).
 */
typedef struct ExtCopyOut
{
	CopyFormatOptions opts;
	int			file_encoding;
	bool		need_transcoding;
	bool		encoding_embeds_ascii;
	TupleDesc	tupdesc;
	FmgrInfo   *out_functions;
	List	   *attnumlist;
	StringInfoData line;		/* the rows made, for the caller to send */
	MemoryContext rowcontext;
} ExtCopyOut;

#ifdef GP_EXTTABLE_INTERNAL

extern bool rel_is_external_table(Oid relid);
extern List *TokenizeLocationUris(char *locations);
extern PGDLLEXPORT ExtTableEntry *GetExtTableEntry(Oid relid);
extern ExtTableEntry *GetExtTableEntryIfExists(Oid relid);
extern ExtTableEntry *GetExtFromForeignTableOptions(List *ftoptions, Oid relid);

extern PGDLLEXPORT FileScanDesc ExtScanSourceBegin(Relation rel,
												   const char *source,
												   char fmtType, List *options,
												   int rejLimit,
												   bool rejLimitInRows,
												   char logErrors,
												   ExtSourceRead read,
												   void *arg);
extern PGDLLEXPORT HeapTuple ExtScanSourceNext(FileScanDesc scan);
extern PGDLLEXPORT void ExtScanSourceRescan(FileScanDesc scan);
extern PGDLLEXPORT void ExtScanSourceEnd(FileScanDesc scan);

extern PGDLLEXPORT ExtCopyOut *ExtCopyOutBegin(Relation rel, List *options);
extern PGDLLEXPORT void ExtCopyOutRow(ExtCopyOut *co, TupleTableSlot *slot);
extern PGDLLEXPORT void ExtCopyOutEnd(ExtCopyOut *co);

#else							/* a module of another library */

/* gp_exttable's function of that name, found once */
#define GP_EXTTABLE_FUNCTION(ptr, name) \
	do { \
		if ((ptr) == NULL) \
			(ptr) = (__typeof__(ptr)) \
				load_external_function("$libdir/gp_exttable", (name), true, NULL); \
	} while (0)

/* The table's options as Cloudberry's pg_exttable kept them. */
static inline ExtTableEntry *
GetExtTableEntry(Oid relid)
{
	static ExtTableEntry *(*fn) (Oid) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "GetExtTableEntry");
	return fn(relid);
}

/*
 * A scan of rel's rows in the data read reads, in fmtType's format ('t' text,
 * 'c' CSV) with options -- COPY's, and Cloudberry's fill_missing_fields and
 * newline -- and SEGMENT REJECT LIMIT rejLimit (-1 for none), rows or
 * percent, whose rows LOG ERRORS logs with source as their file.
 */
static inline FileScanDesc
ExtScanSourceBegin(Relation rel, const char *source, char fmtType,
				   List *options, int rejLimit, bool rejLimitInRows,
				   char logErrors, ExtSourceRead read, void *arg)
{
	static FileScanDesc (*fn) (Relation, const char *, char, List *, int,
							   bool, char, ExtSourceRead, void *) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtScanSourceBegin");
	return fn(rel, source, fmtType, options, rejLimit, rejLimitInRows,
			  logErrors, read, arg);
}

/* The scan's next row, NULL at its end; the next call frees it. */
static inline HeapTuple
ExtScanSourceNext(FileScanDesc scan)
{
	static HeapTuple (*fn) (FileScanDesc) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtScanSourceNext");
	return fn(scan);
}

/* The scan again from the start, the source's read function called afresh. */
static inline void
ExtScanSourceRescan(FileScanDesc scan)
{
	static void (*fn) (FileScanDesc) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtScanSourceRescan");
	fn(scan);
}

/* The scan's end: the rows it rejected counted, for the statement's NOTICE. */
static inline void
ExtScanSourceEnd(FileScanDesc scan)
{
	static void (*fn) (FileScanDesc) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtScanSourceEnd");
	fn(scan);
}

/* A writer of rel's rows with COPY TO's options. */
static inline ExtCopyOut *
ExtCopyOutBegin(Relation rel, List *options)
{
	static ExtCopyOut *(*fn) (Relation, List *) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtCopyOutBegin");
	return fn(rel, options);
}

/* A row, appended to co->line with its newline. */
static inline void
ExtCopyOutRow(ExtCopyOut *co, TupleTableSlot *slot)
{
	static void (*fn) (ExtCopyOut *, TupleTableSlot *) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtCopyOutRow");
	fn(co, slot);
}

static inline void
ExtCopyOutEnd(ExtCopyOut *co)
{
	static void (*fn) (ExtCopyOut *) = NULL;

	GP_EXTTABLE_FUNCTION(fn, "ExtCopyOutEnd");
	fn(co);
}

#endif							/* GP_EXTTABLE_INTERNAL */

#endif							/* EXTERNAL_H */
