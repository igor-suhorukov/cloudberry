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
 *   1. a partition takes its parent's distribution, and one the user made
 *      says so;
 *   2. the columns every PRIMARY KEY and UNIQUE constraint has in common,
 *      silently -- a unique constraint is only enforceable on one segment if
 *      every row it compares is there;
 *   3. an inheriting table takes its parent's, and says so -- before the
 *      statement runs, as Cloudberry says it while analyzing it;
 *   4. a table made LIKE another takes that one's, and says so;
 *   5. with gp.create_table_random_default_distribution on, random;
 *   6. the first column whose type can be hashed, with the NOTICE;
 *   7. random, when no column can be, with the NOTICE Cloudberry gives then.
 *
 * CREATE TABLE AS has its own order, Cloudberry's planner's
 * (cdbllize_adjust_top_path): random if the session says so, else the
 * query's own distribution, where it becomes columns of the table, else the
 * first column that can be hashed.
 *
 * A key rules 2 and 6 choose, or DISTRIBUTED BY names without an operator
 * class, is hashed with its types' legacy cdbhash_*_ops classes where
 * gp.use_legacy_hashops asks for them, as Cloudberry's gp_use_legacy_hashops
 * does, and the label names them; the key CREATE TABLE AS takes from its
 * query has the types' defaults, as Cloudberry's planner gives it.
 *
 * And a distribution the statement names is checked as Cloudberry checks it,
 * against the table and its constraints and indexes, in Cloudberry's words:
 * see "What DISTRIBUTED BY may say" below.
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
 * Every new table is spread over every segment, unless the session has said
 * otherwise through gp_debug_numsegments, Cloudberry's extension for making
 * partial tables -- the first so many segments, as a cluster's expansion
 * leaves its tables until each is expanded.  The label's numsegments key
 * records it, only where it is not every segment; a partition is spread as
 * its parent is.
 *
 * Cloudberry sources this file stands in for:
 *	  transformDistributedBy() in src/backend/parser/parse_utilcmd.c, and
 *	  gpcontrib/gp_debug_numsegments
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "gp_core_api.h"
#include "gp_grammar_int.h"
#include "gp_label.h"
#include "gp_policy.h"
#include "gp_sql.h"

bool		gp_create_table_random_default_distribution = false;
bool		gp_enable_statement_trigger = false;

/*
 * How many segments a new table is spread over: a count, or one of
 * Cloudberry's three words for one -- every segment, a random count, one.
 * gp_debug_numsegments sets it, and resets it to what it last reset it to.
 */
#define GP_DEFAULT_NUMSEGMENTS_FULL		(-1)
#define GP_DEFAULT_NUMSEGMENTS_RANDOM	(-2)
#define GP_DEFAULT_NUMSEGMENTS_MINIMAL	(-3)

static int	create_table_default_numsegments = GP_DEFAULT_NUMSEGMENTS_FULL;
static int	reset_numsegments = GP_DEFAULT_NUMSEGMENTS_FULL;

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

/* The key's column names, or NIL for a policy that has none. */
static List *
key_names(const char *policy)
{
	List	   *names = NIL;
	ListCell   *lc;

	foreach(lc, GpPolicyParseKey(policy, InvalidOid))
		names = lappend(names, ((GpPolicyKeyName *) lfirst(lc))->column);
	return names;
}

static void
set_policy_label(Oid relid, const char *policy)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_distributed_by, policy);
}

/* The segments a new table is spread over, as Cloudberry's GP_POLICY_DEFAULT_NUMSEGMENTS(). */
static int
default_numsegments(void)
{
	int			cluster = GpCoreApiLookup()->get_segment_count();

	switch (create_table_default_numsegments)
	{
		case GP_DEFAULT_NUMSEGMENTS_FULL:
			return cluster;
		case GP_DEFAULT_NUMSEGMENTS_RANDOM:
			return 1 + (int) pg_prng_uint64_range(&pg_global_prng_state, 0,
												  cluster - 1);
		case GP_DEFAULT_NUMSEGMENTS_MINIMAL:
			return 1;
		default:
			return Min(create_table_default_numsegments, cluster);
	}
}

/* The label's numsegments: none where it is every segment, the default. */
static void
set_numsegments_label(Oid relid, int numsegments)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	if (numsegments >= GpCoreApiLookup()->get_segment_count())
	{
		if (GpLabelGet(&addr, GP_LABEL_numsegments) != NULL)
			GpLabelSet(&addr, GP_LABEL_numsegments, NULL);
	}
	else
		GpLabelSet(&addr, GP_LABEL_numsegments, psprintf("%d", numsegments));
}

void
GpDistributionSetNew(Oid relid, const char *policy)
{
	set_policy_label(relid, policy);
	set_numsegments_label(relid, default_numsegments());
}

/* A partition, distributed and spread as its parent is. */
static void
set_policy_as_parent(Oid relid, Oid parent, const char *policy)
{
	ObjectAddress addr;
	char	   *numsegments;

	set_policy_label(relid, policy);
	ObjectAddressSet(addr, RelationRelationId, parent);
	numsegments = GpLabelGet(&addr, GP_LABEL_numsegments);
	if (numsegments != NULL)
	{
		ObjectAddressSet(addr, RelationRelationId, relid);
		GpLabelSet(&addr, GP_LABEL_numsegments, numsegments);
	}
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
 * The key a table gets by default on these columns, as DISTRIBUTED BY naming
 * them would give it, as Cloudberry's is: checked, and each column hashed
 * with the operator class gp.use_legacy_hashops chooses.
 */
static char *
default_key(Oid relid, List *names)
{
	return GpDistributionCheckKey(relid, column_list(names), -1, NULL, false);
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
	List	   *unique_keys;
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
			GpDistributionSetNew(relid, "replicated");
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
			set_policy_as_parent(relid, parent, policy);
		return;
	}

	/* 2. What every unique constraint has in common. */
	rel = relation_open(relid, AccessShareLock);
	unique_keys = unique_key_columns(rel);
	if (unique_keys != NIL)
	{
		relation_close(rel, AccessShareLock);
		GpDistributionSetNew(relid, default_key(relid, unique_keys));
		return;
	}

	/* 3. A table that inherits is distributed as its parent is. */
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
			/* the NOTICE was given before the statement ran */
			relation_close(rel, AccessShareLock);
			GpDistributionSetNew(relid, policy);
			return;
		}
	}

	/* 4. A table made LIKE another is distributed as that one is. */
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
			relation_close(rel, AccessShareLock);
			ereport(NOTICE,
					(errmsg("table doesn't have 'DISTRIBUTED BY' clause, defaulting to distribution columns from LIKE table")));
			GpDistributionSetNew(relid, policy);
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
		GpDistributionSetNew(relid, "random");
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
			GpDistributionSetNew(relid, default_key(relid, list_make1(name)));
			return;
		}
	}

	/* 7. Nothing to hash: random. */
	relation_close(rel, AccessShareLock);
	ereport(NOTICE,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("Table doesn't have 'DISTRIBUTED BY' clause, and no column type is suitable for a distribution key. Creating a NULL policy entry.")));
	GpDistributionSetNew(relid, "random");
}

