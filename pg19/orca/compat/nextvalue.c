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
 * compat/nextvalue.c
 *	  An identity column's next value, as ORCA carries it.
 *
 * An INSERT that leaves an identity column out or says DEFAULT for it, and
 * an UPDATE that sets one to DEFAULT, ask for the column's next value with a
 * NextValueExpr (rewriteHandler.c, build_column_default).  ORCA has no scalar
 * for it; Cloudberry's translator falls into its "unsupported node" default,
 * and until now the port's refused it by name.
 *
 * It is not nextval().  The executor takes the value without the USAGE
 * privilege on the sequence that nextval() checks (execExprInterp.c,
 * ExecEvalNextValueExpr, against sequence.c, nextval_internal): a column's
 * default is the table's affair, and whoever may write the table may take
 * its identity column's next value.  So ORCA cannot be handed nextval().
 *
 * It is handed a call of a function of gp_orca's own, one for each type an
 * identity column can have -- smallint, integer and bigint -- over the
 * sequence's OID.  The functions are volatile, as nextval() is, so ORCA
 * evaluates a call once per row and never folds it, and DXL to PlannedStmt
 * turns each call back into the NextValueExpr it stands for: no plan runs
 * one.  Their body raises all the same, and PUBLIC may not execute them.
 *
 * A QUERY MAY NOT CALL THEM ITSELF.  Its call would come back from ORCA as a
 * NextValueExpr too, and take a sequence's next value without the privilege
 * nextval() asks for.  So the scalar translator refuses such a call before
 * ORCA sees it, and the query goes to the planner, which leaves it the call
 * it is: EXECUTE is refused, or the body raises.
 *
 * The functions belong to gp_orca's extension and are found in its schema.
 * Where the extension is not installed there are none, and ORCA declines
 * the statement.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

#include "cb_nextvalue.h"

PG_FUNCTION_INFO_V1(gp_orca_identity_nextval);

/* gp_orca's function for a next value of type `typeid`, or InvalidOid. */
static Oid
next_value_func(Oid typeid)
{
	const char *name;
	Oid			argtype = OIDOID;
	Oid			nspoid;

	switch (typeid)
	{
		case INT2OID:
			name = "identity_nextval_int2";
			break;
		case INT4OID:
			name = "identity_nextval_int4";
			break;
		case INT8OID:
			name = "identity_nextval_int8";
			break;
		default:
			/* an identity column can have no other type */
			return InvalidOid;
	}

	/*
	 * Looked up in the catalog rather than resolved as a name: this is the
	 * port finding its own object, and what the user may see of the schema
	 * does not come into it.
	 */
	nspoid = get_namespace_oid("gp_orca", true);
	if (!OidIsValid(nspoid))
		return InvalidOid;

	return GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						   CStringGetDatum(name),
						   PointerGetDatum(buildoidvector(&argtype, 1)),
						   ObjectIdGetDatum(nspoid));
}

FuncExpr *
gp_orca_next_value_call(const NextValueExpr *next_value)
{
	Oid			funcid = next_value_func(next_value->typeId);
	Const	   *seq;

	if (!OidIsValid(funcid))
		return NULL;

	seq = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
					ObjectIdGetDatum(next_value->seqid), false, true);

	return makeFuncExpr(funcid, next_value->typeId, list_make1(seq),
						InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
}

bool
gp_orca_is_next_value_func(Oid funcid)
{
	/* an extension's functions have ordinary OIDs; this is most calls */
	if (funcid < FirstNormalObjectId)
		return false;

	return funcid == next_value_func(get_func_rettype(funcid));
}

NextValueExpr *
gp_orca_next_value_from_call(const FuncExpr *call)
{
	NextValueExpr *next_value;
	Const	   *seq;

	if (call->funcid < FirstNormalObjectId ||
		call->funcid != next_value_func(call->funcresulttype))
		return NULL;

	/* what gp_orca_next_value_call() made: the sequence, as a constant */
	seq = list_length(call->args) == 1 ? (Const *) linitial(call->args) : NULL;
	if (seq == NULL || !IsA(seq, Const) ||
		seq->consttype != OIDOID || seq->constisnull)
		elog(ERROR, "ORCA returned %s with arguments it was not given",
			 format_procedure(call->funcid));

	next_value = makeNode(NextValueExpr);
	next_value->seqid = DatumGetObjectId(seq->constvalue);
	next_value->typeId = call->funcresulttype;

	return next_value;
}

/*
 * The functions' body, which no plan runs.  DXL to PlannedStmt turns every
 * call ORCA plans back into a NextValueExpr, and a query that calls one of
 * them itself is left to the planner, whose executor lets nobody but a
 * superuser execute it.  So this is reached only by a superuser's call, and
 * says why that is no use.
 */
Datum
gp_orca_identity_nextval(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s cannot be called",
					format_procedure(fcinfo->flinfo->fn_oid)),
			 errdetail("It stands for an identity column's next value in the plans ORCA makes, and no plan runs it.")));

	PG_RETURN_NULL();			/* keep the compiler quiet */
}
