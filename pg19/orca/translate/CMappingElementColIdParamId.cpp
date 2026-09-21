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
//	Ported from github/cloudberry/src/backend/gpopt/translate/CMappingElementColIdParamId.cpp,
//	unchanged but for the include paths.  Cloudberry's notice for
//	the original follows, as the Apache License requires it to.
//
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CMappingElementColIdParamId.cpp
//
//	@doc:
//		Implementation of the functions for the mapping element between ColId
//		and ParamId during DXL->PlStmt translation
//
//	@test:
//
//
//---------------------------------------------------------------------------

extern "C" {
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
}

#include "CMappingElementColIdParamId.h"

using namespace gpdxl;
using namespace gpos;

//---------------------------------------------------------------------------
//	@function:
//		CMappingElementColIdParamId::CMappingElementColIdParamId
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CMappingElementColIdParamId::CMappingElementColIdParamId(ULONG colid,
														 ULONG paramid,
														 IMDId *mdid,
														 INT type_modifier)
	: m_colid(colid),
	  m_paramid(paramid),
	  m_mdid(mdid),
	  m_type_modifier(type_modifier)
{
}

// EOF