/* A relation's name in SQL, pg_temp for a temporary one. */
static char *
sql_name(Oid relid)
{
	Oid			nsp = get_rel_namespace(relid);

	if (isAnyTempNamespace(nsp))
		return psprintf("pg_temp.%s", quote_identifier(get_rel_name(relid)));
	return quote_qualified_identifier(get_namespace_name(nsp), get_rel_name(relid));
}

static void
run_sql(const char *sql, int expected)
{
	int			rc = SPI_execute(sql, false, 0);

	if (rc != expected)
		elog(ERROR, "could not redistribute the table: %s gave %d", sql, rc);
}

/*
 * The objects that depend on a table's system columns: a view that reads
 * ctid, say.  Cloudberry does not show a replicated table's system columns
 * -- each segment's copy of a row has a ctid and an xmin of its own -- and so
 * refuses to make one replicated while anything reads them, in
 * checkDependencies' words.  A view reads them through its rule, and is
 * named for it.
 */
/* The relation a rule is on: a view, for its _RETURN rule. */
static Oid
rule_relation(Oid ruleoid)
{
	Relation	rewrite = table_open(RewriteRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			relid = InvalidOid;

	ScanKeyInit(&key, Anum_pg_rewrite_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleoid));
	scan = systable_beginscan(rewrite, RewriteOidIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		relid = ((Form_pg_rewrite) GETSTRUCT(tuple))->ev_class;
	systable_endscan(scan);
	table_close(rewrite, AccessShareLock);
	return relid;
}

static void
refuse_system_column_dependents(Oid relid)
{
	Relation	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	StringInfoData detail;
	int			n = 0;

	initStringInfo(&detail);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tuple);
		ObjectAddress dependent;
		ObjectAddress column;

		if (dep->refobjsubid >= 0)
			continue;
		ObjectAddressSubSet(dependent, dep->classid, dep->objid, dep->objsubid);
		if (dep->classid == RewriteRelationId)
		{
			Oid			view = rule_relation(dep->objid);

			if (OidIsValid(view))
				ObjectAddressSet(dependent, RelationRelationId, view);
		}
		ObjectAddressSubSet(column, RelationRelationId, relid, dep->refobjsubid);
		appendStringInfo(&detail, "%s%s depends on %s", n++ > 0 ? "\n" : "",
						 getObjectDescription(&dependent, false),
						 getObjectDescription(&column, false));
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	if (n > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot set distributed replicated because other object depend on its system columns"),
				 errdetail_internal("%s", detail.data),
				 errhint("system columns of replicated table will be exposed to users after altering, resolve dependencies first")));
}

/* Does the table have a PRIMARY KEY, or else a unique index?  'p', 'u' or 0. */
static char
has_unique_index(Oid relid)
{
	Relation	rel = relation_open(relid, NoLock);
	char		found = 0;
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(rel))
	{
		HeapTuple	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index idx;

		if (!HeapTupleIsValid(tuple))
			continue;
		idx = (Form_pg_index) GETSTRUCT(tuple);
		if (idx->indisprimary)
			found = 'p';
		else if (idx->indisunique && found == 0)
			found = 'u';
		ReleaseSysCache(tuple);
	}
	relation_close(rel, NoLock);
	return found;
}

/*
 * ALTER TABLE ... SET DISTRIBUTED, and SET WITH (REORGANIZE = ...).
 *
 * Checked first as Cloudberry checks it -- PostgreSQL's ALTER TABLE never
 * sees this subcommand, so its own checks are made here too: the table's
 * owner, and no system catalog.  Then Cloudberry's (ATPrepCmd and
 * ATExecSetDistributedBy in tablecmds.c): the key's columns; an interior
 * partition's policy set not at all, a leaf's only to its parent's, and a
 * partitioned table's neither to replicated nor ONLY; a random policy on no
 * table with a unique index, a replicated one on none whose system columns
 * something reads; and a key that each unique index and exclusion constraint
 * can still be enforced under.  A policy that is the table's already is left
 * as it is, with Cloudberry's WARNING, unless REORGANIZE says to move the
 * rows anyway.
 *
 * Cloudberry's rules for when the rows move: always if REORGANIZE is true,
 * never if it is false, and otherwise when the policy changes to anything
 * but random -- rows are where a random policy may leave them -- except
 * from replicated, where every segment has every row and random would count
 * each once per segment.  A partition keeps its parent's policy, as
 * Cloudberry requires; a partitioned table's partitions take the new one
 * with it.
 *
 * The rows move as the statements that would move them: copied out under
 * the old policy into a temporary table, the table emptied, the policy set,
 * and the rows put back, each on the segment the new policy names.  In the
 * statement's transaction, under its AccessExclusiveLock.  "recurse" is
 * false for ALTER TABLE ONLY.
 */
