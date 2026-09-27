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
 * gp_partanalyze.c
 *	  ANALYZE of a partitioned table as Cloudberry does it: which of its
 *	  relations a statement takes, and in what order.
 *
 * PostgreSQL 19's ANALYZE of a partitioned table takes the table, every
 * partitioned table under it and every leaf, and gives each partitioned one
 * statistics of its own from a sample of its leaves; it takes a table's
 * inheritance children too, since PostgreSQL 18.  Cloudberry's takes what two
 * settings say (expand_vacuum_rel() and get_all_vacuum_rels(), vacuum.c):
 *
 *   - gp.optimizer_analyze_root_partition, on by default: a root partitioned
 *     table is analyzed as well as its leaves;
 *   - gp.optimizer_analyze_midlevel_partition, off by default: a partitioned
 *     table under another is analyzed too -- where it is off, one named in
 *     the statement is refused with a WARNING, leaves and all, VACUUM's too;
 *   - ROOTPARTITION, an option of ANALYZE, which O26 makes of Cloudberry's
 *     ANALYZE ROOTPARTITION t (gp_desugar.c): the root alone, whatever the
 *     setting says, and a WARNING for anything else named;
 *   - the leaves before the table they are in, which Cloudberry's merge of
 *     the leaves' statistics needs, and after a partition, the tables above
 *     it whose leaves all have statistics now;
 *   - and a table's inheritance children not at all, as in PostgreSQL 16.
 *
 * So a ProcessUtility hook makes that list of the statement's, each relation
 * by its OID -- which PostgreSQL's vacuum() takes as it is -- and hands the
 * statement on with it, outermost of gp_core's hooks, so that pg_stat_last_
 * operation (gp_metatrack.c) and the dispatch of VACUUM (gp_ddl.c) see the
 * list the statement runs.  A database-wide statement keeps no list of its
 * own unless there is something to leave out, so that a cluster's VACUUM
 * ANALYZE does not send the segments every table's OID.
 *
 * Where this applies: in a database that has gp_core -- every database a
 * cluster's coordinator makes, and one a single node's user made it in, as
 * Cloudberry's single node applies the settings too -- and not on a segment
 * running what the coordinator sent, whose list is made already.  Elsewhere
 * ANALYZE is PostgreSQL's, and PostgreSQL's own tests on a single node run
 * where gp_core is not made.
 *
 * An analyzed or vacuumed table with no rows says so with one page, as
 * Cloudberry's vac_update_relstats() writes it, so that "analyzed and empty"
 * and "never analyzed" can be told apart by its pages: on a cluster where
 * the segments' counts are brought back (gp_analyze.c), and on one node here.
 *
 * Cloudberry sources this file stands in for:
 *	  expand_vacuum_rel() and get_all_vacuum_rels() in
 *	  src/backend/commands/vacuum.c, leaf_parts_analyzed() in
 *	  src/backend/commands/analyzeutils.c, and the settings in
 *	  src/backend/utils/misc/guc_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_partanalyze.h"

bool		gp_optimizer_analyze_root_partition = true;
bool		gp_optimizer_analyze_midlevel_partition = false;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* What a VACUUM or ANALYZE statement asks for, as its options say. */
typedef struct VacuumAsk
{
	uint32		options;		/* VACOPT_VACUUM, _ANALYZE, _VERBOSE, _SKIP_LOCKED */
	bool		rootonly;		/* ROOTPARTITION */
	bool		fullscan;		/* FULLSCAN */
	List	   *other_options;	/* the rest, left for PostgreSQL */
} VacuumAsk;

/*
 * The statement this backend runs, with the list it was given: what O3's
 * hook and the merge of a root's statistics ask of it (GpPartAnalyzeTarget()).
 * NIL relations are every relation of the database.
 */
typedef struct AnalyzeStatement
{
	VacuumAsk	ask;
	List	   *rels;			/* of VacuumRelation, each by its OID */
	HTAB	   *byoid;			/* the same, by OID, once asked */
} AnalyzeStatement;

typedef struct AnalyzeTarget
{
	Oid			relid;			/* the hash key */
	List	   *va_cols;
} AnalyzeTarget;

static AnalyzeStatement *current = NULL;

