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
 * gp_storage_probe.c
 *	  A storage handler for the tests: a directory stands for the store.
 *
 * It serves the storage servers whose "protocol" is "probe": a server's
 * "root" option is the directory it keeps files under, and its "secret"
 * option what a user's mapping must give as "secret" to be let in -- so that
 * a file is written only with the credentials gp_sql read, as the user, from
 * the maintenance database.  gp_storage_probe.credentials() says what they
 * are wherever it is called, a segment included.
 *
 * It is a test module.  It is not part of the port, and it is installed only
 * where the tests run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "gp_storage.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_storage_probe",
					.version = "1.0"
);

/* The file under the server's root; its credentials checked first. */
static char *
probe_path(const GpStorageFile *file)
{
	const char *root = GpStorageOption(file->server_options, "root");
	const char *want = GpStorageOption(file->server_options, "secret");
	const char *given = GpStorageOption(file->user_options, "secret");

	if (root == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("storage server \"%s\" has no \"root\" option", file->server)));
	if (want != NULL && (given == NULL || strcmp(want, given) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("storage server \"%s\" refused the credentials of role \"%s\"",
						file->server, GetUserNameFromId(GetUserId(), false)),
				 given == NULL ? errdetail("The role has no user mapping for the server that gives a secret.") : 0));
	return psprintf("%s/%s", root, file->path);
}

static void
probe_write_file(const GpStorageFile *file, const char *data, Size len)
{
	char	   *path = probe_path(file);
	char	   *dir = pstrdup(path);
	int			fd;

	*strrchr(dir, '/') = '\0';
	if (pg_mkdir_p(dir, pg_dir_create_mode) != 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir)));
	fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0 && errno == EEXIST)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
				 errmsg("file \"%s\" already exists on storage server \"%s\"",
						file->path, file->server)));
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));
	if (len > 0 && write(fd, data, len) != (ssize_t) len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	CloseTransientFile(fd);
}

static bytea *
probe_read_file(const GpStorageFile *file)
{
	char	   *path = probe_path(file);
	struct stat st;
	bytea	   *result;
	int			fd;

	if (stat(path, &st) != 0)
		return NULL;
	result = (bytea *) palloc(VARHDRSZ + st.st_size);
	SET_VARSIZE(result, VARHDRSZ + st.st_size);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0 || read(fd, VARDATA(result), st.st_size) != st.st_size)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	CloseTransientFile(fd);
	return result;
}

static bool
probe_remove_file(const GpStorageFile *file)
{
	return unlink(probe_path(file)) == 0;
}

static void
probe_remove_directory(const GpStorageFile *file)
{
	(void) rmtree(probe_path(file), true);
}

static const GpStorageHandler probe_handler = {
	.protocol = "probe",
	.write_file = probe_write_file,
	.read_file = probe_read_file,
	.remove_file = probe_remove_file,
	.remove_directory = probe_remove_directory,
};

PG_FUNCTION_INFO_V1(gp_storage_probe_credentials);

/*
 * gp_storage_probe.credentials(server text) -> text
 *
 * The calling user's mapping for a storage server, as a handler is given
 * it: "name=value" pairs in the mapping's order, or NULL for none.
 */
Datum
gp_storage_probe_credentials(PG_FUNCTION_ARGS)
{
	List	   *options = GpStorageUserOptions(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	StringInfoData buf;

	if (options == NIL)
		PG_RETURN_NULL();
	initStringInfo(&buf);
	foreach_node(DefElem, def, options)
		appendStringInfo(&buf, "%s%s=%s", buf.len > 0 ? "," : "",
						 def->defname, strVal(def->arg));
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

void
_PG_init(void)
{
	GpStorageRegisterHandler(&probe_handler);
}
