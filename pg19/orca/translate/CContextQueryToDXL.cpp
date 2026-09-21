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
//	Ported from github/cloudberry/src/backend/gpopt/translate/CContextQueryToDXL.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//  Greenplum Database
//	Copyright (C) 2018-Present VMware, Inc. or its affiliates.
//
//	@filename:
//		CContextQueryToDXL.cpp
//
//	@doc:
//		Implementation of the methods used to hold information about
//		the whole query, when translate a query into DXL tree. All
//		translator methods allocate memory in the provided memory pool,
//		and the caller is responsible for freeing it
//
//---------------------------------------------------------------------------
extern "C" {
#include "postgres.h"
}

#include "CContextQueryToDXL.h"
#include "CTranslatorUtils.h"
#include "naucrates/dxl/CIdGenerator.h"

using namespace gpdxl;

CContextQueryToDXL::CContextQueryToDXL(CMemoryPool *mp)
	: m_mp(mp),
	  m_has_distributed_tables(false),
	  m_distribution_hashops(DistrHashOpsNotDeterminedYet)
{
	// map that stores gpdb att to optimizer col mapping
	m_colid_counter = GPOS_NEW(mp) CIdGenerator(GPDXL_COL_ID_START);
	m_queryid_counter = GPOS_NEW(mp) CIdGenerator(GPDXL_QUERY_ID_START);
	m_cte_id_counter = GPOS_NEW(mp) CIdGenerator(GPDXL_CTE_ID_START);
}

CContextQueryToDXL::~CContextQueryToDXL()
{
	GPOS_DELETE(m_queryid_counter);
	GPOS_DELETE(m_colid_counter);
	GPOS_DELETE(m_cte_id_counter);
}

ULONG
CContextQueryToDXL::GetNextQueryId()
{
	return m_queryid_counter->next_id();
}
