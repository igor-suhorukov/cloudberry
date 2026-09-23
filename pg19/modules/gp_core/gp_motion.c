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
 * Motion in the receiving fragment takes them as they come.  Which slice
 * receives each Motion the translator says (GpMotionSetParent).
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
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "access/xact.h"
#include "common/pg_prng.h"
#include "lib/binaryheap.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "port/pg_bswap.h"
#include "storage/buffile.h"
#include "parser/parse_func.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "varatt.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_hash.h"
#include "gp_ic.h"
#include "gp_motion.h"
#include "gp_policy.h"
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

/* A Motion whose receiving slice the translator did not say. */
#define MOTION_PARENT_UNKNOWN		(-3)

/*
 * How a Motion between segments is carried out: its slices all at once,
 * each sender streaming to its receivers (tcp), or a slice at a time, the
 * rows relayed through the coordinator (relay).
 */
#define GP_INTERCONNECT_RELAY	0
#define GP_INTERCONNECT_TCP		1

static const struct config_enum_entry interconnect_type_options[] = {
	{"relay", GP_INTERCONNECT_RELAY, false},
	{"tcp", GP_INTERCONNECT_TCP, false},
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

/* The SQL a batch of a Motion's rows travels to its receiving segment in. */
#define MOTION_PUT_SQL	"SELECT gp_internal.motion_put($1, $2, $3)"

/* How much of a segment's rows the coordinator holds before sending them. */
#define MOTION_BATCH_BYTES	(256 * 1024)

typedef struct MotionState
{
	CustomScanState css;

	int			content;		/* -1: every segment */
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
	bool		send_binary;
	FmgrInfo   *outprocs;
	ExprState **hashexprs;
	GpHash		hash;
	int		   *receiver_of;	/* by content id: a receiver's index, or -1 */
	int			nreceivers;
	char	  **receivers;
	char	   *token;

	/* On a segment, a Motion that receives a streaming slice. */
	bool		streamed;
	int			nsenders;
	GpIcReceiver *icrecv;
	bool		stream_done;
	Tuplestorestate *spool;		/* what came, for a rescan */
	TupleTableSlot *spoolslot;
	bool		replaying;

	/* A merge: each segment's next row, and which of them is least. */
	int			nsegs;
	TupleTableSlot **segslots;
	TupleTableSlot *receive;	/* what a row is received into first */
	binaryheap *heap;
	bool		merging;
	int			last;			/* the segment whose row went out last */
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
		   type == GP_MOTION_RANDOM);
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
	cscan->methods = &motion_scan_methods;

	return cscan;
}

