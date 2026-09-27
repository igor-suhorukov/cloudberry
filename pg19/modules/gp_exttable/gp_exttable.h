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
 * gp_exttable.h
 *	  External tables: what the module's files share.
 *
 * Cloudberry spread these over access/external.h, access/url.h,
 * access/extprotocol.h, access/formatter.h, cdb/cdbsreh.h, utils/uri.h and
 * gp_exttable_fdw's extaccess.h, headers of its server; here they are one
 * module's, and the four of access/ its headers for the modules of other
 * libraries too (include/access/).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_EXTTABLE_H
#define GP_EXTTABLE_H

#include "access/htup.h"
#include "access/sdir.h"
#include "access/tupdesc.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

/*
 * What gp_exttable shares with the modules of other libraries, whose copies
 * of these find its functions (external.h): here, its own.
 */
#define GP_EXTTABLE_INTERNAL
#include "access/external.h"
#include "access/url.h"

/* The foreign server every external table is a foreign table of. */
#define GP_EXTTABLE_SERVER_NAME		"gp_exttable_server"
#define GP_EXTTABLE_FDW_NAME		"gp_exttable_fdw"

/* ------------------------------------------------------------------------- */
/* URIs (uri.c; Cloudberry's utils/misc/uriparser.c)                         */
/* ------------------------------------------------------------------------- */

typedef enum UriProtocol
{
	URI_FILE,
	URI_FTP,
	URI_HTTP,
	URI_GPFDIST,
	URI_CUSTOM,
	URI_GPFDISTS
} UriProtocol;

#define PROTOCOL_FILE		"file://"
#define PROTOCOL_FTP		"ftp://"
#define PROTOCOL_HTTP		"http://"
#define PROTOCOL_GPFDIST	"gpfdist://"
#define PROTOCOL_GPFDISTS	"gpfdists://"

#define IS_FILE_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_FILE, strlen(PROTOCOL_FILE)) == 0)
#define IS_HTTP_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_HTTP, strlen(PROTOCOL_HTTP)) == 0)
#define IS_GPFDIST_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_GPFDIST, strlen(PROTOCOL_GPFDIST)) == 0)
#define IS_GPFDISTS_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_GPFDISTS, strlen(PROTOCOL_GPFDISTS)) == 0)
#define IS_FTP_URI(uri_str) (pg_strncasecmp(uri_str, PROTOCOL_FTP, strlen(PROTOCOL_FTP)) == 0)

typedef struct Uri
{
	UriProtocol protocol;
	char	   *hostname;
	int			port;
	char	   *path;
	char	   *customprotocol;
} Uri;

extern Uri *ParseExternalTableUri(const char *uri);
extern void FreeExternalTableUri(Uri *uri);

/* ------------------------------------------------------------------------- */
/* What a foreign table of gp_exttable_server says (external.c)              */
/* ------------------------------------------------------------------------- */

/*
 * What a scan of an external table needs, which Cloudberry keeps in its own
 * ExternalScanInfo node.  A plan's fdw_private is copied and written out as
 * text when a slice is dispatched, so it has to be of nodes PostgreSQL knows:
 * ExternalScanInfoToList() and ExternalScanInfoFromList() make it a List.
 */
typedef struct ExternalScanInfo
{
	List	   *uriList;		/* String per segment, "" for none */
	char		fmtType;
	bool		isMasterOnly;
	int			rejLimit;
	bool		rejLimitInRows;
	char		logErrors;
	int			encoding;
	uint32		scancounter;
	List	   *extOptions;		/* DefElem */
} ExternalScanInfo;

extern ExternalScanInfo *MakeExternalScanInfo(ExtTableEntry *extEntry, Oid relid);
extern List *ExternalScanInfoToList(ExternalScanInfo *info);
extern ExternalScanInfo *ExternalScanInfoFromList(List *list);

/* ------------------------------------------------------------------------- */
/* Single row error handling (sreh.c; Cloudberry's cdb/cdbsreh.c)            */
/* ------------------------------------------------------------------------- */

#define IS_LOG_TO_FILE(c)				((c) == 't' || (c) == 'p')
#define IS_LOG_ERRORS_ENABLE(c)			((c) == 't')
#define IS_LOG_ERRORS_PERSISTENTLY(c)	((c) == 'p')
#define IS_LOG_ERRORS_DISABLE(c)		((c) == 'f')

/* Cloudberry's ERRCODE_T_R_GP_REJECT_LIMIT_REACHED, class 54 */
#define ERRCODE_GP_REJECT_LIMIT_REACHED	MAKE_SQLSTATE('5','4','0','0','0')