void
GpDistributionAlter(Oid relid, const char *policy, int reorganize, bool recurse)
{
	const GpCoreApi *core = GpCoreApiLookup();
	char		relkind = get_rel_relkind(relid);
	char	   *old = policy_label_of(relid);
	const char *new;
	bool		move;
	List	   *rels;
	ListCell   *lc;
	char	   *tmp = NULL;
	Relation	target = relation_open(relid, NoLock);
	bool		catalog = IsSystemRelation(target);

	relation_close(target, NoLock);
	if (!allowSystemTableMods && catalog)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						get_rel_name(relid))));
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(relkind),
					   get_rel_name(relid));
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", get_rel_name(relid))));
	if (old == NULL)
		old = "random";

	if (policy != NULL)
	{
		new = GpDistributionCheckKey(relid, policy, -1, NULL, true);

		if (relkind == RELKIND_PARTITIONED_TABLE && get_rel_relispartition(relid))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("can't set the distribution policy of \"%s\"",
							get_rel_name(relid)),
					 errhint("Distribution policy can not be set for an interior branch.")));
		if (strcmp(old, new) != 0)
		{
			if (get_rel_relispartition(relid))
			{
				char	   *parent = policy_label_of(get_partition_parent(relid, true));

				if (parent != NULL && strcmp(parent, new) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("can't set the distribution policy of \"%s\"",
									get_rel_name(relid)),
							 errhint("Distribution policy of a partition can only be the same as its parent's.")));
			}
			if (relkind == RELKIND_PARTITIONED_TABLE && strcmp(new, "replicated") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("can't set the distribution policy of a partition table to REPLICATED")));
			if (relkind == RELKIND_PARTITIONED_TABLE && !recurse)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("can't set the distribution policy of \"%s\" ONLY",
								get_rel_name(relid)),
						 errhint("Distribution policy can be set for an entire partitioned table, not for one of its leaf parts or an interior branch.")));
		}

		if (strcmp(new, "random") == 0)
		{
			char		unique = has_unique_index(relid);

			if (unique != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot set to DISTRIBUTED RANDOMLY because relation has %s",
								unique == 'p' ? "primary Key" : "unique index"),
						 errhint("Drop the %s first.",
								 unique == 'p' ? "primary key" : "unique index")));
			if (reorganize != 1 && strcmp(old, "random") == 0)
				ereport(WARNING,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("distribution policy of relation \"%s\" already set to DISTRIBUTED RANDOMLY",
								get_rel_name(relid)),
						 errhint("Use ALTER TABLE \"%s\" SET WITH (REORGANIZE=TRUE) DISTRIBUTED RANDOMLY to force a random redistribution.",
								 get_rel_name(relid))));
		}
		else if (strcmp(new, "replicated") == 0)
		{
			if (strcmp(old, "replicated") == 0)
			{
				ereport(WARNING,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("distribution policy of relation \"%s\" already set to DISTRIBUTED REPLICATED",
								get_rel_name(relid)),
						 errhint("Use ALTER TABLE \"%s\" SET WITH (REORGANIZE=TRUE) DISTRIBUTED REPLICATED to force a replicated redistribution.",
								 get_rel_name(relid))));
				return;
			}
			refuse_system_column_dependents(relid);
		}
		else
		{
			if (reorganize != 1 && strcmp(old, new) == 0)
			{
				StringInfoData names;

				initStringInfo(&names);
				foreach(lc, key_names(new))
					appendStringInfo(&names, "%s%s", names.len > 0 ? ", " : "",
									 (char *) lfirst(lc));
				ereport(WARNING,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("distribution policy of relation \"%s\" already set to (%s)",
								get_rel_name(relid), names.data),
						 errhint("Use ALTER TABLE \"%s\" SET WITH (REORGANIZE=TRUE) DISTRIBUTED BY (%s) to force redistribution",
								 get_rel_name(relid), names.data)));
				return;
			}
			GpDistributionCheckIndexes(relid, new, true);
		}
	}
	else
		new = old;

	if (reorganize == 1)
		move = true;
	else if (reorganize == 0)
		move = false;
	else
		move = strcmp(old, new) != 0 ?
			(strcmp(new, "random") != 0 || strcmp(old, "replicated") == 0) : false;

	/* On one node, or on a segment, the policy is only a label. */
	if (core == NULL || core->is_single_node() ||
		core->get_role() != GP_ROLE_DISPATCH)
		move = false;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	if (move)
	{
		tmp = psprintf("gp_redistribute_%u", relid);
		run_sql(psprintf("CREATE TEMP TABLE %s AS SELECT * FROM %s DISTRIBUTED RANDOMLY",
						 quote_identifier(tmp), sql_name(relid)),
				SPI_OK_UTILITY);
	}

	rels = find_all_inheritors(relid, NoLock, NULL);
	foreach(lc, rels)
		set_policy_label(lfirst_oid(lc), new);

	if (move)
	{
		Relation	rel = relation_open(relid, NoLock);
		TupleDesc	tupdesc = RelationGetDescr(rel);
		StringInfoData cols;
		bool		first = true;

		initStringInfo(&cols);
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped || att->attgenerated != '\0')
				continue;
			appendStringInfo(&cols, "%s%s", first ? "" : ", ",
							 quote_identifier(NameStr(att->attname)));
			first = false;
		}
		relation_close(rel, NoLock);

		run_sql(psprintf("TRUNCATE %s", sql_name(relid)), SPI_OK_UTILITY);
		run_sql(psprintf("INSERT INTO %s (%s) OVERRIDING SYSTEM VALUE SELECT %s FROM pg_temp.%s",
						 sql_name(relid), cols.data, cols.data, quote_identifier(tmp)),
				SPI_OK_INSERT);
		run_sql(psprintf("DROP TABLE pg_temp.%s", quote_identifier(tmp)),
				SPI_OK_UTILITY);
	}

	SPI_finish();
}

/* ------------------------------------------------------------------------- */
/* The key's columns, as the table's change                                  */
/* ------------------------------------------------------------------------- */

/*
 * The label records the key by its columns' names, where Cloudberry's
 * catalog records their numbers: so a column renamed is renamed in the
 * label, and one dropped from the key leaves the table random, as Cloudberry
 * leaves it.  On a segment the coordinator's label arrives with the
 * statement, and nothing is done here.
 */


/* The coordinator of a cluster, where a distribution is decided. */
static bool
on_cluster_coordinator(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && !core->is_single_node() &&
		core->get_role() == GP_ROLE_DISPATCH;
}

static bool
acting_here(void)
{
	const GpCoreApi *core = GpCoreApiLookup();

	return core != NULL && core->get_role() != GP_ROLE_EXECUTE;
}

void
GpDistributionColumnDropped(Oid relid, AttrNumber attnum)
{
	char	   *name;
	ListCell   *lc;

	if (!acting_here())
		return;
	name = get_attname(relid, attnum, true);
	if (name == NULL)
		return;
	foreach(lc, key_names(policy_label_of(relid)))
	{
		if (strcmp((char *) lfirst(lc), name) != 0)
			continue;

		set_policy_label(relid, "random");
		/* one node has no distribution to speak of, as Cloudberry's has none */
		if (!GpCoreApiLookup()->is_single_node())
			ereport(NOTICE,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("dropping a column that is part of the distribution policy forces a random distribution policy")));
		return;
	}
}

