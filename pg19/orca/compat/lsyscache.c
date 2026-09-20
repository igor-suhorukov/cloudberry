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
 * compat/lsyscache.c
 *	  The catalog lookups Cloudberry adds to PostgreSQL's lsyscache.c.
 *
 * Ported from github/cloudberry/src/backend/utils/cache/lsyscache.c, which
 * is a PostgreSQL file Cloudberry modified and the port therefore does not
 * build.  Each function keeps Cloudberry's name and signature, so the
 * translator above calls them as ORCA has always called them, and the
 * differences are underneath.
 *
 * The comments here record what had to change for PostgreSQL 19 and what did
 * not, because that is the part a reader cannot recover by diffing: a
 * function that looks identical to Cloudberry's was checked against
 * PostgreSQL 19 and found to need nothing.
 *
 * See compat/cb_lsyscache.h for why the header is not named utils/lsyscache.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/syscache.h"

#include "cb_lsyscache.h"

/*
 * pfree_ptr_array
 *		Free an array of pointers, and the array.
 *
 * get_func_arg_info hands back three palloc'd arrays and the caller owns
 * them.  Cloudberry keeps this beside the functions that call it rather than
 * writing the loop twice.
 */
void
pfree_ptr_array(char **ptrarray, int nelements)
{
	if (ptrarray == NULL)
		return;

	for (int i = 0; i < nelements; i++)
	{
		if (ptrarray[i] != NULL)
			pfree(ptrarray[i]);
	}
	pfree(ptrarray);
}

/*
 * get_type_name
 *		The name of the type with this OID, palloc'd, or NULL if there is no
 *		such type.
 *
 * PostgreSQL has format_type_be(), which ORCA does not want: that spells a
 * type the way SQL does, schema-qualifying it when it is not visible and
 * rendering an array as "integer[]".  ORCA wants the bare pg_type.typname,
 * because what it builds from it is a metadata object keyed by OID, and the
 * name is only ever shown to a human reading a minidump.
 *
 * Returning NULL rather than raising is the contract: ORCA's metadata cache
 * asks about OIDs it has seen before, and a type dropped since is a miss,
 * not an error.
 */
char *
get_type_name(Oid oid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(typtup->typname));
		ReleaseSysCache(tp);
		return result;
	}

	return NULL;
}

/*
 * function_exists
 *		Is there a function with this OID?
 */
bool
function_exists(Oid oid)
{
	return SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(oid));
}

/*
 * aggregate_exists
 *		Is there an aggregate with this OID?
 *
 * An aggregate has a row in pg_proc as well, so function_exists() is true of
 * every aggregate.  This asks the narrower question.
 */
bool
aggregate_exists(Oid oid)
{
	return SearchSysCacheExists1(AGGFNOID, ObjectIdGetDatum(oid));
}

/*
 * get_func_arg_types
 *		Every declared argument type of this function, in order.
 *
 * This is proargtypes, so it is the input arguments only, and it is what the
 * function was declared with rather than what a particular call resolved to.
 */
