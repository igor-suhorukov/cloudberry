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
 * gp_fault.h
 *	  Cloudberry's fault injector, for the port's tests.
 *
 * A fault is set by name, on one node, with gp_inject_fault() (the
 * gp_inject_fault extension gp_core carries), and a place in the port's code
 * that may fail asks for it with GP_FAULT(name): see gp_fault.c.  The same
 * name set on a PostgreSQL 19 injection point fires there too.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_FAULT_H
#define GP_FAULT_H

#include "postgres.h"

/* What a fault does; Cloudberry's names, faultinjector_lists.h. */
typedef enum GpFaultType
{
	GP_FAULT_NONE = 0,
	GP_FAULT_SLEEP,
	GP_FAULT_FATAL,
	GP_FAULT_PANIC,
	GP_FAULT_ERROR,
	GP_FAULT_INFINITE_LOOP,
	GP_FAULT_SUSPEND,
	GP_FAULT_RESUME,
	GP_FAULT_SKIP,
	GP_FAULT_RESET,
	GP_FAULT_STATUS,
	GP_FAULT_SEGV,
	GP_FAULT_INTERRUPT,
	GP_FAULT_FINISH_PENDING,
	GP_FAULT_WAIT_UNTIL_TRIGGERED,
} GpFaultType;

/* Is any fault set on this node?  Read without a lock: a hint. */
extern PGDLLIMPORT volatile int *gp_fault_active;

/*
 * Fire the fault of that name if it is set here, and answer what it did:
 * GP_FAULT_NONE when nothing, GP_FAULT_SKIP where the caller is to skip what
 * it was about to do.  Errors, waits and sleeps happen inside.
 */
extern GpFaultType GpFaultTrigger(const char *name, const char *database,
								  const char *table);

#define GP_FAULT(name) \
	((gp_fault_active == NULL || *gp_fault_active == 0) ? GP_FAULT_NONE \
	 : GpFaultTrigger((name), "", ""))

/* Shared memory, from gp_core's _PG_init. */
extern void GpFaultInit(void);

#endif							/* GP_FAULT_H */
