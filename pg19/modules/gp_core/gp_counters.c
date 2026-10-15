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
 * gp_counters.c
 *	  The node's dispatcher and interconnect figures, over every backend,
 *	  for a module that reports the cluster's metrics (gp_core_api.h,
 *	  1.16; pg_otel_probe's cluster metrics on the coordinator).
 *
 * The dispatcher counts the gangs it makes and the statements that take one
 * already there, the connections it opens to the segments and the time that
 * takes, the dispatches -- the client's statements and their parts it sends
 * the segments, each once whatever the number of segments: a gather's
 * statement, a slice's fragment, a write, a utility statement, a COPY; not a
 * module's own commands and queries, nor what it sends around them, its
 * settings, transaction control, snapshots and a gather's FETCH and CLOSE --
 * and the errors a segment's answer raised, once for each statement they
 * ended.  The interconnect counts, for each transport, the rows a Motion
 * sends and receives through it and their bytes, a row sent to every
 * receiver once for each, and the packets udpifc sent again: in the
 * process, as GpIcSend() and GpIcRecv() run, and into the node's counters
 * as a sender or a receiver ends, so that a row costs no shared atomic.
 *
 * Cloudberry keeps none of these as counters of its own; gpperfmon's and
 * gp_stats_collector's views read figures of the same kinds from its
 * statements' instrumentation.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "gp_counters.h"
#include "gp_ic.h"

typedef struct GpIcShared
{
	pg_atomic_uint64 bytes_sent;
	pg_atomic_uint64 bytes_received;
	pg_atomic_uint64 tuples_sent;
	pg_atomic_uint64 tuples_received;
	pg_atomic_uint64 retransmissions;
} GpIcShared;

typedef struct GpCountersShared
{
	pg_atomic_uint64 gangs_created;
	pg_atomic_uint64 gangs_reused;
	pg_atomic_uint64 connections;
	pg_atomic_uint64 connect_us;
	pg_atomic_uint64 dispatches;
	pg_atomic_uint64 errors;
	GpIcShared	ic[GP_IC_MAX_TRANSPORTS];
} GpCountersShared;

static GpCountersShared *counters = NULL;

/* This process's interconnect figures, not yet added to the node's. */
typedef struct GpIcLocal
{
	uint64		bytes_sent;
	uint64		bytes_received;
	uint64		tuples_sent;
	uint64		tuples_received;
	uint64		retransmissions;
} GpIcLocal;

static GpIcLocal ic_local[GP_IC_MAX_TRANSPORTS];
static bool ic_pending = false;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

static void
counters_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(sizeof(GpCountersShared)));
}

static void
counters_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	counters = ShmemInitStruct("gp_core counters", sizeof(GpCountersShared), &found);
	if (!found)
	{
		pg_atomic_init_u64(&counters->gangs_created, 0);
		pg_atomic_init_u64(&counters->gangs_reused, 0);
		pg_atomic_init_u64(&counters->connections, 0);
		pg_atomic_init_u64(&counters->connect_us, 0);
		pg_atomic_init_u64(&counters->dispatches, 0);
		pg_atomic_init_u64(&counters->errors, 0);
		for (int i = 0; i < GP_IC_MAX_TRANSPORTS; i++)
		{
			pg_atomic_init_u64(&counters->ic[i].bytes_sent, 0);
			pg_atomic_init_u64(&counters->ic[i].bytes_received, 0);
			pg_atomic_init_u64(&counters->ic[i].tuples_sent, 0);
			pg_atomic_init_u64(&counters->ic[i].tuples_received, 0);
			pg_atomic_init_u64(&counters->ic[i].retransmissions, 0);
		}
	}
	LWLockRelease(AddinShmemInitLock);
}

/* ------------------------------------------------------------------------- */
/* The dispatcher's                                                          */
/* ------------------------------------------------------------------------- */

void
GpCountGang(bool created)
{
	if (counters == NULL)
		return;
	pg_atomic_fetch_add_u64(created ? &counters->gangs_created : &counters->gangs_reused, 1);
}

void
GpCountConnections(int n, double ms)
{
	if (counters == NULL || n <= 0)
		return;
	pg_atomic_fetch_add_u64(&counters->connections, (uint64) n);
	if (ms > 0)
		pg_atomic_fetch_add_u64(&counters->connect_us, (uint64) (ms * 1000.0));
}

void
GpCountDispatch(void)
{
	if (counters != NULL)
		pg_atomic_fetch_add_u64(&counters->dispatches, 1);
}

