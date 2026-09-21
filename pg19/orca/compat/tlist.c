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
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * cb_split_pathtarget_at_srfs() and what it calls are PostgreSQL 19's, from
 * its src/backend/optimizer/util/tlist.c.
 *
 * compat/tlist.c
 *	  What Cloudberry adds to PostgreSQL's tlist.c for ORCA.
 *
 * Ported from github/cloudberry/src/backend/optimizer/util/tlist.c.
 *
 * TWO THINGS, AND THE SECOND IS NOT AN ADDITION BUT A CHANGE.
 *
 * tlist_members() is Cloudberry's own.  The other is PostgreSQL's
 * split_pathtarget_at_srfs(), which Cloudberry changed so that it accepts a
 * null PlannerInfo: ORCA's translator builds the nested ProjectSet nodes a
 * target list with set-returning functions needs by calling it, and it has
 * no PlannerInfo to give.  PostgreSQL 19's dereferences root, so the port
 * carries PostgreSQL 19's copy here with Cloudberry's one-line guard.
 *
 * It is renamed, cb_split_pathtarget_at_srfs, and it has to be: the server
 * exports split_pathtarget_at_srfs, and a definition of the same name in
 * this module would not be the one this module called.  A shared library's
 * references to a symbol the executable defines bind to the executable's.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "rewrite/rewriteManip.h"

#include "cb_tlist.h"


/*
 * Test if an expression node represents a SRF call.  Beware multiple eval!
 *
 * Please note that this is only meant for use in split_pathtarget_at_srfs();
 * if you use it anywhere else, your code is almost certainly wrong for SRFs
 * nested within expressions.  Use expression_returns_set() instead.
 */
#define IS_SRF_CALL(node) \
	((IsA(node, FuncExpr) && ((FuncExpr *) (node))->funcretset) || \
	 (IsA(node, OpExpr) && ((OpExpr *) (node))->opretset))

/*
 * Data structures for split_pathtarget_at_srfs().  To preserve the identity
 * of sortgroupref items even if they are textually equal(), what we track is
 * not just bare expressions but expressions plus their sortgroupref indexes.
 */
typedef struct
{
	Node	   *expr;			/* some subexpression of a PathTarget */
	Index		sortgroupref;	/* its sortgroupref, or 0 if none */
} split_pathtarget_item;

typedef struct
{
	PlannerInfo *root;
	bool		is_grouping_target; /* true if processing grouping target */
	/* This is a List of bare expressions: */
	List	   *input_target_exprs; /* exprs available from input */
	/* These are Lists of Lists of split_pathtarget_items: */
	List	   *level_srfs;		/* SRF exprs to evaluate at each level */
	List	   *level_input_vars;	/* input vars needed at each level */
	List	   *level_input_srfs;	/* input SRFs needed at each level */
	/* These are Lists of split_pathtarget_items: */
	List	   *current_input_vars; /* vars needed in current subexpr */
	List	   *current_input_srfs; /* SRFs needed in current subexpr */
	/* Auxiliary data for current split_pathtarget_walker traversal: */
	int			current_depth;	/* max SRF depth in current subexpr */
	Index		current_sgref;	/* current subexpr's sortgroupref, or 0 */
} split_pathtarget_context;

static void split_pathtarget_at_srfs_extended(PlannerInfo *root,
											  PathTarget *target,
											  PathTarget *input_target,
											  List **targets,
											  List **targets_contain_srfs,
											  bool is_grouping_target);
static bool split_pathtarget_walker(Node *node,
									split_pathtarget_context *context);
static void add_sp_item_to_pathtarget(PathTarget *target,
									  split_pathtarget_item *item);
static void add_sp_items_to_pathtarget(PathTarget *target, List *items);


/*
 * tlist_members
 *		Every entry of the target list whose expression is equal() to the
 *		given one.  NIL if there is none.
 *
 * PostgreSQL has tlist_member(), which returns the first match and stops
 * (pg19/src/backend/optimizer/util/tlist.c:88).  That is the right answer
 * for its callers, which want *a* place the expression is computed.  ORCA
 * wants all of them, because it rewrites references rather than picking one:
 * a target list can compute the same expression twice, under two resjunk
 * flags or two sort groups, and leaving the second occurrence pointing at
 * the first one's column would change what the plan projects.
 *
 * Nothing here changed for PostgreSQL 19.  The entries are not copied, as
 * Cloudberry's comment says; the list is the caller's to free and the
 * TargetEntrys in it are not.
 *
 * Cloudberry asserts IsA(tlentry, TargetEntry) per entry.  The port leaves
 * that to equal(), which reaches the same answer: a list member that is not
 * a TargetEntry has no ->expr to compare and equal() would already be
 * reading the wrong field.  Assert is compiled out in the builds that
 * matter, so it protected nothing the release build could rely on.
 */
