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

#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_policy.h"

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
 * Split "(a,"" b"",c)" into its column names.
 *
 * The names were written by quote_identifier(), so a name is either bare or
 * double-quoted with doubled quotes inside it.  The scanner had already
 * downcased an unquoted identifier and dequoted a quoted one before the label
 * was written, so what comes out here is the true column name and needs no
 * further folding.
 *
 * Every malformed shape raises.  The writer refuses them, but a label can
 * also be set by hand through SECURITY LABEL, and a policy read wrong is a
 * plan built on the wrong distribution.
 */
static List *
parse_column_list(const char *value, Oid relid)
{
	const char *p = value;
	List	   *names = NIL;
	StringInfoData buf;

	Assert(*p == '(');
	p++;

	initStringInfo(&buf);

	for (;;)
	{
		resetStringInfo(&buf);

		while (*p == ' ')
			p++;

		if (*p == '"')
		{
			p++;
			for (;;)
			{
				if (*p == '\0')
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unterminated quoted column name in distribution policy \"%s\" on \"%s\"",
									value, get_rel_name(relid))));
				if (*p == '"')
				{
					if (p[1] == '"')	/* "" is one quote */
					{
						appendStringInfoChar(&buf, '"');
						p += 2;
						continue;
					}
					p++;
					break;
				}
				appendStringInfoChar(&buf, *p++);
			}
		}
		else
		{
			while (*p != '\0' && *p != ',' && *p != ')')
				appendStringInfoChar(&buf, *p++);

			/* A bare name cannot end in spaces, so they are separators. */
			while (buf.len > 0 && buf.data[buf.len - 1] == ' ')
				buf.data[--buf.len] = '\0';
		}

		if (buf.len == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("empty column name in distribution policy \"%s\" on \"%s\"",
							value, get_rel_name(relid))));

		names = lappend(names, pstrdup(buf.data));

		while (*p == ' ')
			p++;

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

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("malformed distribution policy \"%s\" on \"%s\"",
						value, get_rel_name(relid)),
				 errhint("A column list is a parenthesised list of column names, such as \"(a,b)\".")));
	}

	if (*p != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("trailing text after the column list in distribution policy \"%s\" on \"%s\"",
						value, get_rel_name(relid))));

	return names;
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

static GpPolicy *
policy_read(Oid relid, bool check)
{
	ObjectAddress addr;
	char	   *value;
	GpPolicy   *policy;
	List	   *names;
	ListCell   *lc;
	int			i = 0;
	int			numsegments;

	ObjectAddressSet(addr, RelationRelationId, relid);
	value = GpLabelGet(&addr, GP_LABEL_distributed_by);

	if (value == NULL)
		return NULL;

	numsegments = policy_numsegments(&addr, check);

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

	names = parse_column_list(value, relid);
	policy = make_policy(POLICYTYPE_PARTITIONED, list_length(names),
						 numsegments);

	foreach(lc, names)
	{
		const char *name = (const char *) lfirst(lc);
		AttrNumber	attnum = get_attnum(relid, name);
		Oid			typeoid;
		Oid			opclass;

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

GpPolicy *
GpPolicyGet(Oid relid)
{
	return policy_read(relid, true);
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
