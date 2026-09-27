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
 * gp_explicit.c
 *	  Writing a distributed table from the coordinator's plan, when
 *	  PostgreSQL's planner plans: Cloudberry's Explicit Redistribute Motion.
 *
 * gp_modify.c writes a distributed table two ways: an INSERT's rows routed
 * by COPY, and an UPDATE or DELETE sent to the segments as it stands.  What
 * neither can do -- an UPDATE or DELETE that reads another distributed
 * table, one of a partitioned table, one in a WITH query, and a write with
 * RETURNING -- is done here: the coordinator runs the plan PostgreSQL's
 * planner made, over the rows the gathers bring it, and every row that plan
 * would change is changed on the segment that holds it.
 *
 * Which row, and where, is what a gather of the table being changed reads
 * with it: its ctid, and the segment it came from (gp_scan.c).  A ctid is
 * only one segment's, so the gather gives the plan a ctid of its own for
 * the pair, from a map this file keeps for the statement, and the plan
 * carries it up to the write as it carries any row's ctid.  The write --
 * this node, in ModifyTable's place -- runs the plan to its end, looks each
 * row's pair up, and sends each segment its rows, a batch a statement:
 *
 *	   UPDATE t AS gp_t SET a = gp_s.gp_c1, ...
 *		 FROM (VALUES ($1::tid, $2::oid, $3::int8, $4::type, ...), ...)
 *			  AS gp_s (gp_ctid, gp_toid, gp_n, gp_c1, ...)
 *		WHERE gp_t.ctid = gp_s.gp_ctid AND gp_t.tableoid = gp_s.gp_toid
 *
 * so that each segment's own UPDATE checks the constraints, fires the row
 * triggers and moves a row between partitions, as Cloudberry's segments
 * do; a partitioned table's rows are written through its root.  A target
 * row the plan finds more than once is sent once, by the first of the
 * plan's rows that finds it, as PostgreSQL's UPDATE ... FROM changes it
 * once; a Split refuses the second, in Cloudberry's words.  The rows are
 * sent once the plan has finished with the segments, as the routed INSERT's
 * are, and an UPDATE or DELETE locks the table as Cloudberry without its
 * global deadlock detector locks it, so that no row changes between being
 * read and being written.  With the detector on, rows are locked instead,
 * and one may: a segment's statement then rechecks its new version, which
 * the ctid it was sent never matches, and passes it over.  So a statement
 * that wrote fewer rows than it was sent asks the segment why
 * (explicit_recheck(), gp_split.c): a row another transaction updated fails
 * it, as the recheck below Cloudberry's Motion fails it, and one deleted is
 * passed over, as PostgreSQL passes it over -- but for a Split's, and a
 * MERGE's that may insert instead.
 *
 * An INSERT's rows go to the segment their key hashes to, every segment for
 * a replicated table.  An UPDATE that sets a column of the key moves each
 * row, as Cloudberry's Split Update does: deleted where it is, returning it,
 * and its new version inserted where it hashes -- by the segments' split
 * functions, which fire no trigger, as Cloudberry's Split fires none, where
 * the cluster has the secret a segment trusts the coordinator by
 * (gp_split.c), and by a DELETE and an INSERT, which fire them, where it
 * does not (explicit_send_split).  RETURNING is evaluated
 * here: each segment returns the rows it wrote, with the number of the
 * plan's row that asked, and the list the planner made is evaluated over
 * them and that row.  Where it reads old or new by name, each row comes
 * back with its other version too -- an UPDATE's old row, an upsert's
 * existing one, a moved row's deleted one -- and one that is not there
 * is null, as ExecProcessReturning() has it.
 *
 * A replicated table's row is on every segment, and each copy has a ctid of
 * its own: the plan read one segment's.  So the write finds a row by what
 * it holds instead -- its text, read on that segment by the ctid the plan
 * carried, which every segment's copy has too, since every segment was
 * given the same rows -- and sends every segment its statement, counted
 * once:
 *
 *	   UPDATE t AS gp_t SET a = gp_s.gp_c1, ...
 *		 FROM (VALUES ($1::text, $2::oid, $3::int8, $4::type, ...), ...)
 *			  AS gp_s (gp_old, gp_toid, gp_n, gp_c1, ...)
 *		WHERE gp_t::text = gp_s.gp_old AND gp_t.tableoid = gp_s.gp_toid
 *
 * Two copies of one row on a segment are changed together, which is right:
 * Cloudberry does not show a replicated table's system columns (gp_segment.c),
 * so no statement can tell them apart, and one that changes one changes the
 * other.  The new values are computed once, here, so a volatile function
 * gives every segment the same row, where Cloudberry refuses the plan.
 *
 * INSERT ... ON CONFLICT goes the same way, the clause after each segment's
 * VALUES: each row to the segment its key hashes to, where a row it
 * conflicts with is, since every unique index of a distributed table holds
 * its key; the clause is PostgreSQL's own ruleutils' text of it, printed
 * before the planner changes the statement (GpExplicitOnConflict) -- one in
 * a WITH query with the statement it is in (gp_modify.c).  As
 * Cloudberry's analyze.c refuses them, in its words, DO UPDATE refuses a
 * column of the key and, of a replicated table, a volatile function; and a
 * subquery in SET or WHERE is refused, which a segment would answer from
 * its own rows alone.  An upsert that updates locks the table as an UPDATE
 * does, without the global deadlock detector, as Cloudberry's parser locks
 * it (parse_clause.c).  A statement's rows reach a segment in batches of up
 * to EXPLICIT_BATCH_ROWS, so two rows of one key in different batches are
 * the second updating the first, where one statement would refuse them.
 *
 * Check options.  A view's WITH CHECK OPTION is the coordinator's: the
 * segments' statements name the table.  A table's row-level security is
 * each segment's too, its statements running as the session's role; its
 * USING conditions chose the plan's rows here.  The rows written come back
 * for a view's checks, and every check the plan has is evaluated here over
 * them, as ExecInsert() and ExecUpdate() evaluate it (explicit_check); for
 * a policy alone they do not, as a statement that returned rows would
 * apply the table's SELECT policies too.
 *
 * MERGE.  The plan joins source and target here, and carries the target's
 * row up with each row it joins, as a junk column gp_modify.c asks the
 * planner for; this node does for each what ExecMerge() does -- the first
 * action whose WHEN condition holds, of those its match allows, over the
 * target's row as the scan tuple and the plan's as the inner one -- and
 * sends each action's rows by a statement of its kind: an UPDATE of that
 * action's SET columns, a DELETE, an INSERT.  An UPDATE action that sets a
 * column of the key moves its rows, a Split, as Cloudberry's SplitMerge
 * moves them.  The checks a MERGE's rows are put to are made here, on the
 * rows it computed: the target's row against the policies' USING, the new
 * one against their WITH CHECK and a view's.  A row changed twice is
 * refused, in PostgreSQL's words.  The DELETEs go first, then the UPDATEs,
 * the Split, the INSERTs.
 *
 * A write in a WITH query runs to its end whether or not the query reads
 * it, as PostgreSQL's ModifyTable does (ExecPostprocessPlan()).
 *
 * Refused, by name (GpExplicitCannot): an UPDATE of the key of a table with
 * UPDATE triggers, which a moved row would not fire, in Cloudberry's words;
 * statement-level triggers, which would fire on every segment; and a
 * MERGE's RETURNING, whose merge_action() only a MERGE's own node answers.
 *
 * Cloudberry sources this file stands in for:
 *	  the Explicit Redistribute Motion cdbpath.c puts below a ModifyTable
 *	  whose rows came from elsewhere, and the segments' ModifyTable above it;
 *	  the SplitMerge node (nodeSplitMerge.c) of a MERGE that moves rows
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tupconvert.h"
#include "access/xact.h"
#include "optimizer/optimizer.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_dispatch.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_settings.h"

/* Rows a statement carries, at most, and parameters, at most. */
#define EXPLICIT_BATCH_ROWS		1000
#define EXPLICIT_MAX_PARAMS		30000

/* Rows a call of a Split's function on a segment is given, at most. */
#define EXPLICIT_SPLIT_ROWS		10000

/* ------------------------------------------------------------------------- */
/* Row identity: a ctid for a segment's row                                  */
/* ------------------------------------------------------------------------- */

/* A row where it is: its segment, and its ctid there. */
typedef struct RowIdentity
{
	int32		content;
	ItemPointerData tid;
} RowIdentity;

typedef struct RowIdentityEntry
{
	RowIdentity key;			/* zeroed before it is filled: it has padding */
	uint64		id;
} RowIdentityEntry;

/* A statement's rows, by the ctid the plan knows each by. */
typedef struct RowIdentityMap
{
	EState	   *estate;
	HTAB	   *ids;
	RowIdentity *rows;			/* by id */
	uint64		nrows;
	uint64		maxrows;
} RowIdentityMap;

/* In TopMemoryContext; a map is in its statement's memory, and leaves with it. */
static List *identity_maps = NIL;

static void
identity_map_forget(void *arg)
{
	identity_maps = list_delete_ptr(identity_maps, arg);
}

static RowIdentityMap *
identity_map(EState *estate, bool create)
{
	RowIdentityMap *map;
	MemoryContextCallback *cb;
	HASHCTL		ctl;
	MemoryContext oldcxt;
	ListCell   *lc;

	foreach(lc, identity_maps)
	{
		map = (RowIdentityMap *) lfirst(lc);
		if (map->estate == estate)
			return map;
	}
	if (!create)
		return NULL;

	map = MemoryContextAllocZero(estate->es_query_cxt, sizeof(RowIdentityMap));
	map->estate = estate;
	ctl.keysize = sizeof(RowIdentity);
	ctl.entrysize = sizeof(RowIdentityEntry);
	ctl.hcxt = estate->es_query_cxt;
	map->ids = hash_create("gp row identities", 1024, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	map->maxrows = 1024;
	map->rows = MemoryContextAlloc(estate->es_query_cxt,
								   map->maxrows * sizeof(RowIdentity));

	cb = MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback));
	cb->func = identity_map_forget;
	cb->arg = map;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	identity_maps = lappend(identity_maps, map);
	MemoryContextSwitchTo(oldcxt);
	return map;
}

/*
 * The ctid the plan knows a row by: its number in the statement's map, as a
 * block and an offset.  The same row, read again, is the same number.
 */
void
GpRowIdentityMake(EState *estate, int content, ItemPointer tid,
				  ItemPointer result)
{
	RowIdentityMap *map = identity_map(estate, true);
	RowIdentity key;
	RowIdentityEntry *entry;
	bool		found;

	memset(&key, 0, sizeof(key));
	key.content = content;
	ItemPointerCopy(tid, &key.tid);

	entry = (RowIdentityEntry *) hash_search(map->ids, &key, HASH_ENTER, &found);
	if (!found)
	{
		if (map->nrows == map->maxrows)
		{
			map->maxrows *= 2;
			map->rows = repalloc_huge(map->rows,
									  map->maxrows * sizeof(RowIdentity));
		}
		map->rows[map->nrows] = key;
		entry->id = map->nrows++;
	}
	ItemPointerSet(result, (BlockNumber) (entry->id >> 11),
				   (OffsetNumber) ((entry->id & 0x7FF) + 1));
}

/* Where the row the plan knows by this ctid is; false if nowhere. */
static bool
row_identity_find(EState *estate, ItemPointer synthetic, int *content,
				  ItemPointer tid)
{
	RowIdentityMap *map = identity_map(estate, false);
	uint64		id;

	if (map == NULL || ItemPointerGetOffsetNumberNoCheck(synthetic) == 0)
		return false;
	id = ((uint64) ItemPointerGetBlockNumberNoCheck(synthetic) << 11) |
		(ItemPointerGetOffsetNumberNoCheck(synthetic) - 1);
	if (id >= map->nrows)
		return false;
	*content = map->rows[id].content;
	ItemPointerCopy(&map->rows[id].tid, tid);
	return true;
}

/* ------------------------------------------------------------------------- */
/* The node                                                                  */
/* ------------------------------------------------------------------------- */

static Node *explicit_create_state(CustomScan *cscan);
static void explicit_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *explicit_exec(CustomScanState *node);
static void explicit_end(CustomScanState *node);
static void explicit_rescan(CustomScanState *node);
static void explicit_explain(CustomScanState *node, List *ancestors,
							 ExplainState *es);

static const CustomScanMethods explicit_scan_methods = {
	.CustomName = "Explicit Redistribute Motion",
	.CreateCustomScanState = explicit_create_state,
};

static const CustomExecMethods explicit_exec_methods = {
	.CustomName = "Explicit Redistribute Motion",
	.BeginCustomScan = explicit_begin,
	.ExecCustomScan = explicit_exec,
	.EndCustomScan = explicit_end,
	.ReScanCustomScan = explicit_rescan,
	.ExplainCustomScan = explicit_explain,
};

/* custom_private, in order: the ModifyTable's own */
#define EXPLICIT_OPERATION		0	/* CmdType */
#define EXPLICIT_RESULT_RELS	1	/* range table indexes, resultRelations */
#define EXPLICIT_ROOT_REL		2	/* rootRelation: a partitioned root, or 0 */
#define EXPLICIT_UPDATE_COLNOS	3	/* the first result relation's */
#define EXPLICIT_RETURNING		4	/* returningLists */
#define EXPLICIT_CAN_SET_TAG	5
#define EXPLICIT_ON_CONFLICT	6	/* the clause's text, or "" */
#define EXPLICIT_CONFLICT_ACTION 7	/* OnConflictAction */
#define EXPLICIT_CHECKS			8	/* withCheckOptionLists */
#define EXPLICIT_MERGE_ACTIONS	9	/* mergeActionLists */
#define EXPLICIT_MERGE_JOINS	10	/* mergeJoinConditions */

/*
 * MERGE: the statement each kind of action's rows are written by -- an
 * UPDATE's, of its SET columns, a DELETE's, an INSERT's -- and the rows,
 * each segment's.
 */
typedef struct MergeShape
{
	CmdType		cmd;			/* UPDATE, DELETE or INSERT */
	char	   *head;			/* before VALUES */
	char	   *tail;			/* after */
	List	   *casts;			/* each parameter's type */
	List	  **batches;		/* per segment, of const char ** */
	List	   *everywhere;		/* every segment's: a replicated table's */
	int			nvals;			/* an INSERT's values, the root's columns */
	AttrNumber *attnos;
	FmgrInfo   *out;
} MergeShape;

