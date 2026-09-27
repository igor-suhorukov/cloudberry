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
 * gp_partmaint.c
 *	  gp_toolkit's functions of a partitioned table: a range partition's
 *	  rank, the lowest and highest of them, and every partition under a
 *	  table with its level, strategy and rank, which gp_partitions shows.
 *
 * Cloudberry's gp_toolkit has them in C (gp_partition_maint.c) over
 * PostgreSQL's partitioning, which PostgreSQL 19 has the same: a partition
 * descriptor lists a table's partitions in the order of their bounds, a
 * range table's default one last.  So these are Cloudberry's, with the two
 * helpers of its own core they use -- rel_is_range_part_nondefault()
 * (partition.c) and PartitionStrategyGetName() (partbounds.h) -- here.
 *
 * Cloudberry sources this file stands in for:
 *	  gpcontrib/gp_toolkit/gp_partition_maint.c,
 *	  src/backend/catalog/partition.c (rel_is_range_part_nondefault()) and
 *	  src/include/partitioning/partbounds.h (PartitionStrategyGetName())
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/parsenodes.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(gp_partition_rank);
PG_FUNCTION_INFO_V1(gp_partition_lowest_child);
PG_FUNCTION_INFO_V1(gp_partition_highest_child);
PG_FUNCTION_INFO_V1(gp_get_partitions);

/* A strategy's name, as gp_partitions shows it: PartitionStrategyGetName(). */
static const char *
strategy_name(char strategy)
{
	switch (strategy)
	{
		case PARTITION_STRATEGY_LIST:
			return "list";
		case PARTITION_STRATEGY_HASH:
			return "hash";
		case PARTITION_STRATEGY_RANGE:
			return "range";
	}
	ereport(ERROR, (errmsg("unrecognized partitioning strategy")));
	return NULL;				/* keep the compiler quiet */
}

/* Is this a range partition, not its table's default one? */
static bool
is_range_partition(Oid relid)
{
	HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Form_pg_class classForm;
	bool		result = false;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	classForm = (Form_pg_class) GETSTRUCT(tuple);
	if ((classForm->relkind == RELKIND_RELATION ||
		 classForm->relkind == RELKIND_PARTITIONED_TABLE) &&
		classForm->relispartition)
	{
		PartitionBoundSpec *boundspec = NULL;
		bool		isnull;
		Datum		datum = SysCacheGetAttr(RELOID, tuple,
											Anum_pg_class_relpartbound, &isnull);

		if (!isnull)
			boundspec = stringToNode(TextDatumGetCString(datum));
		if (boundspec == NULL)
			elog(ERROR, "missing relpartbound for relation %u", relid);
		if (!IsA(boundspec, PartitionBoundSpec))
			elog(ERROR, "invalid relpartbound for relation %u", relid);
		result = boundspec->strategy == PARTITION_STRATEGY_RANGE &&
			!boundspec->is_default;
	}
	ReleaseSysCache(tuple);
	return result;
}

/*
 * gp_toolkit.pg_partition_rank(regclass): a range partition's place among
 * its table's range partitions, from 1, in the order of their bounds --
 * NULL for any other relation.
 */
Datum
gp_partition_rank(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	parent;
	PartitionDesc partdesc;
	int			rank = 0;

	if (!is_range_partition(relid))
		PG_RETURN_NULL();

	parent = relation_open(get_partition_parent(relid, true), AccessShareLock);
	partdesc = RelationGetPartitionDesc(parent, false);
	for (int i = 0; i < partdesc->nparts; i++)
		if (partdesc->oids[i] == relid)
			rank = i + 1;
	relation_close(parent, AccessShareLock);
	if (rank == 0)
		PG_RETURN_NULL();
	PG_RETURN_INT32(rank);
}

/*
 * The partition of a table partitioned by range whose bounds are the lowest,
 * or the highest, not the default one; InvalidOid for none.
 */
static Oid
range_partition_at(Oid relid, bool highest)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	PartitionDesc partdesc;
	Oid			result = InvalidOid;

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		RelationGetPartitionKey(rel)->strategy == PARTITION_STRATEGY_RANGE &&
		(partdesc = RelationGetPartitionDesc(rel, false))->nparts > 0)
	{
		/* in the order of their bounds, the default partition last */
		int			n = partdesc->nparts;

		if (OidIsValid(get_default_partition_oid(relid)))
			n--;
		if (n > 0)
			result = partdesc->oids[highest ? n - 1 : 0];
	}
	relation_close(rel, AccessShareLock);
	return result;
}

/* gp_toolkit.pg_partition_lowest_child(regclass) */
Datum
gp_partition_lowest_child(PG_FUNCTION_ARGS)
{
	Oid			child = range_partition_at(PG_GETARG_OID(0), false);

	if (!OidIsValid(child))
		PG_RETURN_NULL();
	PG_RETURN_OID(child);
}

/* gp_toolkit.pg_partition_highest_child(regclass) */
Datum
gp_partition_highest_child(PG_FUNCTION_ARGS)
{
	Oid			child = range_partition_at(PG_GETARG_OID(0), true);

	if (!OidIsValid(child))
		PG_RETURN_NULL();
	PG_RETURN_OID(child);
}

/*
 * gp_toolkit.gp_get_partitions(regclass): each partition under a
 * partitioned table, level by level, each table's in the order of their
 * bounds -- the table itself, which pg_partition_tree() has, not among
 * them: its parent, whether it is a leaf, its level below the table given,
 * its parent's strategy, its rank where it is a range partition not the
 * default one, and whether it is the default one.  Nothing of a table not
 * partitioned.
 */
Datum
gp_get_partitions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			rootid = PG_GETARG_OID(0);
	Relation	root;
	List	   *parents;
	int			level = 0;

	InitMaterializedSRF(fcinfo, 0);

	root = relation_open(rootid, AccessShareLock);
	if (root->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		relation_close(root, AccessShareLock);
		return (Datum) 0;
	}
	relation_close(root, NoLock);

	/* the hierarchy locked, as pg_partition_tree() locks it */
	(void) find_all_inheritors(rootid, AccessShareLock, NULL);

	parents = list_make1_oid(rootid);
	while (parents != NIL)
	{
		List	   *next = NIL;

		level++;
		foreach_oid(parentid, parents)
		{
			Relation	parent = relation_open(parentid, NoLock);
			PartitionDesc partdesc = RelationGetPartitionDesc(parent, false);
			PartitionBoundInfo bounds = partdesc->boundinfo;

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Datum		values[7];
				bool		nulls[7] = {0};
				bool		isdefault = bounds->default_index == i;

				values[0] = ObjectIdGetDatum(partdesc->oids[i]);
				values[1] = ObjectIdGetDatum(parentid);
				values[2] = BoolGetDatum(partdesc->is_leaf[i]);
				values[3] = Int32GetDatum(level);
				values[4] = CStringGetTextDatum(strategy_name(bounds->strategy));
				if (bounds->strategy == PARTITION_STRATEGY_RANGE && !isdefault)
					values[5] = Int32GetDatum(i + 1);
				else
					nulls[5] = true;
				values[6] = BoolGetDatum(isdefault);
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
				if (!partdesc->is_leaf[i])
					next = lappend_oid(next, partdesc->oids[i]);
			}
			relation_close(parent, NoLock);
		}
		parents = next;
	}
	return (Datum) 0;
}
