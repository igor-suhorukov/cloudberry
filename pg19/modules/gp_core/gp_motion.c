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
 * gp_motion.c
 *	  ORCA's Gather Motion: a plan fragment carried out on the segments.
 *
 * Cloudberry's Motion is a node of its executor, and a slice below one is a
 * plan the dispatcher serializes and every segment runs, sending its rows up
 * through the interconnect.  Here the Motion is a CustomScan, and the slice
 * below it -- its outer plan, as ORCA built it -- travels as a PlannedStmt of
 * its own: the whole statement's range table, subplans and parameter types,
 * with the fragment as its tree.  Each segment receives it over the
 * dispatcher's ordinary libpq connection as
 *
 *     SELECT gp_internal.exec_fragment('<the PlannedStmt, as nodeToString>')
 *
 * behind a binary cursor, as every gather is (gp_dispatch.c); the segment's
 * planner_hook recognises the call, and instead of planning a function call
 * returns the fragment, which the cursor then runs.  The rows come back as
 * any gather's do.
 *
 * A Motion between segments -- Redistribute, Broadcast, a random
 * redistribution -- streams, as Cloudberry's interconnect does: every slice
 * below a Gather runs at the same time as the Gather's, the Gather's on the
 * writer, the session's backend on each segment, and each of the others on
 * a reader, one more backend of the session there, which reads as a part of
 * the writer's transaction (gp_share.c).  A reader's fragment is the Motion
 * it sends through: it pulls its slice's rows and sends each to the process
 * that runs the receiving slice on the segment its hash chooses, on every
 * segment, or on the next in turn, over the interconnect (gp_ic.c), and the
 * Motion in the receiving fragment takes them as they come -- each row a
 * tuple, as Cloudberry's interconnect sends it (motion_tuples()).  Which
 * slice receives each Motion the translator says (GpMotionSetParent).
 *
 * A Gather in a fragment a segment runs is one of these too: ORCA's Gather
 * to one segment, into a slice that runs there alone -- Cloudberry's
 * singleton reader, an aggregate of the subquery a DELETE compares with --
 * whose senders stream every row to the one process that runs it.
 *
 * Where a slice cannot stream -- the coordinator's own slice, a temporary
 * table, which only the session's own backend can read -- or with
 * gp.interconnect_type = relay, the Motion is carried out as it was first
 * built, before the Gather above it sends its fragment, by the coordinator:
 * the Motion's own fragment, the slice that sends, is gathered as any is,
 * and each row goes on to the segment its hash chooses, to every segment,
 * or to the next in turn, in batches of rows the receiving segment keeps in
 * a temporary file for the rest of the transaction
 * (gp_internal.motion_put()).  In the fragment the receiving slice runs, the
 * Motion reads that file.  That is a relay: the slices run one at a time,
 * and the rows cross the coordinator.  Which Motions a Gather has to carry
 * out first, and in what order, the translator works out and gives it
 * (GpMotionSetPrepare).
 *
 * A plan is carried out as it stands -- its permission checks are part of it
 * -- so a segment takes one only from the coordinator: a connection that is
 * dispatched and carries the cluster secret (gp_cluster.c).  Without a
 * secret, ORCA is told plans cannot be dispatched, and its plans with a
 * Motion fall back to the planner, whose gathers send SQL.
 *
 * On the coordinator the fragment is never run.  It is initialised only for
 * EXPLAIN, so that EXPLAIN prints it below the Motion as Cloudberry prints a
 * slice; its Vars are read through the Motion's custom_scan_tlist, which
 * names the fragment's columns as OUTER_VAR, so ruleutils finds them in the
 * outer plan as it would under a Motion.
 *
 * A sorted Motion -- Cloudberry's merge-receive -- merges the segments'
 * streams, each already in order, with a binary heap as MergeAppend does.
 * A sorted Gather into one segment, whose senders' rows come mixed on one
 * stream, sorts them there instead: the order the merge would give.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeSubplan.h"
#include "nodes/params.h"
#include "utils/datum.h"
#include "access/xact.h"
#include "common/pg_prng.h"
#include "lib/binaryheap.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "storage/buffile.h"
#include "storage/fileset.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "varatt.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_ic.h"
#include "gp_motion.h"
#include "gp_policy.h"
#include "gp_refresh.h"
#include "gp_scan.h"
#include "gp_settings.h"
#include "gp_share.h"

/*
 * custom_private, in order: the segment it reads from (-1 every one), the
 * slice it receives, and for a sorted Motion the sort keys -- columns of the
 * fragment's output, their ordering operators, collations and NULLS FIRST.
 */
#define MOTION_PRIVATE_CONTENT		0
#define MOTION_PRIVATE_SLICE		1
#define MOTION_PRIVATE_KEYS			2
#define MOTION_PRIVATE_SORTOPS		3
#define MOTION_PRIVATE_COLLATIONS	4
#define MOTION_PRIVATE_NULLSFIRST	5
#define MOTION_PRIVATE_TYPE			6	/* GP_MOTION_* */
#define MOTION_PRIVATE_HASHFUNCS	7	/* a Redistribute: its hash functions */
#define MOTION_PRIVATE_PREPARE		8	/* a Gather: slices it runs first */
#define MOTION_PRIVATE_PARENT		9	/* the slice that receives */
#define MOTION_PRIVATE_EXEC_PARAMS	10	/* values its fragment is sent with */
#define MOTION_PRIVATE_EXTERN_PARAMS	11	/* and statement parameters */
#define MOTION_PRIVATE_CONTENTS		12	/* direct dispatch's segments, if
										 * several */

/* A Motion whose receiving slice the translator did not say. */
#define MOTION_PARENT_UNKNOWN		(-3)

/*
 * How a Motion between segments is carried out: its slices all at once,
 * each sender streaming to its receivers over TCP (tcp) or in UDP packets,
 * acknowledged, as Cloudberry's udpifc sends them (udpifc) -- see gp_ic.c --
 * or a slice at a time, the rows relayed through the coordinator (relay).
 */
#define GP_INTERCONNECT_RELAY	0
#define GP_INTERCONNECT_TCP		1
#define GP_INTERCONNECT_UDPIFC	2

static const struct config_enum_entry interconnect_type_options[] = {
	{"relay", GP_INTERCONNECT_RELAY, false},
	{"tcp", GP_INTERCONNECT_TCP, false},
	{"udpifc", GP_INTERCONNECT_UDPIFC, false},
	{NULL, 0, false}
};

static int	gp_interconnect_type = GP_INTERCONNECT_TCP;

/*
 * On a fragment's PlannedStmt, where the coordinator runs its slices at
 * once: the statement's token and, for each slice that streams, how many
 * send and where its receivers are (GP_STREAM_MARK); and for the writer's
 * fragment, the key its readers find its snapshot under (GP_SHARE_MARK).
 */
#define GP_STREAM_MARK	"gp_stream"
#define GP_SHARE_MARK	"gp_share"

/* The slice table ORCA's translator keeps in the plan; see compat/cb_motion.h. */
#define GP_SLICE_TABLE	"gp_slice_table"

/*
 * On a fragment's PlannedStmt: the values of the parameters it reads that
 * nothing in it sets -- (PARAM_EXEC or PARAM_EXTERN, id, Const) each -- as
 * the coordinator had them when it sent the fragment.
 */
#define GP_PARAMS_MARK	"gp_params"

/* One slice that streams, as the coordinator plans it. */
typedef struct StreamSlice
{
	CustomScan *motion;			/* the Motion it sends to */
	int			slice;
	int			parent;			/* the slice that receives */
	int			ncontents;		/* the segments that send */
	int		   *contents;
	int		   *readers;		/* each one's reader in the GpStream */
	const char **addresses;		/* and where that reader receives */
} StreamSlice;

/*
 * The SQL a batch of a Motion's rows travels to its receiving segment in: to
 * the writer's own files, or to files its reader opens (motion_put_shared).
 */
#define MOTION_PUT_SQL	"SELECT gp_internal.motion_put($1, $2, $3)"
#define MOTION_PUT_SHARED_SQL	"SELECT gp_internal.motion_put_shared($1, $2, $3)"

/* How much of a segment's rows the coordinator holds before sending them. */
#define MOTION_BATCH_BYTES	(256 * 1024)

typedef struct MotionState
{
	CustomScanState css;

	int			content;		/* -1: every segment, or these: */
	int			ncontents;		/* direct dispatch's, when several */
	int		   *contents;
	int			slice;
	int			nkeys;			/* 0: not sorted */
	AttrNumber *keys;
	SortSupport sortkeys;

	GpGatherState *gather;
	bool		done;

	int			type;			/* GP_MOTION_* */

	/* A Gather: the name its Motions' rows are kept under on the segments. */
	char	   *key;
	bool		prepared;

	/* On a segment, a Motion that receives: the rows the coordinator sent. */
	bool		receiving;
	BufFile    *file;
	off_t		offset;			/* where the next row is */
	int			fileno;
	bool		binary;
	FmgrInfo   *inprocs;
	Oid		   *inparams;

	/* The coordinator, streaming: the slices below, and their readers. */
	bool		streaming;
	List	   *stream_slices;	/* StreamSlice */
	GpStream   *stream;

	/* On a segment, the Motion a reader's fragment is: it sends. */
	bool		sending;
	bool		send_tuples;	/* rows as tuples (motion_tuples()) */
	bool		send_binary;
	FmgrInfo   *outprocs;
	ExprState **hashexprs;
	GpHash		hash;
	int		   *receiver_of;	/* by content id: a receiver's index, or -1 */
	int			nreceivers;
	char	  **receivers;
	char	   *token;

	bool		file_own;		/* a reader's, of its writer's files: closed here */

	/* On a segment, a Motion that receives a streaming slice. */
	bool		streamed;
	bool		stream_here;	/* and this process is one of its receivers */
	bool		recv_tuples;	/* rows as tuples (motion_tuples()) */
	int			nsenders;
	GpIcReceiver *icrecv;
	bool		stream_done;
	Tuplestorestate *spool;		/* what came, for a rescan */
	TupleTableSlot *spoolslot;
	bool		replaying;

	/* On a segment, a sorted Gather into it: what came, sorted. */
	Tuplesortstate *sort;
	TupleTableSlot *sortslot;

	/* A merge: each segment's next row, and which of them is least. */
	int			nsegs;
	TupleTableSlot **segslots;
	TupleTableSlot *receive;	/* what a row is received into first */
	binaryheap *heap;
	bool		merging;
	int			last;			/* the segment whose row went out last */

	/* A write: carried out, and the rows its RETURNING gave, to hand out. */
	bool		written;
	Tuplestorestate *returned;
	TupleTableSlot *returnedslot;
} MotionState;

static Node *motion_create_state(CustomScan *cscan);
static void motion_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *motion_exec(CustomScanState *node);
static void motion_end(CustomScanState *node);
static void motion_rescan(CustomScanState *node);
static void motion_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomScanMethods motion_scan_methods = {
	.CustomName = GP_MOTION_NAME,
	.CreateCustomScanState = motion_create_state,
};

static const CustomExecMethods motion_exec_methods = {
	.CustomName = GP_MOTION_NAME,
	.BeginCustomScan = motion_begin,
	.ExecCustomScan = motion_exec,
	.EndCustomScan = motion_end,
	.ReScanCustomScan = motion_rescan,
	.ExplainCustomScan = motion_explain,
};

static const CustomExecMethods hash_filter_exec_methods;

static planner_hook_type prev_planner = NULL;
static explain_node_label_hook_type prev_explain_node_label = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;

/* How a fragment's PlannedStmt says it is one, on the segment that runs it. */
#define GP_FRAGMENT_MARK	"gp_fragment"

/*
 * The coordinator's text of the statement a fragment is part of, which the
 * segment process shows as its query while it runs the fragment, as
 * Cloudberry's shows the dispatcher's (pg_stat_activity); at most what
 * track_activity_query_size keeps of it.
 */
#define GP_SOURCE_MARK	"gp_source"

/* How many fragments this segment process is running, one inside another. */
static int	fragment_depth = 0;

/* ------------------------------------------------------------------------- */
/* Building one, for ORCA's translator                                       */
/* ------------------------------------------------------------------------- */

/* The Motion's own expressions read its scan tuple, not an outer plan. */
static Node *
outer_to_index_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *var = (Var *) copyObject(node);

		var->varno = INDEX_VAR;
		return (Node *) var;
	}
	return expression_tree_mutator(node, outer_to_index_mutator, context);
}

/* The oid of gp_internal.exec_fragment(text), or InvalidOid. */
static Oid
exec_fragment_oid(void)
{
	Oid			argtypes[2] = {TEXTOID, TEXTOID};

	return LookupFuncName(list_make2(makeString("gp_internal"),
									 makeString("exec_fragment")),
						  2, argtypes, true);
}

bool
GpMotionCanDispatchPlans(void)
{
	return GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!GpDispatchIsRecording() &&
		GpClusterHasSecret() &&
		OidIsValid(exec_fragment_oid());
}

static CustomScan *motion_make(int type, Plan *fragment, List *targetlist,
							   List *qual, int content, int slice);

Plan *
GpMotionMakeGather(Plan *fragment, List *targetlist, List *qual,
				   int content, int slice, int nkeys,
				   const AttrNumber *keys, const Oid *sortops,
				   const Oid *collations, const bool *nullsfirst)
{
	CustomScan *cscan = motion_make(GP_MOTION_GATHER, fragment, targetlist,
									qual, content, slice);
	List	   *keylist = NIL;
	List	   *oplist = NIL;
	List	   *colllist = NIL;
	List	   *nflist = NIL;

	/*
	 * A sort key is a column of the Motion's output; the merge compares the
	 * fragment's rows, so each has to be a column of those, passed through.
	 */
	for (int i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = get_tle_by_resno(cscan->scan.plan.targetlist,
											keys[i]);

		if (tle == NULL || !IsA(tle->expr, Var) ||
			((Var *) tle->expr)->varno != INDEX_VAR)
			return NULL;
		keylist = lappend_int(keylist, ((Var *) tle->expr)->varattno);
		oplist = lappend_oid(oplist, sortops[i]);
		colllist = lappend_oid(colllist, collations[i]);
		nflist = lappend_int(nflist, nullsfirst[i] ? 1 : 0);
	}

	list_nth_cell(cscan->custom_private, MOTION_PRIVATE_KEYS)->ptr_value = keylist;
	list_nth_cell(cscan->custom_private, MOTION_PRIVATE_SORTOPS)->ptr_value = oplist;
	list_nth_cell(cscan->custom_private, MOTION_PRIVATE_COLLATIONS)->ptr_value = colllist;
	list_nth_cell(cscan->custom_private, MOTION_PRIVATE_NULLSFIRST)->ptr_value = nflist;
	return (Plan *) cscan;
}

Plan *
GpMotionMakeSend(int type, Plan *fragment, List *targetlist, List *qual,
				 int content, int slice, List *hashexprs, List *hashfuncs)
{
	CustomScan *cscan;

	Assert(type == GP_MOTION_HASH || type == GP_MOTION_BROADCAST ||
		   type == GP_MOTION_RANDOM || type == GP_MOTION_EXPLICIT);
	Assert(list_length(hashexprs) == list_length(hashfuncs));

	cscan = motion_make(type, fragment, targetlist, qual, content, slice);

	/* The hash expressions read the fragment's output, as ORCA made them. */
	cscan->custom_exprs = hashexprs;
	list_nth_cell(cscan->custom_private, MOTION_PRIVATE_HASHFUNCS)->ptr_value =
		hashfuncs;
	return (Plan *) cscan;
}

static CustomScan *
motion_make(int type, Plan *fragment, List *targetlist, List *qual,
			int content, int slice)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *scan_tlist = NIL;
	List	   *tlist;
	ListCell   *lc;

	foreach(lc, fragment->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *var = makeVar(OUTER_VAR, tle->resno,
								  exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) var,
											 list_length(scan_tlist) + 1,
											 tle->resname, false));
	}

	tlist = (List *) outer_to_index_mutator((Node *) targetlist, NULL);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = (List *) outer_to_index_mutator((Node *) qual, NULL);
	cscan->scan.plan.lefttree = fragment;
	cscan->scan.scanrelid = 0;
	cscan->flags = 0;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_relids = NULL;
	cscan->custom_private = list_make4(makeInteger(content),
									   makeInteger(slice), NIL, NIL);
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* collations */
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* nulls first */
	cscan->custom_private = lappend(cscan->custom_private, makeInteger(type));
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* hash functions */
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* to prepare */
	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(MOTION_PARENT_UNKNOWN));
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* PARAM_EXEC */
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* PARAM_EXTERN */
	cscan->custom_private = lappend(cscan->custom_private, NIL);	/* segments */
	cscan->methods = &motion_scan_methods;

	return cscan;
}

