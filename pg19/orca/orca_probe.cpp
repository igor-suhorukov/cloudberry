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
//		orca_probe.cpp
//
//	@doc:
//		Reaching the gpdb:: wrapper layer from C, so that SQL can test it.
//
//		The wrappers are C++ and the module is C, so without this nothing
//		could call one until the translator exists -- and the translator is
//		33k lines away.  That would leave the whole layer untested at the
//		moment it is most likely to be wrong: it has just been ported.
//
//		Each probe is the same three things: run inside gpos_exec, because
//		ORCA's pools are keyed to a CTask; catch, because gpos_exec rethrows
//		and a C++ exception reaching a C frame is std::terminate; and copy
//		the answer into the caller's context, because the pool goes when the
//		task ends.  See the note over GpOrcaTraceFlags in orca_api.cpp.
//
//		AND A FOURTH, WHICH BELONGS TO THE CALLER.  When *raised is set
//		because a PostgreSQL error was swallowed -- GP_WRAP catches it with
//		sigsetjmp and re-raises it as a GPOS exception -- PostgreSQL's own
//		error handling never ran.  The transaction is not aborted, the
//		resource owner still holds what the failed call took, and the error
//		stack still has an entry on it.  A caller that then returns a value
//		leaves the backend in that state, and the next thing it does takes
//		the server down.  So every caller of these raises a PostgreSQL error
//		when *raised is set, which is what hands the cleanup back to
//		PostgreSQL.  Cloudberry does the same, in CGPOptimizer, through
//		errstart(ERROR).
//
//		GpOrcaUnportedRaise is the one exception, and it is safe for a
//		reason worth naming: the exception it catches is a GPOS_RAISE from
//		GP_UNPORTED, thrown by C++ that never entered PostgreSQL, so there is
//		no half-finished PostgreSQL error behind it.
//
//		These probe what the *port* changed, not what Cloudberry wrote:
//		the operator OIDs the port had to name itself, the extended-statistics
//		call whose "not built" case PostgreSQL raises on and Cloudberry does
//		not, the two renamed access-method functions, the syscache callback
//		whose signature changed, the one wrapper that gained an answer with
//		the "gp" label, and the raise that stands in for a milestone that has
//		not arrived.
//
//---------------------------------------------------------------------------

extern "C"
{
#include "postgres.h"

#include "utils/memutils.h"
#include "utils/rel.h"

#include "gp_policy.h"
}

#include "gpos/_api.h"
#include "gpos/error/CException.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "naucrates/exception.h"

#include "gpdbwrappers.h"

#include "gp_orca_api.h"

// GPOS_TRY names CErrorHandler without qualification, so it only compiles
// with gpos in scope.  Cloudberry's files do the same thing.
using namespace gpos;

namespace
{
//---------------------------------------------------------------------------
//	What every probe returns: an answer, or the exception raised instead.
//
//	A probe cannot report a GPOS exception by raising one of its own -- it is
//	called from C -- so the two travel together and the caller decides.
//---------------------------------------------------------------------------
struct ProbeResult
{
	Oid			arg = InvalidOid;
	const char *name = nullptr;
	MemoryContext caller = nullptr;

	bool		raised = false;
	gpos::ULONG major = 0;
	gpos::ULONG minor = 0;

	bool		flag = false;
	int			number = 0;
	char	   *text = nullptr;
};

//	Copy into the caller's context; the pool dies with the task.
char *
CopyOut(ProbeResult *r, const char *s)
{
	if (s == nullptr)
	{
		return nullptr;
	}
	return MemoryContextStrdup(r->caller, s);
}

//---------------------------------------------------------------------------
//	Run one probe body as a GPOS task, and turn any exception into a flag.
//---------------------------------------------------------------------------
int
RunProbe(void *(*body)(void *), ProbeResult *r)
{
	gpos_exec_params params;
	bool		abort_flag = false;
	int			rc = 0;

	GpOrcaEnsureInitialized();

	memset(&params, 0, sizeof(params));
	params.func = body;
	params.arg = r;
	params.stack_start = &params;
	params.abort_requested = &abort_flag;

	GPOS_TRY
	{
		rc = gpos_exec(&params);
	}
	GPOS_CATCH_EX(ex)
	{
		r->raised = true;
		r->major = ex.Major();
		r->minor = ex.Minor();
		/*
		 * No GPOS_RESET_EX here, and this is the trap.  It expands to
		 * ITask::Self()->GetErrCtxt()->Reset(), and ITask::Self() is null
		 * outside a task -- which is exactly where this catch runs, because
		 * gpos_exec has already unwound the task by the time it rethrows.
		 * Calling it segfaults the backend, in a release build with no
		 * assertion to say why.
		 *
		 * There is nothing to reset in any case: the error context belonged
		 * to the task and went with it.  Cloudberry's two catches around
		 * gpos_exec, in COptTasks::Execute and CGPOptimizer, do not reset
		 * either.
		 */
		rc = 0;					// reported through r->raised, not as failure
	}
	GPOS_CATCH_END;

	return rc;
}
}  // namespace

//---------------------------------------------------------------------------
//	Is this operator NDV-preserving?
//
//	The interesting part is not the answer but the eleven OIDs the switch is
//	written over: PostgreSQL names none of them, so the port names them in
//	compat/cb_operator_oids.h.  A wrong OID there would compile and would
//	tell ORCA that a lossy operator preserves distinct values.
//---------------------------------------------------------------------------
static void *
ProbeNDVPreserving(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::IsOpNDVPreserving(r->arg);
	return nullptr;
}

