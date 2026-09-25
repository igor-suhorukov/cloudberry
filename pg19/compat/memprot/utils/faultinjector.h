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
 * memprot/utils/faultinjector.h
 *	  The include overlay of Cloudberry's memory protection: a fault of
 *	  Cloudberry's is gp_core's (gp_fault.h), by the same name.  See
 *	  cdb/cdbvars.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_MEMPROT_FAULTINJECTOR_H
#define GP_COMPAT_MEMPROT_FAULTINJECTOR_H

#include "gp_fault.h"

#define SIMPLE_FAULT_INJECTOR(name) ((void) GP_FAULT(name))

#endif							/* GP_COMPAT_MEMPROT_FAULTINJECTOR_H */
