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
 * The file is read once per backend, in _PG_init, and never again: a cluster
 * does not change shape at M2, because nothing yet moves a segment.  When FTS
 * arrives (M4) the live copy becomes shared memory that the FTS worker owns
 * and this file becomes its durable form; the readers below do not change,
 * only where they read from.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_CLUSTER_H
#define GP_CLUSTER_H

#include "postgres.h"

/*
 * One node of the cluster.  The fields are the ones of Cloudberry's
 * gp_segment_configuration that anything needs yet; mode and status arrive
 * with FTS, which is what maintains them.
 */
typedef struct GpSegmentConfig
{
	int			dbid;			/* unique over the cluster, 1 is the coordinator */
	int			content;		/* -1 the coordinator, 0..n-1 a segment */
	char		role;			/* 'p' primary, 'm' mirror */
	char	   *hostname;		/* a host name, or a directory for a socket */
	int			port;
	char	   *datadir;
} GpSegmentConfig;

/*
 * The primaries with a content id of 0 or more, in content order.  Returns
 * NULL and sets *nsegments to 0 on a node with no cluster configured, which is
 * what every single-node server is.
 */
extern const GpSegmentConfig *GpClusterSegments(int *nsegments);

/* The primary that holds this content id, or NULL. */
extern const GpSegmentConfig *GpClusterSegmentByContent(int content);

/* This node's own entry, or NULL when no cluster is configured. */
extern const GpSegmentConfig *GpClusterSelf(void);

/*
 * How many segments to compute with.  Never 0 -- consumers divide by it; see
 * gp_core_api.h -- so a server with no segments answers 1, as Cloudberry's own
 * getgpsegmentCount() does for a singleton.
 */
extern int	GpClusterSegmentCount(void);

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

/* Defines the settings and reads the file; called from gp_core's _PG_init. */
extern void GpClusterInit(void);

#endif							/* GP_CLUSTER_H */
