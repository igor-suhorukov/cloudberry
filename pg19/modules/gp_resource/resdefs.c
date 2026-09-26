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
 * resdefs.c
 *	  The definitions of resource queues and resource groups, and their DDL.
 *
 * Cloudberry keeps a queue in the shared catalogs pg_resqueue and
 * pg_resqueuecapability, a group in pg_resgroup and pg_resgroupcapability,
 * and which queue and group a role is in in pg_authid's rolresqueue and
 * rolresgroup.  An extension can make none of these, so, as decided on
 * 2026-09-25 (cloudberry.md, "The resource catalogs"):
 *
 *   - the definitions of each kind are one JSON label, provider
 *     "gp_resource", on a NOLOGIN role of that kind's own --
 *     gp_resource_queues and gp_resource_groups, which the extension's
 *     script makes or finds made -- as tag definitions are on
 *     gp_tag_definitions:
 *
 *       {"pg_default": {"oid": 6055, "active_statements": 20, "max_cost": -1,
 *                       "cost_overcommit": false, "min_cost": 0,
 *                       "priority": "medium", "memory_limit": "-1"}}
 *
 *     A role's label is the cluster's, so a queue defined in one database is
 *     one in every other, as pg_resqueue's row is, and pg_dumpall writes it
 *     with the roles.  Where no label is written yet, the definitions are the
 *     ones Cloudberry's initdb makes: the queue pg_default, and the groups
 *     default_group, admin_group and system_group, under Cloudberry's OIDs,
 *     which its tests print;
 *
 *   - a role's queue and group are the "gp" label keys resource_queue and
 *     resource_group on the role, by OID, as its profile is; a role with
 *     neither is in pg_default, and in admin_group if it is a superuser and
 *     default_group if not, which is what Cloudberry writes into the columns
 *     of a role made without one;
 *
 *   - pg_resourcetype, which is fixed, is this file's table below.
 *
 * pg_catalog has each of Cloudberry's catalogs by its name, as a view over a
 * function here (gp_resource--1.0.sql), and rolresqueue and rolresgroup are
 * names of pg_roles and pg_authid through O10, as gp_segment_id is of a
 * table (gp_resource.c).
 *
 * The statements -- CREATE, ALTER, DROP and COMMENT ON RESOURCE QUEUE and
 * RESOURCE GROUP -- are CALLs of the procedures here, which O26 writes them
 * as (gp_desugar.c), with Cloudberry's options in the order its grammar gave
 * them.  Each checks what Cloudberry's queue.c and resgroupcmds.c check, in
 * their words, rewrites its kind's label under a lock on the carrier role,
 * which serialises the statements of a kind as a catalog's row lock would,
 * and has the live copy in shared memory changed as the transaction commits
 * (resqueue.c, resgroup.c).  The label is sent to the segments as every
 * label is (gp_dispatch.c): a segment needs a group's limits for its
 * processes' cgroup, and a queue's priority.
 *
 * What it costs: every statement of a kind rewrites the whole label, and the
 * statements of a kind take turns, which statements this rare can afford; a
 * definition's OID is the module's, kept in the label, not a catalog's; and
 * a comment on a queue or a group is the definition's, not pg_shdescription's.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/commands/queue.c, resgroupcmds.c, and the resource queue
 *	  and group code of user.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "executor/spi.h"
#include "common/int.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_label.h"
#include "gp_fault.h"
#include "gp_resource.h"

/* Cloudberry's predefined role that may manage groups (pg_authid.dat) */
#define ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME	"pg_manage_resource_groups"

/* Cloudberry's defaults of CREATE RESOURCE GROUP (resgroupcmds.c) */
#define RESGROUP_DEFAULT_CONCURRENCY	20
#define RESGROUP_DEFAULT_CPU_WEIGHT		100
#define RESGROUP_MIN_CONCURRENCY		0
#define RESGROUP_MAX_CPU_MAX_PERCENT	100
#define RESGROUP_MIN_CPU_MAX_PERCENT	1
#define RESGROUP_MIN_CPU_WEIGHT			1
#define RESGROUP_MAX_CPU_WEIGHT			500
#define RESGROUP_MIN_MEMORY_QUOTA		0
#define RESGROUP_DEFAULT_MEMORY_QUOTA	(-1)
#define RESGROUP_MIN_MIN_COST			0
#define CPU_MAX_PERCENT_DISABLED		(-1)

/* ------------------------------------------------------------------------- */
/* pg_resourcetype                                                           */
/* ------------------------------------------------------------------------- */

/* Cloudberry's pg_resourcetype.dat, which nothing changes */
static const struct
{
	Oid			oid;
	const char *resname;
	int16		restypid;
	bool		resrequired;
	bool		reshasdefault;
	bool		reshasdisable;
	const char *resdefaultsetting;
	const char *resdisabledsetting; /* NULL: none */
}			resource_types[PG_RESRCTYPE_COUNT] = {
	{6454, "active_statements", 1, false, true, true, "-1", "-1"},
	{6455, "max_cost", 2, false, true, true, "-1", "-1"},
	{6456, "min_cost", 3, false, true, true, "-1", "0"},
	{6457, "cost_overcommit", 4, false, true, true, "-1", "-1"},
	{6458, "priority", 5, false, true, false, "medium", NULL},
	{6459, "memory_limit", 6, false, true, true, "-1", "-1"},
};

/* The type of a resource's name, or 0: GetResourceTypeByName() */
static int
resource_type_by_name(const char *name)
{
	for (int i = 0; i < PG_RESRCTYPE_COUNT; i++)
	{
		if (strcmp(resource_types[i].resname, name) == 0)
			return resource_types[i].restypid;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* The carrier roles and their labels                                        */
/* ------------------------------------------------------------------------- */

static Oid
carrier_role(const char *name)
{
	return get_role_oid(name, true);
}

bool
ResDefsIsCarrierRole(Oid roleid)
{
	return OidIsValid(roleid) &&
		(roleid == carrier_role(GP_RESQUEUE_ROLE) ||
		 roleid == carrier_role(GP_RESGROUP_ROLE));
}

/*
 * The one statement of a kind that may change its definitions: whoever gets
 * here first rewrites the label, the next waits and reads what it wrote, as
 * tag.c's statements do.  The lock's acquisition brings the catalog snapshot
 * up to date.
 */
static Oid
lock_definitions(const char *carrier)
{
	Oid			role = carrier_role(carrier);

	if (!OidIsValid(role))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\", which carries the resource manager's definitions, does not exist",
						carrier),
				 errhint("The \"%s\" extension makes it: create the extension.",
						 GP_RESOURCE_SCHEMA)));
	LockSharedObject(AuthIdRelationId, role, 0, ShareUpdateExclusiveLock);
	return role;
}

/* A JSON value of an object, as text, or NULL where it is absent or null */
static char *
json_text(JsonbContainer *obj, const char *key)
{
	JsonbValue	buf;
	JsonbValue *v = getKeyJsonValueFromContainer(obj, key, strlen(key), &buf);

	if (v == NULL || v->type == jbvNull)
		return NULL;
	if (v->type == jbvString)
		return pnstrdup(v->val.string.val, v->val.string.len);
	if (v->type == jbvNumeric)
		return DatumGetCString(DirectFunctionCall1(numeric_out,
												   NumericGetDatum(v->val.numeric)));
	if (v->type == jbvBool)
		return pstrdup(v->val.boolean ? "true" : "false");
	return NULL;
}

static double
json_number(JsonbContainer *obj, const char *key, double dflt)
{
	char	   *s = json_text(obj, key);

	if (s == NULL)
		return dflt;
	return float8in_internal(s, NULL, "double precision", s, NULL);
}

static bool
json_bool(JsonbContainer *obj, const char *key)
{
	char	   *s = json_text(obj, key);

	return s != NULL && strcmp(s, "true") == 0;
}

/* Every definition in a carrier's label, as (name, object) pairs */
typedef void (*DefReader) (const char *name, JsonbContainer *obj, void *arg);

static bool
read_label(const char *carrier, DefReader reader, void *arg)
{
	Oid			role = carrier_role(carrier);
	ObjectAddress addr;
	char	   *label;
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	char	   *name = NULL;

	if (!OidIsValid(role))
		return false;
	ObjectAddressSet(addr, AuthIdRelationId, role);
	label = GetSecurityLabel(&addr, GP_RESOURCE_PROVIDER);
	if (label == NULL)
		return false;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(label)));
	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY)
			name = pnstrdup(v.val.string.val, v.val.string.len);
		else if (tok == WJB_VALUE && name != NULL && v.type == jbvBinary)
		{
			reader(name, v.val.binary.data, arg);
			name = NULL;
		}
	}
	return true;
}

