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
//	Ported from github/cloudberry/src/include/gpopt/relcache/CMDProviderRelcache.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CMDProviderRelcache.h
//
//	@doc:
//		Relcache-based provider of metadata objects.
//
//	@test:
//
//
//---------------------------------------------------------------------------



#ifndef GPMD_CMDProviderRelcache_H
#define GPMD_CMDProviderRelcache_H

#include "gpos/base.h"
#include "gpos/string/CWStringBase.h"

#include "naucrates/md/CSystemId.h"
#include "naucrates/md/IMDId.h"
#include "naucrates/md/IMDProvider.h"

// fwd decl
namespace gpopt
{
class CMDAccessor;
}

namespace gpmd
{
using namespace gpos;

//---------------------------------------------------------------------------
//	@class:
//		CMDProviderRelcache
//
//	@doc:
//		Relcache-based provider of metadata objects.
//
//---------------------------------------------------------------------------
class CMDProviderRelcache : public IMDProvider
{
public:
	CMDProviderRelcache(const CMDProviderRelcache &) = delete;

	// ctor/dtor
	CMDProviderRelcache() = default;

	~CMDProviderRelcache() override = default;

	// returns the DXL string of the requested metadata object
	CWStringBase *GetMDObjDXLStr(CMemoryPool *mp, CMDAccessor *md_accessor,
								 IMDId *md_id) const override;

	// return the requested metadata object
	IMDCacheObject *GetMDObj(CMemoryPool *mp, CMDAccessor *md_accessor,
							 IMDId *mdid,
							 IMDCacheObject::Emdtype mdtype) const override;

	// return the mdid for the requested type
	IMDId *
	MDId(CMemoryPool *mp, CSystemId sysid,
		 IMDType::ETypeInfo type_info) const override
	{
		return GetGPDBTypeMdid(mp, sysid, type_info);
	}
};
}  // namespace gpmd



#endif	// !GPMD_CMDProviderRelcache_H

// EOF
