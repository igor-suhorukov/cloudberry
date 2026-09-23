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
 * Where this backend receives rows, opening its listener on first use:
 * "unix:<path>" beside the node's socket, or "tcp:<host>:<port>".
 */
extern const char *GpIcAddress(void);

/* A slice's rows on their way from this process to its receivers. */
typedef struct GpIcSender GpIcSender;

/*
 * Connect to the receivers of "slice": one per receiving segment, at
 * "addresses".  "self" is this segment's content id, which a receiver is
 * told.
 */
extern GpIcSender *GpIcSendBegin(const char *token, int slice, int self,
								 int nreceivers, char **addresses);

/*
 * One row, to receiver "receiver" (an index into the addresses), or to every
 * one with -1.  A receiver that has gone -- it needs no more rows -- is left
 * out from then on.
 */
extern void GpIcSend(GpIcSender *sender, int receiver, const char *data,
					 int len);

/* Does any receiver still want rows? */
extern bool GpIcSendWanted(GpIcSender *sender);

/* The end of the rows, to every receiver still there. */
extern void GpIcSendEnd(GpIcSender *sender);

/* A Motion's rows as they reach this process, from its "nsenders" senders. */
typedef struct GpIcReceiver GpIcReceiver;

extern GpIcReceiver *GpIcRecvBegin(const char *token, int slice, int nsenders);

/*
 * The next row from any sender, valid until the next call; false once every
 * sender has sent its last.
 */
extern bool GpIcRecv(GpIcReceiver *receiver, char **data, int *len);

/* Done, whether or not every row arrived: the senders stop. */
extern void GpIcRecvEnd(GpIcReceiver *receiver);

/* The statement "token" names is over here: close what it left waiting. */
extern void GpIcForget(const char *token);

/* The transaction callback that closes what an error left open. */
extern void GpIcInit(void);

#endif							/* GP_IC_H */
