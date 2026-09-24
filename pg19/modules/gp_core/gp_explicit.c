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
 * row the plan finds more than once is changed once, as PostgreSQL's UPDATE
 * ... FROM changes it.  The rows are sent once the plan has finished with
 * the segments, as the routed INSERT's are, and an UPDATE or DELETE locks
 * the table as Cloudberry without its global deadlock detector locks it,
 * so that no row changes between being read and being written.
 *
 * An INSERT's rows go to the segment their key hashes to, every segment for
 * a replicated table.  An UPDATE that sets a column of the key moves each
 * row, as Cloudberry's Split Update does: deleted where it is, returning it,
 * and its new version inserted where it hashes.  RETURNING is evaluated
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
 * Refused, by name (GpExplicitCannot): an UPDATE of the key of a table with
 * UPDATE triggers, which a moved row would not fire, in Cloudberry's words;
 * statement-level triggers, which would fire on every segment; and MERGE.
 *
 * Cloudberry sources this file stands in for:
 *	  the Explicit Redistribute Motion cdbpath.c puts below a ModifyTable
 *	  whose rows came from elsewhere, and the segments' ModifyTable above it
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tupconvert.h"
#include "optimizer/optimizer.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
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
	 * A Split: an UPDATE of the distribution key.  Each row is deleted where
	 * it is, returning it, and its new version -- the old one with the SET
	 * columns' new values -- inserted where it hashes.
	 */
	bool		split;
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
} ExplicitState;

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
has_statement_triggers(Relation rel, CmdType operation)
{
	TriggerDesc *td = rel->trigdesc;

	if (td == NULL)
		return false;
	switch (operation)
	{
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

	if (mt->operation == CMD_MERGE)
		return "MERGE into a distributed table is not supported yet.";
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
		triggers = has_statement_triggers(rel, mt->operation);
		table_close(rel, NoLock);
		if (triggers)
			return psprintf("\"%s\" has statement-level triggers, which would fire on every segment.",
							get_rel_name(rte->relid));
	}

	if (mt->rootRelation != 0)
	{
		Oid			rootid = rt_fetch(mt->rootRelation, stmt->rtable)->relid;
		Relation	rel = table_open(rootid, NoLock);
		bool		triggers = has_statement_triggers(rel, mt->operation);

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
	 * Cloudberry's create_modifytable_path() asks it.  The DELETE and INSERT
	 * a moved row is made of fire their row triggers on the segments, as
	 * PostgreSQL fires them for a row moved between partitions; Cloudberry's
	 * Split fires none.
	 */
	policy = GpScanDistributedPolicy(rt_fetch(first, stmt->rtable)->relid);
	if (mt->operation == CMD_UPDATE && GpPolicyIsHashPartitioned(policy))
	{
		Oid			firstid = rt_fetch(first, stmt->rtable)->relid;

		foreach(lc, (List *) linitial(mt->updateColnosLists))
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
 * asked (with_n), the table the row is in, and the target's columns but the
 * dropped ones -- and, with_other, the table and the columns of the row's
 * other version, all null where it has none.
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
	desc = CreateTemplateTupleDesc((ncols + 1) * (with_other ? 2 : 1) +
								   (with_n ? 1 : 0));
	if (with_n)
		TupleDescInitEntry(desc, col++, "gp_n", INT8OID, -1, 0);
	for (int image = 0; image < (with_other ? 2 : 1); image++)
	{
		TupleDescInitEntry(desc, col++, "gp_toid", OIDOID, -1, 0);
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
	state->back = state->returning || view_checks ||
		(state->split && state->checks);
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

		appendStringInfo(&tail, " RETURNING %sgp_t.tableoid, gp_t.*%s",
						 state->operation != CMD_INSERT ? "gp_s.gp_n, " : "",
						 state->other ? ", old.tableoid, old.*" : "");
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

		initStringInfo(&dh);
		appendStringInfo(&dh, "DELETE FROM %s%s AS gp_t USING (VALUES ",
						 state->only ? "ONLY " : "",
						 GpDispatchRelationName(RelationGetRelid(state->target)));
		state->delete_head = dh.data;
		state->delete_tail = ") AS gp_s (gp_ctid, gp_toid, gp_n) WHERE gp_t.ctid = gp_s.gp_ctid AND gp_t.tableoid = gp_s.gp_toid RETURNING gp_s.gp_n, gp_t.tableoid, gp_t.*";
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
		state->insert_tail = state->back ? " RETURNING gp_t.tableoid, gp_t.*" : "";
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
	 * Cloudberry without its global deadlock detector: an UPDATE or DELETE
	 * of a distributed table locks the table, so that two of them never
	 * wait for each other on different segments -- and here, so that a row
	 * the plan read does not change before it is written.  A partitioned
	 * table's partitions it writes are locked as it is, as Cloudberry's
	 * planner locks them in the table's mode; an INSERT into one locks every
	 * partition.
	 */
	if ((state->operation == CMD_UPDATE || state->operation == CMD_DELETE ||
		 state->on_conflict == ONCONFLICT_UPDATE) &&
		!gp_enable_global_deadlock_detector)
	{
		LockRelationOid(RelationGetRelid(state->target), ExclusiveLock);
		for (i = 0; i < state->nrels; i++)
			if (state->rels[i] != state->target)
				LockRelationOid(RelationGetRelid(state->rels[i]), ExclusiveLock);
	}
	if (state->operation == CMD_INSERT)
		GpModifyLockPartitions(RelationGetRelid(state->target),
							   state->on_conflict == ONCONFLICT_UPDATE
							   ? ExclusiveLock : RowExclusiveLock);

	GpClusterSegments(&state->nsegs);
	state->batches = palloc0_array(List *, state->nsegs);
	state->rowcxt = AllocSetContextCreate(estate->es_query_cxt,
										  "gp explicit rows",
										  ALLOCSET_DEFAULT_SIZES);
	GpReportDispatch(0, false, state->numsegments);
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

/*
 * One segment's rows, a statement a batch; how many it wrote.  "store" takes
 * what RETURNING gave, where it is to be kept.
 */
static uint64
explicit_send_statements(int content, List *rows, int nparams,
						 const char *head, const char *tail, List *casts,
						 TupleDesc desc, Tuplestorestate *store)
{
	int			per = Min(EXPLICIT_BATCH_ROWS, EXPLICIT_MAX_PARAMS / Max(nparams, 1));
	uint64		total = 0;
	ListCell   *lc = list_head(rows);

	while (lc != NULL)
	{
		StringInfoData sql;
		const char **values;
		int			nrows = 0;
		int			n = 0;

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

		total += GpDispatchWriteOnContent(content, sql.data, n, values,
										  desc, store);
		pfree(sql.data);
		pfree(values);
	}
	return total;
}

static uint64
explicit_send_rows(ExplicitState *state, int content, List *rows,
				   Tuplestorestate *store)
{
	return explicit_send_statements(content, rows,
									state->nvals + (state->operation != CMD_INSERT ? 3 : 0),
									state->sql_head, state->sql_tail,
									state->casts, state->retdesc, store);
}

/*
 * A Split: every row deleted where it is, returning it; its new version, the
 * old with the SET columns' new values, hashed by its key and inserted
 * where that says; and what the INSERTs return, each with the number of the
 * plan's row, for RETURNING.  The rows it moved are the rows it deleted.
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
	List	  **olders = palloc0_array(List *, state->nsegs);
	uint64		deleted = 0;
	MemoryContext oldcxt;

	for (int seg = 0; seg < state->nsegs; seg++)
		if (state->batches[seg] != NIL)
			deleted += explicit_send_statements(seg, state->batches[seg], 3,
												state->delete_head,
												state->delete_tail,
												list_copy_head(state->casts, 3),
												state->olddesc, olds);

	oldcxt = MemoryContextSwitchTo(state->rowcxt);
	while (tuplestore_gettupleslot(olds, true, false, oldslot))
	{
		int64		n;
		int			col = 2;
		int			seg;
		const char **params;

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

		/* the SET columns' new values, from the plan's row that asked */
		ExecStoreMinimalTuple(state->saved[n], state->outerslot, false);
		for (int k = 0; k < state->nvals; k++)
			values[state->setattnos[k] - 1] =
				slot_getattr(state->outerslot, state->valcols[k],
							 &nulls[state->setattnos[k] - 1]);

		seg = GpHashSegment(state->hash, values, nulls);
		params = palloc_array(const char *, Max(state->ninsert, 1));
		for (int i = 0; i < state->ninsert; i++)
		{
			AttrNumber	a = state->insattnos[i] - 1;

			params[i] = nulls[a] ? NULL
				: OutputFunctionCall(&state->insout[i], values[a]);
		}
		inserts[seg] = lappend(inserts[seg], params);
		numbers[seg] = lappend(numbers[seg], makeInteger((int) n));
		/* the deleted row, RETURNING's old one */
		if (state->other)
			olders[seg] = lappend(olders[seg], ExecCopySlotMinimalTuple(oldslot));
	}
	MemoryContextSwitchTo(oldcxt);
	tuplestore_end(olds);

	for (int seg = 0; seg < state->nsegs; seg++)
	{
		Tuplestorestate *news;
		TupleTableSlot *newslot;
		ListCell   *ln;

		if (inserts[seg] == NIL)
			continue;
		if (!state->back)
		{
			(void) explicit_send_statements(seg, inserts[seg], state->ninsert,
											state->insert_head,
											state->insert_tail,
											state->insert_casts, NULL, NULL);
			continue;
		}

		/* an INSERT returns its rows in the order it was given them */
		news = tuplestore_begin_heap(false, false, work_mem);
		(void) explicit_send_statements(seg, inserts[seg], state->ninsert,
										state->insert_head, state->insert_tail,
										state->insert_casts, state->newdesc,
										news);
		newslot = MakeSingleTupleTableSlot(state->newdesc, &TTSOpsMinimalTuple);
		ln = list_head(numbers[seg]);
		while (ln != NULL && tuplestore_gettupleslot(news, true, false, newslot))
		{
			Datum	   *rv = palloc_array(Datum, state->retdesc->natts);
			bool	   *rn = palloc_array(bool, state->retdesc->natts);
			int			width = newslot->tts_tupleDescriptor->natts;

			slot_getallattrs(newslot);
			rv[0] = Int64GetDatum((int64) intVal(lfirst(ln)));
			rn[0] = false;
			memcpy(&rv[1], newslot->tts_values, width * sizeof(Datum));
			memcpy(&rn[1], newslot->tts_isnull, width * sizeof(bool));
			if (state->other)
			{
				/* the old row after the new: its table and columns */
				ExecStoreMinimalTuple((MinimalTuple) list_nth(olders[seg],
															  list_cell_number(numbers[seg], ln)),
									  oldslot, false);
				slot_getallattrs(oldslot);
				memcpy(&rv[1 + width], &oldslot->tts_values[1], width * sizeof(Datum));
				memcpy(&rn[1 + width], &oldslot->tts_isnull[1], width * sizeof(bool));
			}
			tuplestore_putvalues(state->returned, state->retdesc, rv, rn);
			ln = lnext(numbers[seg], ln);
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

	if (state->back)
		state->returned = tuplestore_begin_heap(false, false, work_mem);

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
			rows = explicit_rows_by_content(state, from, state->batches[from]);
			for (int seg = 0; seg < Min(state->nsegs, state->numsegments); seg++)
			{
				uint64		n = explicit_send_rows(state, seg, rows,
												   seg == 0 ? state->returned : NULL);

				if (seg == 0)
					total += n;
			}
		}
	}
	else
		for (int seg = 0; seg < state->nsegs; seg++)
			if (state->batches[seg] != NIL)
				total += explicit_send_rows(state, seg, state->batches[seg],
											state->returned);

	/*
	 * a replicated table's rows are written on every segment of it -- a
	 * partial table's first so many -- and counted once
	 */
	if (state->everywhere != NIL)
		for (int seg = 0; seg < Min(state->nsegs, state->numsegments); seg++)
		{
			uint64		n = explicit_send_rows(state, seg, state->everywhere,
											   seg == 0 ? state->returned : NULL);

			if (seg == 0)
				total += n;
		}

	if (state->canSetTag)
		estate->es_processed += total;
}

/*
 * A version of a row the segments sent back, from column *col of it on: its
 * table, then its columns, as the root has them, in "rootslot"; and as the
 * result relation relidx has them, where that is not the root.  NULL where
 * the row has no such version, its table null.
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

	/* the statement a segment is sent, with one row of VALUES */
	initStringInfo(&sql);
	appendStringInfo(&sql, "%s(", state->sql_head);
	foreach(lc, state->casts)
		appendStringInfo(&sql, "%s$%d::%s", foreach_current_index(lc) > 0 ? ", " : "",
						 foreach_current_index(lc) + 1, (char *) lfirst(lc));
	appendStringInfo(&sql, ")%s", state->sql_tail);
	ExplainPropertyText("Remote SQL", sql.data, es);
}

void
GpExplicitInit(void)
{
	RegisterCustomScanMethods(&explicit_scan_methods);
}
