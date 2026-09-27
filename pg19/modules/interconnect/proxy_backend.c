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
 * proxy_backend.c
 *	  The proxy transport's backend half: gp.interconnect_type = proxy.
 *
 * A segment process sends a Motion's rows to a process on another node, or
 * on its own, through its node's proxy (interconnect.c): each logical
 * connection -- a sender and one of its receivers -- is a connection of the
 * process's own to its proxy, on the Unix socket the proxy listens on
 * beside every node's (ic_proxy.h's ic_proxy_build_server_sock_path()),
 * which the process introduces with a HELLO that names it, the process at
 * the other end, and the statement and slices, as Cloudberry's backend half
 * does (ic_proxy_backend.c).  The proxy answers HELLO ACK, and from then on
 * carries what one end writes to the other: the two proxies' connection is
 * one of the pair of nodes', and a connection whose other end has not come
 * yet waits in the proxy's placeholder for it.
 *
 * What travels is ic-tcp's packets, as Cloudberry's backends send them
 * through a proxy: a length, four bytes in the host's order, the length
 * counted, and the bytes; here the bytes are the stream of the rows' frames
 * gp_ic.c's transports send (gp_ic.h), cut into packets no larger than the
 * proxy's, which a receiver joins again.
 *
 * A receiver that needs no more rows cannot close, as a tcp one does: the
 * proxy would drop the connection's end on its side, and what the sender
 * still sent would wait in a placeholder no receiver ever claims.  So it
 * writes the sender a packet of its own, STOP, which the proxy carries back
 * to it, and keeps the connection; the sender, which reads its connection as
 * it writes, stops at a STOP or at the connection's end, and closes; its
 * proxy's BYE ends the connection at the receiver, which reads to that end
 * before it closes, at the statement's end here (stmt_end) -- as udpifc's
 * receivers wait for their senders' CLOSE (gp_ic.c).  A connection is
 * closed at both ends, so neither proxy keeps anything of it.  An error
 * closes every connection at once.
 *
 * Cloudberry's backend half is ic-tcp's code, once the connection is made;
 * this one is the port's own, its waits gp_ic.c's, and ic-tcp's code stays
 * out of both.
 *
 * Cloudberry sources this file stands in for:
 *	  contrib/interconnect/proxy/ic_proxy_backend.c, and ic-tcp's part of
 *	  a proxied connection (contrib/interconnect/tcp/ic_tcp.c)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/postmaster.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_ic.h"

#include "ic_proxy.h"
#include "ic_proxy_key.h"
#include "ic_proxy_packet.h"

#include "proxy_backend.h"

/* A packet's header: its length, the header counted, in the host's order */
#define PROXY_HEADER		((int) sizeof(uint32))

/* How much a sender holds for a receiver before it writes, as gp_ic.c's */
#define PROXY_FLUSH_BYTES	(64 * 1024)

/* A receiver's word to its sender: no more rows (the packet's one byte) */
#define PROXY_STOP			'S'

/* How often a process tries its proxy again, not listening yet */
#define PROXY_CONNECT_RETRY_MS	100

/* Where this process receives: "proxy:<dbid>:<content>:<pid>". */
static char *proxy_address_string = NULL;

/* One sender's connection, as its receiver has it. */
typedef struct ProxyIn
{
	pgsocket	sock;
	char		header[PROXY_HEADER];	/* the packet header coming */
	int			headerlen;
	uint32		left;			/* the packet's bytes still to come */
	char	   *buf;			/* its stream of frames, as it came */
	int			size;
	int			start;			/* the next unread byte */
	int			end;			/* one past the last byte had */
	bool		ended;			/* its end came */
	bool		closed;			/* and its proxy's end of the connection */
	bool		stopped;		/* told STOP */
} ProxyIn;

typedef struct ProxyReceiver
{
	GpIcReceiver base;
	GpIcStream *stream;
	int			slice;
	int			nins;			/* a sender each, in the slice's order */
	ProxyIn    *ins;
	int			nended;
	int			next;			/* whose rows to look at first */
	WaitEventSet *wes;			/* the latch, and the connections open */
	int			wes_one;		/* the one it waits on, or -1: every one */
} ProxyReceiver;

/* One receiver's connection, as its sender has it. */
typedef struct ProxyOut
{
	pgsocket	sock;
	char	   *buf;			/* packets to write */
	int			len;
	int			size;
	int			packet;			/* where the packet being filled begins, or -1 */
	bool		wanted;			/* no STOP, and the connection open */
} ProxyOut;

