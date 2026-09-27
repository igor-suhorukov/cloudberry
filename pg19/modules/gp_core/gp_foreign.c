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
 * gp_foreign.c
 *	  Where a foreign table is read: Cloudberry's mpp_execute and
 *	  num_segments, options of a foreign-data wrapper, a server or a foreign
 *	  table, the table's winning.
 *
 * Cloudberry keeps them among the object's options, as the statement gave
 * them, and takes them out of what the wrapper's validator is given and of
 * what the wrapper reads (transformGenericOptions(), GetForeignTable()): a
 * wrapper that refuses an option it does not know, as postgres_fdw's
 * validator does, takes them.  PostgreSQL 19 gives the validator every
 * option.  So a statement that sets them runs without them, and they are
 * written into the object's options after it, where psql's \d+ and pg_dump
 * find them: gp_ddl.c runs each such statement here, on the coordinator and
 * on each segment, after the tree, options and all, has been kept to send
 * the segments; on one node, where DDL dispatch installs no hook, a hook of
 * this file's does (GpForeignInit()).  An ALTER of an object that has them already runs with them
 * out of its options too, since PostgreSQL validates the options an ALTER
 * leaves, among them the ones set before.
 *
 * A wrapper reads every option still -- GetForeignTable() has no hook -- so
 * one that hands a table's options to something that refuses what it does
 * not know refuses a table that has them: PostgreSQL's file_fdw hands them
 * to COPY, where Cloudberry's GetForeignTable() takes them out first.
 *
 * mpp_execute 'all segments' is read on the segments, each segment its
 * share, as Cloudberry's planner makes a foreign scan's locus strewn over
 * num_segments of them (pathnode.c, make_cdbpathlocus_for_foreign_relations())
 * and its GpPolicyFetch() a random policy (cdbcat.c): gp_policy.c's
 * GpPolicyGet() answers such a policy, which gp_distribution_policy, which
 * shows what labels record, does not show, as Cloudberry's does not.
 * 'coordinator' -- or 'master' -- and the default are read on the
 * coordinator; 'any' too, of which Cloudberry's planner makes a general
 * locus, and ORCA a universal distribution, which it maps as Cloudberry's.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/foreign/foreign.c (SeparateOutMppExecute(),
 *	  SeparateOutNumSegments(), GetForeignTable()'s and GetForeignServer()'s
 *	  exec_location and num_segments), src/backend/commands/foreigncmds.c
 *	  (transformGenericOptions()'s, and AlterForeignDataWrapper()'s and
 *	  AlterForeignServer()'s own)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "tcop/utility.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_foreign.h"

#define MPP_EXECUTE		"mpp_execute"
#define NUM_SEGMENTS	"num_segments"

/* ------------------------------------------------------------------------- */
/* The options' values                                                       */
/* ------------------------------------------------------------------------- */

static bool
is_placement(const DefElem *def)
{
	return strcmp(def->defname, MPP_EXECUTE) == 0 ||
		strcmp(def->defname, NUM_SEGMENTS) == 0;
}

/* mpp_execute's value, as Cloudberry's SeparateOutMppExecute() takes it */
static char
exec_location_of(DefElem *def)
{
	char	   *value = defGetString(def);

	if (pg_strcasecmp(value, "any") == 0)
		return GP_FOREIGN_ANY;
	if (pg_strcasecmp(value, "master") == 0 ||
		pg_strcasecmp(value, "coordinator") == 0)
		return GP_FOREIGN_COORDINATOR;
	if (pg_strcasecmp(value, "all segments") == 0)
		return GP_FOREIGN_ALL_SEGMENTS;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("\"%s\" is not a valid mpp_execute value", value)));
	return GP_FOREIGN_COORDINATOR;	/* keep the compiler quiet */
}

/* num_segments' value, as SeparateOutNumSegments() takes it */
static int
num_segments_of(DefElem *def)
{
	char	   *endp;
	int32		n = strtol(defGetString(def), &endp, 10);

	if (n <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("\"%d\" is not a valid num_segments value", n)));
	return n;
}

/*
 * An object's mpp_execute and, if numsegments is not NULL, num_segments;
 * each left as it is where the options have none.
 */
