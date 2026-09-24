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
 * tag.c
 *	  Tags: metadata a user hangs on an object.
 *
 * Cloudberry keeps tag definitions in the shared catalog pg_tag and the
 * assignments in pg_tag_description, keyed by (database, class, object).  An
 * extension can create neither, so both become security labels.  An
 * assignment is the object's own:
 *
 *	  SECURITY LABEL FOR gp_tag ON TABLE t IS '{"env": "prod"}'
 *
 * which is what Cloudberry writes as TAG (env = 'prod').  A label is the
 * right shape for this: it is transactional, it is dropped with its object,
 * and pg_dump writes it -- so assignments survive a dump and restore, which
 * they do not in Cloudberry, whose own tools never dumped them.
 *
 * The definitions are one shared label, "gp_tag_definitions", on the NOLOGIN
 * role of that name, which the extension's script makes or finds made:
 *
 *	  {"env": {"oid": 16390, "owner": 10, "allowed_values": ["prod", "dev"]}}
 *
 * A role's label is the cluster's, as pg_tag is, so a tag defined in one
 * database is one in every other -- the extension need not even be there, but
 * for an index's tags, which are rows of its own (see below) -- and pg_dumpall
 * writes it with the roles.  A definition is no secret, so a label anybody can
 * read costs nothing.  What it does cost: every CREATE, ALTER and DROP TAG
 * rewrites the one label, so two of them in flight take turns; and DROP TAG
 * sees only its own database's assignments, where Cloudberry's shared
 * pg_tag_description holds every database's, so an assignment whose tag is
 * gone is passed over rather than failed on.
 *
 * Why providers of their own, beside gp_core's "gp".  The keys of a "gp"
 * label are fixed, and checked against a list when the label is set, because
 * they are the port's own and a key that is not on the list is a typo.  Tag
 * names are the user's, and what constrains them is the definitions rather
 * than a list in a header, so they need a check of their own.  The label is
 * a JSON object rather than the "gp" provider's key=value text, because a tag
 * value is user data and may hold a comma.  And the definitions are not the
 * carrier role's tags, which a "gp_tag" label on it would say they were.
 *
 * Cloudberry source this file is made of:
 *	  src/backend/commands/tag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "commands/defrem.h"
#include "catalog/pg_type.h"
#include "commands/seclabel.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/tuplestore.h"

#include "gp_core_api.h"
#include "gp_sql.h"

/*
 * Cloudberry's MAX_TAG_NUMBER (src/backend/commands/tag.c:62): how many tags
 * one object may carry.
 */
#define GP_TAG_MAX_PER_OBJECT	50

/* ------------------------------------------------------------------------- */
/* The definitions                                                           */
/* ------------------------------------------------------------------------- */

/*
 * One of the module's own tables, or InvalidOid when it is not there.
 *
 * The table rather than the schema, because DROP EXTENSION drops them one at
 * a time: while it runs, the schema still exists and the tables are going
 * away under it, and this module's own hooks fire on each drop.
 */
static Oid
tag_table_oid(const char *relname)
{
	Oid			nsp = get_namespace_oid(GP_SQL_SCHEMA, true);

	if (!OidIsValid(nsp))
		return InvalidOid;

	return get_relname_relid(relname, nsp);
}

/*
 * An index's tags are rows in gp_sql.index_tag, so tagging one needs the
 * extension in the index's database.  Say that, rather than let a query fail
 * with "relation does not exist".
 */
static void
tag_require_extension(void)
{
	if (!OidIsValid(tag_table_oid("index_tag")))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tags of an index need the \"%s\" extension in this database",
						GP_SQL_SCHEMA),
				 errhint("Run \"CREATE EXTENSION gp_sql\".")));
}

/*
 * A tag as its definition says it: what Cloudberry's pg_tag row holds.  The
 * OID is one GetNewObjectId() gave it, for pg_tag.oid and
 * pg_tag_description.tagid to name it by; nothing else is numbered by it.
 */
typedef struct TagDef
{
	char	   *name;
	Oid			oid;
	Oid			owner;
	bool		listed;			/* false: any value is allowed */
	List	   *values;			/* of char *, in the order they were added */
} TagDef;