/* MERGE: an action, as a result relation takes it (ExecInitMerge()) */
typedef struct MergeExec
{
	MergeAction *action;
	ExprState  *when;			/* its WHEN condition */
	ProjectionInfo *proj;		/* an UPDATE's or an INSERT's new row */
	MergeShape *shape;			/* the statement its rows are written by */
	bool		moves;			/* an UPDATE of a column of the key: a Split */
	FmgrInfo   *out;			/* an UPDATE's SET values' output functions */
} MergeExec;

typedef struct ExplicitState
{
	CustomScanState css;
	CmdType		operation;
	bool		canSetTag;
	int			nrels;
	Relation   *rels;			/* the result relations */
	Relation	target;			/* what the segments' statements name */
	bool		only;			/* ONLY: it has no partitions of its own here */
	bool		replicated;
	bool		by_content;		/* a replicated table's rows, found by their
								 * text on every segment */
	GpHash	   *hash;			/* an INSERT's routing */
	AttrNumber	ctidcol;		/* the plan's junk ctid, and tableoid */
	AttrNumber	tableoidcol;
	int			nvals;			/* values a row carries: SET's, or INSERT's */
	AttrNumber *valcols;		/* where in the plan's row */
	Oid		   *valtypes;
	FmgrInfo   *valout;
	char	   *sql_head;		/* before VALUES */
	char	   *sql_tail;		/* after */
	OnConflictAction on_conflict;
	List	   *casts;			/* each parameter's type, as VALUES casts it */
	bool		target_opened;	/* the root, opened besides the result relations */

	/* RETURNING, and the check options */
	bool		returning;
	bool		checks;			/* a result relation has check options */
	bool		back;			/* the rows written come back */
	bool		other;			/* each row comes back with its other
								 * version: an UPDATE's old row, an upsert's
								 * existing one */
	ResultRelInfo *rris;		/* the result relations', with their checks */
	ResultRelInfo *rootrri;		/* the root's, where there is one */
	ProjectionInfo **projs;
	TupleTableSlot **relslots;
	TupleTableSlot **orelslots; /* the other version, as each has it */
	TupleConversionMap **maps;
	TupleDesc	retdesc;
	TupleTableSlot *retslot;
	TupleTableSlot *rootslot;
	TupleTableSlot *otherslot;	/* the other version, as the root has it */
	TupleTableSlot *outerslot;
	MinimalTuple *saved;		/* the plan's rows, by number */
	uint64		nsaved;
	uint64		maxsaved;
	Tuplestorestate *returned;

	int			nsegs;
	int			numsegments;	/* the target's: every segment, or a partial
								 * table's first so many */
	List	  **batches;		/* per segment, of const char ** */
	List	   *everywhere;		/* a replicated table's INSERTed rows */
	MemoryContext rowcxt;
	bool		done;

	/*
	 * Each target row is sent once, so that a statement's count is exact;
	 * with the global deadlock detector on, rows the segments' statements
	 * came short of are asked about (explicit_recheck()).
	 */
	bool		recheck;
	HTAB	   *sent;			/* the rows sent, by the plan's ctid */
	int			mfrom;			/* a replicated MERGE target's rows' segment */

	/*
	 * A Split: an UPDATE of the distribution key.  Each row is deleted where
	 * it is, returning it, and its new version -- the old one with the SET
	 * columns' new values -- inserted where it hashes.
	 */
	bool		split;
	bool		split_calls;	/* moved by the segments' split functions */
	AttrNumber *setattnos;		/* each SET column, in the target */
	char	   *delete_head;
	char	   *delete_tail;
	char	   *insert_head;
	char	   *insert_tail;
	List	   *insert_casts;
	int			ninsert;
	AttrNumber *insattnos;		/* the columns an INSERT gives values */
	FmgrInfo   *insout;
	TupleDesc	olddesc;		/* a deleted row: gp_n, its table, its columns */
	TupleDesc	newdesc;		/* an inserted one: its table, its columns */

	/* MERGE (explicit_begin_merge) */
	bool		merge;
	AttrNumber	targetcol;		/* the target's row, junk the plan carries */
	List	  **mactions;		/* per result relation and match kind:
								 * MergeExec */
	ExprState **mjoins;			/* per result relation: the join condition */
	TupleTableSlot **moldslots;	/* per result relation: the target's row */
	TupleTableSlot **mnewslots;	/* and an UPDATE's new one */
	TupleConversionMap **mmaps;	/* the root's row to the relation's */
	TupleTableSlot *mrootslot;	/* the target's row, as the root has it */
	TupleTableSlot *minsslot;	/* an INSERT's new row, as the root has it */
	List	   *mshapes;		/* MergeShape, in the order they are sent */
	MergeShape *mdelete;
	MergeShape *minsert;
	HTAB	   *mtouched;		/* rows an action changed, by the plan's ctid */
	HeapTuple  *mnew;			/* a Split's new rows, by the plan's row */
	TupleConversionMap **mtoroot;	/* the relation's row to the root's */
	FmgrInfo	mtextout;		/* a replicated table's row, as its text */
} ExplicitState;

/*
 * What a statement's shortfall is asked about on a segment
 * (explicit_recheck()): the table; what a row another transaction deleted
 * meanwhile does -- 'p' it is passed over, 's' it fails a Split, 'm' a
 * MERGE that may insert instead; and, for a replicated table's rows, which
 * are sent by their text, the segment they were read on and each one's
 * ctid there.
 */
typedef struct ExplicitRecheck
{
	Relation	target;
	char		deleted;
	int			content;		/* the rows' segment, or -1: the one sent to */
	const char **ctids;			/* in the rows' order, or NULL: params[0] */
} ExplicitRecheck;

/* A row a write changes: its table, and the ctid the plan knows it by. */
typedef struct RowTouched
{
	Oid			relid;
	ItemPointerData tid;		/* zeroed before it is filled: it has padding */
} RowTouched;

/* Does an expression read RETURNING's old or new explicitly? */
static bool
returning_qualified_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) &&
		((Var *) node)->varreturningtype != VAR_RETURNING_DEFAULT)
		return true;
	if (IsA(node, ReturningExpr))
		return true;
	return expression_tree_walker(node, returning_qualified_walker, context);
}

/* Does the relation have an enabled UPDATE trigger, of a row or a statement? */
static bool
has_update_triggers(Oid relid)
{
	Relation	rel = table_open(relid, NoLock);
	TriggerDesc *td = rel->trigdesc;
	bool		found = false;

	for (int i = 0; td != NULL && i < td->numtriggers && !found; i++)
		found = td->triggers[i].tgenabled != TRIGGER_DISABLED &&
			TRIGGER_FOR_UPDATE(td->triggers[i].tgtype);
	table_close(rel, NoLock);
	return found;
}

static bool
has_statement_triggers(Relation rel, CmdType operation, List *merge_actions)
{
	TriggerDesc *td = rel->trigdesc;

	if (td == NULL)
		return false;
	switch (operation)
	{
		case CMD_MERGE:
			/* a MERGE fires those of each kind of action it has */
			foreach_node(MergeAction, action, merge_actions)
				if (action->commandType != CMD_NOTHING &&
					has_statement_triggers(rel, action->commandType, NIL))
					return true;
			return false;
		case CMD_INSERT:
			return td->trig_insert_before_statement ||
				td->trig_insert_after_statement;
		case CMD_UPDATE:
			return td->trig_update_before_statement ||
				td->trig_update_after_statement;
		case CMD_DELETE:
			return td->trig_delete_before_statement ||
				td->trig_delete_after_statement;
		default:
			return false;
	}
}

/*
 * Why this ModifyTable, of a distributed table, cannot be written this way;
 * NULL if it can.
 */
const char *
GpExplicitCannot(PlannedStmt *stmt, ModifyTable *mt, const char *on_conflict)
{
	ListCell   *lc;
	Index		first = linitial_int(mt->resultRelations);
	GpPolicy   *policy;

	if (mt->operation == CMD_MERGE && mt->returningLists != NIL)
		return "Its RETURNING would say which action wrote each row, which only a MERGE's own node can.";
	if (mt->onConflictAction != ONCONFLICT_NONE && on_conflict == NULL)
		return "ON CONFLICT into a distributed table is written from the statement's own text, which was not printed for this one.";

	foreach(lc, mt->resultRelations)
	{
		RangeTblEntry *rte = rt_fetch(lfirst_int(lc), stmt->rtable);
		Relation	rel;
		bool		triggers;

		policy = GpScanDistributedPolicy(rte->relid);
		if (policy == NULL)
			return psprintf("Of the tables it writes, \"%s\" has its rows on the coordinator and others on the segments.",
							get_rel_name(rte->relid));
		rel = table_open(rte->relid, NoLock);
		triggers = has_statement_triggers(rel, mt->operation,
										  mt->mergeActionLists != NIL
										  ? linitial(mt->mergeActionLists) : NIL);
		table_close(rel, NoLock);
		if (triggers)
			return psprintf("\"%s\" has statement-level triggers, which would fire on every segment.",
							get_rel_name(rte->relid));
	}

	if (mt->rootRelation != 0)
	{
		Oid			rootid = rt_fetch(mt->rootRelation, stmt->rtable)->relid;
		Relation	rel = table_open(rootid, NoLock);
		bool		triggers = has_statement_triggers(rel, mt->operation,
													  mt->mergeActionLists != NIL
													  ? linitial(mt->mergeActionLists) : NIL);

		table_close(rel, NoLock);
		if (triggers)
			return psprintf("\"%s\" has statement-level triggers, which would fire on every segment.",
							get_rel_name(rootid));
	}

	/*
	 * A row whose key changes belongs on another segment, and is moved there:
	 * deleted where it is and inserted where it hashes, as Cloudberry's Split
	 * Update moves it.  Its UPDATE triggers would not fire, and Cloudberry
	 * refuses an UPDATE of the key of a table that has any, in its words
	 * (make_splitupdate_path, cdbpath.c) -- asking the plan's first result
	 * relation, which for a partitioned table is its first partition, as
	 * Cloudberry's create_modifytable_path() asks it.  Its INSERT and DELETE
	 * triggers do not fire either, as Cloudberry's Split fires none -- but
	 * on a cluster without the secret, whose segments cannot tell the
	 * coordinator's split functions from anyone's call, and move the row by
	 * a DELETE and an INSERT, which fire them (explicit_send_split()).
	 */
	policy = GpScanDistributedPolicy(rt_fetch(first, stmt->rtable)->relid);
	if ((mt->operation == CMD_UPDATE || mt->operation == CMD_MERGE) &&
		GpPolicyIsHashPartitioned(policy))
	{
		Oid			firstid = rt_fetch(first, stmt->rtable)->relid;
		List	   *setcols = NIL;

		/* an UPDATE's SET columns, or a MERGE's UPDATE actions' */
		if (mt->operation == CMD_UPDATE)
			setcols = (List *) linitial(mt->updateColnosLists);
		else
			foreach_node(MergeAction, action, (List *) linitial(mt->mergeActionLists))
				if (action->commandType == CMD_UPDATE)
					setcols = list_concat(setcols, action->updateColnos);

		foreach(lc, setcols)
		{
			for (int k = 0; k < policy->nattrs; k++)
			{
				if (policy->attrs[k] != lfirst_int(lc))
					continue;
				if (has_update_triggers(firstid))
					ereport(ERROR,
							(errcode(MAKE_SQLSTATE('0', 'A', 'M', '0', '1')),
							 errmsg("UPDATE on distributed key column not allowed on relation with update triggers")));
			}
		}
	}

	return NULL;
}

/* Does this expression hold a subquery, which a segment would answer alone? */
static bool
has_sublink(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
		return true;
	return expression_tree_walker(node, has_sublink, context);
}

/*
 * GpExplicitOnConflict
 *		The ON CONFLICT clause of an INSERT into a distributed table, as text a
 *		segment runs after the VALUES of its rows: PostgreSQL's own ruleutils
 *		prints the statement, before the planner changes it, with its target
 *		called gp_t, as the rows' statements call it, and its source reduced
 *		to NULLs, so that the first " ON CONFLICT" of the text is where the
 *		clause begins.  Refuses what Cloudberry's analyze.c refuses of a DO
 *		UPDATE, in its words, and a subquery in its SET or WHERE.
 */
char *
GpExplicitOnConflict(Query *parse, GpPolicy *policy)
{
	Query	   *q;
	RangeTblEntry *target;
	OnConflictExpr *oc = parse->onConflict;
	ListCell   *lc;
	char	   *sql;
	char	   *clause;

	if (oc->action == ONCONFLICT_UPDATE)
	{
		if (GpPolicyIsHashPartitioned(policy))
			foreach(lc, oc->onConflictSet)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);

				for (int k = 0; k < policy->nattrs; k++)
					if (policy->attrs[k] == tle->resno)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("modification of distribution columns in OnConflictUpdate is not supported")));
			}
		if (GpPolicyIsReplicated(policy) &&
			(contain_volatile_functions((Node *) oc->onConflictSet) ||
			 contain_volatile_functions(oc->onConflictWhere)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("modification of replicated tables containing volatile functions in OnConflictUpdate is not supported")));
		if (has_sublink((Node *) oc->onConflictSet, NULL) ||
			has_sublink(oc->onConflictWhere, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a subquery in ON CONFLICT DO UPDATE of a distributed table is not supported yet"),
					 errdetail("Each segment would answer it from its own rows alone.")));
	}

	q = copyObject(parse);
	target = rt_fetch(q->resultRelation, q->rtable);
	target->alias = makeAlias("gp_t", NIL);
	q->returningList = NIL;
	foreach(lc, q->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		int			rti = foreach_current_index(lc) + 1;

		if (rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_VALUES)
		{
			rte->rtekind = RTE_RESULT;
			rte->subquery = NULL;
			rte->values_lists = NIL;
		}

		/*
		 * Every other entry a name of its own, so that EXCLUDED is printed
		 * as it is written: an INSERT through a view has the view's too,
		 * which ruleutils would otherwise leave "excluded" and call the
		 * table's "excluded_1".
		 */
		if (rti != q->resultRelation && rti != oc->exclRelIndex)
			rte->alias = makeAlias(psprintf("gp_r%d", rti), NIL);
	}
	foreach(lc, q->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (!tle->resjunk)
			tle->expr = (Expr *) makeNullConst(exprType((Node *) tle->expr),
											   exprTypmod((Node *) tle->expr),
											   exprCollation((Node *) tle->expr));
	}

	sql = pg_get_querydef(q, false);
	clause = strstr(sql, " ON CONFLICT");
	if (clause == NULL)
		elog(ERROR, "could not print the ON CONFLICT clause of an INSERT");
	return pstrdup(clause);
}

/*
 * The node that writes in "mt"'s place: its plan below, its RETURNING as the
 * node's output.
 */
