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
 * copysreh.c
 *	  COPY FROM with the options of Cloudberry's that PostgreSQL 19's COPY
 *	  has not: [LOG ERRORS] SEGMENT REJECT LIMIT n [ROWS | PERCENT], FILL
 *	  MISSING FIELDS, NEWLINE, and text's ESCAPE 'OFF' or an escape of its
 *	  own.
 *
 * Cloudberry's COPY catches a line's data error -- its format, a field too
 * many or too few, a value its column's type refuses, a row no partition
 * takes -- logs the line and goes on, until the reject limit is reached
 * (copyfrom.c's error handling, cdbsreh.c); fills the columns a short line
 * has no field for with NULLs; ends a line only where NEWLINE says; and
 * reads text with an escape of the user's, or with none.  PostgreSQL 19's
 * COPY has none of it: its ON_ERROR catches a type's refusal alone.  So the
 * data is read twice.  Here, by PostgreSQL's COPY parser, a line at a time
 * -- its line ending the one NEWLINE names, the data made over first for an
 * escape (raw_made_read()) -- and each line's fields tried as its columns'
 * values, as an external table's scan tries them (extaccess.c); and the
 * lines that pass, completed where fields are missing, by PostgreSQL's own
 * COPY into the table, which gp_core's COPY runs (gp_modify.c) with
 * loader_read() as its data source.  A constraint's refusal is no data
 * error, and fails the COPY there, as it fails Cloudberry's.
 *
 * The loading COPY numbers each line as the data does, its line number set
 * as the line is handed to it, so that its errors -- and a segment's, of a
 * row it routes there -- say the line of the user's data.
 *
 * Cloudberry sources this file stands for:
 *	  src/backend/commands/copyfrom.c (HandleCopyError), copyfromparse.c
 *	  (FILL MISSING FIELDS, NEWLINE, ESCAPE 'OFF'), cdb/cdbsreh.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/stat.h>

#include "access/table.h"
#include "access/tupconvert.h"
#include "catalog/pg_class.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "storage/fd.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"

#include "gp_exttable.h"

/* How much of the data is read at a time where this reads it. */
#define RAW_CHUNK	32768

/*
 * The data, where this reads it rather than PostgreSQL's COPY: the file, the
 * program or the client, opened and read as COPY FROM opens and reads them
 * (copyfrom.c's BeginCopyFrom() and ClosePipeFromProgram(), copyfromparse.c's
 * ReceiveCopyBegin() and CopyGetData(), which PostgreSQL 19 keeps static).
 */
typedef struct RawSource
{
	FILE	   *file;			/* the file, the program's pipe, or stdin */
	const char *filename;
	bool		is_program;
	bool		from_client;	/* the client's COPY data */
	StringInfo	fe_msgbuf;		/* its CopyData being read */
	bool		eof;
} RawSource;

/* A partitioned table a row is routed through: see route_row(). */
typedef struct PartLevel
{
	Relation	rel;
	PartitionKey key;
	PartitionDesc desc;
	List	   *keystate;		/* its key's expressions, prepared */
	TupleTableSlot *slot;		/* the row, in its columns */
	AttrMap    *map;			/* the table's columns to its own, or NULL */
	int		   *needs;			/* the table's columns its key reads */
	int			nneeds;
} PartLevel;

typedef struct PartRoute
{
	EState	   *estate;
	PartitionDirectory pdir;
	List	   *levels;			/* PartLevel, the table's own first */
	bool	   *given;			/* a column the data gives */
	bool	   *pending;		/* a column whose value is its default */
	bool	   *volatile_default;
} PartRoute;

typedef struct SrehCopy
{
	CopyFromState cstate;		/* the data, as it is read */
	CopyFromState loader;		/* the COPY that loads the lines that pass */
	CdbSreh    *sreh;			/* NULL: a line's error fails the COPY */
	bool		fill_missing;	/* FILL MISSING FIELDS */
	EolType		newline;		/* NEWLINE, or EOL_UNKNOWN */
	bool		escape_off;		/* text's ESCAPE 'OFF' */
	char		escape_char;	/* text's escape of its own, or none */
	RawSource  *raw;			/* the data, where made over for an escape */
	char	   *rawbuf;
	StringInfoData made;		/* made over, not yet read */
	int			made_off;
	StringInfoData ready;		/* the line handed on, not yet read */
	int			ready_off;
	uint64		ready_lineno;	/* the line number to give it */
	bool		eof;
	Datum	   *values;
	bool	   *nulls;
	MemoryContext linecxt;		/* a line's values, reset at the next */
	PartRoute  *route;			/* SEGMENT REJECT LIMIT, partitioned table */
	bool		ended;
	struct SrehCopy *outer;		/* the one being read before this one */
} SrehCopy;

/*
 * The data source callbacks take no argument of their caller's (PostgreSQL
 * 19's copy_data_source_cb), and a COPY reads one source at a time: the one
 * being read.
 */
static SrehCopy *reading = NULL;

PGDLLEXPORT void *GpSrehCopyBegin(ParseState *pstate, Relation rel,
								  const char *filename, bool is_program,
								  List *attlist, List *options,
								  int reject_limit, bool limit_in_rows,
								  char log_errors, CopyFromState *loader);
PGDLLEXPORT uint64 GpSrehCopyEnd(void *state);

/* ------------------------------------------------------------------------- */
/* The data, where this reads it                                             */
/* ------------------------------------------------------------------------- */

/* Open it: the file, the program, or the client's COPY data. */
static RawSource *
raw_open(Relation rel, const char *filename, bool is_program, List *attlist)
{
	RawSource  *raw = palloc0_object(RawSource);

	raw->filename = filename;
	raw->is_program = is_program;
	if (filename == NULL)
	{
		if (whereToSendOutput == DestRemote)
		{
			int			natts = list_length(CopyGetAttnums(RelationGetDescr(rel),
														   rel, attlist));
			StringInfoData buf;

			/* CopyInResponse, text, as ReceiveCopyBegin() sends it */
			pq_beginmessage(&buf, PqMsg_CopyInResponse);
			pq_sendbyte(&buf, 0);
			pq_sendint16(&buf, natts);
			for (int i = 0; i < natts; i++)
				pq_sendint16(&buf, 0);
			pq_endmessage(&buf);
			pq_flush();
			raw->from_client = true;
			raw->fe_msgbuf = makeStringInfo();
		}
		else
			raw->file = stdin;
	}
	else if (is_program)
	{
		raw->file = OpenPipeStream(filename, PG_BINARY_R);
		if (raw->file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m", filename)));
	}
	else
	{
		struct stat st;

		raw->file = AllocateFile(filename, PG_BINARY_R);
		if (raw->file == NULL)
		{
			int			save_errno = errno;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m", filename),
					 (save_errno == ENOENT || save_errno == EACCES) ?
					 errhint("COPY FROM instructs the PostgreSQL server process to read a file. "
							 "You may want a client-side facility such as psql's \\copy.") : 0));
		}
		if (fstat(fileno(raw->file), &st))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", filename)));
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", filename)));
	}
	return raw;
}

