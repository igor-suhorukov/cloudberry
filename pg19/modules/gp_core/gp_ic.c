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
 * more rows closes, and its senders stop sending to it.  A row travels as
 * gp_motion.c makes it, framed by its length.
 *
 * And Cloudberry's UDP interconnect, udpifc, in its essentials
 * (ic_udpifc.c): each process has a datagram socket too, and the rows a
 * sender streams to a receiver travel in packets of at most
 * gp.max_packet_size bytes -- the same bytes, framed the same way, the
 * stream's offset of each packet's first byte in its header, which says
 * which statement, slice and sender it is.  The receiver keeps the bytes that
 * come in order, and acknowledges them, telling the sender the room it has
 * left: gp.interconnect_queue_depth packets' worth, which the sender does not
 * send beyond (flow control).  A packet not acknowledged within its time --
 * the round trip measured, as Cloudberry's RTT and RTO are, and doubled at
 * each retry -- is sent again (retransmission).  One that comes before its
 * turn the receiver keeps until the ones ahead of it come, and acknowledges
 * with the same acknowledgement again, which tells the sender that the one
 * whose turn it is was lost: sent again at once.  A sender whose receiver's
 * room stays shut, nothing of its own unacknowledged, asks it every 512 ms
 * what it has, lest the acknowledgement that opened it again was lost --
 * Cloudberry's deadlock check (checkDeadlock) -- and one that hears nothing
 * for gp.interconnect_transmit_timeout gives up, in Cloudberry's words.
 *
 * How much a sender has in flight is Cloudberry's flow control
 * (gp.interconnect_fc_method).  Under "capacity" each receiver's room is all
 * that bounds it, and at most gp.interconnect_snd_queue_depth packets wait to
 * be acknowledged by each.  Under "loss" -- and its variants loss_advance and
 * loss_timer, the same here -- a sender has a congestion window over all its
 * receivers, as Cloudberry's has (snd_control_info): a packet for each
 * receiver at first, one more for each packet acknowledged below the
 * threshold and a fraction of one above it, up to
 * gp.interconnect_snd_queue_depth for each receiver; a packet sent again
 * because another came before it halves the window, and one whose time ran
 * out closes it to where it began.  A receiver with nothing in flight may
 * always have one packet.  Packets whose time has run out are looked for at
 * most every gp.interconnect_timer_checking_period under the loss methods,
 * and a sender waiting on acknowledgements wakes at least every
 * gp.interconnect_timer_period.  A packet sent gp.interconnect_min_retries_
 * before_timeout times draws Cloudberry's WARNING once, and the transmit
 * timeout is an error only for a packet sent more times than that.  And
 * packets that come before the receiver they are for has begun wait for it
 * only while gp.interconnect_cache_future_packets is on; off, they are
 * dropped, and their sender sends them again.
 *
 * A receiver that needs no more rows says STOP, and the sender CLOSE, as it
 * does once its last row is acknowledged.  Cloudberry's receiver has a thread
 * that answers a packet that comes after it has gone; a backend has none, and
 * is deaf when it is idle -- as a segment's is between the coordinator's
 * FETCHes, and after its fragment's last row.  So a receiver whose senders
 * have all sent their last row waits until each has closed; and when a
 * fragment's plan has run out, every receiver it has is ended, and all of
 * their senders waited for, before the process idles -- each told STOP, again
 * and again, until it closes, and one not heard from yet waited for, as every
 * sender sends at least its end.  A CLOSE lost, the sender is done with once
 * it has been silent for a few times the longest a packet waits to be sent
 * again.  Which of the two transports a Motion uses the coordinator decides
 * (gp.interconnect_type), handing the senders each receiver's address for it.
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
 * The two are transports of a table, as Cloudberry's interconnects are
 * MotionIPCLayers of its (cdbmotion.c): a Motion sends and receives through
 * the functions of its statement's transport (gp_ic.h), which a module
 * registers too -- udp2's and the proxy's.  The table is at the level of a
 * row, which is what the port's Motions send, not of Cloudberry's tuple
 * chunks.  What the coordinator told a statement's processes -- its
 * transport, and each slice's senders and receivers and where they receive
 * -- each has as a stream (GpIcStream), from its fragment's start until its
 * end, or its (sub)transaction's.
 *
 * Cloudberry sources this file stands in for:
 *	  contrib/interconnect/tcp/ic_tcp.c, contrib/interconnect/udp/ic_udpifc.c,
 *	  src/backend/cdb/motion/cdbmotion.c
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
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_fault.h"
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
#define IC_END_OF_ROWS	GP_IC_END_OF_ROWS

/* How much a sender holds for a receiver before it sends. */
#define IC_FLUSH_BYTES	(64 * 1024)

/*
 * UDP: a packet's header, in network order -- magic, version, type, payload
 * length, the statement's token, the slice, the sender's content id, which of
 * its receivers it is sent to, the stream offset, the room -- and its types.
 */
#define IC_UDP_MAGIC		"GPIU"
#define IC_UDP_VERSION		1
#define IC_UDP_HEADER		64
#define IC_UDP_DATA			1	/* bytes of the stream, from a sender */
#define IC_UDP_STATUS		2	/* a sender asks what its receiver has */
#define IC_UDP_ACK			3	/* the bytes a receiver has, and its room */
#define IC_UDP_STOP			4	/* a receiver that needs no more */
#define IC_UDP_CLOSE		5	/* a sender acknowledged to its end, or stopped */

typedef struct IcUdpPacket
{
	uint8		type;
	uint16		len;
	char		token[GP_IC_TOKEN_LEN];
	uint32		slice;
	int32		sender;
	uint32		index;
	uint64		offset;			/* DATA: of its first byte; ACK: had in order */
	uint32		window;			/* ACK: bytes of room beyond it */
	const char *payload;
} IcUdpPacket;

/* A packet a sender keeps until its bytes are acknowledged. */
typedef struct IcUdpSent
{
	uint64		offset;
	int			len;
	TimestampTz sent_at;
	int			tries;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} IcUdpSent;

/* Cloudberry's bounds (ic_udpifc.c): MAX_EXPIRATION_PERIOD, DEADLOCK_CHECKING_TIME */
#define IC_UDP_MAX_RTO_US			(1000 * 1000)
#define IC_UDP_DEADLOCK_CHECK_US	(512 * 1000)

/* Streams no receiver has asked for yet, at most. */
#define IC_UDP_MAX_UNCLAIMED	1024

/*
 * How long a receiver's end waits on a sender whose CLOSE has not come: until
 * it has been silent a few times longer than one missing an acknowledgement
 * waits to send again (IC_UDP_MAX_RTO_US) -- telling it STOP this often, if
 * it was.
 */
#define IC_UDP_LINGER_US		(3 * IC_UDP_MAX_RTO_US)
#define IC_UDP_STOP_AGAIN_US	(50 * 1000)

/* A receiver's motion here that has gone: a sender's packets get a STOP. */
typedef struct IcUdpEnded
{
	char		token[GP_IC_TOKEN_LEN];
	uint32		slice;
} IcUdpEnded;

/* udpifc's settings, which udp2 reads too (gp_ic.h), as Cloudberry names them */
int			gp_interconnect_queue_depth = 4;
int			gp_max_packet_size = 8192;
int			gp_interconnect_transmit_timeout = 3600;
int			gp_interconnect_min_rto = 20;
int			gp_interconnect_default_rtt = 20;
int			gp_interconnect_snd_queue_depth = 2;
int			gp_interconnect_min_retries_before_timeout = 100;
int			gp_interconnect_debug_retry_interval = 10;
bool		gp_interconnect_cache_future_packets = true;
int			gp_interconnect_timer_period = 5;
int			gp_interconnect_timer_checking_period = 20;

/* Cloudberry's flow control methods, by its numbers (cdbvars.h) */
#define IC_FC_CAPACITY		0
#define IC_FC_LOSS			2
#define IC_FC_LOSS_ADVANCE	3
#define IC_FC_LOSS_TIMER	4
int			gp_interconnect_fc_method = IC_FC_LOSS;

static const struct config_enum_entry fc_methods[] = {
	{"loss", IC_FC_LOSS, false},
	{"capacity", IC_FC_CAPACITY, false},
	{"loss_advance", IC_FC_LOSS_ADVANCE, false},
	{"loss_timer", IC_FC_LOSS_TIMER, false},
	{NULL, 0, false}
};

#define IC_FC_BY_LOSS() (gp_interconnect_fc_method != IC_FC_CAPACITY)

