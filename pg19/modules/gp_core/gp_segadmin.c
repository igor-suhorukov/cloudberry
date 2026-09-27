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
 * gp_segadmin.c
 *	  Cloudberry's segment administration functions: a mirror or a standby
 *	  added, removed, or put somewhere else, a standby activated, and a
 *	  segment added or removed.
 *
 * Cloudberry's tools change the cluster's nodes with these -- gpinitstandby
 * adds and removes the standby, gpaddmirrors adds mirrors, gprecoverseg puts
 * a failed node somewhere else by removing it and adding it again under its
 * dbid, gpexpand adds segments and gpshrink removes them -- each a row of
 * gp_segment_configuration, a catalog there, several
 * of them in one transaction, which a rollback undoes.  The port's nodes are
 * the file gp.cluster_config names, and on the coordinator shared memory and
 * gpsegconfig_dump beside it (gp_cluster.c).  So a call here checks what it
 * is asked against the nodes as the transaction sees them -- its own changes
 * made -- and keeps the change for the transaction: the session's
 * gp_segment_configuration shows it, a rollback or a savepoint rolled back
 * drops it, and the transaction's last step before it commits writes them all
 * at once: the file first, so that a node started afterwards -- the one
 * added -- reads it, then gpsegconfig_dump and shared memory.  A lock of the
 * whole cluster, taken by the first call and held until the transaction
 * ends, keeps two tools' transactions from changing the nodes at once, as
 * Cloudberry's AccessExclusiveLock on gp_segment_configuration does.  The
 * names, the arguments and the checks are Cloudberry's
 * (src/backend/utils/gp/segadmin.c), and so are the messages where it has
 * one.
 *
 * A segment is a primary for a content after the last, added with its
 * mirror in one transaction, as gpexpand adds it, and one of the last is
 * removed, its mirror first, as gpshrink removes it: the contents run from 0
 * without a hole, which the commit checks (gp_cluster.c), and each session
 * takes their new number as its next transaction begins (gp_expand.c).
 *
 * What differs.  The coordinator has no utility mode, so the functions
 * Cloudberry runs only in one run in any session of the coordinator's.  A
 * node has one host, which is also its address: the one given as the
 * address is kept.  A primary is added to a content that has none, and one
 * is removed only with its segment, once its mirror is, where Cloudberry's
 * catalog takes any row, gpexpand's and gpshrink's being these.  And a standby promoted
 * with pg_ctl changes no node: gp_activate_standby(), which Cloudberry's
 * startup process calls as it promotes one, is called by the tool that
 * activates it, afterwards -- on the port a node's file may be every node's,
 * as the harness's is, where the old coordinator may still run.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "gp_cluster.h"
#include "gp_segadmin.h"

/*
 * The lock of the cluster's nodes, an advisory lock of no database -- which
 * no user's advisory lock is -- of a kind of its own.
 */
#define SEGADMIN_LOCK_KEY1	0x67700000	/* "gp" */
#define SEGADMIN_LOCK_KEY2	0x73656761	/* "sega" */
#define SEGADMIN_LOCK_KIND	3

/*
 * A change this transaction made and has not committed: a node removed, or
 * set -- added, or changed in its place -- made at a subtransaction's level,
 * which its rollback drops.  In TopTransactionContext.
 */
typedef struct SegadminChange
{
	int			level;
	bool		removal;
	GpSegmentConfig node;
} SegadminChange;

static List *changes = NIL;

/*
 * Cloudberry's mirroring_sanity_check(), for what the port has of it: a
 * superuser, on the coordinator -- the file's or a standby promoted since --
 * not a standby that is one still.
 */
static void
segadmin_check(const char *func)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s can only be run by a superuser", func)));
	if (GpClusterIsSingleNode() || GpClusterContentId() != -1 ||
		RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s must be run on the master", func)));
}

