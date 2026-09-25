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
 * ao_options.c
 *	  An append-optimized table's options: blocksize, compresstype,
 *	  compresslevel and checksum, beside heap's.
 *
 * Cloudberry parses them through its amoptions callback and keeps them in
 * pg_class.reloptions; the port does the same through O14, which asks an
 * access method that registered a parser to parse its tables' options.  The
 * access method's own go to its own parser and the rest to heap's, each
 * validating when asked, so a name neither knows is refused as heap refuses
 * one, and what the relcache keeps begins with heap's StdRdOptions, which
 * the core reads from every table.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "gp_ao.h"

#define AO_MIN_BLOCKSIZE		8192
#define AO_MAX_BLOCKSIZE		(2 * 1024 * 1024)

static relopt_kind ao_relopt_kind;

/* What the access method's own parser fills, before it is resolved. */
typedef struct AoOwnOptions
{
	int32		vl_len_;
	int			blocksize;
	int			compresstype;	/* offset of the string */
	int			compresslevel;
	bool		checksum;
} AoOwnOptions;

static const char *const own_names[] = {
	"blocksize", "compresstype", "compresslevel", "checksum"
};

static AoCompressType
compresstype_parse(const char *value, bool *ok)
{
	*ok = true;
	if (value == NULL || pg_strcasecmp(value, "none") == 0 || value[0] == '\0')
		return AO_COMPRESS_NONE;
	if (pg_strcasecmp(value, "zlib") == 0)
		return AO_COMPRESS_ZLIB;
	if (pg_strcasecmp(value, "zstd") == 0)
		return AO_COMPRESS_ZSTD;
	if (pg_strcasecmp(value, "rle_type") == 0)
		return AO_COMPRESS_RLE;
	*ok = false;
	return AO_COMPRESS_NONE;
}

const char *
ao_compresstype_name(AoCompressType type)
{
	switch (type)
	{
		case AO_COMPRESS_NONE:
			return "none";
		case AO_COMPRESS_ZLIB:
			return "zlib";
		case AO_COMPRESS_ZSTD:
			return "zstd";
		case AO_COMPRESS_RLE:
			return "rle_type";
	}
	return "none";
}

static void
validate_compresstype(const char *value)
{
	bool		ok;

	(void) compresstype_parse(value, &ok);
	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown compresstype \"%s\"", value)));
#ifndef GP_AO_HAVE_ZSTD
	if (pg_strcasecmp(value, "zstd") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("compresstype \"zstd\" is not supported: gp_ao was built without zstd")));
#endif
}

void
ao_options_init(void)
{
	ao_relopt_kind = add_reloption_kind();
	add_int_reloption(ao_relopt_kind, "blocksize",
					  "Size of an append-optimized table's blocks, in bytes.",
					  AO_DEFAULT_BLOCKSIZE, AO_MIN_BLOCKSIZE, AO_MAX_BLOCKSIZE,
					  AccessExclusiveLock);
	add_string_reloption(ao_relopt_kind, "compresstype",
						 "How an append-optimized table's blocks are compressed.",
						 NULL, validate_compresstype, AccessExclusiveLock);
	add_int_reloption(ao_relopt_kind, "compresslevel",
					  "How hard an append-optimized table's blocks are compressed.",
					  0, 0, 19, AccessExclusiveLock);
	add_bool_reloption(ao_relopt_kind, "checksum",
					   "Whether an append-optimized table's blocks carry checksums.",
					   true, AccessExclusiveLock);
}

static bool
is_own_option(const char *opt)
{
	for (int i = 0; i < lengthof(own_names); i++)
	{
		size_t		len = strlen(own_names[i]);

		if (pg_strncasecmp(opt, own_names[i], len) == 0 && opt[len] == '=')
			return true;
	}
	return false;
}

/* Did the options name this one? */
static bool
own_given(Datum own, const char *name)
{
	Datum	   *elems;
	int			nelems;
	size_t		len = strlen(name);

	if (own == (Datum) 0)
		return false;
	deconstruct_array_builtin(DatumGetArrayTypeP(own), TEXTOID, &elems, NULL, &nelems);
	for (int i = 0; i < nelems; i++)
	{
		char	   *opt = TextDatumGetCString(elems[i]);

		if (pg_strncasecmp(opt, name, len) == 0 && opt[len] == '=')
			return true;
	}
	return false;
}

