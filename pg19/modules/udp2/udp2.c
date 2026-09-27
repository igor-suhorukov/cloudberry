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
 * udp2.c
 *	  The UDP2 motion transport: gp.interconnect_type = udp2.
 *
 * Cloudberry's UDP2 is a UDP interconnect whose core is C++ that includes
 * no PostgreSQL header (contrib/udp2/ic_common), compiled here where it lies
 * (udp2/meson.build), and an adapter of Cloudberry's to its executor
 * (contrib/udp2/ic_udp2.c), in whose place this file adapts it to the
 * port's transport table (gp_ic.h).  The core keeps a thread in each process
 * that uses it, which takes every packet that comes and acknowledges it,
 * the process asking the core for a connection's packets as it reads them.
 * It is set up for a whole statement at once, from a slice table -- each
 * slice's processes, where each receives, and which slice receives it -- as
 * Cloudberry's SetupInterconnect() sets up a statement's connections at the
 * start of its executor, before any Motion runs.
 *
 * So each process of a statement that streams over udp2 sets the core up
 * as its fragment starts (stmt_begin), from what the coordinator told it
 * (GpIcStream): the slices, each one's senders and the processes that run
 * its parent, and the one this process runs.  A row goes as a frame, its
 * length and itself, as gp_ic.c's transports send one; the core cuts the
 * frames into packets, and a receiver joins each connection's packets into
 * its stream again as they come, and takes its frames from it.  A sender's
 * last frame is gp_ic.h's end, in the core's end-of-stream.
 *
 * Where this adapter differs from Cloudberry's:
 *
 *   - Every process sets the core up in its dispatcher's role, whose
 *     history of the statements it set up tells a late packet of one torn
 *     down from an early one of one not set up yet.  A segment process of
 *     the port's can have several statements set up at once, as
 *     Cloudberry's dispatcher can -- a writer, whose cursor's fragment waits
 *     for its next FETCH while another statement streams (gp_motion.c) --
 *     where the executor's role counts them, one at a time, and would
 *     refuse the late packets of the one of two that ended second.
 *
 *   - An interrupt the core looks for as it waits is taken as PostgreSQL
 *     would take it there, and one that raises an error is caught and the
 *     core told by a C++ exception, which unwinds its frames and releases
 *     the locks they hold (udp2_interrupt.cpp); the error is raised again
 *     once the core has returned.  Cloudberry's adapter raises it from
 *     within the core instead, jumping over its frames, which leaves the
 *     lock its receive waits under held (the std::unique_lock over mtx in
 *     ic_udp2.cpp's receiveChunksUDPIFC()) for its teardown to unlock -- a
 *     statement cancelled in one while another is set up in the same
 *     process would find it held.
 *
 *   - The core's packet size is fixed as it starts, as Cloudberry's
 *     gp_max_packet_size is a backend's (PGC_BACKEND): here the node's
 *     gp.max_packet_size, whatever a session sets for udpifc, so that the
 *     processes of a statement agree on it.
 *
 *   - It listens on the node's address, or on 127.0.0.1 where the cluster
 *     names a node by the directory of its socket: UDP2 speaks IP alone.
 *
 * Cloudberry sources this file stands in for:
 *	  contrib/udp2/ic_udp2.c, contrib/udp2/ic_modules.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "cb_module.h"
#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_ic.h"

/* UDP2's core, whose C interface this adapts */
#include "ic_types.h"
#include "udp2/ic_udp2.h"

PG_MODULE_MAGIC_EXT(
					.name = "udp2",
					.version = GP_VERSION
);

/* The core's dispatcher's role, as it numbers it (ic_utility.hpp) */
#define UDP2_ROLE_DISPATCH		2

/* gp.udpic_dropseg's value for every segment (ic_utility.hpp's UNDEF_SEGMENT) */
#define UDP2_EVERY_SEGMENT		(-2)

/* Cloudberry's settings of UDP2's that gp_core's udpifc has not */
static bool gp_interconnect_full_crc = false;
static int	gp_udp_bufsize_k = 0;
static int	gp_udpic_dropseg = UDP2_EVERY_SEGMENT;
static int	gp_udpic_fault_inject_percent = 0;
static int	gp_udpic_fault_inject_bitmap = 0;
static int	gp_udpic_network_disable_ipv6 = 0;

/* Where this process receives, once the core is up. */
static char *udp2_address_string = NULL;

/*
 * A sender's rows as they come: what its packets carried, until a frame is
 * whole.
 */
typedef struct Udp2Route
{
	char	   *buf;
	int			size;
	int			start;			/* the next unread byte */
	int			end;			/* one past the last byte had */
	bool		ended;			/* its end has come */
} Udp2Route;

/*
 * A Motion's receiver: the core's receiving entry of the slice -- its motion
 * node, in the core's words -- whose routes are the slice's senders, in
 * their order.
 */
typedef struct Udp2Receiver
{
	GpIcReceiver base;
	GpIcStream *stream;
	int			slice;
	int			nroutes;
	Udp2Route  *routes;
	int			nended;
	int			next;			/* whose rows to look at first */
} Udp2Receiver;

/* The core's sending entry of the slice, whose routes are its receivers. */
typedef struct Udp2Sender
{
	GpIcSender	base;
	GpIcStream *stream;
	int			slice;
	bool		wanted;			/* the core's word, from its last send */
} Udp2Sender;

void		_PG_init(void);

/* udp2_interrupt.cpp's: the core's interrupt callback, which asks this */
extern void udp2_check_interrupts(int teardownActive);
extern bool udp2_interrupted(void);

/* ------------------------------------------------------------------------- */
/* The core's errors, and its interrupts                                     */
/* ------------------------------------------------------------------------- */

/* An interrupt's error the core was told of, raised again once it returns. */
static ErrorData *udp2_interrupt_error = NULL;

/*
 * Would an interrupt raise an error now?  Taken as CHECK_FOR_INTERRUPTS()
 * takes one, and its error, if it raises one, caught -- the core is to
 * return first (udp2_interrupt.cpp).  A FATAL one ends the process here, as
 * anywhere.
 */
bool
udp2_interrupted(void)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	bool		raised = false;

	if (!INTERRUPTS_PENDING_CONDITION())
		return false;
	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
	}
	PG_CATCH();
	{
		/* the error aborts the transaction, which frees its copy */
		MemoryContextSwitchTo(TopTransactionContext != NULL
							  ? TopTransactionContext : TopMemoryContext);
		udp2_interrupt_error = CopyErrorData();
		FlushErrorState();
		raised = true;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcxt);
	return raised;
}