bool
GpPartAnalyzeTarget(Oid relid, List **va_cols)
{
	AnalyzeTarget *target;

	*va_cols = NIL;
	if (current == NULL || !(current->ask.options & VACOPT_ANALYZE))
		return false;
	if (current->rels == NIL)
		return true;

	if (current->byoid == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(AnalyzeTarget);
		ctl.hcxt = GetMemoryChunkContext(current);
		current->byoid = hash_create("gp_core ANALYZE list", list_length(current->rels),
									 &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		foreach_node(VacuumRelation, vrel, current->rels)
		{
			bool		found;

			target = hash_search(current->byoid, &vrel->oid, HASH_ENTER, &found);
			if (!found)
				target->va_cols = vrel->va_cols;
		}
	}
	target = hash_search(current->byoid, &relid, HASH_FIND, NULL);
	if (target == NULL)
		return false;
	*va_cols = target->va_cols;
	return true;
}

bool
GpPartAnalyzeVerbose(void)
{
	return current != NULL && (current->ask.options & VACOPT_VERBOSE) != 0;
}

bool
GpPartAnalyzeFullscan(void)
{
	return current != NULL && current->ask.fullscan;
}

bool
GpPartAnalyzeActive(void)
{
	return GpClusterBackendRole() != GP_ROLE_EXECUTE &&
		!IsBinaryUpgrade &&
		OidIsValid(get_extension_oid("gp_core", true));
}

/* ------------------------------------------------------------------------- */
/* Which leaves have statistics                                              */
/* ------------------------------------------------------------------------- */

/* A relation's rows and pages, as pg_class counts them. */
static void
rel_counts(Oid relid, float4 *reltuples, int32 *relpages)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	*reltuples = 0;
	*relpages = 0;
	if (HeapTupleIsValid(tp))
	{
		*reltuples = ((Form_pg_class) GETSTRUCT(tp))->reltuples;
		*relpages = ((Form_pg_class) GETSTRUCT(tp))->relpages;
		ReleaseSysCache(tp);
	}
}

/* The attribute of a leaf a column of its parent is, by name. */
static AttrNumber
leaf_attnum(Oid leaf, const char *attname)
{
	HeapTuple	tp = SearchSysCacheAttName(leaf, attname);
	AttrNumber	result = InvalidAttrNumber;

	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_attribute) GETSTRUCT(tp))->attnum;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * Has the leaf a statistics row of its own for that attribute?  Read from
 * the catalog rather than the cache, as Cloudberry's fetch_leaf_att_stats()
 * reads it: a check of every leaf of a table of thousands would otherwise
 * fill the cache with rows nothing reads again.
 */
