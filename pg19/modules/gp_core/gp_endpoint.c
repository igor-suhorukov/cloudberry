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
 * gp_endpoint.c
 *	  Parallel retrieve cursors: DECLARE ... PARALLEL RETRIEVE CURSOR, its
 *	  endpoints, and RETRIEVE ... FROM ENDPOINT in a retrieve session.
 *
 * A parallel retrieve cursor's rows are not gathered to the coordinator: the
 * cursor's top slice runs on each segment it would be gathered from, and
 * puts its rows into an endpoint there, from which a client reads them over
 * a connection of its own to that segment, a retrieve session -- the
 * segments' rows read in parallel.  A query whose rows have to meet on the
 * coordinator -- ORDER BY, an aggregate of the whole, a catalog, a function
 * -- has its one endpoint on the coordinator.
 *
 * O26 rewrites DECLARE c PARALLEL RETRIEVE CURSOR FOR q into PostgreSQL's
 * DECLARE c NO SCROLL CURSOR FOR q with GP_CURSOR_OPT_PARALLEL_RETRIEVE in
 * its options (gp_core_api.h), which PostgreSQL hands to the planner and
 * keeps with the portal.  ORCA plans every such cursor, whatever gp.optimizer
 * says: Cloudberry's own cursors are its planner's, whose plans have slices,
 * and the port's planner of slices is ORCA (gp_orca_planner.c).  Where ORCA's
 * plan is a Gather Motion over a slice the segments run, GpEndpointPlan()
 * takes the Gather off -- the cursor's plan is the slice, as Cloudberry's
 * planner leaves the top Motion off one (planner.c, cdbllize.c) -- and marks
 * the plan with where the endpoints are; anything else, ORCA's other plans
 * and the planner's where ORCA declines, has its endpoint on the
 * coordinator.  EXPLAIN prints the plan so, and the endpoints in
 * Cloudberry's words (explain_gp.c).
 *
 * On the segments (GpEndpointDispatch(), gp_motion.c) the slice runs on a
 * reader of the session on each endpoint's segment, not on its writer, which
 * the session's other statements need: the reader reads as a part of the
 * writer's transaction (gp_share.c), the writer publishing DECLARE's
 * snapshot for it, and the slices below stream to it as they would to the
 * writer.  The reader first opens the endpoint -- gp_internal.endpoint_open():
 * a slot in the node's shared array of endpoints, and a dynamic shared memory
 * segment with the rows' description and a shm_mq tuple queue, Cloudberry's
 * design (cdbendpoint.c) -- which DECLARE waits for, and then runs the slice
 * with its rows sent into the queue.  Its statement ends once a retrieve
 * session has read every row: that is FINISHED, which
 * gp_wait_parallel_retrieve_cursor() waits for, where Cloudberry's endpoint
 * sends the coordinator a notification.  The endpoint stays until the
 * cursor is closed, and its statement is cancelled if the cursor is closed
 * first.
 *
 * On the coordinator, where Cloudberry runs the cursor's plan in an entry-db
 * process that dispatches its gathers as the coordinator would, the port
 * cannot: a process of the coordinator's that dispatches has to be the
 * session's own, whose connections to the segments these are.  So the
 * coordinator's endpoint is filled as DECLARE runs, the plan run to its end
 * into a file of a FileSet, which the retrieve session reads; its states and
 * messages are the same, and DECLARE takes the query's time.
 *
 * A retrieve session is an ordinary login to the endpoint's node, with the
 * cursor's token: Cloudberry's own form, "-c gp_retrieve_conn=true" and the
 * token as the password, which gp_core's ClientAuthentication_hook asks for
 * once the node's pg_hba.conf method has let the user in -- Cloudberry asks
 * for it in place of pg_hba.conf -- and the port's, a "gp.retrieve_token"
 * startup setting holding it, which works whatever the method.  Its role is
 * utility, as Cloudberry makes it; it runs RETRIEVE and nothing else
 * (post_parse_analyze_hook), and no function by the fast path.  O26 rewrites
 * RETRIEVE { ALL | n } FROM ENDPOINT e into a SELECT of
 * gp_internal.retrieve(e, n) with the endpoint's columns, which
 * GpEndpointRetrieveSql() writes; the function reads the rows from the
 * queue or the file.  A record of no declared type travels described
 * (gp_record.c), since the receiving process does not know the sender's
 * typmods.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/endpoint/cdbendpoint.c, cdbendpointretrieve.c and
 *	  cdbendpointutils.c, and the parallel retrieve cursor's parts of
 *	  portalcmds.c, spi.c, explain_gp.c, auth.c, analyze.c and postgres.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/defrem.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/fileset.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_endpoint.h"
#include "gp_fault.h"
#include "gp_motion.h"
#include "gp_policy.h"
#include "gp_share.h"

/* How many endpoints a node has room for: Cloudberry's MAX_ENDPOINT_SIZE. */
#define ENDPOINT_SLOTS			1024

/* A session's token, as bytes and as the hex a client is shown. */
#define ENDPOINT_TOKEN_LEN		16
#define ENDPOINT_TOKEN_HEXLEN	(2 * ENDPOINT_TOKEN_LEN)

/* An endpoint's tuple queue: PostgreSQL's PARALLEL_TUPLE_QUEUE_SIZE. */
#define ENDPOINT_QUEUE_SIZE		65536

/* Its name: the cursor's, the session's and a count, as Cloudberry makes it. */
#define ENDPOINT_NAME_SESSION_LEN	8
#define ENDPOINT_NAME_COUNT_LEN		8
#define ENDPOINT_NAME_CURSOR_LEN \
	(NAMEDATALEN - 1 - ENDPOINT_NAME_SESSION_LEN - ENDPOINT_NAME_COUNT_LEN)

/* What a cancelled sender says after "canceling MPP operation". */
#define ENDPOINT_MESSAGE_LEN	128

/* The keys of an endpoint's dynamic shared memory segment. */
#define ENDPOINT_TOC_MAGIC		0x1949100119980802U
#define ENDPOINT_KEY_COLUMNS	1	/* the rows' description, as text */
#define ENDPOINT_KEY_QUEUE		2	/* a segment's: the tuple queue */
#define ENDPOINT_KEY_FILESET	3	/* the coordinator's: the rows' file */

/* The name of the coordinator's file of rows, in its FileSet. */
#define ENDPOINT_FILE			"rows"

/* The fault injector's slot-filling endpoints, Cloudberry's names. */
#define DUMMY_ENDPOINT_NAME		"DUMMYENDPOINTNAME"
#define DUMMY_CURSOR_NAME		"DUMMYCURSORNAME"

/* An endpoint's state, Cloudberry's EndpointState, and its names. */
typedef enum EndpointState
{
	ENDPOINT_FREE = 0,
	ENDPOINT_READY,				/* opened, not retrieved from yet */
	ENDPOINT_RETRIEVING,		/* a RETRIEVE is reading it */
	ENDPOINT_ATTACHED,			/* one read part of it */
	ENDPOINT_FINISHED,			/* every row was read */
	ENDPOINT_RELEASED,			/* its retrieve session failed or quit */
} EndpointState;

static const char *const endpoint_state_names[] = {
	"FREE", "READY", "RETRIEVING", "ATTACHED", "FINISHED", "RELEASED"
};

/* One endpoint of the node, in shared memory. */
typedef struct EndpointSlot
{
	bool		used;
	uint64		generation;		/* bumped at each use: a reader's check */
	char		name[NAMEDATALEN];
	char		cursor[NAMEDATALEN];
	Oid			database;
	Oid			user;			/* who declared the cursor */
	int			session;
	int			sender_pid;		/* -1 once the rows are all sent */
	ProcNumber	sender_proc;	/* whose latch hears the state change */
	int			receiver_pid;
	EndpointState state;
	dsm_handle	handle;
	char		message[ENDPOINT_MESSAGE_LEN];	/* why its sender is cancelled */
} EndpointSlot;

/*
 * The token of a session and user, which a retrieve session logs in with:
 * one for all the session's endpoints on the node, for as long as it has
 * one (Cloudberry's EndpointTokenEntry).
 */
typedef struct EndpointToken
{
	int			refcount;		/* 0: free */
	int			session;
	Oid			user;
	uint8		token[ENDPOINT_TOKEN_LEN];
} EndpointToken;

typedef struct EndpointShared
{
	LWLock		lock;
	EndpointSlot slots[ENDPOINT_SLOTS];
	EndpointToken tokens[ENDPOINT_SLOTS];
} EndpointShared;

static EndpointShared *endpoints = NULL;

/*
 * The coordinator's part: a parallel retrieve cursor of this session, from
 * DECLARE until it is closed or its (sub)transaction ends.
 */
typedef struct PrcCursor
{
	QueryDesc  *queryDesc;		/* its portal's, by which it is found */
	char		name[NAMEDATALEN];	/* its endpoints' */
	GpEndpointKind kind;
	SubTransactionId subid;		/* the subtransaction it was declared in */

	/* on the segments: the readers of its held stream (gp_dispatch.c) */
	GpStream   *stream;
	int			nendpoints;
	int		   *contents;		/* each endpoint's segment */
	int		   *readers;		/* and its reader in the stream */
	char	   *key;			/* its Motions' rows' key, or NULL */

	/* on the coordinator: its slot and its segment */
	int			slot;
	uint64		generation;
	dsm_segment *seg;
} PrcCursor;

static List *prc_cursors = NIL;

/*
 * A segment's reader that runs an endpoint's slice: the endpoint, from
 * gp_internal.endpoint_open() until it is released, and while the slice
 * runs, its queue.
 */
typedef struct EndpointSender
{
	int			slot;
	uint64		generation;
	dsm_segment *seg;
	shm_mq_handle *queue;
	TupleDesc	wire;			/* the rows as they travel */
	bool	   *records;		/* which columns go described */
} EndpointSender;

static EndpointSender *sender = NULL;

/* A retrieve session: the session it retrieves for, and what it attached. */
static bool retrieve_session = false;
static int	retrieve_for = -1;

typedef enum RetrieveState
{
	RETRIEVE_INIT,				/* attached, nothing read */
	RETRIEVE_RECEIVING,
	RETRIEVE_FINISHED,
} RetrieveState;

typedef struct RetrieveEntry
{
	char		name[NAMEDATALEN];	/* the key */
	int			slot;
	uint64		generation;
	dsm_segment *seg;
	shm_mq_handle *queue;		/* a segment's endpoint */
	FileSet    *fileset;		/* the coordinator's */
	BufFile    *file;			/* open while a RETRIEVE reads it */
	int			fileno;			/* where its next row is */
	off_t		offset;
	TupleDesc	wire;
	bool	   *records;
	RetrieveState state;
} RetrieveEntry;

static HTAB *retrieve_entries = NULL;
static RetrieveEntry *retrieve_current = NULL;

/* This session's token, made the first time a cursor needs one. */
static uint8 session_token[ENDPOINT_TOKEN_LEN];
static bool session_token_made = false;

/* The coordinator's endpoint is being filled: its run is not SPI's. */
static bool filling = false;

/* How many parallel retrieve cursors this backend declared: their names. */
static uint32 declare_count = 0;

static bool callbacks_registered = false;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static planner_hook_type prev_planner = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static explain_per_plan_hook_type prev_explain_per_plan = NULL;
static ClientAuthentication_hook_type prev_client_auth = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;
static object_access_hook_type prev_object_access = NULL;

