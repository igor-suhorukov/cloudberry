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
 * gp_scan.c
 *	  Reading a distributed table: the gather-all shape.
 *
 * A distributed table's rows are on the segments, and its copy on the
 * coordinator is empty.  When PostgreSQL's planner plans a query -- ORCA
 * declined it, or is off -- every scan of such a table becomes a Gather
 * Motion of the table from the segments: the plan the planner made runs on
 * the coordinator, over the rows gathered.  That is the gather-all fallback of
 * Route A in the plan (Track A §2.2, gpdb_hook.md): correct for every query
 * PostgreSQL can plan, and as slow as moving every row to one node is.  ORCA's
 * own plans, with Motions where it chooses, are the fast path, and this is
 * what a query gets when it is not on it.
 *
 * What it does not move: the columns the plan does not use; and the rows the
 * plan would throw away, when the condition that throws them away can be
 * evaluated on a segment -- one of this table's columns, its system columns,
 * constants, and nothing whose answer could differ there: no parameter, no
 * subquery, no function that is not immutable.  Such a condition is sent as
 * SQL, deparsed by PostgreSQL's own ruleutils.
 *
 * What a row brings with it besides its columns.  The node's scan tuple is
 * its own (custom_scan_tlist), not the table's: the columns the plan uses,
 * and of the table's system columns the ones it names -- ctid, xmin, xmax,
 * cmin and cmax as the segment that holds the row has them, as Cloudberry's
 * QE sends them up, and tableoid; the whole row, where the plan reads it;
 * and gp_segment_id, the segment the row came from, which is how a randomly
 * distributed table's is known here at all (gp_segment.c).  The row's ctid
 * is its scan tuple's TID too, which is what WHERE CURRENT OF reads.
 *
 * And the segments it does not ask: a hash-distributed table whose
 * conditions fix every column of its key to constants -- one each, or a few
 * through IN lists and ORs -- has all the rows they can match on the
 * segments those constants hash to, which are the only ones asked:
 * Cloudberry's direct dispatch, "PARTIAL contents" when several.  So do
 * conditions on gp_segment_id.  A replicated table's rows are all on every
 * segment, so one is asked, a different one per session.  A partial table's
 * rows are on the first so many segments its policy names, and only those
 * are asked.
 *
 * WHERE CURRENT OF a cursor, whose plan gathers the table: the cursor's
 * gather says which segment its current row came from and where it is
 * there, and the one row is read from that segment (gather_current_of()),
 * as Cloudberry's QD sends the cursor's position to the QEs -- in the version
 * the statement's snapshot sees, an update since the cursor read it followed
 * (gp_current_tid()), as PostgreSQL's TID scan follows it.
 *
 * Cloudberry sources this file stands in for:
 *	  the Gather Motion over a scan that cdbllize.c and cdbpath.c put above a
 *	  distributed table, cdbtargeteddispatch.c and predtest_valueset.c, and
 *	  the position of a cursor's row that execCurrent.c reads
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_explain.h"
#include "gp_gdd.h"
#include "gp_hash.h"
#include "gp_motion.h"
#include "gp_policy.h"
#include "gp_scan.h"
#include "gp_segment.h"
#include "gp_settings.h"

/* What starting a gather costs before its first row: a round trip per segment. */
#define GATHER_STARTUP_COST		1000.0
/*
 * And per row, moving it: a send, a receive, and the conversion between --
 * unless gp.motion_cost_per_row says otherwise, as Cloudberry's planner lets
 * it say of a Motion.
 */
#define GATHER_ROW_COST \
	(gp_motion_cost_per_row > 0 ? gp_motion_cost_per_row : 10.0 * DEFAULT_CPU_TUPLE_COST)

/*
 * Where a column of a gather's scan tuple comes from: a column of what the
 * segments send (0 and up), or one of these, made here.
 */
#define GATHER_SRC_TABLEOID		(-1)	/* the relation's OID */
#define GATHER_SRC_WHOLEROW		(-2)	/* the row, as its table's row type */
#define GATHER_SRC_SEGMENT		(-3)	/* the segment it came from */
#define GATHER_SRC_IDENTITY		(-4)	/* its ctid as a write's plan knows it */

/* custom_private, in order */
#define GATHER_PRIVATE_SELECT		0	/* "SELECT ... FROM ONLY t" */
#define GATHER_PRIVATE_WHERE		1	/* the conditions sent, ANDed, or "" */
#define GATHER_PRIVATE_LOCKING		2	/* " FOR UPDATE ...", or "" */
#define GATHER_PRIVATE_CONTENTS		3	/* direct dispatch's segments, or NIL */
#define GATHER_PRIVATE_NSEGMENTS	4	/* the table's segments: 0 for every one */
#define GATHER_PRIVATE_TYPES		5	/* what the segments send: each type */
#define GATHER_PRIVATE_TYPMODS		6	/* and typmod */
#define GATHER_PRIVATE_SOURCES		7	/* each scan column's source */
#define GATHER_PRIVATE_ATTRS		8	/* each attribute's column sent, or -1 */
#define GATHER_PRIVATE_CTID			9	/* the column sent that is ctid, or -1 */
#define GATHER_PRIVATE_CURSOR		10	/* WHERE CURRENT OF: its cursor, or "" */
#define GATHER_PRIVATE_CURSOR_PARAM 11	/* or the parameter naming it, or 0 */
#define GATHER_PRIVATE_IDENTITY		12	/* the rows of a table being changed */
#define GATHER_PRIVATE_LIMIT		13	/* " LIMIT n" a LIMIT above sends, or "" */
#define GATHER_PRIVATE_FOLDED		14	/* the conditions sent, their stable
										 * constant parts as $N, or "" */
#define GATHER_PRIVATE_KEYED		15	/* its rows kept by their key, for
										 * each run of a nested loop's */

/*
 * The first $N a condition's stable constant part is sent as, until the
 * coordinator has computed it as the gather starts (fold_stable()).
 */
#define GATHER_FOLD_PARAM			90001

static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static build_simple_rel_hook_type prev_build_simple_rel = NULL;

static Plan *gather_plan(PlannerInfo *root, RelOptInfo *rel,
						 CustomPath *best_path, List *tlist,
						 List *clauses, List *custom_plans);
static Node *gather_create_state(CustomScan *cscan);
static void gather_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *gather_exec(CustomScanState *node);
static void gather_end(CustomScanState *node);
static void gather_rescan(CustomScanState *node);
static void gather_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomPathMethods gather_path_methods = {
	.CustomName = "Gather Motion",
	.PlanCustomPath = gather_plan,
};

static const CustomScanMethods gather_scan_methods = {
	.CustomName = "Gather Motion",
	.CreateCustomScanState = gather_create_state,
};

static const CustomExecMethods gather_exec_methods = {
	.CustomName = "Gather Motion",
	.BeginCustomScan = gather_begin,
	.ExecCustomScan = gather_exec,
	.EndCustomScan = gather_end,
	.ReScanCustomScan = gather_rescan,
	.ExplainCustomScan = gather_explain,
};

typedef struct GatherScanState
{
	CustomScanState css;
	char	   *select;			/* "SELECT ... FROM ONLY t" */
	char	   *where;			/* the conditions sent, or "" */
	char	   *folded;			/* and with their stable constant parts as
								 * $N, or "" */
	List	   *folds;			/* those parts, each an ExprState */
	char	   *locking;		/* " FOR UPDATE ...", or "" */
	int			ncontents;		/* direct dispatch's segments, or 0 */
	int		   *contents;
	int			nsegments;		/* the table's: every one, or a partial
								 * table's first so many */
	TupleTableSlot *remote;		/* a row as it arrives */
	int			nsources;		/* the scan tuple's columns, and each one's */
	int		   *sources;		/* source: a column of the remote row, or
								 * GATHER_SRC_* */
	int			natts;			/* the relation's attributes, and each one's */
	int		   *attrs;			/* column in the remote row, or -1 */
	int			ctid_remote;	/* the remote row's ctid, or -1 */
	bool		whole;			/* read to its end (GpGatherScanMarkWhole()) */
	char	   *cursor_name;	/* WHERE CURRENT OF this cursor */
	int			cursor_param;	/* or the one this parameter names */
	bool		identity;		/* the rows of a table being changed */
	Datum	   *rowvalues;		/* the whole row's values, being built */
	bool	   *rownulls;
	char	   *limit;			/* " LIMIT n", or "" */
	GpGatherState *gather;
	bool		done;
	int			current_content;	/* the segment of the scan tuple's row */
	Tuplestorestate *spool;		/* what it read, when it may be read again */
	TupleTableSlot *spooled;	/* a row of it, read back */
	int			slice;			/* its slice, as the executor met it */

	/* A recheck's (gather_epq()) */
	AttrNumber	epq_segcol;		/* the segment of a row mark's row, in the
								 * plan's row: 0 not looked for, -1 none */
	TupleTableSlot *epq_row;	/* a row mark's copy of the row */

	/* A keyed gather's rows, kept by their key (keyed_init()), or NULL */
	struct KeyedGather *keyed;
} GatherScanState;

static void keyed_init(GatherScanState *state, CustomScan *cscan,
					   EState *estate);

/*
 * The planning in progress is a cursor's (see GpScanSetCursor()): its
 * gathers bring each row's ctid, for WHERE CURRENT OF to find it by.
 */
static bool planning_cursor = false;

/* ------------------------------------------------------------------------- */
/* Which relations                                                           */
/* ------------------------------------------------------------------------- */

GpPolicy *
GpScanDistributedPolicy(Oid relid)
{
	GpPolicy   *policy;

	if (GpClusterIsSingleNode())
		return NULL;

	/*
	 * A query inside a statement the coordinator dispatches whole reads and
	 * writes the coordinator's copy: each segment runs it on its own
	 * (GpDispatchIsRecording()).
	 */
	if (GpDispatchIsRecording())
		return NULL;
	if (get_rel_relkind(relid) != RELKIND_RELATION &&
		get_rel_relkind(relid) != RELKIND_PARTITIONED_TABLE &&
		get_rel_relkind(relid) != RELKIND_MATVIEW &&
		get_rel_relkind(relid) != RELKIND_FOREIGN_TABLE)
		return NULL;

	policy = GpPolicyGet(relid);
	if (policy == NULL || GpPolicyIsEntry(policy))
		return NULL;
	return policy;
}

int
GpScanReplicatedContent(const GpPolicy *policy)
{
	/*
	 * Every segment of the policy has every row; spread the sessions over
	 * them.  A partial table's are the first numsegments.
	 */
	return MyProcPid % policy->numsegments;
}

/* ------------------------------------------------------------------------- */
/* What can be sent                                                          */
/* ------------------------------------------------------------------------- */

typedef struct ShippableContext
{
	Index		relid;
} ShippableContext;

static bool
shippable_walker(Node *node, ShippableContext *cxt)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				/*
				 * This table's own columns and its system columns, which a
				 * segment has as the row it sends up has them; not the whole
				 * row, which ruleutils would name by a name the segment's
				 * statement does not give it.
				 */
				if (var->varno != cxt->relid || var->varlevelsup != 0 ||
					var->varattno == 0)
					return true;
				return false;
			}
		case T_Const:

			/*
			 * A value of an anonymous record type, which no literal can
			 * name: its text reads back nowhere ("input of anonymous
			 * composite types is not implemented") -- a PL/pgSQL record's
			 * value in a custom plan.
			 */
			return ((Const *) node)->consttype == RECORDOID ||
				((Const *) node)->consttype == RECORDARRAYOID;
		case T_Param:
		case T_SubLink:
		case T_SubPlan:
		case T_AlternativeSubPlan:
		case T_Aggref:
		case T_WindowFunc:
		case T_GroupingFunc:
		case T_PlaceHolderVar:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
			return true;
		default:
			break;
	}

	return expression_tree_walker(node, shippable_walker, cxt);
}

/*
 * gp_segment_id of this table, taken out of a condition before it is judged:
 * it differs between nodes, but a segment's answer is the right one -- the
 * segment that holds the row -- and a random table's is the only one.
 */
static Node *
without_segment_id(Node *node, Index *relid)
{
	if (node == NULL)
		return NULL;
	if (GpSegmentIsSegmentOf(node, *relid))
		return (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
								  Int32GetDatum(0), false, true);
	return expression_tree_mutator(node, without_segment_id, relid);
}

/*
 * The built-in STABLE functions a segment does not evaluate for the
 * coordinator, by name in pg_catalog: those that read what a node has of its
 * own -- its backends and their addresses, its transaction IDs -- those that
 * ask who the session is or what it may do, which a segment answers for the
 * gang's connection, those that look an object up by its name or name one by
 * its OID, the catalog's own, and current_setting(), which reads any setting,
 * sent or not.  Any "pg_" function is one of these, for its statistics,
 * files, backends or temporary schema, but for shipped_pg_names; and so is
 * any "has_", "reg" or "to_reg" one, and the planner's estimators.
 */
static const char *const unshipped_stable_names[] = {
	"inet_client_addr", "inet_client_port", "inet_server_addr",
	"inet_server_port", "txid_current", "txid_current_if_assigned",
	"txid_current_snapshot", "mxid_age", "current_user", "session_user",
	"system_user", "getpgusername", "current_setting", "current_schema",
	"current_schemas", "row_security_active", "format_type", "oidvectortypes",
	"col_description", "obj_description", "shobj_description",
	"_pg_index_position", "aclexplode", "aclitemin", "aclitemout",
	"enum_first", "enum_last", "enum_range", "ts_debug", "ts_parse",
	"ts_token_type", "table_to_xml", "table_to_xmlschema",
	"table_to_xml_and_xmlschema", "schema_to_xml", "schema_to_xmlschema",
	"schema_to_xml_and_xmlschema", "database_to_xml", "database_to_xmlschema",
	"database_to_xml_and_xmlschema",
};

static const char *const shipped_pg_names[] = {
	"pg_input_is_valid", "pg_input_error_info", "pg_char_to_encoding",
	"pg_encoding_to_char", "pg_column_size", "pg_options_to_table",
	"pg_get_keywords", "pg_timezone_names", "pg_timezone_abbrevs_abbrevs",
	"pg_timezone_abbrevs_zone",
};

static bool
name_in(const char *name, const char *const *names, int n)
{
	for (int i = 0; i < n; i++)
		if (strcmp(name, names[i]) == 0)
			return true;
	return false;
}

static bool
name_ends(const char *name, const char *suffix)
{
	size_t		n = strlen(name);
	size_t		m = strlen(suffix);

	return n >= m && strcmp(name + n - m, suffix) == 0;
}

/*
 * May a segment evaluate this built-in STABLE function for the coordinator?
 * Yes where it is stable only for the settings a gang is sent with each
 * statement -- DateStyle, IntervalStyle, TimeZone, lc_monetary, lc_numeric,
 * lc_time, search_path, default_text_search_config, extra_float_digits,
 * bytea_output, xmloption (gp_dispatch.c's synced_settings): a date compared
 * with a time with a zone, a cast to money, to_char(), to_tsvector(), JSON's
 * and XML's output -- or for the transaction's and the statement's start,
 * now() and its kin and age() of one timestamp, which are the coordinator's
 * where the gather's statement brings them ("times").  A user's STABLE
 * function may read a table, which a segment has its own part of, and is
 * not sent.
 */
