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
 * mvaux.c
 *	  Which materialized views could answer a query, and whether each is up
 *	  to date: Cloudberry's gp_matview_aux and gp_matview_tables.
 *
 * Cloudberry keeps a row per materialized view in its catalog gp_matview_aux
 * -- its query, whether a base table is foreign, and its data's status: up
 * to date ('u'), its base tables reorganized ('r'), inserted into ('i'), or
 * changed otherwise ('e') -- and a row per base table in gp_matview_tables,
 * written as the view is made, its base tables written and it is refreshed.
 * AQUMV reads them for the views it may answer a query from (aqumv.c), and a
 * REFRESH of a view that is up to date does nothing.
 *
 * Here they are three tables of this module's, the coordinator's metadata as
 * the port's modules' tables are (gp_sql's distribution.c), and Cloudberry's
 * two names are views over them in pg_catalog (gp_matview--1.0.sql):
 *
 *	gp_matview.matview_aux
 *		a view that is registered, and whether a base table of it is foreign
 *	gp_matview.matview_aux_table
 *		its base tables, a self-join's once
 *	gp_matview.matview_aux_event
 *		what was done to its base tables since it was last refreshed: a row
 *		per kind, 'i', 'e' or 'r'
 *
 * The status is not a column written in place but read off the events: 'e'
 * where there is an 'e', or both an 'i' and an 'r'; else 'i' or 'r' where
 * there is one; else 'u' -- the letter Cloudberry's state machine
 * (SetMatviewAuxStatus_guts()) arrives at from the same writes, in whatever
 * order they came.  A writer only ever inserts, so two of them never update
 * one row, where Cloudberry's second fails with "tuple concurrently
 * updated"; a rollback takes its events with it; and a REFRESH deletes the
 * events it can see before it runs, so that one a writer commits while it
 * runs is left behind, and the view stays stale rather than being called
 * fresh over rows it may lack.  The view's query is its rule's, read where
 * it is needed, rather than a node tree kept beside it that outlives the
 * format it was written in.
 *
 * Who writes an event is Cloudberry's list: INSERT and COPY 'i'; UPDATE,
 * DELETE, a data-modifying CTE, TRUNCATE 'e'; VACUUM FULL and CLUSTER 'r';
 * a partition made, attached, detached or dropped 'e' of its parent's
 * views.  A MERGE writes 'e', where Cloudberry's leaves the view as it was.
 * A statement's write of a partitioned table marks, on a node of its own,
 * the views of the partitions it wrote and of their parents, as Cloudberry
 * marks them by the partitions its segments report they wrote; a cluster's
 * coordinator is told nothing of the partitions, and marks the views of the
 * table, all its partitions and its parents, as Cloudberry marks COPY's.
 *
 * The tables are read and written as the bootstrap superuser, as a
 * catalog is written by whoever changes what it describes: a role that
 * writes a base table does not write the view's bookkeeping itself.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/catalog/gp_matview_aux.c; the calls of it in execMain.c
 *	  (MaintainMaterializedViewStatus), copy.c, tablecmds.c, heap.c,
 *	  vacuum.c, cluster.c and matview.c, and the fast path of REFRESH there
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_core_api.h"
#include "gp_matview.h"

/* The data's status, Cloudberry's MV_DATA_STATUS_* letters. */
#define MV_UP_TO_DATE		'u'
#define MV_REORGANIZED		'r'
#define MV_INSERT_ONLY		'i'
#define MV_EXPIRED			'e'

/*
 * The relations some registered view has as a base table: a set read from
 * gp_matview.matview_aux_table and dropped when that table changes, so that
 * a write to any other relation asks nothing more than whether it is in it
 * -- Cloudberry's pg_class.relmvrefcount.
 */
static HTAB *based = NULL;
static Oid	based_from = InvalidOid;	/* the table it was read from */
static bool based_valid = false;
static bool callback_registered = false;

/* The bookkeeping's own statements are neither answered nor bookkept. */
static int	bookkeeping = 0;

/* ------------------------------------------------------------------------- */
/* The tables                                                                */
/* ------------------------------------------------------------------------- */

/*
 * The OID of one of this module's tables in this database, or InvalidOid
 * where the extension is not made here.
 */
static Oid
mvaux_table(const char *name)
{
	Oid			nsp = get_namespace_oid("gp_matview", true);

	return OidIsValid(nsp) ? get_relname_relid(name, nsp) : InvalidOid;
}

/*
 * Does this backend keep the bookkeeping: a cluster's coordinator or a node
 * of its own, in a database gp_matview is made in?  A segment's statements
 * are the coordinator's, which keeps it for them.
 */
static bool
mvaux_here(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	if (core != NULL && !core->is_single_node() &&
		core->get_role() != GP_ROLE_DISPATCH)
		return false;
	return OidIsValid(mvaux_table("matview_aux_event"));
}

/*
 * SPI, as the bootstrap superuser; see the file's header.  Planned by the
 * planner, whose plan of a query of the coordinator's own tables ORCA's
 * would fall back to -- saying so, where a session traces its fallbacks,
 * for each view a statement touches.  What is to outlive it is made in the
 * caller's context, "cxt".
 */
typedef struct MvauxSpi
{
	Oid			userid;
	int			sec_context;
	int			nestlevel;
	MemoryContext cxt;
} MvauxSpi;

static void
spi_begin(MvauxSpi *s)
{
	s->cxt = CurrentMemoryContext;
	GetUserIdAndSecContext(&s->userid, &s->sec_context);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
						   s->sec_context | SECURITY_LOCAL_USERID_CHANGE);
	s->nestlevel = NewGUCNestLevel();
	if (GetConfigOption("gp.optimizer", true, false) != NULL)
		(void) set_config_option("gp.optimizer", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0,
								 false);
	bookkeeping++;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
}

