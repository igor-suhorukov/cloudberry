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
 * gp_cluster.c
 *	  Which nodes there are, and which of them this one is.
 *
 * Cloudberry reads gp_segment_configuration, a shared catalog.  The port reads
 * a file, because an extension can create no shared catalog -- and because the
 * answer is needed before any database is open, which a per-database table
 * could not give.  Cloudberry's own external-FTS builds take the same step
 * away from the catalog: there the rows come from etcd and
 * gp_segment_configuration becomes a view over a function.
 *
 * The file is read in _PG_init, so a cluster that is described wrongly is a
 * server that does not start, with the line number in the message -- rather
 * than a query, much later, that dispatches somewhere unexpected.
 *
 * The file gives each node the role it prefers.  Which one of a content's
 * two nodes is its primary now, and whether they are in sync and up, is
 * FTS's to say (gp_fts.c), and changes while the cluster runs: on the
 * coordinator it is kept in shared memory, and in gpsegconfig_dump, the
 * file Cloudberry's FTS writes of gp_segment_configuration for readers
 * outside a transaction, which is read back when the server starts.  A
 * backend works from its own copy, which it brings up to date when it is
 * about to connect to the segments (GpClusterRefresh), so that the node a
 * content id stands for does not change under a gang that is in use.
 *
 * The coordinator's standby has them too, as Cloudberry's has its replicated
 * catalog: each time the coordinator writes gpsegconfig_dump it logs the
 * same text in WAL, a record of gp_core's resource manager, and waits for
 * its standby to have it, as a commit waits, before anything is acted on.
 * The standby's replay writes its own gpsegconfig_dump and its shared
 * memory, and it reads the file back when it starts, as the coordinator
 * does.  So a standby promoted -- by gpfts, the coordinator's automatic
 * failover, or by gpactivatestandby -- dispatches to the primaries the old
 * coordinator last had, and its FTS takes up from where the old one was.
 *
 * The nodes change while the cluster runs too, on the coordinator, where
 * Cloudberry's segment administration functions add a mirror or a standby,
 * remove one, or put a failed one somewhere else (gp_segadmin.c).  Such a
 * change is written to the file, rewritten in place -- so that a node
 * started after it, the one added among them, reads it -- then to
 * gpsegconfig_dump and to shared memory, which keeps where each node is
 * beside its state.  The room there is fixed as the server starts: a
 * primary and a mirror for each content the cluster may grow to --
 * gp.max_segments of them, or the file's, if it has more -- the coordinator
 * and a standby.  On a cluster of several hosts, Cloudberry's tools would
 * copy the file to the others.
 *
 * gpexpand adds segments so, and gpshrink removes the last ones (gp_expand.c):
 * the number of segments is kept in shared memory beside the nodes, and a
 * backend takes it with them -- once a transaction, as the transaction first
 * asks for it, so never while a gang made for the old number is in use -- as
 * Cloudberry's backend takes gp_segment_configuration's rows as each
 * transaction starts.  A segment process computes with its coordinator
 * backend's number, which comes with its identity, where Cloudberry's comes
 * with each statement: a segment that was running before the change still
 * has the old number in its file.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/cdbutil.c (the readGpSegConfig half), and
 *	  src/include/catalog/gp_segment_configuration.h
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/syncrep.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dbcopy.h"
#include "gp_segadmin.h"

/* The longest line the configuration file may hold. */
#define GP_CLUSTER_LINE_MAX		4096

/* Settings, all of them "gp.*" because a file may hold them; see gp_core.c. */
static char *gp_cluster_config = NULL;
static int	gp_dbid = 1;
static int	gp_role_setting = GP_ROLE_UTILITY;
static char *gp_qe_identity = NULL;
static int	gp_max_segments = 64;

/* gp.session_id, which is shown and never set; see show_session_id(). */
static int	gp_session_id_shown = -1;
/* gp.contentid, the same; see show_contentid(). */
static int	gp_contentid_shown = -1;
static char *gp_cluster_secret = NULL;
static char *gp_qe_secret = NULL;

/* A secret shorter than this is one somebody could guess. */
#define GP_CLUSTER_SECRET_MIN	16

static const struct config_enum_entry gp_role_options[] = {
	{"utility", GP_ROLE_UTILITY, false},
	{"dispatch", GP_ROLE_DISPATCH, false},
	{"execute", GP_ROLE_EXECUTE, false},
	{NULL, 0, false}
};

/*
 * The cluster, as the file described it: its nodes in the file's order, and
 * after them the places where the coordinator may add one, whose dbid is 0.
 * Read in the postmaster, and inherited by every backend; under
 * EXEC_BACKEND the library is loaded again in the child, which reads it
 * again.  A backend adopts the coordinator's changes from shared memory
 * (cluster_adopt_nodes()).
 */
static GpSegmentConfig *cluster = NULL;
static int	cluster_nnodes = 0;

/* An empty place's content id, which no content has. */
#define GP_CLUSTER_NO_CONTENT	(-2)

/*
 * The primaries with content >= 0, in content order: a slice of the above,
 * with room for every content the cluster may grow to, so that it stays
 * where a gang's connections point.  The number of them is the one this
 * backend last took from shared memory (GpClusterAdoptSegments()), or the
 * file's before it first has.
 */
static GpSegmentConfig *cluster_segments = NULL;
static int	cluster_nsegments = 0;
static int	cluster_max_segments = 0;

/* gpexpand's version the number is of, and whether this backend took it. */
static uint64 cluster_expand_version = 0;
static bool cluster_counted = false;

/*
 * What says whether a backend takes the segments changed since it last took
 * them (gp_expand.c), and the transaction it last said it for.
 */
static GpClusterDecider cluster_decider = NULL;
static LocalTransactionId cluster_decided = InvalidLocalTransactionId;

/* On a segment process, its coordinator backend's number; -1 not read yet. */
static int	cluster_qe_nsegments = -1;

/* This node's entry in it. */
static const GpSegmentConfig *cluster_self = NULL;

/*
 * Where a node is, as shared memory keeps it beside its state: in the places
 * cluster[] has, one each.
 */
typedef struct GpClusterSlot
{
	int			dbid;			/* 0: no node in this place */
	int			content;
	char		preferred_role;
	int			port;
	char		hostname[MAXPGPATH];
	char		datadir[MAXPGPATH];
} GpClusterSlot;

/*
 * What FTS last published of each node, as cluster[] orders them, and the
 * number it bumps at each change; in shared memory, on every node that has a
 * cluster, though only the coordinator's changes.  The nodes themselves are
 * in cluster_slots, beside it, changed under "lock", which FTS holds too as
 * it publishes.
 */
typedef struct GpClusterShared
{
	slock_t		mutex;			/* the states, and the versions */
	uint64		version;		/* bumped at each change of a state or a node */
	uint64		nodes_version;	/* bumped at each change of a node */
	uint64		expand_version; /* gpexpand's, 0 as the server starts */
	int			nsegments;		/* the contents 0..n-1 the primaries hold */
	LWLock	   *lock;
	int			nnodes;
	GpClusterNodeState nodes[FLEXIBLE_ARRAY_MEMBER];
} GpClusterShared;

static GpClusterShared *cluster_shared = NULL;
static GpClusterSlot *cluster_slots = NULL;

/* The versions of them this backend's copy is, 0 before the first. */
static uint64 cluster_version = 0;
static uint64 cluster_nodes_version = 0;

/* The server has started: a later shared memory startup is a crash's. */
static bool cluster_started = false;

/*
 * The end of the last record of the states this backend logged, until it has
 * waited for the standby to have it (cluster_dump_replicated()).
 */
static XLogRecPtr cluster_dump_lsn = InvalidXLogRecPtr;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

static void gp_cluster_read_file(const char *path);
static bool cluster_read_dump(int elevel);
static void cluster_build_segments(const char *path);
static void cluster_adopt_nodes(void);

/* An empty place: no node, and a content id no content has. */
static void
cluster_empty_place(GpSegmentConfig *node)
{
	memset(node, 0, sizeof(*node));
	node->content = GP_CLUSTER_NO_CONTENT;
	node->mode = 'n';
	node->status = 'd';
	node->hostname = "";
	node->datadir = "";
}

/* ------------------------------------------------------------------------- */
/* Reading the file                                                          */
/* ------------------------------------------------------------------------- */

/*
 * One field of a line, in place: returns the start of the next word and
 * NUL-terminates it, or NULL when the line holds no more.
 */
static char *
next_field(char **p)
{
	char	   *s = *p;
	char	   *start;

	while (*s != '\0' && isspace((unsigned char) *s))
		s++;
	if (*s == '\0')
	{
		*p = s;
		return NULL;
	}

	start = s;
	while (*s != '\0' && !isspace((unsigned char) *s))
		s++;
	if (*s != '\0')
		*s++ = '\0';
	*p = s;

	return start;
}

