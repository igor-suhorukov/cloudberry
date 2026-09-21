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
	// by every snapshot yet, which T0 plans no scans of.  dependsOnRole is set
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

	// The operators the port translates, which is the first group of
	// cloudberry.md's "Next": T0.  Every other operator is refused here, by its
	// own name, before anything is built -- including the ones whose
	// translation compiles, such as the index scans, because compiling is not
	// the same as having been tested against PostgreSQL 19's executor.  Each
	// later group adds its operators to this list with the tests that prove
	// them, and the refusal is what the fallback counters report until then.
	switch (ulOpId)
	{
		case EdxlopPhysicalTableScan:
		case EdxlopPhysicalResult:
		case EdxlopPhysicalLimit:
		case EdxlopPhysicalSort:
		case EdxlopPhysicalAgg:
		case EdxlopPhysicalValuesScan:
		case EdxlopPhysicalMaterialize:
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
Plan *
CTranslatorDXLToPlStmt::TranslateDXLHashJoin(
	const CDXLNode *hj_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T1: hash joins.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("hash joins");
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
	CDXLPhysicalTVF *dxlop = CDXLPhysicalTVF::Cast(tvf_dxlnode->GetOperator());
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

	if (dxlop->FuncMdId()->IsValid())
	{
		target_list = gpdb::ProcessRecordFuncTargetList(CMDIdGPDB::CastMdid(dxlop->FuncMdId())->Oid(), target_list);
	}
	plan->targetlist = target_list;

	ListCell *lc_target_entry = nullptr;

	rtfunc->funccolnames = NIL;
	rtfunc->funccoltypes = NIL;
	rtfunc->funccoltypmods = NIL;
	rtfunc->funccolcollations = NIL;
	rtfunc->funccolcount = gpdb::ListLength(target_list);
	ForEach(lc_target_entry, target_list)
	{
		TargetEntry *target_entry = (TargetEntry *) lfirst(lc_target_entry);
		OID oid_type = gpdb::ExprType((Node *) target_entry->expr);
		GPOS_ASSERT(InvalidOid != oid_type);

		INT typ_mod = gpdb::ExprTypeMod((Node *) target_entry->expr);
		Oid collation_type_oid = gpdb::TypeCollation(oid_type);

		rtfunc->funccolnames = gpdb::LAppend(
			rtfunc->funccolnames, gpdb::MakeStringValue(target_entry->resname));
		rtfunc->funccoltypes = gpdb::LAppendOid(rtfunc->funccoltypes, oid_type);
		rtfunc->funccoltypmods =
			gpdb::LAppendInt(rtfunc->funccoltypmods, typ_mod);
		// GPDB_91_MERGE_FIXME: collation
		rtfunc->funccolcollations =
			gpdb::LAppendOid(rtfunc->funccolcollations, collation_type_oid);
	}
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

	rtfunc->funccolcount = (int) num_of_cols;
	rtfunc->funcparams = funcparams;
	// GPDB_91_MERGE_FIXME: collation
	// set rtfunc->funccoltypemods & rtfunc->funccolcollations?
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
	// T1: nested loop joins.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("nested loop joins");
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
	// T1: merge joins.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("merge joins");
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
	// T1: hash joins.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("hash joins");
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

// The four helpers below are TranslateDXLWindowAgg's, and have no caller
// while its body refuses; T1 brings it back.  [[maybe_unused]] rather than
// removing them, so that they come back with it unchanged.
[[maybe_unused]] static
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

[[maybe_unused]] static
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

[[maybe_unused]] static
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

[[maybe_unused]] static
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
	// T1: window functions.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("window functions");
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
//		  * into the child's own qual, when the child is a sequential or
//		    values scan or an aggregate, the nodes that test their qual on
//		    the rows they are about to project.  That is where the planner
//		    puts a WHERE and a HAVING.  The filter then computes the child's
//		    columns a second time, so only when none it reads is volatile;
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
	SetParamIds(plan);

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
	// T3: partition selection.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("partition selection");
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
	// T1: Append.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("Append");
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
//		Translate DXL CTE Producer node into GPDB share input scan plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLCTEProducerToSharedScan(
	const CDXLNode *cte_producer_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T1: common table expressions.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("common table expressions");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLCTEConsumerToSharedScan
//
//	@doc:
//		Translate DXL CTE Consumer node into GPDB share input scan plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLCTEConsumerToSharedScan(
	const CDXLNode *cte_consumer_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// T1: common table expressions.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("common table expressions");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLSequence
//
//	@doc:
//		Translate DXL sequence node into GPDB Sequence plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLSequence(
	const CDXLNode *sequence_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T3: Sequence.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("Sequence");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynTblScan
//
//	@doc:
//		Translates a DXL dynamic table scan node into a DynamicSeqScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynTblScan(
	const CDXLNode *dyn_tbl_scan_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray * /*ctxt_translation_prev_siblings*/)
{
	// T3: dynamic table scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("dynamic table scans");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynIdxOnlyScan
//
//	@doc:
//		Translates a DXL dynamic index scan node into a DynamicIndexOnlyScan
//		node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynIdxOnlyScan(
	const CDXLNode *dyn_idx_only_scan_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T3: dynamic index-only scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("dynamic index-only scans");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDynIdxScan
//
//	@doc:
//		Translates a DXL dynamic index scan node into a DynamicIndexScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDynIdxScan(
	const CDXLNode *dyn_idx_only_scan_dxlnode,
	CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T3: dynamic index scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("dynamic index scans");
}

// Not RemapAttrsFromTupDesc, the helper that renumbers a qual's columns from
// the root partition to a leaf: TranslateDXLDynForeignScan was its only
// caller, and that is T3's.  It comes back with it, from Cloudberry's file,
// and brings Cloudberry's change_varattnos_of_a_varno (executor.h) with it.

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
	// T3: dynamic foreign scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("dynamic foreign scans");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateDXLDml
//
//	@doc:
//		Translates a DXL DML node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::TranslateDXLDml(
	const CDXLNode *dml_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T2: INSERT, UPDATE and DELETE.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("INSERT, UPDATE and DELETE");
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
Plan *
CTranslatorDXLToPlStmt::TranslateDXLAssert(
	const CDXLNode *assert_dxlnode, CDXLTranslateContext *output_context,
	CDXLTranslationContextArray *ctxt_translation_prev_siblings)
{
	// T2: runtime assertions.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("runtime assertions");
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
			expr = (Expr *) gpdb::CopyObject(target_entry->expr);
			last_tgt_elem++;
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
			// whether ORCA may plan one at all is T1's to decide.
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
	// T1: bitmap table scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("bitmap table scans");
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
	// T1: bitmap index scans.  This body refuses until then, and Cloudberry's is
	// in github/cloudberry/src/backend/gpopt/translate/CTranslatorDXLToPlStmt.cpp,
	// unchanged, for the step that brings it back.
	GP_UNPORTED("bitmap index scans");
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
