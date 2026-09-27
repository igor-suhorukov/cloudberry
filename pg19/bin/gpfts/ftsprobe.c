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
 * ftsprobe.c
 *	  gpfts's round: the coordinator probed, and its standby promoted when
 *	  the coordinator has stopped.
 *
 * Cloudberry's gpfts probes every primary, and the coordinator, over a
 * connection type of its own (gpconntype=fts) with a message its postmaster
 * answers even in recovery; the coordinator and its standby are one more
 * pair to it, content -1, and on the coordinator's failure the roles are
 * flipped in etcd and the standby sent PROMOTE (src/bin/gpfts/ftsprobe.c).
 * The port's probes the coordinator alone -- the segments are gp_core's
 * FTS's -- in the terms of that FTS (gp_fts.c): a probe is an ordinary libpq
 * connection to the database postgres and the value of gp.fts_status, the
 * node's answer, whose "mirror" on a coordinator is its standby; the
 * promotion is pg_promote(), which the standby, a hot standby, takes, and
 * then gp_activate_standby(), which makes it the coordinator of the cluster
 * file -- what Cloudberry's startup process does as it promotes a standby,
 * and gpactivatestandby asks for -- and a wait for its distributed
 * transaction recovery to reach every segment, gp.dtx_recovered().
 *
 * What etcd holds (fts_etcd.c) is the cluster as the coordinator last showed
 * it, gp_segment_configuration's rows in the form Cloudberry's gpfts keeps
 * them -- loaded with -W 1 at first, and brought up to date by each probe of
 * the coordinator, so that a standby added or removed is seen -- and whether
 * the standby may be promoted: "1" while the coordinator's answer says its
 * standby streams, which Cloudberry's coordinator writes itself at its
 * commits (syncrep.c).  A standby that did not stream at the last probe may
 * lack commits the coordinator acknowledged without it: it is not promoted,
 * a "double fault", as Cloudberry's handler refuses a promotion.
 *
 * A failover is steps each of which a later round takes up, so that a
 * leader that stops midway is finished by the next: the roles flipped in
 * etcd first, as Cloudberry's are; a node that etcd names the coordinator and
 * that is still in recovery is promoted again ("resending promote request");
 * and one out of recovery whose etcd row still prefers the mirror's role is
 * activated -- gp_activate_standby() changes nothing on a coordinator
 * already -- and its recovery waited for.
 *
 * Which segment is whose primary now is not gpfts's to give the promoted
 * standby: the coordinator's FTS logs the nodes' states to WAL as it
 * publishes them, and waits for its standby to have them (gp_cluster.c).
 *
 * Cloudberry sources this file stands in for:
 *	  src/bin/gpfts/ftsprobe.c, its content -1 pair, and the promotion of
 *	  src/backend/fts/ftsmessagehandler.c (HandleFtsWalRepPromote())
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <poll.h>

#include "fe_utils/log.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "postmaster/fts_comm.h"

#include "fts.h"
#include "fts_etcd.h"

/* What gpfts's connections are called, in the nodes' logs. */
#define FTS_APPNAME				"cloudberry gpfts"

/*
 * How long a promotion may take, as gpactivatestandby's pg_ctl promote waits
 * (Cloudberry's MIRROR_PROMOTION_TIMEOUT), and the new coordinator's
 * distributed transaction recovery, as gpstart waits for it.
 */
#define FTS_PROMOTE_TIMEOUT		600
#define FTS_RECOVERY_TIMEOUT	300

/* A node, a line of etcd's configuration. */
typedef struct FtsNode
{
	int			dbid;
	int			content;
	char		role;
	char		preferred_role;
	char		mode;
	char		status;
	int			port;
	char	   *hostname;
	char	   *address;
	char	   *datadir;
} FtsNode;

/* A node's answer to a probe: gp.fts_status. */
typedef struct FtsAnswer
{
	bool		mirror_up;		/* on a coordinator: its standby */
	bool		in_sync;
	bool		role_mirror;	/* it is in recovery */
	bool		restarting;		/* it refused, starting up or recovering */
} FtsAnswer;