static void endpoint_xact_callback(XactEvent event, void *arg);
static void endpoint_subxact_callback(SubXactEvent event,
									  SubTransactionId mySubid,
									  SubTransactionId parentSubid, void *arg);
static void endpoint_exit(int code, Datum arg);

/* ------------------------------------------------------------------------- */
/* The shared array                                                          */
/* ------------------------------------------------------------------------- */

static void
endpoints_init_segment(void *ptr, void *arg)
{
	EndpointShared *shared = (EndpointShared *) ptr;

	memset(shared, 0, sizeof(EndpointShared));
	LWLockInitialize(&shared->lock, LWLockNewTrancheId("gp_core endpoints"));
	for (int i = 0; i < ENDPOINT_SLOTS; i++)
	{
		shared->slots[i].sender_pid = -1;
		shared->slots[i].receiver_pid = -1;
		shared->slots[i].handle = DSM_HANDLE_INVALID;
	}
}

/* Attached on first use: most backends never see an endpoint. */
static void
endpoints_attach(void)
{
	bool		found;

	if (endpoints != NULL)
		return;
	endpoints = GetNamedDSMSegment("gp_core endpoints", sizeof(EndpointShared),
								   endpoints_init_segment, &found, NULL);
}

static uint32
endpoint_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("ParallelRetrieveCursor");
	return event;
}

static void
register_callbacks(void)
{
	if (callbacks_registered)
		return;
	RegisterXactCallback(endpoint_xact_callback, NULL);
	RegisterSubXactCallback(endpoint_subxact_callback, NULL);
	before_shmem_exit(endpoint_exit, (Datum) 0);
	callbacks_registered = true;
}

/* A token's entry of this session and user, made or found; the lock is held. */
static EndpointToken *
token_take(int session, Oid user, const uint8 *token)
{
	EndpointToken *free_one = NULL;

	for (int i = 0; i < ENDPOINT_SLOTS; i++)
	{
		EndpointToken *t = &endpoints->tokens[i];

		if (t->refcount > 0 && t->session == session && t->user == user)
		{
			t->refcount++;
			return t;
		}
		if (t->refcount == 0 && free_one == NULL)
			free_one = t;
	}
	if (free_one == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("failed to allocate endpoint for session id %d", session)));
	free_one->refcount = 1;
	free_one->session = session;
	free_one->user = user;
	memcpy(free_one->token, token, ENDPOINT_TOKEN_LEN);
	return free_one;
}

static EndpointToken *
token_find(int session, Oid user)
{
	for (int i = 0; i < ENDPOINT_SLOTS; i++)
	{
		EndpointToken *t = &endpoints->tokens[i];

		if (t->refcount > 0 && t->session == session && t->user == user)
			return t;
	}
	return NULL;
}

/* Let go of a slot, and of its token's entry with its last; the lock is held. */
static void
slot_free(EndpointSlot *slot)
{
	EndpointToken *t = token_find(slot->session, slot->user);

	if (t != NULL && --t->refcount == 0)
		memset(t, 0, sizeof(EndpointToken));
	slot->used = false;
	slot->state = ENDPOINT_FREE;
	slot->name[0] = '\0';
	slot->cursor[0] = '\0';
	slot->sender_pid = -1;
	slot->receiver_pid = -1;
	slot->handle = DSM_HANDLE_INVALID;
	slot->message[0] = '\0';
}

/* Is the slot still the endpoint this backend knows of it? */
static bool
slot_is(int index, uint64 generation)
{
	EndpointSlot *slot = &endpoints->slots[index];

	return slot->used && slot->generation == generation;
}

/*
 * A slot for a new endpoint, its lock held.  The fault injector's
 * alloc_endpoint_slot_full fills every free one with an endpoint of no
 * cursor's, and alloc_endpoint_slot_full_reset frees them again, as
 * Cloudberry's alloc_endpoint() does.
 */
static int
slot_alloc(const char *name, const char *cursor, int session, Oid user,
		   const uint8 *token, dsm_handle handle)
{
	int			found = -1;
	EndpointSlot *slot;

	if (GP_FAULT("alloc_endpoint_slot_full") == GP_FAULT_SKIP)
	{
		for (int i = 0; i < ENDPOINT_SLOTS; i++)
		{
			slot = &endpoints->slots[i];
			if (slot->used)
				continue;
			slot->used = true;
			slot->generation++;
			strlcpy(slot->name, DUMMY_ENDPOINT_NAME, NAMEDATALEN);
			strlcpy(slot->cursor, DUMMY_CURSOR_NAME, NAMEDATALEN);
			slot->database = MyDatabaseId;
			slot->session = session;
			slot->user = user;
			slot->state = ENDPOINT_READY;
			slot->handle = DSM_HANDLE_INVALID;
		}
	}
	if (GP_FAULT("alloc_endpoint_slot_full_reset") == GP_FAULT_SKIP)
	{
		for (int i = 0; i < ENDPOINT_SLOTS; i++)
		{
			slot = &endpoints->slots[i];
			if (slot->used && strcmp(slot->name, DUMMY_ENDPOINT_NAME) == 0)
			{
				slot->used = false;
				slot->state = ENDPOINT_FREE;
				slot->name[0] = '\0';
			}
		}
	}

	for (int i = 0; i < ENDPOINT_SLOTS && found < 0; i++)
		if (!endpoints->slots[i].used)
			found = i;
	if (found < 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("failed to allocate endpoint for session id %d", session)));

	(void) token_take(session, user, token);
	slot = &endpoints->slots[found];
	slot->used = true;
	slot->generation++;
	strlcpy(slot->name, name, NAMEDATALEN);
	strlcpy(slot->cursor, cursor, NAMEDATALEN);
	slot->database = MyDatabaseId;
	slot->user = user;
	slot->session = session;
	slot->sender_pid = MyProcPid;
	slot->sender_proc = MyProcNumber;
	slot->receiver_pid = -1;
	slot->state = ENDPOINT_READY;
	slot->handle = handle;
	slot->message[0] = '\0';
	return found;
}

/* Wake the process that waits on the endpoint's state; the lock is held. */
static void
slot_wake_sender(EndpointSlot *slot)
{
	PGPROC	   *proc = GetPGProcByNumber(slot->sender_proc);

	if (slot->sender_pid > 0 && proc->pid == slot->sender_pid)
		SetLatch(&proc->procLatch);
}

static void
token_hex(const uint8 *token, char *hex)
{
	hex_encode((const char *) token, ENDPOINT_TOKEN_LEN, hex);
	hex[ENDPOINT_TOKEN_HEXLEN] = '\0';
}

/* ------------------------------------------------------------------------- */
/* The rows as they travel                                                   */
/* ------------------------------------------------------------------------- */

/*
 * The cursor's columns, as the endpoint's description holds them: each
 * one's name, type and typmod.  A record of no declared type is described
 * value by value on the wire, as gp_record.c describes one, and travels as
 * bytea; a record[] of them could not be read back, and is refused.
 */
static char *
columns_describe(TupleDesc tupdesc)
{
	List	   *columns = NIL;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->atttypid == RECORDARRAYOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of a parallel retrieve cursor is an array of records of no declared type",
							NameStr(att->attname))));
		columns = lappend(columns,
						  list_make3(makeString(pstrdup(NameStr(att->attname))),
									 makeInteger((int) att->atttypid),
									 makeInteger(att->atttypmod)));
	}
	return nodeToString(columns);
}

/*
 * The description made again: the cursor's columns, and the rows' as they
 * travel, a record's as bytea.
 */
static void
columns_read(const char *text, TupleDesc *columns, TupleDesc *wire,
			 bool **records)
{
	List	   *list = (List *) stringToNode(text);
	int			natts = list_length(list);
	int			i = 0;

	*columns = CreateTemplateTupleDesc(natts);
	*wire = CreateTemplateTupleDesc(natts);
	*records = palloc0_array(bool, Max(natts, 1));
	foreach_ptr(List, column, list)
	{
		char	   *name = strVal(linitial(column));
		Oid			type = (Oid) intVal(lsecond(column));
		int32		typmod = intVal(lthird(column));

		(*records)[i] = type == RECORDOID;
		TupleDescInitEntry(*columns, i + 1, name, type, typmod, 0);
		TupleDescInitEntry(*wire, i + 1, name,
						   (*records)[i] ? BYTEAOID : type,
						   (*records)[i] ? -1 : typmod, 0);
		i++;
	}
	TupleDescFinalize(*columns);
	TupleDescFinalize(*wire);
}

/*
 * A row as it travels: its values, a record described, a value stored
 * elsewhere fetched -- the retrieve session reads with a snapshot of its
 * own, which may not see a TOAST table's rows the cursor's transaction
 * wrote.
 */
static MinimalTuple
wire_tuple(TupleTableSlot *slot, TupleDesc wire, const bool *records)
{
	Datum	   *values = palloc_array(Datum, Max(wire->natts, 1));
	bool	   *nulls = palloc_array(bool, Max(wire->natts, 1));
	bool	   *made = palloc0_array(bool, Max(wire->natts, 1));
	MinimalTuple tuple;

	slot_getallattrs(slot);
	for (int i = 0; i < wire->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(wire, i);

		nulls[i] = slot->tts_isnull[i];
		values[i] = slot->tts_values[i];
		if (nulls[i])
			continue;
		made[i] = records[i] ||
			(att->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(values[i])));
		if (records[i])
		{
			StringInfoData buf;
			bytea	   *wired;

			initStringInfo(&buf);
			GpRecordWireWrite(&buf, values[i]);
			wired = palloc(VARHDRSZ + buf.len);
			SET_VARSIZE(wired, VARHDRSZ + buf.len);
			memcpy(VARDATA(wired), buf.data, buf.len);
			pfree(buf.data);
			values[i] = PointerGetDatum(wired);
		}
		else if (att->attlen == -1 &&
				 VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			values[i] = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(values[i])));
	}
	tuple = heap_form_minimal_tuple(wire, values, nulls, 0);
	for (int i = 0; i < wire->natts; i++)
		if (made[i])
			pfree(DatumGetPointer(values[i]));
	pfree(values);
	pfree(nulls);
	pfree(made);
	return tuple;
}

/* ------------------------------------------------------------------------- */
/* Planning                                                                  */
/* ------------------------------------------------------------------------- */

List *
GpEndpointPlanMark(PlannedStmt *stmt)
{
	foreach_node(DefElem, def, stmt->extension_state)
		if (strcmp(def->defname, GP_ENDPOINT_MARK) == 0)
			return (List *) def->arg;
	return NIL;
}

static GpEndpointKind
mark_kind(List *mark)
{
	return (GpEndpointKind) intVal(linitial(mark));
}

static List *
mark_contents(List *mark)
{
	return (List *) lsecond(mark);
}

static CustomScan *
mark_gather(List *mark)
{
	return (CustomScan *) lthird(mark);
}

static void
plan_mark(PlannedStmt *stmt, GpEndpointKind kind, List *contents,
		  Plan *gather)
{
	List	   *mark = list_make3(makeInteger(kind), contents, gather);

	stmt->extension_state = lappend(stmt->extension_state,
									makeDefElem(pstrdup(GP_ENDPOINT_MARK),
												(Node *) mark, -1));
}

/*
 * Does the cursor read only replicated tables?  Then the one segment it
 * reads is the session's, as Cloudberry picks it: gp_session_id modulo the
 * tables' segments.
 */
