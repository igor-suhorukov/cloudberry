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
 * shm.c
 *	  The shared-memory motion transport: gp.interconnect_type = shm.
 *
 * NOT IN CLOUDBERRY, whose interconnects are TCP, UDP and the proxy.
 *
 * gp_core's transports carry a Motion's rows through sockets (gp_ic.c), so
 * between two segments of one host the kernel copies each row into a socket
 * and out of it.  This one carries the rows of a sender and a receiver of
 * one host through memory the two share: the sender writes each row into
 * it, and the receiver reads the row where it lies.
 *
 * A segment is a server of its own, with its own postmaster, so the memory
 * is neither a DSM segment of PostgreSQL's, which a process attaches only
 * through its own server's control segment (PostgreSQL's dsm.c:708-720),
 * nor a shm_mq, which wakes its peer through the peer's PGPROC latch
 * (shm_mq.c:219).  It is the sender's own mapping of an anonymous file
 * (memfd_create()), whose descriptor the sender passes each of its
 * receivers over a Unix socket, SCM_RIGHTS, after the statement's token --
 * a random value the coordinator gave both ends over their authenticated
 * connections, which a tcp sender shows its receiver first
 * (gp_ic.c:88-94) -- so that a connection that does not know the token is
 * never read from, as there; and only a process of the server's own user
 * may connect.  Having no name, the file can be mapped by no other
 * process, and it goes with the last process that holds it, a crashed one
 * too.  Where there is no memfd_create(), it is a POSIX shared-memory
 * object named by the token, unlinked as soon as it is made: its
 * descriptor goes over the socket all the same, and nothing of it is left
 * in /dev/shm.
 *
 * The mapping, the sender's channel, has a ring for each of its receivers,
 * and one more that all of them read, each from its own place: the
 * broadcast ring, for a row sent to every receiver -- a Broadcast's, or a
 * Gather's to its one -- which is written once.  A ring has one writer.
 * The writer writes each row into it as a frame -- eight bytes, the row's
 * length and the frame's kind, then the row, the next frame at the next
 * 8-byte boundary -- and says so by moving the ring's head; a reader takes
 * the row where it lies, and frees the frame at its next call, as gp_ic.h
 * lets a row be valid until then (gp_ic.h:110), by moving its tail.  The
 * write is the row's one copy.  A frame never wraps: one that would is
 * written at the ring's start, after a frame that skips its end.  A row
 * longer than the ring goes in pieces of half of it, which the receiver
 * gathers into memory of its own -- a copy, then only.
 *
 * The ring is the receiver's room, gp.shm_ring_size, as
 * gp.interconnect_queue_depth is udpifc's.  A writer whose ring is full, or
 * a reader whose rings are empty, sleeps on an eventfd in its WaitEventSet
 * beside its latch, as on a socket: a cancel or a statement's timeout
 * reaches it as on tcp.  The sender makes one for each receiver, its bell,
 * which the sender rings when it has written rows for a receiver that
 * sleeps; and one for itself, which a receiver rings when it has freed the
 * room the sleeping sender needs.  Two, not one: a side that read the
 * other's word from a bell they shared could leave the other asleep.  Each
 * side says that it sleeps, in the channel, before it looks a last time,
 * and the other looks after it has written, so that neither sleeps through
 * the other's word.  The sender lets a sleeping receiver sleep until 64 kB
 * of rows wait for it, or a quarter of its ring -- as tcp holds 64 kB of a
 * receiver's rows before it sends them (gp_ic.c:166, 2088) -- or until its
 * rows end, or it sleeps itself; a receiver wakes a sender that sleeps for
 * room once a quarter of the ring is free.  Nor does either side say each
 * row it writes or frees, which would cost each row two barriers of the
 * processor's: a ring's head and tail move in the channel once a
 * sixteenth of the ring, 16 kB at most, has been written or freed since,
 * at a row's end, and before either side sleeps -- and a receiver gives back
 * what it has freed at once when its sender says it sleeps for room.
 *
 * A receiver that needs no more rows says STOP by a flag in its place in
 * the channel, which its sender reads before each row, and rings the sender
 * if it sleeps.  The Unix socket stays open until the sender's end and the
 * receiver's, and each side sleeps on it too: a peer that dies closes it.
 * A receiver whose sender is gone before its last row fails, as tcp's does
 * (gp_ic.c:1171-1175); a sender whose receiver is gone sends it no more, as
 * tcp's does once its connection fails (gp_ic.c:1737), the receiver's
 * process having failed the statement itself.
 *
 * Connections come as tcp's do: each sender connects to all its receivers
 * as its slice begins, so a connection can come before the fragment that
 * receives it has started -- it waits among the unclaimed ones until its
 * receiver asks for it, or its statement ends here -- and a receiver that
 * needs no more rows takes every connection of its senders, one not made
 * yet waited for, before it stops them (gp_ic.c:1505-1520): a sender whose
 * ring nobody read would wait on it for ever.  A sender's rows wait in its
 * ring meanwhile, as a tcp sender's wait in its socket.  A process idle
 * between a cursor's FETCHes has nothing to answer: a sender needs no
 * answer to write into a ring, and a STOP is a flag it reads -- so there is
 * no run_end (gp_ic.h:119-122).
 *
 * Several hosts.  A process's address names its host -- the kernel's boot
 * id and the process's mount namespace, so that two processes that give
 * the same are on one kernel, and see one file at a Unix socket's path --
 * beside its Unix socket and its address for gp_core's tcp transport
 * (GpIcFindTransport("tcp")).  A slice all of whose senders and receivers
 * are on one host goes through rings; one with a sender and a receiver on
 * two goes over tcp, every pair of it, through tcp's own functions on both
 * sides.  pg_vector_executor.md (§3.10) takes tcp for the pairs on two
 * hosts alone, and rings for the others, but gp_ic.h's table cannot carry
 * that: tcp's recv() waits until a row comes (gp_ic.c:1227-1294), so a
 * receiver could not wait for tcp's rows and its rings' at once -- one that
 * waited in tcp's while its rings filled would stop their senders, which
 * other receivers may be waiting on, as it may be on theirs -- and a sender
 * that wrote rings beside tcp could not have tcp send what it holds of a
 * receiver's rows (gp_ic.c:2088) before it waited for a ring: the receiver
 * waiting for those could be what the ring waits on.  On a cluster of
 * several hosts a slice spans them, so deciding by receiver would gain
 * nothing over deciding by slice.  Every process of the slice decides
 * alike, from its addresses and contents, which the coordinator gave them
 * all.  gp.shm_debug_remote makes chosen pairs, or all, count as on two
 * hosts, so that one host tests both; like the ring's size, it is read as
 * each process's statement begins, from the settings the coordinator sent
 * it then (gp_dispatch.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"

#include "cb_module.h"
#include "gp_cluster.h"
#include "gp_ic.h"

PG_MODULE_MAGIC_EXT(
					.name = "shm",
					.version = GP_VERSION
);

void		_PG_init(void);

/* ------------------------------------------------------------------------- */
/* The channel, as the sender and its receivers share it                     */
/* ------------------------------------------------------------------------- */

/* What a sender says first, with the channel's descriptors. */
#define SHM_MAGIC			"GPSM"
#define SHM_VERSION			1

/*
 * The descriptors that come with it: the mapping, the receiver's bell, and
 * the sender's.
 */
#define SHM_NFDS			3

/* The hello's flags: the mapping is a memfd whose size is sealed */
#define SHM_HELLO_SEALED	0x1

typedef struct ShmHello
{
	char		magic[4];
	uint32		version;
	char		token[GP_IC_TOKEN_LEN];
	uint32		slice;
	int32		sender;			/* the sender's content id */
	uint32		index;			/* the receiver's place among the readers */
	uint32		nreaders;
	uint32		ring_size;
	uint32		flags;
	uint64		map_size;
} ShmHello;

/* The channel's own mark, at its start. */
#define SHM_CHANNEL_MAGIC	0x4753484D

/* What each side writes, in cache lines apart from what the other does. */
#define SHM_LINE			PG_CACHE_LINE_SIZE

/*
 * A receiver's place in the channel: the head of its own ring, which the
 * sender moves; the tails of its own ring and of the broadcast ring, which
 * it moves as it reads; and, seldom written, that it sleeps for rows, and
 * its STOP -- which the sender sets too, for a receiver it finds gone, so
 * that the broadcast ring no longer waits for it.
 */
typedef struct ShmSlot
{
	pg_attribute_aligned(SHM_LINE) pg_atomic_uint32 head;
	pg_attribute_aligned(SHM_LINE) pg_atomic_uint32 tail;
	pg_atomic_uint32 btail;
	pg_attribute_aligned(SHM_LINE) pg_atomic_uint32 waiting;
	pg_atomic_uint32 stop;
} ShmSlot;

/*
 * The channel, at the mapping's start: what it was made for, which a
 * receiver checks against the hello; the broadcast ring's head, and that
 * the sender sleeps for room -- in which ring, a reader's place or nreaders
 * for the broadcast ring, and for how much; and each receiver's place.  The
 * rings follow, each receiver's own in its order, then the broadcast ring.
 */
typedef struct ShmChannel
{
	uint32		magic;
	uint32		version;
	uint32		nreaders;
	uint32		ring_size;
	uint64		map_size;
	uint64		data_offset;
	uint32		slice;
	int32		sender;
	char		token[GP_IC_TOKEN_LEN];
	pg_attribute_aligned(SHM_LINE) pg_atomic_uint32 bhead;
	pg_atomic_uint32 writer_waiting;
	pg_atomic_uint32 writer_ring;
	pg_atomic_uint32 writer_need;
	ShmSlot		slots[FLEXIBLE_ARRAY_MEMBER];
} ShmChannel;

/* Each side's lines where they are meant to be. */
StaticAssertDecl(sizeof(ShmSlot) == 3 * SHM_LINE &&
				 offsetof(ShmSlot, tail) == SHM_LINE &&
				 offsetof(ShmSlot, waiting) == 2 * SHM_LINE,
				 "a receiver's place is three cache lines");
StaticAssertDecl(offsetof(ShmChannel, bhead) == SHM_LINE &&
				 offsetof(ShmChannel, slots) == 2 * SHM_LINE,
				 "the channel's sender's line is its second");

/* A frame: its header, then its bytes; the next at the next 8-byte boundary. */
typedef struct ShmFrame
{
	uint32		len;
	uint32		kind;
} ShmFrame;

#define SHM_ROW				1	/* a row */
#define SHM_END				2	/* the end of the sender's rows */
#define SHM_SKIP			3	/* nothing more before the ring's start */
#define SHM_PIECE_FIRST		4	/* a long row's first piece: its length in 8
								 * bytes, then the row's start */
#define SHM_PIECE			5	/* and the pieces after it */
#define SHM_PIECE_LAST		6

#define SHM_HEADER			((uint32) sizeof(ShmFrame))
#define SHM_FRAME(len)		(SHM_HEADER + (uint32) TYPEALIGN(8, (len)))

/* A sleeping reader is let sleep until this much waits for it. */
#define SHM_WAKE(size)		Min((size) / 4, 64 * 1024)

/* A ring's head or tail is moved in the channel once this much has gone by. */
#define SHM_PUBLISH(size)	Min((size) / 16, 16 * 1024)

/* The smallest ring, and the largest: gp.shm_ring_size's bounds, in kB. */
#define SHM_RING_MIN_KB		16
#define SHM_RING_MAX_KB		(64 * 1024)

/* Connections not yet taken that a listener keeps, as gp_ic.c's */
#define SHM_BACKLOG			256

/*
 * How many rows a receiver that always has rows takes before it looks at
 * its senders' connections without waiting, and takes the ones that have
 * come: a sender gone is seen by it as tcp's receiver sees one, which reads
 * every connection, and takes those that came, each time it has no row in
 * hand (gp_ic.c:1265-1278) -- which a ring may hold for long.  So a
 * receiver whose sender died ends its statement as soon as tcp's would,
 * whether or not the coordinator cancels it (gp_dispatch.c,
 * gang_cancel_running()).  And a sender that came after the others have
 * rows waits no longer than this to be read.
 */
#define SHM_POLL_ROWS		1024

/* Where the rings begin: past the receivers' places, at a page. */
static Size
shm_data_offset(uint32 nreaders)
{
	return TYPEALIGN(4096, offsetof(ShmChannel, slots) +
					 (Size) nreaders * sizeof(ShmSlot));
}

/* The channel's size: its places, a ring for each receiver, and one more. */
static Size
shm_map_size(uint32 nreaders, uint32 ring_size)
{
	return shm_data_offset(nreaders) + (Size) (nreaders + 1) * ring_size;
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

static int	shm_ring_size = 256;	/* kB */
static char *shm_debug_remote = NULL;
static bool shm_debug_shm_open = false;

/*
 * gp.shm_debug_remote, read: the pairs of a sender's and a receiver's
 * content ids that count as on two hosts, either SHM_ANY for every one.
 */
#define SHM_ANY		INT_MIN

typedef struct ShmPair
{
	int			sender;
	int			receiver;
} ShmPair;

typedef struct ShmRemote
{
	int			npairs;
	ShmPair		pairs[FLEXIBLE_ARRAY_MEMBER];
} ShmRemote;

static ShmRemote *shm_remote = NULL;

/* A content id, or "*": false for anything else. */
static bool
shm_parse_content(char *s, int *content)
{
	char	   *end;
	long		v;

	while (*s == ' ')
		s++;
	end = s + strlen(s);
	while (end > s && end[-1] == ' ')
		*--end = '\0';
	if (strcmp(s, "*") == 0)
	{
		*content = SHM_ANY;
		return true;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (*s == '\0' || *end != '\0' || errno != 0 || v < -1 || v > INT_MAX)
		return false;
	*content = (int) v;
	return true;
}

/* "*", or "sender:receiver, ...", each a content id or "*". */
static bool
shm_remote_check(char **newval, void **extra, GucSource source)
{
	char	   *copy = pstrdup(*newval != NULL ? *newval : "");
	char	   *save = NULL;
	char	   *item;
	int			n = 1;
	ShmRemote  *remote;

	for (char *p = copy; *p != '\0'; p++)
		if (*p == ',')
			n++;
	remote = guc_malloc(LOG, offsetof(ShmRemote, pairs) + n * sizeof(ShmPair));
	if (remote == NULL)
		return false;
	remote->npairs = 0;

	for (item = strtok_r(copy, ",", &save); item != NULL;
		 item = strtok_r(NULL, ",", &save))
	{
		char	   *colon = strchr(item, ':');
		ShmPair    *pair = &remote->pairs[remote->npairs];
		bool		good;

		if (colon == NULL)
		{
			good = shm_parse_content(item, &pair->sender) &&
				pair->sender == SHM_ANY;
			pair->receiver = SHM_ANY;
		}
		else
		{
			*colon = '\0';
			good = shm_parse_content(item, &pair->sender) &&
				shm_parse_content(colon + 1, &pair->receiver);
		}
		if (!good)
		{
			GUC_check_errdetail("Each pair is \"sender:receiver\", each a content id or \"*\"; or \"*\" alone, for every pair.");
			guc_free(remote);
			pfree(copy);
			return false;
		}
		remote->npairs++;
	}
	pfree(copy);
	*extra = remote;
	return true;
}

static void
shm_remote_assign(const char *newval, void *extra)
{
	shm_remote = (ShmRemote *) extra;
}

/* Does the pair count as on two hosts? */
static bool
shm_pair_remote(const ShmRemote *remote, int sender, int receiver)
{
	if (remote == NULL)
		return false;
	for (int i = 0; i < remote->npairs; i++)
	{
		const ShmPair *pair = &remote->pairs[i];

		if ((pair->sender == SHM_ANY || pair->sender == sender) &&
			(pair->receiver == SHM_ANY || pair->receiver == receiver))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* This process's state                                                      */
/* ------------------------------------------------------------------------- */

/* gp_core's tcp, which carries the rows of a slice that spans hosts */
static const GpIcTransport *tcp_transport = NULL;

/* Where this process receives, once it has been asked. */
static pgsocket listen_sock = PGINVALID_SOCKET;
static char *listen_path = NULL;
static char *shm_address_string = NULL;
static char *shm_host = NULL;

/*
 * What a statement of shm's has here (GpIcStream's state), in its memory:
 * its settings as it began, and its senders and receivers, for its end.
 */
typedef struct ShmStream
{
	uint32		ring_size;		/* bytes */
	bool		shm_open;		/* POSIX shared memory, not memfd_create() */
	ShmRemote  *remote;
	List	   *senders;		/* ShmSender */
	List	   *receivers;		/* ShmReceiver */
} ShmStream;

/* A ring as a reader has it. */
typedef struct ShmRring
{
	char	   *data;
	pg_atomic_uint32 *headp;
	pg_atomic_uint32 *tailp;
	uint32		head;			/* as last read */
	uint32		tail;
	uint32		said;			/* the tail as the channel has it */
	uint32		off;			/* where its next frame is */
	bool		ended;			/* its end has come */
} ShmRring;

/*
 * One sender's connection, as its receiver has it: the handshake as it
 * comes, with its descriptors; then the channel, mapped, and its two rings.
 * In TopMemoryContext, as it comes before anything asks for it.
 */
typedef struct ShmIn
{
	pgsocket	sock;
	ShmHello	hello;
	int			hellolen;		/* how much of the hello has come */
	int			fds[SHM_NFDS];
	int			nfds;
	char	   *map;
	Size		mapsize;
	ShmChannel *ch;
	ShmSlot    *slot;
	uint32		size;			/* each ring's */
	int			bell;			/* the receiver's, which the sender rings */
	int			room;			/* the sender's, which the receiver rings */
	ShmRring	rings[2];		/* its own ring, and the broadcast ring */
	bool		eof;			/* its connection has closed */
	bool		ended;			/* its last row came, in both rings */
	char	   *gather;			/* a long row, gathered from its pieces */
	uint32		gather_size;
	uint32		gathered;
	uint32		total;
	ShmRring   *gathering;		/* the ring its pieces come in, or NULL */
} ShmIn;

typedef struct ShmReceiver
{
	GpIcReceiver base;
	ShmStream  *ss;
	char		token[GP_IC_TOKEN_LEN + 1];
	int			slice;
	int			nsenders;
	GpIcReceiver *tcp;			/* every sender's rows over tcp */
	List	   *ins;			/* ShmIn, in the order they came */
	int			nended;
	int			next;			/* whose rows to look at first */
	ShmIn	   *held;			/* the row returned last: its sender, */
	ShmRring   *held_ring;		/* its ring, NULL where gathered, */
	uint32		held_size;		/* and its frame, freed at the next call */
	WaitEventSet *wes;			/* a wait for any sender's rows */
	WaitEvent  *occurred;
	int			nevents;
	bool		wes_stale;
	uint64		wes_gen;
	WaitEventSet *wes_one;		/* for one's, or for connections alone */
	WaitEvent  *occurred_one;
	int			nevents_one;
	ShmIn	   *wes_one_in;
	bool		wes_one_stale;
	uint64		wes_one_gen;
	int			rows;			/* rows taken since it last looked */
	bool		ended;
} ShmReceiver;

/* One receiver reached through the channel, as its sender has it. */
typedef struct ShmOut
{
	int			receiver;		/* which of the slice's receivers */
	pgsocket	sock;
	int			bell;			/* the receiver's */
	ShmSlot    *slot;
	char	   *ring;
	uint32		head;			/* bytes written to its ring */
	uint32		said;			/* the head as the channel has it */
	uint32		off;			/* where its next frame goes */
	uint32		tail;			/* its tail, as last read */
	bool		wanted;			/* no STOP, and its connection open */
} ShmOut;

typedef struct ShmSender
{
	GpIcSender	base;
	ShmStream  *ss;
	int			slice;
	int			nreceivers;
	/* through the channel, a ShmOut for each receiver */
	int			nouts;
	ShmOut	   *outs;
	int			mapfd;
	char	   *map;
	Size		mapsize;
	ShmChannel *ch;
	uint32		size;			/* each ring's */
	char	   *bring;			/* the broadcast ring */
	uint32		bhead;
	uint32		bsaid;
	uint32		boff;
	uint32		btail;			/* its slowest wanted reader's, as last read */
	int			room;			/* the sender's bell */
	WaitEventSet *wes;
	WaitEvent  *occurred;
	int			nevents;
	bool		wes_stale;
	bool		closed;
	/* or every row over tcp */
	GpIcSender *tcp;
	GpIcSlice	tcp_slice;
} ShmSender;

/* Connections no receiver has asked for yet, in TopMemoryContext. */
static List *unclaimed = NIL;	/* ShmIn */
static uint64 unclaimed_gen = 0;	/* moved as one comes or goes */

/*
 * The receivers through rings that have not ended, which a connection that
 * comes is for.
 */
static List *receivers = NIL;	/* ShmReceiver */

#define SHM_LOG(...) \
	do { \
		if (gp_log_interconnect >= GP_IC_VERBOSITY_VERBOSE) \
			ereport(LOG, (errhidestmt(true), errmsg_internal(__VA_ARGS__))); \
	} while (0)

/*
 * tcp's wait events, as a backend waiting on any transport shows it
 * (gp_ic.c:380-390)
 */
static uint32
shm_wait_event(bool send)
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
/* Bells                                                                     */
/* ------------------------------------------------------------------------- */

static int
shm_bell(void)
{
	int			fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	if (fd < 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not create an eventfd for the interconnect: %m")));
	return fd;
}

/*
 * Rung: a write that adds one to its count.  It fails only where the count
 * would pass 2^64 - 2, and for a peer gone, which its socket says.
 */
static void
shm_ring_bell(int fd)
{
	uint64		one = 1;
	ssize_t		rc = write(fd, &one, sizeof(one));

	(void) rc;
}

/* Heard: its count read, and so set to nothing again. */
static void
shm_drain_bell(int fd)
{
	uint64		count;
	ssize_t		rc = read(fd, &count, sizeof(count));

	(void) rc;
}

/* ------------------------------------------------------------------------- */
/* Where this process receives                                               */
/* ------------------------------------------------------------------------- */

/*
 * The host this process is on, as far as a Unix socket and a shared mapping
 * are concerned: the kernel, by its boot id, and the files, by the mount
 * namespace, which two servers in separate containers of one kernel do not
 * share.  Where /proc has neither, the host's name.  Every process of a
 * slice must find the same here, or they would not agree on how its rows
 * go: what /proc has and cannot be read is an error, not the name.
 */
static const char *
shm_host_id(void)
{
	char		boot[64];
	struct stat st;
	int			fd;
	ssize_t		n = 0;
	char	   *mnt = NULL;

	if (shm_host != NULL)
		return shm_host;

	fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | PG_BINARY, 0);
	if (fd < 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open \"%s\": %m", "/proc/sys/kernel/random/boot_id")));
	if (fd >= 0)
	{
		n = read(fd, boot, sizeof(boot) - 1);
		close(fd);
		if (n < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read \"%s\": %m", "/proc/sys/kernel/random/boot_id")));
	}
	boot[Max(n, 0)] = '\0';
	n = 0;
	while (boot[n] != '\0' && (isxdigit((unsigned char) boot[n]) || boot[n] == '-'))
		n++;
	boot[n] = '\0';

	if (stat("/proc/self/ns/mnt", &st) == 0)
		mnt = psprintf(".%llu", (unsigned long long) st.st_ino);
	else if (errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat \"%s\": %m", "/proc/self/ns/mnt")));

	if (n > 0)
		shm_host = MemoryContextStrdup(TopMemoryContext,
									   psprintf("%s%s", boot, mnt != NULL ? mnt : ""));
	else
	{
		char		host[256];

		if (gethostname(host, sizeof(host)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not get the host's name: %m")));
		host[sizeof(host) - 1] = '\0';
		for (char *p = host; *p != '\0'; p++)
			if (*p == '|' || isspace((unsigned char) *p))
				*p = '_';
		shm_host = MemoryContextStrdup(TopMemoryContext,
									   psprintf("host.%s", host));
	}
	return shm_host;
}

/*
 * The directory of the listener's socket: the node's own, where the cluster
 * names nodes by the directory of their socket as the test clusters do, and
 * otherwise the server's first socket directory.
 */
static char *
shm_socket_dir(const GpSegmentConfig *self)
{
	List	   *dirs;

	if (self->hostname[0] == '/')
		return pstrdup(self->hostname);
	if (Unix_socket_directories != NULL &&
		SplitDirectoriesString(pstrdup(Unix_socket_directories), ',', &dirs))
	{
		foreach_ptr(char, dir, dirs)
			if (dir[0] == '/')
				return dir;
	}
	return pstrdup("/tmp");
}

static void
shm_remove_socket_file(int code, Datum arg)
{
	if (listen_path != NULL)
		unlink(listen_path);
}

static void
shm_listen(const GpSegmentConfig *self)
{
	struct sockaddr_un addr;
	char	   *path = psprintf("%s/.s.GPSHM.%d", shm_socket_dir(self), MyProcPid);

	if (strlen(path) >= sizeof(addr.sun_path))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect socket path \"%s\" is too long", path)));
	/* the address's own separator */
	if (strchr(path, '|') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect socket path \"%s\" has a \"|\"", path)));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_sock == PGINVALID_SOCKET)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not create the interconnect socket: %m")));
	unlink(path);				/* a process of this pid that went before */
	if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
		listen(listen_sock, SHM_BACKLOG) < 0 || !pg_set_noblock(listen_sock))
	{
		int			save = errno;

		closesocket(listen_sock);
		listen_sock = PGINVALID_SOCKET;
		errno = save;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not listen on the interconnect socket \"%s\": %m",
						path)));
	}
	listen_path = MemoryContextStrdup(TopMemoryContext, path);
	on_proc_exit(shm_remove_socket_file, 0);
}

