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
 * gp_label.c
 *	  The "gp" security label provider.
 *
 * See gp_label.h for why the port keeps things here that Cloudberry keeps in
 * catalog columns of its own.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_opclass.h"
#include "commands/seclabel.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#include "gp_dispatch.h"
#include "gp_label.h"
#include "gp_policy.h"

static const struct
{
	const char *name;
	bool		takes_value;
}			gp_label_key[GP_LABEL_NKEYS] = {
#define X(key, takes_val, doc)	{#key, takes_val},
	GP_LABEL_KEYS(X)
#undef X
};

/*
 * Parse a label into its keys.  Returns a list of "key=value" strings in
 * order, so that setting one key can put the others back unchanged.
 *
 * A value may hold a comma -- a distribution key list is one -- so a value
 * that would be ambiguous is written in double quotes, with a quote inside it
 * doubled, and the split has to know that.
 */
static List *
gp_label_parse(const char *label)
{
	List	   *items = NIL;
	const char *p = label;

	while (p && *p)
	{
		const char *start;
		bool		in_quotes = false;
		int			len;
		char	   *item;

		while (*p == ' ' || *p == '\t')
			p++;
		start = p;

		while (*p != '\0' && (in_quotes || *p != ','))
		{
			if (*p == '"')
				in_quotes = !in_quotes;
			p++;
		}

		len = (int) (p - start);
		while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
			len--;

		if (len > 0)
		{
			item = palloc(len + 1);
			memcpy(item, start, len);
			item[len] = '\0';
			items = lappend(items, item);
		}

		if (*p == ',')
			p++;
	}

	return items;
}

/* Undo the quoting above.  The result is palloc'd. */
static char *
gp_label_unquote(const char *value)
{
	StringInfoData buf;

	if (value[0] != '"')
		return pstrdup(value);

	initStringInfo(&buf);
	for (const char *p = value + 1; *p != '\0'; p++)
	{
		if (*p == '"')
		{
			if (p[1] == '"')	/* a quote inside, written twice */
				p++;
			else
				break;			/* the closing quote */
		}
		appendStringInfoChar(&buf, *p);
	}

	return buf.data;
}

/* Quote a value that would otherwise be read as two items, or as a quote. */
static void
gp_label_append_value(StringInfo buf, const char *value)
{
	if (strchr(value, ',') == NULL && strchr(value, '"') == NULL &&
		value[0] != ' ' && value[strlen(value) - 1] != ' ')
	{
		appendStringInfoString(buf, value);
		return;
	}

	appendStringInfoChar(buf, '"');
	for (const char *p = value; *p != '\0'; p++)
	{
		if (*p == '"')
			appendStringInfoChar(buf, '"');
		appendStringInfoChar(buf, *p);
	}
	appendStringInfoChar(buf, '"');
}

/* The key an item names, or -1 if it names none of ours. */
static int
gp_label_item_key(const char *item, const char **value)
{
	const char *eq = strchr(item, '=');
	int			namelen = eq ? (int) (eq - item) : (int) strlen(item);

	for (int i = 0; i < GP_LABEL_NKEYS; i++)
	{
		if (namelen == (int) strlen(gp_label_key[i].name) &&
			strncmp(item, gp_label_key[i].name, namelen) == 0)
		{
			if (value)
				*value = eq ? eq + 1 : "";
			return i;
		}
	}

	return -1;
}

/* One key's value in a whole label, or NULL. */
static char *
gp_label_value(const char *label, GpLabelKey key)
{
	ListCell   *lc;

	if (label == NULL)
		return NULL;
	foreach(lc, gp_label_parse(label))
	{
		const char *item = (const char *) lfirst(lc);
		const char *value = NULL;

		if (gp_label_item_key(item, &value) == (int) key)
			return gp_label_unquote(value);
	}
	return NULL;
}

/* The operator classes a distribution policy names, each once. */
static List *
policy_opclasses(const char *policy)
{
	List	   *oids = NIL;
	ListCell   *lc;

	/* a malformed one names none: the reader refuses it, and says why */
	foreach(lc, GpPolicyParseKeyQuietly(policy))
	{
		GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);
		Oid			opclass;

		if (key->opclass == NULL)
			continue;
		opclass = GpPolicyOpclassByName(key->opclass);
		if (OidIsValid(opclass))
			oids = list_append_unique_oid(oids, opclass);
	}
	return oids;
}

/*
 * One of a relation's dependencies on an operator class, deleted: the one a
 * policy recorded.  Only one, because a partition key naming the same class
 * recorded one of its own, which has to stay.
 */
static void
delete_one_opclass_dependency(Oid relid, Oid opclass)
{
	Relation	depRel = table_open(DependRelationId, RowExclusiveLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);

		if (dep->objsubid == 0 && dep->refclassid == OperatorClassRelationId &&
			dep->refobjid == opclass && dep->deptype == DEPENDENCY_NORMAL)
		{
			CatalogTupleDelete(depRel, &tuple->t_self);
			break;
		}
	}
	systable_endscan(scan);
	table_close(depRel, RowExclusiveLock);
}

