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

#include "access/cmptype.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "nodes/nodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "cb_lsyscache.h"
#include "gp_label.h"

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
 * func_exec_location
 *		Where this function may run: 'a'ny node, the 'c'oordinator only, 'i'n
 *		an init plan, or all 's'egments.
 *
 * Cloudberry reads pg_proc.proexeclocation, a column it adds.  An extension
 * cannot add a column, so the port reads the "gp" label's execute_on key --
 * the decision the plan records for EXECUTE ON, and the reason it is a label
 * rather than a SET in proconfig: PG19 routes a function with a proconfig
 * through the security-definer call path and never inlines it
 * (pg19/src/backend/utils/fmgr/fmgr.c:208,
 * pg19/src/backend/optimizer/util/clauses.c:5489,6038), which would cost
 * every EXECUTE ON function its inlining whether or not anything dispatched.
 *
 * WHAT ORCA DOES WITH THE ANSWER.  One comparison, against ANY: a function
 * that must run somewhere in particular is a shape ORCA declines to plan
 * (CTranslatorRelcacheToDXL.cpp:1491).  So an unlabelled function -- which is
 * every function until something labels one -- is the case that plans, and
 * ANY is both PostgreSQL's only possible answer and Cloudberry's own default
 * for the column.
 *
 * A value the port does not know is an error rather than a default.  The
 * alternative is to read an unrecognised word as ANY, which is the one answer
 * that lets the function run anywhere -- so a typo would quietly widen where
 * a function may run, which is the opposite of what the label was written to
 * do.
 */
char
func_exec_location(Oid funcid)
{
	static const struct
	{
		const char *name;
		char		location;
	}			names[] =
	{
		{"any", PROEXECLOCATION_ANY},
		{"coordinator", PROEXECLOCATION_COORDINATOR},
		/* Cloudberry's older spelling of the same place, still in its docs. */
		{"master", PROEXECLOCATION_COORDINATOR},
		{"all_segments", PROEXECLOCATION_ALL_SEGMENTS},
		{"initplan", PROEXECLOCATION_INITPLAN},
	};
	ObjectAddress addr;
	char	   *value;

	/*
	 * Cloudberry's syscache lookup raises on an OID that is not a function,
	 * and ORCA calls this only for a function it has already found; keep the
	 * check so that a caller which has not gets the same error it always did.
	 */
	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(funcid)))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	ObjectAddressSet(addr, ProcedureRelationId, funcid);
	value = GpLabelGet(&addr, GP_LABEL_execute_on);

	if (value == NULL)
		return PROEXECLOCATION_ANY;

	for (size_t i = 0; i < lengthof(names); i++)
	{
		if (pg_strcasecmp(value, names[i].name) == 0)
			return names[i].location;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized \"execute_on\" value \"%s\" on function %s",
					value, format_procedure(funcid)),
			 errhint("Valid values are \"any\", \"coordinator\", \"all_segments\" and \"initplan\".")));
	pg_unreachable();
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
 * is_agg_ordered
 *		Is this an ordered-set or hypothetical-set aggregate?
 *
 * Nothing here is Cloudberry's but the name: pg_aggregate.aggkind is
 * PostgreSQL's own column and AGGKIND_IS_ORDERED_SET its own macro.  The
 * function exists because ORCA reaches the catalogs only through this layer.
 *
 * ORCA asks so that it does not split one.  An ordered-set aggregate is
 * defined over the whole sorted input, so a partial computed per segment and
 * combined afterwards would be a different question answered.
 */
bool
is_agg_ordered(Oid aggid)
{
	HeapTuple	tp;
	char		aggkind;

	tp = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	/*
	 * Cloudberry reads the column through SysCacheGetAttr and asserts it is
	 * not null.  aggkind is a fixed-width NOT NULL column, so GETSTRUCT
	 * reaches it directly and the assertion has nothing left to say.
	 */
	aggkind = ((Form_pg_aggregate) GETSTRUCT(tp))->aggkind;
	ReleaseSysCache(tp);

	return AGGKIND_IS_ORDERED_SET(aggkind);
}

/*
 * is_agg_partial_capable
 *		May this aggregate be computed in two phases?
 *
 * It needs a combine function to merge two transition values, and, when the
 * transition value is internal -- a pointer into the aggregate's own memory,
 * which nothing can send anywhere -- a serial and a deserial function to turn
 * it into bytea and back.
 *
 * ORCA asks twice, for two decisions (CTranslatorRelcacheToDXL.cpp:1628,1633):
 * whether it may split the aggregate across a Motion, and whether it may hash
 * rather than sort.  The second looks unrelated and is not: a hash aggregate
 * may spill, and reading a spilled batch back is the same merge of two
 * transition values that a split needs.
 */
