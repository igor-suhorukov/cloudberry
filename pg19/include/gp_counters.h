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
 * gp_counters.h
 *	  The node's dispatcher and interconnect figures, over every backend;
 *	  see gp_counters.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COUNTERS_H
#define GP_COUNTERS_H

#include "postgres.h"

#include "gp_core_api.h"

/*
 * The dispatcher's (gp_dispatch.c).  A dispatch is a statement of the
 * client's, or a part of one, sent to the segments, once whatever the
 * number of segments it goes to: a gather's statement, a slice's fragment
 * (gp_motion.c), a write, a utility statement, a COPY.
 */
extern void GpCountGang(bool created);
extern void GpCountConnections(int n, double ms);
extern void GpCountDispatch(void);
extern void GpCountDispatchError(void);

/*
 * The interconnect's, by a transport's place (GpIcTransportIndex()):
 * counted in the process, and added to the node's as a sender or a
 * receiver ends -- not a shared atomic for each row.  A row sent to every
 * receiver counts once for each of the "copies".
 */
extern void GpCountIcSent(int transport, int len, int copies);
extern void GpCountIcReceived(int transport, int len);
extern void GpCountIcRetransmitted(int transport, int n);
extern void GpCountIcFlush(void);

/* What the API gives (gp_core_api.h, 1.16). */
extern void GpCountersDispatch(GpCoreDispatchCounters *counters);
extern int	GpCountersInterconnect(GpCoreIcCounters *counters, int max);

extern void GpCountersInit(void);

#endif							/* GP_COUNTERS_H */
