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
 * pg_iceberg_ddl.c
 *	  Object-access integration for the Iceberg table lifecycle.
 *
 * IDENTIFICATION
 *	  contrib/datalake_fdw/src/am_iceberg/pg_iceberg_ddl.c
 *
 *
 * Ported to PostgreSQL 19:
 *   - the metadata engine is called by the coordinator, and by a node on its
 *     own, which is its own coordinator; Cloudberry's asked Gp_role for the
 *     dispatcher, which a single node is not (cb_compat.h's
 *     IS_QD_OR_SINGLENODE());
 *   - the missing-ok lookup of a relation's access method is named apart
 *     from lsyscache's get_rel_relam(), which PostgreSQL 19 has and which
 *     raises where the relation is gone;
 *   - try_relation_open() takes no noWait, which was Cloudberry's;
 *   - a table just created is described by its pg_class row as the creating
 *     command wrote it (created_relation_options()): the relcache entry
 *     heap_create() built has no options, which Cloudberry's had by the time
 *     its hook ran; and those options are checked there, before the engine
 *     hears of the table, which PostgreSQL 19 checks only once the table is
 *     made (O14), where Cloudberry checked them before making it.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "am_iceberg/pg_iceberg_ddl.h"
#include "am_iceberg/pg_iceberg_options.h"
#include "am_iceberg/pg_iceberg_reject.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "meta/iceberg_meta_engine.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "cb_compat.h"

object_access_hook_type pg_iceberg_prev_object_access_hook;

static Oid rel_relam_missing_ok(Oid relid);
static bytea *created_relation_options(Relation rel);
static MetaCtx table_info_meta_ctx(const IcebergTableInfo *info);
static void iceberg_post_create(Oid objectId);
static void iceberg_drop(Oid objectId);

/*
 * lsyscache's get_rel_relam(), but missing-ok: InvalidOid for a relation
 * that is gone.
 */
static Oid
rel_relam_missing_ok(Oid relid)
{
	HeapTuple	tuple;
	Oid			relam = InvalidOid;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tuple))
	{
		relam = ((Form_pg_class) GETSTRUCT(tuple))->relam;
		ReleaseSysCache(tuple);
	}

	return relam;
}

/*
 * The options of a relation its own command has just created, checked and
 * parsed by its access method: its pg_class row is not visible yet, so it is
 * read as this command wrote it, as sepgsql's post-create hook reads a new
 * row (SnapshotSelf).  NULL where the row has none.
 */
static bytea *
created_relation_options(Relation rel)
{
	Relation	class_rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	bytea	   *options = NULL;

	class_rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(class_rel, ClassOidIndexId, true,
							  SnapshotSelf, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		datum = heap_getattr(tuple, Anum_pg_class_reloptions,
							 RelationGetDescr(class_rel), &isnull);
		if (!isnull)
			options = table_am_reloptions(rel->rd_tableam, rel->rd_rel->relkind,
										  datum, true);
	}
	systable_endscan(scan);
	table_close(class_rel, AccessShareLock);

	return options;
}

static MetaCtx
table_info_meta_ctx(const IcebergTableInfo *info)
{
	MetaCtx	ctx = {
		.catalog_name = info->catalog_name,
		.namespace_name = info->opts->namespace,
		.table_name = info->opts->table,
		.catalog_props = info->catalog_props,
		.n_catalog_props = info->n_catalog_props,
		.credential_props = NULL,
		.n_credential_props = 0
	};

	return ctx;
}

static void
iceberg_post_create(Oid objectId)
{
	Relation	rel;
	Oid			amoid;
	IcebergTableInfo *info;

	amoid = pg_iceberg_am_oid();
	if (!OidIsValid(amoid))
		return;

	rel = relation_open(objectId, AccessShareLock);
	if (rel->rd_rel->relam != amoid)
	{
		relation_close(rel, AccessShareLock);
		return;
	}

	info = pg_iceberg_get_table_info_options(rel, created_relation_options(rel));
	InsertLakeTableEntry(objectId, info);

	if (IS_QD_OR_SINGLENODE())
	{
		const IcebergMetaEngine *engine = get_meta_engine();
		MetaCtx		ctx = table_info_meta_ctx(info);
		MetaTableDef def = {.schema_json = ""};
		MetaTable  *table_metadata = NULL;
		DlErrCode	rc;

		/* Always cross the metadata-engine boundary through its wrapper. */
		rc = meta_engine_create_table(engine, &ctx, &def, &table_metadata);
		relation_close(rel, AccessShareLock);

		/* ctx borrows from info, so release only once the call has returned. */
		pg_iceberg_free_table_info(info);

		if (rc != DL_OK)
			dl_error_report(ERROR, rc, "create_table");
		return;
	}

	relation_close(rel, AccessShareLock);
	pg_iceberg_free_table_info(info);
}