/* and its tests': packets dropped as they would be sent, as lost ones are */
int			gp_udpic_dropacks_percent = 0;
int			gp_udpic_dropxmit_percent = 0;

/* How much the transports say of what they do, Cloudberry's gp_log_interconnect */
int			gp_log_interconnect = GP_IC_VERBOSITY_TERSE;

static const struct config_enum_entry log_interconnect_options[] = {
	{"terse", GP_IC_VERBOSITY_TERSE, false},
	{"off", GP_IC_VERBOSITY_OFF, false},
	{"verbose", GP_IC_VERBOSITY_VERBOSE, false},
	{"debug", GP_IC_VERBOSITY_DEBUG, false},
	{NULL, 0, false}
};

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

	/* UDP: the bytes come in packets (udp_poll()), bufsize the room */
	bool		udp;
	uint64		udp_recv;		/* bytes had, in order */
	int			udp_advertised; /* the room the last ACK told of */
	uint32		udp_index;		/* which of its sender's receivers it is */
	struct sockaddr_storage udp_peer;	/* the sender, to answer */
	socklen_t	udp_peerlen;
	List	   *udp_waiting;	/* IcUdpSent come before their turn, by offset */
	bool		udp_stopped;	/* told STOP */
	TimestampTz udp_stop_at;	/* when last */
	bool		udp_closed;		/* its sender said CLOSE */
	TimestampTz udp_last;		/* its sender's last packet, or its first STOP */
} IcIn;

typedef struct IcReceiver
{
	GpIcReceiver base;		/* tcp's or udpifc's */
	char		token[GP_IC_TOKEN_LEN + 1];
	int			slice;
	int			nsenders;
	bool		udp;			/* its senders send in UDP packets */
	bool		done;			/* UDP: ended, its senders not yet all closed */
	List	   *conns;			/* IcIn */
	int			nended;
	int			next;			/* whose row to look at first */
	WaitEventSet *wes;
	bool		wes_stale;		/* a connection came since it was built */
	WaitEventSet *wes_one;		/* ic_recv_from()'s: one sender's socket */
	IcIn	   *wes_one_in;		/* which */
	int			wes_one_conns;	/* and how many had come, and were */
	int			wes_one_unclaimed;	/* unclaimed, when it was built */
} IcReceiver;

/* One receiver's connection, as a sender has it. */
typedef struct IcOut
{
	pgsocket	sock;
	char	   *buf;
	int			len;
	int			size;
	bool		wanted;			/* the receiver has not gone */

	/* UDP */
	bool		udp;
	uint32		index;			/* which receiver it is, echoed in its ACKs */
	struct sockaddr_storage addr;
	socklen_t	addrlen;
	const char *address;
	uint64		sent;			/* bytes put in packets */
	uint64		acked;			/* bytes the receiver has, in order */
	int64		window;			/* bytes beyond them it has room for */
	List	   *unacked;		/* IcUdpSent, in order */
	int64		srtt;			/* the round trip, in microseconds */
	int64		rttvar;
	int64		rto;			/* how long a packet waits to be acknowledged */
	TimestampTz last_heard;		/* its last word */
	TimestampTz last_query;		/* the last status query sent it */
} IcOut;

typedef struct IcSender
{
	GpIcSender	base;		/* tcp's or udpifc's */
	int			slice;
	int			nreceivers;
	IcOut	   *outs;
	char		token[GP_IC_TOKEN_LEN];
	int			self;

	/* UDP, under the loss methods: the congestion window, in packets */
	double		cwnd;
	double		ssthresh;
	int			mincwnd;		/* a packet for each UDP receiver */
	int			maxcwnd;		/* gp.interconnect_snd_queue_depth for each */
	int			inflight;		/* packets sent, not yet acknowledged */
	TimestampTz last_check;		/* when the unacknowledged were last looked at */
	bool		warned;			/* Cloudberry's WARNING of retries given */
} IcSender;

static pgsocket listen_sock = PGINVALID_SOCKET;
static char *listen_address = NULL;
static char *listen_path = NULL;

static pgsocket udp_sock = PGINVALID_SOCKET;
static char *udp_address = NULL;
static char *udp_path = NULL;
static List *udp_ended = NIL;	/* IcUdpEnded, in TopMemoryContext */

/* In TopMemoryContext; what an error leaves here, the transaction's end closes. */
static List *unclaimed = NIL;	/* IcIn */
static List *receivers = NIL;	/* IcReceiver */
static List *senders = NIL;		/* IcSender */

static bool udp_poll(void);

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
	if (udp_path != NULL)
		unlink(udp_path);
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

/*
 * UDP's socket: a datagram socket beside the node's own, when the cluster
 * names nodes by the directory of their socket, and otherwise a UDP port on
 * the node's host; the room of its buffers made as large as the system lets.
 */
static void
ic_udp_open(const GpSegmentConfig *self)
{
	int			bufsize = 2 * 1024 * 1024;

	if (self->hostname[0] == '/')
	{
		struct sockaddr_un addr;
		char	   *path = psprintf("%s/.s.GPICU.%d", self->hostname, MyProcPid);

		if (strlen(path) >= sizeof(addr.sun_path))
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("interconnect socket path \"%s\" is too long", path)));
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
		udp_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (udp_sock != PGINVALID_SOCKET)
			unlink(path);
		if (udp_sock == PGINVALID_SOCKET ||
			bind(udp_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		{
			int			save = errno;

			if (udp_sock != PGINVALID_SOCKET)
				closesocket(udp_sock);
			udp_sock = PGINVALID_SOCKET;
			errno = save;
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not bind the interconnect's datagram socket \"%s\": %m",
							path)));
		}
		udp_path = MemoryContextStrdup(TopMemoryContext, path);
		udp_address = MemoryContextStrdup(TopMemoryContext,
										  psprintf("udpunix:%s", path));
	}
	else
	{
		struct addrinfo hints;
		struct addrinfo *res;
		struct sockaddr_storage addr;
		socklen_t	addrlen = sizeof(addr);
		char		port[NI_MAXSERV];
		int			rc;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		rc = getaddrinfo(self->hostname, "0", &hints, &res);
		if (rc != 0 || res == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not resolve \"%s\" for the interconnect: %s",
							self->hostname, gai_strerror(rc))));
		udp_sock = socket(res->ai_family, SOCK_DGRAM, 0);
		if (udp_sock == PGINVALID_SOCKET ||
			bind(udp_sock, res->ai_addr, res->ai_addrlen) < 0 ||
			getsockname(udp_sock, (struct sockaddr *) &addr, &addrlen) < 0 ||
			getnameinfo((struct sockaddr *) &addr, addrlen, NULL, 0, port,
						sizeof(port), NI_NUMERICSERV) != 0)
		{
			int			save = errno;

			if (udp_sock != PGINVALID_SOCKET)
				closesocket(udp_sock);
			udp_sock = PGINVALID_SOCKET;
			freeaddrinfo(res);
			errno = save;
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not open the interconnect's UDP port on \"%s\": %m",
							self->hostname)));
		}
		freeaddrinfo(res);
		udp_address = MemoryContextStrdup(TopMemoryContext,
										  psprintf("udp:%s:%s", self->hostname, port));
	}

	if (!pg_set_noblock(udp_sock))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not make the interconnect's datagram socket nonblocking: %m")));
	(void) setsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
	(void) setsockopt(udp_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
}

/*
 * Where this process receives, opened the first time: its listener, and its
 * datagram socket -- both, whichever of tcp and udpifc asks, as a receiver
 * waits on both.
 */
static void
ic_open(void)
{
	const GpSegmentConfig *self = GpClusterSelf();

	if (udp_address != NULL)
		return;

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

	ic_udp_open(self);
}

/* ------------------------------------------------------------------------- */
/* UDP: packets                                                              */
/* ------------------------------------------------------------------------- */

/* The room a receiver has for a sender's bytes: its queue's packets. */
static int
udp_room(void)
{
	return gp_interconnect_queue_depth * (gp_max_packet_size - IC_UDP_HEADER);
}

static char udp_buffer[65536];