/* When the coordinator was first found restarting, 0 while it is not. */
static int64 restarting_since = 0;
static int	restarting_dbid = 0;

/* ------------------------------------------------------------------------- */
/* The configuration                                                         */
/* ------------------------------------------------------------------------- */

static void
fts_free_nodes(FtsNode *nodes, int n)
{
	for (int i = 0; i < n; i++)
	{
		pfree(nodes[i].hostname);
		pfree(nodes[i].address);
		pfree(nodes[i].datadir);
	}
	if (nodes != NULL)
		pfree(nodes);
}

/*
 * etcd's configuration: "dbid content role preferred_role mode status port
 * hostname address datadir", a line each, as Cloudberry's gpfts reads it
 * (readGpSegConfigFromETCD()).  -1 where a line is not one.
 */
static int
fts_parse_nodes(char *text, FtsNode **nodes_out)
{
	int			max = 16;
	int			n = 0;
	FtsNode    *nodes = palloc_array(FtsNode, max);

	for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n"))
	{
		FtsNode    *node;
		char		hostname[1024];
		char		address[1024];
		char		datadir[MAXPGPATH];

		if (line[strspn(line, " \t\r")] == '\0')
			continue;
		if (n == max)
		{
			max *= 2;
			nodes = repalloc_array(nodes, FtsNode, max);
		}
		node = &nodes[n];
		if (sscanf(line, "%d %d %c %c %c %c %d %1023s %1023s %1023s", &node->dbid,
				   &node->content, &node->role, &node->preferred_role, &node->mode,
				   &node->status, &node->port, hostname, address, datadir) != 10)
		{
			cbdb_log_error("invalid data in gp_segment_configuration from ETCD: \"%s\"", line);
			fts_free_nodes(nodes, n);
			return -1;
		}
		node->hostname = pg_strdup(hostname);
		node->address = pg_strdup(address);
		node->datadir = pg_strdup(datadir);
		n++;
	}
	*nodes_out = nodes;
	return n;
}

static char *
fts_format_nodes(const FtsNode *nodes, int n)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 0; i < n; i++)
		appendStringInfo(&buf, "%d %d %c %c %c %c %d %s %s %s\n", nodes[i].dbid,
						 nodes[i].content, nodes[i].role, nodes[i].preferred_role,
						 nodes[i].mode, nodes[i].status, nodes[i].port,
						 nodes[i].hostname, nodes[i].address, nodes[i].datadir);
	return buf.data;
}

/* ------------------------------------------------------------------------- */
/* Connections, bounded in time                                              */
/* ------------------------------------------------------------------------- */

/*
 * Wait until the socket is ready, or the deadline passes -- keeping the
 * leader's lease alive meanwhile.  False at the deadline.
 */
static bool
fts_wait_socket(int sock, bool forread, int64 deadline)
{
	for (;;)
	{
		struct pollfd pfd;
		int64		left;

		FtsTick();
		left = deadline - FtsNow();
		if (left <= 0)
			return false;
		pfd.fd = sock;
		pfd.events = forread ? POLLIN : POLLOUT;
		pfd.revents = 0;
		if (poll(&pfd, 1, (int) Min(left, 200)) != 0)
			return true;		/* ready, or an error libpq is to see */
	}
}

/* A message of libpq's, on one line of the log. */
static char *
fts_one_line(char *message)
{
	int			len;

	for (char *p = message; *p != '\0'; p++)
		if (*p == '\n' || *p == '\t')
			*p = ' ';
	len = strlen(message);
	while (len > 0 && message[len - 1] == ' ')
		message[--len] = '\0';
	return message;
}

