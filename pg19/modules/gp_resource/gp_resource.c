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
 * gp_resource.c
 *	  Cloudberry's resource management: resource queues, resource groups
 *	  and memory protection.
 *
 * gp.resource_manager says which of the two managers is on, as Cloudberry's
 * gp_resource_manager does: queues, Cloudberry's default, or groups.  Their
 * definitions are labels (resdefs.c); a query takes a queue's slot on the
 * coordinator (resqueue.c) and a transaction a group's (resgroup.c).
 *
 * This file is the module's frame: its settings, as gp_core's are named --
 * gp_ lost and gp. put in front, statement_mem becoming gp.statement_mem --
 * its shared memory, and the hooks that hand a statement to the manager
 * that is on.  And it takes a role's RESOURCE QUEUE and RESOURCE GROUP off
 * CREATE and ALTER ROLE, which O26 carries there (gp_desugar.c), and gives
 * pg_roles and pg_authid Cloudberry's columns rolresqueue and rolresgroup,
 * through O10, as gp_core gives a table gp_segment_id.
 *
 * Cloudberry sources this module is made of:
 *	  src/backend/utils/resource_manager/, utils/resscheduler/,
 *	  utils/resgroup/, commands/queue.c, commands/resgroupcmds.c, and the
 *	  resource settings of utils/misc/guc_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <limits.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"

#include "cb_module.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_endpoint.h"
#include "gp_resource.h"

PG_MODULE_MAGIC_EXT(
					.name = "gp_resource",
					.version = GP_VERSION
);

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

int			gp_resource_manager_policy = RESOURCE_MANAGER_POLICY_QUEUE;
bool		gp_resource_scheduler = true;
bool		gp_resource_select_only = false;
bool		gp_resource_cleanup_gangs_on_wait = true;
int			gp_max_resource_queues = 9;
int			gp_max_resource_portals_per_transaction = 64;
int			gp_resqueue_memory_policy = RESMANAGER_MEMORY_POLICY_NONE;
int			gp_resqueue_memory_policy_auto_fixed_mem = 100;
bool		gp_log_resqueue_memory = false;
bool		gp_resqueue_print_operator_memory_limits = false;
int			gp_max_statement_mem = 2048000;
bool		gp_resqueue_priority = true;
char	   *gp_resqueue_priority_default_value = NULL;
static double gp_resqueue_priority_cpucores_per_segment = 4.0;
static int	gp_resqueue_priority_sweeper_interval = 1000;

char	   *gp_resource_group_cgroup_parent = NULL;
double		gp_resource_group_cpu_limit = 0.9;
int			gp_resource_group_cpu_priority = 10;
bool		gp_resource_group_bypass = false;
bool		gp_resource_group_bypass_catalog_query = true;
bool		gp_resource_group_bypass_direct_dispatch = true;
int			gp_resource_group_queuing_timeout = 0;
int			gp_resource_group_move_timeout = 30000;
int			gp_resgroup_memory_policy = RESMANAGER_MEMORY_POLICY_EAGER_FREE;
int			gp_resgroup_memory_query_fixed_mem = 0;
int			gp_resgroup_memory_policy_auto_fixed_mem = 100;
bool		gp_log_resgroup_memory = false;

/*
 * What the coordinator's resource manager tells a segment of the statement
 * it dispatches, which gp_core sends with it: "weight=N", its priority's.
 */
char	   *gp_resource_statement = NULL;
bool		gp_resgroup_debug_wait_queue = true;
bool		gp_debug_resource_group = false;

static const struct config_enum_entry resource_manager_options[] = {
	{"queue", RESOURCE_MANAGER_POLICY_QUEUE, false},
	{"group", RESOURCE_MANAGER_POLICY_GROUP, false},
	{"group-v2", RESOURCE_MANAGER_POLICY_GROUP_V2, false},
	{NULL, 0, false}
};

static const struct config_enum_entry memory_policy_options[] = {
	{"none", RESMANAGER_MEMORY_POLICY_NONE, false},
	{"auto", RESMANAGER_MEMORY_POLICY_AUTO, false},
	{"eager_free", RESMANAGER_MEMORY_POLICY_EAGER_FREE, false},
	{NULL, 0, false}
};

/* Cloudberry's check: letters, digits and "-._", not starting with the three */
static bool
check_cgroup_parent(char **newval, void **extra, GucSource source)
{
	const char *p = *newval;

	bool		ok = p != NULL && isalnum((unsigned char) *p);

	for (; ok && *p; p++)
	{
		if (!isalnum((unsigned char) *p) && *p != '-' && *p != '.' && *p != '_')
			ok = false;
	}
	if (!ok)
		GUC_check_errmsg("gp_resource_group_cgroup_parent can only contains alphabet, number and non-leading . _ -");
	return ok;
}

