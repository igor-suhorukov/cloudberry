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
 * funcattr.c
 *	  Where a function may run, and what it does with SQL.
 *
 * Cloudberry keeps these in pg_proc.proexeclocation and prodataaccess, which
 * an extension cannot add; here they are the execute_on and data_access keys
 * of the function's "gp" label, which is what func_exec_location() reads and
 * ORCA asks of every function it meets.  O26 writes Cloudberry's clauses as
 * options of the statement they are on -- EXECUTE ON ALL SEGMENTS is SET
 * gp.execute_on = 'all_segments', READS SQL DATA is SET gp.data_access =
 * 'reads' -- and this takes them out before PostgreSQL would store them as
 * settings of the function, and writes the label once the function exists.
 * They used to be a SECURITY LABEL after the statement, which made CREATE
 * FUNCTION two statements, and which, replacing the whole label, took one
 * key away whenever an ALTER set the other.
 *
 * What a statement does to them is Cloudberry's:
 *
 *   - CREATE [OR REPLACE] FUNCTION says what both are.  One it does not name
 *     is the default -- EXECUTE ON ANY, and CONTAINS SQL for a SQL function
 *     or NO SQL for any other -- which the label spells by leaving the key
 *     out, so a CREATE OR REPLACE takes away what it does not say, as
 *     ProcedureCreate() rewrites both columns in Cloudberry.
 *   - ALTER FUNCTION changes what it names and leaves the other alone.
 *   - Whatever the statement changed, the function as it leaves it is held to
 *     Cloudberry's rules: IMMUTABLE conflicts with READS and MODIFIES SQL
 *     DATA and a SQL function cannot say NO SQL (validate_sql_data_access),
 *     and EXECUTE ON anything but ANY is for a set-returning function
 *     (validate_sql_exec_location).  The check is made once the statement has
 *     run, and before it is over, so a function that breaks a rule is not left
 *     behind.
 *
 * Nothing reads data_access, in the port or in Cloudberry, where
 * prodataaccess is written, dumped and read nowhere outside the DDL path: its
 * whole observable behaviour is the rules above.
 *
 * Cloudberry source this file is made of:
 *	  the EXECUTE ON and data-access code of src/backend/commands/functioncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "nodes/parsenodes.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#include "gp_label.h"
#include "gp_sql.h"

/* The two attributes, by the setting O26 spells each as. */
typedef enum FuncAttrKind
{
	FUNCATTR_NONE = -1,
	FUNCATTR_EXECUTE_ON = 0,
	FUNCATTR_DATA_ACCESS,
	FUNCATTR_NKINDS
} FuncAttrKind;

static const struct
{
	const char *setting;		/* SET <setting> = '...' */
	GpLabelKey	key;			/* the label key it is kept in */
	const char *const *values;	/* what it may be */
}			funcattr_kind[FUNCATTR_NKINDS] = {
	[FUNCATTR_EXECUTE_ON] = {
		"gp.execute_on", GP_LABEL_execute_on,
		(const char *const[]) {"any", "coordinator", "initplan", "all_segments", NULL}
	},
	[FUNCATTR_DATA_ACCESS] = {
		"gp.data_access", GP_LABEL_data_access,
		(const char *const[]) {"none", "contains", "reads", "modifies", NULL}
	},
};

/* What a statement says of one attribute. */
typedef struct FuncAttr
{
	bool		given;			/* the statement names it */
	char	   *value;			/* what it says; NULL is the default */
} FuncAttr;

/* Which of the two a function option sets, if either. */
static FuncAttrKind
funcattr_of(DefElem *def)
{
	VariableSetStmt *set;

	if (strcmp(def->defname, "set") != 0 || def->arg == NULL ||
		!IsA(def->arg, VariableSetStmt))
		return FUNCATTR_NONE;

	set = (VariableSetStmt *) def->arg;
	if (set->name == NULL)
		return FUNCATTR_NONE;

	for (int k = 0; k < FUNCATTR_NKINDS; k++)
	{
		if (strcmp(set->name, funcattr_kind[k].setting) == 0)
			return (FuncAttrKind) k;
	}
	return FUNCATTR_NONE;
}

