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
//	Ported from github/cloudberry/src/include/gpopt/translate/CPartPruneStepsBuilder.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CPartPruneStepsBuilder.h
//
//	@doc:
//		Utility class to construct PartPruneInfos with appropriate
// 		PartPruningSteps from partitioning filter expressions
//---------------------------------------------------------------------------

#ifndef GPDXL_CPartPruneStepsBuilder_H
#define GPDXL_CPartPruneStepsBuilder_H

#include "gpos/base.h"

#include "CMappingColIdVarPlStmt.h"
#include "CTranslatorDXLToScalar.h"
#include "naucrates/dxl/operators/CDXLNode.h"

using namespace gpos;

namespace gpdxl
{
class CPartPruneStepsBuilder
{
private:
	// root partitioned tabled
	Relation m_relation;

	// index in the rtable
	Index m_rtindex;

	// list of pruned scan nodes denoted as an index of the relation's partition_mdids
	ULongPtrArray *m_part_indexes;

	// colid -> var mapping from the subtree
	CMappingColIdVarPlStmt *m_colid_var_mapping;

	// dxl -> scalar translator
	CTranslatorDXLToScalar *m_translator_dxl_to_scalar;

	// ctor
	CPartPruneStepsBuilder(Relation relation, Index rtindex,
						   ULongPtrArray *part_indexes,
						   CMappingColIdVarPlStmt *colid_var_mapping,
						   CTranslatorDXLToScalar *translator_dxl_to_scalar);

	CPartPruneStepsBuilder(const CPartPruneStepsBuilder &) = default;

public:
	// dtor
	~CPartPruneStepsBuilder() = default;

	static List *CreatePartPruneInfos(
		CDXLNode *filterNode, Relation relation, Index rtindex,
		ULongPtrArray *part_indexes, CMappingColIdVarPlStmt *colid_var_mapping,
		CTranslatorDXLToScalar *translator_dxl_to_scalar);

	PartitionedRelPruneInfo *CreatePartPruneInfoForOneLevel(
		CDXLNode *filterNode);

	List *PartPruneStepsFromFilter(CDXLNode *filterNode, INT *step_id,
								   List *steps_list);

	List *PartPruneStepFromScalarCmp(CDXLNode *node, INT *step_id,
									 List *steps_list);

	List *PartPruneStepFromScalarBoolExpr(CDXLNode *node, INT *step_id,
										  List *steps_list);
};
}  // namespace gpdxl

#endif	// !GPDXL_CPartPruneStepsBuilder_H

//EOF