static char *
text_arg(FunctionCallInfo fcinfo, int n, const char *what)
{
	if (PG_ARGISNULL(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s cannot be NULL", what)));
	return TextDatumGetCString(PG_GETARG_DATUM(n));
}

static int
int_arg(FunctionCallInfo fcinfo, int n, bool int16arg, const char *what)
{
	if (PG_ARGISNULL(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s cannot be NULL", what)));
	return int16arg ? PG_GETARG_INT16(n) : PG_GETARG_INT32(n);
}

static char
char_arg(FunctionCallInfo fcinfo, int n, const char *what)
{
	if (PG_ARGISNULL(n))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s cannot be NULL", what)));
	return PG_GETARG_CHAR(n);
}

/* ------------------------------------------------------------------------- */
/* The transaction's changes                                                 */
/* ------------------------------------------------------------------------- */

/* The place of that dbid among the nodes, or -1. */
static int
place_of(const GpSegmentConfig *nodes, int nnodes, int dbid)
{
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].dbid != 0 && nodes[i].dbid == dbid)
			return i;
	return -1;
}

static void
empty_place(GpSegmentConfig *node)
{
	memset(node, 0, sizeof(*node));
	node->content = -2;
	node->mode = 'n';
	node->status = 'd';
	node->hostname = "";
	node->datadir = "";
}

void
GpSegadminOverlay(GpSegmentConfig *nodes, int nnodes)
{
	foreach_ptr(SegadminChange, change, changes)
	{
		int			place = place_of(nodes, nnodes, change->node.dbid);

		if (change->removal)
		{
			if (place >= 0)
				empty_place(&nodes[place]);
			continue;
		}
		for (int i = 0; i < nnodes && place < 0; i++)
			if (nodes[i].dbid == 0)
				place = i;
		if (place < 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the cluster has no room for another node"),
					 errdetail("There is room for a primary and a mirror of each content the cluster may grow to, and the coordinator and a standby."),
					 errhint("\"gp.max_segments\" says how many segments a cluster may grow to while it runs; it is read as the coordinator starts.")));
		nodes[place] = change->node;
	}
}

/*
 * The first call of a transaction takes the lock of the cluster's nodes, as
 * Cloudberry's takes its catalog's; then the nodes as this transaction sees
 * them.
 */
static int
segadmin_nodes(GpSegmentConfig **nodes)
{
	LOCKTAG		tag;
	int			nnodes;

	SET_LOCKTAG_ADVISORY(tag, InvalidOid, SEGADMIN_LOCK_KEY1,
						 SEGADMIN_LOCK_KEY2, SEGADMIN_LOCK_KIND);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	GpClusterLockNodes();
	nnodes = GpClusterLiveNodes(nodes);
	GpClusterUnlockNodes();
	GpSegadminOverlay(*nodes, nnodes);
	return nnodes;
}

/* A change kept for the transaction. */
static void
segadmin_change(const GpSegmentConfig *node, bool removal)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	SegadminChange *change = palloc0_object(SegadminChange);

	change->level = GetCurrentTransactionNestLevel();
	change->removal = removal;
	change->node = *node;
	change->node.hostname = pstrdup(node->hostname != NULL ? node->hostname : "");
	change->node.datadir = pstrdup(node->datadir != NULL ? node->datadir : "");
	changes = lappend(changes, change);
	MemoryContextSwitchTo(old);
}

/*
 * The transaction's last step before it commits: its changes made to the
 * nodes as they are now -- a mirror removed that FTS has made a primary since
 * is refused, and so the commit -- all at once.
 */
static void
segadmin_commit(void)
{
	GpSegmentConfig *nodes;
	int			nnodes;

	GpClusterLockNodes();
	nnodes = GpClusterLiveNodes(&nodes);
	foreach_ptr(SegadminChange, change, changes)
	{
		int			place = place_of(nodes, nnodes, change->node.dbid);

		if (change->removal && change->node.role != 'p' && place >= 0 &&
			nodes[place].role == 'p' && nodes[place].content >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("dbid %d has become the primary of content %d since it was removed",
							change->node.dbid, nodes[place].content)));
	}
	GpSegadminOverlay(nodes, nnodes);
	GpClusterReplaceNodes(nodes);
	GpClusterUnlockNodes();
	changes = NIL;
}

static void
segadmin_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (changes != NIL)
				segadmin_commit();
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (changes != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has changed the cluster's nodes")));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			changes = NIL;		/* the memory goes with the transaction */
			break;
		default:
			break;
	}
}

static void
segadmin_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	List	   *kept = NIL;

	if (event != SUBXACT_EVENT_ABORT_SUB || changes == NIL)
		return;
	foreach_ptr(SegadminChange, change, changes)
		if (change->level < level)
			kept = lappend(kept, change);
	changes = kept;
}

void
GpSegadminInit(void)
{
	/*
	 * Registered before every other callback of gp_core's and of the modules
	 * loaded after it, so that the changes are written after all of theirs
	 * have run at PRE_COMMIT: the last thing before the commit.
	 */
	RegisterXactCallback(segadmin_xact_callback, NULL);
	RegisterSubXactCallback(segadmin_subxact_callback, NULL);
}

/* ------------------------------------------------------------------------- */
/* The functions                                                             */
/* ------------------------------------------------------------------------- */

