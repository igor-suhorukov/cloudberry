//---------------------------------------------------------------------------
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
//	@filename:
//		gpdbwrappers.cpp
//
//	@doc:
//		Everything the translator is allowed to ask the server, implemented.
//
//		Ported from github/cloudberry/src/backend/gpopt/gpdbwrappers.cpp.
//		198 definitions over 197 names -- WalkQueryTree is overloaded -- and
//		the port of them was measured before it was written, by resolving
//		every name each body calls against PostgreSQL 19's headers, the
//		port's compat layer and ORCA's own core.  170 needed nothing but this
//		file's includes.  The other 28 are marked, in six groups, and the
//		comment on each says what it is waiting for.
//
//		RETURNING FROM INSIDE GP_WRAP IS SAFE, and 173 of the 179 bodies
//		here do it, as 188 of Cloudberry's 194 do.  GP_WRAP_START puts a
//		CAutoExceptionStack on the stack, and its destructor puts
//		PG_exception_stack and error_context_stack back on every way out of
//		the block -- a return, the GPOS_RAISE after a longjmp, or an
//		exception passing through.  (An earlier version of this comment said
//		the opposite and attributed it to Cloudberry.  Cloudberry says no
//		such thing, and the guard makes it unnecessary.)  What is not safe
//		is a return from inside PostgreSQL's own PG_TRY, which leaves
//		PG_exception_stack pointing at a dead frame; nothing here uses one.
//
//		./README in Cloudberry's tree lists the catalog tables each wrapper
//		reads, and the `catalog tables:` comments in the bodies are what
//		keeps it honest -- MDCacheNeedsReset below registers an invalidation
//		callback for every one of them, so a wrapper that reads a new
//		catalog without saying so is a stale metadata cache.
//
//---------------------------------------------------------------------------

#include "gpdbwrappers.h"

#include <limits>  // std::numeric_limits

#include "gpos/base.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "gpos/error/CAutoExceptionStack.h"
#include "gpos/error/CException.h"

#include "gpdbdefs.h"
#include "naucrates/exception.h"

#include "catalog/pg_collation.h"
extern "C" {
#include "access/amapi.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "parser/parse_agg.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "parser/parse_coerce.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/snapmgr.h"

/* gp_core's, over the "gp" security label */
#include "gp_policy.h"

/* gp_core's cdbhash: the hash function a distribution key is hashed with */
#include "gp_hash.h"

/*
 * Eleven operator OIDs that PostgreSQL has and does not name.  Cloudberry
 * names them by adding an oid_symbol to pg_operator.dat, which the port
 * cannot do without patching a catalog; see the header for the check that
 * they are the same operators in PostgreSQL 19.
 */
#include "cb_operator_oids.h"

/*
 * Cloudberry's BuildForeignScan(), from foreign/foreign.c, which it patched;
 * see CreateForeignScan.
 */
#include "cb_foreign.h"

/* The scans of a partitioned table's partitions; see PlanForPartition. */
#include "cb_dynamicscan.h"

/* ORCA's Gather Motion, and what stage A can carry out; see CheckMotions. */
#include "cb_motion.h"

/* An identity column's next value, as ORCA carries it; see NextValueCall. */
#include "cb_nextvalue.h"

/* PostGIS's index support function; see IsPostgisIndexSupport. */
#include "gp_orca_postgis.h"

/*
 * Left out of Cloudberry's list:
 *
 *	 access/external.h		M5, external tables.
 *	 cdb/cdbvars.h			gp_core answers these; cb_compat.h, through
 *							gpdbdefs.h, is where they come from.
 *	 optimizer/clauses.h	folded into optimizer/optimizer.h upstream; what
 *							Cloudberry added to it is in cb_clauses.h.
 *
 * Cloudberry declares enable_parallel and max_parallel_workers_per_gather by
 * hand here.  The second is PostgreSQL's and comes from optimizer/optimizer.h;
 * the first is Cloudberry's own setting, and the port has no intra-segment
 * parallelism to switch on -- see IsParallelModeOK at the end of this file.
 */
}
#define GP_WRAP_START                                            \
	sigjmp_buf local_sigjmp_buf;                                 \
	{                                                            \
		CAutoExceptionStack aes((void **) &PG_exception_stack,   \
								(void **) &error_context_stack); \
		if (0 == sigsetjmp(local_sigjmp_buf, 0))                 \
		{                                                        \
			aes.SetLocalJmp(&local_sigjmp_buf)

#define GP_WRAP_END                                        \
	}                                                      \
	else                                                   \
	{                                                      \
		GPOS_RAISE(gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError); \
	}                                                      \
	}
//---------------------------------------------------------------------------
//	A wrapper for something this port does not have yet raises through
//	GP_UNPORTED, which the translator shares; see gp_unported.h for why it
//	raises rather than answering.
//---------------------------------------------------------------------------
#include "gp_unported.h"


using namespace gpos;

bool
gpdb::AggregateExists(Oid oid)
{
	GP_WRAP_START;
	{
		return aggregate_exists(oid);
	}
	GP_WRAP_END;
	return false;
}

Bitmapset *
gpdb::BmsAddMember(Bitmapset *a, int x)
{
	GP_WRAP_START;
	{
		return bms_add_member(a, x);
	}
	GP_WRAP_END;
	return nullptr;
}

Bitmapset *
gpdb::BmsUnion(const Bitmapset *a, const Bitmapset *b)
{
	GP_WRAP_START;
	{
		return bms_union(a, b);
	}
	GP_WRAP_END;
	return nullptr;
}

int
gpdb::BmsNextMember(const Bitmapset *a, int prevbit)
{
	GP_WRAP_START;
	{
		return bms_next_member(a, prevbit);
	}
	GP_WRAP_END;
	return -2;
}

void *
gpdb::CopyObject(void *from)
{
	GP_WRAP_START;
	{
		return copyObjectImpl(from);
	}
	GP_WRAP_END;
	return nullptr;
}

Size
gpdb::DatumSize(Datum value, bool type_by_val, int iTypLen)
{
	GP_WRAP_START;
	{
		return datumGetSize(value, type_by_val, iTypLen);
	}
	GP_WRAP_END;
	return 0;
}

