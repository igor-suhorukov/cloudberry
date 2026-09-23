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
 * gp_ic.c
 *	  The interconnect: a Motion's rows from the segment processes that send
 *	  them to the ones that receive them.
 *
 * Cloudberry's TCP interconnect, in its essentials: each segment process
 * that receives listens on a socket of its own, which the dispatcher learns
 * and hands the senders; a sender connects to every receiver of its slice,
 * says which statement and slice it is, and streams its rows, each receiver
 * taking them from all its senders as they come; a receiver that needs no
 * more rows closes, and its senders stop sending to it.  What is left out is
 * what Cloudberry's transport modules add around that -- UDP, a proxy, the
 * ack protocol -- and the chunk format: a row travels as the relay sent it
 * (gp_motion.c), each column by its type's send or output function, framed
 * by its length.
 *
 * The listener is where the node's own clients reach it: a socket file
 * beside the node's, when the cluster names nodes by the directory of their
 * socket as the test clusters do, and otherwise a TCP port on the node's
 * host.  Anybody who can reach it can connect, so a sender shows the
 * statement's token first -- a random value the coordinator gave both ends
 * over their authenticated connections -- and a connection that does not
 * know it is never read from.
 *
 * A connection can arrive before the fragment that receives it has started:
 * the coordinator starts every slice at once.  It waits among the unclaimed
 * ones until its receiver asks for it, or its statement ends here.
 *
 * Cloudberry sources this file stands in for:
 *	  contrib/interconnect/tcp/ic_tcp.c, src/backend/cdb/motion/cdbmotion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_ic.h"

/* What a sender says first. */
#define IC_MAGIC		"GPIC"
#define IC_VERSION		1
typedef struct IcHandshake
{
	char		magic[4];
	uint32		version;
	char		token[GP_IC_TOKEN_LEN];
	uint32		slice;
	int32		sender;
} IcHandshake;

/* A row's frame: its length, or this for the end of a sender's rows. */
#define IC_END_OF_ROWS	0xFFFFFFFF

/* How much a sender holds for a receiver before it sends. */
#define IC_FLUSH_BYTES	(64 * 1024)

/* One sender's connection, as a receiver has it. */
typedef struct IcIn
{
	pgsocket	sock;
	IcHandshake hs;
	int			hslen;			/* how much of the handshake has come */
	char	   *buf;
	int			bufsize;
	int			start;			/* the next unread byte */
	int			end;			/* one past the last byte read */
	bool		ended;			/* its last row has come */
} IcIn;

struct GpIcReceiver
{
	char		token[GP_IC_TOKEN_LEN + 1];
	int			slice;
	int			nsenders;
	List	   *conns;			/* IcIn */
	int			nended;
	int			next;			/* whose row to look at first */
	WaitEventSet *wes;
	bool		wes_stale;		/* a connection came since it was built */
};

/* One receiver's connection, as a sender has it. */
typedef struct IcOut
{
	pgsocket	sock;
	char	   *buf;
	int			len;
	int			size;
	bool		wanted;			/* the receiver has not gone */
} IcOut;

struct GpIcSender
{
	int			slice;
	int			nreceivers;
	IcOut	   *outs;
};

static pgsocket listen_sock = PGINVALID_SOCKET;
static char *listen_address = NULL;
static char *listen_path = NULL;

/* In TopMemoryContext; what an error leaves here, the transaction's end closes. */
static List *unclaimed = NIL;	/* IcIn */
static List *receivers = NIL;	/* GpIcReceiver */
static List *senders = NIL;		/* GpIcSender */

static uint32
ic_wait_event(bool send)
{
	static uint32 send_event = 0;
	static uint32 recv_event = 0;

	if (send && send_event == 0)
		send_event = WaitEventExtensionNew("CloudberryInterconnectSend");
	if (!send && recv_event == 0)
		recv_event = WaitEventExtensionNew("CloudberryInterconnectReceive");
	return send ? send_event : recv_event;
}

