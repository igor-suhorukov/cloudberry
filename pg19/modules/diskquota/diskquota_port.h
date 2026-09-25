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
 * diskquota_port.h
 *	  What diskquota asks of Cloudberry's server, answered by the port's
 *	  gp_core.
 *
 * Cloudberry's diskquota (gpcontrib/diskquota) is built in Cloudberry's tree
 * and reaches the dispatcher, a backend's role and the fault injector as
 * Cloudberry's own code does.  Here they are gp_core's: a backend's role is
 * the cluster's to say (gp_cluster.c), a query is sent to the segments
 * through a gather (gp_dispatch.c), and a fault is gp_core's GP_FAULT().
 * The files that change a relation's size are O21's events, where Cloudberry
 * has four hooks of its own in its storage manager (gp_activetable.c).
 *
 * On one node the node is its own only segment: what the worker sends the
 * segments it runs here, and the sizes it counts are segment 0's.
 *
 *-------------------------------------------------------------------------
 */
#ifndef DISKQUOTA_PORT_H
#define DISKQUOTA_PORT_H

#include "access/xlog.h"
#include "executor/tuptable.h"
#include "storage/procnumber.h"
#include "storage/relfilelocator.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_fault.h"

/*
 * The coordinator, or the one node: where the launcher and its workers run,
 * and a quota is set.  A segment keeps its active tables and the reject map.
 */
#define IS_QUERY_DISPATCHER() \
	(GpClusterIsSingleNode() || GpClusterBackendRole() == GP_ROLE_DISPATCH)

/* A cluster's coordinator, which has segments to ask. */
#define DQ_HAS_SEGMENTS()	(!GpClusterIsSingleNode())

/*
 * The segment a size is counted for: the node's content id, 0 on one node,
 * whose only segment it is; -1, the coordinator's, is the total of them all.
 */
#define DQ_SEGINDEX() \
	(GpClusterIsSingleNode() ? 0 : GpClusterContentId())

/* The segments a quota's per-segment share is divided among; one on one node. */
#define getgpsegmentCount()	GpClusterSegmentCount()

/*
 * A mirror replays its primary's WAL, and O21 tells the hook of what replay
 * does to files too: a node in recovery keeps no active tables, as
 * Cloudberry's mirror does not.
 */
#define IsRoleMirror()		RecoveryInProgress()

/* Cloudberry's fault injector, as gp_core carries it, in every build. */
#ifndef FAULT_INJECTOR
#define FAULT_INJECTOR
#endif
#define SIMPLE_FAULT_INJECTOR(name)	GP_FAULT(name)
#define FaultInjectorTypeSkip		GP_FAULT_SKIP

/*
 * How many entries a shared map of diskquota's is made with.  Cloudberry's
 * dynahash gave a shared table its entries a batch of at least 32 at a time
 * (choose_nelem_alloc()), so a map asked for 2 held 32, which diskquota's
 * tests rely on; PostgreSQL 19's has exactly as many as it is asked for,
 * from the start.
 */
#define DQ_SHMEM_NELEMS(n)	((long) TYPEALIGN(32, Max((long) (n), 1)))

/* Cloudberry's elog.h: ereport() when the condition holds. */
#define ereportif(p, elevel, ...) \
	do \
	{ \
		if (p) ereport(elevel, __VA_ARGS__); \
	} while (0)

/* The connections to the segments, closed while the worker is paused. */
#define DisconnectAndDestroyAllGangs(destroy)	GpDispatchResetGang()

/*
 * A query sent to every segment, each row of each segment's answer handed to
 * fn as a slot of "desc", which has to be the query's own result, with the
 * segment's content id.  Answers the number of segments asked.  Cloudberry's
 * CdbDispatchCommand() and the PGresults it collects.
 */
typedef void (*DqSegmentRowFn) (TupleTableSlot *slot, int content, void *arg);
extern int	dq_segments_query(const char *sql, TupleDesc desc,
							  DqSegmentRowFn fn, void *arg);

/* A statement sent to every segment, its answers let go. */
extern void dq_segments_command(const char *sql);

/* Is this backend running a segment's part for the worker? (on one node) */
extern bool dq_segment_call(void);

/* The ORCA of the port off in this session, as a worker has it. */
extern void dq_optimizer_off(void);

/*
 * The backend a relation's files are named for: its temporary schema's, for
 * a temporary relation of any session, as the relcache has it.
 */
extern ProcNumber dq_relation_backend(char relpersistence, Oid relnamespace);

/*
 * The temporary relation a file number is of.  Cloudberry's
 * RelidByRelfilenode(), of PostgreSQL 14, finds a temporary relation too,
 * and diskquota maps a temporary table's file to it by that once the table is
 * committed; PostgreSQL 16's and later's passes over one, whose file number is
 * unique only with its backend's.
 */
extern Oid dq_temp_relid_by_relfilenumber(Oid spcOid, RelFileNumber relNumber);

/*
 * What Cloudberry has among its catalogs the port's own modules make in each
 * database, as their extensions' tables: gp_core's, gp_sql's, gp_ao's and
 * the others' (gp_sql's distribution.c lists them too).  diskquota passes
 * over such a relation -- and its TOAST table and indexes -- as it passes
 * over a catalog, whose OID is below FirstNormalObjectId.
 */
#define DQ_PORT_EXTENSIONS \
	"'gp_core', 'gp_sql', 'gp_task', 'gp_security', 'gp_matview', 'gp_orca', " \
	"'gp_ao', 'pax', 'gp_exttable', 'gp_resource', 'gp_tde', 'gp_inject_fault', " \
	"'gp_debug_numsegments'"
#define DQ_PORT_RELATIONS_SQL \
	"WITH m AS (SELECT d.objid FROM pg_catalog.pg_depend d" \
	" JOIN pg_catalog.pg_extension e ON e.oid = d.refobjid" \
	" WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass" \
	" AND d.deptype = 'e' AND e.extname IN (" DQ_PORT_EXTENSIONS ")), " \
	"t AS (SELECT objid AS oid FROM m UNION SELECT c.reltoastrelid FROM pg_catalog.pg_class c" \
	" JOIN m ON c.oid = m.objid WHERE c.reltoastrelid <> 0) " \
	"SELECT oid FROM t UNION SELECT i.indexrelid FROM pg_catalog.pg_index i JOIN t ON i.indrelid = t.oid"

/* Is the relation a catalog, or one of the port's modules' (above)? */
extern bool dq_is_system_relation(Oid relid);

#endif							/* DISKQUOTA_PORT_H */