/*
 * Where this process receives: "shm:<host>|<its socket>|<its tcp address>",
 * its listener and tcp's opened the first time.
 */
static const char *
shm_address(void)
{
	const GpSegmentConfig *self = GpClusterSelf();
	const char *tcp;

	if (shm_address_string != NULL)
		return shm_address_string;
	if (self == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the interconnect needs a cluster")));

	tcp = tcp_transport->address();
	if (listen_sock == PGINVALID_SOCKET)
		shm_listen(self);
	shm_address_string = MemoryContextStrdup(TopMemoryContext,
											 psprintf("shm:%s|%s|%s",
													  shm_host_id(), listen_path,
													  tcp));
	return shm_address_string;
}

/* An address taken apart. */
typedef struct ShmPeer
{
	char	   *host;
	char	   *path;
	char	   *tcp;
} ShmPeer;

static void
shm_peer(const char *address, ShmPeer *peer)
{
	char	   *copy = strncmp(address, "shm:", 4) == 0 ? pstrdup(address + 4) : NULL;
	char	   *bar1 = copy != NULL ? strchr(copy, '|') : NULL;
	char	   *bar2 = bar1 != NULL ? strchr(bar1 + 1, '|') : NULL;

	if (bar2 == NULL)
		elog(ERROR, "invalid shm interconnect address \"%s\"", address);
	*bar1 = '\0';
	*bar2 = '\0';
	peer->host = copy;
	peer->path = bar1 + 1;
	peer->tcp = bar2 + 1;
}

/*
 * Does a slice go through rings: are all of its senders and receivers on
 * one host, and no pair of them said to be on two?  Each of its processes
 * asks this of the same slice, and they agree.
 */
static bool
shm_through_rings(ShmStream *ss, GpIcSlice *slice)
{
	ShmPeer    *senders = palloc_array(ShmPeer, Max(slice->nsenders, 1));

	for (int j = 0; j < slice->nsenders; j++)
		shm_peer(slice->sender_addresses[j], &senders[j]);
	for (int i = 0; i < slice->nreceivers; i++)
	{
		ShmPeer		receiver;

		shm_peer(slice->receiver_addresses[i], &receiver);
		for (int j = 0; j < slice->nsenders; j++)
			if (strcmp(senders[j].host, receiver.host) != 0 ||
				shm_pair_remote(ss->remote, slice->sender_contents[j],
								slice->receiver_contents[i]))
				return false;
	}
	return true;
}

static ShmStream *
shm_stream(GpIcStream *stream)
{
	if (stream->state == NULL)
		elog(ERROR, "shm: the statement's interconnect is not set up");
	return (ShmStream *) stream->state;
}

/* ------------------------------------------------------------------------- */
/* Receiving: connections, and their handshakes                              */
/* ------------------------------------------------------------------------- */

static ShmIn *
in_new(pgsocket sock)
{
	ShmIn	   *in = MemoryContextAllocZero(TopMemoryContext, sizeof(ShmIn));

	in->sock = sock;
	in->bell = in->room = -1;
	for (int i = 0; i < SHM_NFDS; i++)
		in->fds[i] = -1;
	return in;
}

/* What it holds of the system's let go: its mapping, its descriptors. */
static void
in_detach(ShmIn *in)
{
	if (in->map != NULL)
		munmap(in->map, in->mapsize);
	in->map = NULL;
	for (int i = 0; i < SHM_NFDS; i++)
		if (in->fds[i] >= 0)
			close(in->fds[i]);
	in->nfds = 0;
	for (int i = 0; i < SHM_NFDS; i++)
		in->fds[i] = -1;
	if (in->bell >= 0)
		close(in->bell);
	if (in->room >= 0)
		close(in->room);
	in->bell = in->room = -1;
	if (in->sock != PGINVALID_SOCKET)
		closesocket(in->sock);
	in->sock = PGINVALID_SOCKET;
	if (in->gather != NULL)
		pfree(in->gather);
	in->gather = NULL;
	in->gather_size = 0;
}

static void
in_close(ShmIn *in)
{
	in_detach(in);
	pfree(in);
}

/* Is it mapped, and are its rows still to come? */
static inline bool
in_live(ShmIn *in)
{
	return in->map != NULL && !in->ended;
}

/*
 * What has come of a connection's hello, read without waiting, and the
 * descriptors that come with it: 1 once it is whole and one of ours, 0
 * while more is to come, -1 for a connection to close.
 */
static int
in_hello(ShmIn *in)
{
	struct msghdr msg;
	struct iovec iov;
	union
	{
		struct cmsghdr align;
		char		buf[CMSG_SPACE(sizeof(int) * SHM_NFDS)];
	}			control;
	ssize_t		n;
	int			flags = MSG_DONTWAIT;

#ifdef MSG_CMSG_CLOEXEC
	flags |= MSG_CMSG_CLOEXEC;
#endif
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = ((char *) &in->hello) + in->hellolen;
	iov.iov_len = sizeof(ShmHello) - in->hellolen;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buf;
	msg.msg_controllen = sizeof(control.buf);
	n = recvmsg(in->sock, &msg, flags);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return 0;
	if (n < 0)
		return -1;

	/* the descriptors, with the hello's first bytes */
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
	{
		int			nfd;

		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;
		nfd = (int) ((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		for (int i = 0; i < nfd; i++)
		{
			int			fd;

			memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
			if (in->nfds < SHM_NFDS)
				in->fds[in->nfds++] = fd;
			else
			{
				close(fd);
				in->nfds = SHM_NFDS + 1;
			}
		}
	}
	if (n == 0 || (msg.msg_flags & MSG_CTRUNC) || in->nfds > SHM_NFDS)
		return -1;
	in->hellolen += n;
	if (in->hellolen < (int) sizeof(ShmHello))
		return 0;

	if (memcmp(in->hello.magic, SHM_MAGIC, 4) != 0 ||
		in->hello.version != SHM_VERSION || in->nfds != SHM_NFDS ||
		in->hello.nreaders == 0 || in->hello.nreaders > 65536 ||
		in->hello.index >= in->hello.nreaders ||
		in->hello.ring_size < SHM_RING_MIN_KB * 1024 ||
		in->hello.ring_size > SHM_RING_MAX_KB * 1024 ||
		in->hello.ring_size % 8 != 0 ||
		in->hello.map_size != shm_map_size(in->hello.nreaders, in->hello.ring_size))
		return -1;
	return 1;
}

static void in_claim(ShmReceiver *r, ShmIn *in);

/* A connection whose hello has come: to its receiver, if it has begun. */
static void
in_route(ShmIn *in)
{
	MemoryContext oldcxt;

	foreach_ptr(ShmReceiver, r, receivers)
	{
		if (r->slice == (int) in->hello.slice &&
			memcmp(r->token, in->hello.token, GP_IC_TOKEN_LEN) == 0)
		{
			in_claim(r, in);
			return;
		}
	}
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	unclaimed = lappend(unclaimed, in);
	MemoryContextSwitchTo(oldcxt);
	unclaimed_gen++;
}

/*
 * Take the connections that have arrived -- only a process of this server's
 * user's can make one -- and read what has come of the hellos of the ones
 * taken earlier; each whole one goes to its receiver.  Never waits.
 */
static void
shm_accept(void)
{
	List	   *pending = NIL;

	if (listen_sock != PGINVALID_SOCKET)
	{
		for (;;)
		{
			pgsocket	sock = accept(listen_sock, NULL, NULL);
			uid_t		uid;
			gid_t		gid;
			MemoryContext oldcxt;

			if (sock == PGINVALID_SOCKET)
				break;			/* EAGAIN, or nothing to do about it now */
			if (getpeereid(sock, &uid, &gid) != 0 || uid != geteuid() ||
				!pg_set_noblock(sock))
			{
				closesocket(sock);
				continue;
			}
			oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			unclaimed = lappend(unclaimed, in_new(sock));
			MemoryContextSwitchTo(oldcxt);
			unclaimed_gen++;
		}
	}

	foreach_ptr(ShmIn, in, unclaimed)
		if (in->hellolen < (int) sizeof(ShmHello))
			pending = lappend(pending, in);
	foreach_ptr(ShmIn, in, pending)
	{
		int			got = in_hello(in);

		if (got == 0)
			continue;
		unclaimed = list_delete_ptr(unclaimed, in);
		unclaimed_gen++;
		if (got < 0)
			in_close(in);		/* not one of ours */
		else
			in_route(in);
	}
	list_free(pending);
}

pg_noreturn static void
in_corrupt(ShmReceiver *r, ShmIn *in, const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: the shared memory of slice %d from segment %d is not what it should be",
					r->slice, in->hello.sender),
			 errdetail_internal("%s", detail)));
}

