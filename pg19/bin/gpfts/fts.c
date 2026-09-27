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
 * fts.c
 *	  gpfts, the coordinator's automatic failover: the program.
 *
 * Cloudberry's gpfts is its external fault tolerance service: a program that
 * runs outside the server, on hosts of its own, in several instances that
 * elect a leader through an etcd lease; the leader probes every node and
 * fails over from a primary or a coordinator that stops.  The port's FTS is
 * internal, a process of gp_core's on the coordinator (gp_fts.c), which keeps
 * the segments; what it cannot do is fail over from the coordinator it runs
 * on.  So the port's gpfts watches the coordinator alone, and promotes its
 * standby when the coordinator stops -- the case Cloudberry's handles as one
 * more pair, of content -1 (ftsprobe.c).
 *
 * This file is the program: Cloudberry's options, its log, its etcd
 * configuration file (bin/config/cbdb_etcd_default.conf by default), its
 * "warp calls" that load, show and remove what etcd holds, and its loop.
 * Each instance takes a lease and asks etcd's lock service for the cluster's
 * lock with it, as Cloudberry's does (fts_etcd.c); the one that has the lock
 * runs a round every -I seconds and keeps its lease alive, and the others
 * wait for the lock.  Cloudberry's renews the lease from a thread of its own,
 * over the libcurl handle the rest uses, which it guards with a mutex a
 * whole round holds -- so a round longer than the lease let the lease lapse
 * under it, and another instance take over while it still acted.  This one
 * is a single thread, whose every wait -- between rounds, and for a node's
 * answer -- keeps the lease (FtsTick()); an instance that finds its lease
 * gone exits, as Cloudberry's does when it cannot renew it, and one stopped
 * with SIGTERM or SIGINT gives the lock back, so that another takes over at
 * once.  An instance that is not the leader asks for the lock again a second
 * after its request ends -- the request itself waits up to ten seconds for
 * the lock -- where Cloudberry's waits ten more.
 *
 * Cloudberry sources this file stands in for:
 *	  src/bin/gpfts/fts.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "common/etcdutils.h"
#include "common/string.h"
#include "fe_utils/log.h"
#include "getopt_long.h"
#include "lib/stringinfo.h"

#include "fts.h"
#include "fts_etcd.h"

/* Cloudberry's: where the etcd configuration is, and the lock's keys. */
#define FTS_ETCD_CONF_FILE_NAME		"bin/config/cbdb_etcd_default.conf"
#define FTS_GPHOME_PATH				"GPHOME"
#define FTS_CBDB_PATH				"COORDINATOR_DATA_DIRECTORY"
#define FTS_ETCD_HA_LOCK_KEY		"fts_ha_lock"
#define FTS_ETCD_HA_HOSTNAME_KEY	"fts_ha_hostname"
#define FTS_HOSTNAME_LEN			128
#define FTS_ETCD_PATH_KEY_LEN		256
#define FTS_LOG_FILE_DIRECTORY_DEFAULT "log/fts"
#define FTS_LOG_FILE_DIRECTORY_TMP	"/tmp/fts"
#define FTS_LOG_FILE_NAME_DEFAULT	"fts"

static const char *progname;

/* The etcd configuration file's values, Cloudberry's defaults unless it says. */
static char *etcd_account_id = GP_ETCD_ACCOUNT_ID_DEFAULT;
static char *etcd_cluster_id = GP_ETCD_CLUSTER_ID_DEFAULT;
static char *etcd_namespace = GP_ETCD_NAMESPACE_DEFAULT;
static char *etcd_endpoints = GP_ETCD_ENDPOINTS_DEFAULT;
static char fts_lock_path_key[FTS_ETCD_PATH_KEY_LEN];
static char fts_hostname_path_key[FTS_ETCD_PATH_KEY_LEN];

/* -u: the lease's time to live, in seconds. */
static int	fts_ha_lock_lease_timeout = FTS_HA_LOCK_LEASE_TIMEOUT_DEFAULT;

/*
 * The leader's lock, its lease, and when the lease was last renewed and is
 * to be next; NULL while this instance is not the leader.
 */
static char *fts_lock = NULL;
static long long fts_lease = 0;
static int64 fts_lease_kept = 0;
static int64 fts_lease_next = 0;

/* SIGTERM or SIGINT has come. */
static volatile sig_atomic_t shutdown_requested = false;

static int	warp_call_i = -1;
static char *warp_local_fts_file = NULL;

