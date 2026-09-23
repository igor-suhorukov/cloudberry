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
#include "tcop/utility.h"

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

/* The port's own option namespace, which gp_core owns. */
#define GP_OPTION_NS		"gp"

/* gp_sql.c */

/*
 * Watching what a statement makes: arm before running it, ask afterwards,
 * restore whatever the arming saved -- a statement may run inside another.
 */
typedef struct GpSqlPending
{
	bool		armed;
	List	   *classes;
	List	   *objects;
} GpSqlPending;

extern void GpSqlPendingArm(GpSqlPending *save);
extern void GpSqlPendingRestore(const GpSqlPending *save);

/* The first object of this class the statement made, or InvalidOid. */
extern Oid	GpSqlPendingFirst(Oid classId);

/* Run a statement through the ProcessUtility hooks after gp_sql's. */
extern void GpSqlProcessUtilityNext(PlannedStmt *pstmt, const char *queryString,
									bool readOnlyTree, ProcessUtilityContext context,
									ParamListInfo params, QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

/* funcattr.c */

/*
 * CREATE [OR REPLACE] FUNCTION and ALTER FUNCTION, with what Cloudberry's
 * EXECUTE ON and data-access attributes became: SET gp.execute_on and SET
 * gp.data_access.
 */
extern void GpFuncAttrProcessUtility(PlannedStmt *pstmt, const char *queryString,
									 bool readOnlyTree, ProcessUtilityContext context,
									 ParamListInfo params, QueryEnvironment *queryEnv,
									 DestReceiver *dest, QueryCompletion *qc);

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
 * The same for a statement with no namespaced options -- CREATE and ALTER
 * DATABASE, and a foreign table's OPTIONS -- where the tags are the options
 * named "gp_tag.<name>".  One with no value, = DEFAULT, takes the tag away.
 */
extern List *GpTagTakePrefixedOptions(List **options);

/*
 * The tags a statement's rewrite carried to its parse node, where its grammar
 * had no place for them (gp_desugar.c, GpAttachCarriers): DefElems in the
 * "gp_tag" namespace, among whatever else the list holds.
 */
extern bool GpTagHasCarried(List *list);
extern List *GpTagTakeCarried(List **list);

/*
 * Refuse any tag in the list that is not defined, or whose value the
 * definition does not allow.  Called before the statement carrying them runs.
 */
extern void GpTagCheckAll(List *tags);

/*
 * Give a relation the tags a stripped option list named.  Indexes cannot
 * carry a security label, so theirs go to gp_sql.index_tag instead.
 */
extern void GpTagApplyToRelation(Oid relId, List *tags);

/*
 * Give an object the statement has just made -- a relation, a database or a
 * tablespace -- the tags, as its gp_tag label.  The tags are not checked here.
 */
extern void GpTagApplyToObject(Oid classId, Oid objectId, List *tags);

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

/*
 * Make a table of the right shape a directory table: its directory, and the
 * "gp" label that says where it is.  Returns the location.
 */
extern char *GpDirTableClaim(Oid relid);

/* Registered during preload; drains the files a transaction leaves behind. */
extern void GpDirTableRegisterXactCallback(void);

/* storage.c */

/* Take WITH (gp.server = '...') out of a tablespace's option list. */
extern List *GpStorageTakeTablespaceOptions(List **options);

/* Record it on the tablespace, once the statement has made one. */
extern void GpStorageApplyToTablespace(const char *spcname, List *opts);

/* Which storage server a tablespace reaches, or NULL for a local one. */
extern char *GpStorageTablespaceServer(Oid spcId);

/* partition.c */

/* Cloudberry's gp_max_partition_level: 0, no limit. */
/* distribution.c */
extern PGDLLIMPORT bool gp_create_table_random_default_distribution;
/*
 * A table nobody distributed, distributed as Cloudberry would: "stmt" is the
 * CREATE TABLE that made it as it was before it ran -- PostgreSQL's analysis
 * rewrites its list of elements -- or NULL for one CREATE TABLE AS made,
 * whose query GpDistributionApplyCtasDefault() reads first.  What
 * Cloudberry says about a table's parents is said before the statement runs,
 * by GpDistributionNoteDefault(); "quiet" for a partition made for a classic
 * partition clause.
 */
extern void GpDistributionApplyDefault(CreateStmt *stmt, Oid relid);
extern void GpDistributionApplyCtasDefault(Oid relid, Query *query);
extern void GpDistributionNoteDefault(CreateStmt *stmt, bool quiet);

/*
 * A distribution a statement names, checked as Cloudberry checks it:
 * GpDistributionCheckKey() the key's own columns and operator classes,
 * returning it as the label records it; GpDistributionCheckCreate() the rest
 * of a CREATE TABLE's rules; and GpDistributionCheckIndexes() each unique
 * index and exclusion constraint against the policy -- the table's own, or
 * the one it is about to be given, with for_alter.
 */
extern char *GpDistributionCheckKey(Oid relid, const char *policy, int location,
									const char *queryString, bool alter);
extern void GpDistributionCheckCreate(CreateStmt *stmt, Oid relid,
									  const char *policy);
extern void GpDistributionCheckIndexes(Oid relid, const char *policy,
									   bool for_alter);

/*
 * The statements that may make a unique index or exclusion constraint, and
 * the check of the table's after one ran; and an ALTER TABLE's subcommands
 * that bear on the distribution, before it runs and after.
 */
extern bool GpDistributionMakesUniqueIndex(Node *parsetree);
extern void GpDistributionCheckNewIndex(Node *parsetree);
extern List *GpDistributionAlterTableCheck(AlterTableStmt *stmt);
extern void GpDistributionAlterTableDone(AlterTableStmt *stmt, List *changed);

/*
 * A policy for a table the statement just made: the one given, over the
 * segments a new table gets -- every one, unless gp_debug_numsegments says
 * otherwise.
 */
extern void GpDistributionSetNew(Oid relid, const char *policy);

/*
 * A column of a table dropped, or renamed: the key the label records by name
 * follows it.  A key column dropped leaves the table random, as Cloudberry
 * leaves it.
 */
extern void GpDistributionColumnDropped(Oid relid, AttrNumber attnum);
extern void GpDistributionColumnRenamed(Oid relid, const char *oldname,
										const char *newname);

/*
 * ALTER TABLE ... SET DISTRIBUTED: the new policy ("policy", or NULL for the
 * one it has), and on a cluster the rows moved to where it puts them --
 * "reorganize" 1 always, 0 never, -1 as Cloudberry decides; "recurse" false
 * for ALTER TABLE ONLY.
 */
extern void GpDistributionAlter(Oid relid, const char *policy, int reorganize,
								bool recurse);
extern void GpDistributionDefineSettings(void);

extern PGDLLIMPORT int gp_max_partition_level;

/*
 * The partitions of a table CREATE TABLE ... PARTITION BY ... (...) has just
 * made, from the gp.partition_by option the rewrite put in its WITH list.
 */
extern void GpPartitionCreate(Oid relid, DefElem *option, const char *queryString,
							  QueryEnvironment *queryEnv);

/*
 * ALTER TABLE's partition commands, which the rewrite wrote as SET
 * (gp.partition_cmd = '...'): whether a statement has them, taking them out
 * before PostgreSQL runs it, and doing them once it has.
 */
extern bool GpPartitionHasCmds(AlterTableStmt *stmt);
extern List *GpPartitionTakeCmds(AlterTableStmt *stmt);
extern void GpPartitionAlter(AlterTableStmt *stmt, List *options,
							 const char *queryString, QueryEnvironment *queryEnv);

/*
 * WITH (appendonly = ..., orientation = ...) taken out of an option list, and
 * the access method it names returned: Cloudberry's greenplumLegacyAOoptions.
 */
extern char *GpPartitionLegacyAccessMethod(const char *accessMethod, List **options);

/* CREATE TABLE ... PARTITION OF has run: a Cloudberry table's gets its ACL. */
extern void GpPartitionMade(CreateStmt *stmt);

/* Is it a partitioned table of Cloudberry's, whose partitions it named? */
extern bool GpPartitionIsClassic(Oid relid);

/* GRANT and REVOKE on one reach its partitions: the objects, or NIL. */
extern List *GpPartitionGrantObjects(GrantStmt *stmt);

/* A partitioned table was renamed: its partitions follow, as Cloudberry's do. */
extern void GpPartitionRenamed(Oid relid, const char *oldname, const char *newname);

/*
 * Refuse a tag name or value the definitions in gp_sql.tag do not allow.
 * Shared by the label check hook and the shorthand, so that both answer the
 * same.  Called with the extension's tables reachable.
 */
extern void GpTagValidate(const char *tagname, const char *tagvalue);

#endif							/* GP_SQL_H */