typedef struct ProxySender
{
	GpIcSender	base;
	GpIcStream *stream;
	int			slice;
	int			nouts;			/* a receiver each, in the slice's order */
	ProxyOut   *outs;
} ProxySender;

/*
 * The statement's connections here (GpIcStream's state), for its end: the
 * receivers', read to the end then, and every one closed.
 */
typedef struct ProxyState
{
	List	   *receivers;		/* ProxyReceiver */
	List	   *senders;		/* ProxySender */
} ProxyState;

static uint32
proxy_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryInterconnectProxy");
	return event;
}

int
GpIcProxyPacketSize(void)
{
	int			size = pg_strtoint32(GetConfigOptionResetString("gp.max_packet_size"));

	return Min(size, PG_UINT16_MAX - (int) sizeof(ICProxyPkt));
}

/* ------------------------------------------------------------------------- */
/* A connection to the proxy                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Where this process receives, which a proxy's peers are told of by its
 * connections: this node's dbid and content id, and the process.  The node
 * has to be among the proxy's addresses, or no proxy reaches it.
 */
static const char *
proxy_address(void)
{
	char		mine[32];

	if (proxy_address_string != NULL)
		return proxy_address_string;
	if (GpClusterSelf() == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the interconnect needs a cluster")));
	snprintf(mine, sizeof(mine), "%d:%d:", GpClusterDbid(), GpClusterContentId());
	if (gp_interconnect_proxy_addresses == NULL ||
		(strncmp(gp_interconnect_proxy_addresses, mine, strlen(mine)) != 0 &&
		 strstr(gp_interconnect_proxy_addresses, psprintf(",%s", mine)) == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("gp.interconnect_proxy_addresses has no address for this node, dbid %d",
						GpClusterDbid()),
				 errhint("List every node's proxy, \"dbid:content:host:port\", in gp.interconnect_proxy_addresses on every node.")));
	proxy_address_string =
		MemoryContextStrdup(TopMemoryContext,
							psprintf("proxy:%d:%d:%d", GpClusterDbid(),
									 GpClusterContentId(), MyProcPid));
	return proxy_address_string;
}

/* Wait for a socket, or a latch; interrupts taken. */
static void
proxy_wait(pgsocket sock, int events, long timeout)
{
	int			ev = WaitLatchOrSocket(MyLatch,
									   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
									   (timeout >= 0 ? WL_TIMEOUT : 0) | events,
									   sock, timeout, proxy_wait_event());

	if (ev & WL_LATCH_SET)
		ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

/* All of a message, written, waiting for room as needed. */
static void
proxy_write_all(pgsocket sock, const char *data, int len)
{
	while (len > 0)
	{
		ssize_t		n = send(sock, data, len, MSG_NOSIGNAL);

		if (n > 0)
		{
			data += n;
			len -= n;
		}
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			proxy_wait(sock, WL_SOCKET_WRITEABLE, -1);
		else
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect: could not write to the node's proxy: %m")));
	}
}

/*
 * A logical connection to the process "remote" (its proxy address), of
 * slice "send_slice" of the statement, which "recv_slice" receives: this
 * node's proxy connected, and told who is at each end with a HELLO, which
 * it answers once it has the connection.  A proxy not listening yet -- one
 * just started, or started again -- is tried again, until it listens or the
 * statement is cancelled, as Cloudberry's backend tries it again every
 * 100 ms.
 */
static pgsocket
proxy_connect(GpIcStream *stream, int send_slice, int recv_slice,
			  const char *remote)
{
	struct sockaddr_un addr;
	int			dbid;
	int			content;
	int			pid;
	ICProxyKey	key;
	ICProxyPkt	hello;
	ICProxyPkt	ack;
	int			got = 0;
	pgsocket	sock;

	if (sscanf(remote, "proxy:%d:%d:%d", &dbid, &content, &pid) != 3)
		elog(ERROR, "invalid proxy interconnect address \"%s\"", remote);
	ic_proxy_key_init(&key, stream->session, stream->serial,
					  (int16) send_slice, (int16) recv_slice,
					  (int16) GpClusterContentId(), (uint16) GpClusterDbid(),
					  MyProcPid, (int16) content, (uint16) dbid, pid);
	ic_proxy_message_init(&hello, IC_PROXY_MESSAGE_HELLO, &key);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	ic_proxy_build_server_sock_path(addr.sun_path, sizeof(addr.sun_path));
	for (;;)
	{
		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock == PGINVALID_SOCKET)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect: could not create a socket for the node's proxy: %m")));
		if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0)
			break;
		if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN &&
			errno != EINTR)
		{
			int			save = errno;

			closesocket(sock);
			errno = save;
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect: could not connect to the node's proxy at \"%s\": %m",
							addr.sun_path)));
		}
		closesocket(sock);
		proxy_wait(PGINVALID_SOCKET, 0, PROXY_CONNECT_RETRY_MS);
	}

	PG_TRY();
	{
		if (!pg_set_noblock(sock))
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect: could not make the proxy's socket nonblocking: %m")));
		proxy_write_all(sock, (const char *) &hello, sizeof(hello));
		while (got < (int) sizeof(ack))
		{
			ssize_t		n = recv(sock, ((char *) &ack) + got, sizeof(ack) - got, 0);

			if (n > 0)
				got += n;
			else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
				proxy_wait(sock, WL_SOCKET_READABLE, -1);
			else
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 errmsg("interconnect: the node's proxy closed the connection before its HELLO ACK")));
		}
		if (!ic_proxy_pkt_is_valid(&ack) ||
			!ic_proxy_pkt_is(&ack, IC_PROXY_MESSAGE_HELLO_ACK))
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect: the node's proxy answered its HELLO with %s",
							ic_proxy_pkt_to_str(&ack))));
	}
	PG_CATCH();
	{
		closesocket(sock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return sock;
}

static ProxyState *
proxy_state(GpIcStream *stream)
{
	if (stream->state == NULL)
		stream->state = MemoryContextAllocZero(GetMemoryChunkContext(stream),
											   sizeof(ProxyState));
	return (ProxyState *) stream->state;
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

static GpIcSender *
proxy_send_begin(GpIcStream *stream, GpIcSlice *slice)
{
	MemoryContext cxt = GetMemoryChunkContext(stream);
	ProxyState *state = proxy_state(stream);
	ProxySender *s = MemoryContextAllocZero(cxt, sizeof(ProxySender));
	MemoryContext oldcxt;

	s->stream = stream;
	s->slice = slice->slice;
	s->outs = MemoryContextAllocZero(cxt, sizeof(ProxyOut) * Max(slice->nreceivers, 1));
	for (int i = 0; i < slice->nreceivers; i++)
		s->outs[i].sock = PGINVALID_SOCKET;
	oldcxt = MemoryContextSwitchTo(cxt);
	state->senders = lappend(state->senders, s);
	MemoryContextSwitchTo(oldcxt);

	/* each one's connection kept as it is made, for an error to close */
	for (int i = 0; i < slice->nreceivers; i++)
	{
		ProxyOut   *out = &s->outs[i];

		out->sock = proxy_connect(stream, slice->slice, slice->parent,
								  slice->receiver_addresses[i]);
		s->nouts = i + 1;
		out->size = PROXY_FLUSH_BYTES + GpIcProxyPacketSize();
		out->buf = MemoryContextAlloc(cxt, out->size);
		out->packet = -1;
		out->wanted = true;
	}
	return &s->base;
}

/*
 * Has the receiver said STOP, or gone?  Read without waiting: what a
 * receiver writes its sender is only ever STOP, and the connection's end.
 */
static void
proxy_out_check_stop(ProxyOut *out)
{
	char		buf[64];
	ssize_t		n = recv(out->sock, buf, sizeof(buf), 0);

	if (n > 0 || n == 0 ||
		(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
		out->wanted = false;
}

/* The packet being filled, its header written: done. */
static void
proxy_out_seal(ProxyOut *out)
{
	uint32		len;

	if (out->packet < 0)
		return;
	len = (uint32) (out->len - out->packet);
	memcpy(out->buf + out->packet, &len, sizeof(len));
	out->packet = -1;
}

/*
 * What is held for a receiver, written, waiting for room as needed -- and
 * for a STOP, which the proxy may carry back meanwhile, or the connection's
 * end: the rest is not wanted then.
 */
static void
proxy_out_flush(ProxyOut *out)
{
	int			off = 0;

	proxy_out_seal(out);
	if (out->wanted)
		proxy_out_check_stop(out);
	while (out->wanted && off < out->len)
	{
		ssize_t		n = send(out->sock, out->buf + off, out->len - off, MSG_NOSIGNAL);

		if (n > 0)
			off += n;
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		{
			int			ev = WaitLatchOrSocket(MyLatch,
											   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
											   WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE,
											   out->sock, -1, proxy_wait_event());

			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			if (ev & WL_SOCKET_READABLE)
				proxy_out_check_stop(out);
		}
		else
			out->wanted = false;	/* the proxy closed the connection */
	}
	out->len = 0;
}

/* Bytes of the stream of frames, into packets no larger than the proxy's. */
static void
proxy_out_append(ProxyOut *out, const char *data, int len)
{
	int			max = GpIcProxyPacketSize();

	while (out->wanted && len > 0)
	{
		int			n;

		if (out->packet < 0)
		{
			out->packet = out->len;
			out->len += PROXY_HEADER;
		}
		n = Min(len, max - (out->len - out->packet));
		memcpy(out->buf + out->len, data, n);
		out->len += n;
		data += n;
		len -= n;
		if (out->len - out->packet == max)
		{
			proxy_out_seal(out);
			if (out->len + max > out->size)
				proxy_out_flush(out);
		}
	}
	if (out->len >= PROXY_FLUSH_BYTES)
		proxy_out_flush(out);
}

static void
proxy_out_frame(ProxyOut *out, uint32 frame, const char *data, int len)
{
	uint32		nframe = pg_hton32(frame);

	proxy_out_append(out, (const char *) &nframe, sizeof(nframe));
	if (len > 0)
		proxy_out_append(out, data, len);
}

static void
proxy_send(GpIcSender *sender, int receiver, const char *data, int len)
{
	ProxySender *s = (ProxySender *) sender;

	if (receiver >= 0)
	{
		Assert(receiver < s->nouts);
		proxy_out_frame(&s->outs[receiver], (uint32) len, data, len);
		return;
	}
	for (int i = 0; i < s->nouts; i++)
		proxy_out_frame(&s->outs[i], (uint32) len, data, len);
}

static bool
proxy_send_wanted(GpIcSender *sender)
{
	ProxySender *s = (ProxySender *) sender;

	for (int i = 0; i < s->nouts; i++)
		if (s->outs[i].wanted)
			return true;
	return false;
}

/*
 * The end, to every receiver still there, and each connection closed: its
 * proxy's BYE tells the receiver it is over.
 */
static void
proxy_send_end(GpIcSender *sender)
{
	ProxySender *s = (ProxySender *) sender;

	for (int i = 0; i < s->nouts; i++)
	{
		ProxyOut   *out = &s->outs[i];

		proxy_out_frame(out, GP_IC_END_OF_ROWS, NULL, 0);
		proxy_out_flush(out);
		closesocket(out->sock);
		out->sock = PGINVALID_SOCKET;
	}
}

/* ------------------------------------------------------------------------- */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * The connections of every sender of the slice.  They are made as the
 * Motion begins to read; what a sender sent before waits in the proxy.
 */
static GpIcReceiver *
proxy_recv_begin(GpIcStream *stream, GpIcSlice *slice)
{
	MemoryContext cxt = GetMemoryChunkContext(stream);
	ProxyState *state = proxy_state(stream);
	ProxyReceiver *r = MemoryContextAllocZero(cxt, sizeof(ProxyReceiver));
	MemoryContext oldcxt;

	r->stream = stream;
	r->slice = slice->slice;
	r->ins = MemoryContextAllocZero(cxt, sizeof(ProxyIn) * Max(slice->nsenders, 1));
	r->wes_one = -1;
	for (int i = 0; i < slice->nsenders; i++)
		r->ins[i].sock = PGINVALID_SOCKET;
	oldcxt = MemoryContextSwitchTo(cxt);
	state->receivers = lappend(state->receivers, r);
	MemoryContextSwitchTo(oldcxt);

	for (int i = 0; i < slice->nsenders; i++)
	{
		r->ins[i].sock = proxy_connect(stream, slice->slice, slice->parent,
									   slice->sender_addresses[i]);
		r->nins = i + 1;
	}
	return &r->base;
}

/* Room for "len" more bytes of a sender's stream. */
static void
proxy_in_room(ProxyReceiver *r, ProxyIn *in, int len)
{
	if (in->start > 0 && in->start == in->end)
		in->start = in->end = 0;
	if (in->size - in->end >= len)
		return;
	if (in->start > 0)
	{
		memmove(in->buf, in->buf + in->start, in->end - in->start);
		in->end -= in->start;
		in->start = 0;
	}
	if (in->size - in->end < len)
	{
		int			size = Max(Max(in->size * 2, in->end + len), 64 * 1024);

		in->buf = in->buf == NULL
			? MemoryContextAlloc(GetMemoryChunkContext(r), size)
			: repalloc(in->buf, size);
		in->size = size;
	}
}

/*
 * What has come on a connection, without waiting: the packets' headers
 * taken off, their bytes after the stream's.  True if anything came, or
 * its end; a stopped connection's bytes are dropped.
 */
static bool
proxy_in_read(ProxyReceiver *r, ProxyIn *in)
{
	static char buf[64 * 1024];
	ssize_t		n;
	char	   *p = buf;

	if (in->closed)
		return false;
	n = recv(in->sock, buf, sizeof(buf), 0);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return false;
	if (n <= 0)
	{
		in->closed = true;
		if (r->wes != NULL)
			FreeWaitEventSet(r->wes);
		r->wes = NULL;
		return true;
	}
	while (n > 0)
	{
		int			take;

		if (in->left == 0)
		{
			take = Min(n, PROXY_HEADER - in->headerlen);
			memcpy(in->header + in->headerlen, p, take);
			in->headerlen += take;
			p += take;
			n -= take;
			if (in->headerlen < PROXY_HEADER)
				break;
			memcpy(&in->left, in->header, sizeof(in->left));
			in->headerlen = 0;
			if (in->left < PROXY_HEADER)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 errmsg("interconnect: a packet of slice %d from the proxy is %u bytes long",
								r->slice, in->left)));
			in->left -= PROXY_HEADER;
			continue;
		}
		take = (int) Min((uint32) n, in->left);
		if (!in->stopped)
		{
			proxy_in_room(r, in, take);
			memcpy(in->buf + in->end, p, take);
			in->end += take;
		}
		in->left -= take;
		p += take;
		n -= take;
	}
	return true;
}

/* A frame whole among a sender's rows, taken: 0 none, 1 a row, 2 its end. */
static int
proxy_in_take(ProxyIn *in, char **data, int *len)
{
	uint32		frame;

	if (in->end - in->start < (int) sizeof(uint32))
		return 0;
	memcpy(&frame, in->buf + in->start, sizeof(uint32));
	frame = pg_ntoh32(frame);
	if (frame == GP_IC_END_OF_ROWS)
	{
		in->start += sizeof(uint32);
		in->ended = true;
		return 2;
	}
	if (in->end - in->start < (int) (sizeof(uint32) + frame))
		return 0;
	*data = in->buf + in->start + sizeof(uint32);
	*len = (int) frame;
	in->start += sizeof(uint32) + frame;
	return 1;
}

/* A sender's connection ended before its rows did. */
static void
proxy_in_lost(ProxyReceiver *r)
{
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: a sender of slice %d stopped before its last row",
					r->slice)));
}