static bool
leaf_has_stats(Oid leaf, AttrNumber attnum)
{
	Relation	rel;
	ScanKeyData key[3];
	SysScanDesc scan;
	bool		found;

	if (attnum == InvalidAttrNumber)
		return false;
	rel = table_open(StatisticRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_statistic_starelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(leaf));
	ScanKeyInit(&key[1], Anum_pg_statistic_staattnum, BTEqualStrategyNumber,
				F_INT2EQ, Int16GetDatum(attnum));
	ScanKeyInit(&key[2], Anum_pg_statistic_stainherit, BTEqualStrategyNumber,
				F_BOOLEQ, BoolGetDatum(false));
	scan = systable_beginscan(rel, StatisticRelidAttnumInhIndexId, true,
							  NULL, 3, key);
	found = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/*
 * Have all the leaves of "parent" statistics for these columns, so that its
 * own can be merged from theirs?  Cloudberry's leaf_parts_analyzed():
 *
 *   - every leaf but "exclude" -- the partition a statement is about to
 *     analyze -- has been analyzed: it has rows counted, or no rows and a
 *     page, which is how an analyzed empty one is told from one never
 *     analyzed (see the file's header);
 *   - every one of them with rows has a statistics row for each column,
 *     found by the column's name, which a leaf may have at another number;
 *   - and not all of them are empty.
 *
 * NIL for the columns means each column of the parent that is analyzed.
 * Where not, it says why at elevel, in Cloudberry's words.
 */
bool
GpLeafPartsAnalyzed(Oid parent, Oid exclude, List *va_cols, int elevel)
{
	List	   *leaves;
	bool		all_empty = true;

	if (va_cols == NIL)
	{
		Relation	rel = table_open(parent, AccessShareLock);
		TupleDesc	tupdesc = RelationGetDescr(rel);

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (!attribute_is_analyzable(rel, att->attnum, att, NULL))
				continue;
			va_cols = lappend(va_cols, makeString(pstrdup(NameStr(att->attname))));
		}
		table_close(rel, NoLock);
	}

	/*
	 * First that each leaf is analyzed, from pg_class alone, so that a table
	 * one of whose leaves never was costs no look at the others' statistics.
	 */
	leaves = find_all_inheritors(parent, NoLock, NULL);
	foreach_oid(leaf, leaves)
	{
		float4		reltuples;
		int32		relpages;

		if (leaf == exclude || get_rel_relkind(leaf) == RELKIND_PARTITIONED_TABLE)
			continue;
		rel_counts(leaf, &reltuples, &relpages);
		if (reltuples < 0 || (reltuples == 0 && relpages == 0))
		{
			if (!OidIsValid(exclude))
				ereport(elevel,
						(errmsg("partition %s is not analyzed, so ANALYZE will collect sample for stats calculation",
								get_rel_name(leaf))));
			else
				ereport(elevel,
						(errmsg("auto merging of leaf partition stats to calculate root partition stats is not possible because partition %s is not analyzed",
								get_rel_name(leaf))));
			return false;
		}
	}

	foreach_oid(leaf, leaves)
	{
		float4		reltuples;
		int32		relpages;

		if (leaf == exclude || get_rel_relkind(leaf) == RELKIND_PARTITIONED_TABLE)
			continue;

		/* an analyzed leaf with no rows has nothing to say */
		rel_counts(leaf, &reltuples, &relpages);
		if (reltuples == 0)
			continue;
		all_empty = false;

		foreach_node(String, col, va_cols)
		{
			const char *attname = strVal(col);

			if (leaf_has_stats(leaf, leaf_attnum(leaf, attname)))
				continue;
			if (!OidIsValid(exclude))
				ereport(elevel,
						(errmsg("column %s of partition %s is not analyzed, so ANALYZE will collect sample for stats calculation",
								attname, get_rel_name(leaf))));
			else
				ereport(elevel,
						(errmsg("auto merging of leaf partition stats to calculate root partition stats is not possible because column %s of partition %s is not analyzed",
								attname, get_rel_name(leaf))));
			return false;
		}
	}

	return !all_empty;
}

/* ------------------------------------------------------------------------- */
/* The relations a statement takes                                           */
/* ------------------------------------------------------------------------- */

/*
 * The relations a relation named in the statement stands for, in the order
 * they are to be processed -- Cloudberry's expand_vacuum_rel(), with
 * PostgreSQL 19's ONLY taken as "not its partitions".
 */
