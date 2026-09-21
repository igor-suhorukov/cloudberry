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
 * compat/optimizer/walkers.h
 *	  Walking a plan tree, which PostgreSQL 19 has no walker for.
 *
 * ORCA asks for this header by Cloudberry's name for it.  PostgreSQL 19
 * exports walkers for expressions, queries, range tables and PlanState
 * trees, but not for a Plan tree, and ORCA builds finished plans without the
 * planner, so it walks its own.
 *
 * See compat/walkers.c for what carries over and what was left behind.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_WALKERS_H
#define GP_ORCA_COMPAT_WALKERS_H

#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/*
 * A SubPlan names its plan by index into a list, and which list depends on
 * when you are asking: the PlannerInfo holds it during planning and the
 * PlannedStmt after.  Every context passed to the framework starts with one
 * of these so that the walker can find the plan either way.
 */
typedef struct plan_tree_base_prefix
{
	Node	   *node;			/* PlannerInfo * or PlannedStmt * */
} plan_tree_base_prefix;

extern void planner_init_plan_tree_base(plan_tree_base_prefix *base,
										PlannerInfo *root);
extern void exec_init_plan_tree_base(plan_tree_base_prefix *base,
									 PlannedStmt *stmt);
extern Plan *plan_tree_base_subplan_get_plan(plan_tree_base_prefix *base,
											 SubPlan *subplan);

/* The framework itself: the Plan-tree counterpart of expression_tree_walker. */
extern bool walk_plan_node_fields(Plan *plan,
								  bool (*walker) (Node *, void *),
								  void *context);
extern bool plan_tree_walker(Node *node,
							 bool (*walker) (Node *, void *),
							 void *context, bool recurse_into_subplans);

/* Every node of the given tag in a plan tree. */
extern List *extract_nodes(PlannerGlobal *glob, Node *node, int nodeTag);
extern List *extract_nodes_plan(Plan *pl, int nodeTag,
								bool descendIntoSubqueries);

/* The same in an expression. */
extern List *extract_nodes_expression(Node *node, int nodeTag,
									  bool descendIntoSubqueries);

/*
 * The index within nodeTags of the first tag that appears in the tree, or -1.
 */
extern int	find_nodes(Node *node, List *nodeTags);

/*
 * Does anything in this expression carry a collation that is not the default?
 *
 * 1 if so, -1 if not.  It is a flag and never a collation OID, despite the
 * name and despite Cloudberry's callers: Cloudberry's own copy still carries
 * the marker that says as much ("GPDB_91_MERGE_FIXME: collation").  Anything
 * that treats the result as an OID is wrong.
 */
extern int	check_collation(Node *node);

/*
 * Does the query's ORDER BY sort by an ordering operator over a plain
 * column?  Those are the KNN shapes the PostgreSQL planner can turn into a
 * GiST index scan and ORCA cannot, so ORCA declines the query.
 */
extern bool has_orderby_ordering_op(Query *query);

#endif							/* GP_ORCA_COMPAT_WALKERS_H */
