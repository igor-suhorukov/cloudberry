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
 * gp_ic.h
 *	  The interconnect: a Motion's rows from the segment processes that send
 *	  them to the ones that receive them; see gp_ic.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_IC_H
#define GP_IC_H

#include "postgres.h"

/* Cloudberry's SQLSTATE for an interconnect failure. */
#define ERRCODE_GP_INTERCONNECTION_ERROR	MAKE_SQLSTATE('5','8','M','0','1')

/* A statement's token: what a sender shows a receiver, in hex. */
#define GP_IC_TOKEN_LEN		32

/*
 * A row's frame, which every transport's stream of rows is made of: its
 * length, four bytes in network order, and the row; or this, the end of a
 * sender's rows.
 */
#define GP_IC_END_OF_ROWS	0xFFFFFFFF

/* A Motion's rows on their way from this process to their receivers. */
typedef struct GpIcSender GpIcSender;

/* And as they reach this process from its senders. */
typedef struct GpIcReceiver GpIcReceiver;

struct GpIcTransport;

/*
 * A slice whose rows stream, as the coordinator told each process of the
 * statement: the processes that run it -- a reader on each segment that runs
 * it -- and the ones that run the slice that receives it, each by its
 * segment's content id and where it receives over the statement's transport
 * (GpIcAddress()).  A sender's receiver i is receiver_addresses[i].
 */
typedef struct GpIcSlice
{
	int			slice;
	int			parent;			/* the slice that receives it */
	int			nsenders;
	int		   *sender_contents;
	char	  **sender_addresses;
	int			nreceivers;
	int		   *receiver_contents;
	char	  **receiver_addresses;
} GpIcSlice;

/*
 * A statement whose slices run at once, as one of its segment processes has
 * it (gp_motion.c): from its fragment's start until GpIcForget(), or until
 * the (sub)transaction it began in ends.  "self" is the slice this process
 * runs: the one a reader's fragment sends, or "top", the Gather's, which the
 * writers run.  It is allocated in a memory context of its own, which its
 * end deletes, and where its transport may keep what is the statement's.
 */
typedef struct GpIcStream
{
	char		token[GP_IC_TOKEN_LEN + 1];
	const struct GpIcTransport *transport;
	int			session;		/* the coordinator's, GpClusterSessionId() */
	uint32		serial;			/* the statement's number in the session, from 1 */
	int			top;
	int			self;
	int			nslices;
	GpIcSlice  *slices;
	SubTransactionId subxid;	/* where it began */
	void	   *state;			/* the transport's own */
} GpIcStream;

/*
 * A transport: how a Motion's rows travel, as a MotionIPCLayer of
 * Cloudberry's says for its interconnect.  gp_core has tcp and udpifc
 * (gp_ic.c); a module registers another from its _PG_init while it is
 * preloaded, and gp.interconnect_type takes its name from then on.
 *
 *   address      where this process receives, opening what it must the
 *                first time; the coordinator asks each process it gives a
 *                slice, and tells the senders their receivers' addresses
 *   stmt_begin   a statement begins here, before its plan is initialised
 *                (NULL: nothing to do)
 *   send_begin   this process sends "slice" to its receivers
 *   send         one row, to receiver "receiver", or to every one with -1;
 *                a receiver that has gone is left out from then on
 *   send_wanted  does any receiver still want rows?
 *   send_end     the end of the rows, to every receiver still there
 *   recv_begin   this process receives "slice" from its senders
 *   recv         the next row from any sender, valid until the next call;
 *                false once every sender has sent its last
 *   recv_from    the next row from one sender, for a merge of their streams:
 *                the k-th of the slice's senders to have come, in the order
 *                they came, which stays each one's; false once it has sent
 *                its last
 *   recv_end     done, whether or not every row came: the senders stop
 *   stmt_end     the statement is over here: with "error", because its
 *                (sub)transaction is aborting, and it must not fail again
 *
 * A transport's senders and receivers begin with a GpIcSender and a
 * GpIcReceiver, which say whose they are.
 */
typedef struct GpIcTransport
{
	const char *name;
	const char *(*address) (void);
	void		(*stmt_begin) (GpIcStream *stream);
	GpIcSender *(*send_begin) (GpIcStream *stream, GpIcSlice *slice);
	void		(*send) (GpIcSender *sender, int receiver, const char *data,
						 int len);
	bool		(*send_wanted) (GpIcSender *sender);
	void		(*send_end) (GpIcSender *sender);
	GpIcReceiver *(*recv_begin) (GpIcStream *stream, GpIcSlice *slice);
	bool		(*recv) (GpIcReceiver *receiver, char **data, int *len);
	bool		(*recv_from) (GpIcReceiver *receiver, int k, char **data,
							  int *len);
	void		(*recv_end) (GpIcReceiver *receiver);
	void		(*stmt_end) (GpIcStream *stream, bool error);
} GpIcTransport;