/*
 * The rest of the line, with the spaces in front of it taken off and the ones
 * behind it too.  A data directory may hold a space, so it is the last field
 * and is not split.
 */
static char *
rest_of_line(char *p)
{
	char	   *end;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	if (*p == '\0')
		return NULL;

	end = p + strlen(p);
	while (end > p && isspace((unsigned char) end[-1]))
		end--;
	*end = '\0';

	return p;
}

static int
parse_int_field(const char *path, int lineno, const char *what, const char *s)
{
	char	   *endptr;
	long		val;

	errno = 0;
	val = strtol(s, &endptr, 10);
	if (errno != 0 || *endptr != '\0' || val < INT_MIN || val > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid %s \"%s\" in cluster configuration file \"%s\", line %d",
						what, s, path, lineno)));

	return (int) val;
}

/*
 * Parse the file, and check that what it describes could be a cluster: its
 * nodes, in its order, palloc'd, and how many.
 *
 * Every complaint names the file and the line, because this runs in the
 * postmaster while it is starting: the message is all the operator gets.
 */
static int
cluster_parse_file(const char *path, GpSegmentConfig **nodes_out)
{
	FILE	   *fp;
	char		buf[GP_CLUSTER_LINE_MAX];
	int			lineno = 0;
	int			nnodes = 0;
	int			nalloc = 0;
	int			ncoordinators = 0;
	GpSegmentConfig *nodes;

	fp = AllocateFile(path, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open cluster configuration file \"%s\": %m",
						path),
				 errhint("\"gp.cluster_config\" names the file that lists this cluster's nodes.")));

	nodes = NULL;

	while (fgets(buf, sizeof(buf), fp) != NULL)
	{
		char	   *p = buf;
		char	   *field;
		GpSegmentConfig *node;
		size_t		len = strlen(buf);

		lineno++;

		if (len == sizeof(buf) - 1 && buf[len - 1] != '\n')
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("line %d of cluster configuration file \"%s\" is longer than %d bytes",
							lineno, path, GP_CLUSTER_LINE_MAX - 1)));

		/* Comments and blank lines. */
		field = strchr(buf, '#');
		if (field != NULL)
			*field = '\0';
		if (rest_of_line(buf) == NULL)
			continue;

		/* repalloc() will not take a NULL pointer, so the first is a palloc. */
		if (nnodes >= nalloc)
		{
			nalloc = nalloc == 0 ? 8 : nalloc * 2;
			nodes = nodes == NULL
				? (GpSegmentConfig *) palloc_array(GpSegmentConfig, nalloc)
				: (GpSegmentConfig *) repalloc_array(nodes, GpSegmentConfig,
													 nalloc);
		}
		node = &nodes[nnodes];
		memset(node, 0, sizeof(*node));

		field = next_field(&p);
		if (field == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("missing dbid in cluster configuration file \"%s\", line %d",
							path, lineno)));
		node->dbid = parse_int_field(path, lineno, "dbid", field);

		field = next_field(&p);
		if (field == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("missing content id in cluster configuration file \"%s\", line %d",
							path, lineno)));
		node->content = parse_int_field(path, lineno, "content id", field);

		field = next_field(&p);
		if (field == NULL || field[1] != '\0' ||
			(field[0] != 'p' && field[0] != 'm'))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid role \"%s\" in cluster configuration file \"%s\", line %d",
							field ? field : "", path, lineno),
					 errdetail("The role is \"p\" for a primary or \"m\" for a mirror.")));
		node->role = field[0];

		/*
		 * The role the file gives is the node's preferred one, and the one it
		 * has until FTS says otherwise; a primary and its mirror are not
		 * known to be in sync until FTS has asked.
		 */
		node->preferred_role = node->role;
		node->mode = 'n';
		node->status = 'u';

		field = next_field(&p);
		if (field == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("missing host in cluster configuration file \"%s\", line %d",
							path, lineno)));
		node->hostname = pstrdup(field);

		field = next_field(&p);
		if (field == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("missing port in cluster configuration file \"%s\", line %d",
							path, lineno)));
		node->port = parse_int_field(path, lineno, "port", field);
		if (node->port <= 0 || node->port > 65535)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("port %d is out of range in cluster configuration file \"%s\", line %d",
							node->port, path, lineno)));

		/* The data directory is the rest of the line: it may hold a space. */
		field = rest_of_line(p);
		node->datadir = field ? pstrdup(field) : pstrdup("");

		if (node->dbid <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("dbid %d is not positive in cluster configuration file \"%s\", line %d",
							node->dbid, path, lineno)));
		if (node->content < -1)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("content id %d is below -1 in cluster configuration file \"%s\", line %d",
							node->content, path, lineno)));
		if (node->content == -1 && node->role == 'p')
			ncoordinators++;

		for (int i = 0; i < nnodes; i++)
		{
			if (nodes[i].dbid == node->dbid)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("dbid %d appears twice in cluster configuration file \"%s\", line %d",
								node->dbid, path, lineno)));
			if (nodes[i].content == node->content && nodes[i].role == node->role)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("content %d has two nodes with role \"%c\" in cluster configuration file \"%s\", line %d",
								node->content, node->role, path, lineno)));
		}

		nnodes++;
	}

	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read cluster configuration file \"%s\": %m",
						path)));
	FreeFile(fp);

	if (nnodes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cluster configuration file \"%s\" describes no node", path)));
	if (ncoordinators != 1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cluster configuration file \"%s\" has %d coordinators, not one",
						path, ncoordinators),
				 errdetail("The coordinator is the node with content id -1 and role \"p\".")));

	*nodes_out = nodes;
	return nnodes;
}

/*
 * Read the file into "cluster", with room after its nodes for those the
 * coordinator may add, and find this node in it.
 */
static void
gp_cluster_read_file(const char *path)
{
	GpSegmentConfig *nodes;
	int			nnodes;
	MemoryContext oldcxt;

	/*
	 * Read into the postmaster's own context, so that what is parsed here
	 * outlives this function and every backend forked afterwards inherits it.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	nnodes = cluster_parse_file(path, &nodes);

	/*
	 * And after them the room for what the coordinator may add: a primary
	 * and a mirror for each content the cluster may grow to -- as many as
	 * gp.max_segments says, or the file's primaries hold, if they hold more
	 * -- and the coordinator and a standby.
	 */
	{
		int			nsegments = 0;
		int			room;

		for (int i = 0; i < nnodes; i++)
			if (nodes[i].content >= 0 && nodes[i].preferred_role == 'p')
				nsegments++;
		cluster_max_segments = Max(gp_max_segments, nsegments);
		room = Max(nnodes, 2 * (cluster_max_segments + 1));
		cluster = palloc_array(GpSegmentConfig, room);
		memcpy(cluster, nodes, nnodes * sizeof(GpSegmentConfig));
		for (int i = nnodes; i < room; i++)
			cluster_empty_place(&cluster[i]);
		cluster_nnodes = room;
		pfree(nodes);
		nodes = cluster;
	}
	MemoryContextSwitchTo(oldcxt);

	/* Which of them are we? */
	for (int i = 0; i < nnodes; i++)
	{
		if (nodes[i].dbid == gp_dbid)
		{
			cluster_self = &cluster[i];
			break;
		}
	}
	if (cluster_self == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("this node's dbid %d is not in cluster configuration file \"%s\"",
						gp_dbid, path),
				 errhint("\"gp.dbid\" says which node of the cluster this server is.")));

	/*
	 * A role the operator wrote down has to be the role the file gives this
	 * node.  Refusing to start is better than dispatching from a segment, or
	 * waiting to be dispatched to on the coordinator.
	 */
	if (gp_role_setting != GP_ROLE_UTILITY)
	{
		int			from_file = cluster_self->content == -1
			? GP_ROLE_DISPATCH : GP_ROLE_EXECUTE;

		if (gp_role_setting != from_file)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("\"gp.role\" is \"%s\", but cluster configuration file \"%s\" gives dbid %d content id %d",
							gp_role_setting == GP_ROLE_DISPATCH ? "dispatch" : "execute",
							path, gp_dbid, cluster_self->content),
					 errdetail("Content id -1 is the coordinator, which dispatches; every other node executes.")));
	}

	/*
	 * On the coordinator, what FTS last found of the nodes, if it has found
	 * anything: a primary it failed over from is a mirror now, and down, and
	 * must not be dispatched to because the file prefers it.  On its standby,
	 * the same, as it last reached the standby.  A dump that cannot be read
	 * is a server that does not start, as a file that cannot be is.
	 */
	if (cluster_self->content == -1)
		(void) cluster_read_dump(ERROR);

	cluster_build_segments(path);
}

