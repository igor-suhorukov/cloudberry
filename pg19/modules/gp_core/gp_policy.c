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
 * gp_policy.c
 *	  Reading a distribution policy out of the "gp" label.
 *
 * The label carries what DISTRIBUTED BY said, in one of three shapes:
 *
 *	   random			 no key; a row may be on any segment
 *	   replicated		 every segment holds every row
 *	   (a,"b,c")		 hashed on those columns, each quoted where it needs
 *						 to be
 *
 * A column of the key may name the operator class it is hashed with, after
 * it and qualified -- (a public.abs_int_hash_ops,b) -- where that is not its
 * type's default, as DISTRIBUTED BY (a abs_int_hash_ops) says; a column
 * without one is hashed with its type's default, as Cloudberry's is when
 * DISTRIBUTED BY names none.  The table depends on each class it names, as
 * Cloudberry's does through its catalog row (gp_label.c records it).
 *
 * gp_sql.set_distribution() writes it and refuses any other shape; this is
 * the reader.  The parentheses are load-bearing: without them a table hashed
 * on a column called "random" recorded the same label as a randomly
 * distributed one, which is two different distributions under one spelling.
 *
 * Beside it, the label's numsegments key: how many segments the rows are
 * spread over, the first that many, where that is not every segment --
 * Cloudberry's partial tables, which gp_debug_numsegments makes and a
 * cluster's expansion leaves behind.  With no key, every segment.
 *
 * See gp_policy.h for what is kept of Cloudberry's struct and why there is no
 * cache yet.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "gp_core_api.h"
#include "gp_hash.h"
#include "gp_label.h"
#include "gp_policy.h"
#include "gp_settings.h"

/*
 * The opclass a distribution key of this type is hashed with.
 *
 * This is Cloudberry's own answer, not a stand-in for it:
 * cdb_default_distribution_opclass_for_type() checks the type cache for a
 * hash family, a hash procedure and an equality operator, and then returns
 * exactly GetDefaultOpClass(type, HASH_AM_OID)
 * (github/cloudberry/src/backend/cdb/cdbhash.c:366-403).  What stays M2 is
 * the harder question beside it -- whether two families hash compatibly, and
 * whether a function is one of the legacy hashes -- which needs cdbhash
 * itself.  Choosing the opclass does not.
 *
 * InvalidOid when the type cannot be a distribution key; the caller reports
 * which column it was.
 */
Oid
GpPolicyDefaultOpclass(Oid typeoid)
{
	TypeCacheEntry *tcache;

	tcache = lookup_type_cache(typeoid,
							   TYPECACHE_HASH_OPFAMILY |
							   TYPECACHE_HASH_PROC |
							   TYPECACHE_EQ_OPR);

	if (!tcache->hash_opf)
		return InvalidOid;
	if (!tcache->hash_proc)
		return InvalidOid;
	if (!tcache->eq_opr)
		return InvalidOid;

	return GetDefaultOpClass(typeoid, HASH_AM_OID);
}

/*
 * The operator class a distribution key's column is hashed with: the one
 * DISTRIBUTED BY names for it, and where it names none, the type's legacy
 * class if gp.use_legacy_hashops asks for one and there is one, its default
 * otherwise -- Cloudberry's cdb_get_opclass_for_column_def(), and its
 * message where there is none.
 */
Oid
GpPolicyColumnOpclass(List *opclassName, Oid typeoid)
{
	Oid			opclass = InvalidOid;

	if (opclassName != NIL)
		return ResolveOpClass(opclassName, typeoid, "hash", HASH_AM_OID);

	if (gp_use_legacy_hashops)
		opclass = GpLegacyHashOpclassForType(typeoid);
	if (!OidIsValid(opclass))
		opclass = GpPolicyDefaultOpclass(typeoid);
	if (!OidIsValid(opclass))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("data type %s has no default operator class for access method \"%s\"",
						format_type_be(typeoid), "hash"),
				 errhint("You must specify an operator class or define a default operator class for the data type.")));
	return opclass;
}

/* The relation a malformed label is on, for the message; "?" for none. */
static const char *
label_owner(Oid relid)
{
	char	   *name = OidIsValid(relid) ? get_rel_name(relid) : NULL;

	return name != NULL ? name : "?";
}