/* The carrier role, or InvalidOid: no database has made the extension yet. */
static Oid
tagdef_role(void)
{
	return get_role_oid(GP_TAGDEF_ROLE, true);
}

static Oid
json_oid(JsonbContainer *obj, const char *key)
{
	JsonbValue	buf;
	JsonbValue *v = getKeyJsonValueFromContainer(obj, key, strlen(key), &buf);

	if (v == NULL || v->type != jbvNumeric)
		return InvalidOid;
	return (Oid) DatumGetInt64(DirectFunctionCall1(numeric_int8,
												   NumericGetDatum(v->val.numeric)));
}

/*
 * Every definition, from the label, or NIL.  The label is read as the
 * catalog says it now, which a writer has locked first (tagdef_lock).
 */
static List *
tagdef_load(void)
{
	Oid			role = tagdef_role();
	ObjectAddress addr;
	char	   *label;
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	TagDef	   *cur = NULL;
	List	   *defs = NIL;

	if (!OidIsValid(role))
		return NIL;
	ObjectAddressSet(addr, AuthIdRelationId, role);
	label = GetSecurityLabel(&addr, GP_TAGDEF_PROVIDER);
	if (label == NULL)
		return NIL;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(label)));
	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY)
		{
			cur = palloc0(sizeof(TagDef));
			cur->name = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (tok == WJB_VALUE && cur != NULL && v.type == jbvBinary)
		{
			JsonbContainer *obj = v.val.binary.data;
			JsonbValue	buf;
			JsonbValue *list;

			cur->oid = json_oid(obj, "oid");
			cur->owner = json_oid(obj, "owner");
			list = getKeyJsonValueFromContainer(obj, "allowed_values",
												strlen("allowed_values"), &buf);
			if (list != NULL && list->type == jbvBinary)
			{
				JsonbIterator *lit = JsonbIteratorInit(list->val.binary.data);
				JsonbValue	e;

				cur->listed = true;
				while ((tok = JsonbIteratorNext(&lit, &e, true)) != WJB_DONE)
				{
					if (tok == WJB_ELEM && e.type == jbvString)
						cur->values = lappend(cur->values,
											  pnstrdup(e.val.string.val,
													   e.val.string.len));
				}
			}
			defs = lappend(defs, cur);
			cur = NULL;
		}
	}
	return defs;
}

static TagDef *
tagdef_find(List *defs, const char *name)
{
	foreach_ptr(TagDef, d, defs)
	{
		if (strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
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
push_oid(JsonbInState *state, const char *key, Oid value)
{
	JsonbValue	v;

	push_string(state, WJB_KEY, key);
	v.type = jbvNumeric;
	v.val.numeric = int64_to_numeric((int64) value);
	pushJsonbValue(state, WJB_VALUE, &v);
}

/*
 * The one statement that may change the definitions: whoever gets here
 * first rewrites the label, and the next waits for it and reads what it
 * wrote.  A lock on the carrier role, as SECURITY LABEL on it takes, whose
 * acquisition brings the catalog snapshot up to date.
 */
static Oid
tagdef_lock(void)
{
	Oid			role = tagdef_role();

	if (!OidIsValid(role))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\", which carries the tag definitions, does not exist",
						GP_TAGDEF_ROLE),
				 errhint("The \"%s\" extension makes it: drop and create the extension again.",
						 GP_SQL_SCHEMA)));
	LockSharedObject(AuthIdRelationId, role, 0, ShareUpdateExclusiveLock);
	return role;
}

static void
tagdef_store(Oid role, List *defs)
{
	JsonbInState state = {0};
	ObjectAddress addr;
	Jsonb	   *jb;

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
	foreach_ptr(TagDef, d, defs)
	{
		push_string(&state, WJB_KEY, d->name);
		pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
		push_oid(&state, "oid", d->oid);
		push_oid(&state, "owner", d->owner);
		if (d->listed)
		{
			push_string(&state, WJB_KEY, "allowed_values");
			pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
			foreach_ptr(char, value, d->values)
				push_string(&state, WJB_ELEM, value);
			pushJsonbValue(&state, WJB_END_ARRAY, NULL);
		}
		pushJsonbValue(&state, WJB_END_OBJECT, NULL);
	}
	pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	jb = JsonbValueToJsonb(state.result);
	ObjectAddressSet(addr, AuthIdRelationId, role);
	SetSecurityLabel(&addr, GP_TAGDEF_PROVIDER,
					 defs == NIL ? NULL
					 : JsonbToCString(NULL, &jb->root, VARSIZE(jb)));
}