bool
is_agg_partial_capable(Oid aggid)
{
	HeapTuple	tp;
	Form_pg_aggregate aggform;
	bool		result = true;

	tp = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);
	aggform = (Form_pg_aggregate) GETSTRUCT(tp);

	if (aggform->aggcombinefn == InvalidOid)
		result = false;
	else if (aggform->aggtranstype == INTERNALOID)
	{
		if (aggform->aggserialfn == InvalidOid ||
			aggform->aggdeserialfn == InvalidOid)
			result = false;
	}

	ReleaseSysCache(tp);
	return result;
}

/*
 * is_agg_repsafe
 *		May this aggregate be computed on a replicated slice?
 *
 * A replicated slice holds the same rows on every segment, so an aggregate
 * over one is computed everywhere and must give every segment the same
 * answer.  Most do; one whose result depends on the order rows arrive in, or
 * on anything local to a segment, does not, and Cloudberry has to gather
 * instead.
 *
 * Cloudberry reads pg_aggregate.aggrepsafeexec, a column it adds, defaulting
 * to false.  The port reads the "gp" label's replicate_safe flag on the
 * aggregate's pg_proc entry, which is the same default: absent means no.
 * Both are the conservative answer, and at this milestone nothing is
 * replicated, so nothing yet sets the flag -- what this function has to get
 * right is that it can be set at all, and on the object SECURITY LABEL ... ON
 * AGGREGATE addresses.
 */
