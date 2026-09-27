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
 * url.c
 *	  Opening an external table's location, whatever its protocol: a file,
 *	  a command, an HTTP or gpfdist server, or a protocol of the user's.
 *
 * What a command and a gpfdist server are told about the statement --
 * GP_XID, GP_CID and GP_SN name a scan, which every segment reading the same
 * gpfdist file names the same, so that it hands each a share of the file --
 * the coordinator sets once per statement that reads an external table, in
 * gp_exttable.statement_id, which gp_core sends the segments with the
 * statement (gp_exttable.c).
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/access/external/url.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_exttable.h"

int			readable_external_table_timeout = 0;
int			gpfdist_retry_timeout = 300;

/* the statement's name, for GP_XID: see the file's comment */
char	   *gp_exttable_statement_id = NULL;

/*
 * Its text, for GP_QUERY_STRING: a segment runs what the coordinator made
 * of the statement -- a gather's cursor, a plan's fragment -- whose text is
 * not the user's, so the coordinator sends that too (gp_exttable.c).
 */
char	   *gp_exttable_query_string = NULL;

static void base16_encode(char *raw, int len, char *encoded);
static char *get_eol_delimiter(List *params);

void
external_set_env_vars(extvar_t *extvar, char *uri, bool csv, char *escape,
					  char *quote, bool header, uint32 scancounter)
{
	external_set_env_vars_ext(extvar, uri, csv, escape, quote, EOL_UNKNOWN,
							  header, scancounter, NULL);
}

void
external_set_env_vars_ext(extvar_t *extvar, char *uri, bool csv, char *escape,
						  char *quote, EolType eol_type, bool header,
						  uint32 scancounter, List *params)
{
	time_t		now = time(0);
	struct tm  *tm = localtime(&now);
	const GpSegmentConfig *coord = GpClusterIsSingleNode() ? NULL : GpClusterCoordinator();
	char	   *encoded_delim;
	int			line_delim_len;
	int			eol_code;

	switch (eol_type)
	{
		case EOL_NL:
			eol_code = 1;
			break;
		case EOL_CR:
			eol_code = 2;
			break;
		case EOL_CRNL:
			eol_code = 3;
			break;
		default:
			eol_code = 0;
			break;
	}

	snprintf(extvar->GP_CSVOPT, sizeof(extvar->GP_CSVOPT),
			 "m%1dx%3dq%3dn%1dh%1d",
			 csv ? 1 : 0,
			 escape ? 255 & *escape : 0,
			 quote ? 255 & *quote : 0,
			 eol_code,
			 header ? 1 : 0);

	if (coord != NULL)
	{
		extvar->GP_MASTER_PORT = psprintf("%d", coord->port);
		extvar->GP_MASTER_HOST = pstrdup(coord->hostname[0] == '/' ?
										 "localhost" : coord->hostname);
	}
	else
	{
		extvar->GP_MASTER_PORT = psprintf("%d", PostPortNumber);
		extvar->GP_MASTER_HOST = pstrdup("localhost");
	}

	if (MyProcPort)
		extvar->GP_USER = MyProcPort->user_name;
	else
		extvar->GP_USER = "";

	extvar->GP_DATABASE = get_database_name(MyDatabaseId);
	extvar->GP_SEG_PG_CONF = ConfigFileName;
	extvar->GP_SEG_DATADIR = DataDir;
	snprintf(extvar->GP_DATE, sizeof(extvar->GP_DATE), "%04d%02d%02d",
			 1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday);
	snprintf(extvar->GP_TIME, sizeof(extvar->GP_TIME), "%02d%02d%02d",
			 tm->tm_hour, tm->tm_min, tm->tm_sec);

	if (gp_exttable_statement_id != NULL && gp_exttable_statement_id[0] != '\0')
		strlcpy(extvar->GP_XID, gp_exttable_statement_id, sizeof(extvar->GP_XID));
	else
		snprintf(extvar->GP_XID, sizeof(extvar->GP_XID), "%d-%d",
				 GpClusterSessionId(), MyProcPid);

	snprintf(extvar->GP_CID, sizeof(extvar->GP_CID), "%x", 0);
	snprintf(extvar->GP_SN, sizeof(extvar->GP_SN), "%x", scancounter);
	snprintf(extvar->GP_SEGMENT_ID, sizeof(extvar->GP_SEGMENT_ID), "%d",
			 GpClusterContentId());
	snprintf(extvar->GP_SEG_PORT, sizeof(extvar->GP_SEG_PORT), "%d", PostPortNumber);
	snprintf(extvar->GP_SESSION_ID, sizeof(extvar->GP_SESSION_ID), "%d",
			 GpClusterSessionId());
	snprintf(extvar->GP_SEGMENT_COUNT, sizeof(extvar->GP_SEGMENT_COUNT), "%d",
			 GpClusterIsSingleNode() ? 1 : GpClusterSegmentCount());

	extvar->GP_QUERY_STRING = (gp_exttable_query_string != NULL &&
								gp_exttable_query_string[0] != '\0') ?
		gp_exttable_query_string : (char *) debug_query_string;

	if (params != NIL)
	{
		char	   *line_delim_str = get_eol_delimiter(params);

		line_delim_len = (int) strlen(line_delim_str);
		if (line_delim_len > 0)
		{
			encoded_delim = (char *) palloc(line_delim_len * 2 + 1);
			base16_encode(line_delim_str, line_delim_len, encoded_delim);
		}
		else
		{
			line_delim_len = -1;
			encoded_delim = "";
		}
	}
	else
	{
		switch (eol_type)
		{
			case EOL_CR:
				encoded_delim = "0D";
				line_delim_len = 1;
				break;
			case EOL_NL:
				encoded_delim = "0A";
				line_delim_len = 1;
				break;
			case EOL_CRNL:
				encoded_delim = "0D0A";
				line_delim_len = 2;
				break;
			default:
				encoded_delim = "";
				line_delim_len = -1;
				break;
		}
	}
	extvar->GP_LINE_DELIM_STR = pstrdup(encoded_delim);
	snprintf(extvar->GP_LINE_DELIM_LENGTH, sizeof(extvar->GP_LINE_DELIM_LENGTH),
			 "%d", line_delim_len);
}