Node *
gpdb::MutateExpressionTree(Node *node, Node *(*mutator)(Node *, void *), void *context)
{
	GP_WRAP_START;
	{
		return expression_tree_mutator(node, mutator, context);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::WalkExpressionTree(Node *node, bool (*walker)(Node *, void *), void *context)
{
	GP_WRAP_START;
	{
		return expression_tree_walker(node, walker, context);
	}
	GP_WRAP_END;
	return false;
}

gpos::BOOL
gpdb::WalkQueryTree(Query *query, bool (*walker)(), void *context, int flags)
{
	GP_WRAP_START;
	{
		return query_tree_walker(query, walker, context, flags);
	}
	GP_WRAP_END;
	return false;
}

Oid
gpdb::ExprType(Node *expr)
{
	GP_WRAP_START;
	{
		return exprType(expr);
	}
	GP_WRAP_END;
	return 0;
}

int32
gpdb::ExprTypeMod(Node *expr)
{
	GP_WRAP_START;
	{
		return exprTypmod(expr);
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::ExprCollation(Node *expr)
{
	GP_WRAP_START;
	{
		if (expr && IsA(expr, List))
		{
			// GPDB_91_MERGE_FIXME: collation
			List *exprlist = (List *) expr;
			ListCell *lc;

			Oid collation = InvalidOid;
			foreach (lc, exprlist)
			{
				Node *expr = (Node *) lfirst(lc);
				if ((collation = exprCollation(expr)) != InvalidOid)
				{
					break;
				}
			}
			return collation;
		}
		else
		{
			return exprCollation(expr);
		}
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::TypeCollation(Oid type)
{
	GP_WRAP_START;
	{
		// The real oid returned by the get_typcollation function as the result
		// Cancel the logic that used the value DEFAULT_COLLATION_OID
		Oid typcollation = get_typcollation(type);
		return OidIsValid(typcollation) ? typcollation : InvalidOid;
	}
	GP_WRAP_END;
	return 0;
}

void
gpdb::TypLenByVal(Oid typid, int16 *typlen, bool *typbyval)
{
	GP_WRAP_START;
	{
		get_typlenbyval(typid, typlen, typbyval);
	}
	GP_WRAP_END;
}

List *
gpdb::ExtractNodesPlan(Plan *pl, int node_tag, bool descend_into_subqueries)
{
	GP_WRAP_START;
	{
		return extract_nodes_plan(pl, node_tag, descend_into_subqueries);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::ExtractNodesExpression(Node *node, int node_tag,
							 bool descend_into_subqueries)
{
	GP_WRAP_START;
	{
		return extract_nodes_expression(node, node_tag,
										descend_into_subqueries);
	}
	GP_WRAP_END;
	return NIL;
}

void
gpdb::FreeAttrStatsSlot(AttStatsSlot *sslot)
{
	GP_WRAP_START;
	{
		free_attstatsslot(sslot);
		return;
	}
	GP_WRAP_END;
}

bool
gpdb::IsFuncAllowedForPartitionSelection(Oid funcid)
{
	GP_WRAP_START;
	switch (funcid)
	{
			// These are the functions we have allowed as lossy casts for Partition selection.
			// For range partition selection, the logic in ORCA checks on bounds of the partition ranges.
			// Hence these must be increasing functions.
		case F_TIMESTAMP_DATE:		// date(timestamp) -> date
		case F_INT4_FLOAT8:			// int4(float8) -> int4
		case F_INT4_FLOAT4:			// int4(float4) -> int4
		case F_INT2_INT8:			// int2(int8) -> int2
		case F_INT4_INT8:			// int4(int8) -> int4
		case F_INT2_INT4:			// int2(int4) -> int2
		case F_INT8_FLOAT4:			// int8(float4) -> int8
		case F_INT2_FLOAT4:			// int2(float4) -> int2
		case F_NUMERIC_FLOAT4:		// numeric(float4) -> numeric
		case F_INT8_FLOAT8:			// int8(float8) -> int8
		case F_INT2_FLOAT8:			// int2(float4) -> int2
		case F_FLOAT4_FLOAT8:		// float4(float8) -> float4
		case F_FLOAT8_NUMERIC:		// numeric(float8) -> numeric
		case F_NUMERIC_INT8:		// int8(numeric) -> int8
		case F_NUMERIC_INT2:		// int2(numeric) -> int2
		case F_NUMERIC_INT4:		// int4(numeric) -> int4
			return true;
		default:
			return false;
	}
	GP_WRAP_END;
}

bool
gpdb::FuncStrict(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return func_strict(funcid);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsFuncNDVPreserving(Oid funcid)
{
	// Given a function oid, return whether it's one of a list of NDV-preserving
	// functions (estimated NDV of output is similar to that of the input)
	switch (funcid)
	{
		// for now, these are the functions we consider for this optimization
		case F_LOWER_TEXT:
		case F_LTRIM_TEXT:
		case F_BTRIM_TEXT:
		case F_RTRIM_TEXT:
		case F_UPPER_TEXT:
			return true;
		default:
			return false;
	}
}

char
gpdb::FuncStability(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return func_volatile(funcid);
	}
	GP_WRAP_END;
	return '\0';
}

RegProcedure
gpdb::FuncSupport(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_support(funcid);
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::FuncNamespace(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_namespace(funcid);
	}
	GP_WRAP_END;
	return InvalidOid;
}

char
gpdb::FuncExecLocation(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return func_exec_location(funcid);
	}
	GP_WRAP_END;
	return '\0';
}

bool
gpdb::FunctionExists(Oid oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return function_exists(oid);
	}
	GP_WRAP_END;
	return false;
}

Oid
gpdb::GetAggIntermediateResultType(Oid aggid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return get_agg_transtype(aggid);
	}
	GP_WRAP_END;
	return 0;
}

int
gpdb::GetAggregateArgTypes(Aggref *aggref, Oid *inputTypes)
{
	GP_WRAP_START;
	{
		return get_aggregate_argtypes(aggref, inputTypes);
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::ResolveAggregateTransType(Oid aggfnoid, Oid aggtranstype, Oid *inputTypes,
								int numArguments)
{
	GP_WRAP_START;
	{
		return resolve_aggregate_transtype(aggfnoid, aggtranstype, inputTypes,
										   numArguments);
	}
	GP_WRAP_END;
	return 0;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

void
gpdb::GetAggregateInfo(Aggref *aggref, Oid *aggtransfn,
					   Oid *aggfinalfn, Oid *aggcombinefn,
					   Oid *aggserialfn, Oid *aggdeserialfn,
					   Oid *aggtranstype, int *aggtransspace,
					   Datum *initValue, bool *initValueIsNull,
					   bool *shareable)
{
	GP_WRAP_START;
	{
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		Datum		textInitVal;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;

		aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);

		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
		*aggtransfn = aggform->aggtransfn;
		*aggfinalfn = aggform->aggfinalfn;
		*aggcombinefn = aggform->aggcombinefn;
		*aggserialfn = aggform->aggserialfn;
		*aggdeserialfn = aggform->aggdeserialfn;
		*aggtranstype = aggform->aggtranstype;
		*aggtransspace = aggform->aggtransspace;

		/*
		 * Resolve the possibly-polymorphic aggregate transition type.
		 */
		/* extract argument types (ignoring any ORDER BY expressions) */
		numArguments = get_aggregate_argtypes(aggref, inputTypes);

		/* resolve actual type of transition state, if polymorphic */
		*aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid,
											   *aggtranstype,
											   inputTypes,
											   numArguments);

		/* get initial value */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									Anum_pg_aggregate_agginitval,
									initValueIsNull);


		if (*initValueIsNull)
			*initValue = (Datum) 0;
		else
			*initValue = GetAggInitVal(textInitVal, *aggtranstype);
		
		/*
		 * If finalfn is marked read-write, we can't share transition states; but
		 * it is okay to share states for AGGMODIFY_SHAREABLE aggs.
		 *
		 * In principle, in a partial aggregate, we could share the transition
		 * state even if the final function is marked as read-write, because the
		 * partial aggregate doesn't execute the final function.  But it's too
		 * early to know whether we're going perform a partial aggregate.
		 */
		*shareable = (aggform->aggfinalmodify != AGGMODIFY_READ_WRITE);

		ReleaseSysCache(aggTuple);

	}
	GP_WRAP_END;
}


int
gpdb::FindCompatibleAgg(List *agginfos, Aggref *newagg,
						List **same_input_transnos)
{

	GP_WRAP_START;
	{
		return find_compatible_agg(agginfos, newagg, same_input_transnos);
	}
	GP_WRAP_END;
	return -1;
}

int
gpdb::FindCompatibleTrans(List *aggtransinfos, bool shareable,
						  Oid aggtransfn, Oid aggtranstype,
						  int transtypeLen, bool transtypeByVal,
						  Oid aggcombinefn, Oid aggserialfn,
						  Oid aggdeserialfn, Datum initValue, 
						  bool initValueIsNull, List *transnos)
{
	GP_WRAP_START;
	{
		return find_compatible_trans(aggtransinfos, shareable, aggtransfn,
			aggtranstype, transtypeLen, transtypeByVal, aggcombinefn, aggserialfn,
			aggdeserialfn, initValue, initValueIsNull, transnos);
	}
	GP_WRAP_END;
	return -1;
}


Query *
gpdb::FlattenJoinAliasVar(Query *query, gpos::ULONG query_level)
{
	GP_WRAP_START;
	{
		return flatten_join_alias_var_optimizer(query, query_level);
	}
	GP_WRAP_END;

	return nullptr;
}

bool
gpdb::IsOrderedAgg(Oid aggid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return is_agg_ordered(aggid);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsRepSafeAgg(Oid aggid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return is_agg_repsafe(aggid);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsAggPartialCapable(Oid aggid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return is_agg_partial_capable(aggid);
	}
	GP_WRAP_END;
	return false;
}

Oid
gpdb::GetAggregate(const char *agg, Oid type_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return get_aggregate(agg, type_oid);
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::GetArrayType(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_array_type(typid);
	}
	GP_WRAP_END;
	return 0;
}

bool
gpdb::GetAttrStatsSlot(AttStatsSlot *sslot, HeapTuple statstuple, int reqkind,
					   Oid reqop, int flags)
{
	GP_WRAP_START;
	{
		return get_attstatsslot(sslot, statstuple, reqkind, reqop, flags);
	}
	GP_WRAP_END;
	return false;
}

HeapTuple
gpdb::GetAttStats(Oid relid, AttrNumber attnum)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_statistic */
		return get_att_stats(relid, attnum);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::GetExtStats(Relation rel)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_statistic_ext */
		return GetRelationExtStatistics(rel);
	}
	GP_WRAP_END;
	return nullptr;
}

char *
gpdb::GetExtStatsName(Oid statOid)
{
	GP_WRAP_START;
	{
		return GetExtStatisticsName(statOid);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::GetExtStatsKinds(Oid statOid)
{
	GP_WRAP_START;
	{
		return GetExtStatisticsKinds(statOid);
	}
	GP_WRAP_END;
	return nullptr;
}

Oid
gpdb::GetCommutatorOp(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_commutator(opno);
	}
	GP_WRAP_END;
	return 0;
}

char *
gpdb::GetCheckConstraintName(Oid check_constraint_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return get_check_constraint_name(check_constraint_oid);
	}
	GP_WRAP_END;
	return nullptr;
}

Oid
gpdb::GetCheckConstraintRelid(Oid check_constraint_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return get_check_constraint_relid(check_constraint_oid);
	}
	GP_WRAP_END;
	return 0;
}

Node *
gpdb::PnodeCheckConstraint(Oid check_constraint_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return get_check_constraint_expr_tree(check_constraint_oid);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::GetCheckConstraintOids(Oid rel_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return get_check_constraint_oids(rel_oid);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::GetRelationPartConstraints(Relation rel)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_partition, pg_partition_rule, pg_constraint */
		List *part_quals = RelationGetPartitionQual(rel);
		if (part_quals)
		{
			return (Node *) make_ands_explicit(part_quals);
		}
	}
	GP_WRAP_END;
	return nullptr;
}

PartitionKey
gpdb::GetRelationPartitionKey(Relation rel)
{
	GP_WRAP_START;
	{
		return RelationGetPartitionKey(rel);
	}
	GP_WRAP_END;
	return nullptr;
}

PartitionDesc
gpdb::RelationGetPartitionDesc(Relation rel, bool omit_detached)
{
	// Cloudberry changed this function's contract without changing its
	// signature, and the translator depends on the change.  PostgreSQL's
	// RelationGetPartitionDesc asserts that the relation is partitioned;
	// Cloudberry's returns NULL when it is not
	// (github/cloudberry/src/backend/partitioning/partdesc.c), and six
	// callers in the relcache translator ask it as the question "is this
	// partitioned?" -- CheckUnsupportedRelation first, for every relation.
	// Against PostgreSQL 19 the call resolves and compiles, and the
	// wrapper measurement counted it among those that needed nothing; the
	// first plain table the translator met failed the assertion in an
	// assert-enabled build, and in a release build would have read the
	// partition descriptor of a table that has none.  So the wrapper keeps
	// Cloudberry's contract.
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		return nullptr;
	}

	GP_WRAP_START;
	{
		return ::RelationGetPartitionDesc(rel, omit_detached);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::GetCastFunc(Oid src_oid, Oid dest_oid, bool *is_binary_coercible,
				  Oid *cast_fn_oid, CoercionPathType *pathtype)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_cast */
		return get_cast_func(src_oid, dest_oid, is_binary_coercible,
							 cast_fn_oid, pathtype);
	}
	GP_WRAP_END;
	return false;
}

unsigned int
gpdb::GetComparisonType(Oid op_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_comparison_type(op_oid);
	}
	GP_WRAP_END;
	return CmptOther;
}

Oid
gpdb::GetComparisonOperator(Oid left_oid, Oid right_oid, unsigned int cmpt)
{
	GP_WRAP_START;
	{
		SIMPLE_FAULT_INJECTOR("gpdbwrappers_get_comparison_operator");
		/* catalog tables: pg_amop */
		return get_comparison_operator(left_oid, right_oid, (CmpType) cmpt);
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::GetEqualityOp(Oid type_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		Oid eq_opr;

		get_sort_group_operators(type_oid, false, true, false, nullptr, &eq_opr,
								 nullptr, nullptr);

		return eq_opr;
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::GetEqualityOpForOrderingOp(Oid opno, bool *reverse)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_equality_op_for_ordering_op(opno, reverse);
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::GetOrderingOpForEqualityOp(Oid opno, bool *reverse)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_ordering_op_for_equality_op(opno, reverse);
	}
	GP_WRAP_END;
	return InvalidOid;
}

char *
gpdb::GetFuncName(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_name(funcid);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::GetFuncOutputArgTypes(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_output_arg_types(funcid);
	}
	GP_WRAP_END;
	return NIL;
}

namespace
{
// The query's calls of one function in FROM, each a range table entry of its
// own; see FunctionScanColumns.
struct SFunctionCallSearch
{
	Oid funcid;
	List *found;  // RangeTblEntry *
};

bool
FindFunctionCallsWalker(Node *node, void *context)
{
	SFunctionCallSearch *search = (SFunctionCallSearch *) context;

	if (node == nullptr)
		return false;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, FindFunctionCallsWalker,
								 context, QTW_EXAMINE_RTES_BEFORE);

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_FUNCTION && list_length(rte->functions) == 1 &&
			!rte->funcordinality)
		{
			RangeTblFunction *rtfunc =
				(RangeTblFunction *) linitial(rte->functions);

			if (IsA(rtfunc->funcexpr, FuncExpr) &&
				((FuncExpr *) rtfunc->funcexpr)->funcid == search->funcid)
				search->found = lappend(search->found, rte);
		}
		return false;
	}

	return expression_tree_walker(node, FindFunctionCallsWalker, context);
}