/*
 * Collect the primaries into cluster_segments, by content id.  The content ids
 * have to run 0..n-1 without a hole, because everything downstream -- the
 * hash that picks a segment, the gang that connects to them all -- indexes by
 * content id; and each has one primary, which is the node whose role is 'p'
 * now, not the one the file prefers.
 */
static void
cluster_build_segments(const char *path)
{
	int			nsegments = 0;
	MemoryContext oldcxt;

	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].content >= 0 && cluster[i].preferred_role == 'p')
			nsegments++;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	cluster_segments = nsegments > 0
		? (GpSegmentConfig *) palloc0_array(GpSegmentConfig, cluster_max_segments)
		: NULL;
	MemoryContextSwitchTo(oldcxt);
	cluster_nsegments = nsegments;

	for (int i = 0; i < cluster_nnodes; i++)
	{
		if (cluster[i].content >= 0 && cluster[i].preferred_role == 'p' &&
			cluster[i].content >= nsegments)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("cluster configuration file \"%s\" has %d segments, so content id %d is out of range",
							path, nsegments, cluster[i].content),
					 errdetail("The content ids of the primaries run from 0 to one less than their number.")));
		if (cluster[i].content >= 0 && cluster[i].preferred_role == 'm' &&
			cluster[i].content >= nsegments)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("cluster configuration file \"%s\" has a mirror of content id %d, which has no primary",
							path, cluster[i].content)));
	}

	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].content >= 0 && cluster[i].role == 'p')
			cluster_segments[cluster[i].content] = cluster[i];
}

/*
 * Read gpsegconfig_dump, what FTS last wrote of the nodes: a line per node,
 * "dbid content role preferred_role mode status port host address", as
 * Cloudberry's FTS writes it.  Of each line the role, mode and status are
 * taken into states, by dbid, in the places of cluster[]; the file this
 * reads with the rest says which nodes there are and where, so a line for a
 * node it does not list, or with another content id, is passed over with a
 * warning -- the cluster was changed since FTS last wrote -- and a node with
 * no line keeps what states has.  Nothing is taken unless every content id
 * is left with one primary.
 *
 * No file is no change: FTS has not written one yet.  Anything else wrong is
 * reported at elevel, and false returned with nothing changed.
 */
static bool
cluster_parse_dump(GpClusterNodeState *states, int elevel)
{
	FILE	   *fp;
	char		buf[GP_CLUSTER_LINE_MAX];
	int			lineno = 0;
	GpClusterNodeState *found;

	fp = AllocateFile(GP_CLUSTER_DUMP_FILE, "r");
	if (fp == NULL)
	{
		if (errno == ENOENT)
			return true;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", GP_CLUSTER_DUMP_FILE)));
		return false;
	}

	found = palloc_array(GpClusterNodeState, cluster_nnodes);
	memcpy(found, states, cluster_nnodes * sizeof(GpClusterNodeState));

	while (fgets(buf, sizeof(buf), fp) != NULL)
	{
		int			dbid;
		int			content;
		char		role;
		char		preferred;
		char		mode;
		char		status;
		int			i;

		lineno++;
		if (sscanf(buf, "%d %d %c %c %c %c", &dbid, &content, &role,
				   &preferred, &mode, &status) != 6 ||
			(role != 'p' && role != 'm') || (mode != 's' && mode != 'n') ||
			(status != 'u' && status != 'd'))
		{
			FreeFile(fp);
			ereport(elevel,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid line %d in file \"%s\"",
							lineno, GP_CLUSTER_DUMP_FILE),
					 errdetail("A line is \"dbid content role preferred_role mode status port host address\".")));
			return false;
		}

		for (i = 0; i < cluster_nnodes; i++)
			if (cluster[i].dbid == dbid)
				break;
		if (i == cluster_nnodes || cluster[i].content != content)
		{
			ereport(WARNING,
					(errmsg("file \"%s\" has dbid %d with content id %d, which the cluster configuration file does not list; ignored",
							GP_CLUSTER_DUMP_FILE, dbid, content)));
			continue;
		}
		found[i].role = role;
		found[i].mode = mode;
		found[i].status = status;
	}
	if (ferror(fp))
	{
		FreeFile(fp);
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", GP_CLUSTER_DUMP_FILE)));
		return false;
	}
	FreeFile(fp);

	/* One primary for each content id, the coordinator's included. */
	for (int i = 0; i < cluster_nnodes; i++)
	{
		int			nprimaries = 0;

		if (cluster[i].dbid == 0)
			continue;
		for (int j = 0; j < cluster_nnodes; j++)
			if (cluster[j].content == cluster[i].content && found[j].role == 'p')
				nprimaries++;
		if (nprimaries != 1)
		{
			ereport(elevel,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("file \"%s\" gives content id %d %d primaries",
							GP_CLUSTER_DUMP_FILE, cluster[i].content, nprimaries)));
			return false;
		}
	}

	memcpy(states, found, cluster_nnodes * sizeof(GpClusterNodeState));
	pfree(found);
	return true;
}

/* gpsegconfig_dump's states made this process's own copy's. */
static bool
cluster_read_dump(int elevel)
{
	GpClusterNodeState *states = palloc_array(GpClusterNodeState, cluster_nnodes);

	for (int i = 0; i < cluster_nnodes; i++)
	{
		states[i].role = cluster[i].role;
		states[i].mode = cluster[i].mode;
		states[i].status = cluster[i].status;
		states[i].dbid = cluster[i].dbid;
	}
	if (!cluster_parse_dump(states, elevel))
	{
		pfree(states);
		return false;
	}
	for (int i = 0; i < cluster_nnodes; i++)
	{
		cluster[i].role = states[i].role;
		cluster[i].mode = states[i].mode;
		cluster[i].status = states[i].status;
	}
	pfree(states);
	return true;
}

/*
 * gpsegconfig_dump as a whole new file, durably: synced, and renamed over the
 * old one, the directory synced too.
 */
static void
cluster_store_dump(const char *text, int len)
{
	FILE	   *fp;

	fp = AllocateFile(GP_CLUSTER_DUMP_FILE_TMP, "w");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", GP_CLUSTER_DUMP_FILE_TMP)));
	if (fwrite(text, 1, len, fp) != (size_t) len)
	{
		FreeFile(fp);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", GP_CLUSTER_DUMP_FILE_TMP)));
	}
	if (fflush(fp) != 0 || pg_fsync(fileno(fp)) != 0)
	{
		FreeFile(fp);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", GP_CLUSTER_DUMP_FILE_TMP)));
	}
	if (FreeFile(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", GP_CLUSTER_DUMP_FILE_TMP)));

	(void) durable_rename(GP_CLUSTER_DUMP_FILE_TMP, GP_CLUSTER_DUMP_FILE, ERROR);
}

/*
 * Write the states to gpsegconfig_dump, in Cloudberry's form, so that its
 * tools could read it; the host is the address.  The nodes are shared
 * memory's, whose lock the caller holds.  And the same text to WAL, for the
 * standby, whom the caller waits for once it has let go of the lock
 * (cluster_dump_replicated()): where the WAL is minimal there is none.
 */
static void
cluster_write_dump(const GpClusterNodeState *states)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpClusterSlot *node = &cluster_slots[i];

		if (node->dbid == 0)
			continue;
		appendStringInfo(&buf, "%d %d %c %c %c %c %d %s %s\n", node->dbid,
						 node->content, states[i].role, node->preferred_role,
						 states[i].mode, states[i].status, node->port,
						 node->hostname, node->hostname);
	}
	cluster_store_dump(buf.data, buf.len);

	if (XLogIsNeeded() && !RecoveryInProgress())
	{
		XLogBeginInsert();
		XLogRegisterData(buf.data, buf.len);
		cluster_dump_lsn = XLogInsert(GP_CORE_RMGR_ID, XLOG_GP_CORE_CLUSTER);
	}
	pfree(buf.data);
}

/*
 * Wait until the standby has the states this backend last logged, as a
 * commit waits for it -- Cloudberry's FTS commits its change to the catalog,
 * which the standby replays, before it acts on it; so what the coordinator
 * acts on, a standby promoted afterwards knows.  Called with the cluster's
 * lock let go: a slow standby holds up the one that wrote, and nothing else.
 */
static void
cluster_dump_replicated(void)
{
	XLogRecPtr	lsn = cluster_dump_lsn;

	if (!XLogRecPtrIsValid(lsn))
		return;
	cluster_dump_lsn = InvalidXLogRecPtr;
	XLogFlush(lsn);
	HOLD_INTERRUPTS();
	SyncRepWaitForLSN(lsn, false);
	RESUME_INTERRUPTS();
}

