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
//	@filename:
//		RelationWrapper.cpp
//
//	@doc:
//		The two members of RelationWrapper that need CloseRelation, and so
//		cannot be written in the header.
//
//		Ported from
//		github/cloudberry/src/backend/gpopt/utils/RelationWrapper.cpp,
//		unchanged but for the include paths.
//
//		The destructor is `noexcept(false)` on purpose: closing a relation
//		can raise, and GP_WRAP turns that into a GPOS exception, which has to
//		be allowed to leave.  That is Cloudberry's decision and the port keeps
//		it; a destructor that swallowed the error would lose a lock release
//		failure silently.
//
//---------------------------------------------------------------------------

#include "RelationWrapper.h"

#include "gpdbwrappers.h"  // for CloseRelation

namespace gpdb
{
RelationWrapper::~RelationWrapper() noexcept(false)
{
	if (m_relation)
	{
		CloseRelation(m_relation);
	}
}

void
RelationWrapper::Close()
{
	if (m_relation)
	{
		CloseRelation(m_relation);
		m_relation = nullptr;
	}
}
}  // namespace gpdb