/* One packet, to "to"; one that cannot go now is as good as lost. */
static void
udp_put(const struct sockaddr_storage *to, socklen_t tolen, uint8 type,
		const char *token, uint32 slice, int32 sender, uint32 index,
		uint64 offset, uint32 window, const char *payload, int len)
{
	char	   *p = udp_buffer;
	uint16		n16;
	uint32		n32;
	uint64		n64;

	memcpy(p, IC_UDP_MAGIC, 4);
	p[4] = IC_UDP_VERSION;
	p[5] = type;
	n16 = pg_hton16((uint16) len);
	memcpy(p + 6, &n16, 2);
	memcpy(p + 8, token, GP_IC_TOKEN_LEN);
	n32 = pg_hton32(slice);
	memcpy(p + 40, &n32, 4);
	n32 = pg_hton32((uint32) sender);
	memcpy(p + 44, &n32, 4);
	n32 = pg_hton32(index);
	memcpy(p + 48, &n32, 4);
	n64 = pg_hton64(offset);
	memcpy(p + 52, &n64, 8);
	n32 = pg_hton32(window);
	memcpy(p + 60, &n32, 4);
	if (len > 0)
		memcpy(p + IC_UDP_HEADER, payload, len);

	/* a test's lost packet, as Cloudberry's testmode_inject_fault() makes one */
	if ((type == IC_UDP_ACK && gp_udpic_dropacks_percent > 0 &&
		 (int) pg_prng_uint64_range(&pg_global_prng_state, 0, 99) < gp_udpic_dropacks_percent) ||
		(type == IC_UDP_DATA && gp_udpic_dropxmit_percent > 0 &&
		 (int) pg_prng_uint64_range(&pg_global_prng_state, 0, 99) < gp_udpic_dropxmit_percent))
		return;

	(void) sendto(udp_sock, udp_buffer, IC_UDP_HEADER + len, 0,
				  (const struct sockaddr *) to, tolen);
}

static bool
udp_parse(const char *buf, int n, IcUdpPacket *pkt)
{
	uint16		n16;
	uint32		n32;
	uint64		n64;

	if (n < IC_UDP_HEADER || memcmp(buf, IC_UDP_MAGIC, 4) != 0 ||
		buf[4] != IC_UDP_VERSION)
		return false;
	pkt->type = (uint8) buf[5];
	memcpy(&n16, buf + 6, 2);
	pkt->len = pg_ntoh16(n16);
	if (IC_UDP_HEADER + pkt->len != n)
		return false;
	memcpy(pkt->token, buf + 8, GP_IC_TOKEN_LEN);
	memcpy(&n32, buf + 40, 4);
	pkt->slice = pg_ntoh32(n32);
	memcpy(&n32, buf + 44, 4);
	pkt->sender = (int32) pg_ntoh32(n32);
	memcpy(&n32, buf + 48, 4);
	pkt->index = pg_ntoh32(n32);
	memcpy(&n64, buf + 52, 8);
	pkt->offset = pg_ntoh64(n64);
	memcpy(&n32, buf + 60, 4);
	pkt->window = pg_ntoh32(n32);
	pkt->payload = buf + IC_UDP_HEADER;
	return true;
}

static bool
udp_ended_has(const char *token, uint32 slice)
{
	foreach_ptr(IcUdpEnded, e, udp_ended)
		if (e->slice == slice && memcmp(e->token, token, GP_IC_TOKEN_LEN) == 0)
			return true;
	return false;
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
	list_free_deep(in->udp_waiting);
	pfree(in);
}

/* A connection whose handshake has come: to its receiver, if it has one. */
static IcReceiver *
in_route(IcIn *in)
{
	ListCell   *lc;

	foreach(lc, receivers)
	{
		IcReceiver *r = (IcReceiver *) lfirst(lc);

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
			return r;
		}
	}

	/* the transaction's end reads the list, whatever memory the caller is in */
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		unclaimed = lappend(unclaimed, in);
		MemoryContextSwitchTo(oldcxt);
	}
	return NULL;
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

		if (!in->udp && in->hslen < (int) sizeof(IcHandshake))
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
		(void) in_route(in);
	}
	list_free(waiting);
	MemoryContextSwitchTo(oldcxt);
}

/* UDP: a sender's stream here, claimed by its receiver ("owner") or not yet. */
static IcIn *
udp_find_in(const IcUdpPacket *pkt, IcReceiver **owner)
{
	*owner = NULL;
	foreach_ptr(IcReceiver, r, receivers)
	{
		if (r->slice != (int) pkt->slice ||
			memcmp(r->token, pkt->token, GP_IC_TOKEN_LEN) != 0)
			continue;
		foreach_ptr(IcIn, in, r->conns)
		{
			if (in->udp && in->hs.sender == pkt->sender)
			{
				*owner = r;
				return in;
			}
		}
	}
	foreach_ptr(IcIn, in, unclaimed)
		if (in->udp && (int) in->hs.slice == (int) pkt->slice &&
			in->hs.sender == pkt->sender &&
			memcmp(in->hs.token, pkt->token, GP_IC_TOKEN_LEN) == 0)
			return in;
	return NULL;
}

/*
 * A sender's next bytes, into its stream here if there is room.  A row
 * longer than the room -- gp.interconnect_queue_depth packets' worth --
 * could never be taken whole: the room grows to the row's length once the
 * length has come, and the acknowledgements tell the sender so, as
 * Cloudberry's receiver joins a long tuple's chunks in memory of its own.
 */
static bool
udp_take(IcIn *in, const char *data, int len)
{
	int			unread = in->end - in->start;
	uint32		frame;

	if (unread + len > in->bufsize)
		return false;
	if (in->bufsize - in->end < len)
	{
		memmove(in->buf, in->buf + in->start, unread);
		in->start = 0;
		in->end = unread;
	}
	memcpy(in->buf + in->end, data, len);
	in->end += len;
	in->udp_recv += len;

	unread = in->end - in->start;
	if (unread < (int) sizeof(uint32))
		return true;
	memcpy(&frame, in->buf + in->start, sizeof(uint32));
	frame = pg_ntoh32(frame);
	if (frame != IC_END_OF_ROWS && sizeof(uint32) + (uint64) frame > (uint64) in->bufsize)
	{
		memmove(in->buf, in->buf + in->start, unread);
		in->start = 0;
		in->end = unread;
		in->bufsize = sizeof(uint32) + frame;
		in->buf = repalloc(in->buf, in->bufsize);
	}
	return true;
}

/* What a receiver has had of a sender's bytes, and the room it has left. */
static void
udp_ack(IcIn *in)
{
	int			room = in->bufsize - (in->end - in->start);

	in->udp_advertised = room;
	udp_put(&in->udp_peer, in->udp_peerlen, IC_UDP_ACK, in->hs.token,
			in->hs.slice, in->hs.sender, in->udp_index, in->udp_recv,
			(uint32) room, NULL, 0);
}

/*
 * A sender's packet: its bytes kept, if they are the next in order and fit,
 * and acknowledged -- or a status query answered, or its CLOSE noted.  A
 * sender whose first packet this is has its stream made, which waits among
 * the unclaimed connections until its receiver asks; one whose receiver has
 * ended, before its last row, is told STOP.
 */