void
GpDistributionColumnRenamed(Oid relid, const char *oldname, const char *newname)
{
	List	   *rels;
	ListCell   *lr;

	if (!acting_here())
		return;
	rels = find_all_inheritors(relid, NoLock, NULL);
	foreach(lr, rels)
	{
		Oid			rel = lfirst_oid(lr);
		List	   *keys = GpPolicyParseKey(policy_label_of(rel), rel);
		bool		found = false;
		ListCell   *lc;

		foreach(lc, keys)
		{
			GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);

			if (strcmp(key->column, oldname) == 0)
			{
				key->column = pstrdup(newname);
				found = true;
			}
		}
		if (found)
			set_policy_label(rel, GpPolicyFormatKey(keys));
	}
}

/* ------------------------------------------------------------------------- */
/* What DISTRIBUTED BY may say                                               */
/* ------------------------------------------------------------------------- */

/*
 * Where the n'th column of a DISTRIBUTED BY clause is in the user's text, for
 * the caret under an error about it.  The option the rewrite made of the
 * clause stands at the clause's DISTRIBUTED (gp_desugar.c,
 * rw_distribution), so the clause is read again from there; -1 where the
 * text there is not the clause -- an option written by hand, or no text.
 */
static int
key_location(const char *queryString, int location, int n)
{
	GpTokens   *ts;
	int			i = 3;

	if (queryString == NULL || location < 0 ||
		location >= (int) strlen(queryString))
		return -1;
	ts = GpTokenize(queryString + location);
	if (!tok_is(ts, 0, "distributed") || !tok_is(ts, 1, "by") ||
		!tok_is_char(ts, 2, '('))
		return -1;
	for (int k = 0; i < ts->ntoks && !tok_is_char(ts, i, ')'); k++)
	{
		if (k == n)
			return tok_is_name(ts, i) ? location + ts->toks[i].off : -1;
		/* past this column, and its operator class, to the next one */
		while (i < ts->ntoks && !tok_is_char(ts, i, ',') && !tok_is_char(ts, i, ')'))
			i++;
		if (tok_is_char(ts, i, ','))
			i++;
	}
	return -1;
}

static int
key_errposition(const char *queryString, int location, int n)
{
	int			at = key_location(queryString, location, n);

	if (at < 0)
		return 0;
	return errposition(pg_mbstrlen_with_len(queryString, at) + 1);
}

/*
 * A key DISTRIBUTED BY names, checked against the table the statement made or
 * is changing, as Cloudberry checks one (transformDistributedBy and
 * getPolicyForDistributedBy): each column the table's own and not one it
 * generates, which is computed after the row's segment is chosen; each
 * hashed with the operator class it names -- a hash class that takes its
 * type -- or with the one gp.use_legacy_hashops chooses, its type's legacy
 * class or its default, one of which it has to have.  Returns the key as the
 * label records it: an operator class qualified, and left out where it is
 * the type's default anyway.  "alter" says an ALTER TABLE's, whose missing
 * column Cloudberry reports in fewer words; "location" is the option's, for
 * the caret (key_location).  A policy with no key is returned as it is.
 */
char *
GpDistributionCheckKey(Oid relid, const char *policy, int location,
					   const char *queryString, bool alter)
{
	List	   *keys = GpPolicyParseKey(policy, relid);
	Relation	rel;
	ListCell   *lc;
	int			n = 0;

	if (keys == NIL)
		return pstrdup(policy);

	rel = relation_open(relid, NoLock);
	foreach(lc, keys)
	{
		GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);
		AttrNumber	attnum = get_attnum(relid, key->column);
		Form_pg_attribute att;
		Oid			deflt;
		Oid			opclass;

		if (attnum <= 0 && alter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist", key->column)));
		if (attnum <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in DISTRIBUTED BY clause does not exist",
							key->column),
					 key_errposition(queryString, location, n)));
		att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);

		if (att->attgenerated != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use generated column in distribution key"),
					 errdetail("Column \"%s\" is a generated column.", key->column)));

		deflt = GpPolicyDefaultOpclass(att->atttypid);
		opclass = GpPolicyColumnOpclass(key->opclass != NULL ?
										stringToQualifiedNameList(key->opclass, NULL) : NIL,
										att->atttypid);
		key->opclass = (opclass == deflt) ? NULL : GpPolicyOpclassName(opclass);
		n++;
	}
	relation_close(rel, NoLock);

	return GpPolicyFormatKey(keys);
}

/* The name a CREATE TABLE gives a relation it inherits, for a message. */
static void
refuse_replicated_parent(CreateStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->inhRelations)
	{
		RangeVar   *parent = lfirst_node(RangeVar, lc);
		Oid			parentid = RangeVarGetRelid(parent, NoLock, true);
		char	   *policy = OidIsValid(parentid) ? policy_label_of(parentid) : NULL;

		if (policy != NULL && strcmp(policy, "replicated") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot inherit from replicated table \"%s\" to create table \"%s\"",
							parent->relname, stmt->relation->relname),
					 errdetail("An inheritance hierarchy cannot contain a mixture of distributed and non-distributed tables.")));
	}
}

/*
 * The PRIMARY KEY and UNIQUE constraints a CREATE TABLE writes, each as the
 * list of its columns' names: a column's own is that column.
 */
static void
statement_unique_constraints(CreateStmt *stmt, List **pkey, List **uniques)
{
	List	   *constraints = NIL;
	List	   *columns = NIL;
	ListCell   *lc;
	ListCell   *lcol;

	foreach(lc, stmt->tableElts)
	{
		Node	   *elt = (Node *) lfirst(lc);

		if (IsA(elt, ColumnDef))
		{
			ListCell   *c;

			foreach(c, ((ColumnDef *) elt)->constraints)
			{
				constraints = lappend(constraints, lfirst(c));
				columns = lappend(columns, ((ColumnDef *) elt)->colname);
			}
		}
		else if (IsA(elt, Constraint))
		{
			constraints = lappend(constraints, elt);
			columns = lappend(columns, NULL);
		}
	}
	foreach(lc, stmt->constraints)
	{
		constraints = lappend(constraints, lfirst(lc));
		columns = lappend(columns, NULL);
	}

	*pkey = NIL;
	*uniques = NIL;
	forboth(lc, constraints, lcol, columns)
	{
		Constraint *con = (Constraint *) lfirst(lc);
		List	   *names = NIL;
		ListCell   *k;

		if (!IsA(con, Constraint) ||
			(con->contype != CONSTR_PRIMARY && con->contype != CONSTR_UNIQUE))
			continue;
		if (con->keys == NIL && lfirst(lcol) != NULL)
			names = list_make1(lfirst(lcol));
		foreach(k, con->keys)
			names = lappend(names, strVal(lfirst(k)));

		if (con->contype == CONSTR_PRIMARY)
			*pkey = names;
		*uniques = lappend(*uniques, names);
	}
}