Plan *
GpExplicitMake(ModifyTable *mt, const char *on_conflict)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *tlist = NIL;
	ListCell   *lc;

	cscan->scan.plan.startup_cost = mt->plan.startup_cost;
	cscan->scan.plan.total_cost = mt->plan.total_cost;
	cscan->scan.plan.plan_rows = mt->plan.plan_rows;
	cscan->scan.plan.plan_width = mt->plan.plan_width;
	cscan->scan.plan.plan_node_id = mt->plan.plan_node_id;
	cscan->scan.plan.initPlan = mt->plan.initPlan;
	cscan->scan.plan.extParam = mt->plan.extParam;
	cscan->scan.plan.allParam = mt->plan.allParam;
	cscan->scan.plan.lefttree = outerPlan(mt);
	cscan->scan.scanrelid = 0;

	/*
	 * What ModifyTable returns is its first result relation's RETURNING, as
	 * set_plan_references() left it: the relation's own columns, and the
	 * plan's as OUTER_VAR.  The node returns the same, read through its
	 * scan target list.
	 */
	foreach(lc, mt->plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(INDEX_VAR, tle->resno,
														 exprType((Node *) tle->expr),
														 exprTypmod((Node *) tle->expr),
														 exprCollation((Node *) tle->expr),
														 0),
										tle->resno, tle->resname, false));
	}
	cscan->custom_scan_tlist = copyObject(mt->plan.targetlist);
	cscan->scan.plan.targetlist = tlist;

	cscan->custom_private =
		list_make5(makeInteger(mt->operation),
				   list_copy(mt->resultRelations),
				   makeInteger(mt->rootRelation),
				   mt->updateColnosLists != NIL
				   ? list_copy(linitial(mt->updateColnosLists)) : NIL,
				   copyObject(mt->returningLists));
	cscan->custom_private = lappend(cscan->custom_private,
									makeBoolean(mt->canSetTag));
	cscan->custom_private = lappend(cscan->custom_private,
									makeString(pstrdup(on_conflict ? on_conflict : "")));
	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(mt->onConflictAction));
	cscan->custom_private = lappend(cscan->custom_private,
									copyObject(mt->withCheckOptionLists));
	cscan->custom_private = lappend(cscan->custom_private,
									copyObject(mt->mergeActionLists));
	cscan->custom_private = lappend(cscan->custom_private,
									copyObject(mt->mergeJoinConditions));
	cscan->methods = &explicit_scan_methods;
	return &cscan->scan.plan;
}

static Node *
explicit_create_state(CustomScan *cscan)
{
	ExplicitState *state = (ExplicitState *) newNode(sizeof(ExplicitState),
													 T_CustomScanState);

	state->css.methods = &explicit_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

/*
 * What a segment's RETURNING gives back: the number of the plan's row that
 * asked (with_n), the table the row is in, its ctid there, and the target's
 * columns but the dropped ones -- and, with_other, the table, the ctid and
 * the columns of the row's other version, all null where it has none.
 */
static TupleDesc
returned_desc(TupleDesc targetdesc, bool with_n, bool with_other)
{
	int			ncols = 0;
	int			col = 1;
	TupleDesc	desc;

	for (int i = 0; i < targetdesc->natts; i++)
		if (!TupleDescAttr(targetdesc, i)->attisdropped)
			ncols++;
	desc = CreateTemplateTupleDesc((ncols + 2) * (with_other ? 2 : 1) +
								   (with_n ? 1 : 0));
	if (with_n)
		TupleDescInitEntry(desc, col++, "gp_n", INT8OID, -1, 0);
	for (int image = 0; image < (with_other ? 2 : 1); image++)
	{
		TupleDescInitEntry(desc, col++, "gp_toid", OIDOID, -1, 0);
		TupleDescInitEntry(desc, col++, "gp_ctid", TIDOID, -1, 0);
		for (int i = 0; i < targetdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(targetdesc, i);

			if (att->attisdropped)
				continue;
			TupleDescInitEntry(desc, col++, NameStr(att->attname), att->atttypid,
							   att->atttypmod, 0);
		}
	}
	TupleDescFinalize(desc);
	return desc;
}

/* The type a value is cast to in VALUES, as the segment reads it. */
static char *
cast_to(Oid type, int32 typmod)
{
	return format_type_with_typemod(type, typmod);
}


/* ------------------------------------------------------------------------- */
/* MERGE                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The statement one kind of MERGE action's rows are written by: an UPDATE of
 * its SET columns by each row's place, as "desc" numbers them, a DELETE by
 * its place, or an INSERT of every column but the dropped and generated
 * ones -- the same as an UPDATE's, a DELETE's and an INSERT's (explicit_begin).
 */
static MergeShape *
merge_shape(ExplicitState *state, CmdType cmd, List *setcols, TupleDesc desc)
{
	MergeShape *shape = palloc0_object(MergeShape);
	const char *name = GpDispatchRelationName(RelationGetRelid(state->target));
	const char *only = state->only ? "ONLY " : "";
	StringInfoData head;
	StringInfoData tail;
	int			i = 0;

	shape->cmd = cmd;
	initStringInfo(&head);
	initStringInfo(&tail);
	if (cmd != CMD_INSERT)
		shape->casts = list_make3(state->by_content ? "pg_catalog.text" : "pg_catalog.tid",
								  "pg_catalog.oid", "pg_catalog.int8");
	if (cmd == CMD_UPDATE)
	{
		appendStringInfo(&head, "UPDATE %s%s AS gp_t SET ", only, name);
		foreach_int(attno, setcols)
		{
			Form_pg_attribute att = TupleDescAttr(desc, attno - 1);

			appendStringInfo(&head, "%s%s = gp_s.gp_c%d", i > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)), i + 1);
			shape->casts = lappend(shape->casts,
								   cast_to(att->atttypid, att->atttypmod));
			i++;
		}
		appendStringInfoString(&head, " FROM (VALUES ");
		appendStringInfo(&tail, ") AS gp_s (%s, gp_toid, gp_n",
						 state->by_content ? "gp_old" : "gp_ctid");
		for (int k = 0; k < i; k++)
			appendStringInfo(&tail, ", gp_c%d", k + 1);
		appendStringInfo(&tail, ") WHERE %s AND gp_t.tableoid = gp_s.gp_toid",
						 state->by_content ? "gp_t::pg_catalog.text = gp_s.gp_old"
						 : "gp_t.ctid = gp_s.gp_ctid");
	}
	else if (cmd == CMD_DELETE)
	{
		appendStringInfo(&head, "DELETE FROM %s%s AS gp_t USING (VALUES ", only, name);
		appendStringInfoString(&tail, state->by_content
							   ? ") AS gp_s (gp_old, gp_toid, gp_n) WHERE gp_t::pg_catalog.text = gp_s.gp_old AND gp_t.tableoid = gp_s.gp_toid"
							   : ") AS gp_s (gp_ctid, gp_toid, gp_n) WHERE gp_t.ctid = gp_s.gp_ctid AND gp_t.tableoid = gp_s.gp_toid");
	}
	else
	{
		bool		identity = false;

		shape->attnos = palloc_array(AttrNumber, desc->natts);
		shape->out = palloc_array(FmgrInfo, desc->natts);
		appendStringInfo(&head, "INSERT INTO %s AS gp_t (", name);
		for (int k = 0; k < desc->natts; k++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, k);
			Oid			func;
			bool		isvarlena;

			if (att->attisdropped || att->attgenerated != '\0')
				continue;
			if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
				identity = true;
			appendStringInfo(&head, "%s%s", shape->nvals > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
			shape->casts = lappend(shape->casts,
								   cast_to(att->atttypid, att->atttypmod));
			getTypeOutputInfo(att->atttypid, &func, &isvarlena);
			fmgr_info(func, &shape->out[shape->nvals]);
			shape->attnos[shape->nvals++] = att->attnum;
		}
		appendStringInfo(&head, ")%s VALUES ",
						 identity ? " OVERRIDING SYSTEM VALUE" : "");
	}
	shape->head = head.data;
	shape->tail = tail.data;
	return shape;
}

/*
 * MERGE: each result relation's actions, as ExecInitMerge() makes them --
 * their WHEN conditions, and their new rows' projections over the target's
 * row as the scan tuple and the plan's as the inner one -- and the
 * statements their rows are written by.  An UPDATE that sets a column of
 * the key moves its rows, a Split, as Cloudberry's SplitMerge does.
 */
static void
explicit_begin_merge(ExplicitState *state, GpPolicy *policy)
{
	CustomScanState *node = &state->css;
	List	   *priv = ((CustomScan *) node->ss.ps.plan)->custom_private;
	List	   *actlists = (List *) list_nth(priv, EXPLICIT_MERGE_ACTIONS);
	List	   *joins = (List *) list_nth(priv, EXPLICIT_MERGE_JOINS);
	List	   *first = (List *) linitial(actlists);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleDesc	rootdesc = RelationGetDescr(state->target);
	TupleDesc	desc0 = RelationGetDescr(state->rels[0]);
	int			nactions = list_length(first);
	MergeShape **shapes = palloc0_array(MergeShape *, Max(nactions, 1));
	bool	   *moves = palloc0_array(bool, Max(nactions, 1));
	Oid			func;
	bool		isvarlena;

	state->merge = true;
	state->targetcol = ExecFindJunkAttributeInTlist(outerPlan(node->ss.ps.plan)->targetlist,
													GP_MERGE_TARGET_JUNK);
	getTypeOutputInfo(rootdesc->tdtypeid, &func, &isvarlena);
	fmgr_info(func, &state->mtextout);
	if (policy != NULL)
		state->hash = GpHashMake(policy, rootdesc);

	/* one statement for each UPDATE action, one for DELETEs, one for INSERTs */
	foreach_node(MergeAction, action, first)
	{
		int			k = foreach_current_index(action);

		switch (action->commandType)
		{
			case CMD_UPDATE:
				shapes[k] = merge_shape(state, CMD_UPDATE, action->updateColnos, desc0);
				if (policy != NULL && GpPolicyIsHashPartitioned(policy))
					foreach_int(attno, action->updateColnos)
					{
						AttrNumber	t = attnameAttNum(state->target,
													  NameStr(TupleDescAttr(desc0, attno - 1)->attname),
													  false);

						for (int j = 0; j < policy->nattrs; j++)
							if (policy->attrs[j] == t)
								moves[k] = true;
					}
				if (moves[k])
					state->split = true;
				else
					state->mshapes = lappend(state->mshapes, shapes[k]);
				break;
			case CMD_DELETE:
				if (state->mdelete == NULL)
					state->mdelete = merge_shape(state, CMD_DELETE, NIL, desc0);
				shapes[k] = state->mdelete;
				break;
			case CMD_INSERT:
				if (state->minsert == NULL)
					state->minsert = merge_shape(state, CMD_INSERT, NIL, rootdesc);
				shapes[k] = state->minsert;
				break;
			default:
				break;
		}
	}

	/* each result relation's actions, by what the row's match allows */
	state->mactions = palloc0_array(List *, state->nrels * NUM_MERGE_MATCH_KINDS);
	state->mjoins = palloc0_array(ExprState *, state->nrels);
	state->moldslots = palloc0_array(TupleTableSlot *, state->nrels);
	state->mnewslots = palloc0_array(TupleTableSlot *, state->nrels);
	state->mmaps = palloc0_array(TupleConversionMap *, state->nrels);
	state->mtoroot = palloc0_array(TupleConversionMap *, state->nrels);
	state->mrootslot = MakeSingleTupleTableSlot(rootdesc, &TTSOpsVirtual);
	state->minsslot = MakeSingleTupleTableSlot(rootdesc, &TTSOpsVirtual);
	for (int i = 0; i < state->nrels; i++)
	{
		TupleDesc	reldesc = RelationGetDescr(state->rels[i]);

		state->moldslots[i] = MakeSingleTupleTableSlot(reldesc, &TTSOpsVirtual);
		state->mnewslots[i] = MakeSingleTupleTableSlot(reldesc, &TTSOpsVirtual);
		if (state->rels[i] != state->target)
		{
			state->mmaps[i] = convert_tuples_by_name(rootdesc, reldesc);
			state->mtoroot[i] = convert_tuples_by_name(reldesc, rootdesc);
		}
		state->mjoins[i] = ExecInitQual((List *) (joins != NIL ? list_nth(joins, i) : NULL),
										&node->ss.ps);
		foreach_node(MergeAction, action, (List *) list_nth(actlists, i))
		{
			int			k = foreach_current_index(action);
			MergeExec  *e = palloc0_object(MergeExec);

			e->action = action;
			e->when = ExecInitQual((List *) action->qual, &node->ss.ps);
			e->shape = shapes[k];
			e->moves = moves[k];
			if (action->commandType == CMD_UPDATE)
			{
				int			j = 0;

				e->proj = ExecBuildUpdateProjection(action->targetList, true,
													action->updateColnos,
													reldesc, econtext,
													state->mnewslots[i],
													&node->ss.ps);
				e->out = palloc_array(FmgrInfo, Max(list_length(action->updateColnos), 1));
				foreach_int(attno, action->updateColnos)
				{
					getTypeOutputInfo(TupleDescAttr(reldesc, attno - 1)->atttypid,
									  &func, &isvarlena);
					fmgr_info(func, &e->out[j++]);
				}
			}
			else if (action->commandType == CMD_INSERT)
				e->proj = ExecBuildProjectionInfo(action->targetList, econtext,
												  state->minsslot, &node->ss.ps,
												  rootdesc);
			state->mactions[i * NUM_MERGE_MATCH_KINDS + action->matchKind] =
				lappend(state->mactions[i * NUM_MERGE_MATCH_KINDS + action->matchKind], e);
		}
	}
}