static bool
stable_is_shipped(Oid funcid, bool times)
{
	HeapTuple	tp;
	Form_pg_proc proc;
	const char *name;
	bool		shipped;

	if (funcid >= FirstNormalObjectId)
		return false;
	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		return false;
	proc = (Form_pg_proc) GETSTRUCT(tp);
	name = NameStr(proc->proname);

	if (proc->pronamespace != PG_CATALOG_NAMESPACE)
		shipped = false;
	else if (strncmp(name, "pg_", 3) == 0)
		shipped = name_in(name, shipped_pg_names, lengthof(shipped_pg_names));
	else if (strncmp(name, "has_", 4) == 0 || strncmp(name, "reg", 3) == 0 ||
			 strncmp(name, "to_reg", 6) == 0 || strncmp(name, "fmgr_", 5) == 0 ||
			 name_ends(name, "sel") || name_ends(name, "_typanalyze"))
		shipped = false;
	else
		shipped = !name_in(name, unshipped_stable_names,
						   lengthof(unshipped_stable_names));

	/* age() of a transaction ID is this node's; of one timestamp, the day's */
	if (shipped && strcmp(name, "age") == 0)
		shipped = proc->pronargs > 0 && proc->proargtypes.values[0] != XIDOID &&
			(proc->pronargs > 1 || times);
	if (shipped && !times &&
		(strcmp(name, "now") == 0 || strcmp(name, "transaction_timestamp") == 0 ||
		 strcmp(name, "statement_timestamp") == 0))
		shipped = false;

	ReleaseSysCache(tp);
	return shipped;
}

static bool
unshippable_function(Oid funcid, void *context)
{
	switch (func_volatile(funcid))
	{
		case PROVOLATILE_IMMUTABLE:
			return false;
		case PROVOLATILE_STABLE:
			return !stable_is_shipped(funcid, *(bool *) context);
		default:
			return true;
	}
}

/*
 * contain_mutable_functions(), less what a segment may evaluate for the
 * coordinator (stable_is_shipped()): CURRENT_DATE and the rest of the SQL
 * value functions of time where the gather brings the coordinator's times,
 * none of those that name the session's user, database or schema.  JSON's
 * constructors and expressions are stable for the dates and times they
 * write, as TimeZone and DateStyle, which are sent, say.
 */
static bool
contain_unshippable_functions(Node *node, void *context)
{
	bool		times = *(bool *) context;

	if (node == NULL)
		return false;
	if (check_functions_in_node(node, unshippable_function, context))
		return true;
	if (IsA(node, SQLValueFunction))
	{
		switch (((SQLValueFunction *) node)->op)
		{
			case SVFOP_CURRENT_DATE:
			case SVFOP_CURRENT_TIME:
			case SVFOP_CURRENT_TIME_N:
			case SVFOP_CURRENT_TIMESTAMP:
			case SVFOP_CURRENT_TIMESTAMP_N:
			case SVFOP_LOCALTIME:
			case SVFOP_LOCALTIME_N:
			case SVFOP_LOCALTIMESTAMP:
			case SVFOP_LOCALTIMESTAMP_N:
				if (!times)
					return true;
				break;
			default:
				return true;
		}
	}
	if (IsA(node, NextValueExpr) || IsA(node, Query))
		return true;
	return expression_tree_walker(node, contain_unshippable_functions, context);
}

/*
 * Can this expression be computed on its own, before a row is read: no
 * column, parameter, subquery or aggregate in it, nothing a node above it
 * gives it (CASE's value, a domain's), and no volatile function?
 */
static bool
stands_alone_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Param:
		case T_PlaceHolderVar:
		case T_CaseTestExpr:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_Aggref:
		case T_WindowFunc:
		case T_GroupingFunc:
		case T_SubLink:
		case T_SubPlan:
		case T_AlternativeSubPlan:
			return true;
		default:
			break;
	}
	return expression_tree_walker(node, stands_alone_walker, context);
}

/*
 * A condition's stable constant parts -- a STABLE function, or CURRENT_DATE
 * and its kin, over constants alone -- each put in the condition as a
 * parameter, $N from GATHER_FOLD_PARAM, and kept in *folds: the coordinator
 * computes them as the gather starts, and sends their values, as
 * Cloudberry's coordinator computes a plan's stable functions before it
 * dispatches it (exec_make_plan_constant()).  So a segment compares a
 * column with a constant, which a table access method's statistics can
 * skip files and groups for -- PAX's cannot for a cast of a constant -- and
 * evaluates the function once, not for each row.
 */
static bool
fold_candidate(Node *node)
{
	switch (nodeTag(node))
	{
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		case T_ScalarArrayOpExpr:
		case T_BoolExpr:
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:
		case T_RelabelType:
		case T_CaseExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_ArrayExpr:
		case T_NullTest:
		case T_BooleanTest:
			return true;
		default:
			return false;
	}
}

static Node *
fold_stable(Node *node, List **folds)
{
	if (node == NULL)
		return NULL;
	if (fold_candidate(node) && exprType(node) != RECORDOID &&
		exprType(node) != RECORDARRAYOID &&
		contain_mutable_functions(node) && !contain_volatile_functions(node) &&
		!stands_alone_walker(node, NULL))
	{
		Param	   *param = makeNode(Param);

		param->paramkind = PARAM_EXTERN;
		param->paramid = GATHER_FOLD_PARAM + list_length(*folds);
		param->paramtype = exprType(node);
		param->paramtypmod = exprTypmod(node);
		param->paramcollid = exprCollation(node);
		param->location = -1;
		*folds = lappend(*folds, copyObject(node));
		return (Node *) param;
	}
	return expression_tree_mutator(node, fold_stable, folds);
}

/*
 * Can this condition be evaluated on a segment and mean the same there?
 * Nothing whose answer could differ between nodes: only this table's columns,
 * no parameter or subquery, and no function that is not immutable -- but the
 * built-in STABLE ones a segment evaluates as the coordinator would, with the
 * settings each statement is sent and, where the cluster has its secret, the
 * coordinator's times (stable_is_shipped()), as Cloudberry's segments
 * evaluate a scan's stable conditions.  gp_segment_id is the exception.
 */
static bool
is_shippable(Expr *expr, Index relid)
{
	ShippableContext cxt = {.relid = relid};
	bool		times = GpClusterHasSecret();

	expr = (Expr *) without_segment_id((Node *) expr, &relid);
	if (contain_unshippable_functions((Node *) expr, &times))
		return false;
	return !shippable_walker((Node *) expr, &cxt);
}

/* ------------------------------------------------------------------------- */
/* The values a column can have: Cloudberry's PossibleValueSet               */
/* ------------------------------------------------------------------------- */

/*
 * What the conditions allow a column -- one of the key's, or gp_segment_id --
 * to be: any value, or only these constants.  Kept as Cloudberry keeps them,
 * in a hash table of the constants' bytes (predtest_valueset.c), so that the
 * segments they hash to come out in its order, which is the order its INFO
 * line names them in.
 */
typedef struct ValueSet
{
	bool		any;			/* any value is possible */
	HTAB	   *set;			/* else these, of ValueEntry */
} ValueSet;

typedef struct ValueEntry
{
	Const	   *c;
} ValueEntry;

/* The column: a key column's Var, or else gp_segment_id of varno's row. */
typedef struct ValueTarget
{
	AttrNumber	attno;			/* 0 for gp_segment_id */
	Index		varno;
	Oid			opfamily;		/* the hash family whose equality counts */
} ValueTarget;

/* The size of an array Cloudberry takes apart, as PostgreSQL's predtest.c */
#define VALUESET_MAX_ARRAY	100

static uint32
value_hash(const void *key, Size keysize)
{
	const Const *c = *((Const *const *) key);

	if (c->constisnull)
		return 0;
	if (c->constbyval)
		return hash_any((const unsigned char *) &c->constvalue, sizeof(Datum));
	return hash_any((const unsigned char *) DatumGetPointer(c->constvalue),
					datumGetSize(c->constvalue, c->constbyval, c->constlen));
}

static int
value_match(const void *key1, const void *key2, Size keysize)
{
	return equal(*((Node *const *) key1), *((Node *const *) key2)) ? 0 : 1;
}

static void
valueset_add(ValueSet *vs, Const *c)
{
	bool		found;
	ValueEntry *entry;

	if (vs->set == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Const *);
		ctl.entrysize = sizeof(ValueEntry);
		ctl.hash = value_hash;
		ctl.match = value_match;
		ctl.hcxt = CurrentMemoryContext;
		vs->set = hash_create("gp direct dispatch values", 16, &ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
							  HASH_CONTEXT);
	}
	vs->any = false;
	(void) hash_search(vs->set, &c, HASH_FIND, &found);
	if (!found)
	{
		Const	   *copy = copyObject(c);

		entry = hash_search(vs->set, &copy, HASH_ENTER, &found);
		entry->c = copy;
	}
}

static bool
valueset_contains(ValueSet *vs, Const *c)
{
	bool		found = false;

	if (vs->set != NULL)
		(void) hash_search(vs->set, &c, HASH_FIND, &found);
	return found;
}

/* The constants, in the hash table's order. */
static List *
valueset_values(ValueSet *vs)
{
	HASH_SEQ_STATUS status;
	ValueEntry *entry;
	List	   *result = NIL;

	if (vs->set == NULL)
		return NIL;
	hash_seq_init(&status, vs->set);
	while ((entry = (ValueEntry *) hash_seq_search(&status)) != NULL)
		result = lappend(result, entry->c);
	return result;
}

/* Is this the column the set is about? */
static bool
value_target_is(Node *node, ValueTarget *target)
{
	if (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (target->attno == 0)
		return GpSegmentIsSegmentOf(node, target->varno);
	return node != NULL && IsA(node, Var) &&
		((Var *) node)->varno == target->varno &&
		((Var *) node)->varattno == target->attno &&
		((Var *) node)->varlevelsup == 0;
}

/* An equality of the column and a constant: the constant it may be. */
static bool
value_atom(Node *clause, ValueTarget *target, ValueSet *result)
{
	result->any = true;
	result->set = NULL;

	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *left = strip_implicit_coercions(linitial(op->args));
		Node	   *right = strip_implicit_coercions(lsecond(op->args));
		Node	   *column;
		Const	   *con;

		if (IsA(right, Const))
		{
			column = left;
			con = (Const *) right;
		}
		else if (IsA(left, Const))
		{
			column = right;
			con = (Const *) left;
		}
		else
			return false;
		if (con->constisnull || !value_target_is(column, target))
			return false;

		/*
		 * An equality the key's hash family hashes for: a constant of another
		 * of its types hashes with its own type's function as the column's
		 * value it equals would (GpHashSegmentForKey).
		 */
		if (get_op_opfamily_strategy(op->opno, target->opfamily) !=
			HTEqualStrategyNumber)
			return false;
		valueset_add(result, con);
		return true;
	}

	if (IsA(clause, NullTest) &&
		((NullTest *) clause)->nulltesttype == IS_NULL &&
		!((NullTest *) clause)->argisrow &&
		value_target_is((Node *) ((NullTest *) clause)->arg, target))
	{
		Node	   *arg = (Node *) ((NullTest *) clause)->arg;

		valueset_add(result, makeNullConst(exprType(arg), -1,
										   exprCollation(arg)));
		return true;
	}

	return false;
}

static ValueSet possible_values(Node *clause, ValueTarget *target);

/* AND: each member narrows the set; OR: each widens it. */
static ValueSet
possible_values_of(List *args, bool and, ValueTarget *target)
{
	ValueSet	result = {.any = true,.set = NULL};

	foreach_ptr(Node, arg, args)
	{
		ValueSet	child = possible_values(arg, target);

		if (and)
		{
			if (child.any)
				continue;
			if (result.any)
			{
				result = child;
				continue;
			}
			/* the intersection */
			foreach_ptr(Const, c, valueset_values(&result))
				if (!valueset_contains(&child, c))
					(void) hash_search(result.set, &c, HASH_REMOVE, NULL);
		}
		else
		{
			if (child.any)
				return child;	/* any value, whatever the rest say */
			if (result.any)
			{
				result = child;
				continue;
			}
			/* the union */
			foreach_ptr(Const, c, valueset_values(&child))
				valueset_add(&result, c);
		}
	}
	return result;
}

/* A ScalarArrayOpExpr, taken apart into its elements' comparisons. */
static ValueSet
possible_values_of_array(ScalarArrayOpExpr *saop, ValueTarget *target)
{
	ValueSet	any = {.any = true,.set = NULL};
	Node	   *array = (Node *) lsecond(saop->args);
	List	   *comparisons = NIL;

	if (IsA(array, Const) && !((Const *) array)->constisnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(((Const *) array)->constvalue);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) > VALUESET_MAX_ARRAY)
			return any;
		get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign,
						  &elems, &nulls, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			Const	   *c = makeConst(ARR_ELEMTYPE(arr), -1,
									  ((Const *) array)->constcollid, elmlen,
									  elems[i], nulls[i], elmbyval);

			comparisons = lappend(comparisons,
								  make_opclause(saop->opno, BOOLOID, false,
												linitial(saop->args),
												(Expr *) c, InvalidOid,
												saop->inputcollid));
		}
	}
	else if (IsA(array, ArrayExpr) && !((ArrayExpr *) array)->multidims &&
			 list_length(((ArrayExpr *) array)->elements) <= VALUESET_MAX_ARRAY)
	{
		foreach_ptr(Expr, elem, ((ArrayExpr *) array)->elements)
			comparisons = lappend(comparisons,
								  make_opclause(saop->opno, BOOLOID, false,
												linitial(saop->args), elem,
												InvalidOid, saop->inputcollid));
	}
	else
		return any;

	return possible_values_of(comparisons, !saop->useOr, target);
}

static ValueSet
possible_values(Node *clause, ValueTarget *target)
{
	ValueSet	result = {.any = true,.set = NULL};

	if (clause == NULL)
		return result;
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	if (IsA(clause, List))
		return possible_values_of((List *) clause, true, target);
	if (is_andclause(clause))
		return possible_values_of(((BoolExpr *) clause)->args, true, target);
	if (is_orclause(clause))
		return possible_values_of(((BoolExpr *) clause)->args, false, target);
	if (IsA(clause, ScalarArrayOpExpr))
		return possible_values_of_array((ScalarArrayOpExpr *) clause, target);
	if (value_atom(clause, target, &result))
		return result;
	result.any = true;
	return result;
}

/* ------------------------------------------------------------------------- */
/* Which segments                                                            */
/* ------------------------------------------------------------------------- */

/* The hash family of gp_segment_id's type, int4: integer_ops. */
static Oid
segment_id_opfamily(void)
{
	Oid			opclass = GetDefaultOpClass(INT4OID, HASH_AM_OID);

	return OidIsValid(opclass) ? get_opclass_family(opclass) : InvalidOid;
}

/*
 * The segments that hold every row the conditions can match, in Cloudberry's
 * order, or NIL where that is every segment of the table -- as
 * GetContentIdsFromPlanForSingleRelation() works them out
 * (cdbtargeteddispatch.c).  First gp_segment_id, which a condition may fix
 * to a few segments of a hashed or a random table; then a hashed table's
 * key, each combination of the values its columns may take hashed to its
 * segment, as long as there are fewer than three per segment.  Conditions
 * no row can meet name segment 0 alone, where the query finds nothing, as
 * Cloudberry's cdbllize.c has it.
 */