static void
spi_end(MvauxSpi *s)
{
	SPI_finish();
	bookkeeping--;
	AtEOXact_GUC(false, s->nestlevel);
	SetUserIdAndSecContext(s->userid, s->sec_context);
}

static void
spi_run(const char *sql, int nargs, Oid *types, Datum *values, int expected)
{
	int			ret = SPI_execute_with_args(sql, nargs, types, values, NULL,
											false, 0);

	if (ret != expected)
		elog(ERROR, "gp_matview: %s failed (%d)", sql, ret);
}

/* Is the bookkeeping itself running a statement? */
bool
GpMvauxBusy(void)
{
	return bookkeeping > 0;
}

/* ------------------------------------------------------------------------- */
/* The relations with views                                                  */
/* ------------------------------------------------------------------------- */

static void
based_callback(Datum arg, Oid relid)
{
	if (!OidIsValid(relid) || relid == based_from)
		based_valid = false;
}

/* The set, read again where it has been dropped. */
static void
based_load(Oid aux_table)
{
	HASHCTL		ctl;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

	if (!callback_registered)
	{
		CacheRegisterRelcacheCallback(based_callback, (Datum) 0);
		callback_registered = true;
	}
	if (based_valid && based_from == aux_table)
		return;

	if (based != NULL)
		hash_destroy(based);
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Oid);
	ctl.hcxt = TopMemoryContext;
	based = hash_create("gp_matview base tables", 64, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* valid from here: an invalidation while it is read drops it again */
	based_from = aux_table;
	based_valid = true;

	rel = table_open(aux_table, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Oid			relid = DatumGetObjectId(heap_getattr(tup, 2,
														  RelationGetDescr(rel),
														  &isnull));

		(void) hash_search(based, &relid, HASH_ENTER, NULL);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
}

/* Is the relation a base table of some registered view? */
static bool
has_views(Oid relid)
{
	Oid			aux_table = mvaux_table("matview_aux_table");

	if (!OidIsValid(aux_table))
		return false;
	based_load(aux_table);
	return hash_search(based, &relid, HASH_FIND, NULL) != NULL;
}

/* Is any relation a base table of a registered view? */
static bool
any_views(void)
{
	Oid			aux_table = mvaux_table("matview_aux_table");

	if (!OidIsValid(aux_table))
		return false;
	based_load(aux_table);
	return hash_get_num_entries(based) > 0;
}

/* The base tables changed: every backend reads them again. */
static void
based_changed(void)
{
	Oid			aux_table = mvaux_table("matview_aux_table");

	if (OidIsValid(aux_table))
		CacheInvalidateRelcacheByRelid(aux_table);
}

/* ------------------------------------------------------------------------- */
/* Registering a view                                                        */
/* ------------------------------------------------------------------------- */

/*
 * A join tree's base tables, into *relids, a self-join's once: false where
 * it reads anything else -- a subquery, a function, a table of an
 * inheritance tree that is not a partitioned one's.  Cloudberry's
 * extract_base_relids_from_jointree().
 */
static bool
jointree_relids(Node *jtnode, List *rtable, List **relids, bool *has_foreign)
{
	if (jtnode == NULL)
		return false;

	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblEntry *rte = rt_fetch(((RangeTblRef *) jtnode)->rtindex, rtable);
		char		relkind;
		bool		partition;

		if (rte->rtekind != RTE_RELATION)
			return false;
		relkind = get_rel_relkind(rte->relid);
		if (relkind != RELKIND_RELATION &&
			relkind != RELKIND_PARTITIONED_TABLE &&
			relkind != RELKIND_FOREIGN_TABLE)
			return false;
		if (relkind == RELKIND_FOREIGN_TABLE)
			*has_foreign = true;
		partition = relkind == RELKIND_PARTITIONED_TABLE ||
			get_rel_relispartition(rte->relid);
		if (!partition && (has_superclass(rte->relid) || has_subclass(rte->relid)))
			return false;
		*relids = list_append_unique_oid(*relids, rte->relid);
		return true;
	}
	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		return jointree_relids(j->larg, rtable, relids, has_foreign) &&
			jointree_relids(j->rarg, rtable, relids, has_foreign);
	}
	if (IsA(jtnode, FromExpr))
	{
		ListCell   *lc;

		foreach(lc, ((FromExpr *) jtnode)->fromlist)
			if (!jointree_relids((Node *) lfirst(lc), rtable, relids, has_foreign))
				return false;
		return true;
	}
	return false;
}