typedef struct CdbSreh
{
	/* bad row information */
	char	   *errmsg;			/* the error message for this bad data row */
	StringInfo	rawdata;		/* the bad data row which may contain \0 */
	char	   *relname;		/* target relation */
	int64		linenumber;		/* line number of error in original file */
	uint64		processed;		/* num logical input rows processed so far */
	bool		is_server_enc;	/* was bad row converted to server encoding? */

	/* reject limit state */
	int			rejectlimit;	/* SEGMENT REJECT LIMIT value */
	int64		rejectcount;	/* how many were rejected so far */
	bool		is_limit_in_rows;	/* ROWS = true, PERCENT = false */

	MemoryContext badrowcontext;	/* per-badrow evaluation context */
	char		filename[MAXPGPATH];	/* "uri [filename]" */
	char		logerrors;
	Oid			relid;
} CdbSreh;

extern int	gp_initial_bad_row_limit;
extern int	gp_reject_percent_threshold;

extern CdbSreh *makeCdbSreh(int rejectlimit, bool is_limit_in_rows,
							char *filename, char *relname, char logerrors);
extern void destroyCdbSreh(CdbSreh *cdbsreh);
extern void HandleSingleRowError(CdbSreh *cdbsreh);
extern void ReportSrehResults(CdbSreh *cdbsreh, uint64 total_rejected);
extern void ErrorIfRejectLimitReached(CdbSreh *cdbsreh);
extern bool ExceedSegmentRejectHardLimit(CdbSreh *cdbsreh);
extern bool IsRejectLimitReached(CdbSreh *cdbsreh);
extern void VerifyRejectLimit(char rejectlimittype, int rejectlimit);
extern bool ErrorLogDelete(Oid databaseId, Oid relationId);
extern bool PersistentErrorLogDelete(Oid databaseId, Oid namespaceId,
									 const char *fname);
extern void SrehNoteRejected(int64 rejected);
extern void SrehReportRejected(void);
extern void SrehForgetRejected(void);

/* ------------------------------------------------------------------------- */
/* Formatters and protocols of the user's (formatter.h, extprotocol.h)       */
/* ------------------------------------------------------------------------- */

/*
 * A formatter's or a protocol's function is called with one of these as its
 * fcinfo->context, as Cloudberry's server calls it: Cloudberry's own API,
 * which a formatter or a protocol of the user's builds against too.
 */
#include "access/extprotocol.h"
#include "access/formatter.h"

typedef struct ExternalSelectDescData
{
	ProjectionInfo *projInfo;
	List	   *filter_quals;
} ExternalSelectDescData;

/* ------------------------------------------------------------------------- */
/* The data sources (url*.c)                                                 */
/* ------------------------------------------------------------------------- */

extern bool gp_external_enable_exec;
extern int	gp_external_max_segs;
extern bool gp_external_enable_filter_pushdown;
extern int	writable_external_table_bufsize;
extern bool verify_gpfdists_cert;

extern URL_FILE *url_file_fopen(char *url, bool forwrite, extvar_t *ev,
								CopyFormatOptions *opts, char *relname);
extern void url_file_fclose(URL_FILE *file, bool failOnError, const char *relname);
extern bool url_file_feof(URL_FILE *file, int bytesread);
extern bool url_file_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen);
extern size_t url_file_fread(void *ptr, size_t size, URL_FILE *file,
							 CopyFromState pstate);

extern URL_FILE *url_execute_fopen(char *url, bool forwrite, extvar_t *ev);
extern void url_execute_fclose(URL_FILE *file, bool failOnError, const char *relname);
extern bool url_execute_feof(URL_FILE *file, int bytesread);
extern bool url_execute_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen);
extern size_t url_execute_fread(void *ptr, size_t size, URL_FILE *file,
								CopyFromState pstate);
extern size_t url_execute_fwrite(void *ptr, size_t size, URL_FILE *file);

extern URL_FILE *url_curl_fopen(char *url, bool forwrite, extvar_t *ev,
								CopyFormatOptions *opts);
extern void url_curl_fclose(URL_FILE *file, bool failOnError, const char *relname);
extern bool url_curl_feof(URL_FILE *file, int bytesread);
extern bool url_curl_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen);
extern size_t url_curl_fread(void *ptr, size_t size, URL_FILE *file,
							 CopyFromState pstate);
extern size_t url_curl_fwrite(void *ptr, size_t size, URL_FILE *file);
extern void url_curl_fflush(URL_FILE *file);

extern URL_FILE *url_custom_fopen(char *url, bool forwrite, extvar_t *ev,
								  ExternalSelectDesc desc, Relation rel);
extern void url_custom_fclose(URL_FILE *file, bool failOnError, const char *relname);
extern bool url_custom_feof(URL_FILE *file, int bytesread);
extern bool url_custom_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen);
extern size_t url_custom_fread(void *ptr, size_t size, URL_FILE *file,
							   CopyFromState pstate);