/*
 * The replay of the states the coordinator logged, on its standby: its own
 * gpsegconfig_dump, and its shared memory, so that a promotion finds them
 * there.  Not in crash recovery, where the node's own file is newer than its
 * WAL, having been written before the record: only where the server follows
 * a primary or recovers from an archive, as gp_dbcopy.c's record.
 */
void
GpClusterRedo(const char *text, int len)
{
	GpClusterNodeState *states;

	if (!ArchiveRecoveryRequested || cluster_shared == NULL)
		return;

	cluster_store_dump(text, len);

	states = palloc_array(GpClusterNodeState, cluster_nnodes);
	(void) GpClusterLiveStates(states);
	if (cluster_parse_dump(states, WARNING))
	{
		LWLockAcquire(cluster_shared->lock, LW_EXCLUSIVE);
		SpinLockAcquire(&cluster_shared->mutex);
		for (int i = 0; i < cluster_nnodes; i++)
			if (cluster_shared->nodes[i].dbid == states[i].dbid)
				cluster_shared->nodes[i] = states[i];
		cluster_shared->version++;
		SpinLockRelease(&cluster_shared->mutex);
		LWLockRelease(cluster_shared->lock);
	}
	pfree(states);
}

/* ------------------------------------------------------------------------- */
/* The live copy                                                             */
/* ------------------------------------------------------------------------- */

/*
 * How many segments these nodes are: one more than the greatest content id
 * any of them has, the coordinator's -1 being none.
 */
static int
count_segments(const GpSegmentConfig *nodes, int nnodes)
{
	int			nsegments = 0;

	for (int i = 0; i < nnodes; i++)
		if (nodes[i].dbid != 0 && nodes[i].content >= nsegments)
			nsegments = nodes[i].content + 1;
	return nsegments;
}

static Size
cluster_shared_size(void)
{
	return add_size(offsetof(GpClusterShared, nodes),
					mul_size(cluster_nnodes, sizeof(GpClusterNodeState)));
}

static Size
cluster_slots_size(void)
{
	return mul_size(cluster_nnodes, sizeof(GpClusterSlot));
}

static void
cluster_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(MAXALIGN(cluster_shared_size()));
	RequestAddinShmemSpace(MAXALIGN(cluster_slots_size()));
	RequestNamedLWLockTranche("gp_core cluster", 1);
}

/*
 * The file again, in the postmaster, as its shared memory is made again after
 * a crash: the coordinator may have changed the nodes since it was first read
 * (gp_segadmin.c).  What cannot be read keeps what was, with a message.
 */
