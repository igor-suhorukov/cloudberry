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
 * config/gp_orca_guc.h
 *	  ORCA's settings, under the names PostgreSQL 19 will accept.
 *
 * Every one of Cloudberry's settings gains a "gp." prefix, not only the ones
 * written to a file: PostgreSQL 19 will not define a custom variable whose
 * name is not "two or more identifiers separated by dots"
 * (pg19/src/backend/utils/misc/guc.c:951-957).  So optimizer_join_order
 * becomes gp.optimizer_join_order, and so on for all of them.  This is the
 * correction cloudberry.md records under "Corrections to the plan": the
 * file-versus-runtime distinction still decides what *else* has to happen to
 * a setting, but not whether its name changes.  It always changes.
 *
 * WHY THEY ARE HERE AND NOT IN gp_core.  These are the optimizer's, read by
 * config/CConfigParamMapping.cpp and by nothing else, and gp_orca is the
 * module that owns them.  gp.optimizer and gp.optimizer_trace_fallback are
 * the exception: they decide whether ORCA is asked at all, so they live with
 * the planner hook in gp_orca_planner.c.
 *
 * The list is an X-macro so that a setting is declared, defined and
 * registered from one row.  Cloudberry writes each of these three times, in
 * three files, and a setting that is added to two of them is a variable
 * nobody can set.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_GUC_H
#define GP_ORCA_GUC_H

#include "postgres.h"

/*
 * The bounds below are written as INT_MAX and DBL_MAX, as Cloudberry's own
 * table writes them.  postgres.h brings in neither, so this header does: a
 * limit named inside an X-macro is expanded in whatever file uses the macro,
 * and that file should not have to know what the macro needed.
 */
#include <float.h>
#include <limits.h>

/*
 * The boolean settings, taken from Cloudberry's guc_gp.c with their defaults
 * and their descriptions: X(variable, default, description)
 */