static void
placement_of(List *options, char *location, int *numsegments)
{
	foreach_node(DefElem, def, options)
	{
		if (strcmp(def->defname, MPP_EXECUTE) == 0)
			*location = exec_location_of(def);
		else if (numsegments != NULL && strcmp(def->defname, NUM_SEGMENTS) == 0)
			*numsegments = num_segments_of(def);
	}
}

char
GpForeignExecLocation(Oid relid, int *numsegments)
{
	ForeignTable *table = GetForeignTable(relid);
	ForeignServer *server = GetForeignServer(table->serverid);
	ForeignDataWrapper *fdw = GetForeignDataWrapper(server->fdwid);
	char		location = '\0';
	char		server_location = '\0';
	char		fdw_location = GP_FOREIGN_COORDINATOR;
	int			n = 0;
	int			server_n = 0;

	placement_of(table->options, &location, &n);
	placement_of(server->options, &server_location, &server_n);

	/* the wrapper's num_segments is the wrapper's own, as Cloudberry's is */
	placement_of(fdw->options, &fdw_location, NULL);

	if (location == '\0')
		location = server_location != '\0' ? server_location : fdw_location;
	if (n <= 0)
		n = server_n;

	/*
	 * All of them where none says; and no more than the cluster has: the
	 * rows are the server's, not the segments', so reading them on every
	 * segment there is loses none -- where Cloudberry takes a num_segments
	 * greater than its cluster's as it is, and fails the scan it plans over
	 * them.
	 */
	if (n <= 0 || n > GpClusterSegmentCount())
		n = GpClusterSegmentCount();
	*numsegments = n;
	return location;
}

/* ------------------------------------------------------------------------- */
/* The statements that set them                                              */
/* ------------------------------------------------------------------------- */

/* The object a statement sets the options of, and how they are kept. */
typedef struct ForeignObject
{
	Oid			catalog;		/* pg_foreign_data_wrapper, _server, _table */
	int			cacheid;		/* its syscache, by OID */
	AttrNumber	optionsattr;	/* its options */
	bool		is_create;
	bool		if_not_exists;
} ForeignObject;

bool
GpForeignSetsOptions(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateForeignTableStmt:
			return true;
		case T_AlterTableStmt:
			foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) parsetree)->cmds)
				if (cmd->subtype == AT_GenericOptions)
					return true;
			return false;
		default:
			return false;
	}
}

static ForeignObject
object_of(Node *parsetree)
{
	ForeignObject o = {0};

	switch (nodeTag(parsetree))
	{
		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
			o.catalog = ForeignDataWrapperRelationId;
			o.cacheid = FOREIGNDATAWRAPPEROID;
			o.optionsattr = Anum_pg_foreign_data_wrapper_fdwoptions;
			o.is_create = IsA(parsetree, CreateFdwStmt);
			break;
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
			o.catalog = ForeignServerRelationId;
			o.cacheid = FOREIGNSERVEROID;
			o.optionsattr = Anum_pg_foreign_server_srvoptions;
			o.is_create = IsA(parsetree, CreateForeignServerStmt);
			o.if_not_exists = o.is_create &&
				((CreateForeignServerStmt *) parsetree)->if_not_exists;
			break;
		default:				/* a foreign table's */
			o.catalog = ForeignTableRelationId;
			o.cacheid = FOREIGNTABLEREL;
			o.optionsattr = Anum_pg_foreign_table_ftoptions;
			o.is_create = IsA(parsetree, CreateForeignTableStmt);
			o.if_not_exists = o.is_create &&
				((CreateForeignTableStmt *) parsetree)->base.if_not_exists;
			break;
	}
	return o;
}