bool
is_agg_repsafe(Oid aggid)
{
	ObjectAddress addr;

	if (!SearchSysCacheExists1(AGGFNOID, ObjectIdGetDatum(aggid)))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	/*
	 * An aggregate's OID is its pg_proc OID -- AGGFNOID is keyed on
	 * aggfnoid -- so the object a label goes on is the pg_proc row, which is
	 * also what SECURITY LABEL ... ON AGGREGATE resolves to.
	 */
	ObjectAddressSet(addr, ProcedureRelationId, aggid);
	return GpLabelHas(&addr, GP_LABEL_replicate_safe);
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

/*
 * get_comparison_type
 *		What this operator means, as a comparison.
 *
 * CmptOther when it is not a comparison at all, or belongs to no index
 * family that says what it means.
 *
 * THIS IS THE FUNCTION PostgreSQL 19 CHANGED MOST.  Cloudberry calls
 * get_op_btree_interpretation(), reads a StrategyNumber out of an
 * OpBtreeInterpretation, and switches on BTLessStrategyNumber and friends --
 * with ROWCOMPARE_NE standing in for "not equal", which has no btree
 * strategy number of its own.  All three names moved:
 *
 *	get_op_btree_interpretation	-> get_op_index_interpretation
 *	OpBtreeInterpretation		-> OpIndexInterpretation
 *	.strategy (StrategyNumber)	-> .cmptype (CompareType)
 *	ROWCOMPARE_NE				-> COMPARE_NE, in access/cmptype.h
 *
 * The last one is not a rename but a change of kind, and it is the point of
 * the exercise upstream: the field no longer holds an index AM's private
 * numbering, it holds what the operator means, and the AM is asked to do the
 * translation.  So the switch below is over meanings and no longer over
 * btree's numbers, which is what ORCA wanted in the first place -- it was
 * only ever reading the strategy to recover the meaning.
 *
 * The "first family wins" rule is Cloudberry's and is kept.  An operator can
 * belong to several families -- a reverse-ordering family that sorts
 * descending would call its "<" a greater-than -- so the answer is ambiguous
 * in principle.  Taking the first is arbitrary, and correct for every
 * operator in practice.
 */
CmpType
get_comparison_type(Oid oidOp)
{
	List	   *interpretations;
	OpIndexInterpretation *interpretation;

	interpretations = get_op_index_interpretation(oidOp);

	if (interpretations == NIL)
		return CmptOther;		/* belongs to no index family */

	interpretation = (OpIndexInterpretation *) linitial(interpretations);

	switch (interpretation->cmptype)
	{
		case COMPARE_LT:
			return CmptLT;
		case COMPARE_LE:
			return CmptLEq;
		case COMPARE_EQ:
			return CmptEq;
		case COMPARE_GE:
			return CmptGEq;
		case COMPARE_GT:
			return CmptGT;
		case COMPARE_NE:
			return CmptNEq;
		default:

			/*
			 * COMPARE_OVERLAP and COMPARE_CONTAINED_BY reach here.  They are
			 * real meanings, and PostgreSQL 19 grew them for the index AMs
			 * that have them; ORCA has no counterpart, so they are "some
			 * other operator".  Cloudberry raised an error in this arm
			 * because a btree strategy outside its five really was
			 * impossible.  That is no longer true, so this returns rather
			 * than raising: an operator ORCA cannot classify is not a
			 * failure, it is one ORCA will not reason about.
			 */
			return CmptOther;
	}
}

/*
 * get_comparison_operator
 *		The btree operator of this meaning over these two types, or InvalidOid.
 *
 * The inverse of get_comparison_type(), and ORCA uses it to build a
 * comparison the query did not write -- the equality a hash join needs
 * between two columns whose types it has just decided on, for instance.
 *
 * Only equality-shaped and ordering-shaped meanings can be built this way:
 * CmptNEq and CmptOther return InvalidOid, because "not equal" has no btree
 * strategy number to look up, which is the same asymmetry that made
 * Cloudberry borrow ROWCOMPARE_NE on the way out.
 *
 * The scan is Cloudberry's and is kept as it is, including its two
 * roughnesses: pg_amop has no index on this combination, so this reads the
 * whole catalog, and there can be several matching operators, of which the
 * first is taken.  PostgreSQL 19 has get_opfamily_member_for_cmptype(), but
 * it answers within one family, and this question is deliberately not asked
 * of a family: ORCA has two types and a meaning, and no family in hand.
 */
Oid
get_comparison_operator(Oid oidLeft, Oid oidRight, CmpType cmpt)
{
	int16		opstrat;
	HeapTuple	ht;
	Oid			result = InvalidOid;
	Relation	pg_amop;
	ScanKeyData scankey[4];
	SysScanDesc sscan;

	switch (cmpt)
	{
		case CmptLT:
			opstrat = BTLessStrategyNumber;
			break;
		case CmptLEq:
			opstrat = BTLessEqualStrategyNumber;
			break;
		case CmptEq:
			opstrat = BTEqualStrategyNumber;
			break;
		case CmptGEq:
			opstrat = BTGreaterEqualStrategyNumber;
			break;
		case CmptGT:
			opstrat = BTGreaterStrategyNumber;
			break;
		default:
			return InvalidOid;
	}

	pg_amop = table_open(AccessMethodOperatorRelationId, AccessShareLock);

	/*
	 * SELECT amopopr FROM pg_amop
	 *  WHERE amoplefttype = :1 AND amoprighttype = :2
	 *    AND amopmethod = btree AND amopstrategy = :3
	 */
	ScanKeyInit(&scankey[0],
				Anum_pg_amop_amoplefttype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidLeft));
	ScanKeyInit(&scankey[1],
				Anum_pg_amop_amoprighttype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidRight));
	ScanKeyInit(&scankey[2],
				Anum_pg_amop_amopmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(BTREE_AM_OID));
	ScanKeyInit(&scankey[3],
				Anum_pg_amop_amopstrategy,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(opstrat));

	sscan = systable_beginscan(pg_amop, InvalidOid, false, NULL, 4, scankey);

	if (HeapTupleIsValid(ht = systable_getnext(sscan)))
		result = ((Form_pg_amop) GETSTRUCT(ht))->amopopr;

	systable_endscan(sscan);
	table_close(pg_amop, AccessShareLock);

	return result;
}

/*
 * get_operator_opfamilies
 *		Every operator family this operator belongs to.
 *
 * ORCA uses the families to decide whether two operators can be reasoned
 * about together -- whether a join's equality and a table's distribution
 * hash agree, for instance.
 */
List *
get_operator_opfamilies(Oid opno)
{
	List	   *opfam_oids = NIL;
	CatCList   *catlist;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	htup = &catlist->members[i]->tuple;
		Form_pg_amop amop_tuple = (Form_pg_amop) GETSTRUCT(htup);

		opfam_oids = lappend_oid(opfam_oids, amop_tuple->amopfamily);
	}

	ReleaseSysCacheList(catlist);
	return opfam_oids;
}

/*
 * get_index_opfamilies
 *		The operator family of each key column of this index, in order.
 *
 * Key columns only: an index's INCLUDE columns have no opclass, so
 * indnkeyatts is the bound rather than indnatts.  ORCA needs this to know
 * which quals an index can answer.
 */