static void
udp_on_data(const IcUdpPacket *pkt, const struct sockaddr_storage *from,
			socklen_t fromlen)
{
	IcReceiver *r;
	IcIn	   *in = udp_find_in(pkt, &r);

	if (pkt->type == IC_UDP_CLOSE)
	{
		if (in != NULL)
		{
			in->udp_closed = true;
			in->udp_last = GetCurrentTimestamp();
		}
		return;
	}

	if (in == NULL)
	{
		MemoryContext oldcxt;

		if (udp_ended_has(pkt->token, pkt->slice))
		{
			udp_put(from, fromlen, IC_UDP_STOP, pkt->token, pkt->slice,
					pkt->sender, pkt->index, 0, 0, NULL, 0);
			return;
		}
		if (list_length(unclaimed) >= IC_UDP_MAX_UNCLAIMED)
			return;				/* it will come again */

		/*
		 * Before its receiver has begun: kept for it, as Cloudberry caches a
		 * future packet -- or, gp.interconnect_cache_future_packets off,
		 * dropped, to come again.
		 */
		if (!gp_interconnect_cache_future_packets)
		{
			bool		begun = false;

			foreach_ptr(IcReceiver, rr, receivers)
				if (rr->slice == (int) pkt->slice &&
					memcmp(rr->token, pkt->token, GP_IC_TOKEN_LEN) == 0)
					begun = true;
			if (!begun)
				return;
		}

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		in = palloc0(sizeof(IcIn));
		in->sock = PGINVALID_SOCKET;
		in->udp = true;
		memcpy(in->hs.magic, IC_MAGIC, 4);
		in->hs.version = IC_VERSION;
		memcpy(in->hs.token, pkt->token, GP_IC_TOKEN_LEN);
		in->hs.slice = pkt->slice;
		in->hs.sender = pkt->sender;
		in->hslen = sizeof(IcHandshake);
		in->bufsize = udp_room();
		in->buf = palloc(in->bufsize);
		MemoryContextSwitchTo(oldcxt);
		r = in_route(in);
	}
	memcpy(&in->udp_peer, from, fromlen);
	in->udp_peerlen = fromlen;
	in->udp_index = pkt->index;
	in->udp_last = GetCurrentTimestamp();

	/* its receiver ended before its last row: told to stop, and again */
	if (r != NULL && r->done && !in->ended && !in->udp_stopped)
	{
		in->udp_stopped = true;
		in->udp_stop_at = in->udp_last;
	}
	if (in->udp_stopped)
	{
		udp_put(from, fromlen, IC_UDP_STOP, pkt->token, pkt->slice,
				pkt->sender, pkt->index, in->udp_recv, 0, NULL, 0);
		return;
	}

	if (pkt->type == IC_UDP_DATA && pkt->len > 0)
	{
		uint64		room = (uint64) (in->bufsize - (in->end - in->start));

		if (pkt->offset == in->udp_recv)
		{
			/* its turn: taken, and the ones that came before theirs after it */
			if (udp_take(in, pkt->payload, pkt->len))
			{
				ListCell   *lc;

				foreach(lc, in->udp_waiting)
				{
					IcUdpSent  *w = (IcUdpSent *) lfirst(lc);

					if (w->offset + w->len <= in->udp_recv)
						;		/* had already */
					else if (w->offset != in->udp_recv ||
							 !udp_take(in, w->data, w->len))
						break;
					in->udp_waiting = foreach_delete_current(in->udp_waiting, lc);
					pfree(w);
				}
			}
		}
		else if (pkt->offset > in->udp_recv &&
				 pkt->offset + pkt->len <= in->udp_recv + room)
		{
			/* before its turn, one lost ahead of it: kept, in order, for it */
			ListCell   *lc;
			int			pos = 0;
			bool		had = false;
			IcUdpSent  *w;
			MemoryContext oldcxt;

			foreach(lc, in->udp_waiting)
			{
				IcUdpSent  *o = (IcUdpSent *) lfirst(lc);

				if (o->offset == pkt->offset)
					had = true;
				if (o->offset >= pkt->offset)
					break;
				pos++;
			}
			if (!had)
			{
				w = MemoryContextAlloc(TopMemoryContext,
									   offsetof(IcUdpSent, data) + pkt->len);
				w->offset = pkt->offset;
				w->len = pkt->len;
				w->tries = 0;
				memcpy(w->data, pkt->payload, pkt->len);
				oldcxt = MemoryContextSwitchTo(TopMemoryContext);
				in->udp_waiting = list_insert_nth(in->udp_waiting, pos, w);
				MemoryContextSwitchTo(oldcxt);
			}
		}
	}

	/*
	 * Acknowledged, each packet: one before its turn repeats the last
	 * acknowledgement, which tells the sender one is missing.
	 */
	udp_ack(in);
}

static IcReceiver *
ic_recv_begin(const char *token, int slice, int nsenders, bool udp)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	IcReceiver *r = palloc0(sizeof(IcReceiver));
	List	   *mine = NIL;
	ListCell   *lc;

	Assert(strlen(token) == GP_IC_TOKEN_LEN);
	ic_open();				/* the coordinator asked for it already */

	memcpy(r->token, token, GP_IC_TOKEN_LEN + 1);
	r->slice = slice;
	r->nsenders = nsenders;
	r->udp = udp;
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

	/* UDP: a packet's worth of room made since the sender was told, told of */
	if (in->udp &&
		in->bufsize - (in->end - in->start) - in->udp_advertised >=
		gp_max_packet_size - IC_UDP_HEADER)
		udp_ack(in);
	return 1;
}

/* Read what has come on a connection, without waiting; true if anything. */
static bool
in_read(IcReceiver *r, IcIn *in)
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
recv_wait(IcReceiver *r)
{
	WaitEvent	occurred[1];
	ListCell   *lc;

	if (r->wes_stale || r->wes == NULL)
	{
		int			n = 4 + list_length(r->conns) + list_length(unclaimed);

		if (r->wes != NULL)
			FreeWaitEventSet(r->wes);
		r->wes = CreateWaitEventSet(NULL, n);
		AddWaitEventToSet(r->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(r->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);
		AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, listen_sock, NULL, NULL);
		if (udp_sock != PGINVALID_SOCKET)
			AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, udp_sock, NULL, NULL);
		foreach(lc, r->conns)
		{
			IcIn	   *in = (IcIn *) lfirst(lc);

			if (!in->ended && !in->udp)
				AddWaitEventToSet(r->wes, WL_SOCKET_READABLE, in->sock, NULL, NULL);
		}
		/* a handshake still coming could be one of ours */
		foreach(lc, unclaimed)
		{
			IcIn	   *in = (IcIn *) lfirst(lc);

			if (!in->udp && in->hslen < (int) sizeof(IcHandshake))
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

static bool
ic_recv(GpIcReceiver *receiver, char **data, int *len)
{
	IcReceiver *r = (IcReceiver *) receiver;

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
			if (!in->ended && !in->udp && in_read(r, in))
				progress = true;
		}
		if (udp_poll())
			progress = true;
		if (progress)
			continue;

		before = list_length(r->conns);
		ic_accept();
		if (list_length(r->conns) != before)
			continue;

		/*
		 * UDP: before waiting, each sender is told of the room taking its
		 * rows made, where its last acknowledgement told of less.  A row the
		 * next packet ends leaves less than a packet's worth, which
		 * in_take() waits for, and a sender whose window it shut would wait
		 * for its next status query.
		 */
		foreach_ptr(IcIn, in, r->conns)
			if (in->udp && !in->ended &&
				in->bufsize - (in->end - in->start) > in->udp_advertised)
				udp_ack(in);

		recv_wait(r);
	}
}

/*
 * ic_recv_from()'s wait: for sender "in" alone, or none yet connected -- the
 * others' sockets stay readable while their rows wait their turn, and would
 * wake a wait on them at once, for ever.
 */
