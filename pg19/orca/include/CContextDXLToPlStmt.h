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
//	Ported from github/cloudberry/src/include/gpopt/translate/CContextDXLToPlStmt.h,
//	and changed for PostgreSQL 19: past the include paths, a comment beside
//	each change, or beside what replaced it, says what and why.
//	Cloudberry's notice for the original follows, as the Apache License
//	requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CContextDXLToPlStmt.h
//
//	@doc:
//		Class providing access to CIdGenerators (needed to number initplans, motion
//		nodes as well as params), list of RangeTableEntries and Subplans
//		generated so far during DXL-->PlStmt translation.
//
//	@test:
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CContextDXLToPlStmt_H
#define GPDXL_CContextDXLToPlStmt_H

#include <vector>

#include "gpos/base.h"

#include "gpdbwrappers.h"
#include "CDXLTranslateContext.h"
#include "CDXLTranslateContextBaseTable.h"
#include "CTranslatorUtils.h"
#include "naucrates/dxl/CIdGenerator.h"
#include "naucrates/dxl/gpdb_types.h"
#include "naucrates/dxl/operators/CDXLScalarIdent.h"

extern "C" {
#include "nodes/plannodes.h"

// PlanSlice, which Cloudberry adds to plannodes.h and PostgreSQL 19 does not have.
#include "cdb/cdb_plan_nodes.h"
}

namespace gpdxl
{
// fwd decl
class CDXLTranslateContext;

using HMUlDxltrctx =
	CHashMap<ULONG, CDXLTranslateContext, gpos::HashValue<ULONG>,
			 gpos::Equals<ULONG>, CleanupDelete<ULONG>,
			 CleanupDelete<CDXLTranslateContext>>;

//---------------------------------------------------------------------------
//	@class:
//		CContextDXLToPlStmt
//
//	@doc:
//		Class providing access to CIdGenerators (needed to number initplans, motion
//		nodes as well as params), list of RangeTableEntries and Subplans
//		generated so far during DXL-->PlStmt translation.
//
//---------------------------------------------------------------------------
class CContextDXLToPlStmt
{
private:
	// cte producer information
	struct SCTEEntryInfo
	{
		// producer idx mapping
		ULongPtrArray *m_pidxmap;

		// producer plan
		// A Plan, not Cloudberry's ShareInputScan: that node does not exist on
		// PostgreSQL 19, and what a CTE producer becomes here is decided with
		// the CTE operators, at T1.  Until then nothing stores one.
		Plan *m_cte_producer_plan;


		// ctor
		SCTEEntryInfo(ULongPtrArray *idxmap, Plan *plan_cte) : 
		m_pidxmap(idxmap), m_cte_producer_plan(plan_cte)
		{
			GPOS_ASSERT(plan_cte);
		}

		~SCTEEntryInfo() = default;
	};

	// hash maps mapping ULONG -> SCTEEntryInfo
	using HMUlCTEProducerInfo =
		CHashMap<ULONG, SCTEEntryInfo, gpos::HashValue<ULONG>,
				 gpos::Equals<ULONG>, CleanupDelete<ULONG>,
				 CleanupDelete<SCTEEntryInfo>>;

	using HMUlIndex =
		CHashMap<ULONG, Index, gpos::HashValue<ULONG>, gpos::Equals<ULONG>,
				 CleanupDelete<ULONG>, CleanupDelete<Index>>;

	CMemoryPool *m_mp;

	// counter for generating plan ids
	CIdGenerator *m_plan_id_counter;

	// counter for generating motion ids
	CIdGenerator *m_motion_id_counter;

	// counter for generating unique param ids
	CIdGenerator *m_param_id_counter;
	List *m_param_types_list;

	// What operator classes to use for distribution keys?
	DistributionHashOpsKind m_distribution_hashops;

	// list of all rtable entries
	List *m_rtable_entries_list;

	// list of all RTEPermissionInfo entries
	List *m_perminfo_list;

	// list of all subplan entries
	List *m_subplan_entries_list;
	List *m_subplan_sliceids_list;

	// The subplans whose plans the executor should be ready to rescan from
	// the start: PlannedStmt.rewindPlanIDs.  Not in Cloudberry's context,
	// whose Material forces its own tuplestore; see TranslateDXLMaterialize.
	Bitmapset *m_rewind_plan_ids;

	// List of PlanSlices
	List *m_slices_list;

	PlanSlice *m_current_slice;

	// index of the target relation in the rtable or 0 if not a DML statement
	ULONG m_result_relation_index;

