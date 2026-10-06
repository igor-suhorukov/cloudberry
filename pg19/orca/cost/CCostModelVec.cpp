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
//		CCostModelVec.cpp
//
//	@doc:
//		NOT IN CLOUDBERRY.  ORCA's cost model with a vectorized executor's
//		nodes priced in (pg_vector_executor.md §3.3.4).
//
//		A subclass of CCostModelGPDB, which ORCA's core keeps unmodified: it
//		is chosen for a statement a vectorized executor registered with
//		gp_orca's API takes (COptTasks::GetCostModel), and ORCA's search
//		then chooses its join orders, join methods, aggregation stages,
//		Motions and windows with the executor's nodes in their prices.
//
//		Each physical operator is priced as CCostModelGPDB prices it, and
//		then, where one of the executor's nodes would run it, as that node:
//
//		  * which operators: a table scan of a relation the executor scans,
//		    a filter or a projection over an operator that hands batches up,
//		    a hash join of any type but full and NOT IN's, a hashed or plain
//		    aggregation of any stage, a sort, and a hashed window -- as the
//		    executor's oracle accepts each one's expressions, asked by the
//		    PostgreSQL OIDs ORCA's metadata ids carry, the operator's own
//		    expressions alone, so that what an operator is to the executor
//		    is known the first time it is priced and kept for its parents;
//
//		  * the kernels' work: the same formulas, with the units of the
//		    work kernels do scaled by the executor's two factors -- per
//		    tuple and per byte by one, per column and per call by the other
//		    -- for the share of the operator's steps its kernels take, the
//		    rest at ORCA's own units (m_kernels, a CCostModelGPDB with the
//		    scaled parameters, blended with the row price by that share);
//
//		  * scans: a relation whose storage keeps its columns apart is
//		    charged the width of the columns the plan needs of it, where
//		    ORCA charges its every column; one read a row at a time pays
//		    for transposing its rows into batches;
//
//		  * a hashed window: its partitions hashed, each sorted by the
//		    window's order alone, under PostgreSQL's WindowAgg -- or, where
//		    the executor would not run it, its lowering, a WindowAgg over a
//		    Sort of all its input, which ORCA's own price leaves out (the
//		    translator's MakeWindowInputSort); and out of reach over input
//		    in an order, which the translator refuses;
//
//		  * crossings: wherever an operator that reads rows meets a child
//		    that hands batches up, or the other way, the conversion of the
//		    child's rows, charged by the parent, which knows both sides;
//
//		  * a node's setup, and below the executor's minimum of rows, no
//		    node of its at all, as its own planners decide.
//
//		Translation then builds the executor's nodes where this priced them
//		(gp_orca_vec.h, build_node), with nothing added to ORCA's DXL.
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "gp_orca_vec.h"
}

#include <cmath>

#include "gpos/base.h"

#include "gpdbcost/CCostModelParamsGPDB.h"
#include "gpopt/base/CColRef.h"
#include "gpopt/base/CCostContext.h"
#include "gpopt/base/CDistributionSpecHashed.h"
#include "gpopt/base/CDrvdPropPlan.h"
#include "gpopt/search/CGroup.h"
#include "gpopt/search/CGroupExpression.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/COrderSpec.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/metadata/CTableDescriptor.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CPhysicalAgg.h"
#include "gpopt/operators/CPhysicalHashJoin.h"
#include "gpopt/operators/CPhysicalHashSequenceProject.h"
#include "gpopt/operators/CPhysicalMotionGather.h"
#include "gpopt/operators/CPhysicalScan.h"
#include "gpopt/operators/CPhysicalSort.h"
#include "gpopt/operators/CScalarAggFunc.h"
#include "gpopt/operators/CScalarArrayCmp.h"
#include "gpopt/operators/CScalarCast.h"
#include "gpopt/operators/CScalarCmp.h"
#include "gpopt/operators/CScalarFunc.h"
#include "gpopt/operators/CScalarIdent.h"
#include "gpopt/operators/CScalarNullIf.h"
#include "gpopt/operators/CScalarOp.h"
#include "naucrates/md/CMDIdGPDB.h"
#include "naucrates/md/IMDType.h"
#include "naucrates/statistics/IStatistics.h"

#include "CCostModelVec.h"
#include "gpdbwrappers.h"

using namespace gpos;
using namespace gpdbcost;
using namespace gpopt;
using namespace gpmd;

// the least a factor may be: ORCA's formulas assert positive units
#define VEC_LEAST_FACTOR 1e-6