/* gp.statement_mem is gp_core's; gp.max_statement_mem bounds it */
static bool
check_max_statement_mem(int *newval, void **extra, GucSource source)
{
	return true;
}

/*
 * gpvars_check_rg_query_fixed_mem(): a query's fixed memory less than a
 * statement's most, in Cloudberry's words
 */
static bool
check_rg_query_fixed_mem(int *newval, void **extra, GucSource source)
{
	if (*newval >= gp_max_statement_mem)
	{
		GUC_check_errmsg("Invalid input for gp_resgroup_memory_query_fixed_mem, must be less than max_statement_mem (%d kB)",
						 gp_max_statement_mem);
		return false;
	}
	return true;
}

/* gpvars_check_gp_resqueue_priority_default_value(): one of the priorities */
static bool
check_priority_default(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && ResQueuePriorityLookup(*newval) > 0)
		return true;
	GUC_check_errmsg("invalid value for gp_resqueue_priority_default_value: \"%s\"",
					 *newval != NULL ? *newval : "");
	return false;
}

/*
 * gp.resource_group_bypass may not change while the transaction holds a
 * group's slot (guc_gp.c's check_gp_resource_group_bypass()).
 */
static bool
check_resource_group_bypass(bool *newval, void **extra, GucSource source)
{
	if (source != PGC_S_DEFAULT && ResGroupIsAssigned())
	{
		GUC_check_errmsg("SET gp_resource_group_bypass cannot run inside a transaction block");
		return false;
	}
	return true;
}