extern size_t url_custom_fwrite(void *ptr, size_t size, URL_FILE *file);

extern URL_FILE *url_source_fopen(const char *name, ExtSourceRead read,
								  void *arg);

/* a user's protocol: its functions, as CREATE PROTOCOL recorded them */
extern Oid	LookupExtProtocolFunction(const char *prot_name, bool iswritable,
									  bool error);
extern Oid	get_extprotocol_oid(const char *prot_name, bool missing_ok);

/* option.c */
extern void ExtRegisterLabelProvider(void);

/* protocol.c */
extern Oid	ExtProtocolOid(const char *name, bool missing_ok);
extern Oid	ExtProtocolFunction(const char *name, bool iswritable, bool *exists);
extern void ExtProtocolCheckUse(const char *name, bool iswritable);

/* A protocol's label on its function, which a dump carries; see protocol.c. */
extern void ExtProtocolRegisterLabelProvider(void);

/* ------------------------------------------------------------------------- */
/* Scans and writes (extaccess.c; gp_exttable_fdw's)                          */
/* ------------------------------------------------------------------------- */

typedef struct ExternalInsertDescData
{
	Relation	ext_rel;
	URL_FILE   *ext_file;
	char	   *ext_uri;
	bool		ext_noop;
	TupleDesc	ext_tupDesc;
	FmgrInfo   *ext_custom_formatter_func;
	List	   *ext_custom_formatter_params;
	FormatterData *ext_formatter_data;
	ExtCopyOut *ext_out;
	MemoryContext ext_rowcontext;
} ExternalInsertDescData;

typedef ExternalInsertDescData *ExternalInsertDesc;

typedef struct FileScanDescData
{
	Relation	fs_rd;
	struct URL_FILE *fs_file;
	char	   *fs_uri;
	bool		fs_noop;
	uint32		fs_scancounter;

	CopyFromState fs_pstate;
	CdbSreh    *fs_sreh;		/* NULL when every error is the query's */
	MemoryContext fs_rowcontext;	/* the row returned, reset for the next */
	AttrNumber	num_phys_attrs;
	Datum	   *values;
	bool	   *nulls;
	FmgrInfo   *in_functions;
	Oid		   *typioparams;
	Oid			in_func_oid;

	TupleDesc	fs_tupDesc;
	HeapTupleData fs_ctup;

	FmgrInfo   *fs_custom_formatter_func;
	List	   *fs_custom_formatter_params;
	FormatterData *fs_formatter;

	char		fs_fmttype;
	bool		fs_csv;
	bool		fs_escape_off;	/* text's ESCAPE 'OFF': backslashes doubled */
	char		fs_escape_char; /* text's escape of its own: made the backslash,
								 * backslashes doubled; or 0 */
	bool		fs_delim_off;	/* text's DELIMITER 'OFF': tabs escaped */
	char	   *fs_escape;
	char	   *fs_quote;
	bool		fs_header;
	EolType		fs_eol_type;
	List	   *fs_options;
	int			fs_encoding;	/* -1: COPY's own, or the options' */

	/* the caller's read function, for a scan of data it reads itself */
	ExtSourceRead fs_source_read;
	void	   *fs_source_arg;
} FileScanDescData;

extern FileScanDesc external_beginscan(Relation relation, uint32 scancounter,
									   List *uriList, char fmtType,
									   bool isMasterOnly, int rejLimit,
									   bool rejLimitInRows, char logErrors,
									   int encoding, List *extOptions);
extern void external_rescan(FileScanDesc scan);
extern void external_endscan(FileScanDesc scan);
extern void external_stopscan(FileScanDesc scan);
extern ExternalSelectDesc external_getnext_init(PlanState *state);
extern HeapTuple external_getnext(FileScanDesc scan, ScanDirection direction,
								  ExternalSelectDesc desc);
extern ExternalInsertDesc external_insert_init(Relation rel);
extern void external_insert(ExternalInsertDesc extInsertDesc,
							TupleTableSlot *slot);
extern void external_insert_finish(ExternalInsertDesc extInsertDesc);
extern List *appendCopyEncodingOption(List *copyFmtOpts, int encoding);

/* ------------------------------------------------------------------------- */
/* DDL (exttable_ddl.c; Cloudberry's commands/exttablecmds.c)                 */
/* ------------------------------------------------------------------------- */

extern void ExtTableTransformCreate(CreateForeignTableStmt *stmt,
									const char *queryString);
extern bool ExtTableIsCreate(CreateForeignTableStmt *stmt);
extern void ExtTableExpandLike(CreateStmt *stmt, const char *queryString);
extern void ExtTableDropped(Oid relid);

/* option.c */
extern bool is_valid_locationuris(List *location_list, bool is_writable);

#endif							/* GP_EXTTABLE_H */