/* Only its owner changes a tag, as Cloudberry's pg_tag_ownercheck says. */
static void
tagdef_check_owner(const TagDef *d)
{
	if (!has_privs_of_role(GetUserId(), d->owner))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of tag %s", d->name)));
}

static TagDef *
tagdef_existing(List *defs, const char *name)
{
	TagDef	   *d = tagdef_find(defs, name);

	if (d == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tag \"%s\" does not exist", name)));
	return d;
}

static List *
text_array_list(ArrayType *arr)
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	List	   *values = NIL;

	deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
	for (int i = 0; i < n; i++)
	{
		if (!nulls[i])
			values = lappend(values, TextDatumGetCString(elems[i]));
	}
	return values;
}

/*
 * A segment is told a tag was set by the coordinator, which checked it: the
 * definitions are the coordinator's label, and a segment has none.
 */
static bool
tag_checked_elsewhere(void)
{
	return GpCoreApiLookup()->get_role() == GP_ROLE_EXECUTE;
}

/*
 * Refuse a tag that is not defined, or a value the definition does not allow.
 *
 * This is Cloudberry's rule in AddTagDescriptions: a tag with no allowed
 * values takes any value, and one with a list takes only what is on it.
 */
void
GpTagValidate(const char *tagname, const char *tagvalue)
{
	TagDef	   *d;

	if (tag_checked_elsewhere())
		return;

	d = tagdef_find(tagdef_load(), tagname);
	if (d == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tag \"%s\" does not exist", tagname)));

	if (d->listed)
	{
		foreach_ptr(char, value, d->values)
		{
			if (strcmp(value, tagvalue) == 0)
				return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tag value \"%s\" is not in tag \"%s\" allowed values",
						tagvalue, tagname)));
	}
}

/*
 * The definitions' own label, written by SECURITY LABEL rather than by the
 * functions below: a restore of pg_dumpall's output, which a superuser runs.
 * Nothing else may write it, and only on the carrier role.
 */
static void
gp_tagdef_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;

	if (object->classId != AuthIdRelationId ||
		strcmp(GetUserNameFromId(object->objectId, false), GP_TAGDEF_ROLE) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label goes on role \"%s\" only",
						GP_TAGDEF_PROVIDER, GP_TAGDEF_ROLE)));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may write the tag definitions directly"),
				 errhint("Use CREATE TAG, ALTER TAG and DROP TAG.")));
	if (seclabel == NULL)
		return;
	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));
	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label must be a JSON object",
						GP_TAGDEF_PROVIDER)));
}

/* ------------------------------------------------------------------------- */
/* The label                                                                 */
/* ------------------------------------------------------------------------- */

/* Is `key` one of the names in a DefElem list? */
static bool
tag_named(List *tags, const char *key, int keylen)
{
	ListCell   *lc;

	foreach(lc, tags)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if ((int) strlen(def->defname) == keylen &&
			strncmp(def->defname, key, keylen) == 0)
			return true;
	}

	return false;
}

static void
tag_push_pair(JsonbInState *state, const char *key, const char *value)
{
	JsonbValue	jv;

	jv.type = jbvString;
	jv.val.string.len = strlen(key);
	jv.val.string.val = unconstify(char *, key);
	pushJsonbValue(state, WJB_KEY, &jv);

	jv.type = jbvString;
	jv.val.string.len = strlen(value);
	jv.val.string.val = unconstify(char *, value);
	pushJsonbValue(state, WJB_VALUE, &jv);
}

/*
 * The object's label with `tags` added to it, as JSON text.  Keys already
 * there keep their place unless the new list names them, which is what makes
 * one TAG clause add to the tags an object already has rather than replace
 * them.
 */