Plan *
GpMotionMakeDml(Plan *modify, int content, int slice)
{
	List	   *tlist = NIL;

	Assert(IsA(modify, ModifyTable) || GpSplitModifyIs(modify, NULL));

	/*
	 * A write with RETURNING sends its rows up, as a Gather's fragment does:
	 * the Motion's columns are the ModifyTable's, which are its RETURNING
	 * list's, as setrefs.c makes them.  Without it the ModifyTable has none,
	 * and neither has the Motion.
	 */
	foreach_node(TargetEntry, tle, modify->targetlist)
	{
		Var		   *var = makeVar(OUTER_VAR, tle->resno,
								  exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);

		tlist = lappend(tlist, makeTargetEntry((Expr *) var, tle->resno,
											   tle->resname, false));
	}
	return (Plan *) motion_make(GP_MOTION_DML, modify, tlist, NIL, content,
								slice);
}

int
GpMotionType(Plan *plan)
{
	Assert(GpMotionIs(plan));
	return intVal(list_nth(((CustomScan *) plan)->custom_private,
						   MOTION_PRIVATE_TYPE));
}

int
GpMotionSlice(Plan *plan)
{
	Assert(GpMotionIs(plan));
	return intVal(list_nth(((CustomScan *) plan)->custom_private,
						   MOTION_PRIVATE_SLICE));
}

void
GpMotionSetPrepare(Plan *plan, List *slices)
{
	Assert(GpMotionIs(plan) &&
		   (GpMotionType(plan) == GP_MOTION_GATHER ||
			GpMotionType(plan) == GP_MOTION_DML));
	list_nth_cell(((CustomScan *) plan)->custom_private,
				  MOTION_PRIVATE_PREPARE)->ptr_value = slices;
}

void
GpMotionSetParent(Plan *plan, int parent)
{
	Assert(GpMotionIs(plan));
	intVal(list_nth(((CustomScan *) plan)->custom_private,
					MOTION_PRIVATE_PARENT)) = parent;
}

void
GpMotionSetParams(Plan *plan, List *exec_params, List *extern_params)
{
	List	   *priv = ((CustomScan *) plan)->custom_private;

	Assert(GpMotionIs(plan));
	list_nth_cell(priv, MOTION_PRIVATE_EXEC_PARAMS)->ptr_value = exec_params;
	list_nth_cell(priv, MOTION_PRIVATE_EXTERN_PARAMS)->ptr_value = extern_params;
}

int
GpMotionParent(Plan *plan)
{
	Assert(GpMotionIs(plan));
	return intVal(list_nth(((CustomScan *) plan)->custom_private,
						   MOTION_PRIVATE_PARENT));
}

bool
GpMotionIs(Plan *plan)
{
	return plan != NULL && IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &motion_scan_methods;
}

void
GpMotionSetSegment(Plan *plan, int content)
{
	CustomScan *cscan = (CustomScan *) plan;

	Assert(GpMotionIs(plan));
	intVal(list_nth(cscan->custom_private, MOTION_PRIVATE_CONTENT)) = content;
}

int
GpMotionSegment(Plan *plan)
{
	Assert(GpMotionIs(plan));
	return intVal(list_nth(((CustomScan *) plan)->custom_private,
						   MOTION_PRIVATE_CONTENT));
}

/*
 * Direct dispatch to several segments: a Gather or a write sent to these,
 * in the order Cloudberry's INFO line names them.  One is its segment.
 */
void
GpMotionSetSegments(Plan *plan, List *contents)
{
	List	   *priv = ((CustomScan *) plan)->custom_private;

	Assert(GpMotionIs(plan));
	if (list_length(contents) == 1)
	{
		GpMotionSetSegment(plan, linitial_int(contents));
		return;
	}
	list_nth_cell(priv, MOTION_PRIVATE_CONTENTS)->ptr_value = list_copy(contents);
}

List *
GpMotionSegments(Plan *plan)
{
	List	   *priv = ((CustomScan *) plan)->custom_private;

	Assert(GpMotionIs(plan));
	if (list_length(priv) <= MOTION_PRIVATE_CONTENTS)
		return NIL;
	return (List *) list_nth(priv, MOTION_PRIVATE_CONTENTS);
}

/*
 * Direct dispatch, for ORCA: the segment that holds every row whose
 * distribution key is these values, or -1.  The values are the key's, in the
 * key's order, each of a type its column's hash family hashes, as a
 * constant compared to the column by the family's equality is: 1::int4 finds
 * an int2 key's segment (GpHashSegmentForKey).
 */
int
GpMotionDirectDispatchSegment(Oid relid, int nvalues, const Oid *types,
							  const Datum *values, const bool *isnull)
{
	GpPolicy   *policy;

	if (!gp_enable_direct_dispatch)
		return -1;
	policy = GpPolicyGet(relid);
	if (policy == NULL || !GpPolicyIsHashPartitioned(policy) ||
		policy->nattrs != nvalues)
		return -1;
	return GpHashSegmentForKey(policy, types, values, isnull);
}

/* ------------------------------------------------------------------------- */
/* Carrying it out, on the coordinator                                       */
/* ------------------------------------------------------------------------- */

static Node *
motion_create_state(CustomScan *cscan)
{
	MotionState *state = (MotionState *) newNode(sizeof(MotionState),
												 T_CustomScanState);

	state->css.methods = &motion_exec_methods;
	return (Node *) state;
}

/* ------------------------------------------------------------------------- */
/* The rows a segment receives                                               */
/* ------------------------------------------------------------------------- */

/*
 * A Motion's rows on the segment that receives them: a temporary file per
 * statement and slice, for the rest of the transaction, since the slice that
 * reads them runs later and may run again.  The files belong to the
 * transaction's resource owner, which removes them if it aborts.
 */
typedef struct MotionFile
{
	char	   *key;
	int			slice;
	BufFile    *file;
	int			endfile;		/* where the rows end, which BufFileSeek's */
	off_t		endoffset;		/* SEEK_END does not know while buffered */
} MotionFile;

static List *motion_files = NIL;	/* in TopTransactionContext */
static bool motion_xact_callback_registered = false;

/*
 * The statements whose rows a reader of this writer's transaction receives,
 * by their keys: files of a FileSet the reader finds by the writer's process
 * ID and the key (motion_fileset()), removed with the statement's rows, or
 * as the transaction ends.  In TopMemoryContext.
 */
static List *motion_filesets = NIL;

static MotionFile *
motion_file_find(const char *key, int slice)
{
	ListCell   *lc;

	foreach(lc, motion_files)
	{
		MotionFile *mf = (MotionFile *) lfirst(lc);

		if (mf->slice == slice && strcmp(mf->key, key) == 0)
			return mf;
	}
	return NULL;
}

/*
 * In place: the list is the transaction's, and a new one made here would be
 * in the memory of the function call that asked, gone when it returns.
 */
static void
motion_files_close(const char *key)
{
	ListCell   *lc;

	foreach(lc, motion_files)
	{
		MotionFile *mf = (MotionFile *) lfirst(lc);

		if (key == NULL || strcmp(mf->key, key) == 0)
		{
			BufFileClose(mf->file);
			motion_files = foreach_delete_current(motion_files, lc);
		}
	}
}

/*
 * The FileSet of a statement's rows that a reader of "writer_pid" receives:
 * numbered by the key's counter above any number FileSetInit() gives, in the
 * database's default tablespace, so that writer and reader name it alike.
 */
static void
motion_fileset(FileSet *fileset, int writer_pid, const char *key)
{
	const char *counter = strrchr(key, '_');

	memset(fileset, 0, sizeof(FileSet));
	fileset->creator_pid = writer_pid;
	fileset->number = 0x80000000U |
		(uint32) (counter != NULL ? strtoul(counter + 1, NULL, 10) : 0);
	fileset->ntablespaces = 1;
	fileset->tablespaces[0] = MyDatabaseTableSpace;
}

/* A statement's files for readers, or all of them (NULL), removed. */
static void
motion_filesets_delete(const char *key)
{
	ListCell   *lc;

	foreach(lc, motion_filesets)
	{
		char	   *k = (char *) lfirst(lc);
		FileSet		fileset;

		if (key != NULL && strcmp(k, key) != 0)
			continue;
		motion_fileset(&fileset, MyProcPid, k);
		FileSetDeleteAll(&fileset);
		motion_filesets = foreach_delete_current(motion_filesets, lc);
		pfree(k);
	}
}

static void
motion_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			/* closed here, rather than warned about as leaked at commit */
			motion_files_close(NULL);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/* an abort's resource owner closes the files */
			motion_files = NIL;
			motion_filesets_delete(NULL);
			break;
	}
}

/* The key a fragment's Motions' rows are kept under, from its PlannedStmt. */
static char *
fragment_key(PlannedStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->extension_state)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (strcmp(def->defname, GP_FRAGMENT_MARK) == 0 && def->arg != NULL)
			return strVal(def->arg);
	}
	return "";
}

PG_FUNCTION_INFO_V1(gp_motion_put);

/*
 * gp_internal.motion_put(key, slice, rows)
 *
 * A batch of a Motion's rows, relayed by the coordinator, added to the ones
 * this segment has for that statement and slice.  Only from the coordinator:
 * the rows are read as the plan's own.
 */
Datum
gp_motion_put(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			slice = PG_GETARG_INT32(1);
	bytea	   *rows = PG_GETARG_BYTEA_PP(2);
	MotionFile *mf;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a Motion's rows are taken only from the coordinator"),
				 errdetail("The connection does not carry this cluster's secret.")));

	mf = motion_file_find(key, slice);
	if (mf == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);
		ResourceOwner oldowner = CurrentResourceOwner;

		mf = palloc0(sizeof(MotionFile));
		mf->key = pstrdup(key);
		mf->slice = slice;
		CurrentResourceOwner = TopTransactionResourceOwner;
		mf->file = BufFileCreateTemp(false);
		CurrentResourceOwner = oldowner;
		motion_files = lappend(motion_files, mf);
		MemoryContextSwitchTo(oldcxt);
	}

	if (BufFileSeek(mf->file, mf->endfile, mf->endoffset, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to the end of a Motion's rows: %m")));
	BufFileWrite(mf->file, VARDATA_ANY(rows), VARSIZE_ANY_EXHDR(rows));
	BufFileTell(mf->file, &mf->endfile, &mf->endoffset);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_motion_put_shared);

/*
 * gp_internal.motion_put_shared(key, slice, rows)
 *
 * A batch of a Motion's rows, relayed by the coordinator to this segment for
 * a reader of its transaction, which cannot open the writer's own temporary
 * files: added to a file of the statement's FileSet, closed again so that it
 * is whole on disk when the reader opens it.  Only from the coordinator.
 */
Datum
gp_motion_put_shared(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			slice = PG_GETARG_INT32(1);
	bytea	   *rows = PG_GETARG_BYTEA_PP(2);
	FileSet		fileset;
	char		name[32];
	BufFile    *file;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a Motion's rows are taken only from the coordinator"),
				 errdetail("The connection does not carry this cluster's secret.")));

	motion_fileset(&fileset, MyProcPid, key);
	snprintf(name, sizeof(name), "slice%d", slice);
	file = BufFileOpenFileSet(&fileset, name, O_RDWR, true);
	if (file == NULL)
	{
		bool		known = false;

		foreach_ptr(char, k, motion_filesets)
			if (strcmp(k, key) == 0)
				known = true;
		if (!known)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

			motion_filesets = lappend(motion_filesets, pstrdup(key));
			MemoryContextSwitchTo(oldcxt);
		}
		file = BufFileCreateFileSet(&fileset, name);
	}
	else if (BufFileSeek(file, 0, 0, SEEK_END) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to the end of a Motion's rows: %m")));
	BufFileWrite(file, VARDATA_ANY(rows), VARSIZE_ANY_EXHDR(rows));
	BufFileClose(file);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_motion_drop);

/*
 * gp_internal.motion_drop(key)
 *
 * The statement is done with its Motions' rows: the coordinator's word, at
 * the end of the Gather that had them sent.
 */
Datum
gp_motion_drop(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a Motion's rows are dropped only for the coordinator")));
	motion_files_close(key);
	motion_filesets_delete(key);
	PG_RETURN_VOID();
}

/* Read exactly len bytes of a received row; false at the end of the rows. */
static bool
motion_read(MotionState *state, void *ptr, size_t len, bool eof_ok)
{
	size_t		got = BufFileReadMaybeEOF(state->file, ptr, len, eof_ok);

	if (got == 0 && eof_ok)
		return false;
	if (got != len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a Motion's rows end in the middle of a row")));
	return true;
}

static TupleTableSlot *
motion_recv_next(MotionState *state)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	MotionFile *mf;
	uint16		natts;
	MemoryContext oldcxt;

	if (state->file == NULL)
	{
		mf = motion_file_find(state->key, state->slice);
		if (mf != NULL)
			state->file = mf->file;
		else if (GpShareIsReader())
		{
			/* a reader's: the files its writer keeps them in for it */
			FileSet    *fileset;
			char		name[32];

			oldcxt = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
			fileset = palloc(sizeof(FileSet));
			motion_fileset(fileset, GpShareWriterPid(), state->key);
			snprintf(name, sizeof(name), "slice%d", state->slice);
			state->file = BufFileOpenFileSet(fileset, name, O_RDONLY, true);
			MemoryContextSwitchTo(oldcxt);
			state->file_own = state->file != NULL;
		}
		if (state->file == NULL)
			return ExecClearTuple(slot);	/* nothing was sent here */
		state->fileno = 0;
		state->offset = 0;
	}

	/* Another reader of the file may have moved it: back to where we were. */
	if (BufFileSeek(state->file, state->fileno, state->offset, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a Motion's rows: %m")));

	if (!motion_read(state, &natts, sizeof(natts), true))
		return ExecClearTuple(slot);
	natts = pg_ntoh16(natts);
	if (natts != tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a Motion's row has %d columns, not %d",
						natts, tupdesc->natts)));

	ExecClearTuple(slot);
	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	for (int i = 0; i < natts; i++)
	{
		int32		len;
		char	   *data;

		motion_read(state, &len, sizeof(len), false);
		len = (int32) pg_ntoh32((uint32) len);
		if (len < 0)
		{
			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			continue;
		}
		data = palloc(len + 1);
		if (len > 0)
			motion_read(state, data, len, false);
		data[len] = '\0';
		slot->tts_isnull[i] = false;
		if (state->binary)
		{
			StringInfoData buf;

			initReadOnlyStringInfo(&buf, data, len);
			slot->tts_values[i] = ReceiveFunctionCall(&state->inprocs[i], &buf,
													  state->inparams[i],
													  TupleDescAttr(tupdesc, i)->atttypmod);
		}
		else
			slot->tts_values[i] = InputFunctionCall(&state->inprocs[i], data,
													state->inparams[i],
													TupleDescAttr(tupdesc, i)->atttypmod);
	}
	MemoryContextSwitchTo(oldcxt);

	BufFileTell(state->file, &state->fileno, &state->offset);
	return ExecStoreVirtualTuple(slot);
}

/* ------------------------------------------------------------------------- */
/* Streaming, on a segment                                                   */
/* ------------------------------------------------------------------------- */

static bool motion_tuples(TupleDesc tupdesc);

/* A DefElem of a fragment's PlannedStmt, by name. */
static Node *
fragment_mark(PlannedStmt *stmt, const char *name)
{
	ListCell   *lc;

	foreach(lc, stmt->extension_state)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (strcmp(def->defname, name) == 0)
			return def->arg;
	}
	return NULL;
}

/*
 * What the coordinator said of a slice that streams: (slice, senders,
 * receivers' contents, receivers' addresses); NULL if it does not.
 */
static List *
stream_entry(PlannedStmt *stmt, int slice)
{
	List	   *info = (List *) fragment_mark(stmt, GP_STREAM_MARK);
	ListCell   *lc;

	if (info == NULL)
		return NULL;
	foreach(lc, (List *) lsecond(info))
	{
		List	   *entry = (List *) lfirst(lc);

		if (intVal(linitial(entry)) == slice)
			return entry;
	}
	return NULL;
}

static char *
stream_token(PlannedStmt *stmt)
{
	return strVal(linitial((List *) fragment_mark(stmt, GP_STREAM_MARK)));
}

/* Do its slices stream in UDP packets (udpifc)? */
static bool
stream_udp(PlannedStmt *stmt)
{
	return boolVal(lthird((List *) fragment_mark(stmt, GP_STREAM_MARK)));
}

/*
 * Is this process one of a streaming slice's receivers?  A fragment carries
 * every subplan of the statement, the ones below other slices' Motions too,
 * whose Motions this process never reads.
 */