List *
get_index_opfamilies(Oid oidIndex)
{
	HeapTuple	htup;
	List	   *opfam_oids = NIL;
	bool		isnull = false;
	int			indnkeyatts;
	Datum		indclassDatum;
	oidvector  *indclass;

	htup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oidIndex));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for index %u", oidIndex);

	indnkeyatts = DatumGetInt16(SysCacheGetAttr(INDEXRELID, htup,
												Anum_pg_index_indnkeyatts,
												&isnull));
	Assert(!isnull);

	indclassDatum = SysCacheGetAttr(INDEXRELID, htup, Anum_pg_index_indclass,
									&isnull);
	if (isnull)
	{
		ReleaseSysCache(htup);
		return NIL;
	}
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	for (int i = 0; i < indnkeyatts; i++)
		opfam_oids = lappend_oid(opfam_oids,
								 get_opclass_family(indclass->values[i]));

	ReleaseSysCache(htup);
	return opfam_oids;
}

/*
 * default_partition_opfamily_for_type
 *		The btree family a range partition key of this type would use.
 *
 * InvalidOid when the type cannot be a range partition key at all -- it has
 * no btree family, no comparison procedure, or none of the three ordering
 * operators.
 *
 * The full flag set is Cloudberry's and is kept.  Only TYPECACHE_BTREE_OPFAMILY
 * is needed for the answer; the rest are asked for so that one lookup fills
 * the cache entry the checks below then read, instead of the checks each
 * faulting something in.
 */
Oid
default_partition_opfamily_for_type(Oid typeoid)
{
	TypeCacheEntry *tcache;

	tcache = lookup_type_cache(typeoid,
							   TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR |
							   TYPECACHE_GT_OPR | TYPECACHE_CMP_PROC |
							   TYPECACHE_EQ_OPR_FINFO |
							   TYPECACHE_CMP_PROC_FINFO |
							   TYPECACHE_BTREE_OPFAMILY);

	if (!tcache->btree_opf)
		return InvalidOid;
	if (!tcache->cmp_proc)
		return InvalidOid;
	if (!tcache->eq_opr && !tcache->lt_opr && !tcache->gt_opr)
		return InvalidOid;

	return tcache->btree_opf;
}

/*
 * get_check_constraint_oids
 *		Every validated CHECK constraint on this relation.
 *
 * Validated only: a constraint added NOT VALID may be false of rows already
 * there, so ORCA must not reason with it.  That test is Cloudberry's and is
 * the whole reason this is not a two-line wrapper over a syscache list.
 */
List *
get_check_constraint_oids(Oid oidRel)
{
	List	   *result = NIL;
	HeapTuple	htup;
	Relation	conrel;
	ScanKeyData scankey;
	SysScanDesc sscan;

	conrel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidRel));
	sscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							   NULL, 1, &scankey);

	while (HeapTupleIsValid(htup = systable_getnext(sscan)))
	{
		Form_pg_constraint contuple = (Form_pg_constraint) GETSTRUCT(htup);

		if (contuple->contype != CONSTRAINT_CHECK || !contuple->convalidated)
			continue;

		result = lappend_oid(result, contuple->oid);
	}

	systable_endscan(sscan);
	table_close(conrel, AccessShareLock);

	return result;
}

/*
 * get_check_constraint_name
 *		The name of a check constraint, palloc'd, or NULL.
 *
 * PostgreSQL's own get_constraint_name() answers this already; Cloudberry
 * keeps the alias so that ORCA's calls read as being about check
 * constraints, and it is kept for the same reason.
 */
char *
get_check_constraint_name(Oid oidCheckconstraint)
{
	return get_constraint_name(oidCheckconstraint);
}

/*
 * get_check_constraint_relid
 *		The relation a check constraint is on, or InvalidOid.
 */
Oid
get_check_constraint_relid(Oid oidCheckconstraint)
{
	HeapTuple	tp;

	tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oidCheckconstraint));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);
		Oid			result;

		result = contup->conrelid;
		ReleaseSysCache(tp);
		return result;
	}

	return InvalidOid;
}

/*
 * get_check_constraint_expr_tree
 *		A check constraint's expression, as a palloc'd node tree, or NULL.
 *
 * NULL for a constraint with no expression as well as for one that is not
 * there: pg_constraint.conbin is null for every constraint that is not a
 * CHECK, and ORCA only ever asks this of OIDs get_check_constraint_oids()
 * gave it.
 */