/* The node of that content and role now, as this transaction sees it, or NULL. */
static GpSegmentConfig *
node_of(GpSegmentConfig *nodes, int nnodes, int content, char role)
{
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].dbid != 0 && nodes[i].content == content &&
			nodes[i].role == role)
			return &nodes[i];
	return NULL;
}

/*
 * Cloudberry's add_segment(), less the catalog: a mirror goes where its
 * content has a primary and no mirror, and one whose content has no
 * preferred primary is made the preferred one, for a rebalance to go back
 * to.  A primary goes to a segment's content that has none, a new one --
 * after the last, which the commit checks.
 */
static void
add_node(GpSegmentConfig *nodes, int nnodes, GpSegmentConfig *node)
{
	if (place_of(nodes, nnodes, node->dbid) >= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("dbid %d is already in the cluster", node->dbid)));

	if (node->role == 'm')
	{
		bool		preferred_primary = false;

		if (node_of(nodes, nnodes, node->content, 'p') == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("contentid %i does not point to an existing segment",
							node->content)));
		if (node_of(nodes, nnodes, node->content, 'm') != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("segment already has a mirror defined")));
		for (int i = 0; i < nnodes; i++)
			if (nodes[i].dbid != 0 && nodes[i].content == node->content &&
				nodes[i].preferred_role == 'p')
				preferred_primary = true;
		if (!preferred_primary && node->preferred_role == 'm')
		{
			ereport(NOTICE,
					(errmsg("override preferred_role of this mirror as primary to support rebalance operation.")));
			node->preferred_role = 'p';
		}
	}
	else if (node->content < 0 ||
			 node_of(nodes, nnodes, node->content, 'p') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("content %d has a primary already", node->content),
				 errdetail("A primary is added for a new segment, whose content is after the last.")));

	segadmin_change(node, false);
}

PG_FUNCTION_INFO_V1(gp_add_segment_primary);

/*
 * gp_add_segment_primary(hostname, address, port, datadir)
 *		A new segment: its primary, of the content after the last, under the
 *		least dbid not in use, up and not in sync.
 */
Datum
gp_add_segment_primary(PG_FUNCTION_ARGS)
{
	GpSegmentConfig node;
	GpSegmentConfig *nodes;
	int			nnodes;

	memset(&node, 0, sizeof(node));
	(void) text_arg(fcinfo, 0, "hostname");
	node.hostname = text_arg(fcinfo, 1, "address");
	node.port = int_arg(fcinfo, 2, false, "port");
	node.datadir = text_arg(fcinfo, 3, "datadir");

	segadmin_check("gp_add_segment_primary");

	nnodes = segadmin_nodes(&nodes);
	node.content = 0;
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].dbid != 0 && nodes[i].content >= node.content)
			node.content = nodes[i].content + 1;
	for (node.dbid = 1; place_of(nodes, nnodes, node.dbid) >= 0; node.dbid++)
		;
	if (node.dbid > PG_INT16_MAX)
		ereport(ERROR,
				(errmsg("unable to find available dbid")));
	node.role = 'p';
	node.preferred_role = 'p';
	node.mode = 'n';
	node.status = 'u';
	add_node(nodes, nnodes, &node);

	PG_RETURN_INT16(node.dbid);
}

PG_FUNCTION_INFO_V1(gp_add_segment);

/*
 * gp_add_segment(dbid, content, role, preferred_role, mode, status, port,
 *				  hostname, address, datadir)
 *		A node of that dbid, as gpMgmt adds one it has made itself: a mirror
 *		recovered elsewhere under its old dbid.  The mode is "not in sync",
 *		whatever it is given, as Cloudberry's is.
 */
Datum
gp_add_segment(PG_FUNCTION_ARGS)
{
	GpSegmentConfig node;
	GpSegmentConfig *nodes;
	int			nnodes;

	memset(&node, 0, sizeof(node));
	node.dbid = int_arg(fcinfo, 0, true, "dbid");
	node.content = int_arg(fcinfo, 1, true, "content");
	node.role = char_arg(fcinfo, 2, "role");
	node.preferred_role = char_arg(fcinfo, 3, "preferred_role");
	node.mode = char_arg(fcinfo, 4, "mode");
	node.status = char_arg(fcinfo, 5, "status");
	node.port = int_arg(fcinfo, 6, false, "port");
	(void) text_arg(fcinfo, 7, "hostname");
	node.hostname = text_arg(fcinfo, 8, "address");
	node.datadir = text_arg(fcinfo, 9, "datadir");

	segadmin_check("gp_add_segment");

	node.mode = 'n';
	ereport(NOTICE,
			(errmsg("mode is changed to GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC under walrep.")));

	nnodes = segadmin_nodes(&nodes);
	add_node(nodes, nnodes, &node);

	PG_RETURN_INT16(node.dbid);
}