static int
replicated_segments(PlannedStmt *stmt)
{
	int			numsegments = 0;

	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		GpPolicy   *policy;

		if (rte->rtekind != RTE_RELATION)
			continue;
		policy = GpPolicyGet(rte->relid);
		if (policy == NULL || !GpPolicyIsReplicated(policy))
			return 0;
		numsegments = numsegments == 0 ? policy->numsegments
			: Min(numsegments, policy->numsegments);
	}
	return numsegments;
}

void
GpEndpointPlan(PlannedStmt *stmt)
{
	Plan	   *root = stmt->planTree;
	CustomScan *gather;
	List	   *contents = NIL;
	GpEndpointKind kind;
	int			content;
	int			numsegments;
	ListCell   *lc;
	ListCell   *lf;

	if (GpEndpointPlanMark(stmt) != NIL)
		return;
	if (!GpMotionIs(root) || GpMotionType(root) != GP_MOTION_GATHER ||
		!GpMotionEndpointsCanRun(stmt, root))
	{
		plan_mark(stmt, GP_ENDPOINT_COORDINATOR, NIL, NULL);
		return;
	}

	/*
	 * The Gather goes, and its fragment is the cursor's plan, its columns
	 * named as the Gather's were -- the query's names, which a RETRIEVE's
	 * rows are given.
	 */
	gather = (CustomScan *) root;
	stmt->planTree = outerPlan(gather);
	stmt->planTree->targetlist = copyObject(stmt->planTree->targetlist);
	forboth(lc, stmt->planTree->targetlist, lf, gather->scan.plan.targetlist)
		lfirst_node(TargetEntry, lc)->resname = lfirst_node(TargetEntry, lf)->resname;

	content = GpMotionSegment(root);
	if (GpMotionSegments(root) != NIL)
	{
		kind = GP_ENDPOINT_SOME;
		contents = list_copy(GpMotionSegments(root));
	}
	else if (content >= 0 && (numsegments = replicated_segments(stmt)) > 0)
	{
		kind = GP_ENDPOINT_SINGLE;
		content = GpClusterSessionId() % numsegments;
		GpMotionSetSegment(root, content);
		contents = list_make1_int(content);
	}
	else if (content >= 0)
	{
		kind = GP_ENDPOINT_SOME;
		contents = list_make1_int(content);
	}
	else
	{
		kind = GP_ENDPOINT_ALL;
		for (int seg = 0; seg < GpClusterSegmentCount(); seg++)
			contents = lappend_int(contents, seg);
	}
	plan_mark(stmt, kind, contents, root);
}

/*
 * The planner's route, and every plan ORCA did not make: the endpoint is on
 * the coordinator.  And a cursor declared anywhere but on the coordinator,
 * where its plan would have no segments to go to, is refused, as
 * Cloudberry's planner refuses one outside the dispatcher.
 */
static PlannedStmt *
endpoint_planner(Query *parse, const char *query_string, int cursorOptions,
				 ParamListInfo boundParams, ExplainState *es)
{
	PlannedStmt *stmt;

	if ((cursorOptions & GP_CURSOR_OPT_PARALLEL_RETRIEVE) &&
		GpClusterBackendRole() != GP_ROLE_DISPATCH && !GpClusterIsSingleNode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Parallel retrieve cursor should run on the dispatcher only")));

	if (prev_planner)
		stmt = prev_planner(parse, query_string, cursorOptions, boundParams, es);
	else
		stmt = standard_planner(parse, query_string, cursorOptions,
								boundParams, es);

	if ((cursorOptions & GP_CURSOR_OPT_PARALLEL_RETRIEVE) &&
		GpEndpointPlanMark(stmt) == NIL)
		plan_mark(stmt, GP_ENDPOINT_COORDINATOR, NIL, NULL);
	return stmt;
}

/* EXPLAIN's line of where the endpoints are, in Cloudberry's words. */
static void
endpoint_explain_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
						  ExplainState *es, const char *queryString,
						  ParamListInfo params, QueryEnvironment *queryEnv)
{
	List	   *mark = GpEndpointPlanMark(plannedstmt);

	if (prev_explain_per_plan)
		prev_explain_per_plan(plannedstmt, into, es, queryString, params,
							  queryEnv);
	if (mark == NIL)
		return;

	ExplainOpenGroup("Cursor", "Cursor", true, es);
	switch (mark_kind(mark))
	{
		case GP_ENDPOINT_COORDINATOR:
			ExplainPropertyText("Endpoint", "\"on coordinator\"", es);
			break;
		case GP_ENDPOINT_SINGLE:
			ExplainPropertyText("Endpoint",
								psprintf("\"on segment: contentid [%d]\"",
										 linitial_int(mark_contents(mark))),
								es);
			break;
		case GP_ENDPOINT_SOME:
			{
				StringInfoData buf;

				initStringInfo(&buf);
				appendStringInfoString(&buf, "on segments: contentid [");
				foreach_int(content, mark_contents(mark))
					appendStringInfo(&buf, "%s%d",
									 foreach_current_index(content) > 0 ? ", " : "",
									 content);
				appendStringInfoChar(&buf, ']');
				ExplainPropertyText("Endpoint", buf.data, es);
				break;
			}
		case GP_ENDPOINT_ALL:
			ExplainPropertyText("Endpoint",
								psprintf("on all %d segments",
										 GpClusterSegmentCount()),
								es);
			break;
	}
	ExplainCloseGroup("Cursor", "Cursor", true, es);
}

/* ------------------------------------------------------------------------- */
/* The coordinator: DECLARE, the wait, CLOSE                                 */
/* ------------------------------------------------------------------------- */

/* Cloudberry's endpoint name: the cursor's, the session's, a count. */
static void
endpoint_name(char *name, const char *cursor)
{
	int			len = pg_mbcliplen(cursor, strlen(cursor), ENDPOINT_NAME_CURSOR_LEN);

	memcpy(name, cursor, len);
	snprintf(name + len, NAMEDATALEN - len, "%08x%08x",
			 (uint32) GpClusterSessionId(), ++declare_count);
}

static const uint8 *
token_of_session(void)
{
	if (!session_token_made)
	{
		if (!pg_strong_random(session_token, ENDPOINT_TOKEN_LEN))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to generate a new random token for session id %d",
							GpClusterSessionId())));
		session_token_made = true;
	}
	return session_token;
}

static PrcCursor *
prc_find(QueryDesc *queryDesc)
{
	foreach_ptr(PrcCursor, prc, prc_cursors)
		if (prc->queryDesc == queryDesc)
			return prc;
	return NULL;
}

/*
 * The coordinator's DestReceiver that fills its endpoint: each row, as it
 * travels, after its length, into the endpoint's file.
 */
typedef struct FileDest
{
	DestReceiver pub;
	BufFile    *file;
	TupleDesc	wire;
	bool	   *records;
} FileDest;

static bool
file_receive(TupleTableSlot *slot, DestReceiver *self)
{
	FileDest   *dest = (FileDest *) self;
	MinimalTuple tuple = wire_tuple(slot, dest->wire, dest->records);
	uint32		len = tuple->t_len;

	BufFileWrite(dest->file, &len, sizeof(len));
	BufFileWrite(dest->file, tuple, len);
	pfree(tuple);
	return true;
}

static void
file_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
file_shutdown(DestReceiver *self)
{
}

static void
file_destroy(DestReceiver *self)
{
	pfree(self);
}

/* A segment for an endpoint: its description, and a queue or a FileSet. */
static dsm_segment *
endpoint_segment(const char *columns, bool queue, shm_toc **tocp)
{
	shm_toc_estimator e;
	Size		size;
	dsm_segment *seg;
	shm_toc    *toc;
	char	   *text;

	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, strlen(columns) + 1);
	shm_toc_estimate_chunk(&e, queue ? ENDPOINT_QUEUE_SIZE : sizeof(FileSet));
	shm_toc_estimate_keys(&e, 2);
	size = shm_toc_estimate(&e);

	seg = dsm_create(size, 0);
	dsm_pin_mapping(seg);
	toc = shm_toc_create(ENDPOINT_TOC_MAGIC, dsm_segment_address(seg), size);
	text = shm_toc_allocate(toc, strlen(columns) + 1);
	strcpy(text, columns);
	shm_toc_insert(toc, ENDPOINT_KEY_COLUMNS, text);
	*tocp = toc;
	return seg;
}

/*
 * The coordinator's endpoint: its plan run to its end now, into a file the
 * retrieve session reads.
 */
static void
fill_coordinator_endpoint(PrcCursor *prc, Portal portal, const char *columns)
{
	shm_toc    *toc;
	FileSet    *fileset;
	FileDest   *dest;
	TupleDesc	described;

	prc->seg = endpoint_segment(columns, false, &toc);
	fileset = shm_toc_allocate(toc, sizeof(FileSet));
	FileSetInit(fileset);
	shm_toc_insert(toc, ENDPOINT_KEY_FILESET, fileset);

	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	prc->slot = slot_alloc(prc->name, portal->name, GpClusterSessionId(),
						   GetUserId(), token_of_session(),
						   dsm_segment_handle(prc->seg));
	prc->generation = endpoints->slots[prc->slot].generation;
	LWLockRelease(&endpoints->lock);

	dest = palloc0(sizeof(FileDest));
	dest->pub.receiveSlot = file_receive;
	dest->pub.rStartup = file_startup;
	dest->pub.rShutdown = file_shutdown;
	dest->pub.rDestroy = file_destroy;
	dest->pub.mydest = DestNone;
	columns_read(columns, &described, &dest->wire, &dest->records);
	dest->file = BufFileCreateFileSet(fileset, ENDPOINT_FILE);

	filling = true;
	PG_TRY();
	{
		(void) PortalRunFetch(portal, FETCH_FORWARD, FETCH_ALL,
							  (DestReceiver *) dest);
	}
	PG_FINALLY();
	{
		filling = false;
	}
	PG_END_TRY();
	BufFileClose(dest->file);
}

/*
 * DECLARE ... PARALLEL RETRIEVE CURSOR, once PostgreSQL has made and started
 * its portal: its endpoints opened -- on the segments, by their readers,
 * which then run the slice; on the coordinator, filled -- before DECLARE
 * returns, as Cloudberry's waits for them to be READY (WaitEndpointsReady()).
 */
