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
 *	 gp_configuration_history	a view over FTS's history (gp_fts.c) and a
 *								table of this database's, which a write to
 *								it writes
 *	 gp_distribution_policy		a view over the "gp" labels, which a write
 *								to it writes (gp_policy.c reads them)
 *
 * A catalog is written only with allow_system_table_mods on.  The views here
 * are ordinary ones, so a trigger refuses the rest in Cloudberry's words.
 *
 * And a statistics row written by hand, as ORCA's tests and Cloudberry's
 * gpsd and minirepro write pg_statistic: an array constant given for a
 * column of type anyarray is taken as anyarray, as Cloudberry's parser takes
 * it (coerce_type(), MPP-3786), where PostgreSQL's leaves it the array's own
 * type, which the executor refuses ("table row type and query-specified row
 * type do not match").  And statistics whose values are not of their
 * column's type refused as the planner reads them, as Cloudberry's refuses
 * them, where PostgreSQL's would crash comparing them.
 *
 * Cloudberry sources this file stands in for:
 *	  src/include/catalog/gp_id.h, gp_segment_configuration.h,
 *	  gp_configuration_history.h, gp_distribution_policy.h, the checks in
 *	  copy.c and parse_clause.c that refuse a write to a system catalog, and
 *	  parse_coerce.c's coerce_type() for a constant of type anyarray, and
 *	  selfuncs.c's check of the statistics' values
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp_catalog.h"
#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_label.h"
#include "gp_policy.h"

PG_FUNCTION_INFO_V1(gp_catalog_write_check);
PG_FUNCTION_INFO_V1(gp_stat_force_next_flush);

/*
 * gp_stat_force_next_flush(): pg_stat_force_next_flush() here and on every
 * segment, as Cloudberry's system_views.sql makes it, so that a test reads
 * the counters of what it just did from every node.
 */
Datum
gp_stat_force_next_flush(PG_FUNCTION_ARGS)
{
	(void) DirectFunctionCall1(pg_stat_force_next_flush, (Datum) 0);
	if (!GpClusterIsSingleNode() && GpClusterBackendRole() == GP_ROLE_DISPATCH)
		GpDispatchCommand("SELECT pg_catalog.pg_stat_force_next_flush()");
	PG_RETURN_VOID();
}

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
/* gp_configuration_history                                                  */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_catalog_configuration_history_write);

/*
 * gp_internal.configuration_history_write(), INSTEAD OF each row written to
 * gp_configuration_history -- by a user with allow_system_table_mods on,
 * which gp_catalog_write_check() checks first.  A row inserted goes to this
 * database's table of them; one updated or deleted is found there, the
 * first of that time, dbid and description, and one that is FTS's is not
 * there, and is left alone, as its file is FTS's to write.
 */
Datum
gp_catalog_configuration_history_write(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc	tupdesc;
	Oid			argtypes[6] = {TIMESTAMPTZOID, INT2OID, TEXTOID,
		TIMESTAMPTZOID, INT2OID, TEXTOID};
	Datum		values[6];
	char		nulls[6];
	HeapTuple	result;
	const char *match = "ctid = (SELECT ctid FROM gp_internal.configuration_history"
		" WHERE \"time\" = $1 AND dbid = $2 AND \"desc\" IS NOT DISTINCT FROM $3 LIMIT 1)";

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "gp_catalog_configuration_history_write: not called by the trigger manager");
	tupdesc = RelationGetDescr(trigdata->tg_relation);

	for (int i = 0; i < 3; i++)
	{
		bool		isnull;

		values[i] = heap_getattr(trigdata->tg_trigtuple, i + 1, tupdesc, &isnull);
		nulls[i] = isnull ? 'n' : ' ';
		if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		{
			values[i + 3] = heap_getattr(trigdata->tg_newtuple, i + 1, tupdesc,
										 &isnull);
			nulls[i + 3] = isnull ? 'n' : ' ';
		}
	}

	SPI_connect();
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
	{
		SPI_execute_with_args("INSERT INTO gp_internal.configuration_history VALUES ($1, $2, $3)",
							  3, argtypes, values, nulls, false, 0);
		result = trigdata->tg_trigtuple;
	}
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		SPI_execute_with_args(psprintf("UPDATE gp_internal.configuration_history"
									   " SET \"time\" = $4, dbid = $5, \"desc\" = $6 WHERE %s",
									   match),
							  6, argtypes, values, nulls, false, 0);
		result = SPI_processed > 0 ? trigdata->tg_newtuple : NULL;
	}
	else
	{
		SPI_execute_with_args(psprintf("DELETE FROM gp_internal.configuration_history WHERE %s",
									   match),
							  3, argtypes, values, nulls, false, 0);
		result = SPI_processed > 0 ? trigdata->tg_trigtuple : NULL;
	}
	SPI_finish();

	return PointerGetDatum(result);
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

/* ------------------------------------------------------------------------- */
/* A statistics row written by hand                                          */
/* ------------------------------------------------------------------------- */

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static get_relation_stats_hook_type prev_get_relation_stats_hook = NULL;

/*
 * An array constant, as anyarray: Cloudberry's coerce_type() gives a
 * constant coerced to anyarray that type, and its value is the array's, as
 * a statistics row keeps it.  Anything else is left as it is.
 */
