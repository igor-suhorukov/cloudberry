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
 * fts_etcd.h
 *	  gpfts's keys in etcd, and the lease its leader holds.  See fts_etcd.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef FTS_ETCD_H
#define FTS_ETCD_H

#include "utils/etcd.h"

/* The lease of the leader's lock, in seconds, unless -u says otherwise. */
#define FTS_HA_LOCK_LEASE_TIMEOUT_DEFAULT 30

/* What keepFTSLeaseFromETCD() found. */
typedef enum FtsLeaseState
{
	FTS_LEASE_KEPT,				/* renewed */
	FTS_LEASE_LOST,				/* etcd has let it lapse */
	FTS_LEASE_UNKNOWN			/* no endpoint answered */
} FtsLeaseState;

extern bool initETCD(const char *etcd_endpoints, const char *etcd_namespace,
					 const char *etcd_account_id, const char *etcd_cluster_id);
extern int	readFTSDumpFromETCD(char **value);
extern int	writeFTSDumpFromETCD(const char *value);
extern int	delFTSInfoFromETCD(void);
extern int	readStandbyPromoteReadyFromETCD(char **value);
extern int	writeStandbyPromoteReadyToETCD(bool ready);
extern bool getFTSLockFromETCD(const char *key, char **lock, long long *lease,
							   int lease_timeout, const char *hostname,
							   const char *hostnamekey);
extern FtsLeaseState keepFTSLeaseFromETCD(long long lease);
extern void releaseFTSLockFromETCD(const char *lock);

#endif							/* FTS_ETCD_H */