/*
 * The base tables of a view's query, as written, or NIL where the query is
 * none AQUMV could use: Cloudberry's GetViewBaseRelids().  Its functions
 * have to be immutable -- AQUMV answers a query from what they computed when
 * the view was refreshed.
 */
static List *
view_base_relids(Query *query, bool *has_foreign)
{
	List	   *relids = NIL;

	*has_foreign = false;
	if (query->commandType != CMD_SELECT || query->rowMarks != NIL ||
		query->distinctClause != NIL || query->cteList != NIL ||
		query->groupingSets != NIL || query->havingQual != NULL ||
		query->setOperations != NULL || query->hasWindowFuncs ||
		query->hasDistinctOn || query->hasModifyingCTE ||
		query->groupDistinct || query->hasSubLinks || query->hasTargetSRFs)
		return NIL;
	if (contain_mutable_functions((Node *) query))
		return NIL;
	if (!jointree_relids((Node *) query->jointree, query->rtable, &relids,
						 has_foreign))
		return NIL;
	return relids;
}

/*
 * A materialized view just made: registered if AQUMV could answer from it,
 * with its base tables, and expired where it was made WITH NO DATA.
 * Cloudberry's InsertMatviewAuxEntry().
 */
void
GpMvauxRegister(Oid mvoid, Query *viewQuery, bool skipdata)
{
	List	   *relids;
	bool		has_foreign;
	MvauxSpi	s;
	ListCell   *lc;
	Oid			types[2] = {OIDOID, BOOLOID};
	Datum		values[2];

	if (!mvaux_here())
		return;
	relids = view_base_relids(viewQuery, &has_foreign);
	if (relids == NIL)
		return;

	spi_begin(&s);
	values[0] = ObjectIdGetDatum(mvoid);
	values[1] = BoolGetDatum(has_foreign);
	spi_run("INSERT INTO gp_matview.matview_aux VALUES ($1, $2)"
			" ON CONFLICT DO NOTHING",
			2, types, values, SPI_OK_INSERT);
	types[1] = OIDOID;
	foreach(lc, relids)
	{
		values[1] = ObjectIdGetDatum(lfirst_oid(lc));
		spi_run("INSERT INTO gp_matview.matview_aux_table VALUES ($1, $2)"
				" ON CONFLICT DO NOTHING",
				2, types, values, SPI_OK_INSERT);
	}
	if (skipdata)
	{
		types[1] = CHAROID;
		values[1] = CharGetDatum(MV_EXPIRED);
		spi_run("INSERT INTO gp_matview.matview_aux_event VALUES ($1, $2)",
				2, types, values, SPI_OK_INSERT);
	}
	spi_end(&s);
	based_changed();
}