static bool
stream_receives(PlannedStmt *stmt, List *entry)
{
	const char *self = GpIcAddressOf(GpIcAddress(), stream_udp(stmt));

	foreach_node(String, address, (List *) lfourth(entry))
		if (strcmp(strVal(address), self) == 0)
			return true;
	return false;
}

/*
 * The Motion at the top of a reader's fragment: it pulls the rows of the
 * slice below it, and sends each where the Motion sends it -- the segment
 * its keys hash to, every one, the next in turn, the one a Gather gathers to
 * -- to the process that runs the receiving slice there.
 */
static void
motion_begin_sending(MotionState *state, EState *estate, int eflags,
					 List *entry)
{
	CustomScan *cscan = (CustomScan *) state->css.ss.ps.plan;
	List	   *hashfuncs = (List *) list_nth(cscan->custom_private,
											  MOTION_PRIVATE_HASHFUNCS);
	List	   *contents = (List *) lthird(entry);
	List	   *addresses = (List *) lfourth(entry);
	int			nsegs = GpClusterSegmentCount();
	TupleDesc	tupdesc;
	int			nkeys = list_length(cscan->custom_exprs);
	int			i;
	ListCell   *lc,
			   *lf;

	state->sending = true;
	state->token = stream_token(estate->es_plannedstmt);
	outerPlanState(state) = ExecInitNode(outerPlan(cscan), estate, eflags);
	tupdesc = ExecGetResultType(outerPlanState(state));

	state->send_tuples = motion_tuples(tupdesc);
	state->send_binary = GpTupleDescHasBinaryIO(tupdesc);
	state->outprocs = palloc0_array(FmgrInfo, tupdesc->natts);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Oid			proc;
		bool		isvarlena;

		if (state->send_binary)
			getTypeBinaryOutputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
									&proc, &isvarlena);
		else
			getTypeOutputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
							  &proc, &isvarlena);
		fmgr_info(proc, &state->outprocs[i]);
	}

	/* A Redistribute hashes its keys as cdbhash hashes a table's. */
	memset(&state->hash, 0, sizeof(GpHash));
	state->hash.ptype = POLICYTYPE_PARTITIONED;
	state->hash.numsegs = nsegs;
	state->hash.nattrs = nkeys;
	state->hash.attrs = palloc_array(AttrNumber, Max(nkeys, 1));
	state->hash.hashfuncs = palloc_array(FmgrInfo, Max(nkeys, 1));
	state->hashexprs = palloc_array(ExprState *, Max(nkeys, 1));
	i = 0;
	forboth(lc, cscan->custom_exprs, lf, hashfuncs)
	{
		state->hashexprs[i] = ExecInitExpr((Expr *) lfirst(lc),
										   &state->css.ss.ps);
		state->hash.attrs[i] = i + 1;
		if (OidIsValid(lfirst_oid(lf)))
			GpHashSetFunction(&state->hash, i, lfirst_oid(lf));
		i++;
	}

	/* The receivers, and which of them is on each segment. */
	state->nreceivers = list_length(addresses);
	state->receivers = palloc_array(char *, Max(state->nreceivers, 1));
	state->receiver_of = palloc_array(int, nsegs);
	for (i = 0; i < nsegs; i++)
		state->receiver_of[i] = -1;
	i = 0;
	forboth(lc, contents, lf, addresses)
	{
		int			content = intVal(lfirst(lc));

		if (content >= 0 && content < nsegs)
			state->receiver_of[content] = i;
		state->receivers[i++] = strVal(lfirst(lf));
	}
}

/* One row, as it travels: a count, then each value; see append_row_raw(). */
static void append_row_raw(StringInfo buf, int natts, const char **values,
						   const int *lengths);

/*
 * Does a streaming slice send its rows as tuples, as Cloudberry's
 * interconnect does (tupser.c): each row a MinimalTuple, its bytes as they
 * are, every value that lives outside it -- in the table's TOAST relation, or
 * expanded -- brought in first, since the receiver can read neither.  Every
 * process of the cluster is the same build, so a value's bytes mean the same
 * in any of them -- but an anonymous record's, which carries a type the
 * sending process made up (its typmod), where Cloudberry remaps it
 * (tupleremap.c): a row with one travels as each value's own text or binary
 * form, as the relay's do.  Sender and receiver ask this of the same row
 * type, and agree.
 */
static bool
motion_tuples(TupleDesc tupdesc)
{
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Oid			type = getBaseType(TupleDescAttr(tupdesc, i)->atttypid);

		if (type == RECORDOID || type == RECORDARRAYOID)
			return false;
	}
	return true;
}

/* A row as a tuple, into "buf": its values brought in, then formed. */
static void
motion_tuple(TupleTableSlot *slot, StringInfo buf)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	Datum	   *values = palloc_array(Datum, Max(tupdesc->natts, 1));
	MinimalTuple tuple;

	slot_getallattrs(slot);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		values[i] = slot->tts_values[i];
		if (!slot->tts_isnull[i] && TupleDescAttr(tupdesc, i)->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			values[i] = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(values[i])));
	}
	tuple = heap_form_minimal_tuple(tupdesc, values, slot->tts_isnull, 0);
	appendBinaryStringInfo(buf, (char *) tuple, tuple->t_len);
}

/*
 * An Explicit Redistribute's segment for a row: the gp_segment_id it
 * carries, which is the segment it was read on, and is to be written on.
 */
static int
explicit_target(Datum value, bool isnull, int nsegs)
{
	int			target = isnull ? -1 : DatumGetInt32(value);

	if (target < 0 || target >= nsegs)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("an Explicit Redistribute Motion's row names segment %d, which the cluster does not have",
						target)));
	return target;
}

static void
motion_send_all(MotionState *state)
{
	PlanState  *child = outerPlanState(state);
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	TupleDesc	tupdesc = ExecGetResultType(child);
	int			natts = tupdesc->natts;
	int			nsegs = GpClusterSegmentCount();
	int			nkeys = state->hash.nattrs;
	const char **values = palloc_array(const char *, Max(natts, 1));
	int		   *lengths = palloc_array(int, Max(natts, 1));
	Datum	   *keyvalues = palloc_array(Datum, Max(nkeys, 1));
	bool	   *keynulls = palloc_array(bool, Max(nkeys, 1));
	int			next = (int) (pg_prng_uint32(&pg_global_prng_state) % nsegs);
	StringInfoData row;
	GpIcSender *sender;

	initStringInfo(&row);
	sender = GpIcSendBegin(state->token, state->slice, GpClusterContentId(),
						   state->nreceivers, state->receivers);

	/* Until the rows end, or no receiver wants more: a LIMIT above them. */
	while (GpIcSendWanted(sender))
	{
		TupleTableSlot *slot = ExecProcNode(child);
		MemoryContext oldcxt;
		int			target = -1;

		if (TupIsNull(slot))
			break;
		slot_getallattrs(slot);

		ResetExprContext(econtext);
		oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
		resetStringInfo(&row);
		if (state->send_tuples)
			motion_tuple(slot, &row);
		else
		{
			for (int i = 0; i < natts; i++)
			{
				if (slot->tts_isnull[i])
				{
					values[i] = NULL;
					lengths[i] = -1;
				}
				else if (state->send_binary)
				{
					bytea	   *b = SendFunctionCall(&state->outprocs[i],
													 slot->tts_values[i]);

					values[i] = VARDATA(b);
					lengths[i] = VARSIZE(b) - VARHDRSZ;
				}
				else
				{
					values[i] = OutputFunctionCall(&state->outprocs[i],
												   slot->tts_values[i]);
					lengths[i] = strlen(values[i]);
				}
			}
			append_row_raw(&row, natts, values, lengths);
		}

		if (state->type == GP_MOTION_HASH)
		{
			econtext->ecxt_outertuple = slot;
			for (int i = 0; i < nkeys; i++)
				keyvalues[i] = ExecEvalExpr(state->hashexprs[i], econtext,
											&keynulls[i]);
			target = GpHashSegment(&state->hash, keyvalues, keynulls);
		}
		else if (state->type == GP_MOTION_EXPLICIT)
		{
			econtext->ecxt_outertuple = slot;
			keyvalues[0] = ExecEvalExpr(state->hashexprs[0], econtext,
										&keynulls[0]);
			target = explicit_target(keyvalues[0], keynulls[0], nsegs);
		}
		else if (state->type == GP_MOTION_RANDOM)
			target = next++ % nsegs;
		MemoryContextSwitchTo(oldcxt);

		/* a Gather's receivers are the one process it gathers to */
		if (state->type == GP_MOTION_BROADCAST ||
			state->type == GP_MOTION_GATHER)
			GpIcSend(sender, -1, row.data, row.len);
		else if (state->receiver_of[target] >= 0)
			GpIcSend(sender, state->receiver_of[target], row.data, row.len);

		/*
		 * A row for a segment that runs no receiver is one the plan does not
		 * read there: direct dispatch sent the receiving slice to one
		 * segment, as the relay's rows for the others are never read.
		 */
	}

	GpIcSendEnd(sender);
	pfree(row.data);
}

/*
 * A row as it travels, into the scan slot -- a tuple taken apart into it, its
 * values in the row's memory, as the others' are.  The slot stays the
 * virtual one the Motion's parents compiled their expressions for.
 */
static TupleTableSlot *
motion_decode_row(MotionState *state, const char *data, int len)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	const char *p = data;
	const char *end = data + len;
	uint16		natts;
	MemoryContext oldcxt;

	if (state->recv_tuples)
	{
		MinimalTuple tuple;
		HeapTupleData htup;

		if (len < (int) SizeofMinimalTupleHeader)
			goto corrupt;
		tuple = (MinimalTuple) MemoryContextAlloc(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory,
												  len);
		memcpy(tuple, data, len);
		if (tuple->t_len != (uint32) len ||
			(tuple->t_infomask2 & HEAP_NATTS_MASK) > tupdesc->natts)
			goto corrupt;
		htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
		htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
		ExecClearTuple(slot);
		heap_deform_tuple(&htup, tupdesc, slot->tts_values, slot->tts_isnull);
		return ExecStoreVirtualTuple(slot);
	}

	if (len < (int) sizeof(uint16))
		goto corrupt;
	memcpy(&natts, p, sizeof(natts));
	p += sizeof(natts);
	natts = pg_ntoh16(natts);
	if (natts != tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a Motion's row has %d columns, not %d",
						natts, tupdesc->natts)));

	ExecClearTuple(slot);
	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	for (int i = 0; i < natts; i++)
	{
		int32		vlen;
		char	   *value;

		if (end - p < (int) sizeof(vlen))
			goto corrupt;
		memcpy(&vlen, p, sizeof(vlen));
		p += sizeof(vlen);
		vlen = (int32) pg_ntoh32((uint32) vlen);
		if (vlen < 0)
		{
			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			continue;
		}
		if (end - p < vlen)
			goto corrupt;
		value = palloc(vlen + 1);
		memcpy(value, p, vlen);
		value[vlen] = '\0';
		p += vlen;
		slot->tts_isnull[i] = false;
		if (state->binary)
		{
			StringInfoData buf;

			initReadOnlyStringInfo(&buf, value, vlen);
			slot->tts_values[i] = ReceiveFunctionCall(&state->inprocs[i], &buf,
													  state->inparams[i],
													  TupleDescAttr(tupdesc, i)->atttypmod);
		}
		else
			slot->tts_values[i] = InputFunctionCall(&state->inprocs[i], value,
													state->inparams[i],
													TupleDescAttr(tupdesc, i)->atttypmod);
	}
	MemoryContextSwitchTo(oldcxt);
	return ExecStoreVirtualTuple(slot);

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: a row of slice %d ends in the middle",
					state->slice)));
	return NULL;
}

/*
 * The next row a streaming slice sent this process.  What came is kept, so
 * that the Motion can be read again: Cloudberry's executor refuses to, and
 * ORCA puts a Materialize above one that would be, but a Materialize with
 * nothing to rewind asks the node below it again, and the relay answered.
 */
static TupleTableSlot *
motion_stream_next(MotionState *state)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	char	   *data;
	int			len;

	if (state->replaying)
	{
		if (tuplestore_gettupleslot(state->spool, true, false, state->spoolslot))
			return ExecCopySlot(slot, state->spoolslot);
		return ExecClearTuple(slot);
	}
	if (state->stream_done)
		return ExecClearTuple(slot);

	if (state->icrecv == NULL)
		state->icrecv = GpIcRecvBegin(state->token, state->slice,
									  state->nsenders,
									  stream_udp(state->css.ss.ps.state->es_plannedstmt));
	if (GpIcRecv(state->icrecv, &data, &len))
	{
		TupleTableSlot *row = motion_decode_row(state, data, len);

		if (state->spool != NULL)
			tuplestore_puttupleslot(state->spool, row);
		return row;
	}
	GpIcRecvEnd(state->icrecv);
	state->icrecv = NULL;
	state->stream_done = true;
	return ExecClearTuple(slot);
}

/*
 * A sorted Gather into this segment: every row its senders sent, mixed as
 * they came, sorted by its keys -- the order Cloudberry's merge of their
 * streams gives -- and kept, for a rescan.
 */
static TupleTableSlot *
motion_sorted_next(MotionState *state)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;

	if (state->sort == NULL)
	{
		List	   *priv = ((CustomScan *) state->css.ss.ps.plan)->custom_private;
		List	   *keys = (List *) list_nth(priv, MOTION_PRIVATE_KEYS);
		List	   *sortops = (List *) list_nth(priv, MOTION_PRIVATE_SORTOPS);
		List	   *colls = (List *) list_nth(priv, MOTION_PRIVATE_COLLATIONS);
		List	   *nfs = (List *) list_nth(priv, MOTION_PRIVATE_NULLSFIRST);
		AttrNumber *attnums = palloc_array(AttrNumber, state->nkeys);
		Oid		   *ops = palloc_array(Oid, state->nkeys);
		Oid		   *collations = palloc_array(Oid, state->nkeys);
		bool	   *nullsfirst = palloc_array(bool, state->nkeys);
		MemoryContext oldcxt;

		for (int i = 0; i < state->nkeys; i++)
		{
			attnums[i] = (AttrNumber) list_nth_int(keys, i);
			ops[i] = list_nth_oid(sortops, i);
			collations[i] = list_nth_oid(colls, i);
			nullsfirst[i] = list_nth_int(nfs, i) != 0;
		}
		oldcxt = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
		state->sort = tuplesort_begin_heap(slot->tts_tupleDescriptor,
										   state->nkeys, attnums, ops,
										   collations, nullsfirst, work_mem,
										   NULL, TUPLESORT_RANDOMACCESS);
		state->sortslot = MakeSingleTupleTableSlot(slot->tts_tupleDescriptor,
												   &TTSOpsMinimalTuple);
		MemoryContextSwitchTo(oldcxt);

		for (;;)
		{
			TupleTableSlot *row = state->streamed
				? motion_stream_next(state)
				: motion_recv_next(state);

			if (TupIsNull(row))
				break;
			tuplesort_puttupleslot(state->sort, row);
			ResetExprContext(state->css.ss.ps.ps_ExprContext);
		}
		tuplesort_performsort(state->sort);
	}

	if (tuplesort_gettupleslot(state->sort, true, false, state->sortslot, NULL))
		return ExecCopySlot(slot, state->sortslot);
	return ExecClearTuple(slot);
}

/*
 * A streaming slice's senders stop sending here -- the ones of a Motion never
 * read too, whose rows would wait for it, if they send them here.
 */
static void
motion_end_stream(MotionState *state)
{
	if (state->streamed && state->stream_here && state->icrecv == NULL &&
		!state->stream_done)
		state->icrecv = GpIcRecvBegin(state->token, state->slice,
									  state->nsenders,
									  stream_udp(state->css.ss.ps.state->es_plannedstmt));
	if (state->icrecv != NULL)
		GpIcRecvEnd(state->icrecv);
	state->icrecv = NULL;
	state->stream_done = true;
}

/* Every streaming slice a plan receives, ended. */
static bool
motion_end_streams(PlanState *planstate, void *context)
{
	if (planstate == NULL)
		return false;
	if (IsA(planstate, CustomScanState) &&
		((CustomScanState *) planstate)->methods == &motion_exec_methods)
		motion_end_stream((MotionState *) planstate);
	return planstate_tree_walker(planstate, motion_end_streams, context);
}

/*
 * A fragment the coordinator initialised only to describe it, under EXPLAIN
 * ANALYZE.  PostgreSQL 19's index scans allocate the counter of their index
 * searches only when they are to run, and EXPLAIN ANALYZE reads it without
 * asking -- which no plan of PostgreSQL's own meets, since it never mixes
 * the two.  Here they are given one, at zero, which is what the coordinator
 * counted: it ran them no times.
 */
