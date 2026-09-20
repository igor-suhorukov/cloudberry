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
 * compat/plancat.c
 *	  What Cloudberry adds to PostgreSQL's plancat.c for ORCA.
 *
 * Ported from github/cloudberry/src/backend/optimizer/util/plancat.c, which
 * is a PostgreSQL file Cloudberry modified and the port therefore does not
 * build.  As in compat/lsyscache.c, each function keeps Cloudberry's name
 * and signature so that the translator calls them as ORCA has always called
 * them, and the comments record what had to change for PostgreSQL 19.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "nodes/pathnodes.h"
#include "optimizer/plancat.h"
#include "statistics/statistics.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "cb_plancat.h"

/*
 * See cb_plancat.h.  Off by default, as in Cloudberry
 * (github/cloudberry/src/backend/utils/misc/guc_gp.c:287); gp_orca's
 * _PG_init registers it as gp.enable_relsize_collection, because PostgreSQL
 * 19 requires a custom setting's name to hold a dot.
 */
bool		gp_enable_relsize_collection = false;

static void get_relation_statistics_worker(List **stainfos, Oid statOid,
										   bool inh, Bitmapset *keys);

/*
 * get_relation_statistics_worker
 *		One statistics object, one value of stxdinherit: append a
 *		StatisticExtInfo for every kind actually built.
 *
 * PostgreSQL's own copy of this is static in plancat.c and takes a RelOptInfo
 * and an expression list besides.  Neither is wanted here; see
 * GetRelationExtStatistics below for why.
 */
static void
get_relation_statistics_worker(List **stainfos, Oid statOid, bool inh,
							   Bitmapset *keys)
{
	Form_pg_statistic_ext_data dataForm;
	HeapTuple	dtup;
	static const char kinds[] = {
		STATS_EXT_NDISTINCT,
		STATS_EXT_DEPENDENCIES,
		STATS_EXT_MCV,
		STATS_EXT_EXPRESSIONS,
	};

	dtup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(statOid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(dtup))
		return;

	dataForm = (Form_pg_statistic_ext_data) GETSTRUCT(dtup);

	/*
	 * PostgreSQL writes the four kinds out as four near-identical blocks.
	 * A loop says the same thing, and says it once, which matters here
	 * because a fifth kind added upstream should need a line rather than a
	 * block.
	 */
	for (int i = 0; i < (int) lengthof(kinds); i++)
	{
		StatisticExtInfo *info;

		if (!statext_is_kind_built(dtup, kinds[i]))
			continue;

		info = makeNode(StatisticExtInfo);
		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = NULL;
		info->kind = kinds[i];
		info->keys = bms_copy(keys);
		info->exprs = NIL;

		*stainfos = lappend(*stainfos, info);
	}

	ReleaseSysCache(dtup);
}

/*
 * GetRelationExtStatistics
 *		The extended statistics objects defined on a relation.
 *
 * Cloudberry reaches this by widening PostgreSQL's static
 * get_relation_statistics() to accept a NULL RelOptInfo and calling it with
 * one (plancat.c:1723).  The port cannot widen a function in a file it does
 * not build, so this is that function with the RelOptInfo taken out rather
 * than made optional -- ORCA has no RelOptInfo to give, at this point or
 * ever, because it builds its own relation metadata and never PostgreSQL's.
 *
 * TWO THINGS ARE LEFT OUT, and both follow from the RelOptInfo being absent
 * rather than from a decision about what ORCA deserves.
 *
 * info->rel is NULL, as it is in Cloudberry.
 *
 * info->exprs is NIL, where Cloudberry runs the object's expressions through
 * stringToNode, expand_generated_columns_in_expr, ChangeVarNodes,
 * eval_const_expressions and fix_opfuncids.  The reason is that the varno
 * those expressions would carry is the RelOptInfo's, and there is none:
 * Cloudberry's widened function computes `varno = rel ? rel->relid : 0` and
 * then rewrites varno 1 to varno 0, which is not a range table index at all.
 * So the expressions it produces for ORCA do not refer to anything, and the
 * one consumer -- CTranslatorRelcacheToDXL::RetrieveExtStatsInfo -- reads
 * only statOid, kind and keys, and skips STATS_EXT_EXPRESSIONS objects
 * outright ("CBDB_MERGE_FIXME: support expr ext stats in the feature").
 * Producing them would be work whose result is unreadable and unread.  When
 * ORCA learns to use expression statistics it will need them built against
 * a range table it chooses, which is a question for then.
 *
 * Both stxdinherit values are reported, as in Cloudberry, so a partitioned
 * table analyzed both ways contributes two entries per kind that differ only
 * in a field ORCA does not read.  That is Cloudberry's behaviour and the
 * port keeps it; deduplicating would change which statistics ORCA sees.
 */