static List *
expand_named(VacuumRelation *vrel, const VacuumAsk *ask)
{
	List	   *vacrels = NIL;
	List	   *reversed = NIL;
	Oid			relid;
	HeapTuple	tuple;
	Form_pg_class classForm;
	bool		partitioned;
	bool		ispartition;
	bool		permitted;
	bool		skip_this = false;
	bool		skip_children = false;
	bool		skip_midlevel = false;

	/* A relation given by its OID is taken as it is, as vacuum() takes it. */
	if (OidIsValid(vrel->oid))
		return list_make1(vrel);

	/*
	 * The lookup PostgreSQL's expand_vacuum_rel() makes, with its lock
	 * taken for a moment and its WARNING where SKIP_LOCKED found it taken.
	 */
	relid = RangeVarGetRelidExtended(vrel->relation, AccessShareLock,
									 (ask->options & VACOPT_SKIP_LOCKED) ?
									 RVR_SKIP_LOCKED : 0,
									 NULL, NULL);
	if (!OidIsValid(relid))
	{
		if (ask->options & VACOPT_VACUUM)
			ereport(WARNING,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping vacuum of \"%s\" --- lock not available",
							vrel->relation->relname)));
		else
			ereport(WARNING,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("skipping analyze of \"%s\" --- lock not available",
							vrel->relation->relname)));
		return NIL;
	}

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	partitioned = classForm->relkind == RELKIND_PARTITIONED_TABLE;
	ispartition = classForm->relispartition;

	if (ask->rootonly)
	{
		/* ROOTPARTITION: the root alone, and a WARNING for anything else */
		if (!partitioned || ispartition)
		{
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze a non-root partition using ANALYZE ROOTPARTITION",
							NameStr(classForm->relname))));
			skip_this = true;
		}
		skip_children = true;
	}
	else if (partitioned && ispartition && !gp_optimizer_analyze_midlevel_partition)
	{
		/*
		 * A mid-level partitioned table is refused, its leaves too: its
		 * statistics are ORCA's to read, which reads the root's, and the
		 * planner reads the leaves'.
		 */
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot analyze a mid-level partition. "
						"Please run ANALYZE on the root partition table.",
						NameStr(classForm->relname))));
		skip_this = true;
		skip_children = true;
	}
	else
	{
		if (partitioned && !gp_optimizer_analyze_root_partition)
			skip_this = true;
		if (!gp_optimizer_analyze_midlevel_partition)
			skip_midlevel = true;
	}

	/* its WARNING even where the relation is skipped, as Cloudberry's is */
	permitted = vacuum_is_permitted_for_relation(relid, classForm, ask->options);
	if (permitted && !skip_this)
		vacrels = lappend(vacrels, makeVacuumRelation(vrel->relation, relid,
													  vrel->va_cols));

	if ((ask->options & VACOPT_VACUUM) && partitioned && !vrel->relation->inh)
		ereport(WARNING,
				(errmsg("VACUUM ONLY of partitioned table \"%s\" has no effect",
						vrel->relation->relname)));
	ReleaseSysCache(tuple);

	/* A partitioned table's partitions, but never a table's children. */
	if (partitioned && vrel->relation->inh && !skip_children)
	{
		foreach_oid(part, find_all_inheritors(relid, NoLock, NULL))
		{
			if (part == relid)
				continue;
			if (skip_midlevel && get_rel_relkind(part) == RELKIND_PARTITIONED_TABLE)
				continue;
			vacrels = lappend(vacrels, makeVacuumRelation(NULL, part,
														  vrel->va_cols));
		}
	}
	UnlockRelationOid(relid, AccessShareLock);

	/* The partitions before the table they are in. */
	foreach_ptr(VacuumRelation, v, vacrels)
		reversed = lcons(v, reversed);

	/*
	 * And after a partition ANALYZE takes, each table above it whose leaves
	 * all have statistics once it has been analyzed, up to the root: the
	 * root always, a mid-level one where the setting says so.  Cloudberry
	 * says why it stopped at LOG where the statement is VERBOSE.
	 */
	if ((ask->options & VACOPT_ANALYZE) && gp_optimizer_analyze_root_partition &&
		!skip_this)
	{
		Oid			child = relid;
		int			elevel = (ask->options & VACOPT_VERBOSE) ? LOG : DEBUG2;

		while (ispartition)
		{
			Oid			parent = get_partition_parent(child, false);

			ispartition = get_rel_relispartition(parent);
			if (!GpLeafPartsAnalyzed(parent, child, vrel->va_cols, elevel))
				break;
			if (!ispartition || gp_optimizer_analyze_midlevel_partition)
				reversed = lappend(reversed, makeVacuumRelation(vrel->relation,
																parent,
																vrel->va_cols));
			child = parent;
		}
	}

	return reversed;
}

/*
 * Is a partitioned table one a statement that names no relation leaves
 * out: a mid-level one where its setting says so, and a root where its
 * setting does and ROOTPARTITION does not ask for it?
 */
static bool
left_out(Form_pg_class classForm, const VacuumAsk *ask)
{
	if (classForm->relkind != RELKIND_PARTITIONED_TABLE)
		return false;
	if (classForm->relispartition)
		return !gp_optimizer_analyze_midlevel_partition;
	return !gp_optimizer_analyze_root_partition && !ask->rootonly;
}

/*
 * The relations of a statement that names none -- Cloudberry's
 * get_all_vacuum_rels(): PostgreSQL's, less those left out above.  With
 * "list" false only whether any is left out, and nothing checked; with it,
 * the list, each relation checked as PostgreSQL checks it, WARNING and all.
 */
