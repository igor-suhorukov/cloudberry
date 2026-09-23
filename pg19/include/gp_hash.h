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
 * gp_hash.h
 *	  Which segment a row belongs on.
 *
 * Cloudberry's cdbhash: each key column's value through its opclass's hash
 * function, the results folded together by rotating and xoring, and the sum
 * reduced to a segment by jump consistent hashing.  The arithmetic is kept
 * exactly, so that a row lands on the segment Cloudberry would put it on.
 *
 * And its legacy cdbhash, Greenplum 5's, for a key hashed with one of the
 * cdbhash_*_ops operator classes (gp_legacyhash.c): FNV-1, each column's
 * hash going on from the columns' before it, reduced by a bitmask or a
 * modulo.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_HASH_H
#define GP_HASH_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "fmgr.h"

#include "gp_policy.h"

/* What GpHashSegment() answers for a replicated table: every segment. */
#define GP_HASH_ALL_SEGMENTS	(-1)

typedef struct GpHash
{
	GpPolicyType ptype;
	int			numsegs;
	int			nattrs;			/* 0 for random and replicated */
	AttrNumber *attrs;			/* the key's columns, in the relation */
	FmgrInfo   *hashfuncs;		/* and each one's hash function */
	bool		legacy;			/* hashed as Cloudberry's legacy cdbhash */
} GpHash;

/*
 * For a relation with this policy.  "tupdesc" is the relation's, which is
 * where the key's attribute numbers point.
 */
extern GpHash *GpHashMake(const GpPolicy *policy, TupleDesc tupdesc);

/*
 * The i'th key column's hash function, for a GpHash made by hand: a legacy
 * one makes the whole key hash as the legacy cdbhash does, as Cloudberry's
 * makeCdbHash() decides.
 */
extern void GpHashSetFunction(GpHash *h, int i, Oid funcid);

/*
 * The segment for a row whose columns are values[]/isnull[], indexed as the
 * relation's attributes are, from 0.  GP_HASH_ALL_SEGMENTS for a replicated
 * table; a random segment for a randomly distributed one.
 */
extern int	GpHashSegment(GpHash *h, const Datum *values, const bool *isnull);

/*
 * The hash function a distribution key of this type is hashed with, in this
 * family; InvalidOid, if missing_ok, where the family has none.
 */
extern Oid	GpHashProcInOpfamily(Oid opfamily, Oid typeoid, bool missing_ok);

/*
 * The segment a relation's key holds these values on: values[k] of types[k]
 * for the k'th column of the key, a type of the column's hash family --
 * direct dispatch's constants, which need not be of the column's own type.
 * -1 where the family cannot hash one of them.
 */
extern int	GpHashSegmentForKey(const GpPolicy *policy, const Oid *types,
								const Datum *values, const bool *isnull);

/* Cloudberry's reduction of a 32-bit hash to one of n segments. */
extern int	GpJumpConsistentHash(uint64 key, int32 num_segments);

/* ------------------------------------------------------------------------- */
/* The legacy cdbhash (gp_legacyhash.c)                                      */
/* ------------------------------------------------------------------------- */

/* FNV-1's offset basis: where a legacy key's hash starts. */
#define GP_LEGACY_HASH_INIT		((uint32) 0x811c9dc5)

/*
 * The hash of the key's columns before the one a legacy function is hashing,
 * which it goes on from: Cloudberry's magic_hash_stash.  GpHashSegment()
 * sets it for each column and puts it back to GP_LEGACY_HASH_INIT after, so
 * that a legacy function called on its own hashes one value alone.
 */
extern uint32 GpLegacyHashStash;

/* A NULL key column's legacy hash, going on from GpLegacyHashStash. */
extern uint32 GpLegacyHashNull(void);

/* Is this one of gp_core's legacy hash functions? */
extern bool GpHashIsLegacyFunction(Oid funcid);

/*
 * The legacy operator class a type is hashed with where gp.use_legacy_hashops
 * asks for one, or InvalidOid; and the legacy hash family an equality
 * operator is in, or InvalidOid.
 */
extern Oid	GpLegacyHashOpclassForType(Oid typid);
extern Oid	GpLegacyHashOpfamilyOfOperator(Oid opno);

#endif							/* GP_HASH_H */