/* A connection to the node's database postgres, made by the deadline. */
static PGconn *
fts_connect(const FtsNode *node, const fts_config *config, int64 deadline,
			char **error)
{
	const char *keywords[6];
	const char *values[6];
	char		port[16];
	int			n = 0;
	PGconn	   *conn;
	PostgresPollingStatusType status = PGRES_POLLING_WRITING;

	snprintf(port, sizeof(port), "%d", node->port);
	keywords[n] = "host";
	values[n++] = node->address;
	keywords[n] = "port";
	values[n++] = port;
	keywords[n] = "dbname";
	values[n++] = "postgres";
	keywords[n] = "application_name";
	values[n++] = FTS_APPNAME;
	if (config->user != NULL)
	{
		keywords[n] = "user";
		values[n++] = config->user;
	}
	keywords[n] = NULL;
	values[n] = NULL;

	conn = PQconnectStartParams(keywords, values, 0);
	if (conn == NULL)
	{
		*error = pg_strdup("out of memory");
		return NULL;
	}
	while (PQstatus(conn) != CONNECTION_BAD && status != PGRES_POLLING_OK)
	{
		if (status == PGRES_POLLING_FAILED)
			break;
		if (!fts_wait_socket(PQsocket(conn), status == PGRES_POLLING_READING, deadline))
		{
			*error = psprintf("timeout expired after %d seconds", config->probe_timeout);
			PQfinish(conn);
			return NULL;
		}
		status = PQconnectPoll(conn);
	}
	if (PQstatus(conn) != CONNECTION_OK)
	{
		*error = fts_one_line(pg_strdup(PQerrorMessage(conn)));
		PQfinish(conn);
		return NULL;
	}
	return conn;
}

/*
 * The statement's last result, if it succeeded, by the deadline; NULL, with
 * the error, otherwise, and then the connection is to be closed.
 */
static PGresult *
fts_exec(PGconn *conn, const char *sql, int64 deadline, char **error)
{
	PGresult   *last = NULL;

	if (!PQsendQuery(conn, sql))
	{
		*error = fts_one_line(pg_strdup(PQerrorMessage(conn)));
		return NULL;
	}
	for (;;)
	{
		PGresult   *res;

		if (!PQconsumeInput(conn))
		{
			*error = fts_one_line(pg_strdup(PQerrorMessage(conn)));
			PQclear(last);
			return NULL;
		}
		if (PQisBusy(conn))
		{
			if (!fts_wait_socket(PQsocket(conn), true, deadline))
			{
				*error = pg_strdup("timeout expired");
				PQclear(last);
				return NULL;
			}
			continue;
		}
		res = PQgetResult(conn);
		if (res == NULL)
			break;
		PQclear(last);
		last = res;
	}
	if (last != NULL && (PQresultStatus(last) == PGRES_TUPLES_OK ||
						 PQresultStatus(last) == PGRES_COMMAND_OK))
		return last;
	*error = fts_one_line(pg_strdup(last != NULL ? PQresultErrorMessage(last) : PQerrorMessage(conn)));
	PQclear(last);
	return NULL;
}

/* A statement of one value, 't' or 'f', on a connection that has answered. */
static bool
fts_exec_bool(PGconn *conn, const char *sql, int timeout, bool *value, char **error)
{
	PGresult   *res = fts_exec(conn, sql, FtsNow() + (int64) timeout * 1000, error);

	if (res == NULL)
		return false;
	if (PQntuples(res) != 1 || PQnfields(res) != 1 || PQgetisnull(res, 0, 0))
	{
		*error = psprintf("\"%s\" did not answer with one value", sql);
		PQclear(res);
		return false;
	}
	*value = PQgetvalue(res, 0, 0)[0] == 't';
	PQclear(res);
	return true;
}

/* ------------------------------------------------------------------------- */
/* The probe                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Probe the node: its gp.fts_status -- "dbid content mirror_up in_sync
 * syncrep_on role_mirror retry ready" (gp_fts.c) -- which must be the
 * node's own, attempt after attempt a second apart, -R times after the
 * first, each within -T seconds, as Cloudberry's prober retries.  The open
 * connection, or NULL.
 */
