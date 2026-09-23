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
 * gp_catalog.c
 *	  Cloudberry's catalogs by their own names, over what the port keeps in
 *	  their place.
 *
 * Cloudberry's tests and tools read its catalogs by name, and some write
 * them.  The port keeps what they hold elsewhere -- the cluster in a file,
 * a table's distribution in its "gp" label -- so gp_core's script makes each
 * one a view or a table in pg_catalog under Cloudberry's name, in
 * Cloudberry's columns, and this file is what those need:
 *
 *	 gp_id						a view of one fixed row
 *	 gp_segment_configuration	a view over the cluster file (gp_cluster.c)
 *	 gp_configuration_history	a table, until FTS writes it at M4
 *	 gp_distribution_policy		a view over the "gp" labels, which a write
 *								to it writes (gp_policy.c reads them)
 *
 * A catalog is written only with allow_system_table_mods on.  The table and
 * the view here are ordinary ones, so a trigger refuses the rest in
 * Cloudberry's words.
 *
 * Cloudberry sources this file stands in for:
 *	  src/include/catalog/gp_id.h, gp_segment_configuration.h,
 *	  gp_configuration_history.h, gp_distribution_policy.h, and the checks
 *	  in copy.c and parse_clause.c that refuse a write to a system catalog
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_seclabel.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_cluster.h"
#include "gp_label.h"
#include "gp_policy.h"

PG_FUNCTION_INFO_V1(gp_catalog_write_check);

/*
 * gp_internal.catalog_write_check(), a statement trigger: a write to one of
 * these as a table, refused unless allow_system_table_mods is on, as
 * Cloudberry refuses a write to a catalog.  In the words of its COPY, whose
 * hint its INSERT, UPDATE and DELETE leave out; a statement trigger fires
 * for both, and for a statement that finds no row, as Cloudberry's check at
 * parse time does.
 */
Datum
gp_catalog_write_check(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "gp_catalog_write_check: not called by the trigger manager");

	if (!allowSystemTableMods)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(trigdata->tg_relation)),
				 errhint("Make sure the configuration parameter allow_system_table_mods is set.")));

	return PointerGetDatum(NULL);
}

/* ------------------------------------------------------------------------- */
/* gp_distribution_policy                                                    */
/* ------------------------------------------------------------------------- */

#define POLICY_NATTS	5

PG_FUNCTION_INFO_V1(gp_catalog_distribution_policy);

/*
 * gp_internal.distribution_policy()
 *		The rows of Cloudberry's gp_distribution_policy, which the view of
 *		that name selects: one for each relation of this database whose "gp"
 *		label records a distribution.
 *
 * In Cloudberry's columns: the relation; 'p' for hashed and random, 'r'
 * replicated; how many segments; the key's attribute numbers; and the
 * operator class each is hashed with, which is its type's default hash
 * class.  A relation with no policy has no row, and on one node none has:
 * every relation is where it is, as Cloudberry's single node keeps no
 * policy at all.  A policy is shown as it is recorded, even where it could
 * not be read for a plan (GpPolicyGetRecorded).
 */
Datum
gp_catalog_distribution_policy(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	seclabel;
	SysScanDesc scan;
	HeapTuple	tuple;

	InitMaterializedSRF(fcinfo, 0);

	if (GpClusterIsSingleNode())
		return (Datum) 0;

	seclabel = table_open(SecLabelRelationId, AccessShareLock);
	scan = systable_beginscan(seclabel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		FormData_pg_seclabel *form = (FormData_pg_seclabel *) GETSTRUCT(tuple);
		Datum		provider;
		bool		isnull;
		GpPolicy   *policy;
		Datum		values[POLICY_NATTS];
		bool		nulls[POLICY_NATTS] = {0};
		int16	   *attrs;

		if (form->classoid != RelationRelationId || form->objsubid != 0)
			continue;
		provider = heap_getattr(tuple, Anum_pg_seclabel_provider,
								RelationGetDescr(seclabel), &isnull);
		if (isnull || strcmp(TextDatumGetCString(provider), GP_LABEL_PROVIDER) != 0)
			continue;

		policy = GpPolicyGetRecorded(form->objoid);
		if (policy == NULL || policy->ptype == POLICYTYPE_ENTRY)
			continue;

		attrs = palloc_array(int16, Max(policy->nattrs, 1));
		for (int i = 0; i < policy->nattrs; i++)
			attrs[i] = policy->attrs[i];

		values[0] = ObjectIdGetDatum(form->objoid);
		values[1] = CharGetDatum(GpPolicyIsReplicated(policy) ? 'r' : 'p');
		values[2] = Int32GetDatum(policy->numsegments);
		values[3] = PointerGetDatum(buildint2vector(attrs, policy->nattrs));
		values[4] = PointerGetDatum(buildoidvector(policy->opclasses,
												   policy->nattrs));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	systable_endscan(scan);
	table_close(seclabel, AccessShareLock);

	return (Datum) 0;
}

/*
 * What a row written to gp_distribution_policy says, as the "gp" label
 * records it: "replicated", "random" or the key's columns by name, each with
 * the hash operator class it gives where that is not the type's default.
 */
static char *
policy_text_of(Oid relid, char policytype, int2vector *distkey,
			   oidvector *distclass)
{
	List	   *keys = NIL;

	if (policytype == 'r')
	{
		if (distkey->dim1 > 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a replicated table has no distribution key")));
		return "replicated";
	}
	if (policytype != 'p')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized distribution policy type '%c'", policytype),
				 errhint("A policy is 'p', hashed or random, or 'r', replicated; delete the row for a table on the coordinator alone.")));
	if (distkey->dim1 == 0)
		return "random";
	if (distclass->dim1 != 0 && distclass->dim1 != distkey->dim1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("distclass has %d operator classes, and distkey %d columns",
						distclass->dim1, distkey->dim1)));

	for (int i = 0; i < distkey->dim1; i++)
	{
		AttrNumber	attnum = distkey->values[i];
		HeapTuple	atttup = attnum > 0 ? SearchSysCacheAttNum(relid, attnum) : NULL;
		GpPolicyKeyName *key = palloc0(sizeof(GpPolicyKeyName));
		Oid			typeoid;

		/* not there, or dropped */
		if (!HeapTupleIsValid(atttup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column %d of relation \"%s\" does not exist",
							attnum, get_rel_name(relid))));
		key->column = pstrdup(NameStr(((Form_pg_attribute) GETSTRUCT(atttup))->attname));
		typeoid = ((Form_pg_attribute) GETSTRUCT(atttup))->atttypid;
		ReleaseSysCache(atttup);

		if (distclass->dim1 != 0 &&
			distclass->values[i] != GpPolicyDefaultOpclass(typeoid))
		{
			Oid			opclass = distclass->values[i];

			if (get_opclass_method(opclass) != HASH_AM_OID ||
				!IsBinaryCoercible(typeoid, get_opclass_input_type(opclass)))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("operator class %u does not hash column \"%s\" of type %s",
								opclass, key->column, format_type_be(typeoid))));
			key->opclass = GpPolicyOpclassName(opclass);
		}
		keys = lappend(keys, key);
	}
	return GpPolicyFormatKey(keys);
}

