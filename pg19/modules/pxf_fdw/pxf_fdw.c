/*
 * pxf_fdw.c
 *		  Foreign-data wrapper for PXF (Platform Extension Framework)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_fdw.c
 *
 * Ported to PostgreSQL 19:
 *   - Cloudberry's COPY FROM reads a caller's data through its callback and
 *     handles its bad rows itself (SEGMENT REJECT LIMIT, LOG ERRORS);
 *     PostgreSQL 19's does neither, and gp_exttable does both for its own
 *     scans, so a PXF table's rows are read by gp_exttable's scan of the data
 *     PxfBridgeRead() reads (access/external.h), its error context an
 *     external table's;
 *   - Cloudberry's COPY TO exports its per-row writer (CopyOneRowTo());
 *     PostgreSQL 19's keeps it static, so a row sent to PXF is made by
 *     gp_exttable's writer, from COPY TO's options (Cloudberry's NEWLINE and
 *     FILL MISSING FIELDS, which a writer does not take, left out);
 *   - Cloudberry reads a table whose mpp_execute is 'all segments' on its
 *     segments, each its share of the fragments the coordinator asks PXF
 *     for and sends them in the plan, and any other on the coordinator.
 *     gp_core reads it where its mpp_execute says too (gp_foreign.c), but a
 *     segment's plan is not the coordinator's begun -- on the planner's
 *     route a segment plans the query it is sent itself -- so each segment
 *     asks PXF for the list, and keeps its share of it
 *     (FilterFragmentsForSegment()); the coordinator, or the one node, keeps
 *     them all;
 *   - a COPY FROM into the table, which is how gp_core sends a segment the
 *     rows an INSERT spreads over them, as for any table, begins and ends
 *     an insert as INSERT does (BeginForeignInsert, EndForeignInsert); and
 *     gp_exttable is loaded with the module (_PG_init());
 *   - PostgreSQL 19's planner and table APIs (create_foreignscan_path()'s
 *     arguments, table_close()), the scan's connection to PXF closed as it
 *     ends, and a plan with no filter to send PXF one that can be written
 *     out, which gp_core does to send ORCA's slice to the segments.
 */

#include "postgres.h"

#include "pxf_fdw.h"
#include "pxf_bridge.h"
#include "pxf_filter.h"
#include "pxf_fragment.h"

#include "access/external.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "cdb/cdbvars.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/*
 * gp_exttable, whose scan and writer pxf_fdw reads and writes with, loaded
 * with it: on the coordinator too, which reads nothing of a table its
 * segments read, but says the rows they rejected, which is gp_exttable's to
 * say (its notice filter, sreh.c).
 */
void
_PG_init(void)
{
	load_file("$libdir/gp_exttable", false);
}

#define DEFAULT_PXF_FDW_STARTUP_COST   50000

extern Datum pxf_fdw_handler(PG_FUNCTION_ARGS);

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(pxf_fdw_handler);

/*
 * FDW functions declarations
 */
static void pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);

static void pxfGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);

static ForeignScan *pxfGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);

static void pxfExplainForeignScan(ForeignScanState *node, ExplainState *es);

static void pxfBeginForeignScan(ForeignScanState *node, int eflags);

static TupleTableSlot *pxfIterateForeignScan(ForeignScanState *node);

static void pxfReScanForeignScan(ForeignScanState *node);

static void pxfEndForeignScan(ForeignScanState *node);

/* Foreign updates */
static void pxfBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo, List *fdw_private, int subplan_index, int eflags);
static TupleTableSlot *pxfExecForeignInsert(EState *estate, ResultRelInfo *resultRelInfo, TupleTableSlot *slot, TupleTableSlot *planSlot);

static void pxfEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo);

static void pxfBeginForeignInsert(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo);
static void pxfEndForeignInsert(EState *estate, ResultRelInfo *resultRelInfo);

static int	pxfIsForeignRelUpdatable(Relation rel);

/*
 * Helper functions
 */
static void InitCopyState(PxfFdwScanState *pxfsstate);
static void InitCopyStateForModify(PxfFdwModifyState *pxfmstate);

/*
 * Foreign-data wrapper handler functions:
 * returns a struct with pointers to the
 * pxf_fdw callback routines.
 */
