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
//		gpdbwrappers.cpp
//
//	@doc:
//		The gpdb:: wrapper layer: everything ORCA is allowed to ask the
//		server, in one place.
//
//		This is the seam.  ORCA's four core libraries include no PostgreSQL
//		header at all -- that is why the port compiles them from Cloudberry's
//		tree unmodified -- but they do call into this namespace, and this is
//		the file that answers.  So this file, and the translator beside it,
//		are where PostgreSQL 16 had to become PostgreSQL 19.
//
//		Ported from github/cloudberry/src/backend/gpopt/gpdbwrappers.cpp.
//		It fills up as the translator lands; what is here is what ORCA's core
//		needs to link.
//
//---------------------------------------------------------------------------

extern "C"
{
#include "postgres.h"
}

namespace gpdb
{
//---------------------------------------------------------------------------
//	@function:
//		gpdb::IsParallelModeOK
//
//	@doc:
//		May ORCA consider an intra-segment parallel plan?
//
//		Cloudberry answers from enable_parallel, single-node mode and
//		max_parallel_workers_per_gather (gpdbwrappers.cpp:2774).  The port
//		answers no: decision 2 defers intra-segment parallelism until after
//		M7, and says the first variant tried then is a PostgreSQL Gather
//		inside each segment process, which needs no answer here at all.
//
//		Cloudberry's parallel xforms are compiled in regardless -- they come
//		with the core, which the port takes whole -- so this is the switch
//		that keeps them from firing.  When the decision is revisited, this is
//		the one place that changes.
//
//---------------------------------------------------------------------------
bool
IsParallelModeOK(void)
{
	return false;
}
}							 // namespace gpdb