static void
explicit_begin(CustomScanState *node, EState *estate, int eflags)
{
	ExplicitState *state = (ExplicitState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	List	   *resultrels = (List *) list_nth(priv, EXPLICIT_RESULT_RELS);
	Index		rootrti = intVal(list_nth(priv, EXPLICIT_ROOT_REL));
	List	   *setcols = (List *) list_nth(priv, EXPLICIT_UPDATE_COLNOS);
	List	   *returning = (List *) list_nth(priv, EXPLICIT_RETURNING);
	List	   *checks = (List *) list_nth(priv, EXPLICIT_CHECKS);
	Plan	   *subplan = outerPlan(cscan);
	TupleDesc	targetdesc;
	GpPolicy   *policy;
	StringInfoData head;
	StringInfoData tail;
	bool		view_checks = false;
	int			i;

	outerPlanState(node) = ExecInitNode(subplan, estate, eflags);

	state->operation = (CmdType) intVal(list_nth(priv, EXPLICIT_OPERATION));
	state->canSetTag = boolVal(list_nth(priv, EXPLICIT_CAN_SET_TAG));
	state->on_conflict = (OnConflictAction) intVal(list_nth(priv, EXPLICIT_CONFLICT_ACTION));
	state->nrels = list_length(resultrels);
	state->rels = palloc_array(Relation, state->nrels);
	i = 0;
	foreach_int(rti, resultrels)
		state->rels[i++] = table_open(exec_rt_fetch(rti, estate)->relid, NoLock);
	state->target_opened = rootrti != 0;
	state->target = rootrti != 0
		? table_open(exec_rt_fetch(rootrti, estate)->relid, NoLock)
		: state->rels[0];
	state->only = rootrti == 0 && state->nrels == 1;
	targetdesc = RelationGetDescr(state->target);
	policy = GpScanDistributedPolicy(RelationGetRelid(state->target));
	state->replicated = policy != NULL && GpPolicyIsReplicated(policy);
	state->by_content = state->replicated && state->operation != CMD_INSERT;
	state->numsegments = policy != NULL ? policy->numsegments : 0;

	/* The plan's row: its values first, in order, then its junk. */
	state->ctidcol = ExecFindJunkAttributeInTlist(subplan->targetlist, "ctid");
	state->tableoidcol = ExecFindJunkAttributeInTlist(subplan->targetlist,
													  "tableoid");
	if (state->operation != CMD_INSERT && !AttributeNumberIsValid(state->ctidcol))
		elog(ERROR, "the plan of a write of a distributed table has no ctid");

	initStringInfo(&head);
	initStringInfo(&tail);

	/*
	 * a row's place -- or a replicated table's row's text -- its table and
	 * the number of the plan's row
	 */
	if (state->operation != CMD_INSERT)
		state->casts = list_make3(state->by_content ? "pg_catalog.text" : "pg_catalog.tid",
								  "pg_catalog.oid", "pg_catalog.int8");

	if (state->operation == CMD_UPDATE)
	{
		TupleDesc	desc = RelationGetDescr(state->rels[0]);

		state->nvals = list_length(setcols);
		state->valcols = palloc_array(AttrNumber, state->nvals);
		state->valtypes = palloc_array(Oid, state->nvals);
		appendStringInfo(&head, "UPDATE %s%s AS gp_t SET ",
						 state->only ? "ONLY " : "",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		i = 0;
		foreach_int(attno, setcols)
		{
			Form_pg_attribute att = TupleDescAttr(desc, attno - 1);

			state->valcols[i] = i + 1;
			state->valtypes[i] = att->atttypid;
			appendStringInfo(&head, "%s%s = gp_s.gp_c%d", i > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)), i + 1);
			state->casts = lappend(state->casts,
								   cast_to(att->atttypid, att->atttypmod));
			i++;
		}
		appendStringInfoString(&head, " FROM (VALUES ");
		appendStringInfo(&tail, ") AS gp_s (%s, gp_toid, gp_n",
						 state->by_content ? "gp_old" : "gp_ctid");
		for (i = 0; i < state->nvals; i++)
			appendStringInfo(&tail, ", gp_c%d", i + 1);
		appendStringInfo(&tail, ") WHERE %s AND gp_t.tableoid = gp_s.gp_toid",
						 state->by_content ? "gp_t::pg_catalog.text = gp_s.gp_old"
						 : "gp_t.ctid = gp_s.gp_ctid");
	}
	else if (state->operation == CMD_DELETE)
	{
		state->nvals = 0;
		appendStringInfo(&head, "DELETE FROM %s%s AS gp_t USING (VALUES ",
						 state->only ? "ONLY " : "",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		appendStringInfoString(&tail, state->by_content
							   ? ") AS gp_s (gp_old, gp_toid, gp_n) WHERE gp_t::pg_catalog.text = gp_s.gp_old AND gp_t.tableoid = gp_s.gp_toid"
							   : ") AS gp_s (gp_ctid, gp_toid, gp_n) WHERE gp_t.ctid = gp_s.gp_ctid AND gp_t.tableoid = gp_s.gp_toid");
	}
	else if (state->operation == CMD_MERGE)
		explicit_begin_merge(state, policy);
	else
	{
		bool		identity = false;

		/*
		 * Every column but the dropped and generated ones, which the segment
		 * computes; a value for an identity column is the coordinator's,
		 * as the routed INSERT's are.
		 */
		state->valcols = palloc_array(AttrNumber, targetdesc->natts);
		state->valtypes = palloc_array(Oid, targetdesc->natts);
		appendStringInfo(&head, "INSERT INTO %s AS gp_t (",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		state->nvals = 0;
		for (i = 0; i < targetdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(targetdesc, i);

			if (att->attisdropped || att->attgenerated != '\0')
				continue;
			if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
				identity = true;
			appendStringInfo(&head, "%s%s", state->nvals > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
			state->casts = lappend(state->casts,
								   cast_to(att->atttypid, att->atttypmod));
			state->valcols[state->nvals] = att->attnum;
			state->valtypes[state->nvals] = att->atttypid;
			state->nvals++;
		}
		appendStringInfo(&head, ")%s VALUES ",
						 identity ? " OVERRIDING SYSTEM VALUE" : "");
		if (policy != NULL)
			state->hash = GpHashMake(policy, targetdesc);

		/* ON CONFLICT, after the rows, as its text was printed */
		appendStringInfoString(&tail, strVal(list_nth(priv, EXPLICIT_ON_CONFLICT)));
	}

	state->valout = palloc_array(FmgrInfo, Max(state->nvals, 1));
	for (i = 0; i < state->nvals; i++)
	{
		Oid			func;
		bool		isvarlena;

		getTypeOutputInfo(state->valtypes[i], &func, &isvarlena);
		fmgr_info(func, &state->valout[i]);
	}

	/*
	 * A Split, where the UPDATE sets a column of the key: the plan's rows are
	 * kept, whose SET values the new versions take.
	 */
	if (state->operation == CMD_UPDATE && policy != NULL &&
		GpPolicyIsHashPartitioned(policy))
	{
		TupleDesc	desc = RelationGetDescr(state->rels[0]);

		state->setattnos = palloc_array(AttrNumber, Max(state->nvals, 1));
		i = 0;
		foreach_int(attno, setcols)
		{
			AttrNumber	t = attnameAttNum(state->target,
										  NameStr(TupleDescAttr(desc, attno - 1)->attname),
										  false);

			state->setattnos[i++] = t;
			for (int k = 0; k < policy->nattrs; k++)
				if (policy->attrs[k] == t)
					state->split = true;
		}
	}

	/*
	 * The result relations, as ModifyTable has them -- each a partition's
	 * or a child's of the root, where there is one -- and their check
	 * options: a view's WITH CHECK OPTION, which the segments' statements,
	 * naming the table, do not see, and row-level security's, which they
	 * apply as their own, running as the session's role.  Both are checked
	 * here over the rows the segments wrote, which come back for it.
	 */
	if (rootrti != 0)
	{
		state->rootrri = makeNode(ResultRelInfo);
		InitResultRelInfo(state->rootrri, state->target, rootrti, NULL,
						  estate->es_instrument);
	}
	state->rris = palloc0_array(ResultRelInfo, state->nrels);
	i = 0;
	foreach_int(rti, resultrels)
	{
		ResultRelInfo *rri = &state->rris[i];
		List	   *wcos = checks != NIL ? (List *) list_nth(checks, i) : NIL;

		InitResultRelInfo(rri, state->rels[i], rti, state->rootrri,
						  estate->es_instrument);
		rri->ri_WithCheckOptions = wcos;
		foreach_node(WithCheckOption, wco, wcos)
		{
			rri->ri_WithCheckOptionExprs =
				lappend(rri->ri_WithCheckOptionExprs,
						ExecInitQual((List *) wco->qual, &node->ss.ps));
			if (wco->kind == WCO_VIEW_CHECK)
				view_checks = true;
			state->checks = true;
		}
		i++;
	}

	/*
	 * What the segments wrote comes back, for RETURNING and for a view's
	 * check options; and a moved row's new version for its policies, which
	 * the Split's DELETE and INSERT would not check as an UPDATE's.  Not
	 * for a policy alone: a segment's statement checks it, and one that
	 * returned rows would check the table's SELECT policies too, which
	 * the statement written may not have asked for.
	 *
	 * Each segment returns what it wrote as the root's row, with the table
	 * it is in and, but for an INSERT, the number of the plan's row that
	 * asked; a partition's own list is evaluated over the row as that
	 * partition has it.
	 */
	state->returning = returning != NIL;
	state->back = state->operation != CMD_MERGE &&
		(state->returning || view_checks || (state->split && state->checks));
	if (state->back)
	{
		ExprContext *econtext = node->ss.ps.ps_ExprContext;

		/*
		 * old and new by name: an UPDATE's old row and an upsert's existing
		 * one come back too -- and the existing one where an upsert is
		 * checked, whose policies check it, and tell a row it updated from
		 * one it inserted.  A DELETE has no new row, and an INSERT that
		 * updates nothing no old one.
		 */
		state->other = (returning_qualified_walker((Node *) returning, NULL) &&
						(state->operation == CMD_UPDATE ||
						 state->on_conflict == ONCONFLICT_UPDATE)) ||
			(state->checks && state->on_conflict == ONCONFLICT_UPDATE);

		appendStringInfo(&tail, " RETURNING %sgp_t.tableoid, gp_t.ctid, gp_t.*%s",
						 state->operation != CMD_INSERT ? "gp_s.gp_n, " : "",
						 state->other ? ", old.tableoid, old.ctid, old.*" : "");
		state->retdesc = returned_desc(targetdesc,
									   state->operation != CMD_INSERT,
									   state->other);
		state->retslot = MakeSingleTupleTableSlot(state->retdesc,
												  &TTSOpsMinimalTuple);
		state->rootslot = MakeSingleTupleTableSlot(targetdesc, &TTSOpsVirtual);
		if (state->other)
			state->otherslot = MakeSingleTupleTableSlot(targetdesc,
														&TTSOpsVirtual);
		state->outerslot = MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(node)),
													&TTSOpsMinimalTuple);

		state->projs = palloc0_array(ProjectionInfo *, state->nrels);
		state->relslots = palloc0_array(TupleTableSlot *, state->nrels);
		state->orelslots = palloc0_array(TupleTableSlot *, state->nrels);
		state->maps = palloc0_array(TupleConversionMap *, state->nrels);
		for (i = 0; i < state->nrels; i++)
		{
			TupleDesc	reldesc = RelationGetDescr(state->rels[i]);

			if (state->returning)
				state->projs[i] = ExecBuildProjectionInfo((List *) list_nth(returning, i),
														  econtext,
														  node->ss.ps.ps_ResultTupleSlot,
														  &node->ss.ps, reldesc);
			if (state->rels[i] != state->target)
			{
				state->maps[i] = convert_tuples_by_name(targetdesc, reldesc);
				state->relslots[i] = MakeSingleTupleTableSlot(reldesc,
															  &TTSOpsVirtual);
				if (state->other)
					state->orelslots[i] = MakeSingleTupleTableSlot(reldesc,
																   &TTSOpsVirtual);
			}
		}
		state->maxsaved = 64;
		state->saved = palloc_array(MinimalTuple, state->maxsaved);
	}

	state->sql_head = head.data;
	state->sql_tail = tail.data;

	if (state->split)
	{
		StringInfoData dh;
		StringInfoData ih;
		bool		identity = false;

		/* a segment trusts only the coordinator to move rows (gp_split.c) */
		state->split_calls = GpClusterHasSecret();

		initStringInfo(&dh);
		appendStringInfo(&dh, "DELETE FROM %s%s AS gp_t USING (VALUES ",
						 state->only ? "ONLY " : "",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		state->delete_head = dh.data;
		state->delete_tail = ") AS gp_s (gp_ctid, gp_toid, gp_n) WHERE gp_t.ctid = gp_s.gp_ctid AND gp_t.tableoid = gp_s.gp_toid RETURNING gp_s.gp_n, gp_t.tableoid, gp_t.ctid, gp_t.*";
		state->olddesc = returned_desc(targetdesc, true, false);

		initStringInfo(&ih);
		appendStringInfo(&ih, "INSERT INTO %s AS gp_t (",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		state->insattnos = palloc_array(AttrNumber, targetdesc->natts);
		state->insout = palloc_array(FmgrInfo, targetdesc->natts);
		for (i = 0; i < targetdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(targetdesc, i);
			Oid			func;
			bool		isvarlena;

			if (att->attisdropped || att->attgenerated != '\0')
				continue;
			if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
				identity = true;
			appendStringInfo(&ih, "%s%s", state->ninsert > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
			state->insert_casts = lappend(state->insert_casts,
										  cast_to(att->atttypid, att->atttypmod));
			getTypeOutputInfo(att->atttypid, &func, &isvarlena);
			fmgr_info(func, &state->insout[state->ninsert]);
			state->insattnos[state->ninsert++] = att->attnum;
		}
		appendStringInfo(&ih, ")%s VALUES ",
						 identity ? " OVERRIDING SYSTEM VALUE" : "");
		state->insert_head = ih.data;
		state->insert_tail = state->back ? " RETURNING gp_t.tableoid, gp_t.ctid, gp_t.*" : "";
		state->newdesc = returned_desc(targetdesc, false, false);
		state->hash = GpHashMake(policy, targetdesc);
		if (state->outerslot == NULL)
			state->outerslot = MakeSingleTupleTableSlot(ExecGetResultType(outerPlanState(node)),
														&TTSOpsMinimalTuple);
		if (state->saved == NULL)
		{
			state->maxsaved = 64;
			state->saved = palloc_array(MinimalTuple, state->maxsaved);
		}
	}

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * A write in a WITH query runs to its end whether or not the query reads
	 * what it returns, as ExecPostprocessPlan() runs a ModifyTable that does
	 * not set the command's tag (ExecInitModifyTable()).
	 */
	if (!state->canSetTag)
		estate->es_auxmodifytables = lcons(node, estate->es_auxmodifytables);

	/*
	 * Cloudberry without its global deadlock detector: an UPDATE or DELETE
	 * of a distributed table locks the table, so that two of them never
	 * wait for each other on different segments -- and here, so that a row
	 * the plan read does not change before it is written.  A partitioned
	 * table's partitions it writes are locked as it is, as Cloudberry's
	 * planner locks them in the table's mode; an INSERT into one locks every
	 * partition.
	 */
	if ((state->operation == CMD_UPDATE || state->operation == CMD_DELETE ||
		 state->operation == CMD_MERGE ||
		 state->on_conflict == ONCONFLICT_UPDATE) &&
		!gp_enable_global_deadlock_detector)
	{
		LockRelationOid(RelationGetRelid(state->target), ExclusiveLock);
		for (i = 0; i < state->nrels; i++)
			if (state->rels[i] != state->target)
				LockRelationOid(RelationGetRelid(state->rels[i]), ExclusiveLock);
	}

	/*
	 * With the detector on, it locks rows, as Cloudberry's does, and a row
	 * may change between being read and being written: a segment's
	 * statement then rechecks the row's new version, which the ctid it was
	 * sent never matches, and passes it over.  So what a statement came
	 * short of is asked about (explicit_recheck()) -- at READ COMMITTED: a
	 * transaction snapshot's statement fails on such a row itself.
	 */
	state->recheck = gp_enable_global_deadlock_detector &&
		!IsolationUsesXactSnapshot() &&
		(state->operation == CMD_UPDATE || state->operation == CMD_DELETE ||
		 state->operation == CMD_MERGE);
	state->mfrom = -1;

	if (state->operation == CMD_INSERT)
		GpModifyLockPartitions(RelationGetRelid(state->target),
							   state->on_conflict == ONCONFLICT_UPDATE
							   ? ExclusiveLock : RowExclusiveLock);
	if (state->operation == CMD_MERGE)
		GpModifyLockPartitions(RelationGetRelid(state->target), ExclusiveLock);

	GpClusterSegments(&state->nsegs);
	state->batches = palloc0_array(List *, state->nsegs);
	foreach_ptr(MergeShape, shape, state->mshapes)
		shape->batches = palloc0_array(List *, state->nsegs);
	if (state->mdelete != NULL)
		state->mdelete->batches = palloc0_array(List *, state->nsegs);
	if (state->minsert != NULL)
		state->minsert->batches = palloc0_array(List *, state->nsegs);
	state->rowcxt = AllocSetContextCreate(estate->es_query_cxt,
										  "gp explicit rows",
										  ALLOCSET_DEFAULT_SIZES);
	GpReportDispatch(0, false, state->numsegments);
	GpReportDtxReached(estate->es_plannedstmt, NULL, state->numsegments);
}

/* The result relation a row of this table is written as. */
static int
result_rel_of(ExplicitState *state, Oid relid)
{
	for (int i = 0; i < state->nrels; i++)
		if (RelationGetRelid(state->rels[i]) == relid)
			return i;
	elog(ERROR, "a row of relation %u is not one this write writes", relid);
	return 0;					/* keep the compiler quiet */
}

/*
 * Whether the plan names this target row for the first time.  Each is sent
 * once -- as PostgreSQL's UPDATE ... FROM changes a row once, by the first
 * of the plan's rows that finds it, and passes the others over -- so that
 * a statement that writes fewer rows than it was sent came short of some.
 */
static bool
explicit_first_time(ExplicitState *state, ItemPointer synthetic, Oid relid)
{
	RowTouched	key;
	bool		found;

	if (state->sent == NULL)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(RowTouched);
		ctl.entrysize = sizeof(RowTouched);
		ctl.hcxt = state->css.ss.ps.state->es_query_cxt;
		state->sent = hash_create("gp explicit rows sent", 256, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(synthetic, &key.tid);
	(void) hash_search(state->sent, &key, HASH_ENTER, &found);
	return !found;
}

/* Run the plan to its end: each row it would write, to its segment's batch. */
static void
explicit_collect(ExplicitState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	PlanState  *child = outerPlanState(state);
	int			nparams = state->nvals + (state->operation != CMD_INSERT ? 3 : 0);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		MemoryContext oldcxt;
		const char **params;
		int			content = -1;
		int			relidx = 0;
		int			p = 0;

		if (TupIsNull(slot))
			break;
		CHECK_FOR_INTERRUPTS();

		oldcxt = MemoryContextSwitchTo(state->rowcxt);
		params = palloc0_array(const char *, Max(nparams, 1));

		if (state->operation != CMD_INSERT)
		{
			ItemPointerData tid;
			bool		isnull;
			Datum		d;

			if (state->nrels > 1)
			{
				d = slot_getattr(slot, state->tableoidcol, &isnull);
				if (isnull)
					elog(ERROR, "a row to write has no tableoid");
				relidx = result_rel_of(state, DatumGetObjectId(d));
			}
			d = slot_getattr(slot, state->ctidcol, &isnull);
			if (isnull)
				elog(ERROR, "a row to write has no ctid");
			if (!row_identity_find(estate, (ItemPointer) DatumGetPointer(d),
								   &content, &tid))
				elog(ERROR, "a row to write was not read from a segment");

			/*
			 * A row the plan found before is passed over; a Split's is
			 * refused, as Cloudberry's is (split_multiple_updates(),
			 * gp_split.c): its second new version could not be taken back.
			 */
			if (!explicit_first_time(state, (ItemPointer) DatumGetPointer(d),
									 RelationGetRelid(state->rels[relidx])))
			{
				if (state->split)
					ereport(ERROR,
							(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
							 errmsg("multiple updates to a row by the same query is not allowed")));
				pfree(params);
				MemoryContextSwitchTo(oldcxt);
				ResetPerTupleExprContext(estate);
				continue;
			}

			params[p++] = DatumGetCString(DirectFunctionCall1(tidout,
															  ItemPointerGetDatum(&tid)));
			params[p++] = psprintf("%u", RelationGetRelid(state->rels[relidx]));
			params[p++] = psprintf(UINT64_FORMAT, state->nsaved);
		}

		for (int i = 0; i < state->nvals; i++)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, state->valcols[i], &isnull);

			params[p++] = isnull ? NULL : OutputFunctionCall(&state->valout[i], d);
		}

		if (state->operation == CMD_INSERT)
		{
			slot_getallattrs(slot);
			content = state->hash != NULL
				? GpHashSegment(state->hash, slot->tts_values, slot->tts_isnull)
				: 0;
		}

		if (content == GP_HASH_ALL_SEGMENTS)
			state->everywhere = lappend(state->everywhere, params);
		else
			state->batches[content] = lappend(state->batches[content], params);
		MemoryContextSwitchTo(oldcxt);

		if (state->back || state->split)
		{
			if (state->nsaved == state->maxsaved)
			{
				state->maxsaved *= 2;
				state->saved = repalloc_huge(state->saved,
											 state->maxsaved * sizeof(MinimalTuple));
			}
			oldcxt = MemoryContextSwitchTo(state->rowcxt);
			state->saved[state->nsaved] = ExecCopySlotMinimalTuple(slot);
			MemoryContextSwitchTo(oldcxt);
		}
		state->nsaved++;
		ResetPerTupleExprContext(estate);
	}
}


static uint64 explicit_send_statements(int content, List *rows, int nparams,
									   const char *head, const char *tail,
									   List *casts, TupleDesc desc,
									   Tuplestorestate *store,
									   const ExplicitRecheck *recheck);
static uint64 explicit_send_split(ExplicitState *state);

/*
 * A row an action of this MERGE changes, by its table and the ctid the plan
 * knows it by -- two partitions' rows may share a segment and a ctid: a
 * second is refused, as the SQL standard has it and PostgreSQL refuses it
 * (ExecMergeMatched()).
 */
static void
merge_touch(ExplicitState *state, ItemPointer synthetic, Oid relid)
{
	RowTouched	key;
	bool		found;

	if (state->mtouched == NULL)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(RowTouched);
		ctl.entrysize = sizeof(RowTouched);
		ctl.hcxt = state->css.ss.ps.state->es_query_cxt;
		state->mtouched = hash_create("gp merge rows", 256, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(synthetic, &key.tid);
	(void) hash_search(state->mtouched, &key, HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
		/* translator: %s is a SQL command name */
				 errmsg("%s command cannot affect row a second time",
						"MERGE"),
				 errhint("Ensure that not more than one source row matches any one target row.")));
}

/*
 * A new row's checks, as ExecUpdateAct() and ExecInsert() make them: its
 * generated columns computed, a policy's WITH CHECK, then a view's.  The
 * segment's own statement computes the row again, and checks its policies.
 */
static void
merge_check_new(ExplicitState *state, ResultRelInfo *rri,
				TupleTableSlot *slot, CmdType cmd)
{
	EState	   *estate = state->css.ss.ps.state;
	TupleConstr *constr = RelationGetDescr(rri->ri_RelationDesc)->constr;

	if (rri->ri_WithCheckOptions == NIL)
		return;
	if (constr != NULL && constr->has_generated_stored)
		ExecComputeStoredGenerated(rri, estate, slot, cmd);
	ExecWithCheckOptions(cmd == CMD_UPDATE ? WCO_RLS_UPDATE_CHECK : WCO_RLS_INSERT_CHECK,
						 rri, slot, estate);
	ExecWithCheckOptions(WCO_VIEW_CHECK, rri, slot, estate);
}

/* A row's place, for an UPDATE's or a DELETE's statement: its params' first */
static const char **
merge_row_params(ExplicitState *state, int nparams, ItemPointer synthetic,
				 Datum target, Oid relid, int *content)
{
	const char **params = palloc0_array(const char *, Max(nparams, 1) + 1);

	if (state->by_content)
	{
		ItemPointerData tid;

		/*
		 * a replicated table's row, by its text, on every segment -- and,
		 * after its parameters, its ctid on the segment it was read on, for
		 * a statement that comes short of it (explicit_recheck())
		 */
		params[0] = OutputFunctionCall(&state->mtextout, target);
		if (!row_identity_find(state->css.ss.ps.state, synthetic, &state->mfrom, &tid))
			elog(ERROR, "a row to write was not read from a segment");
		params[nparams] = DatumGetCString(DirectFunctionCall1(tidout,
															  ItemPointerGetDatum(&tid)));
		*content = GP_HASH_ALL_SEGMENTS;
	}
	else
	{
		ItemPointerData tid;

		if (!row_identity_find(state->css.ss.ps.state, synthetic, content, &tid))
			elog(ERROR, "a row to write was not read from a segment");
		params[0] = DatumGetCString(DirectFunctionCall1(tidout,
														ItemPointerGetDatum(&tid)));
	}
	params[1] = psprintf("%u", relid);
	params[2] = psprintf(UINT64_FORMAT, state->nsaved);
	return params;
}

static void
merge_add(MergeShape *shape, int content, const char **params)
{
	if (content == GP_HASH_ALL_SEGMENTS)
		shape->everywhere = lappend(shape->everywhere, params);
	else
		shape->batches[content] = lappend(shape->batches[content], params);
}

/*
 * A row the plan joined to a row of the target: the first MATCHED action
 * whose WHEN condition holds -- or NOT MATCHED BY SOURCE, where the join
 * condition fails -- over the target's row, as ExecMergeMatched() does it.
 */
static void
merge_matched(ExplicitState *state, TupleTableSlot *slot, ItemPointer synthetic)
{
	EState	   *estate = state->css.ss.ps.state;
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	TupleDesc	rootdesc = RelationGetDescr(state->target);
	int			relidx = 0;
	ResultRelInfo *rri;
	TupleTableSlot *oldslot;
	HeapTupleHeader td;
	HeapTupleData tuple;
	List	   *actions;
	Datum		target;
	bool		isnull;
	Oid			relid;

	if (!AttributeNumberIsValid(state->targetcol))
		return;					/* no action reads a matched row */
	if (state->nrels > 1)
		relidx = result_rel_of(state,
							   DatumGetObjectId(slot_getattr(slot, state->tableoidcol,
															 &isnull)));
	rri = &state->rris[relidx];
	relid = RelationGetRelid(state->rels[relidx]);

	/*
	 * The target's row, as the root has it and as its relation does -- or
	 * as its partition has it, where ORCA's plan carried it up from the
	 * partition's scan (gp_orca's merge.c).
	 */
	target = slot_getattr(slot, state->targetcol, &isnull);
	if (isnull)
		elog(ERROR, "a row MERGE matched came without the target's row");
	td = DatumGetHeapTupleHeader(target);
	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = relid;
	tuple.t_data = td;
	if (HeapTupleHeaderGetTypeId(td) != rootdesc->tdtypeid &&
		state->rels[relidx] != state->target)
	{
		TupleDesc	reldesc = RelationGetDescr(state->rels[relidx]);

		if (HeapTupleHeaderGetTypeId(td) != reldesc->tdtypeid)
			elog(ERROR, "a row MERGE matched came as a row of another table");
		oldslot = state->moldslots[relidx];
		ExecClearTuple(oldslot);
		heap_deform_tuple(&tuple, reldesc, oldslot->tts_values, oldslot->tts_isnull);
		ExecStoreVirtualTuple(oldslot);
	}
	else
	{
		ExecClearTuple(state->mrootslot);
		heap_deform_tuple(&tuple, rootdesc, state->mrootslot->tts_values,
						  state->mrootslot->tts_isnull);
		ExecStoreVirtualTuple(state->mrootslot);
		oldslot = state->mrootslot;
		if (state->rels[relidx] != state->target)
			oldslot = state->mmaps[relidx] != NULL
				? execute_attr_map_slot(state->mmaps[relidx]->attrMap,
										state->mrootslot, state->moldslots[relidx])
				: ExecCopySlot(state->moldslots[relidx], state->mrootslot);
	}
	oldslot->tts_tableOid = relid;
	econtext->ecxt_scantuple = oldslot;

	actions = state->mactions[relidx * NUM_MERGE_MATCH_KINDS +
							  (ExecQual(state->mjoins[relidx], econtext)
							   ? MERGE_WHEN_MATCHED
							   : MERGE_WHEN_NOT_MATCHED_BY_SOURCE)];
	foreach_ptr(MergeExec, e, actions)
	{
		CmdType		cmd = e->action->commandType;
		MemoryContext oldcxt;

		if (!ExecQual(e->when, econtext))
			continue;
		if (cmd == CMD_NOTHING)
			break;

		/* the target's row, against the policies' USING of what it does */
		merge_touch(state, synthetic, relid);
		if (rri->ri_WithCheckOptions != NIL)
			ExecWithCheckOptions(cmd == CMD_UPDATE ? WCO_RLS_MERGE_UPDATE_CHECK
								 : WCO_RLS_MERGE_DELETE_CHECK,
								 rri, oldslot, estate);

		if (cmd == CMD_DELETE)
		{
			int			content;
			const char **params;

			oldcxt = MemoryContextSwitchTo(state->rowcxt);
			params = merge_row_params(state, 3, synthetic, target, relid, &content);
			merge_add(e->shape, content, params);
			MemoryContextSwitchTo(oldcxt);
		}
		else if (cmd == CMD_UPDATE)
		{
			TupleTableSlot *newslot = ExecProject(e->proj);

			merge_check_new(state, rri, newslot, CMD_UPDATE);
			slot_getallattrs(newslot);
			oldcxt = MemoryContextSwitchTo(state->rowcxt);
			if (e->moves)
			{
				/*
				 * A Split: the row deleted where it is, and its new version,
				 * as the root has it, inserted where it hashes.
				 */
				TupleTableSlot *rootnew = newslot;
				ItemPointerData tid;
				int			content;
				const char **params = palloc_array(const char *, 3);

				if (!row_identity_find(estate, synthetic, &content, &tid))
					elog(ERROR, "a row to write was not read from a segment");
				params[0] = DatumGetCString(DirectFunctionCall1(tidout,
																ItemPointerGetDatum(&tid)));
				params[1] = psprintf("%u", relid);
				params[2] = psprintf(UINT64_FORMAT, state->nsaved);
				state->batches[content] = lappend(state->batches[content], params);
				if (state->mtoroot[relidx] != NULL)
					rootnew = execute_attr_map_slot(state->mtoroot[relidx]->attrMap,
													newslot, state->minsslot);
				slot_getallattrs(rootnew);
				state->mnew[state->nsaved] = heap_form_tuple(rootdesc,
															 rootnew->tts_values,
															 rootnew->tts_isnull);
			}
			else
			{
				int			nset = list_length(e->action->updateColnos);
				int			content;
				const char **params = merge_row_params(state, 3 + nset, synthetic,
													   target, relid, &content);
				int			j = 0;

				foreach_int(attno, e->action->updateColnos)
				{
					params[3 + j] = newslot->tts_isnull[attno - 1] ? NULL
						: OutputFunctionCall(&e->out[j], newslot->tts_values[attno - 1]);
					j++;
				}
				merge_add(e->shape, content, params);
			}
			MemoryContextSwitchTo(oldcxt);
		}
		break;
	}
}

/*
 * A row of the source the plan joined to no row of the target: the first
 * NOT MATCHED action whose WHEN condition holds, as ExecMergeNotMatched()
 * does it -- the first result relation's, whose INSERT is the root's.
 */
static void
merge_not_matched(ExplicitState *state)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;

	econtext->ecxt_scantuple = NULL;
	foreach_ptr(MergeExec, e, state->mactions[MERGE_WHEN_NOT_MATCHED_BY_TARGET])
	{
		TupleTableSlot *newslot;
		TupleTableSlot *checked;
		MemoryContext oldcxt;
		const char **params;
		MergeShape *shape = e->shape;
		int			content;

		if (!ExecQual(e->when, econtext))
			continue;
		if (e->action->commandType != CMD_INSERT)
			break;

		/* checked as the first result relation checks it, its columns its own */
		newslot = ExecProject(e->proj);
		slot_getallattrs(newslot);
		checked = newslot;
		if (state->mmaps[0] != NULL)
			checked = execute_attr_map_slot(state->mmaps[0]->attrMap, newslot,
											state->mnewslots[0]);
		else if (state->rels[0] != state->target)
			checked = ExecCopySlot(state->mnewslots[0], newslot);
		merge_check_new(state, &state->rris[0], checked, CMD_INSERT);

		content = state->hash != NULL
			? GpHashSegment(state->hash, newslot->tts_values, newslot->tts_isnull)
			: 0;
		oldcxt = MemoryContextSwitchTo(state->rowcxt);
		params = palloc_array(const char *, Max(shape->nvals, 1));
		for (int i = 0; i < shape->nvals; i++)
		{
			AttrNumber	a = shape->attnos[i] - 1;

			params[i] = newslot->tts_isnull[a] ? NULL
				: OutputFunctionCall(&shape->out[i], newslot->tts_values[a]);
		}
		merge_add(shape, content, params);
		MemoryContextSwitchTo(oldcxt);
		break;
	}
}

/*
 * MERGE: run the plan to its end, and do for each row what ExecMerge() does,
 * each action's row to its statement's batch.
 */
static void
explicit_collect_merge(ExplicitState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	PlanState  *child = outerPlanState(state);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		bool		isnull;
		Datum		ctid;

		if (TupIsNull(slot))
			break;
		CHECK_FOR_INTERRUPTS();
		ResetExprContext(econtext);

		/* a Split's new rows, by the plan's row */
		if (state->split && state->mnew == NULL)
		{
			state->maxsaved = 64;
			state->mnew = MemoryContextAllocZero(estate->es_query_cxt,
												 state->maxsaved * sizeof(HeapTuple));
		}
		else if (state->split && state->nsaved == state->maxsaved)
		{
			state->mnew = repalloc0_array(state->mnew, HeapTuple,
										  state->maxsaved, state->maxsaved * 2);
			state->maxsaved *= 2;
		}

		/* the source's row, as the inner tuple, as MERGE's executor has it */
		econtext->ecxt_innertuple = slot;
		econtext->ecxt_outertuple = NULL;
		ctid = slot_getattr(slot, state->ctidcol, &isnull);
		if (isnull)
			merge_not_matched(state);
		else
			merge_matched(state, slot, (ItemPointer) DatumGetPointer(ctid));
		state->nsaved++;
		ResetPerTupleExprContext(estate);
	}
}