/*
 * One identifier of a label at *p, bare or double-quoted, into buf; *p is
 * left after it.  A bare one ends at a space, a comma, a dot or the closing
 * parenthesis, none of which quote_identifier() leaves unquoted.  "what" it
 * names, for the message; false, with noerror, where it is malformed.
 */
static bool
parse_identifier(const char **pp, StringInfo buf, const char *value, Oid relid,
				 const char *what, bool noerror)
{
	const char *p = *pp;

	resetStringInfo(buf);
	if (*p == '"')
	{
		p++;
		for (;;)
		{
			if (*p == '\0')
			{
				if (noerror)
					return false;
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unterminated quoted %s in distribution policy \"%s\" on \"%s\"",
								what, value, label_owner(relid))));
			}
			if (*p == '"')
			{
				if (p[1] == '"')	/* "" is one quote */
				{
					appendStringInfoChar(buf, '"');
					p += 2;
					continue;
				}
				p++;
				break;
			}
			appendStringInfoChar(buf, *p++);
		}
	}
	else
	{
		while (*p != '\0' && *p != ',' && *p != ')' && *p != ' ' && *p != '.')
			appendStringInfoChar(buf, *p++);
	}

	if (buf->len == 0)
	{
		if (noerror)
			return false;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty %s in distribution policy \"%s\" on \"%s\"",
						what, value, label_owner(relid))));
	}
	*pp = p;
	return true;
}

/*
 * Split "(a,"" b"" public.abs_ops,c)" into its columns, each with the
 * operator class it names, if it names one.
 *
 * The names were written by quote_identifier(), so a name is either bare or
 * double-quoted with doubled quotes inside it.  The scanner had already
 * downcased an unquoted identifier and dequoted a quoted one before the label
 * was written, so what comes out here is the true column name and needs no
 * further folding.  An operator class is kept as the label spells it, which
 * stringToQualifiedNameList() reads.
 *
 * Every malformed shape raises.  The writer refuses them, but a label can
 * also be set by hand through SECURITY LABEL, and a policy read wrong is a
 * plan built on the wrong distribution.  With noerror, one is NIL instead,
 * for a reader that only wants what a well-formed one says.
 */
static List *
parse_key(const char *value, Oid relid, bool noerror)
{
	const char *p = value;
	List	   *keys = NIL;
	StringInfoData buf;

	if (value == NULL || value[0] != '(')
		return NIL;
	p++;

	initStringInfo(&buf);

	for (;;)
	{
		GpPolicyKeyName *key = palloc0(sizeof(GpPolicyKeyName));

		while (*p == ' ')
			p++;
		if (!parse_identifier(&p, &buf, value, relid, "column name", noerror))
			return NIL;
		key->column = pstrdup(buf.data);

		while (*p == ' ')
			p++;

		/* its operator class: a name, or a schema's name and a dot before it */
		if (*p != ',' && *p != ')' && *p != '\0')
		{
			const char *start = p;

			if (!parse_identifier(&p, &buf, value, relid, "operator class name", noerror))
				return NIL;
			if (*p == '.')
			{
				p++;
				if (!parse_identifier(&p, &buf, value, relid, "operator class name", noerror))
					return NIL;
			}
			key->opclass = pnstrdup(start, p - start);
			while (*p == ' ')
				p++;
		}

		keys = lappend(keys, key);

		if (*p == ',')
		{
			p++;
			continue;
		}
		if (*p == ')')
		{
			p++;
			break;
		}

		if (noerror)
			return NIL;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("malformed distribution policy \"%s\" on \"%s\"",
						value, label_owner(relid)),
				 errhint("A column list is a parenthesised list of column names, each with an operator class if it names one, such as \"(a,b public.b_ops)\".")));
	}

	if (*p != '\0')
	{
		if (noerror)
			return NIL;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("trailing text after the column list in distribution policy \"%s\" on \"%s\"",
						value, label_owner(relid))));
	}

	return keys;
}

List *
GpPolicyParseKey(const char *value, Oid relid)
{
	return parse_key(value, relid, false);
}

List *
GpPolicyParseKeyQuietly(const char *value)
{
	return parse_key(value, InvalidOid, true);
}