static bool
has_name(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
		if (strcmp((char *) lfirst(lc), name) == 0)
			return true;
	return false;
}

/*
 * The rest of Cloudberry's transformDistributedBy for a CREATE TABLE that
 * named its distribution: a replicated table neither inherits nor is
 * partitioned; a table does not inherit a replicated one; and the key is
 * among the columns of the PRIMARY KEY and of each UNIQUE constraint the
 * statement writes, since a segment enforces one only among its own rows.
 * A random table is checked no further here, as in Cloudberry: its unique
 * indexes are refused by the index check (GpDistributionCheckIndexes).
 * "stmt" is the statement as it was before it ran, which PostgreSQL's
 * analysis rewrites.
 */
void
GpDistributionCheckCreate(CreateStmt *stmt, Oid relid, const char *policy)
{
	List	   *key = key_names(policy);
	List	   *pkey;
	List	   *uniques;
	ListCell   *lc;
	ListCell   *u;

	if (strcmp(policy, "replicated") == 0)
	{
		if (stmt->inhRelations != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INHERITS clause cannot be used with DISTRIBUTED REPLICATED clause")));
		if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PARTITION BY clause cannot be used with DISTRIBUTED REPLICATED clause")));
		return;
	}
	if (key == NIL)
		return;

	refuse_replicated_parent(stmt);

	statement_unique_constraints(stmt, &pkey, &uniques);
	foreach(lc, key)
	{
		if (pkey != NIL && !has_name(pkey, (char *) lfirst(lc)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("PRIMARY KEY and DISTRIBUTED BY definitions are incompatible"),
					 errhint("When there is both a PRIMARY KEY and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be a subset of the PRIMARY KEY.")));
	}
	foreach(u, uniques)
	{
		foreach(lc, key)
		{
			if (!has_name((List *) lfirst(u), (char *) lfirst(lc)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("UNIQUE constraint and DISTRIBUTED BY definitions are incompatible"),
						 errhint("When there is both a UNIQUE constraint and a DISTRIBUTED BY clause, the DISTRIBUTED BY clause must be a subset of the UNIQUE constraint.")));
		}
	}
}

/*
 * Before a CREATE TABLE with no DISTRIBUTED BY runs: what Cloudberry says
 * about its parents while it analyzes the statement, and so before anything
 * PostgreSQL says while it runs it -- "merging multiple inherited
 * definitions", say.  A table does not inherit a replicated one; and one that
 * takes its distribution from its parent says so, unless a PRIMARY KEY or
 * UNIQUE constraint of its own decides it (rule 2).  "quiet" for the
 * partitions Cloudberry's classic partition clauses make, as Cloudberry
 * makes them with their parent's distribution named.
 */
void
GpDistributionNoteDefault(CreateStmt *stmt, bool quiet)
{
	List	   *pkey;
	List	   *uniques;
	ListCell   *lc;
	bool		inherits = false;

	if (!on_cluster_coordinator() || creating_extension)
		return;
	if (stmt->if_not_exists &&
		OidIsValid(RangeVarGetRelid(stmt->relation, NoLock, true)))
		return;

	if (stmt->partbound != NULL)
	{
		if (!quiet)
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("table has parent, setting distribution columns to match parent table")));
		return;
	}
	if (stmt->inhRelations == NIL)
		return;

	refuse_replicated_parent(stmt);

	statement_unique_constraints(stmt, &pkey, &uniques);
	if (uniques != NIL)
		return;
	foreach(lc, stmt->inhRelations)
	{
		Oid			parent = RangeVarGetRelid(lfirst_node(RangeVar, lc), NoLock, true);

		if (OidIsValid(parent) && policy_label_of(parent) != NULL)
			inherits = true;
	}
	if (inherits && !quiet)
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("table has parent, setting distribution columns to match parent table")));
}

/* ------------------------------------------------------------------------- */
/* Unique indexes, exclusion constraints and the distribution                */
/* ------------------------------------------------------------------------- */

/*
 * The equality a hash operator family hashes for, for this type: its own, or
 * one for a type it is binary coercible to -- varchar's is text's.
 * Cloudberry's cdb_eqop_in_hash_opfamily().
 */
static Oid
eqop_in_hash_opfamily(Oid opfamily, Oid typeoid)
{
	Oid			eqop = get_opfamily_member(opfamily, typeoid, typeoid,
										   HTEqualStrategyNumber);
	CatCList   *catlist;

	if (OidIsValid(eqop))
		return eqop;

	catlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamily));
	for (int i = 0; i < catlist->n_members; i++)
	{
		Form_pg_amop amop = (Form_pg_amop) GETSTRUCT(&catlist->members[i]->tuple);

		if (amop->amopstrategy == HTEqualStrategyNumber &&
			amop->amoplefttype == amop->amoprighttype &&
			IsBinaryCoercible(typeoid, amop->amoplefttype))
		{
			eqop = amop->amopopr;
			break;
		}
	}
	ReleaseSysCacheList(catlist);
	return eqop;
}

/* An operator class's name for a message: qualified where it is not visible. */
static char *
format_opclass(Oid opclass)
{
	HeapTuple	tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	Form_pg_opclass form;
	char	   *result;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator class %u", opclass);
	form = (Form_pg_opclass) GETSTRUCT(tuple);
	if (OpclassIsVisible(opclass))
		result = pstrdup(quote_identifier(NameStr(form->opcname)));
	else
		result = quote_qualified_identifier(get_namespace_name(form->opcnamespace),
											NameStr(form->opcname));
	ReleaseSysCache(tuple);
	return result;
}

/*
 * Cloudberry's index_check_policy_compatible(): can each segment enforce this
 * unique index or exclusion constraint among its own rows?  Under a random
 * policy none; under a replicated one every one; under a hashed one those
 * that take in each column of the key, compared by the equality the key's
 * operator class hashes for -- then two rows the index holds equal hash
 * alike, and are on one segment.  If not, Cloudberry's error, worded for the
 * index being made, or with for_alter for the policy being set
 * (errdetails_index_policy).
 */
