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
 * gp_resource.h
 *	  What the parts of the resource manager share.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_RESOURCE_H
#define GP_RESOURCE_H

#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/parsenodes.h"
#include "utils/timestamp.h"

#include "gp_settings.h"

#define GP_RESOURCE_SCHEMA		"gp_resource"

/*
 * The definitions, as one JSON label each on a NOLOGIN role of their own
 * (resdefs.c): the queues, which Cloudberry keeps in pg_resqueue and
 * pg_resqueuecapability, and the groups, pg_resgroup and
 * pg_resgroupcapability.
 */
#define GP_RESOURCE_PROVIDER	"gp_resource"
#define GP_RESQUEUE_ROLE		"gp_resource_queues"
#define GP_RESGROUP_ROLE		"gp_resource_groups"

/* Cloudberry's OIDs of the definitions it makes at initdb, which tests print */
#define DEFAULTRESQUEUE_OID		6055
#define DEFAULTRESGROUP_OID		6437
#define ADMINRESGROUP_OID		6438
#define SYSTEMRESGROUP_OID		6448
#define DEFAULT_RESQUEUE_NAME	"pg_default"

/* Cloudberry's pg_resourcetype: the kinds of limit a queue has */
#define PG_RESRCTYPE_ACTIVE_STATEMENTS	1
#define PG_RESRCTYPE_MAX_COST			2
#define PG_RESRCTYPE_MIN_COST			3
#define PG_RESRCTYPE_COST_OVERCOMMIT	4
#define PG_RESRCTYPE_PRIORITY			5
#define PG_RESRCTYPE_MEMORY_LIMIT		6
#define PG_RESRCTYPE_COUNT				6

/* Cloudberry's SQLSTATE 22020, invalid_limit_value, which PostgreSQL 19 has not */
#define ERRCODE_INVALID_LIMIT_VALUE	MAKE_SQLSTATE('2','2','0','2','0')

/* No limit, for a queue's count and cost */
#define INVALID_RES_LIMIT_THRESHOLD		(-1)

/* The least MEMORY_LIMIT a queue may have (Cloudberry's queue.c) */
#define MIN_RESOURCEQUEUE_MEMORY_LIMIT_KB	(10 * 1024)

/* Cloudberry's pg_resgroupcapability.reslimittype */
typedef enum ResGroupLimitType
{
	RESGROUP_LIMIT_TYPE_UNKNOWN = 0,
	RESGROUP_LIMIT_TYPE_CONCURRENCY,
	RESGROUP_LIMIT_TYPE_CPU,		/* cpu_max_percent */
	RESGROUP_LIMIT_TYPE_CPU_SHARES, /* cpu_weight */
	RESGROUP_LIMIT_TYPE_CPUSET,
	RESGROUP_LIMIT_TYPE_MEMORY_LIMIT,	/* memory_quota */
	RESGROUP_LIMIT_TYPE_MIN_COST,
	RESGROUP_LIMIT_TYPE_IO_LIMIT,
	RESGROUP_LIMIT_TYPE_COUNT
} ResGroupLimitType;

/* How many groups there may be, Cloudberry's MaxResourceGroups */
#define MaxResourceGroups		100
#define MaxCpuSetLength			1024

/*
 * A queue's definition: its row of Cloudberry's pg_resqueue and its
 * pg_resqueuecapability rows (priority and memory_limit; a capability of a
 * kind the queue has none of is NULL).
 */
typedef struct ResQueueDef
{
	char	   *name;
	Oid			oid;
	float4		active_statements;	/* rsqcountlimit */
	float4		max_cost;			/* rsqcostlimit */
	bool		cost_overcommit;	/* rsqovercommit */
	float4		min_cost;			/* rsqignorecostlimit */
	char	   *priority;			/* as written: "high", "MeDiUm" */
	char	   *memory_limit;		/* as written: "-1", "500MB" */
	char	   *comment;			/* COMMENT ON RESOURCE QUEUE, or NULL */
} ResQueueDef;

/*
 * A group's definition: its row of pg_resgroup and its seven
 * pg_resgroupcapability values.
 */