/* Up to len bytes of it into buf: none at its end. */
static int
raw_read(RawSource *raw, char *buf, int len)
{
	int			n;

	if (raw->eof)
		return 0;
	if (!raw->from_client)
	{
		n = fread(buf, 1, len, raw->file);
		if (ferror(raw->file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from COPY file: %m")));
		if (n == 0)
			raw->eof = true;
		return n;
	}

	while (raw->fe_msgbuf->cursor >= raw->fe_msgbuf->len)
	{
		int			mtype;
		int			maxmsglen;

readmessage:
		HOLD_CANCEL_INTERRUPTS();
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype == EOF)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		switch (mtype)
		{
			case PqMsg_CopyData:
				maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
				break;
			case PqMsg_CopyDone:
			case PqMsg_CopyFail:
			case PqMsg_Flush:
			case PqMsg_Sync:
				maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected message type 0x%02X during COPY from stdin",
								mtype)));
				maxmsglen = 0;	/* keep compiler quiet */
				break;
		}
		if (pq_getmessage(raw->fe_msgbuf, maxmsglen))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		RESUME_CANCEL_INTERRUPTS();
		switch (mtype)
		{
			case PqMsg_CopyData:
				break;
			case PqMsg_CopyDone:
				raw->eof = true;
				return 0;
			case PqMsg_CopyFail:
				ereport(ERROR,
						(errcode(ERRCODE_QUERY_CANCELED),
						 errmsg("COPY from stdin failed: %s",
								pq_getmsgstring(raw->fe_msgbuf))));
				break;
			case PqMsg_Flush:
			case PqMsg_Sync:
				/* what a client library may send, not noticing the COPY */
				goto readmessage;
		}
	}
	n = Min(len, raw->fe_msgbuf->len - raw->fe_msgbuf->cursor);
	pq_copymsgbytes(raw->fe_msgbuf, buf, n);
	return n;
}

