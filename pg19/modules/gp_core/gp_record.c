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
 * gp_record.c
 *	  A record of no declared type, as it travels between the nodes.
 *
 * A value of type record carries its row type in its header: the OID of a
 * composite type, or RECORDOID and a typmod, the number the process that
 * made the value gave the row type when it registered it (BlessTupleDesc()).
 * The number means nothing to another process, which may have given it to
 * another row type or to none, so neither the value's bytes nor
 * record_out()'s text can be read back there: record_in() and record_recv()
 * refuse a record of no declared type ("input of anonymous composite types
 * is not implemented").  Cloudberry sends the receiver each row type it has
 * not seen, and remaps the typmods of the rows it receives (tupleremap.c);
 * the port sends each such value with its row type described -- each
 * column's type, typmod, collation and name, and whether it was dropped --
 * and then its columns, each in its type's binary form where the type has
 * one and in its text otherwise, and a column that is a record of no
 * declared type described in turn.  The receiver registers the row type,
 * which BlessTupleDesc() gives a typmod of its own there, and makes the value
 * of it.  A value of a composite type with an OID travels the same way, and
 * is read back as that type, which the receiver knows by the same OID.
 *
 * gp_internal.record_wire is the type whose input, output, send and receive
 * functions do that.  GpTransferType() says a record travels as it, so a
 * Motion's rows, a gather's and those of a query of gp_dist_random() alone
 * carry one that way: the sender's output or send function of it is given
 * the record as it is, and the receiver's input or receive function makes a
 * record.  A segment's query of such a column calls
 * gp_internal.record_wire() of it, which gives the value unchanged, as that
 * type.  Its text form is its binary one in hex.  And a parameter the
 * coordinator sends a fragment is described the same way (gp_motion.c).
 *
 * A function keeps, in fn_extra, what it made of the row type it last met --
 * a Motion's rows are all of one type, most often -- and the receiver
 * compares a description as it arrives with the last one's bytes.
 *
 * Cloudberry source this file stands in for:
 *	  src/backend/cdb/motion/tupleremap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "gp_dispatch.h"

/* A column of a row type, as the wire describes it and its value travels. */
typedef struct WireColumn
{
	Oid			type;
	int32		typmod;
	bool		dropped;
	bool		record;			/* a record of no declared type, described */
	bool		binary;			/* else its text */
	FmgrInfo	proc;			/* output or send function, or input or
								 * receive function */
	Oid			ioparam;
} WireColumn;

/* A row type, as a function last met it: its fn_extra. */
typedef struct WireType
{
	Oid			type;			/* the value's, sending */
	int32		typmod;
	char	   *desc;			/* the description's bytes, receiving */
	int			desclen;
	TupleDesc	tupdesc;
	WireColumn *columns;
} WireType;

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

/* A row type of the sending process's, and how each of its columns goes. */
static WireType *
send_type(Oid type, int32 typmod, MemoryContext cxt)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
	TupleDesc	tupdesc = lookup_rowtype_tupdesc(type, typmod);
	WireType   *wt = palloc0_object(WireType);

	wt->type = type;
	wt->typmod = typmod;
	wt->tupdesc = CreateTupleDescCopy(tupdesc);
	ReleaseTupleDesc(tupdesc);
	wt->columns = palloc0_array(WireColumn, Max(wt->tupdesc->natts, 1));
	for (int i = 0; i < wt->tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(wt->tupdesc, i);
		WireColumn *col = &wt->columns[i];
		Oid			proc;
		bool		isvarlena;

		col->dropped = att->attisdropped;
		if (col->dropped)
			continue;
		col->type = att->atttypid;
		col->typmod = att->atttypmod;
		col->record = att->atttypid == RECORDOID;
		if (col->record)
			continue;
		col->binary = GpTypeHasBinaryIO(att->atttypid);
		if (col->binary)
			getTypeBinaryOutputInfo(att->atttypid, &proc, &isvarlena);
		else
			getTypeOutputInfo(att->atttypid, &proc, &isvarlena);
		fmgr_info_cxt(proc, &col->proc, cxt);
	}
	MemoryContextSwitchTo(oldcxt);
	return wt;
}