// Where `name` is among `colnames`, from 1, or 0 unless it is there once.
int
ColumnNamePosition(List *colnames, const char *name)
{
	ListCell *lc;
	int n = 0;
	int found = 0;

	foreach (lc, colnames)
	{
		n++;
		if (strcmp(strVal(lfirst(lc)), name) == 0)
		{
			if (found != 0)
				return 0;
			found = n;
		}
	}
	return found;
}
}  // namespace

bool
gpdb::FunctionScanColumns(Node *funcexpr, Query *query, int ncols,
						  char **names, int *attnos, List **colnames,
						  RangeTblFunction **coldef)
{
	GP_WRAP_START;
	{
		Oid rettype;
		TupleDesc tupdesc;
		TypeFuncClass functypclass =
			get_expr_result_type(funcexpr, &rettype, &tupdesc);
		RangeTblEntry *call = nullptr;

		*colnames = NIL;
		*coldef = nullptr;

		/*
		 * The call in the query's range table: its columns are named as the
		 * query names them, aliases and all, which is how ORCA names them, and
		 * a record's are its column definition list.  It is found by the
		 * function and the names, and used only if every such call the query
		 * makes puts the names in the same order.
		 */
		if (IsA(funcexpr, FuncExpr) && query != nullptr)
		{
			SFunctionCallSearch search = {((FuncExpr *) funcexpr)->funcid, NIL};
			int *these = (int *) palloc(sizeof(int) * Max(ncols, 1));
			ListCell *lc;

			(void) FindFunctionCallsWalker((Node *) query, &search);

			foreach (lc, search.found)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
				bool all = true;

				for (int i = 0; i < ncols && all; i++)
				{
					these[i] = ColumnNamePosition(rte->eref->colnames, names[i]);
					all = (these[i] != 0);
				}
				if (!all)
					continue;
				if (call == nullptr)
				{
					memcpy(attnos, these, sizeof(int) * ncols);
					call = rte;
				}
				else if (memcmp(attnos, these, sizeof(int) * ncols) != 0)
					return false; /* two calls, two orders */
			}

			if (call != nullptr)
			{
				RangeTblFunction *rtfunc =
					(RangeTblFunction *) linitial(call->functions);

				*colnames = list_copy_deep(call->eref->colnames);
				if (rtfunc->funccolnames != NIL)
					*coldef = rtfunc;
				return true;
			}
		}

		/*
		 * None: a call ORCA folded to a constant.  A composite result has
		 * names of its own, and a scalar one has one column.
		 */
		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			for (int a = 0; a < tupdesc->natts; a++)
			{
				Form_pg_attribute att = TupleDescAttr(tupdesc, a);

				*colnames = lappend(
					*colnames,
					makeString(pstrdup(att->attisdropped ? ""
														 : NameStr(att->attname))));
			}
			for (int i = 0; i < ncols; i++)
			{
				attnos[i] = ColumnNamePosition(*colnames, names[i]);
				if (attnos[i] == 0)
					return false;
			}
			return true;
		}

		if (functypclass == TYPEFUNC_SCALAR && ncols == 1)
		{
			attnos[0] = 1;
			*colnames = list_make1(makeString(pstrdup(names[0])));
			return true;
		}

		return false;
	}
	GP_WRAP_END;
	return false;
}

List *
gpdb::GetFuncArgTypes(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_arg_types(funcid);
	}
	GP_WRAP_END;
	return NIL;
}

bool
gpdb::GetFuncRetset(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_retset(funcid);
	}
	GP_WRAP_END;
	return false;
}

Oid
gpdb::GetFuncRetType(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_rettype(funcid);
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::GetInverseOp(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_negator(opno);
	}
	GP_WRAP_END;
	return 0;
}

RegProcedure
gpdb::GetOpFunc(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_opcode(opno);
	}
	GP_WRAP_END;
	return 0;
}

char *
gpdb::GetOpName(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_opname(opno);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::GetRelationKeys(Oid relid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return get_relation_keys(relid);
	}
	GP_WRAP_END;
	return NIL;
}

Oid
gpdb::GetTypeRelid(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_typ_typrelid(typid);
	}
	GP_WRAP_END;
	return 0;
}

char *
gpdb::GetTypeName(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_type_name(typid);
	}
	GP_WRAP_END;
	return nullptr;
}

int
gpdb::GetGPSegmentCount(void)
{
	GP_WRAP_START;
	{
		return getgpsegmentCount();
	}
	GP_WRAP_END;
	return 0;
}

