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
//	Ported from github/cloudberry/src/backend/gpopt/utils/CMemoryPoolPalloc.cpp,
//	and changed for PostgreSQL 19: past the include paths, a comment beside
//	each change, or beside what replaced it, says what and why.
//	Cloudberry's notice for the original follows, as the Apache License
//	requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2019 VMware, Inc. or its affiliates.
//
//	@filename:
//		CMemoryPoolPalloc.cpp
//
//	@doc:
//		CMemoryPool implementation that uses PostgreSQL memory
//		contexts.
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
}

#include "gpos/memory/CMemoryPool.h"

#include "gpdbwrappers.h"
#include "CMemoryPoolPalloc.h"

using namespace gpos;

// ctor
CMemoryPoolPalloc::CMemoryPoolPalloc()
{
	m_cxt = gpdb::GPDBAllocSetContextCreate();
}

void *
CMemoryPoolPalloc::NewImpl(const ULONG bytes, const CHAR *, const ULONG,
						   CMemoryPool::EAllocationType eat)
{
	// if it's a singleton allocation, allocate requested memory
	if (CMemoryPool::EatSingleton == eat)
	{
		return gpdb::GPDBMemoryContextAlloc(m_cxt, bytes);
	}
	// if it's an array allocation, allocate header + requested memory
	else
	{
		ULONG alloc_size = GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader) +
						   GPOS_MEM_ALIGNED_SIZE(bytes);

		void *ptr = gpdb::GPDBMemoryContextAlloc(m_cxt, alloc_size);

		SArrayAllocHeader *header = static_cast<SArrayAllocHeader *>(ptr);

		header->m_user_size = bytes;
		return static_cast<BYTE *>(ptr) +
			   GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
	}
}

void
CMemoryPoolPalloc::DeleteImpl(void *ptr, CMemoryPool::EAllocationType eat)
{
	if (CMemoryPool::EatSingleton == eat)
	{
		gpdb::GPDBFree(ptr);
	}
	else
	{
		void *header = static_cast<BYTE *>(ptr) -
					   GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
		gpdb::GPDBFree(header);
	}
}

// Prepare the memory pool to be deleted
void
CMemoryPoolPalloc::TearDown()
{
	gpdb::GPDBMemoryContextDelete(m_cxt);
}

// Total allocated size including management overheads
ULLONG
CMemoryPoolPalloc::TotalAllocatedSize() const
{
	// Cloudberry's MemoryContextGetCurrentSpace(), which PostgreSQL 19 does
	// not have, answers what this does: the bytes this context holds from
	// malloc, its own overhead and free space included, and not its
	// children's -- a pool's context has none.
	return MemoryContextMemAllocated(m_cxt, false);
}

// get user requested size of array allocation. Note: this is ONLY called for arrays
ULONG
CMemoryPoolPalloc::UserSizeOfAlloc(const void *ptr)
{
	GPOS_ASSERT(ptr != nullptr);
	void *void_header = static_cast<BYTE *>(const_cast<void *>(ptr)) -
						GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
	const SArrayAllocHeader *header =
		static_cast<SArrayAllocHeader *>(void_header);
	return header->m_user_size;
}


// EOF