static List *
dispatch_contents(GpPolicy *policy, List *quals, Index relid)
{
	MemoryContext cxt;
	MemoryContext oldcxt;
	List	   *result = NIL;
	ValueTarget target;
	ValueSet	vs;

	if (!gp_enable_direct_dispatch || quals == NIL ||
		!(GpPolicyIsHashPartitioned(policy) ||
		  GpPolicyIsRandomPartitioned(policy)))
		return NIL;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "gp direct dispatch",
								ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* gp_segment_id = 1, gp_segment_id IN (0, 2) */
	target.attno = 0;
	target.varno = relid;
	target.opfamily = segment_id_opfamily();
	vs = possible_values((Node *) quals, &target);
	if (!vs.any)
	{
		List	   *values = valueset_values(&vs);
		bool		ok = values != NIL && list_length(values) < policy->numsegments;
		List	   *contents = NIL;

		foreach_ptr(Const, c, values)
		{
			int64		content;

			if (c->constisnull)
				continue;
			switch (c->consttype)
			{
				case INT2OID:
					content = DatumGetInt16(c->constvalue);
					break;
				case INT4OID:
					content = DatumGetInt32(c->constvalue);
					break;
				case INT8OID:
					content = DatumGetInt64(c->constvalue);
					break;
				default:
					ok = false;
					continue;
			}
			/* a segment the table has, or none: a row of no other is there */
			if (content >= 0 && content < policy->numsegments)
				contents = list_append_unique_int(contents, (int) content);
		}
		if (ok)
		{
			MemoryContextSwitchTo(oldcxt);
			result = list_copy(contents);
			MemoryContextDelete(cxt);
			return result != NIL ? result : list_make1_int(0);
		}
	}

	if (GpPolicyIsHashPartitioned(policy))
	{
		int			nattrs = policy->nattrs;
		List	  **values = palloc0_array(List *, nattrs);
		long		combinations = 1;

		for (int k = 0; k < nattrs && combinations > 0; k++)
		{
			target.attno = policy->attrs[k];
			target.varno = relid;
			target.opfamily = get_opclass_family(policy->opclasses[k]);
			vs = possible_values((Node *) quals, &target);
			if (vs.any)
				combinations = -1;
			else
			{
				values[k] = valueset_values(&vs);
				combinations *= list_length(values[k]);
			}
		}

		if (combinations == 0)
			result = list_make1_int(-1);	/* no row can match */
		else if (combinations > 0 && combinations < policy->numsegments * 3)
		{
			Oid		   *types = palloc_array(Oid, nattrs);
			Datum	   *datums = palloc_array(Datum, nattrs);
			bool	   *isnull = palloc_array(bool, nattrs);

			for (long index = 0; index < combinations; index++)
			{
				long		cur = index;
				int			content;

				for (int k = 0; k < nattrs; k++)
				{
					int			n = list_length(values[k]);
					Const	   *c = (Const *) list_nth(values[k], cur % n);

					types[k] = c->consttype;
					datums[k] = c->constvalue;
					isnull[k] = c->constisnull;
					cur /= n;
				}
				content = GpHashSegmentForKey(policy, types, datums, isnull);
				if (content < 0)
				{
					result = NIL;	/* a type the family cannot hash */
					break;
				}
				result = list_append_unique_int(result, content);
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);
	if (result != NIL && linitial_int(result) == -1)
		result = list_make1_int(0);
	else
		result = list_copy(result);
	MemoryContextDelete(cxt);

	return result;
}

/*
 * The segments an UPDATE or DELETE sent as it stands can go to: its WHERE
 * fixes the target's key, or its gp_segment_id.  varno is the target's range
 * table index in the Query the conditions are from.
 */
List *
GpScanDirectDispatchContents(Oid relid, Node *quals, Index varno)
{
	GpPolicy   *policy = GpScanDistributedPolicy(relid);

	if (policy == NULL || quals == NULL)
		return NIL;
	return dispatch_contents(policy, make_ands_implicit((Expr *) quals), varno);
}

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

static void add_segment_junk(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);

/*
 * The size of a distributed table.  The planner scales pg_class's reltuples
 * by the pages the table has now, and the coordinator's copy has none, so a
 * table ANALYZE has counted would be estimated at no rows at all.  What
 * ANALYZE wrote is the size across the segments (gp_analyze.c), and is taken
 * as it stands.
 *
 * A table never analyzed keeps the planner's own guess, which for a heap
 * table is PostgreSQL's for one never vacuumed: ten pages of rows as wide as
 * its columns (table_block_relation_estimate_size()).  An append-optimized
 * or PAX table's method counts its own files instead, of which the
 * coordinator has none, and says no rows: a join of two such tables would
 * be a nested loop that runs its inner scan on the segments again for each
 * outer row.  It is guessed at as a heap table is, as Cloudberry guesses at
 * every distributed table alike (cdb_estimate_rel_size()): not through
 * table_block_relation_estimate_size() itself, which reads a fillfactor
 * from options that are the method's own.
 *
 * And the junk column an UPDATE's or a DELETE's plan carries the segment of
 * another table's row in (add_segment_junk()).
 */
static void
gp_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	HeapTuple	tuple;
	Form_pg_class classForm;

	if (prev_build_simple_rel)
		prev_build_simple_rel(root, rel, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	add_segment_junk(root, rel, rte);
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
		return;
	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	if (!HeapTupleIsValid(tuple))
		return;
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	if (classForm->relpages > 0 && classForm->reltuples >= 0)
	{
		rel->pages = (BlockNumber) classForm->relpages;
		rel->tuples = classForm->reltuples;
		rel->allvisfrac = Min(1.0, (double) classForm->relallvisible /
							  classForm->relpages);
	}
	else if (classForm->reltuples < 0 && !classForm->relhassubclass &&
			 rel->tuples <= 0)
	{
		int32		width;

		width = get_relation_data_width(rte->relid,
										rel->attr_widths - rel->min_attr);
		width += MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData);
		rel->pages = 10;
		/* integer division, as the heap's */
		rel->tuples = rint(clamp_row_est((BLCKSZ - SizeOfPageHeaderData) / width) *
						   rel->pages);
		rel->allvisfrac = 0;
	}
	ReleaseSysCache(tuple);
}

/*
 * The outer joins whose nullable side range table entry "relid" is on, into
 * *result: those a reference to it in the statement's target list is nulled
 * by, which a placeholder there says (phnullingrels), as the parser has a
 * Var there say it (varnullingrels).  False where the join tree has no such
 * entry.
 */
static bool
nulling_joins(Node *jtnode, Index relid, Relids above, Relids *result)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, RangeTblRef))
	{
		if (((RangeTblRef *) jtnode)->rtindex != (int) relid)
			return false;
		*result = above;
		return true;
	}
	if (IsA(jtnode, FromExpr))
	{
		foreach_ptr(Node, item, ((FromExpr *) jtnode)->fromlist)
			if (nulling_joins(item, relid, above, result))
				return true;
		return false;
	}
	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		left = above;
		Relids		right = above;

		if (j->rtindex > 0 && (j->jointype == JOIN_LEFT || j->jointype == JOIN_FULL))
			right = bms_add_member(bms_copy(above), j->rtindex);
		if (j->rtindex > 0 && (j->jointype == JOIN_RIGHT || j->jointype == JOIN_FULL))
			left = bms_add_member(bms_copy(above), j->rtindex);
		return nulling_joins(j->larg, relid, left, result) ||
			nulling_joins(j->rarg, relid, right, result);
	}
	return false;
}

/*
 * An UPDATE or DELETE of a distributed table, with the global deadlock
 * detector on, rechecks a row another transaction updated between the
 * plan's read of it and its segment's write, as PostgreSQL's READ COMMITTED
 * update rechecks it (gp_explicit.c): its plan runs again with the row's
 * newest version, and with the row of each other table it read by a row
 * mark that the version it read was joined to -- found by its ctid, which
 * the planner's row mark carries in a junk column (ROW_MARK_REFERENCE).  A
 * ctid is one segment's, so each distributed table's row carries its
 * segment too: gp_internal.row_segment() of its ctid, which the gather
 * answers (gather_plan()), a placeholder the planner has the gather compute
 * and carries up as it carries a column.  Added as the planner builds the
 * relation, before it gives each relation the columns the target list
 * needs of it.
 */
static void
add_segment_junk(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	PlanRowMark *mark = NULL;
	PlaceHolderVar *phv;
	Oid			func;
	Relids		nulled = NULL;
	char		resname[32];

	if (!gp_enable_global_deadlock_detector ||
		(parse->commandType != CMD_UPDATE && parse->commandType != CMD_DELETE) ||
		rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION ||
		rel->relid == (Index) parse->resultRelation ||
		GpScanDistributedPolicy(rte->relid) == NULL ||
		GpScanDistributedPolicy(rt_fetch(parse->resultRelation,
										 parse->rtable)->relid) == NULL)
		return;
	foreach_node(PlanRowMark, rc, root->rowMarks)
		if (rc->rti == rel->relid && rc->prti == rc->rti)
			mark = rc;
	if (mark == NULL || mark->markType != ROW_MARK_REFERENCE)
		return;
	func = GpSegmentRowSegmentFunction();
	if (!OidIsValid(func) ||
		!nulling_joins((Node *) parse->jointree, rel->relid, NULL, &nulled))
		return;

	phv = makeNode(PlaceHolderVar);
	phv->phexpr = (Expr *) makeFuncExpr(func, INT4OID,
										list_make1(makeVar(rel->relid,
														   SelfItemPointerAttributeNumber,
														   TIDOID, -1, InvalidOid, 0)),
										InvalidOid, InvalidOid,
										COERCE_EXPLICIT_CALL);
	phv->phrels = bms_make_singleton(rel->relid);
	phv->phnullingrels = nulled;
	phv->phid = ++(root->glob->lastPHId);
	phv->phlevelsup = 0;
	snprintf(resname, sizeof(resname), GP_SEGMENT_JUNK, mark->rowmarkId);
	root->processed_tlist = lappend(root->processed_tlist,
									makeTargetEntry((Expr *) phv,
													list_length(root->processed_tlist) + 1,
													pstrdup(resname), true));
}

static Node *find_segment_of(Node *tree, Index relid);

/* The relations a NOT IN's subquery was noticed of, in this planning. */
static List *noticed_relids = NIL;

/* What a condition reads of the table: its ctid, and its gp_segment_id. */
typedef struct CtidInventory
{
	Index		relid;
	bool		uses_ctid;
	bool		uses_segid;
} CtidInventory;

static bool
ctid_inventory_walker(Node *node, CtidInventory *inv)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == inv->relid && var->varlevelsup == 0 &&
			var->varattno == SelfItemPointerAttributeNumber)
			inv->uses_ctid = true;
		return false;
	}
	if (GpSegmentIsSegmentOf(node, inv->relid))
	{
		inv->uses_segid = true;
		return false;
	}
	return expression_tree_walker(node, ctid_inventory_walker, inv);
}

/*
 * A distributed table's ctid is a place on one segment, and names a row only
 * with gp_segment_id beside it.  A query whose conditions -- its own or a
 * join's, not what it returns -- read a ctid without it is told so, and an
 * UPDATE or DELETE is refused: Cloudberry's
 * cdbmutate_warn_ctid_without_segid(), in its words.
 */
static void
warn_ctid_without_segment_id(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte)
{
	CtidInventory inv = {.relid = rel->relid};
	Relids		needed;
	const char *cmd;
	int			elevel;

	if (GpScanDistributedPolicy(rte->relid) == NULL)
		return;

	/* ctid needed above the scan, but not only by the final target list */
	needed = rel->attr_needed[SelfItemPointerAttributeNumber - rel->min_attr];
	if (bms_nonempty_difference(needed, bms_make_singleton(0)))
		inv.uses_ctid = true;
	foreach_node(RestrictInfo, ri, rel->joininfo)
		(void) ctid_inventory_walker((Node *) ri->clause, &inv);
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		(void) ctid_inventory_walker((Node *) ri->clause, &inv);

	/* a join's equality is an equivalence class, not a condition of either */
	foreach_node(EquivalenceClass, ec, root->eq_classes)
		foreach_node(EquivalenceMember, em, ec->ec_members)
			if (find_segment_of((Node *) em->em_expr, rel->relid) != NULL)
				inv.uses_segid = true;

	if (!inv.uses_ctid || inv.uses_segid ||
		list_member_oid(noticed_relids, rte->relid))
		return;

	switch (root->parse->commandType)
	{
		case CMD_UPDATE:
			cmd = "UPDATE";
			elevel = ERROR;
			break;
		case CMD_DELETE:
			cmd = "DELETE";
			elevel = ERROR;
			break;
		case CMD_MERGE:
			cmd = "MERGE";
			elevel = ERROR;
			break;
		default:
			cmd = "SELECT";
			elevel = NOTICE;
	}
	ereport(elevel,
			(errmsg("%s uses system-defined column \"%s.ctid\" without the necessary companion column \"%s.gp_segment_id\"",
					cmd, rte->eref->aliasname, rte->eref->aliasname),
			 errhint("To uniquely identify a row within a distributed table, use the \"gp_segment_id\" column together with the \"ctid\" column.")));
}

/*
 * NOT IN (SELECT ctid FROM t), and <> ALL: Cloudberry makes it an anti-join
 * whose condition compares t's ctid, and notices it there; PostgreSQL's
 * planner keeps it a subplan, whose ctid is only what the subquery returns.
 * So such a subquery is noticed as it stands, once for its relation, before
 * the statement is planned.
 */
typedef struct SublinkNoticeContext
{
	CmdType		cmd;			/* of the query the SubLink is in */
	bool		under_not;
} SublinkNoticeContext;

static bool
sublink_notice_walker(Node *node, SublinkNoticeContext *cxt)
{
	if (node == NULL)
		return false;

	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == NOT_EXPR)
	{
		bool		save = cxt->under_not;
		bool		result;

		cxt->under_not = true;
		result = expression_tree_walker(node, sublink_notice_walker, cxt);
		cxt->under_not = save;
		return result;
	}

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *sub = (Query *) sublink->subselect;
		bool		save = cxt->under_not;

		if ((sublink->subLinkType == ALL_SUBLINK ||
			 (sublink->subLinkType == ANY_SUBLINK && cxt->under_not)) &&
			sub->commandType == CMD_SELECT)
		{
			foreach_node(TargetEntry, tle, sub->targetList)
			{
				Var		   *var = (Var *) tle->expr;
				RangeTblEntry *rte;
				const char *cmd;

				if (tle->resjunk || !IsA(var, Var) || var->varlevelsup != 0 ||
					var->varattno != SelfItemPointerAttributeNumber)
					continue;
				rte = rt_fetch(var->varno, sub->rtable);
				if (rte->rtekind != RTE_RELATION ||
					GpScanDistributedPolicy(rte->relid) == NULL ||
					find_segment_of((Node *) sub->targetList, var->varno) != NULL ||
					list_member_oid(noticed_relids, rte->relid))
					continue;
				cmd = cxt->cmd == CMD_UPDATE ? "UPDATE" :
					cxt->cmd == CMD_DELETE ? "DELETE" :
					cxt->cmd == CMD_MERGE ? "MERGE" : "SELECT";
				ereport(cxt->cmd == CMD_UPDATE || cxt->cmd == CMD_DELETE ||
						cxt->cmd == CMD_MERGE ? ERROR : NOTICE,
						(errmsg("%s uses system-defined column \"%s.ctid\" without the necessary companion column \"%s.gp_segment_id\"",
								cmd, rte->eref->aliasname, rte->eref->aliasname),
						 errhint("To uniquely identify a row within a distributed table, use the \"gp_segment_id\" column together with the \"ctid\" column.")));
				noticed_relids = lappend_oid(noticed_relids, rte->relid);
			}
		}

		/* the test expression, and the subquery's own */
		cxt->under_not = false;
		(void) sublink_notice_walker(sublink->testexpr, cxt);
		(void) query_tree_walker(sub, sublink_notice_walker, cxt, 0);
		cxt->under_not = save;
		return false;
	}

	if (IsA(node, Query))
	{
		CmdType		save = cxt->cmd;
		bool		result;

		cxt->cmd = ((Query *) node)->commandType;
		result = query_tree_walker((Query *) node, sublink_notice_walker, cxt, 0);
		cxt->cmd = save;
		return result;
	}

	cxt->under_not = false;
	return expression_tree_walker(node, sublink_notice_walker, cxt);
}

void
GpScanNoticeSublinkCtid(Query *parse)
{
	SublinkNoticeContext cxt = {.cmd = parse->commandType,.under_not = false};

	/* the last planning's list went with its memory */
	noticed_relids = NIL;
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	(void) query_tree_walker(parse, sublink_notice_walker, &cxt, 0);
}