Node *
get_check_constraint_expr_tree(Oid oidCheckconstraint)
{
	HeapTuple	tp;
	Node	   *result = NULL;

	tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oidCheckconstraint));
	if (HeapTupleIsValid(tp))
	{
		Datum		conbin;
		bool		isnull;

		conbin = SysCacheGetAttr(CONSTROID, tp, Anum_pg_constraint_conbin,
								 &isnull);
		if (!isnull)
			result = stringToNode(TextDatumGetCString(conbin));

		ReleaseSysCache(tp);
	}

	return result;
}

/*
 * get_relation_keys
 *		The unique keys of a relation: a List of Lists of attribute numbers.
 *
 * ORCA turns each into a functional dependency, which is what lets it drop a
 * grouping or prove a join does not duplicate rows.
 *
 * Two filters, both Cloudberry's and both load-bearing.  UNIQUE and PRIMARY
 * KEY only, because those are the constraint kinds that promise uniqueness.
 * And not deferrable, because a deferrable constraint may be false in the
 * middle of a transaction -- which is exactly when a query might run.
 *
 * Note this reads pg_constraint and not pg_index, so a plain unique index
 * with no constraint behind it is not reported.  That is Cloudberry's
 * behaviour and ORCA is built on it; it costs a missed inference, never a
 * wrong one.
 */
List *
get_relation_keys(Oid relid)
{
	List	   *keys = NIL;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	htup;

	rel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		Form_pg_constraint contuple = (Form_pg_constraint) GETSTRUCT(htup);
		List	   *key = NIL;
		Datum		dat;
		bool		isnull = false;
		Datum	   *dats = NULL;
		int			numKeys = 0;

		if (contuple->contype != CONSTRAINT_UNIQUE &&
			contuple->contype != CONSTRAINT_PRIMARY)
			continue;

		if (contuple->condeferrable)
			continue;

		dat = heap_getattr(htup, Anum_pg_constraint_conkey,
						   RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		deconstruct_array(DatumGetArrayTypeP(dat), INT2OID, 2, true, TYPALIGN_SHORT,
						  &dats, NULL, &numKeys);

		for (int i = 0; i < numKeys; i++)
			key = lappend_int(key, DatumGetInt16(dats[i]));

		keys = lappend(keys, key);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return keys;
}

/*
 * get_att_stats
 *		A copy of the pg_statistic row for a column, or NULL if there is none.
 *
 * The caller owns the tuple and frees it.  ORCA unpacks the slots itself
 * rather than going through get_attstatsslot(), because it turns them into
 * its own histogram objects.
 *
 * Inherited statistics are preferred, and the comment Cloudberry leaves here
 * is worth keeping: ORCA does not know there are two kinds.  A partitioned
 * table's useful statistics are the inherited ones, which cover the
 * children; the non-inherited row describes the parent alone, which for a
 * partitioned table is empty.  Asking for inherited first and falling back
 * is how one call serves both.
 */
HeapTuple
get_att_stats(Oid relid, AttrNumber attrnum)
{
	HeapTuple	result;

	result = SearchSysCacheCopy3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(attrnum),
								 BoolGetDatum(true));
	if (!result)
		result = SearchSysCacheCopy3(STATRELATTINH,
									 ObjectIdGetDatum(relid),
									 Int16GetDatum(attrnum),
									 BoolGetDatum(false));

	return result;
}

/*
 * has_subclass_slow
 *		Does this relation really have a child?
 *
 * PostgreSQL's has_subclass() reads pg_class.relhassubclass, which is a hint:
 * it is set when a child is added and not always cleared when the last one
 * goes, so it can say yes where the answer is no.  This asks pg_inherits.
 *
 * The cheap test comes first and short-circuits, so the scan runs only for
 * relations that might have children -- which means the expensive answer is
 * paid for only when the cheap one was true.
 */
bool
has_subclass_slow(Oid relationId)
{
	ScanKeyData scankey;
	Relation	rel;
	SysScanDesc sscan;
	bool		result;

	if (!has_subclass(relationId))
		return false;

	rel = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&scankey, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationId));

	/* No index on inhparent. */
	sscan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &scankey);

	result = (systable_getnext(sscan) != NULL);

	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * get_trigger_type
 *		The tgtype bitmask of a trigger.
 */