static bool
fragment_instrument_walker(PlanState *planstate, void *context)
{
	if (planstate == NULL)
		return false;

	switch (nodeTag(planstate))
	{
		case T_IndexScanState:
			if (((IndexScanState *) planstate)->iss_Instrument == NULL)
				((IndexScanState *) planstate)->iss_Instrument =
					palloc0_object(IndexScanInstrumentation);
			break;
		case T_IndexOnlyScanState:
			if (((IndexOnlyScanState *) planstate)->ioss_Instrument == NULL)
				((IndexOnlyScanState *) planstate)->ioss_Instrument =
					palloc0_object(IndexScanInstrumentation);
			break;
		case T_BitmapIndexScanState:
			if (((BitmapIndexScanState *) planstate)->biss_Instrument == NULL)
				((BitmapIndexScanState *) planstate)->biss_Instrument =
					palloc0_object(IndexScanInstrumentation);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, fragment_instrument_walker, context);
}

static void
motion_begin(CustomScanState *node, EState *estate, int eflags)
{
	MotionState *state = (MotionState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	List	   *keys = (List *) list_nth(priv, MOTION_PRIVATE_KEYS);
	List	   *sortops = (List *) list_nth(priv, MOTION_PRIVATE_SORTOPS);
	List	   *colls = (List *) list_nth(priv, MOTION_PRIVATE_COLLATIONS);
	List	   *nfs = (List *) list_nth(priv, MOTION_PRIVATE_NULLSFIRST);

	state->content = intVal(list_nth(priv, MOTION_PRIVATE_CONTENT));
	state->contents = palloc_array(int, Max(list_length(GpMotionSegments((Plan *) cscan)), 1));
	foreach_int(c, GpMotionSegments((Plan *) cscan))
		state->contents[state->ncontents++] = c;
	state->slice = intVal(list_nth(priv, MOTION_PRIVATE_SLICE));
	state->type = intVal(list_nth(priv, MOTION_PRIVATE_TYPE));
	state->nkeys = list_length(keys);

	/*
	 * On a segment, a Motion between segments receives: what the coordinator
	 * relayed to this segment, in the file it keeps under the statement's
	 * key.  So does a Gather, which runs on a segment only in a slice that
	 * runs there alone, the one it gathers to.
	 */
	if (GpClusterBackendRole() == GP_ROLE_EXECUTE &&
		(state->type == GP_MOTION_HASH || state->type == GP_MOTION_BROADCAST ||
		 state->type == GP_MOTION_RANDOM || state->type == GP_MOTION_EXPLICIT ||
		 state->type == GP_MOTION_GATHER) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		TupleDesc	tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		List	   *entry = stream_entry(estate->es_plannedstmt, state->slice);

		/* A reader's fragment is the Motion it sends through. */
		if (entry != NULL &&
			estate->es_plannedstmt->planTree == (Plan *) cscan)
		{
			motion_begin_sending(state, estate, eflags, entry);
			return;
		}

		/*
		 * A slice that streams, received as it comes rather than from a
		 * file; kept for a rescan, but where it is sorted, which keeps it.
		 */
		if (entry != NULL)
		{
			state->streamed = true;
			state->stream_here = stream_receives(estate->es_plannedstmt, entry);
			state->token = stream_token(estate->es_plannedstmt);
			state->nsenders = intVal(lsecond(entry));
			if (state->nkeys == 0)
			{
				state->spool = tuplestore_begin_heap(false, false, work_mem);
				state->spoolslot = MakeSingleTupleTableSlot(tupdesc,
															&TTSOpsMinimalTuple);
			}
			state->recv_tuples = motion_tuples(tupdesc);
		}

		state->receiving = true;
		state->key = fragment_key(estate->es_plannedstmt);
		state->binary = GpTupleDescHasBinaryIO(tupdesc);
		state->inprocs = palloc0_array(FmgrInfo, tupdesc->natts);
		state->inparams = palloc0_array(Oid, tupdesc->natts);
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Oid			proc;

			if (state->binary)
				getTypeBinaryInputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
									   &proc, &state->inparams[i]);
			else
				getTypeInputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
								 &proc, &state->inparams[i]);
			fmgr_info(proc, &state->inprocs[i]);
		}
		return;
	}

	if (state->nkeys > 0)
	{
		state->keys = palloc_array(AttrNumber, state->nkeys);
		state->sortkeys = palloc0_array(SortSupportData, state->nkeys);
		for (int i = 0; i < state->nkeys; i++)
		{
			SortSupport sk = &state->sortkeys[i];

			state->keys[i] = (AttrNumber) list_nth_int(keys, i);
			sk->ssup_cxt = CurrentMemoryContext;
			sk->ssup_collation = list_nth_oid(colls, i);
			sk->ssup_nulls_first = list_nth_int(nfs, i) != 0;
			sk->ssup_attno = state->keys[i];
			sk->abbreviate = false;
			PrepareSortSupportFromOrderingOp(list_nth_oid(sortops, i), sk);
		}
	}

	/*
	 * The fragment is the segments' to run.  Here it is only described: for
	 * EXPLAIN, and for EXPLAIN ANALYZE, where it shows as never executed.
	 */
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) || estate->es_instrument)
	{
		outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate,
											eflags | EXEC_FLAG_EXPLAIN_ONLY);
		if (estate->es_instrument)
			(void) fragment_instrument_walker(outerPlanState(node), NULL);
	}
}

/*
 * A fragment, as the query a segment is sent: the PlannedStmt it is part of
 * with it as the tree, and the key its Motions' rows are kept under.
 */
/*
 * A parameter's value as a Const, whole: a varlena detoasted and an expanded
 * object flattened, so that nodeToString() writes the value itself.
 */
static Const *
param_const(Oid type, Datum value, bool isnull)
{
	int16		typlen;
	bool		typbyval;

	/*
	 * A record of no declared type is described by a typmod this backend
	 * registered, which a segment has never heard of.
	 */
	if (type == RECORDOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a value of an anonymous record type cannot be sent to the segments")));

	get_typlenbyval(type, &typlen, &typbyval);
	if (!isnull && typlen == -1)
		value = PointerGetDatum(PG_DETOAST_DATUM_COPY(value));
	return makeConst(type, -1, InvalidOid, typlen, value, isnull, typbyval);
}

/*
 * The values a Motion's fragment is sent with: Cloudberry's dispatcher sends
 * a slice the parameters it reads (cdbdisp_query.c), and so does this -- the
 * statement's parameters the client bound, and the values the coordinator
 * computed, an initplan's among them, evaluated now if nothing has asked
 * for them yet.  The translator says which (compat/motion.c): only ones
 * the coordinator sets, never ones another fragment would.
 */
static List *
fragment_params(EState *estate, CustomScan *motion, ExprContext *econtext)
{
	List	   *priv = motion->custom_private;
	List	   *result = NIL;
	ListCell   *lc;

	if (list_length(priv) <= MOTION_PRIVATE_EXTERN_PARAMS)
		return NIL;

	foreach(lc, (List *) list_nth(priv, MOTION_PRIVATE_EXEC_PARAMS))
	{
		int			id = lfirst_int(lc);
		ParamExecData *prm = &estate->es_param_exec_vals[id];
		Oid			type = list_nth_oid(estate->es_plannedstmt->paramExecTypes, id);

		if (prm->execPlan != NULL)
			ExecSetParamPlan((SubPlanState *) prm->execPlan, econtext);
		result = lappend(result,
						 list_make3(makeInteger(PARAM_EXEC), makeInteger(id),
									param_const(type, prm->value, prm->isnull)));
	}

	foreach(lc, (List *) list_nth(priv, MOTION_PRIVATE_EXTERN_PARAMS))
	{
		int			id = lfirst_int(lc);
		ParamListInfo params = estate->es_param_list_info;
		ParamExternData *prm;
		ParamExternData prmdata;

		if (params == NULL || id <= 0 || id > params->numParams)
			elog(ERROR, "there is no value for parameter $%d to send", id);
		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, id, false, &prmdata);
		else
			prm = &params->params[id - 1];
		if (!OidIsValid(prm->ptype))
			elog(ERROR, "parameter $%d has no type to send it as", id);
		result = lappend(result,
						 list_make3(makeInteger(PARAM_EXTERN), makeInteger(id),
									param_const(prm->ptype, prm->value, prm->isnull)));
	}

	return result;
}

/*
 * On a segment, before a fragment starts: the statement's parameters it was
 * sent, as the query's own; and after, the coordinator's values, in the
 * slots the fragment reads them from.
 */
static void
fragment_params_before_start(QueryDesc *queryDesc, List *params)
{
	ParamListInfo list;
	int			n = 0;
	ListCell   *lc;

	foreach(lc, params)
	{
		List	   *entry = (List *) lfirst(lc);

		if (intVal(linitial(entry)) == PARAM_EXTERN)
			n = Max(n, intVal(lsecond(entry)));
	}
	if (n == 0)
		return;

	list = makeParamList(n);
	for (int i = 0; i < n; i++)
	{
		list->params[i].isnull = true;
		list->params[i].pflags = 0;
		list->params[i].ptype = InvalidOid;
	}
	foreach(lc, params)
	{
		List	   *entry = (List *) lfirst(lc);
		Const	   *c = (Const *) lthird(entry);
		ParamExternData *prm;

		if (intVal(linitial(entry)) != PARAM_EXTERN)
			continue;
		prm = &list->params[intVal(lsecond(entry)) - 1];
		prm->value = c->constvalue;
		prm->isnull = c->constisnull;
		prm->pflags = PARAM_FLAG_CONST;
		prm->ptype = c->consttype;
	}
	queryDesc->params = list;
}

static void
fragment_params_after_start(QueryDesc *queryDesc, List *params)
{
	EState	   *estate = queryDesc->estate;
	MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
	ListCell   *lc;

	foreach(lc, params)
	{
		List	   *entry = (List *) lfirst(lc);
		Const	   *c = (Const *) lthird(entry);
		ParamExecData *prm;

		if (intVal(linitial(entry)) != PARAM_EXEC)
			continue;
		prm = &estate->es_param_exec_vals[intVal(lsecond(entry))];
		prm->execPlan = NULL;
		prm->isnull = c->constisnull;
		prm->value = c->constisnull ? (Datum) 0
			: datumCopy(c->constvalue, c->constbyval, c->constlen);
	}
	MemoryContextSwitchTo(oldcxt);
}

static char *
fragment_sql_ex(EState *estate, Plan *fragment, CustomScan *motion,
				ExprContext *econtext, const char *key, List *marks,
				bool reader)
{
	List	   *params;

	PlannedStmt *whole = estate->es_plannedstmt;
	PlannedStmt *frag = makeNode(PlannedStmt);
	bool		split = GpSplitModifyIs(fragment, NULL);
	bool		write = IsA(fragment, ModifyTable) || split;

	/*
	 * A write is the statement's own command, and returns its RETURNING's
	 * rows where it has one; everything else reads.
	 */
	memcpy(frag, whole, sizeof(PlannedStmt));
	frag->commandType = !write ? CMD_SELECT
		: split ? CMD_UPDATE : ((ModifyTable *) fragment)->operation;
	frag->hasReturning = write && !split &&
		((ModifyTable *) fragment)->returningLists != NIL;
	frag->hasModifyingCTE = false;
	frag->canSetTag = true;
	frag->planTree = fragment;
	if (!write)
		frag->resultRelationRelids = NULL;

	/*
	 * The rows a SELECT ... FOR UPDATE locks on the segments are locked by
	 * the LockRows at the top of its fragment -- or under a LIMIT -- which
	 * finds the tables it locks in the statement's row marks (ORCA's
	 * lockrows.c).  The writer runs it: a reader's transaction reads only.
	 */
	frag->rowMarks = NIL;
	if (!write && whole->rowMarks != NIL &&
		(IsA(fragment, LockRows) ||
		 (IsA(fragment, Limit) && fragment->lefttree != NULL &&
		  IsA(fragment->lefttree, LockRows))))
		frag->rowMarks = whole->rowMarks;
	frag->extension_state = list_copy(marks);
	frag->utilityStmt = NULL;

	if (estate->es_sourceText != NULL)
	{
		const char *text = estate->es_sourceText;
		int			len = pg_mbcliplen(text, strlen(text),
									   pgstat_track_activity_query_size - 1);

		frag->extension_state = lappend(frag->extension_state,
										makeDefElem(pstrdup(GP_SOURCE_MARK),
													(Node *) makeString(pnstrdup(text, len)),
													-1));
	}

	params = fragment_params(estate, motion, econtext);
	if (params != NIL)
		frag->extension_state = lappend(frag->extension_state,
										makeDefElem(pstrdup(GP_PARAMS_MARK),
													(Node *) params, -1));

	/*
	 * A reader only reads, in a transaction that may not write, and a
	 * fragment that carries the statement's INSERT or UPDATE privileges is
	 * refused there as a write.  Its privileges are not a reader's to check:
	 * the coordinator checked the statement's before sending any of it, and
	 * the writer's fragment carries them all again.  So a reader's has none,
	 * and its range table points at none.
	 */
	if (reader)
	{
		List	   *rtable = NIL;
		ListCell   *lc;

		foreach(lc, whole->rtable)
		{
			RangeTblEntry *rte = copyObject(lfirst_node(RangeTblEntry, lc));

			rte->perminfoindex = 0;
			rtable = lappend(rtable, rte);
		}
		frag->rtable = rtable;
		frag->permInfos = NIL;
	}

	return psprintf("SELECT gp_internal.exec_fragment(%s, %s)",
					quote_literal_cstr(nodeToString(frag)),
					quote_literal_cstr(key ? key : ""));
}



/* Every Motion in a plan tree, the fragments below them included. */
static void
collect_motions(Plan *plan, List **motions)
{
	ListCell   *lc;

	if (plan == NULL)
		return;
	if (GpMotionIs(plan))
		*motions = lappend(*motions, plan);

	collect_motions(plan->lefttree, motions);
	collect_motions(plan->righttree, motions);
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				collect_motions(lfirst(lc), motions);
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				collect_motions(lfirst(lc), motions);
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				collect_motions(lfirst(lc), motions);
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				collect_motions(lfirst(lc), motions);
			break;
		case T_SubqueryScan:
			collect_motions(((SubqueryScan *) plan)->subplan, motions);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) plan)->custom_plans)
				collect_motions(lfirst(lc), motions);
			break;
		default:
			break;
	}
}

/* One row, as a receiving segment reads it: a count, then each value. */
static void
append_row_raw(StringInfo buf, int natts, const char **values,
			   const int *lengths)
{
	pq_sendint16(buf, natts);
	for (int i = 0; i < natts; i++)
	{
		pq_sendint32(buf, lengths[i]);
		if (lengths[i] > 0)
			appendBinaryStringInfo(buf, values[i], lengths[i]);
	}
}

static void
motion_flush(const char *key, int slice, int content, StringInfo buf,
			 bool shared)
{
	const char *values[3];
	int			lengths[3];
	int			formats[3] = {0, 0, 1};
	char		slicetext[16];

	if (buf->len == 0)
		return;
	snprintf(slicetext, sizeof(slicetext), "%d", slice);
	values[0] = key;
	values[1] = slicetext;
	values[2] = buf->data;
	lengths[0] = lengths[1] = 0;
	lengths[2] = buf->len;
	GpDispatchParamsOnContent(content,
							  shared ? MOTION_PUT_SHARED_SQL : MOTION_PUT_SQL,
							  3, values, lengths, formats);
	resetStringInfo(buf);
}

static StreamSlice *stream_slice_find(MotionState *state, int slice);

/*
 * Carry out a Motion between segments: run the slice that sends, and relay
 * each of its rows to the segments that receive it -- a Gather's to segment
 * "to", the one its receiving slice runs on, or to every one with -1.  The
 * receiving slice reads them later, from the file each segment keeps them
 * in.
 */