/*
 * A connection the receiver takes: its channel mapped, checked against its
 * hello, and its descriptor closed -- the mapping keeps it -- and its two
 * rings found in it.
 */
static void
in_attach(ShmReceiver *r, ShmIn *in)
{
	struct stat st;
	ShmChannel *ch;
	Size		data = shm_data_offset(in->hello.nreaders);

	if (fstat(in->fds[0], &st) != 0 || (Size) st.st_size < in->hello.map_size)
		in_corrupt(r, in, "Its file is shorter than its sender says.");
#ifdef F_GET_SEALS
	if (in->hello.flags & SHM_HELLO_SEALED)
	{
		int			seals = fcntl(in->fds[0], F_GET_SEALS);

		if (seals < 0 || (seals & F_SEAL_SHRINK) == 0)
			in_corrupt(r, in, "Its file may shrink.");
	}
#endif
	in->map = mmap(NULL, in->hello.map_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, in->fds[0], 0);
	if (in->map == MAP_FAILED)
	{
		in->map = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not map the interconnect's shared memory of slice %d from segment %d: %m",
						r->slice, in->hello.sender)));
	}
	in->mapsize = in->hello.map_size;
	close(in->fds[0]);
	in->fds[0] = -1;
	in->bell = in->fds[1];
	in->room = in->fds[2];
	in->fds[1] = in->fds[2] = -1;
	in->nfds = 0;

	ch = in->ch = (ShmChannel *) in->map;
	if (ch->magic != SHM_CHANNEL_MAGIC || ch->version != SHM_VERSION ||
		ch->nreaders != in->hello.nreaders ||
		ch->ring_size != in->hello.ring_size ||
		ch->map_size != in->hello.map_size || ch->data_offset != data ||
		ch->slice != in->hello.slice ||
		memcmp(ch->token, in->hello.token, GP_IC_TOKEN_LEN) != 0)
		in_corrupt(r, in, "It is not the one its sender's hello says.");

	in->size = in->hello.ring_size;
	in->slot = &ch->slots[in->hello.index];
	in->rings[0].data = in->map + data + (Size) in->hello.index * in->size;
	in->rings[0].headp = &in->slot->head;
	in->rings[0].tailp = &in->slot->tail;
	in->rings[1].data = in->map + data + (Size) in->hello.nreaders * in->size;
	in->rings[1].headp = &ch->bhead;
	in->rings[1].tailp = &in->slot->btail;
	for (int k = 0; k < 2; k++)
	{
		in->rings[k].tail = pg_atomic_read_u32(in->rings[k].tailp);
		in->rings[k].head = in->rings[k].tail;
		in->rings[k].said = in->rings[k].tail;
		in->rings[k].off = 0;
		if (in->rings[k].tail != 0)
			in_corrupt(r, in, "A ring has been read before its receiver came.");
	}
}