/* ------------------------------------------------------------------------- */
/* The listener                                                              */
/* ------------------------------------------------------------------------- */

static void
ic_remove_socket_file(int code, Datum arg)
{
	if (listen_path != NULL)
		unlink(listen_path);
}

static void
ic_listen_unix(const char *dir)
{
	struct sockaddr_un addr;
	char	   *path = psprintf("%s/.s.GPIC.%d", dir, MyProcPid);

	if (strlen(path) >= sizeof(addr.sun_path))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect socket path \"%s\" is too long", path)));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

	listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_sock == PGINVALID_SOCKET)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not create the interconnect socket: %m")));
	unlink(path);				/* a process of this pid that went before */
	if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		int			save = errno;

		closesocket(listen_sock);
		listen_sock = PGINVALID_SOCKET;
		errno = save;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not bind the interconnect socket \"%s\": %m",
						path)));
	}
	listen_path = MemoryContextStrdup(TopMemoryContext, path);
	on_proc_exit(ic_remove_socket_file, 0);
	listen_address = MemoryContextStrdup(TopMemoryContext,
										 psprintf("unix:%s", path));
}

static void
ic_listen_tcp(const char *host)
{
	struct addrinfo hints;
	struct addrinfo *res;
	struct sockaddr_storage addr;
	socklen_t	addrlen = sizeof(addr);
	char		port[NI_MAXSERV];
	int			rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(host, "0", &hints, &res);
	if (rc != 0 || res == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not resolve \"%s\" for the interconnect: %s",
						host, gai_strerror(rc))));

	listen_sock = socket(res->ai_family, SOCK_STREAM, 0);
	if (listen_sock == PGINVALID_SOCKET ||
		bind(listen_sock, res->ai_addr, res->ai_addrlen) < 0 ||
		getsockname(listen_sock, (struct sockaddr *) &addr, &addrlen) < 0 ||
		getnameinfo((struct sockaddr *) &addr, addrlen, NULL, 0, port,
					sizeof(port), NI_NUMERICSERV) != 0)
	{
		int			save = errno;

		if (listen_sock != PGINVALID_SOCKET)
			closesocket(listen_sock);
		listen_sock = PGINVALID_SOCKET;
		freeaddrinfo(res);
		errno = save;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not open the interconnect port on \"%s\": %m",
						host)));
	}
	freeaddrinfo(res);
	listen_address = MemoryContextStrdup(TopMemoryContext,
										 psprintf("tcp:%s:%s", host, port));
}

const char *
GpIcAddress(void)
{
	const GpSegmentConfig *self = GpClusterSelf();

	if (listen_address != NULL)
		return listen_address;

	if (self == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the interconnect needs a cluster")));

	if (self->hostname[0] == '/')
		ic_listen_unix(self->hostname);
	else
		ic_listen_tcp(self->hostname);

	if (listen(listen_sock, 256) < 0 || !pg_set_noblock(listen_sock))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not listen on the interconnect socket: %m")));
	return listen_address;
}

/* ------------------------------------------------------------------------- */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------- */

static void
in_close(IcIn *in)
{
	if (in->sock != PGINVALID_SOCKET)
		closesocket(in->sock);
	in->sock = PGINVALID_SOCKET;
	if (in->buf != NULL)
		pfree(in->buf);
	pfree(in);
}

/* A connection whose handshake has come: to its receiver, if it has one. */
static void
in_route(IcIn *in)
{
	ListCell   *lc;

	foreach(lc, receivers)
	{
		GpIcReceiver *r = (GpIcReceiver *) lfirst(lc);

		if (r->slice == (int) in->hs.slice &&
			memcmp(r->token, in->hs.token, GP_IC_TOKEN_LEN) == 0)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

			r->conns = lappend(r->conns, in);
			r->wes_stale = true;
			MemoryContextSwitchTo(oldcxt);
			if (list_length(r->conns) > r->nsenders)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 errmsg("interconnect: slice %d has more senders than the %d it was given",
								r->slice, r->nsenders)));
			return;
		}
	}

	unclaimed = lappend(unclaimed, in);
}