static void
motion_relay(MotionState *gather, CustomScan *motion, int to)
{
	EState	   *estate = gather->css.ss.ps.state;
	List	   *priv = motion->custom_private;
	int			type = intVal(list_nth(priv, MOTION_PRIVATE_TYPE));
	int			content = intVal(list_nth(priv, MOTION_PRIVATE_CONTENT));
	int			slice = intVal(list_nth(priv, MOTION_PRIVATE_SLICE));
	List	   *hashfuncs = (List *) list_nth(priv, MOTION_PRIVATE_HASHFUNCS);
	Plan	   *child = outerPlan(motion);
	TupleDesc	tupdesc = ExecTypeFromTL(child->targetlist);
	int			natts = tupdesc->natts;
	int			nsegs = GpClusterSegmentCount();
	StringInfoData *bufs = palloc_array(StringInfoData, nsegs);
	ExprContext *econtext = CreateStandaloneExprContext();
	TupleTableSlot *keyslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	int			nkeys = list_length(motion->custom_exprs);
	ExprState **keyexprs = palloc_array(ExprState *, Max(nkeys, 1));
	Datum	   *keyvalues = palloc_array(Datum, Max(nkeys, 1));
	bool	   *keynulls = palloc_array(bool, Max(nkeys, 1));
	Bitmapset  *keycols = NULL;
	GpHash		hash;
	int			next = (int) (pg_prng_uint32(&pg_global_prng_state) % nsegs);
	const char **values = palloc_array(const char *, natts);
	int		   *lengths = palloc_array(int, natts);
	StringInfoData row;
	int			i;

	/* a reader, not the writer, receives the rows of one below a slice that streams */
	int			parent = GpMotionParent((Plan *) motion);
	bool		shared = gather->streaming &&
		parent != GpMotionSlice(gather->css.ss.ps.plan) &&
		stream_slice_find(gather, parent) != NULL;

	for (i = 0; i < nsegs; i++)
		initStringInfo(&bufs[i]);
	initStringInfo(&row);

	/* A Redistribute hashes its keys as cdbhash hashes a table's. */
	memset(&hash, 0, sizeof(hash));
	hash.ptype = POLICYTYPE_PARTITIONED;
	hash.numsegs = nsegs;
	hash.nattrs = nkeys;
	hash.attrs = palloc_array(AttrNumber, Max(nkeys, 1));
	hash.hashfuncs = palloc_array(FmgrInfo, Max(nkeys, 1));
	i = 0;
	{
		ListCell   *lc,
				   *lf;

		forboth(lc, motion->custom_exprs, lf, hashfuncs)
		{
			keyexprs[i] = ExecInitExpr((Expr *) lfirst(lc), NULL);
			pull_varattnos((Node *) lfirst(lc), OUTER_VAR, &keycols);
			hash.attrs[i] = i + 1;
			if (OidIsValid(lfirst_oid(lf)))
				GpHashSetFunction(&hash, i, lfirst_oid(lf));
			i++;
		}
	}

	if (content == GP_MOTION_FROM_COORDINATOR)
	{
		/*
		 * The slice that sends is the coordinator's own -- a VALUES list, a
		 * function, a catalog -- and runs here, its rows encoded as a segment
		 * would have sent them.
		 */
		PlanState  *ps = ExecInitNode(child, estate, 0);
		bool		binary = GpTupleDescHasBinaryIO(tupdesc);
		FmgrInfo   *outprocs = palloc0_array(FmgrInfo, natts);

		for (i = 0; i < natts; i++)
		{
			Oid			proc;
			bool		isvarlena;

			if (binary)
				getTypeBinaryOutputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
										&proc, &isvarlena);
			else
				getTypeOutputInfo(GpTransferType(TupleDescAttr(tupdesc, i)->atttypid),
								  &proc, &isvarlena);
			fmgr_info(proc, &outprocs[i]);
		}

		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(ps);
			MemoryContext oldcxt;
			int			target;

			if (TupIsNull(slot))
				break;
			slot_getallattrs(slot);
			oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
			for (i = 0; i < natts; i++)
			{
				if (slot->tts_isnull[i])
				{
					values[i] = NULL;
					lengths[i] = -1;
				}
				else if (binary)
				{
					bytea	   *b = SendFunctionCall(&outprocs[i],
													 slot->tts_values[i]);

					values[i] = VARDATA(b);
					lengths[i] = VARSIZE(b) - VARHDRSZ;
				}
				else
				{
					values[i] = OutputFunctionCall(&outprocs[i],
												   slot->tts_values[i]);
					lengths[i] = strlen(values[i]);
				}
			}
			resetStringInfo(&row);
			append_row_raw(&row, natts, values, lengths);

			econtext->ecxt_outertuple = slot;
			for (i = 0; i < nkeys; i++)
				keyvalues[i] = ExecEvalExpr(keyexprs[i], econtext, &keynulls[i]);
			MemoryContextSwitchTo(oldcxt);

			target = type == GP_MOTION_HASH ?
				GpHashSegment(&hash, keyvalues, keynulls) :
				type == GP_MOTION_EXPLICIT ?
				explicit_target(keyvalues[0], keynulls[0], nsegs) :
				type == GP_MOTION_GATHER ? to :
				next++ % nsegs;
			if (type == GP_MOTION_BROADCAST ||
				(type == GP_MOTION_GATHER && to < 0))
			{
				for (i = 0; i < nsegs; i++)
					appendBinaryStringInfo(&bufs[i], row.data, row.len);
			}
			else
				appendBinaryStringInfo(&bufs[target], row.data, row.len);
			ResetExprContext(econtext);

			for (i = 0; i < nsegs; i++)
				if (bufs[i].len >= MOTION_BATCH_BYTES)
					motion_flush(gather->key, slice, i, &bufs[i], shared);
		}
		ExecEndNode(ps);
	}
	else
	{
		GpGatherState *g;
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

		g = GpGatherStartOn(fragment_sql_ex(estate, child, motion,
											gather->css.ss.ps.ps_ExprContext,
											gather->key, NIL, false),
							tupdesc, content);
		MemoryContextSwitchTo(oldcxt);

		while (GpGatherNextRaw(g, values, lengths))
		{
			int			target = 0;

			resetStringInfo(&row);
			append_row_raw(&row, natts, values, lengths);

			if (type == GP_MOTION_HASH || type == GP_MOTION_EXPLICIT)
			{
				/* only the columns the keys read are made Datums again */
				oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
				ExecClearTuple(keyslot);
				for (i = 0; i < natts; i++)
				{
					keyslot->tts_isnull[i] = true;
					keyslot->tts_values[i] = (Datum) 0;
					if (lengths[i] >= 0 &&
						bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
									  keycols))
					{
						keyslot->tts_values[i] =
							GpGatherDecodeValue(g, i, values[i], lengths[i]);
						keyslot->tts_isnull[i] = false;
					}
				}
				ExecStoreVirtualTuple(keyslot);
				econtext->ecxt_outertuple = keyslot;
				for (i = 0; i < nkeys; i++)
					keyvalues[i] = ExecEvalExpr(keyexprs[i], econtext,
												&keynulls[i]);
				target = type == GP_MOTION_EXPLICIT
					? explicit_target(keyvalues[0], keynulls[0], nsegs)
					: GpHashSegment(&hash, keyvalues, keynulls);
				MemoryContextSwitchTo(oldcxt);
				ResetExprContext(econtext);
			}
			else if (type == GP_MOTION_RANDOM)
				target = next++ % nsegs;
			else if (type == GP_MOTION_GATHER)
				target = to;

			if (type == GP_MOTION_BROADCAST ||
				(type == GP_MOTION_GATHER && to < 0))
			{
				for (i = 0; i < nsegs; i++)
					appendBinaryStringInfo(&bufs[i], row.data, row.len);
			}
			else
				appendBinaryStringInfo(&bufs[target], row.data, row.len);

			for (i = 0; i < nsegs; i++)
				if (bufs[i].len >= MOTION_BATCH_BYTES)
					motion_flush(gather->key, slice, i, &bufs[i], shared);
		}
		GpGatherEnd(g);
	}

	for (i = 0; i < nsegs; i++)
		motion_flush(gather->key, slice, i, &bufs[i], shared);

	ExecDropSingleTupleTableSlot(keyslot);
	FreeExprContext(econtext, true);
}

/* The segments a slice runs on: every one, or the one it names. */
static bool
content_includes(int content, int segment)
{
	return content == -1 || content == segment;
}

/* The segments the Gather runs on: those, where direct dispatch named some. */
static bool
state_includes(MotionState *state, int segment)
{
	if (state->ncontents > 0)
	{
		for (int i = 0; i < state->ncontents; i++)
			if (state->contents[i] == segment)
				return true;
		return false;
	}
	return content_includes(state->content, segment);
}

/* A slice a subplan's top part runs in: the one that calls it, which its plan does not say. */
#define SLICE_OF_CALLER		INT_MIN

/*
 * The slices of "plan", run by slice "slice", that scan a temporary table,
 * which only its session's own backend -- the writer -- can read: added to
 * *slices, but for "top", the writer's own.  A scan of one in a subplan's top
 * part, which runs in whichever slice calls it, sets *unknown where the plan
 * does not say which slice that is.
 */
static void
temp_scan_slices(Plan *plan, int slice, int top, List *rtable, List **slices,
				 bool *unknown)
{
	ListCell   *lc;

	if (plan == NULL)
		return;
	if (GpMotionIs(plan))
	{
		temp_scan_slices(outerPlan(plan), GpMotionSlice(plan), top, rtable,
						 slices, unknown);
		return;
	}

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_CustomScan:
			{
				Index		scanrelid = ((Scan *) plan)->scanrelid;
				RangeTblEntry *rte;

				if (scanrelid == 0)
					break;
				rte = rt_fetch(scanrelid, rtable);
				if (rte->rtekind != RTE_RELATION ||
					get_rel_persistence(rte->relid) != RELPERSISTENCE_TEMP)
					break;
				if (slice == SLICE_OF_CALLER)
					*unknown = true;
				else if (slice != top)
					*slices = list_append_unique_int(*slices, slice);
				break;
			}
		default:
			break;
	}

	temp_scan_slices(plan->lefttree, slice, top, rtable, slices, unknown);
	temp_scan_slices(plan->righttree, slice, top, rtable, slices, unknown);
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				temp_scan_slices(lfirst(lc), slice, top, rtable, slices, unknown);
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				temp_scan_slices(lfirst(lc), slice, top, rtable, slices, unknown);
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				temp_scan_slices(lfirst(lc), slice, top, rtable, slices, unknown);
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				temp_scan_slices(lfirst(lc), slice, top, rtable, slices, unknown);
			break;
		case T_SubqueryScan:
			temp_scan_slices(((SubqueryScan *) plan)->subplan, slice, top,
							 rtable, slices, unknown);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) plan)->custom_plans)
				temp_scan_slices(lfirst(lc), slice, top, rtable, slices, unknown);
			break;
		default:
			break;
	}
}

/*
 * Can the Motions below this Gather stream -- every slice running at once,
 * a reader on each segment for each slice the writer does not run?  When
 * they can, the slices that stream, with the one each sends to.
 *
 * Some slices are relayed first, as the relay carries every slice, and the
 * rest stream: the coordinator's own, which runs here; one that scans a
 * temporary table, which only the writer can read, and so runs there; and
 * every slice below one of them, whose rows it reads from files.  A reader
 * that receives a relayed slice reads the files its writer keeps for it
 * (gp_motion_put_shared()).
 *
 * The relay stays for all of it where a temporary table is scanned in a
 * subplan's own part, which runs in whichever slice calls it, and the plan
 * does not say which slice that is (GP_SUBPLAN_SLICES); where the translator
 * did not say which slice receives one; and where a slice would run on a
 * reader of a segment whose writer runs none of the Gather's fragment, and
 * so publishes no snapshot for it.  Direct dispatch would make that last
 * plan, and makes none: ORCA's translator sends a plan to some segments only
 * when every Motion in it is a Gather, as Cloudberry sends only a plan of
 * one slice, and the planner's Motions here are Gathers.
 */
static bool
stream_plan(MotionState *state, List *order, List *motions)
{
	EState	   *estate = state->css.ss.ps.state;
	int			top = GpMotionSlice(state->css.ss.ps.plan);
	int			nsegs = GpClusterSegmentCount();
	List	   *slices = NIL;
	List	   *relayed = NIL;
	List	   *callers = (List *) fragment_mark(estate->es_plannedstmt,
												 GP_SUBPLAN_SLICES);
	bool		unknown = false;
	bool		more;
	ListCell   *lc;

	if (gp_interconnect_type == GP_INTERCONNECT_RELAY)
		return false;

	temp_scan_slices(outerPlan(state->css.ss.ps.plan), top, top,
					 estate->es_range_table, &relayed, &unknown);

	/*
	 * A subplan's own part runs in the slice that calls it: this Gather's
	 * own or one below it, another Gather's, the coordinator's -- or, where
	 * the plan does not say, whichever.
	 */
	foreach(lc, estate->es_plannedstmt->subplans)
	{
		int			i = foreach_current_index(lc);
		int			caller = i < list_length(callers)
			? list_nth_int(callers, i) : GP_SUBPLAN_UNKNOWN;

		if (caller == GP_SUBPLAN_COORDINATOR ||
			(caller >= 0 && caller != top && !list_member_int(order, caller)))
			continue;
		temp_scan_slices((Plan *) lfirst(lc),
						 caller >= 0 ? caller : SLICE_OF_CALLER, top,
						 estate->es_range_table, &relayed, &unknown);
	}
	if (unknown)
		return false;

	foreach(lc, order)
	{
		int			slice = lfirst_int(lc);
		CustomScan *motion = NULL;

		foreach_ptr(Plan, m, motions)
			if (GpMotionSlice(m) == slice)
				motion = (CustomScan *) m;
		if (motion == NULL ||
			GpMotionParent((Plan *) motion) == MOTION_PARENT_UNKNOWN)
			return false;
		if (GpMotionSegment((Plan *) motion) == GP_MOTION_FROM_COORDINATOR)
			relayed = list_append_unique_int(relayed, slice);
	}

	/* a slice below a relayed one is relayed before it */
	do
	{
		more = false;
		foreach_ptr(Plan, m, motions)
		{
			if (list_member_int(relayed, GpMotionParent(m)) &&
				!list_member_int(relayed, GpMotionSlice(m)))
			{
				relayed = lappend_int(relayed, GpMotionSlice(m));
				more = true;
			}
		}
	} while (more);

	foreach(lc, order)
	{
		int			slice = lfirst_int(lc);
		CustomScan *motion = NULL;
		StreamSlice *ss;
		int			content;

		if (list_member_int(relayed, slice))
			continue;
		foreach_ptr(Plan, m, motions)
			if (GpMotionSlice(m) == slice)
				motion = (CustomScan *) m;
		content = GpMotionSegment((Plan *) motion);

		ss = palloc0(sizeof(StreamSlice));
		ss->motion = motion;
		ss->slice = slice;
		ss->parent = GpMotionParent((Plan *) motion);
		ss->contents = palloc_array(int, nsegs);
		for (int seg = 0; seg < nsegs; seg++)
		{
			if (!content_includes(content, seg))
				continue;
			if (!state_includes(state, seg))
				return false;	/* no writer there to read as */
			ss->contents[ss->ncontents++] = seg;
		}
		ss->readers = palloc_array(int, Max(ss->ncontents, 1));
		ss->addresses = palloc_array(const char *, Max(ss->ncontents, 1));
		slices = lappend(slices, ss);
	}

	/* every receiving slice is one that runs here */
	foreach_ptr(StreamSlice, ss, slices)
	{
		bool		found = ss->parent == top;

		foreach_ptr(StreamSlice, p, slices)
			if (p->slice == ss->parent)
				found = true;
		if (!found)
			return false;
	}

	state->stream_slices = slices;
	return slices != NIL;
}

/* The slice that streams, of this Gather's, by its number; NULL if relayed. */
static StreamSlice *
stream_slice_find(MotionState *state, int slice)
{
	foreach_ptr(StreamSlice, ss, state->stream_slices)
		if (ss->slice == slice)
			return ss;
	return NULL;
}

/*
 * Start the slices that stream, each on a reader of every segment that runs
 * it, and answer what the writer's own fragment has to carry: where every
 * slice's receivers are, and the key the readers find its snapshot under.
 */