Plan *
GpMotionMakeDml(Plan *modify, int content, int slice)
{
	Assert(IsA(modify, ModifyTable) || GpSplitModifyIs(modify, NULL));
	return (Plan *) motion_make(GP_MOTION_DML, modify, NIL, NIL, content,
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
 * Direct dispatch, for ORCA: the segment that holds every row whose
 * distribution key is these values, or -1.  The values are the key's, in the
 * key's order, and each has to be of its column's own type -- a hash is a
 * property of the type, and 1::int8 and 1::int4 need not hash alike.
 */
int
GpMotionDirectDispatchSegment(Oid relid, int nvalues, const Oid *types,
							  const Datum *values, const bool *isnull)
{
	GpPolicy   *policy;
	Relation	rel;
	TupleDesc	tupdesc;
	Datum	   *rowvalues;
	bool	   *rownulls;
	int			segment = -1;

	policy = GpPolicyGet(relid);
	if (policy == NULL || !GpPolicyIsHashPartitioned(policy) ||
		policy->nattrs != nvalues)
		return -1;

	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);
	rowvalues = palloc0_array(Datum, tupdesc->natts);
	rownulls = palloc_array(bool, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
		rownulls[i] = true;

	for (int k = 0; k < nvalues; k++)
	{
		AttrNumber	attno = policy->attrs[k];

		if (TupleDescAttr(tupdesc, attno - 1)->atttypid != types[k])
			goto done;
		rowvalues[attno - 1] = values[k];
		rownulls[attno - 1] = isnull[k];
	}
	segment = GpHashSegment(GpHashMake(policy, tupdesc), rowvalues, rownulls);

done:
	relation_close(rel, AccessShareLock);
	return segment;
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
		if (mf == NULL)
			return ExecClearTuple(slot);	/* nothing was sent here */
		state->file = mf->file;
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

/*
 * The Motion at the top of a reader's fragment: it pulls the rows of the
 * slice below it, and sends each where the Motion sends it -- the segment
 * its keys hash to, every one, the next in turn -- to the process that runs
 * the receiving slice there.
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

	state->send_binary = GpTupleDescHasBinaryIO(tupdesc);
	state->outprocs = palloc0_array(FmgrInfo, tupdesc->natts);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Oid			proc;
		bool		isvarlena;

		if (state->send_binary)
			getTypeBinaryOutputInfo(TupleDescAttr(tupdesc, i)->atttypid,
									&proc, &isvarlena);
		else
			getTypeOutputInfo(TupleDescAttr(tupdesc, i)->atttypid,
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
		fmgr_info(lfirst_oid(lf), &state->hash.hashfuncs[i]);
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
		resetStringInfo(&row);
		append_row_raw(&row, natts, values, lengths);

		if (state->type == GP_MOTION_HASH)
		{
			econtext->ecxt_outertuple = slot;
			for (int i = 0; i < nkeys; i++)
				keyvalues[i] = ExecEvalExpr(state->hashexprs[i], econtext,
											&keynulls[i]);
			target = GpHashSegment(&state->hash, keyvalues, keynulls);
		}
		else if (state->type == GP_MOTION_RANDOM)
			target = next++ % nsegs;
		MemoryContextSwitchTo(oldcxt);

		if (state->type == GP_MOTION_BROADCAST)
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

/* A row as it travels, into the scan slot. */
static TupleTableSlot *
motion_decode_row(MotionState *state, const char *data, int len)
{
	TupleTableSlot *slot = state->css.ss.ss_ScanTupleSlot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	const char *p = data;
	const char *end = data + len;
	uint16		natts;
	MemoryContext oldcxt;

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
									  state->nsenders);
	if (GpIcRecv(state->icrecv, &data, &len))
	{
		motion_decode_row(state, data, len);
		tuplestore_puttupleslot(state->spool, slot);
		return slot;
	}
	GpIcRecvEnd(state->icrecv);
	state->icrecv = NULL;
	state->stream_done = true;
	return ExecClearTuple(slot);
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
	state->slice = intVal(list_nth(priv, MOTION_PRIVATE_SLICE));
	state->type = intVal(list_nth(priv, MOTION_PRIVATE_TYPE));
	state->nkeys = list_length(keys);

	/*
	 * On a segment, a Motion between segments receives: what the coordinator
	 * relayed to this segment, in the file it keeps under the statement's
	 * key.  A Gather is never run on a segment.
	 */
	if (GpClusterBackendRole() == GP_ROLE_EXECUTE &&
		(state->type == GP_MOTION_HASH || state->type == GP_MOTION_BROADCAST ||
		 state->type == GP_MOTION_RANDOM) &&
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

		/* A slice that streams, received as it comes rather than from a file. */
		if (entry != NULL)
		{
			state->streamed = true;
			state->token = stream_token(estate->es_plannedstmt);
			state->nsenders = intVal(lsecond(entry));
			state->spool = tuplestore_begin_heap(false, false, work_mem);
			state->spoolslot = MakeSingleTupleTableSlot(tupdesc,
														&TTSOpsMinimalTuple);
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
				getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
									   &proc, &state->inparams[i]);
			else
				getTypeInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
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
		outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate,
											eflags | EXEC_FLAG_EXPLAIN_ONLY);
}

/*
 * A fragment, as the query a segment is sent: the PlannedStmt it is part of
 * with it as the tree, and the key its Motions' rows are kept under.
 */
static char *
fragment_sql_ex(EState *estate, Plan *fragment, const char *key, List *marks,
				bool reader)
{
	PlannedStmt *whole = estate->es_plannedstmt;
	PlannedStmt *frag = makeNode(PlannedStmt);
	bool		split = GpSplitModifyIs(fragment, NULL);
	bool		write = IsA(fragment, ModifyTable) || split;

	/* a write is the statement's own command; everything else reads */
	memcpy(frag, whole, sizeof(PlannedStmt));
	frag->commandType = !write ? CMD_SELECT
		: split ? CMD_UPDATE : ((ModifyTable *) fragment)->operation;
	frag->hasReturning = false;
	frag->hasModifyingCTE = false;
	frag->canSetTag = true;
	frag->planTree = fragment;
	if (!write)
		frag->resultRelationRelids = NULL;
	frag->rowMarks = NIL;
	frag->extension_state = marks;
	frag->utilityStmt = NULL;

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

static char *
fragment_sql(EState *estate, Plan *fragment, const char *key)
{
	return fragment_sql_ex(estate, fragment, key, NIL, false);
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
motion_flush(const char *key, int slice, int content, StringInfo buf)
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
	GpDispatchParamsOnContent(content, MOTION_PUT_SQL, 3, values, lengths,
							  formats);
	resetStringInfo(buf);
}

/*
 * Carry out a Motion between segments: run the slice that sends, and relay
 * each of its rows to the segments that receive it.  The receiving slice
 * reads them later, from the file each segment keeps them in.
 */
static void
motion_relay(MotionState *gather, CustomScan *motion)
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
			fmgr_info(lfirst_oid(lf), &hash.hashfuncs[i]);
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
				getTypeBinaryOutputInfo(TupleDescAttr(tupdesc, i)->atttypid,
										&proc, &isvarlena);
			else
				getTypeOutputInfo(TupleDescAttr(tupdesc, i)->atttypid,
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
				GpHashSegment(&hash, keyvalues, keynulls) : next++ % nsegs;
			if (type == GP_MOTION_BROADCAST)
			{
				for (i = 0; i < nsegs; i++)
					appendBinaryStringInfo(&bufs[i], row.data, row.len);
			}
			else
				appendBinaryStringInfo(&bufs[target], row.data, row.len);
			ResetExprContext(econtext);

			for (i = 0; i < nsegs; i++)
				if (bufs[i].len >= MOTION_BATCH_BYTES)
					motion_flush(gather->key, slice, i, &bufs[i]);
		}
		ExecEndNode(ps);
	}
	else
	{
		GpGatherState *g;
		MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

		g = GpGatherStartOn(fragment_sql(estate, child, gather->key), tupdesc,
							content);
		MemoryContextSwitchTo(oldcxt);

		while (GpGatherNextRaw(g, values, lengths))
		{
			int			target = 0;

			resetStringInfo(&row);
			append_row_raw(&row, natts, values, lengths);

			if (type == GP_MOTION_HASH)
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
				target = GpHashSegment(&hash, keyvalues, keynulls);
				MemoryContextSwitchTo(oldcxt);
				ResetExprContext(econtext);
			}
			else if (type == GP_MOTION_RANDOM)
				target = next++ % nsegs;

			if (type == GP_MOTION_BROADCAST)
			{
				for (i = 0; i < nsegs; i++)
					appendBinaryStringInfo(&bufs[i], row.data, row.len);
			}
			else
				appendBinaryStringInfo(&bufs[target], row.data, row.len);

			for (i = 0; i < nsegs; i++)
				if (bufs[i].len >= MOTION_BATCH_BYTES)
					motion_flush(gather->key, slice, i, &bufs[i]);
		}
		GpGatherEnd(g);
	}

	for (i = 0; i < nsegs; i++)
		motion_flush(gather->key, slice, i, &bufs[i]);

	ExecDropSingleTupleTableSlot(keyslot);
	FreeExprContext(econtext, true);
}