static void
raw_close(RawSource *raw)
{
	if (raw->is_program)
	{
		int			pclose_rc = ClosePipeStream(raw->file);

		if (pclose_rc == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close pipe to external command: %m")));
		else if (pclose_rc != 0)
		{
			/* a program stopped before its end may fail with SIGPIPE */
			if (!raw->eof && wait_result_is_signal(pclose_rc, SIGPIPE))
				return;
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("program \"%s\" failed", raw->filename),
					 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
		}
	}
	else if (raw->file != NULL && raw->file != stdin && FreeFile(raw->file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", raw->filename)));
}

/*
 * The reading COPY's data: the data made over for text's escape, as an
 * external table's is (extaccess.c): each backslash doubled, and the escape
 * made the backslash -- so that COPY's text, whose escape is the backslash
 * always, reads a backslash as itself and the escape as its own.  A
 * character's other bytes embed no ASCII in the encoding, which was checked
 * as the COPY began.  A line \. is made data too, where Cloudberry's ends
 * the data at it; a client's COPY ends without one, which psql leaves out.
 */
static int
raw_made_read(void *outbuf, int minread, int maxread)
{
	SrehCopy   *sc = reading;
	StringInfo	made = &sc->made;
	int			n;

	Assert(sc != NULL && sc->raw != NULL);
	while (made->len - sc->made_off < minread && !sc->raw->eof)
	{
		int			got = raw_read(sc->raw, sc->rawbuf, RAW_CHUNK);

		if (sc->made_off > 0)
		{
			memmove(made->data, made->data + sc->made_off, made->len - sc->made_off);
			made->len -= sc->made_off;
			made->data[made->len] = '\0';
			sc->made_off = 0;
		}
		enlargeStringInfo(made, 2 * got);
		for (int i = 0; i < got; i++)
		{
			char		c = sc->rawbuf[i];

			if (c == '\\')
				made->data[made->len++] = '\\';
			else if (sc->escape_char != '\0' && c == sc->escape_char)
				c = '\\';
			made->data[made->len++] = c;
		}
		made->data[made->len] = '\0';
		CHECK_FOR_INTERRUPTS();
	}
	n = Min(maxread, made->len - sc->made_off);
	memcpy(outbuf, made->data + sc->made_off, n);
	sc->made_off += n;
	return n;
}

/* ------------------------------------------------------------------------- */
/* A row no partition takes                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Under SEGMENT REJECT LIMIT a row that no partition of the table takes is
 * a data error, which Cloudberry's COPY rejects: PostgreSQL's COPY fails at
 * it (ExecFindPartition()).  So the row is routed here first, as
 * ExecFindPartition() routes it -- the partition key of each partitioned
 * table on its way, and the partition whose bounds take it
 * (get_partition_for_tuple(), which PostgreSQL 19 keeps static) -- without
 * the executor's result relations, which would open each partition's
 * indexes and begin a foreign partition's insert.  A column the data leaves
 * to its default is given it only where a key reads it; a volatile default,
 * which the loading COPY evaluates again, leaves the row to the loading COPY.
 */
static PartLevel *
route_level(PartRoute *route, Relation root, Relation rel)
{
	PartLevel  *level = palloc0_object(PartLevel);
	TupleDesc	rootdesc = RelationGetDescr(root);
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *attnos = NULL;
	int			attno = -1;

	level->rel = rel;
	level->key = RelationGetPartitionKey(rel);
	level->desc = PartitionDirectoryLookup(route->pdir, rel);
	if (level->key->partexprs != NIL)
		level->keystate = ExecPrepareExprList(level->key->partexprs, route->estate);
	if (rel != root)
	{
		level->map = build_attrmap_by_name_if_req(rootdesc, desc, false);
		level->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	}

	/* the table's columns the key reads, found by name */
	for (int i = 0; i < level->key->partnatts; i++)
		if (level->key->partattrs[i] != 0)
			attnos = bms_add_member(attnos, level->key->partattrs[i] -
									FirstLowInvalidHeapAttributeNumber);
	pull_varattnos((Node *) level->key->partexprs, 1, &attnos);
	level->needs = palloc_array(int, Max(bms_num_members(attnos), 1));
	while ((attno = bms_next_member(attnos, attno)) >= 0)
	{
		AttrNumber	a = attno + FirstLowInvalidHeapAttributeNumber;
		const char *name;

		if (a <= 0)
			continue;
		name = NameStr(TupleDescAttr(desc, a - 1)->attname);
		for (int m = 0; m < rootdesc->natts; m++)
			if (!TupleDescAttr(rootdesc, m)->attisdropped &&
				strcmp(NameStr(TupleDescAttr(rootdesc, m)->attname), name) == 0)
				level->needs[level->nneeds++] = m;
	}
	route->levels = lappend(route->levels, level);
	return level;
}

static PartRoute *
route_begin(SrehCopy *sc)
{
	CopyFromState cstate = sc->cstate;
	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	PartRoute  *route = palloc0_object(PartRoute);
	PartLevel  *root;

	route->estate = CreateExecutorState();
	route->pdir = CreatePartitionDirectory(CurrentMemoryContext, false);
	route->given = palloc0_array(bool, tupDesc->natts);
	route->pending = palloc0_array(bool, tupDesc->natts);
	route->volatile_default = palloc0_array(bool, tupDesc->natts);
	foreach_int(attnum, cstate->attnumlist)
		route->given[attnum - 1] = true;
	for (int m = 0; m < tupDesc->natts; m++)
		if (cstate->defexprs[m] != NULL)
			route->volatile_default[m] =
				contain_volatile_functions((Node *) cstate->defexprs[m]->expr);
	root = route_level(route, cstate->rel, cstate->rel);
	root->slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsVirtual);
	return route;
}

static void
route_end(PartRoute *route)
{
	foreach_ptr(PartLevel, level, route->levels)
	{
		ExecDropSingleTupleTableSlot(level->slot);
		if (level != linitial(route->levels))
			table_close(level->rel, NoLock);
	}
	DestroyPartitionDirectory(route->pdir);
	FreeExecutorState(route->estate);
}

/* The index of the partition that takes the key, or -1. */
static int
route_partition(PartLevel *level, Datum *values, bool *isnull)
{
	PartitionKey key = level->key;
	PartitionBoundInfo boundinfo = level->desc->boundinfo;
	int			part_index = -1;

	if (level->desc->nparts == 0)
		return -1;
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				uint64		rowHash = compute_partition_hash_value(key->partnatts,
																   key->partsupfunc,
																   key->partcollation,
																   values, isnull);

				return boundinfo->indexes[rowHash % boundinfo->nindexes];
			}
		case PARTITION_STRATEGY_LIST:
			if (isnull[0])
			{
				if (partition_bound_accepts_nulls(boundinfo))
					return boundinfo->null_index;
			}
			else
			{
				bool		equal;
				int			bound_offset = partition_list_bsearch(key->partsupfunc,
																  key->partcollation,
																  boundinfo,
																  values[0], &equal);

				if (bound_offset >= 0 && equal)
					part_index = boundinfo->indexes[bound_offset];
			}
			break;
		case PARTITION_STRATEGY_RANGE:
			{
				bool		equal = false;
				bool		hasnull = false;

				for (int i = 0; i < key->partnatts; i++)
					hasnull |= isnull[i];
				/* a NULL is in no range, and goes to the default if any */
				if (!hasnull)
				{
					int			bound_offset = partition_range_datum_bsearch(key->partsupfunc,
																			 key->partcollation,
																			 boundinfo,
																			 key->partnatts,
																			 values, &equal);

					part_index = boundinfo->indexes[bound_offset + 1];
				}
			}
			break;
	}
	if (part_index < 0)
		part_index = boundinfo->default_index;
	return part_index;
}