static void
define_settings(void)
{
	DefineCustomEnumVariable("gp.resource_manager",
							 "Sets the type of resource manager.",
							 "Only support \"queue\", \"group\" and \"group-v2\" for now.",
							 &gp_resource_manager_policy,
							 RESOURCE_MANAGER_POLICY_QUEUE, resource_manager_options,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_scheduler",
							 "Enable resource scheduling.",
							 NULL, &gp_resource_scheduler, true,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_select_only",
							 "Enable resource locking of SELECT only.",
							 NULL, &gp_resource_select_only, false,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_cleanup_gangs_on_wait",
							 "Enable idle gang cleanup before resource lockwait.",
							 "Accepted: a session's segment connections are kept while it waits.",
							 &gp_resource_cleanup_gangs_on_wait, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.max_resource_queues",
							"Maximum number of resource queues.",
							NULL, &gp_max_resource_queues, 9, 1, INT_MAX,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.max_resource_portals_per_transaction",
							"Maximum number of resource queues.",
							NULL, &gp_max_resource_portals_per_transaction,
							64, 1, INT_MAX, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.resqueue_memory_policy",
							 "Sets the policy for memory allocation of queries.",
							 "Valid values are NONE, AUTO, EAGER_FREE.",
							 &gp_resqueue_memory_policy,
							 RESMANAGER_MEMORY_POLICY_NONE, memory_policy_options,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resqueue_memory_policy_auto_fixed_mem",
							"Sets the fixed amount of memory reserved for non-memory intensive operators in the AUTO policy.",
							NULL, &gp_resqueue_memory_policy_auto_fixed_mem,
							100, 50, INT_MAX, PGC_USERSET, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.log_resqueue_memory",
							 "Prints out messages related to resource queue's memory management.",
							 NULL, &gp_log_resqueue_memory, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resqueue_print_operator_memory_limits",
							 "Prints out the memory limit for operators (in explain) assigned by resource queue's memory management.",
							 NULL, &gp_resqueue_print_operator_memory_limits, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.max_statement_mem",
							"Sets the maximum value for statement_mem setting.",
							NULL, &gp_max_statement_mem, 2048000, 32768, INT_MAX,
							PGC_SUSET, GUC_UNIT_KB, check_max_statement_mem,
							NULL, NULL);
	DefineCustomBoolVariable("gp.resqueue_priority",
							 "Enables priority scheduling.",
							 "Accepted: a query's priority is recorded and reported, and no process is slowed for it.",
							 &gp_resqueue_priority, true,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("gp.resqueue_priority_cpucores_per_segment",
							 "Number of processing units associated with a segment.",
							 "Accepted: nothing is weighed by it.",
							 &gp_resqueue_priority_cpucores_per_segment,
							 4.0, 0.1, 512.0, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resqueue_priority_sweeper_interval",
							"Frequency (in ms) at which sweeper process re-evaluates CPU shares.",
							"Accepted: there is no sweeper.",
							&gp_resqueue_priority_sweeper_interval,
							1000, 500, 15000, PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("gp.resqueue_priority_default_value",
							   "Default weight when one cannot be associated with a statement.",
							   NULL, &gp_resqueue_priority_default_value, "MEDIUM",
							   PGC_POSTMASTER, 0, check_priority_default,
							   NULL, NULL);

	DefineCustomStringVariable("gp.resource_group_cgroup_parent",
							   "The root of gpdb cgroup hierarchy.",
							   NULL, &gp_resource_group_cgroup_parent, "gpdb.service",
							   PGC_POSTMASTER, GUC_SUPERUSER_ONLY,
							   check_cgroup_parent, NULL, NULL);
	DefineCustomRealVariable("gp.resource_group_cpu_limit",
							 "Maximum percentage of CPU resources assigned to a cluster.",
							 NULL, &gp_resource_group_cpu_limit, 0.9, 0.1, 1.0,
							 PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resource_group_cpu_priority",
							"Sets the cpu priority for postgres processes when resource group is enabled.",
							NULL, &gp_resource_group_cpu_priority, 10, 1, 50,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_group_bypass",
							 "If the value is true, the query in this session will not be limited by resource group.",
							 NULL, &gp_resource_group_bypass, false,
							 PGC_USERSET, 0, check_resource_group_bypass, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_group_bypass_catalog_query",
							 "Bypass all catalog only queries.",
							 NULL, &gp_resource_group_bypass_catalog_query, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resource_group_bypass_direct_dispatch",
							 "Bypass direct dispatch queries.",
							 NULL, &gp_resource_group_bypass_direct_dispatch, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resource_group_queuing_timeout",
							"A transaction gives up on queuing on a resource group after this timeout (in ms).",
							NULL, &gp_resource_group_queuing_timeout, 0, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resource_group_move_timeout",
							"Wait up to the specified time (in ms) while moving process to other resource group.",
							NULL, &gp_resource_group_move_timeout, 30000, 10, INT_MAX,
							PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomEnumVariable("gp.resgroup_memory_policy",
							 "Sets the policy for memory allocation of queries.",
							 "Valid values are AUTO, EAGER_FREE.",
							 &gp_resgroup_memory_policy,
							 RESMANAGER_MEMORY_POLICY_EAGER_FREE, memory_policy_options,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("gp.resgroup_memory_query_fixed_mem",
							"Sets the fixed amount of memory reserved for a query.",
							NULL, &gp_resgroup_memory_query_fixed_mem, 0, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_KB, check_rg_query_fixed_mem,
							NULL, NULL);
	DefineCustomIntVariable("gp.resgroup_memory_policy_auto_fixed_mem",
							"Sets the fixed amount of memory reserved for non-memory intensive operators in the AUTO policy.",
							NULL, &gp_resgroup_memory_policy_auto_fixed_mem,
							100, 50, INT_MAX, PGC_USERSET, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.log_resgroup_memory",
							 "Prints out messages related to resource group's memory management.",
							 NULL, &gp_log_resgroup_memory, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.resgroup_debug_wait_queue",
							 "Enable the debugging check on the wait queue of resource group.",
							 NULL, &gp_resgroup_debug_wait_queue, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("gp.debug_resource_group",
							 "Prints resource groups debug logs.",
							 NULL, &gp_debug_resource_group, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("gp_resource.statement",
							   "What the coordinator's resource manager tells a segment of the statement it dispatches.",
							   NULL, &gp_resource_statement, "",
							   PGC_USERSET, GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* What this node is                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Where a query takes a queue's slot and a transaction a group's: the
 * coordinator of a cluster, or a single node -- Cloudberry's
 * Gp_role == GP_ROLE_DISPATCH || IS_SINGLENODE().  Not a retrieve session,
 * whose RETRIEVE reads the rows of a cursor that took its slot, and which is
 * a utility session in Cloudberry, where it takes none (gp_endpoint.c); on a
 * cluster its role says so.
 */
bool
GpResourceIsDispatcher(void)
{
	const GpCoreApi *api = GpCoreApiLookup();

	if (api->get_role() == GP_ROLE_DISPATCH)
		return true;
	return api->get_role() == GP_ROLE_UTILITY && api->is_single_node() &&
		!GpEndpointIsRetrieveSession();
}

/* A segment's process, which the coordinator dispatched to */
bool
GpResourceIsSegment(void)
{
	return GpCoreApiLookup()->get_role() == GP_ROLE_EXECUTE;
}

/* The coordinator of a cluster: Cloudberry's Gp_role == GP_ROLE_DISPATCH */
bool
GpResourceIsCoordinator(void)
{
	return GpCoreApiLookup()->get_role() == GP_ROLE_DISPATCH;
}

/* ------------------------------------------------------------------------- */
/* Shared memory                                                             */
/* ------------------------------------------------------------------------- */

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
gp_resource_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(add_size(ResQueueShmemSize(), ResGroupShmemSize()));
	ResQueueShmemRequest();
	ResGroupShmemRequest();
}

static void
gp_resource_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	ResQueueShmemInit();
	ResGroupShmemInit();
}

/* ------------------------------------------------------------------------- */
/* The executor                                                              */
/* ------------------------------------------------------------------------- */

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static explain_per_plan_hook_type prev_explain_per_plan_hook = NULL;

/*
 * The budgets of the queries this backend has started and not ended, in kB:
 * a cursor's is used again at each FETCH, between other queries.
 */
typedef struct QueryBudget
{
	QueryDesc  *queryDesc;
	int			kb;
} QueryBudget;

static List *query_budgets = NIL;

static int
query_budget(QueryDesc *queryDesc)
{
	foreach_ptr(QueryBudget, b, query_budgets)
	{
		if (b->queryDesc == queryDesc)
			return b->kb;
	}
	return 0;
}

static void
forget_budget(QueryDesc *queryDesc)
{
	foreach_ptr(QueryBudget, b, query_budgets)
	{
		if (b->queryDesc == queryDesc)
		{
			query_budgets = list_delete_ptr(query_budgets, b);
			pfree(b);
			return;
		}
	}
}

/*
 * How deep in statements the backend is: the one a client sent, or the
 * coordinator dispatched, is the outermost, and one it runs through SPI or
 * a portal of its own is inside it.  The outermost is the one a queue's
 * priority is recorded for (ResQueuePriorityStart()), as Cloudberry's
 * backoff entry is the statement's the backend runs.
 */
static int	statement_depth = 0;

static void
statement_begin(void)
{
	if (statement_depth++ == 0)
		ResQueuePriorityStart();
}

static void
statement_end(void)
{
	if (--statement_depth == 0)
		ResQueuePriorityEnd();
}

/*
 * What the segments are told of the statement the coordinator is about to
 * dispatch, which gp_core sends with it when it has changed
 * (gp_resource.statement): the weight its queue's priority gives it, the
 * group it runs in, with the group's limits, and the memory it is given, in
 * kB, which a segment's executor runs it with as its work_mem too.
 */
static int	statement_budget_kb = 0;	/* the budget it was last set with */

static void
set_statement_setting(int budget_kb)
{
	StringInfoData buf;
	int			weight;

	statement_budget_kb = budget_kb;
	/*
	 * nor in a parallel operation, where no setting may change: a parallel
	 * worker's statement, a coordinator's process too, dispatches nothing
	 */
	if (!GpResourceIsCoordinator() || !IsTransactionState() || IsInParallelMode())
		return;
	initStringInfo(&buf);
	weight = ResQueueDispatchWeight();
	if (weight > 0)
		appendStringInfo(&buf, "weight=%d", weight);
	ResGroupDispatchInfo(&buf);
	if (budget_kb > 0)
		appendStringInfo(&buf, "%sbudget=%d", buf.len > 0 ? " " : "", budget_kb);
	if (gp_resource_statement == NULL || strcmp(gp_resource_statement, buf.data) != 0)
		(void) set_config_option("gp_resource.statement", buf.data, PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SET, true, 0, false);
	pfree(buf.data);
}

/*
 * gp_core's call before it tells the segments what changed: the setting
 * again where the transaction was moved to another group since the
 * statement set it (resgroup.c), so that what the statement sends the
 * segments from here names the group it runs in now, as Cloudberry's
 * dispatch does.
 */
static void
gp_resource_before_sync(void)
{
	if (ResGroupDispatchStale())
		set_statement_setting(statement_budget_kb);
}

static object_access_hook_type prev_object_access_hook = NULL;

/*
 * DROP TABLESPACE, as it drops: Cloudberry's check, just before its drop
 * hook, that no group's io_limit names the tablespace (resgroup.c).
 */
static void
gp_resource_object_access(ObjectAccessType access, Oid classId, Oid objectId,
						  int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);
	if (access == OAT_DROP && classId == TableSpaceRelationId &&
		GpResourceIsDispatcher())
		ResGroupCheckTablespaceDrop(objectId);
}

/* A segment's: the memory the coordinator gave the statement, in kB */
static int
dispatched_budget(void)
{
	const char *b = gp_resource_statement != NULL ?
		strstr(gp_resource_statement, "budget=") : NULL;

	return b != NULL ? atoi(b + strlen("budget=")) : 0;
}

/*
 * A query about to run.  On the coordinator: the transaction's group, if
 * this is its first statement, and a slot of it, or of the role's queue,
 * where queues are on; the memory that gives it is its budget, which it runs
 * with as its work_mem.  On a segment: the group and the budget the
 * coordinator dispatched with it.
 */
static void
gp_resource_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		toplevel = statement_depth == 0;
	int			kb = 0;

	ResQueueBackendStart();
	ResGroupBackendStart();
	if (GpResourceIsSegment())
	{
		if (toplevel)
			ResGroupSegmentStatementStart();
		kb = dispatched_budget();
	}
	else if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		if (toplevel)
			ResGroupStatementStart(queryDesc->sourceText);
		ResGroupExecutorStart(queryDesc, toplevel);
		ResQueueExecutorStart(queryDesc);
		kb = IsResGroupEnabled() ? ResGroupQueryBudgetKB() : ResQueueQueryBudgetKB();
		/* what standard_ExecutorStart() logs of it on the coordinator */
		if (GpResourceIsDispatcher() &&
			((gp_log_resqueue_memory && IsResQueueEnabled() &&
			  gp_resqueue_memory_policy != RESMANAGER_MEMORY_POLICY_NONE) ||
			 (gp_log_resgroup_memory && IsResGroupEnabled() &&
			  gp_resgroup_memory_policy != RESMANAGER_MEMORY_POLICY_NONE)))
			ereport(NOTICE,
					(errmsg("query requested %.0fKB of memory", (double) kb)));
	}
	if (kb > 0)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		QueryBudget *b = palloc(sizeof(QueryBudget));

		b->queryDesc = queryDesc;
		b->kb = kb;
		query_budgets = lappend(query_budgets, b);
		MemoryContextSwitchTo(oldcxt);
	}

	/* before gp_core's hook dispatches it: what the segments are told */
	if (toplevel)
		set_statement_setting(kb);

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
gp_resource_ExecutorEnd(QueryDesc *queryDesc)
{
	forget_budget(queryDesc);
	/* a move of the transaction the statement's work put off */
	ResGroupMovePoll();
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * EXPLAIN ANALYZE's summary: the memory the statement was given, which
 * Cloudberry's "Statement statistics" print where a memory policy is on --
 * a query given none prints none.  In text alone, where Cloudberry's gives
 * it in every format: PostgreSQL's own tests, which the singlenode suite
 * runs under Cloudberry's memory policy, read plans in JSON, XML and YAML,
 * and a text plan's line the suite takes out as it takes gp_orca's out.
 * Before calling the hook of the module loaded earlier, so the line comes
 * before gp_orca's "Optimizer", as Cloudberry's does.
 */
static void
gp_resource_explain_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
							 ExplainState *es, const char *queryString,
							 ParamListInfo params, QueryEnvironment *queryEnv)
{
	int			kb = 0;

	if (es->analyze && es->summary && es->format == EXPLAIN_FORMAT_TEXT)
	{
		foreach_ptr(QueryBudget, b, query_budgets)
		{
			if (b->queryDesc->plannedstmt == plannedstmt)
				kb = b->kb;
		}
	}
	if (kb > 0)
		appendStringInfo(es->str, "Memory used:  %dkB\n", kb);

	if (prev_explain_per_plan_hook)
		prev_explain_per_plan_hook(plannedstmt, into, es, queryString,
								   params, queryEnv);
}

/*
 * The budget a query takes, as the work_mem it runs with: coarser than
 * Cloudberry's share of it for each operator, as decided (cloudberry.md,
 * Track E's section 2.4); a plan's nodes have no field for their own share.
 */
static void
gp_resource_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
						uint64 count)
{
	int			budget = query_budget(queryDesc);
	int			saved = work_mem;

	if (budget > 0)
		work_mem = budget;
	PG_TRY();
	{
		statement_begin();
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		work_mem = saved;
		statement_end();
	}
	PG_END_TRY();
}

/* A utility statement, before it runs: COPY's and CREATE TABLE AS's slot */
void
ResourceManagerUtilityStart(PlannedStmt *pstmt, const char *queryString,
							int context)
{
	ResQueueBackendStart();
	ResQueueUtilityStart(pstmt);
}

/* ------------------------------------------------------------------------- */
/* CREATE and ALTER ROLE                                                     */
/* ------------------------------------------------------------------------- */

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* What O26 carried for this module: DefElems in the "gp_resource" namespace */
static bool
is_resource_carrier(DefElem *def)
{
	return def->defnamespace != NULL &&
		strcmp(def->defnamespace, "gp_resource") == 0;
}

static List *
take_carriers(List **options)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (!is_resource_carrier(def))
			continue;
		taken = lappend(taken, def);
		*options = foreach_delete_current(*options, lc);
	}
	return taken;
}