static char *
tag_merge(const char *existing, List *tags)
{
	/*
	 * PG19 builds a jsonb through a JsonbInState, which carries the result
	 * rather than returning it; PostgreSQL 16, which Cloudberry is on, passed
	 * a JsonbParseState and got the value back.
	 */
	JsonbInState state = {0};
	Jsonb	   *merged;
	ListCell   *lc;
	int			count = 0;

	pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	if (existing != NULL)
	{
		Jsonb	   *jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
														   CStringGetDatum(existing)));
		JsonbIterator *it = JsonbIteratorInit(&jb->root);
		JsonbIteratorToken tok;
		JsonbValue	v;
		JsonbValue	key;
		bool		have_key = false;

		while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
		{
			if (tok == WJB_KEY)
			{
				key = v;
				have_key = true;
			}
			else if (tok == WJB_VALUE && have_key)
			{
				have_key = false;
				if (tag_named(tags, key.val.string.val, key.val.string.len))
					continue;	/* the new list says what this one is now */
				pushJsonbValue(&state, WJB_KEY, &key);
				pushJsonbValue(&state, WJB_VALUE, &v);
				count++;
			}
		}
	}

	foreach(lc, tags)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		/* No value means RESET: the key is simply not put back. */
		if (def->arg == NULL)
			continue;

		tag_push_pair(&state, def->defname, defGetString(def));
		count++;
	}

	pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	if (count > GP_TAG_MAX_PER_OBJECT)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("an object may carry at most %d tags, and this would be %d",
						GP_TAG_MAX_PER_OBJECT, count)));

	merged = JsonbValueToJsonb(state.result);
	return JsonbToCString(NULL, &merged->root, VARSIZE(merged));
}

/*
 * The relabel check hook: what SECURITY LABEL FOR gp_tag accepts.
 *
 * PostgreSQL has already checked that the user owns the object, so what is
 * left is the shape of the label and the tags it names -- those it adds or
 * changes: one the object already carries stays, even when a DROP TAG in
 * another database, which could not see it, has taken its definition away.
 */
static void
gp_tag_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;
	Jsonb	   *had = NULL;
	char	   *existing;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	char	   *key = NULL;
	int			count = 0;

	if (seclabel == NULL)		/* removing every tag is always fine */
		return;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));
	existing = GetSecurityLabel(object, GP_TAG_PROVIDER);
	if (existing != NULL)
		had = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(existing)));

	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a \"%s\" security label must be a JSON object",
						GP_TAG_PROVIDER),
				 errhint("Write it as {\"env\": \"prod\"}, or use %s.set_tag().",
						 GP_SQL_SCHEMA)));

	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		if (tok == WJB_KEY)
		{
			key = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (tok == WJB_VALUE && key != NULL)
		{
			JsonbValue	buf;
			JsonbValue *before = NULL;

			if (v.type != jbvString)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("tag \"%s\" must be given a string value", key)));

			if (had != NULL && JB_ROOT_IS_OBJECT(had))
				before = getKeyJsonValueFromContainer(&had->root, key,
													  strlen(key), &buf);
			if (before == NULL || before->type != jbvString ||
				before->val.string.len != v.val.string.len ||
				memcmp(before->val.string.val, v.val.string.val,
					   v.val.string.len) != 0)
				GpTagValidate(key, pnstrdup(v.val.string.val, v.val.string.len));
			count++;
			key = NULL;
		}
	}

	if (count > GP_TAG_MAX_PER_OBJECT)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("an object may carry at most %d tags, and this label has %d",
						GP_TAG_MAX_PER_OBJECT, count)));
}

void
GpTagRegisterProvider(void)
{
	register_label_provider(GP_TAG_PROVIDER, gp_tag_check);
	register_label_provider(GP_TAGDEF_PROVIDER, gp_tagdef_check);
}

/* ------------------------------------------------------------------------- */
/* The WITH (gp_tag.x = 'y') shorthand                                       */
/* ------------------------------------------------------------------------- */