namespace
{
// an expression's steps, as the executor's oracle takes them
struct SSteps
{
	ULONG m_kernels;
	ULONG m_fallbacks;
	BOOL m_refused;
};

// a metadata id's PostgreSQL OID, InvalidOid where it carries none
Oid
OidOf(const IMDId *mdid)
{
	if (!IMDId::IsValid(mdid))
	{
		return InvalidOid;
	}
	const CMDIdGPDB *gpdb_mdid = CMDIdGPDB::CastMdid(mdid);
	return nullptr == gpdb_mdid ? InvalidOid : gpdb_mdid->Oid();
}

// a scalar expression's type, InvalidOid for one with none
Oid
TypeOf(CExpression *pexpr)
{
	COperator *pop = pexpr->Pop();
	if (!pop->FScalar())
	{
		return InvalidOid;
	}
	return OidOf(CScalar::PopConvert(pop)->MdidType());
}

// a type's equality operator, as ORCA's metadata has it
Oid
EqualityOf(const IMDId *type_mdid)
{
	if (!IMDId::IsValid(type_mdid))
	{
		return InvalidOid;
	}
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDType *type = md_accessor->RetrieveType(const_cast<IMDId *>(type_mdid));
	return OidOf(type->GetMdidForCmpType(IMDType::EcmptEq));
}

void Walk(CExpression *pexpr, SSteps *steps, BOOL in_agg);

// a call: the executor's kernel, its fallback, or none of its nodes
void
Call(Oid funcid, Oid opno, CExpression *pexpr, SSteps *steps)
{
	Oid argtypes[2] = {InvalidOid, InvalidOid};
	const ULONG nargs = std::min(pexpr->Arity(), (ULONG) 2);

	for (ULONG ul = 0; ul < nargs; ul++)
	{
		argtypes[ul] = TypeOf((*pexpr)[ul]);
	}
	switch (gpdb::VectorCostCall(funcid, opno, (int) nargs, argtypes,
								 InvalidOid))
	{
		case GP_ORCA_VEC_STEP_KERNEL:
			steps->m_kernels++;
			break;
		case GP_ORCA_VEC_STEP_FALLBACK:
			steps->m_fallbacks++;
			break;
		default:
			steps->m_refused = true;
			break;
	}
}

// an aggregate, in its aggregation's project list
void
Aggregate(CExpression *pexpr, SSteps *steps)
{
	CScalarAggFunc *agg = CScalarAggFunc::PopConvert(pexpr->Pop());
	Oid argtypes[2] = {InvalidOid, InvalidOid};
	int nargs = 0;
	BOOL ordered = false;

	if (EaggfuncIndexArgs < pexpr->Arity())
	{
		CExpression *args = (*pexpr)[EaggfuncIndexArgs];
		for (ULONG ul = 0; ul < args->Arity() && nargs < 2; ul++)
		{
			argtypes[nargs++] = TypeOf((*args)[ul]);
		}
		Walk(args, steps, false);
	}
	if (EaggfuncIndexOrder < pexpr->Arity())
	{
		ordered = 0 < (*pexpr)[EaggfuncIndexOrder]->Arity();
	}
	if (EaggfunckindNormal != agg->AggKind())
	{
		ordered = true;		// an ordered-set or hypothetical aggregate
	}
	switch (gpdb::VectorCostAggregate(OidOf(agg->MDId()), nargs, argtypes,
									  agg->IsDistinct(), ordered))
	{
		case GP_ORCA_VEC_STEP_KERNEL:
			steps->m_kernels++;
			break;
		case GP_ORCA_VEC_STEP_FALLBACK:
			steps->m_fallbacks++;
			break;
		default:
			steps->m_refused = true;
			break;
	}
}

// a scalar expression's steps: its calls, and what PostgreSQL's evaluator
// runs of it.  Columns, constants and logic are no steps of their own.
void
Walk(CExpression *pexpr, SSteps *steps, BOOL in_agg)
{
	GPOS_CHECK_STACK_SIZE;

	if (steps->m_refused || nullptr == pexpr)
	{
		return;
	}
	COperator *pop = pexpr->Pop();
	switch (pop->Eopid())
	{
		case COperator::EopScalarIdent:
		case COperator::EopScalarConst:
		case COperator::EopScalarProjectList:
		case COperator::EopScalarProjectElement:
		case COperator::EopScalarBoolOp:
		case COperator::EopScalarNullTest:
		case COperator::EopScalarBooleanTest:
		case COperator::EopScalarValuesList:
			break;
		case COperator::EopScalarCmp:
		case COperator::EopScalarIsDistinctFrom:
			// IS DISTINCT FROM is a comparison of its own operator id
			Call(InvalidOid, OidOf(dynamic_cast<CScalarCmp *>(pop)->MdIdOp()),
				 pexpr, steps);
			break;
		case COperator::EopScalarOp:
			Call(InvalidOid, OidOf(CScalarOp::PopConvert(pop)->MdIdOp()),
				 pexpr, steps);
			break;
		case COperator::EopScalarArrayCmp:
			Call(InvalidOid, OidOf(CScalarArrayCmp::PopConvert(pop)->MdIdOp()),
				 pexpr, steps);
			break;
		case COperator::EopScalarNullIf:
			Call(InvalidOid, OidOf(CScalarNullIf::PopConvert(pop)->MdIdOp()),
				 pexpr, steps);
			break;
		case COperator::EopScalarFunc:
			Call(OidOf(CScalarFunc::PopConvert(pop)->FuncMdId()), InvalidOid,
				 pexpr, steps);
			break;
		case COperator::EopScalarCast:
		{
			// a binary-coercible cast calls nothing
			Oid funcid = OidOf(CScalarCast::PopConvert(pop)->FuncMdId());
			if (InvalidOid != funcid)
			{
				Call(funcid, InvalidOid, pexpr, steps);
			}
			break;
		}
		case COperator::EopScalarAggFunc:
			// only in its aggregation's project list
			if (in_agg)
			{
				Aggregate(pexpr, steps);
			}
			else
			{
				steps->m_refused = true;
			}
			return;
		case COperator::EopScalarWindowFunc:
			// only in its window's project list, which the executor does
			// not walk: PostgreSQL's WindowAgg computes it
			steps->m_refused = true;
			return;
		default:
			// PostgreSQL's evaluator, a row at a time
			steps->m_fallbacks++;
			break;
	}
	for (ULONG ul = 0; ul < pexpr->Arity(); ul++)
	{
		Walk((*pexpr)[ul], steps, in_agg);
	}
}

// the kernels' share of an expression's steps
DOUBLE
KernelShare(const SSteps &steps)
{
	const ULONG all = steps.m_kernels + steps.m_fallbacks;
	return 0 == all ? 1.0 : (DOUBLE) steps.m_kernels / all;
}

// whether the executor hashes a key of this type
BOOL
HashesKey(const IMDId *type_mdid)
{
	Oid eqop = EqualityOf(type_mdid);
	return InvalidOid != eqop && gpdb::VectorCostHashKey(eqop, InvalidOid);
}

// whether the executor hashes keys of these expressions' types, and their
// steps
BOOL
HashesKeys(const CExpressionArray *keys, SSteps *steps)
{
	if (nullptr == keys)
	{
		return true;
	}
	for (ULONG ul = 0; ul < keys->Size(); ul++)
	{
		CExpression *key = (*keys)[ul];
		if (!key->Pop()->FScalar() ||
			!HashesKey(CScalar::PopConvert(key->Pop())->MdidType()))
		{
			return false;
		}
		Walk(key, steps, false);
	}
	return true;
}

// the rows of a child's group, as a whole: its group's statistics, which a
// partial plan's costing info leaves out
DOUBLE
ChildRows(CExpressionHandle &exprhdl, ULONG child_index)
{
	CGroupExpression *gexpr = exprhdl.Pgexpr();
	if (nullptr == gexpr || child_index >= gexpr->Arity())
	{
		return 0;
	}
	IStatistics *stats = (*gexpr)[child_index]->Pstats();
	return nullptr == stats ? 0 : stats->Rows().Get();
}
}  // namespace

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::CCostModelVec
//
//	@doc:
//		Ctor: the row model's parameters are ORCA's defaults until
//		COptTasks sets them, and the kernels' model is made from them after
//		(PrepareKernelParams)
//
//---------------------------------------------------------------------------
CCostModelVec::CCostModelVec(CMemoryPool *mp, ULONG ulSegments,
							 const SFactors &factors)
	: CCostModelGPDB(mp, ulSegments),
	  m_mp_vec(mp),
	  m_factors(factors),
	  m_kernels(nullptr)
{
	m_factors.m_tuple = std::max(m_factors.m_tuple, VEC_LEAST_FACTOR);
	m_factors.m_operator = std::max(m_factors.m_operator, VEC_LEAST_FACTOR);
	m_factors.m_convert = std::max(m_factors.m_convert, 0.0);
	m_factors.m_setup_rows = std::max(m_factors.m_setup_rows, 0.0);
	m_kernels = GPOS_NEW(mp) CCostModelGPDB(mp, ulSegments);
}