List *
tlist_members(Node *node, List *targetlist)
{
	List	   *tlist = NIL;
	ListCell   *lc;

	foreach(lc, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(lc);

		if (equal(node, tlentry->expr))
			tlist = lappend(tlist, tlentry);
	}

	return tlist;
}

/*
 * cb_split_pathtarget_at_srfs
 *		PostgreSQL's split_pathtarget_at_srfs(), accepting a null root.
 *
 * The same as PostgreSQL 19's wrapper, which passes is_grouping_target
 * false -- and with it false, the walker never reads root, so the guard
 * above is the only place a null one had to be handled.
 */
void
cb_split_pathtarget_at_srfs(PlannerInfo *root,
							PathTarget *target, PathTarget *input_target,
							List **targets, List **targets_contain_srfs)
{
	split_pathtarget_at_srfs_extended(root, target, input_target,
									  targets, targets_contain_srfs,
									  false);
}

/*
 * split_pathtarget_at_srfs_extended
 *		Split given PathTarget into multiple levels to position SRFs safely
 *
 * The executor can only handle set-returning functions that appear at the
 * top level of the targetlist of a ProjectSet plan node.  If we have any SRFs
 * that are not at top level, we need to split up the evaluation into multiple
 * plan levels in which each level satisfies this constraint.  This function
 * creates appropriate PathTarget(s) for each level.
 *
 * As an example, consider the tlist expression
 *		x + srf1(srf2(y + z))
 * This expression should appear as-is in the top PathTarget, but below that
 * we must have a PathTarget containing
 *		x, srf1(srf2(y + z))
 * and below that, another PathTarget containing
 *		x, srf2(y + z)
 * and below that, another PathTarget containing
 *		x, y, z
 * When these tlists are processed by setrefs.c, subexpressions that match
 * output expressions of the next lower tlist will be replaced by Vars,
 * so that what the executor gets are tlists looking like
 *		Var1 + Var2
 *		Var1, srf1(Var2)
 *		Var1, srf2(Var2 + Var3)
 *		x, y, z
 * which satisfy the desired property.
 *
 * Another example is
 *		srf1(x), srf2(srf3(y))
 * That must appear as-is in the top PathTarget, but below that we need
 *		srf1(x), srf3(y)
 * That is, each SRF must be computed at a level corresponding to the nesting
 * depth of SRFs within its arguments.
 *
 * In some cases, a SRF has already been evaluated in some previous plan level
 * and we shouldn't expand it again (that is, what we see in the target is
 * already meant as a reference to a lower subexpression).  So, don't expand
 * any tlist expressions that appear in input_target, if that's not NULL.
 *
 * This check requires extra care when processing the grouping target
 * (indicated by the is_grouping_target flag).  In this case input_target is
 * pre-grouping while target is post-grouping, so the latter may carry
 * nullingrels bits from the grouping step that are absent in the former.  We
 * must ignore those bits to correctly recognize that the tlist expressions are
 * available in input_target.
 *
 * It's also important that we preserve any sortgroupref annotation appearing
 * in the given target, especially on expressions matching input_target items.
 *
 * The outputs of this function are two parallel lists, one a list of
 * PathTargets and the other an integer list of bool flags indicating
 * whether the corresponding PathTarget contains any evaluable SRFs.
 * The lists are given in the order they'd need to be evaluated in, with
 * the "lowest" PathTarget first.  So the last list entry is always the
 * originally given PathTarget, and any entries before it indicate evaluation
 * levels that must be inserted below it.  The first list entry must not
 * contain any SRFs (other than ones duplicating input_target entries), since
 * it will typically be attached to a plan node that cannot evaluate SRFs.
 *
 * Note: using a list for the flags may seem like overkill, since there
 * are only a few possible patterns for which levels contain SRFs.
 * But this representation decouples callers from that knowledge.
 */