List *
GpTagTakeOptions(List **options)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace == NULL ||
			strcmp(def->defnamespace, GP_TAG_OPTION_NS) != 0)
			continue;

		if (def->arg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tag \"%s\" needs a value", def->defname),
					 errhint("Write it as %s.%s = 'value'.",
							 GP_TAG_OPTION_NS, def->defname)));

		taken = lappend(taken, makeDefElem(pstrdup(def->defname), def->arg, -1));
		*options = foreach_delete_current(*options, lc);
	}

	return taken;
}

/*
 * The same for a statement whose options have no namespace -- CREATE and
 * ALTER DATABASE, and a foreign table's OPTIONS -- where the desugarer writes
 * each tag as an option named "gp_tag.<name>" (gp_desugar.c, tag_db_option
 * and rw_add_fdw_option).  ALTER DATABASE's "gp_tag.<name>" = DEFAULT, which
 * is UNSET TAG, has no value, and that is what "remove" means below.
 */
List *
GpTagTakePrefixedOptions(List **options)
{
	List	   *taken = NIL;
	const char *prefix = GP_TAG_OPTION_NS ".";
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strncmp(def->defname, prefix, strlen(prefix)) != 0)
			continue;

		taken = lappend(taken, makeDefElem(pstrdup(def->defname + strlen(prefix)),
										   def->arg, -1));
		*options = foreach_delete_current(*options, lc);
	}

	return taken;
}

/* Is this one of the tags a rewrite carried to a statement's parse node? */
static bool
tag_is_carried(Node *node)
{
	return IsA(node, DefElem) &&
		((DefElem *) node)->defnamespace != NULL &&
		strcmp(((DefElem *) node)->defnamespace, GP_TAG_OPTION_NS) == 0;
}

bool
GpTagHasCarried(List *list)
{
	ListCell   *lc;

	foreach(lc, list)
	{
		if (tag_is_carried((Node *) lfirst(lc)))
			return true;
	}

	return false;
}

/*
 * Take them out, so that what is left is the list PostgreSQL wrote.  A tag
 * with no value is UNSET TAG's, which takes it away.
 */
List *
GpTagTakeCarried(List **list)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (!tag_is_carried((Node *) def))
			continue;

		taken = lappend(taken, makeDefElem(pstrdup(def->defname), def->arg, -1));
		*list = foreach_delete_current(*list, lc);
	}

	return taken;
}

List *
GpTagTakeResetOptions(List **options)
{
	List	   *taken = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->defnamespace == NULL ||
			strcmp(def->defnamespace, GP_TAG_OPTION_NS) != 0)
			continue;

		/* RESET (gp_tag.env = '...') would be asking for two things at once. */
		if (def->arg != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("RESET must not be given a value for tag \"%s\"",
							def->defname)));

		taken = lappend(taken, makeDefElem(pstrdup(def->defname), NULL, -1));
		*options = foreach_delete_current(*options, lc);
	}

	return taken;
}

/*
 * Refuse every tag in the list that is not defined, before a statement that
 * carries them runs.  A tag being removed is not looked up: a tag that has
 * since been dropped must still be removable from the objects that carry it.
 */
void
GpTagCheckAll(List *tags)
{
	ListCell   *lc;

	if (tags == NIL)
		return;

	foreach(lc, tags)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->arg != NULL)
			GpTagValidate(def->defname, defGetString(def));
	}
}

/* The name Cloudberry's messages give the object a TAG clause is on. */
static char *
tag_object_name(Oid classId, Oid objectId)
{
	switch (classId)
	{
		case RelationRelationId:
			return get_rel_name(objectId);
		case NamespaceRelationId:
			return get_namespace_name(objectId);
		case AuthIdRelationId:
			return GetUserNameFromId(objectId, false);
		case DatabaseRelationId:
			return get_database_name(objectId);
		case TableSpaceRelationId:
			return get_tablespace_name(objectId);
		default:
			return psprintf("%u", objectId);
	}
}

/*
 * What Cloudberry says of a TAG clause as it applies one (tag.c,
 * AddTagDescriptions, AlterTagDescriptions and UnsetTagDescriptions): with the
 * object it makes, a tag named twice is refused; on an ALTER, a tag the object
 * does not carry yet is added with a WARNING, and one taken away that it does
 * not carry is refused.  An index's tags are rows, and are not asked about.
 */