/*
 * Is the node a segment's primary whose mirror, if it had one, is gone:
 * removing it removes the segment, which the commit allows of the last
 * segments alone, gpshrink's (cluster_check_cluster(), gp_cluster.c)?
 */
static bool
removes_segment(GpSegmentConfig *nodes, int nnodes, const GpSegmentConfig *node)
{
	return node->content >= 0 &&
		node_of(nodes, nnodes, node->content, 'm') == NULL;
}

PG_FUNCTION_INFO_V1(gp_remove_segment);

/*
 * gp_remove_segment(dbid)
 *		The node of that dbid, gone.  Not the coordinator, nor a content's
 *		primary now, without which the content would have none -- but a
 *		primary whose mirror is gone, which removes its segment, one of the
 *		last.
 */
Datum
gp_remove_segment(PG_FUNCTION_ARGS)
{
	int			dbid = int_arg(fcinfo, 0, true, "dbid");
	GpSegmentConfig *nodes;
	int			nnodes;
	int			place;

	segadmin_check("gp_remove_segment");

	nnodes = segadmin_nodes(&nodes);
	place = place_of(nodes, nnodes, dbid);
	if (place < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find configuration entry for dbid %i", dbid)));
	if (nodes[place].role == 'p' && !removes_segment(nodes, nnodes, &nodes[place]))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("dbid %d is the primary of content %d", dbid,
						nodes[place].content),
				 errdetail("A content's primary is removed only once its mirror has been made the primary, or with its segment, once its mirror is removed.")));
	segadmin_change(&nodes[place], true);

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(gp_add_segment_mirror);

/*
 * gp_add_segment_mirror(contentid, hostname, address, port, datadir)
 *		A mirror of that content, under the least dbid not in use, down and
 *		not in sync until FTS finds its primary streaming to it.
 */
Datum
gp_add_segment_mirror(PG_FUNCTION_ARGS)
{
	GpSegmentConfig node;
	GpSegmentConfig *nodes;
	int			nnodes;

	memset(&node, 0, sizeof(node));
	node.content = int_arg(fcinfo, 0, true, "contentid");
	(void) text_arg(fcinfo, 1, "hostname");
	node.hostname = text_arg(fcinfo, 2, "address");
	node.port = int_arg(fcinfo, 3, false, "port");
	node.datadir = text_arg(fcinfo, 4, "datadir");

	segadmin_check("gp_add_segment_mirror");

	nnodes = segadmin_nodes(&nodes);
	for (node.dbid = 1; place_of(nodes, nnodes, node.dbid) >= 0; node.dbid++)
		;
	if (node.dbid > PG_INT16_MAX)
		ereport(ERROR,
				(errmsg("unable to find available dbid")));
	node.role = 'm';
	node.preferred_role = 'm';
	node.mode = 'n';
	node.status = 'd';
	add_node(nodes, nnodes, &node);

	PG_RETURN_INT16(node.dbid);
}

PG_FUNCTION_INFO_V1(gp_remove_segment_mirror);

/*
 * gp_remove_segment_mirror(contentid)
 *		That content's mirror now, gone.
 */
Datum
gp_remove_segment_mirror(PG_FUNCTION_ARGS)
{
	int			content = int_arg(fcinfo, 0, true, "dbid");
	GpSegmentConfig *nodes;
	GpSegmentConfig *mirror;
	int			nnodes;

	segadmin_check("gp_remove_segment_mirror");

	nnodes = segadmin_nodes(&nodes);
	if (node_of(nodes, nnodes, content, 'p') == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no dbid for contentid %i", content)));
	mirror = node_of(nodes, nnodes, content, 'm');
	if (mirror == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("segment does not have a mirror")));
	segadmin_change(mirror, true);

	PG_RETURN_BOOL(true);
}

/*
 * The standby, under the dbid after the greatest: the coordinator's, but
 * where it is given, in sync and up, as Cloudberry adds it -- FTS never
 * probes the coordinator's pair, and its commits wait for the standby once it
 * streams (gp_standby.c).
 */