static PGconn *
fts_probe(const FtsNode *node, const fts_config *config, FtsAnswer *answer)
{
	memset(answer, 0, sizeof(*answer));
	for (int retries = 0;; retries++)
	{
		int64		deadline = FtsNow() + (int64) config->probe_timeout * 1000;
		char	   *error = NULL;
		PGconn	   *conn = fts_connect(node, config, deadline, &error);

		if (conn != NULL)
		{
			PGresult   *res = fts_exec(conn, "SELECT pg_catalog.current_setting('gp.fts_status')",
									   deadline, &error);

			if (res != NULL)
			{
				int			dbid;
				int			content;
				char		f[6];

				if (PQntuples(res) == 1 &&
					sscanf(PQgetvalue(res, 0, 0), "%d %d %c %c %c %c %c %c", &dbid,
						   &content, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) == 8 &&
					dbid == node->dbid && content == node->content)
				{
					answer->mirror_up = f[0] == 't';
					answer->in_sync = f[1] == 't';
					answer->role_mirror = f[3] == 't';
					cbdb_log_debug("segment (content=%d, dbid=%d, role=%c) reported isMirrorUp %d, isInSync %d, isRoleMirror %d to the prober.",
								   node->content, node->dbid, node->role,
								   answer->mirror_up, answer->in_sync, answer->role_mirror);
					PQclear(res);
					return conn;
				}
				error = psprintf("the node at %s:%d answered \"%s\", not as dbid %d",
								 node->address, node->port,
								 PQntuples(res) == 1 ? PQgetvalue(res, 0, 0) : "",
								 node->dbid);
				PQclear(res);
			}
			PQfinish(conn);
		}

		/*
		 * A node that refuses because it is starting up, or recovering from a
		 * crash, is restarting rather than gone (Cloudberry's
		 * checkIfFailedDueToNormalRestart()).
		 */
		answer->restarting =
			strstr(error, "the database system is starting up") != NULL ||
			strstr(error, "the database system is in recovery mode") != NULL;
		cbdb_log_info("FTS: PROBE to (content=%d, dbid=%d) failed, retry_count=%d: %s",
					  node->content, node->dbid, retries, error);
		if (retries >= config->probe_retries)
		{
			if (config->probe_retries > 0)
				cbdb_log_info("max (%d) retries exhausted (content=%d, dbid=%d)",
							  retries, node->content, node->dbid);
			return NULL;
		}
		FtsWait(1000);
	}
}

/*
 * Has the coordinator been restarting for longer than a round's patience?
 * As the port's FTS gives a primary (gp_fts.c): as long as a node that does
 * not answer at all, -R attempts of -T seconds each, since Cloudberry's tells
 * a restart making progress by the WAL position its refusal carries, which
 * PostgreSQL 19's does not.
 */
static bool
fts_restart_too_long(const FtsNode *node, const fts_config *config)
{
	int64		now = FtsNow();

	if (restarting_since == 0 || restarting_dbid != node->dbid)
	{
		restarting_since = now;
		restarting_dbid = node->dbid;
	}
	return now - restarting_since >
		(int64) Max(config->probe_retries, 1) * config->probe_timeout * 1000;
}

/* ------------------------------------------------------------------------- */
/* What a round does                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The coordinator's view into etcd: gp_segment_configuration as it shows it,
 * where that is not what etcd has, and whether its standby may be promoted --
 * whether it streams, as the coordinator's answer says, the WAL sender of
 * gp_walreceiver being its standby's.
 */
static void
fts_refresh(PGconn *conn, const char *known, bool standby_ready,
			const fts_config *config)
{
	char	   *error = NULL;
	char	   *ready = NULL;
	const char *want = standby_ready ? FTS_STANDBY_PROMOTE_READY : FTS_STANDBY_PROMOTE_NO_READY;
	PGresult   *res;

	res = fts_exec(conn,
				   "SELECT dbid, content, role, preferred_role, mode, status, port,"
				   " hostname, address, datadir"
				   " FROM pg_catalog.gp_segment_configuration ORDER BY dbid",
				   FtsNow() + (int64) config->probe_timeout * 1000, &error);
	if (res == NULL)
		cbdb_log_warning("FTS could not read the coordinator's gp_segment_configuration: %s", error);
	else
	{
		StringInfoData text;

		initStringInfo(&text);
		for (int i = 0; i < PQntuples(res); i++)
		{
			for (int j = 0; j < PQnfields(res); j++)
				appendStringInfo(&text, "%s%s", j > 0 ? " " : "", PQgetvalue(res, i, j));
			appendStringInfoChar(&text, '\n');
		}
		PQclear(res);
		if (strcmp(text.data, known) != 0)
		{
			if (writeFTSDumpFromETCD(text.data) == 0)
				cbdb_log_info("Already updated segment infos.");
			else
				cbdb_log_error("FTS could not write the cluster's configuration to etcd.");
		}
		pfree(text.data);
	}

	if (readStandbyPromoteReadyFromETCD(&ready) != 0 || ready == NULL ||
		strcmp(ready, want) != 0)
	{
		if (writeStandbyPromoteReadyToETCD(standby_ready) == 0)
			cbdb_log_info("FTS: standby promote ready is %s.", want);
		else
			cbdb_log_error("FTS could not write standby promote ready to etcd.");
	}
	if (ready != NULL)
		pfree(ready);
}