void
GpTagCheckClause(Oid classId, Oid objectId, List *tags, bool creating)
{
	ObjectAddress addr;
	char	   *label;
	Jsonb	   *had = NULL;
	List	   *seen = NIL;
	char	   *objname;

	if (tags == NIL || tag_checked_elsewhere())
		return;
	if (classId == RelationRelationId)
	{
		char		relkind = get_rel_relkind(objectId);

		if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX ||
			get_rel_persistence(objectId) == RELPERSISTENCE_TEMP)
			return;
	}

	objname = tag_object_name(classId, objectId);
	ObjectAddressSet(addr, classId, objectId);
	label = GetSecurityLabel(&addr, GP_TAG_PROVIDER);
	if (label != NULL)
		had = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(label)));

	foreach_node(DefElem, def, tags)
	{
		JsonbValue	buf;
		bool		carried;

		carried = list_member(seen, makeString(def->defname)) ||
			(had != NULL && JB_ROOT_IS_OBJECT(had) &&
			 getKeyJsonValueFromContainer(&had->root, def->defname,
										  strlen(def->defname), &buf) != NULL);
		if (creating)
		{
			if (carried)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("tag \"%s\" value has been added for object \"%s\".",
								def->defname, objname)));
		}
		else if (def->arg == NULL)
		{
			if (!carried)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("object \"%s\" does not have tag \"%s\"",
								objname, def->defname)));
			seen = list_delete(seen, makeString(def->defname));
			continue;
		}
		else if (!carried)
			ereport(WARNING,
					(errmsg("object \"%s\" does not have tag \"%s\", creating",
							objname, def->defname)));
		seen = lappend(seen, makeString(def->defname));
	}
}

/*
 * Tags an index carries.  A security label cannot reach an index -- PG19's
 * SecLabelSupportsObjectType says so -- so they go to a table of the module's
 * own.  The cost is that they are dumped with that table rather than beside
 * the index, which is the one place tags round-trip less well than the rest.
 */
static void
tag_apply_to_index(Oid indexRelId, List *tags)
{
	ListCell   *lc;

	tag_require_extension();

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	foreach(lc, tags)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		Oid			argtypes[3] = {OIDOID, NAMEOID, TEXTOID};
		Datum		values[3];

		values[0] = ObjectIdGetDatum(indexRelId);
		values[1] = CStringGetDatum(def->defname);

		if (def->arg == NULL)
		{
			if (SPI_execute_with_args("DELETE FROM " GP_SQL_SCHEMA ".index_tag"
									  " WHERE indexrelid = $1 AND tagname = $2",
									  2, argtypes, values, NULL, false, 0) != SPI_OK_DELETE)
				elog(ERROR, "gp_sql: could not remove a tag from an index");
			continue;
		}

		values[2] = CStringGetTextDatum(defGetString(def));

		if (SPI_execute_with_args(
								  "INSERT INTO " GP_SQL_SCHEMA ".index_tag"
								  "       (indexrelid, tagname, tagvalue)"
								  " VALUES ($1, $2, $3)"
								  " ON CONFLICT (indexrelid, tagname)"
								  " DO UPDATE SET tagvalue = excluded.tagvalue",
								  3, argtypes, values, NULL, false, 0) != SPI_OK_INSERT)
			elog(ERROR, "gp_sql: could not record the tags of an index");
	}

	SPI_finish();
}

void
GpTagApplyToRelation(Oid relId, List *tags)
{
	char		relkind;

	if (tags == NIL)
		return;

	GpTagCheckAll(tags);

	/* Cloudberry's temporary table carries no tags, whatever it was given */
	if (get_rel_persistence(relId) == RELPERSISTENCE_TEMP)
		return;

	relkind = get_rel_relkind(relId);
	if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
	{
		tag_apply_to_index(relId, tags);
		return;
	}

	GpTagApplyToObject(RelationRelationId, relId, tags);
}

/*
 * The label of an object the statement being run has just made or altered,
 * with the tags added, or taken away where one has no value: a relation, a
 * schema, a role, a database or a tablespace.
 *
 * SetSecurityLabel rather than a SECURITY LABEL statement: an object this
 * statement made is the user's, and one it altered has been checked already
 * -- ALTER TABLE, ALTER DATABASE and ALTER TABLESPACE check the object is the
 * user's before they alter it, and for ALTER USER, which checks next to
 * nothing when all it carries is tags, gp_sql makes SECURITY LABEL's check
 * itself before it runs.
 */