static void
split_pathtarget_at_srfs_extended(PlannerInfo *root,
								  PathTarget *target, PathTarget *input_target,
								  List **targets, List **targets_contain_srfs,
								  bool is_grouping_target)
{
	split_pathtarget_context context;
	int			max_depth;
	bool		need_extra_projection;
	List	   *prev_level_tlist;
	int			lci;
	ListCell   *lc,
			   *lc1,
			   *lc2,
			   *lc3;

	/*
	 * It's not unusual for planner.c to pass us two physically identical
	 * targets, in which case we can conclude without further ado that all
	 * expressions are available from the input.  (The logic below would
	 * arrive at the same conclusion, but much more tediously.)
	 */
	if (target == input_target)
	{
		*targets = list_make1(target);
		*targets_contain_srfs = list_make1_int(false);
		return;
	}

	/*
	 * Pass 'root', the is_grouping_target flag, and any input_target exprs
	 * down to split_pathtarget_walker().
	 */
	context.root = root;
	context.is_grouping_target = is_grouping_target;
	context.input_target_exprs = input_target ? input_target->exprs : NIL;

	/*
	 * Initialize with empty level-zero lists, and no levels after that.
	 * (Note: we could dispense with representing level zero explicitly, since
	 * it will never receive any SRFs, but then we'd have to special-case that
	 * level when we get to building result PathTargets.  Level zero describes
	 * the SRF-free PathTarget that will be given to the input plan node.)
	 */
	context.level_srfs = list_make1(NIL);
	context.level_input_vars = list_make1(NIL);
	context.level_input_srfs = list_make1(NIL);

	/* Initialize data we'll accumulate across all the target expressions */
	context.current_input_vars = NIL;
	context.current_input_srfs = NIL;
	max_depth = 0;
	need_extra_projection = false;

	/* Scan each expression in the PathTarget looking for SRFs */
	lci = 0;
	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		/* Tell split_pathtarget_walker about this expr's sortgroupref */
		context.current_sgref = get_pathtarget_sortgroupref(target, lci);
		lci++;

		/*
		 * Find all SRFs and Vars (and Var-like nodes) in this expression, and
		 * enter them into appropriate lists within the context struct.
		 */
		context.current_depth = 0;
		split_pathtarget_walker(node, &context);

		/* An expression containing no SRFs is of no further interest */
		if (context.current_depth == 0)
			continue;

		/*
		 * Track max SRF nesting depth over the whole PathTarget.  Also, if
		 * this expression establishes a new max depth, we no longer care
		 * whether previous expressions contained nested SRFs; we can handle
		 * any required projection for them in the final ProjectSet node.
		 */
		if (max_depth < context.current_depth)
		{
			max_depth = context.current_depth;
			need_extra_projection = false;
		}

		/*
		 * If any maximum-depth SRF is not at the top level of its expression,
		 * we'll need an extra Result node to compute the top-level scalar
		 * expression.
		 */
		if (max_depth == context.current_depth && !IS_SRF_CALL(node))
			need_extra_projection = true;
	}

	/*
	 * If we found no SRFs needing evaluation (maybe they were all present in
	 * input_target, or maybe they were all removed by const-simplification),
	 * then no ProjectSet is needed; fall out.
	 */
	if (max_depth == 0)
	{
		*targets = list_make1(target);
		*targets_contain_srfs = list_make1_int(false);
		return;
	}

	/*
	 * The Vars and SRF outputs needed at top level can be added to the last
	 * level_input lists if we don't need an extra projection step.  If we do
	 * need one, add a SRF-free level to the lists.
	 */
	if (need_extra_projection)
	{
		context.level_srfs = lappend(context.level_srfs, NIL);
		context.level_input_vars = lappend(context.level_input_vars,
										   context.current_input_vars);
		context.level_input_srfs = lappend(context.level_input_srfs,
										   context.current_input_srfs);
	}
	else
	{
		lc = list_nth_cell(context.level_input_vars, max_depth);
		lfirst(lc) = list_concat(lfirst(lc), context.current_input_vars);
		lc = list_nth_cell(context.level_input_srfs, max_depth);
		lfirst(lc) = list_concat(lfirst(lc), context.current_input_srfs);
	}

	/*
	 * Now construct the output PathTargets.  The original target can be used
	 * as-is for the last one, but we need to construct a new SRF-free target
	 * representing what the preceding plan node has to emit, as well as a
	 * target for each intermediate ProjectSet node.
	 */
	*targets = *targets_contain_srfs = NIL;
	prev_level_tlist = NIL;

	forthree(lc1, context.level_srfs,
			 lc2, context.level_input_vars,
			 lc3, context.level_input_srfs)
	{
		List	   *level_srfs = (List *) lfirst(lc1);
		PathTarget *ntarget;

		if (lnext(context.level_srfs, lc1) == NULL)
		{
			ntarget = target;
		}
		else
		{
			ntarget = create_empty_pathtarget();

			/*
			 * This target should actually evaluate any SRFs of the current
			 * level, and it needs to propagate forward any Vars needed by
			 * later levels, as well as SRFs computed earlier and needed by
			 * later levels.
			 */
			add_sp_items_to_pathtarget(ntarget, level_srfs);
			for_each_cell(lc, context.level_input_vars,
						  lnext(context.level_input_vars, lc2))
			{
				List	   *input_vars = (List *) lfirst(lc);

				add_sp_items_to_pathtarget(ntarget, input_vars);
			}
			for_each_cell(lc, context.level_input_srfs,
						  lnext(context.level_input_srfs, lc3))
			{
				List	   *input_srfs = (List *) lfirst(lc);
				ListCell   *lcx;

				foreach(lcx, input_srfs)
				{
					split_pathtarget_item *item = lfirst(lcx);

					if (list_member(prev_level_tlist, item->expr))
						add_sp_item_to_pathtarget(ntarget, item);
				}
			}

			/*
			 * The one change, and it is Cloudberry's: ORCA's translator calls
			 * this with no PlannerInfo, and set_pathtarget_cost_width()
			 * dereferences one.  The cost and width it would set are the
			 * planner's; the translator reads only the expressions and their
			 * sortgrouprefs, through make_tlist_from_pathtarget().
			 */
			if (root != NULL)
				set_pathtarget_cost_width(root, ntarget);
		}

		/*
		 * Add current target and does-it-compute-SRFs flag to output lists.
		 */
		*targets = lappend(*targets, ntarget);
		*targets_contain_srfs = lappend_int(*targets_contain_srfs,
											(level_srfs != NIL));

		/* Remember this level's output for next pass */
		prev_level_tlist = ntarget->exprs;
	}
}

