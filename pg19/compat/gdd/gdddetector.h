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
 * gdd/gdddetector.h
 *	  The include overlay: this reaches a header of Cloudberry's own.
 *
 * gp_core compiles Cloudberry's global deadlock detector's graph where it
 * lies -- src/backend/utils/gdd/gdddetector.c, the vertices, the edges,
 * their reduction and the choice of what to cancel -- and asks it through
 * this header (gp_gdd.c).  A vertex is named by Cloudberry's
 * DistributedTransactionId, a uint64 its c.h defines and PostgreSQL 19's
 * does not; the port names one by the coordinator's distributed
 * transaction number, of the same width, and the file is compiled with the
 * same definition (meson.build).  See task/cron.h for why the overlay
 * forwards rather than putting Cloudberry's directory on the include path.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_GDDDETECTOR_H
#define GP_COMPAT_GDDDETECTOR_H

#ifndef DistributedTransactionId
#define DistributedTransactionId uint64
#endif

#include "../../../src/backend/utils/gdd/gdddetector.h"

#endif							/* GP_COMPAT_GDDDETECTOR_H */