static void
cluster_reread_file(void)
{
	MemoryContext cxt = AllocSetContextCreate(TopMemoryContext,
											  "gp_core cluster file",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);

	PG_TRY();
	{
		GpSegmentConfig *nodes;
		int			nnodes = cluster_parse_file(gp_cluster_config, &nodes);

		if (nnodes > cluster_nnodes)
			ereport(ERROR,
					(errmsg("cluster configuration file \"%s\" lists %d nodes, more than the %d this server made room for",
							gp_cluster_config, nnodes, cluster_nnodes)));
		for (int i = 0; i < cluster_nnodes; i++)
		{
			GpSegmentConfig *place = &cluster[i];

			if (i >= nnodes)
			{
				cluster_empty_place(place);
				continue;
			}
			*place = nodes[i];
			place->hostname = MemoryContextStrdup(TopMemoryContext, nodes[i].hostname);
			place->datadir = MemoryContextStrdup(TopMemoryContext, nodes[i].datadir);
		}
		for (int i = 0; i < cluster_nnodes; i++)
			if (cluster[i].dbid == gp_dbid)
				cluster_self = &cluster[i];

		/* and the segments, which the file holds as the nodes last changed */
		if (cluster_segments != NULL)
			cluster_nsegments = Min(count_segments(cluster, cluster_nnodes),
									cluster_max_segments);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(TopMemoryContext);
		EmitErrorReport();
		FlushErrorState();
		ereport(LOG,
				(errmsg("the cluster's nodes are kept as the server started with them")));
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * Runs in the postmaster, when the server starts and again after a crash --
 * when the coordinator may have changed the nodes, and FTS their states,
 * since the files were first read, which is why the coordinator reads them
 * again here.
 */
static void
cluster_shmem_startup(void)
{
	bool		found;
	bool		found_slots;

	if (prev_shmem_startup)
		prev_shmem_startup();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	cluster_shared = ShmemInitStruct("gp_core cluster", cluster_shared_size(),
									 &found);
	cluster_slots = ShmemInitStruct("gp_core cluster nodes", cluster_slots_size(),
									&found_slots);
	if (!found)
	{
		/* the coordinator, or its standby, which keeps its states too */
		bool		coordinator = cluster_self != NULL &&
			cluster_self->content == -1;

		if (!IsUnderPostmaster && coordinator)
		{
			if (cluster_started)
				cluster_reread_file();
			(void) cluster_read_dump(LOG);
		}

		SpinLockInit(&cluster_shared->mutex);
		cluster_shared->version = 1;
		cluster_shared->nodes_version = 1;
		cluster_shared->expand_version = 0;
		cluster_shared->nsegments = cluster_nsegments;
		cluster_shared->lock = &(GetNamedLWLockTranche("gp_core cluster"))->lock;
		cluster_shared->nnodes = cluster_nnodes;
		for (int i = 0; i < cluster_nnodes; i++)
		{
			GpClusterSlot *slot = &cluster_slots[i];

			cluster_shared->nodes[i].role = cluster[i].role;
			cluster_shared->nodes[i].mode = cluster[i].mode;
			cluster_shared->nodes[i].status = cluster[i].status;
			cluster_shared->nodes[i].dbid = cluster[i].dbid;

			memset(slot, 0, sizeof(*slot));
			slot->dbid = cluster[i].dbid;
			slot->content = cluster[i].content;
			slot->preferred_role = cluster[i].preferred_role;
			slot->port = cluster[i].port;
			strlcpy(slot->hostname, cluster[i].hostname, MAXPGPATH);
			strlcpy(slot->datadir, cluster[i].datadir, MAXPGPATH);
		}
		cluster_started = true;
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * Adopt the nodes as the coordinator has them now, where it has changed any
 * since this backend last looked: where each is, and what FTS last found of
 * a node that is new in its place.  Not which is each content's primary,
 * which GpClusterRefresh() alone adopts, where no gang can be using it.  The
 * strings a node had stay: a connection may point at them.
 */
static void
cluster_adopt_nodes(void)
{
	uint64		version;

	if (cluster_shared == NULL)
		return;
	SpinLockAcquire(&cluster_shared->mutex);
	version = cluster_shared->nodes_version;
	SpinLockRelease(&cluster_shared->mutex);
	if (version == cluster_nodes_version)
		return;

	LWLockAcquire(cluster_shared->lock, LW_SHARED);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpClusterSlot *slot = &cluster_slots[i];
		GpSegmentConfig *node = &cluster[i];
		bool		newcomer = node->dbid != slot->dbid;

		if (slot->dbid == 0)
		{
			if (node->dbid != 0)
				cluster_empty_place(node);
			continue;
		}
		node->dbid = slot->dbid;
		node->content = slot->content;
		node->preferred_role = slot->preferred_role;
		node->port = slot->port;
		if (strcmp(node->hostname, slot->hostname) != 0)
			node->hostname = MemoryContextStrdup(TopMemoryContext, slot->hostname);
		if (strcmp(node->datadir, slot->datadir) != 0)
			node->datadir = MemoryContextStrdup(TopMemoryContext, slot->datadir);
		if (newcomer)
		{
			SpinLockAcquire(&cluster_shared->mutex);
			node->role = cluster_shared->nodes[i].role;
			node->mode = cluster_shared->nodes[i].mode;
			node->status = cluster_shared->nodes[i].status;
			SpinLockRelease(&cluster_shared->mutex);
		}
	}
	SpinLockAcquire(&cluster_shared->mutex);
	cluster_nodes_version = cluster_shared->nodes_version;
	SpinLockRelease(&cluster_shared->mutex);
	LWLockRelease(cluster_shared->lock);
}

bool
GpClusterRefresh(void)
{
	uint64		version;
	bool		changed = false;
	bool		whole = true;

	if (cluster_shared == NULL)
		return false;

	cluster_adopt_nodes();

	SpinLockAcquire(&cluster_shared->mutex);
	version = cluster_shared->version;
	if (version != cluster_version)
	{
		for (int i = 0; i < cluster_nnodes; i++)
		{
			/* a node changed since it was adopted is adopted next time */
			if (cluster_shared->nodes[i].dbid != cluster[i].dbid)
			{
				whole = false;
				continue;
			}
			cluster[i].role = cluster_shared->nodes[i].role;
			cluster[i].mode = cluster_shared->nodes[i].mode;
			cluster[i].status = cluster_shared->nodes[i].status;
		}
	}
	SpinLockRelease(&cluster_shared->mutex);

	if (version == cluster_version)
		return false;
	if (whole)
		cluster_version = version;

	for (int i = 0; i < cluster_nnodes; i++)
	{
		int			content = cluster[i].content;

		if (content < 0 || content >= cluster_nsegments || cluster[i].role != 'p')
			continue;
		if (cluster_segments[content].dbid != cluster[i].dbid)
			changed = true;
		cluster_segments[content] = cluster[i];
	}
	return changed;
}

bool
GpClusterStale(void)
{
	uint64		version;

	if (cluster_shared == NULL)
		return false;
	SpinLockAcquire(&cluster_shared->mutex);
	version = cluster_shared->version;
	SpinLockRelease(&cluster_shared->mutex);
	return version != cluster_version;
}

bool
GpClusterSegmentsChanged(void)
{
	bool		changed;

	if (cluster_shared == NULL)
		return false;
	SpinLockAcquire(&cluster_shared->mutex);
	changed = cluster_shared->nsegments != cluster_nsegments ||
		cluster_shared->expand_version != cluster_expand_version;
	SpinLockRelease(&cluster_shared->mutex);
	return changed;
}

/*
 * Take the number of segments from shared memory, gpexpand's version with it,
 * and the primaries of the contents it counts, as FTS last published them.
 * The places a content no longer counted had are emptied.
 */
bool
GpClusterAdoptSegments(void)
{
	int			nsegments;
	uint64		expand_version;

	if (cluster_shared == NULL || cluster_segments == NULL)
		return false;
	cluster_counted = true;
	SpinLockAcquire(&cluster_shared->mutex);
	nsegments = cluster_shared->nsegments;
	expand_version = cluster_shared->expand_version;
	SpinLockRelease(&cluster_shared->mutex);
	if (nsegments == cluster_nsegments &&
		expand_version == cluster_expand_version)
		return false;

	cluster_nsegments = nsegments;
	cluster_expand_version = expand_version;
	for (int content = nsegments; content < cluster_max_segments; content++)
		memset(&cluster_segments[content], 0, sizeof(GpSegmentConfig));

	/* every state again, so that each content counted now has its primary */
	cluster_version = 0;
	(void) GpClusterRefresh();
	return true;
}

uint64
GpClusterAdoptedExpandVersion(void)
{
	GpClusterDecideSegments();
	return cluster_expand_version;
}

int
GpClusterMaxSegments(void)
{
	return cluster_segments != NULL ? cluster_max_segments : 0;
}

void
GpClusterSetDecider(GpClusterDecider decider)
{
	cluster_decider = decider;
}

/*
 * A backend of the coordinator's takes the number once before it first asks
 * for it -- one forked after the segments changed has the postmaster's, the
 * number the server started with -- and then, once in each transaction, as
 * it first asks, whether it takes the segments changed since is the
 * decider's to say: nothing in the transaction has used the number yet.  A
 * process of no database, which a decider could not look into, takes them
 * itself (GpClusterAdoptSegments()).
 */
void
GpClusterDecideSegments(void)
{
	if (!IsUnderPostmaster || cluster_self == NULL ||
		cluster_self->content != -1 || GpClusterIsDispatched())
		return;
	if (MyProc != NULL && IsTransactionState())
	{
		if (cluster_decided == MyProc->vxid.lxid)
			return;
		cluster_decided = MyProc->vxid.lxid;
	}
	else if (cluster_counted)
		return;

	if (!cluster_counted)
		(void) GpClusterAdoptSegments();
	else if (cluster_decider != NULL && OidIsValid(MyDatabaseId) &&
			 GpClusterSegmentsChanged())
		cluster_decider();
}

bool
GpClusterIsPrimaryNow(int dbid)
{
	bool		primary = false;

	if (cluster_shared == NULL)
		return true;

	/* by dbid: a node removed since this backend looked is no primary */
	SpinLockAcquire(&cluster_shared->mutex);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		if (cluster_shared->nodes[i].dbid != dbid)
			continue;
		primary = cluster_shared->nodes[i].role == 'p' &&
			cluster_shared->nodes[i].status == 'u';
		break;
	}
	SpinLockRelease(&cluster_shared->mutex);
	return primary;
}

uint64
GpClusterLiveStates(GpClusterNodeState *states)
{
	uint64		version = 0;

	if (cluster_shared == NULL)
	{
		for (int i = 0; i < cluster_nnodes; i++)
		{
			states[i].role = cluster[i].role;
			states[i].mode = cluster[i].mode;
			states[i].status = cluster[i].status;
			states[i].dbid = cluster[i].dbid;
		}
		return version;
	}

	SpinLockAcquire(&cluster_shared->mutex);
	version = cluster_shared->version;
	for (int i = 0; i < cluster_nnodes; i++)
		states[i] = cluster_shared->nodes[i];
	SpinLockRelease(&cluster_shared->mutex);
	return version;
}

bool
GpClusterPublish(const GpClusterNodeState *states)
{
	Assert(cluster_shared != NULL);

	LWLockAcquire(cluster_shared->lock, LW_EXCLUSIVE);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		if (states[i].dbid != cluster_slots[i].dbid)
		{
			LWLockRelease(cluster_shared->lock);
			return false;
		}
	}

	/*
	 * Durable first, and on the standby once the lock is let go: FTS promotes
	 * a mirror only once this has returned.
	 */
	cluster_write_dump(states);

	SpinLockAcquire(&cluster_shared->mutex);
	for (int i = 0; i < cluster_nnodes; i++)
		cluster_shared->nodes[i] = states[i];
	cluster_shared->version++;
	SpinLockRelease(&cluster_shared->mutex);
	LWLockRelease(cluster_shared->lock);
	cluster_dump_replicated();
	return true;
}

uint64
GpClusterExpandVersion(void)
{
	uint64		version;

	if (cluster_shared == NULL)
		return 0;
	SpinLockAcquire(&cluster_shared->mutex);
	version = cluster_shared->expand_version;
	SpinLockRelease(&cluster_shared->mutex);
	return version;
}

void
GpClusterBumpExpandVersion(void)
{
	if (cluster_shared == NULL)
		return;
	SpinLockAcquire(&cluster_shared->mutex);
	cluster_shared->expand_version++;
	SpinLockRelease(&cluster_shared->mutex);
}

/* ------------------------------------------------------------------------- */
/* Changing the nodes                                                        */
/* ------------------------------------------------------------------------- */

void
GpClusterLockNodes(void)
{
	if (cluster_shared == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this server has no cluster configured")));
	LWLockAcquire(cluster_shared->lock, LW_EXCLUSIVE);
}

void
GpClusterUnlockNodes(void)
{
	LWLockRelease(cluster_shared->lock);
	cluster_dump_replicated();
}

int
GpClusterLiveNodes(GpSegmentConfig **nodes)
{
	GpSegmentConfig *all = palloc_array(GpSegmentConfig, cluster_nnodes);
	GpClusterNodeState *states = palloc_array(GpClusterNodeState, cluster_nnodes);

	Assert(LWLockHeldByMe(cluster_shared->lock));
	(void) GpClusterLiveStates(states);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpClusterSlot *slot = &cluster_slots[i];

		if (slot->dbid == 0)
		{
			cluster_empty_place(&all[i]);
			continue;
		}
		all[i].dbid = slot->dbid;
		all[i].content = slot->content;
		all[i].preferred_role = slot->preferred_role;
		all[i].port = slot->port;
		all[i].hostname = pstrdup(slot->hostname);
		all[i].datadir = pstrdup(slot->datadir);
		all[i].role = states[i].role;
		all[i].mode = states[i].mode;
		all[i].status = states[i].status;
	}
	pfree(states);
	*nodes = all;
	return cluster_nnodes;
}

/* A node line of the file: "dbid content role host port datadir". */
static void
cluster_file_line(StringInfo buf, const GpClusterSlot *slot)
{
	appendStringInfo(buf, "%d %d %c %s %d %s\n", slot->dbid, slot->content,
					 slot->preferred_role, slot->hostname, slot->port,
					 slot->datadir);
}

/*
 * Write the file gp.cluster_config names again, as shared memory has the
 * nodes: a node's line as it was where the node has not changed, and a new
 * one where it has, comments and blank lines as they were; a line of a node
 * that is gone dropped, and one for each node the file did not have at its
 * end.  A new file, synced and renamed over the old one, and the directory
 * synced: a node that starts after this reads the nodes as they are.
 */
static void
cluster_rewrite_file(void)
{
	char		tmp[MAXPGPATH];
	char		buf[GP_CLUSTER_LINE_MAX];
	bool	   *written = palloc0_array(bool, cluster_nnodes);
	StringInfoData out;
	FILE	   *fp;

	initStringInfo(&out);
	fp = AllocateFile(gp_cluster_config, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open cluster configuration file \"%s\": %m",
						gp_cluster_config)));
	while (fgets(buf, sizeof(buf), fp) != NULL)
	{
		char		copy[GP_CLUSTER_LINE_MAX];
		char	   *p = copy;
		char	   *field;
		char	   *hash;
		int			dbid;
		int			i;

		strlcpy(copy, buf, sizeof(copy));
		hash = strchr(copy, '#');
		if (hash != NULL)
			*hash = '\0';
		field = next_field(&p);
		if (field == NULL)
		{
			appendStringInfoString(&out, buf);	/* a comment, or blank */
			continue;
		}
		dbid = atoi(field);
		for (i = 0; i < cluster_nnodes; i++)
			if (cluster_slots[i].dbid == dbid && dbid != 0)
				break;
		if (i == cluster_nnodes || written[i])
			continue;			/* a node that is gone */
		written[i] = true;

		/* the line as it was, where its node is where the line says */
		{
			const GpClusterSlot *slot = &cluster_slots[i];
			char	   *content = next_field(&p);
			char	   *role = next_field(&p);
			char	   *host = next_field(&p);
			char	   *port = next_field(&p);
			char	   *datadir = p != NULL ? rest_of_line(p) : NULL;

			if (content != NULL && atoi(content) == slot->content &&
				role != NULL && role[0] == slot->preferred_role && role[1] == '\0' &&
				host != NULL && strcmp(host, slot->hostname) == 0 &&
				port != NULL && atoi(port) == slot->port &&
				strcmp(datadir != NULL ? datadir : "", slot->datadir) == 0)
			{
				appendStringInfoString(&out, buf);
				if (out.len > 0 && out.data[out.len - 1] != '\n')
					appendStringInfoChar(&out, '\n');
			}
			else
				cluster_file_line(&out, slot);
		}
	}
	if (ferror(fp))
	{
		FreeFile(fp);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read cluster configuration file \"%s\": %m",
						gp_cluster_config)));
	}
	FreeFile(fp);

	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster_slots[i].dbid != 0 && !written[i])
			cluster_file_line(&out, &cluster_slots[i]);

	snprintf(tmp, sizeof(tmp), "%s.tmp", gp_cluster_config);
	fp = AllocateFile(tmp, "w");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp),
				 errhint("The coordinator writes the cluster configuration file as its nodes change.")));
	if (fwrite(out.data, 1, out.len, fp) != (size_t) out.len ||
		fflush(fp) != 0 || pg_fsync(fileno(fp)) != 0)
	{
		FreeFile(fp);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	}
	if (FreeFile(fp) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmp)));
	(void) durable_rename(tmp, gp_cluster_config, ERROR);
	pfree(out.data);
	pfree(written);
}