static void
endpoint_declare(const char *cursor)
{
	Portal		portal = GetPortalByName(cursor);
	QueryDesc  *queryDesc;
	List	   *mark;
	PrcCursor  *prc;
	char	   *columns;
	char		token[ENDPOINT_TOKEN_HEXLEN + 1];
	MemoryContext oldcxt;

	if (!PortalIsValid(portal) || portal->queryDesc == NULL)
		return;
	queryDesc = portal->queryDesc;
	mark = GpEndpointPlanMark(queryDesc->plannedstmt);
	if (mark == NIL)
		return;

	endpoints_attach();
	register_callbacks();
	columns = columns_describe(queryDesc->tupDesc);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	prc = palloc0(sizeof(PrcCursor));
	prc->queryDesc = queryDesc;
	prc->kind = mark_kind(mark);
	prc->subid = GetCurrentSubTransactionId();
	prc->slot = -1;
	endpoint_name(prc->name, cursor);
	prc_cursors = lappend(prc_cursors, prc);
	MemoryContextSwitchTo(oldcxt);

	if (prc->kind == GP_ENDPOINT_COORDINATOR)
	{
		fill_coordinator_endpoint(prc, portal, columns);
		return;
	}

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	prc->nendpoints = list_length(mark_contents(mark));
	prc->contents = palloc_array(int, prc->nendpoints);
	prc->readers = palloc_array(int, prc->nendpoints);
	foreach_int(content, mark_contents(mark))
	{
		prc->contents[foreach_current_index(content)] = content;
		prc->readers[foreach_current_index(content)] = -1;	/* none taken yet */
	}
	MemoryContextSwitchTo(oldcxt);

	token_hex(token_of_session(), token);
	prc->stream = GpStreamBeginHeld();
	prc->key = GpEndpointDispatch(queryDesc, mark_gather(mark), prc->stream,
								  prc->contents, prc->readers,
								  prc->nendpoints, prc->name,
								  psprintf("SELECT gp_internal.endpoint_open(%s, %s, %d, %u, %s, %s)",
										   quote_literal_cstr(prc->name),
										   quote_literal_cstr(cursor),
										   GpClusterSessionId(), GetUserId(),
										   quote_literal_cstr(token),
										   quote_literal_cstr(columns)));
	if (prc->key != NULL)
		prc->key = MemoryContextStrdup(TopMemoryContext, prc->key);
}

/*
 * A cursor's end: on the segments, the readers still running stopped, the
 * endpoints whose rows were all read released, and its Motions' rows
 * dropped; on the coordinator, its endpoint and file.  Raising, what a
 * slice answered is raised, as Cloudberry's CLOSE reports it, and a
 * coordinator's endpoint whose retrieve session gave up says why; the
 * transaction's abort then finishes the cursor's end.
 */
static void
prc_end(PrcCursor *prc, bool raising)
{
	char	   *message = NULL;

	if (prc->kind != GP_ENDPOINT_COORDINATOR && prc->stream != NULL)
	{
		if (raising)
		{
			(void) GpStreamWait(prc->stream, 0);
			GpStreamRaise(prc->stream);
		}
		GpStreamCancel(prc->stream);
		for (int i = 0; i < prc->nendpoints; i++)
			if (prc->readers[i] >= 0 &&
				GpStreamReaderDone(prc->stream, prc->readers[i]))
				GpStreamReaderExec(prc->stream, prc->readers[i],
								   psprintf("SELECT gp_internal.endpoint_release(%s)",
											quote_literal_cstr(prc->name)),
								   false);
		if (raising && prc->key != NULL)
			GpDispatchCommand(psprintf("SELECT gp_internal.motion_drop(%s)",
									   quote_literal_cstr(prc->key)));
		GpStreamRelease(prc->stream);
		prc->stream = NULL;
	}
	else if (prc->kind == GP_ENDPOINT_COORDINATOR && prc->seg != NULL)
	{
		FileSet    *fileset;

		if (prc->slot >= 0)
		{
			LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
			if (slot_is(prc->slot, prc->generation))
			{
				EndpointSlot *slot = &endpoints->slots[prc->slot];

				if (slot->state == ENDPOINT_RELEASED)
					message = pstrdup(slot->message);
				slot_free(slot);
			}
			LWLockRelease(&endpoints->lock);
		}
		fileset = shm_toc_lookup(shm_toc_attach(ENDPOINT_TOC_MAGIC,
												dsm_segment_address(prc->seg)),
								 ENDPOINT_KEY_FILESET, true);
		if (fileset != NULL)
			FileSetDeleteAll(fileset);
		dsm_detach(prc->seg);
		prc->seg = NULL;
	}

	prc_cursors = list_delete_ptr(prc_cursors, prc);
	if (prc->contents != NULL)
		pfree(prc->contents);
	if (prc->readers != NULL)
		pfree(prc->readers);
	pfree(prc);

	if (raising && message != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("canceling MPP operation: \"%s\"", message)));
}

/*
 * The cursors a (sub)transaction's abort takes with it, ended where nothing
 * may be raised: a failure to end one drops the session's connections to the
 * segments, as gp_dispatch.c's abort does.
 */
static void
prc_abort(SubTransactionId subid)
{
	uint32		holdoff = InterruptHoldoffCount;

	foreach_ptr(PrcCursor, prc, list_copy(prc_cursors))
	{
		if (subid != InvalidSubTransactionId && prc->subid != subid)
			continue;
		PG_TRY();
		{
			prc_end(prc, false);
		}
		PG_CATCH();
		{
			InterruptHoldoffCount = holdoff;
			FlushErrorState();
			GpDispatchResetGang();
			if (list_member_ptr(prc_cursors, prc))
			{
				if (prc->stream != NULL)
					GpStreamRelease(prc->stream);
				prc_cursors = list_delete_ptr(prc_cursors, prc);
			}
		}
		PG_END_TRY();
	}
}

static void
endpoint_refuse_fetch(FetchStmt *stmt)
{
	Portal		portal = stmt->portalname != NULL && stmt->portalname[0] != '\0'
		? GetPortalByName(stmt->portalname) : NULL;

	if (!PortalIsValid(portal) ||
		!(portal->cursorOptions & GP_CURSOR_OPT_PARALLEL_RETRIEVE))
		return;
	if (stmt->ismove)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the 'MOVE' statement for PARALLEL RETRIEVE CURSOR is not supported")));
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("cannot specify 'FETCH' for PARALLEL RETRIEVE CURSOR"),
			 errhint("Use 'RETRIEVE' statement on endpoint instead.")));
}

/*
 * ROLLBACK reports what a cursor's slice that failed answered, as
 * Cloudberry's does, where the cursor's check found it while the session
 * was idle (GpParallelRetrieveCursorCheckTimeoutHandler()): here a failed
 * slice has stopped its endpoints already, and the rollback raises its
 * error.  The first cursor that has one.
 */
static PrcCursor *
prc_failed(void)
{
	foreach_ptr(PrcCursor, prc, prc_cursors)
	{
		if (prc->stream == NULL)
			continue;
		(void) GpStreamWait(prc->stream, 0);
		if (GpStreamFailed(prc->stream))
			return prc;
	}
	return NULL;
}

/*
 * EXPLAIN ANALYZE of one would run it here, whose rows are its endpoints':
 * refused, which Cloudberry's tests do not try.
 */
static void
endpoint_refuse_analyze(ExplainStmt *stmt)
{
	Node	   *query = stmt->query;

	/* parse analysis made the DECLARE a utility Query */
	if (IsA(query, Query) && ((Query *) query)->commandType == CMD_UTILITY)
		query = ((Query *) query)->utilityStmt;
	if (!IsA(query, DeclareCursorStmt) ||
		!(((DeclareCursorStmt *) query)->options & GP_CURSOR_OPT_PARALLEL_RETRIEVE))
		return;
	foreach_node(DefElem, opt, stmt->options)
		if (strcmp(opt->defname, "analyze") == 0 && defGetBoolean(opt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("EXPLAIN ANALYZE of a PARALLEL RETRIEVE CURSOR is not supported")));
}

static void
endpoint_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						bool readOnlyTree, ProcessUtilityContext context,
						ParamListInfo params, QueryEnvironment *queryEnv,
						DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	char	   *cursor = NULL;
	PrcCursor  *failed = NULL;

	if (IsA(parsetree, FetchStmt))
		endpoint_refuse_fetch((FetchStmt *) parsetree);
	if (IsA(parsetree, ExplainStmt))
		endpoint_refuse_analyze((ExplainStmt *) parsetree);
	if (IsA(parsetree, TransactionStmt) &&
		((TransactionStmt *) parsetree)->kind == TRANS_STMT_ROLLBACK)
		failed = prc_failed();
	if (IsA(parsetree, DeclareCursorStmt) &&
		(((DeclareCursorStmt *) parsetree)->options & GP_CURSOR_OPT_PARALLEL_RETRIEVE))
		cursor = pstrdup(((DeclareCursorStmt *) parsetree)->portalname);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params,
							queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	if (cursor != NULL)
		endpoint_declare(cursor);

	/* the transaction ends all the same (TBLOCK_ABORT_PENDING) */
	if (failed != NULL)
		GpStreamRaise(failed->stream);
}

/*
 * The coordinator never runs a cursor's slice that the segments run: its
 * plan is set up, and its permissions checked, but described only, as for
 * EXPLAIN.
 */
static void
endpoint_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	List	   *mark = GpEndpointPlanMark(queryDesc->plannedstmt);

	if (mark != NIL && mark_kind(mark) != GP_ENDPOINT_COORDINATOR)
		eflags |= EXEC_FLAG_EXPLAIN_ONLY;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * A cursor's rows are its endpoints': FETCH and MOVE refuse one
 * (endpoint_refuse_fetch()), and so does a PL/pgSQL FETCH, whose SPI would
 * run its plan here, in Cloudberry's words (SPI_cursor_find()).
 */
static void
endpoint_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					 uint64 count)
{
	if (!filling && GpEndpointPlanMark(queryDesc->plannedstmt) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("The PARALLEL RETRIEVE CURSOR is not supported in SPI."),
				 errhint("Use normal cursor statement instead.")));

	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
}

/*
 * The cursor's portal ends, as it was set up, with nothing of its own run:
 * PostgreSQL's ExecutorFinish() is not for a plan only described.
 */