/*
 * One statement's rows, each segment's, and a replicated table's once.  An
 * UPDATE's or a DELETE's that came short of a row another transaction
 * changed fails, and so does one it deleted where the MERGE would have
 * tried its NOT MATCHED actions for it, as PostgreSQL's does.
 */
static uint64
merge_send_shape(ExplicitState *state, MergeShape *shape)
{
	int			nparams = list_length(shape->casts);
	uint64		total = 0;
	ExplicitRecheck recheck = {0};
	const ExplicitRecheck *rc = NULL;

	if (state->recheck && shape->cmd != CMD_INSERT)
	{
		recheck.target = state->target;
		recheck.deleted = state->mactions[MERGE_WHEN_NOT_MATCHED_BY_TARGET] != NIL
			? 'm' : 'p';
		recheck.content = -1;
		rc = &recheck;
	}

	for (int seg = 0; seg < state->nsegs; seg++)
		if (shape->batches[seg] != NIL)
			total += explicit_send_statements(seg, shape->batches[seg], nparams,
											  shape->head, shape->tail,
											  shape->casts, NULL, NULL, rc);
	if (shape->everywhere != NIL)
	{
		if (rc != NULL)
		{
			ListCell   *lc;

			/* a replicated table's rows' ctids where they were read */
			recheck.content = state->mfrom;
			recheck.ctids = palloc_array(const char *, list_length(shape->everywhere));
			foreach(lc, shape->everywhere)
				recheck.ctids[foreach_current_index(lc)] =
					((const char **) lfirst(lc))[nparams];
		}
		for (int seg = 0; seg < Min(state->nsegs, state->numsegments); seg++)
		{
			uint64		n = explicit_send_statements(seg, shape->everywhere, nparams,
													 shape->head, shape->tail,
													 shape->casts, NULL, NULL, rc);

			if (seg == 0)
				total += n;
		}
	}
	return total;
}