static void
wire_write(StringInfo buf, Datum value, FmgrInfo *flinfo)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	Oid			type = HeapTupleHeaderGetTypeId(td);
	int32		typmod = HeapTupleHeaderGetTypMod(td);
	WireType   *wt = flinfo != NULL ? (WireType *) flinfo->fn_extra : NULL;
	HeapTupleData tuple;
	Datum	   *values;
	bool	   *nulls;
	int			natts;

	if (wt == NULL || wt->type != type || wt->typmod != typmod)
	{
		wt = send_type(type, typmod,
					   flinfo != NULL ? flinfo->fn_mcxt : CurrentMemoryContext);
		if (flinfo != NULL)
			flinfo->fn_extra = wt;
	}
	natts = wt->tupdesc->natts;

	/* the row type */
	pq_sendint32(buf, type);
	pq_sendint16(buf, natts);
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(wt->tupdesc, i);
		WireColumn *col = &wt->columns[i];
		int			len = strlen(NameStr(att->attname));

		pq_sendbyte(buf, col->dropped ? 'd' : col->record ? 'r' : col->binary ? 'b' : 't');
		pq_sendint32(buf, col->dropped ? InvalidOid : col->type);
		pq_sendint32(buf, col->dropped ? -1 : col->typmod);
		pq_sendint32(buf, col->dropped ? InvalidOid : att->attcollation);
		pq_sendint32(buf, len);
		pq_sendbytes(buf, NameStr(att->attname), len);
	}

	/* and its columns */
	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = td;
	values = palloc_array(Datum, Max(natts, 1));
	nulls = palloc_array(bool, Max(natts, 1));
	heap_deform_tuple(&tuple, wt->tupdesc, values, nulls);
	for (int i = 0; i < natts; i++)
	{
		WireColumn *col = &wt->columns[i];

		if (col->dropped || nulls[i])
			pq_sendint32(buf, -1);
		else if (col->record)
		{
			StringInfoData inner;

			initStringInfo(&inner);
			wire_write(&inner, values[i], NULL);
			pq_sendint32(buf, inner.len);
			pq_sendbytes(buf, inner.data, inner.len);
			pfree(inner.data);
		}
		else if (col->binary)
		{
			bytea	   *b = SendFunctionCall(&col->proc, values[i]);

			pq_sendint32(buf, VARSIZE(b) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(b), VARSIZE(b) - VARHDRSZ);
		}
		else
		{
			char	   *s = OutputFunctionCall(&col->proc, values[i]);
			int			len = strlen(s);

			pq_sendint32(buf, len);
			pq_sendbytes(buf, s, len);
		}
	}
	pfree(values);
	pfree(nulls);
}

/* ------------------------------------------------------------------------- */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------- */

/* The row type a description gives, registered here, and its columns' input. */
static WireType *
receive_type(StringInfo buf, Oid type, int natts, int descstart,
			 MemoryContext cxt)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
	WireType   *wt = palloc0_object(WireType);
	TupleDesc	tupdesc;

	wt->type = type;
	wt->columns = palloc0_array(WireColumn, Max(natts, 1));
	tupdesc = CreateTemplateTupleDesc(natts);
	for (int i = 0; i < natts; i++)
	{
		WireColumn *col = &wt->columns[i];
		char		kind = pq_getmsgbyte(buf);
		Oid			coltype = pq_getmsgint(buf, 4);
		int32		typmod = pq_getmsgint(buf, 4);
		Oid			collation = pq_getmsgint(buf, 4);
		int			len = pq_getmsgint(buf, 4);
		char	   *name = pnstrdup(pq_getmsgbytes(buf, len), len);

		col->dropped = kind == 'd';
		col->record = kind == 'r';
		col->binary = kind == 'b';
		col->type = col->dropped ? INT4OID : coltype;
		/* a typmod of a record's column is the sender's own number */
		col->typmod = col->record ? -1 : typmod;
		TupleDescInitEntry(tupdesc, i + 1, name, col->type, col->typmod, 0);
		TupleDescInitEntryCollation(tupdesc, i + 1, collation);
		if (col->dropped)
		{
			TupleDescAttr(tupdesc, i)->attisdropped = true;
			populate_compact_attribute(tupdesc, i);
		}
	}
	TupleDescFinalize(tupdesc);

	if (type == RECORDOID)
		wt->tupdesc = BlessTupleDesc(tupdesc);
	else
	{
		/* a composite type's own, which the description has to match */
		TupleDesc	own = lookup_rowtype_tupdesc(type, -1);

		if (own->natts != natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("a value of type %s arrived with %d columns, where the type has %d",
							format_type_be(type), natts, own->natts)));
		wt->tupdesc = CreateTupleDescCopy(own);
		ReleaseTupleDesc(own);
	}

	for (int i = 0; i < natts; i++)
	{
		WireColumn *col = &wt->columns[i];
		Oid			proc;

		if (col->dropped || col->record)
			continue;
		if (col->binary)
			getTypeBinaryInputInfo(col->type, &proc, &col->ioparam);
		else
			getTypeInputInfo(col->type, &proc, &col->ioparam);
		fmgr_info_cxt(proc, &col->proc, cxt);
	}

	wt->desclen = buf->cursor - descstart;
	wt->desc = palloc(Max(wt->desclen, 1));
	memcpy(wt->desc, buf->data + descstart, wt->desclen);
	MemoryContextSwitchTo(oldcxt);
	return wt;
}