/*
 * Recursively examine expressions for split_pathtarget_at_srfs.
 *
 * Note we make no effort here to prevent duplicate entries in the output
 * lists.  Duplicates will be gotten rid of later.
 */
static bool
split_pathtarget_walker(Node *node, split_pathtarget_context *context)
{
	Node	   *sanitized_node = node;

	if (node == NULL)
		return false;

	/*
	 * If we are crossing the grouping boundary (post-grouping target vs
	 * pre-grouping input_target), we must ignore the grouping nulling bit to
	 * correctly check if the subexpression is available in input_target. This
	 * aligns with the matching logic in set_upper_references().
	 */
	if (context->is_grouping_target &&
		context->root->parse->hasGroupRTE &&
		context->root->parse->groupingSets != NIL)
	{
		sanitized_node =
			remove_nulling_relids(node,
								  bms_make_singleton(context->root->group_rtindex),
								  NULL);
	}

	/*
	 * A subexpression that matches an expression already computed in
	 * input_target can be treated like a Var (which indeed it will be after
	 * setrefs.c gets done with it), even if it's actually a SRF.  Record it
	 * as being needed for the current expression, and ignore any
	 * substructure.  (Note in particular that this preserves the identity of
	 * any expressions that appear as sortgrouprefs in input_target.)
	 */
	if (list_member(context->input_target_exprs, sanitized_node))
	{
		split_pathtarget_item *item = palloc_object(split_pathtarget_item);

		item->expr = node;
		item->sortgroupref = context->current_sgref;
		context->current_input_vars = lappend(context->current_input_vars,
											  item);
		return false;
	}

	/*
	 * Vars and Var-like constructs are expected to be gotten from the input,
	 * too.  We assume that these constructs cannot contain any SRFs (if one
	 * does, there will be an executor failure from a misplaced SRF).
	 */
	if (IsA(node, Var) ||
		IsA(node, PlaceHolderVar) ||
		IsA(node, Aggref) ||
		IsA(node, GroupingFunc) ||
		IsA(node, WindowFunc))
	{
		split_pathtarget_item *item = palloc_object(split_pathtarget_item);

		item->expr = node;
		item->sortgroupref = context->current_sgref;
		context->current_input_vars = lappend(context->current_input_vars,
											  item);
		return false;
	}

	/*
	 * If it's a SRF, recursively examine its inputs, determine its level, and
	 * make appropriate entries in the output lists.
	 */
	if (IS_SRF_CALL(node))
	{
		split_pathtarget_item *item = palloc_object(split_pathtarget_item);
		List	   *save_input_vars = context->current_input_vars;
		List	   *save_input_srfs = context->current_input_srfs;
		int			save_current_depth = context->current_depth;
		int			srf_depth;
		ListCell   *lc;

		item->expr = node;
		item->sortgroupref = context->current_sgref;

		context->current_input_vars = NIL;
		context->current_input_srfs = NIL;
		context->current_depth = 0;
		context->current_sgref = 0; /* subexpressions are not sortgroup items */

		(void) expression_tree_walker(node, split_pathtarget_walker, context);

		/* Depth is one more than any SRF below it */
		srf_depth = context->current_depth + 1;

		/* If new record depth, initialize another level of output lists */
		if (srf_depth >= list_length(context->level_srfs))
		{
			context->level_srfs = lappend(context->level_srfs, NIL);
			context->level_input_vars = lappend(context->level_input_vars, NIL);
			context->level_input_srfs = lappend(context->level_input_srfs, NIL);
		}

		/* Record this SRF as needing to be evaluated at appropriate level */
		lc = list_nth_cell(context->level_srfs, srf_depth);
		lfirst(lc) = lappend(lfirst(lc), item);

		/* Record its inputs as being needed at the same level */
		lc = list_nth_cell(context->level_input_vars, srf_depth);
		lfirst(lc) = list_concat(lfirst(lc), context->current_input_vars);
		lc = list_nth_cell(context->level_input_srfs, srf_depth);
		lfirst(lc) = list_concat(lfirst(lc), context->current_input_srfs);

		/*
		 * Restore caller-level state and update it for presence of this SRF.
		 * Notice we report the SRF itself as being needed for evaluation of
		 * surrounding expression.
		 */
		context->current_input_vars = save_input_vars;
		context->current_input_srfs = lappend(save_input_srfs, item);
		context->current_depth = Max(save_current_depth, srf_depth);

		/* We're done here */
		return false;
	}

	/*
	 * Otherwise, the node is a scalar (non-set) expression, so recurse to
	 * examine its inputs.
	 */
	context->current_sgref = 0; /* subexpressions are not sortgroup items */
	return expression_tree_walker(node, split_pathtarget_walker, context);
}