/* The object's OID, or InvalidOid where it is not there. */
static Oid
object_oid(Node *parsetree)
{
	Oid			relid;

	switch (nodeTag(parsetree))
	{
		case T_CreateFdwStmt:
			return get_foreign_data_wrapper_oid(((CreateFdwStmt *) parsetree)->fdwname, true);
		case T_AlterFdwStmt:
			return get_foreign_data_wrapper_oid(((AlterFdwStmt *) parsetree)->fdwname, true);
		case T_CreateForeignServerStmt:
			return get_foreign_server_oid(((CreateForeignServerStmt *) parsetree)->servername, true);
		case T_AlterForeignServerStmt:
			return get_foreign_server_oid(((AlterForeignServerStmt *) parsetree)->servername, true);
		case T_CreateForeignTableStmt:
			relid = RangeVarGetRelid(((CreateForeignTableStmt *) parsetree)->base.relation,
									 NoLock, true);
			break;
		default:
			relid = RangeVarGetRelid(((AlterTableStmt *) parsetree)->relation,
									 NoLock, true);
			break;
	}
	return OidIsValid(relid) && get_rel_relkind(relid) == RELKIND_FOREIGN_TABLE
		? relid : InvalidOid;
}

/*
 * The statement's options of the object, in order: a pointer to each list
 * that holds them, an ALTER TABLE's in each of its OPTIONS clauses.
 */
static List *
option_lists(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CreateFdwStmt:
			return list_make1(&((CreateFdwStmt *) parsetree)->options);
		case T_AlterFdwStmt:
			return list_make1(&((AlterFdwStmt *) parsetree)->options);
		case T_CreateForeignServerStmt:
			return list_make1(&((CreateForeignServerStmt *) parsetree)->options);
		case T_AlterForeignServerStmt:
			return list_make1(&((AlterForeignServerStmt *) parsetree)->options);
		case T_CreateForeignTableStmt:
			return list_make1(&((CreateForeignTableStmt *) parsetree)->options);
		default:
			{
				List	   *lists = NIL;

				foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) parsetree)->cmds)
					if (cmd->subtype == AT_GenericOptions)
						lists = lappend(lists, &cmd->def);
				return lists;
			}
	}
}

/* The object's options, NULL for none. */
static Datum
get_options(ForeignObject *o, Oid objid)
{
	HeapTuple	tuple = SearchSysCache1(o->cacheid, ObjectIdGetDatum(objid));
	Datum		datum;
	bool		isnull;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for object %u of catalog %u", objid, o->catalog);
	datum = SysCacheGetAttr(o->cacheid, tuple, o->optionsattr, &isnull);
	datum = isnull ? (Datum) 0 : datumCopy(datum, false, -1);
	ReleaseSysCache(tuple);
	return datum;
}

