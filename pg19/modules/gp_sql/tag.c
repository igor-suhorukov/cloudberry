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
 * extension can create neither, so definitions become an ordinary table and
 * assignments become a security label:
 *
 *	  SECURITY LABEL FOR gp_tag ON TABLE t IS '{"env": "prod"}'
 *
 * which is what Cloudberry writes as TAG (env = 'prod').  A label is the
 * right shape for this: it is transactional, it is dropped with its object,
 * and pg_dump writes it -- so assignments survive a dump and restore, which
 * they do not in Cloudberry, whose own tools never dumped them.
 *
 * Why a second provider, beside gp_core's "gp".  The keys of a "gp" label are
 * fixed, and checked against a list when the label is set, because they are
 * the port's own and a key that is not on the list is a typo.  Tag names are
 * the user's, and what constrains them is the definitions table rather than a
 * list in a header, so they need a check of their own.  The label is a JSON
 * object rather than the "gp" provider's key=value text, because a tag value
 * is user data and may hold a comma.
 *
 * Cloudberry source this file is made of:
 *	  src/backend/commands/tag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "catalog/pg_type.h"
#include "commands/seclabel.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"

#include "gp_sql.h"

/*
 * Cloudberry's MAX_TAG_NUMBER (src/backend/commands/tag.c:62): how many tags
 * one object may carry.
 */
#define GP_TAG_MAX_PER_OBJECT	50

/* ------------------------------------------------------------------------- */
/* The definitions table                                                     */
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
 * Tags are defined in gp_sql.tag, so this module needs its own extension
 * installed in the database whose objects are being tagged.  Say that, rather
 * than let a query fail with "relation does not exist".
 */
static void
tag_require_extension(void)
{
	if (!OidIsValid(tag_table_oid("tag")))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tags need the \"%s\" extension in this database",
						GP_SQL_SCHEMA),
				 errhint("Run \"CREATE EXTENSION gp_sql\".")));
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
	Oid			argtypes[2] = {NAMEOID, TEXTOID};
	Datum		values[2];
	bool		defined;
	bool		allowed = false;
	bool		isnull = true;

	tag_require_extension();

	/*
	 * SPI nests, so this works whether or not the caller is already running
	 * SQL.  Nothing is finished on the error paths: a transaction that is
	 * unwinding closes the stack itself.
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	values[0] = CStringGetDatum(tagname);
	values[1] = CStringGetTextDatum(tagvalue);

	if (SPI_execute_with_args("SELECT t.allowed_values IS NULL"
							  "       OR $2 = ANY (t.allowed_values)"
							  "  FROM " GP_SQL_SCHEMA ".tag t"
							  " WHERE t.tagname = $1",
							  2, argtypes, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "gp_sql: could not read " GP_SQL_SCHEMA ".tag");

	defined = (SPI_processed > 0);
	if (defined)
		allowed = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0],
											 SPI_tuptable->tupdesc, 1, &isnull));

	SPI_finish();

	if (!defined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tag \"%s\" does not exist", tagname)));

	if (isnull || !allowed)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tag value \"%s\" is not in tag \"%s\" allowed values",
						tagvalue, tagname)));
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
 * left is the shape of the label and the tags it names.
 */
static void
gp_tag_check(const ObjectAddress *object, const char *seclabel)
{
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	char	   *key = NULL;
	int			count = 0;

	if (seclabel == NULL)		/* removing every tag is always fine */
		return;

	tag_require_extension();

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(seclabel)));

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
			if (v.type != jbvString)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("tag \"%s\" must be given a string value", key)));

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
 * The same for CREATE DATABASE, whose options have no namespace: the
 * desugarer writes each tag there as an option named "gp_tag.<name>"
 * (gp_desugar.c, tag_option).
 */
List *
GpTagTakeDatabaseOptions(List **options)
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

	tag_require_extension();

	foreach(lc, tags)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (def->arg != NULL)
			GpTagValidate(def->defname, defGetString(def));
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

	relkind = get_rel_relkind(relId);
	if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
	{
		tag_apply_to_index(relId, tags);
		return;
	}

	GpTagApplyToObject(RelationRelationId, relId, tags);
}

/*
 * The label of an object the statement being run has just made, with the
 * tags added: a relation, a database or a tablespace.
 *
 * SetSecurityLabel rather than a SECURITY LABEL statement: the object was
 * created by this statement, so its owner is the user running it, and there
 * is nothing left for the ownership check to find out.
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
