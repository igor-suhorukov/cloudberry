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
 * gp_dispatch.h
 *	  Reaching the segments.
 *
 * Cloudberry's dispatcher opens a libpq connection per segment, marks it as
 * internal with high bits in the protocol version, and sends plans in messages
 * of its own ('M' and 'T').  PostgreSQL 19 rejects both -- a major version
 * above 3 and an unknown message type end the session -- so the port's
 * dispatcher speaks ordinary libpq to an ordinary backend, and what makes that
 * backend a segment process is a startup setting it carries, "gp.qe_identity".
 *
 * The connections are the session's: opened on first use, kept until the
 * session ends or one of them breaks.  Cloudberry calls a set of them a gang,
 * and so does this.
 *
 * Whatever is sent is done inside the coordinator's transaction, savepoints
 * included, and committed when the coordinator commits; see gp_dispatch.c.
 * Two-phase commit and distributed snapshots are M3's.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_DISPATCH_H
#define GP_DISPATCH_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "utils/tuplestore.h"

/*
 * Run a statement on every segment and wait for all of them.
 *
 * Raises if any segment failed, naming the segment and repeating its own
 * message; the other segments are waited for first, so the connections are
 * left usable.
 */
extern void GpDispatchCommand(const char *sql);

/* The same, on one segment. */
extern void GpDispatchCommandOnContent(int content, const char *sql);

/*
 * Reading rows from every segment at once.
 *
 * The rows arrive as they are produced -- libpq's single-row mode -- so a
 * segment that has more of them does not wait for one that has fewer, and the
 * coordinator holds one row per segment rather than a whole result.
 */
typedef struct GpGatherState GpGatherState;

/*
 * Send the query to every segment.  "tupdesc" is what its rows will be
 * converted into, and it has to match the query's own result, which is the
 * caller's to arrange.
 */
extern GpGatherState *GpGatherStart(const char *sql, TupleDesc tupdesc);

/*
 * The next row from any segment, into the slot; false when every segment has
 * finished.  *content, when not NULL, is told which segment the row came from.
 */
extern bool GpGatherNext(GpGatherState *gather, TupleTableSlot *slot,
						 int *content);

/*
 * How many segments it reads from, and the next row from one of them, by its
 * place among them (0 .. count - 1): what a merge of sorted streams needs.
 */
extern int	GpGatherSegmentCount(GpGatherState *gather);
extern bool GpGatherNextFrom(GpGatherState *gather, int seg,
							 TupleTableSlot *slot);

/*
 * The next row from any segment as it arrived, for a caller that passes rows
 * on rather than reading them: each column's value, NULL for a null, and its
 * length.  Valid until the next call.  GpGatherIsBinary() says how they are
 * encoded -- a type's send function, or its output function -- and
 * GpGatherDecodeValue() turns one into a Datum.
 */
extern bool GpGatherNextRaw(GpGatherState *gather, const char **values,
							int *lengths);
extern bool GpGatherIsBinary(GpGatherState *gather);
extern Datum GpGatherDecodeValue(GpGatherState *gather, int col,
								 const char *value, int length);

/* Done with it, whether or not it was read to the end. */
extern void GpGatherEnd(GpGatherState *gather);

/* The same, from one segment only: a replicated table's rows, or one key's. */
extern GpGatherState *GpGatherStartOn(const char *sql, TupleDesc tupdesc,
									  int content);

/* The same, from the first nsegments segments: a partial table's. */
extern GpGatherState *GpGatherStartOnSegments(const char *sql,
											  TupleDesc tupdesc,
											  int nsegments);

/*
 * The type a value travels between the nodes as: itself, or text or bytea
 * for the few types that refuse to be read back (pg_node_tree and the
 * extended statistics' values), each binary-coercible to it.  A column of
 * a segment's query cast to it, and a query's select list of a relation's
 * columns, as "*" gives them, each so cast.
 */
extern Oid	GpTransferType(Oid type);
extern void GpAppendTransferColumn(StringInfo buf, const char *column, Oid type);
extern char *GpTransferSelectList(TupleDesc tupdesc);

/* Can every column of this descriptor travel in binary? */
extern bool GpTupleDescHasBinaryIO(TupleDesc tupdesc);

/*
 * Send a dispatched statement (gp_ddl.c builds it) to every segment and wait.
 * "own_xact" is for a statement that cannot run inside a transaction block --
 * CREATE DATABASE, VACUUM, CREATE INDEX CONCURRENTLY -- which each segment
 * runs in a transaction of its own; everything else joins the coordinator's.
 */
extern void GpDispatchUtility(const char *payload, bool own_xact);