/*
 * Wait for the connections still open -- or for one, "one" -- to have
 * something, or for the latch.
 */
static void
proxy_recv_wait(ProxyReceiver *r, int one)
{
	WaitEvent	occurred[1];

	if (r->wes == NULL || r->wes_one != one)
	{
		if (r->wes != NULL)
			FreeWaitEventSet(r->wes);
		r->wes = CreateWaitEventSet(NULL, r->nins + 2);
		AddWaitEventToSet(r->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		AddWaitEventToSet(r->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
		for (int i = 0; i < r->nins; i++)
			if (!r->ins[i].closed && (one < 0 || i == one))
				AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, r->ins[i].sock,
								  NULL, NULL);
		r->wes_one = one;
	}
	if (WaitEventSetWait(r->wes, -1, occurred, 1, proxy_wait_event()) > 0 &&
		(occurred[0].events & WL_LATCH_SET))
		ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

static bool
proxy_recv(GpIcReceiver *receiver, char **data, int *len)
{
	ProxyReceiver *r = (ProxyReceiver *) receiver;

	for (;;)
	{
		bool		progress = false;

		/* a row already here, the senders taking turns */
		for (int k = 0; k < r->nins; k++)
		{
			int			i = (r->next + k) % r->nins;
			ProxyIn    *in = &r->ins[i];
			int			got;

			if (in->ended)
				continue;
			got = proxy_in_take(in, data, len);
			if (got == 1)
			{
				r->next = (i + 1) % r->nins;
				return true;
			}
			if (got == 2)
			{
				r->nended++;
				progress = true;
			}
			else if (in->closed)
				proxy_in_lost(r);
		}
		if (r->nended == r->nins)
			return false;
		if (progress)
			continue;

		for (int i = 0; i < r->nins; i++)
			if (!r->ins[i].ended && proxy_in_read(r, &r->ins[i]))
				progress = true;
		if (!progress)
			proxy_recv_wait(r, -1);
	}
}

static bool
proxy_recv_from(GpIcReceiver *receiver, int k, char **data, int *len)
{
	ProxyReceiver *r = (ProxyReceiver *) receiver;
	ProxyIn    *in = &r->ins[k];

	Assert(k >= 0 && k < r->nins);
	for (;;)
	{
		int			got;

		if (in->ended)
			return false;
		got = proxy_in_take(in, data, len);
		if (got == 1)
			return true;
		if (got == 2)
		{
			r->nended++;
			return false;
		}
		if (in->closed)
			proxy_in_lost(r);
		if (!proxy_in_read(r, in))
			proxy_recv_wait(r, k);
	}
}

/*
 * Done: a sender whose end has not come is told STOP, and its connection
 * kept for the statement's end, which reads it to its end (stmt_end).
 */
static void
proxy_recv_end(GpIcReceiver *receiver)
{
	ProxyReceiver *r = (ProxyReceiver *) receiver;

	for (int i = 0; i < r->nins; i++)
	{
		ProxyIn    *in = &r->ins[i];

		if (!in->ended && !in->closed)
		{
			char		packet[PROXY_HEADER + 1];
			uint32		len = sizeof(packet);

			memcpy(packet, &len, PROXY_HEADER);
			packet[PROXY_HEADER] = PROXY_STOP;
			proxy_write_all(in->sock, packet, sizeof(packet));
		}
		in->stopped = true;
		if (in->buf != NULL)
			pfree(in->buf);
		in->buf = NULL;
		in->start = in->end = in->size = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* A statement                                                               */
/* ------------------------------------------------------------------------- */

/* Every connection of the statement's here, closed. */
static void
proxy_close_all(ProxyState *state)
{
	foreach_ptr(ProxyReceiver, r, state->receivers)
	{
		for (int i = 0; i < r->nins; i++)
			if (r->ins[i].sock != PGINVALID_SOCKET)
				closesocket(r->ins[i].sock);
		if (r->wes != NULL)
			FreeWaitEventSet(r->wes);
	}
	foreach_ptr(ProxySender, s, state->senders)
		for (int i = 0; i < s->nouts; i++)
			if (s->outs[i].sock != PGINVALID_SOCKET)
				closesocket(s->outs[i].sock);
}

/*
 * The statement's end: every receiver's connections read to their end --
 * each sender closes its own, and its proxy says so -- and every connection
 * closed, whatever the reading meets.  At an error, closed at once.
 */
static void
proxy_stmt_end(GpIcStream *stream, bool error)
{
	ProxyState *state = (ProxyState *) stream->state;

	if (state == NULL)
		return;
	stream->state = NULL;

	if (error)
	{
		proxy_close_all(state);
		return;
	}
	PG_TRY();
	{
		foreach_ptr(ProxyReceiver, r, state->receivers)
		{
			for (int i = 0; i < r->nins; i++)
			{
				ProxyIn    *in = &r->ins[i];

				in->stopped = true;
				while (!in->closed)
				{
					if (!proxy_in_read(r, in))
						proxy_recv_wait(r, i);
				}
			}
		}
	}
	PG_FINALLY();
	{
		proxy_close_all(state);
	}
	PG_END_TRY();
}

static const GpIcTransport proxy_transport = {
	.name = "proxy",
	.address = proxy_address,
	.send_begin = proxy_send_begin,
	.send = proxy_send,
	.send_wanted = proxy_send_wanted,
	.send_end = proxy_send_end,
	.recv_begin = proxy_recv_begin,
	.recv = proxy_recv,
	.recv_from = proxy_recv_from,
	.recv_end = proxy_recv_end,
	.stmt_end = proxy_stmt_end,
};

void
GpIcProxyInit(void)
{
	GpIcRegisterTransport(&proxy_transport);
}
