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
 * task/cron.h
 *	  The include overlay: this reaches a header of Cloudberry's own.
 *
 * gp_task compiles Cloudberry's cron expression parser where it lies --
 * src/backend/task/entry.c and misc.c, which are Paul Vixie's cron under his
 * own licence, with their own copyright headers.  Those files ask for
 * "task/cron.h", and the directory that holds it cannot go on the include
 * path: it also holds Cloudberry's copies of PostgreSQL 16 headers, which
 * would then be found instead of PostgreSQL 19's.
 *
 * So the overlay holds one forwarding header per Cloudberry header a module
 * needs, and nothing else.  That is the shape "Porting the Cloudberry code in
 * github/cloudberry" gives the overlay, at the size the port needs so far.
 *
 *-------------------------------------------------------------------------
 */
#include "../../../src/include/task/cron.h"