static List *
stream_start(MotionState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	int			top = GpMotionSlice(state->css.ss.ps.plan);
	int			nsegs = GpClusterSegmentCount();
	int		   *writer_pid = palloc0_array(int, nsegs);
	const char **writer_address = palloc0_array(const char *, nsegs);
	uint8		random[GP_IC_TOKEN_LEN / 2];
	char		token[GP_IC_TOKEN_LEN + 1];
	char	   *sharekey;
	List	   *entries = NIL;
	DefElem    *streammark;
	GpStream   *stream;
	static uint32 share_counter = 0;

	if (!pg_strong_random(random, sizeof(random)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate a random interconnect token")));
	for (int i = 0; i < (int) sizeof(random); i++)
		snprintf(token + 2 * i, 3, "%02x", random[i]);
	sharekey = psprintf("%d_%u", MyProcPid, ++share_counter);

	for (int seg = 0; seg < nsegs; seg++)
		if (state_includes(state, seg))
			writer_address[seg] = GpStreamWriterAddress(seg, &writer_pid[seg]);

	stream = GpStreamBegin();
	state->stream = stream;
	foreach_ptr(StreamSlice, ss, state->stream_slices)
		for (int i = 0; i < ss->ncontents; i++)
			ss->readers[i] = GpStreamAddReader(stream, ss->contents[i],
											   &ss->addresses[i]);

	/* where each slice's receivers are: the writers, or its parent's readers */
	foreach_ptr(StreamSlice, ss, state->stream_slices)
	{
		List	   *contents = NIL;
		List	   *addresses = NIL;

		if (ss->parent == top)
		{
			for (int seg = 0; seg < nsegs; seg++)
			{
				if (!state_includes(state, seg))
					continue;
				contents = lappend(contents, makeInteger(seg));
				addresses = lappend(addresses,
									makeString(pstrdup(GpIcAddressOf(writer_address[seg],
																	 gp_interconnect_type == GP_INTERCONNECT_UDPIFC))));
			}
		}
		else
		{
			foreach_ptr(StreamSlice, p, state->stream_slices)
			{
				if (p->slice != ss->parent)
					continue;
				for (int i = 0; i < p->ncontents; i++)
				{
					contents = lappend(contents, makeInteger(p->contents[i]));
					addresses = lappend(addresses,
										makeString(pstrdup(GpIcAddressOf(p->addresses[i],
																		 gp_interconnect_type == GP_INTERCONNECT_UDPIFC))));
				}
			}
		}
		entries = lappend(entries,
						  list_make4(makeInteger(ss->slice),
									 makeInteger(ss->ncontents),
									 contents, addresses));
	}
	streammark = makeDefElem(pstrdup(GP_STREAM_MARK),
							 (Node *) list_make3(makeString(pstrdup(token)),
												 entries,
												 makeBoolean(gp_interconnect_type == GP_INTERCONNECT_UDPIFC)),
							 -1);

	/*
	 * Each reader: its own transaction, read as a part of its writer's, and
	 * the Motion it sends through as its fragment.
	 */
	foreach_ptr(StreamSlice, ss, state->stream_slices)
	{
		char	   *fragment = fragment_sql_ex(estate, (Plan *) ss->motion,
											   ss->motion,
											   state->css.ss.ps.ps_ExprContext,
											   state->key,
											   list_make1(streammark), true);

		for (int i = 0; i < ss->ncontents; i++)
		{
			int			seg = ss->contents[i];
			char	   *sql;

			sql = psprintf("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY; "
						   "SET LOCAL %s = %s; %s; COMMIT",
						   GP_SHARE_SETTING,
						   quote_literal_cstr(psprintf("%d/%s", writer_pid[seg],
													   sharekey)),
						   fragment);
			GpStreamStartReader(stream, ss->readers[i], sql);
		}
	}

	return list_make2(streammark,
					  makeDefElem(pstrdup(GP_SHARE_MARK),
								  (Node *) makeString(sharekey), -1));
}

/* The readers are done: they finished their slices, or were not wanted. */
static void
stream_end(MotionState *state)
{
	GpStream   *stream = state->stream;

	state->stream = NULL;
	if (stream != NULL)
		GpStreamEnd(stream);
}

/*
 * The segment a Gather below the one being prepared gathers to: the one its
 * receiving slice runs on, which the Motion that slice sends through names
 * -- or -1, for every one, where the receiving slice is the prepared one's
 * own (a cluster of one segment).
 */
static int
gather_target(CustomScan *motion, List *motions)
{
	int			parent = GpMotionParent((Plan *) motion);

	if (GpMotionType((Plan *) motion) != GP_MOTION_GATHER)
		return -1;
	foreach_ptr(Plan, m, motions)
		if (GpMotionSlice(m) == parent)
			return Max(GpMotionSegment(m), -1);
	return -1;
}

/*
 * Before a Gather sends its fragment: the Motions below it that move rows
 * between segments, in the order the translator gave -- a Motion's senders
 * before its receivers -- each carried out once, however often the Gather
 * is read again.  Where the slices can stream, only the coordinator's own
 * are carried out first; the rest run with the Gather's, each time it runs.
 */
static void
motion_prepare(MotionState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	CustomScan *cscan = (CustomScan *) state->css.ss.ps.plan;
	List	   *order = (List *) list_nth(cscan->custom_private,
										  MOTION_PRIVATE_PREPARE);
	List	   *motions = NIL;
	ListCell   *lc;
	static uint32 motion_counter = 0;

	state->prepared = true;
	if (order == NIL)
		return;

	state->key = MemoryContextStrdup(estate->es_query_cxt,
									 psprintf("%d_%u", MyProcPid,
											  ++motion_counter));

	collect_motions(outerPlan(cscan), &motions);
	foreach(lc, estate->es_plannedstmt->subplans)
		collect_motions((Plan *) lfirst(lc), &motions);

	state->streaming = stream_plan(state, order, motions);

	foreach(lc, order)
	{
		int			slice = lfirst_int(lc);
		ListCell   *lm;
		CustomScan *motion = NULL;

		foreach(lm, motions)
			if (GpMotionSlice((Plan *) lfirst(lm)) == slice)
				motion = (CustomScan *) lfirst(lm);
		if (motion == NULL)
			elog(ERROR, "no Motion sends slice %d", slice);

		/* A streaming slice runs with the Gather's; the ones relayed, first. */
		if (state->streaming && stream_slice_find(state, slice) != NULL)
			continue;
		motion_relay(state, motion, gather_target(motion, motions));
	}
}

/*
 * A write of a distributed table: its Motions first, then the ModifyTable
 * on the segments, and the rows they changed are the statement's.
 */
static void
motion_dml_run(MotionState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	Plan	   *write = outerPlan(state->css.ss.ps.plan);
	Index		rti;
	CmdType		operation;
	RangeTblEntry *rte;
	GpPolicy   *policy;
	int			nsegs = GpClusterSegmentCount();
	uint64	   *counts = palloc0_array(uint64, nsegs);
	uint64		total = 0;

	if (GpSplitModifyIs(write, &rti))
		operation = CMD_UPDATE;
	else
	{
		ModifyTable *mt = (ModifyTable *) write;

		/* a partitioned table's partitions are written through the table */
		rti = mt->rootRelation > 0 ? mt->rootRelation
			: linitial_int(mt->resultRelations);
		operation = mt->operation;
	}
	rte = rt_fetch(rti, estate->es_range_table);
	policy = GpPolicyGet(rte->relid);

	/*
	 * Cloudberry without its global deadlock detector: an UPDATE or DELETE
	 * of a distributed table locks the table, so that two of them never wait
	 * for each other on different segments, a partitioned table's every
	 * partition too.  With it, rows (gp_gdd.c).  An INSERT into a
	 * partitioned table locks every partition (gp_modify.c).
	 */
	if ((operation == CMD_UPDATE || operation == CMD_DELETE) &&
		!gp_enable_global_deadlock_detector)
	{
		LockRelationOid(rte->relid, ExclusiveLock);
		GpModifyLockPartitions(rte->relid, ExclusiveLock);
	}
	if (operation == CMD_INSERT)
		GpModifyLockPartitions(rte->relid, RowExclusiveLock);

	if (!state->prepared)
		motion_prepare(state);

	/*
	 * With RETURNING, what the segments return comes back with their counts
	 * and is handed out from here: every segment's rows, but a replicated
	 * table's, which every segment returns alike, once.
	 */
	if (write->targetlist != NIL)
	{
		TupleDesc	tupdesc = state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

		state->returned = tuplestore_begin_heap(false, false, work_mem);
		state->returnedslot = MakeSingleTupleTableSlot(tupdesc,
													   &TTSOpsMinimalTuple);
		MemoryContextSwitchTo(oldcxt);

		GpDispatchWriteReturning(fragment_sql_ex(estate, write,
												 (CustomScan *) state->css.ss.ps.plan,
												 state->css.ss.ps.ps_ExprContext,
												 state->key,
												 state->streaming ? stream_start(state) : NIL,
												 false),
								 state->ncontents > 0 ? -1 : state->content,
								 state->ncontents > 0 ? state->contents : NULL,
								 state->ncontents, tupdesc, state->returned,
								 policy != NULL && GpPolicyIsReplicated(policy),
								 counts);
	}
	else if (state->ncontents > 0)
		GpDispatchCommandParamsOnContents(fragment_sql_ex(estate, write,
														  (CustomScan *) state->css.ss.ps.plan,
														  state->css.ss.ps.ps_ExprContext,
														  state->key,
														  state->streaming ? stream_start(state) : NIL,
														  false),
										  0, NULL, NULL, state->contents,
										  state->ncontents, counts);
	else
		GpDispatchCommandParams(fragment_sql_ex(estate, write,
												(CustomScan *) state->css.ss.ps.plan,
												state->css.ss.ps.ps_ExprContext,
												state->key,
												state->streaming ? stream_start(state) : NIL,
												false),
								0, NULL, NULL, state->content, 0, counts);
	stream_end(state);

	/* every segment writes a replicated table's rows alike: count them once */
	if (policy != NULL && GpPolicyIsReplicated(policy))
		total = counts[0];
	else
		for (int i = 0; i < (state->ncontents > 0 ? state->ncontents : nsegs); i++)
			total += counts[i];
	estate->es_processed += total;
}

static void
motion_start(MotionState *state)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	MemoryContext oldcxt;

	if (!state->prepared)
		motion_prepare(state);

	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
	{
		char	   *sql = fragment_sql_ex(state->css.ss.ps.state,
										  outerPlan(state->css.ss.ps.plan),
										  (CustomScan *) state->css.ss.ps.plan,
										  state->css.ss.ps.ps_ExprContext,
										  state->key,
										  state->streaming ? stream_start(state) : NIL,
										  false);

		state->gather = state->ncontents > 0
			? GpGatherStartOnContents(sql, slot->tts_tupleDescriptor,
									  state->contents, state->ncontents)
			: GpGatherStartOn(sql, slot->tts_tupleDescriptor, state->content);
	}
	MemoryContextSwitchTo(oldcxt);
}

static void
motion_finish(MotionState *state)
{
	if (state->gather != NULL)
		GpGatherEnd(state->gather);
	state->gather = NULL;
	stream_end(state);
	state->done = true;
}

/* binaryheap is a max-heap; the least row has to come out first. */
static int32
motion_heap_compare(Datum a, Datum b, void *arg)
{
	MotionState *state = (MotionState *) arg;
	TupleTableSlot *sa = state->segslots[DatumGetInt32(a)];
	TupleTableSlot *sb = state->segslots[DatumGetInt32(b)];

	for (int i = 0; i < state->nkeys; i++)
	{
		SortSupport sk = &state->sortkeys[i];
		Datum		va,
					vb;
		bool		na,
					nb;
		int			cmp;

		va = slot_getattr(sa, sk->ssup_attno, &na);
		vb = slot_getattr(sb, sk->ssup_attno, &nb);
		cmp = ApplySortComparator(va, na, vb, nb, sk);
		if (cmp != 0)
			return -cmp;
	}
	return 0;
}

/* A segment's next row into its slot; false when it has none. */
static bool
merge_read(MotionState *state, int seg)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	MemoryContext oldcxt;
	bool		got;

	oldcxt = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	got = GpGatherNextFrom(state->gather, seg, state->receive);
	MemoryContextSwitchTo(oldcxt);

	if (got)
		ExecCopySlot(state->segslots[seg], state->receive);
	else
		ExecClearTuple(state->segslots[seg]);
	return got;
}

static TupleTableSlot *
motion_merge_next(MotionState *state)
{
	if (!state->merging)
	{
		TupleDesc	tupdesc = state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

		state->nsegs = GpGatherSegmentCount(state->gather);
		if (state->segslots == NULL)
		{
			state->segslots = palloc_array(TupleTableSlot *, state->nsegs);
			/*
			 * Virtual, as the scan slot is: the node above compiled its reads
			 * of this node's rows for the kind of slot the scan slot is, and
			 * the row that goes out is one of these.  Copying into a virtual
			 * slot makes its own copy of the row's values.
			 */
			for (int i = 0; i < state->nsegs; i++)
				state->segslots[i] = MakeSingleTupleTableSlot(tupdesc,
															  &TTSOpsVirtual);
			state->receive = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
			state->heap = binaryheap_allocate(state->nsegs, motion_heap_compare,
											  state);
		}
		binaryheap_reset(state->heap);
		for (int i = 0; i < state->nsegs; i++)
			if (merge_read(state, i))
				binaryheap_add_unordered(state->heap, Int32GetDatum(i));
		binaryheap_build(state->heap);
		state->merging = true;
		state->last = -1;
	}
	else if (state->last >= 0)
	{
		/* the row that went out last is gone; its segment's next takes its place */
		if (merge_read(state, state->last))
			binaryheap_replace_first(state->heap, Int32GetDatum(state->last));
		else
			(void) binaryheap_remove_first(state->heap);
	}

	if (binaryheap_empty(state->heap))
		return NULL;

	state->last = DatumGetInt32(binaryheap_first(state->heap));
	return state->segslots[state->last];
}

static TupleTableSlot *
motion_next(ScanState *ss)
{
	MotionState *state = (MotionState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	MemoryContext oldcxt;
	bool		got;

	if (state->done)
		return ExecClearTuple(slot);

	if (state->sending)
	{
		motion_send_all(state);
		state->done = true;
		return ExecClearTuple(slot);
	}

	if (state->receiving && state->nkeys > 0)
		return motion_sorted_next(state);

	if (state->streamed)
		return motion_stream_next(state);

	if (state->receiving)
		return motion_recv_next(state);

	if (state->type == GP_MOTION_DML)
	{
		if (!state->written)
		{
			motion_dml_run(state);
			state->written = true;
		}
		if (state->returned != NULL &&
			tuplestore_gettupleslot(state->returned, true, false,
									state->returnedslot))
			return ExecCopySlot(slot, state->returnedslot);
		state->done = true;
		return ExecClearTuple(slot);
	}

	if (state->gather == NULL)
		motion_start(state);

	if (state->nkeys > 0)
	{
		TupleTableSlot *next = motion_merge_next(state);

		if (next != NULL)
			return next;
		motion_finish(state);
		return ExecClearTuple(slot);
	}

	/* The row's values live until ExecScan resets the per-tuple context. */
	oldcxt = MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	got = GpGatherNext(state->gather, slot, NULL);
	MemoryContextSwitchTo(oldcxt);

	if (got)
		return slot;

	motion_finish(state);
	return ExecClearTuple(slot);
}

static bool
motion_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

/*
 * EvalPlanQual runs the plan below an UPDATE or DELETE again, in an EState
 * of its own, over the new version of a row another transaction changed
 * while the statement waited for it -- which only happens where the rows
 * are locked rather than the table, with the global deadlock detector on.
 * A Motion's rows come from other processes, which ran their slices once,
 * and there is no running them again for one row: the recheck would find
 * none -- motion_recheck() stores no row -- and the row would be passed
 * over, its update lost.  Cloudberry refuses the recheck, as a
 * serialization failure the client may retry, in these words
 * (ExecInitMotion(), nodeMotion.c).  Here as the Motion runs rather than as
 * it is initialised, since EvalPlanQual initialises every subplan of the
 * statement, whichever it runs.
 */
static TupleTableSlot *
motion_exec(CustomScanState *node)
{
	if (node->ss.ps.state->es_epq_active != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("EvalPlanQual can not handle subPlan with Motion node")));

	return ExecScan(&node->ss, motion_next, motion_recheck);
}

static void
motion_end(CustomScanState *node)
{
	MotionState *state = (MotionState *) node;

	motion_finish(state);
	motion_end_stream(state);
	if (state->file_own)
		BufFileClose(state->file);
	state->file = NULL;
	state->file_own = false;
	if (state->spool != NULL)
	{
		tuplestore_end(state->spool);
		ExecDropSingleTupleTableSlot(state->spoolslot);
		state->spool = NULL;
	}
	if (state->sort != NULL)
	{
		tuplesort_end(state->sort);
		ExecDropSingleTupleTableSlot(state->sortslot);
		state->sort = NULL;
	}
	if (state->returned != NULL)
	{
		tuplestore_end(state->returned);
		ExecDropSingleTupleTableSlot(state->returnedslot);
		state->returned = NULL;
	}


	/* The segments are done with the rows this Gather's Motions sent them. */
	if (!state->receiving && state->key != NULL)
		GpDispatchCommand(psprintf("SELECT gp_internal.motion_drop(%s)",
								   quote_literal_cstr(state->key)));
	if (state->segslots != NULL)
	{
		for (int i = 0; i < state->nsegs; i++)
			ExecDropSingleTupleTableSlot(state->segslots[i]);
		ExecDropSingleTupleTableSlot(state->receive);
	}
	if (outerPlanState(node) != NULL)
		ExecEndNode(outerPlanState(node));
}