typedef struct ResGroupDef
{
	char	   *name;
	Oid			oid;
	int			concurrency;
	int			cpu_max_percent;	/* -1: the group has a cpuset instead */
	int			cpu_weight;
	char	   *cpuset;				/* "-1": none */
	int			memory_quota;		/* MB; -1: none */
	int			min_cost;
	char	   *io_limit;			/* "-1": none */
	char	   *comment;
} ResGroupDef;

/* The settings, gp.* spellings of Cloudberry's (gp_resource.c) */
typedef enum ResourceManagerPolicy
{
	RESOURCE_MANAGER_POLICY_NONE,	/* the port's: nothing is managed */
	RESOURCE_MANAGER_POLICY_QUEUE,
	RESOURCE_MANAGER_POLICY_GROUP,
	RESOURCE_MANAGER_POLICY_GROUP_V2,
} ResourceManagerPolicy;

typedef enum ResManagerMemoryPolicy
{
	RESMANAGER_MEMORY_POLICY_NONE,
	RESMANAGER_MEMORY_POLICY_AUTO,
	RESMANAGER_MEMORY_POLICY_EAGER_FREE,
} ResManagerMemoryPolicy;

extern PGDLLIMPORT int gp_resource_manager_policy;
extern PGDLLIMPORT bool gp_resource_scheduler;
extern PGDLLIMPORT bool gp_resource_select_only;
extern PGDLLIMPORT bool gp_resource_cleanup_gangs_on_wait;
extern PGDLLIMPORT int gp_max_resource_queues;
extern PGDLLIMPORT int gp_max_resource_portals_per_transaction;
extern PGDLLIMPORT int gp_resqueue_memory_policy;
extern PGDLLIMPORT int gp_resqueue_memory_policy_auto_fixed_mem;
extern PGDLLIMPORT bool gp_log_resqueue_memory;
extern PGDLLIMPORT bool gp_resqueue_print_operator_memory_limits;
extern PGDLLIMPORT int gp_max_statement_mem;
extern PGDLLIMPORT bool gp_resqueue_priority;
extern PGDLLIMPORT char *gp_resqueue_priority_default_value;

extern PGDLLIMPORT char *gp_resource_group_cgroup_parent;
extern PGDLLIMPORT double gp_resource_group_cpu_limit;
extern PGDLLIMPORT int gp_resource_group_cpu_priority;
extern PGDLLIMPORT bool gp_resource_group_bypass;
extern PGDLLIMPORT bool gp_resource_group_bypass_catalog_query;
extern PGDLLIMPORT bool gp_resource_group_bypass_direct_dispatch;
extern PGDLLIMPORT int gp_resource_group_queuing_timeout;
extern PGDLLIMPORT int gp_resource_group_move_timeout;
extern PGDLLIMPORT int gp_resgroup_memory_policy;
extern PGDLLIMPORT int gp_resgroup_memory_query_fixed_mem;
extern PGDLLIMPORT int gp_resgroup_memory_policy_auto_fixed_mem;
extern PGDLLIMPORT bool gp_log_resgroup_memory;
extern PGDLLIMPORT bool gp_resgroup_debug_wait_queue;
extern PGDLLIMPORT bool gp_debug_resource_group;
extern PGDLLIMPORT char *gp_resource_statement;


/* Which manager is on */
static inline bool
IsResQueueEnabled(void)
{
	return gp_resource_scheduler &&
		gp_resource_manager_policy == RESOURCE_MANAGER_POLICY_QUEUE;
}

static inline bool
IsResGroupEnabled(void)
{
	return gp_resource_scheduler &&
		(gp_resource_manager_policy == RESOURCE_MANAGER_POLICY_GROUP ||
		 gp_resource_manager_policy == RESOURCE_MANAGER_POLICY_GROUP_V2);
}

/* The coordinator, or a single node: where a query takes a queue's slot */
extern bool GpResourceIsDispatcher(void);
extern bool GpResourceIsSegment(void);
extern bool GpResourceIsCoordinator(void);

