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
 * url_file.c
 *	  A file:// location: a file, a directory or a pattern of files on the
 *	  segment's host, compressed or not, read through fstream, as gpfdist
 *	  reads them.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/access/external/url_file.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fstream/fstream.h>

#include "gp_exttable.h"

typedef struct URL_FSTREAM_FILE
{
	URL_FILE	common;
	fstream_t  *fp;
} URL_FSTREAM_FILE;

URL_FILE *
url_file_fopen(char *url, bool forwrite, extvar_t *ev, CopyFormatOptions *opts,
			   char *relname)
{
	URL_FSTREAM_FILE *file;
	char	   *path = strchr(url + strlen(PROTOCOL_FILE), '/');
	struct fstream_options fo;
	int			response_code;
	const char *response_string;

	if (forwrite)
		elog(ERROR, "cannot change a readable external table \"%s\"", relname);

	memset(&fo, 0, sizeof fo);

	if (!path)
		elog(ERROR, "External Table error opening file: '%s', invalid "
			 "file path", url);

	file = palloc0(sizeof(URL_FSTREAM_FILE));
	file->common.type = CFTYPE_FILE;
	file->common.url = pstrdup(url);
	strlcpy(file->common.current, url, sizeof(file->common.current));

	fo.is_csv = (opts->format == COPY_FORMAT_CSV);
	fo.quote = opts->quote ? *opts->quote : 0;
	fo.escape = opts->escape ? *opts->escape : 0;
	fo.eol_type = 0;
	fo.header = opts->header_line != COPY_HEADER_FALSE;
	fo.bufsize = 32 * 1024;

	/*
	 * fstream skips each file's header itself, where it opens the file: COPY
	 * would skip only the first file's.
	 */
	opts->header_line = COPY_HEADER_FALSE;

	file->fp = fstream_open(path, &fo, &response_code, &response_string);
	if (!file->fp)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %d %s",
						path, response_code, response_string)));

	return (URL_FILE *) file;
}

void
url_file_fclose(URL_FILE *file, bool failOnError, const char *relname)
{
	URL_FSTREAM_FILE *ffile = (URL_FSTREAM_FILE *) file;

	fstream_close(ffile->fp);
	pfree(ffile->common.url);
	pfree(ffile);
}

size_t
url_file_fread(void *ptr, size_t size, URL_FILE *file, CopyFromState pstate)
{
	URL_FSTREAM_FILE *ffile = (URL_FSTREAM_FILE *) file;
	struct fstream_filename_and_offset fo;
	int			n;

	n = fstream_read(ffile->fp, ptr, size, &fo, 0, "", -1);

	if (n > 0 && fo.line_number)
	{
		if (pstate)
			pstate->cur_lineno = fo.line_number;
		snprintf(ffile->common.current, sizeof(ffile->common.current),
				 "%s [%s]", ffile->common.url, fo.fname);
	}
	return n > 0 ? n : 0;
}

bool
url_file_feof(URL_FILE *file, int bytesread)
{
	return fstream_eof(((URL_FSTREAM_FILE *) file)->fp) != 0;
}

bool
url_file_ferror(URL_FILE *file, int bytesread, char *ebuf, int ebuflen)
{
	return fstream_get_error(((URL_FSTREAM_FILE *) file)->fp) != 0;
}
