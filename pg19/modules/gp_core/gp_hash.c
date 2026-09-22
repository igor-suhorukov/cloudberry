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
 * gp_hash.c
 *	  Which segment a row belongs on: Cloudberry's cdbhash.
 *
 * Kept exactly as Cloudberry computes it -- the opclass's hash function per
 * key column, called with the default collation as Cloudberry calls it; the
 * running hash rotated left one bit before each column and xored with it, a
 * NULL contributing the rotation alone; and jump consistent hashing to reduce
 * it to a segment -- so that a row lands where Cloudberry puts it, and a
 * cluster that grows moves as few rows as Cloudberry's does.
 *
 * Not kept: the legacy hash opclasses (FNV-1 and a bitmask or modulo
 * reduction), which Cloudberry has for tables pg_upgraded from Greenplum 5.
 * No module of the port installs them, so no policy can name one.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/cdb/cdbhash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "common/pg_prng.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_hash.h"

/*
 * Cloudberry's cdb_hashproc_in_opfamily(): the family's hash function for the
 * type, or one for a type it is binary coercible to -- varchar hashes with
 * text's.
 */
Oid
GpHashProcInOpfamily(Oid opfamily, Oid typeoid)
{
	Oid			hashfunc;
	CatCList   *catlist;

	hashfunc = get_opfamily_proc(opfamily, typeoid, typeoid, HASHSTANDARD_PROC);
	if (OidIsValid(hashfunc))
		return hashfunc;

	catlist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamily));
	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amproc amproc = (Form_pg_amproc) GETSTRUCT(tuple);

		if (amproc->amprocnum != HASHSTANDARD_PROC)
			continue;
		if (amproc->amproclefttype != amproc->amprocrighttype)
			continue;
		if (IsBinaryCoercible(typeoid, amproc->amproclefttype))
		{
			hashfunc = amproc->amproc;
			break;
		}
	}
	ReleaseSysCacheList(catlist);

	if (!OidIsValid(hashfunc))
		elog(ERROR, "could not find hash function for type %u in operator family %u",
			 typeoid, opfamily);

	return hashfunc;
}

/*
 * Jump consistent hashing (Lamping and Veach, 2014), as Cloudberry has it:
 * growing a cluster from n to n+1 segments moves a key only to the new one.
 */
int
GpJumpConsistentHash(uint64 key, int32 num_segments)
{
	int64		b = -1;
	int64		j = 0;

	while (j < num_segments)
	{
		b = j;
		key = key * 2862933555777941757ULL + 1;
		j = (b + 1) * ((double) (1LL << 31) / (double) ((key >> 33) + 1));
	}
	return (int) b;
}

GpHash *
GpHashMake(const GpPolicy *policy, TupleDesc tupdesc)
{
	GpHash	   *h = (GpHash *) palloc0(sizeof(GpHash));

	h->ptype = policy ? policy->ptype : POLICYTYPE_ENTRY;
	h->numsegs = policy ? policy->numsegments : 1;

	if (policy == NULL || !GpPolicyIsHashPartitioned(policy))
		return h;

	h->nattrs = policy->nattrs;
	h->attrs = (AttrNumber *) palloc(sizeof(AttrNumber) * policy->nattrs);
	h->hashfuncs = (FmgrInfo *) palloc(sizeof(FmgrInfo) * policy->nattrs);

	for (int i = 0; i < policy->nattrs; i++)
	{
		AttrNumber	attnum = policy->attrs[i];
		Oid			typeoid = TupleDescAttr(tupdesc, attnum - 1)->atttypid;
		Oid			opfamily = get_opclass_family(policy->opclasses[i]);

		h->attrs[i] = attnum;
		fmgr_info(GpHashProcInOpfamily(opfamily, typeoid), &h->hashfuncs[i]);
	}

	return h;
}

int
GpHashSegment(GpHash *h, const Datum *values, const bool *isnull)
{
	uint32		hash = 0;

	if (h->ptype == POLICYTYPE_REPLICATED)
		return GP_HASH_ALL_SEGMENTS;

	/*
	 * No key: a random segment, as Cloudberry's cdbhashrandomseg() chooses
	 * one.  Its modulo favours low segments by one part in 2^31 / n, which it
	 * says is acceptable, and is here.
	 */
	if (h->nattrs == 0)
		return (int) (pg_prng_uint32(&pg_global_prng_state) % (uint32) h->numsegs);

	for (int i = 0; i < h->nattrs; i++)
	{
		int			att = h->attrs[i] - 1;

		/* rotate the running hash left one bit at each column */
		hash = (hash << 1) | ((hash & 0x80000000) ? 1 : 0);

		if (!isnull[att])
		{
			LOCAL_FCINFO(fcinfo, 1);
			uint32		hkey;

			/*
			 * The default collation, as Cloudberry passes it: a text key hashes
			 * the same whatever the column's collation, so that the segment a
			 * row is on does not depend on it.
			 */
			InitFunctionCallInfoData(*fcinfo, &h->hashfuncs[i], 1,
									 DEFAULT_COLLATION_OID, NULL, NULL);
			fcinfo->args[0].value = values[att];
			fcinfo->args[0].isnull = false;

			hkey = DatumGetUInt32(FunctionCallInvoke(fcinfo));
			if (fcinfo->isnull)
				elog(ERROR, "function %u returned NULL", fcinfo->flinfo->fn_oid);

			hash ^= hkey;
		}
	}

	return GpJumpConsistentHash(hash, h->numsegs);
}
