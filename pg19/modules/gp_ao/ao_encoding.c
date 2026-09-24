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
 * ao_encoding.c
 *	  A column's own storage options, a partitioned table's for its
 *	  partitions, and gp.default_storage_options.
 *
 * Cloudberry keeps what an ENCODING clause says of a column of a table by
 * column in pg_attribute_encoding, a type's default in pg_type_encoding, and
 * a partitioned table's storage options in its pg_class.reloptions, which
 * PostgreSQL 19 refuses a partitioned table.  The port keeps all three as
 * security labels of gp_ao's provider, on the column, the type and the
 * table: a label is the object's for as long as it is, goes with it, and is
 * dumped and restored with it, which a row of a table of gp_ao's keyed by
 * OID would not be.  A label is an option list as reloptions writes one,
 * "compresstype=zlib,compresslevel=1,blocksize=32768".
 *
 * A column is given its options as its table is made or it is added, in
 * Cloudberry's order (transformColumnEncoding): its own ENCODING clause, a
 * COLUMN ... ENCODING clause naming it, the table's DEFAULT COLUMN ENCODING
 * or else the table's own compression options, the column its partition's
 * parent has of that name, its type's default, and else what the table
 * has; whichever it is is filled in -- compresstype, compresslevel and
 * blocksize, in that order after what was given -- as Cloudberry fills it.
 * The grammar (gp_sql's rewriter) gives the clauses as options gp_ao.* of the
 * statement; see ao_encoding_take().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "gp_ao.h"

#define AO_LABEL_PROVIDER	"gp_ao"

/* Cloudberry's gp_default_storage_options, of what it still takes. */
char	   *gp_default_storage_options = NULL;

/* ------------------------------------------------------------------------- */
/* Option lists                                                              */
/* ------------------------------------------------------------------------- */

/* "k=v,k=v", as reloptions' elements are written, into DefElems of strings. */
List *
ao_enc_parse(const char *text)
{
	List	   *result = NIL;
	char	   *copy = pstrdup(text);
	char	   *save;

	for (char *item = strtok_r(copy, ",", &save); item != NULL;
		 item = strtok_r(NULL, ",", &save))
	{
		char	   *eq = strchr(item, '=');
		char	   *name;

		while (*item == ' ')
			item++;
		if (*item == '\0')
			continue;
		if (eq == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid storage option \"%s\"", item),
					 errhint("A storage option is written name=value.")));
		*eq = '\0';
		name = item;
		for (char *p = eq - 1; p >= name && *p == ' '; p--)
			*p = '\0';
		eq++;
		while (*eq == ' ')
			eq++;
		for (char *c = name; *c; c++)
			*c = pg_tolower((unsigned char) *c);
		result = lappend(result, makeDefElem(pstrdup(name),
											 (Node *) makeString(pstrdup(eq)), -1));
	}
	return result;
}

char *
ao_enc_format(List *opts)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, opts)
	{
		DefElem    *def = lfirst(lc);

		if (buf.len > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfo(&buf, "%s=%s", def->defname, defGetString(def));
	}
	return buf.data;
}

static DefElem *
find_opt(List *opts, const char *name)
{
	ListCell   *lc;

	foreach(lc, opts)
	{
		DefElem    *def = lfirst(lc);

		if (strcmp(def->defname, name) == 0)
			return def;
	}
	return NULL;
}

/*
 * Are these a column's options -- or, with table, a partitioned table's?
 * Each name once, known, and its value one the table's options take.
 */
void
ao_enc_validate(List *opts, bool table)
{
	ListCell   *lc;
	List	   *seen = NIL;

	foreach(lc, opts)
	{
		DefElem    *def = lfirst(lc);

		if (strcmp(def->defname, "checksum") == 0 && !table)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"checksum\" is not a column specific option")));
		if (strcmp(def->defname, "compresstype") != 0 &&
			strcmp(def->defname, "compresslevel") != 0 &&
			strcmp(def->defname, "blocksize") != 0 &&
			strcmp(def->defname, "checksum") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter \"%s\"", def->defname)));
		if (list_member(seen, makeString(def->defname)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("parameter \"%s\" specified more than once",
							def->defname)));
		seen = lappend(seen, makeString(def->defname));
	}

	/* The values, as the table's option parser checks them. */
	{
		List	   *tmp = NIL;
		Datum		d;

		foreach(lc, opts)
			tmp = lappend(tmp, makeDefElem(((DefElem *) lfirst(lc))->defname,
										   (Node *) makeString(defGetString(lfirst(lc))), -1));
		d = transformRelOptions((Datum) 0, tmp, NULL, NULL, false, false);
		(void) ao_column_reloptions(d, RELKIND_RELATION, true);
	}
}