/* A view dropped: its rows go.  Cloudberry's dependency does it there. */
static void
mvaux_forget(Oid mvoid)
{
	MvauxSpi	s;
	Oid			types[1] = {OIDOID};
	Datum		values[1];
	bool		registered;

	if (!mvaux_here())
		return;

	spi_begin(&s);
	values[0] = ObjectIdGetDatum(mvoid);
	spi_run("DELETE FROM gp_matview.matview_aux WHERE mvoid = $1",
			1, types, values, SPI_OK_DELETE);
	registered = SPI_processed > 0;
	if (registered)
	{
		spi_run("DELETE FROM gp_matview.matview_aux_table WHERE mvoid = $1",
				1, types, values, SPI_OK_DELETE);
		spi_run("DELETE FROM gp_matview.matview_aux_event WHERE mvoid = $1",
				1, types, values, SPI_OK_DELETE);
	}
	spi_end(&s);
	if (registered)
		based_changed();
}

/* ------------------------------------------------------------------------- */
/* The status                                                                */
/* ------------------------------------------------------------------------- */

/*
 * A registered view's status, read off its events; '\0' for a view that is
 * not registered.
 */
char
GpMvauxStatus(Oid mvoid, bool *has_foreign)
{
	MvauxSpi	s;
	Oid			types[1] = {OIDOID};
	Datum		values[1];
	char		status = '\0';

	*has_foreign = false;
	if (!mvaux_here())
		return status;

	spi_begin(&s);
	values[0] = ObjectIdGetDatum(mvoid);
	spi_run("SELECT m.has_foreign,"
			" coalesce(bool_or(e.kind = 'e'), false),"
			" coalesce(bool_or(e.kind = 'i'), false),"
			" coalesce(bool_or(e.kind = 'r'), false)"
			" FROM gp_matview.matview_aux m"
			" LEFT JOIN gp_matview.matview_aux_event e ON e.mvoid = m.mvoid"
			" WHERE m.mvoid = $1 GROUP BY m.has_foreign",
			1, types, values, SPI_OK_SELECT);
	if (SPI_processed == 1)
	{
		HeapTuple	tup = SPI_tuptable->vals[0];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		bool		isnull;
		bool		expired = DatumGetBool(SPI_getbinval(tup, desc, 2, &isnull));
		bool		inserted = DatumGetBool(SPI_getbinval(tup, desc, 3, &isnull));
		bool		reorganized = DatumGetBool(SPI_getbinval(tup, desc, 4, &isnull));

		*has_foreign = DatumGetBool(SPI_getbinval(tup, desc, 1, &isnull));
		if (expired || (inserted && reorganized))
			status = MV_EXPIRED;
		else if (inserted)
			status = MV_INSERT_ONLY;
		else if (reorganized)
			status = MV_REORGANIZED;
		else
			status = MV_UP_TO_DATE;
	}
	spi_end(&s);
	return status;
}

/*
 * The registered views whose base tables are exactly "relids", in the order
 * they were registered: the candidates AQUMV tries (aqumv.c).
 */
List *
GpMvauxViewsOver(List *relids)
{
	MvauxSpi	s;
	Oid			types[2] = {OIDOID, INT4OID};
	Datum		values[2];
	List	   *views = NIL;

	if (relids == NIL || !mvaux_here() || !has_views(linitial_oid(relids)))
		return NIL;

	spi_begin(&s);
	values[0] = ObjectIdGetDatum(linitial_oid(relids));
	values[1] = Int32GetDatum(list_length(relids));
	spi_run("SELECT t.mvoid, array_agg(t.relid)"
			" FROM gp_matview.matview_aux_table t"
			" WHERE t.mvoid IN (SELECT mvoid FROM gp_matview.matview_aux_table"
			" WHERE relid = $1)"
			" GROUP BY t.mvoid HAVING count(*) = $2 ORDER BY t.mvoid",
			2, types, values, SPI_OK_SELECT);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tup = SPI_tuptable->vals[i];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		bool		isnull;
		Oid			mvoid = DatumGetObjectId(SPI_getbinval(tup, desc, 1, &isnull));
		ArrayType  *arr = DatumGetArrayTypeP(SPI_getbinval(tup, desc, 2, &isnull));
		Datum	   *elems;
		int			n;
		bool		all = true;

		deconstruct_array_builtin(arr, OIDOID, &elems, NULL, &n);
		for (int k = 0; k < n && all; k++)
			all = list_member_oid(relids, DatumGetObjectId(elems[k]));
		if (all)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(s.cxt);

			views = lappend_oid(views, mvoid);
			MemoryContextSwitchTo(oldcxt);
		}
	}
	spi_end(&s);
	return views;
}