/* Skip a description whose bytes are known, returning where it ends. */
static int
description_end(StringInfo buf, int natts)
{
	int			cursor = buf->cursor;

	for (int i = 0; i < natts; i++)
	{
		int32		len;

		cursor += 1 + 4 + 4 + 4;
		if (cursor + 4 > buf->len)
			return -1;
		memcpy(&len, buf->data + cursor, 4);
		cursor += 4 + (int) pg_ntoh32(len);
	}
	return cursor <= buf->len ? cursor : -1;
}

static Datum
wire_read(StringInfo buf, FmgrInfo *flinfo)
{
	int			descstart = buf->cursor;
	Oid			type = pq_getmsgint(buf, 4);
	int			natts = pq_getmsgint(buf, 2);
	WireType   *wt = flinfo != NULL ? (WireType *) flinfo->fn_extra : NULL;
	int			end = description_end(buf, natts);
	Datum	   *values;
	bool	   *nulls;
	HeapTuple	tuple;

	if (end < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("a record's description is cut short")));
	if (wt != NULL && wt->type == type && wt->desclen == end - descstart &&
		memcmp(wt->desc, buf->data + descstart, wt->desclen) == 0)
		buf->cursor = end;
	else
	{
		wt = receive_type(buf, type, natts, descstart,
						  flinfo != NULL ? flinfo->fn_mcxt : CurrentMemoryContext);
		if (flinfo != NULL)
			flinfo->fn_extra = wt;
	}

	values = palloc_array(Datum, Max(natts, 1));
	nulls = palloc_array(bool, Max(natts, 1));
	for (int i = 0; i < natts; i++)
	{
		WireColumn *col = &wt->columns[i];
		int			len = pq_getmsgint(buf, 4);
		StringInfoData item;

		nulls[i] = len < 0;
		values[i] = (Datum) 0;
		if (nulls[i])
			continue;
		initReadOnlyStringInfo(&item, unconstify(char *, pq_getmsgbytes(buf, len)), len);
		if (col->record)
			values[i] = wire_read(&item, NULL);
		else if (col->binary)
			values[i] = ReceiveFunctionCall(&col->proc, &item, col->ioparam,
											col->typmod);
		else
		{
			values[i] = InputFunctionCall(&col->proc, pnstrdup(item.data, len),
										  col->ioparam, col->typmod);
			item.cursor = item.len;
		}
		if (item.cursor != item.len)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("improper binary format in a record's column %d", i + 1)));
	}
	tuple = heap_form_tuple(wt->tupdesc, values, nulls);
	pfree(values);
	pfree(nulls);
	return HeapTupleGetDatum(tuple);
}

/* ------------------------------------------------------------------------- */
/* The type, and what the rest of gp_core calls                              */
/* ------------------------------------------------------------------------- */

/*
 * The OID of gp_internal.record_wire in this database, InvalidOid where
 * gp_core is not installed.
 */
Oid
GpRecordWireType(void)
{
	Oid			nsp = get_namespace_oid("gp_internal", true);

	if (!OidIsValid(nsp))
		return InvalidOid;
	return GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   CStringGetDatum("record_wire"),
						   ObjectIdGetDatum(nsp));
}