/*
 * Keyed gathers.  A join the planner may not hash -- enable_hashjoin off, as
 * a test turns it off to have its nested loop -- runs its inner side for each
 * outer row, and a gather there gives what it kept each time, every row of it
 * (mark_rescans()): a nested loop of two tables gathered compares every row
 * of one with every row of the other, on the coordinator, where Cloudberry's
 * planner redistributes them so that only the rows that may match meet, on a
 * segment -- deadlock's left join of 100,000 rows by 100,000 took minutes.
 * So where hash joins are off a gather is offered parameterized by a join's
 * equality, as an index scan is: it gathers its table once, keeps the rows by
 * their key's hash (keyed_init()), and gives each run of the loop the rows
 * whose key hashes as the outer row's value does; the equality is the scan's
 * condition still, which checks each of them.  With hash joins on no such
 * gather is offered: the planner hashes the join itself, and no plan of any
 * query is other than it was.  Not for a statement that writes or locks rows,
 * or a cursor's, whose gathers bring each row's ctid; nor for a rel read
 * laterally, whose gather is parameterized by what it reads already, or a
 * partition, which its parent's Append gathers: a table read on its own.
 */

/* A join clause whose one side is the rel's own, the other of rels outside it. */
static bool
keyable_clause(RestrictInfo *rinfo, RelOptInfo *rel)
{
	if (!OidIsValid(rinfo->hashjoinoperator) || rinfo->pseudoconstant)
		return false;
	return (bms_equal(rinfo->left_relids, rel->relids) &&
			!bms_overlap(rinfo->right_relids, rel->relids)) ||
		(bms_equal(rinfo->right_relids, rel->relids) &&
		 !bms_overlap(rinfo->left_relids, rel->relids));
}

/* The outer rels a keyable clause parameterizes the rel's gather by. */
static void
keyed_candidate(PlannerInfo *root, RelOptInfo *rel, RestrictInfo *rinfo,
				List **ppis)
{
	Relids		outer;

	if (!keyable_clause(rinfo, rel) || !join_clause_is_movable_to(rinfo, rel))
		return;
	outer = bms_del_member(bms_copy(rinfo->clause_relids), rel->relid);
	if (bms_is_empty(outer) || !bms_is_subset(outer, root->all_baserels))
		return;
	*ppis = list_append_unique_ptr(*ppis, get_baserel_parampathinfo(root, rel, outer));
}

/* An equivalence class's member of the rel's alone, one at a time. */
typedef struct KeyedMember
{
	Expr	   *current;
	List	   *used;
} KeyedMember;

static bool
keyed_member(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
			 EquivalenceMember *em, void *arg)
{
	KeyedMember *member = (KeyedMember *) arg;

	if (member->current != NULL)
		return equal(em->em_expr, member->current);
	if (list_member(member->used, em->em_expr))
		return false;
	member->current = em->em_expr;
	return true;
}

/*
 * A keyed gather's path for each set of outer rels a join's equality reads,
 * found as postgres_fdw finds its parameterized paths: in the rel's join
 * clauses, and in the equivalence classes a join's equalities are made of.
 * The gather is one, however often the loop runs its inner side: its cost is
 * spread over the outer rel's rows, each run costing the lookup and the rows
 * it gives.
 */
static void
add_keyed_gather_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *ppis = NIL;
	Cost		gathered;

	if (enable_hashjoin || rel->reloptkind != RELOPT_BASEREL ||
		!bms_is_empty(rel->lateral_relids) || planning_cursor ||
		root->parse->commandType != CMD_SELECT || root->rowMarks != NIL)
		return;

	/* its rows kept must fit hash_mem, as a hash join's in one batch do */
	if (rel->rows * (MAXALIGN(rel->reltarget->width) +
					 MAXALIGN(SizeofMinimalTupleHeader)) > get_hash_memory_limit())
		return;

	foreach_node(RestrictInfo, rinfo, rel->joininfo)
		keyed_candidate(root, rel, rinfo, &ppis);
	if (rel->has_eclass_joins)
	{
		KeyedMember member = {NULL, NIL};

		for (;;)
		{
			List	   *clauses;

			member.current = NULL;
			clauses = generate_implied_equalities_for_column(root, rel,
															 keyed_member,
															 &member,
															 rel->lateral_referencers);
			if (member.current == NULL)
				break;
			foreach_node(RestrictInfo, rinfo, clauses)
				keyed_candidate(root, rel, rinfo, &ppis);
			member.used = lappend(member.used, member.current);
		}
	}

	/* the plain gather's cost (gp_set_rel_pathlist()) */
	gathered = GATHER_STARTUP_COST + rel->rows * (GATHER_ROW_COST + cpu_tuple_cost);

	foreach_ptr(ParamPathInfo, ppi, ppis)
	{
		CustomPath *cp = makeNode(CustomPath);
		double		runs = 1;
		int			nkeys = 0;
		int			relid = -1;

		foreach_node(RestrictInfo, rinfo, ppi->ppi_clauses)
			if (keyable_clause(rinfo, rel))
				nkeys++;
		if (nkeys == 0)
			continue;
		while ((relid = bms_next_member(ppi->ppi_req_outer, relid)) >= 0)
			runs = Max(runs, find_base_rel(root, relid)->rows);

		cp->path.pathtype = T_CustomScan;
		cp->path.parent = rel;
		cp->path.pathtarget = rel->reltarget;
		cp->path.param_info = ppi;
		cp->path.parallel_aware = false;
		cp->path.parallel_safe = false;
		cp->path.parallel_workers = 0;
		cp->path.rows = ppi->ppi_rows;
		/* the gather, and each row kept by its key's hash, once in all */
		cp->path.startup_cost = (gathered + rel->rows *
								 (nkeys * cpu_operator_cost + cpu_tuple_cost)) / runs;
		/* and each run's lookup, and the rows it gives */
		cp->path.total_cost = cp->path.startup_cost +
			nkeys * cpu_operator_cost + ppi->ppi_rows * cpu_tuple_cost;
		cp->path.pathkeys = NIL;
		cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
		cp->custom_paths = NIL;
		cp->custom_private = list_make1(makeBoolean(true));
		cp->methods = &gather_path_methods;
		add_path(rel, &cp->path);
	}
}

static void
gp_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					RangeTblEntry *rte)
{
	CustomPath *cp;
	GpPolicy   *policy;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return;
	if (rel->reloptkind == RELOPT_BASEREL && rte->rtekind == RTE_RELATION)
		warn_ctid_without_segment_id(root, rel, rte);
	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	/* A parent whose children are scanned instead has no scan of its own. */
	if (rte->rtekind != RTE_RELATION || rte->inh)
		return;
	if (IS_DUMMY_REL(rel))
		return;
	/*
	 * a table's, a materialized view's, or a foreign table's the segments
	 * read -- an external table's, or one whose mpp_execute says so
	 */
	if (get_rel_relkind(rte->relid) != RELKIND_RELATION &&
		get_rel_relkind(rte->relid) != RELKIND_MATVIEW &&
		get_rel_relkind(rte->relid) != RELKIND_FOREIGN_TABLE)
		return;

	policy = GpScanDistributedPolicy(rte->relid);
	if (policy == NULL)
		return;

	/*
	 * Every other way of reading it reads the coordinator's empty copy, so
	 * this is the only path left: not the cheapest of several, the one that
	 * gives the right answer.
	 */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/*
	 * And a foreign table's wrapper may not take a join or an aggregate over
	 * it to its server (GetForeignJoinPaths(), GetForeignUpperPaths()),
	 * which the coordinator would ask for all of the table: its rows are
	 * what each segment reads.
	 */
	if (rel->fdwroutine != NULL)
	{
		rel->serverid = InvalidOid;
		rel->fdwroutine = NULL;
	}

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	/*
	 * Parameterized by what the rel reads laterally, as PostgreSQL's own
	 * scans of it are: a column of another table its target list computes
	 * (a pulled-up LATERAL subquery's) is that table's nestloop parameter,
	 * computed here as the rows arrive, and the gather is run again for each
	 * of that table's rows.
	 */
	cp->path.param_info = get_baserel_parampathinfo(root, rel,
													rel->lateral_relids);
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = cp->path.param_info ? cp->path.param_info->ppi_rows :
		rel->rows;
	cp->path.startup_cost = GATHER_STARTUP_COST;
	cp->path.total_cost = GATHER_STARTUP_COST +
		rel->rows * (GATHER_ROW_COST + cpu_tuple_cost);
	cp->path.pathkeys = NIL;

	/*
	 * It projects: a query of this table alone computes what it returns in
	 * the gather itself, gp_segment_id among it, which only the gather knows
	 * of a random table's row.
	 */
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &gather_path_methods;

	add_path(rel, &cp->path);

	/* and, where joins are not hashed, keyed gathers of it */
	add_keyed_gather_paths(root, rel);
}

/* The locking clause a gather sends the segments; see GpScanSetLocking(). */
static Oid	locking_relid = InvalidOid;
static LockClauseStrength locking_strength;
static LockWaitPolicy locking_wait;

void
GpScanSetLocking(Oid relid, LockClauseStrength strength,
				 LockWaitPolicy waitPolicy)
{
	locking_relid = relid;
	locking_strength = strength;
	locking_wait = waitPolicy;
}

void
GpScanClearLocking(void)
{
	locking_relid = InvalidOid;
}

bool
GpScanSetCursor(bool cursor)
{
	bool		was = planning_cursor;

	planning_cursor = cursor;
	return was;
}

/* The first expression in a tree that is gp_segment_id of varno's row. */
static bool
find_segment_of_walker(Node *node, void *context)
{
	void	  **cxt = (void **) context;

	if (node == NULL)
		return false;
	if (GpSegmentIsSegmentOf(node, *(Index *) cxt[0]))
	{
		cxt[1] = node;
		return true;
	}
	return expression_tree_walker(node, find_segment_of_walker, context);
}

static Node *
find_segment_of(Node *tree, Index relid)
{
	void	   *cxt[2] = {&relid, NULL};

	(void) find_segment_of_walker(tree, cxt);
	return (Node *) cxt[1];
}

/*
 * The whole-row Var of varno's row the plan reads, first in the target the
 * rel gives the plan above it: of the table's row type, or of RECORD, as the
 * planner makes the one a foreign table's UPDATE reads, and O20's for a
 * table whose access method takes its old row from the plan.  The scan
 * tuple's column has the type the plan's Var expects, or the executor
 * refuses it; and one column serves every whole-row Var of the rel, which
 * setrefs.c matches by column number alone.  So RECORD's, where the plan
 * has one: the other is gp_segment_id's argument, segment_of(t.*), whose
 * call is a column of the scan tuple of its own and reads no whole row.
 */
static bool
find_whole_row_walker(Node *node, void *context)
{
	void	  **cxt = (void **) context;

	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == *(Index *) cxt[0] &&
		((Var *) node)->varattno == InvalidAttrNumber &&
		((Var *) node)->varlevelsup == 0)
	{
		if (cxt[1] == NULL || ((Var *) node)->vartype == RECORDOID)
			cxt[1] = node;
		return ((Var *) node)->vartype == RECORDOID;
	}
	return expression_tree_walker(node, find_whole_row_walker, context);
}

static Var *
find_whole_row(Node *tree, Index relid)
{
	void	   *cxt[2] = {&relid, NULL};

	(void) find_whole_row_walker(tree, cxt);
	return (Var *) cxt[1];
}

/* One more column of what the segments send. */
static int
remote_column(StringInfo sql, List **types, List **typmods,
			  const char *column, Oid type, int32 typmod)
{
	int			n = list_length(*types);

	if (n > 0)
		appendStringInfoString(sql, ", ");
	GpAppendTransferColumn(sql, column, type);
	*types = lappend_oid(*types, type);
	*typmods = lappend_int(*typmods, typmod);
	return n;
}

static Plan *
gather_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation;
	TupleDesc	tupdesc;
	GpPolicy   *policy;
	List	   *pushed = NIL;
	List	   *local = NIL;
	CurrentOfExpr *current_of = NULL;
	List	   *dpcontext;
	Bitmapset  *needed = NULL;
	StringInfoData select;
	StringInfoData where;
	StringInfoData folded;
	List	   *folds = NIL;
	StringInfoData locking;
	List	   *types = NIL;
	List	   *typmods = NIL;
	List	   *scan_tlist = NIL;
	List	   *sources = NIL;
	List	   *attrs = NIL;
	List	   *contents = NIL;
	int			ctid_remote = -1;
	bool		whole_row;
	bool		identity;
	bool		keyed;
	Node	   *segment_of = NULL;
	static const AttrNumber sysattrs[] = {
		MinTransactionIdAttributeNumber, MinCommandIdAttributeNumber,
		MaxTransactionIdAttributeNumber, MaxCommandIdAttributeNumber
	};

#define NEEDED(attno) \
	bms_is_member((attno) - FirstLowInvalidHeapAttributeNumber, needed)
