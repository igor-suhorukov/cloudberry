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
//	Ported from github/cloudberry/src/backend/gpopt/relcache/CMDProviderRelcache.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CMDProviderRelcache.cpp
//
//	@doc:
//		Implementation of a relcache-based metadata provider, which uses GPDB's
//		relcache to lookup objects given their ids.
//
//	@test:
//
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"
}
#include "gpopt/mdcache/CMDAccessor.h"
#include "CMDProviderRelcache.h"
#include "CTranslatorRelcacheToDXL.h"
#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/exception.h"

using namespace gpos;
using namespace gpdxl;
using namespace gpmd;

CWStringBase *
CMDProviderRelcache::GetMDObjDXLStr(CMemoryPool *mp, CMDAccessor *md_accessor,
									IMDId *md_id) const
{
	// not used
	return nullptr;
}

// return the requested metadata object
IMDCacheObject *
CMDProviderRelcache::GetMDObj(CMemoryPool *mp, CMDAccessor *md_accessor,
							  IMDId *mdid, IMDCacheObject::Emdtype mdtype) const
{
	IMDCacheObject *md_obj =
		CTranslatorRelcacheToDXL::RetrieveObject(mp, md_accessor, mdid, mdtype);
	GPOS_ASSERT(nullptr != md_obj);

	return md_obj;
}

// EOF