/* A record, described, onto "buf". */
void
GpRecordWireWrite(StringInfo buf, Datum record)
{
	wire_write(buf, record, NULL);
}

/* And made again here, of a row type registered here. */
Datum
GpRecordWireRead(StringInfo buf)
{
	return wire_read(buf, NULL);
}

PG_FUNCTION_INFO_V1(gp_record_from_wire);
PG_FUNCTION_INFO_V1(gp_record_wire_in);
PG_FUNCTION_INFO_V1(gp_record_wire_out);
PG_FUNCTION_INFO_V1(gp_record_wire_recv);
PG_FUNCTION_INFO_V1(gp_record_wire_send);
PG_FUNCTION_INFO_V1(gp_record_wire);

/* gp_internal.record_wire_in(cstring, oid, int4): the hex of the binary form */
Datum
gp_record_wire_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
	size_t		len = strlen(s);
	StringInfoData buf;
	Datum		result;

	if (len % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "gp_internal.record_wire")));
	initStringInfo(&buf);
	enlargeStringInfo(&buf, len / 2);
	buf.len = hex_decode(s, len, buf.data);
	buf.data[buf.len] = '\0';
	result = wire_read(&buf, fcinfo->flinfo);
	if (buf.cursor != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "gp_internal.record_wire")));
	PG_RETURN_DATUM(result);
}

/* gp_internal.record_wire_out(record_wire) */
Datum
gp_record_wire_out(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	char	   *hex;

	initStringInfo(&buf);
	wire_write(&buf, PG_GETARG_DATUM(0), fcinfo->flinfo);
	hex = palloc(buf.len * 2 + 1);
	hex[hex_encode(buf.data, buf.len, hex)] = '\0';
	PG_RETURN_CSTRING(hex);
}

/* gp_internal.record_wire_recv(internal, oid, int4) */
Datum
gp_record_wire_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_DATUM(wire_read(buf, fcinfo->flinfo));
}

/* gp_internal.record_wire_send(record_wire) */
Datum
gp_record_wire_send(PG_FUNCTION_ARGS)
{
	StringInfoData buf;

	pq_begintypsend(&buf);
	wire_write(&buf, PG_GETARG_DATUM(0), fcinfo->flinfo);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* What gp_record_from_wire() keeps of the last value it made: its fn_extra. */
typedef struct FromWire
{
	text	   *hex;
	Datum		value;
} FromWire;

/*
 * gp_internal.record_from_wire(text) -> record: a record of no declared type
 * made from record_wire's text of it, of a row type registered here.  ORCA's
 * constant folding makes a constant of a parameter, and of an immutable
 * function's call, and a record of no declared type as a constant would reach
 * a segment with the coordinator's typmod: in the plan, a call of this takes
 * its place (orca.c), which makes the value again wherever it is evaluated --
 * once in each process, the value kept for the next row.
 */
Datum
gp_record_from_wire(PG_FUNCTION_ARGS)
{
	text	   *hex = PG_GETARG_TEXT_PP(0);
	FromWire   *last = (FromWire *) fcinfo->flinfo->fn_extra;

	if (last == NULL || VARSIZE_ANY(last->hex) != VARSIZE_ANY(hex) ||
		memcmp(VARDATA_ANY(last->hex), VARDATA_ANY(hex), VARSIZE_ANY_EXHDR(hex)) != 0)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		char	   *s = text_to_cstring(hex);
		size_t		len = strlen(s);
		StringInfoData buf;

		if (len % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "gp_internal.record_wire")));
		initStringInfo(&buf);
		enlargeStringInfo(&buf, len / 2);
		buf.len = hex_decode(s, len, buf.data);
		buf.data[buf.len] = '\0';
		if (last == NULL)
			last = palloc0_object(FromWire);
		last->value = wire_read(&buf, NULL);
		last->hex = (text *) PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(0));
		fcinfo->flinfo->fn_extra = last;
		MemoryContextSwitchTo(oldcxt);
	}
	PG_RETURN_DATUM(datumCopy(last->value, false, -1));
}

/*
 * gp_internal.record_wire(record) -> record_wire: the value as it is, which
 * a segment's query calls of a column of a record of no declared type, to
 * send it as record_wire.
 */
Datum
gp_record_wire(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}