/* The value of a boolean role option the statement gives, or -1 */
static int
role_option_bool(List *options, const char *name)
{
	foreach_node(DefElem, def, options)
	{
		if (def->defnamespace == NULL && strcmp(def->defname, name) == 0)
			return boolVal(def->arg) ? 1 : 0;
	}
	return -1;
}

static const char *
carried_value(List *carried, const char *name)
{
	const char *value = NULL;

	foreach_node(DefElem, def, carried)
	{
		if (strcmp(def->defname, name) == 0)
		{
			if (value != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			value = strVal(def->arg);
		}
	}
	return value;
}

/*
 * CREATE ROLE, as user.c's CreateRole() tells about the queue and the group
 * a role gets without asking for one: where the manager is on, on the
 * coordinator, NOTICEs say which.
 */
static void
role_created(Oid roleid, bool issuper, List *carried)
{
	const char *queue = carried_value(carried, "resource_queue");
	const char *group = carried_value(carried, "resource_group");

	if (queue != NULL)
		ResDefsAssignRoleQueue(roleid, queue, true);
	else if (IsResQueueEnabled() && GpResourceIsCoordinator() && !issuper)
		ereport(NOTICE,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("resource queue required -- using default resource queue \"%s\"",
						DEFAULT_RESQUEUE_NAME)));

	if (group != NULL)
		ResDefsAssignRoleGroup(roleid, group, true);
	else if (IsResGroupEnabled() && GpResourceIsCoordinator())
	{
		if (issuper)
			ereport(NOTICE,
					(errmsg("resource group required -- using admin resource group \"admin_group\"")));
		else
			ereport(NOTICE,
					(errmsg("resource group required -- using default resource group \"default_group\"")));
	}
}

