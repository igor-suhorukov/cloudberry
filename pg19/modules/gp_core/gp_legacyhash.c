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
 * gp_legacyhash.c
 *	  Cloudberry's legacy cdbhash: the hash of Greenplum 5 and before, which
 *	  its cdbhash_*_ops operator classes keep for tables that still hash so.
 *
 * Cloudberry has these as built-in functions and operator classes, so that
 * a table distributed before Greenplum 6 need not be redistributed after an
 * upgrade, and its tests make tables with them: DISTRIBUTED BY (a, b
 * cdbhash_float4_ops).  gp_core's script makes the operator classes under
 * Cloudberry's names, in pg_catalog, over the functions here, which are
 * Cloudberry's own.
 *
 * A legacy hash is FNV-1 over the value's bytes, and it is not like the
 * other hash functions of an operator class.  Several columns are not
 * hashed one by one and combined; each column's hash starts from the hash
 * of the columns before it.  So a legacy function reads the hash so far
 * from a variable, Cloudberry's magic_hash_stash, which is its initial value
 * whenever the function is called on its own -- what a hash join or a hash
 * index calls it for -- and which gp_hash.c sets when it hashes a key.  And
 * the hash is reduced to a segment by a bitmask or a modulo, not by jump
 * consistent hashing: GpHashSetFunction() knows a legacy function by its C
 * name, and gp_hash.c hashes that key as Cloudberry hashes it (cdbhash.c).
 *
 * Not kept: complex, a type of Cloudberry's own that PostgreSQL 19 has not
 * got.  Arrays keep their class, which only a key that names it uses: its
 * hash is of an array's bytes, which two equal arrays need not share, and
 * Cloudberry gives no type it by default (greenplum-db/gpdb#5467).
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 * Cloudberry sources this file is made of:
 *	  src/backend/cdb/cdblegacyhash.c
 *	  get_compatible_legacy_hash_opfamily() in src/backend/utils/cache/lsyscache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/catcache.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/varbit.h"

#include "gp_hash.h"

/* Constant prime value used for an FNV1 hash */
#define FNV_32_PRIME ((uint32) 0x01000193)

/* Constant used for hashing a NULL value */
#define NULL_VAL ((uint32) 0XF0F0F0F1)

/* Constant used for hashing a NAN value */
#define NAN_VAL ((uint32) 0XE0E0E0E1)

/*
 * The hash of the key's columns before this one, as cdbhash.c leaves it for
 * the next legacy function; its initial value between keys, so that a
 * legacy function called on its own hashes one value as a key of one column.
 */
uint32		GpLegacyHashStash = GP_LEGACY_HASH_INIT;

/*
 * fnv1_32_buf - perform a 32 bit FNV 1 hash on a buffer
 *
 * input:
 *	buf - start of buffer to hash
 *	len - length of buffer in octets (bytes)
 *	hval	- previous hash value or FNV1_32_INIT if first call.
 *
 * returns:
 *	32 bit hash as a static hash type
 */
static uint32
fnv1_32_buf(const void *buf, size_t len, uint32 hval)
{
	const unsigned char *bp = (const unsigned char *) buf;	/* start of buffer */
	const unsigned char *be = bp + len; /* beyond end of buffer */

	/*
	 * FNV-1 hash each octet in the buffer
	 */
	while (bp < be)
	{
		/* multiply by the 32 bit FNV magic prime mod 2^32 */
		hval += (hval << 1) + (hval << 4) + (hval << 7) + (hval << 8) + (hval << 24);

		/* xor the bottom with the current octet */
		hval ^= (uint32) *bp++;
	}

	/* return our new hash value */
	return hval;
}

static uint32
hashFn(const void *buf, int len)
{
	return fnv1_32_buf(buf, len, GpLegacyHashStash);
}

/*
 * Given the original length of the data array this function is
 * recalculating the length after ignoring any trailing blanks. The
 * actual data remains unmodified.
 */
static int
ignoreblanks(const char *data, int len)
{
	/* look for trailing blanks and skip them in the hash calculation */
	while (data[len - 1] == ' ')
	{
		len--;

		/*
		 * If only 1 char is left, leave it alone! The string is either empty
		 * or has 1 char
		 */
		if (len == 1)
			break;
	}

	return len;
}

/*
 * Support function for hashing on inet/cidr (see network.c)
 *
 * Since network_cmp considers only ip_family, ip_bits, and ip_addr,
 * only these fields may be used in the hash; in particular don't use type.
 */
static int
inet_getkey(inet *addr, unsigned char *inet_key, int key_size)
{
	int			addrsize;

	switch (((inet_struct *) VARDATA_ANY(addr))->family)
	{
		case PGSQL_AF_INET:
			addrsize = 4;
			break;
		case PGSQL_AF_INET6:
			addrsize = 16;
			break;
		default:
			addrsize = 0;
	}

	Assert(addrsize + 2 <= key_size);
	inet_key[0] = ((inet_struct *) VARDATA_ANY(addr))->family;
	inet_key[1] = ((inet_struct *) VARDATA_ANY(addr))->bits;
	memcpy(inet_key + 2, ((inet_struct *) VARDATA_ANY(addr))->ipaddr, addrsize);

	return (addrsize + 2);
}

/* The hash of a NULL key column, from the hash before it. */
uint32
GpLegacyHashNull(void)
{
	uint32		nullbuf = NULL_VAL; /* stores the constant value that
									 * represents a NULL */

	return hashFn(&nullbuf, sizeof(nullbuf));
}

PG_FUNCTION_INFO_V1(gp_cdblegacyhash_int2);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_int4);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_int8);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_float4);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_float8);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_numeric);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_char);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_text);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_bytea);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_name);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_oid);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_tid);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_timestamp);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_date);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_time);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_timetz);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_interval);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_inet);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_macaddr);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_bit);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_bool);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_array);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_oidvector);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_cash);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_uuid);
PG_FUNCTION_INFO_V1(gp_cdblegacyhash_anyenum);