static void
endpoint_ExecutorFinish(QueryDesc *queryDesc)
{
	if ((queryDesc->estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY) &&
		GpEndpointPlanMark(queryDesc->plannedstmt) != NIL)
		return;

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

/* CLOSE, and a transaction's end that drops the cursor: its endpoints end. */
static void
endpoint_ExecutorEnd(QueryDesc *queryDesc)
{
	PrcCursor  *prc = prc_find(queryDesc);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	if (prc != NULL)
		prc_end(prc, true);
}

/*
 * Has every endpoint of the cursor on the segments had its rows read, its
 * reader about to end its statement, which is all the coordinator hears of
 * it?  A RETRIEVE that read an endpoint's last row has returned by then, and
 * a check right after it is to find the cursor finished, as Cloudberry's
 * finds the endpoint's acknowledgement.
 */
static bool
prc_all_read(PrcCursor *prc)
{
	int			nsegs = GpClusterSegmentCount();
	char	  **left = palloc0_array(char *, nsegs);

	GpDispatchQueryFirstValues(psprintf("SELECT count(*) FROM gp_internal.segment_endpoints() "
										"WHERE endpointname = %s AND sessionid = %d AND state <> 'FINISHED'",
										quote_literal_cstr(prc->name),
										GpClusterSessionId()),
							   -1, left);
	for (int i = 0; i < nsegs; i++)
		if (left[i] == NULL || strcmp(left[i], "0") != 0)
			return false;
	return true;
}

/*
 * gp_wait_parallel_retrieve_cursor(cursorname text, timeout_sec int4)
 *
 * Whether every endpoint of the cursor has had its rows read, waiting for it
 * timeout_sec seconds at most -- none: look once; less than none: until it
 * has.  A slice of the cursor that failed fails it, as Cloudberry's does.
 */
PG_FUNCTION_INFO_V1(gp_wait_parallel_retrieve_cursor);

Datum
gp_wait_parallel_retrieve_cursor(PG_FUNCTION_ARGS)
{
	char	   *cursor = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			timeout = PG_GETARG_INT32(1);
	FuncCallContext *funcctx;
	Portal		portal = GetPortalByName(cursor);
	PrcCursor  *prc;
	bool		finished = false;

	/* one row, the answer */
	if (!SRF_IS_FIRSTCALL())
	{
		funcctx = SRF_PERCALL_SETUP();
		SRF_RETURN_DONE(funcctx);
	}
	funcctx = SRF_FIRSTCALL_INIT();

	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor)));
	if (!(portal->cursorOptions & GP_CURSOR_OPT_PARALLEL_RETRIEVE))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cursor is not a PARALLEL RETRIEVE CURSOR")));
	prc = prc_find(portal->queryDesc);
	if (prc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor)));

	if (prc->kind != GP_ENDPOINT_COORDINATOR)
	{
		finished = GpStreamWait(prc->stream,
								timeout < 0 ? -1 : (long) timeout * 1000);
		if (!finished && !GpStreamFailed(prc->stream) && prc_all_read(prc))
			finished = GpStreamWait(prc->stream, -1);
		(void) GP_FAULT("gp_wait_parallel_retrieve_cursor_after_udf");
		GpStreamRaise(prc->stream);
		for (int i = 0; finished && i < prc->nendpoints; i++)
			if (!GpStreamReaderDone(prc->stream, prc->readers[i]))
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("lost the connection to the endpoint on segment %d",
								prc->contents[i])));
	}
	else
	{
		TimestampTz deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
														   (long) Max(timeout, 0) * 1000);
		char	   *message = NULL;

		for (;;)
		{
			EndpointState state;

			CHECK_FOR_INTERRUPTS();
			LWLockAcquire(&endpoints->lock, LW_SHARED);
			state = slot_is(prc->slot, prc->generation)
				? endpoints->slots[prc->slot].state : ENDPOINT_FREE;
			if (state == ENDPOINT_RELEASED)
				message = pstrdup(endpoints->slots[prc->slot].message);
			LWLockRelease(&endpoints->lock);
			if (state == ENDPOINT_FINISHED || state == ENDPOINT_RELEASED)
			{
				finished = state == ENDPOINT_FINISHED;
				break;
			}
			if (timeout == 0 ||
				(timeout > 0 && GetCurrentTimestamp() >= deadline))
				break;
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 100, endpoint_wait_event());
			ResetLatch(MyLatch);
		}
		(void) GP_FAULT("gp_wait_parallel_retrieve_cursor_after_udf");
		if (message != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling MPP operation: \"%s\"", message)));
	}

	SRF_RETURN_NEXT(funcctx, BoolGetDatum(finished));
}

/* ------------------------------------------------------------------------- */
/* A segment's reader: the endpoint's sender                                 */
/* ------------------------------------------------------------------------- */

/* Let go of this backend's endpoint: its slot first, then its segment. */
static void
sender_free(void)
{
	EndpointSender *s = sender;

	if (s == NULL)
		return;
	sender = NULL;
	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (slot_is(s->slot, s->generation))
		slot_free(&endpoints->slots[s->slot]);
	LWLockRelease(&endpoints->lock);

	/*
	 * A retrieve session reading the queue finds it detached, and the slot
	 * gone: the cursor was aborted, not finished.
	 */
	if (s->queue != NULL)
		shm_mq_detach(s->queue);
	dsm_detach(s->seg);
	FreeTupleDesc(s->wire);
	pfree(s->records);
	pfree(s);
}

/*
 * gp_internal.endpoint_open(name text, cursorname text, session int4,
 *							 userid oid, token text, columns text)
 *
 * On a segment's reader, for the coordinator: the endpoint its slice will
 * send its rows to, which a retrieve session may attach to from now on.
 */
PG_FUNCTION_INFO_V1(gp_endpoint_open);

Datum
gp_endpoint_open(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *cursor = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			session = PG_GETARG_INT32(2);
	Oid			user = PG_GETARG_OID(3);
	char	   *hex = text_to_cstring(PG_GETARG_TEXT_PP(4));
	char	   *columns = text_to_cstring(PG_GETARG_TEXT_PP(5));
	uint8		token[ENDPOINT_TOKEN_LEN];
	shm_toc    *toc;
	shm_mq	   *mq;
	TupleDesc	described;
	EndpointSender *s;
	MemoryContext oldcxt;

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("an endpoint is opened only for the coordinator")));
	if (strlen(hex) != ENDPOINT_TOKEN_HEXLEN)
		elog(ERROR, "an endpoint's token is %d hexadecimal digits",
			 ENDPOINT_TOKEN_HEXLEN);
	hex_decode(hex, ENDPOINT_TOKEN_HEXLEN, (char *) token);

	endpoints_attach();
	register_callbacks();
	sender_free();

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	s = palloc0(sizeof(EndpointSender));
	s->seg = endpoint_segment(columns, true, &toc);
	mq = shm_mq_create(shm_toc_allocate(toc, ENDPOINT_QUEUE_SIZE),
					   ENDPOINT_QUEUE_SIZE);
	shm_toc_insert(toc, ENDPOINT_KEY_QUEUE, mq);
	shm_mq_set_sender(mq, MyProc);
	s->queue = shm_mq_attach(mq, s->seg, NULL);
	columns_read(columns, &described, &s->wire, &s->records);
	FreeTupleDesc(described);
	MemoryContextSwitchTo(oldcxt);

	PG_TRY();
	{
		LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
		s->slot = slot_alloc(name, cursor, session, user, token,
							 dsm_segment_handle(s->seg));
		s->generation = endpoints->slots[s->slot].generation;
		LWLockRelease(&endpoints->lock);
	}
	PG_CATCH();
	{
		shm_mq_detach(s->queue);
		dsm_detach(s->seg);
		PG_RE_THROW();
	}
	PG_END_TRY();
	sender = s;
	PG_RETURN_VOID();
}

/*
 * gp_internal.endpoint_release(name text)
 *
 * The cursor is closed: its endpoint on this reader goes, its rows all read.
 */
PG_FUNCTION_INFO_V1(gp_endpoint_release);

Datum
gp_endpoint_release(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!GpClusterDispatchTrusted())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("an endpoint is released only for the coordinator")));
	if (sender != NULL)
	{
		bool		mine;

		LWLockAcquire(&endpoints->lock, LW_SHARED);
		mine = slot_is(sender->slot, sender->generation) &&
			strcmp(endpoints->slots[sender->slot].name, name) == 0;
		LWLockRelease(&endpoints->lock);
		if (mine || !slot_is(sender->slot, sender->generation))
			sender_free();
	}
	PG_RETURN_VOID();
}

/*
 * gp_internal.share_publish(key text)
 *
 * On a segment's writer, for the coordinator: its transaction, as DECLARE's
 * statement has it, published for the readers that run a parallel retrieve
 * cursor's slices -- what the writer's own fragment publishes as it starts
 * where it runs one (gp_share.c).
 */
PG_FUNCTION_INFO_V1(gp_share_publish);

Datum
gp_share_publish(PG_FUNCTION_ARGS)
{
	GpSharePublish(text_to_cstring(PG_GETARG_TEXT_PP(0)), GetActiveSnapshot());
	PG_RETURN_VOID();
}

/* The DestReceiver of an endpoint's slice: each row, as it travels, queued. */
typedef struct QueueDest
{
	DestReceiver pub;
	EndpointSender *sender;
} QueueDest;

static bool
queue_receive(TupleTableSlot *slot, DestReceiver *self)
{
	EndpointSender *s = ((QueueDest *) self)->sender;
	MinimalTuple tuple = wire_tuple(slot, s->wire, s->records);
	shm_mq_result result;

	result = shm_mq_send(s->queue, tuple->t_len, tuple, false, false);
	pfree(tuple);
	if (result == SHM_MQ_DETACHED)
		return false;
	if (result != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not send tuple to shared-memory queue")));
	return true;
}

static void
queue_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	EndpointSender *s = ((QueueDest *) self)->sender;

	if (typeinfo->natts != s->wire->natts)
		elog(ERROR, "an endpoint's slice has %d columns, its cursor %d",
			 typeinfo->natts, s->wire->natts);
}

static void
queue_shutdown(DestReceiver *self)
{
}

static void
queue_destroy(DestReceiver *self)
{
	pfree(self);
}

DestReceiver *
GpEndpointRunDest(QueryDesc *queryDesc)
{
	Node	   *name = NULL;
	QueueDest  *dest;

	foreach_node(DefElem, def, queryDesc->plannedstmt->extension_state)
		if (strcmp(def->defname, GP_ENDPOINT_RUN_MARK) == 0)
			name = def->arg;
	if (name == NULL)
		return NULL;
	if (sender == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("endpoint %s is not open here", strVal(name))));

	dest = palloc0(sizeof(QueueDest));
	dest->pub.receiveSlot = queue_receive;
	dest->pub.rStartup = queue_startup;
	dest->pub.rShutdown = queue_shutdown;
	dest->pub.rDestroy = queue_destroy;
	dest->pub.mydest = DestTupleQueue;
	dest->sender = sender;
	return (DestReceiver *) dest;
}

/*
 * The slice has run: the queue is detached, which tells the retrieve session
 * the rows are all there, and the statement waits until it has read them --
 * FINISHED -- or given up, which cancels it (retrieve_cancel()), as
 * Cloudberry's DestroyEndpointExecState() waits.  The reader's connection to
 * the coordinator is watched meanwhile, as Cloudberry's wait_receiver()
 * watches it, and while the slice runs (client_connection_check_interval,
 * set for its transaction): a coordinator that went away takes it along.
 */
void
GpEndpointRunDone(void)
{
	EndpointSender *s = sender;

	if (s == NULL)
		return;
	if (s->queue != NULL)
	{
		shm_mq_detach(s->queue);
		s->queue = NULL;
	}

	for (;;)
	{
		EndpointState state;

		CHECK_FOR_INTERRUPTS();
		LWLockAcquire(&endpoints->lock, LW_SHARED);
		state = slot_is(s->slot, s->generation)
			? endpoints->slots[s->slot].state : ENDPOINT_FREE;
		LWLockRelease(&endpoints->lock);
		if (state == ENDPOINT_FINISHED)
			break;
		if (state == ENDPOINT_RELEASED || state == ENDPOINT_FREE)
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
		if (WaitLatchOrSocket(MyLatch,
							  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH |
							  WL_SOCKET_CLOSED,
							  MyProcPort->sock, 100,
							  endpoint_wait_event()) & WL_SOCKET_CLOSED)
			ereport(FATAL,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("connection to client lost")));
		ResetLatch(MyLatch);
	}

	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (slot_is(s->slot, s->generation))
		endpoints->slots[s->slot].sender_pid = -1;
	LWLockRelease(&endpoints->lock);
}

const char *
GpEndpointCancelMessage(void)
{
	EndpointSlot *slot;

	if (sender == NULL || endpoints == NULL)
		return NULL;
	slot = &endpoints->slots[sender->slot];
	return slot->generation == sender->generation && slot->message[0] != '\0'
		? slot->message : NULL;
}

/* ------------------------------------------------------------------------- */
/* The functions of the endpoints                                            */
/* ------------------------------------------------------------------------- */

/*
 * The endpoints of this node's database that a listing shows, copied with
 * their tokens: every one, or those this user may see -- a superuser every
 * one, and anyone their own.  Copied under the lock and shown after it, whose
 * rows look up names in the catalog.
 */