char *
GpPolicyFormatKey(List *keys)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '(');
	foreach(lc, keys)
	{
		GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);

		if (lc != list_head(keys))
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, quote_identifier(key->column));
		if (key->opclass != NULL)
			appendStringInfo(&buf, " %s", key->opclass);
	}
	appendStringInfoChar(&buf, ')');
	return buf.data;
}

/* The qualified name a label gives an operator class by. */
char *
GpPolicyOpclassName(Oid opclass)
{
	HeapTuple	tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	Form_pg_opclass form;
	char	   *name;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator class %u", opclass);
	form = (Form_pg_opclass) GETSTRUCT(tuple);
	name = quote_qualified_identifier(get_namespace_name(form->opcnamespace),
									  NameStr(form->opcname));
	ReleaseSysCache(tuple);
	return name;
}

/*
 * The hash operator class a label names, or InvalidOid when it names none
 * that is there: a name that stopped resolving is refused where the policy
 * is read, since the table depends on the class and so cannot lose it.
 */
Oid
GpPolicyOpclassByName(const char *name)
{
	List	   *names = stringToQualifiedNameList(name, NULL);

	if (names == NIL)
		return InvalidOid;
	return get_opclass_oid(HASH_AM_OID, names, true);
}

/*
 * How many segments the label spreads the rows over: its numsegments key, or
 * every segment.  Through the published API rather than gp_core.c's static
 * function, so that this file reads the same number every other module does;
 * it is never 0, and a consumer divides by it (see gp_core_api.h).
 *
 * More segments than the cluster has cannot be read: the rows on the ones
 * missing are nowhere to be found.  Cloudberry refuses such a table in a
 * transaction that cannot see the segments an expansion added, which is how
 * it comes by one; here a label written so is how, and it is refused the
 * same, unless "check" is off, for gp_distribution_policy, which reports the
 * label as it is.
 */
static int
policy_numsegments(const ObjectAddress *addr, bool check)
{
	const GpCoreApi *core = GpCoreApiLookup();
	int			cluster = core->get_segment_count();
	char	   *value = GpLabelGet(addr, GP_LABEL_numsegments);
	char	   *end;
	long		n;

	if (value == NULL)
		return cluster;

	errno = 0;
	n = strtol(value, &end, 10);
	if (errno != 0 || *end != '\0' || end == value || n < 1 || n > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid numsegments \"%s\" in the distribution policy of \"%s\"",
						value, get_rel_name(addr->objectId)),
				 errhint("numsegments is a count of segments, from 1 to the size of the cluster.")));

	if (check && n > cluster && !core->is_single_node())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access table \"%s\" in current transaction",
						get_rel_name(addr->objectId)),
				 errdetail("Its distribution policy spreads it over %ld segments, and the cluster has %d.",
						   n, cluster)));

	return (int) n;
}

static GpPolicy *
make_policy(GpPolicyType ptype, int nattrs, int numsegments)
{
	GpPolicy   *policy = (GpPolicy *) palloc0(sizeof(GpPolicy));

	policy->ptype = ptype;
	policy->numsegments = numsegments;
	policy->nattrs = nattrs;

	/*
	 * Cloudberry allocates the two arrays inside the same chunk as the
	 * struct, because a GpPolicy of its is a node that gets copied flat.  The
	 * port's is not a node and nothing copies it, so separate allocations are
	 * simply clearer.
	 */
	if (nattrs > 0)
	{
		policy->attrs = (AttrNumber *) palloc0(nattrs * sizeof(AttrNumber));
		policy->opclasses = (Oid *) palloc0(nattrs * sizeof(Oid));
	}

	return policy;
}

/*
 * The policy a label's distributed_by value describes for this relation, over
 * numsegments segments.
 */