static void
check_index_policy(Relation rel, Relation index, GpPolicy *policy,
				   bool for_alter)
{
	Form_pg_index idx = index->rd_index;
	Oid		   *exclops = NULL;
	oidvector  *indclass;
	bool		primary = idx->indisprimary;
	bool		is_constraint;
	const char *name = RelationGetRelationName(index);

	if (!idx->indisunique && !idx->indisexclusion)
		return;
	if (GpPolicyIsEntry(policy) || GpPolicyIsReplicated(policy))
		return;

	if (idx->indisexclusion)
	{
		Oid		   *procs;
		uint16	   *strats;

		RelationGetExclusionInfo(index, &exclops, &procs, &strats);
	}
	is_constraint = OidIsValid(get_index_constraint(RelationGetRelid(index)));

	if (GpPolicyIsRandomPartitioned(policy))
	{
		if (primary)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("PRIMARY KEY and DISTRIBUTED RANDOMLY are incompatible")));
		if (exclops != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("exclusion constraint and DISTRIBUTED RANDOMLY are incompatible")));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("UNIQUE and DISTRIBUTED RANDOMLY are incompatible")));
	}

	indclass = (oidvector *) DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID,
																	index->rd_indextuple,
																	Anum_pg_index_indclass));
	for (int i = 0; i < policy->nattrs; i++)
	{
		AttrNumber	attr = policy->attrs[i];
		Oid			typeoid = TupleDescAttr(RelationGetDescr(rel), attr - 1)->atttypid;
		Oid			eqop = eqop_in_hash_opfamily(get_opclass_family(policy->opclasses[i]),
												 typeoid);
		char	   *attname = NameStr(TupleDescAttr(RelationGetDescr(rel), attr - 1)->attname);
		Oid			found_class = InvalidOid;
		bool		found = false;
		char	   *msg;

		for (int j = 0; j < idx->indnkeyatts; j++)
		{
			Oid			indeqop;

			if (idx->indkey.values[j] != attr)
				continue;
			if (exclops != NULL)
				indeqop = exclops[j];
			else
			{
				found_class = indclass->values[j];
				indeqop = get_opfamily_member(index->rd_opfamily[j],
											  index->rd_opcintype[j],
											  index->rd_opcintype[j],
											  BTEqualStrategyNumber);
			}
			if (indeqop == eqop)
			{
				found = true;
				break;
			}
		}
		if (found)
			continue;

		/*
		 * Worded for what is being changed, as Cloudberry words it: the policy
		 * against the index it would break, or the index against the policy.
		 * Cloudberry cannot tell a UNIQUE constraint from a unique index when
		 * the policy changes, and says index.
		 */
		if (for_alter)
			msg = primary ? pstrdup("distribution policy is not compatible with the table's PRIMARY KEY")
				: exclops != NULL ? psprintf("distribution policy is not compatible with exclusion constraint \"%s\"", name)
				: psprintf("distribution policy is not compatible with UNIQUE index \"%s\"", name);
		else
			msg = primary ? "PRIMARY KEY definition must contain all columns in the table's distribution key"
				: exclops != NULL ? "exclusion constraint is not compatible with the table's distribution policy"
				: is_constraint ? "UNIQUE constraint must contain all columns in the table's distribution key"
				: "UNIQUE index must contain all columns in the table's distribution key";

		if (exclops != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg_internal("%s", msg),
					 errdetail("Distribution key column \"%s\" is not included in the constraint.",
							   attname),
					 errhint("Add \"%s\" to the constraint with the %s operator.",
							 attname, format_operator(eqop))));
		if (OidIsValid(found_class))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg_internal("%s", msg),
					 errdetail("Operator class %s of distribution key column \"%s\" is not compatible with operator class %s used in the constraint.",
							   format_opclass(policy->opclasses[i]), attname,
							   format_opclass(found_class))));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg_internal("%s", msg),
				 errdetail("Distribution key column \"%s\" is not included in the constraint.",
						   attname)));
	}
}

/*
 * Every unique index and exclusion constraint of a relation, against a policy:
 * the one it has, when "policy" is NULL -- after a CREATE TABLE, CREATE INDEX
 * or ALTER TABLE that may have made one -- or the one it is about to be given.
 * Only the coordinator of a cluster, where the distribution is.
 */
void
GpDistributionCheckIndexes(Oid relid, const char *policy, bool for_alter)
{
	Relation	rel;
	GpPolicy   *pol;
	ListCell   *lc;

	if (!on_cluster_coordinator())
		return;
	pol = policy != NULL ? GpPolicyMake(relid, policy) : GpPolicyGet(relid);
	if (GpPolicyIsEntry(pol) || GpPolicyIsReplicated(pol))
		return;

	rel = relation_open(relid, NoLock);
	foreach(lc, RelationGetIndexList(rel))
	{
		Relation	index = index_open(lfirst_oid(lc), AccessShareLock);

		check_index_policy(rel, index, pol, for_alter);
		index_close(index, AccessShareLock);
	}
	relation_close(rel, NoLock);
}

/* ------------------------------------------------------------------------- */
/* ALTER TABLE's other subcommands                                           */
/* ------------------------------------------------------------------------- */

/* Does the table have a row, on any segment? */
static bool
table_has_rows(Oid relid)
{
	bool		found;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	run_sql(psprintf("SELECT 1 FROM %s LIMIT 1", sql_name(relid)), SPI_OK_SELECT);
	found = SPI_processed > 0;
	SPI_finish();
	return found;
}

/*
 * Before an ALTER TABLE on a cluster's coordinator runs, Cloudberry's rules
 * for its subcommands that bear on the distribution.  INHERIT joins no
 * replicated table to an inheritance tree (ATExecAddInherit).  ALTER COLUMN
 * TYPE of a key column to a type its hash family does not hash -- a family
 * of its own, or none -- would put its rows on other segments, and is
 * refused while the table has any (ATPrepAlterColumnType); of an empty one
 * it is carried out, and the columns are returned for
 * GpDistributionAlterTableDone() to follow.
 */
List *
GpDistributionAlterTableCheck(AlterTableStmt *stmt)
{
	Oid			relid;
	GpPolicy   *policy;
	List	   *changed = NIL;
	ListCell   *lc;

	if (!on_cluster_coordinator() || stmt->objtype != OBJECT_TABLE)
		return NIL;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
		return NIL;
	policy = GpPolicyGet(relid);

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype == AT_AddInherit)
		{
			Oid			parent = RangeVarGetRelid((RangeVar *) cmd->def, NoLock, true);

			if (GpPolicyIsReplicated(policy))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Replicated table cannot inherit a parent")));
			if (OidIsValid(parent) && GpPolicyIsReplicated(GpPolicyGet(parent)))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Replicated table cannot be inherited")));
		}
		else if (cmd->subtype == AT_AlterColumnType &&
				 GpPolicyIsHashPartitioned(policy))
		{
			AttrNumber	attnum = get_attnum(relid, cmd->name);
			ColumnDef  *def = (ColumnDef *) cmd->def;
			Oid			newtype;
			Oid			newclass;

			for (int i = 0; i < policy->nattrs; i++)
			{
				if (attnum <= 0 || policy->attrs[i] != attnum)
					continue;
				newtype = typenameTypeId(NULL, def->typeName);
				newclass = GetDefaultOpClass(newtype, HASH_AM_OID);
				if (OidIsValid(newclass) &&
					get_opclass_family(newclass) == get_opclass_family(policy->opclasses[i]))
					continue;
				if (table_has_rows(relid))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used in a distribution policy")));
				changed = lappend(changed, pstrdup(cmd->name));
			}
		}
	}
	return changed;
}