Datum
pxf_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdw_routine = makeNode(FdwRoutine);

	/*
	 * foreign table scan support
	 */

	/* master - only */
	fdw_routine->GetForeignRelSize = pxfGetForeignRelSize;
	fdw_routine->GetForeignPaths = pxfGetForeignPaths;
	fdw_routine->GetForeignPlan = pxfGetForeignPlan;
	fdw_routine->ExplainForeignScan = pxfExplainForeignScan;

	/* the process that scans the table */
	fdw_routine->BeginForeignScan = pxfBeginForeignScan;
	fdw_routine->IterateForeignScan = pxfIterateForeignScan;
	fdw_routine->ReScanForeignScan = pxfReScanForeignScan;
	fdw_routine->EndForeignScan = pxfEndForeignScan;

	/*
	 * foreign table insert support
	 */

	/*
	 * AddForeignUpdateTargets set to NULL, no extra target expressions are
	 * added
	 */
	fdw_routine->AddForeignUpdateTargets = NULL;

	/*
	 * PlanForeignModify set to NULL, no additional plan-time actions are
	 * taken
	 */
	fdw_routine->PlanForeignModify = NULL;
	fdw_routine->BeginForeignModify = pxfBeginForeignModify;
	fdw_routine->ExecForeignInsert = pxfExecForeignInsert;

	/*
	 * ExecForeignUpdate and ExecForeignDelete set to NULL since updates and
	 * deletes are not supported
	 */
	fdw_routine->ExecForeignUpdate = NULL;
	fdw_routine->ExecForeignDelete = NULL;
	fdw_routine->EndForeignModify = pxfEndForeignModify;
	fdw_routine->IsForeignRelUpdatable = pxfIsForeignRelUpdatable;

	/* COPY FROM into the table, which is how a segment is sent its rows */
	fdw_routine->BeginForeignInsert = pxfBeginForeignInsert;
	fdw_routine->EndForeignInsert = pxfEndForeignInsert;

	PG_RETURN_POINTER(fdw_routine);
}

typedef struct PxfFdwRelationInfo
{
	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *remote_conds;
	List	   *local_conds;

	/* List of attributes (columns) that we need to get */
	List	   *retrieved_attrs;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;
}			PxfFdwRelationInfo;

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Currently we store:
 *
 * 1) WHERE clause text to be sent to the remote server
 * 2) Integer list of attribute numbers retrieved by the SELECT
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the WHERE clauses:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateWhereClauses));
 */
enum FdwScanPrivateIndex
{
	/* WHERE clauses to be sent to PXF (as a String node) */
	FdwScanPrivateWhereClauses,
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs
};

/*
 * GetForeignRelSize
 *		set relation size estimates for a foreign table
 */
static void
pxfGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize starts on segment: %d", PXF_SEGMENT_ID);
	Relation	rel;
	ListCell   *lc;

	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) palloc(sizeof(PxfFdwRelationInfo));

	baserel->fdw_private = (void *) fpinfo;

	fpinfo->attrs_used = NULL;

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);

	rel = table_open(rte->relid, NoLock);

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	classifyConditions(root, baserel, baserel->baserestrictinfo,
					   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server
	 */
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid, &fpinfo->attrs_used);
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid, &fpinfo->attrs_used);
	}

	deparseTargetList(rel, fpinfo->attrs_used, &fpinfo->retrieved_attrs);

	table_close(rel, NoLock);

	/* Use an artificial number of estimated rows */
	baserel->rows = 1000;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignRelSize ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * GetForeignPaths
 *		create access path for a scan on the foreign table
 */
static void
pxfGetForeignPaths(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid)
{
	ForeignPath *path = NULL;
	int			total_cost = DEFAULT_PXF_FDW_STARTUP_COST;
	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) baserel->fdw_private;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths starts on segment: %d", PXF_SEGMENT_ID);

	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   baserel->rows,
								   0,	/* no disabled plan nodes */
								   DEFAULT_PXF_FDW_STARTUP_COST,
								   total_cost,
								   NIL, /* no pathkeys */
								   NULL,	/* no outer rel either */
								   NULL,	/* no extra plan */
								   NIL, /* no restrictions of its own */
								   fpinfo->retrieved_attrs);

	/*
	 * Create a ForeignPath node and add it as only possible path.
	 */
	add_path(baserel, (Path *) path);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPaths ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * GetForeignPlan
 *		create a ForeignScan plan node
 */
static ForeignScan *
pxfGetForeignPlan(PlannerInfo *root,
				  RelOptInfo *baserel,
				  Oid foreigntableid,
				  ForeignPath *best_path,
				  List *tlist,
				  List *scan_clauses,
				  Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;
	PxfFdwRelationInfo *fpinfo = (PxfFdwRelationInfo *) baserel->fdw_private;
	List	   *fdw_private;

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan starts on segment: %d", PXF_SEGMENT_ID);

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwScanPrivateIndex, above.
	 */

	/*
	 * here we serialize the WHERE clauses -- "" for none: the plan is written
	 * out as text to send ORCA's slice to the segments, and a String node
	 * without its string cannot be
	 */
	char	   *where_clauses_str = SerializePxfFilterQuals(fpinfo->remote_conds);

	fdw_private = list_make2(makeString(where_clauses_str ? where_clauses_str : pstrdup("")),
							 fpinfo->retrieved_attrs);

	elog(DEBUG5, "pxf_fdw: pxfGetForeignPlan ends on segment: %d", PXF_SEGMENT_ID);

	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							fdw_private,
							NIL,
							NIL,
							outer_plan);
}