/*
 * What a node may be, to be written to the file and read back: a content the
 * room is for, a host that is one word, a data directory on the rest of a
 * line, neither holding the file's comment sign.  A cluster whose file had no
 * segment has none to add either: it is a single node.
 */
static void
cluster_check_node(const GpSegmentConfig *node)
{
	const char *bad = NULL;
	int			max_segments = cluster_segments != NULL ? cluster_max_segments : 0;

	if (node->dbid <= 0)
		bad = psprintf("dbid %d is not positive", node->dbid);
	else if (node->content >= max_segments)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the cluster has no room for content %d", node->content),
				 errdetail("There is room for %d segments.", max_segments),
				 errhint("\"gp.max_segments\" says how many segments a cluster may grow to while it runs; it is read as the coordinator starts.")));
	else if (node->content < -1)
		bad = psprintf("content id %d is none of this cluster's", node->content);
	else if ((node->role != 'p' && node->role != 'm') ||
			 (node->preferred_role != 'p' && node->preferred_role != 'm'))
		bad = "a role is \"p\" or \"m\"";
	else if ((node->mode != 's' && node->mode != 'n') ||
			 (node->status != 'u' && node->status != 'd'))
		bad = "a mode is \"s\" or \"n\", a status \"u\" or \"d\"";
	else if (node->port <= 0 || node->port > 65535)
		bad = psprintf("port %d is out of range", node->port);
	else if (node->hostname == NULL || node->hostname[0] == '\0' ||
			 strlen(node->hostname) >= MAXPGPATH ||
			 strpbrk(node->hostname, " \t\r\n#") != NULL)
		bad = "a host is one word, without \"#\"";
	else if (node->datadir == NULL || node->datadir[0] == '\0' ||
			 strlen(node->datadir) >= MAXPGPATH ||
			 strpbrk(node->datadir, "\r\n#") != NULL ||
			 isspace((unsigned char) node->datadir[0]) ||
			 isspace((unsigned char) node->datadir[strlen(node->datadir) - 1]))
		bad = "a data directory is on one line, without \"#\" or spaces at its ends";
	if (bad != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid node for the cluster configuration: %s", bad)));
}

/*
 * Nodes that are a cluster: a dbid once each, and one primary for each
 * content, the coordinator's included, the contents running from -1 without
 * a hole.
 */
static void
cluster_check_cluster(const GpSegmentConfig *nodes)
{
	int			nsegments = count_segments(nodes, cluster_nnodes);

	for (int i = 0; i < cluster_nnodes; i++)
	{
		int			nprimaries = 0;

		if (nodes[i].dbid == 0)
			continue;
		for (int j = 0; j < cluster_nnodes; j++)
		{
			if (nodes[j].dbid == 0)
				continue;
			if (j != i && nodes[j].dbid == nodes[i].dbid)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("dbid %d would be in the cluster twice", nodes[i].dbid)));
			if (nodes[j].content == nodes[i].content && nodes[j].role == 'p')
				nprimaries++;
		}
		if (nprimaries != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("content %d would have %d primaries", nodes[i].content,
							nprimaries)));
	}
	for (int content = -1; content < nsegments; content++)
	{
		bool		found = false;

		for (int i = 0; i < cluster_nnodes && !found; i++)
			if (nodes[i].dbid != 0 && nodes[i].content == content)
				found = true;
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("content %d would have no node", content)));
	}
}

void
GpClusterReplaceNodes(const GpSegmentConfig *nodes)
{
	GpClusterSlot *saved = palloc_array(GpClusterSlot, cluster_nnodes);
	GpClusterNodeState *states = palloc_array(GpClusterNodeState, cluster_nnodes);
	int			nsegments;

	Assert(LWLockHeldByMeInMode(cluster_shared->lock, LW_EXCLUSIVE));
	for (int i = 0; i < cluster_nnodes; i++)
		if (nodes[i].dbid != 0)
			cluster_check_node(&nodes[i]);
	cluster_check_cluster(nodes);

	memcpy(saved, cluster_slots, cluster_nnodes * sizeof(GpClusterSlot));
	for (int i = 0; i < cluster_nnodes; i++)
	{
		GpClusterSlot *slot = &cluster_slots[i];

		memset(slot, 0, sizeof(*slot));
		if (nodes[i].dbid == 0)
		{
			slot->content = GP_CLUSTER_NO_CONTENT;
			states[i].role = '\0';
			states[i].mode = 'n';
			states[i].status = 'd';
			states[i].dbid = 0;
			continue;
		}
		slot->dbid = nodes[i].dbid;
		slot->content = nodes[i].content;
		slot->preferred_role = nodes[i].preferred_role;
		slot->port = nodes[i].port;
		strlcpy(slot->hostname, nodes[i].hostname, MAXPGPATH);
		strlcpy(slot->datadir, nodes[i].datadir, MAXPGPATH);
		states[i].role = nodes[i].role;
		states[i].mode = nodes[i].mode;
		states[i].status = nodes[i].status;
		states[i].dbid = nodes[i].dbid;
	}

	/* the file, then the dump, then shared memory: what is adopted is written */
	PG_TRY();
	{
		cluster_rewrite_file();
		if (cluster_self != NULL && cluster_self->content == -1)
			cluster_write_dump(states);
	}
	PG_CATCH();
	{
		memcpy(cluster_slots, saved, cluster_nnodes * sizeof(GpClusterSlot));
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * A segment added or removed: the number every backend takes at its next
	 * transaction, gpexpand's version bumped with it, as gp_expand_bump_version()
	 * bumps it, and every relation's cache invalidated as this transaction
	 * commits -- the plans cached for the old number, and ORCA's metadata of
	 * the tables, whose policies are read again.
	 */
	nsegments = count_segments(nodes, cluster_nnodes);
	if (nsegments != cluster_shared->nsegments)
		CacheInvalidateRelcacheAll();

	SpinLockAcquire(&cluster_shared->mutex);
	for (int i = 0; i < cluster_nnodes; i++)
		cluster_shared->nodes[i] = states[i];
	cluster_shared->nodes_version++;
	cluster_shared->version++;
	if (nsegments != cluster_shared->nsegments)
	{
		cluster_shared->nsegments = nsegments;
		cluster_shared->expand_version++;
	}
	SpinLockRelease(&cluster_shared->mutex);
	pfree(saved);
	pfree(states);
}

/* ------------------------------------------------------------------------- */
/* What the rest of the port asks                                            */
/* ------------------------------------------------------------------------- */

const GpSegmentConfig *
GpClusterSegments(int *nsegments)
{
	GpClusterDecideSegments();
	*nsegments = cluster_nsegments;
	return cluster_segments;
}

const GpSegmentConfig *
GpClusterSegmentByContent(int content)
{
	GpClusterDecideSegments();
	if (content < 0 || content >= cluster_nsegments)
		return NULL;
	return &cluster_segments[content];
}

const GpSegmentConfig *
GpClusterSelf(void)
{
	return cluster_self;
}

int
GpClusterNodes(const GpSegmentConfig **nodes)
{
	cluster_adopt_nodes();
	*nodes = cluster;
	return cluster_nnodes;
}

bool
GpClusterHasMirrors(void)
{
	cluster_adopt_nodes();
	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].content >= 0 && cluster[i].preferred_role == 'm')
			return true;
	return false;
}