/* resdefs.c: the definitions */
extern void ResDefsRegisterProvider(void);
extern List *ResQueueDefsLoad(void);	/* of ResQueueDef, by OID */
extern List *ResGroupDefsLoad(void);	/* of ResGroupDef, by OID */
extern ResQueueDef *ResQueueDefFind(List *defs, const char *name);
extern ResQueueDef *ResQueueDefByOid(List *defs, Oid oid);
extern ResGroupDef *ResGroupDefFind(List *defs, const char *name);
extern ResGroupDef *ResGroupDefByOid(List *defs, Oid oid);
extern Oid	ResQueueOidByName(const char *name, bool missing_ok);
extern Oid	ResGroupOidByName(const char *name, bool missing_ok);
extern char *ResGroupNameByOid(Oid oid);
extern char *ResQueueNameByOid(Oid oid);

/* A queue's MEMORY_LIMIT in kB, -1 for none */
extern int64 ResQueueDefMemoryLimitKB(const ResQueueDef *def);

/* A role's queue and group, as its "gp" label keys say, or the defaults */
extern Oid	GetResQueueForRole(Oid roleid);
extern Oid	GetResGroupForRole(Oid roleid);

/*
 * A role's queue or group given by CREATE and ALTER ROLE ... RESOURCE QUEUE
 * and RESOURCE GROUP (gp_resource.c takes them off the statement).
 */
extern void ResDefsAssignRoleQueue(Oid roleid, const char *queue, bool creating);
extern void ResDefsAssignRoleGroup(Oid roleid, const char *group, bool creating);

/* Are these the definitions' carrier roles? */
extern bool ResDefsIsCarrierRole(Oid roleid);

/* resqueue.c: the queues at run time */
struct QueryDesc;
struct PlannedStmt;
extern Size ResQueueShmemSize(void);
extern void ResQueueShmemRequest(void);
extern void ResQueueShmemInit(void);
extern void ResQueueInit(void);
extern void ResQueueBackendStart(void);
extern void ResQueueWaitEventInit(void);
extern void ResQueueDefsChanged(void);
extern bool ResQueueCanCreate(void);
extern void ResQueueCheckDrop(Oid queueid);
extern void ResQueueCheckAlter(Oid queueid, const ResQueueDef *def);
extern int64 ResQueueGetMemoryKB(Oid queueid);
extern void ResQueueRoleChanged(Oid roleid);
extern void ResQueueExecutorStart(struct QueryDesc *queryDesc);
extern void ResQueueUtilityStart(struct PlannedStmt *pstmt);
extern int	ResQueueQueryBudgetKB(void);
extern PGDLLEXPORT bool GpResQueueMemoryLimitInSync(const char *queuename);
extern int	ResQueuePriorityLookup(const char *priority);
extern void ResQueuePriorityStart(void);
extern void ResQueuePriorityDispatch(void);
extern void ResQueuePriorityEnd(void);

/* resgroup.c: the groups at run time */
extern PGDLLIMPORT bool gp_resource_group_enable_cgroup_cpuset;
extern Size ResGroupShmemSize(void);
extern void ResGroupShmemRequest(void);
extern void ResGroupShmemInit(void);
extern void ResGroupInit(void);
extern void ResGroupDefsChanged(void);
extern bool ResGroupIsAssigned(void);
extern Bitmapset *ResGroupCpusetToBitset(const char *cpuset, int len);
extern bool ResGroupCpusetIsValid(const char *cpuset);
extern void ResGroupEnsureCpusetIsAvailable(void);
extern char *ResGroupNormalizeIoLimit(const char *io_limit);
extern Oid	ResGroupNewOid(void);
extern void ResGroupValidate(List *defs, ResGroupDef *def);
extern void ResGroupCreated(ResGroupDef *def);
extern void ResGroupAltered(ResGroupDef *old, ResGroupDef *def,
							ResGroupLimitType type);
extern void ResGroupDropped(Oid groupid);
extern void ResGroupCheckDrop(Oid groupid, const char *name);

/* gp_resource.c: a utility statement, before it runs */
extern void ResourceManagerUtilityStart(struct PlannedStmt *pstmt,
										const char *queryString,
										int context);

/* memprot.c: memory protection, Cloudberry's vmem tracker on O25 */
extern void MemProtInit(void);

#endif							/* GP_RESOURCE_H */
