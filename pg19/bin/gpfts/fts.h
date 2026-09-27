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
 * fts.h
 *	  gpfts, the coordinator's automatic failover: what its two halves share.
 *	  See fts.c and ftsprobe.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef FTS_H
#define FTS_H

/* Cloudberry's defaults (src/bin/gpfts/fts.h). */
#define GP_FTS_PROBE_RETRIES	5
#define GP_FTS_PROBE_TIMEOUT	20
#define GP_FTS_PROBE_INTERVAL	60

/* What the options say. */
typedef struct fts_config
{
	int			probe_retries;	/* -R: attempts after the first */
	int			probe_timeout;	/* -T: seconds an attempt may take */
	int			probe_interval; /* -I: seconds from a round to the next */
	char	   *user;			/* -U: whom to connect as, NULL the default */
	bool		disable_promote_standby;	/* -D */
	bool		one_round;		/* -A */
} fts_config;

/* fts.c: the clock, and waiting, which keeps the leader's lease alive. */
extern int64 FtsNow(void);
extern void FtsTick(void);
extern void FtsWait(int64 ms);
extern void FtsCheckLease(void);

/* ftsprobe.c: a round, and the probe -W 4 makes. */
extern void FtsProbeRound(const fts_config *config);
extern bool FtsProbeOnce(const fts_config *config);

#endif							/* FTS_H */