List *
GetRelationExtStatistics(Relation relation)
{
	List	   *statoidlist;
	List	   *stainfos = NIL;
	ListCell   *l;

	statoidlist = RelationGetStatExtList(relation);

	foreach(l, statoidlist)
	{
		Oid			statOid = lfirst_oid(l);
		Form_pg_statistic_ext staForm;
		HeapTuple	htup;
		Bitmapset  *keys = NULL;

		htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statOid));
		if (!HeapTupleIsValid(htup))
			elog(ERROR, "cache lookup failed for statistics object %u", statOid);
		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);

		for (int i = 0; i < staForm->stxkeys.dim1; i++)
			keys = bms_add_member(keys, staForm->stxkeys.values[i]);

		get_relation_statistics_worker(&stainfos, statOid, true, keys);
		get_relation_statistics_worker(&stainfos, statOid, false, keys);

		ReleaseSysCache(htup);
		bms_free(keys);
	}

	list_free(statoidlist);

	return stainfos;
}

/*
 * GetExtStatisticsName
 *		The name of one extended statistics object, palloc'd.
 *
 * A DEFECT FOUND AND NOT CARRIED OVER.  Cloudberry's copy releases the
 * syscache entry and then reads through the pointer it took out of it:
 *
 *		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);
 *		ReleaseSysCache(htup);
 *		return NameStr(staForm->stxname);
 *
 * (plancat.c:1658-1670.)  Once released, the entry may be evicted and its
 * memory reused, so the caller gets whatever is there by then.  It has gone
 * unnoticed because the caller -- RetrieveExtStatsInfo -- copies the string
 * into ORCA's memory pool on the next line, and nothing else runs in
 * between.  The port copies before releasing, which is what the rest of
 * PostgreSQL does and what makes the returned pointer the caller's.
 *
 * This is the second defect of this shape the compat layer has turned up;
 * the first was get_cast_func returning without writing *pathtype.  Both
 * were found by reading the code rather than by running it, which is the
 * argument for re-implementing these functions instead of lifting them.
 */
char *
GetExtStatisticsName(Oid statOid)
{
	Form_pg_statistic_ext staForm;
	HeapTuple	htup;
	char	   *result;

	htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statOid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", statOid);

	staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);
	result = pstrdup(NameStr(staForm->stxname));
	ReleaseSysCache(htup);

	return result;
}

/*
 * GetExtStatisticsKinds
 *		The kinds an extended statistics object was asked to hold, as a list
 *		of the single characters pg_statistic_ext.stxkind stores.
 *
 * Asked for, not built: stxkind records what CREATE STATISTICS requested,
 * and whether ANALYZE has since produced anything is pg_statistic_ext_data's
 * business.  ORCA reads this when it fetches the statistics themselves, and
 * copes with a kind that has no data.
 */
List *
GetExtStatisticsKinds(Oid statOid)
{
	HeapTuple	htup;
	Datum		datum;
	bool		isnull;
	ArrayType  *arr;
	char	   *enabled;
	List	   *types = NIL;

	htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statOid));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", statOid);

	datum = SysCacheGetAttr(STATEXTOID, htup,
							Anum_pg_statistic_ext_stxkind, &isnull);

	/*
	 * Cloudberry does not test isnull and hands the null datum to
	 * DatumGetArrayTypeP, which would dereference 0.  stxkind is NOT NULL in
	 * the catalog, so the case is unreachable today; saying so out loud
	 * costs a branch and stops it being a crash if that ever changes.
	 */
	if (isnull)
		elog(ERROR, "stxkind is null for statistics object %u", statOid);

	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a 1-D char array");

	enabled = (char *) ARR_DATA_PTR(arr);
	for (int i = 0; i < ARR_DIMS(arr)[0]; i++)
		types = lappend_int(types, (int) enabled[i]);

	ReleaseSysCache(htup);

	return types;
}