/*
 * The integers, an oid and an enum's value hash as the same 8-byte integer,
 * so that 1::int2 and 1::int8 hash alike, as cdbhash_integer_ops's cross-type
 * equality needs.
 */
Datum
gp_cdblegacyhash_int2(PG_FUNCTION_ARGS)
{
	int64		intbuf = (int64) PG_GETARG_INT16(0);

	PG_RETURN_UINT32(hashFn(&intbuf, sizeof(intbuf)));
}

Datum
gp_cdblegacyhash_int4(PG_FUNCTION_ARGS)
{
	int64		intbuf = (int64) PG_GETARG_INT32(0);

	PG_RETURN_UINT32(hashFn(&intbuf, sizeof(intbuf)));
}

Datum
gp_cdblegacyhash_int8(PG_FUNCTION_ARGS)
{
	int64		intbuf = PG_GETARG_INT64(0);

	PG_RETURN_UINT32(hashFn(&intbuf, sizeof(intbuf)));
}

Datum
gp_cdblegacyhash_float4(PG_FUNCTION_ARGS)
{
	float4		buf_f4 = PG_GETARG_FLOAT4(0);

	/*
	 * On IEEE-float machines, minus zero and zero have different bit
	 * patterns but should compare as equal.  We must ensure that they
	 * have the same hash value, which is most easily done this way:
	 */
	if (buf_f4 == (float4) 0)
		buf_f4 = 0.0;

	PG_RETURN_UINT32(hashFn(&buf_f4, sizeof(buf_f4)));
}

Datum
gp_cdblegacyhash_float8(PG_FUNCTION_ARGS)
{
	float8		buf_f8 = PG_GETARG_FLOAT8(0);

	/* minus zero is zero, as above */
	if (buf_f8 == (float8) 0)
		buf_f8 = 0.0;

	PG_RETURN_UINT32(hashFn(&buf_f8, sizeof(buf_f8)));
}

/*
 * A numeric's digits, as Cloudberry's hashes them (NUMERIC_DIGITS and
 * NUMERIC_NDIGITS).  PostgreSQL 19 keeps those macros in numeric.c, so the
 * digits are found as they find them: after a detoasted numeric's varlena
 * header, one 16-bit word of header where its high bit says the short form,
 * two otherwise.
 */