static void
usage(void)
{
	printf(_("%s - Fault Tolerance Server\n\n"), progname);
	printf(_("gpfts watches the coordinator of a cluster, and promotes its standby when the\n"
			 "coordinator stops.  Several may run, on hosts of their own: they elect a\n"
			 "leader through an etcd lease, and the leader probes.  The segments are the\n"
			 "cluster's own FTS's, which runs on the coordinator.\n\n"));
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -U, --user <user>                 Use the specified user to connect to the coordinator\n"
			 "                                    and its standby, default is current login user.\n"));
	printf(_("  -A, --one-round                   Skip the FTS loop probe, only a single probe check is performed,\n"
			 "                                    without the lock, default is \"false\"\n"));
	printf(_("  -D, --disable-promote-standby     Not allow promote standby,\n"
			 "                                    default is \"false\"\n"));
	printf(_("  -R, --probe-retries <retries>     Probe number of retries,\n"
			 "                                    default is \"5\"\n"));
	printf(_("  -T, --probe-timeout <timeout>     Probe timeout(second),\n"
			 "                                    default is \"20\"\n"));
	printf(_("  -I, --probe-interval <interval>   FTS polling interval(second),\n"
			 "                                    default is \"60\"\n"));
	printf(_("  -v, --debug                       Enable debug log(lowest log level)\n"));
	printf(_("  -W, --warp-call <number>          FTS tools, The log level will be set debug level.\n"));
	printf(_("      --warp-call 1 -L <file>       Dump fts file into ETCD. -L specifies the FTS file\n"
			 "                                    which will be written to ETCD: gp_segment_configuration's\n"
			 "                                    rows, \"dbid content role preferred_role mode status port\n"
			 "                                    hostname address datadir\", a line each.\n"));
	printf(_("      --warp-call 2                 Dump FTS info from ETCD.\n"));
	printf(_("      --warp-call 3                 Delete FTS info from ETCD.\n"));
	printf(_("      --warp-call 4                 Probe the coordinator ETCD names once, and print its answer.\n"
			 "                                    Used to verify whether the FTS node can connect to it.\n"));
	printf(_("      --warp-call 5                 Dump standby_promote_ready info from ETCD.\n"
			 "                                    standby_promote_ready is a ETCD key used to confirm\n"
			 "                                    whether the standby node can be promoted safely.\n"));
	printf(_("  -h, -?, --help                    Show this help, then exit\n"));
	printf(_("  -F --etcd_conf_file               ETCD related configuration file path.\n"));
	printf(_("  -u --etcd_lease_timeout           ETCD ha lock lease timeout configuration.\n"));
	printf(_("  -d --log_directory                FTS log directory path.\n"));
	printf(_("  -n --log_rotate_line              FTS log maximum line for archieve rotating, default is \"5000\"\n"));
	printf(_("  -a, -C                            Accepted, as Cloudberry's tools pass them, and change nothing:\n"
			 "                                    they tune a thread that renews the lease, and a wait for\n"
			 "                                    DNS, which this gpfts has not.\n"));
}

static struct option long_options[] = {
	{"help", no_argument, NULL, '?'},
	{"warp-call", required_argument, NULL, 'W'},
	{"local-fts-file", required_argument, NULL, 'L'},
	{"user", required_argument, NULL, 'U'},
	{"one-round", no_argument, NULL, 'A'},
	{"disable-promote-standby", no_argument, NULL, 'D'},
	{"probe-retries", required_argument, NULL, 'R'},
	{"probe-timeout", required_argument, NULL, 'T'},
	{"probe-interval", required_argument, NULL, 'I'},
	{"debug", no_argument, NULL, 'v'},
	{"etcd_conf_file", required_argument, NULL, 'F'},
	{"etcd_lease_timeout", required_argument, NULL, 'u'},
	{"log_directory", required_argument, NULL, 'd'},
	{"log_rotate_line", required_argument, NULL, 'n'},
	{"fts_standalone_enabled", no_argument, NULL, 'a'},
	{"fts_k8s_compatibility_enabled", no_argument, NULL, 'C'},
	{NULL, 0, NULL, 0}
};

/* ------------------------------------------------------------------------- */
/* The clock, and the lease                                                  */
/* ------------------------------------------------------------------------- */