/*
 * The options given, with what is missing filled in, as Cloudberry fills a
 * column's: compresstype -- the defaults', or zlib where a level alone was
 * given -- then compresslevel -- the defaults' for the defaults' type, 1 for
 * another, 0 for none -- then blocksize.
 */
List *
ao_enc_fillin(List *given, const AoOptions *dflt)
{
	List	   *result = list_copy(given);
	DefElem    *type = find_opt(given, "compresstype");
	DefElem    *level = find_opt(given, "compresslevel");
	const char *typename;

	if (type == NULL)
	{
		if (level != NULL && atoi(defGetString(level)) > 0 &&
			dflt->compresstype == AO_COMPRESS_NONE)
			typename = "zlib";
		else
			typename = ao_compresstype_name(dflt->compresstype);
		result = lappend(result, makeDefElem("compresstype",
											 (Node *) makeString(pstrdup(typename)), -1));
	}
	else
		typename = defGetString(type);

	if (level == NULL)
	{
		int			l;

		if (pg_strcasecmp(typename, "none") == 0)
			l = 0;
		else if (pg_strcasecmp(typename, ao_compresstype_name(dflt->compresstype)) == 0 &&
				 dflt->compresslevel > 0)
			l = dflt->compresslevel;
		else
			l = 1;
		result = lappend(result, makeDefElem("compresslevel",
											 (Node *) makeString(psprintf("%d", l)), -1));
	}

	if (find_opt(given, "blocksize") == NULL)
		result = lappend(result, makeDefElem("blocksize",
											 (Node *) makeString(psprintf("%d", dflt->blocksize)), -1));
	return result;
}

/* A column's options, onto the table's, as a writer uses them. */
static void
enc_apply(List *opts, AoOptions *o)
{
	ListCell   *lc;

	foreach(lc, opts)
	{
		DefElem    *def = lfirst(lc);
		const char *v = defGetString(def);

		if (strcmp(def->defname, "compresstype") == 0)
		{
			if (pg_strcasecmp(v, "zlib") == 0)
				o->compresstype = AO_COMPRESS_ZLIB;
			else if (pg_strcasecmp(v, "zstd") == 0)
				o->compresstype = AO_COMPRESS_ZSTD;
			else if (pg_strcasecmp(v, "rle_type") == 0)
				o->compresstype = AO_COMPRESS_RLE;
			else
				o->compresstype = AO_COMPRESS_NONE;
		}
		else if (strcmp(def->defname, "compresslevel") == 0)
			o->compresslevel = atoi(v);
		else if (strcmp(def->defname, "blocksize") == 0)
			o->blocksize = atoi(v);
	}
}

/* ------------------------------------------------------------------------- */
/* Labels                                                                    */
/* ------------------------------------------------------------------------- */

static void
label_check(const ObjectAddress *object, const char *seclabel)
{
	if (seclabel == NULL)
		return;
	ao_enc_validate(ao_enc_parse(seclabel),
					object->classId == RelationRelationId && object->objectSubId == 0);
}

void
ao_encoding_init(void)
{
	register_label_provider(AO_LABEL_PROVIDER, label_check);
}

static char *
label_get(Oid classid, Oid objid, int subid)
{
	ObjectAddress addr;

	addr.classId = classid;
	addr.objectId = objid;
	addr.objectSubId = subid;
	return GetSecurityLabel(&addr, AO_LABEL_PROVIDER);
}

static void
label_set(Oid classid, Oid objid, int subid, const char *label)
{
	ObjectAddress addr;

	addr.classId = classid;
	addr.objectId = objid;
	addr.objectSubId = subid;
	SetSecurityLabel(&addr, AO_LABEL_PROVIDER, label);
	/* SetSecurityLabel() finds a label to replace with the catalog snapshot. */
	CommandCounterIncrement();
}

