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
 * gp_seq.c
 *	  A sequence's next value, taken in a slice the segments run.
 *
 * A sequence is the coordinator's: DDL gives each segment a copy, and none
 * of the copies is the one the statement's values come from.  Cloudberry's
 * segments ask the coordinator for the next value of a sequence in a slice
 * they run -- nextval() there is sent to the coordinator's session over its
 * connection to the segment, and answered from the sequence there, a cached
 * block at a time (cdb_sequence_nextval_qe(), sequence.c).  PostgreSQL 19's
 * protocol has no way for a segment to ask the coordinator's session in the
 * middle of a query, so a segment here asks the coordinator's server: over
 * a connection of its own to the coordinator, which carries the cluster
 * secret, gp_internal.sequence_values() takes the next values there, as
 * many as the sequence's CACHE says, checking the privilege nextval() asks
 * of the segment's current user, in one round trip; and the segment hands
 * them out one by one, as PostgreSQL's cache hands out a block a session
 * took.  ORCA's plan calls
 * gp_internal.nextval() where it has nextval() in a slice the segments run,
 * and gp_internal.identity_nextval() where it has an identity column's next
 * value (gp_orca's compat/motion.c).
 *
 * Because the values are taken by another backend, of another transaction,
 * the translator leaves to the planner a sequence that backend could not
 * take them from: one this transaction made, or reset, or holds a lock on
 * that nextval() would wait for, and a temporary one, which is this
 * session's alone.  currval(), lastval() and setval() in a slice the
 * segments run stay the planner's too: Cloudberry refuses them on a
 * cluster.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_loopback.h"
#include "gp_seq.h"

/* The values this segment process has taken of one sequence, not yet given. */
typedef struct SeqValues
{
	Oid			relid;			/* hash key */
	int64	   *values;			/* in TopMemoryContext */
	int			count;
	int			next;
} SeqValues;

static HTAB *seq_values = NULL;

/* Whether the fragment running now is part of a read-only transaction. */
static bool fragment_read_only = false;

void
GpSeqSetReadOnly(bool read_only)
{
	fragment_read_only = read_only;
}

/* How many values of the sequence one asking takes: its CACHE, as a session's. */
static int
seq_block(Oid relid)
{
	HeapTuple	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	int64		cache = 1;

	if (HeapTupleIsValid(tuple))
	{
		cache = ((Form_pg_sequence) GETSTRUCT(tuple))->seqcache;
		ReleaseSysCache(tuple);
	}
	return (int) Max(1, Min(cache, 1000));
}

/*
 * The next value of sequence "relid" for a slice this segment runs: from the
 * values taken from the coordinator, or, where none are left, from a new
 * block of them.  "identity" is an identity column's, whose sequence the
 * statement's INSERT may take a value of without a privilege on it.
 */
static int64
segment_nextval(Oid relid, bool identity)
{
	SeqValues  *entry;
	bool		found;

	if (seq_values == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(SeqValues);
		seq_values = hash_create("gp_core sequence values", 16, &ctl,
								 HASH_ELEM | HASH_BLOBS);
	}
	entry = hash_search(seq_values, &relid, HASH_ENTER, &found);
	if (!found)
	{
		entry->values = NULL;
		entry->count = 0;
		entry->next = 0;
	}

	if (entry->next >= entry->count)
	{
		const char *dbname = get_database_name(MyDatabaseId);
		int			block = seq_block(relid);
		char	   *text;
		ArrayType  *array;
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		/* PostgreSQL's words, as nextval() says them in such a transaction */
		if (fragment_read_only)
			ereport(ERROR,
					(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
					 errmsg("cannot execute %s in a read-only transaction",
							"nextval()")));

		text = GpLoopbackCoordinatorValue(dbname,
										  psprintf("SELECT gp_internal.sequence_values(%u, %d, %s, %u)",
												   relid, block,
												   identity ? "true" : "false",
												   GetUserId()));
		if (text == NULL)
			elog(ERROR, "the coordinator gave no values of sequence %u", relid);
		array = DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN, text,
														INT8OID, -1));
		deconstruct_array(array, INT8OID, sizeof(int64), FLOAT8PASSBYVAL,
						  TYPALIGN_DOUBLE, &elems, &nulls, &n);
		if (n < 1)
			elog(ERROR, "the coordinator gave no values of sequence %u", relid);

		if (entry->values != NULL)
			pfree(entry->values);
		entry->values = MemoryContextAlloc(TopMemoryContext, n * sizeof(int64));
		for (int i = 0; i < n; i++)
			entry->values[i] = DatumGetInt64(elems[i]);
		entry->count = n;
		entry->next = 0;
	}

	return entry->values[entry->next++];
}

