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
 * gp_extmark.h
 *	  The extension marks: the entries of a database directory that the port's
 *	  modules keep there, which are no relation's pages.  See gp_extmark.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_EXTMARK_H
#define GP_EXTMARK_H

/*
 * The list of marks, one suffix a line, in the data directory's root: the
 * name pg_checksums reads (O23), and the format its reader accepts.
 */
#define GP_EXTENSION_MARKS_FILE	"extension_marks"

typedef struct GpExtensionMarks
{
	int			nmarks;
	char	  **suffixes;		/* each one a valid mark */
} GpExtensionMarks;

/*
 * Mark the entries named by a number and "suffix" in every database directory
 * as a module's own.  Called while the postmaster loads the module, through
 * gp_core's API (extension_mark_add), so that every node it runs on has the
 * mark, a standby's too.
 */
extern void GpExtensionMarkAdd(const char *suffix);

/* The marks of the data directory "datadir"; NULL when it has none. */
extern GpExtensionMarks *GpExtensionMarksLoad(const char *datadir);

/* Is "name", an entry of a database directory, a number and a marked suffix? */
extern bool GpExtensionMarkedFileLookup(const GpExtensionMarks *marks,
										const char *name);

#endif							/* GP_EXTMARK_H */
