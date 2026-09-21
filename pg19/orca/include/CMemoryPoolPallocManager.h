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
//	Ported from github/cloudberry/src/include/gpopt/utils/CMemoryPoolPallocManager.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2019 VMware, Inc. or its affiliates.
//
//	@filename:
//		CMemoryPoolPallocManager.h
//
//	@doc:
//		MemoryPoolManager implementation that creates
//		CMemoryPoolPalloc memory pools
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CMemoryPoolPallocManager_H
#define GPDXL_CMemoryPoolPallocManager_H

#include "gpos/base.h"
#include "gpos/memory/CMemoryPoolManager.h"

namespace gpos
{
// memory pool manager that uses GPDB memory contexts
class CMemoryPoolPallocManager : public CMemoryPoolManager
{
private:
public:
	CMemoryPoolPallocManager(const CMemoryPoolPallocManager &) = delete;

	// ctor
	CMemoryPoolPallocManager(CMemoryPool *internal,
							 EMemoryPoolType memory_pool_type);

	// allocate new memorypool
	CMemoryPool *NewMemoryPool() override;

	// free allocation
	void DeleteImpl(void *ptr, CMemoryPool::EAllocationType eat) override;

	// get user requested size of allocation
	ULONG UserSizeOfAlloc(const void *ptr) override;

	static void Init();
};
}  // namespace gpos

#endif	// !GPDXL_CMemoryPoolPallocManager_H

// EOF