/* ------------------------------------------------------------------------- */
/* Writes                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * An event of "kind" for every view whose base tables include one of
 * "relids", where it has none of that kind yet; and each view that has one
 * now invalidated, so that a plan the plan cache keeps of a query AQUMV
 * answered from it is made again.
 */
static void
add_events(List *relids, char kind)
{
	MvauxSpi	s;
	Oid			types[2] = {OIDARRAYOID, CHAROID};
	Datum		values[2];
	Datum	   *elems;
	int			n = 0;
	ListCell   *lc;

	elems = palloc_array(Datum, Max(list_length(relids), 1));
	foreach(lc, relids)
		if (has_views(lfirst_oid(lc)))
			elems[n++] = ObjectIdGetDatum(lfirst_oid(lc));
	if (n == 0)
		return;

	spi_begin(&s);
	values[0] = PointerGetDatum(construct_array_builtin(elems, n, OIDOID));
	values[1] = CharGetDatum(kind);
	spi_run("INSERT INTO gp_matview.matview_aux_event"
			" SELECT DISTINCT t.mvoid, $2 FROM gp_matview.matview_aux_table t"
			" WHERE t.relid = ANY ($1) AND NOT EXISTS"
			" (SELECT 1 FROM gp_matview.matview_aux_event e"
			" WHERE e.mvoid = t.mvoid AND e.kind = $2)"
			" RETURNING mvoid",
			2, types, values, SPI_OK_INSERT_RETURNING);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		Datum		mvoid = SPI_getbinval(SPI_tuptable->vals[i],
										  SPI_tuptable->tupdesc, 1, &isnull);

		CacheInvalidateRelcacheByRelid(DatumGetObjectId(mvoid));
	}
	spi_end(&s);
}

/*
 * The relations whose views a write of "relid" reaches: it and its parents
 * -- and, "all", its partitions too, which a write through a partitioned
 * table may have reached.  Cloudberry's SetRelativeMatviewAuxStatus(), its
 * directions ALL and UP.
 */
static List *
written_relids(List *relids, Oid relid, bool all)
{
	char		relkind = get_rel_relkind(relid);

	if (relkind == '\0')
		return relids;
	if (all && relkind == RELKIND_PARTITIONED_TABLE)
		relids = list_concat(relids, find_all_inheritors(relid, NoLock, NULL));
	else
		relids = lappend_oid(relids, relid);
	if (get_rel_relispartition(relid))
		relids = list_concat(relids, get_partition_ancestors(relid));
	return relids;
}

/* A relation written, "kind" of event; see written_relids(). */
static void
mvaux_written(Oid relid, char kind, bool all)
{
	if (bookkeeping > 0 || !mvaux_here() || !any_views())
		return;
	add_events(written_relids(NIL, relid, all), kind);
}

/* Is the relation a partition, at any depth, of "parent"? */
static bool
partition_of(Oid relid, Oid parent)
{
	return get_rel_relispartition(relid) &&
		list_member_oid(get_partition_ancestors(relid), parent);
}

/*
 * A statement's executor finished: the views of the tables it wrote.  An
 * INSERT's are inserted into, as Cloudberry has it; an UPDATE's, DELETE's,
 * MERGE's and a data-modifying CTE's expired.  One that wrote no row
 * changed nothing -- but a statement that does not set the command's tag,
 * a rule's action, says nothing by its count.
 *
 * A node of its own ran the whole statement, and its executor's result
 * relations are the tables it wrote: those of the plan, less the partitions
 * pruned, and the partitions rows were routed to, which were inserted into
 * -- by an INSERT, or by an UPDATE that moved a row to another partition.
 * A partitioned table among them was written through those, so that its
 * partitions no row reached keep their views' status, as Cloudberry's do,
 * where its segments report the partitions they wrote; one with no
 * partition among them marks all its partitions' views.  A cluster's
 * coordinator routes no rows, and knows no more than the plan says: a write
 * of a partitioned table marks the views of all its partitions.
 */
