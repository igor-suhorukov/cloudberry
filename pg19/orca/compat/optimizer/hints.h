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
 * compat/optimizer/hints.h
 *	  Plan hints, as Cloudberry declares them.
 *
 * The include overlay, as pg19/compat/task/ is for the cron parser: Cloudberry
 * put this header in src/include/optimizer/, a directory that also holds its
 * copies of PostgreSQL 16's headers, so that directory cannot go on the
 * include path.  This file reaches the one header out of it that ORCA needs.
 *
 * It is a forwarding header rather than a copy because there is nothing to
 * port.  Cloudberry added the file whole -- PostgreSQL has no optimizer/hints.h
 * -- and it depends on nothing but postgres.h, nodes/pathnodes.h and
 * utils/guc.h, all of which PostgreSQL 19 has unchanged.  So it compiles as it
 * stands, and a merge from apache/cloudberry keeps working on it.
 *
 * What reads it: COptTasks, which passes a HintState to ORCA's hint xforms.
 * Those xforms are in ORCA's core, so they come with the core whether or not
 * anything produces hints; plan_hint_hook (compat/optimizer/orca.h) is what
 * would.  Nothing in the port sets that hook yet.
 *
 *-------------------------------------------------------------------------
 */
#include "../../../../src/include/optimizer/hints.h"