typedef struct EndpointCopy
{
	EndpointSlot slot;
	char		token[ENDPOINT_TOKEN_HEXLEN + 1];
} EndpointCopy;

static EndpointCopy *
endpoints_copy(bool all, int *n)
{
	EndpointCopy *copies = palloc_array(EndpointCopy, ENDPOINT_SLOTS);
	bool		super = superuser();
	Oid			user = GetUserId();

	*n = 0;
	endpoints_attach();
	LWLockAcquire(&endpoints->lock, LW_SHARED);
	for (int i = 0; i < ENDPOINT_SLOTS; i++)
	{
		EndpointSlot *slot = &endpoints->slots[i];
		EndpointToken *t;

		if (!slot->used || slot->database != MyDatabaseId ||
			!(all || super || slot->user == user))
			continue;
		copies[*n].slot = *slot;
		t = token_find(slot->session, slot->user);
		if (t != NULL)
			token_hex(t->token, copies[*n].token);
		else
			copies[*n].token[0] = '\0';
		(*n)++;
	}
	LWLockRelease(&endpoints->lock);
	return copies;
}

static char *
user_name(Oid user)
{
	char	   *name = GetUserNameFromId(user, true);

	return name != NULL ? name : "";
}

/*
 * gp_get_segment_endpoints()
 *
 * The endpoints of this node, Cloudberry's ten columns: any node's, the
 * coordinator's too, as a utility session there sees them in Cloudberry.
 */
PG_FUNCTION_INFO_V1(gp_get_segment_endpoints);

Datum
gp_get_segment_endpoints(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			n;
	EndpointCopy *copies;

	InitMaterializedSRF(fcinfo, 0);
	copies = endpoints_copy(false, &n);
	for (int i = 0; i < n; i++)
	{
		EndpointSlot *slot = &copies[i].slot;
		Datum		values[10];
		bool		nulls[10] = {0};

		values[0] = CStringGetTextDatum(copies[i].token);
		values[1] = ObjectIdGetDatum(slot->database);
		values[2] = Int32GetDatum(slot->sender_pid);
		values[3] = Int32GetDatum(slot->receiver_pid);
		values[4] = CStringGetTextDatum(endpoint_state_names[slot->state]);
		values[5] = Int32GetDatum(GpClusterContentId());
		values[6] = Int32GetDatum(slot->session);
		values[7] = CStringGetTextDatum(user_name(slot->user));
		values[8] = CStringGetTextDatum(slot->name);
		values[9] = CStringGetTextDatum(slot->cursor);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * gp_internal.segment_endpoints()
 *
 * For the coordinator's gp_get_endpoints(): this segment's endpoints of the
 * database, with who each is, for the coordinator to choose -- every user's
 * for the coordinator's own connection, which carries the cluster's secret,
 * and otherwise those its user may see.
 */
PG_FUNCTION_INFO_V1(gp_segment_endpoints);

Datum
gp_segment_endpoints(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			n;
	EndpointCopy *copies;

	InitMaterializedSRF(fcinfo, 0);
	copies = endpoints_copy(GpClusterDispatchTrusted(), &n);
	for (int i = 0; i < n; i++)
	{
		EndpointSlot *slot = &copies[i].slot;
		Datum		values[7];
		bool		nulls[7] = {0};

		values[0] = CStringGetTextDatum(copies[i].token);
		values[1] = CStringGetTextDatum(slot->cursor);
		values[2] = Int32GetDatum(slot->session);
		values[3] = ObjectIdGetDatum(slot->user);
		values[4] = CStringGetTextDatum(endpoint_state_names[slot->state]);
		values[5] = CStringGetTextDatum(slot->name);
		values[6] = Int32GetDatum(GpClusterContentId());
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* Where a node's retrieve sessions connect: its host and port. */
static void
node_address(int content, char **host, int *port)
{
	const GpSegmentConfig *node = content >= 0
		? GpClusterSegmentByContent(content) : GpClusterCoordinator();

	if (node != NULL)
	{
		*host = node->hostname;
		*port = node->port;
	}
	else
	{
		char		name[256];

		/* one node, as gp_segment_configuration lists it */
		if (gethostname(name, sizeof(name)) != 0)
			strlcpy(name, "localhost", sizeof(name));
		name[sizeof(name) - 1] = '\0';
		*host = pstrdup(name);
		*port = PostPortNumber;
	}
}

static void
put_endpoint_row(ReturnSetInfo *rsinfo, int content, const char *token,
				 const char *cursor, int session, Oid user, const char *state,
				 const char *name)
{
	Datum		values[9];
	bool		nulls[9] = {0};
	char	   *host;
	int			port;

	node_address(content, &host, &port);
	values[0] = Int32GetDatum(content);
	values[1] = CStringGetTextDatum(token);
	values[2] = CStringGetTextDatum(cursor);
	values[3] = Int32GetDatum(session);
	values[4] = CStringGetTextDatum(host);
	values[5] = Int32GetDatum(port);
	values[6] = CStringGetTextDatum(user_name(user));
	values[7] = CStringGetTextDatum(state);
	values[8] = CStringGetTextDatum(name);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * gp_get_endpoints()
 *
 * Every endpoint of the cluster the user may see, a superuser every one:
 * the segments', which each is asked for, as Cloudberry's dispatches
 * gp_get_segment_endpoints(), and then the coordinator's.
 */
PG_FUNCTION_INFO_V1(gp_get_endpoints);

Datum
gp_get_endpoints(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		super = superuser();
	int			n;
	EndpointCopy *copies;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH && !GpClusterIsSingleNode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp_get_endpoints() could only be called on QD")));
	InitMaterializedSRF(fcinfo, 0);

	if (!GpClusterIsSingleNode())
	{
		TupleDesc	desc = CreateTemplateTupleDesc(7);
		TupleTableSlot *row;
		GpGatherState *gather;

		TupleDescInitEntry(desc, 1, "auth_token", TEXTOID, -1, 0);
		TupleDescInitEntry(desc, 2, "cursorname", TEXTOID, -1, 0);
		TupleDescInitEntry(desc, 3, "sessionid", INT4OID, -1, 0);
		TupleDescInitEntry(desc, 4, "userid", OIDOID, -1, 0);
		TupleDescInitEntry(desc, 5, "state", TEXTOID, -1, 0);
		TupleDescInitEntry(desc, 6, "endpointname", TEXTOID, -1, 0);
		TupleDescInitEntry(desc, 7, "gp_segment_id", INT4OID, -1, 0);
		TupleDescFinalize(desc);
		row = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		gather = GpGatherStart("SELECT * FROM gp_internal.segment_endpoints()",
							   desc);
		while (GpGatherNext(gather, row, NULL))
		{
			bool		isnull;
			Oid			user;

			slot_getallattrs(row);
			user = DatumGetObjectId(slot_getattr(row, 4, &isnull));
			if (super || user == GetUserId())
				put_endpoint_row(rsinfo,
								 DatumGetInt32(row->tts_values[6]),
								 TextDatumGetCString(row->tts_values[0]),
								 TextDatumGetCString(row->tts_values[1]),
								 DatumGetInt32(row->tts_values[2]), user,
								 TextDatumGetCString(row->tts_values[4]),
								 TextDatumGetCString(row->tts_values[5]));
			ExecClearTuple(row);
		}
		GpGatherEnd(gather);
		ExecDropSingleTupleTableSlot(row);
	}

	copies = endpoints_copy(false, &n);
	for (int i = 0; i < n; i++)
	{
		EndpointSlot *slot = &copies[i].slot;

		put_endpoint_row(rsinfo, -1, copies[i].token, slot->cursor,
						 slot->session, slot->user,
						 endpoint_state_names[slot->state], slot->name);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------- */
/* The retrieve session                                                      */
/* ------------------------------------------------------------------------- */

bool
GpEndpointIsRetrieveSession(void)
{
	return retrieve_session;
}

/*
 * Take a setting out of the startup packet's options: "-c name=value",
 * "--name=value", or an option of its own.  What it said, or NULL.
 */
static char *
take_cmdline_option(Port *port, const char *name)
{
	char	  **av;
	int			maxac;
	int			ac = 0;
	char	   *found = NULL;
	StringInfoData rest;
	size_t		namelen = strlen(name);

	if (port->cmdline_options == NULL)
		return NULL;
	maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;
	av = palloc0_array(char *, maxac);
	pg_split_opts(av, &ac, port->cmdline_options);

	initStringInfo(&rest);
	for (int i = 0; i < ac; i++)
	{
		const char *opt = av[i];
		const char *setting = NULL;
		int			used = 1;

		if (strcmp(opt, "-c") == 0 && i + 1 < ac)
		{
			setting = av[i + 1];
			used = 2;
		}
		else if (strncmp(opt, "-c", 2) == 0)
			setting = opt + 2;
		else if (strncmp(opt, "--", 2) == 0)
			setting = opt + 2;

		if (setting != NULL && strncmp(setting, name, namelen) == 0 &&
			setting[namelen] == '=')
		{
			found = pstrdup(setting + namelen + 1);
			i += used - 1;
			continue;
		}
		for (int j = 0; j < used; j++)
		{
			if (rest.len > 0)
				appendStringInfoChar(&rest, ' ');
			for (const char *p = av[i + j]; *p; p++)
			{
				if (*p == ' ' || *p == '\\')
					appendStringInfoChar(&rest, '\\');
				appendStringInfoChar(&rest, *p);
			}
		}
		i += used - 1;
	}
	if (found != NULL)
		port->cmdline_options = MemoryContextStrdup(TopMemoryContext, rest.data);
	return found;
}

static char *
take_guc_option(Port *port, const char *name)
{
	char	   *found = NULL;
	List	   *kept = NIL;
	ListCell   *lc;
	MemoryContext oldcxt;

	for (lc = list_head(port->guc_options); lc != NULL; lc = lnext(port->guc_options, lc))
	{
		char	   *option = lfirst(lc);
		char	   *value;

		lc = lnext(port->guc_options, lc);
		value = lc != NULL ? lfirst(lc) : "";
		if (strcmp(option, name) == 0)
		{
			found = pstrdup(value);
			continue;
		}
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		kept = lappend(lappend(kept, option), value);
		MemoryContextSwitchTo(oldcxt);
		if (lc == NULL)
			break;
	}
	if (found != NULL)
		port->guc_options = kept;
	return found;
}

static char *
take_option(Port *port, const char *name)
{
	char	   *cmdline = take_cmdline_option(port, name);
	char	   *guc = take_guc_option(port, name);

	return guc != NULL ? guc : cmdline;
}

/* auth.c's recv_password_packet(), static there: the client's password. */
static char *
receive_password(Port *port)
{
	StringInfoData buf;
	int			mtype;

	pq_startmsgread();
	mtype = pq_getbyte();
	if (mtype != PqMsg_PasswordMessage)
	{
		if (mtype != EOF)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected password response, got message type %d",
							mtype)));
		return NULL;
	}
	initStringInfo(&buf);
	if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))
	{
		pfree(buf.data);
		return NULL;
	}
	if (strlen(buf.data) + 1 != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));
	return buf.data;
}

/*
 * A retrieve session's login, once the node's pg_hba.conf method has let
 * the user in: its token, Cloudberry's password or the port's setting, is
 * one of the user's on this node, which binds the session to the cursors'.
 */
static void
endpoint_client_auth(Port *port, int status)
{
	char	   *conn;
	char	   *token_text;
	uint8		token[ENDPOINT_TOKEN_LEN];
	bool		asked = false;
	Oid			user;
	int			session = -1;

	if (prev_client_auth)
		prev_client_auth(port, status);
	if (status != STATUS_OK)
		return;

	conn = take_option(port, "gp_retrieve_conn");
	if (conn == NULL)
		conn = take_option(port, "gp.retrieve_conn");
	token_text = take_option(port, "gp.retrieve_token");
	if (token_text == NULL &&
		(conn == NULL || !parse_bool(conn, &asked) || !asked))
		return;

	if (token_text == NULL)
	{
		sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);
		token_text = receive_password(port);
		if (token_text == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PASSWORD),
					 errmsg("Failed to Retrieve the authentication password")));
	}
	if (strlen(token_text) != ENDPOINT_TOKEN_HEXLEN ||
		strspn(token_text, "0123456789abcdefABCDEF") != ENDPOINT_TOKEN_HEXLEN)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("retrieve auth token is invalid")));
	hex_decode(token_text, ENDPOINT_TOKEN_HEXLEN, (char *) token);

	user = get_role_oid(port->user_name, false);
	endpoints_attach();
	LWLockAcquire(&endpoints->lock, LW_SHARED);
	for (int i = 0; i < ENDPOINT_SLOTS && session < 0; i++)
	{
		EndpointToken *t = &endpoints->tokens[i];

		if (t->refcount > 0 && t->user == user &&
			memcmp(t->token, token, ENDPOINT_TOKEN_LEN) == 0)
			session = t->session;
	}
	LWLockRelease(&endpoints->lock);
	if (session < 0)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("Authentication failure (Wrong password or no endpoint for the user)")));

	retrieve_session = true;
	retrieve_for = session;
	register_callbacks();
}