struct GpIcSender
{
	const GpIcTransport *transport;
};

struct GpIcReceiver
{
	const GpIcTransport *transport;
};

/* The most transports a server has: gp_core's two, and the modules'. */
#define GP_IC_MAX_TRANSPORTS	8

/* A module's transport, registered from its _PG_init. */
extern PGDLLEXPORT void GpIcRegisterTransport(const GpIcTransport *transport);

/* The transport of that name, or NULL where nothing registered it. */
extern PGDLLEXPORT const GpIcTransport *GpIcFindTransport(const char *name);

/* Its place among the registered ones: from 0, below GP_IC_MAX_TRANSPORTS. */
extern PGDLLEXPORT int GpIcTransportIndex(const GpIcTransport *transport);

/* Where this process receives over a transport. */
extern PGDLLEXPORT const char *GpIcAddress(const GpIcTransport *transport);

/*
 * A statement's stream begins here, its transport told; the one of a token,
 * or NULL; and its end here, its transport told and the stream freed.
 */
extern PGDLLEXPORT void GpIcStatementBegin(GpIcStream *stream);
extern PGDLLEXPORT GpIcStream *GpIcStatementFind(const char *token);
extern PGDLLEXPORT void GpIcForget(GpIcStream *stream);

/* A slice of the stream that streams, by its number; NULL if it does not. */
extern PGDLLEXPORT GpIcSlice *GpIcStreamSlice(GpIcStream *stream, int slice);

/*
 * Through the stream's transport's table.  The two Begins are where
 * Cloudberry sets up its interconnect: the fault interconnect_setup_palloc,
 * once each, whatever the transport.
 */
extern PGDLLEXPORT GpIcSender *GpIcSendBegin(GpIcStream *stream, int slice);
extern PGDLLEXPORT void GpIcSend(GpIcSender *sender, int receiver,
								 const char *data, int len);
extern PGDLLEXPORT bool GpIcSendWanted(GpIcSender *sender);
extern PGDLLEXPORT void GpIcSendEnd(GpIcSender *sender);
extern PGDLLEXPORT GpIcReceiver *GpIcRecvBegin(GpIcStream *stream, int slice);
extern PGDLLEXPORT bool GpIcRecv(GpIcReceiver *receiver, char **data, int *len);
extern PGDLLEXPORT bool GpIcRecvFrom(GpIcReceiver *receiver, int k,
									 char **data, int *len);
extern PGDLLEXPORT void GpIcRecvEnd(GpIcReceiver *receiver);

/*
 * The UDP interconnect's settings, Cloudberry's gp_interconnect_* and
 * gp_udpic_*, which udpifc and udp2 read; and gp.log_interconnect, with
 * Cloudberry's values (GPVARS_VERBOSITY_*).
 */
extern PGDLLIMPORT int gp_interconnect_queue_depth;
extern PGDLLIMPORT int gp_max_packet_size;
extern PGDLLIMPORT int gp_interconnect_transmit_timeout;
extern PGDLLIMPORT int gp_interconnect_min_rto;
extern PGDLLIMPORT int gp_interconnect_default_rtt;
extern PGDLLIMPORT int gp_interconnect_snd_queue_depth;
extern PGDLLIMPORT int gp_interconnect_min_retries_before_timeout;
extern PGDLLIMPORT int gp_interconnect_debug_retry_interval;
extern PGDLLIMPORT bool gp_interconnect_cache_future_packets;
extern PGDLLIMPORT int gp_interconnect_timer_period;
extern PGDLLIMPORT int gp_interconnect_timer_checking_period;
extern PGDLLIMPORT int gp_interconnect_fc_method;
extern PGDLLIMPORT int gp_udpic_dropacks_percent;
extern PGDLLIMPORT int gp_udpic_dropxmit_percent;
extern PGDLLIMPORT int gp_log_interconnect;

#define GP_IC_VERBOSITY_OFF			1
#define GP_IC_VERBOSITY_TERSE		2
#define GP_IC_VERBOSITY_VERBOSE		3
#define GP_IC_VERBOSITY_DEBUG		4

/* The settings, gp_core's transports, and the transaction callbacks. */
extern void GpIcInit(void);

#endif							/* GP_IC_H */