/* A sender's connection, to its receiver: kept in the order they came. */
static void
in_claim(ShmReceiver *r, ShmIn *in)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(r));

	r->ins = lappend(r->ins, in);
	MemoryContextSwitchTo(oldcxt);
	r->wes_stale = r->wes_one_stale = true;
	if (list_length(r->ins) > r->nsenders)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect: slice %d has more senders than the %d it was given",
						r->slice, r->nsenders)));
	in_attach(r, in);
}

/* ------------------------------------------------------------------------- */
/* Receiving: rows                                                           */
/* ------------------------------------------------------------------------- */

/*
 * A sender sleeping for room in this ring, woken once it has what it said
 * it needs: in its own ring, what is not read; in the broadcast ring, what
 * the furthest behind of the receivers that have not stopped has not.
 */
static void
in_wake_writer(ShmIn *in, ShmRring *ring)
{
	ShmChannel *ch = in->ch;
	bool		bcast = ring == &in->rings[1];
	uint32		which;
	uint32		need;
	uint32		behind = 0;

	pg_read_barrier();
	which = pg_atomic_read_u32(&ch->writer_ring);
	need = pg_atomic_read_u32(&ch->writer_need);
	if (which != (bcast ? in->hello.nreaders : in->hello.index))
		return;
	if (!bcast)
		behind = pg_atomic_read_u32(ring->headp) - ring->tail;
	else
	{
		uint32		head = pg_atomic_read_u32(ring->headp);

		for (uint32 i = 0; i < in->hello.nreaders; i++)
			if (pg_atomic_read_u32(&ch->slots[i].stop) == 0)
				behind = Max(behind, head - pg_atomic_read_u32(&ch->slots[i].btail));
	}
	if (in->size - Min(behind, in->size) >= need &&
		pg_atomic_exchange_u32(&ch->writer_waiting, 0) != 0)
		shm_ring_bell(in->room);
}

/*
 * What a ring's reader has freed, said in the channel, and the sender woken
 * if it sleeps for it.
 */
static void
rring_say(ShmIn *in, ShmRring *ring)
{
	if (ring->said == ring->tail)
		return;
	/* what was read of the frames, read before their room is given back */
	pg_memory_barrier();
	pg_atomic_write_u32(ring->tailp, ring->tail);
	ring->said = ring->tail;
	/* given back before the sender's sleep is looked at: it looks after */
	pg_memory_barrier();
	if (pg_atomic_read_u32(&in->ch->writer_waiting) != 0)
		in_wake_writer(in, ring);
}

/* Every ring of the sender's, its freed room said: before a sleep. */
static void
in_say(ShmIn *in)
{
	if (in_live(in))
		for (int k = 0; k < 2; k++)
			rring_say(in, &in->rings[k]);
}

/*
 * A frame read: its room given back -- in the channel once SHM_PUBLISH() of
 * it has gone by, or at once where the sender sleeps for room.
 */
static void
rring_free(ShmIn *in, ShmRring *ring, uint32 n)
{
	ring->tail += n;
	ring->off += n;
	if (ring->off >= in->size)
		ring->off = 0;
	if (ring->tail - ring->said >= SHM_PUBLISH(in->size) ||
		pg_atomic_read_u32(&in->ch->writer_waiting) != 0)
		rring_say(in, ring);
}

#define SHM_TOOK_NONE	0
#define SHM_TOOK_ROW	1
#define SHM_TOOK_END	2

/*
 * The next row in a ring, without waiting: a row, valid until the
 * receiver's next call -- in the ring, or gathered from its pieces -- or
 * the ring's end, or nothing yet.  Every frame is checked against the ring
 * before it is read.
 */
static int
rring_take(ShmReceiver *r, ShmIn *in, ShmRring *ring, char **data, int *len)
{
	for (;;)
	{
		ShmFrame	f;
		uint32		avail;
		uint32		fsize;
		char	   *payload;

		if (ring->tail == ring->head)
		{
			ring->head = pg_atomic_read_u32(ring->headp);
			/* the frames written before the head said so */
			pg_read_barrier();
			if (ring->tail == ring->head)
				return SHM_TOOK_NONE;
		}
		avail = ring->head - ring->tail;
		if (avail > in->size || avail < SHM_HEADER ||
			(Size) ring->off + SHM_HEADER > in->size)
			in_corrupt(r, in, psprintf("A ring of %u bytes has %u unread at %u.",
									   in->size, avail, ring->off));
		memcpy(&f, ring->data + ring->off, sizeof(f));
		payload = ring->data + ring->off + SHM_HEADER;
		if (f.len > in->size)
			fsize = PG_UINT32_MAX;
		else if (f.kind == SHM_SKIP)
			fsize = in->size - ring->off;
		else
			fsize = SHM_FRAME(f.len);
		if (fsize > avail || (Size) ring->off + fsize > in->size ||
			(f.kind == SHM_SKIP && f.len != fsize - SHM_HEADER))
			in_corrupt(r, in, psprintf("A frame of kind %u is %u bytes long at %u of a ring of %u bytes, %u of them unread.",
									   f.kind, f.len, ring->off, in->size, avail));

		switch (f.kind)
		{
			case SHM_SKIP:
				rring_free(in, ring, fsize);
				continue;

			case SHM_END:
				if (in->gathering != NULL)
					in_corrupt(r, in, "A row in pieces ends with its sender's rows.");
				rring_free(in, ring, fsize);
				ring->ended = true;
				return SHM_TOOK_END;

			case SHM_ROW:
				if (in->gathering == ring)
					in_corrupt(r, in, "A row comes in the middle of another's pieces.");
				*data = payload;
				*len = (int) f.len;
				r->held = in;
				r->held_ring = ring;
				r->held_size = fsize;
				return SHM_TOOK_ROW;

			case SHM_PIECE_FIRST:
				{
					uint32		total;

					if (in->gathering != NULL || f.len < 8)
						in_corrupt(r, in, "A row's first piece comes in the middle of another's.");
					memcpy(&total, payload, sizeof(total));
					if (total <= f.len - 8 || total > MaxAllocSize)
						in_corrupt(r, in, psprintf("A row in pieces is %u bytes long.", total));
					if (in->gather_size < total)
					{
						if (in->gather != NULL)
							pfree(in->gather);
						in->gather = NULL;
						in->gather = MemoryContextAlloc(TopMemoryContext, total);
						in->gather_size = total;
					}
					in->total = total;
					memcpy(in->gather, payload + 8, f.len - 8);
					in->gathered = f.len - 8;
					in->gathering = ring;
					rring_free(in, ring, fsize);
					continue;
				}

			case SHM_PIECE:
			case SHM_PIECE_LAST:
				if (in->gathering != ring || f.len > in->total - in->gathered)
					in_corrupt(r, in, "A piece of a row comes with no row to go in.");
				memcpy(in->gather + in->gathered, payload, f.len);
				in->gathered += f.len;
				rring_free(in, ring, fsize);
				if (f.kind == SHM_PIECE)
					continue;
				if (in->gathered != in->total)
					in_corrupt(r, in, "A row in pieces is shorter than it says.");
				in->gathering = NULL;
				*data = in->gather;
				*len = (int) in->total;
				r->held = in;
				r->held_ring = NULL;
				r->held_size = 0;
				return SHM_TOOK_ROW;

			default:
				in_corrupt(r, in, psprintf("A frame is of kind %u.", f.kind));
		}
	}
}