static List *
all_relations(const VacuumAsk *ask, bool list, bool *any_left_out)
{
	List	   *vacrels = NIL;
	Relation	pgclass;
	TableScanDesc scan;
	HeapTuple	tuple;

	*any_left_out = false;
	pgclass = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(pgclass, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);

		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_PARTITIONED_TABLE)
			continue;
		if (classForm->relpersistence == RELPERSISTENCE_TEMP &&
			!isTempOrTempToastNamespace(classForm->relnamespace))
			continue;
		if (left_out(classForm, ask))
		{
			*any_left_out = true;
			if (!list)
				break;
			continue;
		}
		if (list && vacuum_is_permitted_for_relation(classForm->oid, classForm,
													 ask->options))
			vacrels = lappend(vacrels, makeVacuumRelation(NULL, classForm->oid, NIL));
	}
	table_endscan(scan);
	table_close(pgclass, AccessShareLock);
	return vacrels;
}

/* What the statement's options say, and the options PostgreSQL is to see. */
static void
read_options(VacuumStmt *stmt, VacuumAsk *ask)
{
	bool		analyze = !stmt->is_vacuumcmd;

	ask->options = stmt->is_vacuumcmd ? VACOPT_VACUUM : VACOPT_ANALYZE;
	ask->rootonly = false;
	ask->fullscan = false;
	ask->other_options = NIL;

	foreach_node(DefElem, opt, stmt->options)
	{
		/*
		 * Cloudberry's two options of ANALYZE come out; given to VACUUM they
		 * are left for PostgreSQL, which refuses them, as Cloudberry would
		 * have them on no VACUUM.
		 */
		if (!stmt->is_vacuumcmd && strcmp(opt->defname, "rootpartition") == 0)
		{
			ask->rootonly = defGetBoolean(opt);
			continue;
		}
		if (!stmt->is_vacuumcmd && strcmp(opt->defname, "fullscan") == 0)
		{
			ask->fullscan = defGetBoolean(opt);
			continue;
		}

		if (strcmp(opt->defname, "verbose") == 0 && defGetBoolean(opt))
			ask->options |= VACOPT_VERBOSE;
		else if (strcmp(opt->defname, "skip_locked") == 0 && defGetBoolean(opt))
			ask->options |= VACOPT_SKIP_LOCKED;
		else if (stmt->is_vacuumcmd && strcmp(opt->defname, "analyze") == 0)
			analyze = defGetBoolean(opt);
		ask->other_options = lappend(ask->other_options, opt);
	}
	if (analyze)
		ask->options |= VACOPT_ANALYZE;
}