/*
 * Promote the standby, as Cloudberry's handler of PROMOTE does -- once:
 * a standby already out of recovery is left as it is -- and wait until it is
 * out of recovery.  Its synchronous_standby_names, the old coordinator's,
 * gp_core drops as its recovery ends (gp_standby.c).
 */
static bool
fts_promote(const FtsNode *standby, const fts_config *config)
{
	FtsAnswer	answer;
	PGconn	   *conn = fts_probe(standby, config, &answer);
	char	   *error = NULL;
	bool		value;

	if (conn == NULL)
	{
		cbdb_log_warning("FTS: the standby (content=%d, dbid=%d) does not answer: not promoted.",
						 standby->content, standby->dbid);
		return false;
	}
	if (answer.role_mirror)
	{
		cbdb_log_info("promoting mirror (content=%d, dbid=%d) to be the new primary",
					  standby->content, standby->dbid);
		if (!fts_exec_bool(conn, "SELECT pg_catalog.pg_promote(false)",
						   config->probe_timeout, &value, &error) || !value)
		{
			cbdb_log_warning("FTS could not promote (content=%d, dbid=%d): %s",
							 standby->content, standby->dbid,
							 error != NULL ? error : "pg_promote() refused");
			PQfinish(conn);
			return false;
		}
		for (int waited = 0;; waited++)
		{
			if (!fts_exec_bool(conn, "SELECT pg_catalog.pg_is_in_recovery()",
							   config->probe_timeout, &value, &error))
			{
				cbdb_log_warning("FTS lost (content=%d, dbid=%d) as it was promoted: %s",
								 standby->content, standby->dbid, error);
				PQfinish(conn);
				return false;
			}
			if (!value)
				break;
			if (waited >= FTS_PROMOTE_TIMEOUT)
			{
				cbdb_log_warning("FTS: (content=%d, dbid=%d) is still in recovery %d seconds after its promotion",
								 standby->content, standby->dbid, FTS_PROMOTE_TIMEOUT);
				PQfinish(conn);
				return false;
			}
			FtsWait(1000);
		}
		cbdb_log_info("mirror (content=%d, dbid=%d) promotion triggered successfully",
					  standby->content, standby->dbid);
	}
	PQfinish(conn);
	return true;
}

/*
 * Make the promoted standby the coordinator of the cluster file, as
 * gpactivatestandby does -- gp_activate_standby() changes nothing on a
 * coordinator already -- and wait for its distributed transaction recovery to
 * reach every segment, as gpstart waits for a coordinator's.
 */