static void
recv_wait_one(IcReceiver *r, IcIn *in)
{
	WaitEvent	occurred[1];

	if (r->wes_one == NULL || r->wes_one_in != in ||
		r->wes_one_conns != list_length(r->conns) ||
		r->wes_one_unclaimed != list_length(unclaimed))
	{
		int			n = 5 + list_length(unclaimed);

		if (r->wes_one != NULL)
			FreeWaitEventSet(r->wes_one);
		r->wes_one = CreateWaitEventSet(NULL, n);
		AddWaitEventToSet(r->wes_one, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(r->wes_one, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);
		AddWaitEventToSet(r->wes_one, WL_SOCKET_READABLE, listen_sock, NULL, NULL);
		if (udp_sock != PGINVALID_SOCKET)
			AddWaitEventToSet(r->wes_one, WL_SOCKET_READABLE, udp_sock, NULL, NULL);
		if (in != NULL && !in->ended && !in->udp)
			AddWaitEventToSet(r->wes_one, WL_SOCKET_READABLE, in->sock, NULL, NULL);
		foreach_ptr(IcIn, u, unclaimed)
			if (!u->udp && u->hslen < (int) sizeof(IcHandshake))
				AddWaitEventToSet(r->wes_one, WL_SOCKET_READABLE, u->sock, NULL, NULL);
		r->wes_one_in = in;
		r->wes_one_conns = list_length(r->conns);
		r->wes_one_unclaimed = list_length(unclaimed);
	}
	if (WaitEventSetWait(r->wes_one, 100, occurred, 1, ic_wait_event(false)) > 0 &&
		(occurred[0].events & WL_LATCH_SET))
		ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
}

/*
 * The next row from one sender, for a merge of the senders' streams: the
 * k-th to have come, in the order the senders came, which stays each one's.
 */
static bool
ic_recv_from(GpIcReceiver *receiver, int k, char **data, int *len)
{
	IcReceiver *r = (IcReceiver *) receiver;

	Assert(k >= 0 && k < r->nsenders);
	for (;;)
	{
		IcIn	   *in = k < list_length(r->conns)
			? (IcIn *) list_nth(r->conns, k) : NULL;
		int			before;

		if (in != NULL)
		{
			int			got;

			if (in->ended)
				return false;
			got = in_take(in, data, len);
			if (got == 1)
				return true;
			if (got == 2)
			{
				r->nended++;
				r->wes_stale = true;
				return false;
			}
			if (!in->udp && in_read(r, in))
				continue;
		}
		if (udp_poll())
			continue;
		before = list_length(r->conns);
		ic_accept();
		if (list_length(r->conns) != before)
			continue;

		/* UDP: each sender told of the room taking its rows made, as below */
		foreach_ptr(IcIn, i, r->conns)
			if (i->udp && !i->ended &&
				i->bufsize - (i->end - i->start) > i->udp_advertised)
				udp_ack(i);

		recv_wait_one(r, in);
	}
}

static void
receiver_free(IcReceiver *r)
{
	foreach_ptr(IcIn, in, r->conns)
		in_close(in);
	list_free(r->conns);
	if (r->wes != NULL)
		FreeWaitEventSet(r->wes);
	if (r->wes_one != NULL)
		FreeWaitEventSet(r->wes_one);
	pfree(r);
}

/*
 * UDP: a receiver ended -- the senders that have not sent their last row told
 * STOP, what it had of their rows let go.  It stays, to answer the packets
 * that still come, until udp_wait().
 */
static void
udp_recv_stop(IcReceiver *r)
{
	TimestampTz now = GetCurrentTimestamp();

	foreach_ptr(IcIn, in, r->conns)
	{
		if (!in->ended && !in->udp_stopped)
		{
			udp_put(&in->udp_peer, in->udp_peerlen, IC_UDP_STOP, in->hs.token,
					in->hs.slice, in->hs.sender, in->udp_index, in->udp_recv,
					0, NULL, 0);
			in->udp_stopped = true;
			in->udp_stop_at = in->udp_last = now;
		}
		if (in->buf != NULL)
			pfree(in->buf);
		in->buf = NULL;
		in->bufsize = in->start = in->end = 0;
		list_free_deep(in->udp_waiting);
		in->udp_waiting = NIL;
	}
	r->done = true;
}

/*
 * UDP: ended receivers, until every one of their senders has closed -- each
 * told STOP again while it has not, if it was, and one not heard from waited
 * for -- or, its CLOSE lost, been silent IC_UDP_LINGER_US.  Their packets
 * are answered meanwhile.
 */
static void
udp_wait(List *rs)
{
	for (;;)
	{
		TimestampTz now = GetCurrentTimestamp();
		bool		open = false;

		foreach_ptr(IcReceiver, r, rs)
		{
			if (list_length(r->conns) < r->nsenders)
				open = true;
			foreach_ptr(IcIn, in, r->conns)
			{
				if (in->udp_closed || now - in->udp_last >= IC_UDP_LINGER_US)
					continue;
				open = true;
				if (in->udp_stopped &&
					now - in->udp_stop_at >= IC_UDP_STOP_AGAIN_US)
				{
					udp_put(&in->udp_peer, in->udp_peerlen, IC_UDP_STOP,
							in->hs.token, in->hs.slice, in->hs.sender,
							in->udp_index, in->udp_recv, 0, NULL, 0);
					in->udp_stop_at = now;
				}
			}
		}
		if (!open)
			return;
		if (!udp_poll())
		{
			int			ev = WaitLatchOrSocket(MyLatch,
											   WL_LATCH_SET | WL_SOCKET_READABLE |
											   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
											   udp_sock,
											   IC_UDP_STOP_AGAIN_US / 1000,
											   ic_wait_event(false));

			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

/* UDP: a receiver gone; a packet that comes still, while this process listens, gets a STOP. */
static void
udp_recv_free(IcReceiver *r)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	IcUdpEnded *e = palloc(sizeof(IcUdpEnded));

	memcpy(e->token, r->token, GP_IC_TOKEN_LEN);
	e->slice = (uint32) r->slice;
	udp_ended = lappend(udp_ended, e);
	MemoryContextSwitchTo(oldcxt);

	receivers = list_delete_ptr(receivers, r);
	receiver_free(r);
}

static void
ic_recv_end(GpIcReceiver *receiver)
{
	IcReceiver *r = (IcReceiver *) receiver;

	if (!r->udp)
	{
		receivers = list_delete_ptr(receivers, r);
		receiver_free(r);
		return;
	}

	/*
	 * UDP: a receiver that had every sender's last row waits for their CLOSE
	 * now: a sender waits for the acknowledgement of its last packet, and
	 * nothing else, before it closes.  One that ended before waits with the
	 * rest of its statement's here, all stopped first (udp_finish()): a
	 * receiver here still taking no rows could be what its senders wait on.
	 */
	udp_recv_stop(r);
	if (r->nended == r->nsenders)
	{
		udp_wait(list_make1(r));
		udp_recv_free(r);
	}
}

/*
 * The UDP receivers of statement "token", every one ended, until all of
 * their senders have closed.
 */
static void
udp_finish(const char *token)
{
	List	   *mine = NIL;

	foreach_ptr(IcReceiver, r, receivers)
	{
		if (!r->udp || memcmp(r->token, token, GP_IC_TOKEN_LEN) != 0)
			continue;
		if (!r->done)
			udp_recv_stop(r);
		mine = lappend(mine, r);
	}
	if (mine == NIL)
		return;
	udp_wait(mine);
	foreach_ptr(IcReceiver, r, mine)
		udp_recv_free(r);
	list_free(mine);
}

static void
ic_forget(const char *token)
{
	List	   *keep = NIL;
	MemoryContext oldcxt;

	udp_finish(token);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

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

/* ------------------------------------------------------------------------- */
/* UDP: sending                                                              */
/* ------------------------------------------------------------------------- */

/* A packet of a sender's stream, sent -- again, if it was before. */
static void
udp_transmit(IcSender *s, IcOut *out, IcUdpSent *p)
{
	udp_put(&out->addr, out->addrlen, IC_UDP_DATA, s->token, (uint32) s->slice,
			s->self, out->index, p->offset, 0, p->data, p->len);
	p->sent_at = GetCurrentTimestamp();
	p->tries++;
	if (p->tries > 1 && (p->tries - 1) % gp_interconnect_debug_retry_interval == 0)
		elog(DEBUG1, "resending packet (offset " UINT64_FORMAT ") to %s with %d retries",
			 p->offset, out->address, p->tries - 1);
}

/* A packet acknowledged: under the loss methods, the window opened by it. */
static void
udp_cwnd_acked(IcSender *s)
{
	s->inflight--;
	if (!IC_FC_BY_LOSS())
		return;
	s->cwnd += s->cwnd < s->ssthresh ? 1.0 : 1.0 / s->cwnd;
	s->cwnd = Min(s->cwnd, (double) s->maxcwnd);
}

/*
 * A packet lost, under the loss methods: the window halved where another came
 * before it, and closed to where it began where its time ran out.
 */
static void
udp_cwnd_lost(IcSender *s, bool timeout)
{
	if (!IC_FC_BY_LOSS())
		return;
	s->ssthresh = Max(s->cwnd / 2, (double) s->mincwnd);
	s->cwnd = timeout ? (double) s->mincwnd : s->ssthresh;
}

/*
 * May another packet go to this receiver?  Its room willing: under
 * "capacity", while fewer than gp.interconnect_snd_queue_depth wait on it;
 * under the loss methods, while the sender's window has room, or nothing
 * waits on this receiver at all.
 */
static bool
udp_may_send(IcSender *s, IcOut *out)
{
	if (!IC_FC_BY_LOSS())
		return list_length(out->unacked) < gp_interconnect_snd_queue_depth;
	return out->unacked == NIL || s->inflight < (int) s->cwnd;
}

/* Cloudberry's round trip and RTO: a smoothed mean and deviation, bounded. */
static void
udp_rtt_sample(IcOut *out, int64 rtt)
{
	int64		err = rtt - out->srtt;

	out->srtt += err / 8;
	out->rttvar += ((err < 0 ? -err : err) - out->rttvar) / 4;
	out->rto = Min(Max(out->srtt + 4 * out->rttvar,
					   (int64) gp_interconnect_min_rto * 1000),
				   IC_UDP_MAX_RTO_US);
}

static void
udp_out_forget(IcSender *s, IcOut *out)
{
	s->inflight -= list_length(out->unacked);
	foreach_ptr(IcUdpSent, p, out->unacked)
		pfree(p);
	list_free(out->unacked);
	out->unacked = NIL;
	out->len = 0;
}

/* A receiver's ACK or STOP, to the sender of its slice here. */
static void
udp_on_ack(const IcUdpPacket *pkt)
{
	foreach_ptr(IcSender, s, senders)
	{
		IcOut	   *out;
		TimestampTz now;

		if (s->slice != (int) pkt->slice || s->self != pkt->sender ||
			memcmp(s->token, pkt->token, GP_IC_TOKEN_LEN) != 0 ||
			pkt->index >= (uint32) s->nreceivers)
			continue;
		out = &s->outs[pkt->index];
		if (!out->udp)
			return;
		now = GetCurrentTimestamp();
		out->last_heard = now;
		if (pkt->type == IC_UDP_STOP)
		{
			out->wanted = false;
			udp_out_forget(s, out);
			udp_put(&out->addr, out->addrlen, IC_UDP_CLOSE, s->token,
					(uint32) s->slice, s->self, out->index, out->sent, 0,
					NULL, 0);
			return;
		}
		if (pkt->offset > out->acked && pkt->offset <= out->sent)
		{
			while (out->unacked != NIL)
			{
				IcUdpSent  *p = (IcUdpSent *) linitial(out->unacked);

				if (p->offset + p->len > pkt->offset)
					break;
				/* a packet sent once times the round trip (Karn's rule) */
				if (p->tries == 1)
					udp_rtt_sample(out, now - p->sent_at);
				out->unacked = list_delete_first(out->unacked);
				pfree(p);
				udp_cwnd_acked(s);
			}
			out->acked = pkt->offset;
		}
		else if (pkt->offset == out->acked && out->unacked != NIL)
		{
			/*
			 * The same acknowledgement again: a packet came before its turn,
			 * the one whose turn it is missing -- sent again now, as
			 * Cloudberry's disorder handling does, unless it was just now.
			 */
			IcUdpSent  *p = (IcUdpSent *) linitial(out->unacked);

			if (p->offset == pkt->offset &&
				now - p->sent_at > Max(out->srtt, (int64) 1000))
			{
				udp_transmit(s, out, p);
				udp_cwnd_lost(s, false);
			}
		}
		if (pkt->offset >= out->acked)
			out->window = pkt->window;
		return;
	}
}

/*
 * Take every packet that has come, without waiting, for whichever receiver or
 * sender here it is; true if any came.
 */
static bool
udp_poll(void)
{
	static char buf[65536];
	bool		any = false;

	if (udp_sock == PGINVALID_SOCKET)
		return false;
	for (;;)
	{
		struct sockaddr_storage from;
		socklen_t	fromlen = sizeof(from);
		ssize_t		n = recvfrom(udp_sock, buf, sizeof(buf), 0,
								 (struct sockaddr *) &from, &fromlen);
		IcUdpPacket pkt;

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			break;				/* EAGAIN: nothing more */
		}
		if (!udp_parse(buf, (int) n, &pkt))
			continue;
		any = true;
		if (pkt.type == IC_UDP_DATA || pkt.type == IC_UDP_STATUS ||
			pkt.type == IC_UDP_CLOSE)
			udp_on_data(&pkt, &from, fromlen);
		else if (pkt.type == IC_UDP_ACK || pkt.type == IC_UDP_STOP)
			udp_on_ack(&pkt);
	}
	return any;
}

/*
 * A sender waiting on a receiver: what has waited out its RTO sent again, the
 * RTO doubled; a status query where the receiver's room is shut and nothing
 * is in flight, every 512 ms (Cloudberry's deadlock check); and, heard
 * nothing from it for gp.interconnect_transmit_timeout, Cloudberry's error.
 * Then a wait for a packet, the next of these, or an interrupt.
 */
static void
udp_sender_wait(IcSender *s, IcOut *out)
{
	TimestampTz now = GetCurrentTimestamp();
	int64		next = IC_UDP_DEADLOCK_CHECK_US;
	int64		checking = (int64) gp_interconnect_timer_checking_period * 1000;
	bool		resent = false;
	int			ev;

	/* under the loss methods, looked at every so often; else every time */
	if (!IC_FC_BY_LOSS() || now - s->last_check >= checking)
	{
		s->last_check = now;
		foreach_ptr(IcUdpSent, p, out->unacked)
		{
			int64		waited = now - p->sent_at;

			if (waited >= out->rto)
			{
				udp_transmit(s, out, p);
				resent = true;
			}
			else
				next = Min(next, out->rto - waited);
		}
	}
	else if (out->unacked != NIL)
		next = Min(next, checking - (now - s->last_check));
	if (resent)
	{
		out->rto = Min(out->rto * 2, IC_UDP_MAX_RTO_US);
		next = Min(next, out->rto);
		udp_cwnd_lost(s, true);
	}
	if (out->unacked != NIL)
		next = Min(next, (int64) gp_interconnect_timer_period * 1000);

	if (out->unacked == NIL && out->acked + out->window <= out->sent)
	{
		if (now - out->last_query >= IC_UDP_DEADLOCK_CHECK_US)
		{
			udp_put(&out->addr, out->addrlen, IC_UDP_STATUS, s->token,
					(uint32) s->slice, s->self, out->index, out->sent, 0,
					NULL, 0);
			out->last_query = now;
		}
		next = Min(next, IC_UDP_DEADLOCK_CHECK_US - (now - out->last_query));
	}

	/*
	 * A packet sent again and again: Cloudberry's WARNING, once, and its
	 * error once it has been sent more times than that and nothing has been
	 * heard for the transmit timeout (checkNetworkTimeout()).  With nothing
	 * in flight, the silence alone.
	 */
	if (out->unacked != NIL)
	{
		IcUdpSent  *p = (IcUdpSent *) linitial(out->unacked);

		if (p->tries - 1 >= gp_interconnect_min_retries_before_timeout && !s->warned)
		{
			ereport(WARNING,
					(errmsg("interconnect may encountered a network error, please check your network"),
					 errdetail("Failed to send packet (offset " UINT64_FORMAT ") to %s after %d retries.",
							   p->offset, out->address, p->tries - 1)));
			s->warned = true;
		}
	}
	if (now - out->last_heard > (int64) gp_interconnect_transmit_timeout * 1000 * 1000 &&
		(out->unacked == NIL ||
		 ((IcUdpSent *) linitial(out->unacked))->tries - 1 > gp_interconnect_min_retries_before_timeout))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect encountered a network error, please check your network"),
				 errdetail("Did not get any response from %s in %d seconds.",
						   out->address, gp_interconnect_transmit_timeout)));

	ev = WaitLatchOrSocket(MyLatch,
						   WL_LATCH_SET | WL_SOCKET_READABLE | WL_TIMEOUT |
						   WL_EXIT_ON_PM_DEATH,
						   udp_sock, Max(next / 1000, 1), ic_wait_event(true));
	if (ev & WL_LATCH_SET)
		ResetLatch(MyLatch);
	CHECK_FOR_INTERRUPTS();
	(void) udp_poll();
}

/*
 * Send what is held for a UDP receiver: as many packets as its room allows,
 * waiting for its acknowledgements to open more -- and, with "drain", until
 * it has every byte.
 */
static void
udp_out_flush(IcSender *s, IcOut *out, bool drain)
{
	int			payload = gp_max_packet_size - IC_UDP_HEADER;
	int			off = 0;

	while (out->wanted && (off < out->len || (drain && out->acked < out->sent)))
	{
		int64		room = (int64) (out->acked + out->window) - (int64) out->sent;

		if (off < out->len && room > 0 && udp_may_send(s, out))
		{
			int			n = (int) Min(Min((int64) (out->len - off), (int64) payload), room);
			IcUdpSent  *p = MemoryContextAlloc(TopMemoryContext,
											   offsetof(IcUdpSent, data) + n);
			MemoryContext oldcxt;

			p->offset = out->sent;
			p->len = n;
			p->tries = 0;
			memcpy(p->data, out->buf + off, n);
			off += n;
			out->sent += n;
			oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			out->unacked = lappend(out->unacked, p);
			MemoryContextSwitchTo(oldcxt);
			s->inflight++;
			udp_transmit(s, out, p);
			continue;
		}
		udp_sender_wait(s, out);
		/* a packet that came may say what lets the next go */
		(void) udp_poll();
	}

	if (!out->wanted)
		out->len = 0;
	else if (off > 0)
	{
		memmove(out->buf, out->buf + off, out->len - off);
		out->len -= off;
	}
}

static void
out_append(IcSender *s, IcOut *out, uint32 frame, const char *data, int len)
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
	{
		if (out->udp)
			udp_out_flush(s, out, false);
		else
			out_flush(out);
	}
}

