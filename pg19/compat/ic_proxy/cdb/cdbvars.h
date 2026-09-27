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
 * ic_proxy/cdb/cdbvars.h
 *	  The include overlay of Cloudberry's interconnect proxy: what the files
 *	  of its server, compiled where they lie, ask of Cloudberry's cdbvars.h.
 *
 * The interconnect module compiles the proxy's server
 * (contrib/interconnect/proxy, but for ic_proxy_backend.c, whose place
 * modules/interconnect/proxy_backend.c takes) unchanged, with this directory
 * first on its include path (meson.build).  What the files ask of
 * Cloudberry's globals is answered here, in the port's terms: the node the
 * proxy runs on and the packet size it was started with, which the proxy's
 * worker sets as it starts (modules/interconnect/interconnect.c), and the
 * settings, gp_core's gp.log_interconnect and the module's
 * gp.interconnect_proxy_addresses.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_IC_PROXY_CDBVARS_H
#define GP_COMPAT_IC_PROXY_CDBVARS_H

#include "postgres.h"

#include "gp_ic.h"

/* Cloudberry's gp_log_interconnect's values */
#define GPVARS_VERBOSITY_OFF		GP_IC_VERBOSITY_OFF
#define GPVARS_VERBOSITY_TERSE		GP_IC_VERBOSITY_TERSE
#define GPVARS_VERBOSITY_VERBOSE	GP_IC_VERBOSITY_VERBOSE
#define GPVARS_VERBOSITY_DEBUG		GP_IC_VERBOSITY_DEBUG

/* Cloudberry's elog.h: a message logged only where "p" says so */
#define elogif(p, ...) \
	do { \
		if (p) \
			elog(__VA_ARGS__); \
	} while (false)

/* The node this is: Cloudberry's gp_id row */
typedef struct GpId
{
	int32		dbid;
	int32		segindex;
} GpId;

extern PGDLLIMPORT GpId gp_ic_proxy_identity;
#define GpIdentity gp_ic_proxy_identity

/* The most a packet carries, fixed as the proxy starts */
extern PGDLLIMPORT int gp_ic_proxy_packet_size;
#define Gp_max_packet_size gp_ic_proxy_packet_size

/* Every node's proxy: dbid:content:host:port, and a comma between two */
extern PGDLLIMPORT char *gp_interconnect_proxy_addresses;

#endif							/* GP_COMPAT_IC_PROXY_CDBVARS_H */