/*
 * Take the connections that have arrived, and read what has come of the
 * handshakes of the ones taken earlier; each complete one goes to its
 * receiver.  Never waits.
 */
static void
ic_accept(void)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	List	   *waiting = NIL;
	ListCell   *lc;

	if (listen_sock != PGINVALID_SOCKET)
	{
		for (;;)
		{
			pgsocket	sock = accept(listen_sock, NULL, NULL);
			IcIn	   *in;

			if (sock == PGINVALID_SOCKET)
				break;			/* EAGAIN, or nothing we can do about it now */
			if (!pg_set_noblock(sock))
			{
				closesocket(sock);
				continue;
			}
			in = palloc0(sizeof(IcIn));
			in->sock = sock;
			unclaimed = lappend(unclaimed, in);
		}
	}

	/* the ones whose handshake is not complete */
	foreach(lc, unclaimed)
	{
		IcIn	   *in = (IcIn *) lfirst(lc);

		if (in->hslen < (int) sizeof(IcHandshake))
			waiting = lappend(waiting, in);
	}
	foreach(lc, waiting)
	{
		IcIn	   *in = (IcIn *) lfirst(lc);
		ssize_t		n;

		n = recv(in->sock, ((char *) &in->hs) + in->hslen,
				 sizeof(IcHandshake) - in->hslen, 0);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			continue;
		if (n <= 0)
		{
			unclaimed = list_delete_ptr(unclaimed, in);
			in_close(in);
			continue;
		}
		in->hslen += n;
		if (in->hslen < (int) sizeof(IcHandshake))
			continue;

		unclaimed = list_delete_ptr(unclaimed, in);
		in->hs.version = pg_ntoh32(in->hs.version);
		in->hs.slice = pg_ntoh32(in->hs.slice);
		in->hs.sender = (int32) pg_ntoh32((uint32) in->hs.sender);
		if (memcmp(in->hs.magic, IC_MAGIC, 4) != 0 ||
			in->hs.version != IC_VERSION)
		{
			in_close(in);		/* not one of ours */
			continue;
		}
		in_route(in);
	}
	list_free(waiting);
	MemoryContextSwitchTo(oldcxt);
}

GpIcReceiver *
GpIcRecvBegin(const char *token, int slice, int nsenders)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	GpIcReceiver *r = palloc0(sizeof(GpIcReceiver));
	List	   *mine = NIL;
	ListCell   *lc;

	Assert(strlen(token) == GP_IC_TOKEN_LEN);
	(void) GpIcAddress();		/* the coordinator asked for it already */

	memcpy(r->token, token, GP_IC_TOKEN_LEN + 1);
	r->slice = slice;
	r->nsenders = nsenders;
	r->wes_stale = true;
	receivers = lappend(receivers, r);

	/* the ones that came before we asked */
	foreach(lc, unclaimed)
	{
		IcIn	   *in = (IcIn *) lfirst(lc);

		if (in->hslen == sizeof(IcHandshake) && (int) in->hs.slice == slice &&
			memcmp(in->hs.token, token, GP_IC_TOKEN_LEN) == 0)
			mine = lappend(mine, in);
	}
	foreach(lc, mine)
	{
		unclaimed = list_delete_ptr(unclaimed, lfirst(lc));
		r->conns = lappend(r->conns, lfirst(lc));
	}
	list_free(mine);
	MemoryContextSwitchTo(oldcxt);
	return r;
}