/*
 * After it ran: an empty table's key column now of a type its old hash class
 * does not hash is hashed with its new type's default, or, where that type
 * has none, the table is random -- silently, as Cloudberry makes it.  The
 * same for each partition.
 */
void
GpDistributionAlterTableDone(AlterTableStmt *stmt, List *changed)
{
	Oid			relid;
	ListCell   *lr;

	if (changed == NIL)
		return;
	relid = RangeVarGetRelid(stmt->relation, NoLock, false);
	CommandCounterIncrement();
	foreach(lr, find_all_inheritors(relid, NoLock, NULL))
	{
		Oid			rel = lfirst_oid(lr);
		List	   *keys = GpPolicyParseKey(policy_label_of(rel), rel);
		bool		random = false;
		ListCell   *lc;

		foreach(lc, keys)
		{
			GpPolicyKeyName *key = (GpPolicyKeyName *) lfirst(lc);

			if (!has_name(changed, key->column))
				continue;
			key->opclass = NULL;
			if (!OidIsValid(GpPolicyDefaultOpclass(get_atttype(rel,
															   get_attnum(rel, key->column)))))
				random = true;
		}
		if (keys != NIL)
			set_policy_label(rel, random ? "random" : GpPolicyFormatKey(keys));
	}
}

/*
 * The statements that may make a unique index or an exclusion constraint:
 * CREATE INDEX, and ALTER TABLE adding a PRIMARY KEY, a UNIQUE or EXCLUDE
 * constraint, or a column that has one.
 */
static bool
constraint_makes_index(Node *node)
{
	Constraint *con = (Constraint *) node;

	return node != NULL && IsA(node, Constraint) &&
		(con->contype == CONSTR_PRIMARY || con->contype == CONSTR_UNIQUE ||
		 con->contype == CONSTR_EXCLUSION);
}

bool
GpDistributionMakesUniqueIndex(Node *parsetree)
{
	ListCell   *lc;

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt  *stmt = (IndexStmt *) parsetree;

		return stmt->unique || stmt->primary || stmt->excludeOpNames != NIL;
	}
	if (!IsA(parsetree, AlterTableStmt))
		return false;
	foreach(lc, ((AlterTableStmt *) parsetree)->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if ((cmd->subtype == AT_AddConstraint && constraint_makes_index(cmd->def)) ||
			cmd->subtype == AT_AddIndex || cmd->subtype == AT_AddIndexConstraint)
			return true;
		if (cmd->subtype == AT_AddColumn && IsA(cmd->def, ColumnDef))
		{
			ListCell   *c;

			foreach(c, ((ColumnDef *) cmd->def)->constraints)
				if (constraint_makes_index(lfirst(c)))
					return true;
		}
	}
	return false;
}

/* After one of those ran: the table's unique indexes against its policy. */
void
GpDistributionCheckNewIndex(Node *parsetree)
{
	RangeVar   *rv = IsA(parsetree, IndexStmt) ? ((IndexStmt *) parsetree)->relation
		: ((AlterTableStmt *) parsetree)->relation;
	Oid			relid;

	CommandCounterIncrement();
	relid = RangeVarGetRelid(rv, NoLock, true);
	if (OidIsValid(relid))
		GpDistributionCheckIndexes(relid, NULL, false);
}

/* ------------------------------------------------------------------------- */
/* CREATE TABLE AS                                                           */
/* ------------------------------------------------------------------------- */

/*
 * The expressions a query's rows are hashed on as it produces them, or NIL
 * when that is not known: Cloudberry's planner derives it from the plan's
 * locus (get_partitioned_policy_from_path); this is the part of that a query
 * shows without a plan.  Rows read from one hash-distributed table, through
 * subqueries in FROM, and not grouped, aggregated, windowed, made distinct,
 * limited or combined with others are where the table's rows were: hashed on
 * its key.  Anything else -- a join, an aggregate, a UNION -- is where the
 * plan puts it, and the caller falls back to the first column.  Each
 * expression is a Var of this query's own range table.
 */
static List *
query_hash_key(Query *query)
{
	RangeTblRef *rtr;
	RangeTblEntry *rte;
	List	   *key = NIL;

	if (query->commandType != CMD_SELECT || query->setOperations != NULL ||
		query->hasAggs || query->groupClause != NIL ||
		query->groupingSets != NIL || query->hasWindowFuncs ||
		query->distinctClause != NIL || query->limitCount != NULL ||
		query->limitOffset != NULL || query->hasTargetSRFs ||
		query->havingQual != NULL || query->cteList != NIL ||
		query->jointree == NULL || list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef))
		return NIL;

	rtr = linitial_node(RangeTblRef, query->jointree->fromlist);
	rte = rt_fetch(rtr->rtindex, query->rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		GpPolicy   *policy = GpPolicyGet(rte->relid);

		/* a partial table's rows are on fewer segments than a new table's */
		if (!GpPolicyIsHashPartitioned(policy) ||
			policy->numsegments != GpCoreApiLookup()->get_segment_count())
			return NIL;
		for (int i = 0; i < policy->nattrs; i++)
		{
			Oid			type;
			int32		typmod;
			Oid			collation;

			get_atttypetypmodcoll(rte->relid, policy->attrs[i], &type, &typmod,
								  &collation);
			key = lappend(key, makeVar(rtr->rtindex, policy->attrs[i], type,
									   typmod, collation, 0));
		}
		return key;
	}

	if (rte->rtekind == RTE_SUBQUERY)
	{
		ListCell   *lc;

		foreach(lc, query_hash_key(rte->subquery))
		{
			Var		   *inner = (Var *) lfirst(lc);
			TargetEntry *found = NULL;
			ListCell   *t;

			/* the subquery's column the key comes out as */
			foreach(t, rte->subquery->targetList)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, t);

				if (!tle->resjunk && equal(tle->expr, inner))
				{
					found = tle;
					break;
				}
			}
			if (found == NULL)
				return NIL;
			key = lappend(key, makeVar(rtr->rtindex, found->resno,
									   exprType((Node *) found->expr),
									   exprTypmod((Node *) found->expr),
									   exprCollation((Node *) found->expr), 0));
		}
		return key;
	}

	return NIL;
}