extern "C" bool
GpOrcaOpNDVPreserving(Oid opno, bool *raised)
{
	ProbeResult r;

	r.arg = opno;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeNDVPreserving, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	The access method a relation is stored with, and whether an index access
//	method handler resolves.
//
//	Cloudberry calls GetAmName() and GetIndexAmRoutine(); PostgreSQL 19 spells
//	the first get_am_name() and returns the second as const.
//---------------------------------------------------------------------------
static void *
ProbeRelAmName(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->text = CopyOut(r, gpdb::GetRelAmName(r->arg));
	return nullptr;
}

extern "C" char *
GpOrcaRelAmName(Oid reloid, bool *raised)
{
	ProbeResult r;

	r.arg = reloid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeRelAmName, &r);
	*raised = r.raised;
	return r.text;
}

static void *
ProbeIndexAmRoutine(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::GetIndexAmRoutineFromAmHandler(r->arg) != nullptr;
	return nullptr;
}

extern "C" bool
GpOrcaIndexAmRoutineExists(Oid am_handler, bool *raised)
{
	ProbeResult r;

	r.arg = am_handler;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeIndexAmRoutine, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	Does this extended statistics object have functional dependencies?
//
//	1 when it has, 0 when it has none, and `raised` when asking was an error.
//
//	This is the one that would have been wrong.  Cloudberry passes
//	allow_null=true to a three-argument statext_dependencies_load();
//	PostgreSQL's takes two and raises when the dependencies are not built.
//	ORCA asks this of every statistics object it meets, so "not built" is the
//	common case, and a port that dropped the third argument would turn every
//	ordinary statistics object into a failed plan.
//---------------------------------------------------------------------------
static void *
ProbeMVDependencies(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->number = gpdb::GetMVDependencies(r->arg) != nullptr ? 1 : 0;
	return nullptr;
}

extern "C" int
GpOrcaMVDependencyState(Oid stat_oid, bool *raised)
{
	ProbeResult r;

	r.arg = stat_oid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbeMVDependencies, &r);
	*raised = r.raised;
	return r.number;
}

//---------------------------------------------------------------------------
//	Has the catalog changed since this backend last asked?
//
//	Registers the invalidation callbacks on first call.  PostgreSQL 19 types
//	the callback's cache id as SysCacheIdentifier rather than int, which is
//	the same argument in C and a different one in C++, so this is where a
//	port that only fixed the compile error would show it.
//---------------------------------------------------------------------------
static void *
ProbeMDCacheNeedsReset(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->flag = gpdb::MDCacheNeedsReset();
	return nullptr;
}

extern "C" bool
GpOrcaMDCacheNeedsReset(bool *raised)
{
	ProbeResult r;

	r.caller = CurrentMemoryContext;
	RunProbe(ProbeMDCacheNeedsReset, &r);
	*raised = r.raised;
	return r.flag;
}

//---------------------------------------------------------------------------
//	A wrapper that belongs to a later milestone.
//
//	Returns the ORCA exception it raised, as "major/minor", so that a test can
//	see the fallback path works rather than trusting that it does.  Which
//	wrapper is chosen does not matter much; this one is reached for every
//	distributed table, so it is the one M2 will fill in first.
//---------------------------------------------------------------------------
static void *
ProbeUnported(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;

	r->number = gpdb::CdbHashRandomSeg(2);
	return nullptr;
}

extern "C" char *
GpOrcaUnportedRaise(void)
{
	ProbeResult r;
	char		buf[64];

	r.caller = CurrentMemoryContext;
	RunProbe(ProbeUnported, &r);

	if (!r.raised)
	{
		return nullptr;
	}

	snprintf(buf, sizeof(buf), "%u/%u", (unsigned) r.major, (unsigned) r.minor);
	return MemoryContextStrdup(r.caller, buf);
}

//---------------------------------------------------------------------------
//	What distribution policy does ORCA see for this relation?
//
//	"entry", "replicated", "partitioned" or "random", or nothing when the
//	relation has no policy.  The wrapper is the port's own -- Cloudberry reads
//	gp_distribution_policy and the port reads the "gp" security label -- and
//	it is asked of every relation the relcache translator meets, which is why
//	it had to work at M1 rather than M2.
//
//	A relation with no policy is ordinary here and impossible in Cloudberry,
//	whose DDL gives every table one.
//---------------------------------------------------------------------------
static void *
ProbePolicyKind(void *ptr)
{
	ProbeResult *r = (ProbeResult *) ptr;
	Relation	rel = gpdb::GetRelation(r->arg).get();
	GpPolicy   *policy;

	if (rel == nullptr)
	{
		return nullptr;
	}

	policy = gpdb::GetDistributionPolicy(rel);
	if (policy == nullptr)
	{
		r->text = nullptr;
	}
	else if (GpPolicyIsEntry(policy))
	{
		r->text = CopyOut(r, "entry");
	}
	else if (GpPolicyIsReplicated(policy))
	{
		r->text = CopyOut(r, "replicated");
	}
	else if (GpPolicyIsHashPartitioned(policy))
	{
		r->text = CopyOut(r, "partitioned");
	}
	else if (GpPolicyIsRandomPartitioned(policy))
	{
		r->text = CopyOut(r, "random");
	}
	else
	{
		r->text = CopyOut(r, "unknown");
	}

	gpdb::CloseRelation(rel);
	return nullptr;
}

extern "C" char *
GpOrcaPolicyKind(Oid relid, bool *raised)
{
	ProbeResult r;

	r.arg = relid;
	r.caller = CurrentMemoryContext;
	RunProbe(ProbePolicyKind, &r);
	*raised = r.raised;
	return r.text;
}
