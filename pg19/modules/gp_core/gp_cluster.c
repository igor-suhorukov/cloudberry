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
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/cdbutil.c (the readGpSegConfig half), and
 *	  src/include/catalog/gp_segment_configuration.h
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_core_api.h"

/* The longest line the configuration file may hold. */
#define GP_CLUSTER_LINE_MAX		4096

/* Settings, all of them "gp.*" because a file may hold them; see gp_core.c. */
static char *gp_cluster_config = NULL;
static int	gp_dbid = 1;
static int	gp_role_setting = GP_ROLE_UTILITY;
static char *gp_qe_identity = NULL;
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
 * The cluster, as the file described it.  Read once, in the postmaster, and
 * inherited by every backend; under EXEC_BACKEND the library is loaded again
 * in the child, which reads it again.  Nothing writes it afterwards.
 */
static GpSegmentConfig *cluster = NULL;
static int	cluster_nnodes = 0;

/* The primaries with content >= 0, in content order: a slice of the above. */
static GpSegmentConfig *cluster_segments = NULL;
static int	cluster_nsegments = 0;

/* This node's entry in it. */
static const GpSegmentConfig *cluster_self = NULL;

static void gp_cluster_read_file(const char *path);

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
 * Read the file into "cluster", check that what it describes could be a
 * cluster, and find this node in it.
 *
 * Every complaint names the file and the line, because this runs in the
 * postmaster while it is starting: the message is all the operator gets.
 */
static void
gp_cluster_read_file(const char *path)
{
	FILE	   *fp;
	char		buf[GP_CLUSTER_LINE_MAX];
	int			lineno = 0;
	int			nnodes = 0;
	int			nalloc = 0;
	int			ncoordinators = 0;
	GpSegmentConfig *nodes;
	MemoryContext oldcxt;

	fp = AllocateFile(path, "r");
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open cluster configuration file \"%s\": %m",
						path),
				 errhint("\"gp.cluster_config\" names the file that lists this cluster's nodes.")));

	/*
	 * Read into the postmaster's own context, so that what is parsed here
	 * outlives this function and every backend forked afterwards inherits it.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
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

	/*
	 * Collect the primaries.  The content ids have to run 0..n-1 without a
	 * hole, because everything downstream -- the hash that picks a segment,
	 * the gang that connects to them all -- indexes by content id.
	 */
	cluster_nsegments = 0;
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].content >= 0 && nodes[i].role == 'p')
			cluster_nsegments++;

	cluster_segments = cluster_nsegments > 0
		? (GpSegmentConfig *) palloc0_array(GpSegmentConfig, cluster_nsegments)
		: NULL;

	for (int i = 0; i < nnodes; i++)
	{
		if (nodes[i].content >= 0 && nodes[i].role == 'p')
		{
			if (nodes[i].content >= cluster_nsegments)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("cluster configuration file \"%s\" has %d segments, so content id %d is out of range",
								path, cluster_nsegments, nodes[i].content),
						 errdetail("The content ids of the primaries run from 0 to one less than their number.")));
			cluster_segments[nodes[i].content] = nodes[i];
		}
	}

	cluster = nodes;
	cluster_nnodes = nnodes;
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
}

/* ------------------------------------------------------------------------- */
/* What the rest of the port asks                                            */
/* ------------------------------------------------------------------------- */

const GpSegmentConfig *
GpClusterSegments(int *nsegments)
{
	*nsegments = cluster_nsegments;
	return cluster_segments;
}

const GpSegmentConfig *
GpClusterSegmentByContent(int content)
{
	if (content < 0 || content >= cluster_nsegments)
		return NULL;
	return &cluster_segments[content];
}

const GpSegmentConfig *
GpClusterSelf(void)
{
	return cluster_self;
}

const GpSegmentConfig *
GpClusterNodeByDbid(int dbid)
{
	for (int i = 0; i < cluster_nnodes; i++)
		if (cluster[i].dbid == dbid)
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

int
GpClusterSegmentCount(void)
{
	/*
	 * One, not zero, when there are no segments.  This is how many segments to
	 * *compute with*, and consumers divide by it: ORCA asserts 0 < segments
	 * when it builds its cost model and computes 1.0 / segments in its skew
	 * model.  Cloudberry answers 1 here for the same reason, and says so: "1
	 * represents a singleton postgresql in utility mode".
	 */
	return cluster_nsegments > 0 ? cluster_nsegments : 1;
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

	if (gp_cluster_config != NULL && gp_cluster_config[0] != '\0')
		gp_cluster_read_file(gp_cluster_config);
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
 * read it constantly.  This is the same rows from the file; the columns are
 * the ones that exist yet, and mode and status join them with FTS, which is
 * what would maintain them.
 */
Datum
gp_segment_configuration(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpSegmentConfig *node = &cluster[i];
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		char		role[2];

		role[0] = node->role;
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
				int port, const char *host, const char *datadir)
{
	Datum		values[11];
	bool		nulls[11] = {0};

	values[0] = Int16GetDatum(dbid);
	values[1] = Int16GetDatum(content);
	values[2] = CharGetDatum(role);
	values[3] = CharGetDatum(role);		/* preferred_role */
	values[4] = CharGetDatum('n');		/* mode: no mirror in sync with it */
	values[5] = CharGetDatum('u');		/* status: up */
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
 * What the file lists, in Cloudberry's columns and types.  Mode and status
 * are what FTS keeps, and FTS is M4's: until then every node is up ('u') and
 * has no mirror in sync with it ('n'), which is what Cloudberry says of a
 * primary without one; each node's preferred role is the role it has.  The
 * address is the host, as gpinitsystem writes it when it is given no other,
 * and the warehouse is none.  With no cluster configured, the node itself,
 * as Cloudberry's single-node mode lists it.
 */
Datum
gp_catalog_segment_configuration(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	if (cluster_nnodes == 0)
	{
		char		host[256];

		if (gethostname(host, sizeof(host)) != 0)
			strlcpy(host, "localhost", sizeof(host));
		host[sizeof(host) - 1] = '\0';
		put_catalog_row(rsinfo, gp_dbid, -1, 'p', PostPortNumber, host,
						DataDir);
		return (Datum) 0;
	}

	for (int i = 0; i < cluster_nnodes; i++)
	{
		const GpSegmentConfig *node = &cluster[i];

		put_catalog_row(rsinfo, node->dbid, node->content, node->role,
						node->port, node->hostname, node->datadir);
	}

	return (Datum) 0;
}