/*
 * The table's columns a CREATE TABLE AS query's distribution is on, by name,
 * or NIL where it has none or it is not among them.
 */
static List *
ctas_key_from_query(Oid relid, Query *query)
{
	List	   *names = NIL;
	ListCell   *lc;
	List	   *key = query_hash_key(query);

	if (key == NIL)
		return NIL;
	foreach(lc, key)
	{
		int			column = 0;
		bool		found = false;
		ListCell   *t;

		foreach(t, query->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, t);

			if (tle->resjunk)
				continue;
			column++;
			if (equal(tle->expr, lfirst(lc)))
			{
				found = true;
				break;
			}
		}
		if (!found)
			return NIL;
		names = lappend(names, get_attname(relid, column, false));
	}
	return names;
}

/*
 * The distribution of a table CREATE TABLE AS made on a cluster with no
 * DISTRIBUTED BY, as Cloudberry's planner decides it: see the file header.
 */
void
GpDistributionApplyCtasDefault(Oid relid, Query *query)
{
	List	   *key = NIL;
	StringInfoData names;
	ListCell   *lc;

	if (!on_cluster_coordinator() || policy_label_of(relid) != NULL)
		return;

	if (!gp_create_table_random_default_distribution && !creating_extension &&
		query != NULL)
		key = ctas_key_from_query(relid, query);
	if (key == NIL)
	{
		GpDistributionApplyDefault(NULL, relid);
		return;
	}

	initStringInfo(&names);
	foreach(lc, key)
		appendStringInfo(&names, "%s%s", names.len > 0 ? ", " : "",
						 (char *) lfirst(lc));
	ereport(NOTICE,
			(errcode(ERRCODE_SUCCESSFUL_COMPLETION),
			 errmsg("Table doesn't have 'DISTRIBUTED BY' clause -- Using column(s) "
					"named '%s' as the Apache Cloudberry data distribution key for this "
					"table. ", names.data),
			 errhint("The 'DISTRIBUTED BY' clause determines the distribution of data."
					 " Make sure column(s) chosen are the optimal data distribution key to minimize skew.")));
	GpDistributionSetNew(relid, column_list(key));
}

/* ------------------------------------------------------------------------- */
/* gp_debug_numsegments                                                      */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_debug_set_create_table_default_numsegments);
PG_FUNCTION_INFO_V1(gp_debug_reset_create_table_default_numsegments);
PG_FUNCTION_INFO_V1(gp_debug_get_create_table_default_numsegments);

/*
 * gp_debug_set_create_table_default_numsegments(integer | text)
 *		How many segments the tables this session creates are spread over:
 *		a count from 1 to the cluster's, or 'full', 'minimal' or 'random';
 *		answers it as gp_debug_get_create_table_default_numsegments() does.
 */
Datum
gp_debug_set_create_table_default_numsegments(PG_FUNCTION_ARGS)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	int			cluster = GpCoreApiLookup()->get_segment_count();

	if (argtype == INT4OID)
	{
		int			numsegments = PG_GETARG_INT32(0);

		if (numsegments < 1 || numsegments > cluster)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("invalid integer value for default numsegments: %d",
							numsegments),
					 errhint("Valid range: [1, %d (gp_num_contents_in_cluster)]",
							 cluster)));
		create_table_default_numsegments = numsegments;
	}
	else
	{
		char	   *str = text_to_cstring(PG_GETARG_TEXT_PP(0));

		if (pg_strcasecmp(str, "full") == 0)
			create_table_default_numsegments = GP_DEFAULT_NUMSEGMENTS_FULL;
		else if (pg_strcasecmp(str, "random") == 0)
			create_table_default_numsegments = GP_DEFAULT_NUMSEGMENTS_RANDOM;
		else if (pg_strcasecmp(str, "minimal") == 0)
			create_table_default_numsegments = GP_DEFAULT_NUMSEGMENTS_MINIMAL;
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("invalid text value for default numsegments: '%s'", str),
					 errhint("Valid values: 'full', 'minimal', 'random'")));
	}

	return gp_debug_get_create_table_default_numsegments(fcinfo);
}

/*
 * gp_debug_reset_create_table_default_numsegments([integer | text])
 *		With an argument, set it as above, and make it what a reset returns
 *		to; without, return to that, or to 'full'.
 */
Datum
gp_debug_reset_create_table_default_numsegments(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() == 1)
	{
		(void) gp_debug_set_create_table_default_numsegments(fcinfo);
		reset_numsegments = create_table_default_numsegments;
	}
	else
		create_table_default_numsegments = reset_numsegments;

	PG_RETURN_VOID();
}

/* gp_debug_get_create_table_default_numsegments(): FULL, RANDOM, MINIMAL or the count */
Datum
gp_debug_get_create_table_default_numsegments(PG_FUNCTION_ARGS)
{
	const char *result;

	switch (create_table_default_numsegments)
	{
		case GP_DEFAULT_NUMSEGMENTS_FULL:
			result = "FULL";
			break;
		case GP_DEFAULT_NUMSEGMENTS_RANDOM:
			result = "RANDOM";
			break;
		case GP_DEFAULT_NUMSEGMENTS_MINIMAL:
			result = "MINIMAL";
			break;
		default:
			result = psprintf("%d", create_table_default_numsegments);
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * CREATE TRIGGER ... FOR EACH STATEMENT on a cluster, which Cloudberry's
 * grammar refuses unless gp_enable_statement_trigger is on: a statement the
 * coordinator sends to every segment is a statement on each, and a trigger
 * for it would fire once per segment -- the port's own writes refuse a
 * table that has one for the same reason (gp_explicit.c).  An extension's
 * script may make one, as gp_core's does for its catalogs; on one node a
 * statement is one statement, and nothing is refused.
 */
void
GpDistributionCheckTrigger(CreateTrigStmt *stmt)
{
	if (stmt->row || gp_enable_statement_trigger || creating_extension ||
		!on_cluster_coordinator())
		return;
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("Triggers for statements are not yet supported")));
}

void
GpDistributionDefineSettings(void)
{
	DefineCustomBoolVariable("gp.enable_statement_trigger",
							 "Enables statement triggers to be created instead of erroring out.",
							 NULL,
							 &gp_enable_statement_trigger,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

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