static void
push_string(JsonbInState *state, JsonbIteratorToken tok, const char *s)
{
	JsonbValue	v;

	v.type = jbvString;
	v.val.string.len = strlen(s);
	v.val.string.val = unconstify(char *, s);
	pushJsonbValue(state, tok, &v);
}

static void
push_key_string(JsonbInState *state, const char *key, const char *s)
{
	JsonbValue	v;

	push_string(state, WJB_KEY, key);
	if (s == NULL)
	{
		v.type = jbvNull;
		pushJsonbValue(state, WJB_VALUE, &v);
	}
	else
		push_string(state, WJB_VALUE, s);
}

static void
push_key_number(JsonbInState *state, const char *key, double value)
{
	JsonbValue	v;
	char	   *s = float8out_internal(value);

	push_string(state, WJB_KEY, key);
	v.type = jbvNumeric;
	v.val.numeric = DatumGetNumeric(DirectFunctionCall3(numeric_in,
														CStringGetDatum(s),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
	pushJsonbValue(state, WJB_VALUE, &v);
}

static void
push_key_bool(JsonbInState *state, const char *key, bool value)
{
	JsonbValue	v;

	push_string(state, WJB_KEY, key);
	v.type = jbvBool;
	v.val.boolean = value;
	pushJsonbValue(state, WJB_VALUE, &v);
}

/* Write a carrier's label, and have the segments told */
static void
write_label(Oid role, Jsonb *jb)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, role);
	SetSecurityLabel(&addr, GP_RESOURCE_PROVIDER,
					 JsonbToCString(NULL, &jb->root, VARSIZE(jb)));
	GpDispatchNoteLabelOf(&addr, GP_RESOURCE_PROVIDER);
	CommandCounterIncrement();
}

/* By OID, which is the order the definitions were made in */
static int
cmp_queue_oid(const ListCell *a, const ListCell *b)
{
	return pg_cmp_u32(((const ResQueueDef *) lfirst(a))->oid,
					  ((const ResQueueDef *) lfirst(b))->oid);
}

static int
cmp_group_oid(const ListCell *a, const ListCell *b)
{
	return pg_cmp_u32(((const ResGroupDef *) lfirst(a))->oid,
					  ((const ResGroupDef *) lfirst(b))->oid);
}

/* ------------------------------------------------------------------------- */
/* Queues                                                                    */
/* ------------------------------------------------------------------------- */

/* Cloudberry's pg_default, as its initdb makes it */
static ResQueueDef *
default_queue(void)
{
	ResQueueDef *d = palloc0(sizeof(ResQueueDef));

	d->name = pstrdup(DEFAULT_RESQUEUE_NAME);
	d->oid = DEFAULTRESQUEUE_OID;
	d->active_statements = 20;
	d->max_cost = -1;
	d->cost_overcommit = false;
	d->min_cost = 0;
	d->priority = pstrdup("medium");
	d->memory_limit = pstrdup("-1");
	return d;
}

static void
read_queue(const char *name, JsonbContainer *obj, void *arg)
{
	List	  **defs = (List **) arg;
	ResQueueDef *d = palloc0(sizeof(ResQueueDef));

	d->name = pstrdup(name);
	d->oid = (Oid) json_number(obj, "oid", InvalidOid);
	d->active_statements = (float4) json_number(obj, "active_statements", -1);
	d->max_cost = (float4) json_number(obj, "max_cost", -1);
	d->cost_overcommit = json_bool(obj, "cost_overcommit");
	d->min_cost = (float4) json_number(obj, "min_cost", 0);
	d->priority = json_text(obj, "priority");
	d->memory_limit = json_text(obj, "memory_limit");
	d->comment = json_text(obj, "comment");
	*defs = lappend(*defs, d);
}

List *
ResQueueDefsLoad(void)
{
	List	   *defs = NIL;

	if (!read_label(GP_RESQUEUE_ROLE, read_queue, &defs))
		defs = list_make1(default_queue());
	list_sort(defs, cmp_queue_oid);
	return defs;
}

static void
store_queues(Oid role, List *defs)
{
	JsonbInState state = {0};

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	foreach_ptr(ResQueueDef, d, defs)
	{
		push_string(&state, WJB_KEY, d->name);
		pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
		push_key_number(&state, "oid", d->oid);
		push_key_number(&state, "active_statements", d->active_statements);
		push_key_number(&state, "max_cost", d->max_cost);
		push_key_bool(&state, "cost_overcommit", d->cost_overcommit);
		push_key_number(&state, "min_cost", d->min_cost);
		push_key_string(&state, "priority", d->priority);
		push_key_string(&state, "memory_limit", d->memory_limit);
		if (d->comment != NULL)
			push_key_string(&state, "comment", d->comment);
		pushJsonbValue(&state, WJB_END_OBJECT, NULL);
	}
	pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	write_label(role, JsonbValueToJsonb(state.result));
	ResQueueDefsChanged();
}