/* The segments a slice runs on: every one, or the one it names. */
static bool
content_includes(int content, int segment)
{
	return content == -1 || content == segment;
}

/*
 * Can the Motions below this Gather stream -- every slice running at once,
 * a reader on each segment for each slice the writer does not run?  When
 * they can, the slices that stream, with the one each sends to.
 *
 * The relay stays for what streaming cannot do yet: a slice the coordinator
 * sends to one the writer does not run, whose rows only the writer's files
 * can take; a temporary table, which only its session's own backend -- the
 * writer -- can read; a reader on a segment whose writer runs nothing and so
 * publishes no snapshot, which direct dispatch makes; a slice whose receiver
 * the translator did not say.
 */
static bool
stream_plan(MotionState *state, List *order, List *motions)
{
	EState	   *estate = state->css.ss.ps.state;
	int			top = GpMotionSlice(state->css.ss.ps.plan);
	int			nsegs = GpClusterSegmentCount();
	List	   *slices = NIL;
	ListCell   *lc;

	if (gp_interconnect_type != GP_INTERCONNECT_TCP)
		return false;

	foreach(lc, estate->es_range_table)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_RELATION &&
			get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
			return false;
	}

	foreach(lc, order)
	{
		int			slice = lfirst_int(lc);
		CustomScan *motion = NULL;
		StreamSlice *ss;
		int			content;
		int			parent;

		foreach_ptr(Plan, m, motions)
			if (GpMotionSlice(m) == slice)
				motion = (CustomScan *) m;
		if (motion == NULL)
			return false;
		content = GpMotionSegment((Plan *) motion);
		parent = GpMotionParent((Plan *) motion);
		if (parent == MOTION_PARENT_UNKNOWN)
			return false;
		if (content == GP_MOTION_FROM_COORDINATOR)
		{
			if (parent != top)
				return false;
			continue;
		}

		ss = palloc0(sizeof(StreamSlice));
		ss->motion = motion;
		ss->slice = slice;
		ss->parent = parent;
		ss->contents = palloc_array(int, nsegs);
		for (int seg = 0; seg < nsegs; seg++)
		{
			if (!content_includes(content, seg))
				continue;
			if (!content_includes(state->content, seg))
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
		if (content_includes(state->content, seg))
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
				if (!content_includes(state->content, seg))
					continue;
				contents = lappend(contents, makeInteger(seg));
				addresses = lappend(addresses,
									makeString(pstrdup(writer_address[seg])));
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
										makeString(pstrdup(p->addresses[i])));
				}
			}
		}
		entries = lappend(entries,
						  list_make4(makeInteger(ss->slice),
									 makeInteger(ss->ncontents),
									 contents, addresses));
	}
	streammark = makeDefElem(pstrdup(GP_STREAM_MARK),
							 (Node *) list_make2(makeString(pstrdup(token)),
												 entries), -1);

	/*
	 * Each reader: its own transaction, read as a part of its writer's, and
	 * the Motion it sends through as its fragment.
	 */
	foreach_ptr(StreamSlice, ss, state->stream_slices)
	{
		char	   *fragment = fragment_sql_ex(estate, (Plan *) ss->motion,
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

		/* A streaming slice runs with the Gather's; the coordinator's, first. */
		if (state->streaming &&
			GpMotionSegment((Plan *) motion) != GP_MOTION_FROM_COORDINATOR)
			continue;
		motion_relay(state, motion);
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
		rti = linitial_int(((ModifyTable *) write)->resultRelations);
		operation = ((ModifyTable *) write)->operation;
	}
	rte = rt_fetch(rti, estate->es_range_table);
	policy = GpPolicyGet(rte->relid);

	/*
	 * Cloudberry without its global deadlock detector: an UPDATE or DELETE
	 * of a distributed table locks the table, so that two of them never wait
	 * for each other on different segments.
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
		LockRelationOid(rte->relid, ExclusiveLock);

	if (!state->prepared)
		motion_prepare(state);

	GpDispatchCommandParams(fragment_sql_ex(estate, write, state->key,
											state->streaming ? stream_start(state) : NIL,
											false),
							0, NULL, state->content, counts);
	stream_end(state);

	/* every segment writes a replicated table's rows alike: count them once */
	if (policy != NULL && GpPolicyIsReplicated(policy))
		total = counts[0];
	else
		for (int i = 0; i < nsegs; i++)
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
	state->gather = GpGatherStartOn(fragment_sql_ex(state->css.ss.ps.state,
													outerPlan(state->css.ss.ps.plan),
													state->key,
													state->streaming ? stream_start(state) : NIL,
													false),
									slot->tts_tupleDescriptor,
									state->content);
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

	if (state->streamed)
		return motion_stream_next(state);

	if (state->receiving)
		return motion_recv_next(state);

	if (state->type == GP_MOTION_DML)
	{
		motion_dml_run(state);
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

static TupleTableSlot *
motion_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, motion_next, motion_recheck);
}

