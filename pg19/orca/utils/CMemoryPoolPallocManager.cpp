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
//	Ported from github/cloudberry/src/backend/gpopt/utils/CMemoryPoolPallocManager.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2019 VMware, Inc. or its affiliates.
//
//	@filename:
//		CMemoryPoolPallocManager.cpp
//
//	@doc:
//		MemoryPoolManager implementation that creates
//		CMemoryPoolPalloc memory pools
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
}

#include "CMemoryPoolPalloc.h"
#include "CMemoryPoolPallocManager.h"

using namespace gpos;

// ctor
CMemoryPoolPallocManager::CMemoryPoolPallocManager(CMemoryPool *internal,
												   EMemoryPoolType)
	: CMemoryPoolManager(internal, EMemoryPoolExternal)
{
}

// create new memory pool
CMemoryPool *
CMemoryPoolPallocManager::NewMemoryPool()
{
	return GPOS_NEW(GetInternalMemoryPool()) CMemoryPoolPalloc();
}

void
CMemoryPoolPallocManager::DeleteImpl(void *ptr,
									 CMemoryPool::EAllocationType eat)
{
	CMemoryPoolPalloc::DeleteImpl(ptr, eat);
}

// get user requested size of allocation
ULONG
CMemoryPoolPallocManager::UserSizeOfAlloc(const void *ptr)
{
	return CMemoryPoolPalloc::UserSizeOfAlloc(ptr);
}

void
CMemoryPoolPallocManager::Init()
{
	CMemoryPoolManager::SetupGlobalMemoryPoolManager<CMemoryPoolPallocManager,
													 CMemoryPoolPalloc>();
}

// EOF