/* A complete row in a connection's buffer: take it.  0 none, 1 row, 2 end. */
static int
in_take(IcIn *in, char **data, int *len)
{
	uint32		frame;

	if (in->end - in->start < (int) sizeof(uint32))
		return 0;
	memcpy(&frame, in->buf + in->start, sizeof(uint32));
	frame = pg_ntoh32(frame);
	if (frame == IC_END_OF_ROWS)
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

/* Read what has come on a connection, without waiting; true if anything. */
static bool
in_read(GpIcReceiver *r, IcIn *in)
{
	ssize_t		n;

	/* room: move what is unread to the front, and grow for a long row */
	if (in->start > 0 && in->start == in->end)
		in->start = in->end = 0;
	if (in->bufsize - in->end < 8192)
	{
		if (in->start > 0)
		{
			memmove(in->buf, in->buf + in->start, in->end - in->start);
			in->end -= in->start;
			in->start = 0;
		}
		if (in->bufsize - in->end < 8192)
		{
			int			size = Max(in->bufsize * 2, 64 * 1024);

			in->buf = in->buf == NULL
				? MemoryContextAlloc(TopMemoryContext, size)
				: repalloc(in->buf, size);
			in->bufsize = size;
		}
	}

	n = recv(in->sock, in->buf + in->end, in->bufsize - in->end, 0);
	if (n > 0)
	{
		in->end += n;
		return true;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return false;

	/* closed, or broken, before the end of its rows */
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: the sender of slice %d on segment %d stopped before its last row",
					r->slice, in->hs.sender)));
	return false;
}

static void
recv_wait(GpIcReceiver *r)
{
	WaitEvent	occurred[1];
	ListCell   *lc;

	if (r->wes_stale || r->wes == NULL)
	{
		int			n = 3 + list_length(r->conns) + list_length(unclaimed);

		if (r->wes != NULL)
			FreeWaitEventSet(r->wes);
		r->wes = CreateWaitEventSet(NULL, n);
		AddWaitEventToSet(r->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(r->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);
		AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, listen_sock, NULL, NULL);
		foreach(lc, r->conns)
		{
			IcIn	   *in = (IcIn *) lfirst(lc);

			if (!in->ended)
				AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, in->sock, NULL, NULL);
		}
		/* a handshake still coming could be one of ours */
		foreach(lc, unclaimed)
		{
			IcIn	   *in = (IcIn *) lfirst(lc);

			if (in->hslen < (int) sizeof(IcHandshake))
				AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, in->sock, NULL, NULL);
		}
		r->wes_stale = false;
	}

	/*
	 * A timeout, so that a handshake that finishes coming on a connection the
	 * set does not have is looked at again without its own event.
	 */
	if (WaitEventSetWait(r->wes, 100, occurred, 1, ic_wait_event(false)) > 0 &&
		(occurred[0].events & WL_LATCH_SET))
		ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

bool
GpIcRecv(GpIcReceiver *r, char **data, int *len)
{
	for (;;)
	{
		int			n = list_length(r->conns);
		bool		progress = false;
		int			before;

		/* a row already in hand, the senders taking turns */
		for (int k = 0; k < n; k++)
		{
			int			i = (r->next + k) % n;
			IcIn	   *in = (IcIn *) list_nth(r->conns, i);
			int			got;

			if (in->ended)
				continue;
			got = in_take(in, data, len);
			if (got == 1)
			{
				r->next = (i + 1) % n;
				return true;
			}
			if (got == 2)
			{
				r->nended++;
				r->wes_stale = true;
				progress = true;
			}
		}
		if (r->nended == r->nsenders)
			return false;
		if (progress)
			continue;

		foreach_ptr(IcIn, in, r->conns)
		{
			if (!in->ended && in_read(r, in))
				progress = true;
		}
		if (progress)
			continue;

		before = list_length(r->conns);
		ic_accept();
		if (list_length(r->conns) != before)
			continue;

		recv_wait(r);
	}
}

static void
receiver_free(GpIcReceiver *r)
{
	foreach_ptr(IcIn, in, r->conns)
		in_close(in);
	list_free(r->conns);
	if (r->wes != NULL)
		FreeWaitEventSet(r->wes);
	pfree(r);
}

void
GpIcRecvEnd(GpIcReceiver *r)
{
	receivers = list_delete_ptr(receivers, r);
	receiver_free(r);
}

