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
 * compat/assertop.c
 *	  Assert, as a CustomScan.
 *
 * What it does is Cloudberry's AssertOp
 * (github/cloudberry/src/backend/executor/nodeAssertOp.c): pass each row of
 * its child through, after testing it, and raise an error at the first row
 * that fails.  What differs is where it lives and what it says:
 *
 *	1. It is a CustomScan, because PostgreSQL 19 has no such node and a
 *	   module cannot add a node type.  See cb_assertop.h for the plan node.
 *	2. The error is the one the node was built with, as the message, not
 *	   Cloudberry's "one or more assertions failed" with the reason as the
 *	   detail.  The one assertion the port builds one for is ORCA's "at most
 *	   one row" over a scalar subquery it made a join, and the planner has an
 *	   error of its own for the same case (nodeSubplan.c); the query should
 *	   fail the same way whichever of the two planned it.
 *	3. Each test is its own expression, not one AND of all of them, so that a
 *	   NULL answer passes as Cloudberry's lets it: a NULL is not a failure.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "utils/ruleutils.h"

#include "cb_assertop.h"

typedef struct AssertOpState
{
	CustomScanState css;
	List	   *tests;			/* ExprState of each test */
	int			sqlerrcode;
	char	   *message;
} AssertOpState;

static Node *create_assert_state(CustomScan *cscan);
static void begin_assert(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *exec_assert(CustomScanState *node);
static void end_assert(CustomScanState *node);
static void rescan_assert(CustomScanState *node);
static void explain_assert(CustomScanState *node, List *ancestors,
						   ExplainState *es);

const CustomScanMethods gp_orca_assert_methods = {
	.CustomName = "Assert",
	.CreateCustomScanState = create_assert_state,
};

static const CustomExecMethods assert_exec_methods = {
	.CustomName = "Assert",
	.BeginCustomScan = begin_assert,
	.ExecCustomScan = exec_assert,
	.EndCustomScan = end_assert,
	.ReScanCustomScan = rescan_assert,
	.ExplainCustomScan = explain_assert,
};

void
gp_orca_register_assert(void)
{
	RegisterCustomScanMethods(&gp_orca_assert_methods);
}

static Node *
create_assert_state(CustomScan *cscan)
{
	AssertOpState *state = palloc0_object(AssertOpState);

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &assert_exec_methods;

	return (Node *) state;
}

static void
begin_assert(CustomScanState *node, EState *estate, int eflags)
{
	AssertOpState *state = (AssertOpState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	const char *sqlstate;
	ListCell   *lc;

	Assert(outerPlan(cscan) != NULL);
	Assert(list_length(cscan->custom_private) == 2);

	/*
	 * The child is the node's outer plan, which the executor leaves to the
	 * node to start.  ExecInitCustomScan built the projection before this
	 * ran, so it reads the child's slots without knowing their kind; that
	 * costs a little speed, not correctness.
	 */
	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);

	foreach(lc, cscan->custom_exprs)
		state->tests = lappend(state->tests,
							   ExecInitExpr((Expr *) lfirst(lc),
											&node->ss.ps));

	sqlstate = strVal(linitial(cscan->custom_private));
	Assert(strlen(sqlstate) == 5);
	state->sqlerrcode = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
									  sqlstate[3], sqlstate[4]);
	state->message = strVal(lsecond(cscan->custom_private));
}

static TupleTableSlot *
exec_assert(CustomScanState *node)
{
	AssertOpState *state = (AssertOpState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *slot;
	ListCell   *lc;

	slot = ExecProcNode(outerPlanState(node));
	if (TupIsNull(slot))
		return NULL;

	ResetExprContext(econtext);
	econtext->ecxt_outertuple = slot;

	foreach(lc, state->tests)
	{
		bool		isnull;
		Datum		passed;

		passed = ExecEvalExprSwitchContext((ExprState *) lfirst(lc),
										   econtext, &isnull);
		if (!isnull && !DatumGetBool(passed))
			ereport(ERROR,
					(errcode(state->sqlerrcode),
					 errmsg("%s", state->message)));
	}

	if (node->ss.ps.ps_ProjInfo != NULL)
		return ExecProject(node->ss.ps.ps_ProjInfo);

	/*
	 * No projection is built for an empty target list; the row the node
	 * returns then has no columns, as the node's result type says.
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	return ExecStoreVirtualTuple(node->ss.ss_ScanTupleSlot);
}

static void
end_assert(CustomScanState *node)
{
	ExecEndNode(outerPlanState(node));
}

static void
rescan_assert(CustomScanState *node)
{
	/*
	 * ExecReScan() has already passed a parameter change on to the child,
	 * which will rescan itself when next asked for a row.
	 */
	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

static void
explain_assert(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *context;
	bool		useprefix;
	char	   *tests;

	if (cscan->custom_exprs == NIL)
		return;

	/* as show_upper_qual() decides it, for a node over another */
	useprefix = (list_length(es->rtable) > 1 || es->verbose);
	context = set_deparse_context_plan(es->deparse_cxt, &cscan->scan.plan,
									   ancestors);
	tests = deparse_expression((Node *) make_ands_explicit(cscan->custom_exprs),
							   context, useprefix, false);

	ExplainPropertyText("Assert Cond", tests, es);
}
