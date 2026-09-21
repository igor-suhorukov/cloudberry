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
//	Ported from github/cloudberry/src/backend/gpopt/CGPOptimizer.cpp,
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
//		CGPOptimizer.cpp
//
//	@doc:
//		Entry point to GP optimizer
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "CGPOptimizer.h"

#include "COptTasks.h"

#include "gpos/_api.h"
#include "gpos/error/CException.h"

#include "gpdbwrappers.h"
#include "naucrates/exception.h"

extern "C" {
#include "utils/elog.h"
#include "utils/memutils.h"
}

//---------------------------------------------------------------------------
//	@function:
//		CGPOptimizer::GPOPTOptimizedPlan
//
//	@doc:
//		Optimize given query using GP optimizer
//
//		CHANGED FOR THE PORT: NOTHING LEAVES THE CATCH BY longjmp.  Cloudberry
//		re-throws a PostgreSQL error with PG_RE_THROW() from inside the catch
//		block below, and raises "optimizer failed to init" and friends with
//		errstart(ERROR) there too.  A longjmp out of a C++ catch handler
//		skips __cxa_end_catch: the exception being handled is never freed,
//		and it stays on the runtime's list of caught exceptions, where the
//		next rethrow in this process can find it.  So the port hands both
//		back instead -- a PostgreSQL error as failure->from_postgres, which
//		GpOrcaOptimize's C caller re-throws once no C++ frame is left, and
//		ORCA's reason for making no plan as failure->message, which gp_orca's
//		planner hook counts and, under gp.optimizer_trace_fallback, reports.
//		The report is Cloudberry's INFO, word for word, with ORCA's reason as
//		its DETAIL; it moved, it did not change.
//
//---------------------------------------------------------------------------
PlannedStmt *
CGPOptimizer::GPOPTOptimizedPlan(Query *query, OptimizerOptions *opts,
								 GpOrcaFailure *failure)
{
	SOptContext gpopt_context;
	PlannedStmt *plStmt = nullptr;

	failure->unexpected = false;
	failure->from_postgres = false;
	failure->message = nullptr;

	GPOS_TRY
	{
		plStmt = COptTasks::GPOPTOptimizedPlan(query, &gpopt_context, opts);
		// clean up context
		gpopt_context.Free(gpopt_context.epinQuery, gpopt_context.epinPlStmt);
	}
	GPOS_CATCH_EX(ex)
	{
		// clone the error message before context free.
		//
		// Into the caller's context rather than Cloudberry's MessageContext:
		// the message is read by the caller, before it returns, and the
		// planner is not always called while a client message is being
		// processed.
		BOOL clone_failed = false;
		CHAR *serialized_error_msg =
			gpopt_context.CloneErrorMsg(CurrentMemoryContext, &clone_failed);
		// clean up context
		gpopt_context.Free(gpopt_context.epinQuery, gpopt_context.epinPlStmt);

		// Special handler for a few common user-facing errors. In particular,
		// we want to use the correct error code for these, in case an application
		// tries to do something smart with them.

		if (clone_failed || GPOS_MATCH_EX(ex, gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError))
		{
			// A PostgreSQL error, still on PostgreSQL's error stack: the
			// statement's error, not a reason to fall back.
			failure->from_postgres = true;
		}
		else
		{
			if (GPOS_MATCH_EX(ex, CException::ExmaInvalid,
							  CException::ExmiORCAInvalidState))
			{
				// A worker left registered by an earlier call that never
				// returned through gpos_exec.  Logged as Cloudberry logs it --
				// errstart(LOG) returns, so it may stay in the catch -- but
				// without Cloudberry's GPOS_RESET_EX after it, which expands
				// to ITask::Self()->GetErrCtxt()->Reset(): outside a task that
				// is a null dereference, and with a stale worker it is a write
				// through the task that worker last ran, which is gone.  The
				// C caller keeps ORCA from being asked again in this backend
				// instead; see optimize_query().
				if (errstart(LOG, TEXTDOMAIN))
				{
					errcode(ERRCODE_INTERNAL_ERROR);
					errmsg(
						"Worker is already registered! This is an invalid state, please report this error. ");
					errfinish(ex.Filename(), ex.Line(), nullptr);
				}
			}

			// Failed to produce a plan, but it wasn't an error that should
			// be propagated to the user.  The caller counts it and falls
			// back to the Postgres planner.
			failure->unexpected = gpopt_context.m_is_unexpected_failure ||
								  GPOS_MATCH_EX(ex, CException::ExmaInvalid,
												CException::ExmiORCAInvalidState);
			failure->message = serialized_error_msg;
		}
	}
	GPOS_CATCH_END;
	return plStmt;
}


//---------------------------------------------------------------------------
//	@function:
//		CGPOptimizer::SerializeDXLPlan
//
//	@doc:
//		Serialize planned statement into DXL
//
//		The error is raised after the catch rather than in it, for the reason
//		GPOPTOptimizedPlan gives.  Nothing calls this yet: it is what EXPLAIN's
//		DXL option will call, through an EXPLAIN option gp_orca registers.
//
//---------------------------------------------------------------------------
char *
CGPOptimizer::SerializeDXLPlan(Query *query)
{
	char *result = nullptr;
	bool failed = false;
	const CHAR *filename = nullptr;
	ULONG line = 0;

	GPOS_TRY
	{
		result = COptTasks::Optimize(query);
	}
	GPOS_CATCH_EX(ex)
	{
		failed = true;
		filename = ex.Filename();
		line = ex.Line();
	}
	GPOS_CATCH_END;

	if (failed)
	{
		if (errstart(ERROR, TEXTDOMAIN))
		{
			errcode(ERRCODE_INTERNAL_ERROR);
			errmsg("optimizer failed to produce plan");
			errfinish(filename, line, nullptr);
		}
	}

	return result;
}

//---------------------------------------------------------------------------
//	Not InitGPOPT() and TerminateGPOPT(), nor the four extern "C" functions
//	that expose them and the two above to Cloudberry's planner.c.  Bringing
//	ORCA up is GpOrcaEnsureInitialized()'s, in orca_api.cpp, which is where
//	Cloudberry's gp.optimizer_use_gpdb_allocators is read now; and what C may
//	call in ORCA is declared in gp_orca_api.h, and only there.
//---------------------------------------------------------------------------

extern "C" PlannedStmt *
GpOrcaOptimize(Query *query, OptimizerOptions *opts, GpOrcaFailure *failure)
{
	return CGPOptimizer::GPOPTOptimizedPlan(query, opts, failure);
}

// EOF