/* A sender's next row, from either of its rings, or the end of both. */
static int
in_take(ShmReceiver *r, ShmIn *in, char **data, int *len)
{
	for (int k = 0; k < 2; k++)
	{
		ShmRring   *ring = &in->rings[k];

		while (!ring->ended)
		{
			int			got = rring_take(r, in, ring, data, len);

			if (got == SHM_TOOK_ROW)
				return SHM_TOOK_ROW;
			if (got == SHM_TOOK_NONE)
				break;
		}
	}
	return in->rings[0].ended && in->rings[1].ended ? SHM_TOOK_END : SHM_TOOK_NONE;
}

/* Has it written what is not read yet? */
static bool
in_ready(ShmIn *in)
{
	for (int k = 0; k < 2; k++)
		if (!in->rings[k].ended &&
			pg_atomic_read_u32(in->rings[k].headp) != in->rings[k].tail)
			return true;
	return false;
}

/* Its last row has come: what it holds let go, and the waits made again. */
static void
in_end(ShmReceiver *r, ShmIn *in)
{
	in->ended = true;
	r->nended++;
	in_detach(in);
	r->wes_stale = r->wes_one_stale = true;
}

/*
 * Its connection is readable: closed, as its sender's process ended -- or
 * failed; what it wrote before is in its rings, which say which -- or
 * something a sender never writes.
 */
static void
in_check_eof(ShmReceiver *r, ShmIn *in)
{
	char		buf[16];
	ssize_t		n = recv(in->sock, buf, sizeof(buf), MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n > 0)
		in_corrupt(r, in, "Its sender wrote on its connection after its hello.");
	in->eof = true;
	closesocket(in->sock);
	in->sock = PGINVALID_SOCKET;
	r->wes_stale = r->wes_one_stale = true;
	/* the rings as they were when it closed */
	pg_memory_barrier();
}

/* Gone, its rings read out, before its last row. */
pg_noreturn static void
in_lost(ShmReceiver *r, ShmIn *in)
{
	ereport(ERROR,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg("interconnect: the sender of slice %d on segment %d stopped before its last row",
					r->slice, in->hello.sender)));
}

/* STOP: no more rows, the sender woken if it sleeps for room. */
static void
in_stop(ShmIn *in)
{
	if (!in_live(in))
		return;
	pg_atomic_write_u32(&in->slot->stop, 1);
	pg_memory_barrier();
	if (pg_atomic_read_u32(&in->ch->writer_waiting) != 0 &&
		pg_atomic_exchange_u32(&in->ch->writer_waiting, 0) != 0)
		shm_ring_bell(in->room);
}

/*
 * The row returned last, freed: gp_ic.h's rows are valid until the next
 * call.
 */
static void
r_release(ShmReceiver *r)
{
	if (r->held != NULL && r->held_ring != NULL && in_live(r->held))
		rring_free(r->held, r->held_ring, r->held_size);
	r->held = NULL;
	r->held_ring = NULL;
}

/*
 * A wait for any sender's rows ("any"), or for "one" sender's -- the others'
 * bells stay rung while their rows wait their turn, and would wake a wait
 * on them at once, for ever, as gp_ic.c:1296-1300 says of sockets --
 * or, "one" NULL, for connections alone: the latch, the listener while not
 * every sender has come, the hellos still coming, and each sender's bell
 * and socket.
 */
static WaitEventSet *
r_wes(ShmReceiver *r, bool any, ShmIn *one, WaitEvent **occurred, int *nevents)
{
	WaitEventSet **wesp = any ? &r->wes : &r->wes_one;
	WaitEvent **occp = any ? &r->occurred : &r->occurred_one;
	int		   *np = any ? &r->nevents : &r->nevents_one;
	bool		stale = any
		? r->wes_stale || r->wes_gen != unclaimed_gen
		: r->wes_one_stale || r->wes_one_gen != unclaimed_gen || r->wes_one_in != one;

	if (*wesp == NULL || stale)
	{
		int			n = 3 + list_length(unclaimed) + 2 * list_length(r->ins);
		WaitEventSet *wes;

		if (*wesp != NULL)
			FreeWaitEventSet(*wesp);
		*wesp = NULL;
		wes = CreateWaitEventSet(NULL, n);
		*wesp = wes;
		AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
		if (list_length(r->ins) < r->nsenders && listen_sock != PGINVALID_SOCKET)
			AddWaitEventToSet(wes, WL_SOCKET_READABLE, listen_sock, NULL, NULL);
		/* a hello still coming could be for it */
		foreach_ptr(ShmIn, u, unclaimed)
			if (u->hellolen < (int) sizeof(ShmHello) && u->sock != PGINVALID_SOCKET)
				AddWaitEventToSet(wes, WL_SOCKET_READABLE, u->sock, NULL, NULL);
		foreach_ptr(ShmIn, in, r->ins)
		{
			if (!in_live(in) || (!any && in != one))
				continue;
			AddWaitEventToSet(wes, WL_SOCKET_READABLE, in->bell, NULL, in);
			if (in->sock != PGINVALID_SOCKET)
				AddWaitEventToSet(wes, WL_SOCKET_READABLE, in->sock, NULL, in);
		}
		if (*np < n)
		{
			if (*occp != NULL)
				pfree(*occp);
			*occp = MemoryContextAlloc(GetMemoryChunkContext(r), sizeof(WaitEvent) * n);
			*np = n;
		}
		if (any)
		{
			r->wes_stale = false;
			r->wes_gen = unclaimed_gen;
		}
		else
		{
			r->wes_one_stale = false;
			r->wes_one_gen = unclaimed_gen;
			r->wes_one_in = one;
		}
	}
	*occurred = *occp;
	*nevents = *np;
	return *wesp;
}

/* What a wait of the receiver's found: bells heard, connections closed. */
static void
r_heard(ShmReceiver *r, WaitEvent *occurred, int n)
{
	for (int i = 0; i < n; i++)
	{
		WaitEvent  *e = &occurred[i];
		ShmIn	   *in = (ShmIn *) e->user_data;

		if (e->events & WL_LATCH_SET)
			ResetLatch(MyLatch);
		else if (in != NULL && e->fd == in->bell)
			shm_drain_bell(in->bell);
		else if (in != NULL && e->fd == in->sock)
			in_check_eof(r, in);
		/* the listener, or a hello: taken as the caller looks again */
	}
}

/*
 * Sleep until a sender has written rows -- any of the receiver's, or "one"
 * alone -- or a connection comes, or the latch is set: each sender told
 * that it is slept on first, and looked at a last time after, since it
 * writes, then looks whether to ring.
 */
static void
r_wait(ShmReceiver *r, bool any, ShmIn *one)
{
	bool		ready = false;

	/* every sender's room given back first: one could be what it waits on */
	foreach_ptr(ShmIn, in, r->ins)
		in_say(in);
	foreach_ptr(ShmIn, in, r->ins)
		if (in_live(in) && (any || in == one))
			pg_atomic_write_u32(&in->slot->waiting, 1);
	pg_memory_barrier();
	foreach_ptr(ShmIn, in, r->ins)
		if (in_live(in) && (any || in == one) && in_ready(in))
			ready = true;

	if (!ready)
	{
		WaitEvent  *occurred;
		int			nevents;
		WaitEventSet *wes = r_wes(r, any, one, &occurred, &nevents);

		r_heard(r, occurred,
				WaitEventSetWait(wes, -1, occurred, nevents, shm_wait_event(false)));
	}

	foreach_ptr(ShmIn, in, r->ins)
		if (in_live(in) && (any || in == one))
			pg_atomic_write_u32(&in->slot->waiting, 0);
	r->rows = 0;
	CHECK_FOR_INTERRUPTS();
}

/*
 * A look at the receiver's connections, without waiting, and the ones that
 * have come taken: SHM_POLL_ROWS.
 */
static void
r_poll(ShmReceiver *r)
{
	WaitEvent  *occurred;
	int			nevents;
	WaitEventSet *wes;

	foreach_ptr(ShmIn, in, r->ins)
		in_say(in);
	shm_accept();
	wes = r_wes(r, true, NULL, &occurred, &nevents);
	r_heard(r, occurred, WaitEventSetWait(wes, 0, occurred, nevents,
										  shm_wait_event(false)));
	r->rows = 0;
	CHECK_FOR_INTERRUPTS();
}

static GpIcReceiver *
shm_recv_begin(GpIcStream *stream, GpIcSlice *slice)
{
	ShmStream  *ss = shm_stream(stream);
	MemoryContext cxt = GetMemoryChunkContext(stream);
	ShmReceiver *r = MemoryContextAllocZero(cxt, sizeof(ShmReceiver));
	MemoryContext oldcxt;
	List	   *mine = NIL;

	r->ss = ss;
	memcpy(r->token, stream->token, GP_IC_TOKEN_LEN + 1);
	r->slice = slice->slice;
	r->nsenders = slice->nsenders;
	oldcxt = MemoryContextSwitchTo(cxt);
	ss->receivers = lappend(ss->receivers, r);
	MemoryContextSwitchTo(oldcxt);

	/* a slice that spans hosts: its rows over tcp */
	if (!shm_through_rings(ss, slice))
	{
		SHM_LOG("interconnect shm: slice %d of statement %.8s receives from %d senders over tcp",
				slice->slice, stream->token, slice->nsenders);
		r->tcp = tcp_transport->recv_begin(stream, slice);
		return &r->base;
	}

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	receivers = lappend(receivers, r);
	MemoryContextSwitchTo(oldcxt);
	r->wes_stale = r->wes_one_stale = true;

	/* the ones that came before it asked, in the order they came */
	foreach_ptr(ShmIn, in, unclaimed)
		if (in->hellolen == (int) sizeof(ShmHello) &&
			(int) in->hello.slice == slice->slice &&
			memcmp(in->hello.token, stream->token, GP_IC_TOKEN_LEN) == 0)
			mine = lappend(mine, in);
	foreach_ptr(ShmIn, in, mine)
	{
		unclaimed = list_delete_ptr(unclaimed, in);
		unclaimed_gen++;
		in_claim(r, in);
	}
	list_free(mine);

	SHM_LOG("interconnect shm: slice %d of statement %.8s receives from %d senders through shared memory",
			slice->slice, stream->token, slice->nsenders);
	return &r->base;
}

