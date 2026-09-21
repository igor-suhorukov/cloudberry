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
//	Ported from github/cloudberry/src/include/gpopt/translate/CMappingElementColIdParamId.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CMappingElementColIdParamId.h
//
//	@doc:
//		Wrapper class providing functions for the mapping element between ColId
//		and ParamId during DXL->PlStmt translation
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef GPDXL_CMappingElementColIdParamId_H
#define GPDXL_CMappingElementColIdParamId_H

#include "gpos/base.h"

#include "naucrates/md/IMDId.h"

namespace gpdxl
{
using namespace gpos;
using namespace gpmd;
//---------------------------------------------------------------------------
//	@class:
//		CMappingElementColIdParamId
//
//	@doc:
//		Wrapper class providing functions for the mapping element between
//		ColId and ParamId during DXL->PlStmt translation
//
//---------------------------------------------------------------------------
class CMappingElementColIdParamId : public CRefCount
{
private:
	// column identifier that is used as the key
	ULONG m_colid;

	// param identifier
	ULONG m_paramid;

	// param type
	IMDId *m_mdid;

	INT m_type_modifier;

public:
	// ctors and dtor
	CMappingElementColIdParamId(ULONG colid, ULONG paramid, IMDId *mdid,
								INT type_modifier);

	~CMappingElementColIdParamId() override = default;

	// return the ColId
	ULONG
	GetColId() const
	{
		return m_colid;
	}

	// return the ParamId
	ULONG
	ParamId() const
	{
		return m_paramid;
	}

	// return the type
	IMDId *
	MdidType() const
	{
		return m_mdid;
	}

	INT
	TypeModifier() const
	{
		return m_type_modifier;
	}
};
}  // namespace gpdxl

#endif	// GPDXL_CMappingElementColIdParamId_H

// EOF
