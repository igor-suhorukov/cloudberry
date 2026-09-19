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
 * gp_sql.h
 *	  What the parts of the Cloudberry-only SQL surface share.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SQL_H
#define GP_SQL_H

#include "postgres.h"

#include "nodes/parsenodes.h"

struct QueryDesc;

/* The extension's own schema, and the label provider tags are kept in. */
#define GP_SQL_SCHEMA		"gp_sql"
#define GP_TAG_PROVIDER		"gp_tag"

/*
 * The namespace of the shorthand:
 *
 *	  CREATE TABLE t (...) WITH (gp_tag.env = 'prod')
 *
 * which is what Cloudberry writes as TAG (env = 'prod').
 */
#define GP_TAG_OPTION_NS	"gp_tag"

/* tag.c */

/*
 * Take WITH (gp_tag.<name> = '<value>') out of an option list, so that what
 * is left is something PostgreSQL will accept.  Returns the pairs it removed
 * as a list of DefElem with the namespace stripped, or NIL.
 */
extern List *GpTagTakeOptions(List **options);

/*
 * The same for RESET (gp_tag.<name>), which takes a tag away.  What it
 * returns are DefElem with no argument, and that is what "remove" means
 * everywhere below.
 */
extern List *GpTagTakeResetOptions(List **options);

/*
 * Refuse any tag in the list that is not defined, or whose value the
 * definition does not allow.  Called before the statement carrying them runs.
 */
extern void GpTagCheckAll(List *tags);

/*
 * Give an object the tags a stripped option list named.  `classId` is
 * RelationRelationId for everything the shorthand reaches.  Indexes cannot
 * carry a security label, so theirs go to gp_sql.index_tag instead.
 */
extern void GpTagApplyToRelation(Oid relId, List *tags);

/* An index is being dropped: forget the tags kept for it. */
extern void GpTagIndexDropped(Oid indexRelId);

/* Registered during preload. */
extern void GpTagRegisterProvider(void);

/* dirtable.c */

/* Cloudberry's allow_dml_directory_table. */
extern PGDLLIMPORT bool gp_allow_dml_directory_table;

/* Where a relation keeps its files, or NULL if it is not a directory table. */
extern char *GpDirTableLocation(Oid relid);

/* Refuse the DML on a directory table that Cloudberry refuses in ExecMain. */
extern void GpDirTableCheckDML(struct QueryDesc *queryDesc);

/* TRUNCATE would orphan every file a directory table has. */
extern void GpDirTableCheckTruncate(TruncateStmt *stmt);

/* A directory table is being dropped: its files follow it at commit. */
extern void GpDirTableDropped(Oid relid);

/* Registered during preload; drains the files a transaction leaves behind. */
extern void GpDirTableRegisterXactCallback(void);

/*
 * Refuse a tag name or value the definitions in gp_sql.tag do not allow.
 * Shared by the label check hook and the shorthand, so that both answer the
 * same.  Called with the extension's tables reachable.
 */
extern void GpTagValidate(const char *tagname, const char *tagvalue);

#endif							/* GP_SQL_H */