Datum
gp_cdblegacyhash_numeric(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	const void *buf;
	size_t		len;
	uint32		hash;

	if (numeric_is_nan(num))
	{
		static const uint32 nanbuf = NAN_VAL;

		buf = &nanbuf;
		len = sizeof(nanbuf);
	}
	else
	{
		uint16		header = *(uint16 *) VARDATA(num);
		size_t		headersz = (header & 0x8000) != 0
			? sizeof(uint16) : sizeof(uint16) + sizeof(int16);

		buf = VARDATA(num) + headersz;
		len = VARSIZE(num) - VARHDRSZ - headersz;
	}

	hash = hashFn(buf, len);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(num, 0);

	PG_RETURN_UINT32(hash);
}

Datum
gp_cdblegacyhash_char(PG_FUNCTION_ARGS)
{
	char		char_buf = PG_GETARG_CHAR(0);

	PG_RETURN_UINT32(hashFn(&char_buf, 1));
}

/* also for bpchar and varchar */
Datum
gp_cdblegacyhash_text(PG_FUNCTION_ARGS)
{
	text	   *text_buf = PG_GETARG_TEXT_PP(0);
	int			len;
	void	   *buf;
	uint32		hash;

	buf = (void *) VARDATA_ANY(text_buf);
	len = VARSIZE_ANY_EXHDR(text_buf);
	/* adjust length to not include trailing blanks */
	if (len > 1)
		len = ignoreblanks((char *) buf, len);

	hash = hashFn(buf, len);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(text_buf, 0);

	PG_RETURN_UINT32(hash);
}

Datum
gp_cdblegacyhash_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *bytea_buf = PG_GETARG_BYTEA_PP(0);
	uint32		hash;

	hash = hashFn(VARDATA_ANY(bytea_buf), VARSIZE_ANY_EXHDR(bytea_buf));

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(bytea_buf, 0);

	PG_RETURN_UINT32(hash);
}

Datum
gp_cdblegacyhash_name(PG_FUNCTION_ARGS)
{
	char	   *namebuf = NameStr(*PG_GETARG_NAME(0));

	/* adjust length to not include trailing blanks */
	PG_RETURN_UINT32(hashFn(namebuf, ignoreblanks(namebuf, NAMEDATALEN)));
}

Datum
gp_cdblegacyhash_oid(PG_FUNCTION_ARGS)
{
	int64		intbuf = (int64) PG_GETARG_INT32(0);

	PG_RETURN_UINT32(hashFn(&intbuf, sizeof(intbuf)));
}

Datum
gp_cdblegacyhash_tid(PG_FUNCTION_ARGS)
{
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(0);

	/* See hashtid() for why we're not using sizeof(ItemPointerData) here */
	PG_RETURN_UINT32(hashFn(tid, sizeof(BlockIdData) + sizeof(OffsetNumber)));
}

/* timestamp and timestamptz, both an int64 */
Datum
gp_cdblegacyhash_timestamp(PG_FUNCTION_ARGS)
{
	Timestamp	tsbuf = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_UINT32(hashFn(&tsbuf, sizeof(tsbuf)));
}

Datum
gp_cdblegacyhash_date(PG_FUNCTION_ARGS)
{
	DateADT		datebuf = PG_GETARG_DATEADT(0);

	PG_RETURN_UINT32(hashFn(&datebuf, sizeof(datebuf)));
}

Datum
gp_cdblegacyhash_time(PG_FUNCTION_ARGS)
{
	TimeADT		timebuf = PG_GETARG_TIMEADT(0);

	PG_RETURN_UINT32(hashFn(&timebuf, sizeof(timebuf)));
}

Datum
gp_cdblegacyhash_timetz(PG_FUNCTION_ARGS)
{
	TimeTzADT  *timetzptr = PG_GETARG_TIMETZADT_P(0);

	/*
	 * Specify hash length as sizeof(double) + sizeof(int4), not as
	 * sizeof(TimeTzADT), so that any garbage pad bytes in the
	 * structure won't be included in the hash!
	 */
	PG_RETURN_UINT32(hashFn(timetzptr,
							sizeof(timetzptr->time) + sizeof(timetzptr->zone)));
}

Datum
gp_cdblegacyhash_interval(PG_FUNCTION_ARGS)
{
	Interval   *intervalptr = PG_GETARG_INTERVAL_P(0);

	/*
	 * Specify hash length as sizeof(double) + sizeof(int4), not as
	 * sizeof(Interval), so that any garbage pad bytes in the
	 * structure won't be included in the hash!
	 */
	PG_RETURN_UINT32(hashFn(intervalptr,
							sizeof(intervalptr->time) + sizeof(intervalptr->month)));
}