void
GpIcForget(const char *token)
{
	List	   *keep = NIL;
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	foreach_ptr(IcIn, in, unclaimed)
	{
		if (in->hslen == sizeof(IcHandshake) &&
			memcmp(in->hs.token, token, GP_IC_TOKEN_LEN) == 0)
			in_close(in);
		else
			keep = lappend(keep, in);
	}
	list_free(unclaimed);
	unclaimed = keep;
	MemoryContextSwitchTo(oldcxt);
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

/* Connect to one receiver, waiting as long as it takes, interruptibly. */
static pgsocket
ic_connect(const char *address)
{
	pgsocket	sock = PGINVALID_SOCKET;
	int			rc = -1;

	if (strncmp(address, "unix:", 5) == 0)
	{
		struct sockaddr_un addr;

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strlcpy(addr.sun_path, address + 5, sizeof(addr.sun_path));
		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock != PGINVALID_SOCKET && pg_set_noblock(sock))
		{
			/* a full backlog is EAGAIN here, not a connect in progress */
			while ((rc = connect(sock, (struct sockaddr *) &addr,
								 sizeof(addr))) < 0 &&
				   (errno == EAGAIN || errno == EINTR))
			{
				(void) WaitLatch(MyLatch,
								 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 10, ic_wait_event(true));
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
			}
		}
	}
	else if (strncmp(address, "tcp:", 4) == 0)
	{
		char	   *host = pstrdup(address + 4);
		char	   *colon = strrchr(host, ':');
		struct addrinfo hints;
		struct addrinfo *res;

		if (colon == NULL)
			elog(ERROR, "invalid interconnect address \"%s\"", address);
		*colon = '\0';
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not resolve interconnect address \"%s\"",
							address)));
		sock = socket(res->ai_family, SOCK_STREAM, 0);
		if (sock != PGINVALID_SOCKET && pg_set_noblock(sock))
		{
			int			on = 1;

			(void) setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
			rc = connect(sock, res->ai_addr, res->ai_addrlen);
		}
		freeaddrinfo(res);
	}
	else
		elog(ERROR, "invalid interconnect address \"%s\"", address);

	if (rc < 0 && sock != PGINVALID_SOCKET && errno == EINPROGRESS)
	{
		/* a listener whose backlog is full, or TCP's handshake */
		for (;;)
		{
			int			err = 0;
			socklen_t	errlen = sizeof(err);
			int			ev;

			ev = WaitLatchOrSocket(MyLatch,
								   WL_LATCH_SET | WL_SOCKET_CONNECTED |
								   WL_EXIT_ON_PM_DEATH,
								   sock, -1, ic_wait_event(true));
			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			if (!(ev & WL_SOCKET_CONNECTED))
				continue;
			if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0)
				err = errno;
			if (err == 0)
			{
				rc = 0;
				break;
			}
			if (err != EINPROGRESS && err != EAGAIN)
			{
				errno = err;
				break;
			}
		}
	}

	if (rc < 0)
	{
		int			save = errno;

		if (sock != PGINVALID_SOCKET)
			closesocket(sock);
		errno = save;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect: could not connect to \"%s\": %m",
						address)));
	}
	return sock;
}

/* Send what is held for a receiver, waiting for room if need be. */
static void
out_flush(IcOut *out)
{
	int			off = 0;

	while (out->wanted && off < out->len)
	{
		ssize_t		n = send(out->sock, out->buf + off, out->len - off,
							 MSG_NOSIGNAL);

		if (n > 0)
		{
			off += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			int			ev = WaitLatchOrSocket(MyLatch,
											   WL_LATCH_SET | WL_SOCKET_WRITEABLE |
											   WL_EXIT_ON_PM_DEATH,
											   out->sock, -1,
											   ic_wait_event(true));

			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		if (n < 0 && errno == EINTR)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}

		/* It closed: it has what it needs, or it failed, and says so itself. */
		out->wanted = false;
	}
	out->len = 0;
}

