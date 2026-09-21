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
//	Ported from github/cloudberry/src/include/gpopt/utils/CMemoryPoolPalloc.h,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2019 VMware, Inc. or its affiliates.
//
//	@filename:
//		CMemoryPoolPalloc.h
//
//	@doc:
//		CMemoryPool implementation that uses PostgreSQL memory
//		contexts.
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CMemoryPoolPalloc_H
#define GPDXL_CMemoryPoolPalloc_H

#include "gpos/base.h"
#include "gpos/memory/CMemoryPool.h"

namespace gpos
{
// Memory pool that maps to a Postgres MemoryContext.
class CMemoryPoolPalloc : public CMemoryPool
{
private:
	MemoryContext m_cxt{nullptr};

	// When destroying arrays, we need to call the destructor of each element
	// To do this, we need the size of the allocation, which we then divide by the
	// the size of the element to get number of elements to iterate through.
	// This struct is only used for array allocations (GPOS_NEW_ARRAY())
	struct SArrayAllocHeader
	{
		ULONG m_user_size;
	};

public:
	// ctor
	CMemoryPoolPalloc();

	// allocate memory
	void *NewImpl(const ULONG bytes, const CHAR *file, const ULONG line,
				  CMemoryPool::EAllocationType eat) override;

	// free memory
	static void DeleteImpl(void *ptr, CMemoryPool::EAllocationType eat);

	// prepare the memory pool to be deleted
	void TearDown() override;

	// return total allocated size include management overhead
	ULLONG TotalAllocatedSize() const override;

	// get user requested size of allocation
	static ULONG UserSizeOfAlloc(const void *ptr);
};
}  // namespace gpos

#endif	// !GPDXL_CMemoryPoolPalloc_H

// EOF
