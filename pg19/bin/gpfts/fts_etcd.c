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
 * fts_etcd.c
 *	  gpfts's keys in etcd, and the lease its leader holds.
 *
 * Cloudberry's gpfts keeps, under /cbdb/fts/<namespace>/<account>/<cluster>/,
 * the cluster's configuration (fts_dump_file_key), whether the coordinator's
 * standby may be promoted (fts_standby_promote_ready_key), the lock its
 * instances elect a leader by (fts_ha_lock) and the leader's host name
 * (fts_ha_hostname), all through its etcd client, an HTTP client of etcd's
 * JSON gateway (src/backend/utils/etcd_lib/etcd.c), which the port compiles
 * where it lies.  This file is Cloudberry's, with what the port's gpfts adds
 * to it: the promote-ready flag written, which Cloudberry's coordinator
 * writes itself at its commits (syncrep.c); the lease kept alive with etcd's
 * answer read, so that a lease etcd has let lapse is seen to have gone,
 * where Cloudberry's renewal takes any answer for a renewal; and the lock
 * given back, for another instance to take at once.
 *
 * Cloudberry sources this file stands in for:
 *	  src/bin/gpfts/fts_etcd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <curl/curl.h>
#include <jansson.h>

#include "common/etcdutils.h"
#include "fe_utils/log.h"
#include "lib/stringinfo.h"
#include "postmaster/fts_comm.h"

#include "fts_etcd.h"

/*
 * The prefix of etcd's JSON gateway Cloudberry's client uses (etcd.c), which
 * etcd 3.4 and 3.5 serve; and how long a request to it may take.
 */
#define FTS_ETCD_URL_PREFIX		"v3beta"
#define FTS_ETCD_REQUEST_TIMEOUT 5

static etcdlib_t *etcdlib = NULL;
static char fts_dump_file_key[GP_ETCD_KEY_LEN];
static char fts_standby_promote_ready_key[GP_ETCD_KEY_LEN];
static etcdlib_endpoint_t fts_etcd_endpoints[GP_ETCD_ENDPOINTS_NUM];
static int	fts_etcd_endpoints_num = 0;

/* The leader's own requests, of the lease: a handle kept between them. */
static CURL *lease_curl = NULL;

bool
initETCD(const char *etcd_endpoints, const char *etcd_namespace,
		 const char *etcd_account_id, const char *etcd_cluster_id)
{
	char	   *endpoints = pg_strdup(etcd_endpoints);

	if (!generateGPSegConfigKey(fts_dump_file_key, etcd_namespace, etcd_account_id,
								etcd_cluster_id, endpoints, fts_etcd_endpoints,
								&fts_etcd_endpoints_num))
	{
		cbdb_log_fatal("initETCD could not read the etcd endpoints \"%s\".", etcd_endpoints);
		return false;
	}
	generateGPFtsPromoteReadyKey(fts_standby_promote_ready_key, etcd_namespace,
								 etcd_account_id, etcd_cluster_id);
	etcdlib = etcdlib_create(fts_etcd_endpoints, fts_etcd_endpoints_num, 0);
	if (etcdlib == NULL)
		return false;
	cbdb_log_info("initETCD successfully with fts_dump_file_key:%s.", fts_dump_file_key);
	return true;
}

int
readFTSDumpFromETCD(char **value)
{
	return etcdlib_get(etcdlib, fts_dump_file_key, value, NULL);
}

int
writeFTSDumpFromETCD(const char *value)
{
	return etcdlib_set(etcdlib, fts_dump_file_key, value, 0, false);
}

int
delFTSInfoFromETCD(void)
{
	return etcdlib_del(etcdlib, fts_dump_file_key);
}

int
readStandbyPromoteReadyFromETCD(char **value)
{
	return etcdlib_get(etcdlib, fts_standby_promote_ready_key, value, NULL);
}

/* What Cloudberry's setStandbyPromoteReady() writes (cdbutil.c). */
int
writeStandbyPromoteReadyToETCD(bool ready)
{
	return etcdlib_set(etcdlib, fts_standby_promote_ready_key,
					   ready ? FTS_STANDBY_PROMOTE_READY : FTS_STANDBY_PROMOTE_NO_READY,
					   0, false);
}

/*
 * A lease, and etcd's lock of the key with it: the lock service answers once
 * the lock is this lease's, or fails the request when the client's timeout
 * (etcd.c's, ten seconds) passes first -- which is how an instance that is
 * not the leader waits.  Then the leader's host name, as Cloudberry's does.
 */