static void
role_altered(Oid roleid, List *carried)
{
	const char *queue = carried_value(carried, "resource_queue");
	const char *group = carried_value(carried, "resource_group");

	if (queue != NULL)
		ResDefsAssignRoleQueue(roleid, queue, false);
	if (group != NULL)
		ResDefsAssignRoleGroup(roleid, group, false);
	ResQueueRoleChanged(roleid);
}

static void
resource_process_utility(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	List	   *carried = NIL;
	char	   *rolename = NULL;
	bool		creating = false;
	int			issuper = -1;

	/* A segment runs what the coordinator dispatched as it is */
	if (GpDispatchIsDispatchedStatement(parsetree))
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		return;
	}

	if (IsA(parsetree, CreateRoleStmt) || IsA(parsetree, AlterRoleStmt))
	{
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
		}
		if (IsA(parsetree, CreateRoleStmt))
		{
			CreateRoleStmt *stmt = (CreateRoleStmt *) parsetree;

			carried = take_carriers(&stmt->options);
			rolename = stmt->role;
			creating = true;
			issuper = role_option_bool(stmt->options, "superuser");
		}
		else
			carried = take_carriers(&((AlterRoleStmt *) parsetree)->options);
	}

	/* COPY and CREATE TABLE AS take a queue's slot */
	ResourceManagerUtilityStart(pstmt, queryString, (int) context);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (creating)
	{
		CommandCounterIncrement();
		role_created(get_role_oid(rolename, false), issuper == 1, carried);
	}
	else if (carried != NIL)
	{
		Oid			roleid = get_rolespec_oid(((AlterRoleStmt *) parsetree)->role,
											  false);

		CommandCounterIncrement();
		role_altered(roleid, carried);
	}
}