/*
 * What a SET or RESET of one says: a value, which must be one the attribute
 * has, or, for RESET and SET ... TO DEFAULT, the default.  MASTER is
 * Cloudberry's older word for COORDINATOR, and records as it does.
 */
static char *
funcattr_value(FuncAttrKind kind, VariableSetStmt *set)
{
	A_Const    *con;
	char	   *value;

	switch (set->kind)
	{
		case VAR_SET_DEFAULT:
		case VAR_RESET:
			return NULL;
		case VAR_SET_VALUE:
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s can only be set to a value", funcattr_kind[kind].setting)));
	}

	if (list_length(set->args) != 1 || !IsA(linitial(set->args), A_Const) ||
		!IsA(&((A_Const *) linitial(set->args))->val, String))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s takes one value, as a string", funcattr_kind[kind].setting)));

	con = (A_Const *) linitial(set->args);
	value = strVal(&con->val);

	if (kind == FUNCATTR_EXECUTE_ON && strcmp(value, "master") == 0)
		value = "coordinator";

	for (int i = 0; funcattr_kind[kind].values[i] != NULL; i++)
	{
		if (strcmp(value, funcattr_kind[kind].values[i]) == 0)
			return pstrdup(value);
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid value for %s: \"%s\"", funcattr_kind[kind].setting, value),
			 errhint("It is one of: %s.",
					 kind == FUNCATTR_EXECUTE_ON ? "any, coordinator, initplan, all_segments"
					 : "none, contains, reads, modifies")));
	return NULL;				/* keep compiler quiet */
}

/*
 * Take this module's settings out of a function's option list, before
 * PostgreSQL would store them as settings of the function -- which would
 * send every call of it through the security-definer path and keep it from
 * being inlined -- and say what they were.  Returns whether there were any.
 */
static bool
funcattr_take(List **options, FuncAttr *attrs)
{
	bool		found = false;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		FuncAttrKind kind = funcattr_of(def);

		if (kind == FUNCATTR_NONE)
			continue;

		if (attrs[kind].given)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant options"),
					 errdetail("%s is given more than once.", funcattr_kind[kind].setting)));

		attrs[kind].given = true;
		attrs[kind].value = funcattr_value(kind, (VariableSetStmt *) def->arg);
		*options = foreach_delete_current(*options, lc);
		found = true;
	}

	return found;
}

/*
 * Cloudberry's rules, checked against the function as the statement leaves
 * it, with the messages Cloudberry gives: validate_sql_data_access() and
 * validate_sql_exec_location() in its functioncmds.c.  A default breaks none
 * of them.
 */
static void
funcattr_validate(Oid funcOid, const char *execute_on, const char *data_access)
{
	HeapTuple	tup;
	Form_pg_proc proc;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	proc = (Form_pg_proc) GETSTRUCT(tup);

	if (data_access != NULL)
	{
		if (proc->provolatile == PROVOLATILE_IMMUTABLE &&
			strcmp(data_access, "reads") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting options"),
					 errhint("IMMUTABLE conflicts with READS SQL DATA.")));
		if (proc->provolatile == PROVOLATILE_IMMUTABLE &&
			strcmp(data_access, "modifies") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting options"),
					 errhint("IMMUTABLE conflicts with MODIFIES SQL DATA.")));
		if (proc->prolang == SQLlanguageId && strcmp(data_access, "none") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting options"),
					 errhint("A SQL function cannot specify NO SQL.")));
	}

	if (execute_on != NULL && strcmp(execute_on, "any") != 0 && !proc->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXECUTE ON %s is only supported for set-returning functions",
						strcmp(execute_on, "coordinator") == 0 ? "COORDINATOR" :
						strcmp(execute_on, "initplan") == 0 ? "INITPLAN" :
						"ALL SEGMENTS")));

	ReleaseSysCache(tup);
}

/*
 * Write both keys, when either changed, in one order whatever order they were
 * written in, so that two spellings of the same function give the same
 * label.  GpLabelSet puts a key it sets at the end, so both go and come back.
 */
static bool
funcattr_same(const char *a, const char *b)
{
	return (a == NULL) ? (b == NULL) : (b != NULL && strcmp(a, b) == 0);
}