/*
 * MERGE's rows: the DELETEs', then each UPDATE's, then the Split's, then the
 * INSERTs' -- so that a row an INSERT brings does not meet one this MERGE
 * deletes or moves.  A MERGE's count is of the rows its actions wrote.
 */
static void
explicit_send_merge(ExplicitState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	uint64		total = 0;

	if (state->mdelete != NULL)
		total += merge_send_shape(state, state->mdelete);
	foreach_ptr(MergeShape, shape, state->mshapes)
		total += merge_send_shape(state, shape);
	if (state->split)
		total += explicit_send_split(state);
	if (state->minsert != NULL)
		total += merge_send_shape(state, state->minsert);
	if (state->canSetTag)
		estate->es_processed += total;
}

/*
 * Why a statement wrote fewer of a batch's rows -- rows[first] on, nrows of
 * them -- than it was sent: asked of the segment they were read on, which
 * fails the statement where another transaction changed one since
 * (explicit_recheck(), gp_split.c).
 */
static void
explicit_recheck(const ExplicitRecheck *recheck, int content, List *rows,
				 int first, int nrows)
{
	StringInfoData tids;
	StringInfoData oids;
	const char *values[2];
	char	   *sql;

	initStringInfo(&tids);
	initStringInfo(&oids);
	appendStringInfoChar(&tids, '{');
	appendStringInfoChar(&oids, '{');
	for (int i = first; i < first + nrows; i++)
	{
		const char **params = (const char **) list_nth(rows, i);

		appendStringInfo(&tids, "%s\"%s\"", i > first ? "," : "",
						 recheck->ctids != NULL ? recheck->ctids[i] : params[0]);
		appendStringInfo(&oids, "%s%s", i > first ? "," : "", params[1]);
	}
	appendStringInfoChar(&tids, '}');
	appendStringInfoChar(&oids, '}');
	values[0] = tids.data;
	values[1] = oids.data;
	sql = psprintf("SELECT gp_internal.explicit_recheck(NULL::%s, $1::pg_catalog.tid[], $2::pg_catalog.oid[], '%c')",
				   GpDispatchRelationName(RelationGetRelid(recheck->target)),
				   recheck->deleted);
	(void) GpDispatchWriteOnContent(recheck->content >= 0 ? recheck->content : content,
									sql, 2, values, NULL, NULL);
	pfree(sql);
	pfree(tids.data);
	pfree(oids.data);
}

/*
 * One segment's rows, a statement a batch; how many it wrote.  "store" takes
 * what RETURNING gave, where it is to be kept; "recheck", where it is not
 * NULL, says what a batch that came short is asked about.
 */
static uint64
explicit_send_statements(int content, List *rows, int nparams,
						 const char *head, const char *tail, List *casts,
						 TupleDesc desc, Tuplestorestate *store,
						 const ExplicitRecheck *recheck)
{
	int			per = Min(EXPLICIT_BATCH_ROWS, EXPLICIT_MAX_PARAMS / Max(nparams, 1));
	uint64		total = 0;
	ListCell   *lc = list_head(rows);
	int			first = 0;

	while (lc != NULL)
	{
		StringInfoData sql;
		const char **values;
		int			nrows = 0;
		int			n = 0;
		uint64		written;

		initStringInfo(&sql);
		appendStringInfoString(&sql, head);
		values = palloc_array(const char *, per * Max(nparams, 1));
		for (; lc != NULL && nrows < per; lc = lnext(rows, lc), nrows++)
		{
			const char **params = (const char **) lfirst(lc);

			appendStringInfoString(&sql, nrows > 0 ? ", (" : "(");
			for (int i = 0; i < nparams; i++)
			{
				values[n] = params[i];
				appendStringInfo(&sql, "%s$%d::%s", i > 0 ? ", " : "", n + 1,
								 (char *) list_nth(casts, i));
				n++;
			}
			appendStringInfoChar(&sql, ')');
		}
		appendStringInfoString(&sql, tail);

		written = GpDispatchWriteOnContent(content, sql.data, n, values,
										   desc, store);
		if (recheck != NULL && written < (uint64) nrows)
			explicit_recheck(recheck, content, rows, first, nrows);
		total += written;
		first += nrows;
		pfree(sql.data);
		pfree(values);
	}
	return total;
}

static uint64
explicit_send_rows(ExplicitState *state, int content, List *rows,
				   Tuplestorestate *store, const ExplicitRecheck *recheck)
{
	return explicit_send_statements(content, rows,
									state->nvals + (state->operation != CMD_INSERT ? 3 : 0),
									state->sql_head, state->sql_tail,
									state->casts, state->retdesc, store,
									recheck);
}

/*
 * One half of a Split on one segment, by gp_internal.split_delete() or
 * split_insert() (gp_split.c): the rows -- the ctids of those to delete, or
 * the new versions, as the root's rows -- their tables and their numbers,
 * each an array, EXPLICIT_SPLIT_ROWS at a time.  What the call returns --
 * each row's number, its table, its ctid there, and it as the root has it
 * -- goes to "store".
 */
static uint64
explicit_split_call(ExplicitState *state, int content, bool insert,
					List *rows, List *tables, List *numbers,
					Tuplestorestate *store)
{
	Oid			rowtype = RelationGetDescr(state->target)->tdtypeid;
	const char *name = GpDispatchRelationName(RelationGetRelid(state->target));
	char	   *sql;
	uint64		total = 0;
	int			first = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	get_typlenbyvalalign(rowtype, &typlen, &typbyval, &typalign);
	sql = insert
		? psprintf("SELECT gp_n, gp_toid, gp_ctid, (gp_row).* FROM gp_internal.split_insert(NULL::%s, $1::%s[], $2::pg_catalog.oid[], $3::pg_catalog.int8[])",
				   name, name)
		: psprintf("SELECT gp_n, gp_toid, gp_ctid, (gp_row).* FROM gp_internal.split_delete(NULL::%s, $1::pg_catalog.tid[], $2::pg_catalog.oid[], $3::pg_catalog.int8[])",
				   name);

	while (first < list_length(rows))
	{
		int			count = Min(EXPLICIT_SPLIT_ROWS, list_length(rows) - first);
		StringInfoData items;
		StringInfoData oids;
		StringInfoData ns;
		const char *values[3];

		initStringInfo(&items);
		initStringInfo(&oids);
		initStringInfo(&ns);
		appendStringInfoChar(&oids, '{');
		appendStringInfoChar(&ns, '{');
		if (!insert)
			appendStringInfoChar(&items, '{');
		for (int i = first; i < first + count; i++)
		{
			const char *sep = i > first ? "," : "";

			if (insert)
			{
				appendStringInfo(&oids, "%s%u", sep, list_nth_oid(tables, i));
				appendStringInfo(&ns, "%s%d", sep, intVal(list_nth(numbers, i)));
			}
			else
			{
				/* a row to delete: its ctid, its table and its number */
				const char **params = (const char **) list_nth(rows, i);

				appendStringInfo(&items, "%s\"%s\"", sep, params[0]);
				appendStringInfo(&oids, "%s%s", sep, params[1]);
				appendStringInfo(&ns, "%s%s", sep, params[2]);
			}
		}
		appendStringInfoChar(&oids, '}');
		appendStringInfoChar(&ns, '}');
		if (insert)
		{
			/* the new versions, an array of the root's rows */
			Datum	   *elems = palloc_array(Datum, count);
			ArrayType  *array;

			for (int i = 0; i < count; i++)
				elems[i] = PointerGetDatum(list_nth(rows, first + i));
			array = construct_array(elems, count, rowtype, typlen, typbyval,
									typalign);
			appendStringInfoString(&items,
								   OidOutputFunctionCall(F_ARRAY_OUT,
														 PointerGetDatum(array)));
		}
		else
			appendStringInfoChar(&items, '}');

		values[0] = items.data;
		values[1] = oids.data;
		values[2] = ns.data;
		total += GpDispatchWriteOnContent(content, sql, 3, values,
										  state->olddesc, store);
		first += count;
	}
	return total;
}

