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
 * gp_fts.h
 *	  FTS, the fault tolerance service: the coordinator's prober, and what a
 *	  segment answers it.  See gp_fts.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_FTS_H
#define GP_FTS_H

/*
 * The name a mirror's WAL receiver connects to its primary under, which is
 * how the primary tells its mirror's WAL sender from any other's; Cloudberry's
 * GP_WALRECEIVER_APPNAME.
 */
#define GP_WALRECEIVER_APPNAME	"gp_walreceiver"

/*
 * What the prober's connections are called, on the segments and in their
 * logs: a connection's start knows them by it (gp_dispatch.c).
 */
#define GP_FTS_APPNAME			"cloudberry fts"

/*
 * The slot a primary keeps for its mirror, which a mirror FTS promotes makes
 * for the primary it failed over from; Cloudberry's
 * INTERNAL_WAL_REPLICATION_SLOT_NAME.
 */
#define GP_WAL_REPLICATION_SLOT	"internal_wal_replication_slot"

/*
 * Ask the prober for a probe, and wait until one that began after the asking
 * has ended: Cloudberry's FtsNotifyProber().  Returns at once where no
 * prober runs.
 */
extern void GpFtsNotifyProber(void);

/*
 * The process id of this node's WAL sender to the receiver that connected
 * as gp_walreceiver -- on a primary its mirror's, on the coordinator its
 * standby's -- or 0 while there is none.
 */
extern int	GpFtsWalreceiverSender(void);

/*
 * Settings, shared memory, the prober and what a segment answers it; from
 * gp_core's _PG_init, after the cluster is read.
 */
extern void GpFtsInit(void);

#endif							/* GP_FTS_H */
