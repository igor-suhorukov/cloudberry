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
//	@filename:
//		CCostModelVec.h
//
//	@doc:
//		NOT IN CLOUDBERRY.  ORCA's cost model with a vectorized executor's
//		nodes priced in (pg_vector_executor.md §3.3.4).  See
//		cost/CCostModelVec.cpp.
//
//---------------------------------------------------------------------------
#ifndef GP_ORCA_CCostModelVec_H
#define GP_ORCA_CCostModelVec_H

#include <unordered_map>

#include "gpdbcost/CCostModelGPDB.h"

namespace gpopt
{
class CCostContext;
}

namespace gpdbcost
{
using namespace gpos;
using namespace gpopt;

//---------------------------------------------------------------------------
//	@class:
//		CCostModelVec
//
//	@doc:
//		CCostModelGPDB, and for each operator a vectorized executor's node
//		would run, its price there: the same formulas with the executor's
//		factors on the units of the work its kernels do, the columns a
//		columnar scan reads rather than its table's, a hashed window's
//		partitions hashed rather than sorted, and every crossing between
//		rows and batches charged where it happens.
//
//---------------------------------------------------------------------------
class CCostModelVec : public CCostModelGPDB
{
public:
	// the executor's prices (gp_orca_vec.h, GpOrcaVecCosts)
	struct SFactors
	{
		DOUBLE m_tuple;		  // the vector share of per-tuple, per-byte units
		DOUBLE m_operator;	  // of per-column, per-call units
		DOUBLE m_convert;	  // a crossing, per byte of a row, in tuple units
		DOUBLE m_setup_rows;  // a node's setup, in rows of its output
		DOUBLE m_min_rows;	  // fewer rows stay ORCA's; 0: none do
		ULONG m_kinds;		  // the node kinds it builds, 1 << GpOrcaVecKind
	};

private:
	// what an operator is to the executor, from the operator and its
	// scalar children alone, whatever plans its children have
	struct SOpInfo
	{
		ULONG m_kind;		   // GpOrcaVecKind: OTHER, none of its nodes
		BOOL m_accepted;	   // one of its nodes can run the operator
		DOUBLE m_kernel_share;	// of its expressions' steps, the kernels'
		ULONG m_rel;		   // a scan's: GP_ORCA_VEC_REL_*
	};

	// the memory pool
	CMemoryPool *m_mp_vec;

	// the executor's prices
	SFactors m_factors;

	// CCostModelGPDB with the units of the kernels' work scaled
	CCostModelGPDB *m_kernels;

	// each operator of the memo, as it was first priced: an operator is
	// priced after its children, so a parent finds its children here
	mutable std::unordered_map<const COperator *, SOpInfo> m_ops;

	// the operator attached to the handle, as the executor takes it
	const SOpInfo &Info(CExpressionHandle &exprhdl,
						const SCostingInfo *pci) const;

	// an operator priced before, or nullptr
	const SOpInfo *Known(const COperator *pop) const;

	// whether the executor runs the attached operator, in this plan
	BOOL Runs(CExpressionHandle &exprhdl, const SOpInfo &info) const;

	// whether the plan of a child of the attached operator hands batches up
	BOOL HandsBatches(CExpressionHandle &exprhdl, ULONG child_index) const;

	// whether the plan of a cost context hands batches up
	BOOL ContextHandsBatches(CCostContext *pcc) const;

	// whether the translator takes the rows of a child's plan, or of a cost
	// context's plan, to be in an order (its OrcaMayDeriveOrder())
	BOOL TranslatorTakesOrdered(CExpressionHandle &exprhdl,
								ULONG child_index) const;
	BOOL ContextTakesOrdered(CCostContext *pcc) const;

	// a parameter of the row model
	DOUBLE Param(ULONG param) const;

	// a hashed window, priced as the executor runs it or as its lowering
	CCost CostHashedWindow(CExpressionHandle &exprhdl, const SCostingInfo *pci,
						   const SOpInfo &info, CCost row) const;

public:
	CCostModelVec(const CCostModelVec &) = delete;

	// ctor
	CCostModelVec(CMemoryPool *mp, ULONG ulSegments, const SFactors &factors);

	// dtor
	~CCostModelVec() override;

	// the kernels' model, once the row model's parameters are set
	// (COptTasks::SetCostModelParams)
	void PrepareKernelParams();

	// main driver for cost computation
	CCost Cost(CExpressionHandle &exprhdl,
			   const SCostingInfo *pci) const override;

};	// class CCostModelVec

}  // namespace gpdbcost

#endif	// !GP_ORCA_CCostModelVec_H

// EOF