/*
 * Read again: the segments run the fragment again.  ORCA puts a Materialize
 * above a Motion that would be rescanned, as Cloudberry's executor cannot
 * rescan one; this one can, at the price of the round trip.
 */
static void
motion_rescan(CustomScanState *node)
{
	MotionState *state = (MotionState *) node;

	/*
	 * A sorted Gather into this segment reads again what it sorted, and one
	 * that has sorted nothing yet has read nothing.
	 */
	if (state->receiving && state->nkeys > 0)
	{
		if (state->sort != NULL)
			tuplesort_rescan(state->sort);
		return;
	}

	/* What a streaming slice sent is read again from what was kept of it. */
	if (state->streamed)
	{
		while (!state->stream_done && !state->replaying)
		{
			TupleTableSlot *slot = motion_stream_next(state);

			if (TupIsNull(slot))
				break;
			ResetExprContext(state->css.ss.ps.ps_ExprContext);
		}
		state->replaying = true;
		tuplestore_rescan(state->spool);
		return;
	}

	motion_finish(state);
	state->done = false;
	state->merging = false;
	state->fileno = 0;
	state->offset = 0;
}

/* How many send: one segment, the coordinator, direct dispatch's, or all. */
static int
motion_segments(MotionState *state)
{
	if (state->ncontents > 0)
		return state->ncontents;
	return state->content >= 0 || state->content == GP_MOTION_FROM_COORDINATOR
		? 1 : GpClusterSegmentCount();
}

static const char *
motion_type_name(int type)
{
	switch (type)
	{
		case GP_MOTION_HASH:
			return "Redistribute";
		case GP_MOTION_BROADCAST:
			return "Broadcast";
		case GP_MOTION_RANDOM:
			return "Redistribute";	/* Cloudberry prints a random one so too */
		case GP_MOTION_EXPLICIT:
			return "Explicit Redistribute";
		case GP_MOTION_DML:
			return "Dispatch";
		default:
			return "Gather";
	}
}

static void
motion_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	MotionState *state = (MotionState *) node;
	List	   *context;
	List	   *result = NIL;
	bool		useprefix;
	TupleDesc	tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/* In text the node's name says what it is; other formats need saying. */
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyText("Motion Type", motion_type_name(state->type), es);
		ExplainPropertyInteger("Slice", NULL, state->slice, es);
		ExplainPropertyInteger("Senders", NULL, motion_segments(state), es);
	}

	context = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan,
									   ancestors);
	useprefix = list_length(es->rtable) > 1 || es->verbose;

	/* Cloudberry's words for what a Redistribute hashes. */
	if (state->type == GP_MOTION_HASH)
	{
		ListCell   *lc;

		foreach(lc, ((CustomScan *) node->ss.ps.plan)->custom_exprs)
			result = lappend(result,
							 deparse_expression((Node *) lfirst(lc), context,
												useprefix, true));
		ExplainPropertyList("Hash Key", result, es);
		result = NIL;
	}

	if (state->nkeys == 0)
		return;

	/* And for a sorted Motion's keys. */
	for (int i = 0; i < state->nkeys; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, state->keys[i] - 1);
		Var		   *var = makeVar(INDEX_VAR, state->keys[i], att->atttypid,
								  att->atttypmod, att->attcollation, 0);

		result = lappend(result,
						 deparse_expression((Node *) var, context,
											useprefix, true));
	}
	ExplainPropertyList("Merge Key", result, es);
}

/*
 * What EXPLAIN calls it, through O4: "Gather Motion 2:1  (slice1; segments:
 * 2)", as Cloudberry does -- how many segments send, to the one that
 * receives, which slice they are, and how many run it.
 */
static void
motion_explain_label(PlanState *planstate, ExplainState *es,
					 const char **pname, const char **suffix)
{
	if (GpSplitExplainLabel(planstate, es, pname, suffix))
		return;

	if (IsA(planstate, CustomScanState) &&
		((CustomScanState *) planstate)->methods == &motion_exec_methods)
	{
		MotionState *state = (MotionState *) planstate;
		int			nsegs = motion_segments(state);
		int			receivers = state->type == GP_MOTION_GATHER ? 1
			: GpClusterSegmentCount();
		const char *name = motion_type_name(state->type);

		/*
		 * The segments write, and send nothing up but their counts -- or,
		 * with RETURNING, the rows it gives, as the Gather Motion over the
		 * ModifyTable of Cloudberry's plan sends them.
		 */
		if (state->type == GP_MOTION_DML &&
			planstate->plan->targetlist == NIL)
		{
			*pname = "Dispatch";
			*suffix = psprintf("  (slice%d; segments: %d)", state->slice, nsegs);
			return;
		}
		if (state->type == GP_MOTION_DML)
		{
			name = "Gather";
			receivers = 1;
		}
		*pname = psprintf("%s Motion %d:%d", name, nsegs, receivers);
		*suffix = psprintf("  (slice%d; segments: %d)", state->slice, nsegs);
		return;
	}

	/* Cloudberry's Result with hash filters, which this is */
	if (IsA(planstate, CustomScanState) &&
		((CustomScanState *) planstate)->methods == &hash_filter_exec_methods)
	{
		*pname = "Result";
		return;
	}

	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

/* ------------------------------------------------------------------------- */
/* A segment's share of rows every segment has                               */
/* ------------------------------------------------------------------------- */

/*
 * Where ORCA would redistribute rows that every segment already has -- a
 * replicated table, a function every segment computes alike -- it keeps on
 * each segment the rows that hash to it instead: Cloudberry's Result with
 * hash filters, from which no row moves.  With no key to hash, the rows are
 * kept on one segment chosen when the plan was made.  The filter reads the
 * node's own output columns, as Cloudberry's does.
 */
typedef struct HashFilterState
{
	CustomScanState css;
	int			nkeys;
	AttrNumber *cols;
	GpHash		hash;
	int			segment;		/* with no keys, the one segment that keeps */
} HashFilterState;

static Node *hash_filter_create_state(CustomScan *cscan);
static void hash_filter_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *hash_filter_exec(CustomScanState *node);
static void hash_filter_end(CustomScanState *node);
static void hash_filter_rescan(CustomScanState *node);
static void hash_filter_explain(CustomScanState *node, List *ancestors,
								ExplainState *es);

static const CustomScanMethods hash_filter_scan_methods = {
	.CustomName = "GpHashFilter",
	.CreateCustomScanState = hash_filter_create_state,
};

static const CustomExecMethods hash_filter_exec_methods = {
	.CustomName = "GpHashFilter",
	.BeginCustomScan = hash_filter_begin,
	.ExecCustomScan = hash_filter_exec,
	.EndCustomScan = hash_filter_end,
	.ReScanCustomScan = hash_filter_rescan,
	.ExplainCustomScan = hash_filter_explain,
};

Plan *
GpMotionMakeHashFilter(Plan *child, List *targetlist, List *qual, int nkeys,
					   const AttrNumber *cols, const Oid *hashfuncs,
					   int segment)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *collist = NIL;
	List	   *funclist = NIL;
	List	   *scan_tlist = NIL;
	ListCell   *lc;

	for (int i = 0; i < nkeys; i++)
	{
		collist = lappend_int(collist, cols[i]);
		funclist = lappend_oid(funclist, hashfuncs[i]);
	}

	/* the scan tuple: the child's row */
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
															  exprType((Node *) tle->expr),
															  exprTypmod((Node *) tle->expr),
															  exprCollation((Node *) tle->expr),
															  0),
											 list_length(scan_tlist) + 1,
											 tle->resname, false));
	}

	/*
	 * Its expressions read the child's row as its scan tuple, as a scan
	 * node's are expected to; custom_scan_tlist says where that comes from.
	 */
	cscan->scan.plan.targetlist =
		(List *) outer_to_index_mutator((Node *) targetlist, NULL);
	cscan->scan.plan.qual = (List *) outer_to_index_mutator((Node *) qual, NULL);
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = list_make3(collist, funclist, makeInteger(segment));
	cscan->methods = &hash_filter_scan_methods;
	return (Plan *) cscan;
}

static Node *
hash_filter_create_state(CustomScan *cscan)
{
	HashFilterState *state = (HashFilterState *) newNode(sizeof(HashFilterState),
														 T_CustomScanState);

	state->css.methods = &hash_filter_exec_methods;
	return (Node *) state;
}

static void
hash_filter_begin(CustomScanState *node, EState *estate, int eflags)
{
	HashFilterState *state = (HashFilterState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *cols = (List *) linitial(cscan->custom_private);
	List	   *funcs = (List *) lsecond(cscan->custom_private);

	state->nkeys = list_length(cols);
	state->segment = intVal(lthird(cscan->custom_private));
	state->cols = palloc_array(AttrNumber, Max(state->nkeys, 1));
	state->hash.ptype = POLICYTYPE_PARTITIONED;
	state->hash.numsegs = GpClusterSegmentCount();
	state->hash.nattrs = state->nkeys;
	state->hash.attrs = palloc_array(AttrNumber, Max(state->nkeys, 1));
	state->hash.hashfuncs = palloc_array(FmgrInfo, Max(state->nkeys, 1));
	for (int i = 0; i < state->nkeys; i++)
	{
		state->cols[i] = (AttrNumber) list_nth_int(cols, i);
		state->hash.attrs[i] = i + 1;
		GpHashSetFunction(&state->hash, i, list_nth_oid(funcs, i));
	}

	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
}

static TupleTableSlot *
hash_filter_exec(CustomScanState *node)
{
	HashFilterState *state = (HashFilterState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			self = GpClusterContentId();

	for (;;)
	{
		TupleTableSlot *child = ExecProcNode(outerPlanState(node));
		TupleTableSlot *slot;

		if (TupIsNull(child))
			return NULL;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = child;
		if (node->ss.ps.qual != NULL && !ExecQual(node->ss.ps.qual, econtext))
			continue;

		/*
		 * With no projection the child's row goes out as it is, through the
		 * scan slot: the node above reads this node's rows as the kind of
		 * slot that is.
		 */
		slot = node->ss.ps.ps_ProjInfo != NULL
			? ExecProject(node->ss.ps.ps_ProjInfo)
			: ExecCopySlot(node->ss.ss_ScanTupleSlot, child);

		if (state->nkeys == 0)
		{
			if (self != state->segment)
				continue;
		}
		else
		{
			Datum	   *values = palloc_array(Datum, state->nkeys);
			bool	   *isnull = palloc_array(bool, state->nkeys);

			for (int i = 0; i < state->nkeys; i++)
				values[i] = slot_getattr(slot, state->cols[i], &isnull[i]);
			if (GpHashSegment(&state->hash, values, isnull) != self)
				continue;
		}
		return slot;
	}
}

static void
hash_filter_end(CustomScanState *node)
{
	ExecEndNode(outerPlanState(node));
}

static void
hash_filter_rescan(CustomScanState *node)
{
	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

static void
hash_filter_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	HashFilterState *state = (HashFilterState *) node;
	List	   *context;
	List	   *result = NIL;

	if (state->nkeys == 0)
	{
		ExplainPropertyInteger("Segment", NULL, state->segment, es);
		return;
	}
	context = set_deparse_context_plan(es->deparse_cxt, node->ss.ps.plan,
									   ancestors);
	for (int i = 0; i < state->nkeys; i++)
	{
		TargetEntry *tle = get_tle_by_resno(node->ss.ps.plan->targetlist,
											state->cols[i]);

		result = lappend(result,
						 deparse_expression((Node *) tle->expr, context,
											list_length(es->rtable) > 1 || es->verbose,
											true));
	}
	ExplainPropertyList("Hash Filter", result, es);
}

/* ------------------------------------------------------------------------- */
/* Carrying it out, on a segment                                             */
/* ------------------------------------------------------------------------- */

/*
 * The fragment a query is, if it is one: SELECT gp_internal.exec_fragment()
 * of a string, and nothing else.
 */
static const char *
fragment_payload(Query *parse, char **key)
{
	TargetEntry *tle;
	FuncExpr   *func;
	Const	   *arg;
	Const	   *keyarg;
	char	   *name;

	if (parse->commandType != CMD_SELECT || parse->rtable != NIL ||
		list_length(parse->targetList) != 1 || parse->jointree == NULL ||
		parse->jointree->quals != NULL || parse->hasSubLinks ||
		parse->cteList != NIL || parse->setOperations != NULL)
		return NULL;

	tle = linitial_node(TargetEntry, parse->targetList);
	if (!IsA(tle->expr, FuncExpr))
		return NULL;
	func = (FuncExpr *) tle->expr;
	if (list_length(func->args) != 2 || !IsA(linitial(func->args), Const) ||
		!IsA(lsecond(func->args), Const))
		return NULL;

	/* By name rather than a remembered oid: the extension may be made again. */
	name = get_func_name(func->funcid);
	if (name == NULL || strcmp(name, "exec_fragment") != 0 ||
		get_func_namespace(func->funcid) != get_namespace_oid("gp_internal", true))
		return NULL;

	arg = (Const *) linitial(func->args);
	if (arg->constisnull || arg->consttype != TEXTOID)
		return NULL;
	keyarg = (Const *) lsecond(func->args);
	if (keyarg->constisnull || keyarg->consttype != TEXTOID)
		return NULL;
	*key = TextDatumGetCString(keyarg->constvalue);
	return TextDatumGetCString(arg->constvalue);
}

static PlannedStmt *
fragment_plan(const char *payload, const char *key)
{
	PlannedStmt *stmt;
	Node	   *node;
	ListCell   *lc;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a plan is carried out only for the coordinator"),
				 errdetail("The connection does not carry this cluster's secret."),
				 errhint("Set \"gp.cluster_secret\" to the same value on every node.")));

	node = stringToNode(payload);
	if (!IsA(node, PlannedStmt))
		elog(ERROR, "a dispatched plan fragment is not a PlannedStmt");
	stmt = (PlannedStmt *) node;

	/*
	 * Every column the fragment produces is one the Motion receives: a
	 * resjunk column would be dropped by the portal's junk filter, and the
	 * rows would arrive a column short.
	 */
	if (stmt->commandType == CMD_SELECT)
		foreach(lc, stmt->planTree->targetlist)
			lfirst_node(TargetEntry, lc)->resjunk = false;

	/*
	 * A segment runs a slice in one process.  The fragment is a copy of the
	 * coordinator's whole statement, which may need parallel mode for a
	 * part the coordinator runs; this part starts no workers, and a reader,
	 * a member of its writer's lock group, could not lead any (gp_share.c).
	 */
	stmt->parallelModeNeeded = false;

	/* what the coordinator marked it with, and that it is a fragment */
	stmt->extension_state = lappend(stmt->extension_state,
									makeDefElem(pstrdup(GP_FRAGMENT_MARK),
												(Node *) makeString(pstrdup(key)),
												-1));
	return stmt;
}

static bool
is_fragment(PlannedStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->extension_state)
		if (strcmp(lfirst_node(DefElem, lc)->defname, GP_FRAGMENT_MARK) == 0)
			return true;
	return false;
}

/*
 * The INFO line of each slice the statement dispatches, when
 * gp.test_print_direct_dispatch_info asks for them: read off the slice
 * table, in the order Cloudberry's dispatcher sends the slices
 * (compare_slice_order(), cdb/dispatcher/cdbdisp_query.c) -- the largest
 * gang first, and of two alike the one with fewer slices below it.
 */
typedef struct SliceReport
{
	int			index;
	int			size;
	int			below;
	bool		single;
	List	   *contents;		/* direct dispatch's segments, if several */
} SliceReport;

static int
slice_report_cmp(const void *a, const void *b)
{
	const SliceReport *x = (const SliceReport *) a;
	const SliceReport *y = (const SliceReport *) b;

	if (x->size != y->size)
		return x->size > y->size ? -1 : 1;
	if (x->below != y->below)
		return x->below < y->below ? -1 : 1;
	return x->index - y->index;
}

