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
 * gp_extmark.c
 *	  The extension marks: the entries of a database directory that the port's
 *	  modules keep there, which are no relation's pages.
 *
 * PAX keeps a table's files in <relfilenode>_pax and gp_sql a directory
 * table's in <relid>_dirtable, in a database's directory.  pg_checksums would
 * read their files as relation pages, and stop at them or, with --enable,
 * write a checksum into them; so each module lists its suffix, while the
 * postmaster loads it, in GP_EXTENSION_MARKS_FILE in the data directory's
 * root, which O23 has pg_checksums read and pass over what it names.  And a
 * database copied or moved takes the marked directories with it
 * (gp_dbcopy.c), which PostgreSQL's copy of a database's directory does not.
 *
 * The file's format is O23's: one mark a line, a mark being an underscore,
 * then at most 31 lower-case letters, digits and underscores, and not a
 * fork's name, so that no mark can ever name a relation's own files; a line
 * that is none marks nothing.  Vanilla PostgreSQL never writes the file.
 *
 * pg_upgrade does not carry the marked directories: that matters only for an
 * upgrade between two versions of the port, which cannot happen before a
 * PostgreSQL 20 one, when the port's tooling can copy or link them -- the
 * database OIDs and relfilenumbers pg_upgrade keeps map old paths to new one
 * to one.
 *
 * Cloudberry sources this file stands in for: none -- Cloudberry's
 * pg_checksums and pg_upgrade know nothing of PAX, nor its CREATE DATABASE
 * of a directory table's files.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "common/relpath.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"

#include "gp_extmark.h"

/* The longest mark, its underscore included */
#define GP_EXTMARK_MAX_LEN	32

/* Is mark[0..len) a mark a module may make? */
static bool
mark_is_valid(const char *mark, size_t len)
{
	if (len < 2 || len > GP_EXTMARK_MAX_LEN || mark[0] != '_')
		return false;
	for (size_t i = 1; i < len; i++)
	{
		char		c = mark[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	for (int fork = 0; fork <= MAX_FORKNUM; fork++)
	{
		if (strlen(forkNames[fork]) == len - 1 &&
			strncmp(mark + 1, forkNames[fork], len - 1) == 0)
			return false;
	}
	return true;
}

GpExtensionMarks *
GpExtensionMarksLoad(const char *datadir)
{
	char		path[MAXPGPATH];
	GpExtensionMarks *marks;
	StringInfoData buf;
	char		chunk[1024];
	char	   *line;
	int			fd;
	ssize_t		r;
	int			maxmarks = 8;

	snprintf(path, sizeof(path), "%s/%s", datadir, GP_EXTENSION_MARKS_FILE);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return NULL;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m", path)));
	}
	initStringInfo(&buf);
	while ((r = read(fd, chunk, sizeof(chunk))) > 0)
		appendBinaryStringInfo(&buf, chunk, r);
	if (r < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	marks = palloc(sizeof(GpExtensionMarks));
	marks->nmarks = 0;
	marks->suffixes = palloc(maxmarks * sizeof(char *));
	for (line = buf.data; *line != '\0';)
	{
		char	   *end = strchr(line, '\n');
		size_t		len = end ? (size_t) (end - line) : strlen(line);

		if (mark_is_valid(line, len))
		{
			if (marks->nmarks == maxmarks)
			{
				maxmarks *= 2;
				marks->suffixes = repalloc(marks->suffixes,
										   maxmarks * sizeof(char *));
			}
			marks->suffixes[marks->nmarks++] = pnstrdup(line, len);
		}
		if (end == NULL)
			break;
		line = end + 1;
	}
	pfree(buf.data);
	return marks;
}

bool
GpExtensionMarkedFileLookup(const GpExtensionMarks *marks, const char *name)
{
	const char *suffix = name;

	if (marks == NULL)
		return false;
	while (*suffix >= '0' && *suffix <= '9')
		suffix++;
	if (suffix == name)
		return false;
	for (int i = 0; i < marks->nmarks; i++)
	{
		if (strcmp(suffix, marks->suffixes[i]) == 0)
			return true;
	}
	return false;
}

void
GpExtensionMarkAdd(const char *suffix)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	GpExtensionMarks *marks;
	int			fd;
	int			nmarks;

	if (!mark_is_valid(suffix, strlen(suffix)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension mark \"%s\"", suffix)));

	marks = GpExtensionMarksLoad(DataDir);
	nmarks = marks ? marks->nmarks : 0;
	for (int i = 0; i < nmarks; i++)
	{
		if (strcmp(marks->suffixes[i], suffix) == 0)
			return;
	}

	/* Write the marks there were and this one, and put them in place. */
	snprintf(path, sizeof(path), "%s/%s", DataDir, GP_EXTENSION_MARKS_FILE);
	snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
	fd = OpenTransientFile(tmppath, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
	for (int i = 0; i <= nmarks; i++)
	{
		const char *mark = (i < nmarks) ? marks->suffixes[i] : suffix;
		size_t		len = strlen(mark);

		errno = 0;
		if (write(fd, mark, len) != (ssize_t) len || write(fd, "\n", 1) != 1)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", tmppath)));
		}
	}
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));
	(void) durable_rename(tmppath, path, ERROR);
}
