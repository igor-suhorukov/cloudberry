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
//		orca_api.cpp
//
//	@doc:
//		Bringing ORCA up and down inside a PostgreSQL backend.
//
//		Cloudberry does this in CGPOptimizer::InitGPOPT (see
//		github/cloudberry/src/backend/gpopt/CGPOptimizer.cpp).  The three
//		library inits are the same three; what differs is where the abort
//		callback comes from, because gpdb::IsAbortRequested lives in the
//		wrapper layer that the port has not ported yet, and because the port
//		initialises on demand rather than from _PG_init.
//
//---------------------------------------------------------------------------

extern "C"
{
#include "postgres.h"

#include "miscadmin.h"
}

#include "gpos/_api.h"
#include "gpos/error/CAutoExceptionStack.h"
#include "gpos/error/CException.h"
#include "gpos/common/CAutoP.h"
#include "gpos/common/CBitSet.h"
#include "gpos/common/CBitSetIter.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpopt/init.h"
#include "gpopt/xforms/CXform.h"
#include "gpopt/xforms/CXformFactory.h"
#include "naucrates/init.h"

#include "config/CConfigParamMapping.h"

#include "gp_orca_api.h"

//---------------------------------------------------------------------------
//	Is the backend being asked to give up?
//
//	ORCA's worker checks this between steps so that a long optimization
//	answers a cancel.  It must not itself raise: ORCA is C++ with its own
//	exceptions, and a longjmp out of it would leave its memory pools behind.
//	So this only reports, and ORCA unwinds its own way.
//---------------------------------------------------------------------------
static bool
GpOrcaAbortRequested(void)
{
	return QueryCancelPending || ProcDiePending;
}

//---------------------------------------------------------------------------
//	Per-backend state.
//
//	Not shared memory, and not inherited through fork: a backend that has
//	never planned with ORCA has none of this.
//---------------------------------------------------------------------------
static bool orca_initialized = false;

extern "C" void
GpOrcaEnsureInitialized(void)
{
	if (orca_initialized)
		return;

	struct gpos_init_params params = {GpOrcaAbortRequested};

	/*
	 * One init per library, in dependency order, as Cloudberry does:
	 * gpos_init builds the memory pool manager, gpdxl_init the DXL token
	 * table, gpopt_init the xform factory.  libgpdbcost has no init of its
	 * own; it is reached when ORCA builds a cost model.
	 */
	gpos_init(&params);
	gpdxl_init();
	gpopt_init();

	/*
	 * And a fourth, which is not in InitGPOPT: DXL support -- Xerces, the DXL
	 * token map and the parse-handler factory.  Cloudberry calls InitDXL()
	 * from COptTasks::Execute on every entry into ORCA, so nothing of its
	 * own ever meets DXL without it; the port's first user of DXL outside
	 * COptTasks was the metadata probe, which failed its first serialization
	 * on "Token map not initialized yet".  InitDXL() counts its calls and
	 * does the work once, so here is as good as there, and it covers every
	 * way into ORCA the module has, not only the optimizer's.
	 */
	InitDXL();

	orca_initialized = true;
}

extern "C" bool
GpOrcaIsInitialized(void)
{
	return orca_initialized;
}

extern "C" int
GpOrcaXformIdLimit(void)
{
	return (int) gpopt::CXform::ExfSentinel;
}

extern "C" int
GpOrcaXformCount(void)
{
	gpopt::CXformFactory *factory;
	int			live = 0;

	if (!orca_initialized)
		return 0;

	factory = gpopt::CXformFactory::Pxff();
	if (factory == NULL)
		return 0;

	for (int id = 0; id < (int) gpopt::CXform::ExfSentinel; id++)
	{
		if (factory->IsXformIdUsed((gpopt::CXform::EXformId) id))
			live++;
	}

	return live;
}

