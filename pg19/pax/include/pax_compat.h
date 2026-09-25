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
 * pax_compat.h
 *	  What Cloudberry's server gave PAX that PostgreSQL 19's has not, or
 *	  gives under another name: Cloudberry's spellings over PostgreSQL 19's
 *	  and the port's.  Included by the port's comm/cbdb_api.h, inside its
 *	  extern "C".
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAX_COMPAT_H
#define PAX_COMPAT_H

/* Gp_role, IS_QUERY_DISPATCHER() and the rest, over gp_core's state */
#include "cb_compat.h"
#include "gp_fault.h"

/* numeric's layout and working variable, which Cloudberry's numeric.h has */
#include "pax_numeric.h"

/*
 * PostgreSQL 19 made strtoi64() and strtou64() function-like macros, which
 * protobuf's own strtou64() (google/protobuf/stubs/strutil.h) expands
 * through.  PAX calls neither.
 */
#undef strtoi64
#undef strtou64

/* <sys/param.h>'s, which Cloudberry's c.h brought in. */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* Cloudberry's fault injector, as gp_core carries it (gp_fault.h). */
#ifndef FAULT_INJECTOR
#define FAULT_INJECTOR
#endif
#define SIMPLE_FAULT_INJECTOR(name) GP_FAULT(name)
#define FaultInjectorTypeSkip GP_FAULT_SKIP
#define DDLNotSpecified 0
#define FaultInjector_InjectFaultIfSet(name, ddl, database, table) \
	((gp_fault_active == NULL || *gp_fault_active == 0) ? GP_FAULT_NONE \
	 : GpFaultTrigger((name), (database), (table)))

/* Cloudberry's GpIdentity, of which PAX reads the segment's content ID. */
struct PaxGpIdentity
{
	int			segindex;
};

static inline struct PaxGpIdentity
pax_gp_identity(void)
{
	struct PaxGpIdentity id;

	id.segindex = GpIdentity_segindex;
	return id;
}

#define GpIdentity (pax_gp_identity())

/*
 * Cloudberry's gp_interconnect_queue_depth, gp_core's
 * gp.interconnect_queue_depth, which PAX checks against a wide table.
 */
static inline int
pax_gp_interconnect_queue_depth(void)
{
	const char *value = GetConfigOption("gp.interconnect_queue_depth", true, false);

	return value ? atoi(value) : 4;
}

#define Gp_interconnect_queue_depth (pax_gp_interconnect_queue_depth())

/*
 * Cloudberry's gp_enable_predicate_pushdown, the port's
 * gp.enable_predicate_pushdown (modules/pax/pax.c).
 */
extern bool gp_enable_predicate_pushdown;

/*
 * Cloudberry's GUC flag for a setting the dispatcher sends a segment; the
 * port's gp_core sends the ones on its list (gp_dispatch.c), PAX's among
 * them.
 */
#define GUC_GPDB_NEED_SYNC 0

/* Cloudberry's reloptions.h: an append-optimized table's compresslevel. */
#define AO_DEFAULT_COMPRESSLEVEL 0
#define AO_MIN_COMPRESSLEVEL 0
#define AO_MAX_COMPRESSLEVEL 19

/* Cloudberry's bufmgr.h: the pages a size in bytes would take. */
static inline BlockNumber
RelationGuessNumberOfBlocksFromSize(uint64 szbytes)
{
	return (szbytes + (BLCKSZ - 1)) / BLCKSZ;
}

/* Cloudberry's itemptr.c: a TID as "(block,offset)", in a static buffer. */
static inline char *
ItemPointerToString(ItemPointer tid)
{
	static char buffer[50];

	snprintf(buffer, sizeof(buffer), "(%u,%u)",
			 ItemPointerGetBlockNumberNoCheck(tid),
			 ItemPointerGetOffsetNumberNoCheck(tid));
	return buffer;
}

/*
 * Cloudberry's varatt.h: the tag of PAX's own TOAST pointer, to a value
 * kept in a file of PAX's.  Only PAX reads one; it never leaves PAX's scan.
 */
#define VARTAG_CUSTOM ((vartag_external) 21)

/*
 * The POSIX collation, OID 951 in PostgreSQL 19's pg_collation.dat, which
 * gives it no symbol where Cloudberry's did.
 */
#define POSIX_COLLATION_OID 951

/* Cloudberry's tableam.h: a run of block numbers, the port's O18. */
typedef TableAmBlockSequence BlockSequence;

/* Cloudberry's guc.h: the storage options' names. */
#define SOPT_COMPTYPE "compresstype"
#define SOPT_COMPLEVEL "compresslevel"

/*
 * Cloudberry's skey.h: a scan key a runtime bloom filter made, which the
 * port's executor never makes.
 */
#define SK_BLOOM_FILTER 0x4000

/* PostgreSQL 17 made a backend's number a ProcNumber. */
typedef ProcNumber BackendId;
#define InvalidBackendId INVALID_PROC_NUMBER

/* Cloudberry's Datum of a one-byte integer, int8 in C. */
static inline Datum
Int8GetDatum(int8 X)
{
	return (Datum) X;
}

static inline int8
DatumGetInt8(Datum X)
{
	return (int8) X;
}

/* Cloudberry's c.h: cond1 implies cond2. */
#ifndef AssertImply
#define AssertImply(cond1, cond2) Assert(!(cond1) || (cond2))
#endif