void
GpMvauxStatementEnd(QueryDesc *queryDesc)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	EState	   *estate = queryDesc->estate;
	const GpCoreApi *core = GpCoreApiLookup();
	List	   *relids = NIL;
	List	   *inserted = NIL;
	List	   *partitioned = NIL;
	char		kind;
	ListCell   *lc;

	if (bms_is_empty(stmt->resultRelationRelids) || bookkeeping > 0 ||
		!mvaux_here())
		return;
	if (estate->es_processed == 0 && stmt->canSetTag &&
		!stmt->hasModifyingCTE)
		return;
	if (!any_views())
		return;

	kind = stmt->commandType == CMD_INSERT && !stmt->hasModifyingCTE
		? MV_INSERT_ONLY : MV_EXPIRED;

	if (core != NULL && !core->is_single_node())
	{
		int			rti = -1;

		while ((rti = bms_next_member(stmt->resultRelationRelids, rti)) >= 0)
		{
			RangeTblEntry *rte = rt_fetch(rti, stmt->rtable);

			if (rte->rtekind == RTE_RELATION)
				relids = written_relids(relids, rte->relid, true);
		}
		add_events(relids, kind);
		return;
	}

	foreach(lc, estate->es_opened_result_relations)
	{
		Relation	rel = ((ResultRelInfo *) lfirst(lc))->ri_RelationDesc;

		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			partitioned = lappend_oid(partitioned, RelationGetRelid(rel));
		else
			relids = written_relids(relids, RelationGetRelid(rel), false);
	}
	foreach(lc, estate->es_tuple_routing_result_relations)
	{
		Relation	rel = ((ResultRelInfo *) lfirst(lc))->ri_RelationDesc;

		inserted = written_relids(inserted, RelationGetRelid(rel), false);
	}
	foreach(lc, partitioned)
	{
		Oid			parent = lfirst_oid(lc);
		List	   *written = list_concat_copy(estate->es_opened_result_relations,
											   estate->es_tuple_routing_result_relations);
		bool		reached = false;
		ListCell   *lc2;

		foreach(lc2, written)
		{
			ResultRelInfo *rri = (ResultRelInfo *) lfirst(lc2);

			reached = reached ||
				partition_of(RelationGetRelid(rri->ri_RelationDesc), parent);
		}
		if (!reached)
			relids = written_relids(relids, parent, true);
	}
	add_events(relids, kind);
	add_events(inserted, MV_INSERT_ONLY);
}

/* ------------------------------------------------------------------------- */
/* REFRESH                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Is REFRESH of this view one that need not run: Cloudberry's fast path,
 * for a view up to date -- not incremental, which its maintenance keeps
 * whatever its status says, and reading no foreign table, whose data changes
 * where no event is written.
 */
bool
GpMvauxRefreshNeedless(Oid mvoid)
{
	bool		has_foreign;

	return !GpIvmIsIncremental(mvoid) &&
		GpMvauxStatus(mvoid, &has_foreign) == MV_UP_TO_DATE && !has_foreign;
}

/*
 * A REFRESH is about to run: the events it can see go, which is what it
 * brings the view up to date with.  WITH NO DATA leaves it expired.
 */
void
GpMvauxRefreshing(Oid mvoid, bool skipdata)
{
	MvauxSpi	s;
	Oid			types[2] = {OIDOID, CHAROID};
	Datum		values[2];

	if (!mvaux_here())
		return;

	spi_begin(&s);
	values[0] = ObjectIdGetDatum(mvoid);
	values[1] = CharGetDatum(MV_EXPIRED);
	spi_run("DELETE FROM gp_matview.matview_aux_event WHERE mvoid = $1",
			1, types, values, SPI_OK_DELETE);
	if (skipdata)
		spi_run("INSERT INTO gp_matview.matview_aux_event SELECT $1, $2"
				" WHERE EXISTS (SELECT 1 FROM gp_matview.matview_aux"
				" WHERE mvoid = $1)",
				2, types, values, SPI_OK_INSERT);
	spi_end(&s);
}

/* ------------------------------------------------------------------------- */
/* Utility statements                                                        */
/* ------------------------------------------------------------------------- */

/*
 * The relations a VACUUM FULL names, or every one with views where it names
 * none.
 */
