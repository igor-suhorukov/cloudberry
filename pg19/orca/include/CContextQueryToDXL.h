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
//	Ported from github/cloudberry/src/include/gpopt/translate/CContextQueryToDXL.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CContextQueryToDXL.h
//
//	@doc:
//		Class to hold information about a whole top-level query, while
//		recursively translating a Query tree to DXL tree.
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CContextQueryToDXL_H
#define GPDXL_CContextQueryToDXL_H

#include "gpos/base.h"

#include "CTranslatorUtils.h"
#include "naucrates/dxl/CIdGenerator.h"
#include "naucrates/dxl/operators/CDXLNode.h"

#define GPDXL_CTE_ID_START 1
#define GPDXL_COL_ID_START 1
#define GPDXL_QUERY_ID_START 1

namespace gpdxl
{
// fwd declarations
class CTranslatorQueryToDXL;
class CTranslatorScalarToDXL;

//---------------------------------------------------------------------------
//	@class:
//		CContextQueryToDXL
//
//	@doc:
//		Class to hold information about a whole top-level query, while
//		recursively translating a Query tree to DXL tree.
//
//---------------------------------------------------------------------------
class CContextQueryToDXL
{
	friend class CTranslatorQueryToDXL;
	friend class CTranslatorScalarToDXL;

private:
	// memory pool
	CMemoryPool *m_mp;

	// counter for generating unique column ids
	CIdGenerator *m_colid_counter;

	// counter for generating unique CTE ids
	CIdGenerator *m_cte_id_counter;

	// counter for upper-level query and its subqueries
	CIdGenerator *m_queryid_counter;

	// does the query have any distributed tables?
	BOOL m_has_distributed_tables;

	// What operator classes are used in the distribution keys?
	DistributionHashOpsKind m_distribution_hashops;

public:
	// ctor
	CContextQueryToDXL(CMemoryPool *mp);

	// dtor
	~CContextQueryToDXL();

	ULONG GetNextQueryId();
};
}  // namespace gpdxl
#endif	// GPDXL_CContextQueryToDXL_H

//EOF