static void
base16_encode(char *raw, int len, char *encoded)
{
	const char *raw_bytes = raw;
	char	   *encoded_bytes = encoded;
	int			remaining = len;

	for (; remaining--; encoded_bytes += 2)
		sprintf(encoded_bytes, "%02x", *(raw_bytes++));
}

static char *
get_eol_delimiter(List *params)
{
	ListCell   *lc;

	foreach(lc, params)
		if (pg_strcasecmp(((DefElem *) lfirst(lc))->defname, "line_delim") == 0)
			return pstrdup(defGetString((DefElem *) lfirst(lc)));

	return pstrdup("");
}

URL_FILE *
url_fopen(char *url, bool forwrite, extvar_t *ev, CopyFormatOptions *opts,
		  ExternalSelectDesc desc, Relation rel)
{
	if (pg_strncasecmp(url, EXEC_URL_PREFIX, strlen(EXEC_URL_PREFIX)) == 0)
		return url_execute_fopen(url, forwrite, ev);
	else if (IS_FILE_URI(url))
		return url_file_fopen(url, forwrite, ev, opts,
							  RelationGetRelationName(rel));
	else if (IS_HTTP_URI(url) || IS_GPFDIST_URI(url) || IS_GPFDISTS_URI(url))
		return url_curl_fopen(url, forwrite, ev, opts);
	else
		return url_custom_fopen(url, forwrite, ev, desc, rel);
}

/*
 * A location that is a caller's read function, named for the error log: the
 * data of a scan begun by ExtScanSourceBegin() (extaccess.c), which pxf_fdw
 * reads from its server itself.
 */
typedef struct URL_SOURCE_FILE
{
	URL_FILE	common;
	ExtSourceRead read;
	void	   *arg;
} URL_SOURCE_FILE;

