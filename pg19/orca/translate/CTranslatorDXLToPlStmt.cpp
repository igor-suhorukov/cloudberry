//---------------------------------------------------------------------------
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
//	Ported from github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
//	and changed for PostgreSQL 19: past the include paths, a comment beside
//	each change, or beside what replaced it, says what and why.
//	Cloudberry's notice for the original follows, as the Apache License
//	requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2010 Greenplum, Inc.
//
//	@filename:
//		CTranslatorDXLToPlStmt.cpp
//
//	@doc:
//		Implementation of the methods for translating from DXL tree to GPDB
//		PlannedStmt.
//
//	@test:
//
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/gp_distribution_policy.h"
#include "catalog/pg_collation.h"
// INT4OID and Int4EqualOperator, for the NULL-safe hash keys of IS NOT
// DISTINCT FROM.
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
// Not cdb/cdbutil.h or utils/uri.h: Cloudberry includes both here and uses
// nothing from either.  The slice table's types come from cdb_plan_nodes.h.
#include "cdb/cdb_plan_nodes.h"
#include "cdb/cdbvars.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "jit/jit.h"
#include "nodes/nodes.h"
// AGGSPLIT_INTERMEDIATE, which Cloudberry adds to nodes/nodes.h.
#include "cb_nodes.h"
// Cloudberry's AssertOp, as a CustomScan; see TranslateDXLAssert.
#include "cb_assertop.h"
// The scans of a partitioned table; see TranslateDXLDynTblScan.
#include "cb_dynamicscan.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "optimizer/cost.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/typcache.h"
}

#include <algorithm>
#include <limits>  // std::numeric_limits
#include <numeric>
#include <tuple>

#include "gpos/base.h"
#include "gpos/common/CBitSet.h"
#include "gpos/common/CBitSetIter.h"

#include "gpopt/base/CUtils.h"
#include "gpdbwrappers.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "CIndexQualInfo.h"
#include "CPartPruneStepsBuilder.h"
#include "CTranslatorDXLToPlStmt.h"
#include "gp_unported.h"
#include "CTranslatorUtils.h"
#include "naucrates/dxl/operators/CDXLDatumGeneric.h"
#include "naucrates/dxl/operators/CDXLDirectDispatchInfo.h"
#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/operators/CDXLPhysicalAgg.h"
#include "naucrates/dxl/operators/CDXLPhysicalAppend.h"
#include "naucrates/dxl/operators/CDXLPhysicalAssert.h"
#include "naucrates/dxl/operators/CDXLPhysicalBitmapTableScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalCTAS.h"
#include "naucrates/dxl/operators/CDXLPhysicalCTEConsumer.h"
#include "naucrates/dxl/operators/CDXLPhysicalCTEProducer.h"
#include "naucrates/dxl/operators/CDXLPhysicalDynamicBitmapTableScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalDynamicForeignScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalDynamicIndexOnlyScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalDynamicIndexScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalDynamicTableScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalGatherMotion.h"
#include "naucrates/dxl/operators/CDXLPhysicalHashJoin.h"
#include "naucrates/dxl/operators/CDXLPhysicalIndexOnlyScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalLimit.h"
#include "naucrates/dxl/operators/CDXLPhysicalMaterialize.h"
#include "naucrates/dxl/operators/CDXLPhysicalMergeJoin.h"
#include "naucrates/dxl/operators/CDXLPhysicalNLJoin.h"
#include "naucrates/dxl/operators/CDXLPhysicalPartitionSelector.h"
#include "naucrates/dxl/operators/CDXLPhysicalRedistributeMotion.h"
#include "naucrates/dxl/operators/CDXLPhysicalResult.h"
#include "naucrates/dxl/operators/CDXLPhysicalRoutedDistributeMotion.h"
#include "naucrates/dxl/operators/CDXLPhysicalSort.h"
#include "naucrates/dxl/operators/CDXLPhysicalSplit.h"
#include "naucrates/dxl/operators/CDXLPhysicalTVF.h"
#include "naucrates/dxl/operators/CDXLPhysicalTableScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalParallelTableScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalValuesScan.h"
#include "naucrates/dxl/operators/CDXLPhysicalWindow.h"
#include "naucrates/dxl/operators/CDXLScalarBitmapBoolOp.h"
#include "naucrates/dxl/operators/CDXLScalarBitmapIndexProbe.h"
#include "naucrates/dxl/operators/CDXLScalarBoolExpr.h"
#include "naucrates/dxl/operators/CDXLScalarFuncExpr.h"
#include "naucrates/dxl/operators/CDXLScalarHashExpr.h"
#include "naucrates/dxl/operators/CDXLScalarNullTest.h"
#include "naucrates/dxl/operators/CDXLScalarOpExpr.h"
#include "naucrates/dxl/operators/CDXLScalarProjElem.h"
#include "naucrates/dxl/operators/CDXLScalarSortCol.h"
#include "naucrates/dxl/operators/CDXLScalarWindowFrameEdge.h"
#include "naucrates/exception.h"
#include "naucrates/md/IMDAggregate.h"
#include "naucrates/md/IMDFunction.h"
#include "naucrates/md/IMDIndex.h"
#include "naucrates/md/IMDScalarOp.h"
#include "naucrates/md/IMDType.h"
#include "naucrates/md/IMDTypeBool.h"
#include "naucrates/md/IMDTypeInt4.h"
#include "naucrates/traceflags/traceflags.h"

#include "nodes/nodeFuncs.h"

using namespace gpdxl;
using namespace gpos;
using namespace gpopt;
using namespace gpmd;