#define SCAN_COLUMN(expr, name, source) \
	do { \
		scan_tlist = lappend(scan_tlist, \
							 makeTargetEntry((Expr *) (expr), \
											 list_length(scan_tlist) + 1, \
											 pstrdup(name), false)); \
		sources = lappend_int(sources, (source)); \
	} while (0)

	relation = table_open(rte->relid, NoLock);
	tupdesc = RelationGetDescr(relation);
	policy = GpScanDistributedPolicy(rte->relid);

	/*
	 * Which conditions go to the segments, and which stay here; and the
	 * cursor WHERE CURRENT OF names, whose position the gather reads itself.
	 */
	foreach_node(RestrictInfo, ri, clauses)
	{
		if (IsA(ri->clause, CurrentOfExpr) &&
			((CurrentOfExpr *) ri->clause)->cvarno == rel->relid)
			current_of = (CurrentOfExpr *) ri->clause;
		else if (!ri->pseudoconstant && is_shippable(ri->clause, rel->relid))
			pushed = lappend(pushed, ri->clause);
		else
			local = lappend(local, ri);
	}

	/*
	 * Which columns anything here reads: what the rel gives the plan above
	 * it, which is all a projection put in this node's place may compute
	 * from -- the planner hands a node that projects no target list of its
	 * own (CP_IGNORE_TLIST) -- and the conditions left here.
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &needed);
	pull_varattnos((Node *) tlist, rel->relid, &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, false), rel->relid,
				   &needed);
	pull_varattnos((Node *) extract_actual_clauses(local, true), rel->relid,
				   &needed);
	whole_row = NEEDED(InvalidAttrNumber);

	/*
	 * A table an UPDATE, DELETE or MERGE changes: each row with its ctid,
	 * which with the segment it came from is the row's identity to the write
	 * that changes it on that segment (gp_explicit.c).
	 */
	identity = (root->parse->commandType == CMD_UPDATE ||
				root->parse->commandType == CMD_DELETE ||
				root->parse->commandType == CMD_MERGE) &&
		bms_is_member(rel->relid, root->all_result_relids);

	/* What the segments send, and the scan tuple made of it. */
	initStringInfo(&select);
	appendStringInfoString(&select, "SELECT ");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		int			remote = -1;

		if (!att->attisdropped && (whole_row || NEEDED(att->attnum)))
		{
			remote = remote_column(&select, &types, &typmods,
								   quote_identifier(NameStr(att->attname)),
								   att->atttypid, att->atttypmod);
			if (NEEDED(att->attnum))
				SCAN_COLUMN(makeVar(rel->relid, att->attnum, att->atttypid,
									att->atttypmod, att->attcollation, 0),
							NameStr(att->attname), remote);
		}
		attrs = lappend_int(attrs, remote);
	}

	/*
	 * ctid, where the plan reads it, where it is a write's identity, and in a
	 * cursor, whose rows WHERE CURRENT OF finds by it.
	 */
	if (NEEDED(SelfItemPointerAttributeNumber) || identity || planning_cursor ||
		current_of != NULL)
	{
		ctid_remote = remote_column(&select, &types, &typmods, "ctid",
									TIDOID, -1);
		if (NEEDED(SelfItemPointerAttributeNumber))
			SCAN_COLUMN(makeVar(rel->relid, SelfItemPointerAttributeNumber,
								TIDOID, -1, InvalidOid, 0),
						"ctid", identity ? GATHER_SRC_IDENTITY : ctid_remote);
	}
	for (int i = 0; i < lengthof(sysattrs); i++)
	{
		const FormData_pg_attribute *att = SystemAttributeDefinition(sysattrs[i]);

		if (NEEDED(sysattrs[i]))
			SCAN_COLUMN(makeVar(rel->relid, sysattrs[i], att->atttypid, -1,
								InvalidOid, 0),
						NameStr(att->attname),
						remote_column(&select, &types, &typmods,
									  NameStr(att->attname), att->atttypid,
									  -1));
	}
	if (NEEDED(TableOidAttributeNumber))
		SCAN_COLUMN(makeVar(rel->relid, TableOidAttributeNumber, OIDOID, -1,
							InvalidOid, 0),
					"tableoid", GATHER_SRC_TABLEOID);
	if (whole_row)
	{
		Var		   *wholerow = find_whole_row((Node *) rel->reltarget->exprs,
											  rel->relid);

		if (wholerow == NULL || wholerow->vartype != RECORDOID)
		{
			Var		   *other = find_whole_row((Node *) tlist, rel->relid);

			if (other != NULL && (wholerow == NULL || other->vartype == RECORDOID))
				wholerow = other;
		}
		SCAN_COLUMN(wholerow != NULL ? copyObject(wholerow)
					: makeWholeRowVar(rte, rel->relid, 0, false),
					RelationGetRelationName(relation), GATHER_SRC_WHOLEROW);
	}

	/*
	 * gp_segment_id of a hashed or random table, where the query names it:
	 * the segment the row came from, which is the one that holds it.  Only a
	 * random table's needs this -- nothing in the row says -- and a hashed
	 * table's comes free.  It is in the scan tuple as the call the query
	 * makes, so that the planner's own references to that call, where this
	 * node computes them, read it (setrefs.c).
	 */
	if (policy != NULL && !GpPolicyIsReplicated(policy))
	{
		/* a partition's, in its own target, of its row as its parent's */
		segment_of = find_segment_of((Node *) rel->reltarget->exprs, rel->relid);
		if (segment_of == NULL)
			segment_of = find_segment_of((Node *) root->processed_tlist, rel->relid);
		if (segment_of == NULL)
			segment_of = find_segment_of((Node *) extract_actual_clauses(local, false),
										 rel->relid);
		if (segment_of != NULL)
			SCAN_COLUMN(copyObject(segment_of), "gp_segment_id",
						GATHER_SRC_SEGMENT);
	}

	/*
	 * The segment each row came from, where an UPDATE or a DELETE that reads
	 * this table besides the one it writes carries it up as a junk column
	 * (add_segment_junk()): the placeholder is a column of the scan tuple,
	 * which the planner's references to it read, and its call is not made.
	 */
	foreach_ptr(Node, expr, rel->reltarget->exprs)
	{
		if (IsA(expr, PlaceHolderVar) &&
			GpSegmentIsRowSegment((Node *) ((PlaceHolderVar *) expr)->phexpr,
								  rel->relid))
		{
			SCAN_COLUMN(copyObject(expr), "gp_segment_id", GATHER_SRC_SEGMENT);
			break;
		}
	}

	/* A row with no column still has to be a row. */
	if (types == NIL)
		(void) remote_column(&select, &types, &typmods, "NULL::pg_catalog.bool",
							 BOOLOID, -1);

	appendStringInfo(&select, " FROM ONLY %s",
					 GpDispatchRelationName(RelationGetRelid(relation)));

	dpcontext = deparse_context_for(RelationGetRelationName(relation),
									rte->relid);
	initStringInfo(&where);
	initStringInfo(&folded);
	foreach_ptr(Node, clause, pushed)
	{
		Node	   *qual = copyObject(clause);

		/* deparse_context_for() knows this relation as range table entry 1 */
		ChangeVarNodes(qual, rel->relid, 1, 0);
		/* ruleutils brackets an operator's operands itself */
		appendStringInfo(&where, "%s%s", where.len > 0 ? " AND " : "",
						 deparse_expression(qual, dpcontext, false, true));
		/* and as sent: its stable constant parts computed as it starts */
		appendStringInfo(&folded, "%s%s", folded.len > 0 ? " AND " : "",
						 deparse_expression(fold_stable(qual, &folds),
											dpcontext, false, true));
	}

	/* the rows a SELECT ... FOR UPDATE locks, locked where they are */
	initStringInfo(&locking);
	if (OidIsValid(locking_relid) && locking_relid == rte->relid)
		appendStringInfo(&locking, " FOR %s%s",
						 locking_strength == LCS_FORKEYSHARE ? "KEY SHARE" :
						 locking_strength == LCS_FORSHARE ? "SHARE" :
						 locking_strength == LCS_FORNOKEYUPDATE ? "NO KEY UPDATE" :
						 "UPDATE",
						 locking_wait == LockWaitSkip ? " SKIP LOCKED" :
						 locking_wait == LockWaitError ? " NOWAIT" : "");

	if (policy != NULL && GpPolicyIsReplicated(policy))
		contents = list_make1_int(GpScanReplicatedContent(policy));
	else if (policy != NULL && current_of == NULL)
		contents = dispatch_contents(policy, pushed, rel->relid);

	table_close(relation, NoLock);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(local, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = folds;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = list_make5(makeString(select.data),
									   makeString(where.data),
									   makeString(locking.data),
									   contents,
									   makeInteger(policy != NULL ? policy->numsegments : 0));
	cscan->custom_private = lappend(cscan->custom_private, types);
	cscan->custom_private = lappend(cscan->custom_private, typmods);
	cscan->custom_private = lappend(cscan->custom_private, sources);
	cscan->custom_private = lappend(cscan->custom_private, attrs);
	cscan->custom_private = lappend(cscan->custom_private, makeInteger(ctid_remote));
	cscan->custom_private = lappend(cscan->custom_private,
									makeString(current_of != NULL && current_of->cursor_name != NULL
											   ? pstrdup(current_of->cursor_name) : ""));
	cscan->custom_private = lappend(cscan->custom_private,
									makeInteger(current_of != NULL ? current_of->cursor_param : 0));
	cscan->custom_private = lappend(cscan->custom_private, makeBoolean(identity));
	cscan->custom_private = lappend(cscan->custom_private, makeString(""));
	cscan->custom_private = lappend(cscan->custom_private,
									makeString(folds != NIL ? folded.data : ""));
	/* a keyed gather's path, where no row's ctid is read: it reads them again */
	keyed = best_path->custom_private != NIL &&
		boolVal(linitial(best_path->custom_private)) &&
		ctid_remote < 0 && !identity && locking.len == 0 && current_of == NULL;
	cscan->custom_private = lappend(cscan->custom_private, makeBoolean(keyed));
	cscan->methods = &gather_scan_methods;

	return &cscan->scan.plan;
#undef NEEDED
#undef SCAN_COLUMN
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

static int *
int_array(List *list, int *n)
{
	int		   *result = palloc_array(int, Max(list_length(list), 1));

	*n = 0;
	foreach_int(i, list)
		result[(*n)++] = i;
	return result;
}

static Node *
gather_create_state(CustomScan *cscan)
{
	GatherScanState *state = (GatherScanState *) newNode(sizeof(GatherScanState),
														 T_CustomScanState);
	List	   *priv = cscan->custom_private;

	state->css.methods = &gather_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	state->select = strVal(list_nth(priv, GATHER_PRIVATE_SELECT));
	state->where = strVal(list_nth(priv, GATHER_PRIVATE_WHERE));
	state->folded = strVal(list_nth(priv, GATHER_PRIVATE_FOLDED));
	state->locking = strVal(list_nth(priv, GATHER_PRIVATE_LOCKING));
	state->contents = int_array((List *) list_nth(priv, GATHER_PRIVATE_CONTENTS),
								&state->ncontents);
	state->nsegments = intVal(list_nth(priv, GATHER_PRIVATE_NSEGMENTS));
	state->sources = int_array((List *) list_nth(priv, GATHER_PRIVATE_SOURCES),
							   &state->nsources);
	state->attrs = int_array((List *) list_nth(priv, GATHER_PRIVATE_ATTRS),
							 &state->natts);
	state->ctid_remote = intVal(list_nth(priv, GATHER_PRIVATE_CTID));
	state->cursor_name = strVal(list_nth(priv, GATHER_PRIVATE_CURSOR));
	if (state->cursor_name[0] == '\0')
		state->cursor_name = NULL;
	state->cursor_param = intVal(list_nth(priv, GATHER_PRIVATE_CURSOR_PARAM));
	state->identity = boolVal(list_nth(priv, GATHER_PRIVATE_IDENTITY));
	state->limit = strVal(list_nth(priv, GATHER_PRIVATE_LIMIT));
	return (Node *) state;
}

static bool
gather_is_current_of(GatherScanState *state)
{
	return state->cursor_name != NULL || state->cursor_param > 0;
}

static void
gather_begin(CustomScanState *node, EState *estate, int eflags)
{
	GatherScanState *state = (GatherScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	List	   *types = (List *) list_nth(cscan->custom_private, GATHER_PRIVATE_TYPES);
	List	   *typmods = (List *) list_nth(cscan->custom_private, GATHER_PRIVATE_TYPMODS);
	TupleDesc	remote = CreateTemplateTupleDesc(list_length(types));
	int			i = 0;
	ListCell   *lt,
			   *lm;

	forboth(lt, types, lm, typmods)
	{
		i++;
		TupleDescInitEntry(remote, i, NULL, lfirst_oid(lt), lfirst_int(lm), 0);
	}
	TupleDescFinalize(remote);
	state->remote = ExecInitExtraTupleSlot(estate, remote, &TTSOpsVirtual);
	state->rowvalues = palloc_array(Datum, Max(state->natts, 1));
	state->rownulls = palloc_array(bool, Max(state->natts, 1));
	state->folds = ExecInitExprList(cscan->custom_exprs, &node->ss.ps);
	state->current_content = -1;

	/*
	 * A recheck's copy of the plan (gather_epq()), which reads a row or two,
	 * is no slice of the statement's, and reaches no segment it has not.
	 */
	if (estate->es_epq_active != NULL)
	{
		state->epq_row = ExecInitExtraTupleSlot(estate,
												RelationGetDescr(node->ss.ss_currentRelation),
												&TTSOpsVirtual);
		return;
	}

	/*
	 * Each gather is a slice of its own, numbered as the executor meets it --
	 * in a plan EXPLAIN only describes too, whose label and slice table say
	 * it (gp_explain.c).
	 */
	state->slice = GpNextGatherSlice();
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		int			slice = state->slice;

		if (gather_is_current_of(state))
			GpReportDispatch(slice, true, state->nsegments);
		else if (state->ncontents > 0)
		{
			GpReportDispatchContents(slice, state->contents, state->ncontents);
			GpReportDtxReached(estate->es_plannedstmt, state->contents,
							   state->ncontents);
		}
		else
		{
			GpReportDispatch(slice, false, state->nsegments);
			GpReportDtxReached(estate->es_plannedstmt, NULL, state->nsegments);
		}
	}

	/* a keyed gather's rows, kept by their key (add_keyed_gather_paths()) */
	if (list_length(cscan->custom_private) > GATHER_PRIVATE_KEYED &&
		boolVal(list_nth(cscan->custom_private, GATHER_PRIVATE_KEYED)))
		keyed_init(state, cscan, estate);
}

/* The name WHERE CURRENT OF gives the cursor by: its own, or a parameter's. */
static char *
cursor_name_of(GatherScanState *state)
{
	ParamListInfo params = state->css.ss.ps.ps_ExprContext->ecxt_param_list_info;
	int			id = state->cursor_param;

	if (state->cursor_name != NULL)
		return state->cursor_name;

	if (params != NULL && id > 0 && id <= params->numParams)
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, id, false, &prmdata);
		else
			prm = &params->params[id - 1];

		if (OidIsValid(prm->ptype) && !prm->isnull)
		{
			if (prm->ptype != REFCURSOROID)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								id, format_type_be(prm->ptype),
								format_type_be(REFCURSOROID))));
			return TextDatumGetCString(prm->value);
		}
	}
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", id)));
	return NULL;
}

/*
 * The gather of relid in a cursor's plan, as execCurrent.c's
 * search_plan_tree() finds a scan: through an Append -- one of whose inputs
 * only can be on a row -- a Result, a Limit and a subquery; not through
 * anything that holds rows back.
 */
static GatherScanState *
find_cursor_gather(PlanState *node, Oid relid, bool *pending_rescan)
{
	GatherScanState *result = NULL;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_CustomScanState:
			if (((CustomScanState *) node)->methods == &gather_exec_methods &&
				((ScanState *) node)->ss_currentRelation != NULL &&
				RelationGetRelid(((ScanState *) node)->ss_currentRelation) == relid)
				result = (GatherScanState *) node;
			break;
		case T_AppendState:
			{
				AppendState *astate = (AppendState *) node;

				for (int i = 0; i < astate->as_nplans; i++)
				{
					GatherScanState *elem = find_cursor_gather(astate->appendplans[i],
															   relid,
															   pending_rescan);

					if (elem == NULL)
						continue;
					if (result != NULL)
						return NULL;	/* more than one */
					result = elem;
				}
			}
			break;
		case T_ResultState:
		case T_LimitState:
			result = find_cursor_gather(outerPlanState(node), relid,
										pending_rescan);
			break;
		case T_SubqueryScanState:
			result = find_cursor_gather(((SubqueryScanState *) node)->subplan,
										relid, pending_rescan);
			break;
		default:
			break;
	}

	if (result != NULL && node->chgParam != NULL)
		*pending_rescan = true;
	return result;
}

/*
 * WHERE CURRENT OF: where the cursor's current row of this relation is -- its
 * segment and its ctid there -- or false if another relation's scan made it,
 * as execCurrentOf() answers, with its errors.
 */
static bool
gather_current_of(GatherScanState *state, int *content, ItemPointer tid)
{
	Oid			relid = RelationGetRelid(state->css.ss.ss_currentRelation);
	char	   *cursor_name = cursor_name_of(state);
	char	   *table_name = get_rel_name(relid);
	Portal		portal;
	QueryDesc  *queryDesc;
	GatherScanState *cursor;
	bool		pending_rescan = false;
	TupleTableSlot *slot;

	portal = GetPortalByName(cursor_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor_name)));
	if (portal->strategy != PORTAL_ONE_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a SELECT query", cursor_name)));
	queryDesc = portal->queryDesc;
	if (queryDesc == NULL || queryDesc->estate == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is held from a previous transaction",
						cursor_name)));

	cursor = find_cursor_gather(queryDesc->planstate, relid, &pending_rescan);
	if (cursor == NULL || cursor->ctid_remote < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
						cursor_name, table_name)));
	if (portal->atStart || portal->atEnd)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not positioned on a row",
						cursor_name)));

	slot = cursor->css.ss.ss_ScanTupleSlot;
	if (TupIsNull(slot) || pending_rescan || cursor->current_content < 0)
		return false;
	*content = cursor->current_content;
	ItemPointerCopy(&slot->tts_tid, tid);
	return true;
}

PG_FUNCTION_INFO_V1(gp_current_tid);