URL_FILE *
url_source_fopen(const char *name, ExtSourceRead read, void *arg)
{
	URL_SOURCE_FILE *file = palloc0(sizeof(URL_SOURCE_FILE));

	file->common.type = CFTYPE_SOURCE;
	file->common.url = pstrdup(name);
	file->read = read;
	file->arg = arg;
	return (URL_FILE *) file;
}

void
url_fclose(URL_FILE *file, bool failOnError, const char *relname)
{
	if (file == NULL)
	{
		elog(WARNING, "internal error: call url_fclose with bad parameter");
		return;
	}

	switch (file->type)
	{
		case CFTYPE_FILE:
			url_file_fclose(file, failOnError, relname);
			break;
		case CFTYPE_EXEC:
			url_execute_fclose(file, failOnError, relname);
			break;
		case CFTYPE_CURL:
			url_curl_fclose(file, failOnError, relname);
			break;
		case CFTYPE_CUSTOM:
			url_custom_fclose(file, failOnError, relname);
			break;
		case CFTYPE_SOURCE:
			pfree(file->url);
			pfree(file);
			break;
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
			break;
	}
}

bool
url_feof(URL_FILE *file, int bytesread)
{
	switch (file->type)
	{
		case CFTYPE_FILE:
			return url_file_feof(file, bytesread);
		case CFTYPE_EXEC:
			return url_execute_feof(file, bytesread);
		case CFTYPE_CURL:
			return url_curl_feof(file, bytesread);
		case CFTYPE_CUSTOM:
			return url_custom_feof(file, bytesread);
		case CFTYPE_SOURCE:
			return bytesread == 0;
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
	}
	return true;
}

bool
url_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen)
{
	switch (file->type)
	{
		case CFTYPE_FILE:
			return url_file_ferror(file, bytesread, ebuf, ebuflen);
		case CFTYPE_EXEC:
			return url_execute_ferror(file, bytesread, ebuf, ebuflen);
		case CFTYPE_CURL:
			return url_curl_ferror(file, bytesread, ebuf, ebuflen);
		case CFTYPE_CUSTOM:
			return url_custom_ferror(file, bytesread, ebuf, ebuflen);
		case CFTYPE_SOURCE:
			return bytesread == -1;
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
	}
	return true;
}

size_t
url_fread(void *ptr, size_t size, URL_FILE *file, CopyFromState pstate)
{
	switch (file->type)
	{
		case CFTYPE_FILE:
			return url_file_fread(ptr, size, file, pstate);
		case CFTYPE_EXEC:
			return url_execute_fread(ptr, size, file, pstate);
		case CFTYPE_CURL:
			return url_curl_fread(ptr, size, file, pstate);
		case CFTYPE_CUSTOM:
			return url_custom_fread(ptr, size, file, pstate);
		case CFTYPE_SOURCE:
			return (size_t) ((URL_SOURCE_FILE *) file)->read(((URL_SOURCE_FILE *) file)->arg,
															ptr, (int) size);
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
	}
	return 0;
}

size_t
url_fwrite(void *ptr, size_t size, URL_FILE *file)
{
	switch (file->type)
	{
		case CFTYPE_FILE:
			elog(ERROR, "CFTYPE_FILE not yet supported in url.c");
			return 0;
		case CFTYPE_EXEC:
			return url_execute_fwrite(ptr, size, file);
		case CFTYPE_CURL:
			return url_curl_fwrite(ptr, size, file);
		case CFTYPE_CUSTOM:
			return url_custom_fwrite(ptr, size, file);
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
	}
	return 0;
}

void
url_fflush(URL_FILE *file)
{
	switch (file->type)
	{
		case CFTYPE_FILE:
			elog(ERROR, "CFTYPE_FILE not yet supported in url.c");
			break;
		case CFTYPE_EXEC:
		case CFTYPE_CUSTOM:
			break;
		case CFTYPE_CURL:
			url_curl_fflush(file);
			break;
		default:
			elog(ERROR, "unrecognized external table type: %d", file->type);
	}
}