void
GpTagApplyToObject(Oid classId, Oid objectId, List *tags)
{
	ObjectAddress addr;
	char	   *merged;

	if (tags == NIL)
		return;

	ObjectAddressSet(addr, classId, objectId);
	merged = tag_merge(GetSecurityLabel(&addr, GP_TAG_PROVIDER), tags);

	/* An object with no tags left loses its label rather than keeping "{}". */
	SetSecurityLabel(&addr, GP_TAG_PROVIDER,
					 strcmp(merged, "{}") == 0 ? NULL : merged);
}

/*
 * An index is being dropped.  Its tags are rows in a table rather than a
 * label, so nothing drops them for us.
 */
void
GpTagIndexDropped(Oid indexRelId)
{
	Oid			argtypes[1] = {OIDOID};
	Datum		values[1];

	if (!OidIsValid(tag_table_oid("index_tag")))
		return;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	values[0] = ObjectIdGetDatum(indexRelId);
	if (SPI_execute_with_args("DELETE FROM " GP_SQL_SCHEMA ".index_tag"
							  " WHERE indexrelid = $1",
							  1, argtypes, values, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "gp_sql: could not forget the tags of a dropped index");

	SPI_finish();
}

/* ------------------------------------------------------------------------- */
/* SQL                                                                       */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_sql_validate_tag);

/*
 * gp_sql.validate_tag(name, text)
 *
 * The check the label provider makes, for the paths that do not go through
 * SECURITY LABEL -- an index tag, which is a row rather than a label.
 */
Datum
gp_sql_validate_tag(PG_FUNCTION_ARGS)
{
	Name		tagname = PG_GETARG_NAME(0);
	text	   *tagvalue = PG_GETARG_TEXT_PP(1);

	GpTagValidate(NameStr(*tagname), text_to_cstring(tagvalue));

	PG_RETURN_VOID();
}

/* The two that take a NULL list take no NULL name. */
static void
tagdef_require_name(FunctionCallInfo fcinfo)
{
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a tag must have a name")));
}

PG_FUNCTION_INFO_V1(gp_sql_tag_definitions);
PG_FUNCTION_INFO_V1(gp_sql_lock_tag_definitions);
PG_FUNCTION_INFO_V1(gp_sql_define_tag);
PG_FUNCTION_INFO_V1(gp_sql_redefine_tag);
PG_FUNCTION_INFO_V1(gp_sql_rename_tag_definition);
PG_FUNCTION_INFO_V1(gp_sql_undefine_tag);
PG_FUNCTION_INFO_V1(gp_sql_change_tag_owner);

/*
 * gp_sql.tag_definitions() -> SETOF (oid, tagname, tagowner, allowed_values)
 *
 * The definitions as Cloudberry's pg_tag has them.
 */
Datum
gp_sql_tag_definitions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	foreach_ptr(TagDef, d, tagdef_load())
	{
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};
		NameData   *name = palloc0(sizeof(NameData));

		namestrcpy(name, d->name);
		values[0] = ObjectIdGetDatum(d->oid);
		values[1] = NameGetDatum(name);
		values[2] = ObjectIdGetDatum(d->owner);
		if (d->listed)
		{
			int			n = list_length(d->values);
			Datum	   *elems = palloc_array(Datum, Max(n, 1));
			int			i = 0;

			foreach_ptr(char, value, d->values)
				elems[i++] = CStringGetTextDatum(value);
			values[3] = PointerGetDatum(construct_array_builtin(elems, n, TEXTOID));
		}
		else
			nulls[3] = true;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * gp_sql.lock_tag_definitions(tagname name DEFAULT NULL)
 *
 * What CREATE, ALTER and DROP TAG begin with, so that what they read of the
 * definitions is still so when they write them: the definitions locked until
 * the transaction ends, and a tag named, if it exists, its caller's.
 */