/*
 * A retrieve session runs RETRIEVE and nothing else: the query has to be the
 * SELECT O26 writes for one, Cloudberry's check (transformOptionalSelectInto())
 * made on what the rewrite gave -- the function's arguments constants, its
 * count a literal of int8 for that.
 */
static bool
is_retrieve_query(Query *query)
{
	RangeTblEntry *rte;
	RangeTblFunction *rtfunc;
	FuncExpr   *func;

	if (query->commandType != CMD_SELECT || query->utilityStmt != NULL ||
		list_length(query->rtable) != 1 || query->hasSubLinks ||
		query->cteList != NIL || query->setOperations != NULL ||
		query->jointree == NULL || query->jointree->quals != NULL ||
		list_length(query->jointree->fromlist) != 1 ||
		query->groupClause != NIL || query->havingQual != NULL ||
		query->sortClause != NIL || query->limitCount != NULL ||
		query->limitOffset != NULL || query->distinctClause != NIL ||
		query->hasAggs || query->hasWindowFuncs || query->hasTargetSRFs ||
		query->rowMarks != NIL)
		return false;
	rte = linitial_node(RangeTblEntry, query->rtable);
	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1)
		return false;
	rtfunc = linitial_node(RangeTblFunction, rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return false;
	func = (FuncExpr *) rtfunc->funcexpr;
	if (list_length(func->args) != 2 || !IsA(linitial(func->args), Const) ||
		!IsA(lsecond(func->args), Const) ||
		get_func_namespace(func->funcid) != get_namespace_oid("gp_internal", true) ||
		strcmp(get_func_name(func->funcid), "retrieve") != 0)
		return false;
	foreach_node(TargetEntry, tle, query->targetList)
		if (!IsA(tle->expr, Var))
			return false;
	return true;
}

static void
endpoint_post_parse_analyze(ParseState *pstate, Query *query,
							const JumbleState *jstate)
{
	if (prev_post_parse_analyze)
		prev_post_parse_analyze(pstate, query, jstate);
	if (retrieve_session && !is_retrieve_query(query))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("This is a retrieve connection, but the query is not a RETRIEVE.")));
}

/*
 * A function called by the fast path, which a retrieve session may not use,
 * as Cloudberry's refuses the message (forbidden_in_retrieve_handler()): the
 * one place a function is run in it with no portal active.
 */
static void
endpoint_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					   int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);
	if (access == OAT_FUNCTION_EXECUTE && retrieve_session &&
		ActivePortal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("protocol '%c' is not supported in a GPDB parallel retrieve cursor connection",
						'F')));
}

/*
 * The endpoint of that name of the session this one retrieves for, checked
 * as Cloudberry's validate_retrieve_endpoint() checks it the first time a
 * session retrieves from it; the lock is held.
 */
static int
retrieve_check(const char *name)
{
	int			found = -1;
	EndpointSlot *slot;

	for (int i = 0; i < ENDPOINT_SLOTS && found < 0; i++)
	{
		slot = &endpoints->slots[i];
		if (slot->used && slot->session == retrieve_for &&
			slot->database == MyDatabaseId &&
			strncmp(slot->name, name, NAMEDATALEN) == 0)
			found = i;
	}
	if (found < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the endpoint %s does not exist for session id %d",
						name, retrieve_for)));
	slot = &endpoints->slots[found];
	if (slot->user != GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("the PARALLEL RETRIEVE CURSOR was created by a different user"),
				 errhint("Use the same user as the PARALLEL RETRIEVE CURSOR creator to retrieve.")));
	switch (slot->state)
	{
		case ENDPOINT_FINISHED:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("another session (pid: %d) used the endpoint and completed retrieving",
							slot->receiver_pid)));
			break;
		case ENDPOINT_READY:
		case ENDPOINT_ATTACHED:
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("endpoint %s (state: %s) was used by another retrieve session (pid: %d)",
							name, endpoint_state_names[slot->state],
							slot->receiver_pid),
					 errdetail("If pid is -1, that session has been detached.")));
	}
	if (slot->receiver_pid != -1 && slot->receiver_pid != MyProcPid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("endpoint %s was already attached by receiver(pid: %d)",
						name, slot->receiver_pid),
				 errdetail("An endpoint can only be attached by one retrieving session.")));
	return found;
}

/* An entry of this session's still its endpoint's?  The lock is held. */
static void
retrieve_entry_check(RetrieveEntry *entry)
{
	if (!slot_is(entry->slot, entry->generation))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("endpoint is not available because the parallel retrieve cursor was aborted")));
}

static void
retrieve_entries_init(void)
{
	HASHCTL		ctl;

	if (retrieve_entries != NULL)
		return;
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(RetrieveEntry);
	retrieve_entries = hash_create("gp_core retrieve entries", 64, &ctl,
								   HASH_ELEM | HASH_STRINGS);
}

/*
 * What this session knows of the endpoint: the entry of one it retrieved
 * from before, still the endpoint, or, the first time, the endpoint checked
 * and attached -- this session its receiver, its segment and its queue or
 * its file this session's for the rest of it.
 */
static RetrieveEntry *
retrieve_attach(const char *name)
{
	RetrieveEntry *entry;
	bool		found;
	int			index;
	uint64		generation;
	dsm_handle	handle;
	EndpointSlot *slot;
	dsm_segment *seg;
	shm_toc    *toc;
	TupleDesc	columns;
	TupleDesc	wire;
	bool	   *records;
	MemoryContext oldcxt;

	retrieve_entries_init();
	entry = hash_search(retrieve_entries, name, HASH_FIND, &found);
	if (found)
	{
		LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
		retrieve_entry_check(entry);
		slot = &endpoints->slots[entry->slot];
		if (slot->state == ENDPOINT_READY || slot->state == ENDPOINT_ATTACHED)
			slot->state = ENDPOINT_RETRIEVING;
		LWLockRelease(&endpoints->lock);
		return entry;
	}

	/* its segment, attached before the endpoint is this session's */
	LWLockAcquire(&endpoints->lock, LW_SHARED);
	index = retrieve_check(name);
	generation = endpoints->slots[index].generation;
	handle = endpoints->slots[index].handle;
	LWLockRelease(&endpoints->lock);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	seg = dsm_attach(handle);
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("endpoint is not available because the parallel retrieve cursor was aborted")));
	dsm_pin_mapping(seg);
	toc = shm_toc_attach(ENDPOINT_TOC_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid magic number in dynamic shared memory segment")));
	columns_read(shm_toc_lookup(toc, ENDPOINT_KEY_COLUMNS, false), &columns,
				 &wire, &records);
	FreeTupleDesc(columns);
	MemoryContextSwitchTo(oldcxt);

	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (!slot_is(index, generation) || retrieve_check(name) != index)
	{
		LWLockRelease(&endpoints->lock);
		dsm_detach(seg);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("endpoint is not available because the parallel retrieve cursor was aborted")));
	}
	slot = &endpoints->slots[index];
	slot->receiver_pid = MyProcPid;
	slot->state = ENDPOINT_RETRIEVING;
	entry = hash_search(retrieve_entries, name, HASH_ENTER, NULL);
	entry->slot = index;
	entry->generation = generation;
	entry->seg = seg;
	entry->queue = NULL;
	entry->fileset = shm_toc_lookup(toc, ENDPOINT_KEY_FILESET, true);
	entry->file = NULL;
	entry->fileno = 0;
	entry->offset = 0;
	entry->wire = wire;
	entry->records = records;
	entry->state = RETRIEVE_INIT;
	LWLockRelease(&endpoints->lock);

	if (entry->fileset == NULL)
	{
		shm_mq	   *mq = shm_toc_lookup(toc, ENDPOINT_KEY_QUEUE, false);

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		shm_mq_set_receiver(mq, MyProc);
		entry->queue = shm_mq_attach(mq, seg, NULL);
		MemoryContextSwitchTo(oldcxt);
	}
	return entry;
}

/* The endpoint's rows are all read: FINISHED, which its sender waits for. */
static void
retrieve_finished(RetrieveEntry *entry)
{
	entry->state = RETRIEVE_FINISHED;
	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (slot_is(entry->slot, entry->generation))
	{
		EndpointSlot *slot = &endpoints->slots[entry->slot];

		slot->state = ENDPOINT_FINISHED;
		slot_wake_sender(slot);
	}
	LWLockRelease(&endpoints->lock);
}

/*
 * The endpoint's next row, as it travels, into the slot; false when there
 * is none left.  A sender that went away before sending its last row was
 * aborted: its slot is gone before its queue.
 */
static bool
retrieve_next(RetrieveEntry *entry, TupleTableSlot *slot)
{
	CHECK_FOR_INTERRUPTS();
	(void) GP_FAULT("fetch_tuples_from_endpoint");
	entry->state = RETRIEVE_RECEIVING;

	if (entry->queue != NULL)
	{
		Size		nbytes;
		void	   *data;
		shm_mq_result result = shm_mq_receive(entry->queue, &nbytes, &data,
											  false);

		if (result == SHM_MQ_SUCCESS)
		{
			MinimalTuple tuple = palloc(nbytes);

			memcpy(tuple, data, nbytes);
			ExecStoreMinimalTuple(tuple, slot, true);
			return true;
		}
		LWLockAcquire(&endpoints->lock, LW_SHARED);
		retrieve_entry_check(entry);
		LWLockRelease(&endpoints->lock);
		shm_mq_detach(entry->queue);
		entry->queue = NULL;
	}
	else if (entry->fileset != NULL)
	{
		uint32		len;

		/*
		 * The file, opened for this RETRIEVE, where the last one stopped: a
		 * file is the transaction's, and each RETRIEVE is one of its own.
		 */
		if (entry->file == NULL)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(TopTransactionContext);

			LWLockAcquire(&endpoints->lock, LW_SHARED);
			retrieve_entry_check(entry);
			LWLockRelease(&endpoints->lock);
			entry->file = BufFileOpenFileSet(entry->fileset, ENDPOINT_FILE,
											 O_RDONLY, false);
			MemoryContextSwitchTo(oldcxt);
			if (BufFileSeek(entry->file, entry->fileno, entry->offset,
							SEEK_SET) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in the endpoint's file")));
		}
		if (BufFileReadMaybeEOF(entry->file, &len, sizeof(len), true) == sizeof(len))
		{
			MinimalTuple tuple = palloc(len);

			BufFileReadExact(entry->file, tuple, len);
			ExecStoreMinimalTuple(tuple, slot, true);
			return true;
		}
	}

	if (entry->state != RETRIEVE_FINISHED)
		retrieve_finished(entry);
	return false;
}