/*
 * After a call of the core's: the error of an interrupt it was told of,
 * raised again; or its own, as Cloudberry's adapter raises it
 * (HandleLastError()), with Cloudberry's SQLSTATE for the interconnect.
 */
static void
udp2_raise(void)
{
	ICError    *error = GetLastError();
	char	   *msg;
	int			len;

	if (udp2_interrupt_error != NULL)
	{
		ErrorData  *edata = udp2_interrupt_error;

		udp2_interrupt_error = NULL;
		ReThrowError(edata);
	}
	if (error->level == LEVEL_OK)
		return;

	/* its words end with the place in its source it threw them from */
	msg = pstrdup(error->msg);
	len = strlen(msg);
	while (len > 0 && msg[len - 1] == '\n')
		msg[--len] = '\0';
	ereport(error->level == LEVEL_FATAL ? FATAL : ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("%s", msg)));
}

static bool
udp2_postmaster_alive(void)
{
	return PostmasterIsAlive();
}

/* ------------------------------------------------------------------------- */
/* The core, and where this process receives                                 */
/* ------------------------------------------------------------------------- */

static void
udp2_quit(int code, Datum arg)
{
	UDP2_WaitQuitUDPIFC();
	UDP2_CleanUpUDPIFC();
}

/*
 * Where this process receives: "udp2:<dbid>:<content>:<pid>:<host>:<port>",
 * the core started the first time it is asked -- its sockets, and its
 * thread, which blocks every signal and so leaves them this process's.
 */