static void
motion_end(CustomScanState *node)
{
	MotionState *state = (MotionState *) node;

	motion_finish(state);

	/* A streaming slice's senders stop sending here. */
	if (state->icrecv != NULL)
		GpIcRecvEnd(state->icrecv);
	state->icrecv = NULL;
	if (state->spool != NULL)
	{
		tuplestore_end(state->spool);
		ExecDropSingleTupleTableSlot(state->spoolslot);
		state->spool = NULL;
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

/* How many send: one segment, the coordinator, or all of them. */
static int
motion_segments(MotionState *state)
{
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

		if (state->type == GP_MOTION_DML)
		{
			/* the segments write, and send nothing up but their counts */
			*pname = "Dispatch";
			*suffix = psprintf("  (slice%d; segments: %d)", state->slice, nsegs);
			return;
		}
		*pname = psprintf("%s Motion %d:%d", motion_type_name(state->type),
						  nsegs, receivers);
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
		fmgr_info(list_nth_oid(funcs, i), &state->hash.hashfuncs[i]);
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
	 * The coordinator's parser locked the relations the statement reads; here
	 * nothing has parsed them, and the executor expects them locked.
	 */
	foreach(lc, stmt->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_RELATION)
			LockRelationOid(rte->relid,
							rte->rellockmode != NoLock ? rte->rellockmode
							: AccessShareLock);
	}

	/*
	 * Every column the fragment produces is one the Motion receives: a
	 * resjunk column would be dropped by the portal's junk filter, and the
	 * rows would arrive a column short.
	 */
	if (stmt->commandType == CMD_SELECT)
		foreach(lc, stmt->planTree->targetlist)
			lfirst_node(TargetEntry, lc)->resjunk = false;

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
 * The writer's fragment of a statement whose slices run at once: its
 * snapshot and its transaction's state, for its readers, before anything of
 * it runs -- they wait for it to start.
 */
static void
motion_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (GpClusterIsDispatched() && is_fragment(queryDesc->plannedstmt) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		Node	   *key = fragment_mark(queryDesc->plannedstmt, GP_SHARE_MARK);

		if (key != NULL)
			GpSharePublish(strVal(key), queryDesc->snapshot);
	}

	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/* A statement's connections that nothing here asked for are closed with it. */
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
}

/*
 * A query a function runs while a fragment is carried out on a segment.
 *
 * The fragment is one segment's share of the statement, and a query that a
 * function in it plans here would read this segment's share of a table as
 * if it were the table.  Cloudberry refuses such a query on a QE unless it
 * reads only catalogs and replicated tables, and only reads
 * (querytree_safe_for_qe(), executor/functions.c); so does the port, with
 * Cloudberry's words -- and replicated tables refused too, because a
 * segment does not know a table's distribution: the "gp" label that says it
 * is kept on the coordinator.  A statement the coordinator dispatched itself
 * is planned before any fragment runs, and is not affected.
 */
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
			if (!IsCatalogNamespace(nsp) && !IsToastNamespace(nsp))
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
		return prev_planner(parse, query_string, cursorOptions, boundParams,
							es);
	return standard_planner(parse, query_string, cursorOptions, boundParams,
							es);
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
							 "the node's own or a TCP port.  \"relay\": a slice "
							 "at a time, its rows relayed through the coordinator "
							 "to files the receiving segments keep.",
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