void
GpCountDispatchError(void)
{
	if (counters != NULL)
		pg_atomic_fetch_add_u64(&counters->errors, 1);
}

/* ------------------------------------------------------------------------- */
/* The interconnect's                                                        */
/* ------------------------------------------------------------------------- */

void
GpCountIcSent(int transport, int len, int copies)
{
	if (transport < 0 || transport >= GP_IC_MAX_TRANSPORTS || copies <= 0)
		return;
	ic_local[transport].tuples_sent += (uint64) copies;
	ic_local[transport].bytes_sent += (uint64) Max(len, 0) * (uint64) copies;
	ic_pending = true;
}

void
GpCountIcReceived(int transport, int len)
{
	if (transport < 0 || transport >= GP_IC_MAX_TRANSPORTS)
		return;
	ic_local[transport].tuples_received++;
	ic_local[transport].bytes_received += (uint64) Max(len, 0);
	ic_pending = true;
}

void
GpCountIcRetransmitted(int transport, int n)
{
	if (transport < 0 || transport >= GP_IC_MAX_TRANSPORTS || n <= 0)
		return;
	ic_local[transport].retransmissions += (uint64) n;
	ic_pending = true;
}

void
GpCountIcFlush(void)
{
	if (!ic_pending || counters == NULL)
		return;
	for (int i = 0; i < GP_IC_MAX_TRANSPORTS; i++)
	{
		GpIcLocal  *l = &ic_local[i];
		GpIcShared *s = &counters->ic[i];

		if (l->bytes_sent)
			pg_atomic_fetch_add_u64(&s->bytes_sent, l->bytes_sent);
		if (l->bytes_received)
			pg_atomic_fetch_add_u64(&s->bytes_received, l->bytes_received);
		if (l->tuples_sent)
			pg_atomic_fetch_add_u64(&s->tuples_sent, l->tuples_sent);
		if (l->tuples_received)
			pg_atomic_fetch_add_u64(&s->tuples_received, l->tuples_received);
		if (l->retransmissions)
			pg_atomic_fetch_add_u64(&s->retransmissions, l->retransmissions);
	}
	memset(ic_local, 0, sizeof(ic_local));
	ic_pending = false;
}

/* ------------------------------------------------------------------------- */
/* What the API gives                                                        */
/* ------------------------------------------------------------------------- */

void
GpCountersDispatch(GpCoreDispatchCounters *c)
{
	memset(c, 0, sizeof(*c));
	if (counters == NULL)
		return;
	c->gangs_created = pg_atomic_read_u64(&counters->gangs_created);
	c->gangs_reused = pg_atomic_read_u64(&counters->gangs_reused);
	c->connections = pg_atomic_read_u64(&counters->connections);
	c->connect_us = pg_atomic_read_u64(&counters->connect_us);
	c->dispatches = pg_atomic_read_u64(&counters->dispatches);
	c->errors = pg_atomic_read_u64(&counters->errors);
}

/*
 * Every registered transport's figures, the relay's -- the coordinator's
 * rows, which no transport carries -- not among them.
 */
int
GpCountersInterconnect(GpCoreIcCounters *c, int max)
{
	static const char *const names[] = {"tcp", "udpifc", "udp2", "proxy", "shm"};
	int			n = 0;

	for (int k = 0; k < lengthof(names); k++)
	{
		const GpIcTransport *t = GpIcFindTransport(names[k]);
		int			i;

		if (t == NULL || (i = GpIcTransportIndex(t)) < 0 || i >= GP_IC_MAX_TRANSPORTS)
			continue;
		if (n < max)
		{
			GpCoreIcCounters *e = &c[n];

			e->transport = t->name;
			if (counters != NULL)
			{
				e->bytes_sent = pg_atomic_read_u64(&counters->ic[i].bytes_sent);
				e->bytes_received = pg_atomic_read_u64(&counters->ic[i].bytes_received);
				e->tuples_sent = pg_atomic_read_u64(&counters->ic[i].tuples_sent);
				e->tuples_received = pg_atomic_read_u64(&counters->ic[i].tuples_received);
				e->retransmissions = pg_atomic_read_u64(&counters->ic[i].retransmissions);
			}
			else
				memset(&e->bytes_sent, 0, sizeof(*e) - offsetof(GpCoreIcCounters, bytes_sent));
		}
		n++;
	}
	return n;
}

void
GpCountersInit(void)
{
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = counters_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = counters_shmem_startup;
}