ResQueueDef *
ResQueueDefFind(List *defs, const char *name)
{
	foreach_ptr(ResQueueDef, d, defs)
	{
		if (strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
}

ResQueueDef *
ResQueueDefByOid(List *defs, Oid oid)
{
	foreach_ptr(ResQueueDef, d, defs)
	{
		if (d->oid == oid)
			return d;
	}
	return NULL;
}

Oid
ResQueueOidByName(const char *name, bool missing_ok)
{
	ResQueueDef *d = ResQueueDefFind(ResQueueDefsLoad(), name);

	if (d == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist", name)));
	return d != NULL ? d->oid : InvalidOid;
}

char *
ResQueueNameByOid(Oid oid)
{
	ResQueueDef *d = ResQueueDefByOid(ResQueueDefsLoad(), oid);

	return d != NULL ? d->name : NULL;
}

/* A queue's MEMORY_LIMIT in kB: ResourceQueueGetMemoryLimit()'s reading */
int64
ResQueueDefMemoryLimitKB(const ResQueueDef *def)
{
	int			kb;

	if (def->memory_limit == NULL ||
		!parse_int(def->memory_limit, &kb, GUC_UNIT_KB, NULL))
		return -1;
	return kb;
}

/* ------------------------------------------------------------------------- */
/* Groups                                                                    */
/* ------------------------------------------------------------------------- */

static ResGroupDef *
make_group(const char *name, Oid oid, int concurrency, int cpu)
{
	ResGroupDef *d = palloc0(sizeof(ResGroupDef));

	d->name = pstrdup(name);
	d->oid = oid;
	d->concurrency = concurrency;
	d->cpu_max_percent = cpu;
	d->cpu_weight = RESGROUP_DEFAULT_CPU_WEIGHT;
	d->cpuset = pstrdup("-1");
	d->memory_quota = RESGROUP_DEFAULT_MEMORY_QUOTA;
	d->min_cost = 500;
	d->io_limit = pstrdup("-1");
	return d;
}

/* Cloudberry's three groups, as pg_resgroup.dat and its capabilities make them */
static List *
default_groups(void)
{
	return list_make3(make_group("default_group", DEFAULTRESGROUP_OID, 20, 20),
					  make_group("admin_group", ADMINRESGROUP_OID, 10, 10),
					  make_group("system_group", SYSTEMRESGROUP_OID, 0, 10));
}

static void
read_group(const char *name, JsonbContainer *obj, void *arg)
{
	List	  **defs = (List **) arg;
	ResGroupDef *d = palloc0(sizeof(ResGroupDef));

	d->name = pstrdup(name);
	d->oid = (Oid) json_number(obj, "oid", InvalidOid);
	d->concurrency = (int) json_number(obj, "concurrency", 20);
	d->cpu_max_percent = (int) json_number(obj, "cpu_max_percent", -1);
	d->cpu_weight = (int) json_number(obj, "cpu_weight", 100);
	d->cpuset = json_text(obj, "cpuset");
	if (d->cpuset == NULL)
		d->cpuset = pstrdup("-1");
	d->memory_quota = (int) json_number(obj, "memory_quota", -1);
	d->min_cost = (int) json_number(obj, "min_cost", 0);
	d->io_limit = json_text(obj, "io_limit");
	if (d->io_limit == NULL)
		d->io_limit = pstrdup("-1");
	d->comment = json_text(obj, "comment");
	*defs = lappend(*defs, d);
}

List *
ResGroupDefsLoad(void)
{
	List	   *defs = NIL;

	if (!read_label(GP_RESGROUP_ROLE, read_group, &defs))
		defs = default_groups();
	list_sort(defs, cmp_group_oid);
	return defs;
}

static void
store_groups(Oid role, List *defs)
{
	JsonbInState state = {0};

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	foreach_ptr(ResGroupDef, d, defs)
	{
		push_string(&state, WJB_KEY, d->name);
		pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
		push_key_number(&state, "oid", d->oid);
		push_key_number(&state, "concurrency", d->concurrency);
		push_key_number(&state, "cpu_max_percent", d->cpu_max_percent);
		push_key_number(&state, "cpu_weight", d->cpu_weight);
		push_key_string(&state, "cpuset", d->cpuset);
		push_key_number(&state, "memory_quota", d->memory_quota);
		push_key_number(&state, "min_cost", d->min_cost);
		push_key_string(&state, "io_limit", d->io_limit);
		if (d->comment != NULL)
			push_key_string(&state, "comment", d->comment);
		pushJsonbValue(&state, WJB_END_OBJECT, NULL);
	}
	pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	write_label(role, JsonbValueToJsonb(state.result));
	ResGroupDefsChanged();
}

ResGroupDef *
ResGroupDefFind(List *defs, const char *name)
{
	foreach_ptr(ResGroupDef, d, defs)
	{
		if (strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
}

ResGroupDef *
ResGroupDefByOid(List *defs, Oid oid)
{
	foreach_ptr(ResGroupDef, d, defs)
	{
		if (d->oid == oid)
			return d;
	}
	return NULL;
}

Oid
ResGroupOidByName(const char *name, bool missing_ok)
{
	ResGroupDef *d = ResGroupDefFind(ResGroupDefsLoad(), name);

	if (d == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource group \"%s\" does not exist", name)));
	return d != NULL ? d->oid : InvalidOid;
}

/* GetResGroupNameForId(): "unknown" where there is none */
char *
ResGroupNameByOid(Oid oid)
{
	ResGroupDef *d = ResGroupDefByOid(ResGroupDefsLoad(), oid);

	return d != NULL ? d->name : "unknown";
}

/* ------------------------------------------------------------------------- */
/* A role's queue and group                                                  */
/* ------------------------------------------------------------------------- */

static Oid
role_label_oid(Oid roleid, GpLabelKey key)
{
	ObjectAddress addr;
	char	   *value;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	value = GpLabelGet(&addr, key);
	if (value == NULL)
		return InvalidOid;
	return (Oid) strtoul(value, NULL, 10);
}

Oid
GetResQueueForRole(Oid roleid)
{
	Oid			queueid = role_label_oid(roleid, GP_LABEL_resource_queue);

	return OidIsValid(queueid) ? queueid : DEFAULTRESQUEUE_OID;
}

Oid
GetResGroupForRole(Oid roleid)
{
	Oid			groupid = role_label_oid(roleid, GP_LABEL_resource_group);

	if (OidIsValid(groupid))
		return groupid;
	return superuser_arg(roleid) ? ADMINRESGROUP_OID : DEFAULTRESGROUP_OID;
}

static void
set_role_key(Oid roleid, GpLabelKey key, Oid value)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, AuthIdRelationId, roleid);
	GpLabelSet(&addr, key, OidIsValid(value) ? psprintf("%u", value) : NULL);
}

/*
 * CREATE and ALTER ROLE ... RESOURCE QUEUE q, as user.c takes it: q must
 * exist; "none" is refused at CREATE and means pg_default at ALTER, which
 * Cloudberry says with a NOTICE where queues are on.  The role's superuser
 * attribute is the one the statement left it with.
 */
void
ResDefsAssignRoleQueue(Oid roleid, const char *queue, bool creating)
{
	bool		issuper = superuser_arg(roleid);
	Oid			queueid;

	if (strcmp(queue, "none") == 0)
	{
		if (creating)
			ereport(ERROR,
					(errcode(ERRCODE_RESERVED_NAME),
					 errmsg("resource queue name \"%s\" is reserved", queue)));
		if (!issuper && IsResQueueEnabled() && GpResourceIsDispatcher())
			ereport(NOTICE,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("resource queue required -- using default resource queue \"%s\"",
							DEFAULT_RESQUEUE_NAME)));
		queue = DEFAULT_RESQUEUE_NAME;
	}

	queueid = ResQueueOidByName(queue, true);
	if (!OidIsValid(queueid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist", queue)));

	if (!IsResQueueEnabled() && !issuper)
		ereport(WARNING,
				(errmsg("resource queue is disabled"),
				 errhint(creating ? "To enable set gp_resource_manager=queue"
						 : "To enable set gp_resource_manager=queue.")));

	set_role_key(roleid, GP_LABEL_resource_queue,
				 queueid == DEFAULTRESQUEUE_OID ? InvalidOid : queueid);
}

/*
 * CREATE and ALTER ROLE ... RESOURCE GROUP g: admin_group only for a
 * superuser, never system_group, "none" the default for the role's kind.
 */
void
ResDefsAssignRoleGroup(Oid roleid, const char *group, bool creating)
{
	bool		issuper = superuser_arg(roleid);
	Oid			groupid;

	if (!creating && strcmp(group, "none") == 0)
	{
		group = issuper ? "admin_group" : "default_group";
		if (IsResGroupEnabled() && GpResourceIsDispatcher())
			ereport(NOTICE,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("resource group required -- using default resource group \"%s\"",
							group)));
	}

	groupid = ResGroupOidByName(group, false);

	if (groupid == ADMINRESGROUP_OID && !issuper)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can be assigned to admin resgroup")));
	if (groupid == SYSTEMRESGROUP_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("assigning to system resgroup is not allowed")));

	if (!IsResGroupEnabled() && GpResourceIsDispatcher())
		ereport(WARNING,
				(errmsg("resource group is disabled"),
				 errhint("To enable set gp_resource_manager=group")));

	set_role_key(roleid, GP_LABEL_resource_group, groupid);
}

/* ------------------------------------------------------------------------- */
/* Options, as O26 carries them                                              */
/* ------------------------------------------------------------------------- */

/*
 * A statement's options, as O26 wrote them into the CALL: one text each,
 * in the order Cloudberry's grammar lists them, "name" alone or
 * "name=k:value", k saying what the value was -- i an integer, f a number
 * with a fraction or exponent, s a string, w a word (a type name, to
 * Cloudberry's grammar) -- which is what decides the node Cloudberry's
 * defGet*() were given, and so their answers.  A NULL option, which O26
 * never writes, is refused, as deconstruct_array() refuses one given no
 * array for NULLs.
 */
static List *
options_from_array(ArrayType *arr)
{
	Datum	   *elems;
	int			n;
	List	   *options = NIL;

	deconstruct_array_builtin(arr, TEXTOID, &elems, NULL, &n);
	for (int i = 0; i < n; i++)
	{
		char	   *item = TextDatumGetCString(elems[i]);
		char	   *eq = strchr(item, '=');
		Node	   *arg = NULL;

		if (eq != NULL)
		{
			char		kind = eq[1];
			char	   *value = eq + 3;

			if (kind == '\0' || eq[2] != ':')
				elog(ERROR, "O26: unrecognized option \"%s\"", item);
			*eq = '\0';
			switch (kind)
			{
				case 'i':
					{
						int64		v = strtoi64(value, NULL, 10);

						if (v >= INT_MIN && v <= INT_MAX)
							arg = (Node *) makeInteger((int) v);
						else
							arg = (Node *) makeFloat(pstrdup(value));
					}
					break;
				case 'f':
					arg = (Node *) makeFloat(pstrdup(value));
					break;
				case 's':
					arg = (Node *) makeString(pstrdup(value));
					break;
				case 'w':
					arg = (Node *) makeTypeName(pstrdup(value));
					break;
				default:
					elog(ERROR, "O26: unrecognized option kind in \"%s\"", item);
			}
		}
		options = lappend(options, makeDefElem(pstrdup(item), arg, -1));
	}
	return options;
}

/*
 * A statement's name and options, as its CALL passes them: never NULL from
 * O26, and refused NULL from a CALL of the user's own, before either is read.
 */