Datum
gp_sql_lock_tag_definitions(PG_FUNCTION_ARGS)
{
	TagDef	   *d;

	(void) tagdef_lock();
	if (!PG_ARGISNULL(0) &&
		(d = tagdef_find(tagdef_load(), NameStr(*PG_GETARG_NAME(0)))) != NULL)
		tagdef_check_owner(d);
	PG_RETURN_VOID();
}

/*
 * gp_sql.define_tag(tagname name, allowed_values text[])
 *
 * A new tag, the caller's.  The list is taken as given: create_tag has
 * checked it by Cloudberry's rules.  NULL, for any value.
 */
Datum
gp_sql_define_tag(PG_FUNCTION_ARGS)
{
	const char *name;
	Oid			role;
	List	   *defs;
	TagDef	   *d;

	tagdef_require_name(fcinfo);
	name = NameStr(*PG_GETARG_NAME(0));
	role = tagdef_lock();
	defs = tagdef_load();

	if (tagdef_find(defs, name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tag \"%s\" already exists", name)));

	d = palloc0(sizeof(TagDef));
	d->name = pstrdup(name);
	d->oid = GetNewObjectId();
	d->owner = GetUserId();
	if (!PG_ARGISNULL(1))
	{
		d->listed = true;
		d->values = text_array_list(PG_GETARG_ARRAYTYPE_P(1));
	}
	tagdef_store(role, lappend(defs, d));
	PG_RETURN_VOID();
}

/*
 * gp_sql.redefine_tag(tagname name, allowed_values text[])
 *
 * A tag's list replaced, by its owner; NULL takes the list away.
 */
Datum
gp_sql_redefine_tag(PG_FUNCTION_ARGS)
{
	Oid			role;
	List	   *defs;
	TagDef	   *d;

	tagdef_require_name(fcinfo);
	role = tagdef_lock();
	defs = tagdef_load();
	d = tagdef_existing(defs, NameStr(*PG_GETARG_NAME(0)));

	tagdef_check_owner(d);
	d->listed = !PG_ARGISNULL(1);
	d->values = d->listed ? text_array_list(PG_GETARG_ARRAYTYPE_P(1)) : NIL;
	tagdef_store(role, defs);
	PG_RETURN_VOID();
}

/* gp_sql.rename_tag_definition(tagname name, newname name) */
Datum
gp_sql_rename_tag_definition(PG_FUNCTION_ARGS)
{
	const char *newname = NameStr(*PG_GETARG_NAME(1));
	Oid			role = tagdef_lock();
	List	   *defs = tagdef_load();
	TagDef	   *d = tagdef_existing(defs, NameStr(*PG_GETARG_NAME(0)));

	tagdef_check_owner(d);
	if (tagdef_find(defs, newname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tag \"%s\" already exists", newname)));
	d->name = pstrdup(newname);
	tagdef_store(role, defs);
	PG_RETURN_VOID();
}

/* gp_sql.undefine_tag(tagname name) */
Datum
gp_sql_undefine_tag(PG_FUNCTION_ARGS)
{
	Oid			role = tagdef_lock();
	List	   *defs = tagdef_load();
	TagDef	   *d = tagdef_existing(defs, NameStr(*PG_GETARG_NAME(0)));

	tagdef_check_owner(d);
	tagdef_store(role, list_delete_ptr(defs, d));
	PG_RETURN_VOID();
}

/*
 * gp_sql.change_tag_owner(tagname name, newowner name)
 *
 * By AlterObjectOwner_internal's rules, which Cloudberry's ALTER TAG ...
 * OWNER TO goes through: the owner, or a superuser, and a new owner the
 * caller may become.
 */
Datum
gp_sql_change_tag_owner(PG_FUNCTION_ARGS)
{
	Oid			role = tagdef_lock();
	List	   *defs = tagdef_load();
	TagDef	   *d = tagdef_existing(defs, NameStr(*PG_GETARG_NAME(0)));
	Oid			newowner = get_role_oid(NameStr(*PG_GETARG_NAME(1)), false);

	tagdef_check_owner(d);
	if (!superuser())
		check_can_set_role(GetUserId(), newowner);
	d->owner = newowner;
	tagdef_store(role, defs);
	PG_RETURN_VOID();
}