static bool
shm_recv(GpIcReceiver *receiver, char **data, int *len)
{
	ShmReceiver *r = (ShmReceiver *) receiver;

	if (r->tcp != NULL)
		return tcp_transport->recv(r->tcp, data, len);

	r_release(r);
	if (++r->rows >= SHM_POLL_ROWS)
		r_poll(r);
	for (;;)
	{
		int			n = list_length(r->ins);
		bool		progress = false;
		int			before;

		/* a row already written, the senders taking turns */
		for (int k = 0; k < n; k++)
		{
			int			i = (r->next + k) % n;
			ShmIn	   *in = (ShmIn *) list_nth(r->ins, i);
			int			got;

			if (in->ended)
				continue;
			got = in_take(r, in, data, len);
			if (got == SHM_TOOK_ROW)
			{
				r->next = (i + 1) % n;
				return true;
			}
			if (got == SHM_TOOK_END)
			{
				in_end(r, in);
				progress = true;
			}
			else if (in->eof)
				in_lost(r, in);
		}
		if (r->nended == r->nsenders)
			return false;
		if (progress)
			continue;

		before = list_length(r->ins);
		shm_accept();
		if (list_length(r->ins) != before)
			continue;
		r_wait(r, true, NULL);
	}
}

/*
 * The next row from one sender, for a merge of their streams: the k-th to
 * have come, in the order they came, which stays each one's.
 */
static bool
shm_recv_from(GpIcReceiver *receiver, int k, char **data, int *len)
{
	ShmReceiver *r = (ShmReceiver *) receiver;

	if (r->tcp != NULL)
		return tcp_transport->recv_from(r->tcp, k, data, len);

	Assert(k >= 0 && k < r->nsenders);
	r_release(r);
	for (;;)
	{
		ShmIn	   *in = k < list_length(r->ins)
			? (ShmIn *) list_nth(r->ins, k) : NULL;
		int			before;

		if (in != NULL)
		{
			int			got;

			if (in->ended)
				return false;
			got = in_take(r, in, data, len);
			if (got == SHM_TOOK_ROW)
				return true;
			if (got == SHM_TOOK_END)
			{
				in_end(r, in);
				return false;
			}
			if (in->eof)
				in_lost(r, in);
		}
		before = list_length(r->ins);
		shm_accept();
		if (list_length(r->ins) != before)
			continue;
		r_wait(r, false, in);
	}
}

/* Every connection of the receiver's let go, each sender told STOP. */
static void
r_close(ShmReceiver *r)
{
	receivers = list_delete_ptr(receivers, r);
	foreach_ptr(ShmIn, in, r->ins)
	{
		in_stop(in);
		in_close(in);
	}
	list_free(r->ins);
	r->ins = NIL;
	if (r->wes != NULL)
		FreeWaitEventSet(r->wes);
	if (r->wes_one != NULL)
		FreeWaitEventSet(r->wes_one);
	r->wes = r->wes_one = NULL;
	r->held = NULL;
	r->held_ring = NULL;
	r->ended = true;
}

/*
 * Done, whether or not every row came.  Every sender's connection is taken
 * first -- one not made yet waited for, as every sender connects to all its
 * receivers as its slice begins (gp_ic.c:1505-1520): a sender whose ring
 * nobody read would wait on it for ever -- and each told STOP.
 */
static void
shm_recv_end(GpIcReceiver *receiver)
{
	ShmReceiver *r = (ShmReceiver *) receiver;

	if (r->tcp != NULL)
	{
		GpIcReceiver *tcp = r->tcp;

		r->tcp = NULL;
		r->ended = true;
		tcp_transport->recv_end(tcp);
		return;
	}

	r_release(r);
	while (list_length(r->ins) < r->nsenders)
	{
		int			before = list_length(r->ins);

		shm_accept();
		if (list_length(r->ins) == before)
			r_wait(r, false, NULL);
	}
	r_close(r);
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

/* Does the receiver still want rows: no STOP, and its connection open? */
static bool
out_wanted(ShmOut *out)
{
	if (out->wanted && pg_atomic_read_u32(&out->slot->stop) != 0)
		out->wanted = false;
	return out->wanted;
}

/*
 * Does a ring's reader want rows: a receiver's own, or with "out" NULL any
 * reader of the broadcast ring?
 */
static bool
w_wanted(ShmSender *s, ShmOut *out)
{
	if (out != NULL)
		return out_wanted(out);
	for (int i = 0; i < s->nouts; i++)
		if (out_wanted(&s->outs[i]))
			return true;
	return false;
}

/*
 * The room in a ring, its readers' tails read again: in a receiver's own,
 * what it has not read; in the broadcast ring, what the furthest behind of
 * the receivers that want rows has not.
 */
static uint32
w_room(ShmSender *s, ShmOut *out)
{
	uint32		behind = 0;

	if (out != NULL)
	{
		out->tail = pg_atomic_read_u32(&out->slot->tail);
		behind = out->head - out->tail;
	}
	else
	{
		for (int i = 0; i < s->nouts; i++)
			if (out_wanted(&s->outs[i]))
				behind = Max(behind, s->bhead - pg_atomic_read_u32(&s->outs[i].slot->btail));
		s->btail = s->bhead - behind;
	}
	if (behind > s->size)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect: a receiver of slice %d says it has read rows not yet written",
						s->slice)));
	return s->size - behind;
}

/* The room as last seen, the tails not read again. */
static inline uint32
w_room_seen(ShmSender *s, ShmOut *out)
{
	return s->size - (out != NULL ? out->head - out->tail : s->bhead - s->btail);
}

/* A receiver that sleeps, rung if "least" bytes of rows wait for it. */
static void
w_wake(ShmSender *s, ShmOut *out, uint32 least)
{
	uint32		unread;

	if (pg_atomic_read_u32(&out->slot->waiting) == 0)
		return;
	unread = (out->said - pg_atomic_read_u32(&out->slot->tail)) +
		(s->bsaid - pg_atomic_read_u32(&out->slot->btail));
	if (unread >= least && pg_atomic_exchange_u32(&out->slot->waiting, 0) != 0)
		shm_ring_bell(out->bell);
}

/*
 * What is written is said so -- the ring's head moved in the channel -- and
 * a reader that sleeps is rung, once SHM_WAKE() waits for it, or now, at
 * "flush".
 */
static void
w_publish(ShmSender *s, ShmOut *out, bool flush)
{
	uint32		least = flush ? 1 : SHM_WAKE(s->size);

	if ((out != NULL ? out->said : s->bsaid) != (out != NULL ? out->head : s->bhead))
	{
		/* the frames written before the head says so */
		pg_write_barrier();
		if (out != NULL)
		{
			pg_atomic_write_u32(&out->slot->head, out->head);
			out->said = out->head;
		}
		else
		{
			pg_atomic_write_u32(&s->ch->bhead, s->bhead);
			s->bsaid = s->bhead;
		}
		/* and said so before a reader's sleep is looked at: it looks after */
		pg_memory_barrier();
	}
	if (out != NULL)
		w_wake(s, out, least);
	else
		for (int i = 0; i < s->nouts; i++)
			if (s->outs[i].wanted)
				w_wake(s, &s->outs[i], least);
}

/*
 * The receiver's connection is readable: closed -- it has what it needs, or
 * it failed, and says so itself -- or something a receiver never writes.
 * A receiver gone is stopped in its place too, so that the broadcast ring
 * waits for it no more.
 */
static void
out_check(ShmSender *s, ShmOut *out)
{
	char		buf[16];
	ssize_t		n = recv(out->sock, buf, sizeof(buf), MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n > 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect: a receiver of slice %d wrote on its connection",
						s->slice)));
	out->wanted = false;
	pg_atomic_write_u32(&out->slot->stop, 1);
	closesocket(out->sock);
	out->sock = PGINVALID_SOCKET;
	s->wes_stale = true;
}