static void
vacuum_full(VacuumStmt *stmt)
{
	ListCell   *lc;
	bool		full = false;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "full") == 0)
			full = defGetBoolean(opt);
	}
	if (!full || !stmt->is_vacuumcmd)
		return;

	if (stmt->rels == NIL)
	{
		HASH_SEQ_STATUS seq;
		Oid		   *relid;
		List	   *relids = NIL;

		if (!OidIsValid(mvaux_table("matview_aux_table")))
			return;
		based_load(mvaux_table("matview_aux_table"));
		hash_seq_init(&seq, based);
		while ((relid = (Oid *) hash_seq_search(&seq)) != NULL)
			relids = lappend_oid(relids, *relid);
		add_events(relids, MV_REORGANIZED);
		return;
	}
	foreach(lc, stmt->rels)
	{
		VacuumRelation *vrel = (VacuumRelation *) lfirst(lc);
		Oid			relid = OidIsValid(vrel->oid) ? vrel->oid
			: RangeVarGetRelid(vrel->relation, NoLock, true);

		if (OidIsValid(relid))
			mvaux_written(relid, MV_REORGANIZED, true);
	}
}

/*
 * A utility statement done: the views it changed the status of.  COPY FROM
 * that brought rows inserted into its table; VACUUM FULL, CLUSTER and REPACK
 * reorganized theirs; a partition made, attached or detached expired its
 * parent's.
 */
void
GpMvauxUtilityEnd(Node *parsetree, QueryCompletion *qc)
{
	if (bookkeeping > 0 || !mvaux_here())
		return;

	switch (nodeTag(parsetree))
	{
		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;
				Oid			relid;

				if (!stmt->is_from || stmt->relation == NULL || qc == NULL ||
					qc->nprocessed == 0)
					break;
				relid = RangeVarGetRelid(stmt->relation, NoLock, true);
				if (OidIsValid(relid))
					mvaux_written(relid, MV_INSERT_ONLY, true);
				break;
			}
		case T_VacuumStmt:
			vacuum_full((VacuumStmt *) parsetree);
			break;
		case T_RepackStmt:
			{
				RepackStmt *stmt = (RepackStmt *) parsetree;
				Oid			relid = InvalidOid;

				if (stmt->relation != NULL)
					relid = OidIsValid(stmt->relation->oid) ? stmt->relation->oid
						: RangeVarGetRelid(stmt->relation->relation, NoLock, true);
				if (OidIsValid(relid))
					mvaux_written(relid, MV_REORGANIZED, true);
				break;
			}
		case T_CreateStmt:
			{
				CreateStmt *stmt = (CreateStmt *) parsetree;
				Oid			parent;

				if (stmt->partbound == NULL || list_length(stmt->inhRelations) != 1)
					break;
				parent = RangeVarGetRelid(linitial_node(RangeVar, stmt->inhRelations),
										  NoLock, true);
				if (OidIsValid(parent))
					mvaux_written(parent, MV_EXPIRED, false);
				break;
			}
		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
				ListCell   *lc;
				Oid			relid;

				foreach(lc, stmt->cmds)
				{
					AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lc);

					if (cmd->subtype != AT_AttachPartition &&
						cmd->subtype != AT_DetachPartition &&
						cmd->subtype != AT_DetachPartitionFinalize)
						continue;
					relid = RangeVarGetRelid(stmt->relation, NoLock, true);
					if (OidIsValid(relid))
						mvaux_written(relid, MV_EXPIRED, false);
					break;
				}
				break;
			}
		default:
			break;
	}
}

/*
 * The object access hook's part: a view dropped forgets its rows; a
 * partition dropped expires its parents' views; a table truncated expires
 * its views -- once for each, cascades and partitions among them, before
 * it is truncated, and rolled back with the statement.
 */
void
GpMvauxObjectAccess(ObjectAccessType access, Oid classId, Oid objectId,
					int subId)
{
	char		relkind;

	if (classId != RelationRelationId || subId != 0 || bookkeeping > 0)
		return;

	if (access == OAT_TRUNCATE)
	{
		mvaux_written(objectId, MV_EXPIRED, true);
		return;
	}
	if (access != OAT_DROP)
		return;

	relkind = get_rel_relkind(objectId);
	if (relkind == RELKIND_MATVIEW)
		mvaux_forget(objectId);
	else if ((relkind == RELKIND_RELATION ||
			  relkind == RELKIND_PARTITIONED_TABLE ||
			  relkind == RELKIND_FOREIGN_TABLE) &&
			 get_rel_relispartition(objectId))
		mvaux_written(get_partition_parent(objectId, true), MV_EXPIRED, false);
}
