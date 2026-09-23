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
 *
 * A catalog is written only with allow_system_table_mods on.  The table here
 * is an ordinary one, so a trigger refuses the rest in Cloudberry's words.
 *
 * Cloudberry sources this file stands in for:
 *	  src/include/catalog/gp_id.h, gp_segment_configuration.h,
 *	  gp_configuration_history.h, and the checks in copy.c and
 *	  parse_clause.c that refuse a write to a system catalog
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/trigger.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/rel.h"

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