/*
 * RETRIEVE's end: the endpoint is ATTACHED to this session, some of its
 * rows still to read, or FINISHED; resetting, this session is no longer its
 * receiver.
 */
static void
retrieve_done(RetrieveEntry *entry, bool reset)
{
	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (slot_is(entry->slot, entry->generation))
	{
		EndpointSlot *slot = &endpoints->slots[entry->slot];

		if (reset && slot->receiver_pid == MyProcPid)
			slot->receiver_pid = -1;
		if (slot->state == ENDPOINT_RETRIEVING)
			slot->state = entry->state == RETRIEVE_FINISHED
				? ENDPOINT_FINISHED : ENDPOINT_ATTACHED;
	}
	LWLockRelease(&endpoints->lock);
}

/*
 * This session gave up on an endpoint it had not read to the end: RELEASED,
 * and its sender cancelled, saying why, as Cloudberry's
 * retrieve_cancel_action() does.  The coordinator's endpoint has no sender
 * but the cursor's session, which finds it so as it waits or closes.
 */
static void
retrieve_cancel(RetrieveEntry *entry, const char *message)
{
	LWLockAcquire(&endpoints->lock, LW_EXCLUSIVE);
	if (slot_is(entry->slot, entry->generation))
	{
		EndpointSlot *slot = &endpoints->slots[entry->slot];

		if (slot->receiver_pid == MyProcPid &&
			slot->state != ENDPOINT_FINISHED)
		{
			slot->receiver_pid = -1;
			slot->state = ENDPOINT_RELEASED;
			strlcpy(slot->message, message, ENDPOINT_MESSAGE_LEN);
			if (entry->fileset == NULL && slot->sender_pid > 0)
				(void) kill(slot->sender_pid, SIGINT);
			slot_wake_sender(slot);
		}
	}
	LWLockRelease(&endpoints->lock);
}

/*
 * gp_internal.retrieve(endpoint text, count int8) RETURNS SETOF record
 *
 * A retrieve session's RETRIEVE, as O26 rewrites it: the next count rows of
 * the endpoint, every one left for -1, in the columns the rewrite gave it,
 * which have to be the endpoint's.
 */
PG_FUNCTION_INFO_V1(gp_retrieve);

Datum
gp_retrieve(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		count = PG_GETARG_INT64(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	RetrieveEntry *entry;
	TupleDesc	desc;
	TupleTableSlot *slot;
	Datum	   *values;
	bool	   *nulls;
	MemoryContext rowcxt;

	if (!retrieve_session)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("This is not a retrieve connection, but the query is a RETRIEVE.")));
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	desc = rsinfo->setDesc;

	entry = retrieve_attach(name);
	retrieve_current = entry;
	if (count <= 0 && count != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RETRIEVE statement only supports forward scan, count should not be: " INT64_FORMAT,
						count)));
	if (desc->natts != Max(entry->wire->natts, 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("the columns do not match endpoint %s's", name)));
	for (int i = 0; i < entry->wire->natts; i++)
		if (TupleDescAttr(desc, i)->atttypid !=
			(entry->records[i] ? RECORDOID : TupleDescAttr(entry->wire, i)->atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("the columns do not match endpoint %s's", name)));

	slot = MakeSingleTupleTableSlot(entry->wire, &TTSOpsMinimalTuple);
	values = palloc0_array(Datum, desc->natts);
	nulls = palloc_array(bool, desc->natts);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "gp_retrieve row",
								   ALLOCSET_DEFAULT_SIZES);
	while (entry->state != RETRIEVE_FINISHED && (count == -1 || count-- > 0))
	{
		MemoryContext oldcxt;

		MemoryContextReset(rowcxt);
		oldcxt = MemoryContextSwitchTo(rowcxt);
		if (!retrieve_next(entry, slot))
		{
			MemoryContextSwitchTo(oldcxt);
			break;
		}
		slot_getallattrs(slot);
		for (int i = 0; i < desc->natts; i++)
		{
			nulls[i] = i >= entry->wire->natts || slot->tts_isnull[i];
			if (nulls[i])
				continue;
			values[i] = slot->tts_values[i];
			if (entry->records[i])
			{
				bytea	   *wired = DatumGetByteaPP(values[i]);
				StringInfoData buf;

				initReadOnlyStringInfo(&buf, VARDATA_ANY(wired),
									   VARSIZE_ANY_EXHDR(wired));
				values[i] = GpRecordWireRead(&buf);
			}
		}
		MemoryContextSwitchTo(oldcxt);
		tuplestore_putvalues(rsinfo->setResult, desc, values, nulls);
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	MemoryContextDelete(rowcxt);
	if (entry->file != NULL)
	{
		BufFileTell(entry->file, &entry->fileno, &entry->offset);
		BufFileClose(entry->file);
		entry->file = NULL;
	}

	retrieve_done(entry, false);
	retrieve_current = NULL;
	return (Datum) 0;
}

/*
 * O26's RETRIEVE { ALL | count } FROM ENDPOINT name, in a retrieve session:
 * a SELECT of gp_internal.retrieve() with the endpoint's columns, each named
 * as the cursor's -- a column definition list's names apart, since a
 * cursor's may be the same -- and each of its type, which the endpoint is
 * first checked for, as Cloudberry's GetRetrieveStmtTupleDesc() does.
 */
char *
GpEndpointRetrieveSql(const char *endpoint, bool all, int64 count)
{
	dsm_handle	handle = DSM_HANDLE_INVALID;
	dsm_segment *seg;
	shm_toc    *toc;
	TupleDesc	columns;
	TupleDesc	wire;
	bool	   *records;
	RetrieveEntry *entry = NULL;
	StringInfoData select;
	StringInfoData coldefs;

	if (!retrieve_session)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("This is not a retrieve connection, but the query is a RETRIEVE.")));
	endpoints_attach();

	/* one this session retrieved from, or one it may */
	if (retrieve_entries != NULL)
		entry = hash_search(retrieve_entries, endpoint, HASH_FIND, NULL);
	LWLockAcquire(&endpoints->lock, LW_SHARED);
	if (entry != NULL)
		retrieve_entry_check(entry);
	else
		handle = endpoints->slots[retrieve_check(endpoint)].handle;
	LWLockRelease(&endpoints->lock);

	if (entry != NULL)
		seg = entry->seg;
	else
	{
		seg = dsm_attach(handle);
		if (seg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("endpoint is not available because the parallel retrieve cursor was aborted")));
	}
	toc = shm_toc_attach(ENDPOINT_TOC_MAGIC, dsm_segment_address(seg));
	columns_read(shm_toc_lookup(toc, ENDPOINT_KEY_COLUMNS, false), &columns,
				 &wire, &records);
	if (entry == NULL)
		dsm_detach(seg);
	if (!all && count <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RETRIEVE statement only supports forward scan, count should not be: " INT64_FORMAT,
						count)));

	initStringInfo(&select);
	initStringInfo(&coldefs);
	appendStringInfoString(&select, "SELECT ");
	for (int i = 0; i < columns->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(columns, i);

		appendStringInfo(&select, "%sr.c%d AS %s", i > 0 ? ", " : "", i + 1,
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(&coldefs, "%sc%d %s", i > 0 ? ", " : "", i + 1,
						 format_type_extended(att->atttypid, att->atttypmod,
											  FORMAT_TYPE_TYPEMOD_GIVEN |
											  FORMAT_TYPE_FORCE_QUALIFY));
	}
	if (columns->natts == 0)
		appendStringInfoString(&coldefs, "c0 pg_catalog.bool");
	appendStringInfo(&select, " FROM gp_internal.retrieve(%s, '" INT64_FORMAT "') AS r(%s)",
					 quote_literal_cstr(endpoint), all ? (int64) -1 : count,
					 coldefs.data);
	return select.data;
}

/*
 * A retrieve session's statement failed or its session ends: the endpoint
 * it was reading, or every one it had not read to the end, is given up.
 */
static void
retrieve_abort(bool quitting)
{
	if (retrieve_current != NULL)
	{
		/* its file, which the transaction's end closes */
		retrieve_current->file = NULL;
		if (retrieve_current->state != RETRIEVE_FINISHED)
			retrieve_cancel(retrieve_current,
							quitting ? "Endpoint retrieve session is quitting. All unfinished parallel retrieve cursors on the session will be terminated."
							: "Endpoint retrieve statement aborted");
		retrieve_done(retrieve_current, true);
		retrieve_current = NULL;
	}
	if (quitting && retrieve_entries != NULL)
	{
		HASH_SEQ_STATUS status;
		RetrieveEntry *entry;

		hash_seq_init(&status, retrieve_entries);
		while ((entry = hash_seq_search(&status)) != NULL)
		{
			if (entry->state != RETRIEVE_FINISHED)
				retrieve_cancel(entry,
								"Endpoint retrieve session is quitting. All unfinished parallel retrieve cursors on the session will be terminated.");
			if (entry->queue != NULL)
				shm_mq_detach(entry->queue);
			entry->queue = NULL;
			if (entry->seg != NULL)
				dsm_detach(entry->seg);
			entry->seg = NULL;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Transactions and the backend's end                                        */
/* ------------------------------------------------------------------------- */

static void
endpoint_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT)
		return;
	if (retrieve_session)
		retrieve_abort(false);
	sender_free();
	prc_abort(InvalidSubTransactionId);
}

static void
endpoint_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_COMMIT_SUB:
			foreach_ptr(PrcCursor, prc, prc_cursors)
				if (prc->subid == mySubid)
					prc->subid = parentSubid;
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			if (retrieve_session)
				retrieve_abort(false);
			prc_abort(mySubid);
			break;
		default:
			break;
	}
}

static void
endpoint_exit(int code, Datum arg)
{
	if (retrieve_session)
		retrieve_abort(true);
	if (endpoints != NULL)
		sender_free();
}

void
GpEndpointInit(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = endpoint_ProcessUtility;
	prev_planner = planner_hook;
	planner_hook = endpoint_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = endpoint_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = endpoint_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = endpoint_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = endpoint_ExecutorEnd;
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = endpoint_explain_per_plan;
	prev_client_auth = ClientAuthentication_hook;
	ClientAuthentication_hook = endpoint_client_auth;
	prev_post_parse_analyze = post_parse_analyze_hook;
	post_parse_analyze_hook = endpoint_post_parse_analyze;
	prev_object_access = object_access_hook;
	object_access_hook = endpoint_object_access;
}