static char *
name_arg(FunctionCallInfo fcinfo, const char *what)
{
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s name must not be null", what)));
	return NameStr(*PG_GETARG_NAME(0));
}

static List *
options_arg(FunctionCallInfo fcinfo)
{
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("options array must not be null")));
	return options_from_array(PG_GETARG_ARRAYTYPE_P(1));
}

/* Cloudberry's statements that may not run in a transaction block */
static void
prevent_in_transaction_block(const char *stmt)
{
	/*
	 * PreventInTransactionBlock(), of the CALL the statement became: not
	 * top-level where a function ran it (GpResourceCallIsTopLevel()).
	 */
	if (IsTransactionBlock())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("%s cannot run inside a transaction block", stmt)));
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("%s cannot run inside a subtransaction", stmt)));
	if (!GpResourceCallIsTopLevel())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("%s cannot be executed from a function", stmt)));
}

/* ------------------------------------------------------------------------- */
/* CREATE, ALTER, DROP and COMMENT ON RESOURCE QUEUE                         */
/* ------------------------------------------------------------------------- */

static bool
valid_priority(const char *s)
{
	static const char *const priorities[] = {"MAX", "HIGH", "MEDIUM", "LOW", "MIN"};

	for (int i = 0; i < lengthof(priorities); i++)
	{
		if (pg_strcasecmp(priorities[i], s) == 0)
			return true;
	}
	return false;
}

/* ValidateResqueueCapabilityEntry(), for the two kinds there are */
static void
validate_capability(int restypid, const char *setting)
{
	if (restypid == PG_RESRCTYPE_PRIORITY)
	{
		if (!valid_priority(setting))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Invalid parameter value \"%s\" for resource type \"%s\"",
							setting, "PRIORITY")));
	}
	else if (restypid == PG_RESRCTYPE_MEMORY_LIMIT)
	{
		int			kb;

		if (!parse_int(setting, &kb, GUC_UNIT_KB, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Invalid parameter value \"%s\" for resource type \"%s\". Value must be in kB, MB or GB.",
							setting, "MEMORY_LIMIT")));
		if (kb != -1 && kb < MIN_RESOURCEQUEUE_MEMORY_LIMIT_KB)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Invalid parameter value \"%s\" for resource type \"%s\". Value must be at least %dkB",
							setting, "MEMORY_LIMIT", MIN_RESOURCEQUEUE_MEMORY_LIMIT_KB)));
	}
}

/*
 * The WITH and WITHOUT items after "withliststart", as
 * AlterResqueueCapabilityEntry() takes them: the four thresholds were read
 * already; each other item must name a resource type; WITHOUT sets it to
 * what turns it off, where it has such a value; and CREATE gives each type
 * with a default the default where it was not named.
 */
static void
apply_capabilities(ResQueueDef *def, List *options, ListCell *initcell,
				   bool creating)
{
	bool		without = false;
	List	   *dupcheck = NIL;
	ListCell   *lc;

	for_each_cell(lc, options, lnext(options, initcell))
	{
		DefElem    *defel = lfirst_node(DefElem, lc);
		int			restypid;
		char	   *setting;

		if (!without && strcmp(defel->defname, "withoutliststart") == 0)
		{
			without = true;
			continue;
		}
		if (strcmp(defel->defname, "active_statements") == 0 ||
			strcmp(defel->defname, "max_cost") == 0 ||
			strcmp(defel->defname, "cost_overcommit") == 0 ||
			strcmp(defel->defname, "min_cost") == 0)
			continue;

		restypid = resource_type_by_name(defel->defname);
		if (restypid == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" is not a valid resource type",
							defel->defname)));

		if (!without)
			setting = defGetString(defel);
		else
		{
			if (!resource_types[restypid - 1].reshasdisable)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("option \"%s\" cannot be disabled",
								defel->defname)));
			setting = pstrdup(resource_types[restypid - 1].resdisabledsetting);
		}

		if (list_member(dupcheck, makeString(defel->defname)))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant option for \"%s\"",
							defel->defname)));
		dupcheck = lappend(dupcheck, makeString(defel->defname));

		validate_capability(restypid, setting);
		if (restypid == PG_RESRCTYPE_PRIORITY)
			def->priority = setting;
		else if (restypid == PG_RESRCTYPE_MEMORY_LIMIT)
			def->memory_limit = setting;
	}

	if (creating)
	{
		if (def->priority == NULL)
			def->priority = pstrdup(resource_types[PG_RESRCTYPE_PRIORITY - 1].resdefaultsetting);
		if (def->memory_limit == NULL)
			def->memory_limit = pstrdup(resource_types[PG_RESRCTYPE_MEMORY_LIMIT - 1].resdefaultsetting);
	}
}

/* The threshold options, as CreateQueue() and AlterQueue() read them */
typedef struct QueueOptions
{
	DefElem    *active;
	DefElem    *cost;
	DefElem    *overcommit;
	DefElem    *ignore;
	ListCell   *withlist;		/* the "withliststart" item */
	bool		with;
	bool		without;
	int			numopts;
} QueueOptions;

static void
read_queue_options(List *options, bool altering, QueueOptions *qo)
{
	ListCell   *option;

	memset(qo, 0, sizeof(*qo));
	foreach(option, options)
	{
		DefElem    *defel = lfirst_node(DefElem, option);
		DefElem   **slot = NULL;

		if (strcmp(defel->defname, "active_statements") == 0)
			slot = &qo->active;
		else if (strcmp(defel->defname, "max_cost") == 0)
			slot = &qo->cost;
		else if (strcmp(defel->defname, "cost_overcommit") == 0)
			slot = &qo->overcommit;
		else if (strcmp(defel->defname, "min_cost") == 0)
			slot = &qo->ignore;

		if (slot != NULL)
		{
			if (*slot != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			/* WITHOUT (x): x as it is when turned off (AlterQueue()) */
			if (qo->without)
			{
				if (slot == &qo->active)
					defel = makeDefElem("active_statements",
										(Node *) makeFloat("-1"), -1);
				else if (slot == &qo->cost)
					defel = makeDefElem("max_cost",
										(Node *) makeInteger(-1), -1);
				else if (slot == &qo->overcommit)
					defel = makeDefElem("cost_overcommit",
										(Node *) makeInteger(0), -1);
				else
					defel = makeDefElem("min_cost", (Node *) makeFloat("0"), -1);
			}
			*slot = defel;
			qo->numopts++;
		}
		else if (strcmp(defel->defname, "withliststart") == 0)
		{
			if (qo->with)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("option \"%s\" is not a valid resource type",
								defel->defname)));
			qo->withlist = option;
			qo->with = true;
		}
		else if (altering && strcmp(defel->defname, "withoutliststart") == 0)
		{
			if (!qo->with || qo->without)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("option \"%s\" is not a valid resource type",
								defel->defname)));
			qo->without = true;
		}
		else
		{
			/* the WITH list's other items, read with the capabilities */
			if (!qo->with)
				elog(ERROR, "option \"%s\" not recognized", defel->defname);
			qo->numopts++;
		}
	}
}

/* The thresholds' range checks; "create" words the cost check as CREATE does */
static void
check_queue_thresholds(QueueOptions *qo, ResQueueDef *def, bool creating)
{
	if (qo->active)
	{
		double		v = (double) defGetInt64(qo->active);

		if (!(v == INVALID_RES_LIMIT_THRESHOLD || v > 0))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("active threshold cannot be less than %d or equal to 0",
							INVALID_RES_LIMIT_THRESHOLD)));
		def->active_statements = (float4) v;
	}
	if (qo->cost)
	{
		double		v = defGetNumeric(qo->cost);

		if (!(v == INVALID_RES_LIMIT_THRESHOLD || v > 0))
		{
			if (creating)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cost threshold cannot be less than %d or equal to 0",
								INVALID_RES_LIMIT_THRESHOLD)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cost threshold must be equal to %d or greater than 0",
								INVALID_RES_LIMIT_THRESHOLD)));
		}
		def->max_cost = (float4) v;
	}
	if (qo->overcommit)
		def->cost_overcommit = defGetBoolean(qo->overcommit);
	if (qo->ignore)
	{
		double		v = defGetNumeric(qo->ignore);

		if (!(v == INVALID_RES_LIMIT_THRESHOLD || v >= 0))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("min_cost threshold cannot be negative")));
		def->min_cost = (float4) v;
	}
}

