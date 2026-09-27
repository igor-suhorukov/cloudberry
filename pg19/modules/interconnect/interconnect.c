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
 * interconnect.c
 *	  Motion transports: the proxy.
 *
 * Cloudberry's interconnect module carries tcp, udpifc and the proxy.  The
 * port's tcp and udpifc are gp_core's (gp_ic.c), beside the Motions they
 * carry; this module is the proxy's, Cloudberry's ic-proxy: a background
 * worker on each node, the proxy, which carries every pair of nodes' Motions
 * over one TCP connection between their proxies, the nodes' addresses given
 * by gp.interconnect_proxy_addresses; and its backend half, the transport
 * gp.interconnect_type = proxy names, by which a segment process sends and
 * receives a Motion's rows through its node's proxy (proxy_backend.c).
 *
 * The worker is Cloudberry's server, compiled where it lies (meson.build),
 * which reads the node it runs on, the packet size and the addresses through
 * the include overlay (compat/ic_proxy/); here they are set, and the worker
 * registered to start on every node of a cluster once it is out of recovery,
 * and again when it exits -- whatever a session's gp.interconnect_type, as a
 * session may choose the proxy for any statement.  Built where libuv is: the
 * module only loads without it.
 *
 * Cloudberry sources this module is made of:
 *	  contrib/interconnect/ic_modules.c (the proxy's entries),
 *	  contrib/interconnect/proxy/, src/backend/cdb/motion/ic_proxy_bgworker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include "cb_module.h"
#include "gp_cluster.h"
#include "gp_core_api.h"

#ifdef USE_IC_PROXY
#include "ic_proxy_server.h"
#include "proxy_backend.h"
#endif

PG_MODULE_MAGIC_EXT(
					.name = "interconnect",
					.version = GP_VERSION
);

void		_PG_init(void);

/* Every node's proxy, Cloudberry's gp_interconnect_proxy_addresses */
char	   *gp_interconnect_proxy_addresses = NULL;

#ifdef USE_IC_PROXY

/* What the proxy's server reads as Cloudberry's globals (compat/ic_proxy/) */
GpId		gp_ic_proxy_identity = {0, -2};
int			gp_ic_proxy_packet_size = 0;

PGDLLEXPORT void GpIcProxyMain(Datum main_arg);

/*
 * The proxy: Cloudberry's server, on the node this is, its packets as large
 * as the node's -- the size a backend of the node sends them in too
 * (proxy_backend.c).
 */
void
GpIcProxyMain(Datum main_arg)
{
	gp_ic_proxy_identity.dbid = GpClusterDbid();
	gp_ic_proxy_identity.segindex = GpClusterContentId();
	gp_ic_proxy_packet_size = GpIcProxyPacketSize();

	/*
	 * The server takes SIGHUP, SIGINT, SIGTERM and SIGQUIT through libuv,
	 * and unblocks the signals itself; it answers non-zero, to be started
	 * again, unless it was told to quit.
	 */
	proc_exit(ic_proxy_server_main());
}
#endif							/* USE_IC_PROXY */

void
_PG_init(void)
{
	CB_REQUIRE_PRELOAD("interconnect");
	CB_REQUIRE_CORE("interconnect");

	DefineCustomStringVariable("gp.interconnect_proxy_addresses",
							   "Sets the ic-proxy addresses as \"dbid:content:host:port\", a comma between two, for every node of the cluster.",
							   "Where each node's proxy listens for the others', mirrors' and the standby's too: the same on every node.  Cloudberry calls this gp_interconnect_proxy_addresses.",
							   &gp_interconnect_proxy_addresses,
							   "", PGC_SIGHUP, GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);

#ifdef USE_IC_PROXY
	if (!GpClusterIsSingleNode())
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 1;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "interconnect");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "GpIcProxyMain");
		snprintf(worker.bgw_name, BGW_MAXLEN, "ic proxy process");
		snprintf(worker.bgw_type, BGW_MAXLEN, "ic proxy process");
		RegisterBackgroundWorker(&worker);
	}

	GpIcProxyInit();
#endif
}