/* Record a relation's policy, or with a NULL one, none: the coordinator's alone. */
static void
policy_write(Oid relid, const char *policy, int numsegments)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, RelationRelationId, relid);
	GpLabelSet(&addr, GP_LABEL_distributed_by, policy);
	if (policy == NULL || numsegments == GpClusterSegmentCount())
	{
		if (GpLabelGet(&addr, GP_LABEL_numsegments) != NULL)
			GpLabelSet(&addr, GP_LABEL_numsegments, NULL);
	}
	else
		GpLabelSet(&addr, GP_LABEL_numsegments, psprintf("%d", numsegments));

	/* a plan made for the old distribution is made again */
	CacheInvalidateRelcacheByRelid(relid);
}

PG_FUNCTION_INFO_V1(gp_catalog_distribution_policy_write);

/*
 * gp_internal.distribution_policy_write(), gp_distribution_policy's INSTEAD
 * OF trigger: a row inserted, changed or deleted is the relation's policy
 * recorded, changed or dropped, as a write to Cloudberry's catalog is --
 * which moves no row, as that does not.  A table whose row is deleted is the
 * coordinator's alone.  Cloudberry's tests write it so to make a table of
 * fewer segments, a random one or one on the coordinator.  Only with
 * allow_system_table_mods on, which gp_catalog_write_check() checks first.
 */
Datum
gp_catalog_distribution_policy_write(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc	desc;
	HeapTuple	row;
	Datum		values[POLICY_NATTS];
	bool		nulls[POLICY_NATTS];
	Oid			relid;
	char		relkind;
	int			numsegments;

	if (!CALLED_AS_TRIGGER(fcinfo) ||
		!TRIGGER_FIRED_INSTEAD(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "gp_catalog_distribution_policy_write: not called as an INSTEAD OF row trigger");

	desc = RelationGetDescr(trigdata->tg_relation);
	if (desc->natts != POLICY_NATTS)
		elog(ERROR, "gp_distribution_policy has %d columns, not %d",
			 desc->natts, POLICY_NATTS);

	if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
	{
		bool		isnull;

		relid = DatumGetObjectId(heap_getattr(trigdata->tg_trigtuple, 1, desc,
											  &isnull));
		policy_write(relid, NULL, 0);
		return PointerGetDatum(trigdata->tg_trigtuple);
	}

	row = TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)
		? trigdata->tg_newtuple : trigdata->tg_trigtuple;
	heap_deform_tuple(row, desc, values, nulls);
	for (int i = 0; i < POLICY_NATTS; i++)
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("null value in column \"%s\" of relation \"gp_distribution_policy\"",
							NameStr(TupleDescAttr(desc, i)->attname))));
	relid = DatumGetObjectId(values[0]);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		bool		isnull;

		if (DatumGetObjectId(heap_getattr(trigdata->tg_trigtuple, 1, desc,
										  &isnull)) != relid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a policy cannot be moved to another relation"),
					 errhint("Delete its row, and insert the other relation's.")));
	}

	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_MATVIEW && relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation %u is not a table", relid)));

	numsegments = DatumGetInt32(values[2]);
	if (numsegments < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("numsegments must be at least 1, not %d", numsegments)));

	policy_write(relid,
				 policy_text_of(relid, DatumGetChar(values[1]),
								(int2vector *) PG_DETOAST_DATUM(values[3]),
								(oidvector *) PG_DETOAST_DATUM(values[4])),
				 numsegments);

	return PointerGetDatum(row);
}
