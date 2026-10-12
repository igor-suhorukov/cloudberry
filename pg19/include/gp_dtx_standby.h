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
 * gp_dtx_standby.h
 *	  The distributed transactions in their second phase, which a hot
 *	  standby coordinator's queries see in progress still.  See
 *	  gp_dtx_standby.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DTX_STANDBY_H
#define GP_DTX_STANDBY_H

#include "postgres.h"

#include "access/transam.h"
#include "utils/snapshot.h"

/*
 * The coordinator: a transaction's first phase has ended, every part that
 * wrote prepared, and its commit record is next -- it is one in its second
 * phase from here until it is forgotten, and the WAL says so.  Raises where
 * there is no room to remember it.
 */
extern void GpDtxCommitting(FullTransactionId gxid);

/*
 * The coordinator: every part has its second phase, or the transaction did
 * not commit after all, and it is one in its second phase no more.  "wait"
 * is a commit's own: where the commit waits for the standby to have applied
 * it (synchronous_commit = remote_apply), it waits for this too.  Raises
 * nothing.
 */
extern void GpDtxForget(FullTransactionId gxid, bool wait);

/*
 * The coordinator: a second phase that did not reach every part, left to
 * the recovery process, which forgets the transaction once it has.
 */
extern void GpDtxAbandon(FullTransactionId gxid);

/* The oldest transaction in its second phase; invalid where there is none. */
extern FullTransactionId GpDtxCommittingOldest(void);

/*
 * The recovery process.  The transactions left to it, into a palloc'd
 * array; one whose part it found prepared and committed here, which is left
 * to it too if nobody has it -- true when it was added so, and false with
 * *full set where there was no room; and whether a round has found every
 * such transaction since this node began to write, from which the set is
 * whole and may be logged.
 */
extern int	GpDtxAbandonedList(uint64 **gxids);
extern bool GpDtxNoteInDoubt(FullTransactionId gxid, bool *full);
extern void GpDtxSetGathered(bool gathered);
extern bool GpDtxAnyAbandoned(void);

/*
 * The coordinator's standby process: the whole set to the WAL, where it is
 * due -- every 15 seconds in which WAL that matters was written, and at
 * once where "now" says so or the recovery process has just found it whole.
 */
extern void GpDtxLogRunning(bool now);

/* The redo of the three records (gp_dbcopy.c). */
extern void GpDtxStandbyRedo(uint8 info, const char *data, int len);

/*
 * A hot standby coordinator: wait until the set is known here, or say why a
 * statement cannot be dispatched yet; and the transactions of the set that
 * a snapshot of this backend's is to see in progress though it has their
 * commit, as full transaction IDs in no order, into *gxids (the backend's
 * own memory, good until the next call).
 */
extern void GpDtxStandbyWaitKnown(void);
extern int	GpDtxStandbyHidden(Snapshot snapshot, const uint64 **gxids);

/* gp_internal.dtx_committing()'s rows, for a test: the set, on any node. */
extern int	GpDtxCommittingList(uint64 **gxids, bool **abandoned, bool *known,
								bool *gathered);

/* Shared memory, from gp_core's _PG_init. */
extern void GpDtxStandbyInit(void);

#endif							/* GP_DTX_STANDBY_H */
