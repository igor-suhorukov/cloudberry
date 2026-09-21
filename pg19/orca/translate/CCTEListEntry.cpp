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
//	Ported from github/cloudberry/src/backend/gpopt/translate/CCTEListEntry.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CCTEListEntry.cpp
//
//	@doc:
//		Implementation of the class representing the list of common table
//		expression defined at a query level
//
//	@test:
//
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "nodes/parsenodes.h"
}
#include "gpos/base.h"

#include "gpdbwrappers.h"
#include "CCTEListEntry.h"
using namespace gpdxl;

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::CCTEListEntry
//
//	@doc:
//		Ctor: single CTE
//
//---------------------------------------------------------------------------
CCTEListEntry::CCTEListEntry(CMemoryPool *mp, ULONG query_level,
							 CommonTableExpr *cte, CDXLNode *cte_producer)
	: m_query_level(query_level), m_cte_info(nullptr)
{
	GPOS_ASSERT(nullptr != cte && nullptr != cte_producer);

	m_cte_info = GPOS_NEW(mp) HMSzCTEInfo(mp);
	Query *cte_query = (Query *) cte->ctequery;

	BOOL result GPOS_ASSERTS_ONLY = m_cte_info->Insert(
		cte->ctename,
		GPOS_NEW(mp) SCTEProducerInfo(cte_producer, cte_query->targetList));

	GPOS_ASSERT(result);
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::CCTEListEntry
//
//	@doc:
//		Ctor: multiple CTEs
//
//---------------------------------------------------------------------------
CCTEListEntry::CCTEListEntry(CMemoryPool *mp, ULONG query_level, List *cte_list,
							 CDXLNodeArray *cte_dxl_arr)
	: m_query_level(query_level), m_cte_info(nullptr)
{
	GPOS_ASSERT(nullptr != cte_dxl_arr);
	GPOS_ASSERT(cte_dxl_arr->Size() == gpdb::ListLength(cte_list));

	m_cte_info = GPOS_NEW(mp) HMSzCTEInfo(mp);
	const ULONG num_cte = cte_dxl_arr->Size();

	for (ULONG ul = 0; ul < num_cte; ul++)
	{
		CDXLNode *cte_producer = (*cte_dxl_arr)[ul];
		CommonTableExpr *cte = (CommonTableExpr *) gpdb::ListNth(cte_list, ul);

		Query *cte_query = (Query *) cte->ctequery;

		BOOL result GPOS_ASSERTS_ONLY = m_cte_info->Insert(
			cte->ctename,
			GPOS_NEW(mp) SCTEProducerInfo(cte_producer, cte_query->targetList));

		GPOS_ASSERT(result);
		GPOS_ASSERT(nullptr != m_cte_info->Find(cte->ctename));
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::GetCTEProducer
//
//	@doc:
//		Return the query of the CTE referenced in the range table entry
//
//---------------------------------------------------------------------------
const CDXLNode *
CCTEListEntry::GetCTEProducer(const CHAR *cte_str) const
{
	SCTEProducerInfo *cte_info = m_cte_info->Find(cte_str);
	if (nullptr == cte_info)
	{
		return nullptr;
	}

	return cte_info->m_cte_producer;
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::GetCTEProducerTargetList
//
//	@doc:
//		Return the target list of the CTE referenced in the range table entry
//
//---------------------------------------------------------------------------
List *
CCTEListEntry::GetCTEProducerTargetList(const CHAR *cte_str) const
{
	SCTEProducerInfo *cte_info = m_cte_info->Find(cte_str);
	if (nullptr == cte_info)
	{
		return nullptr;
	}

	return cte_info->m_target_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::AddCTEProducer
//
//	@doc:
//		Add a new CTE producer to this query level
//
//---------------------------------------------------------------------------
void
CCTEListEntry::AddCTEProducer(CMemoryPool *mp, CommonTableExpr *cte,
							  const CDXLNode *cte_producer)
{
	GPOS_ASSERT(nullptr == m_cte_info->Find(cte->ctename) &&
				"CTE entry already exists");
	Query *cte_query = (Query *) cte->ctequery;

	BOOL result GPOS_ASSERTS_ONLY = m_cte_info->Insert(
		cte->ctename,
		GPOS_NEW(mp) SCTEProducerInfo(cte_producer, cte_query->targetList));

	GPOS_ASSERT(result);
}

// EOF