/*
 * A Split: every row deleted where it is, returning it; its new version, the
 * old with the SET columns' new values, hashed by its key and inserted
 * where that says; and what the INSERTs return, each with the number of the
 * plan's row, for RETURNING and the check options.  The rows it moved are
 * the rows it deleted.
 *
 * Where the cluster has its secret, gp_internal.split_delete() and
 * split_insert() move them on the segments, firing no trigger, as
 * Cloudberry's Split fires none, and applying no policy, the coordinator
 * checking the new rows as an UPDATE's.  Without it a segment cannot tell
 * the coordinator's call from anyone's, and they are moved by a DELETE and
 * an INSERT, which fire the table's row triggers and apply its policies,
 * as their own.
 */
static uint64
explicit_send_split(ExplicitState *state)
{
	TupleDesc	targetdesc = RelationGetDescr(state->target);
	Tuplestorestate *olds = tuplestore_begin_heap(false, false, work_mem);
	TupleTableSlot *oldslot = MakeSingleTupleTableSlot(state->olddesc,
													   &TTSOpsMinimalTuple);
	Datum	   *values = palloc_array(Datum, targetdesc->natts);
	bool	   *nulls = palloc_array(bool, targetdesc->natts);
	List	  **inserts = palloc0_array(List *, state->nsegs);
	List	  **numbers = palloc0_array(List *, state->nsegs);
	List	  **tables = palloc0_array(List *, state->nsegs);
	MinimalTuple *olders = NULL;
	uint64		deleted = 0;
	MemoryContext oldcxt;
	ExplicitRecheck recheck = {0};

	/*
	 * A row another transaction changed since is refused, deleted or
	 * updated, as Cloudberry's Split refuses it: by split_delete() itself,
	 * and after a DELETE that came short of it (explicit_recheck()).
	 */
	recheck.target = state->target;
	recheck.deleted = 's';
	recheck.content = -1;

	for (int seg = 0; seg < state->nsegs; seg++)
	{
		if (state->batches[seg] == NIL)
			continue;
		if (state->split_calls)
			deleted += explicit_split_call(state, seg, false, state->batches[seg],
										   NIL, NIL, olds);
		else
			deleted += explicit_send_statements(seg, state->batches[seg], 3,
												state->delete_head,
												state->delete_tail,
												list_copy_head(state->casts, 3),
												state->olddesc, olds,
												state->recheck ? &recheck : NULL);
	}

	/* the deleted rows by number, RETURNING's old ones */
	if (state->other)
		olders = palloc0_array(MinimalTuple, Max(state->nsaved, 1));

	oldcxt = MemoryContextSwitchTo(state->rowcxt);
	while (tuplestore_gettupleslot(olds, true, false, oldslot))
	{
		int64		n;
		int			col = 3;	/* past its number, its table and its ctid */
		int			seg;

		slot_getallattrs(oldslot);
		n = DatumGetInt64(oldslot->tts_values[0]);
		for (int i = 0; i < targetdesc->natts; i++)
		{
			if (TupleDescAttr(targetdesc, i)->attisdropped)
			{
				values[i] = (Datum) 0;
				nulls[i] = true;
				continue;
			}
			values[i] = oldslot->tts_values[col];
			nulls[i] = oldslot->tts_isnull[col];
			col++;
		}

		if (state->merge)
		{
			/* a MERGE's new row, which its UPDATE action made */
			heap_deform_tuple(state->mnew[n], targetdesc, values, nulls);
		}
		else
		{
			/* the SET columns' new values, from the plan's row that asked */
			ExecStoreMinimalTuple(state->saved[n], state->outerslot, false);
			for (int k = 0; k < state->nvals; k++)
				values[state->setattnos[k] - 1] =
					slot_getattr(state->outerslot, state->valcols[k],
								 &nulls[state->setattnos[k] - 1]);
		}

		seg = GpHashSegment(state->hash, values, nulls);
		if (state->split_calls)
		{
			HeapTuple	tuple = heap_form_tuple(targetdesc, values, nulls);

			inserts[seg] = lappend(inserts[seg],
								   DatumGetPointer(heap_copy_tuple_as_datum(tuple, targetdesc)));
			tables[seg] = lappend_oid(tables[seg],
									  DatumGetObjectId(oldslot->tts_values[1]));
		}
		else
		{
			const char **params = palloc_array(const char *, Max(state->ninsert, 1));

			for (int i = 0; i < state->ninsert; i++)
			{
				AttrNumber	a = state->insattnos[i] - 1;

				params[i] = nulls[a] ? NULL
					: OutputFunctionCall(&state->insout[i], values[a]);
			}
			inserts[seg] = lappend(inserts[seg], params);
		}
		numbers[seg] = lappend(numbers[seg], makeInteger((int) n));
		if (state->other)
			olders[n] = ExecCopySlotMinimalTuple(oldslot);
	}
	MemoryContextSwitchTo(oldcxt);
	tuplestore_end(olds);

	for (int seg = 0; seg < state->nsegs; seg++)
	{
		Tuplestorestate *news = NULL;
		TupleTableSlot *newslot;
		ListCell   *ln;

		if (inserts[seg] == NIL)
			continue;
		if (state->back)
			news = tuplestore_begin_heap(false, false, work_mem);
		if (state->split_calls)
			(void) explicit_split_call(state, seg, true, inserts[seg],
									   tables[seg], numbers[seg], news);
		else
			(void) explicit_send_statements(seg, inserts[seg], state->ninsert,
											state->insert_head,
											state->insert_tail,
											state->insert_casts,
											state->back ? state->newdesc : NULL,
											news, NULL);
		if (news == NULL)
			continue;

		/*
		 * What came back, with the number of the plan's row: the call gives
		 * it; an INSERT returns its rows in the order it was given them.
		 */
		newslot = MakeSingleTupleTableSlot(state->split_calls ? state->olddesc
										   : state->newdesc,
										   &TTSOpsMinimalTuple);
		ln = list_head(numbers[seg]);
		while (tuplestore_gettupleslot(news, true, false, newslot))
		{
			Datum	   *rv = palloc_array(Datum, state->retdesc->natts);
			bool	   *rn = palloc_array(bool, state->retdesc->natts);
			int			from = state->split_calls ? 1 : 0;
			int			width = newslot->tts_tupleDescriptor->natts - from;
			int64		n;

			slot_getallattrs(newslot);
			if (state->split_calls)
				n = DatumGetInt64(newslot->tts_values[0]);
			else
			{
				if (ln == NULL)
					elog(ERROR, "segment %d returned more rows than it was given", seg);
				n = intVal(lfirst(ln));
				ln = lnext(numbers[seg], ln);
			}
			rv[0] = Int64GetDatum(n);
			rn[0] = false;
			memcpy(&rv[1], &newslot->tts_values[from], width * sizeof(Datum));
			memcpy(&rn[1], &newslot->tts_isnull[from], width * sizeof(bool));
			if (state->other)
			{
				/* the old row after the new: its table, ctid and columns */
				ExecStoreMinimalTuple(olders[n], oldslot, false);
				slot_getallattrs(oldslot);
				memcpy(&rv[1 + width], &oldslot->tts_values[1], width * sizeof(Datum));
				memcpy(&rn[1 + width], &oldslot->tts_isnull[1], width * sizeof(bool));
			}
			tuplestore_putvalues(state->returned, state->retdesc, rv, rn);
		}
		ExecDropSingleTupleTableSlot(newslot);
		tuplestore_end(news);
	}
	ExecDropSingleTupleTableSlot(oldslot);

	return deleted;
}

typedef struct RowText
{
	ItemPointerData tid;		/* the hash key */
	char	   *text;
} RowText;

/*
 * A replicated table's rows, as the segment the plan read them on holds
 * them: each one's text, by the ctid the plan carried, which a batch's rows
 * are then written with in place of the ctid.  Every row of one write was
 * read on one segment, the one the gather of a replicated table asks.
 */
static List *
explicit_rows_by_content(ExplicitState *state, int content, List *rows)
{
	StringInfoData tids;
	StringInfoData sql;
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	Tuplestorestate *store = tuplestore_begin_heap(false, false, work_mem);
	TupleTableSlot *slot;
	HASHCTL		ctl = {0};
	HTAB	   *texts;
	const char *param;
	ListCell   *lc;

	TupleDescInitEntry(desc, 1, "ctid", TIDOID, -1, 0);
	TupleDescInitEntry(desc, 2, "text", TEXTOID, -1, 0);
	TupleDescFinalize(desc);

	initStringInfo(&tids);
	appendStringInfoChar(&tids, '{');
	foreach(lc, rows)
		appendStringInfo(&tids, "%s\"%s\"", foreach_current_index(lc) > 0 ? "," : "",
						 ((const char **) lfirst(lc))[0]);
	appendStringInfoChar(&tids, '}');
	param = tids.data;

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT gp_r.ctid, gp_r::pg_catalog.text FROM %s%s AS gp_r WHERE gp_r.ctid = ANY ($1::pg_catalog.tid[])",
					 state->only ? "ONLY " : "",
					 GpDispatchRelationName(RelationGetRelid(state->target)));
	(void) GpDispatchWriteOnContent(content, sql.data, 1, &param, desc, store);

	ctl.keysize = sizeof(ItemPointerData);
	ctl.entrysize = sizeof(RowText);
	ctl.hcxt = CurrentMemoryContext;
	texts = hash_create("gp explicit row texts", Max(list_length(rows), 16),
						&ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(store, true, false, slot))
	{
		bool		isnull;
		ItemPointer tid = (ItemPointer) DatumGetPointer(slot_getattr(slot, 1, &isnull));
		RowText    *entry = hash_search(texts, tid, HASH_ENTER, NULL);

		entry->text = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
	}
	ExecDropSingleTupleTableSlot(slot);
	tuplestore_end(store);

	foreach(lc, rows)
	{
		const char **params = (const char **) lfirst(lc);
		ItemPointer tid = (ItemPointer) DatumGetPointer(DirectFunctionCall1(tidin,
																			CStringGetDatum(params[0])));
		RowText    *entry = hash_search(texts, tid, HASH_FIND, NULL);

		if (entry == NULL)
			elog(ERROR, "a row to write is no longer on segment %d", content);
		params[0] = entry->text;
	}
	return rows;
}

static void
explicit_send(ExplicitState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	uint64		total = 0;
	ExplicitRecheck recheck = {0};
	const ExplicitRecheck *rc = NULL;

	if (state->back)
		state->returned = tuplestore_begin_heap(false, false, work_mem);

	/* an UPDATE's or a DELETE's rows; a row deleted meanwhile passed over */
	if (state->recheck)
	{
		recheck.target = state->target;
		recheck.deleted = 'p';
		recheck.content = -1;
		rc = &recheck;
	}

	if (state->split)
		total += explicit_send_split(state);
	else if (state->by_content)
	{
		/*
		 * a replicated table's rows, found by their text on every segment
		 * of it -- a partial table's first so many -- and counted once
		 */
		for (int from = 0; from < state->nsegs; from++)
		{
			List	   *rows;

			if (state->batches[from] == NIL)
				continue;
			if (rc != NULL)
			{
				ListCell   *lc;

				/* their ctids where they were read, before their text */
				recheck.content = from;
				recheck.ctids = palloc_array(const char *,
											 list_length(state->batches[from]));
				foreach(lc, state->batches[from])
					recheck.ctids[foreach_current_index(lc)] =
						((const char **) lfirst(lc))[0];
			}
			rows = explicit_rows_by_content(state, from, state->batches[from]);
			for (int seg = 0; seg < Min(state->nsegs, state->numsegments); seg++)
			{
				uint64		n = explicit_send_rows(state, seg, rows,
												   seg == 0 ? state->returned : NULL,
												   rc);

				if (seg == 0)
					total += n;
			}
		}
	}
	else
		for (int seg = 0; seg < state->nsegs; seg++)
			if (state->batches[seg] != NIL)
				total += explicit_send_rows(state, seg, state->batches[seg],
											state->returned, rc);

	/*
	 * a replicated table's rows are written on every segment of it -- a
	 * partial table's first so many -- and counted once
	 */
	if (state->everywhere != NIL)
		for (int seg = 0; seg < Min(state->nsegs, state->numsegments); seg++)
		{
			uint64		n = explicit_send_rows(state, seg, state->everywhere,
											   seg == 0 ? state->returned : NULL,
											   NULL);

			if (seg == 0)
				total += n;
		}

	if (state->canSetTag)
		estate->es_processed += total;
}

/*
 * A version of a row the segments sent back, from column *col of it on: its
 * table, its ctid on the segment that wrote it -- what RETURNING's ctid
 * says, as a segment's own ModifyTable would say it -- then its columns, as
 * the root has them, in "rootslot"; and as the result relation relidx has
 * them, where that is not the root.  NULL where the row has no such
 * version, its table null.
 */
static TupleTableSlot *
explicit_returned_version(ExplicitState *state, int *col,
						  TupleTableSlot *rootslot, TupleTableSlot *relslot,
						  int relidx)
{
	TupleDesc	rootdesc = RelationGetDescr(state->target);
	TupleTableSlot *slot;
	bool		exists = !state->retslot->tts_isnull[*col];
	Oid			relid = DatumGetObjectId(state->retslot->tts_values[(*col)++]);
	ItemPointerData tid;

	if (state->retslot->tts_isnull[*col])
		ItemPointerSetInvalid(&tid);
	else
		ItemPointerCopy(DatumGetItemPointer(state->retslot->tts_values[*col]), &tid);
	(*col)++;

	/* the root's row, a dropped column null */
	ExecClearTuple(rootslot);
	for (int i = 0; i < rootdesc->natts; i++)
	{
		if (TupleDescAttr(rootdesc, i)->attisdropped)
		{
			rootslot->tts_values[i] = (Datum) 0;
			rootslot->tts_isnull[i] = true;
			continue;
		}
		rootslot->tts_values[i] = state->retslot->tts_values[*col];
		rootslot->tts_isnull[i] = state->retslot->tts_isnull[*col];
		(*col)++;
	}
	ExecStoreVirtualTuple(rootslot);
	if (!exists)
		return NULL;

	slot = rootslot;
	if (relslot != NULL)
	{
		if (state->maps[relidx] != NULL)
			slot = execute_attr_map_slot(state->maps[relidx]->attrMap,
										 rootslot, relslot);
		else
			slot = ExecCopySlot(relslot, rootslot);
	}
	slot->tts_tableOid = relid;
	slot->tts_tid = tid;
	return slot;
}