static void
warn_queues_disabled(void)
{
	ereport(WARNING,
			(errmsg("resource queue is disabled"),
			 errhint("To enable set gp_resource_manager=queue")));
}

PG_FUNCTION_INFO_V1(gp_resource_create_queue);
PG_FUNCTION_INFO_V1(gp_resource_alter_queue);
PG_FUNCTION_INFO_V1(gp_resource_drop_queue);
PG_FUNCTION_INFO_V1(gp_resource_comment_queue);

/* CALL gp_resource.create_resource_queue(name, options): CreateQueue() */
Datum
gp_resource_create_queue(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource queue");
	List	   *options = options_arg(fcinfo);
	QueueOptions qo;
	ResQueueDef *def;
	List	   *defs;
	Oid			role;

	prevent_in_transaction_block("CREATE RESOURCE QUEUE");

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create resource queues")));

	/* CREATE's grammar puts "withliststart" after the THRESHOLD options */
	read_queue_options(options, false, &qo);

	def = palloc0(sizeof(ResQueueDef));
	def->name = pstrdup(name);
	def->active_statements = INVALID_RES_LIMIT_THRESHOLD;
	def->max_cost = INVALID_RES_LIMIT_THRESHOLD;
	def->cost_overcommit = false;
	def->min_cost = 0;
	check_queue_thresholds(&qo, def, true);

	if (!qo.active && !qo.cost)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("at least one threshold (\"ACTIVE_STATEMENTS\", \"MAX_COST\") must be specified")));
	if (def->active_statements == INVALID_RES_LIMIT_THRESHOLD &&
		def->max_cost == INVALID_RES_LIMIT_THRESHOLD)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("the value for at least one threshold (\"ACTIVE_STATEMENTS\", \"MAX_COST\") must be different from no limit (%d)",
						INVALID_RES_LIMIT_THRESHOLD)));
	if (strcmp(name, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("resource queue name \"%s\" is reserved", name)));

	role = lock_definitions(GP_RESQUEUE_ROLE);
	defs = ResQueueDefsLoad();
	if (ResQueueDefFind(defs, name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("resource queue \"%s\" already exists", name)));

	/*
	 * An OID no queue has: the definitions are no catalog whose unique
	 * index would say so, and a restore of pg_dumpall's output brings back
	 * queues whose OIDs another cluster's counter gave -- the roles' labels
	 * name them by those.
	 */
	do
		def->oid = GetNewObjectId();
	while (ResQueueDefByOid(defs, def->oid) != NULL);
	if (qo.with)
		apply_capabilities(def, options, qo.withlist, true);
	else
	{
		def->priority = pstrdup("medium");
		def->memory_limit = pstrdup("-1");
	}

	if (GpResourceIsDispatcher())
	{
		if (IsResQueueEnabled())
		{
			if (!ResQueueCanCreate())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
						 errmsg("insufficient resource queues available"),
						 errhint("Increase max_resource_queues")));
		}
		else
			warn_queues_disabled();
	}

	store_queues(role, lappend(defs, def));
	PG_RETURN_VOID();
}

/* CALL gp_resource.alter_resource_queue(name, options): AlterQueue() */
Datum
gp_resource_alter_queue(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource queue");
	List	   *options = options_arg(fcinfo);
	QueueOptions qo;
	ResQueueDef *def;
	ResQueueDef check;
	List	   *defs;
	Oid			role;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter resource queues")));

	read_queue_options(options, true, &qo);

	/* the ranges first, as AlterQueue() checks them before the queue */
	memset(&check, 0, sizeof(check));
	check_queue_thresholds(&qo, &check, false);

	if (!qo.active && !qo.cost && !qo.overcommit && !qo.ignore &&
		!(qo.with && qo.withlist != NULL && lnext(options, qo.withlist) != NULL &&
		  strcmp(lfirst_node(DefElem, lnext(options, qo.withlist))->defname,
				 "withoutliststart") != 0) &&
		!(qo.with && qo.withlist != NULL && lnext(options, qo.withlist) != NULL &&
		  lnext(options, lnext(options, qo.withlist)) != NULL))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("at least one threshold, overcommit or ignore limit must be specified")));

	role = lock_definitions(GP_RESQUEUE_ROLE);
	defs = ResQueueDefsLoad();
	def = ResQueueDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist", name)));

	check_queue_thresholds(&qo, def, false);

	if (def->active_statements == INVALID_RES_LIMIT_THRESHOLD &&
		def->max_cost == INVALID_RES_LIMIT_THRESHOLD)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the value for at least one threshold (\"ACTIVE_STATEMENTS\", \"MAX_COST\") must be different from no limit (%d)",
						INVALID_RES_LIMIT_THRESHOLD)));

	if (qo.with)
		apply_capabilities(def, options, qo.withlist, false);

	if (GpResourceIsDispatcher())
	{
		if (IsResQueueEnabled())
			ResQueueCheckAlter(def->oid, def);
		else
			warn_queues_disabled();
	}

	store_queues(role, defs);
	PG_RETURN_VOID();
}

/* Does any role have this queue?  pg_authid's rolresqueue index, in labels */
static bool
queue_has_roles(Oid queueid)
{
	bool		found = false;
	char	   *query;
	int			ret;

	query = psprintf("SELECT 1 FROM pg_catalog.pg_shseclabel s"
					 " WHERE s.classoid = %u AND s.provider = 'gp'"
					 " AND s.label ~ '(^|,)resource_queue=%u(,|$)' LIMIT 1",
					 AuthIdRelationId, queueid);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	ret = SPI_execute(query, true, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute failed: %d", ret);
	found = SPI_processed > 0;
	SPI_finish();
	return found;
}

static bool
group_has_roles(Oid groupid)
{
	bool		found = false;
	char	   *query;
	int			ret;

	query = psprintf("SELECT 1 FROM pg_catalog.pg_shseclabel s"
					 " WHERE s.classoid = %u AND s.provider = 'gp'"
					 " AND s.label ~ '(^|,)resource_group=%u(,|$)' LIMIT 1",
					 AuthIdRelationId, groupid);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	ret = SPI_execute(query, true, 1);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute failed: %d", ret);
	found = SPI_processed > 0;
	SPI_finish();
	return found;
}

/* CALL gp_resource.drop_resource_queue(name): DropQueue() */
Datum
gp_resource_drop_queue(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource queue");
	ResQueueDef *def;
	List	   *defs;
	Oid			role;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop resource queues")));

	role = lock_definitions(GP_RESQUEUE_ROLE);
	defs = ResQueueDefsLoad();
	def = ResQueueDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist", name)));

	if (queue_has_roles(def->oid))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("resource queue \"%s\" is used by at least one role",
						name)));
	if (def->oid == DEFAULTRESQUEUE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop default resource queue \"%s\"", name)));

	if (GpResourceIsDispatcher())
	{
		if (IsResQueueEnabled())
			ResQueueCheckDrop(def->oid);
		else
			warn_queues_disabled();
	}

	store_queues(role, list_delete_ptr(defs, def));
	PG_RETURN_VOID();
}

/* CALL gp_resource.comment_on_resource_queue(name, comment) */
Datum
gp_resource_comment_queue(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource queue");
	ResQueueDef *def;
	List	   *defs;
	Oid			role;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to comment on resource queue")));

	role = lock_definitions(GP_RESQUEUE_ROLE);
	defs = ResQueueDefsLoad();
	def = ResQueueDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource queue \"%s\" does not exist", name)));
	def->comment = PG_ARGISNULL(1) || VARSIZE_ANY_EXHDR(PG_GETARG_TEXT_PP(1)) == 0
		? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	store_queues(role, defs);
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------- */
/* CREATE, ALTER, DROP and COMMENT ON RESOURCE GROUP                         */
/* ------------------------------------------------------------------------- */

static bool
has_privs_of_manage_resource_groups(void)
{
	Oid			role = get_role_oid(ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME, true);

	if (superuser())
		return true;
	return OidIsValid(role) && has_privs_of_role(GetUserId(), role);
}

bool
ResDefsCanManageGroups(void)
{
	return has_privs_of_manage_resource_groups();
}