/*
 * pxfExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
pxfExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan starts on segment: %d", PXF_SEGMENT_ID);
	elog(DEBUG5, "pxf_fdw: pxfExplainForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * BeginForeignScan
 *   called during executor startup. perform any initialization
 *   needed, but not start the actual scan.
 */
static void
pxfBeginForeignScan(ForeignScanState *node, int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	List	   *fragments;

	ExprState  *quals = node->ss.ps.qual;
	Oid			foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
	PxfFdwScanState *pxfsstate = NULL;
	Relation	relation = node->ss.ss_currentRelation;
	ForeignScan *foreignScan = (ForeignScan *) node->ss.ps.plan;
	PxfOptions *options = PxfGetOptions(foreigntableid);

	/* retrieve fdw-private information from pxfGetForeignPlan() */
	char	   *filter_str = strVal(list_nth(foreignScan->fdw_private, FdwScanPrivateWhereClauses));
	List	   *retrieved_attrs = (List *) list_nth(foreignScan->fdw_private, FdwScanPrivateRetrievedAttrs);

	/*
	 * The process scanning the table asks PXF for the fragments, and keeps
	 * its share of them: a segment's, or all of them on the coordinator or
	 * the one node.
	 */
	fragments = GetFragmentList(options,
								relation,
								filter_str,
								retrieved_attrs);
	if (fragments != NIL)
		fragments = FilterFragmentsForSegment(fragments);

	/* Assign PXF location for the allocated fragments */
	AssignPxfLocationToFragments(options, fragments);

	/*
	 * Save state in node->fdw_state.  We must save enough information to
	 * begin the scan again.
	 */
	pxfsstate = (PxfFdwScanState *) palloc0(sizeof(PxfFdwScanState));
	initStringInfo(&pxfsstate->uri);

	pxfsstate->filter_str = filter_str;
	pxfsstate->fragments = fragments;
	pxfsstate->options = options;
	pxfsstate->quals = quals;
	pxfsstate->relation = relation;
	pxfsstate->retrieved_attrs = retrieved_attrs;

	InitCopyState(pxfsstate);
	node->fdw_state = (void *) pxfsstate;

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * IterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 *   Fetch one row from the foreign source, returning it in a tuple table slot
 *    (the node's ScanTupleSlot should be used for this purpose).
 *  Return NULL if no more rows are available.
 */
static TupleTableSlot *
pxfIterateForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfIterateForeignScan Executing on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	HeapTuple	tuple;

	/*
	 * The row, the scan's until the next is read, or none at the end; a row
	 * that would not parse is rejected by the scan, under the table's
	 * reject_limit, and counted.
	 */
	tuple = ExtScanSourceNext(pxfsstate->scan);
	if (tuple != NULL)
		ExecStoreHeapTuple(tuple, slot, false);
	else
		ExecClearTuple(slot);

	return slot;
}

/*
 * ReScanForeignScan
 *		Restart the scan from the beginning
 */
static void
pxfReScanForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	PxfBridgeImportCleanup(pxfsstate);
	PxfBridgeImportStart(pxfsstate);
	ExtScanSourceRescan(pxfsstate->scan);

	elog(DEBUG5, "pxf_fdw: pxfReScanForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * EndForeignScan
 *		End the scan and release resources.
 */
static void
pxfEndForeignScan(ForeignScanState *node)
{
	elog(DEBUG5, "pxf_fdw: pxfEndForeignScan starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwScanState *pxfsstate = (PxfFdwScanState *) node->fdw_state;

	/* if pxfsstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfsstate)
	{
		ExtScanSourceEnd(pxfsstate->scan);
		PxfBridgeImportCleanup(pxfsstate);
	}

	elog(DEBUG5, "pxf_fdw: pxfEndForeignScan ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 */
static void
pxfBeginForeignModify(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo,
					  List *fdw_private,
					  int subplan_index,
					  int eflags)
{
	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfOptions *options = NULL;
	PxfFdwModifyState *pxfmstate = NULL;
	Relation	relation = resultRelInfo->ri_RelationDesc;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	options = PxfGetOptions(RelationGetRelid(relation));
	pxfmstate = palloc0(sizeof(PxfFdwModifyState));

	initStringInfo(&pxfmstate->uri);
	pxfmstate->relation = relation;
	pxfmstate->options = options;

	InitCopyStateForModify(pxfmstate);

	resultRelInfo->ri_FdwState = pxfmstate;

	elog(DEBUG5, "pxf_fdw: pxfBeginForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfExecForeignInsert
 *		Insert one row into a foreign table
 */
static TupleTableSlot *
pxfExecForeignInsert(EState *estate,
					 ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot,
					 TupleTableSlot *planSlot)
{
	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;
	ExtCopyOut *out = pxfmstate->out;

	/* TEXT or CSV */
	ExtCopyOutRow(out, slot);

	int			bytes_written = PxfBridgeWrite(pxfmstate, out->line.data, out->line.len);

	if (bytes_written == -1)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to foreign resource: %m")));
	}

	elog(DEBUG3, "pxf_fdw %d bytes written", bytes_written);

	/* Reset our buffer to start clean next round */
	resetStringInfo(&out->line);

	elog(DEBUG5, "pxf_fdw: pxfExecForeignInsert ends on segment: %d", PXF_SEGMENT_ID);
	return slot;
}

/*
 * pxfEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
pxfEndForeignModify(EState *estate,
					ResultRelInfo *resultRelInfo)
{
	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify starts on segment: %d", PXF_SEGMENT_ID);

	PxfFdwModifyState *pxfmstate = (PxfFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If pxfmstate is NULL, we are in EXPLAIN; nothing to do */
	if (pxfmstate == NULL)
		return;

	ExtCopyOutEnd(pxfmstate->out);
	PxfBridgeCleanup(pxfmstate);

	elog(DEBUG5, "pxf_fdw: pxfEndForeignModify ends on segment: %d", PXF_SEGMENT_ID);
}

/*
 * pxfBeginForeignInsert
 *		Begin a COPY FROM into a foreign table: the rows gp_core sends a
 *		segment to write, which it copies into the table there, as it does
 *		for any table
 */
static void
pxfBeginForeignInsert(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo)
{
	pxfBeginForeignModify(mtstate, resultRelInfo, NIL, 0, 0);
}

/*
 * pxfEndForeignInsert
 *		Finish a COPY FROM into a foreign table
 */
static void
pxfEndForeignInsert(EState *estate,
					ResultRelInfo *resultRelInfo)
{
	pxfEndForeignModify(estate, resultRelInfo);
}

/*
 * pxfIsForeignRelUpdatable
 *  Assume table is updatable regardless of settings.
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 */
static int
pxfIsForeignRelUpdatable(Relation rel)
{
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable starts on segment: %d", PXF_SEGMENT_ID);
	elog(DEBUG5, "pxf_fdw: pxfIsForeignRelUpdatable ends on segment: %d", PXF_SEGMENT_ID);
	/* Only INSERTs are allowed at the moment */
	return 1u << (unsigned int) CMD_INSERT | 0u << (unsigned int) CMD_UPDATE | 0u << (unsigned int) CMD_DELETE;
}

/*
 * Initiates the scan for pxfBeginForeignScan(): the connection to the first
 * fragment, and gp_exttable's scan of the data read from it, in the wire
 * format and with the COPY options PxfGetOptions() gathered, and the
 * table's single row error handling.
 */
static void
InitCopyState(PxfFdwScanState *pxfsstate)
{
	PxfOptions *options = pxfsstate->options;
	char		fmttype = 'c';

	PxfBridgeImportStart(pxfsstate);

	foreach_node(DefElem, def, options->copy_options)
		if (strcmp(def->defname, "format") == 0 &&
			pg_strcasecmp(defGetString(def), "text") == 0)
			fmttype = 't';

	pxfsstate->scan = ExtScanSourceBegin(pxfsstate->relation,
										 options->resource,
										 fmttype,
										 options->copy_options,
										 options->reject_limit,
										 options->is_reject_limit_rows,
										 options->log_errors ? LOG_ERRORS_ENABLE : LOG_ERRORS_DISABLE,
										 PxfBridgeRead,
										 pxfsstate);
}

/*
 * Initiates the writer for pxfBeginForeignModify(): the upload, and COPY
 * TO's options -- Cloudberry's NEWLINE and FILL MISSING FIELDS are COPY
 * FROM's, and a row ends with a newline, as Cloudberry's CopyOneRowTo()
 * ends it.
 */
static void
InitCopyStateForModify(PxfFdwModifyState *pxfmstate)
{
	List	   *copy_options = NIL;

	PxfBridgeExportStart(pxfmstate);

	foreach_node(DefElem, def, pxfmstate->options->copy_options)
		if (strcmp(def->defname, "newline") != 0 &&
			strcmp(def->defname, "fill_missing_fields") != 0)
			copy_options = lappend(copy_options, def);

	pxfmstate->out = ExtCopyOutBegin(pxfmstate->relation, copy_options);
}
