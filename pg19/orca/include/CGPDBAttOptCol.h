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
//	Ported from github/cloudberry/src/include/gpopt/translate/CGPDBAttOptCol.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Greenplum
//
//	@filename:
//		CGPDBAttOptCol.h
//
//	@doc:
//		Class to represent pair of GPDB var info to optimizer col info
//
//	@test:
//
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CGPDBAttOptCol_H
#define GPDXL_CGPDBAttOptCol_H

#include "gpos/common/CRefCount.h"

#include "CGPDBAttInfo.h"
#include "COptColInfo.h"

namespace gpdxl
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CGPDBAttOptCol
//
//	@doc:
//		Class to represent pair of GPDB var info to optimizer col info
//
//---------------------------------------------------------------------------
class CGPDBAttOptCol : public CRefCount
{
private:
	// gpdb att info
	CGPDBAttInfo *m_gpdb_att_info;

	// optimizer col info
	COptColInfo *m_opt_col_info;

public:
	CGPDBAttOptCol(const CGPDBAttOptCol &) = delete;

	// ctor
	CGPDBAttOptCol(CGPDBAttInfo *gpdb_att_info, COptColInfo *opt_col_info)
		: m_gpdb_att_info(gpdb_att_info), m_opt_col_info(opt_col_info)
	{
		GPOS_ASSERT(nullptr != m_gpdb_att_info);
		GPOS_ASSERT(nullptr != m_opt_col_info);
	}

	// d'tor
	~CGPDBAttOptCol() override
	{
		m_gpdb_att_info->Release();
		m_opt_col_info->Release();
	}

	// accessor
	const CGPDBAttInfo *
	GetGPDBAttInfo() const
	{
		return m_gpdb_att_info;
	}

	// accessor
	const COptColInfo *
	GetOptColInfo() const
	{
		return m_opt_col_info;
	}
};

}  // namespace gpdxl

#endif	// !GPDXL_CGPDBAttOptCol_H

// EOF