static void
report_slices(PlannedStmt *stmt)
{
	List	   *table = (List *) fragment_mark(stmt, GP_SLICE_TABLE);
	int			n = list_length(table);
	int		   *parent;
	SliceReport *reports;
	int			nreports = 0;
	ListCell   *lc;

	if (!gp_test_print_direct_dispatch_info || table == NIL)
		return;

	parent = palloc_array(int, n);
	reports = palloc0_array(SliceReport, n);
	foreach(lc, table)
	{
		List	   *slice = (List *) lfirst(lc);
		int			index = intVal(linitial(slice));

		if (index >= 0 && index < n)
			parent[index] = intVal(lsecond(slice));
	}

	foreach(lc, table)
	{
		List	   *slice = (List *) lfirst(lc);
		int			index = intVal(linitial(slice));
		int			gang = intVal(list_nth(slice, 2));
		int			nsegs = intVal(list_nth(slice, 3));
		int			direct = intVal(list_nth(slice, 5));
		List	   *several = list_length(slice) > 6 ? (List *) list_nth(slice, 6) : NIL;
		SliceReport *r;

		/* the coordinator's own slice is not dispatched */
		if (gang == 0)
			continue;
		r = &reports[nreports++];
		/* a write on the segments is Cloudberry's root slice, slice 0 */
		r->index = gang == 4 ? 0 : index;
		/* an entry slice, a singleton, and a direct dispatch are one process */
		r->single = gang == 1 || gang == 2 || direct >= 0 || nsegs == 1;
		r->size = r->single ? 1 : several != NIL ? list_length(several) : nsegs;
		r->contents = several;
		for (int i = 0; i < n; i++)
			for (int p = parent[i]; p >= 0 && p < n && p != i; p = parent[p])
				if (p == index)
				{
					r->below++;
					break;
				}
	}

	qsort(reports, nreports, sizeof(SliceReport), slice_report_cmp);
	for (int i = 0; i < nreports; i++)
	{
		if (!reports[i].single && reports[i].contents != NIL)
		{
			int			ncontents = 0;
			int		   *contents = palloc_array(int, list_length(reports[i].contents));

			foreach_int(c, reports[i].contents)
				contents[ncontents++] = c;
			GpReportDispatchContents(reports[i].index, contents, ncontents);
		}
		else
			GpReportDispatch(reports[i].index, reports[i].single, 0);
	}
}

/*
 * A cursor's plan, its gathers started as it is declared.
 *
 * A gather opens its segments' cursors when it is first read, which for a
 * cursor is its first FETCH: the segments' cursors took their snapshots
 * then, and a statement of the transaction's between the DECLARE and the
 * FETCH -- a DELETE of rows the cursor was to read -- was already in what
 * they read, where on the coordinator a cursor reads as of its DECLARE.
 * Cloudberry's dispatches a cursor's slices as it is declared
 * (ExecutorStart(), CdbDispatchPlan()), and so does this: each gather and
 * Gather Motion of the plan, as the executor has made it, opens its
 * segments' cursors now, which take their snapshots before any later
 * statement of the transaction runs there.
 *
 * Not a Motion whose fragment is sent values the coordinator computes as it
 * runs -- a parameter of a NestLoop's above it, or an initplan's -- nor a
 * gather for WHERE CURRENT OF: those are read as they were.
 */
static bool
motion_start_early(MotionState *state)
{
	List	   *priv = ((CustomScan *) state->css.ss.ps.plan)->custom_private;

	if (state->sending || state->receiving || state->streamed ||
		state->type != GP_MOTION_GATHER || state->gather != NULL || state->done)
		return false;
	if (list_length(priv) > MOTION_PRIVATE_EXTERN_PARAMS &&
		(List *) list_nth(priv, MOTION_PRIVATE_EXEC_PARAMS) != NIL)
		return false;
	motion_start(state);
	return true;
}

static bool
start_early_walker(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;
	/* what is below one is the segments' */
	if (GpGatherScanStartEarly(ps))
		return false;
	if (IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &motion_exec_methods)
	{
		(void) motion_start_early((MotionState *) ps);
		return false;
	}
	return planstate_tree_walker(ps, start_early_walker, context);
}

/* Is this the start of a cursor's portal, which other statements may interleave? */
static bool
starting_cursor(void)
{
	return ActivePortal != NULL && ActivePortal->name != NULL &&
		ActivePortal->name[0] != '\0' &&
		ActivePortal->status == PORTAL_DEFINED &&
		ActivePortal->queryDesc == NULL;
}

/*
 * The writer's fragment of a statement whose slices run at once: its
 * snapshot and its transaction's state, for its readers, before anything of
 * it runs -- they wait for it to start.
 */
/*
 * Is this the cursor of a gather whose privileges the coordinator checked
 * (gp_scan.c's gather_start())?  Only its own text says so -- "DECLARE
 * gp_gather_N [BINARY ]NO SCROLL CURSOR FOR" and the marker right after,
 * which nothing a user writes can put there -- and only the coordinator's
 * own connection is believed.
 */
static bool
gather_was_checked(const char *query_string)
{
	const char *p;

	if (query_string == NULL || strncmp(query_string, "DECLARE gp_gather_", 18) != 0 ||
		!GpClusterDispatchTrusted())
		return false;
	p = query_string + 18;
	while (isdigit((unsigned char) *p))
		p++;
	if (strncmp(p, " BINARY", 7) == 0)
		p += 7;
	if (strncmp(p, " NO SCROLL CURSOR FOR ", 22) != 0)
		return false;
	return strncmp(p + 22, GP_CHECKED_MARKER, strlen(GP_CHECKED_MARKER)) == 0;
}

static void
motion_executor_start(QueryDesc *queryDesc, int eflags)
{
	List	   *params = NIL;

	if (GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		report_slices(queryDesc->plannedstmt);

	/* a materialized view read from the segments, and not populated */
	if (GpClusterBackendRole() == GP_ROLE_DISPATCH && !GpClusterIsSingleNode())
		GpRefreshCheckScannable(queryDesc->plannedstmt, eflags);

	/*
	 * A gather the coordinator checked the privileges of: none to check
	 * here (gather_was_checked()), so its plan has none, and its range table
	 * points at none, as a reader's fragment has none.
	 */
	if (GpClusterIsDispatched() && gather_was_checked(queryDesc->sourceText))
	{
		foreach_node(RangeTblEntry, rte, queryDesc->plannedstmt->rtable)
			rte->perminfoindex = 0;
		queryDesc->plannedstmt->permInfos = NIL;
	}

	if (GpClusterIsDispatched() && is_fragment(queryDesc->plannedstmt))
	{
		params = (List *) fragment_mark(queryDesc->plannedstmt, GP_PARAMS_MARK);
		if (params != NIL)
			fragment_params_before_start(queryDesc, params);
	}

	if (GpClusterIsDispatched() && is_fragment(queryDesc->plannedstmt))
	{
		Node	   *source = fragment_mark(queryDesc->plannedstmt, GP_SOURCE_MARK);
		ListCell   *lc;

		if (source != NULL)
			pgstat_report_activity(STATE_RUNNING, strVal(source));

		if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		{
			Node	   *key = fragment_mark(queryDesc->plannedstmt, GP_SHARE_MARK);

			if (key != NULL)
				GpSharePublish(strVal(key), queryDesc->snapshot);
		}

		/*
		 * The relations the fragment reads, locked as the coordinator's parser
		 * locked them there: here nothing has parsed them, and the executor
		 * expects them locked.  After the writer has published what its
		 * readers wait for, as Cloudberry's writer publishes its snapshot
		 * before its executor locks anything -- so that a lock another
		 * session holds keeps them all waiting for it, each as itself, and
		 * not the readers waiting for a writer that waits for the lock.
		 */
		foreach(lc, queryDesc->plannedstmt->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			if (rte->rtekind == RTE_RELATION)
				LockRelationOid(rte->relid,
								rte->rellockmode != NoLock ? rte->rellockmode
								: AccessShareLock);
		}
	}

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/* InitPlan()'s last fault, where its plan is set up */
	(void) GP_FAULT("func_init_plan_end");

	if (params != NIL)
		fragment_params_after_start(queryDesc, params);

	if (GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		GpGatherScanMarkRescans(queryDesc->planstate);

	if (GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && starting_cursor())
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);

		(void) start_early_walker(queryDesc->planstate, NULL);
		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * A statement's connections that nothing here asked for are closed with it,
 * and its UDP senders waited for (GpIcForget()).
 */
static void
motion_executor_end(QueryDesc *queryDesc)
{
	char	   *token = NULL;

	if (GpClusterIsDispatched() && is_fragment(queryDesc->plannedstmt) &&
		fragment_mark(queryDesc->plannedstmt, GP_STREAM_MARK) != NULL)
		token = stream_token(queryDesc->plannedstmt);

	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (token != NULL)
		GpIcForget(token);
}

static void
motion_executor_run(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count)
{
	bool		fragment = is_fragment(queryDesc->plannedstmt);

	if (fragment)
		fragment_depth++;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		if (fragment)
			fragment_depth--;
	}
	PG_END_TRY();

	/*
	 * The fragment's plan has run out, and reads no Motion's rows any more.
	 * Their senders are told so, and waited for, now: the coordinator may
	 * close the cursor long after, and this process, idle meanwhile, would
	 * leave a UDP sender waiting for an answer (gp_ic.c).
	 */
	if (fragment && ScanDirectionIsForward(direction) &&
		(count == 0 || queryDesc->estate->es_processed < count) &&
		GpClusterIsDispatched() &&
		fragment_mark(queryDesc->plannedstmt, GP_STREAM_MARK) != NULL)
	{
		ListCell   *lc;

		(void) motion_end_streams(queryDesc->planstate, NULL);
		foreach(lc, queryDesc->estate->es_subplanstates)
			(void) motion_end_streams((PlanState *) lfirst(lc), NULL);
		GpIcForget(stream_token(queryDesc->plannedstmt));
	}
}

/*
 * A query a function runs while a fragment is carried out on a segment.
 *
 * The fragment is one segment's share of the statement, and a query that a
 * function in it plans here would read this segment's share of a table as
 * if it were the table.  Cloudberry refuses such a query on a QE unless it
 * reads only catalogs and replicated tables, and only reads
 * (querytree_safe_for_qe(), executor/functions.c); so does the port, with
 * Cloudberry's words.  A segment knows which tables are replicated from the
 * "gp" label, which the coordinator sends it whenever it changes one
 * (gp_dispatch.c).  A statement the coordinator dispatched itself is planned
 * before any fragment runs, and is not affected.
 */
static bool
is_replicated(Oid relid)
{
	GpPolicy   *policy = GpPolicyGet(relid);

	return policy != NULL && GpPolicyIsReplicated(policy);
}

static bool
fragment_safe_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		if (query->commandType != CMD_SELECT || query->resultRelation > 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function cannot execute on a QE slice because it issues a non-SELECT statement")));

		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			Oid			nsp;

			if (rte->rtekind != RTE_RELATION)
				continue;
			nsp = get_rel_namespace(rte->relid);
			if (!IsCatalogNamespace(nsp) && !IsToastNamespace(nsp) &&
				!is_replicated(rte->relid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function cannot execute on a QE slice because it accesses relation \"%s.%s\"",
								quote_identifier(get_namespace_name(nsp)),
								quote_identifier(get_rel_name(rte->relid)))));
		}
		return query_tree_walker(query, fragment_safe_walker, context, 0);
	}
	return expression_tree_walker(node, fragment_safe_walker, context);
}

static PlannedStmt *
motion_planner(Query *parse, const char *query_string, int cursorOptions,
			   ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;

	if (GpClusterIsDispatched())
	{
		char	   *key = NULL;
		const char *payload = fragment_payload(parse, &key);

		if (payload != NULL)
			return fragment_plan(payload, key);
		if (fragment_depth > 0)
			(void) fragment_safe_walker((Node *) parse, NULL);
	}

	if (prev_planner)
		stmt = prev_planner(parse, query_string, cursorOptions, boundParams,
							es);
	else
		stmt = standard_planner(parse, query_string, cursorOptions,
								boundParams, es);
	return stmt;
}

/*
 * Does the plan go to one segment at most, as a plan Cloudberry dispatches
 * directly does (resgroup.c's can_bypass_direct_dispatch_plan())?  ORCA's
 * says so in its slice table: each slice it dispatches, to one segment.  The
 * planner's, node by node: a gather of one segment's rows ("Segment: 1"), a
 * Dispatch of a write to one, and an INSERT of one row of constants, which
 * one segment takes; anything else sent to the segments is to more.
 */
typedef struct OneSegment
{
	bool		one;			/* nothing seen goes to more */
	int			seen;			/* what goes to the segments */
} OneSegment;

static bool
plan_one_segment_walker(Plan *plan, OneSegment *os)
{
	if (plan == NULL || !os->one)
		return false;
	if (IsA(plan, CustomScan))
	{
		CustomScan *cscan = (CustomScan *) plan;
		const char *name = cscan->methods->CustomName;

		if (strcmp(name, "Gather Motion") == 0)
		{
			List	   *contents = (List *) list_nth(cscan->custom_private, 3);
			int			nsegments = intVal(list_nth(cscan->custom_private, 4));

			os->seen++;
			if (list_length(contents) != 1 && nsegments != 1)
				os->one = false;
		}
		else if (strcmp(name, "Dispatch") == 0)
		{
			os->seen++;
			if (list_length((List *) lthird(cscan->custom_private)) != 1 &&
				intVal(lfourth(cscan->custom_private)) != 1)
				os->one = false;
		}
		else if (strcmp(name, "Redistribute Motion") == 0)
		{
			Plan	   *rows = linitial(cscan->custom_plans);

			os->seen++;
			if (!IsA(rows, Result) || rows->lefttree != NULL)
				os->one = false;
		}
		else if (strcmp(name, GP_MOTION_NAME) == 0 ||
				 strcmp(name, "Explicit Redistribute Motion") == 0)
		{
			/* an explicit one moves a gather's rows, which are judged below */
		}
	}
	if (plan->lefttree != NULL)
		(void) plan_one_segment_walker(plan->lefttree, os);
	if (plan->righttree != NULL)
		(void) plan_one_segment_walker(plan->righttree, os);
	if (IsA(plan, CustomScan))
	{
		foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
			(void) plan_one_segment_walker(child, os);
	}
	else if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			(void) plan_one_segment_walker(child, os);
	}
	else if (IsA(plan, SubqueryScan))
		(void) plan_one_segment_walker(((SubqueryScan *) plan)->subplan, os);
	return false;
}

bool
GpPlanIsDirectDispatch(PlannedStmt *stmt)
{
	List	   *table = (List *) fragment_mark(stmt, GP_SLICE_TABLE);
	OneSegment	os = {true, 0};
	int			dispatched = 0;

	if (table != NIL)
	{
		foreach_ptr(List, slice, table)
		{
			int			gang = intVal(list_nth(slice, 2));
			int			nsegs = intVal(list_nth(slice, 3));
			int			direct = intVal(list_nth(slice, 5));

			if (gang == 0)
				continue;
			dispatched++;
			if (direct < 0 && nsegs != 1)
				return false;
		}
		return dispatched > 0;
	}

	if (stmt->subplans != NIL)
		return false;
	(void) plan_one_segment_walker(stmt->planTree, &os);
	return os.one && os.seen > 0;
}

PG_FUNCTION_INFO_V1(gp_exec_fragment);

/*
 * gp_internal.exec_fragment(text)
 *
 * Never called: on a segment, the planner hook above puts the fragment in
 * its place.  Reached, it is somebody calling it by hand.
 */
Datum
gp_exec_fragment(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("gp_internal.exec_fragment() is not a function to call"),
			 errdetail("It marks a plan fragment the coordinator sends a segment.")));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(gp_interconnect_address);

/*
 * gp_internal.interconnect_address()
 *
 * Where this segment process receives a Motion's rows, opening its listener
 * if it has none yet: what the coordinator hands the senders.  Only for the
 * coordinator.
 */
Datum
gp_interconnect_address(PG_FUNCTION_ARGS)
{
	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("the interconnect is opened only for the coordinator")));
	PG_RETURN_TEXT_P(cstring_to_text(GpIcAddress()));
}

void
GpMotionInit(void)
{
	DefineCustomEnumVariable("gp.interconnect_type",
							 "How the rows of a Motion between segments travel.",
							 "\"tcp\": every slice of a query runs at once, each "
							 "segment process sending its rows straight to the "
							 "ones that receive them, over a Unix socket beside "
							 "the node's own or a TCP port.  \"udpifc\": the "
							 "same, in UDP packets each receiver acknowledges, as "
							 "Cloudberry's UDP interconnect sends them.  "
							 "\"relay\": a slice at a time, its rows relayed "
							 "through the coordinator to files the receiving "
							 "segments keep.",
							 &gp_interconnect_type,
							 GP_INTERCONNECT_TCP,
							 interconnect_type_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&motion_scan_methods);
	RegisterCustomScanMethods(&hash_filter_scan_methods);
	GpSplitInit();

	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = motion_explain_label;

	prev_planner = planner_hook;
	planner_hook = motion_planner;

	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = motion_executor_run;

	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = motion_executor_start;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = motion_executor_end;

	if (!motion_xact_callback_registered)
	{
		RegisterXactCallback(motion_xact_callback, NULL);
		motion_xact_callback_registered = true;
	}
}