/* The latch, the sender's bell, and its receivers' sockets. */
static void
s_sleep(ShmSender *s)
{
	int			n;

	if (s->wes == NULL || s->wes_stale)
	{
		int			size = 3 + s->nouts;

		if (s->wes != NULL)
			FreeWaitEventSet(s->wes);
		s->wes = NULL;
		s->wes = CreateWaitEventSet(NULL, size);
		AddWaitEventToSet(s->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(s->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
		AddWaitEventToSet(s->wes, WL_SOCKET_READABLE, s->room, NULL, NULL);
		for (int i = 0; i < s->nouts; i++)
			if (s->outs[i].sock != PGINVALID_SOCKET)
				AddWaitEventToSet(s->wes, WL_SOCKET_READABLE, s->outs[i].sock,
								  NULL, &s->outs[i]);
		if (s->occurred == NULL)
		{
			s->occurred = MemoryContextAlloc(GetMemoryChunkContext(s),
											 sizeof(WaitEvent) * size);
			s->nevents = size;
		}
		s->wes_stale = false;
	}

	n = WaitEventSetWait(s->wes, -1, s->occurred, s->nevents,
						 shm_wait_event(true));
	for (int i = 0; i < n; i++)
	{
		WaitEvent  *e = &s->occurred[i];

		if (e->events & WL_LATCH_SET)
			ResetLatch(MyLatch);
		else if (e->fd == s->room)
			shm_drain_bell(s->room);
		else if (e->user_data != NULL)
			out_check(s, (ShmOut *) e->user_data);
	}
}

/*
 * Everything written said, and every receiver that sleeps with rows written
 * for it rung, now.
 */
static void
s_flush(ShmSender *s)
{
	for (int i = 0; i < s->nouts; i++)
		if (s->outs[i].wanted)
			w_publish(s, &s->outs[i], true);
	w_publish(s, NULL, true);
}

/*
 * Sleep for "want" bytes of room in a ring.  The receivers that sleep with
 * rows to read are rung first: any of them could be what frees it.  The
 * sender says which ring it sleeps for, and how much its reader is to free
 * before it rings -- a quarter of the ring, or what it wants -- then that
 * it sleeps, and looks a last time.
 */
static void
s_wait(ShmSender *s, ShmOut *out, uint32 want)
{
	uint32		ring = out != NULL ? (uint32) (out - s->outs) : (uint32) s->nouts;

	s_flush(s);
	pg_atomic_write_u32(&s->ch->writer_ring, ring);
	pg_atomic_write_u32(&s->ch->writer_need, Min(s->size, Max(want, s->size / 4)));
	pg_write_barrier();
	pg_atomic_write_u32(&s->ch->writer_waiting, 1);
	pg_memory_barrier();
	if (w_room(s, out) < want && w_wanted(s, out))
		s_sleep(s);
	pg_atomic_write_u32(&s->ch->writer_waiting, 0);
	CHECK_FOR_INTERRUPTS();
}

/*
 * A frame into a ring -- its header, "prefix" and "data" -- room waited for
 * as long as a reader of the ring wants rows; nothing, once none does.
 */
static void
w_put(ShmSender *s, ShmOut *out, uint32 kind, const char *prefix,
	  uint32 prefixlen, const char *data, uint32 len)
{
	char	   *ring = out != NULL ? out->ring : s->bring;
	uint32	   *head = out != NULL ? &out->head : &s->bhead;
	uint32	   *off = out != NULL ? &out->off : &s->boff;
	uint32		need = SHM_FRAME(prefixlen + len);
	ShmFrame   *f;

	Assert(need <= s->size);
	for (;;)
	{
		uint32		contiguous = s->size - *off;
		uint32		want = Min(need, contiguous);

		if (!w_wanted(s, out))
			return;
		if (w_room_seen(s, out) < want && w_room(s, out) < want)
		{
			s_wait(s, out, want);
			continue;
		}
		if (need <= contiguous)
			break;

		/* the ring's end skipped, and the frame written at its start */
		f = (ShmFrame *) (ring + *off);
		f->len = contiguous - SHM_HEADER;
		f->kind = SHM_SKIP;
		*head += contiguous;
		*off = 0;
	}

	f = (ShmFrame *) (ring + *off);
	f->len = prefixlen + len;
	f->kind = kind;
	if (prefixlen > 0)
		memcpy(((char *) f) + SHM_HEADER, prefix, prefixlen);
	if (len > 0)
		memcpy(((char *) f) + SHM_HEADER + prefixlen, data, len);
	*head += need;
	*off += need;
	if (*off == s->size)
		*off = 0;
	if (kind == SHM_END || kind == SHM_PIECE_LAST ||
		*head - (out != NULL ? out->said : s->bsaid) >= SHM_PUBLISH(s->size))
		w_publish(s, out, kind == SHM_END || kind == SHM_PIECE_LAST);
}

/*
 * A row into a ring: whole where it fits, which is the one copy; and a row
 * longer than the ring in pieces of half of it -- so that the next is
 * written as the reader gathers the last -- the first with the row's length.
 */
static void
s_put_row(ShmSender *s, ShmOut *out, const char *data, int len)
{
	uint32		piece;
	uint32		done;
	uint32		total[2];

	if (SHM_FRAME(len) <= s->size)
	{
		w_put(s, out, SHM_ROW, NULL, 0, data, len);
		return;
	}

	piece = s->size / 2 - SHM_HEADER;
	total[0] = (uint32) len;
	total[1] = 0;
	done = piece - sizeof(total);
	w_put(s, out, SHM_PIECE_FIRST, (const char *) total, sizeof(total), data, done);
	while (done < (uint32) len && w_wanted(s, out))
	{
		uint32		n = Min(piece, (uint32) len - done);

		w_put(s, out, done + n == (uint32) len ? SHM_PIECE_LAST : SHM_PIECE,
			  NULL, 0, data + done, n);
		done += n;
	}
}

/*
 * The channel's file: a memfd, its size sealed, so that a receiver that
 * has checked it can map all of it; or, where there is no memfd_create()
 * or gp.shm_debug_shm_open says so, a POSIX shared-memory object named by
 * the statement's token, unlinked as soon as it is made -- its descriptor
 * goes to the receivers -- and its pages reserved, as PostgreSQL's own
 * POSIX DSM reserves them (PostgreSQL's dsm_impl.c:352-395): a page
 * /dev/shm had no room for would be a SIGBUS when written.
 */
static int
s_file(ShmStream *ss, const char *token, int slice, Size size, bool *sealed)
{
	char	   *name;
	int			fd;
	int			rc;

	*sealed = false;
#ifdef HAVE_MEMFD_CREATE
	if (!ss->shm_open)
	{
		name = psprintf("gp_shm.%.8s.%d.%d", token, slice, GpClusterContentId());
		fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (fd >= 0)
		{
			if (ftruncate(fd, size) < 0 ||
				fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
			{
				int			save = errno;

				close(fd);
				errno = save;
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 errmsg("could not size the interconnect's shared memory to %zu bytes: %m",
								size)));
			}
			*sealed = true;
			return fd;
		}
		if (errno != ENOSYS)
			ereport(ERROR,
					(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					 errmsg("could not create the interconnect's shared memory: %m")));
	}
#endif

	/* the process's id too: a segment may send one slice from two processes */
	name = psprintf("/gp_shm.%s.%d.%d.%d", token, slice, GpClusterContentId(), MyProcPid);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not create the interconnect's shared memory \"%s\": %m",
						name)));
	shm_unlink(name);
	rc = ftruncate(fd, size) < 0 ? errno : 0;
#ifdef HAVE_POSIX_FALLOCATE
	if (rc == 0)
	{
		do
			rc = posix_fallocate(fd, 0, size);
		while (rc == EINTR);
	}
#endif
	if (rc != 0)
	{
		close(fd);
		errno = rc;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not size the interconnect's shared memory \"%s\" to %zu bytes: %m",
						name, size)));
	}
	return fd;
}

/*
 * Connect to a receiver's socket, kept in "out" as soon as it is made, so
 * that the statement's end closes it whatever happens: as long as it takes,
 * interruptibly.  A full backlog is EAGAIN on a Unix socket, not a connect
 * in progress (gp_ic.c:1610).
 */
static void
s_connect(ShmOut *out, const char *path)
{
	struct sockaddr_un addr;
	int			rc;

	if (strlen(path) >= sizeof(addr.sun_path))
		elog(ERROR, "invalid shm interconnect socket \"%s\"", path);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	out->sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (out->sock == PGINVALID_SOCKET || !pg_set_noblock(out->sock))
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not create a socket for the interconnect: %m")));

	while ((rc = connect(out->sock, (struct sockaddr *) &addr, sizeof(addr))) < 0 &&
		   (errno == EAGAIN || errno == EINTR))
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10, shm_wait_event(true));
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	if (rc < 0 && errno == EINPROGRESS)
	{
		for (;;)
		{
			int			err = 0;
			socklen_t	errlen = sizeof(err);
			int			ev = WaitLatchOrSocket(MyLatch,
											   WL_LATCH_SET | WL_SOCKET_CONNECTED |
											   WL_EXIT_ON_PM_DEATH,
											   out->sock, -1, shm_wait_event(true));

			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			if (!(ev & WL_SOCKET_CONNECTED))
				continue;
			if (getsockopt(out->sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0)
				err = errno;
			if (err == EINPROGRESS || err == EAGAIN)
				continue;
			rc = err == 0 ? 0 : -1;
			errno = err;
			break;
		}
	}
	if (rc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("interconnect: could not connect to \"%s\": %m", path)));
}

/*
 * The hello, and with its first byte the channel's descriptors: written
 * whole, waiting for room as need be.  A receiver whose connection fails
 * meanwhile is gone, as tcp's handshake to one is (gp_ic.c:2192-2194, 1737).
 */
static void
s_hello(ShmSender *s, ShmOut *out, const ShmHello *hello, const int *fds)
{
	const char *p = (const char *) hello;
	size_t		left = sizeof(ShmHello);
	bool		first = true;
	int			flags = 0;

#ifdef MSG_NOSIGNAL
	flags |= MSG_NOSIGNAL;
#endif
	while (left > 0)
	{
		struct msghdr msg;
		struct iovec iov;
		union
		{
			struct cmsghdr align;
			char		buf[CMSG_SPACE(sizeof(int) * SHM_NFDS)];
		}			control;
		ssize_t		n;

		memset(&msg, 0, sizeof(msg));
		iov.iov_base = unconstify(char *, p);
		iov.iov_len = left;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		if (first)
		{
			struct cmsghdr *c;

			memset(&control, 0, sizeof(control));
			msg.msg_control = control.buf;
			msg.msg_controllen = sizeof(control.buf);
			c = CMSG_FIRSTHDR(&msg);
			c->cmsg_level = SOL_SOCKET;
			c->cmsg_type = SCM_RIGHTS;
			c->cmsg_len = CMSG_LEN(sizeof(int) * SHM_NFDS);
			memcpy(CMSG_DATA(c), fds, sizeof(int) * SHM_NFDS);
		}
		n = sendmsg(out->sock, &msg, flags);
		if (n > 0)
		{
			p += n;
			left -= n;
			first = false;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		{
			int			ev = WaitLatchOrSocket(MyLatch,
											   WL_LATCH_SET | WL_SOCKET_WRITEABLE |
											   WL_EXIT_ON_PM_DEATH,
											   out->sock, -1, shm_wait_event(true));

			if (ev & WL_LATCH_SET)
				ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		out->wanted = false;
		pg_atomic_write_u32(&out->slot->stop, 1);
		closesocket(out->sock);
		out->sock = PGINVALID_SOCKET;
		return;
	}
}

/*
 * The channel: its file, mapped and laid out, the sender's bell, and each
 * receiver connected to and given the file, its bell and the sender's.  The
 * sender's descriptor of the file is closed once they have it: its mapping
 * keeps the file, and each receiver's will.
 */
static void
s_open(ShmSender *s, GpIcStream *stream, GpIcSlice *slice, ShmPeer *peers)
{
	ShmStream  *ss = s->ss;
	Size		data = shm_data_offset(s->nouts);
	bool		sealed;

	s->size = ss->ring_size;
	s->mapsize = shm_map_size(s->nouts, s->size);
	s->mapfd = s_file(ss, stream->token, slice->slice, s->mapsize, &sealed);
	s->map = mmap(NULL, s->mapsize, PROT_READ | PROT_WRITE, MAP_SHARED, s->mapfd, 0);
	if (s->map == MAP_FAILED)
	{
		s->map = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errmsg("could not map the interconnect's shared memory: %m")));
	}

	s->ch = (ShmChannel *) s->map;
	s->ch->magic = SHM_CHANNEL_MAGIC;
	s->ch->version = SHM_VERSION;
	s->ch->nreaders = s->nouts;
	s->ch->ring_size = s->size;
	s->ch->map_size = s->mapsize;
	s->ch->data_offset = data;
	s->ch->slice = slice->slice;
	s->ch->sender = GpClusterContentId();
	memcpy(s->ch->token, stream->token, GP_IC_TOKEN_LEN);
	pg_atomic_init_u32(&s->ch->bhead, 0);
	pg_atomic_init_u32(&s->ch->writer_waiting, 0);
	pg_atomic_init_u32(&s->ch->writer_ring, 0);
	pg_atomic_init_u32(&s->ch->writer_need, 0);
	for (int i = 0; i < s->nouts; i++)
	{
		ShmSlot    *slot = &s->ch->slots[i];

		pg_atomic_init_u32(&slot->head, 0);
		pg_atomic_init_u32(&slot->tail, 0);
		pg_atomic_init_u32(&slot->btail, 0);
		pg_atomic_init_u32(&slot->waiting, 0);
		pg_atomic_init_u32(&slot->stop, 0);
		s->outs[i].slot = slot;
		s->outs[i].ring = s->map + data + (Size) i * s->size;
		s->outs[i].wanted = true;
	}
	s->bring = s->map + data + (Size) s->nouts * s->size;
	s->room = shm_bell();
	pg_write_barrier();

	for (int i = 0; i < s->nouts; i++)
	{
		ShmOut	   *out = &s->outs[i];
		ShmHello	hello;
		int			fds[SHM_NFDS];

		out->bell = shm_bell();
		s_connect(out, peers[out->receiver].path);
		memset(&hello, 0, sizeof(hello));
		memcpy(hello.magic, SHM_MAGIC, 4);
		hello.version = SHM_VERSION;
		memcpy(hello.token, stream->token, GP_IC_TOKEN_LEN);
		hello.slice = slice->slice;
		hello.sender = GpClusterContentId();
		hello.index = i;
		hello.nreaders = s->nouts;
		hello.ring_size = s->size;
		hello.flags = sealed ? SHM_HELLO_SEALED : 0;
		hello.map_size = s->mapsize;
		fds[0] = s->mapfd;
		fds[1] = out->bell;
		fds[2] = s->room;
		s_hello(s, out, &hello, fds);
	}

	close(s->mapfd);
	s->mapfd = -1;
}

