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
 * compat/foreign.c
 *	  BuildForeignScan(), for a caller that is not the planner.
 *
 * A foreign-data wrapper plans its own scan: it estimates the relation,
 * offers paths, and turns the one chosen into a ForeignScan whose private
 * data its executor callbacks read -- postgres_fdw's remote SQL, file_fdw's
 * options.  ORCA chooses the scan itself, and still has to let the wrapper
 * build it.  Cloudberry's BuildForeignScan
 * (github/cloudberry/src/backend/foreign/foreign.c) does that with a
 * PlannerInfo holding little more than the query and the entry, marked
 * is_from_orca, a field Cloudberry adds; and Cloudberry's clausesel.c
 * returns selectivity 1 for such a PlannerInfo rather than follow the
 * pointers it does not have.  PostgreSQL 19 has neither patch.
 *
 * So the port's PlannerInfo is one a wrapper can plan with, as far as a scan
 * of one relation reaches: a PlannerGlobal with the path strategy mask
 * build_simple_rel() copies (PostgreSQL 19 added it), the range table and
 * relation arrays for the one entry, and the relation's own RelOptInfo, as
 * build_simple_rel() makes it for the planner.  Selectivity is then
 * estimated as the planner estimates it, which costs nothing ORCA's plan
 * depends on: the wrapper's estimates are thrown away with its path.
 *
 * What is Cloudberry's: the order of the calls, taking the wrapper's
 * GetForeignPlan() as the way to fill fdw_private, and fsSystemCol.  What is
 * not: the path is the cheapest unparameterized one, as set_cheapest() picks
 * it, not the first the wrapper offered; and the fields
 * create_foreignscan_plan() fills after the wrapper -- checkAsUser, and the
 * relation sets PostgreSQL 16 added, which postgres_fdw reads at execution
 * to find its table.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "foreign/fdwapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"

#include "cb_foreign.h"

/*
 * The path strategies the planner would allow, from the same settings;
 * standard_planner() computes it the same way (planner.c).
 */
static uint64
default_pgs_mask(void)
{
	uint64		mask;

	mask = PGS_APPEND | PGS_MERGE_APPEND | PGS_FOREIGNJOIN |
		PGS_GATHER | PGS_CONSIDER_NONPARTIAL;
	if (enable_tidscan)
		mask |= PGS_TIDSCAN;
	if (enable_seqscan)
		mask |= PGS_SEQSCAN;
	if (enable_indexscan)
		mask |= PGS_INDEXSCAN | PGS_INDEXONLYSCAN;
	if (enable_indexonlyscan)
		mask |= PGS_CONSIDER_INDEXONLY;
	if (enable_bitmapscan)
		mask |= PGS_BITMAPSCAN;
	if (enable_mergejoin)
	{
		mask |= PGS_MERGEJOIN_PLAIN;
		if (enable_material)
			mask |= PGS_MERGEJOIN_MATERIALIZE;
	}
	if (enable_nestloop)
	{
		mask |= PGS_NESTLOOP_PLAIN;
		if (enable_material)
			mask |= PGS_NESTLOOP_MATERIALIZE;
		if (enable_memoize)
			mask |= PGS_NESTLOOP_MEMOIZE;
	}
	if (enable_hashjoin)
		mask |= PGS_HASHJOIN;
	if (enable_gathermerge)
		mask |= PGS_GATHER_MERGE;
	if (enable_partitionwise_join)
		mask |= PGS_CONSIDER_PARTITIONWISE;

	return mask;
}

/*
 * A qual list with every AND split into its arms, as the planner's quals
 * reach a RelOptInfo (make_restrictinfo() asserts it); ORCA's filter can be
 * one AND of several.
 */
static List *
flatten_and_quals(List *quals)
{
	List	   *flat = NIL;
	ListCell   *lc;

	foreach(lc, quals)
	{
		Node	   *qual = (Node *) lfirst(lc);

		if (is_andclause(qual))
			flat = list_concat(flat,
							   flatten_and_quals(((BoolExpr *) qual)->args));
		else
			flat = lappend(flat, qual);
	}

	return flat;
}

ForeignScan *
BuildForeignScan(Oid relid, Index scanrelid, List *qual, List *targetlist,
				 Query *query, RangeTblEntry *rte)
{
	PlannerGlobal *glob;
	PlannerInfo *root;
	RelOptInfo *rel;
	Path	   *path;
	ForeignScan *fscan;
	List	   *restrict_quals = NIL;
	Bitmapset  *attrs_used = NULL;
	ListCell   *lc;

	glob = makeNode(PlannerGlobal);
	glob->default_pgs_mask = default_pgs_mask();

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;
	/* all the rows, as the planner assumes of anything but a cursor */
	root->tuple_fraction = 0.0;

	/*
	 * The arrays are indexed by range table index, and only the entry being
	 * scanned is in them: the wrapper plans one relation, and asks about no
	 * other.
	 */
	root->simple_rel_array_size = scanrelid + 1;
	root->simple_rel_array = palloc0_array(RelOptInfo *,
										   root->simple_rel_array_size);
	root->simple_rte_array = palloc0_array(RangeTblEntry *,
										   root->simple_rel_array_size);
	root->simple_rte_array[scanrelid] = rte;
	root->all_baserels = bms_make_singleton(scanrelid);
	root->all_query_rels = root->all_baserels;

	/* The wrapper separates what it can send from what it must check. */
	foreach(lc, flatten_and_quals(qual))
		restrict_quals = lappend(restrict_quals,
								 make_simple_restrictinfo(root,
														  (Expr *) lfirst(lc)));

	rel = build_simple_rel(root, scanrelid, NULL);
	rel->baserestrictinfo = restrict_quals;
	/* what the scan returns, which is what a wrapper fetches */
	rel->reltarget = make_pathtarget_from_tlist(targetlist);

	if (rel->fdwroutine == NULL)
		elog(ERROR, "relation %u is not a foreign table", relid);

	rel->fdwroutine->GetForeignRelSize(root, rel, relid);
	rel->fdwroutine->GetForeignPaths(root, rel, relid);

	if (rel->pathlist == NIL)
		elog(ERROR, "foreign-data wrapper offered no path for relation %u",
			 relid);

	set_cheapest(rel);
	path = rel->cheapest_total_path;
	if (!IsA(path, ForeignPath))
		elog(ERROR, "cheapest path for foreign relation %u is not a foreign scan",
			 relid);

	fscan = rel->fdwroutine->GetForeignPlan(root, rel, relid,
											(ForeignPath *) path,
											targetlist, restrict_quals,
											NULL /* outer_plan */ );

	/*
	 * What create_foreignscan_plan() fills in after the wrapper: the user to
	 * check as, the server, and the relations the scan covers, of which
	 * postgres_fdw takes its table from fs_base_relids.
	 */
	fscan->checkAsUser = rel->userid;
	fscan->fs_server = rel->serverid;
	fscan->fs_relids = bms_copy(rel->relids);
	fscan->fs_base_relids = bms_copy(rel->relids);

	/* Does the scan return a system column?  As create_foreignscan_plan(). */
	fscan->fsSystemCol = false;
	pull_varattnos((Node *) rel->reltarget->exprs, scanrelid, &attrs_used);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, scanrelid, &attrs_used);
	}
	for (int i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
	{
		if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
		{
			fscan->fsSystemCol = true;
			break;
		}
	}

	return fscan;
}