static bool
fts_activate(const FtsNode *node, const fts_config *config)
{
	FtsAnswer	answer;
	PGconn	   *conn = fts_probe(node, config, &answer);
	char	   *error = NULL;
	bool		value;

	if (conn == NULL)
		return false;
	if (!fts_exec_bool(conn, "SELECT pg_catalog.gp_activate_standby()",
					   config->probe_timeout, &value, &error) || !value)
	{
		cbdb_log_warning("FTS could not make (content=%d, dbid=%d) the coordinator: %s",
						 node->content, node->dbid,
						 error != NULL ? error : "gp_activate_standby() refused");
		PQfinish(conn);
		return false;
	}
	cbdb_log_info("FTS made (content=%d, dbid=%d) the coordinator of the cluster.",
				  node->content, node->dbid);

	for (int waited = 0;; waited++)
	{
		if (!fts_exec_bool(conn, "SELECT gp.dtx_recovered()", config->probe_timeout,
						   &value, &error))
		{
			cbdb_log_warning("FTS could not ask whether distributed transaction recovery is done on (content=%d, dbid=%d): %s",
							 node->content, node->dbid, error);
			break;
		}
		if (value)
		{
			cbdb_log_info("FTS: distributed transaction recovery of the new coordinator (dbid=%d) has reached every segment.",
						  node->dbid);
			break;
		}
		if (waited >= FTS_RECOVERY_TIMEOUT)
		{
			cbdb_log_warning("FTS: timed out after %d seconds waiting for distributed transaction recovery on (dbid=%d)",
							 FTS_RECOVERY_TIMEOUT, node->dbid);
			break;
		}
		FtsWait(1000);
	}
	PQfinish(conn);
	return true;
}

/*
 * The coordinator made, probed afresh, and its view taken into etcd: the
 * end of a failover, which a round finishing one does at once rather than a
 * round later.
 */
static void
fts_refresh_node(const FtsNode *node, const char *known, const fts_config *config)
{
	FtsAnswer	answer;
	PGconn	   *conn = fts_probe(node, config, &answer);

	if (conn == NULL)
		return;
	fts_refresh(conn, known, answer.in_sync, config);
	PQfinish(conn);
}

/*
 * A round's work, on etcd's configuration of the cluster, known, parsed into
 * nodes: Cloudberry's FtsWalRepMessageSegments() and processResponse(), for
 * the pair of content -1 alone.
 */
static void
fts_round(const fts_config *config, const char *known, FtsNode *nodes, int n)
{
	FtsNode    *coordinator = NULL;
	FtsNode    *standby = NULL;
	FtsAnswer	answer;
	PGconn	   *conn;
	char	   *ready = NULL;
	bool		ready_to_promote;

	for (int i = 0; i < n; i++)
	{
		if (nodes[i].content != -1)
			continue;
		if (nodes[i].role == 'p')
			coordinator = &nodes[i];
		else
			standby = &nodes[i];
	}
	if (coordinator == NULL)
	{
		cbdb_log_warning("FTS found no coordinator in etcd's configuration: skipped current round.");
		return;
	}
	cbdb_log_debug("starting scan of the coordinator (dbid=%d) and its standby (dbid=%d)",
				   coordinator->dbid, standby != NULL ? standby->dbid : -1);

	conn = fts_probe(coordinator, config, &answer);
	if (conn != NULL)
	{
		restarting_since = 0;
		if (answer.role_mirror)
		{
			/* a promotion that did not take, asked for again */
			PQfinish(conn);
			if (coordinator->preferred_role == 'm')
			{
				cbdb_log_info("resending promote request to (content=%d, dbid=%d)",
							  coordinator->content, coordinator->dbid);
				if (fts_promote(coordinator, config) && fts_activate(coordinator, config))
					fts_refresh_node(coordinator, known, config);
			}
			else
				cbdb_log_warning("FTS: the coordinator (dbid=%d) is in recovery.",
								 coordinator->dbid);
			return;
		}
		if (coordinator->preferred_role == 'm')
		{
			/* promoted, and not yet made the coordinator */
			PQfinish(conn);
			if (fts_activate(coordinator, config))
				fts_refresh_node(coordinator, known, config);
			return;
		}
		fts_refresh(conn, known, answer.in_sync, config);
		PQfinish(conn);
		return;
	}

	/* The coordinator is down. */
	if (answer.restarting && !fts_restart_too_long(coordinator, config))
	{
		cbdb_log_info("FTS: detected coordinator is starting up (content=%d, dbid=%d)",
					  coordinator->content, coordinator->dbid);
		return;
	}
	restarting_since = 0;
	if (standby == NULL)
	{
		cbdb_log_warning("FTS: the coordinator (dbid=%d) does not answer, and has no standby.",
						 coordinator->dbid);
		return;
	}
	if (config->disable_promote_standby)
	{
		cbdb_log_info("disable promote standby, skipped promote (content=%d, dbid=%d).",
					  standby->content, standby->dbid);
		return;
	}
	ready_to_promote = readStandbyPromoteReadyFromETCD(&ready) == 0 && ready != NULL &&
		strcmp(ready, FTS_STANDBY_PROMOTE_READY) == 0;
	if (ready != NULL)
		pfree(ready);
	if (!ready_to_promote)
	{
		cbdb_log_warning("double fault detected (content=%d) primary dbid=%d, mirror dbid=%d: ignoring promote request, standby not ready to promote",
						 coordinator->content, coordinator->dbid, standby->dbid);
		return;
	}
	FtsCheckLease();

	/*
	 * The roles flipped in etcd, and the flag reset, before the standby is
	 * promoted, as Cloudberry's updateConfiguration() and its handler do: the
	 * next round -- this instance's or another's -- probes the standby as the
	 * coordinator, and finishes what this one does not.
	 */
	coordinator->role = 'm';
	coordinator->mode = 'n';
	coordinator->status = 'd';
	standby->role = 'p';
	standby->mode = 'n';
	standby->status = 'u';
	if (writeFTSDumpFromETCD(fts_format_nodes(nodes, n)) != 0 ||
		writeStandbyPromoteReadyToETCD(false) != 0)
	{
		cbdb_log_error("FTS could not write the failover of (content=%d, dbid=%d) to etcd: not promoting its standby.",
					   coordinator->content, coordinator->dbid);
		return;
	}
	cbdb_log_info("FTS: the coordinator (dbid=%d) does not answer; failing over to its standby (dbid=%d).",
				  coordinator->dbid, standby->dbid);
	if (fts_promote(standby, config) && fts_activate(standby, config))
		fts_refresh_node(standby, known, config);
}