/*
 * Whether a partition takes the row in sc->values and sc->nulls -- true too
 * where it cannot be known here.
 */
static bool
route_row(SrehCopy *sc)
{
	PartRoute  *route = sc->route;
	CopyFromState cstate = sc->cstate;
	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	PartLevel  *level = linitial(route->levels);
	TupleTableSlot *rootslot = level->slot;
	ExprContext *econtext = GetPerTupleExprContext(route->estate);

	ResetPerTupleExprContext(route->estate);
	ExecClearTuple(rootslot);
	for (int m = 0; m < tupDesc->natts; m++)
	{
		rootslot->tts_values[m] = sc->values[m];
		rootslot->tts_isnull[m] = sc->nulls[m];
		route->pending[m] = cstate->defexprs[m] != NULL &&
			(cstate->defaults[m] || !route->given[m]);
	}
	ExecStoreVirtualTuple(rootslot);

	for (;;)
	{
		Datum		values[PARTITION_MAX_KEYS];
		bool		isnull[PARTITION_MAX_KEYS];
		TupleTableSlot *slot = rootslot;
		ListCell   *expr = list_head(level->keystate);
		int			index;

		for (int i = 0; i < level->nneeds; i++)
		{
			int			m = level->needs[i];

			if (!route->pending[m])
				continue;
			if (route->volatile_default[m])
				return true;
			rootslot->tts_values[m] =
				ExecEvalExprSwitchContext(cstate->defexprs[m], econtext,
										  &rootslot->tts_isnull[m]);
			route->pending[m] = false;
		}
		if (level->slot != rootslot)
			slot = level->map ?
				execute_attr_map_slot(level->map, rootslot, level->slot) :
				rootslot;

		econtext->ecxt_scantuple = slot;
		for (int i = 0; i < level->key->partnatts; i++)
		{
			if (level->key->partattrs[i] != 0)
				values[i] = slot_getattr(slot, level->key->partattrs[i], &isnull[i]);
			else
			{
				values[i] = ExecEvalExprSwitchContext((ExprState *) lfirst(expr),
													  econtext, &isnull[i]);
				expr = lnext(level->keystate, expr);
			}
		}

		index = route_partition(level, values, isnull);
		if (index < 0)
			return false;
		if (level->desc->is_leaf[index])
			return true;

		/* a partitioned partition: the next level, met before or not */
		{
			Oid			child = level->desc->oids[index];
			PartLevel  *next = NULL;

			foreach_ptr(PartLevel, l, route->levels)
				if (RelationGetRelid(l->rel) == child)
					next = l;
			if (next == NULL)
				next = route_level(route, cstate->rel,
								   table_open(child, RowExclusiveLock));
			level = next;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* The lines                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * A line's fields as its columns' values, as NextCopyFrom() makes them, with
 * FILL MISSING FIELDS.  Under SEGMENT REJECT LIMIT, "message" is given:
 * false, and the message, for a field its type refuses, a field too many
 * or too few, or a row no partition takes.  Without it a line's error fails
 * the COPY, as COPY's own does.  The values are only tried: the COPY that
 * loads the line makes them again, defaults and all.
 */
static bool
fields_load(SrehCopy *sc, char **fields, int nfields, char **message)
{
	CopyFromState cstate = sc->cstate;
	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	int			natts = list_length(cstate->attnumlist);
	int			fieldno = 0;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	escontext.details_wanted = true;

	if (nfields > natts)
	{
		if (message == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		*message = pstrdup("extra data after last expected column");
		return false;
	}

	/*
	 * FILL MISSING FIELDS fills a short line, but not an empty one where
	 * more columns than one are read; Cloudberry's message names the
	 * table's second column, whichever are read.
	 */
	if (sc->fill_missing && cstate->line_buf.len == 0 && natts > 1)
	{
		const char *name = NameStr(TupleDescAttr(tupDesc, 1)->attname);

		if (message == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("missing data for column \"%s\", found empty data line",
							name)));
		*message = psprintf("missing data for column \"%s\", found empty data line",
							name);
		return false;
	}

	MemSet(sc->nulls, true, tupDesc->natts * sizeof(bool));
	MemSet(sc->values, 0, tupDesc->natts * sizeof(Datum));
	foreach_int(attnum, cstate->attnumlist)
	{
		int			m = attnum - 1;
		Form_pg_attribute att = TupleDescAttr(tupDesc, m);
		char	   *string = NULL;

		if (fieldno < nfields)
			string = fields[fieldno++];
		else if (!sc->fill_missing)
		{
			/* the message names the column; no other is being read */
			cstate->cur_attname = NULL;
			if (message == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("missing data for column \"%s\"",
								NameStr(att->attname))));
			*message = psprintf("missing data for column \"%s\"",
								NameStr(att->attname));
			return false;
		}

		if (cstate->opts.format == COPY_FORMAT_CSV)
		{
			if (string == NULL && cstate->opts.force_notnull_flags[m])
				string = cstate->opts.null_print;
			else if (string != NULL && cstate->opts.force_null_flags[m] &&
					 strcmp(string, cstate->opts.null_print) == 0)
				string = NULL;
		}

		/* the DEFAULT marker: the column's default, the loading COPY's */
		if (cstate->defaults[m])
			continue;

		cstate->cur_attname = NameStr(att->attname);
		cstate->cur_attval = string;
		if (message == NULL)
			sc->values[m] = InputFunctionCall(&cstate->in_functions[m], string,
											  cstate->typioparams[m],
											  att->atttypmod);
		else if (!InputFunctionCallSafe(&cstate->in_functions[m], string,
										cstate->typioparams[m], att->atttypmod,
										(Node *) &escontext, &sc->values[m]))
		{
			*message = escontext.error_data ?
				psprintf("%s, column %s", escontext.error_data->message,
						 NameStr(att->attname)) :
				pstrdup("invalid input");
			return false;
		}
		sc->nulls[m] = (string == NULL);
	}
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;

	if (message != NULL && sc->route != NULL && !route_row(sc))
	{
		*message = psprintf("no partition of relation \"%s\" found for row",
							RelationGetRelationName(cstate->rel));
		return false;
	}
	return true;
}

/* A line refused: logged, if the COPY logs errors, and counted. */
static void
reject_line(SrehCopy *sc, const char *message)
{
	CdbSreh    *sreh = sc->sreh;
	CopyFromState cstate = sc->cstate;
	MemoryContext old = MemoryContextSwitchTo(sreh->badrowcontext);

	MemoryContextReset(sreh->badrowcontext);
	sreh->errmsg = pstrdup(message);
	resetStringInfo(sreh->rawdata);
	appendBinaryStringInfo(sreh->rawdata, cstate->line_buf.data, cstate->line_buf.len);
	while (sreh->rawdata->len > 0 &&
		   (sreh->rawdata->data[sreh->rawdata->len - 1] == '\n' ||
			sreh->rawdata->data[sreh->rawdata->len - 1] == '\r'))
		sreh->rawdata->data[--sreh->rawdata->len] = '\0';
	sreh->linenumber = cstate->cur_lineno;
	MemoryContextSwitchTo(old);

	/*
	 * Past the limit, the error says the line, and its column where a field
	 * failed, as COPY's context does (the caller's CopyFromErrorCallback()).
	 */
	HandleSingleRowError(sreh);
	ErrorIfRejectLimitReached(sreh);
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
}

/*
 * A line that passed, into sc->ready for the loading COPY: as it was read,
 * in the server's encoding, and with FILL MISSING FIELDS a NULL for each
 * field it has not -- the NULL string after the delimiter, which COPY reads
 * as Cloudberry's fills a column: NULL, or under FORCE NOT NULL the string.
 */
static void
ready_line(SrehCopy *sc, int nfields)
{
	CopyFromState cstate = sc->cstate;
	StringInfo	line = &sc->ready;
	int			natts = list_length(cstate->attnumlist);
	uint64		newlines = 0;

	appendBinaryStringInfo(line, cstate->line_buf.data, cstate->line_buf.len);
	for (int i = nfields; sc->fill_missing && i < natts; i++)
	{
		appendStringInfoString(line, cstate->opts.delim);
		appendStringInfoString(line, cstate->opts.null_print);
	}

	/*
	 * The loading COPY counts the newlines in a CSV line's quoted fields, as
	 * this one's reading counted its own: its line starts that many before
	 * where this reading ended, and ends where it did.
	 */
	if (cstate->opts.format == COPY_FORMAT_CSV)
		for (int i = 0; i < line->len; i++)
			newlines += (line->data[i] == '\n');
	sc->ready_lineno = cstate->cur_lineno - newlines;
	appendStringInfoChar(line, '\n');
}

/*
 * The next line: into sc->ready if it passes, false at the end of the
 * data.  Under SEGMENT REJECT LIMIT a data error of the line's is caught, as
 * Cloudberry's COPY catches it (its class, 22); any other kind is the
 * statement's.
 */
static bool
next_line(SrehCopy *sc)
{
	CopyFromState cstate = sc->cstate;
	char	  **fields = NULL;
	int			nfields = 0;
	bool		got;
	char	   *message = NULL;
	ErrorContextCallback errcallback;
	MemoryContext old = MemoryContextSwitchTo(sc->linecxt);

	MemoryContextReset(sc->linecxt);

	/* the DEFAULT markers of a line, which NextCopyFrom() resets */
	if (cstate->opts.default_print != NULL)
		MemSet(cstate->defaults, false,
			   RelationGetDescr(cstate->rel)->natts * sizeof(bool));

	/* An error says where in the data it is, as COPY's own does. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	if (sc->sreh == NULL)
	{
		got = NextCopyFromRawFields(cstate, &fields, &nfields);
		if (got)
			(void) fields_load(sc, fields, nfields, NULL);
	}
	else
	{
		PG_TRY();
		{
			got = NextCopyFromRawFields(cstate, &fields, &nfields);
			if (got)
				(void) fields_load(sc, fields, nfields, &message);
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			MemoryContextSwitchTo(sc->linecxt);
			edata = CopyErrorData();
			if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
				PG_RE_THROW();
			FlushErrorState();
			message = pstrdup(edata->message);
			got = true;
		}
		PG_END_TRY();
	}
	MemoryContextSwitchTo(old);

	if (got)
	{
		if (sc->sreh != NULL)
			sc->sreh->processed++;
		if (message != NULL)
			reject_line(sc, message);
		else
			ready_line(sc, nfields);
	}
	error_context_stack = errcallback.previous;
	return got;
}

/*
 * The loading COPY's data: a line at a time, each given the number it has
 * in the data as the COPY begins it -- the COPY counts a line before it
 * reads it, and asks for more only once it has read the last.  Its own
 * context, a line of its own counting, is left out of an error of reading,
 * which says where in the data it is.
 */
static int
loader_read(void *outbuf, int minread, int maxread)
{
	SrehCopy   *sc = reading;
	ErrorContextCallback *outer = error_context_stack;
	int			n;

	Assert(sc != NULL);
	if (sc->ready_off == sc->ready.len)
	{
		if (outer != NULL && outer->callback == CopyFromErrorCallback &&
			outer->arg == sc->loader)
			error_context_stack = outer->previous;
		resetStringInfo(&sc->ready);
		sc->ready_off = 0;
		while (sc->ready.len == 0 && !sc->eof)
		{
			CHECK_FOR_INTERRUPTS();
			if (!next_line(sc))
				sc->eof = true;
		}
		error_context_stack = outer;
		if (sc->ready.len > 0)
			sc->loader->cur_lineno = sc->ready_lineno;
	}
	n = Min(maxread, sc->ready.len - sc->ready_off);
	memcpy(outbuf, sc->ready.data + sc->ready_off, n);
	sc->ready_off += n;
	return n;
}

/* A COPY that failed leaves the one before it being read. */
static void
sreh_reset(void *arg)
{
	SrehCopy   *sc = (SrehCopy *) arg;

	if (!sc->ended && reading == sc)
		reading = sc->outer;
}

/*
 * Begin: the options of Cloudberry's taken out -- FILL MISSING FIELDS,
 * NEWLINE, and in text ESCAPE, which PostgreSQL's takes in CSV alone -- the
 * data opened, as COPY FROM opens it or, for an escape, here; and the COPY
 * that loads the lines made, with the rest of the options, less HEADER,
 * which was read here, and in the server's encoding, which the lines are in
 * once read here.  reject_limit is -1 without SEGMENT REJECT LIMIT.
 */
void *
GpSrehCopyBegin(ParseState *pstate, Relation rel, const char *filename,
				bool is_program, List *attlist, List *options,
				int reject_limit, bool limit_in_rows, char log_errors,
				CopyFromState *loader)
{
	SrehCopy   *sc = palloc0_object(SrehCopy);
	List	   *copyopts = NIL;
	List	   *load_options = NIL;
	TupleDesc	tupDesc = RelationGetDescr(rel);
	bool		text = true;
	bool		escape = false;
	int			file_encoding = pg_get_client_encoding();
	MemoryContextCallback *cb;

	foreach_node(DefElem, def, options)
		if (strcmp(def->defname, "format") == 0)
			text = (strcmp(defGetString(def), "text") == 0);

	foreach_node(DefElem, def, options)
	{
		if (strcmp(def->defname, "fill_missing_fields") == 0)
		{
			if (sc->fill_missing)
				errorConflictingDefElem(def, pstate);
			sc->fill_missing = defGetBoolean(def);
			continue;
		}
		if (strcmp(def->defname, "newline") == 0)
		{
			char	   *eol = defGetString(def);

			if (sc->newline != EOL_UNKNOWN)
				errorConflictingDefElem(def, pstate);
			if (pg_strcasecmp(eol, "lf") == 0)
				sc->newline = EOL_NL;
			else if (pg_strcasecmp(eol, "cr") == 0)
				sc->newline = EOL_CR;
			else if (pg_strcasecmp(eol, "crlf") == 0)
				sc->newline = EOL_CRNL;
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("invalid value for NEWLINE \"%s\"", eol),
						 errhint("Valid options are: 'LF', 'CRLF' and 'CR'.")));
			continue;
		}
		if (text && strcmp(def->defname, "escape") == 0)
		{
			/* text's escape, as Cloudberry's COPY has it: one byte, or OFF */
			char	   *esc = defGetString(def);

			if (escape)
				errorConflictingDefElem(def, pstate);
			escape = true;
			if (pg_strcasecmp(esc, "off") == 0)
				sc->escape_off = true;
			else if (strlen(esc) != 1 || IS_HIGHBIT_SET(esc[0]))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY escape must be a single one-byte character")));
			else if (esc[0] != '\\')
				sc->escape_char = esc[0];
			continue;
		}
		if (strcmp(def->defname, "encoding") == 0 &&
			pg_char_to_encoding(defGetString(def)) >= 0)
			file_encoding = pg_char_to_encoding(defGetString(def));
		copyopts = lappend(copyopts, def);
		if (strcmp(def->defname, "header") == 0 ||
			strcmp(def->defname, "encoding") == 0)
			continue;
		load_options = lappend(load_options, def);
	}
	load_options = lappend(load_options,
						   makeDefElem("encoding",
									   (Node *) makeString((char *) GetDatabaseEncodingName()),
									   -1));

	if ((sc->escape_off || sc->escape_char) && PG_ENCODING_IS_CLIENT_ONLY(file_encoding))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY's text ESCAPE other than the backslash is not supported in encoding \"%s\"",
						pg_encoding_to_char(file_encoding))));

	sc->outer = reading;
	reading = sc;
	cb = palloc0_object(MemoryContextCallback);
	cb->func = sreh_reset;
	cb->arg = sc;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, cb);

	if (sc->escape_off || sc->escape_char)
	{
		sc->cstate = BeginCopyFrom(pstate, rel, NULL, NULL, false,
								   raw_made_read, attlist, copyopts);
		sc->rawbuf = palloc(RAW_CHUNK);
		initStringInfo(&sc->made);
		sc->raw = raw_open(rel, filename, is_program, attlist);
	}
	else
		sc->cstate = BeginCopyFrom(pstate, rel, NULL, filename, is_program,
								   NULL, attlist, copyopts);
	if (sc->cstate->opts.format == COPY_FORMAT_BINARY)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s is not supported with binary COPY",
						reject_limit >= 0 ? "SEGMENT REJECT LIMIT" :
						sc->fill_missing ? "FILL MISSING FIELDS" : "NEWLINE")));
	if (sc->newline != EOL_UNKNOWN)
		sc->cstate->eol_type = sc->newline;

	if (reject_limit >= 0)
	{
		sc->sreh = makeCdbSreh(reject_limit, limit_in_rows,
							   filename ? (char *) filename : "<stdin>",
							   pstrdup(RelationGetRelationName(rel)), log_errors);
		sc->sreh->relid = RelationGetRelid(rel);
	}
	initStringInfo(&sc->ready);
	sc->values = palloc0_array(Datum, Max(tupDesc->natts, 1));
	sc->nulls = palloc0_array(bool, Max(tupDesc->natts, 1));
	sc->linecxt = AllocSetContextCreate(CurrentMemoryContext, "SrehCopyLine",
										ALLOCSET_DEFAULT_SIZES);
	if (sc->sreh != NULL && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		sc->route = route_begin(sc);

	*loader = sc->loader = BeginCopyFrom(pstate, rel, NULL, NULL, false,
										 loader_read, attlist, load_options);
	return sc;
}

/*
 * End: the data closed, and how many lines it refused, said as Cloudberry's
 * COPY says it.  The loading COPY is its caller's to end.
 */
uint64
GpSrehCopyEnd(void *state)
{
	SrehCopy   *sc = (SrehCopy *) state;
	uint64		rejected = sc->sreh ? sc->sreh->rejectcount : 0;

	EndCopyFrom(sc->cstate);
	if (sc->raw != NULL)
		raw_close(sc->raw);
	if (sc->route != NULL)
		route_end(sc->route);
	if (sc->sreh != NULL)
	{
		ReportSrehResults(sc->sreh, rejected);
		destroyCdbSreh(sc->sreh);
	}
	MemoryContextDelete(sc->linecxt);
	sc->ended = true;
	reading = sc->outer;
	return rejected;
}