static GpPolicy *
policy_from_text(Oid relid, const char *value, int numsegments, bool check)
{
	GpPolicy   *policy;
	List	   *keys;
	ListCell   *lc;
	int			i = 0;

	if (strcmp(value, "replicated") == 0)
		return make_policy(POLICYTYPE_REPLICATED, 0, numsegments);

	if (strcmp(value, "random") == 0)
		return make_policy(POLICYTYPE_PARTITIONED, 0, numsegments);

	if (value[0] != '(')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized distribution policy \"%s\" on \"%s\"",
						value, get_rel_name(relid)),
				 errhint("Use \"random\", \"replicated\", or a parenthesised "
						 "column list such as \"(a,b)\".")));

	keys = GpPolicyParseKey(value, relid);
	policy = make_policy(POLICYTYPE_PARTITIONED, list_length(keys),
						 numsegments);

	foreach(lc, keys)
	{
		GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);
		const char *name = key->column;
		AttrNumber	attnum = get_attnum(relid, name);
		Oid			typeoid;
		Oid			opclass;

		/* a system column is none of the table's own */
		if (attnum < 0)
			attnum = InvalidAttrNumber;

		/*
		 * The column may have been dropped since the label was written.
		 * Cloudberry cannot reach this: its policy is a catalog row with
		 * dependencies, and it refuses to drop a distribution key column.
		 * The port has no such hook yet, so the case is real and saying which
		 * column is missing is far better than what ORCA does with an attnum
		 * it cannot find, which is to assert "Column not found".
		 */
		if (attnum == InvalidAttrNumber && !check)
		{
			/* as it is recorded: a column that is not there, as 0 */
			policy->attrs[i] = InvalidAttrNumber;
			policy->opclasses[i] = InvalidOid;
			i++;
			continue;
		}
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of the distribution policy of \"%s\" does not exist",
							name, get_rel_name(relid))));

		typeoid = get_atttype(relid, attnum);
		if (key->opclass != NULL)
		{
			opclass = GpPolicyOpclassByName(key->opclass);
			if (!OidIsValid(opclass) && check)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("operator class \"%s\" does not exist for access method \"%s\"",
								key->opclass, "hash"),
						 errdetail("The distribution policy of \"%s\" hashes its column \"%s\" with it.",
								   get_rel_name(relid), name)));
		}
		else
			opclass = GpPolicyDefaultOpclass(typeoid);

		if (!OidIsValid(opclass) && check)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(typeoid), "hash"),
					 errdetail("Column \"%s\" of \"%s\" cannot be a distribution key.",
							   name, get_rel_name(relid)),
					 errhint("You must specify an operator class or define a default operator class for the data type.")));

		policy->attrs[i] = attnum;
		policy->opclasses[i] = opclass;
		i++;
	}

	return policy;
}

static GpPolicy *
policy_read(Oid relid, bool check)
{
	ObjectAddress addr;
	char	   *value;

	ObjectAddressSet(addr, RelationRelationId, relid);
	value = GpLabelGet(&addr, GP_LABEL_distributed_by);

	if (value == NULL)
		return NULL;

	return policy_from_text(relid, value, policy_numsegments(&addr, check),
							check);
}

GpPolicy *
GpPolicyGet(Oid relid)
{
	return policy_read(relid, true);
}

/*
 * The policy a distributed_by value would give this relation, which need not
 * be the one its label records: what a statement is about to set, checked
 * against the table first.  Over the segments the relation is spread over
 * now, which setting a policy does not change.
 */
GpPolicy *
GpPolicyMake(Oid relid, const char *value)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	return policy_from_text(relid, value, policy_numsegments(&addr, false),
							true);
}

GpPolicy *
GpPolicyGetRecorded(Oid relid)
{
	return policy_read(relid, false);
}

bool
GpPolicyIsEntry(const GpPolicy *policy)
{
	return policy == NULL || policy->ptype == POLICYTYPE_ENTRY;
}

bool
GpPolicyIsPartitioned(const GpPolicy *policy)
{
	return policy != NULL && policy->ptype == POLICYTYPE_PARTITIONED;
}

bool
GpPolicyIsRandomPartitioned(const GpPolicy *policy)
{
	return GpPolicyIsPartitioned(policy) && policy->nattrs == 0;
}

bool
GpPolicyIsHashPartitioned(const GpPolicy *policy)
{
	return GpPolicyIsPartitioned(policy) && policy->nattrs > 0;
}

bool
GpPolicyIsReplicated(const GpPolicy *policy)
{
	return policy != NULL && policy->ptype == POLICYTYPE_REPLICATED;
}
