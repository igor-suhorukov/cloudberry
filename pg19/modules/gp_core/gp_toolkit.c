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
 * gp_toolkit.c
 *	  gp_toolkit's functions of a node's disk and files: the space free on
 *	  it, the directory a tablespace keeps this version's files in, and the
 *	  rename that moves an orphaned file away.
 *
 * Cloudberry's gp_disk_free is an external web table that runs df on each
 * segment through gppylib (DiskFree.get_disk_free_info_local()).  The
 * port's is a view of a function each segment answers itself, statvfs() of
 * its data directory, as "df -Pk" reports it: the device of the file system
 * the directory is on -- its entry in /proc/self/mounts, the one of the
 * longest mount point above the directory -- and the space a user who is
 * not root may take, in 1 kB blocks; and the host's name up to its first
 * dot, as gppylib's getLocalHostname() gives it.
 *
 * The checks for orphaned and missing files (gp_core--1.0.sql) list each
 * tablespace's directory of the database, pg_tblspc/<oid>/<version
 * directory>/<database>, whose middle part Cloudberry's
 * get_tablespace_version_directory_name() names; and gp_move_orphaned_files()
 * moves each file it finds with pg_file_rename(), adminpack's, which
 * Cloudberry has built in and PostgreSQL 19 has not.
 *
 * Cloudberry sources this file stands in for:
 *	  gpcontrib/gp_toolkit/gp_toolkit--1.3.sql (gp_disk_free),
 *	  gpMgmt/bin/gppylib/commands/unix.py (DiskFree, getLocalHostname()),
 *	  src/backend/utils/adt/misc.c (get_tablespace_version_directory_name())
 *	  and src/backend/utils/adt/genfile.c (pg_file_rename_v1_1())
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mntent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "catalog/pg_authid.h"
#include "common/relpath.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"

PG_FUNCTION_INFO_V1(gp_disk_free_rows);
PG_FUNCTION_INFO_V1(gp_tablespace_version_directory_name);
PG_FUNCTION_INFO_V1(gp_file_rename);

/* ------------------------------------------------------------------------- */
/* gp_disk_free                                                              */
/* ------------------------------------------------------------------------- */

/*
 * The device of the file system the data directory is on, as df names it:
 * of the mounts on the directory's device, the one whose mount point is the
 * longest above it.  NULL where /proc has no mounts to say.
 */
static char *
data_directory_device(void)
{
	char		dir[MAXPGPATH];
	struct stat st;
	struct mntent *m;
	FILE	   *mounts;
	char	   *device = NULL;
	size_t		longest = 0;

	if (realpath(DataDir, dir) == NULL || stat(dir, &st) != 0 ||
		(mounts = setmntent("/proc/self/mounts", "r")) == NULL)
		return NULL;
	while ((m = getmntent(mounts)) != NULL)
	{
		size_t		len = strlen(m->mnt_dir);
		struct stat mst;

		if (strncmp(dir, m->mnt_dir, len) != 0 ||
			(len > 1 && dir[len] != '/' && dir[len] != '\0') ||
			len < longest || stat(m->mnt_dir, &mst) != 0 ||
			mst.st_dev != st.st_dev)
			continue;
		device = pstrdup(m->mnt_fsname);
		longest = len;
	}
	endmntent(mounts);
	return device;
}

/*
 * gp_toolkit.__gp_disk_free_rows(): the space free for the data directory,
 * each segment's own, as Cloudberry's gp_disk_free has it -- the segment's
 * content id, its host, the device, and the kB free.
 */
Datum
gp_disk_free_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	struct statvfs fs;
	char		host[256];
	char	   *device;
	char	   *dot;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (GpDispatchFunctionToSegments(fcinfo))
		return (Datum) 0;

	InitMaterializedSRF(fcinfo, 0);
	if (statvfs(DataDir, &fs) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file system of directory \"%s\": %m",
						DataDir)));
	if (gethostname(host, sizeof(host)) != 0)
		strlcpy(host, "localhost", sizeof(host));
	host[sizeof(host) - 1] = '\0';
	if ((dot = strchr(host, '.')) != NULL)
		*dot = '\0';
	device = data_directory_device();

	values[0] = Int32GetDatum(GpClusterContentId());
	values[1] = CStringGetTextDatum(host);
	if (device != NULL)
		values[2] = CStringGetTextDatum(device);
	else
		nulls[2] = true;
	values[3] = Int64GetDatum((int64) ((uint64) fs.f_bavail * fs.f_frsize / 1024));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* Tablespaces and files                                                     */
/* ------------------------------------------------------------------------- */

/*
 * pg_catalog.get_tablespace_version_directory_name(): the directory of a
 * tablespace this server's version keeps its files in.
 */
Datum
gp_tablespace_version_directory_name(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(TABLESPACE_VERSION_DIRECTORY));
}

/*
 * A path pg_file_rename() is given, as adminpack checks it: any, for a role
 * that may write the server's files; else one in the data directory or the
 * log directory, or relative and below the data directory.
 */
static char *
checked_path(text *arg)
{
	char	   *path = text_to_cstring(arg);

	canonicalize_path(path);
	if (has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
		return path;

	if (is_absolute_path(path))
	{
		if (path_contains_parent_reference(path))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("reference to parent directory (\"..\") not allowed")));
		if (!path_is_prefix_of_path(DataDir, path) &&
			(!is_absolute_path(Log_directory) ||
			 !path_is_prefix_of_path(Log_directory, path)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("absolute path not allowed")));
	}
	else if (!path_is_relative_and_below_cwd(path))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("path must be in or below the current directory")));
	return path;
}

/*
 * pg_catalog.pg_file_rename(oldname, newname, archivename): rename a file,
 * and where archivename is given, newname to it first -- as adminpack's
 * version 1.1, which Cloudberry has built in: false where a file may not
 * be written, and never over a file that is there.  Privileges are
 * EXECUTE's, which the superuser alone has.
 */
Datum
gp_file_rename(PG_FUNCTION_ARGS)
{
	char	   *fn1;
	char	   *fn2;
	char	   *fn3 = NULL;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	fn1 = checked_path(PG_GETARG_TEXT_PP(0));
	fn2 = checked_path(PG_GETARG_TEXT_PP(1));
	if (!PG_ARGISNULL(2))
		fn3 = checked_path(PG_GETARG_TEXT_PP(2));

	if (access(fn1, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn1)));
		PG_RETURN_BOOL(false);
	}
	if (fn3 != NULL && access(fn2, W_OK) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("file \"%s\" is not accessible: %m", fn2)));
		PG_RETURN_BOOL(false);
	}
	if (access(fn3 != NULL ? fn3 : fn2, W_OK) >= 0 || errno != ENOENT)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FILE),
				 errmsg("cannot rename to target file \"%s\"",
						fn3 != NULL ? fn3 : fn2)));

	if (fn3 != NULL)
	{
		if (rename(fn2, fn3) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename \"%s\" to \"%s\": %m", fn2, fn3)));
		if (rename(fn1, fn2) != 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not rename \"%s\" to \"%s\": %m", fn1, fn2)));
			if (rename(fn3, fn2) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not rename \"%s\" back to \"%s\": %m",
								fn3, fn2)));
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FILE),
					 errmsg("renaming \"%s\" to \"%s\" was reverted", fn2, fn3)));
		}
	}
	else if (rename(fn1, fn2) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename \"%s\" to \"%s\": %m", fn1, fn2)));
	PG_RETURN_BOOL(true);
}