int32
get_trigger_type(Oid triggerid)
{
	Relation	rel;
	HeapTuple	tp;
	int32		result;
	ScanKeyData scankey;
	SysScanDesc sscan;

	ScanKeyInit(&scankey, Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(triggerid));
	rel = table_open(TriggerRelationId, AccessShareLock);
	sscan = systable_beginscan(rel, TriggerOidIndexId, true, NULL, 1, &scankey);

	tp = systable_getnext(sscan);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for trigger %u", triggerid);

	result = ((Form_pg_trigger) GETSTRUCT(tp))->tgtype;

	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * trigger_enabled
 *		Would this trigger fire?
 *
 * Cloudberry's FIXME on the ORIGIN case is kept, because the question it
 * raises is still open here: a trigger set to fire on origin does not fire
 * when session_replication_role is "replica", so strictly this should
 * consult that setting -- and then ORCA's metadata cache would have to be
 * flushed whenever it changed, since a cached plan would have been built on
 * the old answer.  Answering "yes" is the safe direction: ORCA keeps the
 * update path that respects triggers, which is correct either way, just not
 * always the cheapest.
 */
bool
trigger_enabled(Oid triggerid)
{
	Relation	rel;
	HeapTuple	tp;
	bool		result;
	char		tgenabled;
	ScanKeyData scankey;
	SysScanDesc sscan;

	ScanKeyInit(&scankey, Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(triggerid));
	rel = table_open(TriggerRelationId, AccessShareLock);
	sscan = systable_beginscan(rel, TriggerOidIndexId, true, NULL, 1, &scankey);

	tp = systable_getnext(sscan);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for trigger %u", triggerid);

	tgenabled = ((Form_pg_trigger) GETSTRUCT(tp))->tgenabled;

	switch (tgenabled)
	{
		case TRIGGER_FIRES_ON_ORIGIN:
			/* FIXME: see the note above about session_replication_role. */
		case TRIGGER_FIRES_ALWAYS:
			result = true;
			break;
		case TRIGGER_FIRES_ON_REPLICA:
		case TRIGGER_DISABLED:
			result = false;
			break;
		default:
			elog(ERROR, "unknown trigger enabled state: %c", tgenabled);
			result = false;		/* not reached */
			break;
	}

	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * has_update_triggers
 *		Does this relation have an enabled UPDATE trigger?
 *
 * ORCA asks before it plans an update that would move a row: a split update
 * is a delete and an insert, which would fire the wrong triggers, so a table
 * with update triggers keeps the plan that does not split.
 *
 * "including_children" is Cloudberry's and exists because ORCA does not
 * expand a partitioned table's children the way the Postgres planner does.
 * The planner sees each leaf as a relation of its own and asks about each;
 * ORCA sees the parent, so it has to ask about the whole tree at once.
 *
 * NoLock on the children is safe for the same reason it is in Cloudberry:
 * the parent is already locked by the statement that led here, so no
 * partition can be detached underneath this.
 *
 * A TABLE NOBODY PUT A TRIGGER ON CAN ANSWER YES.  A DEFERRABLE unique or
 * primary key constraint is enforced by an internal AFTER ROW trigger, whose
 * tgtype carries row|insert|update -- so a table whose only unusual feature
 * is a deferrable constraint reports an update trigger, and ORCA will not
 * plan a split update on it.  This is Cloudberry's behaviour too, and it is
 * not obviously wrong: the trigger really would fire, and a split update
 * really would fire it as an insert instead.  It is recorded because it is
 * invisible from the SQL a user wrote, and the tests pin it.
 */
bool
has_update_triggers(Oid relid, bool including_children)
{
	Relation	relation;
	bool		result = false;

	relation = RelationIdGetRelation(relid);
	if (!RelationIsValid(relation))
		elog(ERROR, "could not open relation with OID %u", relid);

	if (relation->rd_rel->relhastriggers)
	{
		if (relation->trigdesc == NULL)
			RelationBuildTriggers(relation);

		if (relation->trigdesc)
		{
			for (int i = 0; i < relation->trigdesc->numtriggers; i++)
			{
				Trigger		trigger = relation->trigdesc->triggers[i];

				if (trigger_enabled(trigger.tgoid) &&
					(get_trigger_type(trigger.tgoid) & TRIGGER_TYPE_UPDATE) ==
					TRIGGER_TYPE_UPDATE)
				{
					result = true;
					break;
				}
			}
		}
	}

	if (including_children && !result &&
		relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *partitions = find_inheritance_children(relid, NoLock);
		ListCell   *lc;

		foreach(lc, partitions)
		{
			if (has_update_triggers(lfirst_oid(lc), true))
			{
				result = true;
				break;
			}
		}

		list_free(partitions);
	}

	RelationClose(relation);

	return result;
}