static ResGroupLimitType
group_option_type(const char *defname)
{
	if (strcmp(defname, "cpu_max_percent") == 0)
		return RESGROUP_LIMIT_TYPE_CPU;
	if (strcmp(defname, "concurrency") == 0)
		return RESGROUP_LIMIT_TYPE_CONCURRENCY;
	if (strcmp(defname, "cpuset") == 0)
		return RESGROUP_LIMIT_TYPE_CPUSET;
	if (strcmp(defname, "cpu_weight") == 0)
		return RESGROUP_LIMIT_TYPE_CPU_SHARES;
	if (strcmp(defname, "memory_quota") == 0)
		return RESGROUP_LIMIT_TYPE_MEMORY_LIMIT;
	if (strcmp(defname, "min_cost") == 0)
		return RESGROUP_LIMIT_TYPE_MIN_COST;
	if (strcmp(defname, "io_limit") == 0)
		return RESGROUP_LIMIT_TYPE_IO_LIMIT;
	return RESGROUP_LIMIT_TYPE_UNKNOWN;
}

static int
group_option_value(DefElem *defel)
{
	int64		value = defGetInt64(defel);

	if (value < INT_MIN || value > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("capability %s is out of range", defel->defname)));
	return (int) value;
}

/* checkResgroupCapLimit() */
static void
check_group_cap(ResGroupLimitType type, int value)
{
	switch (type)
	{
		case RESGROUP_LIMIT_TYPE_CONCURRENCY:
			if (value < RESGROUP_MIN_CONCURRENCY || value > MaxConnections)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("concurrency range is [%d, 'max_connections']",
								RESGROUP_MIN_CONCURRENCY)));
			break;
		case RESGROUP_LIMIT_TYPE_CPU:
			if (value > RESGROUP_MAX_CPU_MAX_PERCENT ||
				(value < RESGROUP_MIN_CPU_MAX_PERCENT && value != CPU_MAX_PERCENT_DISABLED))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cpu_max_percent range is [%d, %d] or equals to %d",
								RESGROUP_MIN_CPU_MAX_PERCENT, RESGROUP_MAX_CPU_MAX_PERCENT,
								CPU_MAX_PERCENT_DISABLED)));
			break;
		case RESGROUP_LIMIT_TYPE_CPU_SHARES:
			if (value < RESGROUP_MIN_CPU_WEIGHT || value > RESGROUP_MAX_CPU_WEIGHT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cpu_weight range is [%d, %d]",
								RESGROUP_MIN_CPU_WEIGHT, RESGROUP_MAX_CPU_WEIGHT)));
			break;
		case RESGROUP_LIMIT_TYPE_MEMORY_LIMIT:
			if (value < RESGROUP_MIN_MEMORY_QUOTA && value != RESGROUP_DEFAULT_MEMORY_QUOTA)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("memory_quota range is [%d, INT_MAX] or equals to %d",
								RESGROUP_MIN_MEMORY_QUOTA, RESGROUP_DEFAULT_MEMORY_QUOTA)));
			break;
		case RESGROUP_LIMIT_TYPE_MIN_COST:
			if (value < RESGROUP_MIN_MIN_COST)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("The min_cost value can't be less than %d.",
								RESGROUP_MIN_MIN_COST)));
			break;
		default:
			break;
	}
}

/* checkCpusetSyntax() of one half of a cpuset */
static void
check_cpuset_part(const char *cpuset)
{
	if (strlen(cpuset) >= MaxCpuSetLength)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the length of cpuset reached the upper limit %d",
						MaxCpuSetLength)));
	if (!ResGroupCpusetIsValid(cpuset))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cpuset invalid")));
}

/* checkCpuSetByRole(): "coordinator;segments" or one for both */
static void
check_cpuset(const char *cpuset)
{
	const char *first = strchr(cpuset, ';');
	const char *last = strrchr(cpuset, ';');

	if (first != last)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cpuset invalid")));
	if (first == NULL)
		check_cpuset_part(cpuset);
	else
	{
		check_cpuset_part(pnstrdup(cpuset, first - cpuset));
		check_cpuset_part(first + 1);
	}
}

/* parseStmtOptions() */
static void
parse_group_options(List *options, ResGroupDef *def)
{
	int			mask = 0;

	foreach_node(DefElem, defel, options)
	{
		ResGroupLimitType type = group_option_type(defel->defname);

		if (type == RESGROUP_LIMIT_TYPE_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" not recognized", defel->defname)));
		if (mask & (1 << type))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("found duplicate resource group resource type: %s",
							defel->defname)));
		mask |= 1 << type;

		if (type == RESGROUP_LIMIT_TYPE_CPUSET)
		{
			def->cpuset = defGetString(defel);
			check_cpuset(def->cpuset);
			def->cpu_max_percent = CPU_MAX_PERCENT_DISABLED;
			def->cpu_weight = RESGROUP_DEFAULT_CPU_WEIGHT;
		}
		else if (type == RESGROUP_LIMIT_TYPE_IO_LIMIT)
			def->io_limit = ResGroupNormalizeIoLimit(defGetString(defel));
		else
		{
			int			value = group_option_value(defel);

			check_group_cap(type, value);
			switch (type)
			{
				case RESGROUP_LIMIT_TYPE_CONCURRENCY:
					def->concurrency = value;
					break;
				case RESGROUP_LIMIT_TYPE_CPU:
					def->cpu_max_percent = value;
					def->cpuset = pstrdup("-1");
					break;
				case RESGROUP_LIMIT_TYPE_CPU_SHARES:
					def->cpu_weight = value;
					break;
				case RESGROUP_LIMIT_TYPE_MEMORY_LIMIT:
					def->memory_quota = value;
					break;
				case RESGROUP_LIMIT_TYPE_MIN_COST:
					def->min_cost = value;
					break;
				default:
					break;
			}
		}
	}

	if (mask & (1 << RESGROUP_LIMIT_TYPE_CPUSET))
		ResGroupEnsureCpusetIsAvailable();
	if ((mask & (1 << RESGROUP_LIMIT_TYPE_CPU)) &&
		(mask & (1 << RESGROUP_LIMIT_TYPE_CPUSET)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("can't specify both cpu_max_percent and cpuset")));
	if (!(mask & (1 << RESGROUP_LIMIT_TYPE_CPU)) &&
		!(mask & (1 << RESGROUP_LIMIT_TYPE_CPUSET)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("must specify cpu_max_percent or cpuset")));
	if (!(mask & (1 << RESGROUP_LIMIT_TYPE_CONCURRENCY)))
		def->concurrency = RESGROUP_DEFAULT_CONCURRENCY;
	if (!(mask & (1 << RESGROUP_LIMIT_TYPE_MEMORY_LIMIT)))
		def->memory_quota = RESGROUP_DEFAULT_MEMORY_QUOTA;
	if (!(mask & (1 << RESGROUP_LIMIT_TYPE_MIN_COST)))
		def->min_cost = 0;
	if ((mask & (1 << RESGROUP_LIMIT_TYPE_CPU)) &&
		!(mask & (1 << RESGROUP_LIMIT_TYPE_CPU_SHARES)))
		def->cpu_weight = RESGROUP_DEFAULT_CPU_WEIGHT;
}

PG_FUNCTION_INFO_V1(gp_resource_create_group);
PG_FUNCTION_INFO_V1(gp_resource_alter_group);
PG_FUNCTION_INFO_V1(gp_resource_drop_group);
PG_FUNCTION_INFO_V1(gp_resource_comment_group);

/* CALL gp_resource.create_resource_group(name, options): CreateResourceGroup() */
Datum
gp_resource_create_group(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource group");
	List	   *options = options_arg(fcinfo);
	ResGroupDef *def;
	List	   *defs;
	Oid			role;

	prevent_in_transaction_block("CREATE RESOURCE GROUP");

	if (!has_privs_of_manage_resource_groups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create resource group"),
				 errhint("Must be superuser or have privileges of the pg_manage_resource_groups role.")));
	if (strcmp(name, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("resource group name \"none\" is reserved")));

	def = palloc0(sizeof(ResGroupDef));
	def->name = pstrdup(name);
	def->cpuset = pstrdup("-1");
	def->io_limit = pstrdup("-1");
	parse_group_options(options, def);

	role = lock_definitions(GP_RESGROUP_ROLE);
	defs = ResGroupDefsLoad();
	if (list_length(defs) >= MaxResourceGroups)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient resource groups available")));
	if (ResGroupDefFind(defs, name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("resource group \"%s\" already exists", name)));

	/* one no group has, as a queue's (above) */
	do
		def->oid = ResGroupNewOid();
	while (ResGroupDefByOid(defs, def->oid) != NULL);
	ResGroupValidate(defs, def);

	if (!IsResGroupEnabled() && GpResourceIsDispatcher())
		ereport(WARNING,
				(errmsg("resource group is disabled"),
				 errhint("To enable set gp_resource_manager=group")));

	store_groups(role, lappend(defs, def));
	ResGroupCreated(def);
	/* where Cloudberry's CreateResourceGroup() has it: the group made, then */
	if (IsResGroupEnabled())
		(void) GP_FAULT("create_resource_group_fail");
	PG_RETURN_VOID();
}

