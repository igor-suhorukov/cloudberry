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
 * distribution.c
 *	  The distribution a table gets on a cluster when nobody wrote one.
 *
 * On Cloudberry every table is distributed.  CREATE TABLE with no
 * DISTRIBUTED BY picks a key and says which, in a NOTICE that 345 of its
 * expected outputs hold; this is that choice, in Cloudberry's order and with
 * its words (transformDistributedBy, parse_utilcmd.c):
 *
 *   1. a partition takes its parent's distribution, silently;
 *   2. an inheriting table takes its parent's, and says so;
 *   3. a table made LIKE another takes that one's, and says so;
 *   4. the columns every PRIMARY KEY and UNIQUE constraint has in common,
 *      silently -- a unique constraint is only enforceable on one segment if
 *      every row it compares is there;
 *   5. with gp.create_table_random_default_distribution on, random;
 *   6. the first column whose type can be hashed, with the NOTICE;
 *   7. random, when no column can be, with the NOTICE Cloudberry gives then.
 *
 * On a single node there is nothing to distribute over, and nothing here
 * runs: a table there is where it is, and its label stays what the user
 * wrote, as it has since M1.
 *
 * A table an extension's script makes is replicated (decision 14b), so that a
 * function running on a segment -- PostGIS's ST_Transform reading
 * spatial_ref_sys -- finds every row wherever it runs.  The port's own
 * modules are the exception: their tables are the coordinator's metadata,
 * as Cloudberry's catalogs are, and stay there.
 *
 * Cloudberry sources this file stands in for:
 *	  transformDistributedBy() in src/backend/parser/parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "commands/extension.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_policy.h"
#include "gp_sql.h"

bool		gp_create_table_random_default_distribution = false;

/* The port's own modules, whose scripts make the coordinator's metadata. */
static const char *const port_extensions[] = {
	"gp_core", "gp_sql", "gp_task", "gp_security", "gp_matview", "gp_orca",
	"gp_ao", "pax", "gp_exttable", "gp_resource", "gp_tde", "gp_probe",
};

static bool
creating_port_extension(void)
{
	char	   *name;

	if (!creating_extension)
		return false;
	name = get_extension_name(CurrentExtensionObject);
	if (name == NULL)
		return false;
	for (int i = 0; i < lengthof(port_extensions); i++)
		if (strcmp(name, port_extensions[i]) == 0)
			return true;
	return false;
}

/* The label a relation's distribution is kept in, or NULL. */
static char *
policy_label_of(Oid relid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	return GpLabelGet(&addr, GP_LABEL_distributed_by);
}

static void
set_policy_label(Oid relid, const char *policy)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_distributed_by, policy);
}

/* "(a,b)", each name quoted where it needs to be, as the label reads it. */
static char *
column_list(List *names)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '(');
	foreach(lc, names)
	{
		if (lc != list_head(names))
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, quote_identifier((char *) lfirst(lc)));
	}
	appendStringInfoChar(&buf, ')');
	return buf.data;
}

/*
 * The columns every PRIMARY KEY and UNIQUE constraint of the new table has in
 * common, the primary key's order first.  NIL when there are none; an error,
 * as Cloudberry's, when two of them share no column, because no distribution
 * could then keep both enforceable.
 */
static List *
unique_key_columns(Relation rel)
{
	List	   *indexes = RelationGetIndexList(rel);
	List	   *keys = NIL;
	bool		have_key = false;
	ListCell   *lc;

	/* The primary key first, so that its order is the key's. */
	foreach(lc, indexes)
	{
		Relation	idx = index_open(lfirst_oid(lc), AccessShareLock);

		if (idx->rd_index->indisprimary)
		{
			for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
			{
				AttrNumber	att = idx->rd_index->indkey.values[i];

				if (att > 0)
					keys = lappend_int(keys, att);
			}
			have_key = true;
		}
		index_close(idx, AccessShareLock);
	}

	foreach(lc, indexes)
	{
		Relation	idx = index_open(lfirst_oid(lc), AccessShareLock);
		List	   *common = NIL;
		ListCell   *k;

		if (!idx->rd_index->indisunique || idx->rd_index->indisprimary)
		{
			index_close(idx, AccessShareLock);
			continue;
		}

		if (!have_key)
		{
			for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
			{
				AttrNumber	att = idx->rd_index->indkey.values[i];

				if (att > 0)
					keys = lappend_int(keys, att);
			}
			have_key = true;
			index_close(idx, AccessShareLock);
			continue;
		}

		foreach(k, keys)
		{
			for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
				if (idx->rd_index->indkey.values[i] == lfirst_int(k))
				{
					common = lappend_int(common, lfirst_int(k));
					break;
				}
		}
		index_close(idx, AccessShareLock);

		if (common == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("UNIQUE or PRIMARY KEY definitions are incompatible with each other"),
					 errhint("When there are multiple PRIMARY KEY / UNIQUE constraints, they must have at least one column in common.")));
		keys = common;
	}

	{
		List	   *names = NIL;

		foreach(lc, keys)
			names = lappend(names,
							pstrdup(NameStr(TupleDescAttr(RelationGetDescr(rel),
														  lfirst_int(lc) - 1)->attname)));
		return names;
	}
}