bool
gpdb::IsAccessMethodNamed(Oid am_oid, const char *am_name)
{
	// Cloudberry compares relam against BITMAP_AM_OID, AO_ROW_TABLE_AM_OID,
	// AO_COLUMN_TABLE_AM_OID and PAX_AM_OID, fixed OIDs it adds to pg_am.dat.
	// In the port each of those access methods belongs to a module -- gp_ao
	// and pax, at M5 -- and is created by CREATE EXTENSION with an ordinary
	// OID, so it can only be found by name, as Track A 2.2 proposed for PAX.
	//
	// The InvalidOid test is the one that matters.  A partitioned table, a
	// view and a composite type all have relam 0, and so does the answer to
	// looking up an access method that is not installed -- which on
	// PostgreSQL 19 is every one of these until its module is.  Compared bare,
	// every partitioned table would have been PAX.
	//
	// No invalidation callback on pg_am, for the reason GetRelAmName has
	// none: a relation's relam changes only through pg_class, whose
	// invalidations the metadata cache already takes, and no relation can
	// use an access method that does not exist yet.
	GP_WRAP_START;
	{
		/* catalog tables: pg_am */
		return OidIsValid(am_oid) && get_am_oid(am_name, true) == am_oid;
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsSingleNode(void)
{
	// Not in Cloudberry's wrapper layer, which asks IS_SINGLENODE() in one
	// place (IsParallelModeOK) and otherwise leaves single-node mode to
	// code outside ORCA.  The port's relcache translator needs the answer
	// for every relation -- on one node every relation is reported as
	// coordinator-only, whatever its label records -- so it is asked through
	// a wrapper, as every other question to the server is: the rendezvous
	// lookup behind the macro allocates the first time, and an error there
	// has to become a GPOS exception rather than a longjmp through C++.
	GP_WRAP_START;
	{
		return IS_SINGLENODE();
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::HeapAttIsNull(HeapTuple tup, int attno)
{
	GP_WRAP_START;
	{
		return heap_attisnull(tup, attno, nullptr);
	}
	GP_WRAP_END;
	return false;
}

void
gpdb::FreeHeapTuple(HeapTuple htup)
{
	GP_WRAP_START;
	{
		heap_freetuple(htup);
		return;
	}
	GP_WRAP_END;
}

//---------------------------------------------------------------------------
//	The default distribution opfamily and opclass of a type.
//
//	Cloudberry's cdb_default_distribution_opfamily_for_type() and
//	cdb_default_distribution_opclass_for_type(), from
//	src/backend/cdb/cdbhash.c:366-403.  They were refused here as M2, and
//	are not: ORCA asks for the family of every type it is told about --
//	whether the type could be a distribution key is part of the type's
//	metadata (CTranslatorRelcacheToDXL::RetrieveType) -- so without them no
//	query that names a type could be planned, on one node or on many.  Nor
//	do they need cdbhash: a type can be a distribution key when the type
//	cache finds a hash family, a hash function and an equality operator for
//	it, and then the answer is the hash AM's default.  gp_core runs the same
//	test for DISTRIBUTED BY, as GpPolicyDefaultOpclass() in
//	gp_policy.c.
//---------------------------------------------------------------------------
static bool
TypeIsDistributable(TypeCacheEntry *tcache)
{
	return OidIsValid(tcache->hash_opf) && OidIsValid(tcache->hash_proc) &&
		   OidIsValid(tcache->eq_opr);
}

Oid
gpdb::GetDefaultDistributionOpclassForType(Oid typid)
{
	GP_WRAP_START;
	{
		TypeCacheEntry *tcache =
			lookup_type_cache(typid, TYPECACHE_HASH_OPFAMILY |
										 TYPECACHE_HASH_PROC |
										 TYPECACHE_EQ_OPR);

		if (!TypeIsDistributable(tcache))
			return InvalidOid;

		return GetDefaultOpClass(typid, HASH_AM_OID);
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::GetColumnDefOpclassForType(List *opclassName, Oid typid)
{
	// M2.  Reached when DISTRIBUTED BY names an opclass explicitly.
	GP_UNPORTED("the distribution opclass named in a column definition");
}

Oid
gpdb::GetDefaultDistributionOpfamilyForType(Oid typid)
{
	GP_WRAP_START;
	{
		TypeCacheEntry *tcache =
			lookup_type_cache(typid, TYPECACHE_HASH_OPFAMILY |
										 TYPECACHE_HASH_PROC |
										 TYPECACHE_EQ_OPR);

		if (!TypeIsDistributable(tcache))
			return InvalidOid;

		return tcache->hash_opf;
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::GetDefaultPartitionOpfamilyForType(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type, pg_opclass */
		return default_partition_opfamily_for_type(typid);
	}
	GP_WRAP_END;
	return false;
}

Oid
gpdb::GetHashProcInOpfamily(Oid opfamily, Oid typid)
{
	GP_WRAP_START;
	{
		// Cloudberry's cdb_hashproc_in_opfamily(), which is gp_core's: the
		// support procedure a distribution key is hashed with (gp_hash.c).
		return GpHashProcInOpfamily(opfamily, typid);
	}
	GP_WRAP_END;
	return InvalidOid;
}

Oid
gpdb::IsLegacyCdbHashFunction(Oid funcid)
{
	// None is, for the reason GetCompatibleLegacyHashOpFamily gives: the
	// legacy hash opclasses are Cloudberry built-ins that neither PostgreSQL
	// 19 nor any of the port's modules installs, so no distribution key is
	// hashed with one of their functions.
	(void) funcid;
	return false;
}

Oid
gpdb::GetLegacyCdbHashOpclassForBaseType(Oid typid)
{
	// No type has a legacy hash opclass on this port, for the reason
	// GetCompatibleLegacyHashOpFamily gives: the legacy opclasses are
	// Cloudberry built-ins that neither PostgreSQL 19 nor any of the port's
	// modules installs.  ORCA's relcache translator asks this of every type
	// it describes, so raising here would refuse every type.
	(void) typid;
	return InvalidOid;
}

Oid
gpdb::GetOpclassFamily(Oid opclass)
{
	GP_WRAP_START;
	{
		return get_opclass_family(opclass);
	}
	GP_WRAP_END;
	return false;
}

List *
gpdb::LAppend(List *list, void *datum)
{
	GP_WRAP_START;
	{
		return lappend(list, datum);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::LAppendInt(List *list, int iDatum)
{
	GP_WRAP_START;
	{
		return lappend_int(list, iDatum);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::LAppendOid(List *list, Oid datum)
{
	GP_WRAP_START;
	{
		return lappend_oid(list, datum);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::LPrepend(void *datum, List *list)
{
	GP_WRAP_START;
	{
		return lcons(datum, list);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::LPrependInt(int datum, List *list)
{
	GP_WRAP_START;
	{
		return lcons_int(datum, list);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::LPrependOid(Oid datum, List *list)
{
	GP_WRAP_START;
	{
		return lcons_oid(datum, list);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::ListConcat(List *list1, List *list2)
{
	GP_WRAP_START;
	{
		return list_concat(list1, list2);
	}
	GP_WRAP_END;
	return NIL;
}

List *
gpdb::ListCopy(List *list)
{
	GP_WRAP_START;
	{
		return list_copy(list);
	}
	GP_WRAP_END;
	return NIL;
}

ListCell *
gpdb::ListHead(List *l)
{
	GP_WRAP_START;
	{
		return list_head(l);
	}
	GP_WRAP_END;
	return nullptr;
}

ListCell *
gpdb::ListTail(List *l)
{
	GP_WRAP_START;
	{
		return list_tail(l);
	}
	GP_WRAP_END;
	return nullptr;
}

uint32
gpdb::ListLength(List *l)
{
	GP_WRAP_START;
	{
		return list_length(l);
	}
	GP_WRAP_END;
	return 0;
}

void *
gpdb::ListNth(List *list, int n)
{
	GP_WRAP_START;
	{
		return list_nth(list, n);
	}
	GP_WRAP_END;
	return nullptr;
}

int
gpdb::ListNthInt(List *list, int n)
{
	GP_WRAP_START;
	{
		return list_nth_int(list, n);
	}
	GP_WRAP_END;
	return 0;
}

Oid
gpdb::ListNthOid(List *list, int n)
{
	GP_WRAP_START;
	{
		return list_nth_oid(list, n);
	}
	GP_WRAP_END;
	return 0;
}

bool
gpdb::ListMemberOid(List *list, Oid oid)
{
	GP_WRAP_START;
	{
		return list_member_oid(list, oid);
	}
	GP_WRAP_END;
	return false;
}

void
gpdb::ListFree(List *list)
{
	GP_WRAP_START;
	{
		list_free(list);
		return;
	}
	GP_WRAP_END;
}

void
gpdb::ListFreeDeep(List *list)
{
	GP_WRAP_START;
	{
		list_free_deep(list);
		return;
	}
	GP_WRAP_END;
}

TypeCacheEntry *
gpdb::LookupTypeCache(Oid type_id, int flags)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type, pg_operator, pg_opclass, pg_opfamily, pg_amop */
		return lookup_type_cache(type_id, flags);
	}
	GP_WRAP_END;
	return nullptr;
}

String *
gpdb::MakeStringValue(char *str)
{
	GP_WRAP_START;
	{
		return makeString(str);
	}
	GP_WRAP_END;
	return nullptr;
}

Integer *
gpdb::MakeIntegerValue(long i)
{
	GP_WRAP_START;
	{
		return makeInteger(i);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::MakeIntConst(int32 intValue)
{
	GP_WRAP_START;
	{
		return (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
								  Int32GetDatum(intValue), false, true);
	}
	GP_WRAP_END;
}

Node *
gpdb::MakeBoolConst(bool value, bool isnull)
{
	GP_WRAP_START;
	{
		return makeBoolConst(value, isnull);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::MakeNULLConst(Oid type_oid)
{
	GP_WRAP_START;
	{
		return (Node *) makeNullConst(type_oid, -1 /*consttypmod*/, InvalidOid);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::MakeSegmentFilterExpr(int segid)
{
	// M2.  gp_segment_id is a column a table only has once its rows
	// are on segments.
	GP_UNPORTED("a filter on segment id");
}

TargetEntry *
gpdb::MakeTargetEntry(Expr *expr, AttrNumber resno, char *resname, bool resjunk)
{
	GP_WRAP_START;
	{
		return makeTargetEntry(expr, resno, resname, resjunk);
	}
	GP_WRAP_END;
	return nullptr;
}

Var *
gpdb::MakeVar(Index varno, AttrNumber varattno, Oid vartype, int32 vartypmod,
			  Index varlevelsup)
{
	GP_WRAP_START;
	{
		// GPDB_91_MERGE_FIXME: collation
		Oid collation = TypeCollation(vartype);
		return makeVar(varno, varattno, vartype, vartypmod, collation,
					   varlevelsup);
	}
	GP_WRAP_END;
	return nullptr;
}

// gpdb::MemCtxtAllocZeroAligned is not here.  PostgreSQL 19 removed
// MemoryContextAllocZeroAligned, and its one caller was Palloc0Fast in the
// header, which went with MemSetTest in the same upstream commit.  A wrapper
// named "Aligned" that did not align anything would be worse than its
// absence; MemCtxtAllocZero is what the header uses now.

void *
gpdb::MemCtxtAllocZero(MemoryContext context, Size size)
{
	GP_WRAP_START;
	{
		return MemoryContextAllocZero(context, size);
	}
	GP_WRAP_END;
	return nullptr;
}

void *
gpdb::MemCtxtRealloc(void *pointer, Size size)
{
	GP_WRAP_START;
	{
		return repalloc(pointer, size);
	}
	GP_WRAP_END;
	return nullptr;
}

char *
gpdb::MemCtxtStrdup(MemoryContext context, const char *string)
{
	GP_WRAP_START;
	{
		return MemoryContextStrdup(context, string);
	}
	GP_WRAP_END;
	return nullptr;
}

// Helper function to throw an error with errcode, message and hint, like you
// would with ereport(...) in the backend. This could be extended for other
// fields, but this is all we need at the moment.
void
gpdb::GpdbEreportImpl(int xerrcode, int severitylevel, const char *xerrmsg,
					  const char *xerrhint, const char *filename, int lineno,
					  const char *funcname)
{
	GP_WRAP_START;
	{
		// We cannot use the ereport() macro here, because we want to pass on
		// the caller's filename and line number. This is essentially an
		// expanded version of ereport(). It will be caught by the
		// GP_WRAP_END, and propagated up as a C++ exception, to be
		// re-thrown as a Postgres error once we leave the C++ land.
		if (errstart(severitylevel, TEXTDOMAIN))
		{
			errcode(xerrcode);
			errmsg("%s", xerrmsg);
			if (xerrhint)
			{
				errhint("%s", xerrhint);
			}
			errfinish(filename, lineno, funcname);
		}
	}
	GP_WRAP_END;
}

char *
gpdb::NodeToString(void *obj)
{
	GP_WRAP_START;
	{
		return nodeToString(obj);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::GetTypeDefault(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_typdefault(typid);
	}
	GP_WRAP_END;
	return nullptr;
}


double
gpdb::NumericToDoubleNoOverflow(Numeric num)
{
	GP_WRAP_START;
	{
		return numeric_to_double_no_overflow(num);
	}
	GP_WRAP_END;
	return 0.0;
}

bool
gpdb::NumericIsNan(Numeric num)
{
	GP_WRAP_START;
	{
		return numeric_is_nan(num);
	}
	GP_WRAP_END;
	return false;
}

double
gpdb::ConvertTimeValueToScalar(Datum datum, Oid typid)
{
	bool failure = false;
	GP_WRAP_START;
	{
		return convert_timevalue_to_scalar(datum, typid, &failure);
	}
	GP_WRAP_END;
	return 0.0;
}

double
gpdb::ConvertNetworkToScalar(Datum datum, Oid typid)
{
	bool failure = false;
	GP_WRAP_START;
	{
		return convert_network_to_scalar(datum, typid, &failure);
	}
	GP_WRAP_END;
	return 0.0;
}

bool
gpdb::IsOpHashJoinable(Oid opno, Oid inputtype)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		if (!op_hashjoinable(opno, inputtype))
			return false;

		/*
		 * Even if oprcanhash is true, we need to verify that hash functions
		 * actually exist for this operator. This is because oprcanhash can be
		 * set to true while the operator is only registered in a btree opfamily
		 * and not in a hash opfamily, which would cause execution-time errors
		 * when trying to build hash tables.
		 *
		 * See get_op_hash_functions() in lsyscache.c which requires operators
		 * to be registered in a hash opfamily (amopmethod == HASH_AM_OID).
		 */
		RegProcedure hash_proc;
		if (!get_op_hash_functions(opno, &hash_proc, NULL))
			return false;

		return true;
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsOpMergeJoinable(Oid opno, Oid inputtype)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return op_mergejoinable(opno, inputtype);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsOpStrict(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator, pg_proc */
		return op_strict(opno);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsOpNDVPreserving(Oid opno)
{
	switch (opno)
	{
		// operators are NDV-preserving if the operation does not change the number
		// of NDVs when one argument is a constant.
		// note that we do additional checks later, e.g. col || 'const' is
		// NDV-preserving, while col1 || col2 is not, same with arithmatic
		// operators
		case OIDTextConcatenateOperator:
		case Int4AddOperator:
		case Int8AddOperator:
		case DateIntervalAddOperator:
		case DateInt4AddOperator:
		case DateTimeAddOperator:
		case DateTimetzAddOperator:
		case NumericAddOperator:
		case TimestampIntervalAddOperator:
		case IntervalTimestampAddOperator:
		case Int4DateAddOperator:
			return true;
		default:
			return false;
	}
}

void
gpdb::GetOpInputTypes(Oid opno, Oid *lefttype, Oid *righttype)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_operator */
		op_input_types(opno, lefttype, righttype);
		return;
	}
	GP_WRAP_END;
}

void *
gpdb::GPDBAlloc(Size size)
{
	GP_WRAP_START;
	{
		return palloc(size);
	}
	GP_WRAP_END;
	return nullptr;
}

void
gpdb::GPDBFree(void *ptr)
{
	GP_WRAP_START;
	{
		pfree(ptr);
		return;
	}
	GP_WRAP_END;
}

bool
gpdb::WalkQueryOrExpressionTree(Node *node, bool (*walker)(Node *, void *), void *context,
								int flags)
{
	GP_WRAP_START;
	{
		return query_or_expression_tree_walker(node, walker, context, flags);
	}
	GP_WRAP_END;
	return false;
}

Node *
gpdb::MutateQueryOrExpressionTree(Node *node, Node *(*mutator)(Node *, void *), void *context,
								  int flags)
{
	GP_WRAP_START;
	{
		return query_or_expression_tree_mutator(node, mutator, context, flags);
	}
	GP_WRAP_END;
	return nullptr;
}

Query *
gpdb::MutateQueryTree(Query *query, Node *(*mutator)(Node *, void *), void *context,
					  int flags)
{
	GP_WRAP_START;
	{
		return query_tree_mutator(query, mutator, context, flags);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::WalkQueryTree(Query *query, bool (*walker)(Node *, void *), void *context,
					  int flags)
{
	GP_WRAP_START;
	{
		return query_tree_walker(query, walker, context, flags);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::HasSubclassSlow(Oid rel_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_inherits */
		return has_subclass_slow(rel_oid);
	}
	GP_WRAP_END;
	return false;
}

GpPolicy *
gpdb::GetDistributionPolicy(Relation rel)
{
	GP_WRAP_START;
	{
		// Cloudberry asks rel_is_external_table() first, and builds the
		// policy of an external table by hand from its location list.  The
		// port has no external tables until M5, so that branch is not here;
		// when it arrives it goes above this one.

		// A foreign table's distribution is decided later, as in Cloudberry.
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			return nullptr;
		}

		// Cloudberry reads gp_distribution_policy; the port reads the "gp"
		// security label, which is what replaces that catalog.  Either way
		// this is the answer ORCA's relcache translator asks of every
		// relation -- which is why the policy reader had to exist at M1 and
		// not at M2.  See "ORCA's metadata is not single-node even when the
		// plan is" in cloudberry.md.
		//
		// A NULL policy is ordinary here, and is not in Cloudberry: its DDL
		// guarantees every table has one and a label cannot.
		/* catalog tables: pg_shseclabel */
		return GpPolicyGet(rel->rd_id);
	}
	GP_WRAP_END;
	return nullptr;
}

gpos::BOOL
gpdb::IsChildPartDistributionMismatched(Relation rel)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_class, pg_inherits */
		return child_distribution_mismatch(rel);
	}
	GP_WRAP_END;
	return false;
}

double
gpdb::CdbEstimatePartitionedNumTuples(Relation rel)
{
	GP_WRAP_START;
	{
		return cdb_estimate_partitioned_numtuples(rel);
	}
	GP_WRAP_END;
}

PageEstimate
gpdb::CdbEstimatePartitionedNumPages(Relation rel)
{
	GP_WRAP_START;
	{
		return cdb_estimate_partitioned_numpages(rel);
	}
	GP_WRAP_END;
}

void
gpdb::CloseRelation(Relation rel)
{
	GP_WRAP_START;
	{
		RelationClose(rel);
		return;
	}
	GP_WRAP_END;
}

List *
gpdb::GetRelationIndexes(Relation relation)
{
	GP_WRAP_START;
	{
		if (relation->rd_rel->relhasindex)
		{
			/* catalog tables: from relcache */
			return RelationGetIndexList(relation);
		}
	}
	GP_WRAP_END;
	return NIL;
}

MVNDistinct *
gpdb::GetMVNDistinct(Oid stat_oid)
{
	GP_WRAP_START;
	{
		bool inh = has_subclass(StatisticsGetRelation(stat_oid, false));
		return statext_ndistinct_load(stat_oid, inh);
	}
	GP_WRAP_END;
}

MVDependencies *
gpdb::GetMVDependencies(Oid stat_oid)
{
	GP_WRAP_START;
	{
		bool inh = has_subclass(StatisticsGetRelation(stat_oid, false));
		HeapTuple htup;
		bool isnull;

		// Cloudberry calls a three-argument statext_dependencies_load() whose
		// third argument, allow_null, says "return NULL rather than raising
		// when this statistics object has no dependencies built".
		// PostgreSQL's has two arguments and always raises.
		//
		// The difference is not cosmetic: ORCA asks this of every extended
		// statistics object it finds, and most objects have no dependencies,
		// so "not built" is the ordinary case here and must not be an error.
		// Asking first is the port's allow_null, and it reads the same
		// catalog row that the load would.
		htup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(stat_oid),
							   BoolGetDatum(inh));
		if (!HeapTupleIsValid(htup))
		{
			elog(ERROR, "cache lookup failed for statistics object %u",
				 stat_oid);
		}

		(void) SysCacheGetAttr(STATEXTDATASTXOID, htup,
							   Anum_pg_statistic_ext_data_stxddependencies,
							   &isnull);
		ReleaseSysCache(htup);

		if (isnull)
		{
			return nullptr;
		}

		/* catalog tables: pg_statistic_ext_data */
		return statext_dependencies_load(stat_oid, inh);
	}
	GP_WRAP_END;
}

gpdb::RelationWrapper
gpdb::GetRelation(Oid rel_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: relcache */
		return RelationWrapper{RelationIdGetRelation(rel_oid)};
	}
	GP_WRAP_END;
}

ForeignScan *
gpdb::CreateForeignScan(Oid rel_oid, Index scanrelid, List *qual,
						List *targetlist, Query *query, RangeTblEntry *rte)
{
	// Cloudberry's BuildForeignScan() is in foreign/foreign.c, a file it
	// patched, beside the external-table code that is M5's; the foreign
	// scan is not external tables, and the port's is in compat/foreign.c.
	GP_WRAP_START;
	{
		/* catalog tables: whatever the foreign-data wrapper reads */
		return BuildForeignScan(rel_oid, scanrelid, qual, targetlist, query,
								rte);
	}
	GP_WRAP_END;
	return nullptr;
}

TargetEntry *
gpdb::FindFirstMatchingMemberInTargetList(Node *node, List *targetlist)
{
	GP_WRAP_START;
	{
		return tlist_member((Expr *) node, targetlist);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::FindMatchingMembersInTargetList(Node *node, List *targetlist)
{
	GP_WRAP_START;
	{
		return tlist_members(node, targetlist);
	}
	GP_WRAP_END;

	return NIL;
}

bool
gpdb::Equals(void *p1, void *p2)
{
	GP_WRAP_START;
	{
		return equal(p1, p2);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsCompositeType(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		return type_is_rowtype(typid);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IsTextRelatedType(Oid typid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		char typcategory;
		bool typispreferred;
		get_type_category_preferred(typid, &typcategory, &typispreferred);

		return typcategory == TYPCATEGORY_STRING;
	}
	GP_WRAP_END;
	return false;
}

StringInfo
gpdb::MakeStringInfo(void)
{
	GP_WRAP_START;
	{
		return makeStringInfo();
	}
	GP_WRAP_END;
	return nullptr;
}

void
gpdb::AppendStringInfo(StringInfo str, const char *str1, const char *str2)
{
	GP_WRAP_START;
	{
		appendStringInfo(str, "%s%s", str1, str2);
		return;
	}
	GP_WRAP_END;
}

int
gpdb::FindNodes(Node *node, List *nodeTags)
{
	GP_WRAP_START;
	{
		return find_nodes(node, nodeTags);
	}
	GP_WRAP_END;
	return -1;
}

int
gpdb::CheckCollation(Node *node)
{
	GP_WRAP_START;
	{
		return check_collation(node);
	}
	GP_WRAP_END;
	return -1;
}

bool
gpdb::HasOrderByOrderingOp(Query *query)
{
	GP_WRAP_START;
	{
		return has_orderby_ordering_op(query);
	}
	GP_WRAP_END;
	return false;
}

Node *
gpdb::CoerceToCommonType(ParseState *pstate, Node *node, Oid target_type,
						 const char *context)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type, pg_cast */
		return coerce_to_common_type(pstate, node, target_type, context);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::ResolvePolymorphicArgType(int numargs, Oid *argtypes, char *argmodes,
								FuncExpr *call_expr)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return resolve_polymorphic_argtypes(numargs, argtypes, argmodes,
											(Node *) call_expr);
	}
	GP_WRAP_END;
	return false;
}

// hash a list of const values with GPDB's hash function
int32
gpdb::CdbHashConstList(List *constants, int num_segments, Oid *hashfuncs)
{
	// M2: the cluster has one node until then, so nothing is distributed
	// and nothing hashes a distribution key.
	GP_UNPORTED("hashing a list of constants to a segment");
}

unsigned int
gpdb::CdbHashRandomSeg(int num_segments)
{
	// M2: the cluster has one node until then, so nothing is distributed
	// and nothing hashes a distribution key.
	GP_UNPORTED("choosing a segment at random");
}

// check permissions on range table
//
// Only the permission entries the range table points at, each under a copy
// of its entry renumbered to match, as the planner copies them into a plan
// (setrefs.c, add_rte_to_flat_rtable).  PostgreSQL 19's
// ExecCheckPermissions asserts that every entry in the list is pointed at,
// which a plan's list always is and a Query's need not be: gp_matview's delta
// queries put a subquery in a table's place and leave the table's entry,
// which no planner ever checks.  Handed the Query's lists as they were, an
// assert-enabled server stopped at the first incremental view maintained
// under ORCA.  Cloudberry had removed the assertion.
void
gpdb::CheckRTPermissions(List *rtable, List *rteperminfos)
{
	GP_WRAP_START;
	{
		List *rtes = NIL;
		List *perminfos = NIL;
		ListCell *lc;

		foreach (lc, rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
			RangeTblEntry *copy;

			if (0 == rte->perminfoindex)
				continue;

			perminfos =
				lappend(perminfos, getRTEPermissionInfo(rteperminfos, rte));
			copy = (RangeTblEntry *) palloc(sizeof(RangeTblEntry));
			memcpy(copy, rte, sizeof(RangeTblEntry));
			copy->perminfoindex = list_length(perminfos);
			rtes = lappend(rtes, copy);
		}

		ExecCheckPermissions(rtes, perminfos, true);
		return;
	}
	GP_WRAP_END;
}


// check that a table doesn't have UPDATE triggers.
bool
gpdb::HasUpdateTriggers(Oid relid)
{
	GP_WRAP_START;
	{
		return has_update_triggers(relid, true);
	}
	GP_WRAP_END;
	return false;
}

// get index op family properties
void
gpdb::IndexOpProperties(Oid opno, Oid opfamily, StrategyNumber *strategynumber,
						Oid *righttype)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */

		// Only the right type is returned to the caller, the left
		// type is simply ignored.
		Oid lefttype;
		INT strategy;

		get_op_opfamily_properties(opno, opfamily, false, &strategy, &lefttype,
								   righttype);

		// Ensure the value of strategy doesn't get truncated when converted to StrategyNumber
		GPOS_ASSERT(strategy >= 0 &&
					strategy <= std::numeric_limits<StrategyNumber>::max());
		*strategynumber = static_cast<StrategyNumber>(strategy);
		return;
	}
	GP_WRAP_END;
}

// check whether index column is returnable (for index-only scans)
gpos::BOOL
gpdb::IndexCanReturn(Relation index, int attno)
{
	GP_WRAP_START;
	{
		return index_can_return(index, attno);
	}
	GP_WRAP_END;
}

// get oids of opfamilies for the index keys
List *
gpdb::GetIndexOpFamilies(Oid index_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_index */

		// We return the operator families of the index keys.
		return get_index_opfamilies(index_oid);
	}
	GP_WRAP_END;

	return NIL;
}

// get oids of families this operator belongs to
List *
gpdb::GetOpFamiliesForScOp(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */

		// We return the operator families this operator
		// belongs to.
		return get_operator_opfamilies(opno);
	}
	GP_WRAP_END;

	return NIL;
}

// get the OID of hash equality operator(s) compatible with the given op
Oid
gpdb::GetCompatibleHashOpFamily(Oid opno)
{
	// Not M2 after all.  This comment used to say it was the half of the
	// group that really needs cdbhash; Cloudberry's body is a pg_amop search
	// and never touches cdbhash, and ORCA's relcache translator asks it of
	// every operator it describes -- so deferring it made ORCA refuse every
	// operator on one node, which the first run of the metadata probe showed.
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_compatible_hash_opfamily(opno);
	}
	GP_WRAP_END;
	return InvalidOid;
}

// get the OID of hash equality operator(s) compatible with the given op
Oid
gpdb::GetCompatibleLegacyHashOpFamily(Oid opno)
{
	// The legacy scheme is the pre-Greenplum-6 hash, kept so that an
	// upgraded cluster does not have to redistribute every table.  Cloudberry
	// answers by finding the operator's hash families and keeping the one
	// whose hash function is one of its cdblegacyhash_* built-ins.
	//
	// The port has none of those: they are Cloudberry's pg_proc entries,
	// with fixed OIDs, and neither PostgreSQL 19's catalog nor any of the
	// port's modules has them.  So no operator belongs to a legacy family,
	// and InvalidOid is the true answer rather than a stand-in for one.
	// ORCA asks it of every operator it describes, and uses it only under
	// EopttraceUseLegacyOpfamilies, which COptTasks sets for a query over
	// tables distributed with legacy opclasses -- none, on this port.  If a
	// module ever installs the legacy opclasses, this has to find them.
	(void) opno;
	return InvalidOid;
}

List *
gpdb::GetMergeJoinOpFamilies(Oid opno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop */

		return get_mergejoin_opfamilies(opno);
	}
	GP_WRAP_END;
	return NIL;
}

bool
gpdb::GetOpHashFunctions(Oid opno, Oid *lhs_procno, Oid *rhs_procno)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_amop, pg_amproc */
		return get_op_hash_functions(opno, lhs_procno, rhs_procno);
	}
	GP_WRAP_END;
	return false;
}

bool
gpdb::IndexUsableBySnapshots(Oid index_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_index */
		HeapTuple tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
		if (!HeapTupleIsValid(tup))
		{
			elog(ERROR, "cache lookup failed for index %u", index_oid);
		}

		Form_pg_index index = (Form_pg_index) GETSTRUCT(tup);
		bool usable =
			!(index->indcheckxmin &&
			  !TransactionIdPrecedes(HeapTupleHeaderGetXmin(tup->t_data),
									 TransactionXmin));
		ReleaseSysCache(tup);

		return usable;
	}
	GP_WRAP_END;
	return false;
}

Node *
gpdb::CoerceNullToDomain(Oid typid, int32 typmod)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_type */
		int16 typlen;
		bool typbyval;

		get_typlenbyval(typid, &typlen, &typbyval);
		return coerce_null_to_domain(typid, typmod, get_typcollation(typid),
									 typlen, typbyval);
	}
	GP_WRAP_END;
	return nullptr;
}

RangeTblEntry *
gpdb::PartitionRTE(const RangeTblEntry *root_rte, Oid part_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_class, pg_attribute */
		return gp_orca_partition_rte(root_rte, part_oid);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::PlanForPartition(Plan *scan, Index root_rti, Index part_rti,
					   Oid root_oid, Oid part_oid, int *failure)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_class, pg_attribute, pg_index, pg_inherits */
		return gp_orca_plan_for_partition(scan, root_rti, part_rti, root_oid,
										  part_oid, failure);
	}
	GP_WRAP_END;
	return nullptr;
}

List *
gpdb::DynamicScanTlist(Plan *scan)
{
	GP_WRAP_START;
	{
		return gp_orca_dynamic_scan_tlist(scan);
	}
	GP_WRAP_END;
	return NIL;
}

/*
 * gp_core's Motion, through its API: 1.4 and later.  An older gp_core has
 * none, and ORCA's plans with a Motion go to the planner.
 */
static const GpCoreApi *
motion_api(void)
{
	const GpCoreApi *api = cb_core_api();

	if (api == nullptr || api->version_major != GP_CORE_API_VERSION_MAJOR ||
		api->version_minor < 4)
		return nullptr;
	return api;
}

bool
gpdb::CanDispatchPlans(void)
{
	GP_WRAP_START;
	{
		const GpCoreApi *api = motion_api();

		return api != nullptr && api->motion_can_dispatch();
	}
	GP_WRAP_END;
	return false;
}

Plan *
gpdb::MakeGatherMotion(Plan *fragment, List *targetlist, List *qual,
					   int content, int slice, int nkeys,
					   const AttrNumber *keys, const Oid *sortops,
					   const Oid *collations, const bool *nullsfirst)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_make_gather(fragment, targetlist, qual,
												content, slice, nkeys, keys,
												sortops, collations,
												nullsfirst);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::MakeSendMotion(int type, Plan *fragment, List *targetlist, List *qual,
					 int content, int slice, List *hashexprs, List *hashfuncs)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_make_send(type, fragment, targetlist, qual,
											  content, slice, hashexprs,
											  hashfuncs);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::MakeHashFilter(Plan *child, List *targetlist, List *qual, int nkeys,
					 const AttrNumber *cols, const Oid *hashfuncs,
					 int segment)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_make_hash_filter(child, targetlist, qual,
													 nkeys, cols, hashfuncs,
													 segment);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::MakeDmlMotion(Plan *modify, int content, int slice)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_make_dml(modify, content, slice);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::MakeSplit(Plan *child, List *targetlist, List *deletecols,
				List *insertcols, AttrNumber actioncol)
{
	GP_WRAP_START;
	{
		return motion_api()->split_make(child, targetlist, deletecols,
										insertcols, actioncol);
	}
	GP_WRAP_END;
	return nullptr;
}

Plan *
gpdb::MakeSplitModify(Plan *child, Index rti, int natts, AttrNumber actioncol,
					  AttrNumber ctidcol)
{
	GP_WRAP_START;
	{
		return motion_api()->split_modify_make(child, rti, natts, actioncol,
											   ctidcol);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::HasAnyTriggers(Oid relid)
{
	GP_WRAP_START;
	{
		/* the statement's parser has the table locked */
		Relation	rel = RelationIdGetRelation(relid);
		bool		has = rel->rd_rel->relhastriggers;

		RelationClose(rel);
		return has;
	}
	GP_WRAP_END;
	return true;
}

int
gpdb::MotionType(Plan *motion)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_type(motion);
	}
	GP_WRAP_END;
	return -1;
}

int
gpdb::MotionSegment(Plan *motion)
{
	GP_WRAP_START;
	{
		return motion_api()->motion_segment(motion);
	}
	GP_WRAP_END;
	return -1;
}

void
gpdb::SetMotionSegment(Plan *motion, int content)
{
	GP_WRAP_START;
	{
		motion_api()->motion_set_segment(motion, content);
		return;
	}
	GP_WRAP_END;
}

int
gpdb::DirectDispatchSegment(Oid relid, int nvalues, const Oid *types,
							const Datum *values, const bool *isnull)
{
	GP_WRAP_START;
	{
		const GpCoreApi *api = motion_api();

		if (api == nullptr)
			return -1;
		return api->direct_dispatch_segment(relid, nvalues, types, values,
											isnull);
	}
	GP_WRAP_END;
	return -1;
}

int
gpdb::CheckMotions(PlannedStmt *stmt)
{
	GP_WRAP_START;
	{
		return gp_orca_check_motions(stmt);
	}
	GP_WRAP_END;
	return GP_ORCA_MOTION_OK;
}

FuncExpr *
gpdb::NextValueCall(const NextValueExpr *next_value)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_namespace, pg_proc */
		return gp_orca_next_value_call(next_value);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::IsNextValueFunc(Oid funcid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc, pg_namespace */
		return gp_orca_is_next_value_func(funcid);
	}
	GP_WRAP_END;
	return false;
}

NextValueExpr *
gpdb::NextValueFromCall(const FuncExpr *call)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_namespace, pg_proc */
		return gp_orca_next_value_from_call(call);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::IsPostgisIndexSupport(Oid supportfn)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return GpOrcaIsPostgisIndexSupport(supportfn);
	}
	GP_WRAP_END;
	return false;
}

int
gpdb::TopPartitionIndex(Oid root_oid, Oid leaf_oid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_class, pg_inherits */
		return gp_orca_top_partition_index(root_oid, leaf_oid);
	}
	GP_WRAP_END;
	return -1;
}

char
gpdb::GetAttGenerated(Oid relid, AttrNumber attnum)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_attribute */
		return get_attgenerated(relid, attnum);
	}
	GP_WRAP_END;
	return '\0';
}


// get the OID of base elementtype for a given typid
// eg.: CREATE DOMAIN text_domain as text;
// SELECT oid, typbasetype from pg_type where typname = 'text_domain';
// oid         | XXXXX  --> Oid for text_domain
// typbasetype | 25     --> Oid for base element ie, TEXT
Oid
gpdb::GetBaseType(Oid typid)
{
	GP_WRAP_START;
	{
		return getBaseType(typid);
	}
	GP_WRAP_END;
	return InvalidOid;
}

// Evaluates 'expr' and returns the result as an Expr.
// Caller keeps ownership of 'expr' and takes ownership of the result
Expr *
gpdb::EvaluateExpr(Expr *expr, Oid result_type, int32 typmod)
{
	GP_WRAP_START;
	{
		// GPDB_91_MERGE_FIXME: collation
		return evaluate_expr(expr, result_type, typmod, InvalidOid);
	}
	GP_WRAP_END;
	return nullptr;
}

char *
gpdb::DefGetString(DefElem *defelem)
{
	GP_WRAP_START;
	{
		return defGetString(defelem);
	}
	GP_WRAP_END;
	return nullptr;
}

Expr *
gpdb::TransformArrayConstToArrayExpr(Const *c)
{
	GP_WRAP_START;
	{
		return transform_array_Const_to_ArrayExpr(c);
	}
	GP_WRAP_END;
	return nullptr;
}

Node *
gpdb::EvalConstExpressions(Node *node)
{
	GP_WRAP_START;
	{
		return eval_const_expressions(nullptr, node);
	}
	GP_WRAP_END;
	return nullptr;
}

FaultInjectorType_e
gpdb::InjectFaultInOptTasks(const char *fault_name)
{
	GP_WRAP_START;
	{
		return FaultInjector_InjectFaultIfSet(fault_name, DDLNotSpecified, "",
											  "");
	}
	GP_WRAP_END;
	return FaultInjectorTypeNotSpecified;
}

/*
 * To detect changes to catalog tables that require resetting the Metadata
 * Cache, we use the normal PostgreSQL catalog cache invalidation mechanism.
 * We register a callback to a cache on all the catalog tables that contain
 * information that's contained in the ORCA metadata cache.

 * There is no fine-grained mechanism in the metadata cache for invalidating
 * individual entries ATM, so we just blow the whole cache whenever anything
 * changes. The callback simply increments a counter. Whenever we start
 * planning a query, we check the counter to see if it has changed since the
 * last planned query, and reset the whole cache if it has.
 *
 * To make sure we've covered all catalog tables that contain information
 * that's stored in the metadata cache, there are "catalog tables: xxx"
 * comments in all the calls to backend functions in this file. They indicate
 * which catalog tables each function uses. We conservatively assume that
 * anything fetched via the wrapper functions in this file can end up in the
 * metadata cache and hence need to have an invalidation callback registered.
 */
static bool mdcache_invalidation_counter_registered = false;
static int64 mdcache_invalidation_counter = 0;
static int64 last_mdcache_invalidation_counter = 0;

static void
mdsyscache_invalidation_counter_callback(Datum /*arg*/,
										 SysCacheIdentifier /*cacheid*/,
										 uint32 /*hashvalue*/)
{
	mdcache_invalidation_counter++;
}

static void
mdrelcache_invalidation_counter_callback(Datum /*arg*/, Oid /*relid*/)
{
	mdcache_invalidation_counter++;
}

static void
register_mdcache_invalidation_callbacks(void)
{
	/* These are all the catalog tables that we care about. */
	// PostgreSQL 19 types both this array and the callback's second
	// parameter as SysCacheIdentifier rather than int.  In C that is the same
	// argument; in C++ it is a different one, and the enum is what the
	// function wants.
	SysCacheIdentifier metadata_caches[] = {
		AGGFNOID,		  /* pg_aggregate */
		AMOPOPID,		  /* pg_amop */
		CASTSOURCETARGET, /* pg_cast */
		CONSTROID,		  /* pg_constraint */
		OPEROID,		  /* pg_operator */
		OPFAMILYOID,	  /* pg_opfamily */
		STATRELATTINH,	  /* pg_statistics */
		TYPEOID,		  /* pg_type */
		PROCOID,		  /* pg_proc */

		/*
		 * lookup_type_cache() will also access pg_opclass, via GetDefaultOpClass(),
		 * but there is no syscache for it. Postgres doesn't seem to worry about
		 * invalidating the type cache on updates to pg_opclass, so we don't
		 * worry about that either.
		 */
		/* pg_opclass */

		/*
		 * Information from the following catalogs are included in the
		 * relcache, and any updates will generate relcache invalidation
		 * event. We'll catch the relcache invalidation event and don't need
		 * to register a catcache callback for them.
		 */
		/* pg_class */
		/* pg_index */

		/*
		 * pg_foreign_table is updated when a new external table is dropped/created,
		 * which will trigger a relcache invalidation event.
		 */
		/* pg_foreign_table */

		/*
		 * XXX: no syscache on pg_inherits. Is that OK? For any partitioning
		 * changes, I think there will also be updates on pg_partition and/or
		 * pg_partition_rules.
		 */
		/* pg_inherits */

		/*
		 * We assume that gp_segment_config will not change on the fly in a way that
		 * would affect ORCA
		 */
		/* gp_segment_config */
	};
	unsigned int i;

	for (i = 0; i < lengthof(metadata_caches); i++)
	{
		CacheRegisterSyscacheCallback(metadata_caches[i],
									  &mdsyscache_invalidation_counter_callback,
									  (Datum) 0);
	}

	/* also register the relcache callback */
	CacheRegisterRelcacheCallback(&mdrelcache_invalidation_counter_callback,
								  (Datum) 0);
}

// Has there been any catalog changes since last call?
bool
gpdb::MDCacheNeedsReset(void)
{
	GP_WRAP_START;
	{
		if (!mdcache_invalidation_counter_registered)
		{
			register_mdcache_invalidation_callbacks();
			mdcache_invalidation_counter_registered = true;
		}
		if (last_mdcache_invalidation_counter == mdcache_invalidation_counter)
		{
			return false;
		}
		else
		{
			last_mdcache_invalidation_counter = mdcache_invalidation_counter;
			return true;
		}
	}
	GP_WRAP_END;

	return true;
}

// returns true if a query cancel is requested in GPDB
bool
gpdb::IsAbortRequested(void)
{
	// No GP_WRAP_START/END needed here. We just check these global flags,
	// it cannot throw an ereport().
	return (QueryCancelPending || ProcDiePending);
}

// Given the type OID, get the typelem (InvalidOid if not an array type).
Oid
gpdb::GetElementType(Oid array_type_oid)
{
	GP_WRAP_START;
	{
		return get_element_type(array_type_oid);
	}
	GP_WRAP_END;
}

GpPolicy *
gpdb::MakeGpPolicy(GpPolicyType ptype, int nattrs, int numsegments)
{
	// M2.  gp_core builds policies when it reads a "gp" label
	// (make_policy() in gp_policy.c), but ORCA wants to build one of
	// its own for a CTAS target, which is a plan for distributing
	// rows and so needs the dispatch half.
	GP_UNPORTED("building a distribution policy");
}

uint32
gpdb::HashChar(Datum d)
{
	GP_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(hashchar, d));
	}
	GP_WRAP_END;
}

uint32
gpdb::HashBpChar(Datum d)
{
	GP_WRAP_START;
	{
		return DatumGetUInt32(
			DirectFunctionCall1Coll(hashbpchar, C_COLLATION_OID, d));
	}
	GP_WRAP_END;
}

uint32
gpdb::HashText(Datum d)
{
	GP_WRAP_START;
	{
		return DatumGetUInt32(
			DirectFunctionCall1Coll(hashtext, C_COLLATION_OID, d));
	}
	GP_WRAP_END;
}

uint32
gpdb::HashName(Datum d)
{
	GP_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(hashname, d));
	}
	GP_WRAP_END;
}

uint32
gpdb::UUIDHash(Datum d)
{
	GP_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(uuid_hash, d));
	}
	GP_WRAP_END;
}