static void
iceberg_drop(Oid objectId)
{
	Relation	rel;
	Oid			amoid;
	IcebergTableInfo *info;
	const IcebergMetaEngine *engine;
	MetaCtx		ctx;
	DlErrCode	rc;

	amoid = pg_iceberg_am_oid();
	if (!OidIsValid(amoid) || rel_relam_missing_ok(objectId) != amoid)
		return;

	/*
	 * The dispatcher -- or a node on its own -- makes the single remote
	 * call; everyone else only drops local catalog rows and the dependencies
	 * recorded at creation.
	 *
	 * A utility-mode backend deliberately falls in the second group.  It talks
	 * to one node, so letting it delete the remote table would remove metadata
	 * the other nodes still reference.  The utility guard refuses the
	 * statements that reach a lake table directly; an indirect cascade that
	 * slips past it drops the local rows only, which is recoverable, unlike a
	 * remote catalog entry deleted on one node's say-so.
	 */
	if (!IS_QD_OR_SINGLENODE())
		return;

	/*
	 * Defensive: no path the regression suite covers -- DROP TABLE, DROP SERVER
	 * CASCADE, DROP SCHEMA CASCADE -- reaches this hook with the relation no
	 * longer openable, so the open below has always succeeded.  It stays a try
	 * rather than an open because a hook that raised here would make the object
	 * undroppable, and there is nothing to reconstruct the mapping from anyway.
	 */
	rel = try_relation_open(objectId, AccessShareLock);
	if (rel == NULL)
		return;

	/*
	 * Resolving the mapping can fail -- a server option that no longer parses,
	 * a wrapper renamed out from under the table -- and DROP is exactly the
	 * statement that must still work in that state.  Report what could not be
	 * cleaned up remotely and let the local drop proceed, rather than leaving
	 * the user with a table that cannot be dropped at all.
	 */
	info = NULL;
	PG_TRY();
	{
		info = pg_iceberg_get_table_info_rel(rel);
	}
	PG_CATCH();
	{
		MemoryContext ctxt = MemoryContextSwitchTo(TopTransactionContext);
		ErrorData  *edata = CopyErrorData();

		MemoryContextSwitchTo(ctxt);

		/*
		 * Only a mapping that no longer describes anything usable may be
		 * downgraded here.  A cancelled query or an out-of-memory failure has
		 * nothing to do with the mapping, and turning one of those into a
		 * warning would drop the table while pretending the statement
		 * succeeded.
		 */
		if (edata->sqlerrcode != ERRCODE_INVALID_TABLE_DEFINITION &&
			edata->sqlerrcode != ERRCODE_INVALID_PARAMETER_VALUE &&
			edata->sqlerrcode != ERRCODE_UNDEFINED_OBJECT)
		{
			FreeErrorData(edata);
			relation_close(rel, AccessShareLock);
			PG_RE_THROW();
		}

		FlushErrorState();
		relation_close(rel, AccessShareLock);
		ereport(WARNING,
				(errmsg("iceberg: dropping \"%s\" without notifying the metadata engine",
						get_rel_name(objectId)),
				 errdetail("%s", edata->message)));
		FreeErrorData(edata);
		return;
	}
	PG_END_TRY();

	engine = get_meta_engine();
	ctx = table_info_meta_ctx(info);

	/*
	 * Dropping the table drops this database's reference to it.  Whether the
	 * lake data goes too is the table's own decision, recorded in its options
	 * when it was created; the default is to leave it, so that dropping a
	 * reference cannot destroy data another reader still expects to find.
	 *
	 * A table's identity is its (catalog, namespace, name) triple, so there is
	 * nothing to fence this call against: a remote table under that name is by
	 * definition the table being dropped.
	 */
	rc = meta_engine_drop_table(engine, &ctx, info->opts->purge_on_drop);
	relation_close(rel, AccessShareLock);

	/* ctx borrows from info, so release only once the call has returned. */
	pg_iceberg_free_table_info(info);

	/*
	 * Do not strand the local table when the remote drop fails.  The catalog
	 * DROP continues and the failure stays visible; reconciling what the
	 * remote catalog still holds is the metadata agent's job, not something
	 * this skeleton can record durably.
	 */
	if (rc != DL_OK)
		dl_error_report(WARNING, rc, "drop_table");
}

void
pg_iceberg_object_access(ObjectAccessType access,
						 Oid classId,
						 Oid objectId,
						 int subId,
						 void *arg)
{
	if (pg_iceberg_prev_object_access_hook)
		(*pg_iceberg_prev_object_access_hook) (access, classId, objectId,
											  subId, arg);

	if (classId != RelationRelationId || subId != 0)
		return;

	switch (access)
	{
		case OAT_POST_CREATE:
			{
				ObjectAccessPostCreate *created = (ObjectAccessPostCreate *) arg;

				/*
				 * Relations the system builds for itself -- the transient heap
				 * of a rewrite above all -- must never reach the metadata
				 * engine.  They carry a generated name, they live only until
				 * the rewrite swaps them in, and creating them remotely leaves
				 * an orphan the moment the local work rolls back.  The utility
				 * hook refuses the statements that rewrite a lake table; this
				 * is the backstop for whatever it does not see.
				 */
				if (created != NULL && created->is_internal)
					return;

				iceberg_post_create(objectId);
			}
			break;
		case OAT_DROP:
			iceberg_drop(objectId);
			break;
		case OAT_TRUNCATE:

			/*
			 * TRUNCATE of a table created in an earlier transaction never
			 * reaches the access method's nontransactional path: it goes
			 * through relation_set_new_filelocator, which succeeds because a
			 * lake table has no local storage to reset.  Without this event
			 * the statement would report success while every Iceberg data file
			 * stayed exactly where it was.
			 */
			if (OidIsValid(pg_iceberg_am_oid()) &&
				rel_relam_missing_ok(objectId) == pg_iceberg_am_oid())
				pg_iceberg_not_supported("TRUNCATE");
			break;
		default:
			break;
	}
}