/* A UDP receiver's address, as its process gave it, as a socket's. */
static void
udp_resolve(const char *address, struct sockaddr_storage *addr,
			socklen_t *addrlen)
{
	memset(addr, 0, sizeof(*addr));
	if (strncmp(address, "udpunix:", 8) == 0)
	{
		struct sockaddr_un *un = (struct sockaddr_un *) addr;

		un->sun_family = AF_UNIX;
		strlcpy(un->sun_path, address + 8, sizeof(un->sun_path));
		*addrlen = sizeof(struct sockaddr_un);
	}
	else
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
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not resolve interconnect address \"%s\"",
							address)));
		memcpy(addr, res->ai_addr, res->ai_addrlen);
		*addrlen = res->ai_addrlen;
		freeaddrinfo(res);
	}
}

static IcSender *
ic_send_begin(const char *token, int slice, int self, int nreceivers,
			  char **addresses)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	IcSender   *s = palloc0(sizeof(IcSender));
	IcHandshake hs;

	Assert(strlen(token) == GP_IC_TOKEN_LEN);
	s->slice = slice;
	s->nreceivers = nreceivers;
	memcpy(s->token, token, GP_IC_TOKEN_LEN);
	s->self = self;
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

		/* UDP: nothing to connect; the first packets say who sends */
		if (strncmp(addresses[i], "udp:", 4) == 0 ||
			strncmp(addresses[i], "udpunix:", 8) == 0)
		{
			ic_open();			/* this process's socket, for the ACKs */
			udp_resolve(addresses[i], &out->addr, &out->addrlen);
			out->udp = true;
			out->index = (uint32) i;
			out->address = MemoryContextStrdup(TopMemoryContext, addresses[i]);
			out->window = udp_room();
			out->srtt = (int64) gp_interconnect_default_rtt * 1000;
			out->rttvar = out->srtt / 2;
			out->rto = Min(Max(out->srtt + 4 * out->rttvar,
							   (int64) gp_interconnect_min_rto * 1000),
						   IC_UDP_MAX_RTO_US);
			out->last_heard = out->last_query = GetCurrentTimestamp();
			out->wanted = true;
			s->mincwnd++;
			s->maxcwnd += gp_interconnect_snd_queue_depth;
			continue;
		}

		out->sock = ic_connect(addresses[i]);
		out->wanted = true;
		memcpy(out->buf, &hs, sizeof(hs));
		out->len = sizeof(hs);
		out_flush(out);
	}
	s->cwnd = s->mincwnd;
	s->ssthresh = s->maxcwnd;
	s->last_check = GetCurrentTimestamp();
	return s;
}