/*
 * gp_internal.current_tid(rel, ctid)
 *		WHERE CURRENT OF, on the segment that holds the cursor's row: the ctid
 *		of the version of the row at ctid that the statement's snapshot sees,
 *		following its updates since the cursor read it -- as PostgreSQL's TID
 *		scan finds a cursor's row (TidNext(), table_tuple_get_latest_tid()).
 *		A cursor without FOR UPDATE leaves its row free to be updated, and
 *		the old ctid would find the version the update left behind, which
 *		the statement's snapshot no longer sees, and no row.  The ctid as it
 *		is for a table that is not heap's.  The coordinator checked the
 *		statement's privileges where its own connection asks
 *		(gather_start()); anyone else needs SELECT on the table, as
 *		currtid2() does.
 */
Datum
gp_current_tid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer result = palloc_object(ItemPointerData);
	Relation	rel;

	ItemPointerCopy(PG_GETARG_ITEMPOINTER(1), result);
	rel = table_open(relid, AccessShareLock);
	if (!GpClusterDispatchTrusted() &&
		pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (rel->rd_rel->relkind == RELKIND_RELATION &&
		rel->rd_rel->relam == HEAP_TABLE_AM_OID &&
		ItemPointerIsValid(result) &&
		ItemPointerGetBlockNumber(result) < RelationGetNumberOfBlocks(rel))
	{
		TableScanDesc scan = table_beginscan_tid(rel, GetActiveSnapshot());

		table_tuple_get_latest_tid(scan, result);
		table_endscan(scan);
	}
	table_close(rel, AccessShareLock);
	PG_RETURN_ITEMPOINTER(result);
}

/*
 * A plan's cost less what the planner charges its gathers for starting --
 * the round trip to each segment, which Cloudberry's cost model has no
 * counterpart of -- for what a cost is compared with that was set in
 * Cloudberry's units: a resource group's min_cost, under which a query runs
 * without a slot (gp_resource's resgroup.c).  ORCA's plans have no gathers
 * of the planner's.
 */
static void
gather_startup_walker(Plan *plan, int *ngathers)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gather_scan_methods)
		(*ngathers)++;
	gather_startup_walker(plan->lefttree, ngathers);
	gather_startup_walker(plan->righttree, ngathers);
	if (IsA(plan, CustomScan))
	{
		foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
			gather_startup_walker(child, ngathers);
	}
	else if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			gather_startup_walker(child, ngathers);
	}
	else if (IsA(plan, SubqueryScan))
		gather_startup_walker(((SubqueryScan *) plan)->subplan, ngathers);
}

double
GpPlanCostLessGathers(PlannedStmt *stmt)
{
	int			ngathers = 0;
	double		cost;

	gather_startup_walker(stmt->planTree, &ngathers);
	foreach_ptr(Plan, sub, stmt->subplans)
		gather_startup_walker(sub, &ngathers);
	cost = stmt->planTree->total_cost - ngathers * GATHER_STARTUP_COST;
	return cost > 0 ? cost : 0;
}

/*
 * The conditions sent, their stable constant parts computed now, each as a
 * literal of its type in its placeholder's place (fold_stable()); the
 * placeholders are $N outside a quoted string or name.  Computed again at
 * each start, as the executor computes a scan's runtime keys at each rescan.
 */
static char *
gather_where(GatherScanState *state)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	int			nfolds = list_length(state->folds);
	char	  **literals;
	StringInfoData out;
	const char *p;
	int			i = 0;

	if (nfolds == 0 || state->folded[0] == '\0')
		return state->where;

	literals = palloc_array(char *, nfolds);
	foreach_ptr(ExprState, fold, state->folds)
	{
		Oid			type = exprType((Node *) fold->expr);
		int32		typmod = exprTypmod((Node *) fold->expr);
		bool		isnull;
		Datum		value = ExecEvalExprSwitchContext(fold, econtext, &isnull);
		char	   *typname = format_type_with_typemod(type, typmod);

		if (isnull)
			literals[i++] = psprintf("NULL::%s", typname);
		else
		{
			Oid			out_func;
			bool		isvarlena;

			getTypeOutputInfo(type, &out_func, &isvarlena);
			literals[i++] = psprintf("%s::%s",
									 quote_literal_cstr(OidOutputFunctionCall(out_func, value)),
									 typname);
		}
	}

	initStringInfo(&out);
	for (p = state->folded; *p != '\0';)
	{
		if (*p == '\'' || *p == '"')
		{
			char		quote = *p;

			/* a quoted string or name, its doubled quotes inside */
			appendStringInfoChar(&out, *p++);
			while (*p != '\0')
			{
				if (*p == quote && p[1] == quote)
				{
					appendBinaryStringInfo(&out, p, 2);
					p += 2;
					continue;
				}
				appendStringInfoChar(&out, *p);
				if (*p++ == quote)
					break;
			}
			continue;
		}
		if (*p == '$' && isdigit((unsigned char) p[1]))
		{
			char	   *end;
			long		n = strtol(p + 1, &end, 10);

			if (n >= GATHER_FOLD_PARAM && n < GATHER_FOLD_PARAM + nfolds)
			{
				appendStringInfoString(&out, literals[n - GATHER_FOLD_PARAM]);
				p = end;
				continue;
			}
		}
		appendStringInfoChar(&out, *p++);
	}
	return out.data;
}

/*
 * Start reading: from the segments the plan names, or the cursor's one.
 *
 * The coordinator checked the statement's privileges before it ran any of
 * it, the table's among them, as the user each is checked as -- a view's
 * owner for a table the view reads -- and a segment knows only the session's
 * role.  So the query says it was checked (GP_CHECKED_MARKER), and a segment
 * checks none of its privileges, for the coordinator's connection alone, as
 * Cloudberry's segments check none of a SELECT's (InitPlan()).
 */
static bool
gather_start(GatherScanState *state)
{
	TupleDesc	desc = state->remote->tts_tupleDescriptor;
	StringInfoData sql;
	char	   *where = gather_where(state);

	initStringInfo(&sql);
	appendStringInfoString(&sql, GP_CHECKED_MARKER);

	/*
	 * The coordinator's transaction and statement start, for now() and its
	 * kin in the conditions it sends (is_shippable()), which a segment takes
	 * on the coordinator's own connection (gp_motion.c).
	 */
	appendStringInfo(&sql, GP_TIMES_MARKER INT64_FORMAT " " INT64_FORMAT "*/ ",
					 (int64) GetCurrentTransactionStartTimestamp(),
					 (int64) GetCurrentStatementStartTimestamp());
	appendStringInfoString(&sql, state->select);

	if (gather_is_current_of(state))
	{
		int			content;
		ItemPointerData tid;

		if (!gather_current_of(state, &content, &tid))
			return false;

		/*
		 * The row in the version the statement sees, which an update since
		 * the cursor read it moved (gp_current_tid()) -- where the database
		 * has gp_core's extension, and the ctid as the cursor read it where
		 * it has not.
		 */
		if (OidIsValid(get_namespace_oid("gp_internal", true)))
			appendStringInfo(&sql, " WHERE ctid = gp_internal.current_tid(%u::pg_catalog.oid, '(%u,%u)'::pg_catalog.tid)",
							 RelationGetRelid(state->css.ss.ss_currentRelation),
							 ItemPointerGetBlockNumber(&tid),
							 ItemPointerGetOffsetNumber(&tid));
		else
			appendStringInfo(&sql, " WHERE ctid = '(%u,%u)'::pg_catalog.tid",
							 ItemPointerGetBlockNumber(&tid),
							 ItemPointerGetOffsetNumber(&tid));
		appendStringInfo(&sql, "%s%s%s",
						 where[0] != '\0' ? " AND " : "",
						 where, state->locking);
		state->gather = GpGatherStartOn(sql.data, desc, content);
		return true;
	}

	if (where[0] != '\0')
		appendStringInfo(&sql, " WHERE %s", where);
	appendStringInfoString(&sql, state->limit);
	appendStringInfoString(&sql, state->locking);

	/* read to its end: its segments may run it whole (gp_parallel.c) */
	if (state->whole)
		appendStringInfoString(&sql, GP_WHOLE_MARKER);

	/* the segments direct dispatch named, or the table's: all, or the first */
	if (state->ncontents > 0)
		state->gather = GpGatherStartOnContents(sql.data, desc, state->contents,
												state->ncontents);
	else
		state->gather = GpGatherStartOnSegments(sql.data, desc,
												state->nsegments);
	return true;
}

/* The scan tuple, from a row as it arrived from segment "content". */
static void
gather_store(GatherScanState *state, TupleTableSlot *slot, int content)
{
	TupleTableSlot *remote = state->remote;
	Relation	rel = state->css.ss.ss_currentRelation;
	EState	   *estate = state->css.ss.ps.state;

	ExecClearTuple(slot);
	for (int i = 0; i < state->nsources; i++)
	{
		int			src = state->sources[i];

		slot->tts_isnull[i] = false;
		switch (src)
		{
			case GATHER_SRC_TABLEOID:
				slot->tts_values[i] = ObjectIdGetDatum(RelationGetRelid(rel));
				break;
			case GATHER_SRC_SEGMENT:
				slot->tts_values[i] = Int32GetDatum(content);
				break;
			case GATHER_SRC_WHOLEROW:
				{
					TupleDesc	reldesc = RelationGetDescr(rel);
					HeapTuple	tuple;

					for (int a = 0; a < state->natts; a++)
					{
						int			r = state->attrs[a];

						state->rowvalues[a] = r >= 0 ? remote->tts_values[r] : (Datum) 0;
						state->rownulls[a] = r >= 0 ? remote->tts_isnull[r] : true;
					}
					tuple = heap_form_tuple(reldesc, state->rowvalues,
											state->rownulls);
					slot->tts_values[i] = heap_copy_tuple_as_datum(tuple, reldesc);
				}
				break;
			case GATHER_SRC_IDENTITY:
				{
					ItemPointer synthetic = palloc_object(ItemPointerData);

					/* a recheck's row, in the map of the statement's rows */
					GpRowIdentityMake(estate->es_epq_active != NULL
									  ? estate->es_epq_active->parentestate
									  : estate,
									  content,
									  (ItemPointer) DatumGetPointer(remote->tts_values[state->ctid_remote]),
									  synthetic);
					slot->tts_values[i] = PointerGetDatum(synthetic);
				}
				break;
			default:
				slot->tts_values[i] = remote->tts_values[src];
				slot->tts_isnull[i] = remote->tts_isnull[src];
				break;
		}
	}

	/*
	 * A scan that reads no column -- count(*) -- has the relation's columns
	 * in its tuple, where it has no scan list: they are NULL, and a spool
	 * copies a row of them.
	 */
	for (int i = state->nsources; i < slot->tts_tupleDescriptor->natts; i++)
	{
		slot->tts_values[i] = (Datum) 0;
		slot->tts_isnull[i] = true;
	}
	ExecStoreVirtualTuple(slot);

	if (state->ctid_remote >= 0 && !remote->tts_isnull[state->ctid_remote])
		ItemPointerCopy((ItemPointer) DatumGetPointer(remote->tts_values[state->ctid_remote]),
						&slot->tts_tid);
	else
		ItemPointerSetInvalid(&slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
	state->current_content = content;
}

/*
 * The segments' cursors closed.  What their statements say of their run as
 * they end, under EXPLAIN ANALYZE, is this gather's (gp_explain.c).
 */
static void
gather_close(GatherScanState *state)
{
	PlanState  *prev = GpExplainAnswerFor(&state->css.ss.ps);

	GpGatherEnd(state->gather);
	(void) GpExplainAnswerFor(prev);
	state->gather = NULL;
}

/* The segments' next row, into the scan slot; false when they have no more. */
static bool
gather_fetch(GatherScanState *state, TupleTableSlot *slot)
{
	ScanState  *ss = &state->css.ss;
	MemoryContext oldcxt;
	int			content;
	bool		got;

	if (state->done)
		return false;

	if (state->gather == NULL && !gather_start(state))
	{
		state->done = true;
		state->current_content = -1;
		return false;
	}

	/*
	 * The row's values live in the per-tuple context, which ExecScan resets
	 * before asking for the next one; the query's own context would keep every
	 * row the scan ever read.
	 */
	oldcxt = MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	got = GpGatherNext(state->gather, state->remote, &content);
	if (got)
	{
		slot_getallattrs(state->remote);
		gather_store(state, slot, content);
		if (state->spool != NULL)
			tuplestore_puttupleslot(state->spool, slot);
	}
	MemoryContextSwitchTo(oldcxt);

	if (got)
		return true;

	gather_close(state);
	state->done = true;
	state->current_content = -1;
	return false;
}

/*
 * A keyed gather (add_keyed_gather_paths()): the rows it gathered, kept by
 * the hash of their key, for each run of the nested loop it is the inner side
 * of -- as a hash join keeps its inner side's rows, which it would be but for
 * enable_hashjoin.  Its keys are the scan's conditions that are a hash join's
 * equality of an expression of the row and one of what a run sets, the loop's
 * parameters.  A row whose key is NULL equals nothing, the operator being
 * strict, and is not kept.  Past hash_mem the rows are all kept in a
 * tuplestore that spills, and each run is given them all, the conditions to
 * choose from: what a gather the loop reads again does without keys.
 */
typedef struct KeyedRows
{
	uint32		hash;			/* the key's hash, the entry's own key */
	List	   *rows;			/* the rows whose key hashes so, each a
								 * MinimalTuple */
} KeyedRows;

typedef struct KeyedGather
{
	int			nkeys;
	List	   *keys;			/* each key, of the scan tuple, as planned */
	ExprState **key;			/* and as run */
	ExprState **probe;			/* what it equals, of what a run sets */
	FmgrInfo   *key_hash;		/* the hash function of each */
	FmgrInfo   *probe_hash;
	Oid		   *collations;
	MemoryContext cxt;			/* the table and the rows it keeps */
	HTAB	   *table;			/* hash -> KeyedRows */
	Size		size;			/* the bytes of the rows kept */
	Tuplestorestate *all;		/* past hash_mem: every row, or NULL */
	bool		built;			/* read from the segments to their end */
	bool		probed;			/* this run's rows looked up */
	List	   *rows;			/* this run's rows, */
	ListCell   *next;			/* and the one to give next */
	TupleTableSlot *row;		/* a row kept, read back */
} KeyedGather;

/* What an expression reads: 1 a column of the scan tuple, 2 a parameter. */
static bool
keyed_reads_walker(Node *node, int *reads)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		*reads |= 1;
	else if (IsA(node, Param))
		*reads |= 2;
	return expression_tree_walker(node, keyed_reads_walker, reads);
}

static int
keyed_reads(Node *node)
{
	int			reads = 0;

	(void) keyed_reads_walker(node, &reads);
	return reads;
}

/*
 * The keys, from the scan's conditions as the executor has them: a column of
 * the scan tuple's on one side, and on the other what is the same for a whole
 * run -- the loop's parameters, a constant, an initplan's value.
 */