/*
 * Add a split_pathtarget_item to the PathTarget, unless a matching item is
 * already present.  This is like add_new_column_to_pathtarget, but allows
 * for sortgrouprefs to be handled.  An item having zero sortgroupref can
 * be merged with one that has a sortgroupref, acquiring the latter's
 * sortgroupref.
 *
 * Note that we don't worry about possibly adding duplicate sortgrouprefs
 * to the PathTarget.  That would be bad, but it should be impossible unless
 * the target passed to split_pathtarget_at_srfs already had duplicates.
 * As long as it didn't, we can have at most one split_pathtarget_item with
 * any particular nonzero sortgroupref.
 */
static void
add_sp_item_to_pathtarget(PathTarget *target, split_pathtarget_item *item)
{
	int			lci;
	ListCell   *lc;

	/*
	 * Look for a pre-existing entry that is equal() and does not have a
	 * conflicting sortgroupref already.
	 */
	lci = 0;
	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(target, lci);

		if ((item->sortgroupref == sgref ||
			 item->sortgroupref == 0 ||
			 sgref == 0) &&
			equal(item->expr, node))
		{
			/* Found a match.  Assign item's sortgroupref if it has one. */
			if (item->sortgroupref)
			{
				if (target->sortgrouprefs == NULL)
				{
					target->sortgrouprefs = (Index *)
						palloc0(list_length(target->exprs) * sizeof(Index));
				}
				target->sortgrouprefs[lci] = item->sortgroupref;
			}
			return;
		}
		lci++;
	}

	/*
	 * No match, so add item to PathTarget.  Copy the expr for safety.
	 */
	add_column_to_pathtarget(target, (Expr *) copyObject(item->expr),
							 item->sortgroupref);
}

/*
 * Apply add_sp_item_to_pathtarget to each element of list.
 */
static void
add_sp_items_to_pathtarget(PathTarget *target, List *items)
{
	ListCell   *lc;

	foreach(lc, items)
	{
		split_pathtarget_item *item = lfirst(lc);

		add_sp_item_to_pathtarget(target, item);
	}
}
