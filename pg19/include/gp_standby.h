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
 * gp_standby.h
 *	  The coordinator's standby, whose commits wait for it while it streams.
 *	  See gp_standby.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_STANDBY_H
#define GP_STANDBY_H

#include "postgres.h"

#include "access/xlogdefs.h"

/*
 * Is a WAL sender that has sent up to "sent" caught up within
 * gp.repl_catchup_within_range WAL segments of what this node has flushed --
 * near enough that a commit may wait for its standby, or its mirror, while
 * it catches up?  Cloudberry's WalSndIsCatchupWithinRange().
 */
extern bool GpStandbyWithinRange(XLogRecPtr sent);

/*
 * A standby's or a mirror's WAL sender started or ended on this node: the
 * coordinator's process that makes its standby synchronous looks again.
 * Nothing on a node that has none.
 */
extern void GpStandbyWake(void);

/* The setting, shared memory and, on a coordinator, its process. */
extern void GpStandbyInit(void);

#endif							/* GP_STANDBY_H */