/* A round: etcd's configuration read, and what it says to do done. */
void
FtsProbeRound(const fts_config *config)
{
	char	   *text = NULL;
	char	   *known;
	FtsNode    *nodes = NULL;
	int			n;

	if (readFTSDumpFromETCD(&text) != 0 || text == NULL)
	{
		cbdb_log_warning("FTS found no configuration of the cluster in etcd: skipped current round.");
		return;
	}
	known = pg_strdup(text);
	n = fts_parse_nodes(text, &nodes);
	if (n >= 0)
		fts_round(config, known, nodes, n);
	fts_free_nodes(nodes, n);
	pfree(known);
	pfree(text);
}

/* -W 4: the coordinator etcd names, probed once, and its answer printed. */
bool
FtsProbeOnce(const fts_config *config)
{
	char	   *text = NULL;
	FtsNode    *nodes = NULL;
	FtsAnswer	answer;
	PGconn	   *conn;
	int			n;

	if (readFTSDumpFromETCD(&text) != 0 || text == NULL)
	{
		printf("Fail to read fts info from ETCD.\n");
		return false;
	}
	n = fts_parse_nodes(text, &nodes);
	for (int i = 0; i < n; i++)
	{
		fts_config	once = *config;

		if (nodes[i].content != -1 || nodes[i].role != 'p')
			continue;
		once.probe_retries = 0;
		conn = fts_probe(&nodes[i], &once, &answer);
		if (conn == NULL)
		{
			printf("The coordinator (dbid=%d) at %s:%d does not answer.\n",
				   nodes[i].dbid, nodes[i].address, nodes[i].port);
			return false;
		}
		printf("The coordinator (dbid=%d) at %s:%d answers: standby up %s, in sync %s, in recovery %s.\n",
			   nodes[i].dbid, nodes[i].address, nodes[i].port,
			   answer.mirror_up ? "t" : "f", answer.in_sync ? "t" : "f",
			   answer.role_mirror ? "t" : "f");
		PQfinish(conn);
		return true;
	}
	printf("No coordinator in ETCD's configuration.\n");
	return false;
}