CCostModelVec::~CCostModelVec()
{
	m_kernels->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::PrepareKernelParams
//
//	@doc:
//		The kernels' model: the row model's parameters, as COptTasks set
//		them from ORCA's settings, with the units of the work a kernel does
//		scaled -- per column and per call by the operator factor, per tuple
//		and per byte by the tuple factor -- and the rest, reading, sorting,
//		materializing, sending, as they are
//
//---------------------------------------------------------------------------
void
CCostModelVec::PrepareKernelParams()
{
	ICostModelParams *row = GetCostModelParams();
	ICostModelParams *vec = m_kernels->GetCostModelParams();

	for (ULONG ul = 0; ul < CCostModelParamsGPDB::EcpSentinel; ul++)
	{
		DOUBLE factor = 1.0;

		switch (ul)
		{
			case CCostModelParamsGPDB::EcpFilterColCostUnit:
			case CCostModelParamsGPDB::EcpHashAggInputTupColumnCostUnit:
			case CCostModelParamsGPDB::EcpHashAggInputTupWidthCostUnit:
			case CCostModelParamsGPDB::EcpHJHashTableColumnCostUnit:
			case CCostModelParamsGPDB::EcpJoinFeedingTupColumnCostUnit:
			case CCostModelParamsGPDB::EcpHJFeedingTupColumnSpillingCostUnit:
			case CCostModelParamsGPDB::EcpScalarFuncCost:
				factor = m_factors.m_operator;
				break;
			case CCostModelParamsGPDB::EcpTupDefaultProcCostUnit:
			case CCostModelParamsGPDB::EcpOutputTupCostUnit:
			case CCostModelParamsGPDB::EcpHashAggOutputTupWidthCostUnit:
			case CCostModelParamsGPDB::EcpHJHashTableWidthCostUnit:
			case CCostModelParamsGPDB::EcpJoinFeedingTupWidthCostUnit:
			case CCostModelParamsGPDB::EcpHJHashingTupWidthCostUnit:
			case CCostModelParamsGPDB::EcpJoinOutputTupCostUnit:
			case CCostModelParamsGPDB::EcpHJFeedingTupWidthSpillingCostUnit:
			case CCostModelParamsGPDB::EcpHJHashingTupWidthSpillingCostUnit:
				factor = m_factors.m_tuple;
				break;
			default:
				break;
		}
		ICostModelParams::SCostParam *param = row->PcpLookup(ul);
		vec->SetParam(ul, param->Get() * factor,
					  param->GetLowerBoundVal() * factor,
					  param->GetUpperBoundVal() * factor);
	}
}

// a parameter of the row model
DOUBLE
CCostModelVec::Param(ULONG param) const
{
	return GetCostModelParams()->PcpLookup(param)->Get().Get();
}

// an operator priced before, or nullptr
const CCostModelVec::SOpInfo *
CCostModelVec::Known(const COperator *pop) const
{
	if (nullptr == pop)
	{
		return nullptr;
	}
	auto it = m_ops.find(pop);
	return it == m_ops.end() ? nullptr : &it->second;
}

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::Info
//
//	@doc:
//		The attached operator, as the executor takes it: from the operator
//		and its scalar children alone, and the rows of its input, which are
//		its group's whatever its plan; kept for its parents
//
//---------------------------------------------------------------------------
const CCostModelVec::SOpInfo &
CCostModelVec::Info(CExpressionHandle &exprhdl, const SCostingInfo *pci) const
{
	COperator *pop = exprhdl.Pop();
	auto it = m_ops.find(pop);
	if (it != m_ops.end())
	{
		return it->second;
	}

	SOpInfo info = {GP_ORCA_VEC_OTHER, false, 1.0, GP_ORCA_VEC_REL_NONE};
	SSteps steps = {0, 0, false};
	DOUBLE rows = 0;

	switch (pop->Eopid())
	{
		case COperator::EopPhysicalTableScan:
		{
			CPhysicalScan *scan = CPhysicalScan::PopConvert(pop);
			info.m_kind = GP_ORCA_VEC_SEQSCAN;
			info.m_rel = (ULONG) gpdb::VectorCostRelation(
				OidOf(scan->Ptabdesc()->MDId()));
			steps.m_refused = (GP_ORCA_VEC_REL_NONE == info.m_rel);
			rows = scan->PstatsBaseTable()->Rows().Get();
			break;
		}
		case COperator::EopPhysicalFilter:
		case COperator::EopPhysicalComputeScalar:
			info.m_kind = GP_ORCA_VEC_RESULT;
			Walk(exprhdl.PexprScalarRepChild(1), &steps, false);
			rows = ChildRows(exprhdl, 0);
			break;
		case COperator::EopPhysicalHashAgg:
		case COperator::EopPhysicalHashAggDeduplicate:
		{
			info.m_kind = GP_ORCA_VEC_AGG;
			const CColRefArray *cols =
				CPhysicalAgg::PopConvert(pop)->PdrgpcrGroupingCols();
			for (ULONG ul = 0; nullptr != cols && ul < cols->Size(); ul++)
			{
				if (!HashesKey((*cols)[ul]->RetrieveType()->MDId()))
				{
					steps.m_refused = true;
				}
			}
			Walk(exprhdl.PexprScalarRepChild(1), &steps, true);
			rows = ChildRows(exprhdl, 0);
			break;
		}
		case COperator::EopPhysicalScalarAgg:
			info.m_kind = GP_ORCA_VEC_AGG;
			Walk(exprhdl.PexprScalarRepChild(1), &steps, true);
			rows = ChildRows(exprhdl, 0);
			break;
		case COperator::EopPhysicalInnerHashJoin:
		case COperator::EopPhysicalLeftOuterHashJoin:
		case COperator::EopPhysicalLeftSemiHashJoin:
		case COperator::EopPhysicalLeftAntiSemiHashJoin:
		case COperator::EopPhysicalRightOuterHashJoin:
		{
			CPhysicalHashJoin *join = CPhysicalHashJoin::PopConvert(pop);
			info.m_kind = GP_ORCA_VEC_HASHJOIN;
			if (!HashesKeys(join->PdrgpexprOuterKeys(), &steps) ||
				!HashesKeys(join->PdrgpexprInnerKeys(), &steps))
			{
				steps.m_refused = true;
			}
			Walk(exprhdl.PexprScalarRepChild(2), &steps, false);
			rows = std::max(ChildRows(exprhdl, 0), ChildRows(exprhdl, 1));
			break;
		}
		case COperator::EopPhysicalSort:
			info.m_kind = GP_ORCA_VEC_SORT;
			rows = ChildRows(exprhdl, 0);
			break;
		case COperator::EopPhysicalHashSequenceProject:
		{
			// partitions to hash: a window with none is a sort of its
			// input, which the executor leaves to the lowering
			CPhysicalSequenceProject *window =
				CPhysicalHashSequenceProject::PopConvert(pop);
			info.m_kind = GP_ORCA_VEC_WINDOW;
			if (CDistributionSpec::EdtHashed != window->Pds()->Edt())
			{
				steps.m_refused = true;
			}
			else
			{
				CDistributionSpecHashed *pds =
					CDistributionSpecHashed::PdsConvert(window->Pds());
				SSteps keys = {0, 0, false};
				if (!HashesKeys(pds->Pdrgpexpr(), &keys))
				{
					steps.m_refused = true;
				}
			}
			rows = ChildRows(exprhdl, 0);
			break;
		}
		default:
			break;
	}

	if (GP_ORCA_VEC_OTHER != info.m_kind)
	{
		// a projection or filter runs wherever its child hands batches,
		// as the executor's ORCA front end builds it, of any size
		info.m_accepted =
			!steps.m_refused &&
			0 != (m_factors.m_kinds & (1U << info.m_kind)) &&
			(GP_ORCA_VEC_RESULT == info.m_kind || 0 >= m_factors.m_min_rows ||
			 rows >= m_factors.m_min_rows);
		info.m_kernel_share = KernelShare(steps);
	}
	return m_ops.emplace(pop, info).first->second;
}

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::HandsBatches, ContextHandsBatches
//
//	@doc:
//		Whether a child's plan hands batches up: a scan, a hash join or a
//		sort the executor runs, and a filter or a projection it runs, which
//		it runs only over a child that hands batches.  An aggregation and a
//		window hand rows.  The child's operator was priced before its
//		parent; a filter's child is found through the child's cost context.
//		Outside a cost context -- ORCA's partial plans, priced as a lower
//		bound -- a child's plan is not known, and none hands batches
//
//---------------------------------------------------------------------------
BOOL
CCostModelVec::HandsBatches(CExpressionHandle &exprhdl, ULONG child_index) const
{
	const SOpInfo *info = Known(exprhdl.Pop(child_index));
	if (nullptr == info || !info->m_accepted)
	{
		return false;
	}
	switch (info->m_kind)
	{
		case GP_ORCA_VEC_SEQSCAN:
		case GP_ORCA_VEC_HASHJOIN:
		case GP_ORCA_VEC_SORT:
			return true;
		case GP_ORCA_VEC_RESULT:
		{
			CCostContext *grandchild = nullptr;
			(void) exprhdl.PopGrandchild(child_index, 0, &grandchild);
			return nullptr != grandchild && ContextHandsBatches(grandchild);
		}
		default:
			return false;
	}
}

BOOL
CCostModelVec::ContextHandsBatches(CCostContext *pcc) const
{
	const SOpInfo *info = Known(pcc->Pgexpr()->Pop());
	if (nullptr == info || !info->m_accepted)
	{
		return false;
	}
	switch (info->m_kind)
	{
		case GP_ORCA_VEC_SEQSCAN:
		case GP_ORCA_VEC_HASHJOIN:
		case GP_ORCA_VEC_SORT:
			return true;
		case GP_ORCA_VEC_RESULT:
		{
			CExpressionHandle exprhdl(m_mp_vec);
			exprhdl.Attach(pcc);
			return HandsBatches(exprhdl, 0);
		}
		default:
			return false;
	}
}

namespace
{
// What the translator's OrcaMayDeriveOrder() makes of an operator: that it
// sets no order (0), takes one to be there (1), or passes up the order of
// its relational child *next (2).  Its walk is over DXL; these are the
// physical operators that DXL is translated from.  A Sequence, whose last
// child it reads, is taken to be in an order here: its arity is not known
// from its parent's handle, and a window over one is rare.
int
OrderOf(COperator *pop, ULONG *next)
{
	*next = 0;
	switch (pop->Eopid())
	{
		case COperator::EopPhysicalComputeScalar:
		case COperator::EopPhysicalFilter:
		case COperator::EopPhysicalAssert:
		case COperator::EopPhysicalSpool:
		case COperator::EopPhysicalPartitionSelector:
		case COperator::EopPhysicalSplit:
		case COperator::EopPhysicalCTEProducer:
		case COperator::EopPhysicalLimit:
		case COperator::EopPhysicalSequenceProject:
		case COperator::EopPhysicalHashSequenceProject:
		case COperator::EopPhysicalStreamAgg:
		case COperator::EopPhysicalStreamAggDeduplicate:
		case COperator::EopPhysicalInnerNLJoin:
		case COperator::EopPhysicalInnerIndexNLJoin:
		case COperator::EopPhysicalCorrelatedInnerNLJoin:
		case COperator::EopPhysicalLeftOuterNLJoin:
		case COperator::EopPhysicalLeftOuterIndexNLJoin:
		case COperator::EopPhysicalCorrelatedLeftOuterNLJoin:
		case COperator::EopPhysicalLeftSemiNLJoin:
		case COperator::EopPhysicalCorrelatedLeftSemiNLJoin:
		case COperator::EopPhysicalCorrelatedInLeftSemiNLJoin:
		case COperator::EopPhysicalLeftAntiSemiNLJoin:
		case COperator::EopPhysicalCorrelatedLeftAntiSemiNLJoin:
		case COperator::EopPhysicalLeftAntiSemiNLJoinNotIn:
		case COperator::EopPhysicalCorrelatedNotInLeftAntiSemiNLJoin:
		case COperator::EopPhysicalFullMergeJoin:
			return 2;
		case COperator::EopPhysicalMotionGather:
		{
			// the order it merges by, if it merges
			const COrderSpec *pos = CPhysicalMotionGather::PopConvert(pop)->Pos();
			return nullptr != pos && !pos->IsEmpty() ? 1 : 0;
		}
		case COperator::EopPhysicalTableScan:
		case COperator::EopPhysicalParallelTableScan:
		case COperator::EopPhysicalForeignScan:
		case COperator::EopPhysicalBitmapTableScan:
		case COperator::EopPhysicalDynamicTableScan:
		case COperator::EopPhysicalDynamicBitmapTableScan:
		case COperator::EopPhysicalDynamicForeignScan:
		case COperator::EopPhysicalConstTableGet:
		case COperator::EopPhysicalTVF:
		case COperator::EopPhysicalInnerHashJoin:
		case COperator::EopPhysicalLeftOuterHashJoin:
		case COperator::EopPhysicalLeftSemiHashJoin:
		case COperator::EopPhysicalLeftAntiSemiHashJoin:
		case COperator::EopPhysicalLeftAntiSemiHashJoinNotIn:
		case COperator::EopPhysicalRightOuterHashJoin:
		case COperator::EopPhysicalFullHashJoin:
		case COperator::EopPhysicalSerialUnionAll:
		case COperator::EopPhysicalParallelUnionAll:
		case COperator::EopPhysicalMotionBroadcast:
		case COperator::EopPhysicalMotionHashDistribute:
		case COperator::EopPhysicalMotionRoutedDistribute:
		case COperator::EopPhysicalMotionRandom:
		case COperator::EopPhysicalHashAgg:
		case COperator::EopPhysicalHashAggDeduplicate:
		case COperator::EopPhysicalScalarAgg:
			return 0;
		default:
			// a Sort, an index or index-only scan, a CTE consumer, a
			// Sequence: what the walk does not know, it takes to be ordered
			return 1;
	}
}
}  // namespace

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::TranslatorTakesOrdered, ContextTakesOrdered
//
//	@doc:
//		Whether the translator takes a child's rows to be in an order, as
//		its OrcaMayDeriveOrder() walks the plan's DXL: down the operators
//		that pass an order up, to the first that sets one or none.  A
//		hashed window over such rows it refuses, and the planner plans the
//		statement; so they are out of a hashed window's reach here.  Outside
//		a cost context a child's plan is not known, and none is ordered
//
//---------------------------------------------------------------------------
BOOL
CCostModelVec::TranslatorTakesOrdered(CExpressionHandle &exprhdl,
									  ULONG child_index) const
{
	COperator *pop = exprhdl.Pop(child_index);
	ULONG next;

	if (nullptr == pop)
	{
		return false;
	}
	switch (OrderOf(pop, &next))
	{
		case 0:
			return false;
		case 1:
			return true;
		default:
		{
			CCostContext *grandchild = nullptr;
			(void) exprhdl.PopGrandchild(child_index, next, &grandchild);
			return nullptr == grandchild || ContextTakesOrdered(grandchild);
		}
	}
}

BOOL
CCostModelVec::ContextTakesOrdered(CCostContext *pcc) const
{
	ULONG next;

	switch (OrderOf(pcc->Pgexpr()->Pop(), &next))
	{
		case 0:
			return false;
		case 1:
			return true;
		default:
		{
			CExpressionHandle exprhdl(m_mp_vec);
			exprhdl.Attach(pcc);
			return TranslatorTakesOrdered(exprhdl, next);
		}
	}
}

// whether the executor runs the attached operator in this plan
BOOL
CCostModelVec::Runs(CExpressionHandle &exprhdl, const SOpInfo &info) const
{
	if (!info.m_accepted)
	{
		return false;
	}
	if (GP_ORCA_VEC_RESULT == info.m_kind)
	{
		// a Result over a row child stays a row node: no node of the
		// executor's reads a row child a batch ahead.  In a partial plan
		// its child has no plan yet, and it may be either (Cost)
		return nullptr == exprhdl.Pop(0) || HandsBatches(exprhdl, 0);
	}
	return true;
}

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::CostHashedWindow
//
//	@doc:
//		ORCA's hashed window.  ORCA prices it as its sorted window without
//		the Sort below, which it does not ask for.  The executor hashes its
//		input's rows into their partitions and sorts each by the window's
//		order alone, under PostgreSQL's WindowAgg; without the executor the
//		translator lowers it to a WindowAgg over a Sort of all its input,
//		which ORCA's price leaves out.  Over input ORCA takes to be in an
//		order the translator refuses it, and the planner plans the
//		statement: out of reach, so that ORCA keeps its sorted window there
//
//---------------------------------------------------------------------------
CCost
CCostModelVec::CostHashedWindow(CExpressionHandle &exprhdl,
								const SCostingInfo *pci, const SOpInfo &info,
								CCost row) const
{
	if (nullptr != exprhdl.Pop(0) &&
		((nullptr != exprhdl.Pdpplan(0)->Pos() &&
		  !exprhdl.Pdpplan(0)->Pos()->IsEmpty()) ||
		 TranslatorTakesOrdered(exprhdl, 0)))
	{
		return CCost(GPOS_FP_ABS_MAX);
	}

	CPhysicalSequenceProject *window =
		CPhysicalHashSequenceProject::PopConvert(exprhdl.Pop());
	const DOUBLE rows = std::max(2.0, pci->PdRows()[0]);
	const DOUBLE width = pci->GetWidth()[0];
	const DOUBLE rebinds = pci->PdRebinds()[0];
	const DOUBLE sort_unit = Param(CCostModelParamsGPDB::EcpSortTupWidthCostUnit);

	ULONG order_cols = 0;
	COrderSpecArray *orders = window->Pdrgpos();
	for (ULONG ul = 0; nullptr != orders && ul < orders->Size(); ul++)
	{
		order_cols += (*orders)[ul]->UlSortColumns();
	}

	if (!info.m_accepted)
	{
		// the lowering: a Sort of all its input
		return CCost(row.Get() +
					 rebinds * rows * std::log2(rows) * width * sort_unit);
	}

	// its partitions, from the statistics of its keys, or a tenth of its
	// rows where there are none; in a partial plan, a lower bound with no
	// statistics of its child's, as many as its rows
	CDistributionSpecHashed *pds =
		CDistributionSpecHashed::PdsConvert(window->Pds());
	const CExpressionArray *keys = pds->Pdrgpexpr();
	const BOOL partial = (nullptr == exprhdl.Pop(0));
	DOUBLE partitions = partial ? rows : 1.0;
	BOOL known = partial || nullptr != pci->Pcstats(0);
	for (ULONG ul = 0; !partial && known && ul < keys->Size(); ul++)
	{
		CExpression *key = (*keys)[ul];
		if (COperator::EopScalarIdent != key->Pop()->Eopid())
		{
			known = false;
			break;
		}
		DOUBLE ndv = pci->Pcstats(0)
						 ->GetNDVs(CScalarIdent::PopConvert(key->Pop())->Pcr())
						 .Get();
		if (ndv < 1.0)
		{
			known = false;
			break;
		}
		partitions *= ndv;
	}
	if (!known)
	{
		partitions = rows / 10.0;
	}
	partitions = std::max(1.0, std::min(partitions, rows));

	// its keys hashed, a batch at a time, its rows kept, and each partition
	// sorted by the window's order
	DOUBLE local =
		rebinds *
		(rows * keys->Size() *
			 Param(CCostModelParamsGPDB::EcpHashAggInputTupColumnCostUnit) *
			 m_factors.m_operator +
		 rows * width *
			 Param(CCostModelParamsGPDB::EcpTupDefaultProcCostUnit));
	if (0 < order_cols)
	{
		const DOUBLE per_partition = std::max(2.0, rows / partitions);
		local += rebinds * rows * std::log2(per_partition) * width * sort_unit;
	}
	return CCost(row.Get() + local);
}

//---------------------------------------------------------------------------
//	@function:
//		CCostModelVec::Cost
//
//	@doc:
//		Main driver: ORCA's price, and where the executor runs the
//		operator, its price there; and the crossings with its children
//
//---------------------------------------------------------------------------
CCost
CCostModelVec::Cost(CExpressionHandle &exprhdl, const SCostingInfo *pci) const
{
	const CCost row = CCostModelGPDB::Cost(exprhdl, pci);
	const SOpInfo &info = Info(exprhdl, pci);
	const BOOL vector = Runs(exprhdl, info);
	const DOUBLE tup = Param(CCostModelParamsGPDB::EcpTupDefaultProcCostUnit);

	// the crossings with its children, where a child's plan hands batches
	// up and this reads rows, or the other way; a window keeps the rows it
	// reads, either way.  A partial plan's children have no plans yet: it
	// is ORCA's lower bound, by which it prunes the alternatives it has not
	// priced, and is priced below every plan it may have -- no crossing,
	// and the lesser of the operator's two prices
	DOUBLE crossings = 0;
	BOOL partial = false;
	const BOOL reads_batches = vector && GP_ORCA_VEC_WINDOW != info.m_kind;
	for (ULONG ul = 0; ul < pci->ChildCount(); ul++)
	{
		if (exprhdl.FScalarChild(ul))
		{
			continue;
		}
		if (nullptr == exprhdl.Pop(ul))
		{
			partial = true;
			continue;
		}
		if (vector && GP_ORCA_VEC_WINDOW == info.m_kind)
		{
			continue;
		}
		if (HandsBatches(exprhdl, ul) != reads_batches)
		{
			crossings += m_factors.m_convert * pci->PdRows()[ul] *
						 pci->GetWidth()[ul] * tup * pci->PdRebinds()[ul];
		}
	}

	if (GP_ORCA_VEC_WINDOW == info.m_kind)
	{
		CCost window = CostHashedWindow(exprhdl, pci, info, row);
		if (GPOS_FP_ABS_MAX <= window.Get() || !vector)
		{
			return CCost(window.Get() + crossings);
		}
		return CCost(window.Get() + crossings +
					 m_factors.m_setup_rows * pci->Width() * tup);
	}

	if (!vector)
	{
		return CCost(row.Get() + crossings);
	}

	DOUBLE price;
	if (GP_ORCA_VEC_SEQSCAN == info.m_kind)
	{
		const DOUBLE rows = pci->Rows();
		const DOUBLE rebinds = pci->NumRebinds();
		if (GP_ORCA_VEC_REL_COLUMNS == info.m_rel)
		{
			// the columns the plan needs, not the table's every one
			price = rebinds *
					(Param(CCostModelParamsGPDB::EcpInitScanFactor) +
					 rows * pci->Width() *
						 Param(CCostModelParamsGPDB::EcpTableScanCostUnit));
		}
		else
		{
			// its rows read as ORCA prices them, and transposed
			price = row.Get() +
					rebinds * m_factors.m_convert * rows * pci->Width() * tup;
		}
	}
	else
	{
		const DOUBLE share = info.m_kernel_share;
		price = share * m_kernels->Cost(exprhdl, pci).Get() +
				(1.0 - share) * row.Get();
	}
	price += m_factors.m_setup_rows * pci->Width() * tup;
	if (partial)
	{
		return CCost(std::min(row.Get(), price));
	}
	return CCost(price + crossings);
}

// EOF