#ifdef __cplusplus
extern "C++"
{
/*
 * PostgreSQL 19's PointerGetDatum() is a macro whose conditional expression
 * takes only a pointer, and in C++ neither nullptr nor NULL, which PAX
 * gives it: a function of any pointer, and of a null one.
 */
#undef PointerGetDatum
template <typename T>
static inline Datum
PointerGetDatum(T *X)
{
	return (Datum) (uintptr_t) X;
}

static inline Datum
PointerGetDatum(decltype(nullptr))
{
	return (Datum) 0;
}

/*
 * PostgreSQL 16's DatumGetPointer() was a cast, which PAX gives a pointer
 * too; Cloudberry's Pointer was a char *.
 */
static inline char *
DatumGetPointer(const void *X)
{
	return (char *) X;
}

/* PostgreSQL 19's calls that took one more argument than Cloudberry's */
static inline TupleTableSlot *
MakeTupleTableSlot(TupleDesc tupleDesc, const TupleTableSlotOps *tts_ops)
{
	return MakeTupleTableSlot(tupleDesc, tts_ops, 0);
}

static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot, int nkeys,
				ScanKeyData *key)
{
	return table_beginscan(rel, snapshot, nkeys, key, 0);
}

static inline bool
reindex_relation(Oid relid, int flags, const ReindexParams *params)
{
	return reindex_relation(NULL, relid, flags, params);
}

/*
 * PostgreSQL 19 made the varlena accessors inline functions of a pointer,
 * where PostgreSQL 16's macros took a Datum as readily, which PAX gives
 * them: each again for a Datum.
 */
static inline Size
VARSIZE(Datum d)
{
	return VARSIZE(DatumGetPointer(d));
}

static inline char *
VARDATA(Datum d)
{
	return VARDATA(DatumGetPointer(d));
}

static inline Size
VARSIZE_SHORT(Datum d)
{
	return VARSIZE_SHORT(DatumGetPointer(d));
}

static inline char *
VARDATA_SHORT(Datum d)
{
	return VARDATA_SHORT(DatumGetPointer(d));
}

static inline vartag_external
VARTAG_EXTERNAL(Datum d)
{
	return VARTAG_EXTERNAL(DatumGetPointer(d));
}

static inline Size
VARSIZE_EXTERNAL(Datum d)
{
	return VARSIZE_EXTERNAL(DatumGetPointer(d));
}

static inline char *
VARDATA_EXTERNAL(Datum d)
{
	return VARDATA_EXTERNAL(DatumGetPointer(d));
}

static inline bool
VARATT_IS_COMPRESSED(Datum d)
{
	return VARATT_IS_COMPRESSED(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL(Datum d)
{
	return VARATT_IS_EXTERNAL(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_ONDISK(Datum d)
{
	return VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_INDIRECT(Datum d)
{
	return VARATT_IS_EXTERNAL_INDIRECT(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_EXPANDED_RO(Datum d)
{
	return VARATT_IS_EXTERNAL_EXPANDED_RO(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_EXPANDED_RW(Datum d)
{
	return VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_EXPANDED(Datum d)
{
	return VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTERNAL_NON_EXPANDED(Datum d)
{
	return VARATT_IS_EXTERNAL_NON_EXPANDED(DatumGetPointer(d));
}

static inline bool
VARATT_IS_SHORT(Datum d)
{
	return VARATT_IS_SHORT(DatumGetPointer(d));
}

static inline bool
VARATT_IS_EXTENDED(Datum d)
{
	return VARATT_IS_EXTENDED(DatumGetPointer(d));
}

static inline bool
VARATT_CAN_MAKE_SHORT(Datum d)
{
	return VARATT_CAN_MAKE_SHORT(DatumGetPointer(d));
}

static inline Size
VARATT_CONVERTED_SHORT_SIZE(Datum d)
{
	return VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(d));
}

static inline Size
VARSIZE_ANY(Datum d)
{
	return VARSIZE_ANY(DatumGetPointer(d));
}

static inline Size
VARSIZE_ANY_EXHDR(Datum d)
{
	return VARSIZE_ANY_EXHDR(DatumGetPointer(d));
}

static inline char *
VARDATA_ANY(Datum d)
{
	return VARDATA_ANY(DatumGetPointer(d));
}

static inline Size
VARDATA_COMPRESSED_GET_EXTSIZE(Datum d)
{
	return VARDATA_COMPRESSED_GET_EXTSIZE(DatumGetPointer(d));
}

static inline uint32
VARDATA_COMPRESSED_GET_COMPRESS_METHOD(Datum d)
{
	return VARDATA_COMPRESSED_GET_COMPRESS_METHOD(DatumGetPointer(d));
}

static inline void
SET_VARSIZE(Datum d, Size len)
{
	SET_VARSIZE(DatumGetPointer(d), len);
}

static inline void
SET_VARSIZE_SHORT(Datum d, Size len)
{
	SET_VARSIZE_SHORT(DatumGetPointer(d), len);
}

static inline void
SET_VARSIZE_COMPRESSED(Datum d, Size len)
{
	SET_VARSIZE_COMPRESSED(DatumGetPointer(d), len);
}

static inline void
SET_VARTAG_EXTERNAL(Datum d, vartag_external tag)
{
	SET_VARTAG_EXTERNAL(DatumGetPointer(d), tag);
}
}
#endif

#endif							/* PAX_COMPAT_H */