/* Every column label of a relation, by attnum; NULL where there is none. */
static char **
column_labels(Oid relid, int natts)
{
	char	  **labels = palloc0_array(char *, natts + 1);
	Relation	pg_seclabel = table_open(SecLabelRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&keys[0], Anum_pg_seclabel_objoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[1], Anum_pg_seclabel_classoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	scan = systable_beginscan(pg_seclabel, SecLabelObjectIndexId, true, NULL,
							  2, keys);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		FormData_pg_seclabel *form = (FormData_pg_seclabel *) GETSTRUCT(tup);
		bool		isnull;
		Datum		provider;
		Datum		label;

		if (form->objsubid <= 0 || form->objsubid > natts)
			continue;
		provider = heap_getattr(tup, Anum_pg_seclabel_provider,
								RelationGetDescr(pg_seclabel), &isnull);
		if (isnull || strcmp(TextDatumGetCString(provider), AO_LABEL_PROVIDER) != 0)
			continue;
		label = heap_getattr(tup, Anum_pg_seclabel_label,
							 RelationGetDescr(pg_seclabel), &isnull);
		if (!isnull)
			labels[form->objsubid] = TextDatumGetCString(label);
	}
	systable_endscan(scan);
	table_close(pg_seclabel, AccessShareLock);
	return labels;
}

/*
 * The options each column of a table by column is written with: its label's
 * on the table's own.  A rewrite writes the transient relation make_new_heap()
 * named pg_temp_<the table's OID>, which has no labels of its own; its
 * columns are the table's.
 */
void
ao_column_options(Relation rel, AoOptions *colopts)
{
	TupleDesc	desc = RelationGetDescr(rel);
	AoOptions	table;
	char	  **labels;
	Oid			source = RelationGetRelid(rel);
	Oid			old;

	ao_get_options(rel, &table);
	if (sscanf(RelationGetRelationName(rel), "pg_temp_%u", &old) == 1 &&
		get_rel_relkind(old) == RELKIND_RELATION)
		source = old;
	labels = column_labels(source, desc->natts);

	for (int i = 0; i < desc->natts; i++)
	{
		colopts[i] = table;
		if (labels[i + 1] != NULL)
			enc_apply(ao_enc_parse(labels[i + 1]), &colopts[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* The clauses, as the grammar gives them                                    */
/* ------------------------------------------------------------------------- */

/*
 * gp_sql's rewriter gives the ENCODING clauses of a CREATE TABLE as one
 * option, gp_ao.encoding, whose value is a list of items separated by ';':
 * "DEFAULT(k=v,...)" for DEFAULT COLUMN ENCODING, "COLUMN <name>(k=v,...)"
 * for COLUMN c ENCODING, and "<name>(k=v,...)" for a column definition's --
 * a name as quote_identifier() writes it.
 */
static List *
spec_parse(const char *spec)
{
	List	   *result = NIL;
	const char *p = spec;

	while (*p != '\0')
	{
		AoColumnEncoding *ce = palloc0_object(AoColumnEncoding);
		StringInfoData name;
		const char *open;
		const char *close;

		while (*p == ' ' || *p == ';')
			p++;
		if (*p == '\0')
			break;
		if (strncmp(p, "COLUMN ", 7) == 0)
		{
			ce->directive = true;
			p += 7;
		}
		initStringInfo(&name);
		if (*p == '"')
		{
			for (p++; *p != '\0'; p++)
			{
				if (*p == '"' && p[1] == '"')
					appendStringInfoChar(&name, *p++);
				else if (*p == '"')
				{
					p++;
					break;
				}
				else
					appendStringInfoChar(&name, *p);
			}
		}
		else
		{
			while (*p != '\0' && *p != '(')
				appendStringInfoChar(&name, *p++);
			if (strcmp(name.data, "DEFAULT") == 0)
				ce->is_default = true;
		}
		open = p;
		close = (*open == '(') ? strchr(open, ')') : NULL;
		if (close == NULL)
			elog(ERROR, "malformed gp_ao.encoding option \"%s\"", spec);
		ce->colname = ce->is_default ? NULL : name.data;
		ce->opts = ao_enc_parse(pnstrdup(open + 1, close - open - 1));
		ao_enc_validate(ce->opts, false);
		result = lappend(result, ce);
		p = close + 1;
	}
	return result;
}

/*
 * Take the gp_ao.* options a statement's WITH list has out of it: the
 * encoding clauses, into *encodings.  Any other name is an error.
 */
void
ao_encoding_take(List **options, List **encodings)
{
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = lfirst(lc);

		if (def->defnamespace == NULL || strcmp(def->defnamespace, "gp_ao") != 0)
			continue;
		if (strcmp(def->defname, "encoding") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter \"gp_ao.%s\"", def->defname)));
		*encodings = list_concat(*encodings, spec_parse(defGetString(def)));
		*options = foreach_delete_current(*options, lc);
	}
}

/*
 * The table's own compression options and block size, which a statement's
 * WITH list gives, as a column's options: what Cloudberry makes the default
 * column encoding of when there is no DEFAULT COLUMN ENCODING.
 */
List *
ao_storage_opts_of(List *options)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = lfirst(lc);

		if (def->defnamespace != NULL)
			continue;
		if (strcmp(def->defname, "compresstype") == 0 ||
			strcmp(def->defname, "compresslevel") == 0 ||
			strcmp(def->defname, "blocksize") == 0)
			result = lappend(result, makeDefElem(def->defname,
												 (Node *) makeString(defGetString(def)), -1));
	}
	return result;
}

