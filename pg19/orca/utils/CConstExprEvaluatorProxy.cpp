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
//	Ported from github/cloudberry/src/backend/gpopt/utils/CConstExprEvaluatorProxy.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2014 VMware, Inc. or its affiliates.
//
//	@filename:
//		CConstExprEvaluatorProxy.cpp
//
//	@doc:
//		Wrapper over GPDB's expression evaluator that takes a constant expression,
//		given as DXL, evaluates it and returns the result as DXL. In case the expression
//		has variables, an exception is raised
//
//	@test:
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "executor/executor.h"
}

#include "gpdbwrappers.h"
#include "CTranslatorScalarToDXL.h"
#include "CConstExprEvaluatorProxy.h"
#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/exception.h"

using namespace gpdxl;
using namespace gpmd;
using namespace gpos;


//---------------------------------------------------------------------------
//	@function:
//		CConstExprEvaluatorProxy::EmptyMappingColIdVar::PvarFromDXLNodeScId
//
//	@doc:
//		Raises an exception in case someone looks up a variable
//
//---------------------------------------------------------------------------
Var *
CConstExprEvaluatorProxy::CEmptyMappingColIdVar::VarFromDXLNodeScId(
	const CDXLScalarIdent * /*scalar_ident*/
)
{
	elog(LOG,
		 "Expression passed to CConstExprEvaluatorProxy contains variables. "
		 "Evaluation will fail and an exception will be thrown.");
	GPOS_RAISE(gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError);
	return nullptr;
}

//---------------------------------------------------------------------------
//	@function:
//		CConstExprEvaluatorProxy::EvaluateExpr
//
//	@doc:
//		Evaluate 'expr', assumed to be a constant expression, and return the DXL representation
// 		of the result. Caller keeps ownership of 'expr' and takes ownership of the returned pointer.
//
//---------------------------------------------------------------------------
CDXLNode *
CConstExprEvaluatorProxy::EvaluateExpr(const CDXLNode *dxl_expr)
{
	// Translate DXL -> GPDB Expr
	Expr *expr = m_dxl2scalar_translator.TranslateDXLToScalar(
		dxl_expr, &m_emptymapcidvar);
	GPOS_ASSERT(nullptr != expr);

	// Evaluate the expression
	Expr *result = gpdb::EvaluateExpr(expr, gpdb::ExprType((Node *) expr),
									  gpdb::ExprTypeMod((Node *) expr));

	if (!IsA(result, Const))
	{
#ifdef GPOS_DEBUG
		elog(
			NOTICE,
			"Expression did not evaluate to Const, but to an expression of type %d",
			result->type);
#endif
		GPOS_RAISE(gpdxl::ExmaConstExprEval, gpdxl::ExmiConstExprEvalNonConst);
	}

	Const *const_result = (Const *) result;
	CDXLDatum *datum_dxl = CTranslatorScalarToDXL::TranslateConstToDXL(
		m_mp, m_md_accessor, const_result);
	CDXLNode *dxl_result = GPOS_NEW(m_mp)
		CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarConstValue(m_mp, datum_dxl));
	gpdb::GPDBFree(result);
	gpdb::GPDBFree(expr);

	return dxl_result;
}

// EOF
