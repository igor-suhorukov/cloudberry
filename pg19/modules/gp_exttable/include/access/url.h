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
 * url.h
 *	  An external table's locations, as gp_exttable reads and writes them,
 *	  and the variables a location is given: what the statement, the node
 *	  and the session are.
 *
 * Cloudberry's own header, src/include/access/url.h, was its server's.
 * Here it is gp_exttable's, and a module of another library gets from it
 * the variables alone -- pxf_fdw sends its server the user, the segment
 * and the statement's name as headers of its requests -- through
 * external_set_env_vars(), found with load_external_function() as
 * external.h says.
 *
 *-------------------------------------------------------------------------
 */
#ifndef URL_H
#define URL_H

#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "fmgr.h"

#include "access/extprotocol.h"

/* the global transaction id Cloudberry puts in GP_XID: TMGIDSIZE */
#define EXT_XID_SIZE	64

typedef struct extvar_t
{
	char	   *GP_MASTER_HOST;
	char	   *GP_MASTER_PORT;
	char	   *GP_DATABASE;
	char	   *GP_USER;
	char	   *GP_SEG_PG_CONF;
	char	   *GP_SEG_DATADIR;
	char		GP_DATE[9];		/* YYYYMMDD */
	char		GP_TIME[7];		/* HHMMSS */
	char		GP_XID[EXT_XID_SIZE];
	char		GP_CID[10];
	char		GP_SN[10];
	char		GP_SEGMENT_ID[11];
	char		GP_SEG_PORT[11];
	char		GP_SESSION_ID[11];
	char		GP_SEGMENT_COUNT[11];
	char		GP_CSVOPT[15];
	char	   *GP_LINE_DELIM_STR;
	char		GP_LINE_DELIM_LENGTH[11];
	char	   *GP_QUERY_STRING;
} extvar_t;

#ifdef GP_EXTTABLE_INTERNAL

enum fcurl_type_e
{
	CFTYPE_NONE = 0,
	CFTYPE_FILE = 1,
	CFTYPE_CURL = 2,
	CFTYPE_EXEC = 3,
	CFTYPE_CUSTOM = 4,
	CFTYPE_SOURCE = 5			/* a caller's read function: external.h */
};

typedef struct URL_FILE
{
	enum fcurl_type_e type;
	char	   *url;
	char		current[MAXPGPATH];	/* "url [file]" read now, for the error log */
	/* implementation-specific fields follow */
} URL_FILE;

#define EXEC_URL_PREFIX "execute:"

extern int	readable_external_table_timeout;
extern int	gpfdist_retry_timeout;

extern PGDLLEXPORT void external_set_env_vars(extvar_t *extvar, char *uri,
											  bool csv, char *escape,
											  char *quote, bool header,
											  uint32 scancounter);
extern void external_set_env_vars_ext(extvar_t *extvar, char *uri, bool csv,
									  char *escape, char *quote,
									  EolType eol_type, bool header,
									  uint32 scancounter, List *params);
extern URL_FILE *url_fopen(char *url, bool forwrite, extvar_t *ev,
						   CopyFormatOptions *opts, ExternalSelectDesc desc,
						   Relation rel);
extern void url_fclose(URL_FILE *file, bool failOnError, const char *relname);
extern bool url_feof(URL_FILE *file, int bytesread);
extern bool url_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen);
extern size_t url_fread(void *ptr, size_t size, URL_FILE *file,
						CopyFromState pstate);
extern size_t url_fwrite(void *ptr, size_t size, URL_FILE *file);
extern void url_fflush(URL_FILE *file);
extern char *make_command(const char *cmd, extvar_t *ev);

#else							/* a module of another library */

/*
 * What a location is given, as gp_exttable gives it: the user, the node, the
 * statement's name (GP_XID), the session, how many segments there are.
 */
static inline void
external_set_env_vars(extvar_t *extvar, char *uri, bool csv, char *escape,
					  char *quote, bool header, uint32 scancounter)
{
	static void (*fn) (extvar_t *, char *, bool, char *, char *, bool,
					   uint32) = NULL;

	if (fn == NULL)
		fn = (__typeof__(fn))
			load_external_function("$libdir/gp_exttable",
								   "external_set_env_vars", true, NULL);
	fn(extvar, uri, csv, escape, quote, header, scancounter);
}

#endif							/* GP_EXTTABLE_INTERNAL */

#endif							/* URL_H */
