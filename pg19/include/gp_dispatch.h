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
 * gp_dispatch.h
 *	  Reaching the segments.
 *
 * Cloudberry's dispatcher opens a libpq connection per segment, marks it as
 * internal with high bits in the protocol version, and sends plans in messages
 * of its own ('M' and 'T').  PostgreSQL 19 rejects both -- a major version
 * above 3 and an unknown message type end the session -- so the port's
 * dispatcher speaks ordinary libpq to an ordinary backend, and what makes that
 * backend a segment process is a startup setting it carries, "gp.qe_identity".
 *
 * The connections are the session's: opened on first use, kept until the
 * session ends or one of them breaks.  Cloudberry calls a set of them a gang,
 * and so does this.
 *
 * What is *not* here yet is the distributed transaction: at M2 a dispatched
 * statement commits on each segment by itself, so a failure on one leaves the
 * others committed.  Two-phase commit is M3's, and until then nothing writes
 * from more than one statement at a time.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DISPATCH_H
#define GP_DISPATCH_H

#include "postgres.h"

#include "executor/tuptable.h"

/*
 * Run a statement on every segment and wait for all of them.
 *
 * Raises if any segment failed, naming the segment and repeating its own
 * message; the other segments are waited for first, so the connections are
 * left usable.
 */
extern void GpDispatchCommand(const char *sql);

/* The same, on one segment. */
extern void GpDispatchCommandOnContent(int content, const char *sql);

/*
 * Reading rows from every segment at once.
 *
 * The rows arrive as they are produced -- libpq's single-row mode -- so a
 * segment that has more of them does not wait for one that has fewer, and the
 * coordinator holds one row per segment rather than a whole result.
 */
typedef struct GpGatherState GpGatherState;

/*
 * Send the query to every segment.  "tupdesc" is what its rows will be
 * converted into, and it has to match the query's own result, which is the
 * caller's to arrange.
 */
extern GpGatherState *GpGatherStart(const char *sql, TupleDesc tupdesc);

/*
 * The next row from any segment, into the slot; false when every segment has
 * finished.  *content, when not NULL, is told which segment the row came from.
 */
extern bool GpGatherNext(GpGatherState *gather, TupleTableSlot *slot,
						 int *content);

/* Done with it, whether or not it was read to the end. */
extern void GpGatherEnd(GpGatherState *gather);

/* Close every connection: the session is over, or something went wrong. */
extern void GpDispatchResetGang(void);

/* Defines the settings; called from gp_core's _PG_init. */
extern void GpDispatchInit(void);

#endif							/* GP_DISPATCH_H */