/*
 * On a segment: is this the statement the coordinator dispatched?
 *
 * The coordinator ran it through every module's ProcessUtility hook, and what
 * those hooks did besides the statement -- a label, the partitions of a table
 * -- they did there, and dispatched separately if it was a statement of its
 * own.  So the port's hooks pass such a statement straight on here, and the
 * segment runs it as PostgreSQL alone would.
 */
extern bool GpDispatchIsDispatchedStatement(Node *utilityStmt);

/* Is this the text a dispatched statement travels as?  O26 leaves it alone. */
extern bool GpDispatchIsTreeText(const char *str);

/*
 * A statement with parameters, in text, on one segment (content >= 0), or
 * else on the first nsegments -- every one where that is 0; "types" gives
 * each parameter's type, or is NULL for the segment to infer them; "counts"
 * receives how many rows each segment's statement changed, in content order.
 */
extern void GpDispatchCommandParams(const char *sql, int nparams,
									const Oid *types,
									const char *const *values, int content,
									int nsegments, uint64 *counts);

/*
 * A statement on one segment whose parameters may be binary (formats[i] 1),
 * waited for.
 */
extern void GpDispatchParamsOnContent(int content, const char *sql,
									  int nparams, const char *const *values,
									  const int *lengths, const int *formats);

/*
 * A write with parameters, in text, on one segment: how many rows it changed,
 * and the rows its RETURNING gave, into "store" in "tupdesc"'s columns when
 * store is not NULL.
 */
extern uint64 GpDispatchWriteOnContent(int content, const char *sql,
									   int nparams, const char *const *values,
									   TupleDesc tupdesc,
									   Tuplestorestate *store);

/*
 * COPY ... FROM STDIN on one segment: begin with the COPY statement, send the
 * data in pieces, and end, which answers how many rows the segment took.  One
 * at a time.
 */
extern void GpCopyInBegin(int content, const char *sql);
extern void GpCopyInData(const char *data, int len);
extern uint64 GpCopyInEnd(void);

/*
 * A query on every segment (content -1) or one: the first column of each
 * segment's first row, as text, or NULL; one entry per segment asked.
 */
extern void GpDispatchQueryFirstValues(const char *sql, int content,
									   char **values);

/* A relation's name in SQL a segment is sent; pg_temp for a temporary one. */
extern char *GpDispatchRelationName(Oid relid);

/*
 * gp.dist_random() where there is nothing to dispatch to -- one node, or a
 * session that is not the coordinator's -- reads the relation here, into a
 * tuplestore of its row type or, with_content, of it and gp_segment_id.
 */
extern bool GpDistRandomIsLocal(void);
extern void GpDistRandomLocal(Oid relid, Tuplestorestate *store,
							  TupleDesc desc, bool with_content);

/*
 * A statement whose slices run at once (gp_motion.c): the writer on each
 * segment runs one of them as it runs any fragment, and readers -- more
 * backends of the session on the segment, reading as a part of the writer's
 * transaction (gp_share.c) -- run the others.
 *
 * GpStreamBegin() starts one; GpStreamWriterAddress() says where the writer
 * on a segment receives rows, and its process id; GpStreamAddReader() takes
 * a reader on a segment for it, answering the reader's place among the
 * stream's and where it receives; GpStreamStartReader() sends that reader
 * its slice, the whole of what it runs; GpStreamEnd() waits for every reader
 * to finish.  A reader that fails fails whatever the coordinator is waiting
 * for, and the error raised is the one that caused the others.
 */
typedef struct GpStream GpStream;

extern GpStream *GpStreamBegin(void);
extern const char *GpStreamWriterAddress(int content, int *pid);
extern int	GpStreamAddReader(GpStream *stream, int content,
							  const char **address);
extern void GpStreamStartReader(GpStream *stream, int reader, const char *sql);
extern void GpStreamEnd(GpStream *stream);

/* Close every connection: the session is over, or something went wrong. */
extern void GpDispatchResetGang(void);

/* Defines the settings; called from gp_core's _PG_init. */
/*
 * An object whose "gp" label the coordinator changed: the segments are sent
 * its label before the next statement they run, after the DDL being sent if
 * one is, and before the transaction commits.
 */
struct ObjectAddress;
extern void GpDispatchNoteLabel(const struct ObjectAddress *object);

/*
 * The DDL payload that writes an object's "gp" label on a segment, or NULL;
 * see gp_ddl.c.
 */
extern char *GpDdlLabelPayload(const struct ObjectAddress *object,
							   const char *label);

extern void GpDispatchInit(void);

/* Installs the DDL dispatch hooks, where there is a cluster; see gp_ddl.c. */
extern void GpDdlInit(void);

#endif							/* GP_DISPATCH_H */
