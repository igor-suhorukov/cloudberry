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
 * task_vixie.c
 *	  The globals Cloudberry's cron parser reads, and nothing else.
 *
 * task/cron.h follows the tradition it names: exactly one file of a program
 * defines MAIN_PROGRAM, and the header then defines the variables there
 * instead of declaring them.  entry.c and misc.c, which is where the parser
 * lives, are the ones that read them -- the month and day names a schedule may
 * spell, and the line counter its errors are reported against.  This file is
 * that one file.
 *
 * They are hidden because of what they are called.  PostgreSQL opens every
 * library with RTLD_GLOBAL, so an exported "copyright" or "ProgramName" would
 * go into a namespace every other library in the backend shares, and the one
 * that happened to be loaded first would win.  Hidden visibility keeps them
 * reachable from the two files that need them and out of everybody else's way.
 * It is the same reasoning as rule 8 of "Core patches that keep vanilla
 * behaviour", applied between libraries rather than to the core.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef __GNUC__
#pragma GCC visibility push(hidden)
#endif

#define MAIN_PROGRAM
#include "task/cron.h"

#ifdef __GNUC__
#pragma GCC visibility pop
#endif