static void
ic_send(GpIcSender *sender, int receiver, const char *data, int len)
{
	IcSender   *s = (IcSender *) sender;

	if (receiver >= 0)
	{
		Assert(receiver < s->nreceivers);
		out_append(s, &s->outs[receiver], (uint32) len, data, len);
		return;
	}
	for (int i = 0; i < s->nreceivers; i++)
		out_append(s, &s->outs[i], (uint32) len, data, len);
}

static bool
ic_send_wanted(GpIcSender *sender)
{
	IcSender   *s = (IcSender *) sender;

	for (int i = 0; i < s->nreceivers; i++)
		if (s->outs[i].wanted)
			return true;
	return false;
}

static void
sender_free(IcSender *s)
{
	for (int i = 0; i < s->nreceivers; i++)
	{
		if (s->outs[i].sock != PGINVALID_SOCKET)
			closesocket(s->outs[i].sock);
		if (s->outs[i].udp)
			udp_out_forget(s, &s->outs[i]);
		pfree(s->outs[i].buf);
	}
	pfree(s->outs);
	pfree(s);
}

static void
ic_send_end(GpIcSender *sender)
{
	IcSender   *s = (IcSender *) sender;

	for (int i = 0; i < s->nreceivers; i++)
	{
		IcOut	   *out = &s->outs[i];

		out_append(s, out, IC_END_OF_ROWS, NULL, 0);
		if (!out->udp)
		{
			out_flush(out);
			continue;
		}

		/* every byte acknowledged, then CLOSE: the receiver may go */
		udp_out_flush(s, out, true);
		if (out->wanted)
			udp_put(&out->addr, out->addrlen, IC_UDP_CLOSE, s->token,
					(uint32) s->slice, s->self, out->index, out->sent, 0,
					NULL, 0);
	}
	senders = list_delete_ptr(senders, s);
	sender_free(s);
}

/* ------------------------------------------------------------------------- */
/* tcp and udpifc, as transports                                             */
/* ------------------------------------------------------------------------- */

static const char *
tcp_address(void)
{
	ic_open();
	return listen_address;
}

static const char *
udpifc_address(void)
{
	ic_open();
	return udp_address;
}

/* Either: each receiver's address says which of the two it receives by. */
static GpIcSender *
tcp_send_begin(GpIcStream *stream, GpIcSlice *slice)
{
	IcSender   *s = ic_send_begin(stream->token, slice->slice,
								  GpClusterContentId(), slice->nreceivers,
								  slice->receiver_addresses);

	return &s->base;
}

static GpIcReceiver *
tcp_recv_begin(GpIcStream *stream, GpIcSlice *slice)
{
	return &ic_recv_begin(stream->token, slice->slice, slice->nsenders,
						  false)->base;
}

static GpIcReceiver *
udpifc_recv_begin(GpIcStream *stream, GpIcSlice *slice)
{
	return &ic_recv_begin(stream->token, slice->slice, slice->nsenders,
						  true)->base;
}

/* What an error leaves, the transaction's end closes (ic_xact_callback()). */
static void
tcp_stmt_end(GpIcStream *stream, bool error)
{
	if (!error)
		ic_forget(stream->token);
}

static const GpIcTransport tcp_transport = {
	.name = "tcp",
	.address = tcp_address,
	.send_begin = tcp_send_begin,
	.send = ic_send,
	.send_wanted = ic_send_wanted,
	.send_end = ic_send_end,
	.recv_begin = tcp_recv_begin,
	.recv = ic_recv,
	.recv_from = ic_recv_from,
	.recv_end = ic_recv_end,
	.stmt_end = tcp_stmt_end,
};

static const GpIcTransport udpifc_transport = {
	.name = "udpifc",
	.address = udpifc_address,
	.send_begin = tcp_send_begin,
	.send = ic_send,
	.send_wanted = ic_send_wanted,
	.send_end = ic_send_end,
	.recv_begin = udpifc_recv_begin,
	.recv = ic_recv,
	.recv_from = ic_recv_from,
	.recv_end = ic_recv_end,
	.stmt_end = tcp_stmt_end,
};

/* ------------------------------------------------------------------------- */
/* The transports' table, and the statements' streams                       */
/* ------------------------------------------------------------------------- */

static const GpIcTransport *transports[GP_IC_MAX_TRANSPORTS];
static int	ntransports = 0;

/* GpIcStream, each in a memory context of its own under TopMemoryContext */
static List *streams = NIL;

void
GpIcRegisterTransport(const GpIcTransport *transport)
{
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "interconnect transport \"%s\" registered outside shared_preload_libraries",
			 transport->name);
	if (GpIcFindTransport(transport->name) != NULL)
		elog(ERROR, "interconnect transport \"%s\" registered twice",
			 transport->name);
	if (ntransports >= GP_IC_MAX_TRANSPORTS)
		elog(ERROR, "too many interconnect transports");
	transports[ntransports++] = transport;
}

const GpIcTransport *
GpIcFindTransport(const char *name)
{
	for (int i = 0; i < ntransports; i++)
		if (strcmp(transports[i]->name, name) == 0)
			return transports[i];
	return NULL;
}

int
GpIcTransportIndex(const GpIcTransport *transport)
{
	for (int i = 0; i < ntransports; i++)
		if (transports[i] == transport)
			return i;
	elog(ERROR, "interconnect transport \"%s\" is not registered",
		 transport->name);
	return -1;
}

const char *
GpIcAddress(const GpIcTransport *transport)
{
	return transport->address();
}

/*
 * A stream is kept from here until GpIcForget(), or the end of the
 * (sub)transaction it began in, which ends it as an error does.  Its
 * transport's stmt_begin may fail: the stream is kept first, so that its
 * end finds what it began.
 */
void
GpIcStatementBegin(GpIcStream *stream)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	stream->subxid = GetCurrentSubTransactionId();
	streams = lappend(streams, stream);
	MemoryContextSwitchTo(oldcxt);
	if (stream->transport->stmt_begin != NULL)
		stream->transport->stmt_begin(stream);
}

GpIcStream *
GpIcStatementFind(const char *token)
{
	foreach_ptr(GpIcStream, stream, streams)
		if (strcmp(stream->token, token) == 0)
			return stream;
	return NULL;
}

/* The memory a stream is in, with what its transport kept there. */
static void
stream_free(GpIcStream *stream)
{
	MemoryContextDelete(GetMemoryChunkContext(stream));
}