/* inet and cidr */
Datum
gp_cdblegacyhash_inet(PG_FUNCTION_ARGS)
{
	inet	   *inetptr = PG_GETARG_INET_PP(0);
	unsigned char inet_hkey[sizeof(inet_struct)];
	uint32		hash;

	hash = hashFn(inet_hkey, inet_getkey(inetptr, inet_hkey, sizeof(inet_hkey)));

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(inetptr, 0);

	PG_RETURN_UINT32(hash);
}

Datum
gp_cdblegacyhash_macaddr(PG_FUNCTION_ARGS)
{
	macaddr    *macptr = PG_GETARG_MACADDR_P(0);

	PG_RETURN_UINT32(hashFn(macptr, sizeof(macaddr)));
}

/*
 * bit and bit varying.  Note that these are essentially strings. we don't
 * need to worry about '10' and '010' to compare, b/c they will not, by
 * design.  (see SQL standard, and varbit.c)
 */
Datum
gp_cdblegacyhash_bit(PG_FUNCTION_ARGS)
{
	VarBit	   *vbitptr = PG_GETARG_VARBIT_P(0);
	uint32		hash;

	hash = hashFn(VARBITS(vbitptr), VARBITBYTES(vbitptr));

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(vbitptr, 0);

	PG_RETURN_UINT32(hash);
}

Datum
gp_cdblegacyhash_bool(PG_FUNCTION_ARGS)
{
	char		bool_buf = PG_GETARG_BOOL(0);

	PG_RETURN_UINT32(hashFn(&bool_buf, sizeof(bool_buf)));
}

Datum
gp_cdblegacyhash_array(PG_FUNCTION_ARGS)
{
	ArrayType  *arrbuf = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_UINT32(hashFn(VARDATA(arrbuf), VARSIZE(arrbuf) - VARHDRSZ));
}

Datum
gp_cdblegacyhash_oidvector(PG_FUNCTION_ARGS)
{
	oidvector  *oidvec_buf = (oidvector *) PG_GETARG_POINTER(0);

	PG_RETURN_UINT32(hashFn(oidvec_buf->values, oidvec_buf->dim1 * sizeof(Oid)));
}

Datum
gp_cdblegacyhash_cash(PG_FUNCTION_ARGS)
{
	Cash		cash_buf = PG_GETARG_CASH(0);

	PG_RETURN_UINT32(hashFn(&cash_buf, sizeof(Cash)));
}

Datum
gp_cdblegacyhash_uuid(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid_buf = PG_GETARG_UUID_P(0);

	PG_RETURN_UINT32(hashFn(uuid_buf, UUID_LEN));
}

Datum
gp_cdblegacyhash_anyenum(PG_FUNCTION_ARGS)
{
	int64		intbuf = (int64) PG_GETARG_INT32(0);

	PG_RETURN_UINT32(hashFn(&intbuf, sizeof(intbuf)));
}

/* ------------------------------------------------------------------------- */
/* Which functions and classes are legacy                                    */
/* ------------------------------------------------------------------------- */

/*
 * Is this a legacy hash function?  One of the functions above, by the
 * library and the C name gp_core's script gives it: Cloudberry's
 * isLegacyCdbHashFunction() asks by fixed OID, which an extension's
 * functions have not got.
 */
bool
GpHashIsLegacyFunction(Oid funcid)
{
	HeapTuple	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	bool		legacy = false;

	if (!HeapTupleIsValid(tuple))
		return false;
	if (((Form_pg_proc) GETSTRUCT(tuple))->prolang == ClanguageId)
	{
		bool		isnull;
		Datum		bin = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_probin,
										  &isnull);

		if (!isnull && strcmp(TextDatumGetCString(bin), "$libdir/gp_core") == 0)
		{
			Datum		src = SysCacheGetAttrNotNull(PROCOID, tuple,
													 Anum_pg_proc_prosrc);

			legacy = strncmp(TextDatumGetCString(src), "gp_cdblegacyhash_",
							 strlen("gp_cdblegacyhash_")) == 0;
		}
	}
	ReleaseSysCache(tuple);
	return legacy;
}