const GpSegmentConfig *
GpClusterNodeByDbid(int dbid)
{
	if (dbid <= 0)
		return NULL;
	cluster_adopt_nodes();
	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].dbid == dbid)
			return &cluster[i];
	return NULL;
}

const GpSegmentConfig *
GpClusterCoordinator(void)
{
	cluster_adopt_nodes();
	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].content == -1 && cluster[i].role == 'p')
			return &cluster[i];
	return NULL;
}

int
GpClusterSessionId(void)
{
	const char *identity = GpClusterQeIdentity();
	const char *sess;

	/* a dispatched backend's is its coordinator backend's: "seg0/dbid1/sess42" */
	if (identity[0] != '\0' && (sess = strstr(identity, "/sess")) != NULL)
		return atoi(sess + strlen("/sess"));
	return MyProcPid;
}

/*
 * A segment process's coordinator backend's number of segments, from the
 * identity it was given: "seg0/dbid1/sess42/nseg3".  -1 in one given no
 * number -- the loopback's, a background process's connection -- which
 * computes with its node's.
 */
static int
qe_nsegments(void)
{
	if (cluster_qe_nsegments == -1)
	{
		const char *nseg = strstr(GpClusterQeIdentity(), "/nseg");

		cluster_qe_nsegments = 0;
		if (nseg != NULL)
			cluster_qe_nsegments = atoi(nseg + strlen("/nseg"));
	}
	return cluster_qe_nsegments;
}

int
GpClusterSegmentCount(void)
{
	/*
	 * A segment process computes with its coordinator's number, which is the
	 * one its node's file gives unless segments were added or removed since
	 * the node started, as Cloudberry's QE computes with the numsegments its
	 * QD sends with each statement (getgpsegmentCount()).
	 */
	if (GpClusterIsDispatched() && qe_nsegments() > 0)
		return qe_nsegments();

	/*
	 * One, not zero, when there are no segments.  This is how many segments to
	 * *compute with*, and consumers divide by it: ORCA asserts 0 < segments
	 * when it builds its cost model and computes 1.0 / segments in its skew
	 * model.  Cloudberry answers 1 here for the same reason, and says so: "1
	 * represents a singleton postgresql in utility mode".
	 */
	GpClusterDecideSegments();
	return cluster_nsegments > 0 ? cluster_nsegments : 1;
}

int
GpClusterSegmentCountNow(void)
{
	int			nsegments;

	if (GpClusterIsDispatched() || cluster_shared == NULL ||
		cluster_self == NULL || cluster_self->content != -1)
		return GpClusterSegmentCount();
	SpinLockAcquire(&cluster_shared->mutex);
	nsegments = cluster_shared->nsegments;
	SpinLockRelease(&cluster_shared->mutex);
	return nsegments > 0 ? nsegments : 1;
}

bool
GpClusterIsSingleNode(void)
{
	return cluster_nsegments == 0;
}

int
GpClusterContentId(void)
{
	return cluster_self != NULL ? cluster_self->content : -1;
}

int
GpClusterDbid(void)
{
	return gp_dbid;
}

bool
GpClusterIsDispatched(void)
{
	return gp_qe_identity != NULL && gp_qe_identity[0] != '\0';
}

const char *
GpClusterQeIdentity(void)
{
	return gp_qe_identity != NULL ? gp_qe_identity : "";
}

int
GpClusterBackendRole(void)
{
	/*
	 * A backend the dispatcher started is an executor, whatever the node is.
	 * That is how a segment tells a dispatched connection from a psql somebody
	 * opened on it -- which is a utility session, as it is in Cloudberry.
	 */
	if (GpClusterIsDispatched())
		return GP_ROLE_EXECUTE;

	if (cluster_self != NULL)
		return cluster_self->content == -1 ? GP_ROLE_DISPATCH : GP_ROLE_UTILITY;

	/* No cluster: this server is whatever it was told it is. */
	return gp_role_setting;
}

/*
 * Is the connection this backend serves the coordinator's own?
 *
 * gp.qe_identity says a connection is a dispatched one, and anybody who can
 * connect to a segment can say so: it is a startup setting.  For SQL that is
 * no matter -- a segment parses, analyzes and checks it as it would anyone's
 * -- but a plan is carried out as it stands, its permission checks included,
 * so a segment runs one only from a connection that also carries the cluster
 * secret every node is given.  Compared in constant time.
 */
bool
GpClusterDispatchTrusted(void)
{
	size_t		len;

	if (!GpClusterIsDispatched() || !GpClusterHasSecret())
		return false;
	if (gp_qe_secret == NULL)
		return false;
	len = strlen(gp_cluster_secret);
	if (strlen(gp_qe_secret) != len)
		return false;
	return timingsafe_bcmp(gp_qe_secret, gp_cluster_secret, len) == 0;
}

bool
GpClusterHasSecret(void)
{
	return gp_cluster_secret != NULL && gp_cluster_secret[0] != '\0';
}

const char *
GpClusterSecret(void)
{
	return GpClusterHasSecret() ? gp_cluster_secret : NULL;
}

/*
 * The secret travels in libpq's "options", which splits on whitespace and
 * treats a backslash as an escape, so it is kept to characters that need
 * neither: those of base64 and of a URL-safe token.
 */
static bool
check_cluster_secret(char **newval, void **extra, GucSource source)
{
	const char *p;

	if (*newval == NULL || (*newval)[0] == '\0')
		return true;

	for (p = *newval; *p; p++)
	{
		if (!isalnum((unsigned char) *p) && strchr("+/=._~-", *p) == NULL)
		{
			GUC_check_errdetail("The secret may hold letters, digits and \"+/=._~-\" only.");
			return false;
		}
	}
	if (p - *newval < GP_CLUSTER_SECRET_MIN)
	{
		GUC_check_errdetail("The secret must be at least %d characters long.",
							GP_CLUSTER_SECRET_MIN);
		return false;
	}
	return true;
}

/*
 * gp.session_id: Cloudberry's gp_session_id, read-only.  The session a
 * backend works for, which every process of one coordinator session shares
 * -- the coordinator backend's process id (GpClusterSessionId), for it and
 * for each segment process it dispatched to -- and -1 in a session of a
 * segment's own and on a node with no cluster, as Cloudberry's utility mode
 * says: what such a session holds is no dispatched query's.
 */
static const char *
show_session_id(void)
{
	static char buf[16];
	int			id = -1;

	if (GpClusterIsDispatched() || GpClusterBackendRole() == GP_ROLE_DISPATCH)
		id = GpClusterSessionId();
	snprintf(buf, sizeof(buf), "%d", id);
	return buf;
}

/*
 * gp.contentid: Cloudberry's gp_contentid, read-only.  Which segment this
 * node is, by its line of the cluster's file: -1 for the coordinator and on
 * a node with no cluster.
 */