/* What the sender holds of the system's, let go: its connections, its file. */
static void
sender_close(ShmSender *s)
{
	if (s->closed)
		return;
	s->closed = true;
	for (int i = 0; i < s->nreceivers; i++)
	{
		if (s->outs[i].sock != PGINVALID_SOCKET)
			closesocket(s->outs[i].sock);
		s->outs[i].sock = PGINVALID_SOCKET;
		if (s->outs[i].bell >= 0)
			close(s->outs[i].bell);
		s->outs[i].bell = -1;
	}
	if (s->room >= 0)
		close(s->room);
	s->room = -1;
	if (s->mapfd >= 0)
		close(s->mapfd);
	s->mapfd = -1;
	if (s->map != NULL)
		munmap(s->map, s->mapsize);
	s->map = NULL;
	s->ch = NULL;
	if (s->wes != NULL)
		FreeWaitEventSet(s->wes);
	s->wes = NULL;
}

static GpIcSender *
shm_send_begin(GpIcStream *stream, GpIcSlice *slice)
{
	ShmStream  *ss = shm_stream(stream);
	MemoryContext cxt = GetMemoryChunkContext(stream);
	ShmSender  *s = MemoryContextAllocZero(cxt, sizeof(ShmSender));
	int			n = Max(slice->nreceivers, 1);
	ShmPeer    *peers = palloc_array(ShmPeer, n);
	MemoryContext oldcxt;

	s->ss = ss;
	s->slice = slice->slice;
	s->nreceivers = slice->nreceivers;
	s->mapfd = -1;
	s->room = -1;
	s->outs = MemoryContextAllocZero(cxt, sizeof(ShmOut) * n);
	for (int i = 0; i < n; i++)
	{
		s->outs[i].receiver = i;
		s->outs[i].sock = PGINVALID_SOCKET;
		s->outs[i].bell = -1;
	}
	oldcxt = MemoryContextSwitchTo(cxt);
	ss->senders = lappend(ss->senders, s);
	MemoryContextSwitchTo(oldcxt);
	for (int i = 0; i < slice->nreceivers; i++)
		shm_peer(slice->receiver_addresses[i], &peers[i]);

	/* a slice that spans hosts: its rows over tcp, to tcp's addresses */
	if (!shm_through_rings(ss, slice))
	{
		s->tcp_slice = *slice;
		s->tcp_slice.receiver_addresses = MemoryContextAlloc(cxt, sizeof(char *) * n);
		for (int i = 0; i < slice->nreceivers; i++)
			s->tcp_slice.receiver_addresses[i] = MemoryContextStrdup(cxt, peers[i].tcp);
		SHM_LOG("interconnect shm: slice %d of statement %.8s sends to %d receivers over tcp",
				slice->slice, stream->token, slice->nreceivers);
		s->tcp = tcp_transport->send_begin(stream, &s->tcp_slice);
		return &s->base;
	}

	s->nouts = slice->nreceivers;
	SHM_LOG("interconnect shm: slice %d of statement %.8s sends to %d receivers through shared memory (%s)",
			slice->slice, stream->token, slice->nreceivers,
			ss->shm_open ? "POSIX" : "memfd");
	if (s->nouts > 0)
		s_open(s, stream, slice, peers);
	return &s->base;
}

/* A row to one receiver, or to every one with -1: written once, then. */
static void
shm_send(GpIcSender *sender, int receiver, const char *data, int len)
{
	ShmSender  *s = (ShmSender *) sender;

	if (s->tcp != NULL)
		tcp_transport->send(s->tcp, receiver, data, len);
	else if (receiver >= 0)
	{
		Assert(receiver < s->nouts);
		s_put_row(s, &s->outs[receiver], data, len);
	}
	else if (s->nouts > 0)
		s_put_row(s, NULL, data, len);
}

static bool
shm_send_wanted(GpIcSender *sender)
{
	ShmSender  *s = (ShmSender *) sender;

	if (s->tcp != NULL)
		return tcp_transport->send_wanted(s->tcp);
	for (int i = 0; i < s->nouts; i++)
		if (out_wanted(&s->outs[i]))
			return true;
	return false;
}

/*
 * The end, in each ring a receiver still wants rows from, and rung; then
 * what the sender holds let go.  A receiver yet to map the channel maps it
 * from the descriptor its connection brings, whose rows and end the
 * channel keeps for it.
 */
static void
shm_send_end(GpIcSender *sender)
{
	ShmSender  *s = (ShmSender *) sender;

	if (s->tcp != NULL)
	{
		GpIcSender *tcp = s->tcp;

		s->tcp = NULL;
		s->closed = true;
		tcp_transport->send_end(tcp);
		return;
	}
	for (int i = 0; i < s->nouts; i++)
		w_put(s, &s->outs[i], SHM_END, NULL, 0, NULL, 0);
	if (s->nouts > 0)
		w_put(s, NULL, SHM_END, NULL, 0, NULL, 0);
	sender_close(s);
}

/* ------------------------------------------------------------------------- */
/* A statement                                                               */
/* ------------------------------------------------------------------------- */

/*
 * The statement begins here: its settings as the coordinator sent them,
 * kept for its senders and receivers, which may begin later -- a cursor's
 * at its first FETCH -- after the session has set them otherwise.
 */
static void
shm_stmt_begin(GpIcStream *stream)
{
	MemoryContext cxt = GetMemoryChunkContext(stream);
	ShmStream  *ss = MemoryContextAllocZero(cxt, sizeof(ShmStream));

	ss->ring_size = (uint32) shm_ring_size * 1024;
	ss->shm_open = shm_debug_shm_open;
	if (shm_remote != NULL)
	{
		Size		size = offsetof(ShmRemote, pairs) + shm_remote->npairs * sizeof(ShmPair);

		ss->remote = MemoryContextAlloc(cxt, size);
		memcpy(ss->remote, shm_remote, size);
	}
	stream->state = ss;
}

/*
 * The statement's end here: every sender and receiver of its let go -- a
 * receiver's senders told STOP -- and the connections that came for it and
 * were never asked for closed; then tcp's end, for what it carried.  At an
 * error nothing here fails: nothing but descriptors and mappings is
 * released, and tcp's end does nothing then (gp_ic.c:2314-2319).
 */
static void
shm_stmt_end(GpIcStream *stream, bool error)
{
	ShmStream  *ss = (ShmStream *) stream->state;
	ListCell   *lc;

	if (ss == NULL)
		return;
	stream->state = NULL;

	foreach_ptr(ShmSender, s, ss->senders)
		sender_close(s);
	foreach_ptr(ShmReceiver, r, ss->receivers)
		if (!r->ended && r->tcp == NULL)
			r_close(r);
	foreach(lc, unclaimed)
	{
		ShmIn	   *in = (ShmIn *) lfirst(lc);

		if (in->hellolen == (int) sizeof(ShmHello) &&
			memcmp(in->hello.token, stream->token, GP_IC_TOKEN_LEN) == 0)
		{
			unclaimed = foreach_delete_current(unclaimed, lc);
			unclaimed_gen++;
			in_close(in);
		}
	}
	tcp_transport->stmt_end(stream, error);
}

/*
 * Connections nobody asked for are closed when a transaction ends, as tcp's
 * are (gp_ic.c:2647-2650): every statement here has ended with it
 * (gp_ic.c:2563-2578), whose own ends close what is theirs, after this.
 */
static void
shm_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
		return;
	foreach_ptr(ShmIn, in, unclaimed)
		in_close(in);
	list_free(unclaimed);
	unclaimed = NIL;
	unclaimed_gen++;
	list_free(receivers);
	receivers = NIL;
}

static const GpIcTransport shm_transport = {
	.name = "shm",
	.address = shm_address,
	.stmt_begin = shm_stmt_begin,
	.send_begin = shm_send_begin,
	.send = shm_send,
	.send_wanted = shm_send_wanted,
	.send_end = shm_send_end,
	.recv_begin = shm_recv_begin,
	.recv = shm_recv,
	.recv_from = shm_recv_from,
	.recv_end = shm_recv_end,
	.stmt_end = shm_stmt_end,
};

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("shm");
	CB_REQUIRE_CORE("shm");

	tcp_transport = GpIcFindTransport("tcp");
	if (tcp_transport == NULL)
		elog(ERROR, "shm: gp_core has registered no tcp transport");

	DefineCustomIntVariable("gp.shm_ring_size",
							"Sets the size of each ring of the shared-memory interconnect.",
							"The room a receiver has for each sender's rows on its host, as gp.interconnect_queue_depth is the UDP interconnect's: a sender whose ring is full waits for its receiver to read.  A row longer than the ring goes in pieces, which the receiver copies together.",
							&shm_ring_size,
							256, SHM_RING_MIN_KB, SHM_RING_MAX_KB,
							PGC_USERSET, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomStringVariable("gp.shm_debug_remote",
							   "Makes pairs of segment processes count as on two hosts in the shared-memory interconnect, for testing.",
							   "\"*\" for every pair, or a list of \"sender:receiver\", each a content id or \"*\": a slice with such a pair goes over tcp.",
							   &shm_debug_remote,
							   "", PGC_USERSET,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   shm_remote_check, shm_remote_assign, NULL);
	DefineCustomBoolVariable("gp.shm_debug_shm_open",
							 "Makes the shared-memory interconnect use POSIX shared memory where memfd_create() is, for testing.",
							 NULL,
							 &shm_debug_shm_open,
							 false, PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	GpIcRegisterTransport(&shm_transport);
	if (GpClusterIsSingleNode())
		return;
	RegisterXactCallback(shm_xact_callback, NULL);
}