static bool
relabel_anyarray_const(Node *node)
{
	Const	   *con = (Const *) node;

	if (node == NULL || !IsA(node, Const))
		return false;
	if (con->consttype != ANYARRAYOID)
	{
		if (con->consttype == UNKNOWNOID ||
			!OidIsValid(get_element_type(con->consttype)))
			return false;
		con->consttype = ANYARRAYOID;
		con->consttypmod = -1;
		con->constcollid = InvalidOid;
	}
	return true;
}

/*
 * A column of a VALUES list of several rows, which the target list reads: its
 * constants, and the column itself, as anyarray, where every row's value is
 * a constant.
 */
static void
relabel_anyarray_values(Query *query, Var *var)
{
	RangeTblEntry *rte = rt_fetch(var->varno, query->rtable);
	int			col = var->varattno - 1;

	if (rte->rtekind != RTE_VALUES || col < 0 ||
		col >= list_length(rte->coltypes))
		return;
	foreach_node(List, row, rte->values_lists)
	{
		Node	   *item = list_nth(row, col);

		if (item == NULL || !IsA(item, Const) ||
			(((Const *) item)->consttype != ANYARRAYOID &&
			 !OidIsValid(get_element_type(((Const *) item)->consttype))))
			return;
	}
	foreach_node(List, row, rte->values_lists)
		(void) relabel_anyarray_const(list_nth(row, col));
	list_nth_cell(rte->coltypes, col)->oid_value = ANYARRAYOID;
	list_nth_cell(rte->coltypmods, col)->int_value = -1;
	list_nth_cell(rte->colcollations, col)->oid_value = InvalidOid;
	var->vartype = ANYARRAYOID;
	var->vartypmod = -1;
	var->varcollid = InvalidOid;
}

/*
 * An INSERT or UPDATE of a system catalog -- the only tables a column of
 * type anyarray is in, pg_statistic's stavalues among them -- whose value for
 * such a column is an array constant.
 */
static void
catalog_post_parse_analyze(ParseState *pstate, Query *query,
						   const JumbleState *jstate)
{
	RangeTblEntry *target;

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	if ((query->commandType != CMD_INSERT && query->commandType != CMD_UPDATE) ||
		query->resultRelation <= 0)
		return;
	target = rt_fetch(query->resultRelation, query->rtable);
	if (target->rtekind != RTE_RELATION || !IsCatalogRelationOid(target->relid))
		return;

	foreach_node(TargetEntry, tle, query->targetList)
	{
		if (tle->resjunk || get_atttype(target->relid, tle->resno) != ANYARRAYOID)
			continue;
		if (IsA(tle->expr, Var) && ((Var *) tle->expr)->varlevelsup == 0)
			relabel_anyarray_values(query, (Var *) tle->expr);
		else
			(void) relabel_anyarray_const((Node *) tle->expr);
	}
}

/*
 * One kind of statistics of a column, where the row has it: an MCV list or a
 * histogram whose values are not of the column's type.  The planner would
 * read them as the column's own values and crash comparing them; Cloudberry
 * raises this, in these words (get_variable_range(), selfuncs.c).
 */
static void
check_stats_values(HeapTuple tuple, int16 kind, Oid atttype, const char *what)
{
	Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(tuple);

	for (int i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		Datum		d;
		bool		isnull;
		ArrayType  *values;

		if ((&stats->stakind1)[i] != kind)
			continue;
		d = SysCacheGetAttr(STATRELATTINH, tuple,
							Anum_pg_statistic_stavalues1 + i, &isnull);
		if (isnull)
			continue;
		values = (ArrayType *) PG_DETOAST_DATUM_SLICE(d, 0, sizeof(ArrayType));
		if (!IsBinaryCoercible(ARR_ELEMTYPE(values), atttype))
			elog(ERROR, "invalid %s of type %s, for attribute of type %s", what,
				 format_type_be(ARR_ELEMTYPE(values)), format_type_be(atttype));
	}
}

static void
check_relation_stats(Oid relid, AttrNumber attnum, bool inh, Oid atttype)
{
	HeapTuple	tuple = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid),
										Int16GetDatum(attnum),
										BoolGetDatum(inh));

	if (!HeapTupleIsValid(tuple))
		return;
	check_stats_values(tuple, STATISTIC_KIND_HISTOGRAM, atttype, "histogram");
	check_stats_values(tuple, STATISTIC_KIND_MCV, atttype, "MCV array");
	ReleaseSysCache(tuple);
}

/*
 * The statistics of a column, as the planner is about to read them: checked,
 * and left to it to read as it would.  A row written by hand may hold values
 * of another type -- one the relabelling above lets through, as Cloudberry's
 * parser does, and bfv_statistic writes to see refused.
 */
static bool
catalog_relation_stats(PlannerInfo *root, RangeTblEntry *rte,
					   AttrNumber attnum, VariableStatData *vardata)
{
	Oid			atttype;

	if (prev_get_relation_stats_hook &&
		prev_get_relation_stats_hook(root, rte, attnum, vardata))
		return true;
	if (rte->rtekind != RTE_RELATION || attnum <= 0)
		return false;

	atttype = OidIsValid(vardata->atttype) ? vardata->atttype
		: get_atttype(rte->relid, attnum);
	check_relation_stats(rte->relid, attnum, false, atttype);
	if (rte->inh)
		check_relation_stats(rte->relid, attnum, true, atttype);
	return false;
}

void
GpCatalogInit(void)
{
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = catalog_post_parse_analyze;
	prev_get_relation_stats_hook = get_relation_stats_hook;
	get_relation_stats_hook = catalog_relation_stats;
}