static const char *
show_contentid(void)
{
	static char buf[16];

	snprintf(buf, sizeof(buf), "%d", GpClusterContentId());
	return buf;
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

void
GpClusterInit(void)
{
	/*
	 * Settings are named "gp.*".  An undotted name that PostgreSQL does not
	 * know is an error when it reads postgresql.conf, and that file is read
	 * before shared_preload_libraries is loaded; a dotted name becomes a
	 * placeholder instead and is picked up when we define it here.  So every
	 * setting that may end up in a file has to be dotted, whatever its
	 * context.
	 */
	DefineCustomEnumVariable("gp.role",
							 "Role this node plays in the cluster.",
							 "\"dispatch\" is the coordinator, \"execute\" a segment, "
							 "\"utility\" a node used on its own.  With a cluster "
							 "configured it is read from the file, and a value here "
							 "that disagrees with it is an error.",
							 &gp_role_setting,
							 GP_ROLE_UTILITY,
							 gp_role_options,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("gp.session_id",
							"Session this backend works for, as Cloudberry's gp_session_id.",
							"The coordinator backend's process id, on it and on every "
							"segment process it dispatched to; -1 in a session of a "
							"segment's own.",
							&gp_session_id_shown,
							-1, -1, INT_MAX,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							NULL, NULL, show_session_id);

	DefineCustomStringVariable("gp.qe_identity",
							   "Identity the dispatcher gave this segment process.",
							   "Set by the coordinator on the connection that starts a "
							   "segment process; empty in every other backend.",
							   &gp_qe_identity,
							   "",
							   PGC_BACKEND,
							   0,
							   NULL, NULL, NULL);

	/*
	 * Neither can be read by an ordinary user, on any node: a function a
	 * query runs on a segment runs in the dispatched backend, where
	 * gp.qe_secret is set.
	 */
	DefineCustomStringVariable("gp.cluster_secret",
							   "Secret the nodes of this cluster share.",
							   "The coordinator gives it to every segment process "
							   "it starts, and a segment carries out a plan only "
							   "from a connection that has it.  The same on every "
							   "node; empty, and plans are not dispatched.",
							   &gp_cluster_secret,
							   "",
							   PGC_SIGHUP,
							   GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   check_cluster_secret, NULL, NULL);

	DefineCustomStringVariable("gp.qe_secret",
							   "Cluster secret the dispatcher gave this segment process.",
							   NULL,
							   &gp_qe_secret,
							   "",
							   PGC_BACKEND,
							   GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("gp.cluster_config",
							   "File that lists the nodes of this cluster.",
							   "One node per line: dbid, content id, role (\"p\" or "
							   "\"m\"), host, port, data directory.  Empty on a server "
							   "that has no segments.",
							   &gp_cluster_config,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("gp.dbid",
							"Which node of the cluster this server is.",
							"The dbid of this node's line in \"gp.cluster_config\".  "
							"The coordinator keeps 1.",
							&gp_dbid,
							1,
							1, INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.contentid",
							"Segment this server is, as Cloudberry's gp_contentid.",
							"The content id of this node's line in \"gp.cluster_config\": "
							"-1 for the coordinator, and on a server that has no segments.",
							&gp_contentid_shown,
							-1, -1, INT_MAX,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							NULL, NULL, show_contentid);

	/*
	 * The port's own: Cloudberry's catalog grows as it is written, and the
	 * port's shared memory is made as the server starts.
	 */
	DefineCustomIntVariable("gp.max_segments",
							"How many segments the cluster may grow to while it runs.",
							"gpexpand adds segments to a running cluster up to this "
							"many; each costs some 4 kB of shared memory on every "
							"node.  A cluster whose file lists more has room for "
							"those.",
							&gp_max_segments,
							64,
							1, PG_INT16_MAX,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	if (gp_cluster_config != NULL && gp_cluster_config[0] != '\0')
	{
		gp_cluster_read_file(gp_cluster_config);

		/* The live copy of what FTS finds; see cluster_shmem_startup(). */
		prev_shmem_request = shmem_request_hook;
		shmem_request_hook = cluster_shmem_request;
		prev_shmem_startup = shmem_startup_hook;
		shmem_startup_hook = cluster_shmem_startup;
	}
	else if (gp_role_setting == GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("\"gp.role\" is \"dispatch\" but no cluster is configured"),
				 errhint("Set \"gp.cluster_config\" to the file that lists this cluster's nodes.")));
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_segment_configuration);

/*
 * gp.segment_configuration()
 *		The cluster, as this node knows it.
 *
 * Cloudberry's is a shared catalog anyone can select from, and its own tools
 * read it constantly.  This is the file's rows, with the role the file gives
 * each node; gp_segment_configuration has the one it has now, with FTS's
 * mode and status.
 */
Datum
gp_segment_configuration(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	cluster_adopt_nodes();
	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpSegmentConfig *node = &cluster[i];
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		char		role[2];

		if (node->dbid == 0)
			continue;

		role[0] = node->preferred_role;
		role[1] = '\0';

		values[0] = Int32GetDatum(node->dbid);
		values[1] = Int32GetDatum(node->content);
		values[2] = CStringGetTextDatum(role);
		values[3] = CStringGetTextDatum(node->hostname);
		values[4] = Int32GetDatum(node->port);
		values[5] = CStringGetTextDatum(node->datadir);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/* One row of gp_segment_configuration, in Cloudberry's columns. */
static void
put_catalog_row(ReturnSetInfo *rsinfo, int dbid, int content, char role,
				char preferred_role, char mode, char status,
				int port, const char *host, const char *datadir)
{
	Datum		values[11];
	bool		nulls[11] = {0};

	values[0] = Int16GetDatum(dbid);
	values[1] = Int16GetDatum(content);
	values[2] = CharGetDatum(role);
	values[3] = CharGetDatum(preferred_role);
	values[4] = CharGetDatum(mode);
	values[5] = CharGetDatum(status);
	values[6] = Int32GetDatum(port);
	values[7] = CStringGetTextDatum(host);
	values[8] = CStringGetTextDatum(host);	/* address */
	values[9] = CStringGetTextDatum(datadir);
	values[10] = ObjectIdGetDatum(InvalidOid);	/* warehouseid */

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

PG_FUNCTION_INFO_V1(gp_catalog_segment_configuration);

/*
 * gp_internal.segment_configuration()
 *		The rows of Cloudberry's gp_segment_configuration, which the view of
 *		that name selects.
 *
 * What the file lists, in Cloudberry's columns and types, with what FTS last
 * published of each node (gp_fts.c): the role it has now -- a mirror FTS
 * promoted is a primary, and the primary it failed over from a mirror, down
 * -- and whether a primary and its mirror are in sync.  Until FTS says
 * otherwise each node has the role the file prefers, is up ('u'), and is not
 * known to be in sync ('n'), which is what Cloudberry says of a primary
 * without a mirror.  The address is the host, as gpinitsystem writes it when
 * it is given no other, and the warehouse is none.  With no cluster
 * configured, the node itself, as Cloudberry's single-node mode lists it.
 * The nodes this session's transaction has changed and not yet committed
 * are as it changed them (gp_segadmin.c), as Cloudberry's catalog shows a
 * transaction its own rows.
 */
Datum
gp_catalog_segment_configuration(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	GpClusterNodeState *states;
	GpSegmentConfig *nodes;

	InitMaterializedSRF(fcinfo, 0);

	if (cluster_nnodes == 0)
	{
		char		host[256];

		if (gethostname(host, sizeof(host)) != 0)
			strlcpy(host, "localhost", sizeof(host));
		host[sizeof(host) - 1] = '\0';
		put_catalog_row(rsinfo, gp_dbid, -1, 'p', 'p', 'n', 'u',
						PostPortNumber, host, DataDir);
		return (Datum) 0;
	}

	cluster_adopt_nodes();
	states = palloc_array(GpClusterNodeState, cluster_nnodes);
	nodes = palloc_array(GpSegmentConfig, cluster_nnodes);
	(void) GpClusterLiveStates(states);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		nodes[i] = cluster[i];
		/* a node that changed since it was adopted: an empty place for now */
		if (states[i].dbid != cluster[i].dbid)
			cluster_empty_place(&nodes[i]);
		else
		{
			nodes[i].role = states[i].role;
			nodes[i].mode = states[i].mode;
			nodes[i].status = states[i].status;
		}
	}
	GpSegadminOverlay(nodes, cluster_nnodes);
	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpSegmentConfig *node = &nodes[i];

		if (node->dbid == 0)
			continue;
		put_catalog_row(rsinfo, node->dbid, node->content, node->role,
						node->preferred_role, node->mode, node->status,
						node->port, node->hostname, node->datadir);
	}

	return (Datum) 0;
}