extern "C" const char *
GpOrcaXformName(int xform_id)
{
	if (!orca_initialized)
		return NULL;
	if (xform_id < 0 || xform_id >= (int) gpopt::CXform::ExfSentinel)
		return NULL;

	gpopt::CXformFactory *factory = gpopt::CXformFactory::Pxff();

	if (factory == NULL)
		return NULL;

	/*
	 * The id space has holes in it.  Twenty-four rules have been retired over
	 * ORCA's life, and their ids stay in the enum -- spelled
	 * "..._____removed" -- so that the ids of the rules around them do not
	 * shift, which would invalidate every stored minidump.  The factory has
	 * no entry for a retired id, and Pxf() dereferences what it finds before
	 * returning it, so asking for one takes the backend down on an
	 * assert-enabled build.  IsXformIdUsed() is the guard ORCA provides.
	 */
	if (!factory->IsXformIdUsed((gpopt::CXform::EXformId) xform_id))
		return NULL;

	gpopt::CXform *xform =
		factory->Pxf((gpopt::CXform::EXformId) xform_id);

	return xform == NULL ? NULL : xform->SzId();
}

//---------------------------------------------------------------------------
//	The trace flags the current settings ask for.
//
//	ORCA has no settings of its own: everything a person can turn on or off
//	in it is a bit in a set handed to the optimizer when a query is planned.
//	config/CConfigParamMapping.cpp is where the settings on the outside become
//	the bits on the inside, and this is how a caller in C can read the result
//	before there is an optimizer to hand it to.
//
//	IT RUNS INSIDE gpos_exec, AND HAS TO.  ORCA's memory pools, and the
//	assertions its debug build makes about them, are keyed to a CTask, and
//	there is no task in a plain backend: allocating from a CAutoMemoryPool
//	outside one takes the backend down rather than failing.  Cloudberry never
//	meets this because every entry into ORCA goes through COptTasks::Execute,
//	which is this same wrapper.  Anything else the port calls into ORCA from
//	C will need it too.
//
//	The array is palloc'd in the caller's context; the count is returned.
//---------------------------------------------------------------------------
struct GpOrcaTraceFlagsArg
{
	int		   *flags;
	int			count;
	MemoryContext caller;
};

static void *
GpOrcaTraceFlagsTask(void *ptr)
{
	GpOrcaTraceFlagsArg *arg = (GpOrcaTraceFlagsArg *) ptr;
	gpos::CAutoMemoryPool amp;
	gpos::CMemoryPool *mp = amp.Pmp();

	gpos::CBitSet *bitset = gpdxl::CConfigParamMapping::PackConfigParamInBitset(
		mp, (gpos::ULONG) gpopt::CXform::ExfSentinel, false /* create_vec_plan */);

	int			n = 0;

	{
		gpos::CBitSetIter iter(*bitset);

		while (iter.Advance())
			n++;
	}

	/*
	 * Into the caller's context, not ORCA's pool: the pool goes when this
	 * task ends, and the answer has to outlive it.
	 */
	arg->flags = (int *) MemoryContextAlloc(arg->caller,
											sizeof(int) * (n > 0 ? n : 1));
	arg->count = n;

	{
		gpos::CBitSetIter iter(*bitset);
		int			i = 0;

		while (iter.Advance())
			arg->flags[i++] = (int) iter.Bit();
	}

	bitset->Release();

	return nullptr;
}

extern "C" int
GpOrcaTraceFlags(int **flags)
{
	GpOrcaTraceFlagsArg arg;
	gpos_exec_params params;
	bool		abort_flag = false;
	int			rc = 0;
	ULONG		major = 0;
	ULONG		minor = 0;

	GpOrcaEnsureInitialized();

	arg.flags = NULL;
	arg.count = 0;
	arg.caller = CurrentMemoryContext;

	memset(&params, 0, sizeof(params));
	params.func = GpOrcaTraceFlagsTask;
	params.arg = &arg;
	params.stack_start = &params;
	params.abort_requested = &abort_flag;

	/*
	 * gpos_exec rethrows, so the catch has to be here.  A C++ exception that
	 * reaches a C frame is not an error PostgreSQL can report; it is
	 * std::terminate, which takes the whole server down and restarts it --
	 * which is exactly what happened when this was written without the
	 * catch.  Every entry into ORCA needs this pair, and Cloudberry has it in
	 * CGPOptimizer for the same reason.
	 */
	GPOS_TRY
	{
		rc = gpos_exec(&params);
	}
	GPOS_CATCH_EX(ex)
	{
		major = ex.Major();
		minor = ex.Minor();
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
		rc = -1;
	}
	GPOS_CATCH_END;

	if (rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the optimizer could not report its trace flags"),
				 rc < 0 ? errdetail("ORCA raised %u/%u.", major, minor) : 0));

	*flags = arg.flags;
	return arg.count;
}