bool
getFTSLockFromETCD(const char *key, char **lock, long long *lease, int lease_timeout,
				   const char *hostname, const char *hostnamekey)
{
	int			rc;

	Assert(key != NULL && key[0] != '\0');
	Assert(hostname != NULL && hostname[0] != '\0');
	Assert(lease != NULL);

	rc = etcdlib_grant_lease(etcdlib, lease, lease_timeout);
	if (rc != 0)
	{
		cbdb_log_fatal("FTS getFTSLockFromETCD failed to generate lease for lock rc: %d.", rc);
		return false;
	}

	rc = etcdlib_lock(etcdlib, key, *lease, lock);
	if (rc != 0)
	{
		cbdb_log_warning("FTS getFTSLockFromETCD failed to get FTS lock rc: %d.", rc);
		return false;
	}

	if (*lock == NULL)
	{
		cbdb_log_fatal("FTS getFTSLockFromETCD could not retrieve FTS lock and waiting as standby node.");
		return false;
	}

	rc = etcdlib_set(etcdlib, hostnamekey, hostname, 0, false);
	if (rc != 0)
	{
		cbdb_log_fatal("FTS getFTSLockFromETCD failed to set hostname key for node: %s.", hostname);
		releaseFTSLockFromETCD(*lock);
		return false;
	}
	cbdb_log_info("FTS getFTSLockFromETCD successfully retrieve FTS lock: %s, lease: %lld, %s to be promoted as primary work node.",
				  *lock, *lease, hostname);
	return true;
}

static size_t
lease_reply(void *data, size_t size, size_t nmemb, void *arg)
{
	appendBinaryStringInfo((StringInfo) arg, data, size * nmemb);
	return size * nmemb;
}

/*
 * Keep the lease alive, as Cloudberry's renewFTSLeaseFromETCD() does, from
 * each endpoint in turn until one answers, and read the answer: etcd's
 * LeaseKeepAlive gives a lease it has let lapse no TTL.
 */
FtsLeaseState
keepFTSLeaseFromETCD(long long lease)
{
	FtsLeaseState state = FTS_LEASE_UNKNOWN;
	char	   *request = psprintf("{\"ID\":\"%lld\"}", lease);

	for (int i = 0; i < fts_etcd_endpoints_num && state == FTS_LEASE_UNKNOWN; i++)
	{
		char	   *url = psprintf("http://%s:%d/%s/lease/keepalive",
								   fts_etcd_endpoints[i].etcd_host,
								   fts_etcd_endpoints[i].etcd_port,
								   FTS_ETCD_URL_PREFIX);
		StringInfoData reply;
		CURLcode	res;

		if (lease_curl == NULL)
			lease_curl = curl_easy_init();
		else
			curl_easy_reset(lease_curl);
		if (lease_curl == NULL)
			break;

		initStringInfo(&reply);
		curl_easy_setopt(lease_curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(lease_curl, CURLOPT_TIMEOUT, (long) FTS_ETCD_REQUEST_TIMEOUT);
		curl_easy_setopt(lease_curl, CURLOPT_CONNECTTIMEOUT, (long) FTS_ETCD_REQUEST_TIMEOUT);
		curl_easy_setopt(lease_curl, CURLOPT_URL, url);
		curl_easy_setopt(lease_curl, CURLOPT_POSTFIELDS, request);
		curl_easy_setopt(lease_curl, CURLOPT_WRITEFUNCTION, lease_reply);
		curl_easy_setopt(lease_curl, CURLOPT_WRITEDATA, &reply);
		res = curl_easy_perform(lease_curl);
		if (res != CURLE_OK)
			cbdb_log_warning("FTS could not renew its lease at %s:%d: %s",
							 fts_etcd_endpoints[i].etcd_host,
							 fts_etcd_endpoints[i].etcd_port,
							 curl_easy_strerror(res));
		else
		{
			json_error_t error;
			json_t	   *root = json_loads(reply.data, 0, &error);
			json_t	   *result = root != NULL ? json_object_get(root, "result") : NULL;

			/* an answer with no result is an error of etcd's: another endpoint */
			if (result != NULL)
			{
				json_t	   *ttl = json_object_get(result, "TTL");

				state = ttl != NULL && json_is_string(ttl) &&
					atoll(json_string_value(ttl)) > 0 ? FTS_LEASE_KEPT : FTS_LEASE_LOST;
			}
			else
				cbdb_log_warning("FTS could not renew its lease at %s:%d: %s",
								 fts_etcd_endpoints[i].etcd_host,
								 fts_etcd_endpoints[i].etcd_port, reply.data);
			if (root != NULL)
				json_decref(root);
		}
		pfree(reply.data);
		pfree(url);
	}
	pfree(request);
	return state;
}

/* The lock given back, for an instance that waits for it to take at once. */
void
releaseFTSLockFromETCD(const char *lock)
{
	int			rc = etcdlib_unlock(etcdlib, lock);

	if (rc != 0)
		cbdb_log_warning("FTS could not release its lock: rc %d.", rc);
	else
		cbdb_log_info("FTS released its lock.");
}