/*
 * Take a partitioned table's storage options out of its statement's WITH
 * list -- PostgreSQL 19 refuses a partitioned table any -- for its label.
 */
List *
ao_partitioned_take(List **options)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = lfirst(lc);

		if (def->defnamespace != NULL)
			continue;
		if (strcmp(def->defname, "compresstype") == 0 ||
			strcmp(def->defname, "compresslevel") == 0 ||
			strcmp(def->defname, "blocksize") == 0 ||
			strcmp(def->defname, "checksum") == 0)
		{
			result = lappend(result, makeDefElem(def->defname,
												 (Node *) makeString(defGetString(def)), -1));
			*options = foreach_delete_current(*options, lc);
		}
	}
	return result;
}

void
ao_partitioned_set(Oid relid, List *opts)
{
	if (opts != NIL)
	{
		ao_enc_validate(opts, true);
		label_set(RelationRelationId, relid, 0, ao_enc_format(opts));
	}
}

/*
 * A partition of a partitioned table of the same method is made with the
 * parent's storage options, where its own statement does not give them.
 */
void
ao_partition_inherit(Oid parentid, List **options)
{
	char	   *label = label_get(RelationRelationId, parentid, 0);
	ListCell   *lc;

	if (label == NULL)
		return;
	foreach(lc, ao_enc_parse(label))
	{
		DefElem    *def = lfirst(lc);
		ListCell   *lc2;
		bool		given = false;

		foreach(lc2, *options)
		{
			DefElem    *mine = lfirst(lc2);

			if (mine->defnamespace == NULL && strcmp(mine->defname, def->defname) == 0)
				given = true;
		}
		if (!given)
			*options = lappend(*options, def);
	}
}

/* ------------------------------------------------------------------------- */
/* A table's columns, labelled                                               */
/* ------------------------------------------------------------------------- */

static AoColumnEncoding *
find_encoding(List *encodings, const char *colname)
{
	ListCell   *lc;

	foreach(lc, encodings)
	{
		AoColumnEncoding *ce = lfirst(lc);

		if ((colname == NULL && ce->is_default) ||
			(colname != NULL && !ce->is_default && strcmp(ce->colname, colname) == 0))
			return ce;
	}
	return NULL;
}

/*
 * Give the columns of a table by column their options -- those named in
 * `only`, or with only NIL every one that has none yet -- in Cloudberry's
 * order (see the file's comment).  `encodings` are the statement's
 * clauses; `withopts` the options its own WITH list gave, before any a
 * partition has from its parent.  A partitioned table's columns are given
 * only what its statement says of them, which its partitions take; the
 * rest each partition fills in for itself.
 */