#define GP_ORCA_BOOL_GUCS(X) \
	X(optimizer_apply_left_outer_to_union_all_disregarding_stats, false, "Always apply Left Outer Join to Inner Join UnionAll Left Anti Semi Join without looking at stats.") \
	X(optimizer_array_constraints, true, "Allows the optimizer's constraint framework to derive array constraints.") \
	X(optimizer_cte_inlining, false, "Enable CTE inlining.") \
	X(optimizer_debug_cte, false, "Print the debug info of CTE in ORCA. Only worked with debug version of CBDB.") \
	X(optimizer_disable_dynamic_table_scan, false, "Disable the dynamic seq/bitmap/index scan in partition table.") \
	X(optimizer_discard_redistribute_hashjoin, false, "Discard hash join with redistribute motion in the optimizer.") \
	X(optimizer_dpe_stats, true, "Enable statistics derivation for partitioned tables with dynamic partition elimination.") \
	X(optimizer_enable_assert_maxonerow, true, "Enable Assert MaxOneRow plans to check number of rows at runtime.") \
	X(optimizer_enable_associativity, false, "Enables Join Associativity in optimizer.") \
	X(optimizer_enable_bitmapscan, true, "Enable bitmap plans in the optimizer.") \
	X(optimizer_enable_broadcast_nestloop_outer_child, true, "Enable nested loops join plans with replicated outer child in the optimizer.") \
	X(optimizer_enable_constant_expression_evaluation, true, "Enable constant expression evaluation in the optimizer.") \
	X(optimizer_enable_ctas, true, "Enable CTAS plans in the optimizer.") \
	X(optimizer_enable_derive_stats_all_groups, false, "Enable stats derivation for all groups after exploration.") \
	X(optimizer_enable_direct_dispatch, true, "Enable direct dispatch in the optimizer.") \
	X(optimizer_enable_dml, true, "Enable DML plans in GPORCA.") \
	X(optimizer_enable_dml_constraints, true, "Support DML with CHECK constraints and NOT NULL constraints.") \
	X(optimizer_enable_dynamicbitmapscan, true, "Enables the optimizer's use of plans with dynamic bitmap scan.") \
	X(optimizer_enable_dynamicindexonlyscan, true, "Enables the optimizer's use of plans with dynamic index only scan.") \
	X(optimizer_enable_dynamicindexscan, true, "Enables the optimizer's use of plans with dynamic index scan.") \
	X(optimizer_enable_dynamictablescan, true, "Enables the optimizer's use of plans with dynamic table scan.") \
	X(optimizer_enable_eageragg, false, "Enable Eager Agg transform for pushing aggregate below an innerjoin.") \
	X(optimizer_enable_foreign_table, true, "Enable foreign tables in Orca.") \
	X(optimizer_enable_gather_on_segment_for_dml, true, "Enable DML optimization by enforcing a non-master gather in the optimizer.") \
	X(optimizer_enable_groupagg, true, "Enables GPORCA to use group aggregates.") \
	X(optimizer_enable_hashagg, true, "Enables GPORCA to use hash aggregates.") \
	X(optimizer_enable_hashjoin, true, "Enables the optimizer's use of hash join plans.") \
	X(optimizer_enable_hashjoin_redistribute_broadcast_children, false, "Enable hash join plans with, Redistribute outer child and Broadcast inner child, in the optimizer.") \
	X(optimizer_enable_indexjoin, true, "Enable index nested loops join plans in the optimizer.") \
	X(optimizer_enable_indexonlyscan, true, "Enables the optimizer's use of plans with index only scan.") \
	X(optimizer_enable_indexscan, true, "Enables the optimizer's use of plans with index scan.") \
	X(optimizer_enable_master_only_queries, false, "Process master only queries via the optimizer.") \
	X(optimizer_enable_materialize, true, "Enable plans with Materialize operators in the optimizer.") \
	X(optimizer_enable_mergejoin, true, "Enables the optimizer's support of merge joins.") \
	X(optimizer_enable_motion_broadcast, true, "Enable plans with Motion Broadcast operators in the optimizer.") \
	X(optimizer_enable_motion_gather, true, "Enable plans with Motion Gather operators in the optimizer.") \
	X(optimizer_enable_motion_redistribute, true, "Enable plans with Motion Redistribute operators in the optimizer.") \
	X(optimizer_enable_motions, true, "Enable plans with Motion operators in the optimizer.") \
	X(optimizer_enable_motions_masteronly_queries, false, "Enable plans with Motion operators in the optimizer for queries with no distributed tables.") \
	X(optimizer_enable_multiple_distinct_aggs, false, "Enable plans with multiple distinct aggregates in the optimizer.") \
	X(optimizer_enable_nljoin, true, "Enable nested loops join plans in the optimizer.") \
	X(optimizer_enable_orderedagg, true, "Enable ordered aggregate plans.") \
	X(optimizer_enable_outerjoin_rewrite, true, "Enable outer join to inner join rewrite in the optimizer.") \
	X(optimizer_enable_outerjoin_to_unionall_rewrite, false, "Enable rewriting Left Outer Join to UnionAll.") \
	X(optimizer_enable_partition_propagation, true, "Enable plans with Partition Propagation operators in the optimizer.") \
	X(optimizer_enable_partition_selection, true, "Enable plans with Partition Selection operators in the optimizer.") \
	X(optimizer_enable_push_join_below_union_all, false, "Enable transform of join of union all to union all of joins. May improve the join performance.") \
	X(optimizer_enable_query_parameter, true, "Enable query parameters in Orca.") \
	X(optimizer_enable_range_predicate_dpe, false, "Enable range predicates for dynamic partition elimination.") \
	X(optimizer_enable_redistribute_nestloop_loj_inner_child, true, "Enable nested loops left join plans with redistributed inner child in the optimizer.") \
	X(optimizer_enable_replicated_table, true, "Enable replicated tables.") \
	X(optimizer_enable_right_outer_join, true, "Enable Orca to generate plans containing right outer joins.") \
	X(optimizer_enable_sort, true, "Enable plans with Sort operators in the optimizer.") \
	X(optimizer_enable_space_pruning, true, "Enable space pruning in the optimizer.") \
	X(optimizer_enable_streaming_material, true, "Enable plans with a streaming material node in the optimizer.") \
	X(optimizer_enable_tablescan, true, "Enables the optimizer's use of plans with table scan.") \
	X(optimizer_enable_use_distribution_in_dqa, false, "Enable use the distribution key in DQA.") \
	X(optimizer_enforce_subplans, false, "Enforce correlated execution in the optimizer.") \
	X(optimizer_enumerate_plans, false, "Enable plan enumeration.") \
	X(optimizer_expand_fulljoin, false, "Enables the optimizer's support of expanding full outer joins using union all.") \
	X(optimizer_extract_dxl_stats, false, "Extract plan stats in dxl.") \
	X(optimizer_extract_dxl_stats_all_nodes, false, "Extract plan stats for all physical dxl nodes.") \
	X(optimizer_force_agg_skew_avoidance, true, "Always pick a plan for aggregate distinct that minimizes skew.") \
	X(optimizer_force_comprehensive_join_implementation, false, "Explore a nested loop join even if a hash join is possible.") \
	X(optimizer_force_expanded_distinct_aggs, true, "Always pick plans that expand multiple distinct aggregates into join of single distinct aggregate in the optimizer.") \
	X(optimizer_force_multistage_agg, false, "Force optimizer to always pick multistage aggregates when such a plan alternative is generated.") \
	X(optimizer_force_split_window_function, false, "Always split the window function.") \
	X(optimizer_force_three_stage_scalar_dqa, true, "Force optimizer to always pick 3 stage aggregate plan for scalar distinct qualified aggregate.") \
	X(optimizer_force_window_hash_agg, false, "Enable create window hash agg.") \
	X(optimizer_metadata_caching, true, "This guc enables the optimizer to cache and reuse metadata.") \
	X(optimizer_multilevel_partitioning, true, "Enable optimization of queries on multilevel partitioned tables.") \
	X(optimizer_parallel_union, false, "Enable parallel execution for UNION/UNION ALL queries.") \
	X(optimizer_penalize_skew, true, "Penalize operators with skewed hash redistribute below it.") \
	X(optimizer_print_expression_properties, false, "Print expression properties.") \
	X(optimizer_print_group_properties, false, "Print group properties.") \
	X(optimizer_print_job_scheduler, false, "Print the jobs in the scheduler on each job completion.") \
	X(optimizer_print_memo_after_exploration, false, "Print optimizer memo structure after the exploration phase.") \
	X(optimizer_print_memo_after_implementation, false, "Print optimizer memo structure after the implementation phase.") \
	X(optimizer_print_memo_after_optimization, false, "Print optimizer memo structure after optimization.") \
	X(optimizer_print_missing_stats, true, "Print columns with missing statistics.") \
	X(optimizer_print_optimization_context, false, "Print the optimization context.") \
	X(optimizer_print_optimization_stats, false, "Print optimization stats.") \
	X(optimizer_print_plan, false, "Prints the plan expression tree produced by the optimizer.") \
	X(optimizer_print_preprocess_result, false, "Prints the expression tree produced by the optimizer preprocess(every steps). Only worked with debug version of CBDB.") \
	X(optimizer_print_query, false, "Prints the optimizer's input query expression tree.") \
	X(optimizer_print_xform, false, "Prints optimizer transformation information.") \
	X(optimizer_print_xform_results, false, "Print the input and output of optimizer transformations.") \
	X(optimizer_prune_computed_columns, true, "Prune unused computed columns when pre-processing query.") \
	X(optimizer_push_requirements_from_consumer_to_producer, true, "Optimize CTE producer plan on requirements enforced on top of CTE consumer in the optimizer.") \
	X(optimizer_remove_order_below_dml, false, "Remove OrderBy below a DML operation.") \
	X(optimizer_sample_plans, false, "Enable plan sampling.") \
	X(optimizer_use_external_constant_expression_evaluation_for_ints, false, "Use external constant expression evaluation in the optimizer for all integer types.") \
	X(optimizer_use_streaming_hashagg, true, "Use streaming hash agg in ORCA-generated local partial hash aggregations.")

