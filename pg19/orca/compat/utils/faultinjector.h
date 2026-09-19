/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * compat/utils/faultinjector.h
 *	  Cloudberry's fault injector over PostgreSQL 19's injection points.
 *
 * ORCA asks for this header, so the port answers with one.  It reaches the
 * fault injector in two places and no more:
 *
 *	  SIMPLE_FAULT_INJECTOR("gpdbwrappers_get_comparison_operator")
 *	  gpdb::InjectFaultInOptTasks("opt_clone_error_msg"), whose caller in
 *	  COptTasks compares the answer with FaultInjectorTypeSkip
 *
 * What carries over and what does not.  PostgreSQL 19 has injection points,
 * which a test attaches a callback to; what the fault *does* is then the
 * callback's business.  Cloudberry instead sets a fault to a type and returns
 * that type to the caller, which decides.  So the types collapse here to the
 * one question PG19 can answer -- is anything attached to this point -- and
 * the callback does the rest.  The two sites above only ever ask that, so
 * nothing ORCA needs is lost.
 *
 * Injection points exist only in a build configured with them; without one
 * INJECTION_POINT compiles to nothing and nothing is ever attached, which is
 * the right answer for a production build.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_COMPAT_FAULTINJECTOR_H
#define GP_ORCA_COMPAT_FAULTINJECTOR_H

#include "utils/injection_point.h"

/*
 * Cloudberry has a dozen of these; ORCA compares against one of them.  The
 * rest are left out on purpose: a name that is not here is a site the port
 * has not looked at, and a compile error is the right way to find that out.
 */
typedef enum FaultInjectorType_e
{
	FaultInjectorTypeNotSpecified = 0,
	FaultInjectorTypeSkip,
} FaultInjectorType_e;

/* Cloudberry passes this where a DDL statement kind would go. */
#define DDLNotSpecified 0

/*
 * Run the point, then say whether a test had attached anything to it.
 *
 * The argument list is Cloudberry's, so that its call sites read unchanged;
 * the database and table names it takes are for faults scoped to one object,
 * which PG19 leaves to the callback, so they are ignored here.
 */
#define FaultInjector_InjectFaultIfSet(faultName, ddlStatement, databaseName, tableName) \
	(INJECTION_POINT((faultName), NULL), \
	 IS_INJECTION_POINT_ATTACHED(faultName) ? FaultInjectorTypeSkip \
	 : FaultInjectorTypeNotSpecified)

#ifndef SIMPLE_FAULT_INJECTOR
#define SIMPLE_FAULT_INJECTOR(name)		INJECTION_POINT((name), NULL)
#endif

#endif							/* GP_ORCA_COMPAT_FAULTINJECTOR_H */