/*
 * The legacy operator class a type's key is hashed with where
 * gp.use_legacy_hashops asks for one, as Cloudberry's
 * get_legacy_cdbhash_opclass_for_base_type() chooses it; InvalidOid for a
 * type Greenplum 5 could not hash, or in a database gp_core's script has not
 * made the classes in.
 */
Oid
GpLegacyHashOpclassForType(Oid typid)
{
	const char *name;

	switch (getBaseType(typid))
	{
		case INT2OID:
			name = "cdbhash_int2_ops";
			break;
		case INT4OID:
			name = "cdbhash_int4_ops";
			break;
		case INT8OID:
			name = "cdbhash_int8_ops";
			break;
		case FLOAT4OID:
			name = "cdbhash_float4_ops";
			break;
		case FLOAT8OID:
			name = "cdbhash_float8_ops";
			break;
		case NUMERICOID:
			name = "cdbhash_numeric_ops";
			break;
		case CHAROID:
			name = "cdbhash_char_ops";
			break;
		case BPCHAROID:
			name = "cdbhash_bpchar_ops";
			break;
		case TEXTOID:
		case VARCHAROID:
			name = "cdbhash_text_ops";
			break;
		case BYTEAOID:
			name = "cdbhash_bytea_ops";
			break;
		case NAMEOID:
			name = "cdbhash_name_ops";
			break;
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
			name = "cdbhash_oid_ops";
			break;
		case TIDOID:
			name = "cdbhash_tid_ops";
			break;
		case TIMESTAMPOID:
			name = "cdbhash_timestamp_ops";
			break;
		case TIMESTAMPTZOID:
			name = "cdbhash_timestamptz_ops";
			break;
		case DATEOID:
			name = "cdbhash_date_ops";
			break;
		case TIMEOID:
			name = "cdbhash_time_ops";
			break;
		case TIMETZOID:
			name = "cdbhash_timetz_ops";
			break;
		case INTERVALOID:
			name = "cdbhash_interval_ops";
			break;
		case INETOID:
		case CIDROID:
			name = "cdbhash_inet_ops";
			break;
		case MACADDROID:
			name = "cdbhash_macaddr_ops";
			break;
		case BITOID:
		case VARBITOID:
			name = "cdbhash_bit_ops";
			break;
		case BOOLOID:
			name = "cdbhash_bool_ops";
			break;
		case OIDVECTOROID:
			name = "cdbhash_oidvector_ops";
			break;
		case CASHOID:
			name = "cdbhash_cash_ops";
			break;
		case UUIDOID:
			name = "cdbhash_uuid_ops";
			break;
		case ANYENUMOID:
			name = "cdbhash_enum_ops";
			break;
		default:
			if (!type_is_enum(getBaseType(typid)))
				return InvalidOid;
			name = "cdbhash_enum_ops";
			break;
	}
	return get_opclass_oid(HASH_AM_OID,
						   list_make2(makeString("pg_catalog"), makeString(pstrdup(name))),
						   true);
}

/*
 * The legacy hash family an equality operator is in, as Cloudberry's
 * get_compatible_legacy_hash_opfamily() finds it: the one of the hash
 * families it is the equality of whose hash function for its left type is a
 * legacy one; InvalidOid for none.
 *
 * A family with no hash function for the type is passed over, where
 * Cloudberry's raises.  Its legacy families are in its catalog, with OIDs
 * below any a user's family gets, so its search ends before it reaches one;
 * gp_core's are made by CREATE EXTENSION, after families like PostgreSQL's
 * test_setup's part_test_int4_ops, which has only the extended hash function.
 */
Oid
GpLegacyHashOpfamilyOfOperator(Oid opno)
{
	CatCList   *catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));
	Oid			found = InvalidOid;

	for (int i = 0; i < catlist->n_members && !OidIsValid(found); i++)
	{
		Form_pg_amop amop = (Form_pg_amop) GETSTRUCT(&catlist->members[i]->tuple);
		Oid			proc;

		if (amop->amopmethod != HASH_AM_OID ||
			amop->amopstrategy != HTEqualStrategyNumber)
			continue;
		proc = GpHashProcInOpfamily(amop->amopfamily, amop->amoplefttype, true);
		if (OidIsValid(proc) && GpHashIsLegacyFunction(proc))
			found = amop->amopfamily;
	}
	ReleaseSysCacheList(catlist);
	return found;
}