static void
funcattr_write(Oid funcOid, const char *old_eo, const char *old_da,
			   const char *eo, const char *da)
{
	ObjectAddress addr;

	if (funcattr_same(old_eo, eo) && funcattr_same(old_da, da))
		return;

	ObjectAddressSet(addr, ProcedureRelationId, funcOid);
	if (old_eo != NULL)
		GpLabelSet(&addr, GP_LABEL_execute_on, NULL);
	if (old_da != NULL)
		GpLabelSet(&addr, GP_LABEL_data_access, NULL);
	if (eo != NULL)
		GpLabelSet(&addr, GP_LABEL_execute_on, eo);
	if (da != NULL)
		GpLabelSet(&addr, GP_LABEL_data_access, da);
}

void
GpFuncAttrProcessUtility(PlannedStmt *pstmt, const char *queryString,
						 bool readOnlyTree, ProcessUtilityContext context,
						 ParamListInfo params, QueryEnvironment *queryEnv,
						 DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		creating = IsA(parsetree, CreateFunctionStmt);
	List	  **options;
	FuncAttr	attrs[FUNCATTR_NKINDS] = {{0}};
	bool		carries = false;
	GpSqlPending save;
	volatile Oid funcOid = InvalidOid;
	ObjectAddress addr;
	char	   *old_eo;
	char	   *old_da;
	char	   *eo;
	char	   *da;

	options = creating ? &((CreateFunctionStmt *) parsetree)->options
		: &((AlterFunctionStmt *) parsetree)->actions;

	{
		ListCell   *lc;

		foreach(lc, *options)
			carries |= (funcattr_of((DefElem *) lfirst(lc)) != FUNCATTR_NONE);
	}

	/*
	 * A new function that says nothing of either has nothing to be told.  A
	 * replaced one has, when it had them, and every ALTER is held to the
	 * rules again, as Cloudberry holds it.
	 */
	if (creating && !carries && !((CreateFunctionStmt *) parsetree)->replace)
	{
		GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		return;
	}

	if (carries)
	{
		if (readOnlyTree)
		{
			pstmt = copyObject(pstmt);
			parsetree = pstmt->utilityStmt;
			readOnlyTree = false;
			options = creating ? &((CreateFunctionStmt *) parsetree)->options
				: &((AlterFunctionStmt *) parsetree)->actions;
		}
		(void) funcattr_take(options, attrs);
	}

	/*
	 * CREATE FUNCTION says which function it made to nobody, so the hook on
	 * object creation is asked; ProcedureCreate calls it for a replaced
	 * function too.  ALTER FUNCTION names its function.
	 */
	if (creating)
	{
		GpSqlPendingArm(&save);
		PG_TRY();
		{
			GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
			funcOid = GpSqlPendingFirst(ProcedureRelationId);
		}
		PG_FINALLY();
		{
			GpSqlPendingRestore(&save);
		}
		PG_END_TRY();
	}
	else
	{
		AlterFunctionStmt *stmt = (AlterFunctionStmt *) parsetree;

		GpSqlProcessUtilityNext(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		funcOid = LookupFuncWithArgs(stmt->objtype, stmt->func, false);
	}

	if (!OidIsValid(funcOid))
		elog(ERROR, "gp_sql: the function the statement made was not found");

	CommandCounterIncrement();
	ObjectAddressSet(addr, ProcedureRelationId, funcOid);
	old_eo = GpLabelGet(&addr, GP_LABEL_execute_on);
	old_da = GpLabelGet(&addr, GP_LABEL_data_access);

	if (creating)
	{
		/* the statement says what both are; unnamed is the default */
		eo = attrs[FUNCATTR_EXECUTE_ON].value;
		da = attrs[FUNCATTR_DATA_ACCESS].value;
	}
	else
	{
		eo = attrs[FUNCATTR_EXECUTE_ON].given ? attrs[FUNCATTR_EXECUTE_ON].value : old_eo;
		da = attrs[FUNCATTR_DATA_ACCESS].given ? attrs[FUNCATTR_DATA_ACCESS].value : old_da;
	}

	funcattr_validate(funcOid, eo, da);
	funcattr_write(funcOid, old_eo, old_da, eo, da);
}