/* Declare each one. */
#define X(var, dflt, doc)	extern PGDLLIMPORT bool var;
GP_ORCA_BOOL_GUCS(X)
#undef X

/*
 * The numbers, and the one string.
 *
 * These are the settings that configure the optimizer's *context* -- the cost
 * model's factors, the search strategy, the size of the metadata cache, the
 * thresholds at which a transform stops being tried -- rather than switching a
 * rule on or off.  CConfigParamMapping does not see any of them: it turns
 * settings into trace flags, and none of these is a flag.  COptTasks reads
 * them, once per query, when it builds the COptimizerConfig.
 *
 * That is why they arrive later than the booleans above, and it is worth
 * recording: "ORCA's settings" is two surfaces, not one.  Each row's default
 * and bounds are Cloudberry's, read off guc_gp.c.
 */
#define GP_ORCA_INT_GUCS(X) \
	X(optimizer_array_expansion_threshold, 20, 0, INT_MAX, "Item limit for expansion of arrays in WHERE clause for constraint derivation.") \
	X(optimizer_cte_inlining_bound, 0, 0, INT_MAX, "Set the CTE inlining cutoff.") \
	X(optimizer_join_arity_for_associativity_commutativity, 18, 0, INT_MAX, "Maximum number of children n-ary-join have without disabling commutativity and associativity transform.") \
	X(optimizer_join_order_threshold, 10, 0, 12, "Maximum number of join children to use dynamic programming based join ordering algorithm.") \
	X(optimizer_mdcache_size, 16384, 0, INT_MAX, "Sets the size of MDCache.") \
	X(optimizer_penalize_broadcast_threshold, 100000, 0, INT_MAX, "Maximum number of rows of a relation that can be broadcasted without penalty. A value of 0 disables.") \
	X(optimizer_plan_id, 0, 0, INT_MAX, "Choose a plan alternative.") \
	X(optimizer_push_group_by_below_setop_threshold, 10, 0, INT_MAX, "Maximum number of children setops have to consider pushing group bys below it.") \
	X(optimizer_samples_number, 1000, 1, INT_MAX, "Set the number of plan samples.") \
	X(optimizer_segments, 0, 0, INT_MAX, "Number of segments to be considered by the optimizer during costing, or 0 to take the actual number of segments.") \
	X(optimizer_skew_factor, 0, 0, 100, "Coefficient of skew ratio computed from sample statistics. 0 turns skew computation off; 1 to 100 multiplies the ratio used for costing.") \
	X(optimizer_xform_bind_threshold, 0, 0, INT_MAX, "Maximum number bindings per xform per group expression. A value of 0 disables.")