static void
keyed_init(GatherScanState *state, CustomScan *cscan, EState *estate)
{
	KeyedGather *kg = palloc0_object(KeyedGather);
	List	   *probes = NIL;
	List	   *key_fns = NIL;
	List	   *probe_fns = NIL;
	List	   *collations = NIL;
	HASHCTL		ctl;
	int			i = 0;

	foreach_ptr(Expr, clause, cscan->scan.plan.qual)
	{
		OpExpr	   *op = (OpExpr *) clause;
		RegProcedure left_fn;
		RegProcedure right_fn;
		Node	   *left;
		Node	   *right;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2 ||
			contain_volatile_functions((Node *) op))
			continue;
		left = linitial(op->args);
		right = lsecond(op->args);
		if (!get_op_hash_functions_ext(op->opno, exprType(left), &left_fn, &right_fn))
			continue;
		if (keyed_reads(left) == 1 && (keyed_reads(right) & 1) == 0)
		{
			kg->keys = lappend(kg->keys, left);
			probes = lappend(probes, right);
			key_fns = lappend_oid(key_fns, left_fn);
			probe_fns = lappend_oid(probe_fns, right_fn);
		}
		else if (keyed_reads(right) == 1 && (keyed_reads(left) & 1) == 0)
		{
			kg->keys = lappend(kg->keys, right);
			probes = lappend(probes, left);
			key_fns = lappend_oid(key_fns, right_fn);
			probe_fns = lappend_oid(probe_fns, left_fn);
		}
		else
			continue;
		collations = lappend_oid(collations, op->inputcollid);
	}

	/* none: the gather keeps what it read as any the loop reads again does */
	if (kg->keys == NIL)
		return;

	kg->nkeys = list_length(kg->keys);
	kg->key = palloc_array(ExprState *, kg->nkeys);
	kg->probe = palloc_array(ExprState *, kg->nkeys);
	kg->key_hash = palloc_array(FmgrInfo, kg->nkeys);
	kg->probe_hash = palloc_array(FmgrInfo, kg->nkeys);
	kg->collations = palloc_array(Oid, kg->nkeys);
	foreach_ptr(Expr, key, kg->keys)
	{
		kg->key[i] = ExecInitExpr(key, &state->css.ss.ps);
		kg->probe[i] = ExecInitExpr((Expr *) list_nth(probes, i), &state->css.ss.ps);
		fmgr_info(list_nth_oid(key_fns, i), &kg->key_hash[i]);
		fmgr_info(list_nth_oid(probe_fns, i), &kg->probe_hash[i]);
		kg->collations[i] = list_nth_oid(collations, i);
		i++;
	}
	kg->cxt = AllocSetContextCreate(estate->es_query_cxt, "keyed gather",
									ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(KeyedRows);
	ctl.hcxt = kg->cxt;
	kg->table = hash_create("keyed gather", 1024, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	kg->row = ExecInitExtraTupleSlot(estate,
									 state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor,
									 &TTSOpsMinimalTuple);
	state->keyed = kg;
}

/* The hash of a row's key, or of a run's value; false where one is NULL. */
static bool
keyed_hash(KeyedGather *kg, ExprState **exprs, FmgrInfo *fns,
		   ExprContext *econtext, uint32 *hash)
{
	uint32		h = 0;

	for (int i = 0; i < kg->nkeys; i++)
	{
		bool		isnull;
		Datum		value = ExecEvalExprSwitchContext(exprs[i], econtext, &isnull);

		if (isnull)
			return false;
		h = hash_combine(h, DatumGetUInt32(FunctionCall1Coll(&fns[i],
															 kg->collations[i],
															 value)));
	}
	*hash = h;
	return true;
}

/* Past hash_mem: every row kept in a tuplestore, the table given up. */
static void
keyed_spill(GatherScanState *state)
{
	KeyedGather *kg = state->keyed;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->css.ss.ps.state->es_query_cxt);
	HASH_SEQ_STATUS seq;
	KeyedRows  *entry;

	kg->all = tuplestore_begin_heap(false, false, work_mem);
	MemoryContextSwitchTo(oldcxt);
	hash_seq_init(&seq, kg->table);
	while ((entry = (KeyedRows *) hash_seq_search(&seq)) != NULL)
	{
		foreach_ptr(MinimalTupleData, tuple, entry->rows)
		{
			ExecStoreMinimalTuple(tuple, kg->row, false);
			tuplestore_puttupleslot(kg->all, kg->row);
		}
	}
	ExecClearTuple(kg->row);
	kg->table = NULL;
	MemoryContextReset(kg->cxt);
}

/* The rows from the segments, to their end, each kept by its key. */
static void
keyed_build(GatherScanState *state, TupleTableSlot *slot)
{
	KeyedGather *kg = state->keyed;
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	Size		limit = get_hash_memory_limit();

	while (gather_fetch(state, slot))
	{
		uint32		hash;

		econtext->ecxt_scantuple = slot;
		if (keyed_hash(kg, kg->key, kg->key_hash, econtext, &hash))
		{
			if (kg->all != NULL)
				tuplestore_puttupleslot(kg->all, slot);
			else
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(kg->cxt);
				MinimalTuple tuple = ExecCopySlotMinimalTuple(slot);
				KeyedRows  *entry;
				bool		found;

				entry = (KeyedRows *) hash_search(kg->table, &hash, HASH_ENTER, &found);
				if (!found)
					entry->rows = NIL;
				entry->rows = lappend(entry->rows, tuple);
				kg->size += GetMemoryChunkSpace(tuple) + sizeof(ListCell);
				MemoryContextSwitchTo(oldcxt);
				if (kg->size > limit)
					keyed_spill(state);
			}
		}
		ResetExprContext(econtext);
	}
	if (kg->all != NULL)
		tuplestore_rescan(kg->all);
	kg->built = true;
}

/*
 * The next row of this run's key, into the scan slot: the rows of the key
 * the run's value hashes to, which the scan's conditions check -- or past
 * hash_mem every row.  False when the run has no more.
 */
static bool
keyed_fetch(GatherScanState *state, TupleTableSlot *slot)
{
	KeyedGather *kg = state->keyed;

	if (!kg->built)
		keyed_build(state, slot);

	if (kg->all != NULL)
	{
		if (!tuplestore_gettupleslot(kg->all, true, false, kg->row))
			return false;
	}
	else
	{
		if (!kg->probed)
		{
			KeyedRows  *entry = NULL;
			uint32		hash;

			if (keyed_hash(kg, kg->probe, kg->probe_hash,
						   state->css.ss.ps.ps_ExprContext, &hash))
				entry = (KeyedRows *) hash_search(kg->table, &hash, HASH_FIND, NULL);
			kg->rows = entry != NULL ? entry->rows : NIL;
			kg->next = list_head(kg->rows);
			kg->probed = true;
		}
		if (kg->next == NULL)
			return false;
		ExecStoreMinimalTuple((MinimalTuple) lfirst(kg->next), kg->row, false);
		kg->next = lnext(kg->rows, kg->next);
	}
	ExecCopySlot(slot, kg->row);
	slot->tts_tableOid = RelationGetRelid(state->css.ss.ss_currentRelation);
	return true;
}

/* Another run: its rows are looked up again, of the value it sets. */
static void
keyed_rescan(KeyedGather *kg)
{
	kg->probed = false;
	kg->rows = NIL;
	kg->next = NULL;
	if (kg->all != NULL)
		tuplestore_rescan(kg->all);
}

static void
keyed_end(KeyedGather *kg)
{
	if (kg->all != NULL)
		tuplestore_end(kg->all);
	kg->all = NULL;
	MemoryContextDelete(kg->cxt);
}

static TupleTableSlot *
gather_next(ScanState *ss)
{
	GatherScanState *state = (GatherScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	/*
	 * Read again, what was kept is read first; the segments are read further
	 * only past its end, as a Materialize reads its child.
	 */
	if (state->spool != NULL && !tuplestore_ateof(state->spool) &&
		tuplestore_gettupleslot(state->spool, true, false, state->spooled))
	{
		ExecCopySlot(slot, state->spooled);
		slot->tts_tableOid = RelationGetRelid(ss->ss_currentRelation);
		return slot;
	}

	/* a keyed gather's, the rows of this run's key (keyed_fetch()) */
	if (state->keyed != NULL ? keyed_fetch(state, slot) : gather_fetch(state, slot))
		return slot;
	return ExecClearTuple(slot);
}

static bool
gather_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

/*
 * A recheck's row, read again from segment "content" at "tid" into the scan
 * slot, with the conditions the gather sends: under the statement's
 * snapshot, or -- the newest version of a row the explicit write rechecks,
 * which that snapshot does not see -- under a snapshot taken now.  False
 * when the row is not there, or the conditions do not hold for it.
 */
static bool
gather_epq_fetch(GatherScanState *state, int content, ItemPointer tid,
				 bool latest, TupleTableSlot *slot)
{
	TupleDesc	desc = state->remote->tts_tupleDescriptor;
	char	   *where = gather_where(state);
	StringInfoData sql;
	GpGatherState *gather;
	MemoryContext oldcxt;
	int			from;
	bool		got;

	initStringInfo(&sql);
	appendStringInfoString(&sql, GP_CHECKED_MARKER);
	appendStringInfo(&sql, GP_TIMES_MARKER INT64_FORMAT " " INT64_FORMAT "*/ ",
					 (int64) GetCurrentTransactionStartTimestamp(),
					 (int64) GetCurrentStatementStartTimestamp());
	appendStringInfo(&sql, "%s WHERE ctid = '(%u,%u)'::pg_catalog.tid%s%s",
					 state->select, ItemPointerGetBlockNumber(tid),
					 ItemPointerGetOffsetNumber(tid),
					 where[0] != '\0' ? " AND " : "", where);

	if (latest)
		PushActiveSnapshot(GetLatestSnapshot());
	gather = GpGatherStartOn(sql.data, desc, content);
	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	got = GpGatherNext(gather, state->remote, &from);
	if (got)
	{
		slot_getallattrs(state->remote);
		gather_store(state, slot, content);
	}
	MemoryContextSwitchTo(oldcxt);
	GpGatherEnd(gather);
	if (latest)
		PopActiveSnapshot();
	pfree(sql.data);
	return got;
}

/*
 * A row mark's copy of its table's row (ROW_MARK_COPY: an external table's,
 * whose rows have no ctid to be read by again), made the scan tuple as
 * gather_store() makes one of a row the segments sent.
 */
static bool
gather_epq_copy(GatherScanState *state, EPQState *epq, Index rti,
				TupleTableSlot *slot)
{
	TupleTableSlot *row = state->epq_row;
	TupleTableSlot *remote = state->remote;
	MemoryContext oldcxt;

	if (!EvalPlanQualFetchRowMark(epq, rti, row))
		return false;
	oldcxt = MemoryContextSwitchTo(state->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	slot_getallattrs(row);
	ExecClearTuple(remote);
	for (int i = 0; i < remote->tts_tupleDescriptor->natts; i++)
		remote->tts_isnull[i] = true;
	for (int a = 0; a < state->natts; a++)
	{
		if (state->attrs[a] < 0)
			continue;
		remote->tts_values[state->attrs[a]] = row->tts_values[a];
		remote->tts_isnull[state->attrs[a]] = row->tts_isnull[a];
	}
	if (state->ctid_remote >= 0 && ItemPointerIsValid(&row->tts_tid))
	{
		ItemPointer tid = palloc_object(ItemPointerData);

		ItemPointerCopy(&row->tts_tid, tid);
		remote->tts_values[state->ctid_remote] = PointerGetDatum(tid);
		remote->tts_isnull[state->ctid_remote] = false;
	}
	ExecStoreVirtualTuple(remote);
	gather_store(state, slot, -1);
	MemoryContextSwitchTo(oldcxt);
	return true;
}

/*
 * EvalPlanQual: the explicit write rechecks a row another transaction
 * updated since the plan read it (gp_explicit.c) by running its plan again,
 * each scan giving one row, as PostgreSQL's recheck does
 * (ExecScanFetch()).  The gather of the table being written gives the
 * newest version of the row, which the explicit write names by the ctid the
 * statement's map knows it by, read from its segment under a snapshot that
 * sees it; the gather of each other table the plan read by a row mark gives
 * the row the plan joined it to, whose ctid and segment the plan's row
 * carries (add_segment_junk()), read from that segment under the
 * statement's snapshot -- or the row a row mark copied.  Either with the
 * conditions the gather sends, and then those it evaluates here and its
 * projection, as ExecScan() applies them.  A table the plan reads only in a
 * subquery is read as always; one the recheck gives no row of, none.
 */
static TupleTableSlot *
gather_epq(GatherScanState *state, EPQState *epq)
{
	ScanState  *ss = &state->css.ss;
	Index		rti = ((Scan *) ss->ps.plan)->scanrelid;
	ExprContext *econtext = ss->ps.ps_ExprContext;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	bool		found;

	if (epq->relsubs_done[rti - 1])
		return ExecClearTuple(slot);
	epq->relsubs_done[rti - 1] = true;
	ResetExprContext(econtext);

	if (epq->relsubs_slot[rti - 1] != NULL)
	{
		TupleTableSlot *test = epq->relsubs_slot[rti - 1];
		int			content;
		ItemPointerData tid;

		/* the row the explicit write rechecks, by the ctid its map has */
		if (!state->identity)
			GpMotionRefuseRecheck();
		if (TupIsNull(test))
			return ExecClearTuple(slot);
		if (!GpRowIdentityFind(epq->parentestate, &test->tts_tid, &content, &tid))
			elog(ERROR, "a row to recheck was not read from a segment");
		found = gather_epq_fetch(state, content, &tid, true, slot);
	}
	else
	{
		ExecAuxRowMark *earm = epq->relsubs_rowmark[rti - 1];
		ExecRowMark *erm = earm->rowmark;
		Datum		datum;
		Datum		segment;
		bool		isnull;

		/* a child's row mark, for a row another child gave */
		if (erm->rti != erm->prti)
		{
			datum = ExecGetJunkAttribute(epq->origslot, earm->toidAttNo, &isnull);
			if (isnull || DatumGetObjectId(datum) != erm->relid)
				return ExecClearTuple(slot);
		}

		if (erm->markType == ROW_MARK_COPY)
		{
			/*
			 * A copy of the row, as the plan carried it -- a child's as its
			 * parent's row, which the scan's is not, refused
			 */
			datum = ExecGetJunkAttribute(epq->origslot, earm->wholeAttNo, &isnull);
			if (!isnull &&
				HeapTupleHeaderGetTypeId(DatumGetHeapTupleHeader(datum)) !=
				RelationGetDescr(ss->ss_currentRelation)->tdtypeid)
				GpMotionRefuseRecheck();
			found = gather_epq_copy(state, epq, rti, slot);
		}
		else
		{
			/* the row's segment, carried with its ctid */
			if (state->epq_segcol == 0)
			{
				char		resname[32];

				snprintf(resname, sizeof(resname), GP_SEGMENT_JUNK, erm->rowmarkId);
				state->epq_segcol = ExecFindJunkAttributeInTlist(epq->plan->targetlist,
																 resname);
				if (!AttributeNumberIsValid(state->epq_segcol))
					state->epq_segcol = -1;
			}
			if (state->epq_segcol < 0)
				GpMotionRefuseRecheck();

			/* a row an outer join's null row stood for: none */
			datum = ExecGetJunkAttribute(epq->origslot, earm->ctidAttNo, &isnull);
			if (isnull)
				return ExecClearTuple(slot);
			segment = ExecGetJunkAttribute(epq->origslot, state->epq_segcol, &isnull);
			if (isnull)
				return ExecClearTuple(slot);
			found = gather_epq_fetch(state, DatumGetInt32(segment),
									 (ItemPointer) DatumGetPointer(datum), false,
									 slot);
		}
	}
	if (!found)
		return ExecClearTuple(slot);

	econtext->ecxt_scantuple = slot;
	if (ss->ps.qual != NULL && !ExecQual(ss->ps.qual, econtext))
	{
		InstrCountFiltered1(ss, 1);
		return ExecClearTuple(slot);
	}
	if (ss->ps.ps_ProjInfo != NULL)
		return ExecProject(ss->ps.ps_ProjInfo);
	return slot;
}

static TupleTableSlot *
gather_exec(CustomScanState *node)
{
	EPQState   *epq = node->ss.ps.state->es_epq_active;

	/* a recheck (gather_epq()), where the table is one it gives a row of */
	if (epq != NULL)
	{
		Index		rti = ((Scan *) node->ss.ps.plan)->scanrelid;

		if (epq->relsubs_done[rti - 1] || epq->relsubs_slot[rti - 1] != NULL ||
			epq->relsubs_rowmark[rti - 1] != NULL)
			return gather_epq((GatherScanState *) node, epq);
	}
	return ExecScan(&node->ss, gather_next, gather_recheck);
}

bool
GpGatherScanStartEarly(PlanState *ps)
{
	GatherScanState *state = (GatherScanState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &gather_exec_methods)
		return false;

	/* WHERE CURRENT OF reads the row its cursor is on when it runs */
	if (!gather_is_current_of(state) && state->gather == NULL && !state->done)
		(void) gather_start(state);
	return true;
}

static void
gather_end(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->gather != NULL)
		gather_close(state);
	if (state->spool != NULL)
		tuplestore_end(state->spool);
	state->spool = NULL;
	if (state->keyed != NULL)
		keyed_end(state->keyed);
	state->keyed = NULL;
}

/*
 * Read again: what it kept is read again (GpGatherScanMarkRescans()), a keyed
 * gather's rows of the key the parameters now give -- or, where it keeps
 * nothing, the segments run the query again.  The query the segments run has
 * no parameter in it, which is what makes what it read the answer whatever
 * the parameters now are.
 */
static void
gather_rescan(CustomScanState *node)
{
	GatherScanState *state = (GatherScanState *) node;

	if (state->keyed != NULL && state->keyed->built)
	{
		keyed_rescan(state->keyed);
		return;
	}

	if (state->spool != NULL)
	{
		tuplestore_rescan(state->spool);
		return;
	}

	if (state->gather != NULL)
		gather_close(state);
	state->done = false;
	state->current_content = -1;
}

/*
 * A gather that the plan may read more than once -- the inner side of a
 * nested loop, a subquery run for each row, the recursive part of WITH
 * RECURSIVE -- keeps the rows it read and reads them again, as Cloudberry's
 * planner makes sure a Motion or an external scan it would rescan is read
 * once, by putting a Materialize above it (neither path is "rescannable").
 * An external table's source is read once: a command runs once, a file's
 * rejected rows are counted once, and gpfdist serves a scan once.  A table's
 * rows cross from the segments once: run again for each outer row, the
 * gathers of a join's inner side of many partitions -- a Nested Loop the
 * planner chose for tables it had no statistics of -- took minutes, where
 * reading what they kept takes seconds (bb_mpph's queries 3 and 8).  A
 * gather that brings each row's ctid -- of a table the statement writes, or
 * locks rows of -- is run again, since a row it keeps has none.  A
 * Materialize that already keeps the rows, above a subtree with no parameter
 * to change, reads them once.  A keyed gather keeps its rows by their key
 * itself (keyed_init()).
 */
static void mark_rescans(PlanState *ps, bool again);

static void
mark_rescans_list(List *subplans, bool again)
{
	foreach_node(SubPlanState, sps, subplans)
		mark_rescans(sps->planstate, again);
}

static void
mark_rescans_array(PlanState **planstates, int n, bool again)
{
	for (int i = 0; i < n; i++)
		mark_rescans(planstates[i], again);
}

static void
mark_rescans(PlanState *ps, bool again)
{
	if (ps == NULL)
		return;
	check_stack_depth();

	if (again && IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) ps;

		if (state->spool == NULL && state->keyed == NULL &&
			state->ctid_remote < 0 && !gather_is_current_of(state))
		{
			EState	   *estate = ps->state;
			MemoryContext oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);

			state->spool = tuplestore_begin_heap(false, false, work_mem);
			state->spooled =
				ExecInitExtraTupleSlot(estate,
									   state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor,
									   &TTSOpsMinimalTuple);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	/* an initplan runs again as what it belongs to does; a subplan per row */
	mark_rescans_list(ps->initPlan, again);
	mark_rescans_list(ps->subPlan, true);

	switch (nodeTag(ps))
	{
		case T_NestLoopState:
		case T_RecursiveUnionState:
			mark_rescans(outerPlanState(ps), again);
			mark_rescans(innerPlanState(ps), true);
			return;
		case T_MaterialState:
			if ((((MaterialState *) ps)->eflags & EXEC_FLAG_REWIND) &&
				bms_is_empty(outerPlanState(ps)->plan->allParam))
				again = false;
			break;
		case T_AppendState:
			mark_rescans_array(((AppendState *) ps)->appendplans,
							   ((AppendState *) ps)->as_nplans, again);
			break;
		case T_MergeAppendState:
			mark_rescans_array(((MergeAppendState *) ps)->mergeplans,
							   ((MergeAppendState *) ps)->ms_nplans, again);
			break;
		case T_SubqueryScanState:
			mark_rescans(((SubqueryScanState *) ps)->subplan, again);
			break;
		case T_CustomScanState:
			foreach_ptr(PlanState, child, ((CustomScanState *) ps)->custom_ps)
				mark_rescans(child, again);
			break;
		default:
			break;
	}
	mark_rescans(outerPlanState(ps), again);
	mark_rescans(innerPlanState(ps), again);
}

void
GpGatherScanMarkRescans(PlanState *root)
{
	mark_rescans(root, false);
}

/*
 * The gathers a statement reads to their end, whose segments may run their
 * query whole, with parallel workers where PostgreSQL's planner there finds
 * that they pay (gp_parallel.c).  A node that reads all of its input before
 * it returns a row -- a Sort, a Hash, a plain or hashed Agg, a hashed SetOp,
 * a ModifyTable, a hashed SubPlan -- reads a gather below it to its end
 * whenever it runs at all; a Limit, the inner side of a NestLoop, either
 * side of a merge join, any other SubPlan and a CustomScan of another's may
 * stop short; the rest read their input as far as what reads them does.  A
 * gather that sends its LIMIT is read no further than it, and marked where
 * the walk reaches it, and a keyed gather keeps all it reads.  Not a gather
 * of rows being changed or locked or of a cursor's rows, which are read as
 * they are wanted.
 */
static void mark_whole(PlanState *ps, bool whole);

static void
mark_whole_array(PlanState **planstates, int n, bool whole)
{
	for (int i = 0; i < n; i++)
		mark_whole(planstates[i], whole);
}

static void
mark_whole(PlanState *ps, bool whole)
{
	if (ps == NULL)
		return;
	check_stack_depth();

	if (IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) ps;

		state->whole = (whole || state->keyed != NULL || state->limit[0] != '\0') &&
			!state->identity && state->ctid_remote < 0 &&
			state->locking[0] == '\0' && !gather_is_current_of(state);
		return;
	}

	foreach_node(SubPlanState, sps, ps->initPlan)
		mark_whole(sps->planstate, false);
	foreach_node(SubPlanState, sps, ps->subPlan)
		mark_whole(sps->planstate, sps->subplan->useHashTable);

	switch (nodeTag(ps))
	{
		case T_SortState:
		case T_HashState:
		case T_ModifyTableState:
			whole = true;
			break;
		case T_AggState:
			if (((Agg *) ps->plan)->aggstrategy == AGG_PLAIN ||
				((Agg *) ps->plan)->aggstrategy == AGG_HASHED)
				whole = true;
			break;
		case T_SetOpState:
			if (((SetOp *) ps->plan)->strategy == SETOP_HASHED)
				whole = true;
			break;
		case T_LimitState:
		case T_MergeJoinState:
		case T_RecursiveUnionState:
			whole = false;
			break;
		case T_NestLoopState:
			mark_whole(outerPlanState(ps), whole);
			mark_whole(innerPlanState(ps), false);
			return;
		case T_AppendState:
			mark_whole_array(((AppendState *) ps)->appendplans,
							 ((AppendState *) ps)->as_nplans, whole);
			break;
		case T_MergeAppendState:
			mark_whole_array(((MergeAppendState *) ps)->mergeplans,
							 ((MergeAppendState *) ps)->ms_nplans, whole);
			break;
		case T_SubqueryScanState:
			mark_whole(((SubqueryScanState *) ps)->subplan, whole);
			break;
		case T_CustomScanState:
			foreach_ptr(PlanState, child, ((CustomScanState *) ps)->custom_ps)
				mark_whole(child, false);
			whole = false;
			break;
		default:
			break;
	}
	mark_whole(outerPlanState(ps), whole);
	mark_whole(innerPlanState(ps), whole);
}