static const char *
udp2_address(void)
{
	const GpSegmentConfig *self = GpClusterSelf();
	GlobalMotionLayerIPCParam param;
	const char *host;

	if (udp2_address_string != NULL)
		return udp2_address_string;
	if (self == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the interconnect needs a cluster")));

	host = self->hostname[0] == '/' ? "127.0.0.1" : self->hostname;
	memset(&param, 0, sizeof(param));
	param.interconnect_address = unconstify(char *, host);
	param.Gp_role = UDP2_ROLE_DISPATCH;
	param.segment_number = GpClusterSegmentCount();
	param.ic_htab_size = param.segment_number * 2;
	param.MyProcPid = MyProcPid;
	param.dbid = GpClusterDbid();
	param.segindex = GpClusterContentId();
	param.MyProcPort = MyProcPort != NULL;
	param.myprocport_sock = MyProcPort != NULL ? MyProcPort->sock : -1;
	param.Gp_max_packet_size =
		pg_strtoint32(GetConfigOptionResetString("gp.max_packet_size"));
	param.Gp_udp_bufsize_k = gp_udp_bufsize_k;
	param.Gp_interconnect_address_type = 0; /* the address given, unicast */
	param.checkPostmasterIsAliveCallback = udp2_postmaster_alive;
	param.checkInterruptsCallback = udp2_check_interrupts;

	/*
	 * Not interconnect_setup_palloc, which gp_ic.c's wrappers fire once a
	 * Motion; no remapper of a row's record types, as a row travels whole
	 * (gp_motion.c's motion_tuples()); and no look at the dispatcher's
	 * connections to the segments, which only the core's receive in the
	 * dispatcher's role makes, and which no segment process has.
	 */
	param.simpleFaultInjectorCallback = NULL;
	param.createOpaqueDataCallback = NULL;
	param.destroyOpaqueDataCallback = NULL;
	param.checkCancelOnQDCallback = NULL;

	ResetLastError();
	UDP2_InitUDPIFC(&param);
	udp2_raise();
	on_proc_exit(udp2_quit, 0);

	udp2_address_string =
		MemoryContextStrdup(TopMemoryContext,
							psprintf("udp2:%d:%d:%d:%s:%d",
									 GpClusterDbid(), GpClusterContentId(),
									 MyProcPid, host,
									 (int) UDP2_GetListenPortUDP()));
	return udp2_address_string;
}

/* A process of a slice as the core's slice table has it, from its address. */
static void
udp2_process(const char *address, int content, ICCdbProcess *proc)
{
	int			dbid;
	int			pid;
	int			n = 0;
	const char *colon = strrchr(address, ':');

	if (sscanf(address, "udp2:%d:%*d:%d:%n", &dbid, &pid, &n) != 2 ||
		n == 0 || colon == NULL || colon < address + n)
		elog(ERROR, "invalid udp2 interconnect address \"%s\"", address);
	proc->valid = true;
	proc->listenerAddr = pnstrdup(address + n, colon - (address + n));
	proc->listenerPort = pg_strtoint32(colon + 1);
	proc->pid = pid;
	proc->contentid = content;
	proc->dbid = dbid;
}

/* The processes of a slice, in memory the core frees. */
static ICCdbProcess *
udp2_processes(int n, char **addresses, int *contents)
{
	ICCdbProcess *procs = calloc(Max(n, 1), sizeof(ICCdbProcess));

	if (procs == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	for (int i = 0; i < n; i++)
		udp2_process(addresses[i], contents[i], &procs[i]);
	return procs;
}

/* A slice table the core was not given, as its teardown frees one. */
static void
udp2_table_free(ICSliceTable *table)
{
	for (int i = 0; i < table->numSlices; i++)
	{
		free(table->slices[i].children);
		free(table->slices[i].primaryProcesses);
	}
	free(table->slices);
	free(table);
}

/*
 * The statement's slice table as the core has one (ic_types.h), indexed by
 * slice: each slice that streams, its processes its senders and its
 * children the slices it receives; and the Gather's, whose processes, the
 * writers, are its children's receivers.  In the core's memory, as
 * Cloudberry's adapter makes it (ConvertToICSliceTable()).  The processes'
 * addresses are the stream's, which outlives the core's use of them.
 */
static ICSliceTable *
udp2_slice_table(GpIcStream *stream)
{
	ICSliceTable *table = calloc(1, sizeof(ICSliceTable));
	int			nslices = stream->top + 1;

	if (table == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	for (int i = 0; i < stream->nslices; i++)
		nslices = Max(nslices, stream->slices[i].slice + 1);
	table->localSlice = stream->self;
	table->ic_instance_id = stream->serial;
	table->slices = calloc(nslices, sizeof(ICExecSlice));
	if (table->slices == NULL)
	{
		free(table);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	table->numSlices = nslices;
	for (int s = 0; s < nslices; s++)
	{
		table->slices[s].sliceIndex = s;
		table->slices[s].parentIndex = -1;
	}

	PG_TRY();
	{
		for (int i = 0; i < stream->nslices; i++)
		{
			GpIcSlice  *slice = &stream->slices[i];
			ICExecSlice *mine = &table->slices[slice->slice];
			ICExecSlice *parent = &table->slices[slice->parent];
			int		   *children;

			mine->parentIndex = slice->parent;
			mine->numSegments = slice->nsenders;
			mine->numPrimaryProcesses = slice->nsenders;
			mine->primaryProcesses = udp2_processes(slice->nsenders,
													slice->sender_addresses,
													slice->sender_contents);

			children = realloc(parent->children,
							   (parent->numChildren + 1) * sizeof(int));
			if (children == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			parent->children = children;
			parent->children[parent->numChildren++] = slice->slice;

			if (slice->parent == stream->top && parent->primaryProcesses == NULL)
			{
				parent->numSegments = slice->nreceivers;
				parent->numPrimaryProcesses = slice->nreceivers;
				parent->primaryProcesses = udp2_processes(slice->nreceivers,
														  slice->receiver_addresses,
														  slice->receiver_contents);
			}
		}
	}
	PG_CATCH();
	{
		udp2_table_free(table);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return table;
}

/*
 * The statement's settings, which the core reads as it is set up: the UDP
 * interconnect's, gp_core's and udp2's own, which the coordinator gave every
 * process of the statement alike; and the statement's numbers, the
 * coordinator's session and the statement's in it, by which the core tells
 * one statement's packets from another's.
 */
static void
udp2_session_param(GpIcStream *stream, SessionMotionLayerIPCParam *param)
{
	memset(param, 0, sizeof(*param));
	param->Gp_interconnect_queue_depth = gp_interconnect_queue_depth;
	param->Gp_interconnect_snd_queue_depth = gp_interconnect_snd_queue_depth;
	param->Gp_interconnect_cursor_ic_table_size = 128;
	param->Gp_interconnect_timer_period = gp_interconnect_timer_period;
	param->Gp_interconnect_timer_checking_period = gp_interconnect_timer_checking_period;
	param->Gp_interconnect_default_rtt = gp_interconnect_default_rtt;
	param->Gp_interconnect_min_rto = gp_interconnect_min_rto;
	param->Gp_interconnect_transmit_timeout = gp_interconnect_transmit_timeout;
	param->Gp_interconnect_min_retries_before_timeout = gp_interconnect_min_retries_before_timeout;
	param->Gp_interconnect_debug_retry_interval = gp_interconnect_debug_retry_interval;
	param->gp_interconnect_full_crc = gp_interconnect_full_crc;
	param->gp_interconnect_aggressive_retry = true;
	param->gp_interconnect_cache_future_packets = gp_interconnect_cache_future_packets;
	param->gp_interconnect_log_stats = false;
	param->interconnect_setup_timeout = 7200;
	param->gp_log_interconnect = gp_log_interconnect;
	param->gp_session_id = stream->session;
	param->Gp_interconnect_fc_method = gp_interconnect_fc_method;
	param->gp_command_count = (int) stream->serial;
	param->gp_interconnect_id = stream->serial;
	param->log_min_messages = log_min_messages[MyBackendType];
	param->distTransId = 0;
	param->gp_udpic_dropseg = gp_udpic_dropseg;
	param->gp_udpic_dropacks_percent = gp_udpic_dropacks_percent;
	param->gp_udpic_dropxmit_percent = gp_udpic_dropxmit_percent;
	param->gp_udpic_fault_inject_percent = gp_udpic_fault_inject_percent;
	param->gp_udpic_fault_inject_bitmap = gp_udpic_fault_inject_bitmap;
	param->gp_udpic_network_disable_ipv6 = gp_udpic_network_disable_ipv6;
}

/* ------------------------------------------------------------------------- */
/* A statement                                                               */
/* ------------------------------------------------------------------------- */

/*
 * The statement set up before its plan is: the core's connections of the
 * slices this process receives and of the one it sends, as Cloudberry's
 * SetupInterconnect() makes them, so that a packet that comes finds its
 * connection, or is kept for it.
 */
static void
udp2_stmt_begin(GpIcStream *stream)
{
	SessionMotionLayerIPCParam param;
	ICSliceTable *table;
	ICChunkTransportState *state;

	(void) udp2_address();
	table = udp2_slice_table(stream);
	udp2_session_param(stream, &param);

	ResetLastError();
	state = UDP2_SetupUDP(table, &param);
	if (state == NULL)
	{
		/* the core keeps a table it set up, and not one it failed to */
		udp2_table_free(table);
		udp2_raise();
		elog(ERROR, "UDP2 did not set up the statement's interconnect");
	}
	stream->state = state;
	udp2_raise();
}

/*
 * The statement's end: the core's connections torn down, and what a sender
 * still sends answered with a stop by the core's thread from then on.  At
 * an error what fails is logged only: the (sub)transaction is aborting.
 */
static void
udp2_stmt_end(GpIcStream *stream, bool error)
{
	ICChunkTransportState *state = stream->state;

	if (state == NULL)
		return;
	stream->state = NULL;

	HOLD_INTERRUPTS();
	ResetLastError();
	UDP2_TeardownUDP(state, error);
	RESUME_INTERRUPTS();
	if (error)
	{
		if (GetLastError()->level != LEVEL_OK)
			ereport(LOG,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("UDP2 could not tear a statement down: %s",
							GetLastError()->msg)));
		return;
	}
	udp2_raise();
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

static GpIcSender *
udp2_send_begin(GpIcStream *stream, GpIcSlice *slice)
{
	Udp2Sender *s;

	if (stream->state == NULL)
		elog(ERROR, "udp2: the statement's interconnect is not set up");
	s = MemoryContextAllocZero(GetMemoryChunkContext(stream), sizeof(Udp2Sender));
	s->stream = stream;
	s->slice = slice->slice;
	s->wanted = true;
	return &s->base;
}

/*
 * A row, its frame's length and itself, as two blocks the core puts in its
 * packets: to route "receiver" of the slice -- its receivers are the core's
 * routes, in their order -- or to every one.
 */
static void
udp2_send(GpIcSender *sender, int receiver, const char *data, int len)
{
	Udp2Sender *s = (Udp2Sender *) sender;
	uint32		frame = pg_hton32((uint32) len);
	DataBlock	blocks[2];

	if (!s->wanted)
		return;
	blocks[0].pos = (unsigned char *) &frame;
	blocks[0].len = sizeof(frame);
	blocks[1].pos = (unsigned char *) unconstify(char *, data);
	blocks[1].len = len;

	ResetLastError();
	s->wanted = UDP2_SendData((ICChunkTransportState *) s->stream->state,
							  (int16) s->slice, (int16) Max(receiver, 0),
							  blocks, len > 0 ? 2 : 1, receiver < 0);
	udp2_raise();
}

static bool
udp2_send_wanted(GpIcSender *sender)
{
	return ((Udp2Sender *) sender)->wanted;
}

/* The end, to every receiver still there, each waited for to have it all. */
static void
udp2_send_end(GpIcSender *sender)
{
	Udp2Sender *s = (Udp2Sender *) sender;
	uint32		frame = pg_hton32(GP_IC_END_OF_ROWS);
	DataBlock	block;

	block.pos = (unsigned char *) &frame;
	block.len = sizeof(frame);
	ResetLastError();
	UDP2_SendEOS((ICChunkTransportState *) s->stream->state, s->slice, &block);
	udp2_raise();
	pfree(s);
}

/* ------------------------------------------------------------------------- */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------- */

static GpIcReceiver *
udp2_recv_begin(GpIcStream *stream, GpIcSlice *slice)
{
	MemoryContext cxt = GetMemoryChunkContext(stream);
	Udp2Receiver *r;

	if (stream->state == NULL)
		elog(ERROR, "udp2: the statement's interconnect is not set up");
	r = MemoryContextAllocZero(cxt, sizeof(Udp2Receiver));
	r->stream = stream;
	r->slice = slice->slice;
	r->nroutes = slice->nsenders;
	r->routes = MemoryContextAllocZero(cxt, sizeof(Udp2Route) * Max(r->nroutes, 1));
	return &r->base;
}

/*
 * A frame whole among a sender's rows, taken: 0 none yet, 1 a row, 2 its
 * end -- after which the core is told its route has no more to give, as
 * Cloudberry's motion layer tells it at a stream's end
 * (DeregisterReadInterest()).
 */
static int
udp2_take(Udp2Receiver *r, int route, char **data, int *len)
{
	Udp2Route  *rt = &r->routes[route];
	uint32		frame;

	if (rt->end - rt->start < (int) sizeof(uint32))
		return 0;
	memcpy(&frame, rt->buf + rt->start, sizeof(uint32));
	frame = pg_ntoh32(frame);
	if (frame == GP_IC_END_OF_ROWS)
	{
		rt->start += sizeof(uint32);
		rt->ended = true;
		r->nended++;
		ResetLastError();
		UDP2_DeactiveRoute((ICChunkTransportState *) r->stream->state,
						   r->slice, route, "end of rows");
		udp2_raise();
		return 2;
	}
	if (rt->end - rt->start < (int) (sizeof(uint32) + frame))
		return 0;
	*data = rt->buf + rt->start + sizeof(uint32);
	*len = (int) frame;
	rt->start += sizeof(uint32) + frame;
	return 1;
}

/*
 * A packet's bytes, after the ones its sender sent before, and the packet
 * given back to the core, which acknowledges it: its room made for the
 * next.
 */
static void
udp2_put(Udp2Receiver *r, int route, DataBlock *data)
{
	Udp2Route  *rt;

	if (route < 0 || route >= r->nroutes)
		elog(ERROR, "udp2: a packet of slice %d came by route %d of %d",
			 r->slice, route, r->nroutes);
	rt = &r->routes[route];
	if (rt->start == rt->end)
		rt->start = rt->end = 0;
	if (rt->size - rt->end < data->len && rt->start > 0)
	{
		memmove(rt->buf, rt->buf + rt->start, rt->end - rt->start);
		rt->end -= rt->start;
		rt->start = 0;
	}
	if (rt->size - rt->end < data->len)
	{
		int			size = Max(Max(rt->size * 2, rt->end + data->len), 64 * 1024);

		rt->buf = rt->buf == NULL
			? MemoryContextAlloc(GetMemoryChunkContext(r), size)
			: repalloc(rt->buf, size);
		rt->size = size;
	}
	memcpy(rt->buf + rt->end, data->pos, data->len);
	rt->end += data->len;

	ResetLastError();
	UDP2_ReleaseAndAck((ICChunkTransportState *) r->stream->state, r->slice,
					   route);
	udp2_raise();
}

/* The core has no route open, and a sender's end had not come. */
static void
udp2_lost(Udp2Receiver *r)
{
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: a sender of slice %d stopped before its last row",
					r->slice)));
}

static bool
udp2_recv(GpIcReceiver *receiver, char **data, int *len)
{
	Udp2Receiver *r = (Udp2Receiver *) receiver;

	for (;;)
	{
		DataBlock	block = {NULL, 0};
		int16		route = -1;

		/* a row already here, the senders taking turns */
		for (int k = 0; k < r->nroutes; k++)
		{
			int			i = (r->next + k) % r->nroutes;

			if (!r->routes[i].ended && udp2_take(r, i, data, len) == 1)
			{
				r->next = (i + 1) % r->nroutes;
				return true;
			}
		}
		if (r->nended == r->nroutes)
			return false;

		ResetLastError();
		UDP2_RecvAny((ICChunkTransportState *) r->stream->state, r->slice,
					 &route, NULL, &block);
		udp2_raise();
		if (block.pos == NULL)
			udp2_lost(r);
		udp2_put(r, route, &block);
	}
}

static bool
udp2_recv_from(GpIcReceiver *receiver, int k, char **data, int *len)
{
	Udp2Receiver *r = (Udp2Receiver *) receiver;

	Assert(k >= 0 && k < r->nroutes);
	for (;;)
	{
		DataBlock	block = {NULL, 0};
		int			got;

		if (r->routes[k].ended)
			return false;
		got = udp2_take(r, k, data, len);
		if (got == 1)
			return true;
		if (got == 2)
			return false;

		ResetLastError();
		UDP2_RecvRoute((ICChunkTransportState *) r->stream->state, r->slice,
					   k, NULL, &block);
		udp2_raise();
		if (block.pos == NULL)
			udp2_lost(r);
		udp2_put(r, k, &block);
	}
}

/*
 * Done: the senders that have not sent their end told to stop, as
 * Cloudberry's receiver tells them (SendStopMessage()) -- and what they
 * still send, answered with a stop by the core's thread once the statement
 * is torn down here.
 */
static void
udp2_recv_end(GpIcReceiver *receiver)
{
	Udp2Receiver *r = (Udp2Receiver *) receiver;

	if (r->nended < r->nroutes)
	{
		ResetLastError();
		UDP2_SendStop((ICChunkTransportState *) r->stream->state, r->slice);
		udp2_raise();
	}
	for (int i = 0; i < r->nroutes; i++)
		if (r->routes[i].buf != NULL)
			pfree(r->routes[i].buf);
	pfree(r->routes);
	pfree(r);
}

static const GpIcTransport udp2_transport = {
	.name = "udp2",
	.address = udp2_address,
	.stmt_begin = udp2_stmt_begin,
	.send_begin = udp2_send_begin,
	.send = udp2_send,
	.send_wanted = udp2_send_wanted,
	.send_end = udp2_send_end,
	.recv_begin = udp2_recv_begin,
	.recv = udp2_recv,
	.recv_from = udp2_recv_from,
	.recv_end = udp2_recv_end,
	.stmt_end = udp2_stmt_end,
};

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("udp2");
	CB_REQUIRE_CORE("udp2");

	DefineCustomBoolVariable("gp.interconnect_full_crc",
							 "Sanity check incoming data stream.",
							 "UDP2 checks each packet's CRC.  Cloudberry calls this gp_interconnect_full_crc.",
							 &gp_interconnect_full_crc,
							 false, PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udp_bufsize_k",
							"Sets recv buf size of UDP interconnect, for testing.",
							"The room of UDP2's sockets, in kilobytes, 0 for two megabytes, fixed as a backend starts it.  Cloudberry calls this gp_udp_bufsize_k.",
							&gp_udp_bufsize_k,
							0, 0, 32768, PGC_BACKEND, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_dropseg",
							"Specifies a segment to which the dropacks, and dropxmit settings will be applied, for testing. (The default is to apply the dropacks and dropxmit settings to all segments)",
							"Cloudberry calls this gp_udpic_dropseg.",
							&gp_udpic_dropseg,
							UDP2_EVERY_SEGMENT, UDP2_EVERY_SEGMENT, INT_MAX,
							PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_fault_inject_percent",
							"Sets the percentage of fault injected into system calls, for testing. (affected by gp_udpic_dropseg)",
							"Cloudberry calls this gp_udpic_fault_inject_percent.",
							&gp_udpic_fault_inject_percent,
							0, 0, 100, PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_fault_inject_bitmap",
							"Sets the bitmap for faults injection, for testing. (affected by gp_udpic_dropseg)",
							"Cloudberry calls this gp_udpic_fault_inject_bitmap.",
							&gp_udpic_fault_inject_bitmap,
							0, 0, INT_MAX, PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_network_disable_ipv6",
							"Sets the address info hint to disable the ipv6, for testing. (affected by gp_udpic_dropseg)",
							"Cloudberry calls this gp_udpic_network_disable_ipv6.",
							&gp_udpic_network_disable_ipv6,
							0, 0, 1, PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	GpIcRegisterTransport(&udp2_transport);
}