void
GpIcForget(GpIcStream *stream)
{
	streams = list_delete_ptr(streams, stream);
	PG_TRY();
	{
		stream->transport->stmt_end(stream, false);
	}
	PG_FINALLY();
	{
		stream_free(stream);
	}
	PG_END_TRY();
}

GpIcSlice *
GpIcStreamSlice(GpIcStream *stream, int slice)
{
	for (int i = 0; i < stream->nslices; i++)
		if (stream->slices[i].slice == slice)
			return &stream->slices[i];
	return NULL;
}

GpIcSender *
GpIcSendBegin(GpIcStream *stream, int slice)
{
	GpIcSlice  *s = GpIcStreamSlice(stream, slice);
	GpIcSender *sender;

	/* where Cloudberry's segment sets up its interconnect (SetupInterconnect()) */
	GP_FAULT("interconnect_setup_palloc");
	if (s == NULL)
		elog(ERROR, "interconnect: slice %d of the statement does not stream",
			 slice);
	sender = stream->transport->send_begin(stream, s);
	sender->transport = stream->transport;
	return sender;
}

void
GpIcSend(GpIcSender *sender, int receiver, const char *data, int len)
{
	sender->transport->send(sender, receiver, data, len);
}

bool
GpIcSendWanted(GpIcSender *sender)
{
	return sender->transport->send_wanted(sender);
}

void
GpIcSendEnd(GpIcSender *sender)
{
	sender->transport->send_end(sender);
}

GpIcReceiver *
GpIcRecvBegin(GpIcStream *stream, int slice)
{
	GpIcSlice  *s = GpIcStreamSlice(stream, slice);
	GpIcReceiver *receiver;

	GP_FAULT("interconnect_setup_palloc");
	if (s == NULL)
		elog(ERROR, "interconnect: slice %d of the statement does not stream",
			 slice);
	receiver = stream->transport->recv_begin(stream, s);
	receiver->transport = stream->transport;
	return receiver;
}

bool
GpIcRecv(GpIcReceiver *receiver, char **data, int *len)
{
	return receiver->transport->recv(receiver, data, len);
}

bool
GpIcRecvFrom(GpIcReceiver *receiver, int k, char **data, int *len)
{
	return receiver->transport->recv_from(receiver, k, data, len);
}

void
GpIcRecvEnd(GpIcReceiver *receiver)
{
	receiver->transport->recv_end(receiver);
}

/*
 * The streams a (sub)transaction's end leaves, ended as an error ends them:
 * the transports close what is theirs.
 */
static void
streams_end(SubTransactionId subxid)
{
	List	   *ending = NIL;

	foreach_ptr(GpIcStream, stream, streams)
		if (subxid == InvalidSubTransactionId || stream->subxid == subxid)
			ending = lappend(ending, stream);
	foreach_ptr(GpIcStream, stream, ending)
	{
		streams = list_delete_ptr(streams, stream);
		stream->transport->stmt_end(stream, true);
		stream_free(stream);
	}
	list_free(ending);
}

static void
ic_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		foreach_ptr(GpIcStream, stream, streams)
			if (stream->subxid == mySubid)
				stream->subxid = parentSubid;
	}
	else if (event == SUBXACT_EVENT_ABORT_SUB)
		streams_end(mySubid);
}

/*
 * gp.interconnect_type names a module's transport that no module in
 * shared_preload_libraries registered: the server does not start, rather
 * than run its statements over another -- the check gp_motion.c's check hook
 * leaves for after the modules have loaded.
 */
static shmem_request_hook_type prev_shmem_request = NULL;

static void
ic_check_transport(void)
{
	const char *type = GetConfigOption("gp.interconnect_type", true, false);

	if (prev_shmem_request)
		prev_shmem_request();
	if (type != NULL && strcmp(type, "relay") != 0 &&
		GpIcFindTransport(type) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("gp.interconnect_type is \"%s\", which no module in \"shared_preload_libraries\" provides",
						type),
				 errhint("Add \"%s\" to \"shared_preload_libraries\" after \"gp_core\".",
						 strcmp(type, "proxy") == 0 ? "interconnect" : type)));
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

	streams_end(InvalidSubTransactionId);

	foreach_ptr(IcSender, s, senders)
		sender_free(s);
	list_free(senders);
	senders = NIL;

	foreach_ptr(IcReceiver, r, receivers)
		receiver_free(r);
	list_free(receivers);
	receivers = NIL;

	foreach_ptr(IcIn, in, unclaimed)
		in_close(in);
	list_free(unclaimed);
	unclaimed = NIL;

	list_free_deep(udp_ended);
	udp_ended = NIL;
}

void
GpIcInit(void)
{
	DefineCustomIntVariable("gp.interconnect_queue_depth",
							"Sets the maximum size of the receive queue for each connection in the UDP interconnect",
							"The packets' worth of room a receiver has for each sender's rows, which a sender does not send beyond.",
							&gp_interconnect_queue_depth,
							4, 1, 4096, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.max_packet_size",
							"Sets the max packet size for the Interconnect.",
							NULL,
							&gp_max_packet_size,
							8192, 512, 65507, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_transmit_timeout",
							"Timeout (in seconds) on interconnect to transmit a packet.",
							"A UDP sender that hears nothing from its receiver this long gives up.",
							&gp_interconnect_transmit_timeout,
							3600, 1, 7200, PGC_USERSET, GUC_UNIT_S,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_min_rto",
							"Sets the min RTO (in ms) for UDP interconnect.",
							NULL,
							&gp_interconnect_min_rto,
							20, 1, 1000, PGC_USERSET, GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_default_rtt",
							"Sets the default rtt (in ms) for UDP interconnect.",
							NULL,
							&gp_interconnect_default_rtt,
							20, 1, 1000, PGC_USERSET, GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_snd_queue_depth",
							"Sets the maximum size of the send queue for each connection in the UDP interconnect",
							"The packets a sender keeps waiting on each receiver's acknowledgement: under \"capacity\" flow control each receiver's, and under the loss methods, for each receiver, the most the sender's window grows to.",
							&gp_interconnect_snd_queue_depth,
							2, 1, 4096, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.interconnect_fc_method",
							 "Sets the flow control method used for UDP interconnect.",
							 "Valid values are \"capacity\" and \"loss\".",
							 &gp_interconnect_fc_method,
							 IC_FC_LOSS, fc_methods, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_min_retries_before_timeout",
							"Sets the min retries before reporting a transmit timeout in the interconnect.",
							NULL,
							&gp_interconnect_min_retries_before_timeout,
							100, 1, 4096, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_debug_retry_interval",
							"Sets the interval by retry times to record a debug message for retry.",
							NULL,
							&gp_interconnect_debug_retry_interval,
							10, 1, 4096, PGC_USERSET, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.interconnect_cache_future_packets",
							 "Control whether future packets are cached.",
							 "A packet that comes before the receiver it is for has begun waits for it; off, it is dropped, and sent again.",
							 &gp_interconnect_cache_future_packets,
							 true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_timer_period",
							"Sets the timer period (in ms) for UDP interconnect",
							"The longest a sender waiting on acknowledgements sleeps.",
							&gp_interconnect_timer_period,
							5, 1, 100, PGC_USERSET, GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.interconnect_timer_checking_period",
							"Sets the timer checking period (in ms) for UDP interconnect",
							"How often, under the loss methods, a sender looks for packets whose time has run out.",
							&gp_interconnect_timer_checking_period,
							20, 1, 100, PGC_USERSET, GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_dropacks_percent",
							"Sets the percentage of correctly-received acknowledgment packets to synthetically drop, for testing.",
							NULL,
							&gp_udpic_dropacks_percent,
							0, 0, 100, PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gp.udpic_dropxmit_percent",
							"Sets the percentage of correctly-received data packets to synthetically drop, for testing.",
							NULL,
							&gp_udpic_dropxmit_percent,
							0, 0, 100, PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.log_interconnect",
							 "Sets the verbosity of logged messages pertaining to connections between worker processes.",
							 "Valid values are \"off\", \"terse\", \"verbose\" and \"debug\".  Cloudberry calls this gp_log_interconnect.",
							 &gp_log_interconnect,
							 GP_IC_VERBOSITY_TERSE, log_interconnect_options,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	GpIcRegisterTransport(&tcp_transport);
	GpIcRegisterTransport(&udpifc_transport);
	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = ic_check_transport;

	if (GpClusterIsSingleNode())
		return;
	RegisterXactCallback(ic_xact_callback, NULL);
	RegisterSubXactCallback(ic_subxact_callback, NULL);
}