/*
 * The distribution of a table CREATE TABLE made on a cluster with no
 * DISTRIBUTED BY; see the file header for the order of the rules.
 */
void
GpDistributionApplyDefault(CreateStmt *stmt, Oid relid)
{
	const GpCoreApi *core = GpCoreApiLookup();
	Relation	rel = NULL;
	char	   *policy = NULL;
	char		relkind;

	if (core == NULL || core->is_single_node() ||
		core->get_role() != GP_ROLE_DISPATCH)
		return;

	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		return;

	/* One the user already gave, in whatever way, stands. */
	if (policy_label_of(relid) != NULL)
		return;

	if (creating_extension)
	{
		if (!creating_port_extension())
			set_policy_label(relid, "replicated");
		return;
	}

	/*
	 * A table CREATE TABLE AS made has no statement of its own to look at:
	 * no parent, no LIKE, no constraints.  Its columns decide, from rule 5.
	 */
	if (stmt == NULL)
		goto columns;

	/* 1. A partition is distributed as its parent is. */
	if (stmt->partbound != NULL)
	{
		Oid			parent = RangeVarGetRelid(linitial_node(RangeVar, stmt->inhRelations),
											  NoLock, false);

		policy = policy_label_of(parent);
		if (policy != NULL)
			set_policy_label(relid, policy);
		return;
	}

	/* 2. So is a table that inherits, and it says so. */
	if (stmt->inhRelations != NIL)
	{
		ListCell   *lc;

		foreach(lc, stmt->inhRelations)
		{
			Oid			parent = RangeVarGetRelid(lfirst_node(RangeVar, lc),
												  NoLock, false);

			policy = policy_label_of(parent);
			if (policy != NULL)
				break;
		}
		if (policy != NULL)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("table has parent, setting distribution columns to match parent table")));
			set_policy_label(relid, policy);
			return;
		}
	}

	/* 3. A table made LIKE another is distributed as that one is. */
	{
		ListCell   *lc;

		foreach(lc, stmt->tableElts)
		{
			Node	   *elt = (Node *) lfirst(lc);

			if (IsA(elt, TableLikeClause))
			{
				Oid			like = RangeVarGetRelid(((TableLikeClause *) elt)->relation,
													NoLock, false);

				policy = policy_label_of(like);
				if (policy != NULL)
					break;
			}
		}
		if (policy != NULL)
		{
			ereport(NOTICE,
					(errmsg("table doesn't have 'DISTRIBUTED BY' clause, defaulting to distribution columns from LIKE table")));
			set_policy_label(relid, policy);
			return;
		}
	}

	rel = relation_open(relid, AccessShareLock);

	/* 4. What every unique constraint has in common. */
	{
		List	   *keys = unique_key_columns(rel);

		if (keys != NIL)
		{
			relation_close(rel, AccessShareLock);
			set_policy_label(relid, column_list(keys));
			return;
		}
	}

columns:
	if (rel == NULL)
		rel = relation_open(relid, AccessShareLock);

	/* 5. Random, when that is what the user asked for by default. */
	if (gp_create_table_random_default_distribution)
	{
		relation_close(rel, AccessShareLock);
		ereport(NOTICE,
				(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
				 errmsg("using default RANDOM distribution since no distribution was specified"),
				 errhint("Consider including the 'DISTRIBUTED BY' clause to determine the distribution of rows.")));
		set_policy_label(relid, "random");
		return;
	}

	/* 6. The first column whose type can be hashed. */
	for (int i = 0; i < RelationGetDescr(rel)->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);

		if (att->attisdropped || att->attgenerated != '\0')
			continue;

		if (OidIsValid(GpPolicyDefaultOpclass(att->atttypid)))
		{
			char	   *name = pstrdup(NameStr(att->attname));

			relation_close(rel, AccessShareLock);
			ereport(NOTICE,
					(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
					 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column "
							"named '%s' as the Apache Cloudberry data distribution key for this "
							"table. ", name),
					 errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
							 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
			set_policy_label(relid, column_list(list_make1(name)));
			return;
		}
	}

	/* 7. Nothing to hash: random. */
	relation_close(rel, AccessShareLock);
	ereport(NOTICE,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("Table doesn't have 'DISTRIBUTED BY' clause, and no column type is suitable for a distribution key. Creating a NULL policy entry.")));
	set_policy_label(relid, "random");
}

void
GpDistributionDefineSettings(void)
{
	DefineCustomBoolVariable("gp.create_table_random_default_distribution",
							 "Distribute a table randomly when CREATE TABLE names no distribution.",
							 "Otherwise the key is taken from the table's unique "
							 "constraints, or else its first column that can be hashed.",
							 &gp_create_table_random_default_distribution,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
}