void *
gpdb::GPDBMemoryContextAlloc(MemoryContext context, Size size)
{
	GP_WRAP_START;
	{
		return MemoryContextAlloc(context, size);
	}
	GP_WRAP_END;
	return nullptr;
}

void
gpdb::GPDBMemoryContextDelete(MemoryContext context)
{
	GP_WRAP_START;
	{
		MemoryContextDelete(context);
	}
	GP_WRAP_END;
}

// ORCA's top-level memory context; see the body below.
static MemoryContext optimizer_memory_context = nullptr;

MemoryContext
gpdb::GPDBAllocSetContextCreate()
{
	GP_WRAP_START;
	{
		MemoryContext cxt;

		// Cloudberry keeps a backend-wide OptimizerMemoryContext, declared in
		// utils/memutils.h and created under TopMemoryContext the first time
		// planner.c reaches ORCA.  The port cannot add a context to
		// PostgreSQL's, and does not need to: nothing outside ORCA looks at
		// it and it is asked for in exactly one place, so it is created here
		// instead -- lazily, under TopMemoryContext, which is where
		// Cloudberry's is too.
		if (optimizer_memory_context == nullptr)
		{
			optimizer_memory_context =
				AllocSetContextCreate(TopMemoryContext, "GPORCA",
									  ALLOCSET_DEFAULT_SIZES);
		}

		cxt =
			AllocSetContextCreate(optimizer_memory_context, "GPORCA memory pool",
								  ALLOCSET_DEFAULT_SIZES);

		// Cloudberry calls MemoryContextDeclareAccountingRoot() here, so
		// that MemoryContextGetCurrentSpace() can later report what ORCA has
		// used.  Both are Cloudberry's memory accounting and PostgreSQL 19
		// has neither; its nearest is MemoryContextMemAllocated(cxt, true),
		// which walks the children on each call rather than keeping a
		// running total, and which needs no declaration to work.  So the
		// declaration goes and the context is an ordinary one.
		return cxt;
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::ExpressionReturnsSet(Node *clause)
{
	GP_WRAP_START;
	{
		return expression_returns_set(clause);
	}
	GP_WRAP_END;
}

bool
gpdb::ContainsVolatileFunctions(Node *node)
{
	GP_WRAP_START;
	{
		return contain_volatile_functions(node);
	}
	GP_WRAP_END;
	return true;
}

// Not in Cloudberry's layer: whether an expression reads a column, which a
// window frame offset may not in PostgreSQL 19; see TranslateDXLWindow.
bool
gpdb::ContainsVars(Node *node)
{
	GP_WRAP_START;
	{
		return contain_var_clause(node);
	}
	GP_WRAP_END;
	return true;
}

List *
gpdb::GetRelChildIndexes(Oid reloid)
{
	List *partoids = NIL;
	GP_WRAP_START;
	{
		if (InvalidOid == reloid)
		{
			return NIL;
		}
		partoids = find_inheritance_children(reloid, NoLock);
	}
	GP_WRAP_END;

	return partoids;
}

Oid
gpdb::GetForeignServerId(Oid reloid)
{
	GP_WRAP_START;
	{
		return GetForeignServerIdByRelId(reloid);
	}
	GP_WRAP_END;
	return 0;
}

int16
gpdb::GetAppendOnlySegmentFilesCount(Relation rel)
{
	// M5, with gp_ao.  Cloudberry reads pg_appendonly, which the port
	// replaces; until then no relation here is append-only.
	GP_UNPORTED("the segment file count of an append-only table");
}

// Locks on partition leafs and indexes are held during optimizer (after
// parse-analyze stage). ORCA need this function to lock relation. Here
// we do not need to consider lock-upgrade issue, reasons are:
//   1. Only UPDATE|DELETE statement may upgrade lock level
//   2. ORCA currently does not support DML on partition tables
//   3. If not partition table, then parser should have already locked
//   4. Even later ORCA support DML on partition tables, the lock mode
//      of leafs should be the same as the mode in root's RTE's rellockmode
//   5. Index does not have lock-upgrade problem.
void
gpdb::GPDBLockRelationOid(Oid reloid, LOCKMODE lockmode)
{
	GP_WRAP_START;
	{
		LockRelationOid(reloid, lockmode);
	}
	GP_WRAP_END;
}

char *
gpdb::GetRelFdwName(Oid reloid)
{
	GP_WRAP_START;
	{
		Oid fs_id = GetForeignServerIdByRelId(reloid);
		ForeignServer *fs = GetForeignServer(fs_id);
		ForeignDataWrapper *fdw = GetForeignDataWrapper(fs->fdwid);
		return fdw->fdwname;
	}
	GP_WRAP_END;
	return nullptr;
}

PathTarget *
gpdb::MakePathtargetFromTlist(List *tlist)
{
	GP_WRAP_START;
	{
		return make_pathtarget_from_tlist(tlist);
	}
	GP_WRAP_END;
}

void
gpdb::SplitPathtargetAtSrfs(PlannerInfo *root, PathTarget *target,
							PathTarget *input_target, List **targets,
							List **targets_contain_srfs)
{
	// Cloudberry changed split_pathtarget_at_srfs to accept a null root --
	// it skips set_pathtarget_cost_width then -- and its one caller, in
	// CTranslatorDXLToPlStmt, passes exactly that.  PostgreSQL 19's
	// dereferences root, so the compat layer carries PostgreSQL 19's copy
	// with Cloudberry's guard, under a name of its own (compat/tlist.c).
	GP_WRAP_START;
	{
		cb_split_pathtarget_at_srfs(root, target, input_target, targets,
									targets_contain_srfs);
	}
	GP_WRAP_END;
}

List *
gpdb::MakeTlistFromPathtarget(PathTarget *target)
{
	GP_WRAP_START;
	{
		return make_tlist_from_pathtarget(target);
	}
	GP_WRAP_END;
	return NIL;
}

Node *
gpdb::Expression_tree_mutator(Node *node, Node *(*mutator)(Node*, void*), void *context)
{
	GP_WRAP_START;
	{
		return expression_tree_mutator(node, mutator, context);
	}
	GP_WRAP_END;

	return nullptr;
}

TargetEntry *
gpdb::TlistMember(Expr *node, List *targetlist)
{
	GP_WRAP_START;
	{
		return tlist_member(node, targetlist);
	}
	GP_WRAP_END;

	return nullptr;
}

Var *
gpdb::MakeVarFromTargetEntry(Index varno, TargetEntry *tle)
{
	GP_WRAP_START;
	{
		return makeVarFromTargetEntry(varno, tle);
	}
	GP_WRAP_END;
}

TargetEntry *
gpdb::FlatCopyTargetEntry(TargetEntry *src_tle)
{
	GP_WRAP_START;
	{
		return flatCopyTargetEntry(src_tle);
	}
	GP_WRAP_END;
}


// Returns true if type is a RANGE
// pg_type (typtype = 'r')
bool
gpdb::IsTypeRange(Oid typid)
{
	GP_WRAP_START;
	{
		return type_is_range(typid);
	}
	GP_WRAP_END;
	return false;
}

// The name of the access method a relation is stored with.
//
// NOT a rename.  Cloudberry calls GetAmName(reloid), and GetAmName takes an
// *access method* OID: it looks the argument up in AMOID and reports "cache
// lookup failed for relam object" when it misses.  Passing a relation OID to
// it finds whatever access method happens to share that OID, or nothing.
// The relation's own access method is in pg_class.relam and has to be read
// first.  Cloudberry's parameter is even named `reloid`.
//
// Its other defect is not carried over either: GetAmName releases the
// syscache tuple and then returns a pointer into it.  That is the third
// function of this shape the port has met, after get_cast_func and
// GetExtStatisticsName.
char *
gpdb::GetRelAmName(Oid reloid)
{
	GP_WRAP_START;
	{
		/* catalog tables: pg_class, pg_am */
		Oid amoid = get_rel_relam(reloid);

		if (!OidIsValid(amoid))
		{
			return nullptr;
		}

		return get_am_name(amoid);
	}
	GP_WRAP_END;
	return nullptr;
}

// Get IndexAmRoutine struct for the given access method handler.
//
// const, where Cloudberry's is not: PostgreSQL 19 returns a
// `const IndexAmRoutine *`, and the translator only reads it.  Casting the
// const away to keep Cloudberry's signature would be a lie about a structure
// the access method owns.
const IndexAmRoutine *
gpdb::GetIndexAmRoutineFromAmHandler(Oid am_handler)
{
	GP_WRAP_START;
	{
		return GetIndexAmRoutine(am_handler);
	}
	GP_WRAP_END;
	return nullptr;
}

bool
gpdb::TestexprIsHashable(Node *testexpr, List *param_ids)
{
	GP_WRAP_START;
	{
		return testexpr_is_hashable(testexpr, param_ids);
	}
	GP_WRAP_END;
	return false;
}

RTEPermissionInfo *
gpdb::GetRTEPermissionInfo(List *rteperminfos, const RangeTblEntry *rte)
{
	GP_WRAP_START;
	{
		// Cast away const: upstream getRTEPermissionInfo() only reads
		// rte->perminfoindex and rte->relid but its signature lacks const.
		return getRTEPermissionInfo(rteperminfos, (RangeTblEntry *) rte);
	}
	GP_WRAP_END;
}

//---------------------------------------------------------------------------
//	May ORCA consider an intra-segment parallel plan?
//
//	Cloudberry answers from enable_parallel, single-node mode and
//	max_parallel_workers_per_gather.  The port answers no: decision 2 defers
//	intra-segment parallelism until after M7, and says the first variant
//	tried then is a PostgreSQL Gather inside each segment process, which
//	needs no answer here at all.
//
//	Cloudberry's parallel xforms are compiled in regardless -- they come with
//	the core, which the port takes whole -- so this is the switch that keeps
//	them from firing.  It is also the one wrapper in this file that ORCA's
//	core calls: CXformGet2TableScan and CXformGet2ParallelTableScan, and
//	nothing else in 920 source files.  When the decision is revisited, this
//	is the one place that changes.
//---------------------------------------------------------------------------
bool
gpdb::IsParallelModeOK(void)
{
	return false;
}

// EOF
