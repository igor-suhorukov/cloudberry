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
//	Ported from github/cloudberry/src/include/gpopt/translate/CMappingColIdVarPlStmt.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CMappingColIdVarPlStmt.h
//
//	@doc:
//		Class defining the functions that provide the mapping between Var, Param
//		and variables of Sub-query to CDXLNode during Query->DXL translation
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef GPDXL_CMappingColIdVarPlStmt_H
#define GPDXL_CMappingColIdVarPlStmt_H

#include "gpos/base.h"
#include "gpos/common/CDynamicPtrArray.h"
#include "gpos/common/CHashMap.h"

#include "CDXLTranslateContext.h"
#include "CMappingColIdVar.h"

//fwd decl
struct Var;
struct Plan;

namespace gpdxl
{
// fwd decl
class CDXLTranslateContextBaseTable;
class CContextDXLToPlStmt;

//---------------------------------------------------------------------------
//	@class:
//		CMappingColIdVarPlStmt
//
//	@doc:
//	Class defining functions that provide the mapping between Var, Param
//	and variables of Sub-query to CDXLNode during Query->DXL translation
//
//---------------------------------------------------------------------------
class CMappingColIdVarPlStmt : public CMappingColIdVar
{
private:
	const CDXLTranslateContextBaseTable *m_base_table_context;

	// the array of translator context (one for each child of the DXL operator)
	CDXLTranslationContextArray *m_child_contexts;

	CDXLTranslateContext *m_output_context;

	// translator context used to translate initplan and subplans associated
	// with a param node
	CContextDXLToPlStmt *m_dxl_to_plstmt_context;

public:
	CMappingColIdVarPlStmt(
		CMemoryPool *mp,
		const CDXLTranslateContextBaseTable *base_table_context,
		CDXLTranslationContextArray *child_contexts,
		CDXLTranslateContext *output_context,
		CContextDXLToPlStmt *dxl_to_plstmt_context);

	// translate DXL ScalarIdent node into GPDB Var node
	Var *VarFromDXLNodeScId(const CDXLScalarIdent *dxlop) override;

	// translate DXL ScalarIdent node into GPDB Param node
	Param *ParamFromDXLNodeScId(const CDXLScalarIdent *dxlop);

	// get the output translator context
	CDXLTranslateContext *GetOutputContext();

	// return the context of the DXL->PlStmt translation
	CContextDXLToPlStmt *GetDXLToPlStmtContext();
};
}  // namespace gpdxl

#endif	// GPDXL_CMappingColIdVarPlStmt_H

// EOF