int64
FtsNow(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
handle_shutdown(SIGNAL_ARGS)
{
	shutdown_requested = true;
}

/*
 * What every wait does, a few times a second: stop, when asked to, giving
 * the lock back; and renew the leader's lease a third of its time to live
 * after it was last renewed.  A lease that etcd has let lapse, or that no
 * endpoint has renewed for as long as it lives, is gone, and the lock with
 * it: another instance may lead already, so this one exits.
 */
void
FtsTick(void)
{
	int64		now;
	FtsLeaseState state;

	if (shutdown_requested)
	{
		if (fts_lock != NULL)
			releaseFTSLockFromETCD(fts_lock);
		cbdb_log_info("FTS stopped.");
		exit(0);
	}
	if (fts_lock == NULL)
		return;

	now = FtsNow();
	if (now < fts_lease_next)
		return;
	state = keepFTSLeaseFromETCD(fts_lease);
	if (state == FTS_LEASE_KEPT)
	{
		fts_lease_kept = now;
		fts_lease_next = now + Max(fts_ha_lock_lease_timeout * 1000 / 3, 1000);
		cbdb_log_debug("FTSRenewLease successfully to renew lease for FTS lock.");
		return;
	}
	if (state == FTS_LEASE_LOST ||
		now - fts_lease_kept >= (int64) fts_ha_lock_lease_timeout * 1000)
	{
		cbdb_log_fatal("FTSRenewLease failed to renew lease for FTS lock (lease %lld) and force to exit fts process!",
					   fts_lease);
		exit(1);
	}
	fts_lease_next = now + 1000;
}

void
FtsWait(int64 ms)
{
	int64		until = FtsNow() + ms;

	for (;;)
	{
		int64		left;

		FtsTick();
		left = until - FtsNow();
		if (left <= 0)
			break;
		pg_usleep(Min(left, 200) * 1000L);
	}
}

/*
 * The lease renewed now, before a failover: an instance that has lost it,
 * and so the lock, exits rather than act.  A round with -A takes no lock.
 */
void
FtsCheckLease(void)
{
	fts_lease_next = 0;
	FtsTick();
}

/* ------------------------------------------------------------------------- */
/* The etcd configuration file                                               */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's readGpEtcdConfigFromCbdbFile(): lines of key='value' --
 * gp_etcd_endpoints, a comma-separated list of host:port, and
 * gp_etcd_account_id, gp_etcd_cluster_id and gp_etcd_namespace, which name
 * the cluster's keys -- in the file -F names, or $GPHOME's.
 */
static bool
readGpEtcdConfigFromCbdbFile(const char *conf_file)
{
	char		path[MAXPGPATH];
	char		line[1024];
	FILE	   *fp;
	int			lineno = 0;

	if (conf_file == NULL)
	{
		const char *gphome = getenv(FTS_GPHOME_PATH);

		if (gphome == NULL || gphome[0] == '\0')
		{
			cbdb_log_fatal("Error: failed to get cbdb conf path from environment configuration!");
			fprintf(stderr, _("%s: GPHOME is not set, and no etcd configuration file is given (-F)\n"),
					progname);
			return false;
		}
		snprintf(path, sizeof(path), "%s/%s", gphome, FTS_ETCD_CONF_FILE_NAME);
		conf_file = path;
	}

	fp = fopen(conf_file, "r");
	if (fp == NULL)
	{
		cbdb_log_fatal("FTS readGpEtcdConfigFromCbdbFile Could not open cbdb configuration file:%s.", conf_file);
		fprintf(stderr, _("%s: could not open file \"%s\": %m\n"), progname, conf_file);
		return false;
	}
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		char	   *eq;
		char	   *key;
		char	   *value;
		size_t		len = 0;

		lineno++;
		(void) pg_strip_crlf(line);
		key = line + strspn(line, " \t");
		if (key[0] == '\0' || key[0] == '#')
			continue;
		eq = strchr(key, '=');
		value = eq != NULL ? eq + 1 : NULL;
		if (value != NULL)
		{
			*eq = '\0';
			for (char *end = eq; end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
				end[-1] = '\0';
			value += strspn(value, " \t");
			len = strlen(value);
			while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t'))
				value[--len] = '\0';
		}
		if (value == NULL || len < 2 || value[0] != '\'' || value[len - 1] != '\'')
		{
			cbdb_log_fatal("FTS readGpEtcdConfigFromCbdbFile Invalid data in config file: %s.", conf_file);
			fprintf(stderr, _("%s: invalid line %d in file \"%s\": a line is key='value'\n"),
					progname, lineno, conf_file);
			fclose(fp);
			return false;
		}
		value[len - 1] = '\0';
		value++;

		if (strcmp(key, "gp_etcd_account_id") == 0)
			etcd_account_id = pg_strdup(value);
		else if (strcmp(key, "gp_etcd_cluster_id") == 0)
			etcd_cluster_id = pg_strdup(value);
		else if (strcmp(key, "gp_etcd_namespace") == 0)
			etcd_namespace = pg_strdup(value);
		else if (strcmp(key, "gp_etcd_endpoints") == 0)
			etcd_endpoints = pg_strdup(value);
	}
	fclose(fp);

	snprintf(fts_lock_path_key, sizeof(fts_lock_path_key), "%s/%s/%s/%s/%s",
			 FTS_METADATA_DIR_PREFIX, etcd_namespace, etcd_account_id,
			 etcd_cluster_id, FTS_ETCD_HA_LOCK_KEY);
	snprintf(fts_hostname_path_key, sizeof(fts_hostname_path_key), "%s/%s/%s/%s/%s",
			 FTS_METADATA_DIR_PREFIX, etcd_namespace, etcd_account_id,
			 etcd_cluster_id, FTS_ETCD_HA_HOSTNAME_KEY);
	cbdb_log_info("FTS readGpEtcdConfigFromCbdbFile read configuration from file: %s, etcd_account_id: %s, etcd_cluster_id: %s, etcd_namespace: %s, etcd_endpoints: %s FTS HA lock path: %s, FTS Master node key: %s.",
				  conf_file, etcd_account_id, etcd_cluster_id, etcd_namespace,
				  etcd_endpoints, fts_lock_path_key, fts_hostname_path_key);
	return true;
}

/* ------------------------------------------------------------------------- */
/* The loop                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's FtsLoop(): the lock, and then a round every -I seconds for as
 * long as the lease holds.
 */
static void
FtsLoop(const fts_config *config)
{
	char		hostname[FTS_HOSTNAME_LEN];

	if (config->one_round)
	{
		FtsProbeRound(config);
		return;
	}

	if (gethostname(hostname, sizeof(hostname)) != 0)
		strlcpy(hostname, "localhost", sizeof(hostname));

	for (;;)
	{
		int64		start;

		if (fts_lock == NULL)
		{
			char	   *lock = NULL;
			long long	lease = 0;

			if (!getFTSLockFromETCD(fts_lock_path_key, &lock, &lease,
									fts_ha_lock_lease_timeout, hostname,
									fts_hostname_path_key))
			{
				FtsWait(1000);
				continue;
			}
			fts_lock = lock;
			fts_lease = lease;
			fts_lease_kept = FtsNow();
			fts_lease_next = fts_lease_kept + Max(fts_ha_lock_lease_timeout * 1000 / 3, 1000);
		}

		start = FtsNow();
		FtsProbeRound(config);
		FtsWait((int64) config->probe_interval * 1000 - (FtsNow() - start));
	}
}

/* ------------------------------------------------------------------------- */
/* The warp calls                                                            */
/* ------------------------------------------------------------------------- */

/* Cloudberry's warp_call(): what etcd holds, loaded, shown and removed. */
static int
warp_call(const fts_config *config)
{
	char	   *buf = NULL;
	int			rc;

	cbdb_set_log_level(CBDB_LOG_DEBUG);
	switch (warp_call_i)
	{
		case 1:
			{
				FILE	   *fp;
				StringInfoData text;
				char		line[4096];

				if (warp_local_fts_file == NULL || warp_local_fts_file[0] == '\0')
				{
					printf("Invalid flag -L.\n");
					usage();
					return 1;
				}
				fp = fopen(warp_local_fts_file, "r");
				if (fp == NULL)
				{
					printf("Can't open file: %s\n", warp_local_fts_file);
					return 1;
				}
				initStringInfo(&text);
				while (fgets(line, sizeof(line), fp) != NULL)
					appendStringInfoString(&text, line);
				if (ferror(fp))
				{
					printf("Can't read file: %s\n", warp_local_fts_file);
					fclose(fp);
					return 1;
				}
				fclose(fp);
				rc = writeFTSDumpFromETCD(text.data);
				if (rc != 0)
				{
					printf("Fail to write fts info into ETCD, rc=%d\n", rc);
					return 1;
				}
				printf("Success dump FTS file into ETCD.\n");
				return 0;
			}
		case 2:
			rc = readFTSDumpFromETCD(&buf);
			if (rc != 0 || buf == NULL)
			{
				printf("Fail to read fts info from ETCD, rc=%d\n", rc);
				return 1;
			}
			printf("%s", buf);
			return 0;
		case 3:
			rc = delFTSInfoFromETCD();
			if (rc != 0)
			{
				printf("Fail to delete fts info from ETCD, rc=%d\n", rc);
				return 1;
			}
			if (readFTSDumpFromETCD(&buf) == 0 && buf != NULL)
			{
				printf("Fail to delete fts info from ETCD, still can read it from ETCD.\n");
				return 1;
			}
			printf("Done!\n");
			return 0;
		case 4:
			return FtsProbeOnce(config) ? 0 : 1;
		case 5:
			rc = readStandbyPromoteReadyFromETCD(&buf);
			if (rc != 0 || buf == NULL)
			{
				printf("Fail to read fts info from ETCD, rc=%d\n", rc);
				return 1;
			}
			printf("%s\n", buf);
			return 0;
		default:
			printf("Invalid flag -W, Nothing to do.\n");
			usage();
			return 1;
	}
}

/* An option's number, which is to be at least min. */
static int
number_arg(const char *arg, const char *what, int min)
{
	char	   *end;
	long		n = strtol(arg, &end, 10);

	if (*arg == '\0' || *end != '\0' || n < min || n > INT_MAX)
	{
		fprintf(stderr, _("%s: invalid %s \"%s\": at least %d\n"), progname, what, arg, min);
		exit(1);
	}
	return (int) n;
}

int
main(int argc, char **argv)
{
	fts_config	config = {
		.probe_retries = GP_FTS_PROBE_RETRIES,
		.probe_timeout = GP_FTS_PROBE_TIMEOUT,
		.probe_interval = GP_FTS_PROBE_INTERVAL,
		.user = NULL,
		.disable_promote_standby = false,
		.one_round = false
	};
	char	   *etcd_conf_name = NULL;
	char	   *log_directory = NULL;
	int			c;

	progname = get_progname(argv[0]);
	cbdb_set_log_level(CBDB_LOG_INFO);

	if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0 ||
					 strcmp(argv[1], "-h") == 0))
	{
		usage();
		exit(0);
	}

	while ((c = getopt_long(argc, argv, "F:u:d:n:W:L:U:aACDR:T:I:v",
							long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'F':
				etcd_conf_name = pg_strdup(optarg);
				break;
			case 'u':
				fts_ha_lock_lease_timeout = number_arg(optarg, "lease timeout", 1);
				break;
			case 'd':
				log_directory = pg_strdup(optarg);
				break;
			case 'n':
				cbdb_set_max_log_file_line(number_arg(optarg, "log rotate line", 0));
				break;
			case 'W':
				warp_call_i = number_arg(optarg, "warp call", 1);
				break;
			case 'L':
				warp_local_fts_file = pg_strdup(optarg);
				break;
			case 'U':
				config.user = pg_strdup(optarg);
				break;
			case 'A':
				config.one_round = true;
				break;
			case 'D':
				config.disable_promote_standby = true;
				break;
			case 'R':
				config.probe_retries = number_arg(optarg, "probe retries", 0);
				break;
			case 'T':
				config.probe_timeout = number_arg(optarg, "probe timeout", 1);
				break;
			case 'I':
				config.probe_interval = number_arg(optarg, "probe interval", 1);
				break;
			case 'v':
				cbdb_set_log_level(CBDB_LOG_DEBUG);
				break;
			case 'a':
			case 'C':
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}
	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		exit(1);
	}

	/* Cloudberry's getCbdbLogPath(): the coordinator's directory, or /tmp's */
	if (log_directory == NULL)
	{
		const char *datadir = getenv(FTS_CBDB_PATH);

		log_directory = datadir != NULL && datadir[0] != '\0'
			? psprintf("%s/%s", datadir, FTS_LOG_FILE_DIRECTORY_DEFAULT)
			: pg_strdup(FTS_LOG_FILE_DIRECTORY_TMP);
	}
	if (pg_mkdir_p(log_directory, 0700) != 0 && errno != EEXIST)
	{
		fprintf(stderr, _("%s: could not create directory \"%s\": %m\n"), progname, log_directory);
		exit(1);
	}
	if (!cbdb_set_log_file(log_directory, FTS_LOG_FILE_NAME_DEFAULT))
		exit(1);

	if (!readGpEtcdConfigFromCbdbFile(etcd_conf_name))
		exit(1);
	if (!initETCD(etcd_endpoints, etcd_namespace, etcd_account_id, etcd_cluster_id))
	{
		cbdb_log_fatal("Init ETCD service failed.");
		fprintf(stderr, _("%s: could not use etcd at \"%s\"\n"), progname, etcd_endpoints);
		exit(1);
	}

	if (warp_call_i != -1)
		exit(warp_call(&config));

	pqsignal(SIGTERM, handle_shutdown);
	pqsignal(SIGINT, handle_shutdown);
	pqsignal(SIGPIPE, PG_SIG_IGN);
	cbdb_log_info("FTS started: probe interval %ds, timeout %ds, retries %d, lease %ds%s.",
				  config.probe_interval, config.probe_timeout, config.probe_retries,
				  fts_ha_lock_lease_timeout,
				  config.disable_promote_standby ? ", standby never promoted" : "");
	FtsLoop(&config);
	return 0;
}
