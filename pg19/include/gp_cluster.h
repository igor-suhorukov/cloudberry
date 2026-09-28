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
 * gp_cluster.h
 *	  Which nodes there are, and which of them this one is.
 *
 * Cloudberry keeps this in gp_segment_configuration, a *shared* catalog: every
 * database sees the same rows, and a backend can read them before it has done
 * anything else.  An extension can create no shared catalog, so the port reads
 * the same thing from a file, which is what Cloudberry's own external-FTS
 * builds already do in spirit -- there the rows come from etcd behind a view,
 * and gp_segment_configuration stops being a catalog at all.
 *
 * The file says which nodes there are and where, and is read once, in
 * _PG_init.  What FTS finds of them -- which node of a content is its primary
 * now, whether a primary and its mirror are in sync, whether a node is up --
 * changes while the cluster runs (gp_fts.c).  On the coordinator that is
 * kept in shared memory, whose durable form is gpsegconfig_dump in its data
 * directory, the file Cloudberry's FTS writes for readers outside a
 * transaction: FTS writes the file, then shared memory, and a backend adopts
 * what it finds there when it next connects to the segments
 * (GpClusterRefresh), so that the primaries a gang reaches do not change
 * under it.
 *
 * The nodes themselves change while the cluster runs too, on the coordinator
 * alone: a mirror or a standby added, one removed, a failed one recovered
 * somewhere else, a segment added by gpexpand or the last one removed by
 * gpshrink -- Cloudberry's segment administration functions, which its tools
 * call (gp_segadmin.c).  Such a change is written to the file first, which a
 * node started after it reads, and then to the coordinator's live copy, whose
 * room is fixed as the server starts: a primary and a mirror for each
 * content the cluster may grow to (gp.max_segments), the coordinator and a
 * standby.  A place with no node in it has dbid 0.  The number of segments
 * is kept beside the nodes, and a backend takes it once a transaction, with
 * no gang in use (GpClusterAdoptSegments).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_CLUSTER_H
#define GP_CLUSTER_H

#include "postgres.h"

/*
 * One node of the cluster, in the columns of Cloudberry's
 * gp_segment_configuration.  The file gives each node its preferred role;
 * the role it has now, its mode and its status are FTS's.
 */
typedef struct GpSegmentConfig
{
	int			dbid;			/* unique over the cluster, 1 is the coordinator */
	int			content;		/* -1 the coordinator, 0..n-1 a segment */
	char		role;			/* 'p' primary, 'm' mirror: what it is now */
	char		preferred_role; /* what the file says it is */
	char		mode;			/* 's' in sync with its peer, 'n' not */
	char		status;			/* 'u' up, 'd' down */
	char	   *hostname;		/* a host name, or a directory for a socket */
	int			port;
	char	   *datadir;
} GpSegmentConfig;

/*
 * What FTS keeps of a node, in the order of GpClusterNodes(), and whose it
 * is: the dbid of the node in that place, 0 where there is none.
 */
typedef struct GpClusterNodeState
{
	char		role;
	char		mode;
	char		status;
	int			dbid;
} GpClusterNodeState;

/* FTS's durable form of them, in the coordinator's data directory. */
#define GP_CLUSTER_DUMP_FILE		"gpsegconfig_dump"
#define GP_CLUSTER_DUMP_FILE_TMP	"gpsegconfig_dump.tmp"

/*
 * The primaries with a content id of 0 or more, in content order, as this
 * backend last adopted them (GpClusterRefresh).  Returns NULL and sets
 * *nsegments to 0 on a node with no cluster configured, which is what every
 * single-node server is.
 */
extern const GpSegmentConfig *GpClusterSegments(int *nsegments);

/* The primary that holds this content id, or NULL. */
extern const GpSegmentConfig *GpClusterSegmentByContent(int content);

/*
 * Every place for a node, as this backend last adopted them: the nodes the
 * file lists, mirrors too, and the places where one could be added, whose
 * dbid is 0.
 */
extern int	GpClusterNodes(const GpSegmentConfig **nodes);

/* Does any segment have a mirror? */
extern bool GpClusterHasMirrors(void);

/*
 * Adopt what FTS last published: the role, mode and status of every node,
 * and so which node is each content's primary.  True when a content's
 * primary is another node than before.  A dispatcher calls it before it
 * connects to the segments, never while its connections are in use.
 */
extern bool GpClusterRefresh(void);

/* Has FTS published anything since this backend last adopted it? */
extern bool GpClusterStale(void);

/*
 * Is the node of that dbid its content's primary, and up, as FTS last
 * published -- whatever this backend adopted?  What a dispatcher asks of the
 * nodes its connections are to, when it cannot adopt anything yet.
 */
extern bool GpClusterIsPrimaryNow(int dbid);

/*
 * What FTS last published, whatever this backend adopted: each node's state,
 * in the order of GpClusterNodes(), and the number FTS bumps at each change.
 */
extern uint64 GpClusterLiveStates(GpClusterNodeState *states);

/*
 * FTS only: make these the cluster's states -- written to
 * gpsegconfig_dump durably first, and then to shared memory, so that what a
 * backend adopts has been written, and waited for until the coordinator's
 * standby has them too.  False, and nothing written, where a node was added,
 * removed or moved since FTS read them: its dbids say so.
 */
extern bool GpClusterPublish(const GpClusterNodeState *states);