PG_FUNCTION_INFO_V1(gp_nextval);

/*
 * gp_internal.nextval(regclass)
 *		nextval(), in a slice the segments run: the coordinator's sequence's
 *		next value.  Anywhere else, nextval() itself.
 */
Datum
gp_nextval(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	if (GpClusterBackendRole() != GP_ROLE_EXECUTE || GpClusterIsSingleNode())
		PG_RETURN_INT64(nextval_internal(relid, true));
	PG_RETURN_INT64(segment_nextval(relid, false));
}

PG_FUNCTION_INFO_V1(gp_identity_nextval);

/*
 * gp_internal.identity_nextval(regclass)
 *		An identity column's next value, in a slice the segments run, which
 *		the coordinator sent: its sequence's next value there, taken without
 *		the privilege nextval() asks for, as PostgreSQL's NextValueExpr takes
 *		it.  Only for the coordinator's own plan.
 */
Datum
gp_identity_nextval(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	if (GpClusterBackendRole() != GP_ROLE_EXECUTE || !GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("gp_internal.identity_nextval() takes values only for the coordinator's plans"),
				 errdetail("The connection is no dispatched one that carries this cluster's secret.")));
	PG_RETURN_INT64(segment_nextval(relid, true));
}

PG_FUNCTION_INFO_V1(gp_sequence_values);

/*
 * gp_internal.sequence_values(seq oid, n int4, identity bool, role oid)
 *		On the coordinator, for a segment that asked, over a connection that
 *		carries the cluster secret: the sequence's next n values, as
 *		nextval() takes them, where "role", the segment's current user, may
 *		take them as nextval() asks of it -- or, for an identity column,
 *		without a privilege on the sequence, as an INSERT takes them.
 */
Datum
gp_sequence_values(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		n = PG_GETARG_INT32(1);
	bool		identity = PG_GETARG_BOOL(2);
	Oid			role = PG_GETARG_OID(3);
	Datum	   *values;
	int			count;

	if (GpClusterContentId() >= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a sequence's values are the coordinator's to give")));
	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a sequence's values are given only to the cluster's segments"),
				 errdetail("The connection does not carry this cluster's secret.")));
	if (n < 1 || n > 1000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a segment asks for 1 to 1000 values of a sequence at a time")));

	/* nextval()'s check, of the role the segment runs the statement as */
	if (!identity &&
		pg_class_aclcheck(relid, role, ACL_USAGE | ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s", get_rel_name(relid))));

	/*
	 * The first value is nextval()'s, or its error.  The rest are a block
	 * the segment will hand out, which, as PostgreSQL's cache of one, stops
	 * short at the sequence's limit rather than fail there: each is taken in
	 * a subtransaction, whose rollback lets go of the sequence's buffer
	 * nextval() raised its error holding.
	 */
	values = palloc_array(Datum, n);
	values[0] = Int64GetDatum(nextval_internal(relid, false));
	for (count = 1; count < n; count++)
	{
		MemoryContext oldcxt = CurrentMemoryContext;
		ResourceOwner oldowner = CurrentResourceOwner;
		bool		limit = false;

		CHECK_FOR_INTERRUPTS();
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(oldcxt);
		PG_TRY();
		{
			values[count] = Int64GetDatum(nextval_internal(relid, false));
			ReleaseCurrentSubTransaction();
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			MemoryContextSwitchTo(oldcxt);
			edata = CopyErrorData();
			FlushErrorState();
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
			if (edata->sqlerrcode != ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED)
				ReThrowError(edata);
			limit = true;
		}
		PG_END_TRY();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
		if (limit)
			break;
	}
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(values, count, INT8OID));
}