/* CALL gp_resource.alter_resource_group(name, options): AlterResourceGroup() */
Datum
gp_resource_alter_group(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource group");
	List	   *options = options_arg(fcinfo);
	DefElem    *defel;
	ResGroupLimitType type;
	int			value = 0;
	char	   *cpuset = NULL;
	char	   *io_limit = NULL;
	ResGroupDef *def;
	ResGroupDef old;
	List	   *defs;
	Oid			role;

	prevent_in_transaction_block("ALTER RESOURCE GROUP");

	if (!has_privs_of_manage_resource_groups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter resource group \"%s\"", name),
				 errhint("Must be superuser or have privileges of the pg_manage_resource_groups role.")));

	/* one, as O26 writes ALTER's SET; a CALL of the user's own may not */
	if (list_length(options) != 1)
		elog(ERROR, "O26: ALTER RESOURCE GROUP sets one option, not %d",
			 list_length(options));
	defel = linitial_node(DefElem, options);
	type = group_option_type(defel->defname);
	if (type == RESGROUP_LIMIT_TYPE_UNKNOWN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("option \"%s\" not recognized", defel->defname)));
	else if (type == RESGROUP_LIMIT_TYPE_CPUSET)
	{
		ResGroupEnsureCpusetIsAvailable();
		cpuset = defGetString(defel);
		check_cpuset(cpuset);
	}
	else if (type == RESGROUP_LIMIT_TYPE_IO_LIMIT)
		io_limit = defGetString(defel);
	else
	{
		value = group_option_value(defel);
		check_group_cap(type, value);
	}

	role = lock_definitions(GP_RESGROUP_ROLE);
	defs = ResGroupDefsLoad();
	def = ResGroupDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource group \"%s\" does not exist", name)));

	if (!superuser() &&
		(def->oid == ADMINRESGROUP_OID || def->oid == SYSTEMRESGROUP_OID))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter resource group \"%s\"", name),
				 errhint("Must be superuser to alter a system resource group.")));
	if (type == RESGROUP_LIMIT_TYPE_CONCURRENCY && value == 0 &&
		def->oid == ADMINRESGROUP_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_LIMIT_VALUE),
				 errmsg("admin_group must have at least one concurrency")));

	old = *def;
	switch (type)
	{
		case RESGROUP_LIMIT_TYPE_CPU:
			def->cpu_max_percent = value;
			def->cpuset = pstrdup("-1");
			break;
		case RESGROUP_LIMIT_TYPE_CPU_SHARES:
			def->cpu_weight = value;
			break;
		case RESGROUP_LIMIT_TYPE_CONCURRENCY:
			def->concurrency = value;
			break;
		case RESGROUP_LIMIT_TYPE_CPUSET:
			def->cpuset = cpuset;
			def->cpu_max_percent = CPU_MAX_PERCENT_DISABLED;
			def->cpu_weight = RESGROUP_DEFAULT_CPU_WEIGHT;
			break;
		case RESGROUP_LIMIT_TYPE_MEMORY_LIMIT:
			def->memory_quota = value;
			break;
		case RESGROUP_LIMIT_TYPE_MIN_COST:
			def->min_cost = value;
			break;
		case RESGROUP_LIMIT_TYPE_IO_LIMIT:
			def->io_limit = ResGroupNormalizeIoLimit(io_limit);
			break;
		default:
			break;
	}

	ResGroupValidate(defs, def);
	store_groups(role, defs);
	ResGroupAltered(&old, def, type);
	PG_RETURN_VOID();
}

/* CALL gp_resource.drop_resource_group(name): DropResourceGroup() */
Datum
gp_resource_drop_group(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource group");
	ResGroupDef *def;
	List	   *defs;
	Oid			role;

	prevent_in_transaction_block("DROP RESOURCE GROUP");

	if (!has_privs_of_manage_resource_groups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop resource group \"%s\"", name),
				 errhint("Must be superuser or have privileges of the pg_manage_resource_groups role.")));

	role = lock_definitions(GP_RESGROUP_ROLE);
	defs = ResGroupDefsLoad();
	def = ResGroupDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource group \"%s\" does not exist", name)));
	if (def->oid == DEFAULTRESGROUP_OID || def->oid == ADMINRESGROUP_OID ||
		def->oid == SYSTEMRESGROUP_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop default resource group \"%s\"", name)));

	if (IsResGroupEnabled())
		ResGroupCheckDrop(def->oid, name);

	if (group_has_roles(def->oid))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("resource group is used by at least one role")));

	store_groups(role, list_delete_ptr(defs, def));
	ResGroupDropped(def->oid);
	PG_RETURN_VOID();
}

/* CALL gp_resource.comment_on_resource_group(name, comment) */
Datum
gp_resource_comment_group(PG_FUNCTION_ARGS)
{
	char	   *name = name_arg(fcinfo, "resource group");
	ResGroupDef *def;
	List	   *defs;
	Oid			role;

	if (!has_privs_of_manage_resource_groups())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must have privileges of role \"%s\"",
						ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME)));

	role = lock_definitions(GP_RESGROUP_ROLE);
	defs = ResGroupDefsLoad();
	def = ResGroupDefFind(defs, name);
	if (def == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("resource group \"%s\" does not exist", name)));
	def->comment = PG_ARGISNULL(1) || VARSIZE_ANY_EXHDR(PG_GETARG_TEXT_PP(1)) == 0
		? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	store_groups(role, defs);
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------- */
/* The catalogs by Cloudberry's names                                        */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_resource_resqueue);
PG_FUNCTION_INFO_V1(gp_resource_resqueuecapability);
PG_FUNCTION_INFO_V1(gp_resource_resourcetype);
PG_FUNCTION_INFO_V1(gp_resource_resgroup);
PG_FUNCTION_INFO_V1(gp_resource_resgroupcapability);
PG_FUNCTION_INFO_V1(gp_resource_queue_comment);
PG_FUNCTION_INFO_V1(gp_resource_group_comment);
PG_FUNCTION_INFO_V1(gp_resource_role_queue);
PG_FUNCTION_INFO_V1(gp_resource_role_group);

static Tuplestorestate *
begin_srf(FunctionCallInfo fcinfo, TupleDesc *tupdesc)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	*tupdesc = rsinfo->setDesc;
	return rsinfo->setResult;
}

/* pg_resqueue: oid, rsqname, rsqcountlimit, rsqcostlimit, rsqovercommit, rsqignorecostlimit */
Datum
gp_resource_resqueue(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *store = begin_srf(fcinfo, &tupdesc);

	foreach_ptr(ResQueueDef, d, ResQueueDefsLoad())
	{
		Datum		values[6];
		bool		nulls[6] = {0};
		NameData	name;

		namestrcpy(&name, d->name);
		values[0] = ObjectIdGetDatum(d->oid);
		values[1] = NameGetDatum(&name);
		values[2] = Float4GetDatum(d->active_statements);
		values[3] = Float4GetDatum(d->max_cost);
		values[4] = BoolGetDatum(d->cost_overcommit);
		values[5] = Float4GetDatum(d->min_cost);
		tuplestore_putvalues(store, tupdesc, values, nulls);
	}
	return (Datum) 0;
}

/* pg_resqueuecapability: resqueueid, restypid, ressetting */
Datum
gp_resource_resqueuecapability(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *store = begin_srf(fcinfo, &tupdesc);

	foreach_ptr(ResQueueDef, d, ResQueueDefsLoad())
	{
		const char *settings[2] = {d->priority, d->memory_limit};

		for (int i = 0; i < 2; i++)
		{
			Datum		values[3];
			bool		nulls[3] = {0};

			if (settings[i] == NULL)
				continue;
			values[0] = ObjectIdGetDatum(d->oid);
			values[1] = Int16GetDatum(PG_RESRCTYPE_PRIORITY + i);
			values[2] = CStringGetTextDatum(settings[i]);
			tuplestore_putvalues(store, tupdesc, values, nulls);
		}
	}
	return (Datum) 0;
}