/*
 * The replay of the states the coordinator logs as it writes
 * gpsegconfig_dump, gp_core's resource manager's record (gp_dbcopy.c): on
 * the standby, its own gpsegconfig_dump and shared memory.
 */
extern void GpClusterRedo(const char *text, int len);

/*
 * gpexpand's version of the cluster, Cloudberry's gp_expand_version: 0 as
 * the server starts, bumped by gp_expand_bump_version() and as a segment is
 * added or removed.  0 on a server of no cluster.  And the version this
 * backend's number of segments is of.
 */
extern uint64 GpClusterExpandVersion(void);
extern void GpClusterBumpExpandVersion(void);
extern uint64 GpClusterAdoptedExpandVersion(void);

/*
 * Has a segment been added or removed, or gpexpand's version bumped, since
 * this backend took the number of segments?  GpClusterAdoptSegments() takes
 * it, and the primaries of the contents it counts: true when anything was
 * new.  A dispatcher takes it with no gang open, as a transaction begins:
 * the gang's connections point at the primaries.
 */
extern bool GpClusterSegmentsChanged(void);
extern bool GpClusterAdoptSegments(void);

/*
 * On the coordinator, once in each transaction, as a backend first asks for
 * the segments -- the getters here call it, and a dispatcher does before it
 * uses the gang it has -- whether it takes those changed since: the decider
 * says, which gp_expand.c registers.  Nothing in the transaction has used
 * the number before.
 */
typedef void (*GpClusterDecider) (void);
extern void GpClusterSetDecider(GpClusterDecider decider);
extern void GpClusterDecideSegments(void);

/* How many segments the cluster may grow to while it runs; 0 on one node. */
extern int	GpClusterMaxSegments(void);

/*
 * Changing the nodes, on the coordinator: gp_segadmin.c's.  Between
 * GpClusterLockNodes() and the end of the transaction, or
 * GpClusterUnlockNodes(), nothing else changes them: GpClusterLiveNodes()
 * returns each place as it is now, palloc'd, and GpClusterReplaceNodes()
 * makes the places what it is given -- written to the file gp.cluster_config
 * names, rewritten in place, then to gpsegconfig_dump and shared memory --
 * if they are a cluster: a dbid once each, one primary for each content,
 * every content there, and each node one the file can hold.
 */
extern void GpClusterLockNodes(void);
extern void GpClusterUnlockNodes(void);
extern int	GpClusterLiveNodes(GpSegmentConfig **nodes);
extern void GpClusterReplaceNodes(const GpSegmentConfig *nodes);

/* This node's own entry, or NULL when no cluster is configured. */
extern const GpSegmentConfig *GpClusterSelf(void);

/* The node of that dbid, coordinator included, or NULL, as last adopted. */
extern const GpSegmentConfig *GpClusterNodeByDbid(int dbid);

/* The coordinator, the primary with content id -1; NULL with no cluster. */
extern const GpSegmentConfig *GpClusterCoordinator(void);

/*
 * The session this backend works for: a number the coordinator gives each of
 * its backends, and on a segment the coordinator's backend the dispatcher
 * works for -- what Cloudberry calls gp_session_id, which it shares across
 * the nodes.  On one node a backend's process ID.
 */
extern int	GpClusterSessionId(void);

/* A new one for this coordinator backend, whose gang was lost; the old one. */
extern int	GpClusterNewSessionId(void);

/*
 * A module's call as a coordinator backend takes a new session id, with the
 * old one and the new: gp_resource's session state is the session's.
 */
typedef void (*GpClusterSessionCallback) (int old_session, int new_session);
extern void GpClusterAddSessionCallback(GpClusterSessionCallback callback);

/*
 * How many segments to compute with.  Never 0 -- consumers divide by it; see
 * gp_core_api.h -- so a server with no segments answers 1, as Cloudberry's own
 * getgpsegmentCount() does for a singleton.  On the coordinator the number
 * this backend last took; in a segment process its coordinator backend's.
 */
extern int	GpClusterSegmentCount(void);

/*
 * How many segments the cluster has now, whatever this backend took: what a
 * distribution policy that names no number spreads its rows over.  Another
 * number than the above only in a session that kept the old one when a
 * segment was added or removed (gp_expand.c).
 */
extern int	GpClusterSegmentCountNow(void);

/* Are there no segments at all?  The question the count cannot answer. */
extern bool GpClusterIsSingleNode(void);

/* This node's content id: -1 on the coordinator and on a single node. */
extern int	GpClusterContentId(void);

/* This node's dbid. */
extern int	GpClusterDbid(void);

/*
 * What this *backend* is, which is not always what the node is: a backend the
 * dispatcher started is an executor whatever the node says, and that is how a
 * segment tells a dispatched connection from somebody's psql.
 */
extern int	GpClusterBackendRole(void);

/* Is this backend serving a dispatched request?  (gp.qe_identity is set.) */
extern bool GpClusterIsDispatched(void);

/* The identity the dispatcher gave it, "" when it has none. */
extern const char *GpClusterQeIdentity(void);

/*
 * Is it the coordinator's own connection: dispatched, and carrying the
 * cluster secret?  Only such a connection is given plans to carry out.
 */
extern bool GpClusterDispatchTrusted(void);

/* Is gp.cluster_secret set -- can plans be dispatched at all?  And its value. */
extern bool GpClusterHasSecret(void);
extern const char *GpClusterSecret(void);

/* Defines the settings and reads the file; called from gp_core's _PG_init. */
extern void GpClusterInit(void);

#endif							/* GP_CLUSTER_H */
