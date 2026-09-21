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
//	Ported from github/cloudberry/src/include/gpopt/CGPOptimizer.h,
//	and changed for PostgreSQL 19: past the include paths, a comment beside
//	each change, or beside what replaced it, says what and why.
//	Cloudberry's notice for the original follows, as the Apache License
//	requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 Greenplum, Inc.
//
//	@filename:
//		CGPOptimizer.h
//
//	@doc:
//		Entry point to GP optimizer
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef CGPOptimizer_H
#define CGPOptimizer_H

extern "C" {
#include "postgres.h"

#include "optimizer/orcaopt.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

#include "gp_orca_api.h"
}

class CGPOptimizer
{
public:
	// optimize given query using GP optimizer
	//
	// Returns the plan, or nullptr and why in *failure.  Cloudberry's
	// signature has a bool for an unexpected failure in the failure's place,
	// and reports the rest itself; see the definition for why the port
	// hands both back to its caller.
	static PlannedStmt *GPOPTOptimizedPlan(Query *query, OptimizerOptions *opts,
										   GpOrcaFailure *failure);

	// serialize planned statement into DXL
	static char *SerializeDXLPlan(Query *query);

	// Not InitGPOPT() and TerminateGPOPT(): bringing ORCA up is
	// GpOrcaEnsureInitialized()'s, in orca_api.cpp, and nothing takes it
	// down (see gp_orca_api.h).
};

// Not the four extern "C" functions Cloudberry declares here.  What C may
// call in ORCA is declared in gp_orca_api.h, and only there; the optimizer's
// entry is GpOrcaOptimize().

#endif	// CGPOptimizer_H

// EOF