void
GpGatherScanMarkWhole(PlanState *root, bool whole)
{
	mark_whole(root, whole);
}

/*
 * A LIMIT above a gather, with nothing between them that drops a row: the
 * segments need send no more than its rows and its OFFSET's, and the query
 * sent them says so, as Cloudberry's planner puts a Limit below its Gather
 * Motion.  A segment's scan stops where the query does -- an external
 * table's, before the rows further on it would have rejected.
 */
static bool
limit_value(Node *expr, int64 *value)
{
	if (expr == NULL)
	{
		*value = 0;
		return true;
	}
	if (!IsA(expr, Const) || ((Const *) expr)->constisnull ||
		((Const *) expr)->consttype != INT8OID)
		return false;
	*value = DatumGetInt64(((Const *) expr)->constvalue);
	return *value >= 0;
}

static void
bound_gathers(Plan *plan, int64 bound)
{
	if (plan == NULL)
		return;
	check_stack_depth();

	switch (nodeTag(plan))
	{
		case T_Limit:
			{
				Limit	   *limit = (Limit *) plan;
				int64		count;
				int64		offset;

				if (limit->limitCount != NULL &&
					limit->limitOption == LIMIT_OPTION_COUNT &&
					limit_value(limit->limitCount, &count) &&
					limit_value(limit->limitOffset, &offset) &&
					count <= PG_INT64_MAX - offset)
					bound_gathers(plan->lefttree, count + offset);
				else
					bound_gathers(plan->lefttree, -1);
				return;
			}
		case T_Result:
			/* a projection, or a condition on no row in particular */
			bound_gathers(plan->lefttree, plan->qual == NIL ? bound : -1);
			return;
		case T_SubqueryScan:
			bound_gathers(((SubqueryScan *) plan)->subplan,
						  plan->qual == NIL ? bound : -1);
			return;
		case T_Append:
			/* UNION ALL: no branch needs to give more than the whole */
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				bound_gathers(child, plan->qual == NIL ? bound : -1);
			return;
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods == &gather_scan_methods)
				{
					if (bound >= 0 && plan->qual == NIL &&
						list_length(cscan->custom_private) > GATHER_PRIVATE_LIMIT)
						list_nth_cell(cscan->custom_private,
									  GATHER_PRIVATE_LIMIT)->ptr_value =
							makeString(psprintf(" LIMIT " INT64_FORMAT, bound));
					return;
				}
				foreach_ptr(Plan, child, cscan->custom_plans)
					bound_gathers(child, -1);
				break;
			}
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				bound_gathers(child, -1);
			break;
		case T_BitmapAnd:
			foreach_ptr(Plan, child, ((BitmapAnd *) plan)->bitmapplans)
				bound_gathers(child, -1);
			break;
		case T_BitmapOr:
			foreach_ptr(Plan, child, ((BitmapOr *) plan)->bitmapplans)
				bound_gathers(child, -1);
			break;
		default:
			break;
	}
	bound_gathers(plan->lefttree, -1);
	bound_gathers(plan->righttree, -1);
}

void
GpScanBoundGathers(PlannedStmt *stmt)
{
	bound_gathers(stmt->planTree, -1);
	foreach_ptr(Plan, sub, stmt->subplans)
		bound_gathers(sub, -1);
}

static void
gather_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GatherScanState *state = (GatherScanState *) node;

	if (gather_is_current_of(state))
		ExplainPropertyInteger("Segments", NULL, 1, es);
	else if (state->ncontents == 1)
		ExplainPropertyInteger("Segment", NULL, state->contents[0], es);
	else if (state->ncontents > 1)
	{
		List	   *contents = NIL;

		for (int i = 0; i < state->ncontents; i++)
			contents = lappend(contents, psprintf("%d", state->contents[i]));
		ExplainPropertyList("Segments", contents, es);
	}
	else
		ExplainPropertyInteger("Segments", NULL, state->nsegments, es);

	/* a keyed gather's keys, which its rows are kept by */
	if (state->keyed != NULL)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt,
													   node->ss.ps.plan, ancestors);
		List	   *keys = NIL;

		foreach_ptr(Node, key, state->keyed->keys)
			keys = lappend(keys, deparse_expression(key, context, es->verbose, false));
		ExplainPropertyList("Lookup Key", keys, es);
	}

	if (es->verbose)
	{
		StringInfoData sql;

		initStringInfo(&sql);
		appendStringInfoString(&sql, state->select);
		if (gather_is_current_of(state))
			appendStringInfo(&sql, " WHERE CURRENT OF %s%s%s",
							 state->cursor_name != NULL ? quote_identifier(state->cursor_name)
							 : psprintf("$%d", state->cursor_param),
							 state->where[0] != '\0' ? " AND " : "",
							 state->where);
		else if (state->where[0] != '\0')
			appendStringInfo(&sql, " WHERE %s", state->where);
		if (!gather_is_current_of(state))
			appendStringInfoString(&sql, state->limit);
		appendStringInfoString(&sql, state->locking);
		ExplainPropertyText("Remote SQL", sql.data, es);
	}
}

/*
 * What EXPLAIN calls the node, through O4.  Cloudberry prints its Gather
 * Motion over the scan as two lines, "Gather Motion 2:1  (slice1; segments:
 * 2)" and the scan beneath; the port's node is both, so it is one line that
 * says both -- the motion, the table it reads, and how many segments.
 */
static explain_node_label_hook_type prev_explain_node_label = NULL;

static void
gp_scan_explain_label(PlanState *planstate, ExplainState *es,
					  const char **pname, const char **suffix)
{
	CustomScanState *node = (CustomScanState *) planstate;

	if (IsA(planstate, CustomScanState) && node->methods == &gather_exec_methods)
	{
		GatherScanState *state = (GatherScanState *) node;
		int			nsegs = gather_is_current_of(state) ? 1
			: state->ncontents > 0 ? state->ncontents : state->nsegments;

		*pname = psprintf("Gather Motion %d:1", nsegs);
		*suffix = psprintf("  (slice%d; segments: %d)", state->slice, nsegs);
		return;
	}

	if (prev_explain_node_label)
		prev_explain_node_label(planstate, es, pname, suffix);
}

/*
 * EXPLAIN ANALYZE's end of a gather a LIMIT above it left open: its
 * segments' cursors closed now, so that what they say of their run is in
 * before the plan is printed (gp_explain.c).  Nothing reads it after
 * ExecutorFinish.
 */
bool
GpGatherScanFinish(PlanState *ps)
{
	GatherScanState *state = (GatherScanState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &gather_exec_methods)
		return false;
	if (state->gather != NULL)
		gather_close(state);
	state->done = true;
	return true;
}

/*
 * Is this node a gather?  Its slice, and how many segments it reads, for
 * EXPLAIN's slice table (gp_explain.c).
 */
bool
GpGatherScanSlice(PlanState *ps, int *slice, int *nsegs)
{
	GatherScanState *state = (GatherScanState *) ps;

	if (!IsA(ps, CustomScanState) ||
		((CustomScanState *) ps)->methods != &gather_exec_methods)
		return false;
	*slice = state->slice;
	*nsegs = gather_is_current_of(state) ? 1
		: state->ncontents > 0 ? state->ncontents : state->nsegments;
	return true;
}

void
GpScanInit(void)
{
	if (GpClusterIsSingleNode())
		return;

	RegisterCustomScanMethods(&gather_scan_methods);

	prev_explain_node_label = explain_node_label_hook;
	explain_node_label_hook = gp_scan_explain_label;

	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gp_set_rel_pathlist;

	prev_build_simple_rel = build_simple_rel_hook;
	build_simple_rel_hook = gp_build_simple_rel;
}