static void
out_append(IcOut *out, uint32 frame, const char *data, int len)
{
	uint32		nframe = pg_hton32(frame);
	int			need = out->len + sizeof(uint32) + Max(len, 0);

	if (!out->wanted)
		return;
	if (need > out->size)
	{
		int			size = Max(need, Max(out->size * 2, IC_FLUSH_BYTES + 8192));

		out->buf = repalloc(out->buf, size);
		out->size = size;
	}
	memcpy(out->buf + out->len, &nframe, sizeof(uint32));
	if (len > 0)
		memcpy(out->buf + out->len + sizeof(uint32), data, len);
	out->len = need;
	if (out->len >= IC_FLUSH_BYTES)
		out_flush(out);
}

GpIcSender *
GpIcSendBegin(const char *token, int slice, int self, int nreceivers,
			  char **addresses)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	GpIcSender *s = palloc0(sizeof(GpIcSender));
	IcHandshake hs;

	Assert(strlen(token) == GP_IC_TOKEN_LEN);
	s->slice = slice;
	s->nreceivers = nreceivers;
	s->outs = palloc0_array(IcOut, Max(nreceivers, 1));
	for (int i = 0; i < nreceivers; i++)
	{
		s->outs[i].sock = PGINVALID_SOCKET;
		s->outs[i].size = IC_FLUSH_BYTES + 8192;
		s->outs[i].buf = palloc(s->outs[i].size);
	}
	senders = lappend(senders, s);
	MemoryContextSwitchTo(oldcxt);

	memcpy(hs.magic, IC_MAGIC, 4);
	hs.version = pg_hton32(IC_VERSION);
	memcpy(hs.token, token, GP_IC_TOKEN_LEN);
	hs.slice = pg_hton32((uint32) slice);
	hs.sender = (int32) pg_hton32((uint32) self);

	for (int i = 0; i < nreceivers; i++)
	{
		IcOut	   *out = &s->outs[i];

		out->sock = ic_connect(addresses[i]);
		out->wanted = true;
		memcpy(out->buf, &hs, sizeof(hs));
		out->len = sizeof(hs);
		out_flush(out);
	}
	return s;
}

void
GpIcSend(GpIcSender *s, int receiver, const char *data, int len)
{
	if (receiver >= 0)
	{
		Assert(receiver < s->nreceivers);
		out_append(&s->outs[receiver], (uint32) len, data, len);
		return;
	}
	for (int i = 0; i < s->nreceivers; i++)
		out_append(&s->outs[i], (uint32) len, data, len);
}

bool
GpIcSendWanted(GpIcSender *s)
{
	for (int i = 0; i < s->nreceivers; i++)
		if (s->outs[i].wanted)
			return true;
	return false;
}

static void
sender_free(GpIcSender *s)
{
	for (int i = 0; i < s->nreceivers; i++)
	{
		if (s->outs[i].sock != PGINVALID_SOCKET)
			closesocket(s->outs[i].sock);
		pfree(s->outs[i].buf);
	}
	pfree(s->outs);
	pfree(s);
}

void
GpIcSendEnd(GpIcSender *s)
{
	for (int i = 0; i < s->nreceivers; i++)
	{
		out_append(&s->outs[i], IC_END_OF_ROWS, NULL, 0);
		out_flush(&s->outs[i]);
	}
	senders = list_delete_ptr(senders, s);
	sender_free(s);
}

/* ------------------------------------------------------------------------- */
/* The end of a transaction                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Whatever an error left open is closed when the transaction ends; a sender
 * closed so, before its end, is the receiver's error.  At a commit there is
 * nothing left but connections nobody asked for.
 */
static void
ic_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
		return;

	foreach_ptr(GpIcSender, s, senders)
		sender_free(s);
	list_free(senders);
	senders = NIL;

	foreach_ptr(GpIcReceiver, r, receivers)
		receiver_free(r);
	list_free(receivers);
	receivers = NIL;

	foreach_ptr(IcIn, in, unclaimed)
		in_close(in);
	list_free(unclaimed);
	unclaimed = NIL;
}

void
GpIcInit(void)
{
	if (GpClusterIsSingleNode())
		return;
	RegisterXactCallback(ic_xact_callback, NULL);
}