static bytea *
ao_parse_options(Datum reloptions, char relkind, bool validate, bool columnar)
{
	ArrayBuildState *own = NULL;
	ArrayBuildState *heaps = NULL;
	Datum		own_datum = (Datum) 0;
	Datum		heap_datum = (Datum) 0;
	StdRdOptions *std;
	AoOwnOptions *mine;
	AoRelOptions *result;
	bool		ok;
	AoCompressType type;
	static const relopt_parse_elt tab[] = {
		{"blocksize", RELOPT_TYPE_INT, offsetof(AoOwnOptions, blocksize)},
		{"compresstype", RELOPT_TYPE_STRING, offsetof(AoOwnOptions, compresstype)},
		{"compresslevel", RELOPT_TYPE_INT, offsetof(AoOwnOptions, compresslevel)},
		{"checksum", RELOPT_TYPE_BOOL, offsetof(AoOwnOptions, checksum)},
	};

	if (reloptions != (Datum) 0)
	{
		ArrayType  *array = DatumGetArrayTypeP(reloptions);
		Datum	   *elems;
		int			nelems;

		deconstruct_array_builtin(array, TEXTOID, &elems, NULL, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			char	   *opt = TextDatumGetCString(elems[i]);

			if (is_own_option(opt))
				own = accumArrayResult(own, elems[i], false, TEXTOID,
									   CurrentMemoryContext);
			else
				heaps = accumArrayResult(heaps, elems[i], false, TEXTOID,
										 CurrentMemoryContext);
		}
		if (own)
			own_datum = makeArrayResult(own, CurrentMemoryContext);
		if (heaps)
			heap_datum = makeArrayResult(heaps, CurrentMemoryContext);
	}

	std = (StdRdOptions *) heap_reloptions(relkind, heap_datum, validate);
	mine = (AoOwnOptions *) build_reloptions(own_datum, validate, ao_relopt_kind,
											 sizeof(AoOwnOptions), tab,
											 lengthof(tab));

	result = palloc0(sizeof(AoRelOptions));
	if (std)
		memcpy(&result->std, std, sizeof(StdRdOptions));
	result->blocksize = mine ? mine->blocksize : AO_DEFAULT_BLOCKSIZE;
	result->compresslevel = mine ? mine->compresslevel : 0;
	result->checksum = mine ? mine->checksum : true;
	type = compresstype_parse(mine && mine->compresstype ?
							  (char *) mine + mine->compresstype : NULL, &ok);

	if (validate)
	{
		if (result->blocksize % AO_MIN_BLOCKSIZE != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block size must be between 8KB and 2MB and be an 8KB multiple, got %d",
							result->blocksize)));
		if (type == AO_COMPRESS_RLE && !columnar)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("rle_type cannot be used with Append Only relations row orientation")));
		if (type != AO_COMPRESS_NONE && result->compresslevel == 0 &&
			own_given(own_datum, "compresslevel"))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("compresstype \"%s\" can't be used with compresslevel 0",
							ao_compresstype_name(type))));
		if (type == AO_COMPRESS_ZLIB && result->compresslevel > 9)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("compresslevel=%d is out of range for zlib (should be in the range 1 to 9)",
							result->compresslevel)));
		if (type == AO_COMPRESS_RLE && result->compresslevel > 6)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("compresslevel=%d is out of range for rle_type (should be in the range 1 to 6)",
							result->compresslevel)));
	}

	/*
	 * A level with no type is zlib's, as Cloudberry has it; a type with no
	 * level is its first.
	 */
	if (type == AO_COMPRESS_NONE && result->compresslevel > 0 &&
		!(mine && mine->compresstype))
		type = AO_COMPRESS_ZLIB;
	if (type != AO_COMPRESS_NONE && result->compresslevel == 0)
		result->compresslevel = 1;
	result->compresstype = (int) type;

	SET_VARSIZE(result, sizeof(AoRelOptions));
	return (bytea *) result;
}

bytea *
ao_row_reloptions(Datum reloptions, char relkind, bool validate)
{
	return ao_parse_options(reloptions, relkind, validate, false);
}

bytea *
ao_column_reloptions(Datum reloptions, char relkind, bool validate)
{
	return ao_parse_options(reloptions, relkind, validate, true);
}

static void
resolve_options(const AoRelOptions *ro, AoOptions *opts)
{
	if (ro != NULL && VARSIZE(ro) == sizeof(AoRelOptions))
	{
		opts->blocksize = ro->blocksize;
		opts->compresstype = (AoCompressType) ro->compresstype;
		opts->compresslevel = ro->compresslevel;
		opts->checksum = ro->checksum;
	}
	else
	{
		opts->blocksize = AO_DEFAULT_BLOCKSIZE;
		opts->compresstype = AO_COMPRESS_NONE;
		opts->compresslevel = 0;
		opts->checksum = true;
	}
}

/* rel's options, resolved: the relcache's, or the defaults. */
void
ao_get_options(Relation rel, AoOptions *opts)
{
	resolve_options((AoRelOptions *) rel->rd_options, opts);
}

/*
 * The options pg_class.reloptions holds for a table of the method, resolved,
 * without opening the table: for pg_appendonly, which lists every one.
 */
void
ao_options_from_reloptions(Datum reloptions, char relkind, bool columnar,
						   AoOptions *opts)
{
	AoRelOptions *ro;

	ro = (AoRelOptions *) ao_parse_options(reloptions, relkind, false, columnar);
	resolve_options(ro, opts);
	pfree(ro);
}

/*
 * Give a table whose access method is about to change the options opts and
 * no others, checked as the new method checks them: SET ACCESS METHOD, as
 * Cloudberry clears a table's options when its method changes (gp_ao.c).
 */
void
ao_replace_reloptions(Relation rel, Oid newam, List *opts)
{
	static const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;
	Datum		newOptions;
	HeapTuple	amtup;
	Oid			amhandler;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		repl_val[Natts_pg_class] = {0};
	bool		repl_null[Natts_pg_class] = {0};
	bool		repl_repl[Natts_pg_class] = {0};

	newOptions = transformRelOptions((Datum) 0, opts, NULL, validnsps,
									 false, false);
	amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(newam));
	if (!HeapTupleIsValid(amtup))
		elog(ERROR, "cache lookup failed for access method %u", newam);
	amhandler = ((Form_pg_am) GETSTRUCT(amtup))->amhandler;
	ReleaseSysCache(amtup);
	(void) table_am_reloptions(GetTableAmRoutine(amhandler),
							   rel->rd_rel->relkind, newOptions, true);

	pgclass = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", RelationGetRelid(rel));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;
	repl_repl[Anum_pg_class_reloptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								 repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);
	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);
	heap_freetuple(newtuple);
	heap_freetuple(tuple);
	table_close(pgclass, RowExclusiveLock);
	CommandCounterIncrement();
}