/*
 * cdb_estimate_partitioned_numtuples
 *		How many rows a partitioned table holds, summed over its leaves.
 *
 * PostgreSQL's planner never asks this, because it plans each partition
 * separately and adds the costs at the Append.  ORCA costs the partitioned
 * table as a single object and asks before it has looked at a leaf.
 *
 * THE LEAVES ARE NOT LOCKED.  Cloudberry passes NoLock to
 * find_all_inheritors and says why: locking every leaf here would block
 * concurrent transactions on all of them for the whole of planning, and the
 * partitions that end up being scanned are locked when the plan is written.
 * The cost is that a partition can be dropped between this loop and that
 * lock, which is why RelationIdGetRelation returning NULL is a continue
 * rather than an error.
 *
 * A DEFECT FOUND AND NOT CARRIED OVER.  Since PostgreSQL 14 an unanalyzed
 * relation has reltuples = -1, meaning "unknown", where it used to have 0.
 * Cloudberry adds that -1 into the total (plancat.c:791), so each leaf that
 * has never been analyzed makes the table one row *smaller*, and a
 * partitioned table whose leaves are all fresh reports a negative row count
 * to the optimizer.  Unknown contributes nothing to a sum, so the port adds
 * nothing.  gp_enable_relsize_collection is what turns "unknown" into a real
 * number, and it is off by default, so the reachable case is the common one.
 */
double
cdb_estimate_partitioned_numtuples(Relation rel)
{
	List	   *inheritors;
	ListCell   *lc;
	double		totaltuples;

	if (rel->rd_rel->reltuples > 0)
		return rel->rd_rel->reltuples;

	inheritors = find_all_inheritors(RelationGetRelid(rel), NoLock, NULL);
	totaltuples = 0;

	foreach(lc, inheritors)
	{
		Oid			childid = lfirst_oid(lc);
		Relation	childrel;
		double		childtuples;

		if (childid != RelationGetRelid(rel))
		{
			childrel = RelationIdGetRelation(childid);
			if (childrel == NULL)
				continue;		/* dropped since find_all_inheritors */
		}
		else
			childrel = rel;

		childtuples = childrel->rd_rel->reltuples;

		if (gp_enable_relsize_collection && childtuples < 0)
		{
			BlockNumber numpages;
			double		allvisfrac;

			/*
			 * "Go and ask how big it really is."  On a cluster Cloudberry
			 * dispatches cdbRelMaxSegSize() to the segments, because the
			 * coordinator holds none of the rows.  On one node this backend
			 * holds all of them, so reading the relation is the same answer,
			 * and that is what estimate_rel_size() does when pg_class has
			 * nothing to offer.  M2 is where this becomes a dispatch, and
			 * where the distribution policy decides which of the two it is;
			 * relation_policy() in compat/lsyscache.c already reports one.
			 */
			estimate_rel_size(childrel, NULL, &numpages, &childtuples,
							  &allvisfrac);
		}

		if (childtuples > 0)
			totaltuples += childtuples;

		if (childrel != rel)
			RelationClose(childrel);
	}

	list_free(inheritors);

	return totaltuples;
}

/*
 * cdb_estimate_partitioned_numpages
 *		The pages and all-visible pages of a partitioned table, summed over
 *		its leaves.
 *
 * The counterpart of the above, and it needs no estimate: relpages and
 * relallvisible are never negative, so a leaf that has never been analyzed
 * contributes its honest zero.
 */
PageEstimate
cdb_estimate_partitioned_numpages(Relation rel)
{
	List	   *inheritors;
	ListCell   *lc;
	PageEstimate estimate = {
		.totalpages = rel->rd_rel->relpages >= 0 ?
			(BlockNumber) rel->rd_rel->relpages : 0,
		.totalallvisiblepages = rel->rd_rel->relallvisible,
	};

	if (estimate.totalpages > 0)
		return estimate;

	/* NoLock, for the reason given above. */
	inheritors = find_all_inheritors(RelationGetRelid(rel), NoLock, NULL);

	foreach(lc, inheritors)
	{
		Oid			childid = lfirst_oid(lc);
		Relation	childrel;

		if (childid != RelationGetRelid(rel))
		{
			childrel = RelationIdGetRelation(childid);
			if (childrel == NULL)
				continue;
		}
		else
			childrel = rel;

		if (childrel->rd_rel->relpages > 0)
			estimate.totalpages += (BlockNumber) childrel->rd_rel->relpages;
		estimate.totalallvisiblepages += childrel->rd_rel->relallvisible;

		if (childrel != rel)
			RelationClose(childrel);
	}

	list_free(inheritors);

	return estimate;
}