/* pg_resourcetype, which is fixed */
Datum
gp_resource_resourcetype(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *store = begin_srf(fcinfo, &tupdesc);

	for (int i = 0; i < PG_RESRCTYPE_COUNT; i++)
	{
		Datum		values[8];
		bool		nulls[8] = {0};
		NameData	name;

		namestrcpy(&name, resource_types[i].resname);
		values[0] = ObjectIdGetDatum(resource_types[i].oid);
		values[1] = NameGetDatum(&name);
		values[2] = Int16GetDatum(resource_types[i].restypid);
		values[3] = BoolGetDatum(resource_types[i].resrequired);
		values[4] = BoolGetDatum(resource_types[i].reshasdefault);
		values[5] = BoolGetDatum(resource_types[i].reshasdisable);
		values[6] = CStringGetTextDatum(resource_types[i].resdefaultsetting);
		if (resource_types[i].resdisabledsetting != NULL)
			values[7] = CStringGetTextDatum(resource_types[i].resdisabledsetting);
		else
			nulls[7] = true;
		tuplestore_putvalues(store, tupdesc, values, nulls);
	}
	return (Datum) 0;
}

/* pg_resgroup: oid, rsgname, parent */
Datum
gp_resource_resgroup(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *store = begin_srf(fcinfo, &tupdesc);

	foreach_ptr(ResGroupDef, d, ResGroupDefsLoad())
	{
		Datum		values[3];
		bool		nulls[3] = {0};
		NameData	name;

		namestrcpy(&name, d->name);
		values[0] = ObjectIdGetDatum(d->oid);
		values[1] = NameGetDatum(&name);
		values[2] = ObjectIdGetDatum(InvalidOid);
		tuplestore_putvalues(store, tupdesc, values, nulls);
	}
	return (Datum) 0;
}

/* pg_resgroupcapability: resgroupid, reslimittype, value */
Datum
gp_resource_resgroupcapability(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Tuplestorestate *store = begin_srf(fcinfo, &tupdesc);

	foreach_ptr(ResGroupDef, d, ResGroupDefsLoad())
	{
		char	   *values_str[RESGROUP_LIMIT_TYPE_COUNT];

		values_str[RESGROUP_LIMIT_TYPE_CONCURRENCY] = psprintf("%d", d->concurrency);
		values_str[RESGROUP_LIMIT_TYPE_CPU] = psprintf("%d", d->cpu_max_percent);
		values_str[RESGROUP_LIMIT_TYPE_CPU_SHARES] = psprintf("%d", d->cpu_weight);
		values_str[RESGROUP_LIMIT_TYPE_CPUSET] = d->cpuset;
		values_str[RESGROUP_LIMIT_TYPE_MEMORY_LIMIT] = psprintf("%d", d->memory_quota);
		values_str[RESGROUP_LIMIT_TYPE_MIN_COST] = psprintf("%d", d->min_cost);
		values_str[RESGROUP_LIMIT_TYPE_IO_LIMIT] = d->io_limit;
		for (int t = RESGROUP_LIMIT_TYPE_CONCURRENCY; t < RESGROUP_LIMIT_TYPE_COUNT; t++)
		{
			Datum		values[3];
			bool		nulls[3] = {0};

			values[0] = ObjectIdGetDatum(d->oid);
			values[1] = Int16GetDatum(t);
			values[2] = CStringGetTextDatum(values_str[t]);
			tuplestore_putvalues(store, tupdesc, values, nulls);
		}
	}
	return (Datum) 0;
}

/* A queue's or a group's comment, obj_description()'s for them */
Datum
gp_resource_queue_comment(PG_FUNCTION_ARGS)
{
	ResQueueDef *d = ResQueueDefByOid(ResQueueDefsLoad(), PG_GETARG_OID(0));

	if (d == NULL || d->comment == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(d->comment));
}

Datum
gp_resource_group_comment(PG_FUNCTION_ARGS)
{
	ResGroupDef *d = ResGroupDefByOid(ResGroupDefsLoad(), PG_GETARG_OID(0));

	if (d == NULL || d->comment == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(d->comment));
}

/*
 * rolresqueue and rolresgroup of a row of pg_roles or pg_authid, which O10
 * makes these calls of (gp_resource.c): the role's queue and group, the
 * defaults where it was given none, as Cloudberry's columns say.
 */
static Oid
row_role(HeapTupleHeader row)
{
	bool		isnull;
	Datum		oid = GetAttributeByName(row, "oid", &isnull);

	return isnull ? InvalidOid : DatumGetObjectId(oid);
}

Datum
gp_resource_role_queue(PG_FUNCTION_ARGS)
{
	Oid			roleid = row_role(PG_GETARG_HEAPTUPLEHEADER(0));

	if (!OidIsValid(roleid))
		PG_RETURN_NULL();
	PG_RETURN_OID(GetResQueueForRole(roleid));
}

Datum
gp_resource_role_group(PG_FUNCTION_ARGS)
{
	Oid			roleid = row_role(PG_GETARG_HEAPTUPLEHEADER(0));

	if (!OidIsValid(roleid))
		PG_RETURN_NULL();
	PG_RETURN_OID(GetResGroupForRole(roleid));
}

/* ------------------------------------------------------------------------- */
/* pg_manage_resource_groups                                                 */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_resource_make_manage_role);

/*
 * Cloudberry's predefined role whose members may manage groups without being
 * superusers (its pg_authid.dat, OID 6312).  PostgreSQL 19 makes a role of a
 * name starting "pg_" only at initdb, so the extension's script has this put
 * its row in pg_authid, under Cloudberry's OID where that is free: an OID
 * below FirstUnpinnedObjectId is pinned, so DROP ROLE refuses it as it
 * refuses PostgreSQL's own predefined roles, and pg_dumpall, which passes
 * over roles named pg_*, leaves it to the extension.
 */
Datum
gp_resource_make_manage_role(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		values[Natts_pg_authid];
	bool		nulls[Natts_pg_authid];
	HeapTuple	tuple;
	Oid			roleid = 6312;
	NameData	name;

	if (OidIsValid(get_role_oid(ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME, true)))
		PG_RETURN_VOID();
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to make role \"%s\"",
						ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME)));

	rel = table_open(AuthIdRelationId, RowExclusiveLock);
	if (SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(roleid)))
		roleid = GetNewOidWithIndex(rel, AuthIdOidIndexId, Anum_pg_authid_oid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	namestrcpy(&name, ROLE_PG_MANAGE_RESOURCE_GROUPS_NAME);
	values[Anum_pg_authid_oid - 1] = ObjectIdGetDatum(roleid);
	values[Anum_pg_authid_rolname - 1] = NameGetDatum(&name);
	values[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(true);
	values[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(false);
	values[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(-1);
	nulls[Anum_pg_authid_rolpassword - 1] = true;
	nulls[Anum_pg_authid_rolvaliduntil - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tuple);
	table_close(rel, NoLock);
	CommandCounterIncrement();
	PG_RETURN_VOID();
}

/* ------------------------------------------------------------------------- */
/* The label provider                                                        */
/* ------------------------------------------------------------------------- */

/*
 * The definitions' labels, written by SECURITY LABEL rather than the
 * procedures above: a restore of pg_dumpall's output, which a superuser
 * runs, and a segment told what the coordinator wrote.  Nothing else may
 * write them, and only on the carrier roles.
 */
static void
gp_resource_label_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;

	if (object->classId != AuthIdRelationId || !ResDefsIsCarrierRole(object->objectId))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label goes on role \"%s\" or \"%s\" only",
						GP_RESOURCE_PROVIDER, GP_RESQUEUE_ROLE, GP_RESGROUP_ROLE)));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may write the resource manager's definitions directly"),
				 errhint("Use CREATE, ALTER and DROP RESOURCE QUEUE and RESOURCE GROUP.")));

	/* whatever a segment or a restore is told, shared memory follows */
	if (object->objectId == carrier_role(GP_RESQUEUE_ROLE))
		ResQueueDefsChanged();
	else
		ResGroupDefsChanged();

	if (seclabel == NULL)
		return;
	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));
	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label must be a JSON object",
						GP_RESOURCE_PROVIDER)));
}

void
ResDefsRegisterProvider(void)
{
	register_label_provider(GP_RESOURCE_PROVIDER, gp_resource_label_check);
}