/*
 * The next row the segments sent back, into state->retslot: the result
 * relation the plan wrote it as, whose RETURNING it is -- the one it was
 * read from, where an UPDATE may have moved it to another partition -- and
 * the row as that relation has it (*scan), with its other version (*other)
 * where it came back with one.  The plan's row that asked is the outer
 * tuple.  False when there are no more.
 */
static bool
explicit_returned_row(ExplicitState *state, int *relidx,
					  TupleTableSlot **scan, TupleTableSlot **other)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	int			col = 0;

	if (!tuplestore_gettupleslot(state->returned, true, false, state->retslot))
		return false;
	slot_getallattrs(state->retslot);

	*relidx = 0;
	econtext->ecxt_outertuple = NULL;
	if (state->operation != CMD_INSERT)
	{
		int64		n = DatumGetInt64(state->retslot->tts_values[col++]);

		ExecStoreMinimalTuple(state->saved[n], state->outerslot, false);
		/* the projection takes the plan's slot to be deformed, as its own is */
		slot_getallattrs(state->outerslot);
		econtext->ecxt_outertuple = state->outerslot;

		if (state->nrels > 1)
		{
			bool		isnull;

			*relidx = result_rel_of(state,
									DatumGetObjectId(slot_getattr(state->outerslot,
																  state->tableoidcol,
																  &isnull)));
		}
	}

	*scan = explicit_returned_version(state, &col, state->rootslot,
									  state->relslots[*relidx], *relidx);
	*other = state->other
		? explicit_returned_version(state, &col, state->otherslot,
									state->orelslots[*relidx], *relidx)
		: NULL;
	return true;
}

/*
 * The next row RETURNING gives, from what the segments sent back: old and
 * new, where it reads them by name, as ExecProcessReturning() sets them --
 * a DELETE's row is its old one, an INSERT's or UPDATE's its new one, and
 * one that is not there is a row of nulls.
 */
static TupleTableSlot *
explicit_next_returning(ExplicitState *state)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	EState	   *estate = state->css.ss.ps.state;
	TupleTableSlot *scan;
	TupleTableSlot *other;
	TupleTableSlot *oldslot;
	TupleTableSlot *newslot;
	ProjectionInfo *proj;
	int			relidx;

	ResetExprContext(econtext);
	if (!explicit_returned_row(state, &relidx, &scan, &other))
		return NULL;
	proj = state->projs[relidx];

	oldslot = state->operation == CMD_DELETE ? scan : other;
	newslot = state->operation == CMD_DELETE ? NULL : scan;
	econtext->ecxt_scantuple = scan;
	econtext->ecxt_oldtuple = oldslot != NULL ? oldslot
		: (proj->pi_state.flags & EEO_FLAG_HAS_OLD)
		? ExecGetAllNullSlot(estate, &state->rris[relidx]) : NULL;
	econtext->ecxt_newtuple = newslot != NULL ? newslot
		: (proj->pi_state.flags & EEO_FLAG_HAS_NEW)
		? ExecGetAllNullSlot(estate, &state->rris[relidx]) : NULL;
	if (oldslot == NULL)
		proj->pi_state.flags |= EEO_FLAG_OLD_IS_NULL;
	else
		proj->pi_state.flags &= ~EEO_FLAG_OLD_IS_NULL;
	if (newslot == NULL)
		proj->pi_state.flags |= EEO_FLAG_NEW_IS_NULL;
	else
		proj->pi_state.flags &= ~EEO_FLAG_NEW_IS_NULL;

	return ExecProject(proj);
}

/*
 * The check options, over every row the segments wrote, before RETURNING
 * gives one: as ExecInsert() and ExecUpdate() check a row, a policy's
 * WITH CHECK first and a view's WITH CHECK OPTION after -- an upsert's
 * row that updated one it conflicted with as an UPDATE, the row it
 * conflicted with checked against the UPDATE's USING, as
 * ExecOnConflictUpdate() checks it; and a moved row as an UPDATE, as
 * ExecInsert() checks a row an UPDATE moves between partitions.
 */
static void
explicit_check(ExplicitState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	TupleTableSlot *scan;
	TupleTableSlot *other;
	int			relidx;

	while (explicit_returned_row(state, &relidx, &scan, &other))
	{
		ResultRelInfo *rri = &state->rris[relidx];

		if (rri->ri_WithCheckOptions != NIL && scan != NULL)
		{
			if (state->operation == CMD_INSERT && other == NULL)
				ExecWithCheckOptions(WCO_RLS_INSERT_CHECK, rri, scan, estate);
			else
			{
				if (other != NULL && state->operation == CMD_INSERT)
					ExecWithCheckOptions(WCO_RLS_CONFLICT_CHECK, rri, other,
										 estate);
				ExecWithCheckOptions(WCO_RLS_UPDATE_CHECK, rri, scan, estate);
			}
			ExecWithCheckOptions(WCO_VIEW_CHECK, rri, scan, estate);
		}
		ResetPerTupleExprContext(estate);
		ResetExprContext(state->css.ss.ps.ps_ExprContext);
	}
	tuplestore_rescan(state->returned);
}

static TupleTableSlot *
explicit_exec(CustomScanState *node)
{
	ExplicitState *state = (ExplicitState *) node;

	if (!state->done && state->merge)
	{
		explicit_collect_merge(state);
		explicit_send_merge(state);
		state->done = true;
	}
	if (!state->done)
	{
		explicit_collect(state);
		explicit_send(state);
		if (state->checks && state->returned != NULL)
			explicit_check(state);

		/* back only to be checked */
		if (!state->returning && state->returned != NULL)
		{
			tuplestore_end(state->returned);
			state->returned = NULL;
		}
		state->done = true;
	}

	if (state->returned == NULL)
		return NULL;
	return explicit_next_returning(state);
}

static void
explicit_end(CustomScanState *node)
{
	ExplicitState *state = (ExplicitState *) node;

	ExecEndNode(outerPlanState(node));
	if (state->returned != NULL)
		tuplestore_end(state->returned);

	/* a slot on a relation's descriptor holds a reference to it */
	if (state->back)
	{
		ExecDropSingleTupleTableSlot(state->retslot);
		ExecDropSingleTupleTableSlot(state->rootslot);
		if (state->otherslot != NULL)
			ExecDropSingleTupleTableSlot(state->otherslot);
		for (int i = 0; i < state->nrels; i++)
		{
			if (state->relslots[i] != NULL)
				ExecDropSingleTupleTableSlot(state->relslots[i]);
			if (state->orelslots[i] != NULL)
				ExecDropSingleTupleTableSlot(state->orelslots[i]);
		}
	}
	if (state->outerslot != NULL)
		ExecDropSingleTupleTableSlot(state->outerslot);
	if (state->merge)
	{
		ExecDropSingleTupleTableSlot(state->mrootslot);
		ExecDropSingleTupleTableSlot(state->minsslot);
		for (int i = 0; i < state->nrels; i++)
		{
			ExecDropSingleTupleTableSlot(state->moldslots[i]);
			ExecDropSingleTupleTableSlot(state->mnewslots[i]);
		}
	}
	if (state->target_opened)
		table_close(state->target, NoLock);
	for (int i = 0; i < state->nrels; i++)
		table_close(state->rels[i], NoLock);
}

static void
explicit_rescan(CustomScanState *node)
{
	elog(ERROR, "a write of a distributed table cannot be rescanned");
}

static void
explicit_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ExplicitState *state = (ExplicitState *) node;
	StringInfoData sql;
	ListCell   *lc;

	if (!es->verbose)
		return;

	/* a MERGE's: each kind of action's statement, in the order they are sent */
	if (state->merge)
	{
		List	   *shapes = list_copy(state->mshapes);
		int			nupdate = 0;

		if (state->mdelete != NULL)
			shapes = lcons(state->mdelete, shapes);
		if (state->minsert != NULL)
			shapes = lappend(shapes, state->minsert);
		foreach_ptr(MergeShape, shape, shapes)
		{
			initStringInfo(&sql);
			appendStringInfo(&sql, "%s(", shape->head);
			foreach(lc, shape->casts)
				appendStringInfo(&sql, "%s$%d::%s", foreach_current_index(lc) > 0 ? ", " : "",
								 foreach_current_index(lc) + 1, (char *) lfirst(lc));
			appendStringInfo(&sql, ")%s", shape->tail);
			ExplainPropertyText(shape->cmd == CMD_DELETE ? "Remote SQL (DELETE)"
								: shape->cmd == CMD_INSERT ? "Remote SQL (INSERT)"
								: psprintf("Remote SQL (UPDATE %d)", ++nupdate),
								sql.data, es);
		}
		if (state->split)
			ExplainPropertyText("Split", state->split_calls
								? "gp_internal.split_delete(), gp_internal.split_insert()"
								: "DELETE and INSERT", es);
		return;
	}

	/* the statement a segment is sent, with one row of VALUES */
	initStringInfo(&sql);
	appendStringInfo(&sql, "%s(", state->sql_head);
	foreach(lc, state->casts)
		appendStringInfo(&sql, "%s$%d::%s", foreach_current_index(lc) > 0 ? ", " : "",
						 foreach_current_index(lc) + 1, (char *) lfirst(lc));
	appendStringInfo(&sql, ")%s", state->sql_tail);
	ExplainPropertyText("Remote SQL", sql.data, es);
}

/* ------------------------------------------------------------------------- */
/* A plan ORCA made                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The explicit write over a plan ORCA made (gp_orca's merge.c): the rows come
 * through ORCA's Gather with each target row's segment and ctid, as columns
 * of ORCA's plan, not through a gather of gp_scan.c, which makes the ctid
 * the plan knows the row by as it reads it.  This node does the same over
 * ORCA's plan -- the row at "ctidcol" on the segment in "contentcol" made
 * the ctid the statement's map knows it by (GpRowIdentityMake()) -- and
 * passes on the other columns as they come, under the same names.
 */
typedef struct RowIdentityState
{
	CustomScanState css;
	AttrNumber	contentcol;
	AttrNumber	ctidcol;
} RowIdentityState;

static Node *row_identity_create_state(CustomScan *cscan);
static void row_identity_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *row_identity_exec(CustomScanState *node);
static void row_identity_end(CustomScanState *node);
static void row_identity_rescan(CustomScanState *node);

static const CustomScanMethods row_identity_scan_methods = {
	.CustomName = "Row Identity",
	.CreateCustomScanState = row_identity_create_state,
};

static const CustomExecMethods row_identity_exec_methods = {
	.CustomName = "Row Identity",
	.BeginCustomScan = row_identity_begin,
	.ExecCustomScan = row_identity_exec,
	.EndCustomScan = row_identity_end,
	.ReScanCustomScan = row_identity_rescan,
};

Plan *
GpRowIdentityNodeMake(Plan *child, AttrNumber contentcol, AttrNumber ctidcol)
{
	CustomScan *cscan = makeNode(CustomScan);
	List	   *scan_tlist = NIL;
	List	   *tlist = NIL;

	/* the child's row as the scan tuple, and the same again as the node's */
	foreach_node(TargetEntry, tle, child->targetlist)
	{
		Oid			type = exprType((Node *) tle->expr);
		int32		typmod = exprTypmod((Node *) tle->expr);
		Oid			collation = exprCollation((Node *) tle->expr);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
															  type, typmod,
															  collation, 0),
											 tle->resno, tle->resname,
											 tle->resjunk));
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(INDEX_VAR, tle->resno,
														 type, typmod,
														 collation, 0),
										tle->resno, tle->resname,
										tle->resjunk));
	}
	cscan->scan.plan.startup_cost = child->startup_cost;
	cscan->scan.plan.total_cost = child->total_cost;
	cscan->scan.plan.plan_rows = child->plan_rows;
	cscan->scan.plan.plan_width = child->plan_width;
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = list_make2(makeInteger(contentcol),
									   makeInteger(ctidcol));
	cscan->methods = &row_identity_scan_methods;
	return &cscan->scan.plan;
}

static Node *
row_identity_create_state(CustomScan *cscan)
{
	RowIdentityState *state = (RowIdentityState *) newNode(sizeof(RowIdentityState),
														   T_CustomScanState);

	state->css.methods = &row_identity_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

static void
row_identity_begin(CustomScanState *node, EState *estate, int eflags)
{
	RowIdentityState *state = (RowIdentityState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->contentcol = (AttrNumber) intVal(linitial(cscan->custom_private));
	state->ctidcol = (AttrNumber) intVal(lsecond(cscan->custom_private));
	outerPlanState(node) = ExecInitNode(outerPlan(cscan), estate, eflags);
}

static TupleTableSlot *
row_identity_exec(CustomScanState *node)
{
	RowIdentityState *state = (RowIdentityState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *child = ExecProcNode(outerPlanState(node));
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	int			natts = slot->tts_tupleDescriptor->natts;

	if (TupIsNull(child))
		return NULL;

	ResetExprContext(econtext);
	slot_getallattrs(child);
	ExecClearTuple(slot);
	memcpy(slot->tts_values, child->tts_values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, child->tts_isnull, natts * sizeof(bool));
	if (!slot->tts_isnull[state->ctidcol - 1])
	{
		ItemPointer synthetic;

		if (slot->tts_isnull[state->contentcol - 1])
			elog(ERROR, "a row ORCA's plan read came without its segment");
		synthetic = MemoryContextAlloc(econtext->ecxt_per_tuple_memory,
									   sizeof(ItemPointerData));
		GpRowIdentityMake(node->ss.ps.state,
						  DatumGetInt32(slot->tts_values[state->contentcol - 1]),
						  (ItemPointer) DatumGetPointer(slot->tts_values[state->ctidcol - 1]),
						  synthetic);
		slot->tts_values[state->ctidcol - 1] = PointerGetDatum(synthetic);
	}
	ExecStoreVirtualTuple(slot);
	if (node->ss.ps.ps_ProjInfo == NULL)
		return slot;
	econtext->ecxt_scantuple = slot;
	return ExecProject(node->ss.ps.ps_ProjInfo);
}

static void
row_identity_end(CustomScanState *node)
{
	ExecEndNode(outerPlanState(node));
}

static void
row_identity_rescan(CustomScanState *node)
{
	if (outerPlanState(node)->chgParam == NULL)
		ExecReScan(outerPlanState(node));
}

void
GpExplicitInit(void)
{
	RegisterCustomScanMethods(&explicit_scan_methods);
	RegisterCustomScanMethods(&row_identity_scan_methods);
}