/* Write the object's options: an array, or NULL for none. */
static void
set_options(ForeignObject *o, Oid objid, Datum options)
{
	Relation	rel = table_open(o->catalog, RowExclusiveLock);
	HeapTuple	tuple = SearchSysCacheCopy1(o->cacheid, ObjectIdGetDatum(objid));
	int			attnum = o->optionsattr;
	bool		isnull = DatumGetPointer(options) == NULL;
	HeapTuple	newtuple;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for object %u of catalog %u", objid, o->catalog);
	newtuple = heap_modify_tuple_by_cols(tuple, RelationGetDescr(rel), 1,
										 &attnum, &options, &isnull);
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/* The options without mpp_execute and num_segments. */
static Datum
without_placement(ForeignObject *o, Datum options)
{
	List	   *kept = NIL;

	foreach_node(DefElem, def, untransformRelOptions(options))
		if (!is_placement(def))
			kept = lappend(kept, def);
	return transformGenericOptions(o->catalog, (Datum) 0, kept, InvalidOid);
}

static bool
has_placement(Datum options)
{
	foreach_node(DefElem, def, untransformRelOptions(options))
		if (is_placement(def))
			return true;
	return false;
}

/*
 * A server's mpp_execute and num_segments are its tables' where they have
 * none: their relcache entries go, and the plans that read them, as
 * Cloudberry's AlterForeignServer() sends them.
 */
static void
invalidate_server_tables(Oid serverid)
{
	Relation	rel = table_open(ForeignTableRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
	HeapTuple	tuple;

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_foreign_table ft = (Form_pg_foreign_table) GETSTRUCT(tuple);

		if (ft->ftserver == serverid)
			CacheInvalidateRelcacheByRelid(ft->ftrelid);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
}

void
GpForeignRunStatement(PlannedStmt *pstmt, bool readOnlyTree,
					  GpForeignRunFn run, void *arg)
{
	Node	   *parsetree = pstmt->utilityStmt;
	ForeignObject o = object_of(parsetree);
	List	   *options = NIL;
	bool		placing = false;
	Oid			objid = InvalidOid;
	Datum		result = (Datum) 0;
	PlannedStmt *stripped;
	ListCell   *lc;

	/*
	 * The statement's options, and among them mpp_execute and num_segments,
	 * checked as Cloudberry checks them: a user mapping has neither, which
	 * its wrapper's validator is given as it is, as Cloudberry gives it.
	 */
	foreach(lc, option_lists(parsetree))
		foreach_node(DefElem, def, *(List **) lfirst(lc))
		{
			options = lappend(options, def);
			if (!is_placement(def))
				continue;
			placing = true;
			if (IsA(parsetree, AlterFdwStmt) && strcmp(def->defname, MPP_EXECUTE) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"%s\" of foreign data wrapper is not allowed to be altered",
								def->defname)));
			if (def->defaction == DEFELEM_DROP)
				continue;
			if (strcmp(def->defname, MPP_EXECUTE) == 0)
				(void) exec_location_of(def);
			else
				(void) num_segments_of(def);
		}

	/*
	 * An ALTER of an object that has them runs with them out of its options,
	 * as a statement that names them does: the options it leaves, which
	 * PostgreSQL validates, are what they will be, less those two.
	 */
	if (!o.is_create)
	{
		objid = object_oid(parsetree);
		if (OidIsValid(objid))
		{
			Datum		old = get_options(&o, objid);

			if (placing || has_placement(old))
			{
				result = transformGenericOptions(o.catalog, old, options, InvalidOid);
				set_options(&o, objid, without_placement(&o, old));
				CommandCounterIncrement();
				placing = true;
			}
		}
		else
			placing = false;	/* PostgreSQL says it is not there */
	}
	else if (placing && o.if_not_exists && OidIsValid(object_oid(parsetree)))
		placing = false;		/* PostgreSQL says it is there, and skips it */

	if (!placing)
	{
		run(pstmt, readOnlyTree, arg);
		return;
	}

	stripped = copyObject(pstmt);
	foreach(lc, option_lists(stripped->utilityStmt))
	{
		List	  **list = (List **) lfirst(lc);
		List	   *kept = NIL;

		foreach_node(DefElem, def, *list)
			if (!is_placement(def))
				kept = lappend(kept, def);
		*list = kept;
	}
	run(stripped, false, arg);
	CommandCounterIncrement();

	if (o.is_create)
	{
		objid = object_oid(parsetree);
		if (!OidIsValid(objid))
			elog(ERROR, "gp_core: the object the statement made was not found");
		result = transformGenericOptions(o.catalog, (Datum) 0, options, InvalidOid);
	}
	set_options(&o, objid, result);
	if (o.catalog == ForeignServerRelationId)
		invalidate_server_tables(objid);
	CommandCounterIncrement();
}

/* ------------------------------------------------------------------------- */
/* One node                                                                  */
/* ------------------------------------------------------------------------- */

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* A statement's arguments, for run_next(). */
typedef struct ForeignRunArgs
{
	const char *queryString;
	ProcessUtilityContext context;
	ParamListInfo params;
	QueryEnvironment *queryEnv;
	DestReceiver *dest;
	QueryCompletion *qc;
} ForeignRunArgs;

static void
run_next(PlannedStmt *pstmt, bool readOnlyTree, void *arg)
{
	ForeignRunArgs *a = (ForeignRunArgs *) arg;

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, a->queryString, readOnlyTree, a->context,
							a->params, a->queryEnv, a->dest, a->qc);
	else
		standard_ProcessUtility(pstmt, a->queryString, readOnlyTree,
								a->context, a->params, a->queryEnv, a->dest,
								a->qc);
}

static void
foreign_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	ForeignRunArgs args = {queryString, context, params, queryEnv, dest, qc};

	if (GpForeignSetsOptions(pstmt->utilityStmt))
		GpForeignRunStatement(pstmt, readOnlyTree, run_next, &args);
	else
		run_next(pstmt, readOnlyTree, &args);
}

void
GpForeignInit(void)
{
	if (!GpClusterIsSingleNode())
		return;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = foreign_ProcessUtility;
}