#define GPDXL_ROOT_PLAN_ID -1
#define GPDXL_PLAN_ID_START 1
#define GPDXL_MOTION_ID_START 1
#define GPDXL_PARAM_ID_START 0

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CTranslatorDXLToPlStmt
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CTranslatorDXLToPlStmt::CTranslatorDXLToPlStmt(
	CMemoryPool *mp, CMDAccessor *md_accessor,
	CContextDXLToPlStmt *dxl_to_plstmt_context, ULONG num_of_segments)
	: m_mp(mp),
	  m_md_accessor(md_accessor),
	  m_dxl_to_plstmt_context(dxl_to_plstmt_context),
	  m_cmd_type(CMD_SELECT),
	  m_is_tgt_tbl_distributed(false),
	  m_result_rel_list(nullptr),
	  m_num_of_segments(num_of_segments),
	  m_partition_selector_counter(0)
{
	m_translator_dxl_to_scalar = GPOS_NEW(m_mp)
		CTranslatorDXLToScalar(m_mp, m_md_accessor, m_num_of_segments);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::~CTranslatorDXLToPlStmt
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CTranslatorDXLToPlStmt::~CTranslatorDXLToPlStmt()
{
	GPOS_DELETE(m_translator_dxl_to_scalar);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::GetPlannedStmtFromDXL
//
//	@doc:
//		Translate DXL node into a PlannedStmt
//
//---------------------------------------------------------------------------
PlannedStmt *
CTranslatorDXLToPlStmt::GetPlannedStmtFromDXL(const CDXLNode *dxlnode,
											  const Query *orig_query,
											  bool can_set_tag)
{
	GPOS_ASSERT(nullptr != dxlnode);

	CDXLTranslateContext dxl_translate_ctxt(m_mp, false, orig_query);

	PlanSlice *topslice;

	topslice = (PlanSlice *) gpdb::GPDBAlloc(sizeof(PlanSlice));
	memset(topslice, 0, sizeof(PlanSlice));
	topslice->sliceIndex = 0;
	topslice->parentIndex = -1;
	topslice->gangType = GANGTYPE_UNALLOCATED;
	topslice->numsegments = 1;
	topslice->segindex = -1;
	topslice->directDispatch.isDirectDispatch = false;
	topslice->directDispatch.contentIds = NIL;
	topslice->directDispatch.haveProcessedAnyCalculations = false;

	m_dxl_to_plstmt_context->m_orig_query = (Query *) orig_query;
	m_dxl_to_plstmt_context->AddSlice(topslice);
	m_dxl_to_plstmt_context->SetCurrentSlice(topslice);

	CDXLTranslationContextArray *ctxt_translation_prev_siblings =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	Plan *plan = TranslateDXLOperatorToPlan(dxlnode, &dxl_translate_ctxt,
											ctxt_translation_prev_siblings);
	ctxt_translation_prev_siblings->Release();

	GPOS_ASSERT(nullptr != plan);

	// collect oids from rtable
	//
	// Cloudberry's loop also picks out the one relation in the FROM clause
	// whose distribution policy chooses the hash function for direct
	// dispatch.  That is M2's, with direct dispatch, and comes back with it.
	List *oids_list = NIL;

	ListCell *lc_rte = nullptr;

	ForEach(lc_rte, m_dxl_to_plstmt_context->GetRTableEntriesList())
	{
		RangeTblEntry *pRTE = (RangeTblEntry *) lfirst(lc_rte);

		if (pRTE->rtekind == RTE_RELATION)
		{
			oids_list = gpdb::LAppendOid(oids_list, pRTE->relid);
		}
	}

	// assemble planned stmt
	//
	// PostgreSQL 19's PlannedStmt, filled the way standard_planner fills it
	// (planner.c), not the way Cloudberry's is: Cloudberry's has planGen,
	// intoPolicy and a slice table, which PostgreSQL 19 does not, and lacks
	// fields the executor now relies on.  queryId, stmt_location and stmt_len
	// are the caller's, which has the Query ORCA was handed; see
	// optimize_query().
	PlannedStmt *planned_stmt = MakeNode(PlannedStmt);

	planned_stmt->commandType = m_cmd_type;
	planned_stmt->planOrigin = PLAN_STMT_STANDARD;
	planned_stmt->hasReturning = (orig_query->returningList != NIL);
	planned_stmt->hasModifyingCTE = orig_query->hasModifyingCTE;
	planned_stmt->canSetTag = can_set_tag;

	// transientPlan is the planner's answer for an index that is not usable
	// by every snapshot yet, which no ORCA plan uses: a query that would is
	// refused, and goes to the planner (CheckIndexUsableBySnapshots).
	// dependsOnRole is set
	// by the planner only for a foreign join that assumed the current user and
	// for an inlined SQL function with row security; ORCA plans neither, and
	// the plan cache takes the Query's own row-security dependence separately
	// (plansource->dependsOnRLS).  No parallel plans (decision 2), and no JIT:
	// the cost thresholds that decide it are in the PostgreSQL planner's
	// units, which ORCA's costs are not, so Cloudberry's ORCA plans never JIT
	// either.
	planned_stmt->transientPlan = false;
	planned_stmt->dependsOnRole = false;
	planned_stmt->parallelModeNeeded = false;
	planned_stmt->jitFlags = PGJIT_NONE;

	planned_stmt->planTree = plan;
	planned_stmt->rtable = m_dxl_to_plstmt_context->GetRTableEntriesList();
	planned_stmt->permInfos = m_dxl_to_plstmt_context->GetPermInfosList();
	planned_stmt->subplans = m_dxl_to_plstmt_context->GetSubplanEntriesList();
	planned_stmt->rewindPlanIDs = m_dxl_to_plstmt_context->GetRewindPlanIds();
	planned_stmt->paramExecTypes = m_dxl_to_plstmt_context->GetParamTypes();
	planned_stmt->relationOids = oids_list;

	// Every range table entry is unprunable, because nothing is pruned: T0
	// has no partition pruning.  This is not bookkeeping.  PostgreSQL 19's
	// executor refuses to open a relation that is not a member --
	// ExecGetRangeTableRelation raises "trying to open a pruned relation" --
	// so a plan without it cannot scan anything.
	for (int rti = 1; rti <= gpdb::ListLength(planned_stmt->rtable); rti++)
	{
		planned_stmt->unprunableRelids =
			gpdb::BmsAddMember(planned_stmt->unprunableRelids, rti);
	}

	// The result relations, as a set of RT indexes: PostgreSQL 19 replaced
	// Cloudberry's list with resultRelationRelids.
	ListCell *lc_result = nullptr;
	ForEach(lc_result, m_result_rel_list)
	{
		planned_stmt->resultRelationRelids = gpdb::BmsAddMember(
			planned_stmt->resultRelationRelids, lfirst_int(lc_result));
	}

	// The slice table stays in the context, where every plan's one slice was
	// built; at M2 it goes in PlannedStmt.extension_state, which PostgreSQL 19
	// provides for exactly this.  Direct dispatch is M2's too, and a plan that
	// asked for it at T0 would be one over a distributed table, which the
	// relcache translator does not report on one node -- so it is refused,
	// not ignored, to say so if that ever stops being true.
	if (nullptr != dxlnode->GetDXLDirectDispatchInfo())
	{
		GP_UNPORTED("direct dispatch");
	}

	return planned_stmt;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLOperatorToPlan
//
//	@doc:
//		Translates a DXL tree into a Plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLOperatorToPlan(
	const CDXLNode *dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	GPOS_ASSERT(nullptr != dxlnode);
	GPOS_ASSERT(nullptr != ctxt_translation_prev_siblings);

	Plan *plan;

	const CDXLOperator *dxlop = dxlnode->GetOperator();
	gpdxl::Edxlopid ulOpId = dxlop->GetDXLOperator();

	// The operators the port translates, group by group of cloudberry.md's
	// "Next".  Every other operator is refused here, by its own name, before
	// anything is built -- including the ones whose translation compiles,
	// because compiling is not the same as having been tested against
	// PostgreSQL 19's executor.  Each group adds its operators to this list
	// with the tests that prove them, and the refusal is what the fallback
	// counters report until then.
	//
	// A CTE producer is not among them, though T1 translates it: it is only
	// ever a child of a Sequence, and TranslateDXLSequence translates it
	// itself, so meeting one anywhere else is a shape the port does not know.
	switch (ulOpId)
	{
		// T0: the queries of one table
		case EdxlopPhysicalTableScan:
		case EdxlopPhysicalResult:
		case EdxlopPhysicalLimit:
		case EdxlopPhysicalSort:
		case EdxlopPhysicalAgg:
		case EdxlopPhysicalValuesScan:
		case EdxlopPhysicalMaterialize:
		// T1: joins, index and bitmap scans, Append, window functions, CTEs
		case EdxlopPhysicalHashJoin:
		case EdxlopPhysicalNLJoin:
		case EdxlopPhysicalMergeJoin:
		case EdxlopPhysicalIndexScan:
		case EdxlopPhysicalIndexOnlyScan:
		case EdxlopPhysicalBitmapTableScan:
		case EdxlopPhysicalAppend:
		case EdxlopPhysicalWindow:
		case EdxlopPhysicalCTEConsumer:
		case EdxlopPhysicalSequence:
		// T2: INSERT, UPDATE and DELETE, assertions, functions in FROM,
		// foreign tables.  Not ORCA's CTAS operator, which plans the new
		// table's distribution with it and is M2's; on one node a CREATE
		// TABLE AS is planned as its query (CheckSupportedCmdType).
		case EdxlopPhysicalDML:
		case EdxlopPhysicalAssert:
		case EdxlopPhysicalTVF:
		case EdxlopPhysicalForeignScan:
		// T3: the scans of a partitioned table, and the selection of its
		// partitions while the query runs.  Not the dynamic foreign scan, and
		// not an Append over the partitions, which ORCA plans only with
		// foreign partitions or gp.optimizer_disable_dynamic_table_scan.
		case EdxlopPhysicalDynamicTableScan:
		case EdxlopPhysicalDynamicIndexScan:
		case EdxlopPhysicalDynamicIndexOnlyScan:
		case EdxlopPhysicalDynamicBitmapTableScan:
		case EdxlopPhysicalPartitionSelector:
			break;
		default:
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion,
					   dxlop->GetOpNameStr()->GetBuffer());
	}

	switch (ulOpId)
	{
		default:
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion,
					   dxlnode->GetOperator()->GetOpNameStr()->GetBuffer());
		}
		case EdxlopPhysicalTableScan:
		case EdxlopPhysicalForeignScan:
		{
			plan = TranslateDXLTblScan(dxlnode, output_context,
									   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalParallelTableScan:
		{
			plan = TranslateDXLParallelTblScan(dxlnode, output_context,
											   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalIndexScan:
		{
			plan = TranslateDXLIndexScan(dxlnode, output_context,
										 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalIndexOnlyScan:
		{
			plan = TranslateDXLIndexOnlyScan(dxlnode, output_context,
											 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalHashJoin:
		{
			plan = TranslateDXLHashJoin(dxlnode, output_context,
										ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalNLJoin:
		{
			plan = TranslateDXLNLJoin(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalMergeJoin:
		{
			plan = TranslateDXLMergeJoin(dxlnode, output_context,
										 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalMotionGather:
		case EdxlopPhysicalMotionBroadcast:
		case EdxlopPhysicalMotionRoutedDistribute:
		{
			plan = TranslateDXLMotion(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalMotionRedistribute:
		case EdxlopPhysicalMotionRandom:
		{
			plan = TranslateDXLDuplicateSensitiveMotion(
				dxlnode, output_context, ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalLimit:
		{
			plan = TranslateDXLLimit(dxlnode, output_context,
									 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalAgg:
		{
			plan = TranslateDXLAgg(dxlnode, output_context,
								   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalWindow:
		{
			if (CDXLPhysicalWindow::Cast(dxlnode->GetOperator())->IsWindowHashAgg()) {
				plan = TranslateDXLWindowHashAgg(dxlnode, output_context,
									  	ctxt_translation_prev_siblings);
			} else {
				plan = TranslateDXLWindowAgg(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			}
			break;
		}
		case EdxlopPhysicalSort:
		{
			plan = TranslateDXLSort(dxlnode, output_context,
									ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalResult:
		{
			plan = TranslateDXLResult(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalAppend:
		{
			plan = TranslateDXLAppend(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalMaterialize:
		{
			plan = TranslateDXLMaterialize(dxlnode, output_context,
										   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalSequence:
		{
			plan = TranslateDXLSequence(dxlnode, output_context,
										ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalDynamicTableScan:
		{
			plan = TranslateDXLDynTblScan(dxlnode, output_context,
										  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalDynamicIndexScan:
		{
			plan = TranslateDXLDynIdxScan(dxlnode, output_context,
										  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalDynamicIndexOnlyScan:
		{
			plan = TranslateDXLDynIdxOnlyScan(dxlnode, output_context,
											  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalDynamicForeignScan:
		{
			plan = TranslateDXLDynForeignScan(dxlnode, output_context,
											  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalTVF:
		{
			plan = TranslateDXLTvf(dxlnode, output_context,
								   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalDML:
		{
			plan = TranslateDXLDml(dxlnode, output_context,
								   ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalSplit:
		{
			plan = TranslateDXLSplit(dxlnode, output_context,
									 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalAssert:
		{
			plan = TranslateDXLAssert(dxlnode, output_context,
									  ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalCTEProducer:
		{
			plan = TranslateDXLCTEProducerToSharedScan(
				dxlnode, output_context, ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalCTEConsumer:
		{
			plan = TranslateDXLCTEConsumerToSharedScan(
				dxlnode, output_context, ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalBitmapTableScan:
		case EdxlopPhysicalDynamicBitmapTableScan:
		{
			plan = TranslateDXLBitmapTblScan(dxlnode, output_context,
											 ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalCTAS:
		{
			plan = TranslateDXLCtas(dxlnode, output_context,
									ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalPartitionSelector:
		{
			plan = TranslateDXLPartSelector(dxlnode, output_context,
											ctxt_translation_prev_siblings);
			break;
		}
		case EdxlopPhysicalValuesScan:
		{
			plan = TranslateDXLValueScan(dxlnode, output_context,
										 ctxt_translation_prev_siblings);
			break;
		}
	}

	if (nullptr == plan)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion,
				   dxlnode->GetOperator()->GetOpNameStr()->GetBuffer());
	}
	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetParamIds
//
//	@doc:
//		Set the bitmapset with the param_ids defined in the plan
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetParamIds(Plan *plan)
{
	List *params_node_list = gpdb::ExtractNodesPlan(
		plan, T_Param, true /* descend_into_subqueries */);

	ListCell *lc = nullptr;

	Bitmapset *bitmapset = nullptr;

	ForEach(lc, params_node_list)
	{
		Param *param = (Param *) lfirst(lc);
		bitmapset = gpdb::BmsAddMember(bitmapset, param->paramid);
	}

	plan->extParam = bitmapset;
	plan->allParam = bitmapset;
}

List *
CTranslatorDXLToPlStmt::TranslatePartOids(IMdIdArray *parts, INT lockmode)
{
	List *oids_list = NIL;

	for (ULONG ul = 0; ul < parts->Size(); ul++)
	{
		Oid part = CMDIdGPDB::CastMdid((*parts)[ul])->Oid();
		oids_list = gpdb::LAppendOid(oids_list, part);
		// Since parser locks only root partition, locking the leaf
		// partitions which we have to scan.
		gpdb::GPDBLockRelationOid(part, lockmode);
	}
	return oids_list;
}

List *
CTranslatorDXLToPlStmt::TranslateJoinPruneParamids(
	const ULongPtrArray *selector_ids, OID oid_type,
	CContextDXLToPlStmt *dxl_to_plstmt_context)
{
	List *join_prune_paramids = NIL;

	for (ULONG ul = 0; ul < selector_ids->Size(); ++ul)
	{
		ULONG selector_id = *(*selector_ids)[ul];
		ULONG param_id =
			dxl_to_plstmt_context->GetParamIdForSelector(oid_type, selector_id);
		join_prune_paramids = gpdb::LAppendInt(join_prune_paramids, param_id);
	}
	return join_prune_paramids;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLTblScan
//
//	@doc:
//		Translates a DXL table scan node into a TableScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLTblScan(
	const CDXLNode *tbl_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalTableScan *phy_tbl_scan_dxlop =
		CDXLPhysicalTableScan::Cast(tbl_scan_dxlnode->GetOperator());

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const CDXLTableDescr *dxl_table_descr =
		phy_tbl_scan_dxlop->GetDXLTableDescr();
	const IMDRelation *md_rel =
		m_md_accessor->RetrieveRel(dxl_table_descr->MDId());

	// Lock any table we are to scan, since it may not have been properly locked
	// by the parser (e.g in case of generated scans for partitioned tables)
	OID oidRel = CMDIdGPDB::CastMdid(md_rel->MDId())->Oid();
	GPOS_ASSERT(dxl_table_descr->LockMode() != -1);
	gpdb::GPDBLockRelationOid(oidRel, dxl_table_descr->LockMode());

	Index index = ProcessDXLTblDescr(dxl_table_descr, &base_table_context);

	// a table scan node must have 2 children: projection list and filter
	GPOS_ASSERT(2 == tbl_scan_dxlnode->Arity());

	// translate proj list and filter
	CDXLNode *project_list_dxlnode = (*tbl_scan_dxlnode)[EdxltsIndexProjList];
	CDXLNode *filter_dxlnode = (*tbl_scan_dxlnode)[EdxltsIndexFilter];

	List *targetlist = NIL;

	// List to hold the quals after translating filter_dxlnode node.
	List *query_quals = NIL;

	TranslateProjListAndFilter(
		project_list_dxlnode, filter_dxlnode,
		&base_table_context,  // translate context for the base table
		nullptr,			  // translate_ctxt_left and pdxltrctxRight,
		&targetlist, &query_quals, output_context);

	Plan *plan = nullptr;
	Plan *plan_return = nullptr;

	if (IMDRelation::ErelstorageForeign == md_rel->RetrieveRelStorageType())
	{
		RangeTblEntry *rte = m_dxl_to_plstmt_context->GetRTEByIndex(index);

		// The postgres_fdw wrapper does not support row level security. So
		// passing only the query_quals while creating the foreign scan node.
		//
		// BuildForeignScan internally calls build_simple_rel which looks up
		// RTEPermissionInfo via root->parse->rteperminfos.  The RTE here was
		// newly created by ORCA with its own perminfoindex numbering, which
		// may not match m_orig_query->rteperminfos (e.g. after the rewriter
		// expands external-table ON SELECT rules into subqueries the outer
		// query's rteperminfos shrinks).  Temporarily swap in ORCA's own
		// perminfos list so the indices are consistent.
		Query *orig_query = m_dxl_to_plstmt_context->m_orig_query;
		List *saved_perminfos = orig_query->rteperminfos;
		orig_query->rteperminfos =
			m_dxl_to_plstmt_context->GetPermInfosList();

		ForeignScan *foreign_scan =
			gpdb::CreateForeignScan(oidRel, index, query_quals, targetlist,
									orig_query, rte);

		orig_query->rteperminfos = saved_perminfos;
		foreign_scan->scan.scanrelid = index;
		plan = &(foreign_scan->scan.plan);
		plan_return = (Plan *) foreign_scan;
	}
	else
	{
		SeqScan *seq_scan = MakeNode(SeqScan);
		seq_scan->scan.scanrelid = index;
		plan = &(seq_scan->scan.plan);
		plan_return = (Plan *) seq_scan;

		plan->targetlist = targetlist;

		// List to hold the quals which contain both security quals and query
		// quals.
		List *security_query_quals = NIL;

		// Fetching the RTE of the relation from the rewritten parse tree
		// based on the oidRel and adding the security quals of the RTE in
		// the security_query_quals list.
		AddSecurityQuals(oidRel, &security_query_quals, &index);

		// The security quals should always be executed first when
		// compared to other quals. So appending query quals to the
		// security_query_quals list after the security quals.
		security_query_quals =
			gpdb::ListConcat(security_query_quals, query_quals);
		plan->qual = security_query_quals;
	}

	if (md_rel->IsNonBlockTable())
	{
		CheckSafeTargetListForAOTables(plan->targetlist);
	}

	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(tbl_scan_dxlnode, plan);

	SetParamIds(plan);

	return plan_return;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLParallelTblScan
//
//	@doc:
//		Translates a DXL parallel table scan node into a parallel SeqScan node
Plan *
CTranslatorDXLToPlStmt::TranslateDXLParallelTblScan(
	const CDXLNode *tbl_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// after M7: parallel table scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("parallel table scans");
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetIndexVarAttnoWalker
//
//	@doc:
//		Walker to set index var attno's,
//		attnos of index vars are set to their relative positions in index keys,
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::SetIndexVarAttnoWalker(
	Node *node, SContextIndexVarAttno *ctxt_index_var_attno_walker)
{
	if (nullptr == node)
	{
		return false;
	}

	if (IsA(node, Var) && ((Var *) node)->varno != OUTER_VAR)
	{
		INT attno = ((Var *) node)->varattno;
		const IMDRelation *md_rel = ctxt_index_var_attno_walker->m_md_rel;
		const IMDIndex *index = ctxt_index_var_attno_walker->m_md_index;

		ULONG index_col_pos_idx_max = gpos::ulong_max;
		const ULONG arity = md_rel->ColumnCount();
		for (ULONG col_pos_idx = 0; col_pos_idx < arity; col_pos_idx++)
		{
			const IMDColumn *md_col = md_rel->GetMdCol(col_pos_idx);
			if (attno == md_col->AttrNum())
			{
				index_col_pos_idx_max = col_pos_idx;
				break;
			}
		}

		if (gpos::ulong_max > index_col_pos_idx_max)
		{
			((Var *) node)->varattno =
				1 + index->GetKeyPos(index_col_pos_idx_max);
		}

		return false;
	}

	return gpdb::WalkExpressionTree(
		node, (BOOL(*)(Node *, void *)) CTranslatorDXLToPlStmt::SetIndexVarAttnoWalker,
		ctxt_index_var_attno_walker);
}


// An index that this transaction's snapshots may not read through yet.
//
// NOT IN CLOUDBERRY.  CREATE INDEX over a table whose HOT chains are broken
// marks the index indcheckxmin, and a snapshot taken before it was built can
// miss rows through it.  The planner skips such an index for as long as that
// is so, and marks the plan transient so that the plan cache replans once it
// is not (plancat.c, get_relation_info).  ORCA's metadata admits every valid
// index, and its metadata cache would keep an index it had once left out, so
// the check is made where a plan uses one: the query goes to the planner,
// which skips the index itself.  Rare -- it needs a transaction older than
// the index -- and counted like any other fallback.
static void
CheckIndexUsableBySnapshots(Oid index_oid)
{
	if (!gpdb::IndexUsableBySnapshots(index_oid))
	{
		GP_UNPORTED("an index newer than this transaction's snapshots");
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLIndexScan
//
//	@doc:
//		Translates a DXL index scan node into a IndexScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLIndexScan(
	const CDXLNode *index_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalIndexScan *physical_idx_scan_dxlop =
		CDXLPhysicalIndexScan::Cast(index_scan_dxlnode->GetOperator());

	return TranslateDXLIndexScan(index_scan_dxlnode, physical_idx_scan_dxlop,
								 output_context,
								 ctxt_translation_prev_siblings);
}

void
CTranslatorDXLToPlStmt::TranslatePlan(
	Plan *plan, const CDXLNode *dxlnode, CDXLTranslateContext *output_context,
	CContextDXLToPlStmt *dxl_to_plstmt_context,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	plan->plan_node_id = dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(dxlnode, plan);

	// an index scan node must have 3 children: projection list, filter and index condition list
	GPOS_ASSERT(3 == dxlnode->Arity());

	// translate proj list and filter
	CDXLNode *project_list_dxlnode = (*dxlnode)[EdxlisIndexProjList];
	CDXLNode *filter_dxlnode = (*dxlnode)[EdxlisIndexFilter];

	// translate proj list
	plan->targetlist =
		TranslateDXLProjList(project_list_dxlnode, base_table_context,
							 nullptr /*child_contexts*/, output_context);

	// translate index filter
	plan->qual = TranslateDXLIndexFilter(filter_dxlnode, output_context,
										 base_table_context,
										 ctxt_translation_prev_siblings);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLIndexScan
//
//	@doc:
//		Translates a DXL index scan node into a IndexScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLIndexScan(
	const CDXLNode *index_scan_dxlnode,
	CDXLPhysicalIndexScan *physical_idx_scan_dxlop,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const CDXLTableDescr *dxl_table_descr =
		physical_idx_scan_dxlop->GetDXLTableDescr();
	const IMDRelation *md_rel =
		m_md_accessor->RetrieveRel(dxl_table_descr->MDId());

	// Lock any table we are to scan, since it may not have been properly locked
	// by the parser (e.g in case of generated scans for partitioned tables)
	CMDIdGPDB *mdid = CMDIdGPDB::CastMdid(md_rel->MDId());
	GPOS_ASSERT(dxl_table_descr->LockMode() != -1);
	gpdb::GPDBLockRelationOid(mdid->Oid(), dxl_table_descr->LockMode());

	Index index = ProcessDXLTblDescr(dxl_table_descr, &base_table_context);

	IndexScan *index_scan = nullptr;
	index_scan = MakeNode(IndexScan);
	index_scan->scan.scanrelid = index;

	CMDIdGPDB *mdid_index = CMDIdGPDB::CastMdid(
		physical_idx_scan_dxlop->GetDXLIndexDescr()->MDId());
	const IMDIndex *md_index = m_md_accessor->RetrieveIndex(mdid_index);
	Oid index_oid = mdid_index->Oid();

	GPOS_ASSERT(InvalidOid != index_oid);
	// Lock any index we are to scan, since it may not have been properly locked
	// by the parser (e.g in case of generated scans for partitioned indexes)
	gpdb::GPDBLockRelationOid(index_oid, dxl_table_descr->LockMode());
	CheckIndexUsableBySnapshots(index_oid);
	index_scan->indexid = index_oid;

	Plan *plan = &(index_scan->scan.plan);

	TranslatePlan(plan, index_scan_dxlnode, output_context,
				  m_dxl_to_plstmt_context, &base_table_context,
				  ctxt_translation_prev_siblings);

	index_scan->indexorderdir = CTranslatorUtils::GetScanDirection(
		physical_idx_scan_dxlop->GetIndexScanDir());

	if (md_rel->IsNonBlockTable())
	{
		CheckSafeTargetListForAOTables(plan->targetlist);
	}

	// translate index condition list
	List *index_cond = NIL;
	List *index_orig_cond = NIL;

	// Translate Index Conditions if Index isn't used for order by.
	if (!IsIndexForOrderBy(&base_table_context, ctxt_translation_prev_siblings,
						   output_context,
						   (*index_scan_dxlnode)[EdxlisIndexCondition]))
	{
		TranslateIndexConditions(
			(*index_scan_dxlnode)[EdxlisIndexCondition],
			physical_idx_scan_dxlop->GetDXLTableDescr(),
			false,	// is_bitmap_index_probe
			md_index, md_rel, output_context, &base_table_context,
			ctxt_translation_prev_siblings, &index_cond, &index_orig_cond);
	}

	index_scan->indexqual = index_cond;
	index_scan->indexqualorig = index_orig_cond;
	/*
	 * As of 8.4, the indexstrategy and indexsubtype fields are no longer
	 * available or needed in IndexScan. Ignore them.
	 */
	SetParamIds(plan);

	return (Plan *) index_scan;
}

static List *
TranslateDXLIndexTList(const IMDRelation *md_rel, const IMDIndex *md_index,
					   Index new_varno, const CDXLTableDescr *table_descr,
					   CDXLTranslateContextBaseTable *index_context)
{
	List *target_list = NIL;

	index_context->SetRelIndex(INDEX_VAR);

	// Translate KEY columns
	for (ULONG ul = 0; ul < md_index->Keys(); ul++)
	{
		ULONG key = md_index->KeyAt(ul);

		const IMDColumn *col = md_rel->GetMdCol(key);

		TargetEntry *target_entry = MakeNode(TargetEntry);
		target_entry->resno = (AttrNumber) ul + 1;

		Expr *indexvar = (Expr *) gpdb::MakeVar(
			new_varno, col->AttrNum(),
			CMDIdGPDB::CastMdid(col->MdidType())->Oid(),
			col->TypeModifier() /*vartypmod*/, 0 /*varlevelsup*/);
		target_entry->expr = indexvar;

		// Fix up proj list. Since index only scan does not read full tuples,
		// the var->varattno must be updated as it should no longer point to
		// column in the table, but rather a column in the index. We achieve
		// this by mapping col id to a new varattno based on index columns.
		for (ULONG j = 0; j < table_descr->Arity(); j++)
		{
			const CDXLColDescr *dxl_col_descr =
				table_descr->GetColumnDescrAt(j);
			if (dxl_col_descr->AttrNum() == ((Var *) indexvar)->varattno)
			{
				(void) index_context->InsertMapping(dxl_col_descr->Id(),
													ul + 1);
				break;
			}
		}

		target_list = gpdb::LAppend(target_list, target_entry);
	}

	// Translate INCLUDED columns
	for (ULONG ul = 0; ul < md_index->IncludedCols(); ul++)
	{
		ULONG includecol = md_index->IncludedColAt(ul);

		const IMDColumn *col = md_rel->GetMdCol(includecol);

		TargetEntry *target_entry = MakeNode(TargetEntry);
		// KEY columns preceed INCLUDE columns
		target_entry->resno = (AttrNumber) ul + 1 + md_index->Keys();

		Expr *indexvar = (Expr *) gpdb::MakeVar(
			new_varno, col->AttrNum(),
			CMDIdGPDB::CastMdid(col->MdidType())->Oid(),
			col->TypeModifier() /*vartypmod*/, 0 /*varlevelsup*/);
		target_entry->expr = indexvar;

		for (ULONG j = 0; j < table_descr->Arity(); j++)
		{
			const CDXLColDescr *dxl_col_descr =
				table_descr->GetColumnDescrAt(j);
			if (dxl_col_descr->AttrNum() == ((Var *) indexvar)->varattno)
			{
				(void) index_context->InsertMapping(dxl_col_descr->Id(),
													target_entry->resno);
				break;
			}
		}

		target_list = gpdb::LAppend(target_list, target_entry);
	}

	return target_list;
}

Plan *
CTranslatorDXLToPlStmt::TranslateDXLIndexOnlyScan(
	const CDXLNode *index_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalIndexOnlyScan *physical_idx_scan_dxlop =
		CDXLPhysicalIndexOnlyScan::Cast(index_scan_dxlnode->GetOperator());
	const CDXLTableDescr *table_desc =
		physical_idx_scan_dxlop->GetDXLTableDescr();

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(
		physical_idx_scan_dxlop->GetDXLTableDescr()->MDId());

	Index index = ProcessDXLTblDescr(table_desc, &base_table_context);

	IndexOnlyScan *index_scan = MakeNode(IndexOnlyScan);
	index_scan->scan.scanrelid = index;

	CMDIdGPDB *mdid_index = CMDIdGPDB::CastMdid(
		physical_idx_scan_dxlop->GetDXLIndexDescr()->MDId());
	const IMDIndex *md_index = m_md_accessor->RetrieveIndex(mdid_index);
	Oid index_oid = mdid_index->Oid();

	GPOS_ASSERT(InvalidOid != index_oid);
	CheckIndexUsableBySnapshots(index_oid);
	index_scan->indexid = index_oid;

	CDXLTranslateContextBaseTable index_context(m_mp);

	// translate index targetlist
	index_scan->indextlist = TranslateDXLIndexTList(md_rel, md_index, index,
													table_desc, &index_context);

	Plan *plan = &(index_scan->scan.plan);
	TranslatePlan(plan, index_scan_dxlnode, output_context,
				  m_dxl_to_plstmt_context, &index_context,
				  ctxt_translation_prev_siblings);

	index_scan->indexorderdir = CTranslatorUtils::GetScanDirection(
		physical_idx_scan_dxlop->GetIndexScanDir());

	// translate index condition list
	List *index_cond = NIL;
	List *index_orig_cond = NIL;

	// Translate Index Conditions if Index isn't used for order by.
	if (!IsIndexForOrderBy(&base_table_context, ctxt_translation_prev_siblings,
						   output_context,
						   (*index_scan_dxlnode)[EdxlisIndexCondition]))
	{
		TranslateIndexConditions(
			(*index_scan_dxlnode)[EdxlisIndexCondition],
			physical_idx_scan_dxlop->GetDXLTableDescr(),
			false,	// is_bitmap_index_probe
			md_index, md_rel, output_context, &base_table_context,
			ctxt_translation_prev_siblings, &index_cond, &index_orig_cond);
	}

	index_scan->indexqual = index_cond;

	// PostgreSQL 15 added recheckqual, which Cloudberry's IndexOnlyScan does
	// not have: the index quals that the executor re-evaluates when the
	// index says a match was lossy, as a GiST one may.  It is evaluated
	// against the index tuple, so it is the quals in index-column form --
	// which is what indexqual already is, its key Vars INDEX_VAR and numbered
	// by index column (TranslateIndexConditions) -- and not indexqualorig,
	// whose columns are the table's.  Left NIL, a lossy match would be
	// returned unchecked.
	index_scan->recheckqual = (List *) gpdb::CopyObject(index_cond);
	SetParamIds(plan);

	return (Plan *) index_scan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateIndexFilter
//
//	@doc:
//		Translate the index filter list in an Index scan
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLIndexFilter(
	CDXLNode *filter_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	List *quals_list = NIL;

	// build colid->var mapping
	CMappingColIdVarPlStmt colid_var_mapping(
		m_mp, base_table_context, ctxt_translation_prev_siblings,
		output_context, m_dxl_to_plstmt_context);

	const ULONG arity = filter_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *index_filter_dxlnode = (*filter_dxlnode)[ul];
		Expr *index_filter_expr =
			m_translator_dxl_to_scalar->TranslateDXLToScalar(
				index_filter_dxlnode, &colid_var_mapping);
		quals_list = gpdb::LAppend(quals_list, index_filter_expr);
	}

	return quals_list;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateIndexConditions
//
//	@doc:
//		Translate the index condition list in an Index scan
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateIndexConditions(
	CDXLNode *index_cond_list_dxlnode, const CDXLTableDescr * /*dxl_tbl_descr*/,
	BOOL is_bitmap_index_probe, const IMDIndex *index,
	const IMDRelation *md_rel, CDXLTranslateContext *output_context,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings,
	List **index_cond, List **index_orig_cond)
{
	// array of index qual info
	CIndexQualInfoArray *index_qual_info_array =
		GPOS_NEW(m_mp) CIndexQualInfoArray(m_mp);

	// build colid->var mapping
	CMappingColIdVarPlStmt colid_var_mapping(
		m_mp, base_table_context, ctxt_translation_prev_siblings,
		output_context, m_dxl_to_plstmt_context);

	const ULONG arity = index_cond_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *index_cond_dxlnode = (*index_cond_list_dxlnode)[ul];
		CDXLNode *modified_null_test_cond_dxlnode = nullptr;

		// FIXME: Remove this translation from BoolExpr to NullTest when ORCA gets rid of
		// translation of 'x IS NOT NULL' to 'NOT (x IS NULL)'. Here's the ticket that tracks
		// the issue: https://github.com/greenplum-db/gpdb/issues/16294

		// Translate index condition CDXLScalarBoolExpr of format 'NOT (col IS NULL)'
		// to CDXLScalarNullTest 'col IS NOT NULL', because IndexScan only
		// supports indexquals of types: OpExpr, RowCompareExpr,
		// ScalarArrayOpExpr and NullTest
		if (index_cond_dxlnode->GetOperator()->GetDXLOperator() ==
			EdxlopScalarBoolExpr)
		{
			CDXLScalarBoolExpr *boolexpr_dxlop =
				CDXLScalarBoolExpr::Cast(index_cond_dxlnode->GetOperator());
			if (boolexpr_dxlop->GetDxlBoolTypeStr() == Edxlnot &&
				(*index_cond_dxlnode)[0]->GetOperator()->GetDXLOperator() ==
					EdxlopScalarNullTest)
			{
				CDXLNode *null_test_cond_dxlnode = (*index_cond_dxlnode)[0];
				CDXLNode *scalar_ident_dxlnode = (*null_test_cond_dxlnode)[0];
				scalar_ident_dxlnode->AddRef();
				modified_null_test_cond_dxlnode = GPOS_NEW(m_mp) CDXLNode(
					m_mp, GPOS_NEW(m_mp) CDXLScalarNullTest(m_mp, false),
					scalar_ident_dxlnode);
				index_cond_dxlnode = modified_null_test_cond_dxlnode;
			}
		}
		Expr *original_index_cond_expr =
			m_translator_dxl_to_scalar->TranslateDXLToScalar(
				index_cond_dxlnode, &colid_var_mapping);
		Expr *index_cond_expr =
			m_translator_dxl_to_scalar->TranslateDXLToScalar(
				index_cond_dxlnode, &colid_var_mapping);
		GPOS_ASSERT(
			(IsA(index_cond_expr, OpExpr) ||
			 IsA(index_cond_expr, ScalarArrayOpExpr) ||
			 IsA(index_cond_expr, NullTest)) &&
			"expected OpExpr or ScalarArrayOpExpr or NullTest in index qual");

		// allow Index quals with scalar array only for bitmap and btree indexes
		if (!is_bitmap_index_probe && IsA(index_cond_expr, ScalarArrayOpExpr) &&
			!(IMDIndex::EmdindBitmap == index->IndexType() ||
			  IMDIndex::EmdindBtree == index->IndexType()))
		{
			GPOS_RAISE(
				gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion,
				GPOS_WSZ_LIT("ScalarArrayOpExpr condition on index scan"));
		}

		// We need to perform mapping of Varattnos relative to column positions in index keys
		SContextIndexVarAttno index_varattno_ctxt(md_rel, index);
		SetIndexVarAttnoWalker((Node *) index_cond_expr, &index_varattno_ctxt);

		// find index key's attno
		List *args_list = nullptr;
		if (IsA(index_cond_expr, OpExpr))
		{
			args_list = ((OpExpr *) index_cond_expr)->args;
		}
		else if (IsA(index_cond_expr, ScalarArrayOpExpr))
		{
			args_list = ((ScalarArrayOpExpr *) index_cond_expr)->args;
		}
		else
		{
			// NullTest struct doesn't have List argument, hence ignoring
			// assignment for that type
		}

		Node *left_arg;
		Node *right_arg;
		if (IsA(index_cond_expr, NullTest))
		{
			// NullTest only has one arg
			left_arg = (Node *) (((NullTest *) index_cond_expr)->arg);
			right_arg = nullptr;
		}
		else
		{
			left_arg = (Node *) lfirst(gpdb::ListHead(args_list));
			right_arg = (Node *) lfirst(gpdb::ListTail(args_list));
			// Type Coercion doesn't add much value for IS NULL and IS NOT NULL
			// conditions, and is not supported by ORCA currently
			BOOL is_relabel_type = false;
			if (IsA(left_arg, RelabelType) &&
				IsA(((RelabelType *) left_arg)->arg, Var))
			{
				left_arg = (Node *) ((RelabelType *) left_arg)->arg;
				is_relabel_type = true;
			}
			else if (IsA(right_arg, RelabelType) &&
					 IsA(((RelabelType *) right_arg)->arg, Var))
			{
				right_arg = (Node *) ((RelabelType *) right_arg)->arg;
				is_relabel_type = true;
			}

			if (is_relabel_type)
			{
				List *new_args_list = ListMake2(left_arg, right_arg);
				gpdb::GPDBFree(args_list);
				if (IsA(index_cond_expr, OpExpr))
				{
					((OpExpr *) index_cond_expr)->args = new_args_list;
				}
				else
				{
					((ScalarArrayOpExpr *) index_cond_expr)->args =
						new_args_list;
				}
			}
		}

		GPOS_ASSERT((IsA(left_arg, Var) || IsA(right_arg, Var)) &&
					"expected index key in index qual");

		INT attno = 0;
		if (IsA(left_arg, Var) && ((Var *) left_arg)->varno != OUTER_VAR)
		{
			// index key is on the left side
			attno = ((Var *) left_arg)->varattno;
			// GPDB_92_MERGE_FIXME: helluva hack
			// Upstream commit a0185461 cleaned up how the varno of indices
			// We are patching up varno here, but it seems this really should
			// happen in CTranslatorDXLToScalar::PexprFromDXLNodeScalar .
			// Furthermore, should we guard against nonsensical varno?
			((Var *) left_arg)->varno = INDEX_VAR;
		}
		else
		{
			// index key is on the right side
			GPOS_ASSERT(((Var *) right_arg)->varno != OUTER_VAR &&
						"unexpected outer reference in index qual");
			attno = ((Var *) right_arg)->varattno;
		}

		// create index qual
		index_qual_info_array->Append(GPOS_NEW(m_mp) CIndexQualInfo(
			attno, index_cond_expr, original_index_cond_expr));

		if (modified_null_test_cond_dxlnode != nullptr)
		{
			modified_null_test_cond_dxlnode->Release();
		}
	}

	// the index quals much be ordered by attribute number
	index_qual_info_array->Sort(CIndexQualInfo::IndexQualInfoCmp);

	ULONG length = index_qual_info_array->Size();
	for (ULONG ul = 0; ul < length; ul++)
	{
		CIndexQualInfo *index_qual_info = (*index_qual_info_array)[ul];
		*index_cond = gpdb::LAppend(*index_cond, index_qual_info->m_expr);
		*index_orig_cond =
			gpdb::LAppend(*index_orig_cond, index_qual_info->m_original_expr);
	}

	// clean up
	index_qual_info_array->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLAssertConstraints
//
//	@doc:
//		Translate the constraints from an Assert node into a list of quals
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLAssertConstraints(
	CDXLNode *assert_contraint_list_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *child_contexts)
{
	List *quals_list = NIL;

	// build colid->var mapping
	CMappingColIdVarPlStmt colid_var_mapping(
		m_mp, nullptr /*base_table_context*/, child_contexts, output_context,
		m_dxl_to_plstmt_context);

	const ULONG arity = assert_contraint_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *assert_contraint_dxlnode =
			(*assert_contraint_list_dxlnode)[ul];
		Expr *assert_contraint_expr =
			m_translator_dxl_to_scalar->TranslateDXLToScalar(
				(*assert_contraint_dxlnode)[0], &colid_var_mapping);
		quals_list = gpdb::LAppend(quals_list, assert_contraint_expr);
	}

	return quals_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLLimit
//
//	@doc:
//		Translates a DXL Limit node into a Limit node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLLimit(
	const CDXLNode *limit_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// create limit node
	Limit *limit = MakeNode(Limit);

	Plan *plan = &(limit->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(limit_dxlnode, plan);

	GPOS_ASSERT(4 == limit_dxlnode->Arity());

	CDXLTranslateContext left_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());

	// translate proj list
	CDXLNode *project_list_dxlnode = (*limit_dxlnode)[EdxllimitIndexProjList];
	CDXLNode *child_plan_dxlnode = (*limit_dxlnode)[EdxllimitIndexChildPlan];
	CDXLNode *limit_count_dxlnode = (*limit_dxlnode)[EdxllimitIndexLimitCount];
	CDXLNode *limit_offset_dxlnode =
		(*limit_dxlnode)[EdxllimitIndexLimitOffset];

	// NOTE: Limit node has only the left plan while the right plan is left empty
	Plan *left_plan =
		TranslateDXLOperatorToPlan(child_plan_dxlnode, &left_dxl_translate_ctxt,
								   ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&left_dxl_translate_ctxt);

	plan->targetlist =
		TranslateDXLProjList(project_list_dxlnode,
							 nullptr,  // base table translation context
							 child_contexts, output_context);

	plan->lefttree = left_plan;

	if (nullptr != limit_count_dxlnode && limit_count_dxlnode->Arity() > 0)
	{
		CMappingColIdVarPlStmt colid_var_mapping(m_mp, nullptr, child_contexts,
												 output_context,
												 m_dxl_to_plstmt_context);
		Node *limit_count =
			(Node *) m_translator_dxl_to_scalar->TranslateDXLToScalar(
				(*limit_count_dxlnode)[0], &colid_var_mapping);
		limit->limitCount = limit_count;
	}

	if (nullptr != limit_offset_dxlnode && limit_offset_dxlnode->Arity() > 0)
	{
		CMappingColIdVarPlStmt colid_var_mapping =
			CMappingColIdVarPlStmt(m_mp, nullptr, child_contexts,
								   output_context, m_dxl_to_plstmt_context);
		Node *limit_offset =
			(Node *) m_translator_dxl_to_scalar->TranslateDXLToScalar(
				(*limit_offset_dxlnode)[0], &colid_var_mapping);
		limit->limitOffset = limit_offset;
	}

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return (Plan *) limit;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLHashJoin
//
//	@doc:
//		Translates a DXL hash join node into a HashJoin node
//
//---------------------------------------------------------------------------
// A hash key for one side of an IS NOT DISTINCT FROM condition, which has to
// put a NULL in a bucket like any other value.
//
// Cloudberry's HashJoin evaluates such a condition in hashqualclauses, a
// field of its own, and hashes only non-NULL keys: its executor checks the
// condition against every tuple of the NULL bucket.  PostgreSQL 19's has no
// such field, and a strict hash function makes a NULL key skip the hash table
// altogether -- on the inner side it is never inserted, on the outer side it
// never probes -- so a NULL could never match a NULL.
//
// So the key is the side's own hash function, the one its family pairs with
// the condition's equality operator, applied to the value, with a NULL hashed
// as 0: an int4 that is never NULL.  The HashJoin hashes that int4 again with
// int4's own function, which changes nothing about which keys meet, and the
// condition itself, among the hashclauses, decides whether two rows in a
// bucket match -- so a NULL that shares a bucket with a 0 is still told apart
// from it.  Hashing a value of the key's own type in NULL's place would be
// simpler, but not every type has a value to make up: an empty varlena is
// not a numeric, and a hash function would read past its end.
static Expr *
NullSafeHashKey(Expr *key, Oid hashfn, Oid inputcollid)
{
	FuncExpr *hash = MakeNode(FuncExpr);
	hash->funcid = hashfn;
	hash->funcresulttype = INT4OID;
	hash->funcretset = false;
	hash->funcvariadic = false;
	hash->funcformat = COERCE_EXPLICIT_CALL;
	hash->funccollid = InvalidOid;
	hash->inputcollid = inputcollid;
	hash->args = ListMake1(key);
	hash->location = -1;

	CoalesceExpr *coalesce = MakeNode(CoalesceExpr);
	coalesce->coalescetype = INT4OID;
	coalesce->coalescecollid = InvalidOid;
	coalesce->args = ListMake2(hash, gpdb::MakeIntConst(0));
	coalesce->location = -1;

	return (Expr *) coalesce;
}

Plan *
CTranslatorDXLToPlStmt::TranslateDXLHashJoin(
	const CDXLNode *hj_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	GPOS_ASSERT(hj_dxlnode->GetOperator()->GetDXLOperator() ==
				EdxlopPhysicalHashJoin);
	GPOS_ASSERT(hj_dxlnode->Arity() == EdxlhjIndexSentinel);

	// create hash join node
	HashJoin *hashjoin = MakeNode(HashJoin);

	Join *join = &(hashjoin->join);
	Plan *plan = &(join->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	CDXLPhysicalHashJoin *hashjoin_dxlop =
		CDXLPhysicalHashJoin::Cast(hj_dxlnode->GetOperator());

	// set join type
	//
	// Not Cloudberry's prefetch_inner, a Join field of its own that makes the
	// executor build the hash table before it reads the outer side, so that a
	// Motion below cannot deadlock.  PostgreSQL 19 decides that itself, from
	// the costs, and nothing here moves between processes.
	join->jointype =
		GetGPDBJoinTypeFromDXLJoinType(hashjoin_dxlop->GetJoinType());

	// translate operator costs
	TranslatePlanCosts(hj_dxlnode, plan);

	// translate join children
	CDXLNode *left_tree_dxlnode = (*hj_dxlnode)[EdxlhjIndexHashLeft];
	CDXLNode *right_tree_dxlnode = (*hj_dxlnode)[EdxlhjIndexHashRight];
	CDXLNode *project_list_dxlnode = (*hj_dxlnode)[EdxlhjIndexProjList];
	CDXLNode *filter_dxlnode = (*hj_dxlnode)[EdxlhjIndexFilter];
	CDXLNode *join_filter_dxlnode = (*hj_dxlnode)[EdxlhjIndexJoinFilter];
	CDXLNode *hash_cond_list_dxlnode = (*hj_dxlnode)[EdxlhjIndexHashCondList];

	CDXLTranslateContext left_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());
	CDXLTranslateContext right_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());

	Plan *left_plan =
		TranslateDXLOperatorToPlan(left_tree_dxlnode, &left_dxl_translate_ctxt,
								   ctxt_translation_prev_siblings);

	// the right side of the join is the one where the hash phase is done
	CDXLTranslationContextArray *translation_context_arr_with_siblings =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	translation_context_arr_with_siblings->Append(&left_dxl_translate_ctxt);
	translation_context_arr_with_siblings->AppendArray(
		ctxt_translation_prev_siblings);
	Plan *right_plan =
		(Plan *) TranslateDXLHash(right_tree_dxlnode, &right_dxl_translate_ctxt,
								  translation_context_arr_with_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&left_dxl_translate_ctxt);
	child_contexts->Append(&right_dxl_translate_ctxt);
	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &plan->qual,
							   output_context);

	// translate join filter
	join->joinqual = TranslateDXLFilterToQual(
		join_filter_dxlnode,
		nullptr,  // translate context for the base table
		child_contexts, output_context);

	// translate hash cond
	//
	// The conditions go into hashclauses as they are, IS NOT DISTINCT FROM
	// among them, and that is what the executor checks two rows of one bucket
	// against.  Cloudberry puts the equality form of an IS NOT DISTINCT FROM
	// there and the condition itself in hashqualclauses, a field PostgreSQL
	// 19's HashJoin does not have; the NULLs it is for are hashed below.
	List *hash_conditions_list = NIL;

	const ULONG arity = hash_cond_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *hash_cond_dxlnode = (*hash_cond_list_dxlnode)[ul];

		List *hash_cond_list =
			TranslateDXLScCondToQual(hash_cond_dxlnode,
									 nullptr,  // base table translation context
									 child_contexts, output_context);

		GPOS_ASSERT(1 == gpdb::ListLength(hash_cond_list));

		hash_conditions_list =
			gpdb::ListConcat(hash_conditions_list, hash_cond_list);
	}

	hashjoin->hashclauses = hash_conditions_list;

	GPOS_ASSERT(NIL != hashjoin->hashclauses);

	CDXLTranslationContextArray *hash_child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	hash_child_contexts->Append(&left_dxl_translate_ctxt);
	left_dxl_translate_ctxt.MergeTcxt(&right_dxl_translate_ctxt);
	hash_child_contexts->Append(&right_dxl_translate_ctxt);

	List* hashclause_list = NIL;

	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *hash_cond_dxlnode = (*hash_cond_list_dxlnode)[ul];

		if (EdxlopScalarBoolExpr ==
			hash_cond_dxlnode->GetOperator()->GetDXLOperator())
		{
			// clause is a NOT DISTINCT FROM check -> extract the distinct comparison node
			GPOS_ASSERT(Edxlnot == CDXLScalarBoolExpr::Cast(
										hash_cond_dxlnode->GetOperator())
										->GetDxlBoolTypeStr());
			hash_cond_dxlnode = (*hash_cond_dxlnode)[0];
			GPOS_ASSERT(EdxlopScalarDistinct ==
						hash_cond_dxlnode->GetOperator()->GetDXLOperator());
		}

		CMappingColIdVarPlStmt hj_colid_var_mapping =
				CMappingColIdVarPlStmt(m_mp, nullptr, hash_child_contexts,
									   output_context, m_dxl_to_plstmt_context);

		// translate the DXL scalar or scalar distinct comparison into an equality comparison
		// to store in the hashclause_list
		Expr *hash_clause_expr =
			(Expr *)
				m_translator_dxl_to_scalar->TranslateDXLToScalar(
					hash_cond_dxlnode, &hj_colid_var_mapping);
		hashclause_list =
			gpdb::LAppend(hashclause_list, hash_clause_expr);

	}

	List	   *hashoperators = NIL;
	List	   *hashcollations = NIL;
	List	   *inner_hashkeys = NIL;
	List	   *outer_hashkeys = NIL;
	ListCell   *lc;

	Hash *hash = (Hash *) right_plan;

	ForEach(lc, hashclause_list)
	{
		Node	   *clause = (Node *) lfirst(lc);
		GPOS_ASSERT((IsA(clause, OpExpr) || IsA(clause, DistinctExpr)));
		OpExpr	   *hclause = (OpExpr *) clause;

		Expr *outer_key = (Expr *) linitial(hclause->args);
		Expr *inner_key = (Expr *) lsecond(hclause->args);

		if (IsA(clause, DistinctExpr))
		{
			// IS NOT DISTINCT FROM: see NullSafeHashKey.  A DistinctExpr's
			// opno is the equality operator it negates.
			Oid outer_hashfn = InvalidOid;
			Oid inner_hashfn = InvalidOid;
			if (!gpdb::GetOpHashFunctions(hclause->opno, &outer_hashfn,
										  &inner_hashfn))
			{
				GP_UNPORTED("IS NOT DISTINCT FROM over an operator that cannot hash");
			}
			hashoperators = gpdb::LAppendOid(hashoperators, Int4EqualOperator);
			hashcollations = gpdb::LAppendOid(hashcollations, InvalidOid);
			outer_key =
				NullSafeHashKey(outer_key, outer_hashfn, hclause->inputcollid);
			inner_key =
				NullSafeHashKey(inner_key, inner_hashfn, hclause->inputcollid);
		}
		else
		{
			hashoperators = gpdb::LAppendOid(hashoperators, hclause->opno);
			hashcollations =
				gpdb::LAppendOid(hashcollations, hclause->inputcollid);
		}

		outer_hashkeys = gpdb::LAppend(outer_hashkeys, outer_key);
		inner_hashkeys = gpdb::LAppend(inner_hashkeys, inner_key);
	}

	hashjoin->hashoperators = hashoperators;
	hashjoin->hashcollations = hashcollations;
	hashjoin->hashkeys = outer_hashkeys;
	hash->hashkeys = inner_hashkeys;

	plan->lefttree = left_plan;
	plan->righttree = right_plan;
	SetParamIds(plan);

	// A Partition Selector on the hash side chooses the partitions a Dynamic
	// Scan on the other side reads, and has chosen once the hash table is
	// built.  PostgreSQL 19's hash join may fetch an outer row first, to skip
	// building the table when the outer side is empty -- whenever the outer
	// side's start-up cost is below the Hash node's total -- and the Dynamic
	// Scan would then start before the choice, and read every partition.
	// Cloudberry's hash join has prefetch_inner to say "build first";
	// PostgreSQL 19's decides by the costs, so the outer side's start-up cost
	// is raised to the Hash node's, as far as EXPLAIN shows it.
	if (HasPartitionSelector(right_plan) &&
		left_plan->startup_cost < right_plan->total_cost)
	{
		left_plan->startup_cost = right_plan->total_cost;
		left_plan->total_cost =
			std::max(left_plan->total_cost, left_plan->startup_cost);
	}

	// cleanup
	translation_context_arr_with_siblings->Release();
	child_contexts->Release();
	hash_child_contexts->Release();

	return (Plan *) hashjoin;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::HasPartitionSelector
//
//	@doc:
//		Is there a Partition Selector in the plan -- not inside a subplan an
//		expression calls, which runs on its own schedule?
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::HasPartitionSelector(Plan *plan)
{
	if (nullptr == plan)
	{
		return false;
	}

	if (IsA(plan, CustomScan))
	{
		CustomScan *cscan = (CustomScan *) plan;
		if (cscan->methods == &gp_orca_partition_selector_methods)
		{
			return true;
		}

		ListCell *lc = nullptr;
		ForEach(lc, cscan->custom_plans)
		{
			if (HasPartitionSelector((Plan *) lfirst(lc)))
			{
				return true;
			}
		}
	}

	return HasPartitionSelector(plan->lefttree) ||
		   HasPartitionSelector(plan->righttree);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLTvf
//
//	@doc:
//		Translates a DXL TVF node into a GPDB Function scan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLTvf(
	const CDXLNode *tvf_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// translation context for column mappings
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	// create function scan node
	FunctionScan *func_scan = MakeNode(FunctionScan);
	Plan *plan = &(func_scan->scan.plan);

	RangeTblEntry *rte = TranslateDXLTvfToRangeTblEntry(
		tvf_dxlnode, output_context, &base_table_context);
	GPOS_ASSERT(rte != nullptr);
	GPOS_ASSERT(list_length(rte->functions) == 1);
	RangeTblFunction *rtfunc =
		(RangeTblFunction *) gpdb::CopyObject(linitial(rte->functions));

	// we will add the new range table entry as the last element of the range table
	Index index =
		gpdb::ListLength(m_dxl_to_plstmt_context->GetRTableEntriesList()) + 1;
	base_table_context.SetRelIndex(index);
	func_scan->scan.scanrelid = index;

	m_dxl_to_plstmt_context->AddRTE(rte);

	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(tvf_dxlnode, plan);

	// a table scan node must have at least 1 child: projection list
	GPOS_ASSERT(1 <= tvf_dxlnode->Arity());

	CDXLNode *project_list_dxlnode = (*tvf_dxlnode)[EdxltsIndexProjList];

	// translate proj list
	List *target_list = TranslateDXLProjList(
		project_list_dxlnode, &base_table_context, nullptr, output_context);
	plan->targetlist = target_list;

	// The function's columns are what TranslateDXLTvfToRangeTblEntry said
	// they are: all of them, in the function's order, and the column
	// definition list of one that returns a record.  Cloudberry rebuilt them
	// here from the target list, in ORCA's order, and renumbered the target
	// list by the names of a function's OUT parameters (its
	// ProcessRecordFuncTargetList, from commit e4310714c5d), which mended the
	// output and left the scan reading the wrong column.
	func_scan->functions = ListMake1(rtfunc);

	SetParamIds(plan);

	return (Plan *) func_scan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLTvfToRangeTblEntry
//
//	@doc:
//		Create a range table entry from a CDXLPhysicalTVF node
//
//---------------------------------------------------------------------------
RangeTblEntry *
CTranslatorDXLToPlStmt::TranslateDXLTvfToRangeTblEntry(
	const CDXLNode *tvf_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslateContextBaseTable *base_table_context)
{
	CDXLPhysicalTVF *dxlop = CDXLPhysicalTVF::Cast(tvf_dxlnode->GetOperator());

	RangeTblEntry *rte = MakeNode(RangeTblEntry);
	rte->rtekind = RTE_FUNCTION;

	// get function alias
	Alias *alias = MakeNode(Alias);
	alias->colnames = NIL;
	alias->aliasname = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
		dxlop->Pstr()->GetBuffer());

	// project list
	CDXLNode *project_list_dxlnode = (*tvf_dxlnode)[EdxltsIndexProjList];
	const ULONG num_of_cols = project_list_dxlnode->Arity();

	RangeTblFunction *rtfunc = MakeNode(RangeTblFunction);
	Bitmapset *funcparams = nullptr;

	// invalid funcid indicates TVF evaluates to const
	if (!dxlop->FuncMdId()->IsValid())
	{
		Const *const_expr = MakeNode(Const);

		const_expr->consttype =
			CMDIdGPDB::CastMdid(dxlop->ReturnTypeMdId())->Oid();
		const_expr->consttypmod = -1;

		CDXLNode *constVa = (*tvf_dxlnode)[1];
		CDXLScalarConstValue *constValue =
			CDXLScalarConstValue::Cast(constVa->GetOperator());
		const CDXLDatum *datum_dxl = constValue->GetDatumVal();
		CDXLDatumGeneric *datum_generic_dxl =
			CDXLDatumGeneric::Cast(const_cast<gpdxl::CDXLDatum *>(datum_dxl));
		const IMDType *type =
			m_md_accessor->RetrieveType(datum_generic_dxl->MDId());
		const_expr->constlen = type->Length();
		Datum val = gpdb::DatumFromPointer(datum_generic_dxl->GetByteArray());
		ULONG length =
			(ULONG) gpdb::DatumSize(val, false, const_expr->constlen);
		CHAR *str = (CHAR *) gpdb::GPDBAlloc(length + 1);
		memcpy(str, datum_generic_dxl->GetByteArray(), length);
		str[length] = '\0';
		const_expr->constvalue = gpdb::DatumFromPointer(str);

		rtfunc->funcexpr = (Node *) const_expr;
	}
	else
	{
		FuncExpr *func_expr = MakeNode(FuncExpr);

		func_expr->funcid = CMDIdGPDB::CastMdid(dxlop->FuncMdId())->Oid();
		func_expr->funcretset = gpdb::GetFuncRetset(func_expr->funcid);
		// this is a function call, as opposed to a cast
		func_expr->funcformat = COERCE_EXPLICIT_CALL;
		func_expr->funcresulttype =
			CMDIdGPDB::CastMdid(dxlop->ReturnTypeMdId())->Oid();

		// function arguments
		const ULONG num_of_child = tvf_dxlnode->Arity();
		for (ULONG ul = 1; ul < num_of_child; ++ul)
		{
			CDXLNode *func_arg_dxlnode = (*tvf_dxlnode)[ul];

			CMappingColIdVarPlStmt colid_var_mapping(m_mp, base_table_context,
													 nullptr, output_context,
													 m_dxl_to_plstmt_context);

			Expr *pexprFuncArg =
				m_translator_dxl_to_scalar->TranslateDXLToScalar(
					func_arg_dxlnode, &colid_var_mapping);
			func_expr->args = gpdb::LAppend(func_expr->args, pexprFuncArg);
		}

		// GPDB_91_MERGE_FIXME: collation
		func_expr->inputcollid = gpdb::ExprCollation((Node *) func_expr->args);
		func_expr->funccollid = gpdb::TypeCollation(func_expr->funcresulttype);

		// Populate RangeTblFunction::funcparams, by walking down the entire
		// func_expr to capture ids of all the PARAMs
		ListCell *lc = nullptr;
		List *param_exprs = gpdb::ExtractNodesExpression(
			(Node *) func_expr, T_Param, false /*descend_into_subqueries */);
		ForEach(lc, param_exprs)
		{
			Param *param = (Param *) lfirst(lc);
			funcparams = gpdb::BmsAddMember(funcparams, param->paramid);
		}

		rtfunc->funcexpr = (Node *) func_expr;
	}

	// Which column of the function's each of ORCA's is.  Cloudberry took the
	// project list's order for the function's, and it is not always: ORCA
	// lists a scan's columns in the order of their ids (CTranslatorExprToDXL::
	// PdxlnTVF), and a copy of the scan -- a CTE ORCA puts in place of its
	// reader -- takes the reader's ids for the columns it reads and new ones
	// for the rest.  The scan then read one column for another, and PostGIS's
	// raster and topology tests stopped: "function return row and
	// query-specified return row do not match".  So each is matched by name,
	// among the columns of the query's own call of the function, named as the
	// query names them -- a record's are its column definition list -- and
	// one that cannot be matched for certain is refused (gpdb::
	// FunctionScanColumns).
	char **names = (char **) gpdb::GPDBAlloc(sizeof(char *) * (num_of_cols + 1));
	int *attnos = (int *) gpdb::GPDBAlloc(sizeof(int) * (num_of_cols + 1));
	for (ULONG ul = 0; ul < num_of_cols; ul++)
	{
		CDXLScalarProjElem *dxl_proj_elem = CDXLScalarProjElem::Cast(
			(*project_list_dxlnode)[ul]->GetOperator());

		names[ul] = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
			dxl_proj_elem->GetMdNameAlias()->GetMDName()->GetBuffer());
	}

	List *colnames = NIL;
	RangeTblFunction *coldef = nullptr;
	if (!gpdb::FunctionScanColumns(rtfunc->funcexpr,
								   m_dxl_to_plstmt_context->m_orig_query,
								   (int) num_of_cols, names, attnos, &colnames,
								   &coldef))
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
				   GPOS_WSZ_LIT("a function in FROM whose columns cannot be "
								"told apart by name"));
	}

	for (ULONG ul = 0; ul < num_of_cols; ul++)
	{
		CDXLScalarProjElem *dxl_proj_elem = CDXLScalarProjElem::Cast(
			(*project_list_dxlnode)[ul]->GetOperator());

		// save mapping col id -> the function's column, in translate context
		(void) base_table_context->InsertMapping(dxl_proj_elem->Id(),
												 attnos[ul]);
	}
	alias->colnames = colnames;

	// As the planner has them: a column definition list for a record, and no
	// list otherwise; the count is of every column the function returns.
	rtfunc->funccolcount = gpdb::ListLength(colnames);
	if (nullptr != coldef)
	{
		rtfunc->funccolnames = (List *) gpdb::CopyObject(coldef->funccolnames);
		rtfunc->funccoltypes = (List *) gpdb::CopyObject(coldef->funccoltypes);
		rtfunc->funccoltypmods =
			(List *) gpdb::CopyObject(coldef->funccoltypmods);
		rtfunc->funccolcollations =
			(List *) gpdb::CopyObject(coldef->funccolcollations);
	}
	rtfunc->funcparams = funcparams;
	rte->functions = ListMake1(rtfunc);

	rte->inFromCl = true;
	rte->perminfoindex = 0;

	rte->eref = alias;
	return rte;
}


// create a range table entry from a CDXLPhysicalValuesScan node
RangeTblEntry *
CTranslatorDXLToPlStmt::TranslateDXLValueScanToRangeTblEntry(
	const CDXLNode *value_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslateContextBaseTable *base_table_context)
{
	CDXLPhysicalValuesScan *phy_values_scan_dxlop =
		CDXLPhysicalValuesScan::Cast(value_scan_dxlnode->GetOperator());

	RangeTblEntry *rte = MakeNode(RangeTblEntry);

	rte->relid = InvalidOid;
	rte->subquery = nullptr;
	rte->rtekind = RTE_VALUES;
	rte->inh = false; /* never true for values RTEs */
	rte->inFromCl = true;
	/* No permission checks */
	rte->perminfoindex = 0;

	Alias *alias = MakeNode(Alias);
	alias->colnames = NIL;

	// get value alias
	alias->aliasname = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
		phy_values_scan_dxlop->GetOpNameStr()->GetBuffer());

	// project list
	CDXLNode *project_list_dxlnode = (*value_scan_dxlnode)[EdxltsIndexProjList];

	// get column names
	const ULONG num_of_cols = project_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < num_of_cols; ul++)
	{
		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		CDXLScalarProjElem *dxl_proj_elem =
			CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());

		CHAR *col_name_char_array =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				dxl_proj_elem->GetMdNameAlias()->GetMDName()->GetBuffer());

		String *val_colname = gpdb::MakeStringValue(col_name_char_array);
		alias->colnames = gpdb::LAppend(alias->colnames, val_colname);

		// save mapping col id -> index in translate context
		(void) base_table_context->InsertMapping(dxl_proj_elem->Id(),
												 ul + 1 /*attno*/);
	}

	CMappingColIdVarPlStmt colid_var_mapping =
		CMappingColIdVarPlStmt(m_mp, base_table_context, nullptr,
							   output_context, m_dxl_to_plstmt_context);
	const ULONG num_of_child = value_scan_dxlnode->Arity();
	List *values_lists = NIL;
	List *values_collations = NIL;

	for (ULONG ulValue = EdxlValIndexConstStart; ulValue < num_of_child;
		 ulValue++)
	{
		CDXLNode *value_list_dxlnode = (*value_scan_dxlnode)[ulValue];
		const ULONG num_of_cols = value_list_dxlnode->Arity();
		List *value = NIL;
		for (ULONG ulCol = 0; ulCol < num_of_cols; ulCol++)
		{
			Expr *const_expr = m_translator_dxl_to_scalar->TranslateDXLToScalar(
				(*value_list_dxlnode)[ulCol], &colid_var_mapping);
			value = gpdb::LAppend(value, const_expr);
		}
		values_lists = gpdb::LAppend(values_lists, value);

		// GPDB_91_MERGE_FIXME: collation
		if (NIL == values_collations)
		{
			// Set collation based on the first list of values
			for (ULONG ulCol = 0; ulCol < num_of_cols; ulCol++)
			{
				values_collations = gpdb::LAppendOid(
					values_collations, gpdb::ExprCollation((Node *) value));
			}
		}
	}

	rte->values_lists = values_lists;
	rte->colcollations = values_collations;
	rte->eref = alias;

	return rte;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLNLJoin
//
//	@doc:
//		Translates a DXL nested loop join node into a NestLoop plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLNLJoin(
	const CDXLNode *nl_join_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	GPOS_ASSERT(nl_join_dxlnode->GetOperator()->GetDXLOperator() ==
				EdxlopPhysicalNLJoin);
	GPOS_ASSERT(nl_join_dxlnode->Arity() == EdxlnljIndexSentinel);

	// create hash join node
	NestLoop *nested_loop = MakeNode(NestLoop);

	Join *join = &(nested_loop->join);
	Plan *plan = &(join->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	CDXLPhysicalNLJoin *dxl_nlj =
		CDXLPhysicalNLJoin::PdxlConvert(nl_join_dxlnode->GetOperator());

	// set join type
	join->jointype = GetGPDBJoinTypeFromDXLJoinType(dxl_nlj->GetJoinType());

	// translate operator costs
	TranslatePlanCosts(nl_join_dxlnode, plan);

	// translate join children
	CDXLNode *left_tree_dxlnode = (*nl_join_dxlnode)[EdxlnljIndexLeftChild];
	CDXLNode *right_tree_dxlnode = (*nl_join_dxlnode)[EdxlnljIndexRightChild];

	CDXLNode *project_list_dxlnode = (*nl_join_dxlnode)[EdxlnljIndexProjList];
	CDXLNode *filter_dxlnode = (*nl_join_dxlnode)[EdxlnljIndexFilter];
	CDXLNode *join_filter_dxlnode = (*nl_join_dxlnode)[EdxlnljIndexJoinFilter];

	CDXLTranslateContext left_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());
	CDXLTranslateContext right_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());

	// Not Cloudberry's prefetch_inner, which it sets for every nested loop
	// but an index one, whose inner side cannot run before the outer row it
	// is parameterised by exists.  See TranslateDXLHashJoin for what the field
	// is for, and why PostgreSQL 19 needs none.

	CDXLTranslationContextArray *translation_context_arr_with_siblings =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	Plan *left_plan = nullptr;
	Plan *right_plan = nullptr;
	if (dxl_nlj->IsIndexNLJ())
	{
		const CDXLColRefArray *pdrgdxlcrOuterRefs =
			dxl_nlj->GetNestLoopParamsColRefs();
		const ULONG ulLen = pdrgdxlcrOuterRefs->Size();
		for (ULONG ul = 0; ul < ulLen; ul++)
		{
			CDXLColRef *pdxlcr = (*pdrgdxlcrOuterRefs)[ul];
			IMDId *pmdid = pdxlcr->MdidType();
			ULONG ulColid = pdxlcr->Id();
			INT iTypeModifier = pdxlcr->TypeModifier();
			OID iTypeOid = CMDIdGPDB::CastMdid(pmdid)->Oid();

			if (nullptr ==
				right_dxl_translate_ctxt.GetParamIdMappingElement(ulColid))
			{
				ULONG param_id =
					m_dxl_to_plstmt_context->GetNextParamId(iTypeOid);
				CMappingElementColIdParamId *pmecolidparamid =
					GPOS_NEW(m_mp) CMappingElementColIdParamId(
						ulColid, param_id, pmdid, iTypeModifier);
#ifdef GPOS_DEBUG
				BOOL fInserted GPOS_ASSERTS_ONLY =
#endif
					right_dxl_translate_ctxt.FInsertParamMapping(
						ulColid, pmecolidparamid);
				GPOS_ASSERT(fInserted);
			}
		}
		// right child (the index scan side) has references to left child's columns,
		// we need to translate left child first to load its columns into translation context
		left_plan = TranslateDXLOperatorToPlan(left_tree_dxlnode,
											   &left_dxl_translate_ctxt,
											   ctxt_translation_prev_siblings);

		translation_context_arr_with_siblings->Append(&left_dxl_translate_ctxt);
		translation_context_arr_with_siblings->AppendArray(
			ctxt_translation_prev_siblings);

		// translate right child after left child translation is complete
		right_plan = TranslateDXLOperatorToPlan(
			right_tree_dxlnode, &right_dxl_translate_ctxt,
			translation_context_arr_with_siblings);
	}
	else
	{
		// left child may include a PartitionSelector with references to right child's columns,
		// we need to translate right child first to load its columns into translation context
		right_plan = TranslateDXLOperatorToPlan(right_tree_dxlnode,
												&right_dxl_translate_ctxt,
												ctxt_translation_prev_siblings);

		translation_context_arr_with_siblings->Append(
			&right_dxl_translate_ctxt);
		translation_context_arr_with_siblings->AppendArray(
			ctxt_translation_prev_siblings);

		// translate left child after right child translation is complete
		left_plan = TranslateDXLOperatorToPlan(
			left_tree_dxlnode, &left_dxl_translate_ctxt,
			translation_context_arr_with_siblings);
	}
	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&left_dxl_translate_ctxt);
	child_contexts->Append(&right_dxl_translate_ctxt);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &plan->qual,
							   output_context);

	// translate join condition
	join->joinqual = TranslateDXLFilterToQual(
		join_filter_dxlnode,
		nullptr,  // translate context for the base table
		child_contexts, output_context);

	// create nest loop params for index nested loop joins
	if (dxl_nlj->IsIndexNLJ())
	{
		((NestLoop *) plan)->nestParams = TranslateNestLoopParamList(
			dxl_nlj->GetNestLoopParamsColRefs(), &left_dxl_translate_ctxt,
			&right_dxl_translate_ctxt);
	}
	plan->lefttree = left_plan;
	plan->righttree = right_plan;
	SetParamIds(plan);

	// cleanup
	translation_context_arr_with_siblings->Release();
	child_contexts->Release();

	return (Plan *) nested_loop;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLMergeJoin
//
//	@doc:
//		Translates a DXL merge join node into a MergeJoin node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLMergeJoin(
	const CDXLNode *merge_join_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	GPOS_ASSERT(merge_join_dxlnode->GetOperator()->GetDXLOperator() ==
				EdxlopPhysicalMergeJoin);
	GPOS_ASSERT(merge_join_dxlnode->Arity() == EdxlmjIndexSentinel);

	// create merge join node
	MergeJoin *merge_join = MakeNode(MergeJoin);

	Join *join = &(merge_join->join);
	Plan *plan = &(join->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	CDXLPhysicalMergeJoin *merge_join_dxlop =
		CDXLPhysicalMergeJoin::Cast(merge_join_dxlnode->GetOperator());

	// set join type
	join->jointype =
		GetGPDBJoinTypeFromDXLJoinType(merge_join_dxlop->GetJoinType());

	// translate operator costs
	TranslatePlanCosts(merge_join_dxlnode, plan);

	// translate join children
	CDXLNode *left_tree_dxlnode = (*merge_join_dxlnode)[EdxlmjIndexLeftChild];
	CDXLNode *right_tree_dxlnode = (*merge_join_dxlnode)[EdxlmjIndexRightChild];

	CDXLNode *project_list_dxlnode = (*merge_join_dxlnode)[EdxlmjIndexProjList];
	CDXLNode *filter_dxlnode = (*merge_join_dxlnode)[EdxlmjIndexFilter];
	CDXLNode *join_filter_dxlnode =
		(*merge_join_dxlnode)[EdxlmjIndexJoinFilter];
	CDXLNode *merge_cond_list_dxlnode =
		(*merge_join_dxlnode)[EdxlmjIndexMergeCondList];

	CDXLTranslateContext left_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());
	CDXLTranslateContext right_dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());

	Plan *left_plan =
		TranslateDXLOperatorToPlan(left_tree_dxlnode, &left_dxl_translate_ctxt,
								   ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *translation_context_arr_with_siblings =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	translation_context_arr_with_siblings->Append(&left_dxl_translate_ctxt);
	translation_context_arr_with_siblings->AppendArray(
		ctxt_translation_prev_siblings);

	Plan *right_plan = TranslateDXLOperatorToPlan(
		right_tree_dxlnode, &right_dxl_translate_ctxt,
		translation_context_arr_with_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&left_dxl_translate_ctxt);
	child_contexts->Append(&right_dxl_translate_ctxt);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &plan->qual,
							   output_context);

	// translate join filter
	join->joinqual = TranslateDXLFilterToQual(
		join_filter_dxlnode,
		nullptr,  // translate context for the base table
		child_contexts, output_context);

	// translate merge cond
	List *merge_conditions_list = NIL;

	const ULONG num_join_conds = merge_cond_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < num_join_conds; ul++)
	{
		CDXLNode *merge_condition_dxlnode = (*merge_cond_list_dxlnode)[ul];
		List *merge_condition_list =
			TranslateDXLScCondToQual(merge_condition_dxlnode,
									 nullptr,  // base table translation context
									 child_contexts, output_context);

		GPOS_ASSERT(1 == gpdb::ListLength(merge_condition_list));
		merge_conditions_list =
			gpdb::ListConcat(merge_conditions_list, merge_condition_list);
	}

	GPOS_ASSERT(NIL != merge_conditions_list);

	merge_join->mergeclauses = merge_conditions_list;

	plan->lefttree = left_plan;
	plan->righttree = right_plan;
	SetParamIds(plan);

	// PostgreSQL 18 replaced the per-clause btree strategy numbers with
	// mergeReversals, a flag saying whether each clause's sort is descending.
	// ORCA sorts both sides ascending, NULLs last, for a merge join -- which
	// is what Cloudberry's BTLessStrategyNumber and NullsFirst = false said,
	// and must match CPhysicalFullMergeJoin::PosRequired() -- so no clause is
	// reversed.
	merge_join->mergeFamilies =
		(Oid *) gpdb::GPDBAlloc(sizeof(Oid) * num_join_conds);
	merge_join->mergeCollations =
		(Oid *) gpdb::GPDBAlloc(sizeof(Oid) * num_join_conds);
	merge_join->mergeReversals =
		(bool *) gpdb::GPDBAlloc(sizeof(bool) * num_join_conds);
	merge_join->mergeNullsFirst =
		(bool *) gpdb::GPDBAlloc(sizeof(bool) * num_join_conds);

	ListCell *lc;
	ULONG ul = 0;
	foreach (lc, merge_join->mergeclauses)
	{
		Expr *expr = (Expr *) lfirst(lc);

		if (IsA(expr, OpExpr))
		{
			// we are ok - phew
			OpExpr *opexpr = (OpExpr *) expr;
			List *mergefamilies = gpdb::GetMergeJoinOpFamilies(opexpr->opno);

			GPOS_ASSERT(nullptr != mergefamilies &&
						gpdb::ListLength(mergefamilies) > 0);

			// Pick the first - it's probably what we want
			merge_join->mergeFamilies[ul] = gpdb::ListNthOid(mergefamilies, 0);

			GPOS_ASSERT(gpdb::ListLength(opexpr->args) == 2);
			Expr *leftarg = (Expr *) gpdb::ListNth(opexpr->args, 0);

			Expr *rightarg PG_USED_FOR_ASSERTS_ONLY =
				(Expr *) gpdb::ListNth(opexpr->args, 1);
			GPOS_ASSERT(gpdb::ExprCollation((Node *) leftarg) ==
						gpdb::ExprCollation((Node *) rightarg));

			merge_join->mergeCollations[ul] =
				gpdb::ExprCollation((Node *) leftarg);

			merge_join->mergeReversals[ul] = false;
			merge_join->mergeNullsFirst[ul] = false;
			++ul;
		}
		else
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
					   GPOS_WSZ_LIT("Not an op expression in merge clause"));
			break;
		}
	}

	// cleanup
	translation_context_arr_with_siblings->Release();
	child_contexts->Release();

	return (Plan *) merge_join;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLHash
//
//	@doc:
//		Translates a DXL physical operator node into a Hash node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLHash(
	const CDXLNode *dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	Hash *hash = MakeNode(Hash);

	Plan *plan = &(hash->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate dxl node
	CDXLTranslateContext dxl_translate_ctxt(
		m_mp, false, output_context->GetColIdToParamIdMap());

	Plan *left_plan = TranslateDXLOperatorToPlan(
		dxlnode, &dxl_translate_ctxt, ctxt_translation_prev_siblings);

	GPOS_ASSERT(0 < dxlnode->Arity());

	// create a reference to each entry in the child project list to create the target list of
	// the hash node
	CDXLNode *project_list_dxlnode = (*dxlnode)[0];
	List *target_list = TranslateDXLProjectListToHashTargetList(
		project_list_dxlnode, &dxl_translate_ctxt, output_context);

	// copy costs from child node; the startup cost for the hash node is the total cost
	// of the child plan, see make_hash in createplan.c
	plan->startup_cost = left_plan->total_cost;
	plan->total_cost = left_plan->total_cost;
	plan->plan_rows = left_plan->plan_rows;
	plan->plan_width = left_plan->plan_width;

	plan->targetlist = target_list;
	plan->lefttree = left_plan;
	plan->righttree = nullptr;
	plan->qual = NIL;

	// Not Cloudberry's rescannable, a Hash field of its own that keeps the
	// hash table's batches on disk for a rescan of a hash join below a
	// Motion.  PostgreSQL 19's hash join rebuilds or keeps its table on a
	// rescan by itself, from whether the inner side's parameters changed.
	//
	// No skew table: skewTable stays InvalidOid, as it does for a planner's
	// hash join whose outer side is not a plain relation scan.

	SetParamIds(plan);

	return (Plan *) hash;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDuplicateSensitiveMotion
//
//	@doc:
//		Translate DXL motion node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDuplicateSensitiveMotion(
	const CDXLNode *motion_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalMotion *motion_dxlop =
		CDXLPhysicalMotion::Cast(motion_dxlnode->GetOperator());
	if (CTranslatorUtils::IsDuplicateSensitiveMotion(motion_dxlop))
	{
		return TranslateDXLRedistributeMotionToResultHashFilters(
			motion_dxlnode, output_context, ctxt_translation_prev_siblings);
	}

	return TranslateDXLMotion(motion_dxlnode, output_context,
							  ctxt_translation_prev_siblings);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLMotion
//
//	@doc:
//		Translate DXL motion node into GPDB Motion plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLMotion(
	const CDXLNode *motion_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// M2: Motion.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("Motion");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLRedistributeMotionToResultHashFilters
//
//	@doc:
//		Translate DXL duplicate sensitive redistribute motion node into
//		GPDB result node with hash filters
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLRedistributeMotionToResultHashFilters(
	const CDXLNode *motion_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// M2: Motion.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("Motion");
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateAggFillInfo
//
//	@doc:
//		Fill the aggregate node with aggno and aggtransno
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateAggFillInfo(CContextDXLToPlStmt *ctx,
											 Aggref *aggref)
{
	Oid aggtransfn;
	Oid aggfinalfn;
	Oid aggcombinefn;
	Oid aggserialfn;
	Oid aggdeserialfn;
	Oid aggtranstype;
	int32 aggtranstypmod;
	int32 aggtransspace;

	Datum initValue;
	bool initValueIsNull;
	List *same_input_transnos;

	bool shareable;
	int16 resulttypeLen;
	bool resulttypeByVal;
	int16 transtypeLen;
	bool transtypeByVal;

	int aggno, transno;

	gpdb::GetAggregateInfo(aggref, &aggtransfn, &aggfinalfn,
						   &aggcombinefn, &aggserialfn, &aggdeserialfn,
						   &aggtranstype, &aggtransspace, &initValue,
						   &initValueIsNull, &shareable);

	/*
	 * If transition state is of same type as first aggregated input, assume
	 * it's the same typmod (same width) as well.  This works for cases like
	 * MAX/MIN and is probably somewhat reasonable otherwise.
	 */
	aggtranstypmod = -1;
	if (aggref->args)
	{
		TargetEntry *tle = (TargetEntry *) linitial(aggref->args);

		if (aggtranstype == gpdb::ExprType((Node *) tle->expr))
			aggtranstypmod = gpdb::ExprTypeMod((Node *) tle->expr);
	}

	gpdb::TypLenByVal(aggref->aggtype, &resulttypeLen, &resulttypeByVal);

	/*
	 * 1. See if this is identical to another aggregate function call that
	 * we've seen already.
	 */
	aggno = gpdb::FindCompatibleAgg(ctx->GetAggInfos(), aggref,
									&same_input_transnos);
	if (aggno != -1)
	{
		AggInfo *agginfo = (AggInfo *) gpdb::ListNth(ctx->GetAggInfos(), aggno);

		transno = agginfo->transno;
	}
	else
	{
		AggInfo *agginfo = makeNode(AggInfo);

		agginfo->finalfn_oid = aggfinalfn;
		agginfo->aggrefs = list_make1(aggref);
		agginfo->shareable = shareable;

		aggno = gpdb::ListLength(ctx->GetAggInfos());
		ctx->AppendAggInfos(agginfo);

		gpdb::TypLenByVal(aggtranstype, &transtypeLen, &transtypeByVal);

		/*
		 * 2. See if this aggregate can share transition state with another
		 * aggregate that we've initialized already.
		 */
		transno = gpdb::FindCompatibleTrans(
			ctx->GetAggTransInfos(), shareable, aggtransfn, aggtranstype,
			transtypeLen, transtypeByVal, aggcombinefn, aggserialfn,
			aggdeserialfn, initValue, initValueIsNull, same_input_transnos);
		if (transno == -1)
		{
			// A node, not Cloudberry's bare allocation: PostgreSQL 19's
			// find_compatible_trans(), which the compat layer carries
			// verbatim, reads the list with lfirst_node(AggTransInfo, ...),
			// and an entry without the tag fails that on the second
			// aggregate of a query.
			AggTransInfo *transinfo = MakeNode(AggTransInfo);

			transinfo->args = aggref->args;
			transinfo->aggfilter = aggref->aggfilter;
			transinfo->transfn_oid = aggtransfn;
			transinfo->combinefn_oid = aggcombinefn;
			transinfo->serialfn_oid = aggserialfn;
			transinfo->deserialfn_oid = aggdeserialfn;
			transinfo->aggtranstype = aggtranstype;
			transinfo->aggtranstypmod = aggtranstypmod;
			transinfo->transtypeLen = transtypeLen;
			transinfo->transtypeByVal = transtypeByVal;
			transinfo->aggtransspace = aggtransspace;
			transinfo->initValue = initValue;
			transinfo->initValueIsNull = initValueIsNull;

			transno = gpdb::ListLength(ctx->GetAggTransInfos());
			ctx->AppendAggTransInfos(transinfo);
		}
		agginfo->transno = transno;
	}

	// setting the aggno and transno
	aggref->aggno = aggno;
	aggref->aggtransno = transno;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLAgg
//
//	@doc:
//		Translate DXL aggregate node into GPDB Agg plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLAgg(
	const CDXLNode *agg_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// create aggregate plan node
	Agg *agg = MakeNode(Agg);

	Plan *plan = &(agg->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	CDXLPhysicalAgg *dxl_phy_agg_dxlop =
		CDXLPhysicalAgg::Cast(agg_dxlnode->GetOperator());

	// translate operator costs
	TranslatePlanCosts(agg_dxlnode, plan);

	// translate agg child
	CDXLNode *child_dxlnode = (*agg_dxlnode)[EdxlaggIndexChild];

	CDXLNode *project_list_dxlnode = (*agg_dxlnode)[EdxlaggIndexProjList];
	CDXLNode *filter_dxlnode = (*agg_dxlnode)[EdxlaggIndexFilter];

	CDXLTranslateContext child_context(m_mp, true,
									   output_context->GetColIdToParamIdMap());

	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts,	// pdxltrctxRight,
							   &plan->targetlist, &plan->qual, output_context);

	plan->lefttree = child_plan;

	// translate aggregation strategy
	switch (dxl_phy_agg_dxlop->GetAggStrategy())
	{
		case EdxlaggstrategyPlain:
			agg->aggstrategy = AGG_PLAIN;
			break;
		case EdxlaggstrategySorted:
			agg->aggstrategy = AGG_SORTED;
			break;
		case EdxlaggstrategyHashed:
			agg->aggstrategy = AGG_HASHED;
			break;
		default:
			GPOS_ASSERT(!"Invalid aggregation strategy");
	}

	if (agg->aggstrategy == AGG_HASHED &&
		CTranslatorUtils::HasOrderedAggRefInProjList(project_list_dxlnode))
	{
		GPOS_RAISE(gpopt::ExmaDXL, gpopt::ExmiExpr2DXLUnsupportedFeature,
				   GPOS_WSZ_LIT("Hash aggregation with ORDER BY"));
	}

	// Not agg->streaming: PostgreSQL 19's Agg has no such field.  A streaming
	// hash aggregate is Cloudberry's partial aggregate that, when its hash
	// table fills, passes its groups up unfinished rather than spilling them,
	// which is correct only because a later stage re-aggregates them.
	// PostgreSQL 19's hash aggregate spills instead, and the rows that come
	// out are the same.

	// translate grouping cols
	const ULongPtrArray *grouping_colid_array =
		dxl_phy_agg_dxlop->GetGroupingColidArray();
	agg->numCols = grouping_colid_array->Size();
	if (agg->numCols > 0)
	{
		agg->grpColIdx =
			(AttrNumber *) gpdb::GPDBAlloc(agg->numCols * sizeof(AttrNumber));
		agg->grpOperators = (Oid *) gpdb::GPDBAlloc(agg->numCols * sizeof(Oid));
		agg->grpCollations =
			(Oid *) gpdb::GPDBAlloc(agg->numCols * sizeof(Oid));
	}
	else
	{
		agg->grpColIdx = nullptr;
		agg->grpOperators = nullptr;
		agg->grpCollations = nullptr;
	}

	const ULONG length = grouping_colid_array->Size();
	for (ULONG ul = 0; ul < length; ul++)
	{
		ULONG grouping_colid = *((*grouping_colid_array)[ul]);
		const TargetEntry *target_entry_grouping_col =
			child_context.GetTargetEntry(grouping_colid);
		if (nullptr == target_entry_grouping_col)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
					   grouping_colid);
		}
		agg->grpColIdx[ul] = target_entry_grouping_col->resno;

		// Also find the equality operators to use for each grouping col.
		Oid typeId = gpdb::ExprType((Node *) target_entry_grouping_col->expr);
		agg->grpOperators[ul] = gpdb::GetEqualityOp(typeId);
		agg->grpCollations[ul] =
			gpdb::ExprCollation((Node *) target_entry_grouping_col->expr);
		Assert(agg->grpOperators[ul] != 0);
	}

	agg->numGroups =
		std::max(1L, (long) std::min(agg->plan.plan_rows, (double) LONG_MAX));

	// Set the aggsplit,aggno,aggtransno for the agg node
	ListCell *lc;
	INT aggsplit = 0;
	ForEach (lc, plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		if (IsA(te->expr, Aggref))
		{
			Aggref *aggref = (Aggref *) te->expr;

			if (AGGSPLIT_INTERMEDIATE != aggsplit)
			{
				aggsplit |= aggref->aggsplit;
			}
			TranslateAggFillInfo(m_dxl_to_plstmt_context, aggref);
		}
	}
	agg->aggsplit = (AggSplit) aggsplit;

	ForEach (lc, plan->qual)
	{
		Expr *expr = (Expr *) lfirst(lc);
		if (IsA(expr, Aggref))
		{
			Aggref *aggref = (Aggref *) expr;
			// ORCA won't create the qual but a scalar in AGG
			TranslateAggFillInfo(m_dxl_to_plstmt_context, aggref);
		}
	}

	// One split mode per Agg.  PostgreSQL 19's executor runs an Agg in the
	// node's mode and asserts that each of its Aggrefs has that mode too
	// (nodeAgg.c, "aggref->aggsplit == aggstate->aggsplit"); Cloudberry's
	// decides how to finish each Aggref by the Aggref's own mode, and ORCA
	// counts on it -- it puts a finished aggregate beside a partial one in a
	// single Agg when a DISTINCT aggregate meets multi-stage aggregation.
	// The OR above would run such a plan in a mode half of its aggregates do
	// not have.  It is refused instead, and falls back.
	{
		// extract_nodes_expression() asserts it is given a node, and an Agg
		// with no qual or no target list has NIL for one.
		List *aggrefs = NIL;
		if (NIL != plan->targetlist)
		{
			aggrefs = gpdb::ExtractNodesExpression(
				(Node *) plan->targetlist, T_Aggref,
				false /*descend_into_subqueries*/);
		}
		if (NIL != plan->qual)
		{
			aggrefs = gpdb::ListConcat(
				aggrefs, gpdb::ExtractNodesExpression(
							 (Node *) plan->qual, T_Aggref,
							 false /*descend_into_subqueries*/));
		}
		ForEach (lc, aggrefs)
		{
			if (((Aggref *) lfirst(lc))->aggsplit != agg->aggsplit)
			{
				GP_UNPORTED(
					"an aggregate that mixes aggregation stages in one node");
			}
		}
	}

	m_dxl_to_plstmt_context->ResetAggInfosAndTransInfos();

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return (Plan *) agg;
}

static
int WindowFrameSpecToOptions(const EdxlFrameSpec &dxlFS) {
	int winFrameOptions = 0;
	if (EdxlfsRow == dxlFS)
	{
		winFrameOptions |= FRAMEOPTION_ROWS;
	}
	else if (EdxlfsGroups == dxlFS)
	{
		winFrameOptions |= FRAMEOPTION_GROUPS;
	}
	else
	{
		winFrameOptions |= FRAMEOPTION_RANGE;
	}
	return winFrameOptions;
}

static
int WindowFrameExclusionStrategyToOptions(const EdxlFrameExclusionStrategy &dxlFES) {
	int winFrameOptions = 0;
	if (dxlFES == EdxlfesCurrentRow)
	{
		winFrameOptions |= FRAMEOPTION_EXCLUDE_CURRENT_ROW;
	}
	else if (dxlFES == EdxlfesGroup)
	{
		winFrameOptions |= FRAMEOPTION_EXCLUDE_GROUP;
	}
	else if (dxlFES == EdxlfesTies)
	{
		winFrameOptions |= FRAMEOPTION_EXCLUDE_TIES;
	}

	return winFrameOptions;
}

static
int WindowFrameStartBoundaryToOptions(const EdxlFrameBoundary &dxlFB) {
	int winFrameOptions = 0;
	if (dxlFB == EdxlfbUnboundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_START_UNBOUNDED_PRECEDING;
	}
	if (dxlFB == EdxlfbBoundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_START_OFFSET_PRECEDING;
	}
	if (dxlFB == EdxlfbCurrentRow)
	{
		winFrameOptions |= FRAMEOPTION_START_CURRENT_ROW;
	}
	if (dxlFB == EdxlfbBoundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_START_OFFSET_FOLLOWING;
	}
	if (dxlFB == EdxlfbUnboundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_START_UNBOUNDED_FOLLOWING;
	}
	if (dxlFB == EdxlfbDelayedBoundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_START_OFFSET_PRECEDING;
	}
	if (dxlFB == EdxlfbDelayedBoundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_START_OFFSET_FOLLOWING;
	}
	return winFrameOptions;
}

static
int WindowFrameEndBoundaryToOptions(const EdxlFrameBoundary &dxlFB) {
	int winFrameOptions = 0;
	if (dxlFB == EdxlfbUnboundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_END_UNBOUNDED_PRECEDING;
	}
	if (dxlFB == EdxlfbBoundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_END_OFFSET_PRECEDING;
	}
	if (dxlFB == EdxlfbCurrentRow)
	{
		winFrameOptions |= FRAMEOPTION_END_CURRENT_ROW;
	}
	if (dxlFB == EdxlfbBoundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_END_OFFSET_FOLLOWING;
	}
	if (dxlFB == EdxlfbUnboundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_END_UNBOUNDED_FOLLOWING;
	}
	if (dxlFB == EdxlfbDelayedBoundedPreceding)
	{
		winFrameOptions |= FRAMEOPTION_END_OFFSET_PRECEDING;
	}
	if (dxlFB == EdxlfbDelayedBoundedFollowing)
	{
		winFrameOptions |= FRAMEOPTION_END_OFFSET_FOLLOWING;
	}
	return winFrameOptions;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLWindowAgg
//
//	@doc:
//		Translate DXL window node into GPDB window plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLWindowAgg(
	const CDXLNode *window_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// create a WindowAgg plan node
	WindowAgg *window = MakeNode(WindowAgg);

	Plan *plan = &(window->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	CDXLPhysicalWindow *window_dxlop =
		CDXLPhysicalWindow::Cast(window_dxlnode->GetOperator());

	// translate the operator costs
	TranslatePlanCosts(window_dxlnode, plan);

	// translate children
	CDXLNode *child_dxlnode = (*window_dxlnode)[EdxlwindowIndexChild];
	CDXLNode *project_list_dxlnode = (*window_dxlnode)[EdxlwindowIndexProjList];
	CDXLNode *filter_dxlnode = (*window_dxlnode)[EdxlwindowIndexFilter];

	CDXLTranslateContext child_context(m_mp, true,
									   output_context->GetColIdToParamIdMap());
	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts,	// pdxltrctxRight,
							   &plan->targetlist, &plan->qual, output_context);

	ListCell *lc;

	foreach (lc, plan->targetlist)
	{
		TargetEntry *target_entry = (TargetEntry *) lfirst(lc);
		if (IsA(target_entry->expr, WindowFunc))
		{
			WindowFunc *window_func = (WindowFunc *) target_entry->expr;
			window->winref = window_func->winref;
			break;
		}
	}

	// PostgreSQL 18 made EXPLAIN print each window's definition under its
	// name, and it quotes the name without asking whether there is one.  The
	// planner's are the query's window names, or w1, w2 and so on; ORCA's
	// DXL carries no name, so the window is named for its winref, the same
	// way.
	CHAR winname[NAMEDATALEN];
	snprintf(winname, sizeof(winname), "w%u", window->winref);
	window->winname = PStrDup(winname);

	// PostgreSQL 15's run conditions, which the planner derives from a
	// monotonic window function under a filter, and ORCA does not.  With none,
	// topWindow decides one thing: whether the node may carry a qual, which
	// PostgreSQL 19 asserts only the top window does, because its run-condition
	// logic filters there.  ORCA puts a filter on whichever window node it
	// chose, and without a run condition the executor applies a qual in any of
	// them the same way (nodeWindowAgg.c), so every one says it is the top.
	window->runCondition = NIL;
	window->runConditionOrig = NIL;
	window->topWindow = true;

	plan->lefttree = child_plan;

	// translate partition columns
	const ULongPtrArray *part_by_cols_array =
		window_dxlop->GetPartByColsArray();
	window->partNumCols = part_by_cols_array->Size();

	if (window->partNumCols > 0)
	{
		window->partColIdx = (AttrNumber *) gpdb::GPDBAlloc(
			window->partNumCols * sizeof(AttrNumber));
		window->partOperators =
			(Oid *) gpdb::GPDBAlloc(window->partNumCols * sizeof(Oid));
		window->partCollations =
			(Oid *) gpdb::GPDBAlloc(window->partNumCols * sizeof(Oid));
	} else {
		window->partColIdx = nullptr;
		window->partOperators = nullptr;
		window->partCollations = nullptr;
	}

	const ULONG num_of_part_cols = part_by_cols_array->Size();
	for (ULONG ul = 0; ul < num_of_part_cols; ul++)
	{
		ULONG part_colid = *((*part_by_cols_array)[ul]);
		const TargetEntry *te_part_colid =
			child_context.GetTargetEntry(part_colid);
		if (nullptr == te_part_colid)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
					   part_colid);
		}
		window->partColIdx[ul] = te_part_colid->resno;

		// Also find the equality operators to use for each partitioning key col.
		Oid type_id = gpdb::ExprType((Node *) te_part_colid->expr);
		window->partOperators[ul] = gpdb::GetEqualityOp(type_id);
		Assert(window->partOperators[ul] != 0);
		window->partCollations[ul] =
			gpdb::ExprCollation((Node *) te_part_colid->expr);
	}

	// translate window keys
	const ULONG size = window_dxlop->WindowKeysCount();
	if (size > 1)
	{
		GpdbEreport(ERRCODE_INTERNAL_ERROR, ERROR,
					"ORCA produced a plan with more than one window key",
					nullptr);
	}
	GPOS_ASSERT(size <= 1 && "cannot have more than one window key");

	if (size == 1)
	{
		// translate the sorting columns used in the window key
		const CDXLWindowKey *window_key = window_dxlop->GetDXLWindowKeyAt(0);
		const CDXLWindowFrame *window_frame = window_key->GetWindowFrame();
		const CDXLNode *sort_col_list_dxlnode = window_key->GetSortColListDXL();

		const ULONG num_of_cols = sort_col_list_dxlnode->Arity();

		window->ordNumCols = num_of_cols;
		window->ordColIdx =
			(AttrNumber *) gpdb::GPDBAlloc(num_of_cols * sizeof(AttrNumber));
		window->ordOperators =
			(Oid *) gpdb::GPDBAlloc(num_of_cols * sizeof(Oid));
		window->ordCollations =
			(Oid *) gpdb::GPDBAlloc(num_of_cols * sizeof(Oid));
		bool *is_nulls_first =
			(bool *) gpdb::GPDBAlloc(num_of_cols * sizeof(bool));
		TranslateSortCols(sort_col_list_dxlnode, &child_context,
						  window->ordColIdx, window->ordOperators,
						  window->ordCollations, is_nulls_first);

		// Not Cloudberry's firstOrderCol, firstOrderCmpOperator and
		// firstOrderNullsFirst, WindowAgg fields of its own that PostgreSQL
		// 19's executor finds from the window frame instead.
		gpdb::GPDBFree(is_nulls_first);

		// The ordOperators array is actually supposed to contain equality operators,
		// not ordering operators (< or >). So look up the corresponding equality
		// operator for each ordering operator.
		for (ULONG i = 0; i < num_of_cols; i++)
		{
			window->ordOperators[i] = gpdb::GetEqualityOpForOrderingOp(
				window->ordOperators[i], nullptr);
		}

		// translate the window frame specified in the window key
		if (nullptr != window_key->GetWindowFrame())
		{
			window->frameOptions = FRAMEOPTION_NONDEFAULT | FRAMEOPTION_BETWEEN;
			window->frameOptions |= WindowFrameSpecToOptions(window_frame->ParseDXLFrameSpec());
			window->frameOptions |= WindowFrameExclusionStrategyToOptions(
				window_frame->ParseFrameExclusionStrategy());

			// translate the CDXLNodes representing the leading and trailing edge
			CDXLTranslationContextArray *child_contexts =
				GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
			child_contexts->Append(&child_context);

			CMappingColIdVarPlStmt colid_var_mapping =
				CMappingColIdVarPlStmt(m_mp, nullptr, child_contexts,
									   output_context, m_dxl_to_plstmt_context);

			// Translate lead boundary
			//
			// Note that we don't distinguish between the delayed and undelayed
			// versions beoynd this point. Executor will make that decision
			// without our help.
			//
			CDXLNode *win_frame_leading_dxlnode = window_frame->PdxlnLeading();
			window->frameOptions |= WindowFrameStartBoundaryToOptions(
					CDXLScalarWindowFrameEdge::Cast(win_frame_leading_dxlnode->GetOperator())
					->ParseDXLFrameBoundary());
			if (0 != win_frame_leading_dxlnode->Arity())
			{
				window->startOffset =
					(Node *) m_translator_dxl_to_scalar->TranslateDXLToScalar(
						(*win_frame_leading_dxlnode)[0], &colid_var_mapping);
			}

			// And the same for the trail boundary
			CDXLNode *win_frame_trailing_dxlnode =
				window_frame->PdxlnTrailing();
			window->frameOptions |= WindowFrameEndBoundaryToOptions(
				CDXLScalarWindowFrameEdge::Cast(
					win_frame_trailing_dxlnode->GetOperator())
					->ParseDXLFrameBoundary());

			if (0 != win_frame_trailing_dxlnode->Arity())
			{
				window->endOffset =
					(Node *) m_translator_dxl_to_scalar->TranslateDXLToScalar(
						(*win_frame_trailing_dxlnode)[0], &colid_var_mapping);
			}

			// PostgreSQL 19's WindowAgg evaluates a frame offset once, before
			// it has a row to evaluate it over (nodeWindowAgg.c,
			// calculate_frame_offsets), which is why its parser refuses an
			// offset that names a column.  Greenplum allows one, and
			// Cloudberry's WindowAgg evaluates the offset per row, so ORCA
			// may hand one down: a scalar subquery in the offset comes back
			// as a column of a join beneath the window, and a generic plan's
			// parameter as a column its cast is projected into.  Given to
			// PostgreSQL 19's executor, that Var is read with no slot, and
			// the backend crashes; so such a plan is refused, and the planner
			// evaluates the offset as an initplan or a parameter.
			if ((nullptr != window->startOffset &&
				 gpdb::ContainsVars(window->startOffset)) ||
				(nullptr != window->endOffset &&
				 gpdb::ContainsVars(window->endOffset)))
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
						   GPOS_WSZ_LIT("a window frame offset that reads a column"));
			}

			window->startInRangeFunc = window_frame->PdxlnStartInRangeFunc();
			window->endInRangeFunc = window_frame->PdxlnEndInRangeFunc();
			window->inRangeColl = window_frame->PdxlnInRangeColl();
			window->inRangeAsc = window_frame->PdxlnInRangeAsc();
			window->inRangeNullsFirst = window_frame->PdxlnInRangeNullsFirst();

			// cleanup
			child_contexts->Release();
		}
		else
		{
			window->frameOptions = FRAMEOPTION_DEFAULTS;
		}
	}

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return (Plan *) window;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLWindowHashAgg
//
//	@doc:
//		Translate DXL window node into GPDB window hash plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLWindowHashAgg(
	const CDXLNode *window_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// after M7, with a vectorized executor: hashed window aggregation.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("hashed window aggregation");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLSort
//
//	@doc:
//		Translate DXL sort node into GPDB Sort plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLSort(
	const CDXLNode *sort_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// Ensure operator of sort_dxlnode exists and is EdxlopPhysicalSort
	GPOS_ASSERT(nullptr != sort_dxlnode->GetOperator());
	GPOS_ASSERT(EdxlopPhysicalSort ==
				sort_dxlnode->GetOperator()->GetDXLOperator());

	// create sort plan node
	Sort *sort = MakeNode(Sort);

	Plan *plan = &(sort->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(sort_dxlnode, plan);

	// translate sort child
	CDXLNode *child_dxlnode = (*sort_dxlnode)[EdxlsortIndexChild];
	CDXLNode *project_list_dxlnode = (*sort_dxlnode)[EdxlsortIndexProjList];
	CDXLNode *filter_dxlnode = (*sort_dxlnode)[EdxlsortIndexFilter];

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &plan->qual,
							   output_context);

	plan->lefttree = child_plan;

	// translate sorting columns

	const CDXLNode *sort_col_list_dxl =
		(*sort_dxlnode)[EdxlsortIndexSortColList];

	const ULONG num_of_cols = sort_col_list_dxl->Arity();
	sort->numCols = num_of_cols;
	sort->sortColIdx =
		(AttrNumber *) gpdb::GPDBAlloc(num_of_cols * sizeof(AttrNumber));
	sort->sortOperators = (Oid *) gpdb::GPDBAlloc(num_of_cols * sizeof(Oid));
	sort->collations = (Oid *) gpdb::GPDBAlloc(num_of_cols * sizeof(Oid));
	sort->nullsFirst = (bool *) gpdb::GPDBAlloc(num_of_cols * sizeof(bool));

	TranslateSortCols(sort_col_list_dxl, &child_context, sort->sortColIdx,
					  sort->sortOperators, sort->collations, sort->nullsFirst);

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return (Plan *) sort;
}

//------------------------------------------------------------------------------
// If the top level is not a function returning set then we need to check if the
// project element contains any SRF's deep down the tree. If we found any SRF's
// at lower levels then we will require a result node on top of ProjectSet node.
// Eg.
// <dxl:ProjElem ColId="1" Alias="abs">
//  <dxl:FuncExpr FuncId="0.1397.1.0" FuncRetSet="false" TypeMdid="0.23.1.0">
//   <dxl:FuncExpr FuncId="0.1067.1.0" FuncRetSet="true" TypeMdid="0.23.1.0">
//    ...
//   </dxl:FuncExpr>
//  </dxl:FuncExpr>
// Here we have SRF present at a lower level. So we will require a result node
// on top.
//------------------------------------------------------------------------------
static BOOL
ContainsLowLevelSetReturningFunc(const CDXLNode *scalar_expr_dxlnode)
{
	const ULONG arity = scalar_expr_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *expr_dxlnode = (*scalar_expr_dxlnode)[ul];
		CDXLOperator *op = expr_dxlnode->GetOperator();
		Edxlopid dxlopid = op->GetDXLOperator();

		if ((EdxlopScalarFuncExpr == dxlopid &&
			 CDXLScalarFuncExpr::Cast(op)->ReturnsSet()) ||
			ContainsLowLevelSetReturningFunc(expr_dxlnode))
		{
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------
// This method is required to check if we need a result node on top of
// ProjectSet node. If the project element contains SRF on top then we don't
// require a result node. Eg
//  <dxl:ProjElem ColId="1" Alias="generate_series">
//   <dxl:FuncExpr FuncId="0.1067.1.0" FuncRetSet="true" TypeMdid="0.23.1.0">
//    ...
//    <dxl:FuncExpr FuncId="0.1067.1.0" FuncRetSet="true" TypeMdid="0.23.1.0">
//     ...
//    </dxl:FuncExpr>
//     ...
//   </dxl:FuncExpr>
// Here we have a FuncExpr which returns a set on top. So we don't require a
// result node on top of ProjectSet node.
//------------------------------------------------------------------------------
static BOOL
RequiresResultNode(const CDXLNode *project_list_dxlnode)
{
	const ULONG arity = project_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ++ul)
	{
		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		GPOS_ASSERT(EdxlopScalarProjectElem ==
					proj_elem_dxlnode->GetOperator()->GetDXLOperator());
		GPOS_ASSERT(1 == proj_elem_dxlnode->Arity());
		CDXLNode *expr_dxlnode = (*proj_elem_dxlnode)[0];
		CDXLOperator *op = expr_dxlnode->GetOperator();
		Edxlopid dxlopid = op->GetDXLOperator();
		if (EdxlopScalarFuncExpr == dxlopid)
		{
			if (!(CDXLScalarFuncExpr::Cast(op)->ReturnsSet()) &&
				ContainsLowLevelSetReturningFunc(expr_dxlnode))
			{
				return true;
			}
		}
		else
		{
			if (ContainsLowLevelSetReturningFunc(expr_dxlnode))
			{
				return true;
			}
		}
	}
	return false;
}

//------------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLProjectSet
//
//	@doc:
//		Translate DXL result node into project set node if SRF's are present
//
//------------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLProjectSet(const CDXLNode *result_dxlnode)
{
	// ORCA_FEATURE_NOT_SUPPORTED: The Project Set nodes don't support a qual in
	// the planned statement. Just being defensive here for the case when the
	// result dxl node has a set returning function in the project list and also
	// a qual. In that case will not create a ProjectSet node and will fall back
	// to planner.
	if ((*result_dxlnode)[EdxlresultIndexFilter]->Arity() > 0)
	{
		GPOS_RAISE(
			gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
			GPOS_WSZ_LIT("Unsupported one-time filter in ProjectSet node"));
	}

	// create project set plan node
	ProjectSet *project_set = MakeNode(ProjectSet);

	Plan *plan = &(project_set->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(result_dxlnode, plan);

	SetParamIds(plan);

	return (Plan *) project_set;
}

//------------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CreateProjectSetNodeTree
//
//	@doc:
//		Creates a tree of project set plan nodes to contain the SRF's
//
//------------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::CreateProjectSetNodeTree(const CDXLNode *result_dxlnode,
												 Plan *result_node_plan,
												 Plan *child_plan,
												 Plan *&project_set_child_plan,
												 BOOL &will_require_result_node)
{
	// Method split_pathtarget_at_srfs will split the given PathTarget into
	// multiple levels to position SRFs safely. This list will hold the splited
	// PathTarget created by split_pathtarget_at_srfs method.
	List *targets_with_srf = NIL;

	// List of bool flags indicating whether the corresponding PathTarget
	// contains any evaluatable SRFs
	List *targets_with_srf_bool = NIL;

	// Pointer to the top level ProjectSet node. If a result node is required
	// then this will be attached to the lefttree of the result node.
	Plan *project_set_parent_plan = nullptr;

	// Create Pathtarget object from Result node's targetlist which is required
	// by SplitPathtargetAtSrfs method
	PathTarget *complete_result_pathtarget =
		gpdb::MakePathtargetFromTlist(result_node_plan->targetlist);

	// Split given PathTarget into multiple levels to position SRFs safely
	gpdb::SplitPathtargetAtSrfs(nullptr, complete_result_pathtarget, nullptr,
								&targets_with_srf, &targets_with_srf_bool);

	// If the PathTarget created from Result node's targetlist does not contain
	// any set returning functions then split_pathtarget_at_srfs method will
	// return the same PathTarget back. In this case a ProjectSet node is not
	// required.
	if (1 == gpdb::ListLength(targets_with_srf))
	{
		return nullptr;
	}

	// Do we require a result node to be attached on top of ProjectSet node?
	will_require_result_node =
		RequiresResultNode((*result_dxlnode)[EdxlresultIndexProjList]);

	ListCell *lc;
	ULONG list_cell_pos = 1;
	ULONG targets_with_srf_list_length = gpdb::ListLength(targets_with_srf);

	ForEach(lc, targets_with_srf)
	{
		// The first element of the PathTarget list created by
		// split_pathtarget_at_srfs method will not contain any
		// SRF's. So skipping it.
		if (list_cell_pos == 1)
		{
			list_cell_pos++;
			continue;
		}

		// If a Result node is required on top of a ProjectSet node then the
		// last element of PathTarget list created by split_pathtarget_at_srfs
		// method will contain the PathTarget of the result node. Since result
		// node is already created before, breaking out from the loop. If a
		// result node is not required on top of a ProjectSet node, continue to
		// create a ProjectSet node.
		if (will_require_result_node &&
			targets_with_srf_list_length == list_cell_pos)
		{
			break;
		}

		list_cell_pos++;

		List *target_list_entry =
			gpdb::MakeTlistFromPathtarget((PathTarget *) lfirst(lc));

		Plan *temp_plan_project_set = TranslateDXLProjectSet(result_dxlnode);

		temp_plan_project_set->targetlist = target_list_entry;

		// Creating the links between all the nested ProjectSet nodes
		if (nullptr == project_set_parent_plan)
		{
			project_set_parent_plan = temp_plan_project_set;
			project_set_child_plan = temp_plan_project_set;
		}
		else
		{
			temp_plan_project_set->lefttree = project_set_parent_plan;
			project_set_parent_plan = temp_plan_project_set;
		}
	}

	return project_set_parent_plan;
}

//---------------------------------------------------------------------------
//	The two rewrites PlaceResultFilter makes of an expression that reads a
//	Result's child through OUTER_VAR: to what the child computes, and to a
//	column of a scan of the child's rows.
//---------------------------------------------------------------------------
namespace
{
struct SOuterVarsContext
{
	List *child_tlist;	// to the child's expressions, when not null
	Index scanrelid;	// else to Vars of this range table entry
};

Node *
RewriteOuterVars(Node *node, void *context)
{
	SOuterVarsContext *ctx = (SOuterVarsContext *) context;

	if (nullptr == node)
	{
		return nullptr;
	}

	if (IsA(node, Var) && OUTER_VAR == ((Var *) node)->varno)
	{
		Var *var = (Var *) node;

		if (nullptr != ctx->child_tlist)
		{
			TargetEntry *te = (TargetEntry *) gpdb::ListNth(
				ctx->child_tlist, var->varattno - 1);
			return (Node *) gpdb::CopyObject(te->expr);
		}

		Var *scan_var = (Var *) gpdb::CopyObject(var);
		scan_var->varno = ctx->scanrelid;
		scan_var->varnosyn = ctx->scanrelid;
		scan_var->varattnosyn = var->varattno;
		return (Node *) scan_var;
	}

	return gpdb::Expression_tree_mutator(node, RewriteOuterVars, context);
}

// Every column of the child the filter reads, and whether all of them can be
// computed a second time: not volatile, and not a set.
BOOL
FilterReadsStableColumns(List *qual, List *child_tlist)
{
	List *vars = gpdb::ExtractNodesExpression((Node *) qual, T_Var,
											  false /*descend_into_subqueries*/);
	ListCell *lc = nullptr;
	ForEach(lc, vars)
	{
		Var *var = (Var *) lfirst(lc);
		if (OUTER_VAR != var->varno)
		{
			continue;
		}
		TargetEntry *te =
			(TargetEntry *) gpdb::ListNth(child_tlist, var->varattno - 1);
		if (gpdb::ContainsVolatileFunctions((Node *) te->expr) ||
			gpdb::ExpressionReturnsSet((Node *) te->expr))
		{
			return false;
		}
	}
	return true;
}
}  // namespace

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlaceResultFilter
//
//	@doc:
//		Put a Result's filter where PostgreSQL 19 evaluates one.
//
//		NEW IN THE PORT, AND IT IS A MATTER OF RESULTS, NOT PLAN SHAPE.
//		PostgreSQL 19's Result evaluates no qual: ExecResult() projects
//		every row its child returns (nodeResult.c), and the planner never
//		gives a Result one.  Cloudberry's evaluates plan.qual too ("GPDB: if
//		there's a non-constant qual, check that too"), and ORCA's translator
//		relies on it for each filter it puts above a node that cannot filter
//		-- a HAVING above an aggregate, a WHERE above a LIMIT.  Left where
//		the translator puts it, the filter is printed by EXPLAIN and never
//		applied, and the query returns rows it should not.  So it goes:
//
//		  * into the Result's one-time filter, when the Result has no child
//		    -- there is nothing for the filter to read but constants and
//		    parameters, and it is evaluated once either way;
//
//		  * into the child's own qual, when the child is a scan or an
//		    aggregate, the nodes that test their qual on the rows they are
//		    about to project -- every scan through ExecScan().  That is where
//		    the planner puts a WHERE and a HAVING.  The filter then computes
//		    the child's columns a second time, so only when none it reads is
//		    volatile.  The scans are T0's and T1's; a CTE Scan is the one
//		    that meets this most, because a CTE consumer in DXL has no filter
//		    of its own, so every filter on one arrives on a Result;
//
//		  * and otherwise under a SubqueryScan of the child, which is
//		    PostgreSQL's node for filtering and projecting another plan's
//		    rows.  It scans a range table entry of its own, as every
//		    SubqueryScan does.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PlaceResultFilter(Result *result)
{
	Plan *plan = &(result->plan);
	Plan *child_plan = plan->lefttree;

	if (NIL == plan->qual)
	{
		return plan;
	}

	if (nullptr == child_plan)
	{
		result->resconstantqual = (Node *) gpdb::ListConcat(
			(List *) result->resconstantqual, plan->qual);
		plan->qual = NIL;
		return plan;
	}

	if ((IsA(child_plan, SeqScan) || IsA(child_plan, ValuesScan) ||
		 IsA(child_plan, IndexScan) || IsA(child_plan, IndexOnlyScan) ||
		 IsA(child_plan, BitmapHeapScan) || IsA(child_plan, CteScan) ||
		 IsA(child_plan, Agg)) &&
		FilterReadsStableColumns(plan->qual, child_plan->targetlist))
	{
		SOuterVarsContext ctx = {child_plan->targetlist, 0};
		List *qual = (List *) RewriteOuterVars((Node *) plan->qual, &ctx);

		child_plan->qual = gpdb::ListConcat(child_plan->qual, qual);
		plan->qual = NIL;
		return plan;
	}

	// A range table entry for the child's rows, named for EXPLAIN.
	RangeTblEntry *rte = MakeNode(RangeTblEntry);
	rte->rtekind = RTE_SUBQUERY;
	rte->inFromCl = false;

	Alias *alias = MakeNode(Alias);
	alias->aliasname = PStrDup("filter");
	alias->colnames = NIL;
	ListCell *lc = nullptr;
	ForEach(lc, child_plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		alias->colnames = gpdb::LAppend(
			alias->colnames,
			gpdb::MakeStringValue(PStrDup(nullptr != te->resname
											  ? te->resname
											  : "?column?")));
	}
	rte->eref = alias;

	m_dxl_to_plstmt_context->AddRTE(rte);
	Index scanrelid =
		gpdb::ListLength(m_dxl_to_plstmt_context->GetRTableEntriesList());

	SubqueryScan *subquery_scan = MakeNode(SubqueryScan);
	subquery_scan->scan.scanrelid = scanrelid;
	subquery_scan->subplan = child_plan;
	subquery_scan->scanstatus = SUBQUERY_SCAN_UNKNOWN;

	Plan *scan_plan = &(subquery_scan->scan.plan);
	SOuterVarsContext ctx = {nullptr, scanrelid};
	scan_plan->targetlist =
		(List *) RewriteOuterVars((Node *) plan->targetlist, &ctx);
	scan_plan->qual = (List *) RewriteOuterVars((Node *) plan->qual, &ctx);
	scan_plan->plan_node_id = plan->plan_node_id;
	scan_plan->startup_cost = plan->startup_cost;
	scan_plan->total_cost = plan->total_cost;
	scan_plan->plan_rows = plan->plan_rows;
	scan_plan->plan_width = plan->plan_width;
	scan_plan->extParam = plan->extParam;
	scan_plan->allParam = plan->allParam;
	scan_plan->initPlan = plan->initPlan;

	// A one-time filter the Result had stays, on a Result above the scan.
	if (nullptr != result->resconstantqual)
	{
		Result *gate = MakeNode(Result);
		gate->result_type = RESULT_TYPE_GATING;
		gate->resconstantqual = result->resconstantqual;
		gate->plan = *plan;
		gate->plan.qual = NIL;
		gate->plan.initPlan = NIL;
		gate->plan.lefttree = scan_plan;
		gate->plan.plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
		gate->plan.targetlist = nullptr;
		ForEach(lc, scan_plan->targetlist)
		{
			TargetEntry *te = (TargetEntry *) lfirst(lc);
			gate->plan.targetlist = gpdb::LAppend(
				gate->plan.targetlist,
				gpdb::MakeTargetEntry(
					(Expr *) gpdb::MakeVar(OUTER_VAR, te->resno,
										   gpdb::ExprType((Node *) te->expr),
										   gpdb::ExprTypeMod((Node *) te->expr),
										   0),
					te->resno, te->resname, te->resjunk));
		}
		return &(gate->plan);
	}

	return scan_plan;
}

//---------------------------------------------------------------------------
//	@function:
//		restore_unknown_locale_resname
//
//	@doc:
//		ORCA represents strings using wide characters. Converting a multibyte
//		name to wide format uses vswprintf(), which depends on the database
//		LC_CTYPE. When that locale cannot interpret the name (e.g. LC_CTYPE=C
//		with a UTF-8 alias), ORCA substitutes the generic "UNKNOWN" string
//		(see gpos::clib::Vswprintf). This function restores the original name
//		from the query tree.
//
//		Only the topmost plan node is translated with a context that carries
//		the original query (see GetPlannedStmtFromDXL); everywhere else query
//		is NULL and this is a no-op. The topmost projection list produces the
//		query's output columns in order, so the original name is the non-junk
//		query targetList entry with the same resno. Matching only top-level
//		entries (never descending into subqueries or expressions) also keeps
//		a legitimate alias named "UNKNOWN" intact: its positional match is
//		the entry itself, making the restore a no-op.
//---------------------------------------------------------------------------
static void
restore_unknown_locale_resname(const Query *query, TargetEntry *target_entry)
{
	if (nullptr == query || 0 != strcmp(target_entry->resname, "UNKNOWN"))
	{
		return;
	}

	ListCell *lc;
	ForEach(lc, query->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		if (!te->resjunk && nullptr != te->resname &&
			te->resno == target_entry->resno)
		{
			target_entry->resname = te->resname;
			return;
		}
	}
}

//------------------------------------------------------------------------------
// If a result plan node is not required on top of a project set node then the
// alias parameter needs to be set for all the project set nodes else not
// required as that information will already be present in the result node
// created
//------------------------------------------------------------------------------
void
SetupAliasParameter(const BOOL will_require_result_node,
					const CDXLNode *project_list_dxlnode,
					Plan *project_set_parent_plan, const Query *query)
{
	if (!will_require_result_node)
	{
		// Setting up the alias value (te->resname)
		ULONG ul = 0;
		ListCell *listcell_project_targetentry;

		ForEach(listcell_project_targetentry,
				project_set_parent_plan->targetlist)
		{
			TargetEntry *te =
				(TargetEntry *) lfirst(listcell_project_targetentry);

			CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];

			GPOS_ASSERT(EdxlopScalarProjectElem ==
						proj_elem_dxlnode->GetOperator()->GetDXLOperator());

			CDXLScalarProjElem *sc_proj_elem_dxlop =
				CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());

			GPOS_ASSERT(1 == proj_elem_dxlnode->Arity());

			te->resname =
				CTranslatorUtils::CreateMultiByteCharStringFromWCString(
					sc_proj_elem_dxlop->GetMdNameAlias()
						->GetMDName()
						->GetBuffer());

			// restore aliases that failed the wide character conversion
			restore_unknown_locale_resname(query, te);
			ul++;
		}
	}
}

//------------------------------------------------------------------------------
// This method is used to convert the FUNCEXPR present in upper level
// Result/ProjectSet nodes targetlist to VAR nodes which reference the FUNCEXPR
// present in the leftree plan targetlist.
//------------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::MutateFuncExprToVarProjectSet(Plan *final_plan)
{
	Plan *it_set_upper_ref = final_plan;
	while (it_set_upper_ref->lefttree != nullptr)
	{
		Plan *subplan = it_set_upper_ref->lefttree;
		List *output_targetlist;
		ListCell *l;
		output_targetlist = NIL;

		foreach (l, it_set_upper_ref->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(l);
			Node *newexpr;

			newexpr = FixUpperExprMutatorProjectSet((Node *) tle->expr,
													subplan->targetlist);
			tle = gpdb::FlatCopyTargetEntry(tle);
			tle->expr = (Expr *) newexpr;
			output_targetlist = lappend(output_targetlist, tle);
		}
		it_set_upper_ref->targetlist = output_targetlist;
		it_set_upper_ref = it_set_upper_ref->lefttree;
	}
}

Var *
SearchTlistForNonVarProjectset(Expr *node, List *itlist, Index newvarno)
{
	TargetEntry *tle;

	if (IsA(node, Const))
	{
		return nullptr;
	}

	tle = gpdb::TlistMember(node, itlist);
	if (nullptr != tle)
	{
		/* Found a matching subplan output expression */
		Var *newvar;

		newvar = gpdb::MakeVarFromTargetEntry(newvarno, tle);
		newvar->varnosyn = 0;
		newvar->varattnosyn = 0;
		return newvar;
	}
	return nullptr; /* no match */
}

Node *
CTranslatorDXLToPlStmt::FixUpperExprMutatorProjectSet(Node *node, void *context)
{
	Var *newvar;

	if (node == nullptr)
	{
		return nullptr;
	}

	newvar = SearchTlistForNonVarProjectset((Expr *) node, (List *) context, OUTER_VAR);
	if (nullptr != newvar)
	{
		return (Node *) newvar;
	}

	return gpdb::Expression_tree_mutator(
		node,
		&CTranslatorDXLToPlStmt::FixUpperExprMutatorProjectSet,
		context);
}

//------------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLResult
//
//	@doc:
//		Translate DXL result node into GPDB result plan node and create Project
//		Set plan node if SRV are present. The current approach is to create a
//		Project Set plan node from a result dxl node as it already contains the
//		info to create a project set node from it. But it's not the best
//		approach. The better approach will be to actually create a new Clogical
//		node to handle the set returning functions and then creating CPhysical,
//		dxl and plan nodes.
//------------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLResult(
	const CDXLNode *result_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// Pointer to the child plan of result node
	Plan *child_plan = nullptr;

	// Pointer to the lowest level ProjectSet node. If multiple ProjectSet nodes
	// are required then the child plan of result dxl node will be attched to
	// its lefttree.
	Plan *project_set_child_plan = nullptr;

	// Do we require a result node to be attached on top of ProjectSet node?
	BOOL will_require_result_node = false;

	// create result plan node
	Result *result = MakeNode(Result);
	Plan *plan = &(result->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(result_dxlnode, plan);

	CDXLNode *child_dxlnode = nullptr;
	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	if (result_dxlnode->Arity() - 1 == EdxlresultIndexChild)
	{
		// translate child plan
		child_dxlnode = (*result_dxlnode)[EdxlresultIndexChild];
		child_plan = TranslateDXLOperatorToPlan(child_dxlnode, &child_context,
												ctxt_translation_prev_siblings);
		GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");
	}

	CDXLNode *project_list_dxlnode = (*result_dxlnode)[EdxlresultIndexProjList];
	CDXLNode *filter_dxlnode = (*result_dxlnode)[EdxlresultIndexFilter];
	CDXLNode *one_time_filter_dxlnode =
		(*result_dxlnode)[EdxlresultIndexOneTimeFilter];
	List *quals_list = nullptr;

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &quals_list,
							   output_context);

	// translate one time filter
	List *one_time_quals_list =
		TranslateDXLFilterToQual(one_time_filter_dxlnode,
								 nullptr,  // base table translation context
								 child_contexts, output_context);

	plan->qual = quals_list;
	result->resconstantqual = (Node *) one_time_quals_list;

	// Not SetParamIds here, as Cloudberry has it: the parameters a node's
	// subtree reads are only all there once its child is attached, below.

	// Creating project set nodes plan tree
	Plan *project_set_parent_plan = CreateProjectSetNodeTree(
		result_dxlnode, plan, child_plan, project_set_child_plan,
		will_require_result_node);

	// If Project Set plan nodes are not required return the result plan node
	// created
	if (nullptr == project_set_parent_plan)
	{
		result->plan.lefttree = child_plan;

		// PostgreSQL 19's Result says what it stands in for, and EXPLAIN
		// reads it.  One over a child is a gating Result -- a projection or
		// a one-time filter -- which is what the enum's zero means.  One with
		// no child is a scan of nothing; with no relids either, EXPLAIN
		// prints no "Replaces" line for it, which is how it shows the
		// planner's own SELECT 1.  Left at zero, EXPLAIN asserts that a
		// gating Result has a child.  Cloudberry's Result has neither field.
		result->result_type =
			(nullptr == child_plan) ? RESULT_TYPE_SCAN : RESULT_TYPE_GATING;

		// The parameters the Result and its child read, with the child
		// there to be read.  Cloudberry took them before attaching it, and
		// so gave a Result only its own: a parameter that changed below it
		// then never reached it (execUtils.c, UpdateChangedParamSet), and a
		// node above that keeps what it computed -- a hash aggregate, a sort,
		// a Material -- kept it.  PostgreSQL's partition_join test found it:
		// under a nested loop, a hash aggregate over a Result over an index
		// scan of the loop's parameter answered every outer row with the
		// first row's hash table.
		SetParamIds(plan);

		child_contexts->Release();
		return PlaceResultFilter(result);
	}

	SetupAliasParameter(will_require_result_node, project_list_dxlnode,
						project_set_parent_plan, output_context->GetQuery());

	Plan *final_plan = nullptr;

	if (will_require_result_node)
	{
		result->plan.lefttree = project_set_parent_plan;
		final_plan = &(result->plan);
	}
	else
	{
		final_plan = project_set_parent_plan;
	}

	MutateFuncExprToVarProjectSet(final_plan);

	// Attaching the child plan
	project_set_child_plan->lefttree = child_plan;

	// And then the parameters of each node made here, for the same reason
	// as above; TranslateDXLProjectSet took a ProjectSet's before it had a
	// target list or a child.
	for (Plan *node = final_plan; node != child_plan; node = node->lefttree)
	{
		SetParamIds(node);
	}

	// cleanup
	child_contexts->Release();
	return final_plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLPartSelector
//
//	@doc:
//		Translate DXL PartitionSelector into a GPDB PartitionSelector node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLPartSelector(
	const CDXLNode *partition_selector_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// Cloudberry's body, for a Partition Selector CustomScan
	// (compat/dynamicscan.c) rather than Cloudberry's node: the pruning steps
	// are the same, built by CPartPruneStepsBuilder from ORCA's filter over
	// the rows the selector passes on, and go in custom_private; their
	// expressions go in custom_exprs too, where the plan's walkers find the
	// parameters they read.
	CDXLPhysicalPartitionSelector *partition_selector_dxlop =
		CDXLPhysicalPartitionSelector::Cast(
			partition_selector_dxlnode->GetOperator());

	CustomScan *selector = MakeNode(CustomScan);
	selector->methods = &gp_orca_partition_selector_methods;
	selector->scan.scanrelid = 0;

	Plan *plan = &(selector->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	TranslatePlanCosts(partition_selector_dxlnode, plan);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	// translate child plan
	CDXLNode *child_dxlnode = (*partition_selector_dxlnode)[2];

	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);
	GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");

	plan->lefttree = child_plan;

	child_contexts->Append(&child_context);

	CDXLNode *project_list_dxlnode = (*partition_selector_dxlnode)[0];
	plan->targetlist = TranslateDXLProjList(project_list_dxlnode,
											nullptr /*base_table_context*/,
											child_contexts, output_context);

	CMDIdGPDB *mdid =
		CMDIdGPDB::CastMdid(partition_selector_dxlop->GetRelMdId());
	gpdb::RelationWrapper relation = gpdb::GetRelation(mdid->Oid());

	CMappingColIdVarPlStmt colid_var_mapping = CMappingColIdVarPlStmt(
		m_mp, nullptr /*base_table_context*/, child_contexts, output_context,
		m_dxl_to_plstmt_context);

	OID oid_type =
		CMDIdGPDB::CastMdid(m_md_accessor->PtMDType<IMDTypeInt4>()->MDId())
			->Oid();
	ULONG paramid = m_dxl_to_plstmt_context->GetParamIdForSelector(
		oid_type, partition_selector_dxlop->SelectorId());

	// The table's entry, which the scan the selector selects for made: the
	// scan is on the outer side of the hash join whose inner side this is,
	// and TranslateDXLHashJoin translates that side first.
	Index rtindex = m_dxl_to_plstmt_context->FindRTE(mdid->Oid());
	if (0 == rtindex || (Index) -1 == rtindex)
	{
		GP_UNPORTED("a Partition Selector before the scan it selects for");
	}

	CDXLNode *filterNode = (*partition_selector_dxlnode)[1];
	List *prune_infos = CPartPruneStepsBuilder::CreatePartPruneInfos(
		filterNode, relation.get(), rtindex,
		partition_selector_dxlop->Partitions(), &colid_var_mapping,
		m_translator_dxl_to_scalar);
	PartitionedRelPruneInfo *pinfo = (PartitionedRelPruneInfo *) gpdb::ListNth(
		(List *) gpdb::ListNth(prune_infos, 0), 0);
	List *steps = pinfo->exec_pruning_steps;

	ListCell *lc = nullptr;
	ForEach(lc, steps)
	{
		PartitionPruneStep *step = (PartitionPruneStep *) lfirst(lc);
		if (IsA(step, PartitionPruneStepOp))
		{
			selector->custom_exprs = gpdb::ListConcat(
				selector->custom_exprs,
				(List *) gpdb::CopyObject(((PartitionPruneStepOp *) step)->exprs));
		}
	}
	selector->custom_private =
		ListMake3(gpdb::MakeIntegerValue(paramid),
				  gpdb::MakeIntegerValue((long) (int) mdid->Oid()), steps);

	SetParamIds(plan);
	// cleanup
	child_contexts->Release();

	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLFilterList
//
//	@doc:
//		Translate DXL filter list into GPDB filter list
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLFilterList(
	const CDXLNode *filter_list_dxlnode,
	const CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *child_contexts,
	CDXLTranslateContext *output_context)
{
	GPOS_ASSERT(EdxlopScalarOpList ==
				filter_list_dxlnode->GetOperator()->GetDXLOperator());

	List *filters_list = NIL;

	CMappingColIdVarPlStmt colid_var_mapping =
		CMappingColIdVarPlStmt(m_mp, base_table_context, child_contexts,
							   output_context, m_dxl_to_plstmt_context);
	const ULONG arity = filter_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *child_filter_dxlnode = (*filter_list_dxlnode)[ul];

		if (gpdxl::CTranslatorDXLToScalar::HasConstTrue(child_filter_dxlnode,
														m_md_accessor))
		{
			filters_list = gpdb::LAppend(filters_list, nullptr /*datum*/);
			continue;
		}

		Expr *filter_expr = m_translator_dxl_to_scalar->TranslateDXLToScalar(
			child_filter_dxlnode, &colid_var_mapping);
		filters_list = gpdb::LAppend(filters_list, filter_expr);
	}

	return filters_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLAppend
//
//	@doc:
//		Translate DXL append node into GPDB Append plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLAppend(
	const CDXLNode *append_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// create append plan node
	Append *append = MakeNode(Append);

	// No run-time pruning: PostgreSQL 18 made part_prune_index an index into
	// PlannedStmt.partPruneInfos, with -1 for none, and makeNode's 0 would
	// name the first entry of a list that is empty.
	append->part_prune_index = -1;

	Plan *plan = &(append->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(append_dxlnode, plan);

	const ULONG arity = append_dxlnode->Arity();
	GPOS_ASSERT(EdxlappendIndexFirstChild < arity);
	append->appendplans = NIL;

	// translate table descriptor into a range table entry
	CDXLPhysicalAppend *phy_append_dxlop =
		CDXLPhysicalAppend::Cast(append_dxlnode->GetOperator());

	// An Append ORCA made of a dynamic table scan, one child per partition,
	// carries the root partitioned table's descriptor, and in Cloudberry the
	// ids of the partition selectors that prune it, through join_prune_paramids,
	// an Append field PostgreSQL 19 does not have; the Append of a UNION ALL
	// carries neither.  ORCA makes one only with a foreign partition or with
	// gp.optimizer_disable_dynamic_table_scan on, and T3's Dynamic Scan is
	// what prunes the dynamic scans, so this stays refused.
	if (phy_append_dxlop->GetScanId() != gpos::ulong_max)
	{
		GP_UNPORTED("an Append over the partitions of a table");
	}

	// translate children
	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());
	for (ULONG ul = EdxlappendIndexFirstChild; ul < arity; ul++)
	{
		CDXLNode *child_dxlnode = (*append_dxlnode)[ul];

		Plan *child_plan = TranslateDXLOperatorToPlan(
			child_dxlnode, &child_context, ctxt_translation_prev_siblings);

		GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");

		append->appendplans = gpdb::LAppend(append->appendplans, child_plan);
	}

	CDXLNode *project_list_dxlnode = (*append_dxlnode)[EdxlappendIndexProjList];
	CDXLNode *filter_dxlnode = (*append_dxlnode)[EdxlappendIndexFilter];

	plan->targetlist = NIL;
	const ULONG length = project_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < length; ++ul)
	{
		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		GPOS_ASSERT(EdxlopScalarProjectElem ==
					proj_elem_dxlnode->GetOperator()->GetDXLOperator());

		CDXLScalarProjElem *sc_proj_elem_dxlop =
			CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());
		GPOS_ASSERT(1 == proj_elem_dxlnode->Arity());

		// translate proj element expression
		CDXLNode *expr_dxlnode = (*proj_elem_dxlnode)[0];
		CDXLScalarIdent *sc_ident_dxlop =
			CDXLScalarIdent::Cast(expr_dxlnode->GetOperator());

		Index idxVarno = OUTER_VAR;
		AttrNumber attno = (AttrNumber)(ul + 1);

		Var *var = gpdb::MakeVar(
			idxVarno, attno,
			CMDIdGPDB::CastMdid(sc_ident_dxlop->MdidType())->Oid(),
			sc_ident_dxlop->TypeModifier(),
			0  // varlevelsup
		);

		TargetEntry *target_entry = MakeNode(TargetEntry);
		target_entry->expr = (Expr *) var;
		target_entry->resname =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				sc_proj_elem_dxlop->GetMdNameAlias()->GetMDName()->GetBuffer());
		target_entry->resno = attno;

		// restore aliases that failed the wide character conversion
		restore_unknown_locale_resname(output_context->GetQuery(),
									   target_entry);

		// add column mapping to output translation context
		output_context->InsertMapping(sc_proj_elem_dxlop->Id(), target_entry);

		plan->targetlist = gpdb::LAppend(plan->targetlist, target_entry);
	}

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(output_context);

	// translate filter
	plan->qual = TranslateDXLFilterToQual(
		filter_dxlnode,
		nullptr,  // translate context for the base table
		child_contexts, output_context);

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	// PostgreSQL 19's Append evaluates no qual, as its Result does not: the
	// planner never gives it one, and ExecInitAppend initialises none.  The
	// filter is translated against the Append's own output -- OUTER_VAR, by
	// position -- which is also what a Result above it reads, so it moves up
	// onto one unchanged, and PlaceResultFilter puts it where PostgreSQL 19
	// evaluates a filter.
	if (NIL != plan->qual)
	{
		Result *result = MakeNode(Result);
		result->result_type = RESULT_TYPE_GATING;
		Plan *result_plan = &(result->plan);
		result_plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
		result_plan->startup_cost = plan->startup_cost;
		result_plan->total_cost = plan->total_cost;
		result_plan->plan_rows = plan->plan_rows;
		result_plan->plan_width = plan->plan_width;
		result_plan->targetlist = CreateDirectCopyTargetList(plan->targetlist);
		result_plan->qual = plan->qual;
		result_plan->lefttree = plan;
		plan->qual = NIL;
		SetParamIds(result_plan);

		return PlaceResultFilter(result);
	}

	return (Plan *) append;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLMaterialize
//
//	@doc:
//		Translate DXL materialize node into GPDB Material plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLMaterialize(
	const CDXLNode *materialize_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// create materialize plan node
	Material *materialize = MakeNode(Material);

	Plan *plan = &(materialize->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// Cloudberry's Material has two fields PostgreSQL 19's does not, and
	// neither is needed where nothing moves between processes.
	//
	// cdb_strict is ORCA's eager spool, which reads all of its input before
	// it returns a row, so that the Motions that send into a slice and out of
	// it cannot wait on each other (Cloudberry's nodeMaterial.c).  ORCA asks
	// for one where a Motion makes that hazard, or everywhere when
	// gp.optimizer_enable_streaming_material is off; read lazily, the input
	// gives the same rows.
	//
	// cdb_shield_child_from_rescans keeps a tuplestore whatever the parent
	// asked for, because a Motion cannot be rescanned.  In PostgreSQL 19 the
	// parent decides, through the executor flags it passes down, and for a
	// subplan the plan does, through rewindPlanIDs -- which
	// GetPlannedStmtFromDXL fills the way the planner fills it, so that a
	// Material at the top of a subplan still keeps its rows.

	// translate operator costs
	TranslatePlanCosts(materialize_dxlnode, plan);

	// translate materialize child
	CDXLNode *child_dxlnode = (*materialize_dxlnode)[EdxlmatIndexChild];

	CDXLNode *project_list_dxlnode =
		(*materialize_dxlnode)[EdxlmatIndexProjList];
	CDXLNode *filter_dxlnode = (*materialize_dxlnode)[EdxlmatIndexFilter];

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list and filter
	TranslateProjListAndFilter(project_list_dxlnode, filter_dxlnode,
							   nullptr,	 // translate context for the base table
							   child_contexts, &plan->targetlist, &plan->qual,
							   output_context);

	plan->lefttree = child_plan;

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return (Plan *) materialize;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCTEProducerToSharedScan
//
//	@doc:
//		Translate DXL CTE Producer node into a subplan and the initplan that
//		runs it
//
//		NOT CLOUDBERRY'S SHAPE.  Cloudberry makes a producer a ShareInputScan
//		that writes its child's rows to a store the consumers' ShareInputScans
//		read, under a Sequence node that runs the producers first.  PostgreSQL
//		19 has neither node, and does the same thing for its own CTEs another
//		way: the CTE is a subplan, run by an initplan SubPlan of CTE_SUBLINK
//		type, and every CteScan of it reads one tuplestore, which the first to
//		start fills from the subplan as the scans ask for rows.  So the
//		producer becomes the subplan and its initplan, the consumers become
//		CteScans, and TranslateDXLSequence attaches the initplans where the
//		Sequence was.  Only TranslateDXLSequence calls this.
//
//		The producer's project list becomes a Result over its child, which
//		post-processing removes when the child can project it itself.
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLCTEProducerToSharedScan(
	const CDXLNode *cte_producer_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalCTEProducer *cte_prod_dxlop =
		CDXLPhysicalCTEProducer::Cast(cte_producer_dxlnode->GetOperator());
	ULONG cte_id = cte_prod_dxlop->Id();

	// the rows of the CTE: the producer's projection over its child
	Result *result = MakeNode(Result);
	result->result_type = RESULT_TYPE_GATING;
	Plan *plan = &(result->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate cost of the producer
	TranslatePlanCosts(cte_producer_dxlnode, plan);

	// translate child plan
	CDXLNode *project_list_dxlnode = (*cte_producer_dxlnode)[0];
	CDXLNode *child_dxlnode = (*cte_producer_dxlnode)[1];

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());
	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);
	GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);
	// translate proj list
	plan->targetlist =
		TranslateDXLProjList(project_list_dxlnode,
							 nullptr,  // base table translation context
							 child_contexts, output_context);

	plan->lefttree = child_plan;
	plan->qual = NIL;
	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	// The subplan, and the initplan that runs it, as SS_process_ctes() makes
	// them: CTE_SUBLINK, no inputs, and one output parameter that carries no
	// value -- the CteScans share their tuplestore through its slot -- so its
	// type is none, as assign_special_exec_param() records it.  isInitPlan
	// stays false, as the planner leaves it for a CTE.
	m_dxl_to_plstmt_context->AddSubplan(plan);

	SubPlan *initplan = MakeNode(SubPlan);
	initplan->subLinkType = CTE_SUBLINK;
	initplan->testexpr = nullptr;
	initplan->paramIds = NIL;
	initplan->plan_id =
		gpdb::ListLength(m_dxl_to_plstmt_context->GetSubplanEntriesList());

	// ORCA's DXL does not carry the query's name for the CTE, and ORCA makes
	// CTEs of its own that have none; EXPLAIN shows "CTE cte<id>".
	CHAR cte_name[NAMEDATALEN];
	snprintf(cte_name, sizeof(cte_name), "cte%u", cte_id);
	initplan->plan_name = PStrDup(cte_name);

	// the first column's type, as get_first_col_type() finds it
	initplan->firstColType = VOIDOID;
	initplan->firstColTypmod = -1;
	initplan->firstColCollation = InvalidOid;
	if (NIL != plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) gpdb::ListNth(plan->targetlist, 0);
		if (!te->resjunk)
		{
			initplan->firstColType = gpdb::ExprType((Node *) te->expr);
			initplan->firstColTypmod = gpdb::ExprTypeMod((Node *) te->expr);
			initplan->firstColCollation =
				gpdb::ExprCollation((Node *) te->expr);
		}
	}
	initplan->isInitPlan = false;
	initplan->useHashTable = false;
	initplan->unknownEqFalse = false;
	initplan->parallel_safe = false;
	initplan->setParam = ListMake1Int(
		(int) m_dxl_to_plstmt_context->GetNextParamId(InvalidOid));
	initplan->parParam = NIL;
	initplan->args = NIL;
	initplan->disabled_nodes = 0;
	initplan->startup_cost = plan->total_cost;
	initplan->per_call_cost = 0;

	m_dxl_to_plstmt_context->RegisterCTEProducerInfo(
		cte_id, cte_prod_dxlop->GetOutputColIdxMap(), plan, initplan);

	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCTEConsumerToSharedScan
//
//	@doc:
//		Translate DXL CTE Consumer node into a CteScan
//
//		A CteScan, not Cloudberry's ShareInputScan; see
//		TranslateDXLCTEProducerToSharedScan.  Which of the producer's columns
//		each output column reads is worked out as Cloudberry works it out; only
//		what reads them changes, from OUTER_VAR of a ShareInputScan's child to
//		a Var of the CTE's range table entry.
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLCTEConsumerToSharedScan(
	const CDXLNode *cte_consumer_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	CDXLPhysicalCTEConsumer *cte_consumer_dxlop =
		CDXLPhysicalCTEConsumer::Cast(cte_consumer_dxlnode->GetOperator());
	ULONG cte_id = cte_consumer_dxlop->Id();
	ULongPtrArray *output_colidx_map = cte_consumer_dxlop->GetOutputColIdxMap();

	// ORCA puts the producers of a Sequence before the plan that reads them,
	// and TranslateDXLSequence translates them in that order.
	const CContextDXLToPlStmt::SCTEEntryInfo *producer_info =
		m_dxl_to_plstmt_context->GetCTEProducerInfo(cte_id);
	if (nullptr == producer_info)
	{
		GP_UNPORTED("a CTE read before the plan that produces it");
	}
	ULongPtrArray *producer_colidx_map = producer_info->m_pidxmap;
	Plan *producer_plan = producer_info->m_cte_producer_plan;
	SubPlan *initplan = producer_info->m_initplan;

	// The range table entry the scan reads, as the parser makes one for a
	// reference to a CTE.  Its columns are the producer's.
	RangeTblEntry *rte = MakeNode(RangeTblEntry);
	rte->rtekind = RTE_CTE;
	rte->ctename = PStrDup(initplan->plan_name);
	rte->ctelevelsup = 0;
	rte->self_reference = false;
	rte->perminfoindex = 0;

	Alias *alias = MakeNode(Alias);
	alias->aliasname = PStrDup(initplan->plan_name);
	alias->colnames = NIL;
	ListCell *lc = nullptr;
	ForEach(lc, producer_plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		alias->colnames = gpdb::LAppend(
			alias->colnames,
			gpdb::MakeStringValue(
				PStrDup(nullptr != te->resname ? te->resname : "?column?")));
		rte->coltypes = gpdb::LAppendOid(rte->coltypes,
										 gpdb::ExprType((Node *) te->expr));
		rte->coltypmods = gpdb::LAppendInt(
			rte->coltypmods, gpdb::ExprTypeMod((Node *) te->expr));
		rte->colcollations = gpdb::LAppendOid(
			rte->colcollations, gpdb::ExprCollation((Node *) te->expr));
	}
	rte->eref = alias;

	m_dxl_to_plstmt_context->AddRTE(rte);
	Index scanrelid =
		gpdb::ListLength(m_dxl_to_plstmt_context->GetRTableEntriesList());

	CteScan *cte_scan = MakeNode(CteScan);
	cte_scan->scan.scanrelid = scanrelid;
	cte_scan->ctePlanId = initplan->plan_id;
	cte_scan->cteParam = linitial_int(initplan->setParam);

	Plan *plan = &(cte_scan->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(cte_consumer_dxlnode, plan);

#ifdef GPOS_DEBUG
	ULongPtrArray *output_colids_array =
		cte_consumer_dxlop->GetOutputColIdsArray();
#endif

	// generate the target list of the CTE Consumer
	plan->targetlist = NIL;
	CDXLNode *project_list_dxlnode = (*cte_consumer_dxlnode)[0];
	const ULONG num_of_proj_list_elem = project_list_dxlnode->Arity();
	GPOS_ASSERT(num_of_proj_list_elem == output_colids_array->Size());
	for (ULONG ul = 0; ul < num_of_proj_list_elem; ul++)
	{
		AttrNumber varattno = (AttrNumber)ul + 1;
		if (output_colidx_map) {
			ULONG remapping_idx;
			remapping_idx = *(*output_colidx_map)[ul];
			if (producer_colidx_map) {
				remapping_idx = *(*producer_colidx_map)[remapping_idx];
			}
			GPOS_ASSERT(remapping_idx != gpos::ulong_max);
			varattno = (AttrNumber)remapping_idx + 1;
		}

		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		CDXLScalarProjElem *sc_proj_elem_dxlop =
			CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());
		ULONG colid = sc_proj_elem_dxlop->Id();
		GPOS_ASSERT(colid == *(*output_colids_array)[ul]);

		CDXLNode *sc_ident_dxlnode = (*proj_elem_dxlnode)[0];
		CDXLScalarIdent *sc_ident_dxlop =
			CDXLScalarIdent::Cast(sc_ident_dxlnode->GetOperator());
		OID oid_type = CMDIdGPDB::CastMdid(sc_ident_dxlop->MdidType())->Oid();

		Var *var =
			gpdb::MakeVar(scanrelid, varattno, oid_type,
						  sc_ident_dxlop->TypeModifier(), 0 /* varlevelsup */);
		// the column's collation is the producer's, which a type's default
		// is not when the CTE's query wrote COLLATE
		var->varcollid = (Oid) gpdb::ListNthOid(rte->colcollations,
											   varattno - 1);

		CHAR *resname = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
			sc_proj_elem_dxlop->GetMdNameAlias()->GetMDName()->GetBuffer());
		TargetEntry *target_entry = gpdb::MakeTargetEntry(
			(Expr *) var, (AttrNumber)(ul + 1), resname, false /* resjunk */);
		plan->targetlist = gpdb::LAppend(plan->targetlist, target_entry);

		output_context->InsertMapping(colid, target_entry);
	}

	plan->qual = NIL;

	SetParamIds(plan);

	// A CteScan depends on what the CTE depends on, as finalize_plan() has
	// it: when a parameter the CTE's rows were computed from changes -- a
	// CTE inside a correlated subquery -- the scan has to start over, and
	// its chgParam is what says so.  Not the shared parameter, which only
	// links the scans (subselect.c, "You might think we should add the
	// node's cteParam to paramids").
	plan->extParam = gpdb::BmsUnion(plan->extParam, producer_plan->extParam);
	plan->allParam = gpdb::BmsUnion(plan->allParam, producer_plan->extParam);

	return (Plan *) cte_scan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLSequence
//
//	@doc:
//		Translate DXL sequence node
//
//		NOT CLOUDBERRY'S SHAPE.  A Sequence runs its children in order and
//		returns the last one's rows, and PostgreSQL 19 has no such node.  ORCA
//		makes one to run CTE producers before the plan that reads them: the
//		producers become subplans whose initplans are attached to the last
//		child's plan -- where the planner attaches a query level's CTEs, above
//		everything that reads them -- and the Sequence's projection becomes a
//		Result over that plan, which post-processing removes when the plan
//		can project it itself.
//
//		Cloudberry's Sequence also ran partition selectors before the dynamic
//		scans they pruned, and the plan counted that among T3's.  Its ORCA no
//		longer makes one: the only logical Sequence is a CTE anchor's
//		(CXformCTEAnchor2Sequence), and a Partition Selector is an enforcer
//		ORCA puts over one side of a join (CPartitionPropagationSpec::
//		AppendEnforcers).  A Sequence with any other child is refused, by the
//		name it had, in case that changes.
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLSequence(
	const CDXLNode *sequence_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	ULONG arity = sequence_dxlnode->Arity();
	GPOS_ASSERT(2 <= arity);

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	// every child but the projection list and the last: the producers
	List *initplans = NIL;
	for (ULONG ul = 1; ul < arity - 1; ul++)
	{
		CDXLNode *child_dxlnode = (*sequence_dxlnode)[ul];
		if (EdxlopPhysicalCTEProducer !=
			child_dxlnode->GetOperator()->GetDXLOperator())
		{
			GP_UNPORTED("a Sequence that selects partitions");
		}

		(void) TranslateDXLCTEProducerToSharedScan(
			child_dxlnode, &child_context, ctxt_translation_prev_siblings);

		ULONG cte_id =
			CDXLPhysicalCTEProducer::Cast(child_dxlnode->GetOperator())->Id();
		initplans = gpdb::LAppend(
			initplans,
			m_dxl_to_plstmt_context->GetCTEProducerInfo(cte_id)->m_initplan);
	}

	// the last child, whose rows the Sequence returns
	Plan *child_plan = TranslateDXLOperatorToPlan(
		(*sequence_dxlnode)[arity - 1], &child_context,
		ctxt_translation_prev_siblings);
	GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");

	child_plan->initPlan = gpdb::ListConcat(child_plan->initPlan, initplans);

	// the Sequence's projection
	Result *result = MakeNode(Result);
	result->result_type = RESULT_TYPE_GATING;
	Plan *plan = &(result->plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(sequence_dxlnode, plan);

	CDXLNode *project_list_dxlnode = (*sequence_dxlnode)[0];

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	// translate proj list
	plan->targetlist =
		TranslateDXLProjList(project_list_dxlnode,
							 nullptr,  // base table translation context
							 child_contexts, output_context);

	plan->lefttree = child_plan;
	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynTblScan
//
//	@doc:
//		Translates a DXL dynamic table scan node into a Dynamic Scan
//
//		NOT CLOUDBERRY'S SHAPE, for all five dynamic scans.  Cloudberry's
//		DynamicSeqScan opens the partitions one after another while the
//		query runs, remapping the scan's columns to each, through changes to
//		PostgreSQL's scan nodes that PostgreSQL 19 does not have.  The port
//		plans the scan ORCA chose for the partitioned table, then makes a
//		scan of each partition ORCA's static pruning left from it -- a copy
//		whose columns and indexes are the partition's
//		(gp_orca_plan_for_partition) -- and puts them under a Dynamic Scan
//		(compat/dynamicscan.c), which runs the ones a Partition Selector
//		chose.  The partitions' scans are ordinary scans, run and explained
//		as the planner's children of an Append are.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynTblScan(
	const CDXLNode *dyn_tbl_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalDynamicTableScan *dyn_tbl_scan_dxlop =
		CDXLPhysicalDynamicTableScan::Cast(dyn_tbl_scan_dxlnode->GetOperator());
	const CDXLTableDescr *dxl_table_descr =
		dyn_tbl_scan_dxlop->GetDXLTableDescr();
	GPOS_ASSERT(dxl_table_descr->LockMode() != -1);

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	Index index = ProcessDXLTblDescr(dxl_table_descr, &base_table_context);

	const IMDRelation *md_rel =
		m_md_accessor->RetrieveRel(dxl_table_descr->MDId());
	OID oidRel = CMDIdGPDB::CastMdid(md_rel->MDId())->Oid();

	// The scan of the partitioned table, which each partition's is made from.
	SeqScan *seq_scan = MakeNode(SeqScan);
	seq_scan->scan.scanrelid = index;
	Plan *plan = &(seq_scan->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	TranslatePlanCosts(dyn_tbl_scan_dxlnode, plan);

	GPOS_ASSERT(2 == dyn_tbl_scan_dxlnode->Arity());

	// translate proj list and filter
	CDXLNode *project_list_dxlnode =
		(*dyn_tbl_scan_dxlnode)[EdxltsIndexProjList];
	CDXLNode *filter_dxlnode = (*dyn_tbl_scan_dxlnode)[EdxltsIndexFilter];

	List *query_quals = NIL;
	TranslateProjListAndFilter(
		project_list_dxlnode, filter_dxlnode,
		&base_table_context,  // translate context for the base table
		nullptr,			  // translate_ctxt_left and pdxltrctxRight,
		&plan->targetlist, &query_quals, output_context);

	// The security quals first, as TranslateDXLTblScan puts them: a row the
	// policy hides is not tested by anything the query says.
	List *security_query_quals = NIL;
	AddSecurityQuals(oidRel, &security_query_quals, &index);
	plan->qual = gpdb::ListConcat(security_query_quals, query_quals);

	return TranslateDynamicScan(
		dyn_tbl_scan_dxlnode, plan, index, dxl_table_descr,
		dyn_tbl_scan_dxlop->GetParts(), dyn_tbl_scan_dxlop->GetSelectorIds(),
		output_context, ctxt_translation_prev_siblings);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDynamicScan
//
//	@doc:
//		The Dynamic Scan over a scan of a partitioned table: a copy of the
//		scan for each partition in parts, each under a range table entry of
//		its own, as the planner's children are, with no permission entry --
//		the table's is what the executor checks -- and a lock taken on it
//		here, as TranslatePartOids takes Cloudberry's.  The partitions'
//		scans are in parts' order, which is the partition descriptor's.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDynamicScan(
	const CDXLNode *dynamic_scan_dxlnode, Plan *scan, Index root_rti,
	const CDXLTableDescr *table_descr, IMdIdArray *parts,
	const ULongPtrArray *selector_ids, CDXLTranslateContext *,
	CDXLTranslationContextArray *)
{
	OID root_oid = CMDIdGPDB::CastMdid(table_descr->MDId())->Oid();
	RangeTblEntry *root_rte = m_dxl_to_plstmt_context->GetRTEByIndex(root_rti);

	List *children = NIL;
	List *part_indexes = NIL;
	for (ULONG ul = 0; ul < parts->Size(); ul++)
	{
		OID part_oid = CMDIdGPDB::CastMdid((*parts)[ul])->Oid();
		gpdb::GPDBLockRelationOid(part_oid, table_descr->LockMode());

		m_dxl_to_plstmt_context->AddRTE(gpdb::PartitionRTE(root_rte, part_oid));
		Index part_rti =
			gpdb::ListLength(m_dxl_to_plstmt_context->GetRTableEntriesList());

		int failure = GP_ORCA_PARTITION_OK;
		Plan *child = gpdb::PlanForPartition(scan, root_rti, part_rti,
											 root_oid, part_oid, &failure);
		if (GP_ORCA_PARTITION_NO_INDEX == failure)
		{
			GP_UNPORTED("a partition without its table's index");
		}
		if (GP_ORCA_PARTITION_INDEX_TOO_NEW == failure)
		{
			GP_UNPORTED("an index newer than this transaction's snapshots");
		}

		SetPlanNodeIds(child);
		children = gpdb::LAppend(children, child);
		part_indexes = gpdb::LAppendInt(
			part_indexes, gpdb::TopPartitionIndex(root_oid, part_oid));
	}

	CustomScan *dynamic_scan = MakeNode(CustomScan);
	dynamic_scan->methods = &gp_orca_dynamic_scan_methods;
	dynamic_scan->scan.scanrelid = 0;
	dynamic_scan->custom_plans = children;

	// What the partitions' scans return, in the table's terms, which the
	// node's own target list reads by position, and EXPLAIN by name.
	dynamic_scan->custom_scan_tlist = gpdb::DynamicScanTlist(scan);

	// The table the node reads.  EXPLAIN names it, and counts it as used, as
	// it counts an Append's table, so that a condition over the table's
	// columns names the table and the partitions' scans take the names after
	// it: t, then t_1, t_2 and so on, as under the planner's Append.
	dynamic_scan->custom_relids = gpdb::BmsAddMember(nullptr, root_rti);

	OID oid_type =
		CMDIdGPDB::CastMdid(m_md_accessor->PtMDType<IMDTypeInt4>()->MDId())
			->Oid();

	// For EXPLAIN, which calls the node what Cloudberry calls its dynamic
	// scan of the same kind, over the same index (cb_dynamicscan.h).
	Oid index_oid = InvalidOid;
	if (IsA(scan, IndexScan))
	{
		index_oid = ((IndexScan *) scan)->indexid;
	}
	else if (IsA(scan, IndexOnlyScan))
	{
		index_oid = ((IndexOnlyScan *) scan)->indexid;
	}

	dynamic_scan->custom_private = ListMake2(
		part_indexes, TranslateJoinPruneParamids(selector_ids, oid_type,
												 m_dxl_to_plstmt_context));
	dynamic_scan->custom_private =
		gpdb::LAppend(dynamic_scan->custom_private,
					  gpdb::MakeIntegerValue((long) nodeTag(scan)));
	dynamic_scan->custom_private =
		gpdb::LAppend(dynamic_scan->custom_private,
					  gpdb::MakeIntegerValue((long) index_oid));

	Plan *plan = &(dynamic_scan->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	TranslatePlanCosts(dynamic_scan_dxlnode, plan);

	ListCell *lc = nullptr;
	ForEach(lc, dynamic_scan->custom_scan_tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Var *var = gpdb::MakeVar(INDEX_VAR, te->resno,
								 gpdb::ExprType((Node *) te->expr),
								 gpdb::ExprTypeMod((Node *) te->expr),
								 0 /* varlevelsup */);
		var->varcollid = gpdb::ExprCollation((Node *) te->expr);
		plan->targetlist = gpdb::LAppend(
			plan->targetlist,
			gpdb::MakeTargetEntry((Expr *) var, te->resno,
								  te->resname ? PStrDup(te->resname) : nullptr,
								  te->resjunk));
	}

	SetParamIds(plan);

	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetPlanNodeIds
//
//	@doc:
//		Number a plan made by copying another, and its children, afresh:
//		EXPLAIN ANALYZE and the executor's instrumentation tell nodes apart
//		by plan_node_id.
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetPlanNodeIds(Plan *plan)
{
	if (nullptr == plan)
	{
		return;
	}

	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	SetPlanNodeIds(plan->lefttree);
	SetPlanNodeIds(plan->righttree);

	List *children = NIL;
	if (IsA(plan, BitmapAnd))
	{
		children = ((BitmapAnd *) plan)->bitmapplans;
	}
	else if (IsA(plan, BitmapOr))
	{
		children = ((BitmapOr *) plan)->bitmapplans;
	}

	ListCell *lc = nullptr;
	ForEach(lc, children)
	{
		SetPlanNodeIds((Plan *) lfirst(lc));
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynIdxOnlyScan
//
//	@doc:
//		Translates a DXL dynamic index-only scan node into a Dynamic Scan of
//		index-only scans; see TranslateDXLDynTblScan
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynIdxOnlyScan(
	const CDXLNode *dyn_idx_only_scan_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalDynamicIndexOnlyScan *dyn_index_only_scan_dxlop =
		CDXLPhysicalDynamicIndexOnlyScan::Cast(
			dyn_idx_only_scan_dxlnode->GetOperator());
	const CDXLTableDescr *table_desc =
		dyn_index_only_scan_dxlop->GetDXLTableDescr();

	// The index-only scan of the partitioned table, as TranslateDXLIndexOnlyScan
	// makes one, which each partition's is made from.
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(table_desc->MDId());

	Index index = ProcessDXLTblDescr(table_desc, &base_table_context);

	IndexOnlyScan *index_scan = MakeNode(IndexOnlyScan);
	index_scan->scan.scanrelid = index;

	CMDIdGPDB *mdid_index = CMDIdGPDB::CastMdid(
		dyn_index_only_scan_dxlop->GetDXLIndexDescr()->MDId());
	const IMDIndex *md_index = m_md_accessor->RetrieveIndex(mdid_index);
	Oid index_oid = mdid_index->Oid();

	GPOS_ASSERT(InvalidOid != index_oid);
	index_scan->indexid = index_oid;

	CDXLTranslateContextBaseTable index_context(m_mp);

	// translate index targetlist
	index_scan->indextlist = TranslateDXLIndexTList(md_rel, md_index, index,
													table_desc, &index_context);

	Plan *plan = &(index_scan->scan.plan);
	TranslatePlan(plan, dyn_idx_only_scan_dxlnode, output_context,
				  m_dxl_to_plstmt_context, &index_context,
				  ctxt_translation_prev_siblings);

	index_scan->indexorderdir = CTranslatorUtils::GetScanDirection(
		dyn_index_only_scan_dxlop->GetIndexScanDir());

	// translate index condition list
	List *index_cond = NIL;
	List *index_orig_cond = NIL;

	if (!IsIndexForOrderBy(
			&base_table_context, ctxt_translation_prev_siblings, output_context,
			(*dyn_idx_only_scan_dxlnode)
				[CDXLPhysicalDynamicIndexScan::EdxldisIndexCondition]))
	{
		TranslateIndexConditions(
			(*dyn_idx_only_scan_dxlnode)
				[CDXLPhysicalDynamicIndexScan::EdxldisIndexCondition],
			table_desc,
			false,	// is_bitmap_index_probe
			md_index, md_rel, output_context, &base_table_context,
			ctxt_translation_prev_siblings, &index_cond, &index_orig_cond);
	}

	index_scan->indexqual = index_cond;
	// see TranslateDXLIndexOnlyScan
	index_scan->recheckqual = (List *) gpdb::CopyObject(index_cond);

	return TranslateDynamicScan(
		dyn_idx_only_scan_dxlnode, plan, index, table_desc,
		dyn_index_only_scan_dxlop->GetParts(),
		dyn_index_only_scan_dxlop->GetSelectorIds(), output_context,
		ctxt_translation_prev_siblings);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynIdxScan
//
//	@doc:
//		Translates a DXL dynamic index scan node into a Dynamic Scan of index
//		scans; see TranslateDXLDynTblScan
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynIdxScan(
	const CDXLNode *dyn_idx_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalDynamicIndexScan *dyn_index_scan_dxlop =
		CDXLPhysicalDynamicIndexScan::Cast(dyn_idx_scan_dxlnode->GetOperator());
	const CDXLTableDescr *table_desc = dyn_index_scan_dxlop->GetDXLTableDescr();

	// The index scan of the partitioned table, as TranslateDXLIndexScan makes
	// one, which each partition's is made from.
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(table_desc->MDId());

	Index index = ProcessDXLTblDescr(table_desc, &base_table_context);

	IndexScan *index_scan = MakeNode(IndexScan);
	index_scan->scan.scanrelid = index;

	CMDIdGPDB *mdid_index = CMDIdGPDB::CastMdid(
		dyn_index_scan_dxlop->GetDXLIndexDescr()->MDId());
	const IMDIndex *md_index = m_md_accessor->RetrieveIndex(mdid_index);
	Oid index_oid = mdid_index->Oid();

	GPOS_ASSERT(InvalidOid != index_oid);
	index_scan->indexid = index_oid;

	Plan *plan = &(index_scan->scan.plan);

	TranslatePlan(plan, dyn_idx_scan_dxlnode, output_context,
				  m_dxl_to_plstmt_context, &base_table_context,
				  ctxt_translation_prev_siblings);

	index_scan->indexorderdir = CTranslatorUtils::GetScanDirection(
		dyn_index_scan_dxlop->GetIndexScanDir());

	// translate index condition list
	List *index_cond = NIL;
	List *index_orig_cond = NIL;

	if (!IsIndexForOrderBy(
			&base_table_context, ctxt_translation_prev_siblings, output_context,
			(*dyn_idx_scan_dxlnode)
				[CDXLPhysicalDynamicIndexScan::EdxldisIndexCondition]))
	{
		TranslateIndexConditions(
			(*dyn_idx_scan_dxlnode)
				[CDXLPhysicalDynamicIndexScan::EdxldisIndexCondition],
			table_desc,
			false,	// is_bitmap_index_probe
			md_index, md_rel, output_context, &base_table_context,
			ctxt_translation_prev_siblings, &index_cond, &index_orig_cond);
	}

	index_scan->indexqual = index_cond;
	index_scan->indexqualorig = index_orig_cond;

	return TranslateDynamicScan(
		dyn_idx_scan_dxlnode, plan, index, table_desc,
		dyn_index_scan_dxlop->GetParts(), dyn_index_scan_dxlop->GetSelectorIds(),
		output_context, ctxt_translation_prev_siblings);
}

// Not RemapAttrsFromTupDesc, the helper that renumbers a qual's columns from
// the root partition to a leaf, through Cloudberry's
// change_varattnos_of_a_varno: TranslateDXLDynForeignScan was its only
// caller, and the partitions' scans are renumbered by
// gp_orca_plan_for_partition, with PostgreSQL's map_partition_varattnos().

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynForeignScan
//
//	@doc:
//		Translates a DXL dynamic foreign scan node into a DynamicForeignScan node
//		This is similar to TranslateDXLDynTblScan, but has additional logic to
//		populate the fdw_private_array. Note that because we need to call
//		CreateForeignScan to populate this array, we need to map the qual
//		and targetlist from the child partitions from the root partition
//		While we do some of this in the executor, since we populate the
//		fdw_private for each child here, we also need mapping logic here
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynForeignScan(
	const CDXLNode *dyn_foreign_scan_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// Refused, and not admitted by TranslateDXLOperatorToPlan.  A foreign
	// partition's scan is planned by its wrapper (BuildForeignScan), and
	// build_simple_rel() takes the user to plan as from the partition's own
	// permission entry; a partition read through its table has none, as the
	// planner's children have none, and giving it one would have the
	// executor check a privilege on the partition that PostgreSQL does not.
	// Cloudberry's body is in
	// github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp.
	GP_UNPORTED("dynamic foreign scans");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDml
//
//	@doc:
//		Translates a DXL DML node
//
//		Cloudberry's body, for PostgreSQL 19's ModifyTable.  What changed:
//
//		- No split update.  Cloudberry's ModifyTable can run an UPDATE as a
//		  DELETE and an INSERT, told apart by a "DMLAction" column, which is
//		  how a row moves to another segment; PostgreSQL 19's cannot.  ORCA
//		  plans one when a distribution or partition key changes, which on
//		  one node is never a heap table's -- UPDATE of a partitioned table
//		  is refused before ORCA sees it -- so it is refused here.
//		- Cloudberry's refusal of an UPDATE of a table with UPDATE triggers
//		  is dropped.  With no split, ModifyTable fires them as it does
//		  under the planner's plan, a trigger declared UPDATE OF a column
//		  included, which the ORCA suite checks.
//		- The row is found by "ctid" alone.  Cloudberry adds "gp_segment_id"
//		  beside it, a system column PostgreSQL 19 does not have; the Query
//		  translator gives ORCA tableoid in its place, and it stops here.
//		- An UPDATE names in updateColnos the columns it sets, as the
//		  planner's does, not every column; see CreateUpdateTargetList.
//		- ORCA's permission entry for the table is completed from the
//		  Query's.  PostgreSQL 19 reads the updated columns from it -- to
//		  recompute the generated columns that depend on them, and to fire a
//		  trigger declared UPDATE OF a column -- and ORCA's has none.
//		- An EvalPlanQual parameter.  Under READ COMMITTED a row another
//		  transaction has just updated is re-read and the plan re-run over
//		  it; every node below has to see the parameter change to be
//		  re-run.  Cloudberry's plan has none, and is re-run only where
//		  Cloudberry does not lock the table for the statement (see
//		  CTranslatorQueryToDXL::CheckDMLReadsOnlyTarget).
//		- rootRelation is zero and forceTupleRouting gone: PostgreSQL 19 sets
//		  up tuple routing for an INSERT into a partitioned table by itself,
//		  and names a root relation only when there are several result
//		  relations, which there never are here.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDml(
	const CDXLNode *dml_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalDML *phy_dml_dxlop =
		CDXLPhysicalDML::Cast(dml_dxlnode->GetOperator());

	// create ModifyTable node
	ModifyTable *dml = MakeNode(ModifyTable);
	Plan *plan = &(dml->plan);

	switch (phy_dml_dxlop->GetDmlOpType())
	{
		case gpdxl::Edxldmldelete:
		{
			m_cmd_type = CMD_DELETE;
			break;
		}
		case gpdxl::Edxldmlupdate:
		{
			m_cmd_type = CMD_UPDATE;
			break;
		}
		case gpdxl::Edxldmlinsert:
		{
			m_cmd_type = CMD_INSERT;
			break;
		}
		case gpdxl::EdxldmlSentinel:
		default:
		{
			GPOS_RAISE(
				gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion,
				GPOS_WSZ_LIT("Unexpected error during plan generation."));
			break;
		}
	}

	IMDId *mdid_target_table = phy_dml_dxlop->GetDXLTableDescr()->MDId();
	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(mdid_target_table);

	// ORCA marks every INSERT and DELETE split too (CXformUtils,
	// PexprLogicalDMLOverProject), where it means nothing; for an UPDATE it
	// means the DMLAction column.  Cloudberry also splits every update of an
	// append-only table, which is M5's, and which the relcache translator
	// does not report on this node.
	if (CMD_UPDATE == m_cmd_type &&
		(phy_dml_dxlop->FSplit() || md_rel->IsNonBlockTable()))
	{
		GP_UNPORTED("an UPDATE run as a DELETE and an INSERT");
	}

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	CDXLTableDescr *table_descr = phy_dml_dxlop->GetDXLTableDescr();

	Index index = ProcessDXLTblDescr(table_descr, &base_table_context);

	m_result_rel_list = gpdb::LAppendInt(m_result_rel_list, index);

	CompleteResultRelationPermissions(index);

	CDXLNode *project_list_dxlnode = (*dml_dxlnode)[0];
	CDXLNode *child_dxlnode = (*dml_dxlnode)[1];

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	List *dml_target_list =
		TranslateDXLProjList(project_list_dxlnode,
							 nullptr,  // translate context for the base table
							 child_contexts, output_context);

	// The new row: for an INSERT, one entry per attribute, a NULL for a
	// dropped one (and for a stored generated one), as the executor checks
	// it; for an UPDATE, the new values of the columns the statement sets,
	// named in updateColnos.  A DELETE needs none: on one node ORCA's
	// DELETE carries no column but the row's identity.
	List *update_colnos = NIL;
	if (CMD_INSERT == m_cmd_type)
	{
		dml_target_list = CreateTargetListWithNullsForDroppedCols(
			dml_target_list, md_rel, true /* keepDropedAsNull */);
	}
	else if (CMD_UPDATE == m_cmd_type)
	{
		dml_target_list =
			CreateUpdateTargetList(dml_target_list, md_rel, &update_colnos);
	}

	// The row to change, by the junk column the executor finds by name.
	if (CMD_UPDATE == m_cmd_type || CMD_DELETE == m_cmd_type)
	{
		AddJunkTargetEntryForColId(&dml_target_list, &child_context,
								   phy_dml_dxlop->GetCtIdColId(), "ctid");
	}

	// Add a Result node on top of the child plan, to coerce the target
	// list to match the exact physical layout of the target table,
	// including dropped columns.  Often, the Result node isn't really
	// needed, as the child node could do the projection, but we don't have
	// the information to determine that here. There's a step in the
	// backend optimize_query() function to eliminate unnecessary Results
	// through the plan, hopefully this Result gets eliminated there.
	Result *result = MakeNode(Result);
	Plan *result_plan = &(result->plan);

	result_plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	result_plan->lefttree = child_plan;

	result_plan->targetlist = dml_target_list;
	SetParamIds(result_plan);

	dml->operation = m_cmd_type;
	dml->canSetTag = m_dxl_to_plstmt_context->m_orig_query->canSetTag;
	dml->nominalRelation = index;
	dml->rootRelation = 0;
	dml->resultRelations = ListMake1Int(index);
	if (CMD_UPDATE == m_cmd_type)
	{
		dml->updateColnosLists = ListMake1(update_colnos);
	}
	// one entry per result relation, whether or not it is a foreign table
	dml->fdwPrivLists = ListMake1(NIL);
	dml->onConflictAction = ONCONFLICT_NONE;

	// The planner gives every ModifyTable one (planner.c,
	// assign_special_exec_param), and makes every node below depend on it
	// (subselect.c, finalize_plan).  No value passes through it; a change
	// to it is how EvalPlanQual tells the plan under ModifyTable to start
	// again.
	dml->epqParam = (int) m_dxl_to_plstmt_context->GetNextParamId(InvalidOid);
	AddParamToPlanTree(result_plan, dml->epqParam);

	plan->lefttree = result_plan;
	plan->righttree = nullptr;
	plan->targetlist = NIL;
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	// translate operator costs
	TranslatePlanCosts(dml_dxlnode, plan);

	return (Plan *) dml;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CreateUpdateTargetList
//
//	@doc:
//		The new values of the columns an UPDATE sets, in the order the
//		Query sets them, with their attribute numbers in *update_colnos.
//
//		PostgreSQL's convention (preptlist.c,
//		extract_update_targetlist_colnos): ExecBuildUpdateProjection()
//		takes every other column from the old row, and whatever reads the
//		plan takes updateColnos as the columns the statement changes.
//		Cloudberry named every column that is not dropped and passed each
//		one's value, which builds the same row and tells a reader of the
//		plan that every column changed -- gp_sql's guard, which lets an
//		UPDATE of a directory table set its tag and nothing else, refused
//		an UPDATE of the tag.  ORCA's DML operator passes every column,
//		one per column that is not dropped, and this takes the ones the
//		statement sets.
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::CreateUpdateTargetList(List *target_list,
											   const IMDRelation *md_rel,
											   List **update_colnos)
{
	Query *query = m_dxl_to_plstmt_context->m_orig_query;
	List *result = NIL;

	ListCell *lc = nullptr;
	ForEach(lc, query->targetList)
	{
		TargetEntry *query_te = (TargetEntry *) lfirst(lc);
		if (query_te->resjunk)
		{
			continue;
		}

		// the column's place among the ones that are not dropped, which is
		// its place in what ORCA's DML operator passes
		const IMDColumn *md_col = nullptr;
		ULONG pos = 0;
		for (ULONG ul = 0; ul < md_rel->ColumnCount(); ul++)
		{
			const IMDColumn *col = md_rel->GetMdCol(ul);
			if (col->IsSystemColumn() || col->IsDropped())
			{
				continue;
			}
			if (col->AttrNum() == query_te->resno)
			{
				md_col = col;
				break;
			}
			pos++;
		}
		GPOS_ASSERT(nullptr != md_col);
		if (nullptr == md_col)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
					   query_te->resno);
		}

		TargetEntry *target_entry =
			(TargetEntry *) gpdb::ListNth(target_list, pos);
		CHAR *name_str =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				md_col->Mdname().GetMDName()->GetBuffer());
		result = gpdb::LAppend(
			result, gpdb::MakeTargetEntry(
						(Expr *) gpdb::CopyObject(target_entry->expr),
						gpdb::ListLength(result) + 1, name_str,
						false /*resjunk*/));
		*update_colnos = gpdb::LAppendInt(*update_colnos, query_te->resno);
	}

	return result;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CompleteResultRelationPermissions
//
//	@doc:
//		Complete the permission entry of the result relation from the
//		Query's.  ProcessDXLTblDescr gives it the privileges the table
//		descriptor asks for and no columns; PostgreSQL 19's executor reads
//		the columns an UPDATE sets (ExecGetUpdatedCols) to know which
//		generated columns to recompute and which UPDATE OF triggers fire,
//		and the inserted and updated ones to decide what an error may show.
//		Not in Cloudberry's translator, though its executor reads the entry
//		the same way (execUtils.c, ExecGetUpdatedCols).
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::CompleteResultRelationPermissions(Index index)
{
	Query *query = m_dxl_to_plstmt_context->m_orig_query;
	GPOS_ASSERT(0 < query->resultRelation);

	RangeTblEntry *query_rte = (RangeTblEntry *) gpdb::ListNth(
		query->rtable, query->resultRelation - 1);
	RTEPermissionInfo *query_perminfo =
		gpdb::GetRTEPermissionInfo(query->rteperminfos, query_rte);

	RangeTblEntry *rte = m_dxl_to_plstmt_context->GetRTEByIndex(index);
	GPOS_ASSERT(nullptr != rte && 0 != rte->perminfoindex);
	GPOS_ASSERT(rte->relid == query_rte->relid);

	RTEPermissionInfo *perminfo =
		m_dxl_to_plstmt_context->GetPermInfoByIndex(rte->perminfoindex);

	perminfo->requiredPerms |= query_perminfo->requiredPerms;
	perminfo->checkAsUser = query_perminfo->checkAsUser;
	perminfo->selectedCols = gpdb::BmsUnion(perminfo->selectedCols,
											query_perminfo->selectedCols);
	perminfo->insertedCols = gpdb::BmsUnion(perminfo->insertedCols,
											query_perminfo->insertedCols);
	perminfo->updatedCols = gpdb::BmsUnion(perminfo->updatedCols,
										   query_perminfo->updatedCols);

	rte->rellockmode = query_rte->rellockmode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::AddParamToPlanTree
//
//	@doc:
//		Make every node of a plan tree depend on a parameter -- the node and
//		its children, not the subplans its expressions call, which the
//		planner's finalize_plan() does not reach either.
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::AddParamToPlanTree(Plan *plan, int paramid)
{
	if (nullptr == plan)
	{
		return;
	}

	plan->extParam = gpdb::BmsAddMember(plan->extParam, paramid);
	plan->allParam = gpdb::BmsAddMember(plan->allParam, paramid);

	AddParamToPlanTree(plan->lefttree, paramid);
	AddParamToPlanTree(plan->righttree, paramid);

	List *children = NIL;
	switch (nodeTag(plan))
	{
		case T_Append:
			children = ((Append *) plan)->appendplans;
			break;
		case T_MergeAppend:
			children = ((MergeAppend *) plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = ((BitmapAnd *) plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = ((BitmapOr *) plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = ((CustomScan *) plan)->custom_plans;
			break;
		case T_SubqueryScan:
			AddParamToPlanTree(((SubqueryScan *) plan)->subplan, paramid);
			break;
		default:
			break;
	}

	ListCell *lc = nullptr;
	ForEach(lc, children)
	{
		AddParamToPlanTree((Plan *) lfirst(lc), paramid);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDirectDispatchInfo
//
//	@doc:
//		Translate the direct dispatch info
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLDirectDispatchInfo(
	CDXLDirectDispatchInfo *dxl_direct_dispatch_info,
	RangeTblEntry *pRTEHashFuncCal)
{
	if (!optimizer_enable_direct_dispatch ||
		nullptr == dxl_direct_dispatch_info)
	{
		return NIL;
	}

	CDXLDatum2dArray *dispatch_identifier_datum_arrays =
		dxl_direct_dispatch_info->GetDispatchIdentifierDatumArray();

	if (dispatch_identifier_datum_arrays == nullptr ||
		0 == dispatch_identifier_datum_arrays->Size())
	{
		return NIL;
	}

	CDXLDatumArray *dxl_datum_array = (*dispatch_identifier_datum_arrays)[0];
	GPOS_ASSERT(0 < dxl_datum_array->Size());

	const ULONG length = dispatch_identifier_datum_arrays->Size();

	if (dxl_direct_dispatch_info->FContainsRawValues())
	{
		List *segids_list = NIL;
		INT segid;
		Const *const_expr = nullptr;

		for (ULONG ul = 0; ul < length; ul++)
		{
			CDXLDatumArray *dispatch_identifier_datum_array =
				(*dispatch_identifier_datum_arrays)[ul];
			GPOS_ASSERT(1 == dispatch_identifier_datum_array->Size());
			const_expr =
				(Const *) m_translator_dxl_to_scalar->TranslateDXLDatumToScalar(
					(*dispatch_identifier_datum_array)[0]);

			segid = DatumGetInt32(const_expr->constvalue);
			if (segid >= -1 && segid < (INT) m_num_of_segments)
			{
				segids_list = gpdb::LAppendInt(segids_list, segid);
			}
		}

		if (segids_list == NIL && const_expr)
		{
			// If no valid segids were found, and there were items in the
			// dispatch identifier array, then append the last item to behave
			// in same manner as Planner for consistency. Currently this will
			// lead to a FATAL in the backend when we dispatch.
			segids_list = gpdb::LAppendInt(segids_list, segid);
		}
		return segids_list;
	}

	ULONG hash_code = GetDXLDatumGPDBHash(dxl_datum_array, pRTEHashFuncCal);
	for (ULONG ul = 0; ul < length; ul++)
	{
		CDXLDatumArray *dispatch_identifier_datum_array =
			(*dispatch_identifier_datum_arrays)[ul];
		GPOS_ASSERT(0 < dispatch_identifier_datum_array->Size());
		ULONG hash_code_new = GetDXLDatumGPDBHash(
			dispatch_identifier_datum_array, pRTEHashFuncCal);

		if (hash_code != hash_code_new)
		{
			// values don't hash to the same segment
			return NIL;
		}
	}

	List *segids_list = gpdb::LAppendInt(NIL, hash_code);
	return segids_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::GetDXLDatumGPDBHash
//
//	@doc:
//		Hash a DXL datum
//
//---------------------------------------------------------------------------
ULONG
CTranslatorDXLToPlStmt::GetDXLDatumGPDBHash(CDXLDatumArray *dxl_datum_array,
											RangeTblEntry *pRTEHashFuncCal)
{
	// M2: direct dispatch.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("direct dispatch");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLSplit
//
//	@doc:
//		Translates a DXL Split node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLSplit(
	const CDXLNode *split_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// M2: updates that move a row between segments.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("updates that move a row between segments");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLAssert
//
//	@doc:
//		Translate DXL assert node into GPDB assert plan node
//
//---------------------------------------------------------------------------
//
//		ORCA asserts three things, each with its own SQLSTATE:
//
//		- 23502 and 23514, that a row an INSERT or UPDATE writes meets the
//		  table's NOT NULL and CHECK constraints.  PostgreSQL 19's
//		  ModifyTable checks both itself (ExecConstraints), after the BEFORE
//		  ROW triggers that may change the row and with PostgreSQL's
//		  messages, so the Assert is left out and a Result projects what it
//		  would have.  Kept, it would refuse a row a trigger was about to
//		  fix, in words PostgreSQL does not use.
//		- P0003, that a scalar subquery ORCA turned into a join gave at most
//		  one row.  Nothing else checks that, so it is an Assert node --
//		  Cloudberry's AssertOp, which PostgreSQL 19 does not have, as a
//		  CustomScan (compat/assertop.c) -- and it raises the planner's error
//		  for the same case, "more than one row returned by a subquery used
//		  as an expression" (21000), not ORCA's.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLAssert(
	const CDXLNode *assert_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalAssert *assert_dxlop =
		CDXLPhysicalAssert::Cast(assert_dxlnode->GetOperator());

	const CHAR *error_code = assert_dxlop->GetSQLState();
	GPOS_ASSERT(GPOS_SQLSTATE_LENGTH == clib::Strlen(error_code));

	BOOL is_constraint = (0 == clib::Strcmp(error_code, "23502") ||
						  0 == clib::Strcmp(error_code, "23514"));
	BOOL is_max_one_row = (0 == clib::Strcmp(error_code, "P0003"));

	if (!is_constraint && !is_max_one_row)
	{
		GP_UNPORTED("an assertion of neither a constraint nor one row");
	}

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	// translate child plan
	CDXLNode *child_dxlnode =
		(*assert_dxlnode)[CDXLPhysicalAssert::EdxlassertIndexChild];
	Plan *child_plan = TranslateDXLOperatorToPlan(
		child_dxlnode, &child_context, ctxt_translation_prev_siblings);

	GPOS_ASSERT(nullptr != child_plan && "child plan cannot be NULL");

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	CDXLNode *project_list_dxlnode =
		(*assert_dxlnode)[CDXLPhysicalAssert::EdxlassertIndexProjList];

	// translate proj list
	List *target_list =
		TranslateDXLProjList(project_list_dxlnode,
							 nullptr,  // translate context for the base table
							 child_contexts, output_context);

	Plan *plan = nullptr;
	if (is_constraint)
	{
		Result *result = MakeNode(Result);
		result->result_type = RESULT_TYPE_GATING;
		plan = &(result->plan);
	}
	else
	{
		CDXLNode *filter_dxlnode =
			(*assert_dxlnode)[CDXLPhysicalAssert::EdxlassertIndexFilter];

		CustomScan *assert_scan = MakeNode(CustomScan);
		assert_scan->methods = &gp_orca_assert_methods;
		assert_scan->scan.scanrelid = 0;
		assert_scan->custom_exprs = TranslateDXLAssertConstraints(
			filter_dxlnode, output_context, child_contexts);
		assert_scan->custom_private = ListMake2(
			gpdb::MakeStringValue(PStrDup("21000")),
			gpdb::MakeStringValue(PStrDup(
				"more than one row returned by a subquery used as an "
				"expression")));
		plan = &(assert_scan->scan.plan);
	}

	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	plan->lefttree = child_plan;
	plan->targetlist = target_list;

	// translate operator costs
	TranslatePlanCosts(assert_dxlnode, plan);

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::ProcessDXLTblDescr
//
//	@doc:
//		Translates a DXL table descriptor into a range table entry and stores
//		it in m_dxl_to_plstmt_context if it's needed (in case of DML operations
//		there is more than one table descriptors which point to the result
//		relation, so if rte was alredy translated, this rte will be updated and
//		index of this rte at m_dxl_to_plstmt_context->m_rtable_entries_list
//		(shortened as "rte_list"), will be returned, if the rte wasn't
//		translated, the newly created rte will be appended to rte_list and it's
//		index returned). Also this function fills base_table_context for the
//		mapping from colids to index attnos instead of table attnos.
//		Returns index of translated range table entry at the rte_list.
//
//---------------------------------------------------------------------------
Index
CTranslatorDXLToPlStmt::ProcessDXLTblDescr(
	const CDXLTableDescr *table_descr,
	CDXLTranslateContextBaseTable *base_table_context)
{
	GPOS_ASSERT(nullptr != table_descr);

	BOOL rte_was_translated = false;

	ULONG assigned_query_id = table_descr->GetAssignedQueryIdForTargetRel();
	Index index = m_dxl_to_plstmt_context->GetRTEIndexByAssignedQueryId(
		assigned_query_id, &rte_was_translated);

	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(table_descr->MDId());
	const ULONG num_of_non_sys_cols =
		CTranslatorUtils::GetNumNonSystemColumns(md_rel);

	// get oid for table
	Oid oid = CMDIdGPDB::CastMdid(table_descr->MDId())->Oid();
	GPOS_ASSERT(InvalidOid != oid);

	// save oid and range index in translation context
	base_table_context->SetOID(oid);
	base_table_context->SetRelIndex(index);

	// save mapping col id -> index in translate context
	const ULONG arity = table_descr->Arity();
	for (ULONG ul = 0; ul < arity; ++ul)
	{
		const CDXLColDescr *dxl_col_descr = table_descr->GetColumnDescrAt(ul);
		GPOS_ASSERT(nullptr != dxl_col_descr);

		INT attno = dxl_col_descr->AttrNum();
		GPOS_ASSERT(0 != attno);

		(void) base_table_context->InsertMapping(dxl_col_descr->Id(), attno);
	}

	ULONG acl_mode = table_descr->GetAclMode();
	GPOS_ASSERT(acl_mode <= std::numeric_limits<AclMode>::max());
	AclMode required_perms = static_cast<AclMode>(acl_mode);

	// descriptor was already processed, and translated RTE is stored at
	// context rtable list (only update required perms of this rte is needed)
	if (rte_was_translated)
	{
		RangeTblEntry *rte = m_dxl_to_plstmt_context->GetRTEByIndex(index);
		GPOS_ASSERT(nullptr != rte);

		if (rte->perminfoindex != 0)
		{
			RTEPermissionInfo *pi = m_dxl_to_plstmt_context->GetPermInfoByIndex(rte->perminfoindex);
			pi->requiredPerms |= required_perms;
		}

		return index;
	}

	// create a new RTE (and it's alias) and store it at context rtable list
	RangeTblEntry *rte = MakeNode(RangeTblEntry);
	// A perm info entry corresponding this rte.
	RTEPermissionInfo *pi = MakeNode(RTEPermissionInfo);
	rte->rtekind = RTE_RELATION;
	rte->relid = oid;
	pi->relid = oid;
	pi->checkAsUser = table_descr->GetExecuteAsUserId();
	pi->requiredPerms |= required_perms;
	rte->rellockmode = table_descr->LockMode();

	Alias *alias = MakeNode(Alias);
	alias->colnames = NIL;

	// get table alias
	alias->aliasname = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
		table_descr->MdName()->GetMDName()->GetBuffer());

	// get column names
	INT last_attno = 0;
	for (ULONG ul = 0; ul < arity; ++ul)
	{
		const CDXLColDescr *dxl_col_descr = table_descr->GetColumnDescrAt(ul);
		INT attno = dxl_col_descr->AttrNum();

		if (0 < attno)
		{
			// if attno > last_attno + 1, there were dropped attributes
			// add those to the RTE as they are required by GPDB
			for (INT dropped_col_attno = last_attno + 1;
				 dropped_col_attno < attno; dropped_col_attno++)
			{
				String *val_dropped_colname = gpdb::MakeStringValue(PStrDup(""));
				alias->colnames =
					gpdb::LAppend(alias->colnames, val_dropped_colname);
			}

			// non-system attribute
			CHAR *col_name_char_array =
				CTranslatorUtils::CreateMultiByteCharStringFromWCString(
					dxl_col_descr->MdName()->GetMDName()->GetBuffer());
			String *val_colname = gpdb::MakeStringValue(col_name_char_array);

			alias->colnames = gpdb::LAppend(alias->colnames, val_colname);
			last_attno = attno;
		}
	}

	// if there are any dropped columns at the end, add those too to the RangeTblEntry
	for (ULONG ul = last_attno + 1; ul <= num_of_non_sys_cols; ul++)
	{
		String *val_dropped_colname = gpdb::MakeStringValue(PStrDup(""));
		alias->colnames = gpdb::LAppend(alias->colnames, val_dropped_colname);
	}

	rte->eref = alias;
	rte->alias = alias;

	m_dxl_to_plstmt_context->AddPermInfo(pi);

	// set up rte <> perm info link.
	rte->perminfoindex = gpdb::ListLength(
					m_dxl_to_plstmt_context->GetPermInfosList());

	// A new RTE is added to the range table entries list if it's not found in the look
	// up table. However, it is only added to the look up table if it's a result relation
	// This is because the look up table is our way of merging duplicate result relations
	m_dxl_to_plstmt_context->AddRTE(rte);
	GPOS_ASSERT(gpdb::ListLength(
					m_dxl_to_plstmt_context->GetRTableEntriesList()) == index);
	if (UNASSIGNED_QUERYID != assigned_query_id)
	{
		m_dxl_to_plstmt_context->InsertUsedRTEIndexes(assigned_query_id, index);
	}

	return index;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLProjList
//
//	@doc:
//		Translates a DXL projection list node into a target list.
//		For base table projection lists, the caller should provide a base table
//		translation context with table oid, rtable index and mappings for the columns.
//		For other nodes translate_ctxt_left and pdxltrctxRight give
//		the mappings of column ids to target entries in the corresponding child nodes
//		for resolving the origin of the target entries
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLProjList(
	const CDXLNode *project_list_dxlnode,
	const CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *child_contexts,
	CDXLTranslateContext *output_context)
{
	if (nullptr == project_list_dxlnode)
	{
		return nullptr;
	}

	List *target_list = NIL;

	// translate each DXL project element into a target entry
	const ULONG arity = project_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ++ul)
	{
		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		GPOS_ASSERT(EdxlopScalarProjectElem ==
					proj_elem_dxlnode->GetOperator()->GetDXLOperator());
		CDXLScalarProjElem *sc_proj_elem_dxlop =
			CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());
		GPOS_ASSERT(1 == proj_elem_dxlnode->Arity());

		// translate proj element expression
		CDXLNode *expr_dxlnode = (*proj_elem_dxlnode)[0];

		CMappingColIdVarPlStmt colid_var_mapping =
			CMappingColIdVarPlStmt(m_mp, base_table_context, child_contexts,
								   output_context, m_dxl_to_plstmt_context);

		Expr *expr = m_translator_dxl_to_scalar->TranslateDXLToScalar(
			expr_dxlnode, &colid_var_mapping);

		GPOS_ASSERT(nullptr != expr);

		TargetEntry *target_entry = MakeNode(TargetEntry);
		target_entry->expr = expr;
		target_entry->resname =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				sc_proj_elem_dxlop->GetMdNameAlias()->GetMDName()->GetBuffer());
		target_entry->resno = (AttrNumber)(ul + 1);

		if (IsA(expr, Var))
		{
			// check the origin of the left or the right side
			// of the current operator and if it is derived from a base relation,
			// set resorigtbl and resorigcol appropriately

			if (nullptr != base_table_context)
			{
				// translating project list of a base table
				target_entry->resorigtbl = base_table_context->GetOid();
				target_entry->resorigcol = ((Var *) expr)->varattno;
			}
			else
			{
				// not translating a base table proj list: variable must come from
				// the left or right child of the operator

				GPOS_ASSERT(nullptr != child_contexts);
				GPOS_ASSERT(0 != child_contexts->Size());
				ULONG colid = CDXLScalarIdent::Cast(expr_dxlnode->GetOperator())
								  ->GetDXLColRef()
								  ->Id();

				const CDXLTranslateContext *translate_ctxt_left =
					(*child_contexts)[0];
				GPOS_ASSERT(nullptr != translate_ctxt_left);
				const TargetEntry *pteOriginal =
					translate_ctxt_left->GetTargetEntry(colid);

				if (nullptr == pteOriginal)
				{
					// variable not found on the left side
					GPOS_ASSERT(2 == child_contexts->Size());
					const CDXLTranslateContext *pdxltrctxRight =
						(*child_contexts)[1];

					GPOS_ASSERT(nullptr != pdxltrctxRight);
					pteOriginal = pdxltrctxRight->GetTargetEntry(colid);
				}

				if (nullptr == pteOriginal)
				{
					GPOS_RAISE(gpdxl::ExmaDXL,
							   gpdxl::ExmiDXL2PlStmtAttributeNotFound, colid);
				}
				target_entry->resorigtbl = pteOriginal->resorigtbl;
				target_entry->resorigcol = pteOriginal->resorigcol;
			}
		}

		// restore aliases that failed the wide character conversion; this
		// must cover not only Vars but also other expressions (e.g. Consts
		// and Aggrefs) whose aliases can equally fail the conversion
		restore_unknown_locale_resname(output_context->GetQuery(),
									   target_entry);

		// add column mapping to output translation context
		output_context->InsertMapping(sc_proj_elem_dxlop->Id(), target_entry);

		target_list = gpdb::LAppend(target_list, target_entry);
	}

	return target_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CreateTargetListWithNullsForDroppedCols
//
//	@doc:
//		Construct the target list for a DML statement by adding NULL elements
//		for dropped columns
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::CreateTargetListWithNullsForDroppedCols(
	List *target_list, const IMDRelation *md_rel, bool keepDropedAsNull)
{
	// There are cases where target list can be null
	// Eg. insert rows with no columns into a table with no columns
	//
	// create table foo();
	// insert into foo default values;
	if (nullptr == target_list)
	{
		return nullptr;
	}

	GPOS_ASSERT(gpdb::ListLength(target_list) <= md_rel->ColumnCount());

	List *result_list = NIL;
	ULONG last_tgt_elem = 0;
	ULONG resno = 1;

	const ULONG num_of_rel_cols = md_rel->ColumnCount();
	const OID rel_oid = CMDIdGPDB::CastMdid(md_rel->MDId())->Oid();

	for (ULONG ul = 0; ul < num_of_rel_cols; ul++)
	{
		const IMDColumn *md_col = md_rel->GetMdCol(ul);

		if (md_col->IsSystemColumn())
		{
			continue;
		}

		Expr *expr = nullptr;
		if (md_col->IsDropped())
		{
			if (!keepDropedAsNull)
			{
				continue;
			}

			// add a NULL element
			OID oid_type = CMDIdGPDB::CastMdid(
							   m_md_accessor->PtMDType<IMDTypeInt4>()->MDId())
							   ->Oid();

			expr = (Expr *) gpdb::MakeNULLConst(oid_type);
		}
		else
		{
			TargetEntry *target_entry =
				(TargetEntry *) gpdb::ListNth(target_list, last_tgt_elem);
			last_tgt_elem++;

			// A stored generated column, in the INSERT shape.  The executor
			// computes it, and insists that the plan give it a NULL
			// constant (ExecCheckPlanOutput), as the planner's
			// expand_insert_targetlist() does.  ORCA gives it the NULL the
			// Query translator adds for a column the Query leaves out -- but
			// as a column of the plan below, which is not a constant.  Not
			// in Cloudberry's, which has the same check in its executor.
			if (keepDropedAsNull &&
				'\0' != gpdb::GetAttGenerated(rel_oid, md_col->AttrNum()))
			{
				expr = (Expr *) gpdb::MakeNULLConst(
					gpdb::ExprType((Node *) target_entry->expr));
			}
			else
			{
				expr = (Expr *) gpdb::CopyObject(target_entry->expr);
			}
		}

		CHAR *name_str =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				md_col->Mdname().GetMDName()->GetBuffer());
		TargetEntry *te_new =
			gpdb::MakeTargetEntry(expr, resno, name_str, false /*resjunk*/);
		result_list = gpdb::LAppend(result_list, te_new);
		resno++;
	}

	return result_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLProjectListToHashTargetList
//
//	@doc:
//		Create a target list for the hash node of a hash join plan node by creating a list
//		of references to the elements in the child project list
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLProjectListToHashTargetList(
	const CDXLNode *project_list_dxlnode, CDXLTranslateContext *child_context,
	CDXLTranslateContext *output_context)
{
	List *target_list = NIL;
	const ULONG arity = project_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *proj_elem_dxlnode = (*project_list_dxlnode)[ul];
		CDXLScalarProjElem *sc_proj_elem_dxlop =
			CDXLScalarProjElem::Cast(proj_elem_dxlnode->GetOperator());

		const TargetEntry *te_child =
			child_context->GetTargetEntry(sc_proj_elem_dxlop->Id());
		if (nullptr == te_child)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
					   sc_proj_elem_dxlop->Id());
		}

		// get type oid for project element's expression
		GPOS_ASSERT(1 == proj_elem_dxlnode->Arity());

		// find column type
		OID oid_type = gpdb::ExprType((Node *) te_child->expr);
		INT type_modifier = gpdb::ExprTypeMod((Node *) te_child->expr);

		// find the original varno and attno for this column
		Index idx_varnoold = 0;
		AttrNumber attno_old = 0;

		if (IsA(te_child->expr, Var))
		{
			Var *pv = (Var *) te_child->expr;
			idx_varnoold = pv->varnosyn;
			attno_old = pv->varattnosyn;
		}
		else
		{
			idx_varnoold = OUTER_VAR;
			attno_old = te_child->resno;
		}

		// create a Var expression for this target list entry expression
		Var *var =
			gpdb::MakeVar(OUTER_VAR, te_child->resno, oid_type, type_modifier,
						  0	 // varlevelsup
			);

		// set old varno and varattno since makeVar does not set them
		var->varnosyn = idx_varnoold;
		var->varattnosyn = attno_old;

		CHAR *resname = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
			sc_proj_elem_dxlop->GetMdNameAlias()->GetMDName()->GetBuffer());

		TargetEntry *target_entry =
			gpdb::MakeTargetEntry((Expr *) var, (AttrNumber)(ul + 1), resname,
								  false	 // resjunk
			);

		target_list = gpdb::LAppend(target_list, target_entry);
		output_context->InsertMapping(sc_proj_elem_dxlop->Id(), target_entry);
	}

	return target_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLFilterToQual
//
//	@doc:
//		Translates a DXL filter node into a Qual list.
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLFilterToQual(
	const CDXLNode *filter_dxlnode,
	const CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *child_contexts,
	CDXLTranslateContext *output_context)
{
	const ULONG arity = filter_dxlnode->Arity();
	if (0 == arity)
	{
		return NIL;
	}

	GPOS_ASSERT(1 == arity);

	CDXLNode *filter_cond_dxlnode = (*filter_dxlnode)[0];
	GPOS_ASSERT(CTranslatorDXLToScalar::HasBoolResult(filter_cond_dxlnode,
													  m_md_accessor));

	return TranslateDXLScCondToQual(filter_cond_dxlnode, base_table_context,
									child_contexts, output_context);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLScCondToQual
//
//	@doc:
//		Translates a DXL scalar condition node node into a Qual list.
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLScCondToQual(
	const CDXLNode *condition_dxlnode,
	const CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *child_contexts,
	CDXLTranslateContext *output_context)
{
	List *quals_list = NIL;

	GPOS_ASSERT(CTranslatorDXLToScalar::HasBoolResult(
		const_cast<CDXLNode *>(condition_dxlnode), m_md_accessor));

	CMappingColIdVarPlStmt colid_var_mapping =
		CMappingColIdVarPlStmt(m_mp, base_table_context, child_contexts,
							   output_context, m_dxl_to_plstmt_context);

	Expr *expr = m_translator_dxl_to_scalar->TranslateDXLToScalar(
		condition_dxlnode, &colid_var_mapping);

	quals_list = gpdb::LAppend(quals_list, expr);

	return quals_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslatePlanCosts
//
//	@doc:
//		Translates DXL plan costs into the GPDB cost variables
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslatePlanCosts(const CDXLNode *dxlnode, Plan *plan)
{
	CDXLOperatorCost *costs =
		CDXLPhysicalProperties::PdxlpropConvert(dxlnode->GetProperties())
			->GetDXLOperatorCost();

	plan->startup_cost = CostFromStr(costs->GetStartUpCostStr());
	plan->total_cost = CostFromStr(costs->GetTotalCostStr());
	plan->plan_width = CTranslatorUtils::GetIntFromStr(costs->GetWidthStr());

	// In the Postgres planner, the estimates on each node are per QE
	// process, whereas the row estimates in GPORCA are global, across all
	// processes. Divide the row count estimate by the number of segments
	// executing it.
	plan->plan_rows =
		ceil(CostFromStr(costs->GetRowsOutStr()) /
			 m_dxl_to_plstmt_context->GetCurrentSlice()->numsegments);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateProjListAndFilter
//
//	@doc:
//		Translates DXL proj list and filter into GPDB's target and qual lists,
//		respectively
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateProjListAndFilter(
	const CDXLNode *project_list_dxlnode, const CDXLNode *filter_dxlnode,
	const CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *child_contexts, List **targetlist_out,
	List **qual_out, CDXLTranslateContext *output_context)
{
	// translate proj list
	*targetlist_out = TranslateDXLProjList(
		project_list_dxlnode,
		base_table_context,	 // base table translation context
		child_contexts, output_context);

	// translate filter
	*qual_out = TranslateDXLFilterToQual(
		filter_dxlnode,
		base_table_context,	 // base table translation context
		child_contexts, output_context);
}

//-----------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::AddSecurityQuals
//
//	@doc:
//		This method is used to fetch the range table entry from the rewritten
//		parse tree based on the relId and add it's security quals in the quals
//		list. It also modifies the varno of the VAR node present in the
//		security quals and assigns it the value of the index i.e. the
//		position of this rte at m_dxl_to_plstmt_context->m_rtable_entries_list
//		(shortened as "rte_list")
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::AddSecurityQuals(OID relId, List **qual, Index *index)
{
	SContextSecurityQuals ctxt_security_quals(relId);

	// Find the RTE in the parse tree based on the relId and add the security
	// quals of that RTE to the m_security_quals list present in
	// ctxt_security_quals struct.
	FetchSecurityQuals(m_dxl_to_plstmt_context->m_orig_query,
					   &ctxt_security_quals);

	// The varno of the columns related to a particular table is different in
	// the rewritten parse tree and the planned statement tree. In planned
	// statement the varno of the columns is based on the index of the RTE
	// at m_dxl_to_plstmt_context->m_rtable_entries_list. Since we are adding
	// the security quals from the rewritten parse tree to planned statement
	// tree we need to modify the varno of all the VAR nodes present in the
	// security quals and assign it equal to index of the RTE in the rte_list.
	SetSecurityQualsVarnoWalker((Node *) ctxt_security_quals.m_security_quals,
								index);

	// Adding the security quals from m_security_quals list to the qual list
	*qual = gpdb::ListConcat(*qual, ctxt_security_quals.m_security_quals);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::FetchSecurityQuals
//
//	@doc:
//		This method is used to walk the entire rewritten parse tree and
//		search for a range table entry whose relid is equal to the m_relId
//		field of ctxt_security_quals struct. On finding the RTE this method
//		will also add the security quals present in it to the
//		m_security_quals list of ctxt_security_quals struct.
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::FetchSecurityQuals(
	Query *parsetree, SContextSecurityQuals *ctxt_security_quals)
{
	ListCell *lc;

	// Iterate through all the range table entries present in the the rtable
	// of the parsetree and search for a range table entry whose relid is
	// equal to ctxt_security_quals->m_relId. If found then add the security
	// quals of that RTE in the ctxt_security_quals->m_security_quals list.
	// If the range table entry contains a subquery then recurse through that
	// subquery and continue the search.
	foreach (lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		if (RTE_RELATION == rte->rtekind &&
			rte->relid == ctxt_security_quals->m_relId)
		{
			ctxt_security_quals->m_security_quals = gpdb::ListConcat(
				ctxt_security_quals->m_security_quals, rte->securityQuals);
			return true;
		}

		// Cloudberry also recurses into RTE_TABLEFUNCTION, its kind of range
		// table entry for a function over a subquery's rows; PostgreSQL 19
		// has no such kind.
		if (RTE_SUBQUERY == rte->rtekind &&
			FetchSecurityQuals(rte->subquery, ctxt_security_quals))
		{
			return true;
		}
	}

	// Recurse into ctelist
	foreach (lc, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

		if (FetchSecurityQuals(castNode(Query, cte->ctequery),
							   ctxt_security_quals))
		{
			return true;
		}
	}

	// Recurse into sublink subqueries. We have already recursed the sublink
	// subqueries present in the rtable and ctelist. QTW_IGNORE_RC_SUBQUERIES
	// flag indicates to avoid recursing subqueries present in rtable and
	// ctelist
	if (parsetree->hasSubLinks)
	{
		return gpdb::WalkQueryTree(
			parsetree,
			(bool (*)(Node *, void *)) CTranslatorDXLToPlStmt::FetchSecurityQualsWalker,
			ctxt_security_quals, QTW_IGNORE_RC_SUBQUERIES);
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::FetchSecurityQualsWalker
//
//	@doc:
//		This method is a walker to recurse into SUBLINK nodes and search for
//		an RTE having relid equal to m_relId field of ctxt_security_quals struct
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::FetchSecurityQualsWalker(
	Node *node, SContextSecurityQuals *ctxt_security_quals)
{
	if (nullptr == node)
	{
		return false;
	}

	// If the node is a SUBLINK, fetch its subselect node and start the
	// search again for the RTE based on the m_relId field of
	// ctxt_security_quals struct. If we found the RTE then the flag
	// m_found_rte would have been set to true. In that case returning true
	// which indicates to abort the walk immediately.
	if (IsA(node, SubLink))
	{
		SubLink *sub = (SubLink *) node;

		if (FetchSecurityQuals(castNode(Query, sub->subselect),
							   ctxt_security_quals))
		{
			return true;
		}
	}

	return gpdb::WalkExpressionTree(
		node, (bool (*)(Node *, void *)) CTranslatorDXLToPlStmt::FetchSecurityQualsWalker,
		ctxt_security_quals);
}

//-----------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetSecurityQualsVarnoWalker
//
//	@doc:
//		The varno of the columns related to a particular table is different in
//		the rewritten parse tree and the planned statement tree. In planned
//		statement the varno of the columns is based on the index of the RTE at
//		m_dxl_to_plstmt_context->m_rtable_entries_list. Since we are adding
//		the security quals from the rewritten parse tree to planned statement
//		tree we need to modify the varno of all the VAR nodes present in the
//		security quals and assign it equal to index of the RTE in the rte_list.
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::SetSecurityQualsVarnoWalker(Node *node, Index *index)
{
	if (nullptr == node)
	{
		return false;
	}

	if (IsA(node, Var))
	{
		((Var *) node)->varno = *index;
		return false;
	}

	return gpdb::WalkExpressionTree(
		node, (bool (*)(Node *, void *)) CTranslatorDXLToPlStmt::SetSecurityQualsVarnoWalker,
		index);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateHashExprList
//
//	@doc:
//		Translates DXL hash expression list in a redistribute motion node into
//		GPDB's hash expression and expression types lists, respectively
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateHashExprList(
	const CDXLNode *hash_expr_list_dxlnode,
	const CDXLTranslateContext *child_context, List **hash_expr_out_list,
	List **hash_expr_opfamilies_out_list, CDXLTranslateContext *output_context)
{
	GPOS_ASSERT(NIL == *hash_expr_out_list);
	GPOS_ASSERT(NIL == *hash_expr_opfamilies_out_list);

	List *hash_expr_list = NIL;

	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(child_context);

	const ULONG arity = hash_expr_list_dxlnode->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *hash_expr_dxlnode = (*hash_expr_list_dxlnode)[ul];

		GPOS_ASSERT(1 == hash_expr_dxlnode->Arity());
		CDXLNode *expr_dxlnode = (*hash_expr_dxlnode)[0];

		CMappingColIdVarPlStmt colid_var_mapping =
			CMappingColIdVarPlStmt(m_mp, nullptr, child_contexts,
								   output_context, m_dxl_to_plstmt_context);

		Expr *expr = m_translator_dxl_to_scalar->TranslateDXLToScalar(
			expr_dxlnode, &colid_var_mapping);

		hash_expr_list = gpdb::LAppend(hash_expr_list, expr);

		GPOS_ASSERT((ULONG) gpdb::ListLength(hash_expr_list) == ul + 1);
	}

	List *hash_expr_opfamilies = NIL;
	if (GPOS_FTRACE(EopttraceConsiderOpfamiliesForDistribution))
	{
		for (ULONG ul = 0; ul < arity; ul++)
		{
			CDXLNode *hash_expr_dxlnode = (*hash_expr_list_dxlnode)[ul];
			CDXLScalarHashExpr *hash_expr_dxlop =
				CDXLScalarHashExpr::Cast(hash_expr_dxlnode->GetOperator());
			const IMDId *opfamily = hash_expr_dxlop->MdidOpfamily();
			hash_expr_opfamilies = gpdb::LAppendOid(
				hash_expr_opfamilies, CMDIdGPDB::CastMdid(opfamily)->Oid());
		}
	}

	*hash_expr_out_list = hash_expr_list;
	*hash_expr_opfamilies_out_list = hash_expr_opfamilies;

	// cleanup
	child_contexts->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateSortCols
//
//	@doc:
//		Translates DXL sorting columns list into GPDB's arrays of sorting attribute numbers,
//		and sorting operator ids, respectively.
//		The two arrays must be allocated by the caller.
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateSortCols(
	const CDXLNode *sort_col_list_dxl,
	const CDXLTranslateContext *child_context, AttrNumber *att_no_sort_colids,
	Oid *sort_op_oids, Oid *sort_collations_oids, bool *is_nulls_first)
{
	const ULONG arity = sort_col_list_dxl->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CDXLNode *sort_col_dxlnode = (*sort_col_list_dxl)[ul];
		CDXLScalarSortCol *sc_sort_col_dxlop =
			CDXLScalarSortCol::Cast(sort_col_dxlnode->GetOperator());

		ULONG sort_colid = sc_sort_col_dxlop->GetColId();
		const TargetEntry *te_sort_col =
			child_context->GetTargetEntry(sort_colid);
		if (nullptr == te_sort_col)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
					   sort_colid);
		}

		att_no_sort_colids[ul] = te_sort_col->resno;
		sort_op_oids[ul] =
			CMDIdGPDB::CastMdid(sc_sort_col_dxlop->GetMdIdSortOp())->Oid();
		if (sort_collations_oids)
		{
			sort_collations_oids[ul] =
				gpdb::ExprCollation((Node *) te_sort_col->expr);
		}
		is_nulls_first[ul] = sc_sort_col_dxlop->IsSortedNullsFirst();
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CostFromStr
//
//	@doc:
//		Parses a cost value from a string
//
//---------------------------------------------------------------------------
Cost
CTranslatorDXLToPlStmt::CostFromStr(const CWStringBase *str)
{
	CHAR *sz = CTranslatorUtils::CreateMultiByteCharStringFromWCString(
		str->GetBuffer());
	return gpos::clib::Strtod(sz);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::IsTgtTblDistributed
//
//	@doc:
//		Check if given operator is a DML on a distributed table
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::IsTgtTblDistributed(CDXLOperator *dxlop)
{
	if (EdxlopPhysicalDML != dxlop->GetDXLOperator())
	{
		return false;
	}

	CDXLPhysicalDML *phy_dml_dxlop = CDXLPhysicalDML::Cast(dxlop);
	IMDId *mdid = phy_dml_dxlop->GetDXLTableDescr()->MDId();

	return IMDRelation::EreldistrMasterOnly !=
		   m_md_accessor->RetrieveRel(mdid)->GetRelDistribution();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::AddJunkTargetEntryForColId
//
//	@doc:
//		Add a new target entry for the given colid to the given target list
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::AddJunkTargetEntryForColId(
	List **target_list, CDXLTranslateContext *dxl_translate_ctxt, ULONG colid,
	const char *resname)
{
	GPOS_ASSERT(nullptr != target_list);

	const TargetEntry *target_entry = dxl_translate_ctxt->GetTargetEntry(colid);

	if (nullptr == target_entry)
	{
		// colid not found in translate context
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtAttributeNotFound,
				   colid);
	}

	// TODO: Oct 29, 2012; see if entry already exists in the target list

	OID expr_oid = gpdb::ExprType((Node *) target_entry->expr);
	INT type_modifier = gpdb::ExprTypeMod((Node *) target_entry->expr);
	Var *var =
		gpdb::MakeVar(OUTER_VAR, target_entry->resno, expr_oid, type_modifier,
					  0	 // varlevelsup
		);
	ULONG resno = gpdb::ListLength(*target_list) + 1;
	CHAR *resname_str = PStrDup(resname);
	TargetEntry *te_new = gpdb::MakeTargetEntry(
		(Expr *) var, resno, resname_str, true /* resjunk */);
	*target_list = gpdb::LAppend(*target_list, te_new);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::GetGPDBJoinTypeFromDXLJoinType
//
//	@doc:
//		Translates the join type from its DXL representation into the GPDB one
//
//---------------------------------------------------------------------------
JoinType
CTranslatorDXLToPlStmt::GetGPDBJoinTypeFromDXLJoinType(EdxlJoinType join_type)
{
	GPOS_ASSERT(EdxljtSentinel > join_type);

	JoinType jt = JOIN_INNER;

	switch (join_type)
	{
		case EdxljtInner:
			jt = JOIN_INNER;
			break;
		case EdxljtLeft:
			jt = JOIN_LEFT;
			break;
		case EdxljtFull:
			jt = JOIN_FULL;
			break;
		case EdxljtRight:
			jt = JOIN_RIGHT;
			break;
		case EdxljtIn:
			jt = JOIN_SEMI;
			break;
		case EdxljtLeftAntiSemijoin:
			jt = JOIN_ANTI;
			break;
		case EdxljtLeftAntiSemijoinNotIn:
			// Cloudberry's JOIN_LASJ_NOTIN, an anti-join that treats a NULL
			// on either side the way NOT IN does.  PostgreSQL 19 has no such
			// join type, and its executor no such semantics -- the planner
			// never makes NOT IN an anti-join, for exactly that reason -- so
			// such a plan is refused, and the query goes to the planner,
			// which makes NOT IN a hashed SubPlan.
			//
			// Decided at T1, by measuring the alternative.  With the two
			// transforms that make this join turned off, ORCA keeps NOT IN as
			// an apply and implements it as a SubPlan that counts matches
			// and NULLs for every outer row: correct, and 6.8 seconds for a
			// NOT IN of 20,000 rows over 10,000, where the planner's hashed
			// SubPlan takes 6 milliseconds.  A NULL-aware hash anti-join is
			// executor work, and the fallback counters say how often it is
			// wanted.
			GP_UNPORTED("NOT IN as an anti-join");
		default:
			GPOS_ASSERT(!"Unrecognized join type");
	}

	return jt;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCtas
//
//	@doc:
//		Sets the vartypmod fields in the target entries of the given target list
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetVarTypMod(const CDXLPhysicalCTAS *phy_ctas_dxlop,
									 List *target_list)
{
	// target list can be nullptr in CTAS
	IntPtrArray *var_type_mod_array = phy_ctas_dxlop->GetVarTypeModArray();
	GPOS_ASSERT(var_type_mod_array->Size() == gpdb::ListLength(target_list));

	ULONG ul = 0;
	ListCell *lc = nullptr;
	ForEach(lc, target_list)
	{
		TargetEntry *target_entry = (TargetEntry *) lfirst(lc);
		GPOS_ASSERT(IsA(target_entry, TargetEntry));

		if (IsA(target_entry->expr, Var))
		{
			Var *var = (Var *) target_entry->expr;
			var->vartypmod = *(*var_type_mod_array)[ul];
		}
		++ul;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCtas
//
//	@doc:
//		Translates a DXL CTAS node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLCtas(
	const CDXLNode *ctas_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	CDXLPhysicalCTAS *phy_ctas_dxlop =
		CDXLPhysicalCTAS::Cast(ctas_dxlnode->GetOperator());
	CDXLNode *project_list_dxlnode = (*ctas_dxlnode)[0];
	CDXLNode *child_dxlnode = (*ctas_dxlnode)[1];

	GPOS_ASSERT(
		nullptr ==
		phy_ctas_dxlop->GetDxlCtasStorageOption()->GetDXLCtasOptionArray());

	CDXLTranslateContext child_context(m_mp, false,
									   output_context->GetColIdToParamIdMap());

	Plan *plan = TranslateDXLOperatorToPlan(child_dxlnode, &child_context,
											ctxt_translation_prev_siblings);

	// fix target list to match the required column names
	CDXLTranslationContextArray *child_contexts =
		GPOS_NEW(m_mp) CDXLTranslationContextArray(m_mp);
	child_contexts->Append(&child_context);

	List *target_list = TranslateDXLProjList(project_list_dxlnode,
											 nullptr,  // base_table_context
											 child_contexts, output_context);
	SetVarTypMod(phy_ctas_dxlop, target_list);

	SetParamIds(plan);

	// cleanup
	child_contexts->Release();

	// translate operator costs
	TranslatePlanCosts(ctas_dxlnode, plan);

	GpPolicy *distr_policy =
		TranslateDXLPhyCtasToDistrPolicy(phy_ctas_dxlop, target_list);
	m_dxl_to_plstmt_context->AddCtasInfo(distr_policy);

	GPOS_ASSERT(IMDRelation::EreldistrMasterOnly !=
				phy_ctas_dxlop->Ereldistrpolicy());

	m_is_tgt_tbl_distributed = true;

	// Add a result node on top with the correct projection list
	Result *result = MakeNode(Result);
	Plan *result_plan = &(result->plan);
	result_plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
	result_plan->lefttree = plan;

	result_plan->targetlist = target_list;
	SetParamIds(result_plan);

	plan = (Plan *) result;

	return (Plan *) plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLPhyCtasToDistrPolicy
//
//	@doc:
//		Translates distribution policy given by a physical CTAS operator
//
//---------------------------------------------------------------------------
GpPolicy *
CTranslatorDXLToPlStmt::TranslateDXLPhyCtasToDistrPolicy(
	const CDXLPhysicalCTAS *dxlop, List * /*target_list*/)
{
	ULongPtrArray *distr_col_pos_array = dxlop->GetDistrColPosArray();

	const ULONG num_of_distr_cols =
		(distr_col_pos_array == nullptr) ? 0 : distr_col_pos_array->Size();

	ULONG num_of_distr_cols_alloc = 1;
	if (0 < num_of_distr_cols)
	{
		num_of_distr_cols_alloc = num_of_distr_cols;
	}

	// always set numsegments to ALL for CTAS
	GpPolicy *distr_policy =
		gpdb::MakeGpPolicy(POLICYTYPE_PARTITIONED, num_of_distr_cols_alloc,
						   gpdb::GetGPSegmentCount());

	GPOS_ASSERT(IMDRelation::EreldistrHash == dxlop->Ereldistrpolicy() ||
				IMDRelation::EreldistrRandom == dxlop->Ereldistrpolicy() ||
				IMDRelation::EreldistrReplicated == dxlop->Ereldistrpolicy());

	if (IMDRelation::EreldistrReplicated == dxlop->Ereldistrpolicy())
	{
		distr_policy->ptype = POLICYTYPE_REPLICATED;
	}
	else
	{
		distr_policy->ptype = POLICYTYPE_PARTITIONED;
	}

	distr_policy->nattrs = 0;
	if (IMDRelation::EreldistrHash == dxlop->Ereldistrpolicy())
	{
		GPOS_ASSERT(0 < num_of_distr_cols);
		distr_policy->nattrs = num_of_distr_cols;
		IMdIdArray *opclasses = dxlop->GetDistrOpclasses();
		GPOS_ASSERT(opclasses->Size() == num_of_distr_cols);
		for (ULONG ul = 0; ul < num_of_distr_cols; ul++)
		{
			ULONG col_pos_idx = *((*distr_col_pos_array)[ul]);
			distr_policy->attrs[ul] = col_pos_idx + 1;

			Oid opclass = CMDIdGPDB::CastMdid((*opclasses)[ul])->Oid();
			distr_policy->opclasses[ul] = opclass;
		}
	}
	return distr_policy;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCtasStorageOptions
//
//	@doc:
//		Translates CTAS options
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::TranslateDXLCtasStorageOptions(
	CDXLCtasStorageOptions::CDXLCtasOptionArray *ctas_storage_options)
{
	if (nullptr == ctas_storage_options)
	{
		return NIL;
	}

	const ULONG num_of_options = ctas_storage_options->Size();
	List *options = NIL;
	for (ULONG ul = 0; ul < num_of_options; ul++)
	{
		CDXLCtasStorageOptions::CDXLCtasOption *pdxlopt =
			(*ctas_storage_options)[ul];
		CWStringBase *str_name = pdxlopt->m_str_name;
		CWStringBase *str_value = pdxlopt->m_str_value;
		DefElem *def_elem = MakeNode(DefElem);
		def_elem->defname =
			CTranslatorUtils::CreateMultiByteCharStringFromWCString(
				str_name->GetBuffer());

		if (!pdxlopt->m_is_null)
		{
			NodeTag arg_type = (NodeTag) pdxlopt->m_type;

			GPOS_ASSERT(T_Integer == arg_type || T_String == arg_type);
			if (T_Integer == arg_type)
			{
				def_elem->arg = (Node *) gpdb::MakeIntegerValue(
					CTranslatorUtils::GetLongFromStr(str_value));
			}
			else
			{
				def_elem->arg = (Node *) gpdb::MakeStringValue(
					CTranslatorUtils::CreateMultiByteCharStringFromWCString(
						str_value->GetBuffer()));
			}
		}

		options = gpdb::LAppend(options, def_elem);
	}

	return options;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLBitmapTblScan
//
//	@doc:
//		Translates a DXL bitmap table scan node into a BitmapHeapScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLBitmapTblScan(
	const CDXLNode *bitmapscan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	const CDXLTableDescr *table_descr = nullptr;

	// The dynamic form is this scan of the partitioned table, with a copy of
	// it for each partition put under a Dynamic Scan at the end; see
	// TranslateDXLDynTblScan.
	CDXLOperator *dxl_operator = bitmapscan_dxlnode->GetOperator();
	BOOL is_dynamic = (EdxlopPhysicalDynamicBitmapTableScan ==
					   dxl_operator->GetDXLOperator());
	if (is_dynamic)
	{
		table_descr = CDXLPhysicalDynamicBitmapTableScan::Cast(dxl_operator)
						  ->GetDXLTableDescr();
	}
	else
	{
		table_descr =
			CDXLPhysicalBitmapTableScan::Cast(dxl_operator)->GetDXLTableDescr();
	}

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	const IMDRelation *md_rel = m_md_accessor->RetrieveRel(table_descr->MDId());

	// Lock any table we are to scan, since it may not have been properly locked
	// by the parser (e.g in case of generated scans for partitioned tables)
	CMDIdGPDB *mdid = CMDIdGPDB::CastMdid(md_rel->MDId());
	GPOS_ASSERT(table_descr->LockMode() != -1);
	gpdb::GPDBLockRelationOid(mdid->Oid(), table_descr->LockMode());

	Index index = ProcessDXLTblDescr(table_descr, &base_table_context);

	BitmapHeapScan *bitmap_tbl_scan = MakeNode(BitmapHeapScan);
	bitmap_tbl_scan->scan.scanrelid = index;

	Plan *plan = &(bitmap_tbl_scan->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(bitmapscan_dxlnode, plan);

	GPOS_ASSERT(4 == bitmapscan_dxlnode->Arity());

	// translate proj list and filter
	CDXLNode *project_list_dxlnode = (*bitmapscan_dxlnode)[0];
	CDXLNode *filter_dxlnode = (*bitmapscan_dxlnode)[1];
	CDXLNode *recheck_cond_dxlnode = (*bitmapscan_dxlnode)[2];
	CDXLNode *bitmap_access_path_dxlnode = (*bitmapscan_dxlnode)[3];

	List *quals_list = nullptr;
	TranslateProjListAndFilter(
		project_list_dxlnode, filter_dxlnode,
		&base_table_context,  // translate context for the base table
		ctxt_translation_prev_siblings, &plan->targetlist, &quals_list,
		output_context);

	// No security quals here, unlike TranslateDXLTblScan, and that is safe:
	// ORCA plans no bitmap scan of a relation whose range table entry has
	// any -- CXformSelect2BitmapBoolOp declines a Get that says it has them,
	// as the index scan transforms do -- so the table scan is the only scan
	// that meets them.
	plan->qual = quals_list;

	bitmap_tbl_scan->bitmapqualorig = TranslateDXLFilterToQual(
		recheck_cond_dxlnode, &base_table_context,
		ctxt_translation_prev_siblings, output_context);

	bitmap_tbl_scan->scan.plan.lefttree = TranslateDXLBitmapAccessPath(
		bitmap_access_path_dxlnode, output_context, md_rel, table_descr,
		&base_table_context, ctxt_translation_prev_siblings, bitmap_tbl_scan);

	if (is_dynamic)
	{
		CDXLPhysicalDynamicBitmapTableScan *dyn_bitmap_dxlop =
			CDXLPhysicalDynamicBitmapTableScan::Cast(dxl_operator);
		return TranslateDynamicScan(
			bitmapscan_dxlnode, plan, index, table_descr,
			dyn_bitmap_dxlop->GetParts(), dyn_bitmap_dxlop->GetSelectorIds(),
			output_context, ctxt_translation_prev_siblings);
	}

	SetParamIds(plan);

	return (Plan *) bitmap_tbl_scan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLBitmapAccessPath
//
//	@doc:
//		Translate the tree of bitmap index operators that are under the given
//		(dynamic) bitmap table scan.
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLBitmapAccessPath(
	const CDXLNode *bitmap_access_path_dxlnode,
	CDXLTranslateContext *output_context, const IMDRelation *md_rel,
	const CDXLTableDescr *table_descr,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings,
	BitmapHeapScan *bitmap_tbl_scan)
{
	Edxlopid dxl_op_id =
		bitmap_access_path_dxlnode->GetOperator()->GetDXLOperator();
	if (EdxlopScalarBitmapIndexProbe == dxl_op_id)
	{
		return TranslateDXLBitmapIndexProbe(
			bitmap_access_path_dxlnode, output_context, md_rel, table_descr,
			base_table_context, ctxt_translation_prev_siblings,
			bitmap_tbl_scan);
	}
	GPOS_ASSERT(EdxlopScalarBitmapBoolOp == dxl_op_id);

	return TranslateDXLBitmapBoolOp(
		bitmap_access_path_dxlnode, output_context, md_rel, table_descr,
		base_table_context, ctxt_translation_prev_siblings, bitmap_tbl_scan);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToScalar::TranslateDXLBitmapBoolOp
//
//	@doc:
//		Translates a DML bitmap bool op expression
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLBitmapBoolOp(
	const CDXLNode *bitmap_boolop_dxlnode, CDXLTranslateContext *output_context,
	const IMDRelation *md_rel, const CDXLTableDescr *table_descr,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings,
	BitmapHeapScan *bitmap_tbl_scan)
{
	GPOS_ASSERT(nullptr != bitmap_boolop_dxlnode);
	GPOS_ASSERT(EdxlopScalarBitmapBoolOp ==
				bitmap_boolop_dxlnode->GetOperator()->GetDXLOperator());

	CDXLScalarBitmapBoolOp *sc_bitmap_boolop_dxlop =
		CDXLScalarBitmapBoolOp::Cast(bitmap_boolop_dxlnode->GetOperator());

	CDXLNode *left_tree_dxlnode = (*bitmap_boolop_dxlnode)[0];
	CDXLNode *right_tree_dxlnode = (*bitmap_boolop_dxlnode)[1];

	Plan *left_plan = TranslateDXLBitmapAccessPath(
		left_tree_dxlnode, output_context, md_rel, table_descr,
		base_table_context, ctxt_translation_prev_siblings, bitmap_tbl_scan);
	Plan *right_plan = TranslateDXLBitmapAccessPath(
		right_tree_dxlnode, output_context, md_rel, table_descr,
		base_table_context, ctxt_translation_prev_siblings, bitmap_tbl_scan);
	List *child_plan_list = ListMake2(left_plan, right_plan);

	Plan *plan = nullptr;

	if (CDXLScalarBitmapBoolOp::EdxlbitmapAnd ==
		sc_bitmap_boolop_dxlop->GetDXLBitmapOpType())
	{
		BitmapAnd *bitmapand = MakeNode(BitmapAnd);
		bitmapand->plan.plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
		bitmapand->bitmapplans = child_plan_list;
		bitmapand->plan.targetlist = nullptr;
		bitmapand->plan.qual = nullptr;
		plan = (Plan *) bitmapand;
	}
	else
	{
		BitmapOr *bitmapor = MakeNode(BitmapOr);
		bitmapor->plan.plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();
		bitmapor->bitmapplans = child_plan_list;
		bitmapor->plan.targetlist = nullptr;
		bitmapor->plan.qual = nullptr;
		plan = (Plan *) bitmapor;
	}


	return plan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLBitmapIndexProbe
//
//	@doc:
//		Translate CDXLScalarBitmapIndexProbe into a BitmapIndexScan
//		or a DynamicBitmapIndexScan
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLBitmapIndexProbe(
	const CDXLNode *bitmap_index_probe_dxlnode,
	CDXLTranslateContext *output_context, const IMDRelation *md_rel,
	const CDXLTableDescr *table_descr,
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings,
	BitmapHeapScan *bitmap_tbl_scan)
{
	CDXLScalarBitmapIndexProbe *sc_bitmap_idx_probe_dxlop =
		CDXLScalarBitmapIndexProbe::Cast(
			bitmap_index_probe_dxlnode->GetOperator());

	// Only the plain form, Cloudberry's DynamicBitmapIndexScan included: under
	// a dynamic bitmap table scan it scans the partitioned table's index, and
	// each partition's scan is made from it (TranslateDXLDynTblScan).
	BitmapIndexScan *bitmap_idx_scan = MakeNode(BitmapIndexScan);
	bitmap_idx_scan->scan.scanrelid = bitmap_tbl_scan->scan.scanrelid;

	CMDIdGPDB *mdid_index = CMDIdGPDB::CastMdid(
		sc_bitmap_idx_probe_dxlop->GetDXLIndexDescr()->MDId());
	const IMDIndex *index = m_md_accessor->RetrieveIndex(mdid_index);
	Oid index_oid = mdid_index->Oid();
	// Lock any index we are to scan, since it may not have been properly locked
	// by the parser (e.g in case of generated scans for partitioned indexes)
	gpdb::GPDBLockRelationOid(index_oid, table_descr->LockMode());

	GPOS_ASSERT(InvalidOid != index_oid);
	CheckIndexUsableBySnapshots(index_oid);
	bitmap_idx_scan->indexid = index_oid;
	Plan *plan = &(bitmap_idx_scan->scan.plan);
	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	GPOS_ASSERT(1 == bitmap_index_probe_dxlnode->Arity());
	CDXLNode *index_cond_list_dxlnode = (*bitmap_index_probe_dxlnode)[0];
	List *index_cond = NIL;
	List *index_orig_cond = NIL;

	TranslateIndexConditions(
		index_cond_list_dxlnode, table_descr, true /*is_bitmap_index_probe*/,
		index, md_rel, output_context, base_table_context,
		ctxt_translation_prev_siblings, &index_cond, &index_orig_cond);

	bitmap_idx_scan->indexqual = index_cond;
	bitmap_idx_scan->indexqualorig = index_orig_cond;
	/*
	 * As of 8.4, the indexstrategy and indexsubtype fields are no longer
	 * available or needed in IndexScan. Ignore them.
	 */
	SetParamIds(plan);

	return plan;
}

// translates a DXL Value Scan node into a GPDB Value scan node
Plan *
CTranslatorDXLToPlStmt::TranslateDXLValueScan(
	const CDXLNode *value_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// translation context for column mappings
	CDXLTranslateContextBaseTable base_table_context(m_mp);

	// we will add the new range table entry as the last element of the range table
	Index index =
		gpdb::ListLength(m_dxl_to_plstmt_context->GetRTableEntriesList()) + 1;

	base_table_context.SetRelIndex(index);

	// create value scan node
	ValuesScan *value_scan = MakeNode(ValuesScan);
	value_scan->scan.scanrelid = index;
	Plan *plan = &(value_scan->scan.plan);

	RangeTblEntry *rte = TranslateDXLValueScanToRangeTblEntry(
		value_scan_dxlnode, output_context, &base_table_context);
	GPOS_ASSERT(nullptr != rte);

	value_scan->values_lists = (List *) gpdb::CopyObject(rte->values_lists);

	m_dxl_to_plstmt_context->AddRTE(rte);

	plan->plan_node_id = m_dxl_to_plstmt_context->GetNextPlanId();

	// translate operator costs
	TranslatePlanCosts(value_scan_dxlnode, plan);

	// a table scan node must have at least 2 children: projection list and at least 1 value list
	GPOS_ASSERT(2 <= value_scan_dxlnode->Arity());

	CDXLNode *project_list_dxlnode = (*value_scan_dxlnode)[EdxltsIndexProjList];

	// translate proj list
	List *target_list = TranslateDXLProjList(
		project_list_dxlnode, &base_table_context, nullptr, output_context);

	plan->targetlist = target_list;

	return (Plan *) value_scan;
}

List *
CTranslatorDXLToPlStmt::TranslateNestLoopParamList(
	CDXLColRefArray *pdrgdxlcrOuterRefs, CDXLTranslateContext *dxltrctxLeft,
	CDXLTranslateContext *dxltrctxRight)
{
	List *nest_params_list = NIL;
	for (ULONG ul = 0; ul < pdrgdxlcrOuterRefs->Size(); ul++)
	{
		CDXLColRef *pdxlcr = (*pdrgdxlcrOuterRefs)[ul];
		ULONG ulColid = pdxlcr->Id();
		// left child context contains the target entry for the nest params col refs
		const TargetEntry *target_entry = dxltrctxLeft->GetTargetEntry(ulColid);
		GPOS_ASSERT(nullptr != target_entry);
		Var *old_var = (Var *) target_entry->expr;

		Var *new_var =
			gpdb::MakeVar(OUTER_VAR, target_entry->resno, old_var->vartype,
						  old_var->vartypmod, 0 /*varlevelsup*/);
		new_var->varnosyn = old_var->varnosyn;
		new_var->varattnosyn = old_var->varattnosyn;

		NestLoopParam *nest_params = MakeNode(NestLoopParam);
		// right child context contains the param entry for the nest params col refs
		const CMappingElementColIdParamId *colid_param_mapping =
			dxltrctxRight->GetParamIdMappingElement(ulColid);
		GPOS_ASSERT(nullptr != colid_param_mapping);
		nest_params->paramno = colid_param_mapping->ParamId();
		nest_params->paramval = new_var;
		nest_params_list =
			gpdb::LAppend(nest_params_list, (void *) nest_params);
	}
	return nest_params_list;
}

void
CTranslatorDXLToPlStmt::CheckSafeTargetListForAOTables(List *target_list)
{
	ListCell *lc = nullptr;

	ForEach(lc, target_list)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		// Cloudberry also lets through gp_segment_id and gp_foreign_server,
		// the two system columns it adds.  PostgreSQL 19 has neither, so no
		// target entry can name one.
		if (te->resorigcol < 0 &&
			te->resorigcol != SelfItemPointerAttributeNumber &&
			te->resorigcol != TableOidAttributeNumber)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
					   GPOS_WSZ_LIT("Invalid system target list found for AO table"));
		}
	}
}

List *
CTranslatorDXLToPlStmt::CreateDirectCopyTargetList(List *target_list)
{
	List *result_target_list = NIL;
	ListCell *lc = nullptr;

	ForEach(lc, target_list)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Node *expr = (Node *) te->expr;

		Var *var = gpdb::MakeVar(OUTER_VAR, te->resno, gpdb::ExprType(expr),
				gpdb::ExprTypeMod(expr), 0 /* varlevelsup */);
		TargetEntry *new_te = gpdb::MakeTargetEntry((Expr *) var, te->resno, te->resname, te->resjunk);

		result_target_list = gpdb::LAppend(result_target_list, new_te);
	}

	return result_target_list;
}

List *
CTranslatorDXLToPlStmt::GetRelationActiveColums(const IMDRelation *md_rel)
{
	List *result_list = NIL;
	ULONG resno = 1;

	const ULONG num_of_rel_cols = md_rel->ColumnCount();

	for (ULONG ul = 0; ul < num_of_rel_cols; ul++)
	{
		const IMDColumn *md_col = md_rel->GetMdCol(ul);

		if (md_col->IsSystemColumn())
		{
			continue;
		}

		if (!md_col->IsDropped())
		{
			result_list = gpdb::LAppendInt(result_list, resno);
		}

		resno++;
	}

	return result_list;
}

// A bool Const expression is used as index condition if index column is used
// as part of ORDER BY clause. Because ORDER BY doesn't have any index conditions.
// This function checks if index is used for Order by.
bool
CTranslatorDXLToPlStmt::IsIndexForOrderBy(
	CDXLTranslateContextBaseTable *base_table_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings,
	CDXLTranslateContext *output_context, CDXLNode *index_cond_list_dxlnode)
{
	const ULONG arity = index_cond_list_dxlnode->Arity();
	CMappingColIdVarPlStmt colid_var_mapping(
		m_mp, base_table_context, ctxt_translation_prev_siblings,
		output_context, m_dxl_to_plstmt_context);
	if (arity == 1)
	{
		Expr *index_cond_expr =
			m_translator_dxl_to_scalar->TranslateDXLToScalar(
				(*index_cond_list_dxlnode)[0], &colid_var_mapping);
		if (IsA(index_cond_expr, Const))
		{
			return true;
		}
		return false;
	}
	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::ExtractParallelWorkersFromDXL
//
//	@doc:
//		Extract parallel workers count from DXL node tree recursively.
//		Since parallel degree is uniform across all parallel scans in a query,
//		returns the first parallel degree found from any CDXLPhysicalParallelTableScan,
//		or 1 if no parallel scan exists.
//
//---------------------------------------------------------------------------
ULONG
CTranslatorDXLToPlStmt::ExtractParallelWorkersFromDXL(const CDXLNode *dxlnode)
{
	if (nullptr == dxlnode)
	{
		return 1;
	}

	CDXLOperator *dxlop = dxlnode->GetOperator();
	if (EdxlopPhysicalParallelTableScan == dxlop->GetDXLOperator())
	{
		// Return parallel workers from the parallel table scan operator
		// All parallel scans in the query share the same parallel degree
		CDXLPhysicalParallelTableScan *parallel_scan_dxlop =
			CDXLPhysicalParallelTableScan::Cast(dxlop);
		return parallel_scan_dxlop->UlParallelWorkers();
	}
	else if (EdxlopPhysicalTableScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalDynamicTableScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalIndexScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalIndexOnlyScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalBitmapTableScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalDynamicBitmapTableScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalForeignScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalDynamicForeignScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalDynamicIndexScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalDynamicIndexOnlyScan == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalValuesScan == dxlop->GetDXLOperator())
	{
		// Non-parallel scans (table, index, bitmap, foreign, values)
		// These are leaf nodes in terms of parallel worker extraction
		// Return 1 to indicate no parallel workers
		return 1;
	}
	else if (EdxlopPhysicalMotionGather == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalMotionBroadcast == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalMotionRedistribute == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalMotionRandom == dxlop->GetDXLOperator() ||
			 EdxlopPhysicalMotionRoutedDistribute == dxlop->GetDXLOperator())
	{
		// Motion node creates a slice boundary - do not recurse into child
		// The child's parallel workers belong to the sending slice, not receiving slice
		// Return 0 to indicate the receiving slice (current slice) has no parallel workers
		return 1;
	}

	// Recursively check child nodes, return early when first parallel scan is found
	for (ULONG ul = 0; ul < dxlnode->Arity(); ul++)
	{
		ULONG child_parallel_workers = ExtractParallelWorkersFromDXL((*dxlnode)[ul]);
		if (child_parallel_workers > 1)
		{
			return child_parallel_workers;
		}
	}

	return 1;
}

// EOF