	// hash map of the cte identifiers and the cte consumers with the same cte identifier
	HMUlCTEProducerInfo *m_cte_producer_info;

	// CTAS distribution policy
	GpPolicy *m_distribution_policy;

	UlongToUlongMap *m_part_selector_to_param_map;

	// hash map of the queryid (of DML query) and the target relation index
	HMUlIndex *m_used_rte_indexes;

	// the aggno and aggtransno in agg 
	List	   *m_agg_infos;		/* AggInfo structs */
	List	   *m_agg_trans_infos;	/* AggTransInfo structs */
public:
	// ctor/dtor
	CContextDXLToPlStmt(CMemoryPool *mp, CIdGenerator *plan_id_counter,
						CIdGenerator *motion_id_counter,
						CIdGenerator *param_id_counter,
						DistributionHashOpsKind distribution_hashops);

	// dtor
	~CContextDXLToPlStmt();

	// retrieve the next plan id
	ULONG GetNextPlanId();

	// retrieve the current motion id
	ULONG GetCurrentMotionId();

	// retrieve the next motion id
	ULONG GetNextMotionId();

	// retrieve the current parameter type list
	List *GetParamTypes();

	// retrieve the next parameter id
	ULONG GetNextParamId(OID typeoid);

	// register a newly CTE producer
	void RegisterCTEProducerInfo(ULONG cte_id, ULongPtrArray *producer_output_colidx_map, Plan *producer);

	// return the Share Input Scan plans representing the CTE producer
	std::pair<ULongPtrArray *, Plan *> GetCTEProducerInfo(ULONG cte_id) const;

	// return list of range table entries
	List *
	GetRTableEntriesList() const
	{
		return m_rtable_entries_list;
	}

	// return list of perfinfos
	List *
	GetPermInfosList() const
	{
		return m_perminfo_list;
	}

	List *
	GetSubplanEntriesList() const
	{
		return m_subplan_entries_list;
	}

	// index of result relation in the rtable
	ULONG
	GetResultRelationIndex() const
	{
		return m_result_relation_index;
	}


	int *GetSubplanSliceIdArray();

	PlanSlice *GetSlices(int *numSlices_p);

	// add a range table entry
	void AddRTE(RangeTblEntry *rte, BOOL is_result_relation = false);

	void InsertUsedRTEIndexes(ULONG assigned_query_id_for_target_rel,
							  Index index);

	void AddSubplan(Plan *);

	// a subplan to be ready to rescan from the start, by its plan_id
	void AddRewindPlanId(int plan_id);

	Bitmapset *
	GetRewindPlanIds() const
	{
		return m_rewind_plan_ids;
	}

	// add a slice table entry
	int AddSlice(PlanSlice *);

	PlanSlice *
	GetCurrentSlice() const
	{
		return m_current_slice;
	}

	void
	SetCurrentSlice(PlanSlice *slice)
	{
		m_current_slice = slice;
	}

	// add CTAS information
	void AddCtasInfo(GpPolicy *distribution_policy);

	// CTAS distribution policy
	GpPolicy *
	GetDistributionPolicy() const
	{
		return m_distribution_policy;
	}

	// Get the hash opclass or hash function for given datatype,
	// based on decision made by DetermineDistributionHashOpclasses()
	Oid GetDistributionHashOpclassForType(Oid typid);
	Oid GetDistributionHashFuncForType(Oid typid);

	ULONG GetParamIdForSelector(OID oid_type, const ULONG selectorId);

	Index FindRTE(Oid reloid);

	// used by internal GPDB functions to build the RelOptInfo when creating foreign scans
	Query *m_orig_query;

	// get rte from m_rtable_entries_list by given index
	RangeTblEntry *GetRTEByIndex(Index index);

	Index GetRTEIndexByAssignedQueryId(ULONG assigned_query_id_for_target_rel,
									   BOOL *is_rte_exists);

	// add a permission info entry
	void AddPermInfo(RTEPermissionInfo *pi);

	// get perm info from m_perminfo_list by given index
	RTEPermissionInfo *GetPermInfoByIndex(Index index);

	// List of AggInfo and AggTransInfo
	inline List *GetAggInfos() const 
	{
		return m_agg_infos;
	}

	inline List *GetAggTransInfos() const 
	{
		return m_agg_trans_infos;
	}

	void AppendAggInfos(AggInfo *agginfo);
	void AppendAggTransInfos(AggTransInfo *transinfo);
	void ResetAggInfosAndTransInfos();
};

}  // namespace gpdxl
#endif	// !GPDXL_CContextDXLToPlStmt_H

//EOF
