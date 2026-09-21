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
//		gp_unported.h
//
//	@doc:
//		How the translator says "not here yet".
//
//		One macro, used by the wrapper layer for the wrappers that wait
//		for a later milestone and by the translator for the code paths
//		that do: the distributed layer, which is M2's, and operators whose
//		group has not been built yet.  It raises instead of returning a
//		plausible-looking answer, and ORCA turns that into a fallback that
//		gp_orca_planner counts under the reason given here.  The
//		alternative -- returning nullptr, or 0, or an empty policy -- would
//		hand ORCA a false premise and get a plan built on it, which is the
//		one outcome the fallback design exists to prevent.
//
//		Not inside GP_WRAP: that is for turning a PostgreSQL longjmp into a
//		GPOS exception, and this is a GPOS exception already, raised
//		without entering PostgreSQL.  That is also why a caller may catch
//		it and carry on, which a caller of a GPOS exception that came from
//		a PostgreSQL error may not.
//
//		The text should say what is missing rather than where it is
//		missing from: it is what the fallback counters report.
//
//---------------------------------------------------------------------------
#ifndef GP_UNPORTED_H
#define GP_UNPORTED_H

#include "gpos/base.h"

#include "naucrates/exception.h"

#define GP_UNPORTED(what)                                              \
	GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, \
			   GPOS_WSZ_LIT(what))

#endif	// GP_UNPORTED_H

// EOF