#define X(var, dflt, lo, hi, doc)	extern PGDLLIMPORT int var;
GP_ORCA_INT_GUCS(X)
#undef X

#define GP_ORCA_REAL_GUCS(X) \
	X(optimizer_cost_threshold, 0.0, 0.0, INT_MAX, "Set the threshold for plan sampling relative to the cost of best plan; 0.0 means unbounded.") \
	X(optimizer_damping_factor_filter, 0.75, 0.0, 1.0, "Select predicate damping factor in optimizer; 1.0 means no damping.") \
	X(optimizer_damping_factor_groupby, 0.75, 0.0, 1.0, "Groupby operator damping factor in optimizer; 1.0 means no damping.") \
	X(optimizer_damping_factor_join, 0.0, 0.0, 1.0, "Join predicate damping factor in optimizer; 1.0 means no damping, 0.0 means square root method.") \
	X(optimizer_nestloop_factor, 1024.0, 1.0, DBL_MAX, "Set the nestloop join cost factor in the optimizer.") \
	X(optimizer_sort_factor, 1.0, 0.0, DBL_MAX, "Set the sort cost factor in the optimizer; 1.0 is the default cost, above is more costly, below is less.") \
	X(optimizer_spilling_mem_threshold, 0.0, 0.0, DBL_MAX, "Set the optimizer factor for threshold of spilling to memory; 0.0 means unbounded.")

#define X(var, dflt, lo, hi, doc)	extern PGDLLIMPORT double var;
GP_ORCA_REAL_GUCS(X)
#undef X

/*
 * The search strategy, as a path to an XML file describing it.  Empty means
 * ORCA's own default strategies, which is what every installation that has not
 * been tuned by hand uses.
 */
extern PGDLLIMPORT char *optimizer_search_strategy_path;

/*
 * Whether ORCA allocates through PostgreSQL memory contexts.
 *
 * It is PGC_POSTMASTER in Cloudberry and stays so here, because the allocator
 * is chosen when ORCA's memory pool manager is built and every pool made after
 * that inherits the choice.  gp_orca is in shared_preload_libraries, so a
 * postmaster-context custom variable is one it may define.
 */
extern PGDLLIMPORT bool optimizer_use_gpdb_allocators;

/*
 * The four that are not booleans.
 *
 * The values keep Cloudberry's spelling, because ORCA's own code and its
 * stored minidumps are written against them.
 */

/* gp.optimizer_minidump */
#define OPTIMIZER_MINIDUMP_FAIL		0	/* on failure */
#define OPTIMIZER_MINIDUMP_ALWAYS	1	/* always */
extern PGDLLIMPORT int optimizer_minidump;

/* gp.optimizer_cost_model */
#define OPTIMIZER_GPDB_LEGACY		0
#define OPTIMIZER_GPDB_CALIBRATED	1
#define OPTIMIZER_GPDB_EXPERIMENTAL	2
extern PGDLLIMPORT int optimizer_cost_model;

/* gp.optimizer_join_order */
#define JOIN_ORDER_IN_QUERY			0
#define JOIN_ORDER_GREEDY_SEARCH	1
#define JOIN_ORDER_EXHAUSTIVE_SEARCH	2
#define JOIN_ORDER_EXHAUSTIVE2_SEARCH	3
extern PGDLLIMPORT int optimizer_join_order;

/* gp.optimizer_agg_pds_strategy -- an int with a range, not an enum */
#define OPTIMIZER_AGG_PDS_ALL_KEY			0
#define OPTIMIZER_AGG_PDS_FIRST_KEY			1
#define OPTIMIZER_AGG_PDS_MINIMAL_LEN_KEY	2
#define OPTIMIZER_AGG_PDS_EXCLUDE_NON_FIXED	3
extern PGDLLIMPORT int optimizer_agg_pds_strategy;

/*
 * Which transformation rules are switched off, by xform id.
 *
 * Cloudberry keeps this as an array a SQL function writes into rather than a
 * setting, because there are some 180 of them and a setting each would be
 * unreadable.  Sized at ORCA's sentinel, which the module asks for rather
 * than hardcoding.
 */
extern PGDLLIMPORT bool *optimizer_xforms;

/* Register them all.  Called from gp_orca's _PG_init. */
extern void GpOrcaDefineSettings(void);

#endif							/* GP_ORCA_GUC_H */
