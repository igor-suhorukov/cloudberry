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
#include "gpopt/init.h"
#include "gpopt/xforms/CXform.h"
#include "gpopt/xforms/CXformFactory.h"
#include "naucrates/init.h"

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
