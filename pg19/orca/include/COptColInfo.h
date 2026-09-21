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
//	Ported from github/cloudberry/src/include/gpopt/translate/COptColInfo.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Greenplum
//
//	@filename:
//		COptColInfo.h
//
//	@doc:
//		Class to uniquely identify a column in optimizer
//
//	@test:
//
//
//---------------------------------------------------------------------------

#ifndef GPDXL_COptColInfo_H
#define GPDXL_COptColInfo_H

#include "gpos/base.h"
#include "gpos/common/CRefCount.h"
#include "gpos/utils.h"

namespace gpdxl
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		COptColInfo
//
//	@doc:
//		pair of column id and column name
//
//---------------------------------------------------------------------------
class COptColInfo : public CRefCount
{
private:
	// column id
	ULONG m_colid;

	// column name
	CWStringBase *m_str;

public:
	COptColInfo(const COptColInfo &) = delete;

	// ctor
	COptColInfo(ULONG colid, CWStringBase *str) : m_colid(colid), m_str(str)
	{
		GPOS_ASSERT(m_str);
	}

	// dtor
	~COptColInfo() override
	{
		GPOS_DELETE(m_str);
	}

	// accessors
	ULONG
	GetColId() const
	{
		return m_colid;
	}

	CWStringBase *
	GetOptColName() const
	{
		return m_str;
	}

	// equality check
	BOOL
	Equals(const COptColInfo &optcolinfo) const
	{
		// don't need to check name as column id is unique
		return m_colid == optcolinfo.m_colid;
	}

	// hash value
	ULONG
	HashValue() const
	{
		return gpos::HashValue(&m_colid);
	}
};

// hash function
inline ULONG
UlHashOptColInfo(const COptColInfo *opt_col_info)
{
	GPOS_ASSERT(nullptr != opt_col_info);
	return opt_col_info->HashValue();
}

// equality function
inline BOOL
FEqualOptColInfo(const COptColInfo *opt_col_infoA,
				 const COptColInfo *opt_col_infoB)
{
	GPOS_ASSERT(nullptr != opt_col_infoA && nullptr != opt_col_infoB);
	return opt_col_infoA->Equals(*opt_col_infoB);
}

}  // namespace gpdxl

#endif	// !GPDXL_COptColInfo_H

// EOF