List *
get_func_arg_types(Oid funcid)
{
	HeapTuple	tp;
	Form_pg_proc procstruct;
	oidvector  *args;
	List	   *result = NIL;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procstruct = (Form_pg_proc) GETSTRUCT(tp);
	args = &procstruct->proargtypes;
	for (int i = 0; i < args->dim1; i++)
		result = lappend_oid(result, args->values[i]);

	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_output_arg_types
 *		The OUT, INOUT and TABLE argument types of this function, in order.
 *
 * NIL for a function that returns a scalar: those have no argument modes at
 * all, and get_func_arg_info leaves argmodes NULL to say so.  ORCA uses this
 * to decide what a function's result row looks like, which is why INOUT
 * counts here as well as in get_func_arg_types().
 */
List *
get_func_output_arg_types(Oid funcid)
{
	HeapTuple	tp;
	int			numargs;
	Oid		   *argtypes = NULL;
	char	  **argnames = NULL;
	char	   *argmodes = NULL;
	List	   *result = NIL;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	numargs = get_func_arg_info(tp, &argtypes, &argnames, &argmodes);

	if (argmodes == NULL)
	{
		/* No modes at all: every argument is IN, so there is no output row. */
		pfree_ptr_array(argnames, numargs);
		if (argtypes != NULL)
			pfree(argtypes);
		ReleaseSysCache(tp);
		return NIL;
	}

	for (int i = 0; i < numargs; i++)
	{
		if (argmodes[i] == PROARGMODE_INOUT ||
			argmodes[i] == PROARGMODE_OUT ||
			argmodes[i] == PROARGMODE_TABLE)
			result = lappend_oid(result, argtypes[i]);
	}

	pfree_ptr_array(argnames, numargs);
	pfree(argtypes);
	pfree(argmodes);

	ReleaseSysCache(tp);
	return result;
}

/*
 * get_agg_transtype
 *		The type of this aggregate's transition state.
 *
 * ORCA needs it to cost a split aggregate: the transition value is what
 * travels between the two halves, so its width is the width of the row a
 * partial aggregate emits.
 */
Oid
get_agg_transtype(Oid aggid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	result = ((Form_pg_aggregate) GETSTRUCT(tp))->aggtranstype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_aggregate
 *		The OID of the one-argument aggregate with this name and argument
 *		type, or InvalidOid.
 *
 * ORCA looks aggregates up by name where it has to synthesise a call the
 * query did not write -- count(*) under a split aggregate, for instance.
 *
 * The lookup is deliberately loose about schemas: PROCNAMEARGSNSP is keyed
 * on the name alone here, so this finds a match in any schema and takes the
 * first.  Cloudberry does the same, and it is only safe because the names
 * ORCA asks for are built-in ones.
 */
Oid
get_aggregate(const char *aggname, Oid oidType)
{
	CatCList   *catlist;
	Oid			result = InvalidOid;

	catlist = SearchSysCacheList1(PROCNAMEARGSNSP,
								  CStringGetDatum(aggname));

	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	htup = &catlist->members[i]->tuple;
		Form_pg_proc proctuple = (Form_pg_proc) GETSTRUCT(htup);
		Oid			oidProc = proctuple->oid;

		/* One argument, of the type asked for, and an aggregate. */
		if (proctuple->pronargs != 1 ||
			proctuple->proargtypes.values[0] != oidType)
			continue;

		if (SearchSysCacheExists1(AGGFNOID, ObjectIdGetDatum(oidProc)))
		{
			result = oidProc;
			break;
		}
	}

	ReleaseSysCacheList(catlist);
	return result;
}

/*
 * get_cast_func
 *		Is there an implicit cast from oidSrc to oidDest, and what performs it?
 *
 * Three answers in one call, because ORCA asks them together:
 *
 *	is_binary_coercible	the cast costs nothing -- the value is already in the
 *						target's representation, so ORCA may put a scan's
 *						column straight where the other type is wanted
 *	oidCastFunc			the function that performs it, or 0 when none is
 *						needed
 *	pathtype			how PostgreSQL would perform it, which ORCA carries
 *						into DXL so that the plan it builds says the same
 *
 * The return value is whether a cast exists at all.
 *
 * A relabel is binary-coercible too, and Cloudberry sets the flag for it
 * after the fact rather than before: find_coercion_pathway is what decides
 * that a domain over the type, say, needs no work at run time.
 *
 * A DEFECT NOT CARRIED OVER.  Cloudberry returns from the binary-coercible
 * branch without writing *pathtype, and its one caller declares the variable
 * uninitialized and then switches on it
 * (github/cloudberry/src/backend/gpopt/translate/CTranslatorRelcacheToDXL.cpp:2206,2243).
 * So on that path ORCA reads whatever was on the stack.  Three of the four
 * cases would be wrong -- ARRAYCOERCE asks for the source's element type,
 * which a non-array does not have -- and it has gone unnoticed because the
 * fourth, falling out of the switch, happens to build the same object the
 * right case would.  The right case is COERCION_PATH_RELABELTYPE: no
 * function, nothing to do at run time, which is what binary coercibility
 * means, and its branch asserts the InvalidOid this path already returns.
 * Writing it makes the caller take that branch on purpose.
 */
bool
get_cast_func(Oid oidSrc, Oid oidDest, bool *is_binary_coercible,
			  Oid *oidCastFunc, CoercionPathType *pathtype)
{
	if (IsBinaryCoercible(oidSrc, oidDest))
	{
		*is_binary_coercible = true;
		*oidCastFunc = InvalidOid;
		*pathtype = COERCION_PATH_RELABELTYPE;
		return true;
	}

	*is_binary_coercible = false;

	*pathtype = find_coercion_pathway(oidDest, oidSrc, COERCION_IMPLICIT,
									  oidCastFunc);

	if (*pathtype == COERCION_PATH_RELABELTYPE)
		*is_binary_coercible = true;

	return *pathtype != COERCION_PATH_NONE;
}
