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
//		CConfigParamMapping.h
//
//	@doc:
//		The server's settings as ORCA's trace flags.
//
//		ORCA has no settings of its own.  Everything a person can turn on or
//		off in it is a trace flag -- a bit in a set handed to the optimizer
//		when a query is planned -- and this is where the settings on the
//		outside become the bits on the inside.
//
//		Ported from
//		github/cloudberry/src/backend/gpopt/config/CConfigParamMapping.h.
//
//---------------------------------------------------------------------------
#ifndef GP_ORCA_CCONFIGPARAMMAPPING_H
#define GP_ORCA_CCONFIGPARAMMAPPING_H

#include "gpos/memory/CMemoryPool.h"
#include "gpos/common/CBitSet.h"

#include "gpopt/base/COptCtxt.h"
#include "gpopt/optimizer/COptimizerConfig.h"

namespace gpdxl
{
using namespace gpos;

class CConfigParamMapping
{
private:
	//	One setting, and the flag it turns on.
	struct SConfigMappingElem
	{
		// the trace flag this setting controls
		EOptTraceFlag m_trace_flag;

		// where the setting's value lives
		BOOL *m_is_param;

		// set the flag when the setting is *off*, rather than when it is on
		BOOL m_negate_param;

		// what it is for, in ORCA's own wide strings
		const WCHAR *description_str;
	};

	static SConfigMappingElem m_elements[];

public:
	CConfigParamMapping(const CConfigParamMapping &) = delete;

	// the flags the current settings ask for
	static CBitSet *PackConfigParamInBitset(CMemoryPool *mp, ULONG xform_id,
											BOOL create_vec_plan);
};
}  // namespace gpdxl

#endif	// GP_ORCA_CCONFIGPARAMMAPPING_H