/* Does a VACUUM statement touch no relation: ONLY_DATABASE_STATS? */
static bool
database_stats_only(VacuumStmt *stmt)
{
	foreach_node(DefElem, opt, stmt->options)
	{
		if (strcmp(opt->defname, "only_database_stats") == 0 && defGetBoolean(opt))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* An empty table's page                                                     */
/* ------------------------------------------------------------------------- */

/*
 * After a VACUUM or ANALYZE on one node: each table it took that has no rows
 * and no page is given one, as Cloudberry's vac_update_relstats() gives it
 * (vacuum.c).  A cluster's coordinator does it where it brings the
 * segments' counts back (GpAnalyzeSegmentCounts()).  Written in place, as
 * vac_update_relstats() writes, under the lock VACUUM and ANALYZE take,
 * table by table.
 */
static void
empty_tables_one_page(List *vacrels)
{
	foreach_node(VacuumRelation, vrel, vacrels)
	{
		Relation	rel;

		if (get_rel_relkind(vrel->oid) != RELKIND_RELATION &&
			get_rel_relkind(vrel->oid) != RELKIND_MATVIEW)
			continue;
		if (!ConditionalLockRelationOid(vrel->oid, ShareUpdateExclusiveLock))
			continue;
		rel = try_relation_open(vrel->oid, NoLock);
		if (rel == NULL)
		{
			UnlockRelationOid(vrel->oid, ShareUpdateExclusiveLock);
			continue;
		}
		if (rel->rd_rel->relpages == 0 && rel->rd_rel->reltuples == 0)
			vac_update_relstats(rel, 1, 0, rel->rd_rel->relallvisible,
								rel->rd_rel->relallfrozen,
								rel->rd_rel->relhasindex,
								InvalidTransactionId, InvalidMultiXactId,
								NULL, NULL, true);
		relation_close(rel, NoLock);
	}
}

/* ------------------------------------------------------------------------- */
/* The hook                                                                  */
/* ------------------------------------------------------------------------- */

static void
next_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree, ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/*
 * A VACUUM or ANALYZE statement, with the relations Cloudberry's rules give
 * it, each by its OID, and without Cloudberry's options of ANALYZE, which
 * PostgreSQL's ExecVacuum() would refuse.
 */
static void
partanalyze_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   bool readOnlyTree, ProcessUtilityContext context,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   DestReceiver *dest, QueryCompletion *qc)
{
	VacuumStmt *stmt;
	VacuumStmt *newstmt;
	PlannedStmt *newpstmt;
	VacuumAsk	ask;
	List	   *vacrels = NIL;
	bool		single = GpClusterIsSingleNode();
	AnalyzeStatement *statement;
	AnalyzeStatement *outer;

	if (!IsA(pstmt->utilityStmt, VacuumStmt) ||
		GpDispatchIsDispatchedStatement(pstmt->utilityStmt) ||
		database_stats_only((VacuumStmt *) pstmt->utilityStmt) ||
		!GpPartAnalyzeActive())
	{
		next_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
		return;
	}

	stmt = (VacuumStmt *) pstmt->utilityStmt;
	read_options(stmt, &ask);

	if (stmt->rels != NIL)
	{
		foreach_node(VacuumRelation, vrel, stmt->rels)
			vacrels = list_concat(vacrels, expand_named(vrel, &ask));

		/* everything named was refused, with its WARNING: nothing to run */
		if (vacrels == NIL)
			return;
	}
	else
	{
		bool		any_left_out;

		/*
		 * The whole database: listed where ANALYZE leaves something out, and
		 * on one node, whose tables are needed afterwards and to which
		 * nothing is sent; a cluster's VACUUM ANALYZE of everything is sent
		 * to the segments as it was written.
		 */
		if (ask.options & VACOPT_ANALYZE)
			(void) all_relations(&ask, false, &any_left_out);
		else
			any_left_out = false;
		if (any_left_out || single)
		{
			vacrels = all_relations(&ask, true, &any_left_out);
			if (vacrels == NIL)
				return;
		}
	}

	newstmt = makeNode(VacuumStmt);
	newstmt->options = ask.other_options;
	newstmt->rels = vacrels != NIL ? vacrels : stmt->rels;
	newstmt->is_vacuumcmd = stmt->is_vacuumcmd;
	newpstmt = copyObject(pstmt);
	newpstmt->utilityStmt = (Node *) newstmt;

	/*
	 * Run with its list known to what samples and merges -- a statement's
	 * own, outside which one run by a function's ANALYZE had none -- and
	 * where it ran in the caller's transaction, what was to be written over
	 * PostgreSQL's statistics written as it ends (gp_partmerge.c).
	 */
	statement = palloc0_object(AnalyzeStatement);
	statement->ask = ask;
	statement->rels = newstmt->rels;
	outer = current;
	current = statement;
	PG_TRY();
	{
		next_ProcessUtility(newpstmt, queryString, false, context,
							params, queryEnv, dest, qc);
		GpPartMergeFinish();
	}
	PG_FINALLY();
	{
		current = outer;
	}
	PG_END_TRY();

	if (single)
		empty_tables_one_page(newstmt->rels);
}

void
GpPartAnalyzeInit(void)
{
	DefineCustomBoolVariable("gp.optimizer_analyze_root_partition",
							 "Enable statistics collection on root partitions during ANALYZE",
							 "ANALYZE of a partitioned table takes the root as well as its leaves, and ANALYZE of a partition the root above it once every leaf has statistics. Cloudberry calls this optimizer_analyze_root_partition.",
							 &gp_optimizer_analyze_root_partition,
							 true, PGC_USERSET, 0,
							 NULL, NULL, NULL);

	/* Cloudberry's is also GUC_NO_SHOW_ALL, which would hide it from the harnesses. */
	DefineCustomBoolVariable("gp.optimizer_analyze_midlevel_partition",
							 "Enable statistics collection on intermediate partitions during ANALYZE",
							 "ANALYZE takes a partitioned table under another, which it otherwise refuses when named and leaves out when not. Cloudberry calls this optimizer_analyze_midlevel_partition.",
							 &gp_optimizer_analyze_midlevel_partition,
							 false, PGC_USERSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = partanalyze_ProcessUtility;

	/* the merge of a root's statistics, and what it writes after PostgreSQL */
	GpPartMergeInit();
}
