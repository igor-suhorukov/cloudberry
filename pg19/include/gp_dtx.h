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
 * gp_dtx.h
 *	  Distributed transactions: two-phase commit and distributed snapshots.
 *
 * A transaction that wrote on the segments is prepared on each of them, and
 * the coordinator's own commit record decides it: its transaction ID is the
 * distributed one, and a prepared part is committed when that ID committed
 * here.  A distributed snapshot says a transaction committed exactly when the
 * coordinator's snapshot does, and a segment makes its local snapshots agree:
 * see gp_dtx.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DTX_H
#define GP_DTX_H

#include "postgres.h"

#include "access/transam.h"
#include "utils/snapshot.h"

/*
 * A distributed transaction's part on a segment is prepared under this gid,
 * followed by the coordinator's full transaction ID in decimal.
 */
#define GP_DTX_GID_PREFIX		"gp_dtx_"
#define GP_DTX_GIDLEN			40

/* The setting a segment's transaction carries its distributed snapshot in. */
#define GP_DTX_SNAPSHOT_SETTING	"gp.distributed_snapshot"

/* What the coordinator asks each segment as a transaction commits: did it write? */
#define GP_DTX_STATUS_QUERY \
	"SELECT pg_catalog.pg_current_xact_id_if_assigned() IS NOT NULL"

/* The gid a transaction's parts are prepared under, into gid[GP_DTX_GIDLEN]. */
extern void GpDtxFormGid(FullTransactionId gxid, char *gid);

/*
 * The gid of its part in another database of the coordinator itself,
 * gp_dtx_<xid>_<database OID>: a gid is the server's, and one transaction
 * may have a part in more than one database there (gp_loopback.c).
 */
extern void GpDtxFormLoopbackGid(FullTransactionId gxid, Oid dboid, char *gid);

/* The coordinator's transaction a gid names; false when it is not ours. */
extern bool GpDtxParseGid(const char *gid, FullTransactionId *gxid);

/*
 * The coordinator: the distributed snapshot a snapshot of it stands for, as
 * the setting above carries it to the segments.
 */
extern char *GpDtxSnapshotString(Snapshot snapshot);

/*
 * The coordinator: a transaction's second phase did not reach a segment, and
 * the recovery process should finish it now rather than at its next round.
 */
extern void GpDtxWakeRecovery(void);

/*
 * The settings, the segment's hooks and shared memory, and on the
 * coordinator the recovery process; from gp_core's _PG_init.  After
 * GpMotionInit(): a fragment's snapshot is made distributed before the
 * writer publishes it to its readers.
 */
extern void GpDtxInit(void);

#endif							/* GP_DTX_H */