static Datum
add_standby(FunctionCallInfo fcinfo)
{
	GpSegmentConfig node;
	GpSegmentConfig *nodes;
	GpSegmentConfig *coordinator;
	int			nnodes;
	int			maxdbid = 0;

	memset(&node, 0, sizeof(node));
	(void) text_arg(fcinfo, 0, "host name");
	node.hostname = text_arg(fcinfo, 1, "address");
	node.datadir = text_arg(fcinfo, 2, "datadir");

	segadmin_check("gp_add_master_standby");

	nnodes = segadmin_nodes(&nodes);
	if (node_of(nodes, nnodes, -1, 'm') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("only a single master standby may be defined")));
	coordinator = node_of(nodes, nnodes, -1, 'p');
	for (int i = 0; i < nnodes; i++)
		maxdbid = Max(maxdbid, nodes[i].dbid);

	node.dbid = maxdbid + 1;
	node.content = -1;
	node.role = 'm';
	node.preferred_role = 'm';
	node.mode = 's';
	node.status = 'u';
	node.port = coordinator != NULL ? coordinator->port : 0;
	if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
		node.port = PG_GETARG_INT32(3);
	segadmin_change(&node, false);

	PG_RETURN_INT16(node.dbid);
}

PG_FUNCTION_INFO_V1(gp_add_master_standby);

/*
 * gp_add_master_standby(hostname, address, datadir)
 *		The standby, at the coordinator's port.
 */
Datum
gp_add_master_standby(PG_FUNCTION_ARGS)
{
	return add_standby(fcinfo);
}

PG_FUNCTION_INFO_V1(gp_add_master_standby_port);

/*
 * gp_add_master_standby(hostname, address, datadir, port)
 *		The standby, at that port.
 */
Datum
gp_add_master_standby_port(PG_FUNCTION_ARGS)
{
	return add_standby(fcinfo);
}

PG_FUNCTION_INFO_V1(gp_remove_master_standby);

/*
 * gp_remove_master_standby()
 *		The standby, gone.
 */
Datum
gp_remove_master_standby(PG_FUNCTION_ARGS)
{
	GpSegmentConfig *nodes;
	GpSegmentConfig *standby;
	int			nnodes;

	segadmin_check("gp_remove_master_standby");

	nnodes = segadmin_nodes(&nodes);
	standby = node_of(nodes, nnodes, -1, 'm');
	if (standby == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no master standby defined")));
	segadmin_change(standby, true);

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(gp_activate_standby);

/*
 * gp_activate_standby()
 *		On a standby that has been promoted: what Cloudberry's startup
 *		process does to the catalog as it promotes one
 *		(catalog_activate_standby()) -- the old coordinator gone, and this
 *		node the coordinator, its dbid kept.  True, and nothing changed, on
 *		the coordinator this node is already.
 */
Datum
gp_activate_standby(PG_FUNCTION_ARGS)
{
	GpSegmentConfig *nodes;
	GpSegmentConfig *coordinator;
	GpSegmentConfig self;
	int			nnodes;
	int			place;

	segadmin_check("gp_activate_standby");

	nnodes = segadmin_nodes(&nodes);
	place = place_of(nodes, nnodes, GpClusterDbid());
	if (place < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find configuration entry for dbid %i",
						GpClusterDbid())));
	if (nodes[place].role == 'p')
		PG_RETURN_BOOL(true);

	coordinator = node_of(nodes, nnodes, -1, 'p');
	if (coordinator != NULL)
		segadmin_change(coordinator, true);
	self = nodes[place];
	self.role = 'p';
	self.preferred_role = 'p';
	self.mode = 's';
	self.status = 'u';
	segadmin_change(&self, false);

	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(gp_update_segment_configuration_mode_status);

/*
 * gp_update_segment_configuration_mode_status(dbid, mode, status)
 *		A node's mode and status, as a tool knows them: FTS takes it from
 *		there at its next probe.
 */
Datum
gp_update_segment_configuration_mode_status(PG_FUNCTION_ARGS)
{
	int			dbid = int_arg(fcinfo, 0, false, "dbid");
	char		mode = char_arg(fcinfo, 1, "mode");
	char		status = char_arg(fcinfo, 2, "status");
	GpSegmentConfig *nodes;
	GpSegmentConfig node;
	int			nnodes;
	int			place;

	if (dbid < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dbid should not less than 0.")));
	if (mode != 's' && mode != 'n')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("mode shoud be sync or nosync")));
	if (status != 'u' && status != 'd')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("mode shoud be up or down")));

	segadmin_check("gp_update_segment_configuration_mode_status");

	nnodes = segadmin_nodes(&nodes);
	place = place_of(nodes, nnodes, dbid);
	if (place < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("could not find configuration entry for dbid %i", dbid)));
	node = nodes[place];
	node.mode = mode;
	node.status = status;
	segadmin_change(&node, false);

	PG_RETURN_INT16(1);
}
