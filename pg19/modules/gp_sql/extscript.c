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
 * extscript.c
 *	  What the port makes of PostGIS's scripts as they run.
 *
 * Cloudberry runs PostGIS from a fork of it, "PostGIS 3.3.2 for Apache
 * Cloudberry", whose scripts differ from PostGIS's own.  The port runs stock
 * PostGIS, and makes the fork's changes that the plan decided on while
 * CREATE EXTENSION or ALTER EXTENSION UPDATE runs the script ("PostGIS on
 * the hook-based Cloudberry", decisions 14a to 14c), on every node that runs
 * it:
 *
 *   - ST_Union is one stage (14a).  Its two CREATE AGGREGATE statements lose
 *     combinefunc, serialfunc, deserialfunc and parallel, as the fork comments
 *     them out, so that no plan splits it -- neither a Motion between a
 *     partial aggregate on the segments and a final one on the coordinator,
 *     nor PostgreSQL's parallel aggregation.  PostGIS's other aggregates keep
 *     their stock definitions.
 *   - postgis_topology's functions run on the coordinator (14c).  Topology
 *     edits through SPI, which a segment refuses a function (Cloudberry's
 *     rule for a function run on a segment, gp_core's too), so each of the
 *     extension's functions is labelled execute_on=coordinator, once the
 *     script has run on a cluster's coordinator, and ORCA declines to plan a
 *     query that calls one (CTranslatorRelcacheToDXL's "unsupported exec
 *     location") -- the planner, which calls a function that is not
 *     immutable on the coordinator, plans it instead.  The labels reach the
 *     segments as every label does.  On one node nothing is labelled: there
 *     is no segment for a function to run on.
 *
 * The third, a script's tables replicated (14b), is distribution.c's.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "nodes/parsenodes.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

#include "gp_core_api.h"
#include "gp_label.h"
#include "gp_sql.h"

/* Is the script being run that of this extension? */
static bool
creating(const char *extname)
{
	char	   *name;

	if (!creating_extension)
		return false;
	name = get_extension_name(CurrentExtensionObject);
	return name != NULL && strcmp(name, extname) == 0;
}

/*
 * A statement of an extension's script, before it runs: ST_Union's CREATE
 * AGGREGATE without its two stages (14a).  *pstmt is replaced by a copy when
 * the tree is read-only.
 */
void
GpExtScriptStatement(PlannedStmt **pstmt, bool *readOnlyTree)
{
	DefineStmt *stmt;
	List	   *kept = NIL;
	ListCell   *lc;

	if (!IsA((*pstmt)->utilityStmt, DefineStmt))
		return;
	stmt = (DefineStmt *) (*pstmt)->utilityStmt;
	if (stmt->kind != OBJECT_AGGREGATE ||
		pg_strcasecmp(strVal(llast(stmt->defnames)), "st_union") != 0 ||
		!creating("postgis"))
		return;

	if (*readOnlyTree)
	{
		*pstmt = copyObject(*pstmt);
		*readOnlyTree = false;
		stmt = (DefineStmt *) (*pstmt)->utilityStmt;
	}
	foreach(lc, stmt->definition)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (pg_strcasecmp(def->defname, "combinefunc") == 0 ||
			pg_strcasecmp(def->defname, "serialfunc") == 0 ||
			pg_strcasecmp(def->defname, "deserialfunc") == 0 ||
			pg_strcasecmp(def->defname, "parallel") == 0)
			continue;
		kept = lappend(kept, def);
	}
	stmt->definition = kept;
}

/*
 * A CREATE or ALTER EXTENSION that has run, on a cluster's coordinator:
 * postgis_topology's functions labelled for the coordinator (14c).  One a
 * user has given another place since keeps it: an update script's new
 * functions are labelled, and the rest stay as they are.
 */
void
GpExtScriptDone(Node *parsetree)
{
	const GpCoreApi *core = GpCoreApiLookup();
	const char *extname;
	Oid			extoid;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *funcs = NIL;
	ListCell   *lc;

	if (IsA(parsetree, CreateExtensionStmt))
		extname = ((CreateExtensionStmt *) parsetree)->extname;
	else if (IsA(parsetree, AlterExtensionStmt))
		extname = ((AlterExtensionStmt *) parsetree)->extname;
	else
		return;
	if (strcmp(extname, "postgis_topology") != 0 || core == NULL ||
		core->is_single_node() || core->get_role() != GP_ROLE_DISPATCH)
		return;
	extoid = get_extension_oid(extname, true);
	if (!OidIsValid(extoid))
		return;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(extoid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL,
							  2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

		if (dep->classid == ProcedureRelationId &&
			dep->deptype == DEPENDENCY_EXTENSION)
			funcs = lappend_oid(funcs, dep->objid);
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	foreach(lc, funcs)
	{
		ObjectAddress addr;

		ObjectAddressSet(addr, ProcedureRelationId, lfirst_oid(lc));
		if (GpLabelGet(&addr, GP_LABEL_execute_on) == NULL)
			GpLabelSet(&addr, GP_LABEL_execute_on, "coordinator");
	}
}