/*
 * A table depends on the operator classes its distribution key is hashed
 * with, as Cloudberry's does through its gp_distribution_policy row: DROP
 * OPERATOR CLASS refuses while a table is hashed with it, and CASCADE drops
 * the table.  On every node, since a segment's label arrives as a SECURITY
 * LABEL, which is what reaches the check below, and a DROP ... CASCADE sent
 * to a segment has to find the same tables to drop.  The label's classes
 * before and after are compared, and the dependencies follow the difference.
 */
static void
gp_label_follow_policy(const ObjectAddress *object, const char *old_label,
					   const char *new_label)
{
	List	   *before;
	List	   *after;
	ListCell   *lc;

	if (object->classId != RelationRelationId || object->objectSubId != 0)
		return;
	before = policy_opclasses(gp_label_value(old_label, GP_LABEL_distributed_by));
	after = policy_opclasses(gp_label_value(new_label, GP_LABEL_distributed_by));

	foreach(lc, before)
		if (!list_member_oid(after, lfirst_oid(lc)))
			delete_one_opclass_dependency(object->objectId, lfirst_oid(lc));
	foreach(lc, after)
	{
		ObjectAddress opclass;

		if (list_member_oid(before, lfirst_oid(lc)))
			continue;
		ObjectAddressSet(opclass, OperatorClassRelationId, lfirst_oid(lc));
		recordDependencyOn(object, &opclass, DEPENDENCY_NORMAL);
	}
}

/*
 * Refuse a label the port would not understand, when it is set rather than
 * when it is read: a key we do not know is far more likely to be a typo than
 * a message from the future.  And a label that is set records what it
 * depends on (gp_label_follow_policy).
 */
static void
gp_label_check(const ObjectAddress *object, const char *seclabel)
{
	List	   *items;
	ListCell   *lc;

	if (object->classId == RelationRelationId)
		gp_label_follow_policy(object,
							   GetSecurityLabel(object, GP_LABEL_PROVIDER),
							   seclabel);

	if (seclabel == NULL)		/* removing the label is always fine */
		return;

	items = gp_label_parse(seclabel);
	if (items == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" security label is empty", GP_LABEL_PROVIDER),
				 errhint("A label is a comma-separated list of key=value pairs.")));

	foreach(lc, items)
	{
		const char *item = (const char *) lfirst(lc);
		const char *value = NULL;
		int			key = gp_label_item_key(item, &value);

		if (key < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized \"%s\" security label key in \"%s\"",
							GP_LABEL_PROVIDER, item)));

		value = gp_label_unquote(value);

		if (!gp_label_key[key].takes_value && value[0] != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" takes no value", gp_label_key[key].name)));

		if (gp_label_key[key].takes_value && value[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" needs a value", gp_label_key[key].name)));
	}
}

char *
GpLabelGet(const ObjectAddress *object, GpLabelKey key)
{
	return gp_label_value(GetSecurityLabel(object, GP_LABEL_PROVIDER), key);
}

bool
GpLabelHas(const ObjectAddress *object, GpLabelKey key)
{
	return GpLabelGet(object, key) != NULL;
}

void
GpLabelSet(const ObjectAddress *object, GpLabelKey key, const char *value)
{
	char	   *label = GetSecurityLabel(object, GP_LABEL_PROVIDER);
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);

	/* Everything but the key we are setting, in the order it was in. */
	if (label != NULL)
	{
		foreach(lc, gp_label_parse(label))
		{
			const char *item = (const char *) lfirst(lc);

			if (gp_label_item_key(item, NULL) == (int) key)
				continue;
			if (buf.len > 0)
				appendStringInfoChar(&buf, ',');
			appendStringInfoString(&buf, item);
		}
	}

	if (value != NULL)
	{
		if (buf.len > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, gp_label_key[key].name);
		if (value[0] != '\0')
		{
			appendStringInfoChar(&buf, '=');
			gp_label_append_value(&buf, value);
		}
	}

	if (key == GP_LABEL_distributed_by)
		gp_label_follow_policy(object, label, buf.len > 0 ? buf.data : NULL);

	/* An object with nothing left to say loses its label entirely. */
	SetSecurityLabel(object, GP_LABEL_PROVIDER, buf.len > 0 ? buf.data : NULL);
	pfree(buf.data);

	/* The segments are told, as they are of the catalogs a statement changes. */
	GpDispatchNoteLabel(object);

	/*
	 * Setting two keys of one object is two writes to the same row, and the
	 * second has to see the first -- otherwise it reads the label as it was
	 * and then fails with "tuple already updated by self".
	 */
	CommandCounterIncrement();
}

void
GpLabelRegisterProvider(void)
{
	register_label_provider(GP_LABEL_PROVIDER, gp_label_check);
}