/*
 * Is the CALL running now one a client sent, or the rewriting of one
 * (CREATE RESOURCE GROUP is a CALL, gp_desugar.c), and not one a function
 * ran?  The procedure asks, as PreventInTransactionBlock() asks isTopLevel.
 */
static bool call_toplevel = true;

bool
GpResourceCallIsTopLevel(void)
{
	return call_toplevel;
}

static void
gp_resource_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   bool readOnlyTree, ProcessUtilityContext context,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   DestReceiver *dest, QueryCompletion *qc)
{
	bool		save_call_toplevel = call_toplevel;

	if (IsA(pstmt->utilityStmt, CallStmt))
		call_toplevel = context == PROCESS_UTILITY_TOPLEVEL;
	PG_TRY();
	{
		statement_begin();
		if (statement_depth == 1)
		{
			/* the transaction's group, or the one the coordinator's runs in */
			ResQueueBackendStart();
			ResGroupBackendStart();
			if (GpResourceIsSegment())
				ResGroupSegmentStatementStart();
			else
				ResGroupStatementStart(queryString);
			set_statement_setting(0);
		}
		resource_process_utility(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		statement_end();
		call_toplevel = save_call_toplevel;
	}
	PG_END_TRY();
}

/* ------------------------------------------------------------------------- */
/* rolresqueue and rolresgroup, through O10                                  */
/* ------------------------------------------------------------------------- */

static columnref_fallback_hook_type prev_columnref_fallback_hook = NULL;
static deparse_function_as_column_hook_type prev_deparse_function_as_column_hook = NULL;

/* The functions a name becomes a call of, found once per transaction */
static LocalTransactionId role_funcs_lxid = InvalidLocalTransactionId;
static Oid	role_queue_roles_oid = InvalidOid;
static Oid	role_queue_authid_oid = InvalidOid;
static Oid	role_group_roles_oid = InvalidOid;
static Oid	role_group_authid_oid = InvalidOid;
static Oid	activity_rsgid_oid = InvalidOid;
static Oid	activity_rsgname_oid = InvalidOid;

static Oid
lookup_row_function(const char *name, const char *rowtype)
{
	Oid			nsp = get_namespace_oid(GP_RESOURCE_SCHEMA, true);
	Oid			typid;
	Oid			argtypes[1];

	if (!OidIsValid(nsp))
		return InvalidOid;
	typid = TypenameGetTypid(rowtype);
	if (!OidIsValid(typid))
		return InvalidOid;
	argtypes[0] = typid;
	return LookupFuncName(list_make2(makeString(GP_RESOURCE_SCHEMA),
									 makeString(pstrdup(name))),
						  1, argtypes, true);
}

static void
lookup_role_funcs(void)
{
	if (role_funcs_lxid == MyProc->vxid.lxid)
		return;
	role_queue_roles_oid = lookup_row_function("rolresqueue", "pg_roles");
	role_queue_authid_oid = lookup_row_function("rolresqueue", "pg_authid");
	role_group_roles_oid = lookup_row_function("rolresgroup", "pg_roles");
	role_group_authid_oid = lookup_row_function("rolresgroup", "pg_authid");
	activity_rsgid_oid = lookup_row_function("rsgid", "pg_stat_activity");
	activity_rsgname_oid = lookup_row_function("rsgname", "pg_stat_activity");
	role_funcs_lxid = MyProc->vxid.lxid;
}

/* pg_roles' or pg_authid's entry of a query's range table, as the call wants */
static Oid
role_function_for(ParseNamespaceItem *nsitem, const char *name)
{
	RangeTblEntry *rte = nsitem->p_rte;
	char	   *relname;
	bool		queue = strcmp(name, "rolresqueue") == 0;

	if (rte->rtekind != RTE_RELATION ||
		get_rel_namespace(rte->relid) != PG_CATALOG_NAMESPACE)
		return InvalidOid;
	relname = get_rel_name(rte->relid);
	if (relname == NULL)
		return InvalidOid;
	if (strcmp(name, "rsgid") == 0 || strcmp(name, "rsgname") == 0)
	{
		if (strcmp(relname, "pg_stat_activity") != 0)
			return InvalidOid;
		return strcmp(name, "rsgid") == 0 ? activity_rsgid_oid : activity_rsgname_oid;
	}
	if (strcmp(relname, "pg_roles") == 0)
		return queue ? role_queue_roles_oid : role_group_roles_oid;
	if (strcmp(relname, "pg_authid") == 0)
		return queue ? role_queue_authid_oid : role_group_authid_oid;
	return InvalidOid;
}

static Node *
make_role_call(ParseState *pstate, ParseNamespaceItem *nsitem,
			   int sublevels_up, int location, Oid funcid)
{
	Var		   *var;
	FuncExpr   *fexpr;

	var = makeWholeRowVar(nsitem->p_rte, nsitem->p_rtindex, sublevels_up, true);
	var->location = location;
	markNullableIfNeeded(pstate, var);
	markVarForSelectPriv(pstate, var);

	fexpr = makeFuncExpr(funcid, get_func_rettype(funcid), list_make1(var),
						 InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	fexpr->location = location;
	return (Node *) fexpr;
}

static Node *
gp_resource_columnref_fallback(ParseState *pstate, ColumnRef *cref)
{
	int			nfields = list_length(cref->fields);
	Node	   *last = (Node *) llast(cref->fields);
	const char *name;
	int			levels_up = 0;

	if (prev_columnref_fallback_hook)
	{
		Node	   *node = prev_columnref_fallback_hook(pstate, cref);

		if (node != NULL)
			return node;
	}

	if (nfields > 3 || !IsA(last, String))
		return NULL;
	name = strVal(last);
	if (strcmp(name, "rolresqueue") != 0 && strcmp(name, "rolresgroup") != 0 &&
		strcmp(name, "rsgid") != 0 && strcmp(name, "rsgname") != 0)
		return NULL;
	lookup_role_funcs();

	if (nfields > 1)
	{
		char	   *nspname = nfields == 3 ? strVal(linitial(cref->fields)) : NULL;
		char	   *relname = strVal(list_nth(cref->fields, nfields - 2));
		ParseNamespaceItem *nsitem;
		Oid			funcid;

		nsitem = refnameNamespaceItem(pstate, nspname, relname, cref->location,
									  &levels_up);
		if (nsitem == NULL)
			return NULL;
		funcid = role_function_for(nsitem, name);
		if (!OidIsValid(funcid))
			return NULL;
		return make_role_call(pstate, nsitem, levels_up, cref->location, funcid);
	}

	/* unqualified: the nearest level with one such entry */
	for (ParseState *ps = pstate; ps != NULL; ps = ps->parentParseState)
	{
		ParseNamespaceItem *found = NULL;
		Oid			foundfunc = InvalidOid;

		foreach_ptr(ParseNamespaceItem, nsitem, ps->p_namespace)
		{
			Oid			funcid;

			if (!nsitem->p_cols_visible)
				continue;
			if (nsitem->p_lateral_only && !nsitem->p_lateral_ok)
				continue;
			funcid = role_function_for(nsitem, name);
			if (!OidIsValid(funcid))
				continue;
			if (found != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous", name),
						 parser_errposition(pstate, cref->location)));
			found = nsitem;
			foundfunc = funcid;
		}
		if (found != NULL)
			return make_role_call(pstate, found, levels_up, cref->location,
								  foundfunc);
		levels_up++;
	}
	return NULL;
}

static const char *
gp_resource_deparse_function_as_column(FuncExpr *expr)
{
	lookup_role_funcs();
	if (expr->funcid == role_queue_roles_oid || expr->funcid == role_queue_authid_oid)
		return "rolresqueue";
	if (expr->funcid == role_group_roles_oid || expr->funcid == role_group_authid_oid)
		return "rolresgroup";
	if (expr->funcid == activity_rsgid_oid)
		return "rsgid";
	if (expr->funcid == activity_rsgname_oid)
		return "rsgname";
	if (prev_deparse_function_as_column_hook)
		return prev_deparse_function_as_column_hook(expr);
	return NULL;
}

/* ------------------------------------------------------------------------- */

void
_PG_init(void)
{
	/*
	 * Shared memory, the hooks every statement goes through, and a label
	 * provider every backend that reads a definition needs: all only while
	 * preloading.
	 */
	CB_REQUIRE_PRELOAD("gp_resource");
	CB_REQUIRE_CORE("gp_resource");

	define_settings();
	MarkGUCPrefixReserved("gp_resource");
	MemProtInit();

	ResDefsRegisterProvider();
	ResQueueInit();
	ResGroupInit();

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = gp_resource_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = gp_resource_shmem_startup;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = gp_resource_ProcessUtility;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = gp_resource_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = gp_resource_ExecutorRun;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = gp_resource_ExecutorEnd;
	prev_explain_per_plan_hook = explain_per_plan_hook;
	explain_per_plan_hook = gp_resource_explain_per_plan;

	prev_columnref_fallback_hook = columnref_fallback_hook;
	columnref_fallback_hook = gp_resource_columnref_fallback;
	prev_deparse_function_as_column_hook = deparse_function_as_column_hook;
	deparse_function_as_column_hook = gp_resource_deparse_function_as_column;

	/* O26 carries a role's queue and group to its statement for the hook above */
	*find_rendezvous_variable(CB_RESOURCE_RENDEZVOUS) = (void *) &gp_resource_scheduler;

	/* the statement's setting, set again after a move (above) */
	GpDispatchAddSyncCallback(gp_resource_before_sync);

	prev_object_access_hook = object_access_hook;
	object_access_hook = gp_resource_object_access;

	/* the postmaster, before it forks anything: the groups' cgroups */
	if (!IsUnderPostmaster && IsResGroupEnabled())
		ResGroupCgroupInit();
}
