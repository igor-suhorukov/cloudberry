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
 * gp_metrics.h
 *	  Cloudberry's query metrics in shared memory: each plan node's
 *	  instrumentation in a slot any session of the node can read.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_METRICS_H
#define GP_METRICS_H

#include "executor/instrument.h"

/*
 * The bit of a statement's instrument_options that says it is instrumented
 * for the slots alone, which EXPLAIN ANALYZE's options never carry: what
 * shows a plan's never-run parts where EXPLAIN ANALYZE asks passes them over
 * (gp_motion.c).  PostgreSQL's instrumentation reads its own bits only.
 */
#define GP_INSTR_METRICS_ONLY	(1 << 30)

/*
 * A slot, as Cloudberry's InstrumentationSlot (executor/instrument.h): the
 * node's instrumentation, which its executor writes, and whose it is.
 */
typedef struct GpMetricsSlot
{
	NodeInstrumentation data;
	int32		pid;			/* the process's, 0 for a free slot */
	int32		tmid;			/* the node's start, Cloudberry's gp_gettmid() */
	int32		ssid;			/* the session's, gp_session_id */
	int32		ccnt;			/* the statement's, gp.command_count */
	int16		segid;			/* the node's content */
	int16		nid;			/* the plan node's plan_node_id */
	int32		next_free;		/* the next free slot, -1 for none */
} GpMetricsSlot;

/* Cloudberry's gp_enable_query_metrics */
extern bool gp_enable_query_metrics;

/*
 * The slots, for gp_instrument_shmem: how many there are, 0 where metrics
 * are off, and how many of them are free; and a copy of slot i, taken as it
 * stands, false where it is free.
 */
extern int	GpMetricsSlotCount(int *nfree);
extern bool GpMetricsSlotCopy(int i, GpMetricsSlot *copy);

extern void GpMetricsInit(void);

#endif							/* GP_METRICS_H */