void
ao_encoding_apply(Oid relid, List *encodings, List *withopts, List *only,
				  bool replace)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	char	  **labels = column_labels(relid, desc->natts);
	AoColumnEncoding *deflt = find_encoding(encodings, NULL);
	List	   *table_enc = withopts;
	Oid			parentid = InvalidOid;
	char	  **parent_labels = NULL;
	Relation	parent = NULL;
	AoOptions	dflt;
	bool		partitioned;
	ListCell   *lc;

	/* Each clause names a column the table has. */
	foreach(lc, encodings)
	{
		AoColumnEncoding *ce = lfirst(lc);

		if (!ce->is_default && get_attnum(relid, ce->colname) == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist", ce->colname)));
	}

	partitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	if (partitioned)
	{
		char	   *label = label_get(RelationRelationId, relid, 0);

		/* No defaults of its own: its partitions fill theirs in. */
		memset(&dflt, 0, sizeof(dflt));
		ao_options_from_reloptions((Datum) 0, RELKIND_RELATION, true, &dflt);
		if (label != NULL)
			enc_apply(ao_enc_parse(label), &dflt);
	}
	else
		ao_get_options(rel, &dflt);

	if (rel->rd_rel->relispartition)
	{
		parentid = get_partition_parent(relid, true);
		parent = relation_open(parentid, AccessShareLock);
		parent_labels = column_labels(parentid, RelationGetDescr(parent)->natts);
	}

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		const char *colname = NameStr(att->attname);
		AoColumnEncoding *mine;
		List	   *opts = NIL;
		char	   *typelabel;

		if (att->attisdropped)
			continue;
		if (only != NIL ? !list_member(only, makeString(pstrdup(colname))) :
			(labels[i + 1] != NULL && !replace))
			continue;

		/*
		 * A definition's ENCODING, of a column the table inherits as well,
		 * is lost where Cloudberry merges the two; COLUMN c ENCODING is not.
		 */
		if ((mine = find_encoding(encodings, colname)) != NULL &&
			(mine->directive || att->attinhcount == 0))
			opts = mine->opts;
		else if (deflt != NULL)
			opts = deflt->opts;
		else if (table_enc != NIL)
			opts = table_enc;
		else if (parent_labels != NULL)
		{
			AttrNumber	pattno = get_attnum(parentid, colname);

			if (pattno != InvalidAttrNumber && parent_labels[pattno] != NULL)
				opts = ao_enc_parse(parent_labels[pattno]);
		}
		if (partitioned && opts == NIL)
			continue;
		if (opts == NIL &&
			(typelabel = label_get(TypeRelationId, att->atttypid, 0)) != NULL)
			opts = ao_enc_parse(typelabel);

		label_set(RelationRelationId, relid, att->attnum,
				  ao_enc_format(ao_enc_fillin(opts, &dflt)));
	}

	if (parent)
		relation_close(parent, AccessShareLock);
	relation_close(rel, AccessShareLock);
}

/* Replace a column's options: ALTER COLUMN ... SET ENCODING. */
void
ao_encoding_set_column(Oid relid, const char *colname, List *opts)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	AttrNumber	attnum = get_attnum(relid, colname);
	AoOptions	dflt;

	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist", colname)));
	ao_enc_validate(opts, false);
	ao_get_options(rel, &dflt);
	label_set(RelationRelationId, relid, attnum,
			  ao_enc_format(ao_enc_fillin(opts, &dflt)));
	relation_close(rel, AccessShareLock);
}

/*
 * ALTER TYPE ... SET DEFAULT ENCODING, which the grammar makes a SECURITY
 * LABEL of gp_ao's on the type: its options, filled in as Cloudberry fills
 * a type's.
 */
char *
ao_encoding_type_label(const char *label)
{
	AoOptions	dflt;
	List	   *opts = ao_enc_parse(label);

	ao_enc_validate(opts, false);
	ao_options_from_reloptions((Datum) 0, RELKIND_RELATION, true, &dflt);
	return ao_enc_format(ao_enc_fillin(opts, &dflt));
}

/* ------------------------------------------------------------------------- */
/* gp.default_storage_options                                                */
/* ------------------------------------------------------------------------- */

bool
ao_default_storage_options_check(char **newval, void **extra, GucSource source)
{
	List	   *opts;

	if (*newval == NULL || (*newval)[0] == '\0')
		return true;
	PG_TRY();
	{
		opts = ao_enc_parse(*newval);
		ao_enc_validate(opts, true);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(TopMemoryContext);
		edata = CopyErrorData();
		FlushErrorState();
		GUC_check_errmsg("%s", edata->message);
		FreeErrorData(edata);
		return false;
	}
	PG_END_TRY();
	return true;
}

/*
 * A new append-optimized table's statement is given what
 * gp.default_storage_options says of what the statement does not.
 */
void
ao_default_storage_options_add(List **options)
{
	ListCell   *lc;

	if (gp_default_storage_options == NULL || gp_default_storage_options[0] == '\0')
		return;
	foreach(lc, ao_enc_parse(gp_default_storage_options))
	{
		DefElem    *def = lfirst(lc);
		ListCell   *lc2;
		bool		given = false;

		foreach(lc2, *options)
		{
			DefElem    *mine = lfirst(lc2);

			if (mine->defnamespace == NULL && strcmp(mine->defname, def->defname) == 0)
				given = true;
		}
		if (!given)
			*options = lappend(*options, def);
	}
}
