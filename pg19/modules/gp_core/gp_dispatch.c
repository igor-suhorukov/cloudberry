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
 * gp_dispatch.c
 *	  Reaching the segments: gangs, statements, and rows on the way back.
 *
 * Cloudberry's dispatcher marks its connections as internal with high bits in
 * the protocol version, skips pg_hba for them, and sends plans in messages of
 * its own.  PostgreSQL 19 will have none of it: a protocol major above 3 is
 * rejected before anything else, an unknown message type ends the session, and
 * there is no hook in the message loop.  So the port's dispatcher is an
 * ordinary libpq client of an ordinary backend, and what makes that backend a
 * segment process is a startup setting it carries.  That is the design
 * "Dispatch" in the plan describes, and it needs no core patch.
 *
 * Authentication is therefore real authentication: decision 5 asks for SCRAM
 * on the early milestones, and "gp.internal_passfile" names the password file
 * the dispatcher hands libpq.  A file, not a setting: a setting is readable by
 * any user who can SHOW it, and this is a password for every database in the
 * cluster.  And for certificates in production: "gp.internal_sslmode",
 * "gp.internal_sslcert", "gp.internal_sslkey", "gp.internal_sslrootcert" and
 * "gp.internal_sslcrl" are libpq's options of the same names, the TLS a node
 * asks of the node it connects to, the certificate it shows, and what it
 * checks the other's by.  A segment's pg_hba.conf then takes the node's
 * certificate for any role, through a map (cert map=...), as Cloudberry's
 * takes its internal connections without a password.  Every connection gp_core
 * opens to another node carries them (GpInternalConnOptions()).
 *
 * THE TRANSACTION.  Whatever the segments are sent is done inside the
 * coordinator's transaction: the first statement a transaction dispatches
 * opens one on every segment, a savepoint here is a savepoint there once
 * something is sent inside it, and the segments commit when the coordinator
 * does and roll back when it does.  So BEGIN; CREATE TABLE ...; ROLLBACK
 * leaves no table anywhere, and a statement that fails on a segment undoes
 * itself on the coordinator.  Each statement is sent with the coordinator's
 * snapshot of it, which the segments read as a distributed one, and a
 * transaction that wrote on a segment commits in two phases, decided by the
 * coordinator's own commit record (gp_dtx.c).
 *
 * READERS.  A statement whose slices run at once needs more than one
 * backend on a segment: the writer runs one slice, and each other slice runs
 * on a reader -- Cloudberry's reader gangs -- which is one more connection of
 * the session to the segment, with the writer's identity, kept for the
 * session once opened.  A reader's transaction is its own, begun for each
 * slice REPEATABLE READ and READ ONLY and read as a part of the writer's
 * (gp_share.c), so the coordinator's transaction, its savepoints and its
 * commit remain the writer's alone.  The readers are in the gang's wait set:
 * a slice that fails on one fails whatever the coordinator is waiting for,
 * and every reader is stopped and heard first, so that the error raised is
 * the one that caused the rest.
 *
 * Cloudberry sources this file stands in for:
 *	  src/backend/cdb/dispatcher/ (cdbdisp.c, cdbdisp_query.c, cdbconn.c,
 *	  cdbgang.c), less the parts that exist because Cloudberry speaks its own
 *	  protocol, and the one-phase half of cdbtm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/objectaddress.h"
#include "commands/seclabel.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "common/keywords.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "parser/parser.h"
#include "parser/scanner.h"
#include "portability/instr_time.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/waiteventset.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "gp_cluster.h"
#include "gp_core_api.h"
#include "gp_dispatch.h"
#include "gp_dtm_debug.h"
#include "gp_dtx.h"
#include "gp_endpoint.h"
#include "gp_fault.h"
#include "gp_fts.h"
#include "gp_gdd.h"
#include "gp_grammar_int.h"
#include "gp_ic.h"
#include "gp_label.h"
#include "gp_log.h"
#include "gp_loopback.h"
#include "gp_motion.h"
#include "gp_settings.h"

/* Where libpq finds the password for the segments; see the file header. */
static char *gp_internal_passfile = NULL;

/* The TLS of the connections between nodes, libpq's; see the file header. */
static char *gp_internal_sslmode = NULL;
static char *gp_internal_sslcert = NULL;
static char *gp_internal_sslkey = NULL;
static char *gp_internal_sslrootcert = NULL;
static char *gp_internal_sslcrl = NULL;

/*
 * How often, and how far apart, a gang is tried again while a segment is in
 * recovery: Cloudberry's settings (cdbgang_async.c).
 */
static int	gp_gang_creation_retry_count = 5;
static int	gp_gang_creation_retry_timer = 2000;

/*
 * How long a gang's connections may take, in seconds, 0 for ever: Cloudberry's
 * gp_segment_connect_timeout, the deadline its createGang_async() gives all
 * of a gang's at once (cdbgang_async.c).
 */
static int	gp_segment_connect_timeout = 180;

/*
 * The TCP keepalives of the dispatcher's connections to the segments:
 * Cloudberry's gp_dispatch_keepalives_idle, _interval and _count
 * (cdbconn.c), 0 the system's default.
 */
static int	gp_dispatch_keepalives_idle = 0;
static int	gp_dispatch_keepalives_interval = 0;
static int	gp_dispatch_keepalives_count = 0;

/*
 * gp.log_gang: Cloudberry's gp_log_gang, how much the dispatcher says of its
 * gang in the server log -- the gang made and let go, each connection, each
 * statement sent.
 */
typedef enum GangLogLevel
{
	GANG_LOG_OFF,
	GANG_LOG_TERSE,
	GANG_LOG_VERBOSE,
	GANG_LOG_DEBUG,
} GangLogLevel;

static int	gp_log_gang = GANG_LOG_OFF;

static const struct config_enum_entry gp_log_gang_options[] = {
	{"off", GANG_LOG_OFF, false},
	{"terse", GANG_LOG_TERSE, false},
	{"verbose", GANG_LOG_VERBOSE, false},
	{"debug", GANG_LOG_DEBUG, false},
	{NULL, 0, false}
};

#define GANG_LOG(level, ...) \
	do { \
		if (gp_log_gang >= (level)) \
			ereport(LOG, errmsg_internal(__VA_ARGS__)); \
	} while (0)

/*
 * The settings a segment has to share with the coordinator for a statement to
 * mean the same thing there: which schema a name is looked up in, which role
 * is doing it, how a date is read and written, where a table goes.
 * Cloudberry marks the ones it ships with GUC_GPDB_NEED_SYNC, 189 of them;
 * these are the ones anything the port dispatches yet can tell apart.  A
 * tablespace is every node's, of the same name (gp_ddl.c), so the ones a
 * relation or a temporary file is made in are too.
 */
static const char *const synced_settings[] = {
	/*
	 * the session user, SET SESSION AUTHORIZATION's, first: what a statement
	 * a segment decides for itself -- CLUSTER and VACUUM of every table the
	 * user may -- it decides as the coordinator's user, not as the
	 * connection's, where the connection may take it (sync_value()); and the
	 * settings only a superuser sets, and "role", are set as the session user
	 * it sets
	 */
	"session_authorization",
	/*
	 * PostGIS raster's, which its functions read on a segment as on the
	 * coordinator: where GDAL finds its data, which drivers it may use and
	 * whether a raster's file outside the database may be read -- settings
	 * only a superuser sets, sent only for one (sync_value()), and before
	 * "role", while the connection is still the session user's -- and the
	 * options a session gives GDAL's virtual file systems.  Passed over where
	 * postgis_raster is not loaded, and not set.
	 */
	"postgis.gdal_datapath",
	"postgis.gdal_enabled_drivers",
	"postgis.enable_outdb_rasters",
	"postgis.gdal_cpl_debug",
	"postgis.gdal_vsi_options",
	/*
	 * what a segment logs, as Cloudberry's segments take it from the
	 * coordinator: which messages, and a statement with them, and each
	 * statement's time -- settings only a superuser sets, likewise
	 */
	"log_min_messages",
	"log_min_error_statement",
	"log_min_duration_statement",
	/*
	 * gp_stats_collector's, which Cloudberry syncs (GUC_GPDB_NEED_SYNC):
	 * whether a segment's collector reports, what, and where -- settings
	 * only a superuser sets, likewise
	 */
	"gpsc.enable",
	"gpsc.enable_analyze",
	"gpsc.enable_cdbstats",
	"gpsc.ignored_users_list",
	"gpsc.logging_mode",
	"gpsc.uds_path",
	"gpsc.max_text_size",
	"gpsc.max_plan_size",
	/*
	 * vexec's debug settings, which only a superuser sets: a statement that
	 * fails where no vector node was built, the plan check, and the layouts
	 * drawn at random (pg_vector_executor.md §3.12) -- likewise
	 */
	"vexec.debug_require_vector",
	"vexec.debug_check_plans",
	"vexec.debug_layout_seed",
	"search_path",
	"role",
	"DateStyle",
	"IntervalStyle",
	"TimeZone",
	"default_table_access_method",
	"default_tablespace",
	"temp_tablespaces",
	"check_function_bodies",
	"bytea_output",
	"extra_float_digits",
	"standard_conforming_strings",
	"xmloption",
	"lc_monetary",
	"lc_numeric",
	"lc_time",
	/* to_tsvector() and its kin of one argument, sent in a scan's conditions */
	"default_text_search_config",
	/*
	 * which messages a segment sends: a LOG one too where the client asks
	 * for it, as Cloudberry's segments send it (segment_notice_receiver())
	 */
	"client_min_messages",
	/*
	 * gp_ao's, which a segment's scans and VACUUM read -- a module's setting
	 * is sent where the module is loaded, and passed over where it is not
	 */
	"gp.select_invisible",
	"gp.appendonly_compaction",
	"gp.appendonly_compaction_threshold",
	"gp.appendonly_insert_files",
	"gp.appendonly_insert_files_tuples_range",
	/*
	 * gp_exttable's, which a segment's scan of an external table reads: the
	 * statement's name and text, for gpfdist and a command's environment, and
	 * how rows are rejected and read.  Not gp.external_enable_exec, which
	 * only a superuser sets, and the coordinator checks as it plans.
	 */
	"gp_exttable.statement_id",
	"gp_exttable.query_string",
	"gp.external_max_segs",
	"gp.initial_bad_row_limit",
	"gp.reject_percent_threshold",
	"gp.readable_external_table_timeout",
	"gp.gpfdist_retry_timeout",
	"gp.writable_external_table_bufsize",
	"gp.verify_gpfdists_cert",
	/* the statement's count, which the slots of query metrics carry */
	"gp.command_count",
	/* what EXPLAIN ANALYZE asks a segment to measure (gp_explain.c) */
	"gp.explain_instrument",
	/* the UDP interconnect's, which the segments' senders and receivers use */
	"gp.interconnect_queue_depth",
	"gp.max_packet_size",
	"gp.interconnect_transmit_timeout",
	"gp.interconnect_min_rto",
	"gp.interconnect_default_rtt",
	"gp.debug_print_slice_table",
	"gp.interconnect_snd_queue_depth",
	"gp.interconnect_fc_method",
	"gp.interconnect_min_retries_before_timeout",
	"gp.interconnect_debug_retry_interval",
	"gp.interconnect_cache_future_packets",
	"gp.interconnect_timer_period",
	"gp.interconnect_timer_checking_period",
	"gp.udpic_dropacks_percent",
	"gp.udpic_dropxmit_percent",
	"gp.log_interconnect",
	/* udp2's, which both ends of its connections read, passed over where not loaded */
	"gp.interconnect_full_crc",
	"gp.udpic_dropseg",
	"gp.udpic_fault_inject_percent",
	"gp.udpic_fault_inject_bitmap",
	"gp.udpic_network_disable_ipv6",
	/*
	 * gp_resource's, what the coordinator's resource manager says of the
	 * statement: the weight its queue's priority gives it, the group it runs
	 * in, and the memory it is given
	 */
	"gp_resource.statement",
	/* PAX's, which a segment's scans and writers read */
	"gp.enable_predicate_pushdown",
	"pax.enable_debug",
	"pax.enable_sparse_filter",
	"pax.enable_row_filter",
	"pax.scan_reuse_buffer_size",
	"pax.max_tuples_per_group",
	"pax.max_tuples_per_file",
	"pax.max_size_per_file",
	"pax.enable_toast",
	"pax.min_size_of_compress_toast",
	"pax.min_size_of_external_toast",
	"pax.default_storage_format",
	"pax.bloom_filter_work_memory_bytes",
	"pax.log_filter_tree",
	/*
	 * pgvector's, which a segment's scans of its HNSW and IVFFlat indexes
	 * read: how many candidates an HNSW scan keeps and how many lists an
	 * IVFFlat scan probes, and whether and how far either goes on when a
	 * filter leaves too few -- a stock extension's settings, which Cloudberry
	 * sends a segment as it sends every SET, and its function EXECUTE ON ALL
	 * SEGMENTS scans each segment's index with.  Passed over where pgvector's
	 * library is not loaded, and not set.
	 */
	"hnsw.ef_search",
	"hnsw.iterative_scan",
	"hnsw.max_scan_tuples",
	"hnsw.scan_mem_multiplier",
	"ivfflat.probes",
	"ivfflat.iterative_scan",
	"ivfflat.max_probes",
	/*
	 * the workfile manager's limits of a statement, which a segment's
	 * processes hold their temporary files to (gp_workfile.c)
	 */
	"gp.workfile_limit_per_query",
	"gp.workfile_limit_files_per_query",
	/* the runtime filters of a segment's hash joins (gp_rtfilter.c) */
	"gp.enable_runtime_filter",
	"gp.enable_runtime_filter_pushdown",
	/*
	 * gp_stats_collector's the session may set, which Cloudberry syncs
	 * (GUC_GPDB_NEED_SYNC): which statements a segment's collector reports,
	 * and the coordinator's count of its client's statements, which it
	 * reports them under
	 */
	"gpsc.enable_utility",
	"gpsc.report_nested_queries",
	"gpsc.min_analyze_time",
	"gpsc.command_count",
	/*
	 * whether a segment's writer may start parallel workers, and how many and
	 * where they pay, as PostgreSQL's planner and Gather read them there
	 * (gp_parallel.c) -- not debug_parallel_query, which would put a Gather
	 * above every query a segment plans with them; a reader, a member of its
	 * writer's lock group, plans none whatever they say (gp_share.c)
	 */
	"gp.enable_parallel",
	"max_parallel_workers_per_gather",
	"parallel_setup_cost",
	"parallel_tuple_cost",
	"min_parallel_table_scan_size",
	"min_parallel_index_scan_size",
	"parallel_leader_participation",
	"enable_parallel_append",
	"enable_parallel_hash",
	/*
	 * the size of a segment's buffers for its temporary tables, which it
	 * takes until it has used one and refuses after, as the coordinator does
	 * -- sent as a SET runs (GpDispatchSyncSettingsNow()), so that the SET is
	 * what a segment's refusal fails
	 */
	"temp_buffers",
	/*
	 * whether a function a segment runs in its share of a plan may write,
	 * which the segment reads as it plans the function's queries
	 * (gp_motion.c), as Cloudberry syncs allow_segment_DML
	 */
	"gp.allow_segment_dml",
	/*
	 * vexec's, the vectorized executor's (pg_vector_executor.md §3.10,
	 * §3.12), which a segment reads where it plans for itself -- the gather
	 * route's SQL, through PostgreSQL's planner and vexec's path hooks -- and
	 * where its vector nodes begin: whether and how vector plans are made,
	 * their prices, the in-memory format of batches, and whether a GPU is
	 * used.  A fragment ORCA planned carries its decisions in its plan; these
	 * are what a segment decides itself.  Passed over where vexec is not
	 * loaded.  Not vexec.gpu_segments, the coordinator's alone, nor the
	 * GPU's postmaster settings, each server's own.  Its debug settings, a
	 * superuser's, are with the others above "role".
	 */
	"vexec.mode",
	"vexec.cpu_tuple_factor",
	"vexec.cpu_operator_factor",
	"vexec.convert_cost",
	"vexec.batch_setup_cost",
	"vexec.min_rows",
	"vexec.enable_scan",
	"vexec.enable_agg",
	"vexec.enable_hashjoin",
	"vexec.enable_sort",
	"vexec.enable_window",
	"vexec.enable_insert",
	"vexec.orca",
	"vexec.compact_threshold",
	"vexec.batch_format",
	"vexec.batch_varlena_layout",
	"vexec.batch_bool_layout",
	"vexec.batch_temporal_layout",
	"vexec.batch_numeric_layout",
	"vexec.batch_dictionary",
	"vexec.gpu",
	"vexec.gpu_setup_cost",
	"vexec.gpu_transfer_cost",
	"vexec.gpu_operator_factor",
};

#define NUM_SYNCED_SETTINGS	lengthof(synced_settings)

/* The ones a segment takes from a superuser only, and so is sent by one. */
static const char *const superuser_settings[] = {
	"postgis.gdal_datapath",
	"postgis.gdal_enabled_drivers",
	"postgis.enable_outdb_rasters",
	"postgis.gdal_cpl_debug",
	"log_min_messages",
	"log_min_error_statement",
	"log_min_duration_statement",
	"gpsc.enable",
	"gpsc.enable_analyze",
	"gpsc.enable_cdbstats",
	"gpsc.ignored_users_list",
	"gpsc.logging_mode",
	"gpsc.uds_path",
	"gpsc.max_text_size",
	"gpsc.max_plan_size",
	"vexec.debug_require_vector",
	"vexec.debug_check_plans",
	"vexec.debug_layout_seed",
};

/*
 * Whether the segments have a table access method of this database's: one
 * made before this transaction, as CREATE EXTENSION pax is sent to them once
 * the coordinator has run it.  Heap, every node's, is not looked up: the
 * catalog's page a session's first lookup reads would be counted to the
 * gather whose dispatch asked, in its EXPLAIN ANALYZE's Buffers.
 */
static bool
segments_have_am(const char *amname)
{
	HeapTuple	tuple;
	bool		result;

	if (strcmp(amname, "heap") == 0)
		return true;
	tuple = SearchSysCache1(AMNAME, CStringGetDatum(amname));
	if (!HeapTupleIsValid(tuple))
		return false;
	result = !TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data));
	ReleaseSysCache(tuple);
	return result;
}

/*
 * A setting's value, to be sent -- or NULL, for one not defined here or one
 * the session user may not set, which its segments take from the cluster's
 * configuration, as the coordinator took it.  So is a default table access
 * method the segments have not: pax, set for every database as Cloudberry's
 * CI sets it, in a database the extension is not created in, or not yet.  A
 * segment refuses a SET of it there, where the coordinator took it from its
 * configuration on faith, as PostgreSQL takes one outside a transaction; and
 * neither could make a table with it.  Once they have it, it is sent.  A
 * copy: GetConfigOption() writes a number in a buffer of its own, which the
 * next one overwrites.
 *
 * A setting only a superuser sets goes where the segments' sessions are a
 * superuser's: the coordinator's session user one, and the user the gang
 * connected as, which is the segments' session user where it may not take
 * the coordinator's.  A gang made while the session was a user who is none
 * -- SET SESSION AUTHORIZATION to one, a dispatch, and back -- is sent none
 * of them: a segment refused them, "permission denied to set parameter".
 */
static char *gang_username;
static bool gang_login_super;

static const char *
sync_value(int i)
{
	const char *value;

	for (int j = 0; j < lengthof(superuser_settings); j++)
		if (strcmp(synced_settings[i], superuser_settings[j]) == 0 &&
			(!superuser_arg(GetSessionUserId()) || !gang_login_super))
			return NULL;
	value = GetConfigOption(synced_settings[i], true, false);
	if (value != NULL && IsTransactionState() &&
		strcmp(synced_settings[i], "default_table_access_method") == 0 &&
		!segments_have_am(value))
		return NULL;

	/*
	 * The session user, where the gang's connection may take it: any, where
	 * a superuser logged in, and its own always.  A gang made as a user who
	 * may not -- SET SESSION AUTHORIZATION to another user, since -- keeps
	 * the one it was made as.  A process that has none of its own -- a
	 * background worker connected as the bootstrap superuser, diskquota's
	 * launcher, whose setting is empty -- sends none: a segment takes no
	 * empty name ("role \"\" does not exist").
	 */
	if (value != NULL && value[0] == '\0' &&
		strcmp(synced_settings[i], "session_authorization") == 0)
		return NULL;
	if (value != NULL && IsTransactionState() &&
		strcmp(synced_settings[i], "session_authorization") == 0 &&
		gang_username != NULL && strcmp(value, gang_username) != 0)
	{
		Oid			login = get_role_oid(gang_username, true);

		if (!OidIsValid(login) || !superuser_arg(login))
			return NULL;
	}
	return value != NULL ? pstrdup(value) : NULL;
}

/*
 * A setting only a superuser sets, about to be sent to a segment whose role
 * may be one the session set, which may be no superuser -- it was told one,
 * or what it was told was forgotten: the role reset first, and sent again
 * after it, "role" coming after every such setting in synced_settings[].
 * "sent" is what the segment was told; "reset", that it was reset already.
 */
static void
sync_reset_role(StringInfo sql, char **sent, int i, bool *any, bool *reset)
{
	static int	role = -1;

	if (role < 0)
		for (int j = 0; j < NUM_SYNCED_SETTINGS; j++)
			if (strcmp(synced_settings[j], "role") == 0)
				role = j;
	if (*reset || i > role ||
		(sent[role] != NULL && strcmp(sent[role], "none") == 0))
		return;
	for (int j = 0; j < lengthof(superuser_settings); j++)
		if (strcmp(synced_settings[i], superuser_settings[j]) == 0)
		{
			appendStringInfo(sql, "%spg_catalog.set_config('role', 'none', false)",
							 *any ? ", " : "");
			*any = true;
			*reset = true;
			if (sent[role] != NULL)
				pfree(sent[role]);
			sent[role] = NULL;
			return;
		}
}

/*
 * How many rows a segment sends at a time when a relation is read: one
 * first, ten times as many each batch after, up to a thousand.  A gather not
 * read to its end -- a LIMIT above it -- waits for the batches still on
 * their way before it closes its cursors, and a segment slow to make its
 * rows, a thousand of them in flight, would hold the statement until it had
 * made them all, where Cloudberry's senders stop at the next row: the first
 * rows come as soon as each segment has one, and what is in flight when the
 * statement stops is small.
 */
#define GATHER_FETCH_FIRST	1
#define GATHER_FETCH_ROWS	1000

/* One segment's connection. */
typedef struct GpSegmentConn
{
	int			content;
	const GpSegmentConfig *seg;
	PGconn	   *conn;
	bool		busy;			/* a statement was sent and has not finished */

	/*
	 * When what is in flight is a gather's next batch, whose it is.  Several
	 * gathers share the connections -- a join reads two tables -- and a batch
	 * one of them asked for ahead of need is set aside for it before anything
	 * else is sent; see conn_park().
	 */
	struct GpGatherSeg *fetching;

	/*
	 * What is in flight is a statement sent by conn_send_params(), the Close
	 * of its portal and a Sync, in libpq's pipeline mode; "pipe_step" counts
	 * the commands whose results have all been read.  See
	 * conn_pipeline_own().
	 */
	bool		pipelined;
	int			pipe_step;

	/*
	 * Where its backend receives a Motion's rows, over each transport, by
	 * its place in gp_ic.c's table; NULL until asked.
	 */
	char	   *icaddress[GP_IC_MAX_TRANSPORTS];
} GpSegmentConn;

/*
 * A reader: one more backend on a segment, for a slice of a statement whose
 * slices run at once.  Its transaction is its own, begun for each slice and
 * read as a part of the writer's (gp_share.c); the connection is the
 * session's, kept for the next statement.
 */
typedef struct GpReaderConn
{
	int			content;
	PGconn	   *conn;			/* NULL once broken */
	char	   *icaddress[GP_IC_MAX_TRANSPORTS];	/* as a writer's */
	bool		busy;			/* its slice is running */
	struct GpStream *stream;	/* the statement it is taken for */
	char	   *sent[NUM_SYNCED_SETTINGS];
} GpReaderConn;

typedef struct GpGang
{
	int			nconns;
	GpSegmentConn *conns;
	WaitEventSet *wes;			/* MyLatch plus every connection's socket */
	List	   *readers;		/* GpReaderConn, readers included in wes */

	/* What the segments have been told of the settings above; NULL unknown. */
	char	   *sent[NUM_SYNCED_SETTINGS];
} GpGang;

static GpGang *gang = NULL;
static bool exit_callback_registered = false;

/* A gang closed with the session's part: see session_reset_if_lost(). */
static bool session_lost = false;

/*
 * The database and the user the gang last connected as: who prepared a part,
 * and who may finish it, which is asked after the commit, where no catalog
 * can be read (dtx_finish_again()).
 */
static char *gang_dbname = NULL;
static char *gang_username = NULL;
static bool gang_login_super = false;	/* gang_username is a superuser */

/* The connection a COPY ... FROM STDIN is going through, if any. */
static GpSegmentConn *copying = NULL;

/*
 * What becomes of the context of an error the segment raises in a COPY's
 * data, while its end is being waited for: GpCopyInBegin()'s "context".
 */
static GpCopyInContext copy_context = NULL;
static void *copy_context_arg = NULL;
static bool copy_ending = false;

/*
 * The coordinator's transaction, as the segments know it.  "depth" counts the
 * transaction levels they have been given: 1 for the BEGIN, one more for each
 * savepoint, named after the level it stands for.
 */
static bool gang_in_xact = false;
static int	gang_xact_depth = 0;
static bool gang_xact_lost = false;	/* the gang closed with work in it */

/*
 * The distributed snapshot the segments were last sent in this transaction,
 * as the setting carries it; NULL before the first (gp_dtx.c).
 */
static char *gang_ds_sent = NULL;

/*
 * A transaction's parts prepared on the segments, between the two phases:
 * the gid, which connections were asked to prepare one, and whether every
 * one of them has.
 */
static char dtx_gid[GP_DTX_GIDLEN];
static bool *dtx_prepared = NULL;	/* by connection, in TopMemoryContext */
static int	dtx_prepared_size = 0;
static int	dtx_nprepared = 0;
static bool dtx_all_prepared = false;

/* Names the cursors of the gathers of one transaction apart. */
static uint32 gather_counter = 0;

/*
 * The fields of a segment's error that name what it was about, which the
 * error raised here carries as the segment raised them.
 */
static const char error_names[] = {
	PG_DIAG_SCHEMA_NAME, PG_DIAG_TABLE_NAME, PG_DIAG_COLUMN_NAME,
	PG_DIAG_DATATYPE_NAME, PG_DIAG_CONSTRAINT_NAME
};

/* What a segment answered when it failed. */
typedef struct GpSegmentError
{
	int			content;
	char	   *sqlstate;
	char	   *message;
	char	   *detail;
	char	   *hint;
	char	   *context;
	char	   *names[lengthof(error_names)];
	const char *file;			/* where the segment raised it, if it said */
	int			line;
	const char *func;
} GpSegmentError;

/* A statement whose slices run at once: the readers running them. */
struct GpStream
{
	List	   *readers;		/* GpReaderConn, as they were added */
	List	   *errors;			/* GpSegmentError, what they answered */
	bool		held;			/* a parallel retrieve cursor's; see below */
};

/* The ones running; in TopMemoryContext, as the readers point at them. */
static List *active_streams = NIL;

/* How many readers a segment may have for one session (gp_motion.h). */
#define MAX_READERS_PER_SEGMENT	GP_MAX_READERS_PER_SEGMENT

static void gang_close(void);
static void gang_cancel_and_drain(void);
static void session_reset_if_lost(void);
static void session_reset_temp_tables(int old_session);
static char *strip_trailing_space(char *s);
static void gang_drain_keeping(List **errors);
static void forget_kept_errors(void);
static void gang_build_wes(GpGang *g);
static void segment_notice_receiver(void *arg, const struct pg_result *res);
static void last_word_forget(const PGconn *conn);
static void conn_send_failed(GpSegmentConn *c);
static void collect_error(List **errors, int content, PGresult *res,
						  PGconn *conn, const char *why);
static void raise_segment_errors(List *errors);
static void readers_poll(void);
static void streams_raise_if_failed(void);
static void streams_release(void);
static void readers_cancel_and_drain(List **errors);

/*
 * What pg_stat_activity shows while this backend is waiting for a segment.
 *
 * Registered on first use rather than in _PG_init: PostgreSQL 19 keeps custom
 * wait events in shared memory, which is not there yet while libraries are
 * being preloaded.
 */
static uint32
dispatch_wait_event(void)
{
	static uint32 event = 0;

	if (event == 0)
		event = WaitEventExtensionNew("CloudberryDispatch");
	return event;
}

/* ------------------------------------------------------------------------- */
/* The gang                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The session's end.  A backend ended in the middle of a statement --
 * terminated, as pg_terminate_backend() ends one -- gets here before its
 * transaction's abort (ShutdownPostgres(), registered before this), which
 * would cancel what the segments run for it: cancelled here first, as
 * Cloudberry's abort cancels its dispatch before its gangs go, so that a
 * segment's process does not run the statement on, holding its locks, for a
 * coordinator that is gone.
 */
static void
gang_atexit(int code, Datum arg)
{
	/* as the abort does, nothing raised: the cancel can fail to allocate */
	PG_TRY();
	{
		gang_cancel_and_drain();
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();
	gang_close();
}

/*
 * Give up on the connections.  Called when one of them breaks, when the
 * session ends, and when an abort finds a segment that will not answer.
 *
 * A gang that closes with a transaction open on it takes the segments' part of
 * that transaction with it, and the coordinator's part must not commit alone:
 * gang_xact_lost makes the commit fail instead.
 */
static void
gang_close(void)
{
	if (gang == NULL)
		return;

	/* but for GpDispatchResetGang()'s, the session's part went with it */
	session_lost = true;
	GANG_LOG(GANG_LOG_TERSE, "gang of %d segments closed%s", gang->nconns,
			 gang_in_xact ? ", with a transaction open on it" : "");
	GpDtmDebugGangClosed();
	if (gang_in_xact)
		gang_xact_lost = true;
	gang_in_xact = false;
	gang_xact_depth = 0;
	copying = NULL;
	if (gang_ds_sent != NULL)
		pfree(gang_ds_sent);
	gang_ds_sent = NULL;

	for (int i = 0; i < gang->nconns; i++)
	{
		if (gang->conns[i].conn != NULL)
		{
			libpqsrv_disconnect(gang->conns[i].conn);
			gang->conns[i].conn = NULL;
		}
	}
	streams_release();
	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (r->conn != NULL)
			libpqsrv_disconnect(r->conn);
		r->conn = NULL;
	}
	last_word_forget(NULL);
	if (gang->wes != NULL)
		FreeWaitEventSet(gang->wes);
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
		if (gang->sent[i] != NULL)
			pfree(gang->sent[i]);

	pfree(gang->conns);
	pfree(gang);
	gang = NULL;
}

void
GpDispatchResetGang(void)
{
	bool		lost = session_lost;

	gang_close();
	session_lost = lost;
}

/*
 * A gang that closed but for GpDispatchResetGang(), which a caller asks for
 * -- a connection of it broke, FTS failed over from a primary of it, it was
 * let go of to retry a second phase -- took the session's part on the
 * segments with it: the session takes a new id, as Cloudberry's takes one
 * once its writer gang is lost (resetSessionForPrimaryGangLoss() and
 * GpDropTempTables(), cdbgang.c, before the next command is read), and says
 * so in its words.  What is left of the old one on the segments is then
 * none of the new one's -- a retrieve session bound to it, a backend still
 * ending.  Taken before the next gang is made, and as a statement begins
 * outside a transaction block; never with a gang, whose processes the old
 * id names, as Cloudberry's asserts.
 */
static void
session_reset_if_lost(void)
{
	int			old;

	if (!session_lost || gang != NULL)
		return;
	session_lost = false;
	old = GpClusterNewSessionId();
	if (old == GpClusterSessionId())
		return;
	GpGddNoteSession();
	ereport(LOG,
			(errmsg("The previous session was reset because its gang was disconnected (session id = %d). The new session id = %d",
					old, GpClusterSessionId())));
	session_reset_temp_tables(old);
}

int
GpDispatchGangSockets(int *contents, bool *writers, int *sockets, int max)
{
	int			n = 0;
	ListCell   *lc;

	if (gang == NULL)
		return 0;
	for (int i = 0; i < gang->nconns && n < max; i++)
	{
		if (gang->conns[i].conn == NULL)
			continue;
		contents[n] = gang->conns[i].content;
		writers[n] = true;
		sockets[n++] = PQsocket(gang->conns[i].conn);
	}
	foreach(lc, gang->readers)
	{
		GpReaderConn *r = (GpReaderConn *) lfirst(lc);

		if (n >= max)
			break;
		if (r->conn == NULL)
			continue;
		contents[n] = r->content;
		writers[n] = false;
		sockets[n++] = PQsocket(r->conn);
	}
	return n;
}

/*
 * The gang let go of to retry a part's second phase over a new connection,
 * as Cloudberry's coordinator releases its gangs to retry a broadcast
 * (ResetAllGangs()): the segments' backends end, and this session's
 * temporary tables there with them, which Cloudberry then drops on the
 * coordinator too, saying so (resetSessionForPrimaryGangLoss(), cdbgang.c)
 * -- here as the next statement begins (GpDispatchDropLostTempTables()).
 */
static bool temp_tables_lost = false;	/* to be dropped on the coordinator */
static bool temp_tables_dropped = false;	/* by this transaction */

static void
gang_release_for_retry(void)
{
	Oid			temp_namespace;
	Oid			temp_toast_namespace;

	gang_close();
	GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
	if (OidIsValid(temp_namespace))
	{
		ereport(WARNING,
				(errmsg("Any temporary tables for this session have been dropped because the gang was disconnected (session id = %d)",
						GpClusterSessionId())));
		temp_tables_lost = true;
	}
}

void
GpDispatchDropLostTempTables(void)
{
	if (!IsTransactionBlock())
		session_reset_if_lost();
	if (!temp_tables_lost || temp_tables_dropped || !IsTransactionState())
		return;

	/*
	 * Not under a statement that takes no snapshot -- BEGIN, SET, SHOW --
	 * whose catalog rows' deletion would have none, and which taking one
	 * would turn into one after "any query" (SET TRANSACTION ISOLATION
	 * LEVEL): the next statement that has one drops them.
	 */
	if (!ActiveSnapshotSet())
		return;
	/* as DISCARD TEMP drops them; again, should the transaction roll back */
	ResetTempTableNamespace();
	temp_tables_dropped = true;
}

/*
 * Are a lost gang's temporary tables still to be dropped on the
 * coordinator, the session reset first as GpDispatchDropLostTempTables()
 * resets it?  Asked before a statement is parsed (gp_dtm_debug.c).
 */
bool
GpDispatchLostTempTablesPending(void)
{
	if (!IsTransactionBlock())
		session_reset_if_lost();
	return temp_tables_lost && !temp_tables_dropped && IsTransactionState();
}

/*
 * A gang a broken connection took -- a segment's panic -- while this session
 * had temporary tables: their parts went with the segments' backends.  The
 * session is told so as its transaction aborts, after the error that ended
 * it, as Cloudberry's is told as its abort resets the gangs
 * (resetSessionForPrimaryGangLoss(), cdbgang.c), and the coordinator's are
 * dropped as the next statement begins, as after a retry's.
 */
static bool lost_with_temp = false;

static void
lost_gang_note(void)
{
	Oid			temp_namespace;
	Oid			temp_toast_namespace;

	GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
	if (OidIsValid(temp_namespace))
		lost_with_temp = true;
}

static void
lost_gang_warn(void)
{
	if (!lost_with_temp)
		return;
	lost_with_temp = false;

	/*
	 * Told already, where a retry, the cluster's refresh or a ROLLBACK TO
	 * found the gang gone and marked the tables to be dropped: once is
	 * Cloudberry's.
	 */
	if (temp_tables_lost)
		return;
	ereport(WARNING,
			(errmsg("Any temporary tables for this session have been dropped because the gang was disconnected (session id = %d)",
					GpClusterSessionId())));
	temp_tables_lost = true;
}

/*
 * A session reset because its gang closed with its part on the segments
 * (session_reset_if_lost()): the parts of its temporary tables there went
 * with the segments' backends, whatever closed the gang -- a segment's
 * panic too, which the abort after its error finds as a connection the
 * segment closed (gang_drain_quietly()), where nothing noted the tables --
 * so the coordinator's are dropped too, as Cloudberry's session drops them
 * as it is reset (resetSessionForPrimaryGangLoss() and GpDropTempTables(),
 * cdbgang.c), saying so once.
 */
static void
session_reset_temp_tables(int old_session)
{
	Oid			temp_namespace;
	Oid			temp_toast_namespace;

	lost_with_temp = false;
	if (temp_tables_lost)
		return;
	GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
	if (!OidIsValid(temp_namespace))
		return;
	ereport(WARNING,
			(errmsg("Any temporary tables for this session have been dropped because the gang was disconnected (session id = %d)",
					old_session)));
	temp_tables_lost = true;
}

/*
 * The identity the coordinator gives a segment process.
 *
 * It is what makes the backend on the other end a segment process rather than
 * somebody's psql: gp_cluster.c reads it, and every role macro follows.  The
 * content id is in it as well, so that a log line on a segment says which
 * connection of which coordinator session it belongs to.
 */
static char *
qe_identity_option(int content)
{
	/*
	 * libpq's "options" splits on whitespace, so the value may hold none; the
	 * numbers are joined with characters no shell or parser will take an
	 * interest in.  The last is the number of segments this session computes
	 * with, which the segment process computes with too, as Cloudberry's QE
	 * takes it from each statement: a segment that ran before gpexpand added
	 * one has the old number in its file (gp_cluster.c).  It is fixed for the
	 * gang's life -- the gang is let go of before the session takes another
	 * (gp_expand.c) -- so the connection is where it goes.
	 */
	char	   *option = psprintf("-c gp.qe_identity=seg%d/dbid%d/sess%d/nseg%d",
									content, GpClusterDbid(), GpClusterSessionId(),
									GpClusterSegmentCount());

	/* And the secret, which says it is this coordinator; see gp_cluster.c. */
	if (GpClusterHasSecret())
		option = psprintf("%s -c gp.qe_secret=%s", option, GpClusterSecret());
	return option;
}

/*
 * The keepalives of a connection to a segment, as Cloudberry's dispatcher
 * asks for them: libpq's keepalives_idle, keepalives_interval and
 * keepalives_count, each whose setting is not 0.  libpq sets them on a TCP
 * connection alone.  Up to DISPATCH_KEEPALIVE_OPTIONS entries at n, their
 * values in buf; returns the new n.
 */
#define DISPATCH_KEEPALIVE_OPTIONS	3
static int
dispatch_keepalive_options(const char **keywords, const char **values, int n,
						   char buf[DISPATCH_KEEPALIVE_OPTIONS][16])
{
	if (gp_dispatch_keepalives_idle > 0)
	{
		snprintf(buf[0], 16, "%d", gp_dispatch_keepalives_idle);
		keywords[n] = "keepalives_idle";
		values[n++] = buf[0];
	}
	if (gp_dispatch_keepalives_interval > 0)
	{
		snprintf(buf[1], 16, "%d", gp_dispatch_keepalives_interval);
		keywords[n] = "keepalives_interval";
		values[n++] = buf[1];
	}
	if (gp_dispatch_keepalives_count > 0)
	{
		snprintf(buf[2], 16, "%d", gp_dispatch_keepalives_count);
		keywords[n] = "keepalives_count";
		values[n++] = buf[2];
	}
	return n;
}

/*
 * A connection to a segment being made: a gang's writer, or a reader.  Its
 * options are its own -- the identity names the content -- as is the room
 * for its port's and keepalives' values, which libpq reads as it connects.
 */
#define CONN_ATTEMPT_OPTIONS \
	(7 + DISPATCH_KEEPALIVE_OPTIONS + GP_INTERNAL_CONN_OPTIONS)

typedef struct GpConnAttempt
{
	const GpSegmentConfig *seg;
	const char *keywords[CONN_ATTEMPT_OPTIONS];
	const char *values[CONN_ATTEMPT_OPTIONS];
	char		portbuf[16];
	char		keepalive_buf[DISPATCH_KEEPALIVE_OPTIONS][16];
	PGconn	   *conn;			/* NULL until started, and once given up */
	PostgresPollingStatusType polling;
	instr_time	start;
	double		ms;				/* how long it took, once made */
} GpConnAttempt;

/* The options of a connection to a segment, as the dispatcher makes one. */
static void
conn_attempt_init(GpConnAttempt *a, const GpSegmentConfig *seg,
				  const char *dbname, const char *username, bool reader)
{
	int			n = 0;

	memset(a, 0, sizeof(GpConnAttempt));
	a->seg = seg;
	snprintf(a->portbuf, sizeof(a->portbuf), "%d", seg->port);
	a->keywords[n] = "host";
	a->values[n++] = seg->hostname;
	a->keywords[n] = "port";
	a->values[n++] = a->portbuf;
	a->keywords[n] = "dbname";
	a->values[n++] = dbname;
	a->keywords[n] = "user";
	a->values[n++] = username;
	a->keywords[n] = "application_name";
	a->values[n++] = reader ? "cloudberry reader" : "cloudberry dispatcher";
	a->keywords[n] = "client_encoding";
	a->values[n++] = GetDatabaseEncodingName();
	a->keywords[n] = "options";

	/*
	 * A reader is a member of its writer's lock group, and a member cannot
	 * lead a group of its own: it starts no parallel workers.  Its planner
	 * holds to that whatever a function sets (share_planner()); the setting
	 * spares it the look at each query.
	 */
	a->values[n++] = reader ?
		psprintf("%s -c max_parallel_workers_per_gather=0",
				 qe_identity_option(seg->content)) :
		qe_identity_option(seg->content);
	n = dispatch_keepalive_options(a->keywords, a->values, n, a->keepalive_buf);
	(void) GpInternalConnOptions(a->keywords, a->values, n);
}

/* Cloudberry's name of a segment in its messages: "seg0 host:port". */
static char *
conn_attempt_whoami(const GpConnAttempt *a)
{
	return psprintf("seg%d %s:%d", a->seg->content, a->seg->hostname,
					a->seg->port);
}

/* Give a connection up. */
static void
conn_attempt_drop(GpConnAttempt *a)
{
	if (a->conn != NULL)
		libpqsrv_disconnect(a->conn);
	a->conn = NULL;
}

/*
 * Start each connection not made yet.  A PGconn that libpq could not make
 * holds no descriptor, the one reserved for it given back.
 */
static void
conn_attempts_start(GpConnAttempt *attempts, int n)
{
	for (int i = 0; i < n; i++)
	{
		GpConnAttempt *a = &attempts[i];

		if (a->conn != NULL)
			continue;
		INSTR_TIME_SET_CURRENT(a->start);
		a->conn = libpqsrv_connect_params_start(a->keywords, a->values, false);
		if (a->conn == NULL)
		{
			ReleaseExternalFD();
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		a->polling = PQstatus(a->conn) == CONNECTION_BAD ?
			PGRES_POLLING_FAILED : PGRES_POLLING_WRITING;
	}
}

/*
 * Poll the connections being made until none is, waiting for their sockets
 * all at once: false when the deadline, where there is one, passed first.
 * A new wait set each round: a connection's socket may change as libpq
 * tries the next address of a host.
 */
static bool
conn_attempts_poll(GpConnAttempt *attempts, int n, TimestampTz deadline)
{
	WaitEvent  *occurred = palloc_array(WaitEvent, n + 2);

	for (;;)
	{
		WaitEventSet *wes;
		int			npending = 0;
		long		timeout = -1;
		int			nready;

		for (int i = 0; i < n; i++)
			if (attempts[i].polling != PGRES_POLLING_OK &&
				attempts[i].polling != PGRES_POLLING_FAILED)
				npending++;
		if (npending == 0)
			break;

		/*
		 * Cloudberry's, before it waits for a gang's connections, writer's or
		 * reader's, that are still being made (createGang_async(),
		 * cdbgang_async.c): a test holds the backend here and terminates it,
		 * which ends it with no gang to free -- the gang is made once its
		 * connections all are -- and the connections with the process.
		 */
		GP_FAULT("create_gang_in_progress");

		if (deadline != 0)
		{
			timeout = TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
													  deadline);
			if (timeout <= 0)
			{
				pfree(occurred);
				return false;
			}
		}

		wes = CreateWaitEventSet(NULL, npending + 2);
		AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		if (IsUnderPostmaster)
			AddWaitEventToSet(wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
							  NULL, NULL);
		for (int i = 0; i < n; i++)
		{
			GpConnAttempt *a = &attempts[i];

			if (a->polling == PGRES_POLLING_OK ||
				a->polling == PGRES_POLLING_FAILED)
				continue;
			AddWaitEventToSet(wes, a->polling == PGRES_POLLING_READING ?
							  WL_SOCKET_READABLE : WL_SOCKET_WRITEABLE,
							  PQsocket(a->conn), NULL, a);
		}
		nready = WaitEventSetWait(wes, timeout, occurred, npending + 2,
								  dispatch_wait_event());
		FreeWaitEventSet(wes);

		for (int i = 0; i < nready; i++)
		{
			GpConnAttempt *a = (GpConnAttempt *) occurred[i].user_data;

			if (occurred[i].events & WL_LATCH_SET)
				ResetLatch(MyLatch);
			if (a == NULL ||
				!(occurred[i].events & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE)))
				continue;
			a->polling = PQconnectPoll(a->conn);
			if (a->polling == PGRES_POLLING_OK)
			{
				instr_time	now;

				INSTR_TIME_SET_CURRENT(now);
				INSTR_TIME_SUBTRACT(now, a->start);
				a->ms = INSTR_TIME_GET_MILLISEC(now);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(occurred);
	return true;
}

/*
 * What a segment said as it refused a connection, which says it is starting
 * up, resetting or in recovery -- one to try again (Cloudberry's
 * segment_failure_due_to_recovery(), cdbgang.c).  And a connection its
 * postmaster closed as it ended the backend it had started for it, which
 * PostgreSQL 19's does as it resets, before the backend says anything.
 */
static bool
conn_refused_in_recovery(const char *msg)
{
	return strstr(msg, "the database system is starting up") != NULL ||
		strstr(msg, "the database system is in recovery mode") != NULL ||
		strstr(msg, "the database system is not yet accepting connections") != NULL ||
		strstr(msg, "server closed the connection unexpectedly") != NULL;
}

/* A fault's FATAL, which Cloudberry's message tells apart. */
static bool
conn_refused_by_fault(const char *msg)
{
	const char *fatal = strstr(msg, "FATAL:");

	return fatal != NULL && strstr(fatal, "fault triggered") != NULL;
}

/* Why a gang's connection failed, as Cloudberry's messages tell them apart. */
typedef enum GpConnFailure
{
	CONN_MADE,					/* none: made, or to be tried again */
	CONN_TIMEOUT,				/* the deadline came first */
	CONN_NO_DETAILS,			/* its backend is no segment process */
	CONN_REFUSED_BY_FAULT,		/* a fault's FATAL */
	CONN_REFUSED,				/* anything else libpq says */
} GpConnFailure;

/*
 * A gang's connection failed: FTS is asked to probe and waited for, as
 * Cloudberry's dispatcher asks it as a gang fails (FtsNotifyProber(),
 * cdbgang_async.c) -- a segment it finds down is what is said, and the next
 * transaction connects to its mirror -- and what failed is said otherwise,
 * in Cloudberry's words.
 */
static void
connect_failed(GpConnAttempt *attempts, int n, GpConnAttempt *failed,
			   GpConnFailure failure, const char *msg)
{
	char	   *whoami = conn_attempt_whoami(failed);

	GpFtsNotifyProber();
	for (int i = 0; i < n; i++)
		if (!GpClusterIsPrimaryNow(attempts[i].seg->dbid))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("failed to acquire resources on one or more segments"),
					 errdetail("FTS detected one or more segments are down")));

	switch (failure)
	{
		case CONN_TIMEOUT:
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("failed to acquire resources on one or more segments"),
					 errdetail("timeout expired\n (%s)", whoami)));
			break;
		case CONN_NO_DETAILS:
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("failed to acquire resources on one or more segments"),
					 errdetail("Internal error: No motion listener port (%s)", whoami)));
			break;
		case CONN_REFUSED_BY_FAULT:
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("failed to acquire resources on one or more segments: fault injector"),
					 errdetail("%s\n (%s)", msg, whoami)));
			break;
		case CONN_REFUSED:
		case CONN_MADE:
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("failed to acquire resources on one or more segments"),
			 errdetail("%s\n (%s)", msg, whoami)));
}

/*
 * Make the connections, all at once, as Cloudberry's createGang_async()
 * makes a gang's (cdbgang_async.c): each started and polled until all are
 * made or have failed, under one deadline, gp.segment_connect_timeout.  A
 * segment in recovery is tried again, gp.gang_creation_retry_count times,
 * gp.gang_creation_retry_timer apart, the others kept: one restarting
 * refuses the connection, or closes it, and a mirror FTS promoted, a hot
 * standby until the promotion takes, takes it and says so (in_hot_standby),
 * and could not write.  And a connection made whose backend did not say it
 * is a segment process of this coordinator's (gp.qe_details) is not one.
 *
 * Every connection or none: what fails fails them all, in Cloudberry's
 * words, once FTS has been asked to probe and waited for, as Cloudberry's
 * dispatcher asks it (FtsNotifyProber()) -- a segment it finds down is what
 * is said, and the next transaction connects to its mirror.  Then the
 * cluster's fault: a gang made, gang_created, which fails it too.
 */
static void
connect_segments(GpConnAttempt *attempts, int n)
{
	int			retries = 0;

	PG_TRY();
	{
		for (;;)
		{
			TimestampTz deadline = gp_segment_connect_timeout <= 0 ? 0 :
				TimestampTzPlusSeconds(GetCurrentTimestamp(),
									   gp_segment_connect_timeout);
			GpConnAttempt *failed = NULL;
			GpConnFailure failure = CONN_MADE;
			char	   *msg = NULL;
			bool		in_recovery = false;

			conn_attempts_start(attempts, n);
			if (!conn_attempts_poll(attempts, n, deadline))
			{
				for (int i = 0; i < n && failed == NULL; i++)
					if (attempts[i].polling != PGRES_POLLING_OK &&
						attempts[i].polling != PGRES_POLLING_FAILED)
						failed = &attempts[i];
				failure = CONN_TIMEOUT;
			}

			for (int i = 0; i < n && failure == CONN_MADE; i++)
			{
				GpConnAttempt *a = &attempts[i];

				if (a->polling == PGRES_POLLING_OK)
				{
					const char *hot_standby = PQparameterStatus(a->conn, "in_hot_standby");
					const char *details = PQparameterStatus(a->conn, "gp.qe_details");

					if (hot_standby != NULL && strcmp(hot_standby, "on") == 0)
					{
						conn_attempt_drop(a);
						in_recovery = true;
					}
					else if (details == NULL || details[0] == '\0')
					{
						failed = a;
						failure = CONN_NO_DETAILS;
					}
					continue;
				}

				msg = strip_trailing_space(pstrdup(PQerrorMessage(a->conn)));
				if (conn_refused_in_recovery(msg))
				{
					conn_attempt_drop(a);
					in_recovery = true;
					continue;
				}
				failed = a;
				failure = conn_refused_by_fault(msg) ? CONN_REFUSED_BY_FAULT
					: CONN_REFUSED;
			}

			if (failure != CONN_MADE)
				connect_failed(attempts, n, failed, failure, msg);
			if (!in_recovery)
				break;
			if (retries++ >= gp_gang_creation_retry_count)
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("failed to acquire resources on one or more segments"),
						 errdetail("Segments are in reset/recovery mode.")));
			GANG_LOG(GANG_LOG_TERSE, "a segment is in reset/recovery mode: its connection tried again in %d ms",
					 gp_gang_creation_retry_timer);
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 gp_gang_creation_retry_timer, dispatch_wait_event());
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		GP_FAULT("gang_created");
	}
	PG_CATCH();
	{
		for (int i = 0; i < n; i++)
			conn_attempt_drop(&attempts[i]);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * gp.print_create_gang_time: Cloudberry's gp_print_create_gang_time, an INFO
 * of how long the gang's connections took, the shortest and the longest,
 * where a statement makes it, and that it is reused where a statement
 * dispatches through one made before (printCreateGangTime(), cdbgang.c) --
 * once a statement, which dispatches many times here; of the gang, not of
 * the readers a statement's slices take.
 */
static bool gp_print_create_gang_time = false;
static TimestampTz gang_time_reported = 0;	/* the statement it was said of */

static void
gang_made_times(const GpConnAttempt *attempts, int n)
{
	int			shortest = 0;
	int			longest = 0;

	if (!gp_print_create_gang_time)
		return;
	for (int i = 1; i < n; i++)
	{
		if (attempts[i].ms < attempts[shortest].ms)
			shortest = i;
		if (attempts[i].ms > attempts[longest].ms)
			longest = i;
	}
	ereport(INFO,
			(errmsg("The shortest establish conn time: %.2f ms, segindex: %d,\n"
					"       The longest  establish conn time: %.2f ms, segindex: %d",
					attempts[shortest].ms, attempts[shortest].seg->content,
					attempts[longest].ms, attempts[longest].seg->content)));
	gang_time_reported = GetCurrentStatementStartTimestamp();
}

static void
gang_reused_report(void)
{
	if (!gp_print_create_gang_time ||
		gang_time_reported == GetCurrentStatementStartTimestamp())
		return;
	ereport(INFO,
			(errmsg("(Gang) is reused")));
	gang_time_reported = GetCurrentStatementStartTimestamp();
}

/*
 * Open the session's connections, one per segment, all at once.
 *
 * Every segment or none: a gang that is missing a segment would answer a
 * query with part of the table, which is the one failure that must not be
 * quiet.
 */
static void
gang_connect(void)
{
	const GpSegmentConfig *segs;
	int			nsegs;
	MemoryContext oldcxt;
	const char *dbname;
	const char *username;
	GpConnAttempt *attempts;

	Assert(gang == NULL);

	/* the session a lost gang took with it, a new one before this is made */
	session_reset_if_lost();

	/* The primaries FTS last published: a gang is made to them. */
	(void) GpClusterRefresh();
	segs = GpClusterSegments(&nsegs);
	if (nsegs == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this server has no segments to dispatch to"),
				 errhint("\"gp.cluster_config\" names the file that lists this cluster's nodes.")));

	/*
	 * Every node reads the same file, so a segment knows where the other
	 * segments are and would dispatch to them -- and to itself -- if it were
	 * asked to.  Only the coordinator dispatches: that is what makes one
	 * statement one statement, and it is what Cloudberry's own role check
	 * means.  (Running it found a segment doing exactly that.)
	 */
	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("only the coordinator dispatches to the segments"),
				 errdetail("This node has content id %d.", GpClusterContentId())));

	dbname = get_database_name(MyDatabaseId);
	username = GetUserNameFromId(GetSessionUserId(), false);
	if (gang_dbname != NULL)
		pfree(gang_dbname);
	if (gang_username != NULL)
		pfree(gang_username);
	gang_dbname = MemoryContextStrdup(TopMemoryContext, dbname);
	gang_username = MemoryContextStrdup(TopMemoryContext, username);
	gang_login_super = superuser_arg(GetSessionUserId());

	if (!exit_callback_registered)
	{
		before_shmem_exit(gang_atexit, 0);
		exit_callback_registered = true;
	}

	attempts = palloc_array(GpConnAttempt, nsegs);
	for (int i = 0; i < nsegs; i++)
		conn_attempt_init(&attempts[i], &segs[i], dbname, username, false);
	connect_segments(attempts, nsegs);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	gang = (GpGang *) palloc0(sizeof(GpGang));
	gang->conns = (GpSegmentConn *) palloc0_array(GpSegmentConn, nsegs);
	gang->nconns = nsegs;
	MemoryContextSwitchTo(oldcxt);

	for (int i = 0; i < nsegs; i++)
	{
		PGconn	   *conn = attempts[i].conn;

		gang->conns[i].content = segs[i].content;
		gang->conns[i].seg = &segs[i];
		gang->conns[i].conn = conn;
		gang->conns[i].busy = false;
		last_word_forget(conn);
		PQsetNoticeReceiver(conn, segment_notice_receiver, conn);
		GANG_LOG(GANG_LOG_VERBOSE, "connected to segment %d (%s:%d), backend %d",
				 segs[i].content, segs[i].hostname, segs[i].port,
				 PQbackendPID(conn));
	}
	gang_made_times(attempts, nsegs);
	pfree(attempts);

	gang_build_wes(gang);
	GpDtmDebugGangMade();
	GANG_LOG(GANG_LOG_TERSE, "gang of %d segments made for database \"%s\", user \"%s\"",
			 nsegs, dbname, username);
}

/*
 * A connection of the gang broke: the gang goes, and FTS is asked to probe
 * and waited for, as Cloudberry's dispatcher asks it when a segment's
 * connection fails (FtsNotifyProber(), cdbdisp_async.c) -- so that a primary
 * that is down is failed over from before the session's next statement, which
 * then finds the cluster changed: a transaction on the gang that is gone
 * fails, and one after it goes to the new primary.
 */
static void
gang_close_broken(void)
{
	gang_close();
	GpFtsNotifyProber();
	lost_gang_note();
}

/*
 * One wait set for the gang, built when its connections change: the sockets
 * do not change while the connections live, and building an epoll set per
 * row would cost more than the rows.  It has no resource owner, because the
 * gang outlives the transaction that opened it.  The readers are in it too,
 * so that a slice that fails on one is heard of while the coordinator waits
 * for anything.
 */
static void
gang_build_wes(GpGang *g)
{
	if (g->wes != NULL)
		FreeWaitEventSet(g->wes);
	g->wes = CreateWaitEventSet(NULL, g->nconns + list_length(g->readers) + 2);
	AddWaitEventToSet(g->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
	/* A backend with no postmaster -- single-user mode -- has none to lose. */
	if (IsUnderPostmaster)
		AddWaitEventToSet(g->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
						  NULL, NULL);
	/* not a connection that broke, which has no socket, and goes with the gang */
	for (int i = 0; i < g->nconns; i++)
		if (PQsocket(g->conns[i].conn) != PGINVALID_SOCKET)
			AddWaitEventToSet(g->wes, WL_SOCKET_READABLE,
							  PQsocket(g->conns[i].conn), NULL, &g->conns[i]);
	foreach_ptr(GpReaderConn, r, g->readers)
	{
		if (r->conn != NULL && PQsocket(r->conn) != PGINVALID_SOCKET)
			AddWaitEventToSet(g->wes, WL_SOCKET_READABLE, PQsocket(r->conn),
							  NULL, NULL);
	}
}

/*
 * Is a node the gang is connected to no longer its content's primary?  FTS
 * failed over from it: whatever it answers is no part of the cluster now.
 */
static bool
gang_lost_primary(GpGang *g)
{
	if (!GpClusterStale())
		return false;
	for (int i = 0; i < g->nconns; i++)
		if (!GpClusterIsPrimaryNow(g->conns[i].seg->dbid))
			return true;
	return false;
}

/*
 * The session's gang, connected first if it is not.  A gang whose primaries
 * FTS has since moved is let go as a transaction first asks for it, before
 * the transaction has sent the segments anything, and the next one is made
 * to the new primaries: only then, so that a caller holding the gang it was
 * given earlier in the transaction does not find it gone.  A transaction
 * that has sent them something has its part on the old ones, and cannot go
 * on, even where an old primary still answers (Cloudberry's
 * cdbcomponent_updateCdbComponents() and its gang check, and the isolation2
 * test fts_session_reset).
 */
static GpGang *
gang_get(void)
{
	static LocalTransactionId checked = InvalidLocalTransactionId;

	/* the segments added or removed since, taken before the gang is used */
	GpClusterDecideSegments();

	if (gang != NULL && GpClusterStale())
	{
		if (gang_in_xact)
		{
			if (gang_lost_primary(gang))
			{
				gang_close();
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("gang was lost due to cluster reconfiguration")));
			}
		}
		else if (checked != MyProc->vxid.lxid)
		{
			Oid			temp_ns;
			Oid			temp_toast_ns;

			checked = MyProc->vxid.lxid;
			if (GpClusterRefresh())
			{
				gang_close();

				/*
				 * A session with temporary tables had their segments' parts
				 * in the gang's backends, which are gone with it: it cannot
				 * go on to the new primaries as if they were there, and is
				 * told so, as Cloudberry's session is -- which also forgets
				 * the tables here (resetSessionForPrimaryGangLoss(),
				 * cdbgang.c): the coordinator's are dropped as the next
				 * statement begins, as for a gang let go of to retry a
				 * second phase (GpDispatchDropLostTempTables()).
				 */
				GetTempNamespaceState(&temp_ns, &temp_toast_ns);
				if (OidIsValid(temp_ns))
				{
					ereport(WARNING,
							(errmsg("Any temporary tables for this session have been dropped because the gang was disconnected (session id = %d)",
									GpClusterSessionId())));
					temp_tables_lost = true;
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("gang was lost due to cluster reconfiguration")));
				}
			}
		}
	}
	if (gang == NULL)
		gang_connect();
	else
		gang_reused_report();
	return gang;
}

/* ------------------------------------------------------------------------- */
/* What the segments say besides their answers                               */
/* ------------------------------------------------------------------------- */

/*
 * A NOTICE, WARNING or INFO a segment sent -- a trigger's RAISE NOTICE on
 * the rows it inserted, say -- is the client's, as Cloudberry relays a QE's
 * (MPPnoticeReceiver, cdbconn.c).  libpq hands it to a receiver in the middle
 * of reading a result, where nothing may be raised, so it is queued there, in
 * malloc'd memory as Cloudberry's is, and raised here once the dispatcher is
 * back on its own ground: before a segment's error, since it came first, and
 * whenever the dispatcher waits.  Without Cloudberry's "(seg0 host:port
 * pid=...)" after the message, which its tests' init_file masks anyway.
 *
 * What the port's own statements to the segments make them say is not the
 * client's: a DDL statement's NOTICE is the coordinator's to give, once, and
 * it has; settings, labels and the transaction's BEGIN and COMMIT say nothing
 * the user asked about.  Those are sent quietly (notices_quiet).
 *
 * But for a statement that builds indexes over the coordinator's copy of a
 * table, which for a table whose rows are the segments' is empty: what the
 * build says of the rows it read -- pgvector's IVFFlat, "created with little
 * data" of a copy it had none of -- is no NOTICE of the table's, and the
 * coordinator holds its NOTICEs back as it builds (gp_ddl.c).  The segments,
 * which built from the rows, say it instead: their NOTICEs of the statement
 * are the client's, a NOTICE several give alike given once (notices_relayed,
 * the quiet level whose NOTICEs are relayed).
 */
typedef struct SegmentNotice
{
	struct SegmentNotice *next;
	int			elevel;
	int			sqlerrcode;
	char	   *message;
	char	   *detail;
	char	   *hint;
	char		buf[FLEXIBLE_ARRAY_MEMBER];
} SegmentNotice;

static SegmentNotice *notices_head = NULL;
static SegmentNotice **notices_tail = &notices_head;
static int	notices_quiet = 0;
static int	notices_relayed = 0;
static SegmentNotice *notices_relayed_given = NULL;	/* raised, while relayed */

/* Is a notice like this one among these: the same level, code and words? */
static bool
notice_among(const SegmentNotice *n, const SegmentNotice *list)
{
	for (const SegmentNotice *o = list; o != NULL; o = o->next)
		if (o->elevel == n->elevel && o->sqlerrcode == n->sqlerrcode &&
			strcmp(o->message, n->message) == 0 &&
			(o->detail == NULL ? n->detail == NULL :
			 n->detail != NULL && strcmp(o->detail, n->detail) == 0) &&
			(o->hint == NULL ? n->hint == NULL :
			 n->hint != NULL && strcmp(o->hint, n->hint) == 0))
			return true;
	return false;
}

/* The end of an index build's statement: its notices relayed, forgotten. */
static void
forget_relayed_notices(void)
{
	while (notices_relayed_given != NULL)
	{
		SegmentNotice *n = notices_relayed_given;

		notices_relayed_given = n->next;
		free(n);
	}
	notices_relayed = 0;
}

/*
 * A segment's last word: an error its backend sent while its connection was
 * idle -- the FATAL of a backend terminated between two batches of a gather,
 * as it exits.  libpq hands an ErrorResponse that answers no statement to the
 * notice receiver ("Unexpected message in IDLE state", pqParseInput3()), and
 * finds the connection closed only at its next statement, saying "server
 * closed the connection unexpectedly": the error raised then is the one the
 * segment gave, as Cloudberry's is, whose rows come by the interconnect while
 * its statement is still running there.  One for each connection, in
 * malloc'd memory, as a notice is kept; forgotten when it is raised, when a
 * connection is made at the address, and when the gang closes.
 */
typedef struct LastWord
{
	struct LastWord *next;
	const PGconn *conn;
	char		sqlstate[6];
	char	   *message;
	char	   *detail;
	char	   *hint;
	char		buf[FLEXIBLE_ARRAY_MEMBER];
} LastWord;

static LastWord *last_words = NULL;

/*
 * A module's filters of what the segments say: a NOTICE it raises there to
 * tell the coordinator something -- gp_exttable's count of the rows a scan
 * rejected -- or an INFO, which no client_min_messages holds back --
 * EXPLAIN ANALYZE's statistics (gp_explain.c) -- which the filter takes,
 * and the client never sees.  Called in libpq's notice callback: a filter
 * raises nothing, and pallocs nothing.
 */
#define MAX_NOTICE_FILTERS	8
static GpNoticeFilter notice_filters[MAX_NOTICE_FILTERS];
static int	n_notice_filters = 0;

void
GpDispatchAddNoticeFilter(GpNoticeFilter filter)
{
	for (int i = 0; i < n_notice_filters; i++)
		if (notice_filters[i] == filter)
			return;
	if (n_notice_filters >= MAX_NOTICE_FILTERS)
		elog(ERROR, "too many notice filters");
	notice_filters[n_notice_filters++] = filter;
}

/*
 * The modules' calls before the settings are synced (GpDispatchAddSyncCallback()):
 * each may set a setting of its own again, as a backend's own statement does.
 */
#define MAX_SYNC_CALLBACKS	4
static GpDispatchSyncCallback sync_callbacks[MAX_SYNC_CALLBACKS];
static int	n_sync_callbacks = 0;

void
GpDispatchAddSyncCallback(GpDispatchSyncCallback callback)
{
	for (int i = 0; i < n_sync_callbacks; i++)
		if (sync_callbacks[i] == callback)
			return;
	if (n_sync_callbacks >= MAX_SYNC_CALLBACKS)
		elog(ERROR, "too many dispatch sync callbacks");
	sync_callbacks[n_sync_callbacks++] = callback;
}

static void
run_sync_callbacks(void)
{
	for (int i = 0; i < n_sync_callbacks; i++)
		sync_callbacks[i] ();
}

/* s less its trailing whitespace, in place; s. */
static char *
strip_trailing_space(char *s)
{
	size_t		len = strlen(s);

	while (len > 0 && isspace((unsigned char) s[len - 1]))
		s[--len] = '\0';
	return s;
}

/* Forget a connection's last word, or every connection's (NULL). */
static void
last_word_forget(const PGconn *conn)
{
	LastWord  **prev = &last_words;

	while (*prev != NULL)
	{
		LastWord   *w = *prev;

		if (conn == NULL || w->conn == conn)
		{
			*prev = w->next;
			free(w);
		}
		else
			prev = &w->next;
	}
}

/* Whether a connection has a last word. */
static bool
last_word_said(const PGconn *conn)
{
	for (LastWord *w = last_words; w != NULL; w = w->next)
		if (w->conn == conn)
			return true;
	return false;
}

/* A connection's last word, now the caller's to free; NULL if none. */
static LastWord *
last_word_take(const PGconn *conn)
{
	for (LastWord **prev = &last_words; *prev != NULL; prev = &(*prev)->next)
	{
		LastWord   *w = *prev;

		if (w->conn == conn)
		{
			*prev = w->next;
			return w;
		}
	}
	return NULL;
}

/*
 * libpq calls it with its own PGresult, not the wrapper that libpq-be-fe.h's
 * macros put in the name's place, so they are set aside around it, as
 * PostgreSQL's own libpqsrv_notice_receiver sets them aside.
 */
#undef PGresult
#undef PQresultErrorField

/* Keep an error a segment's connection received idle, as its last word. */
static void
last_word_keep(const PGconn *conn, const struct pg_result *res)
{
	const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	const char *fields[3];
	size_t		size = offsetof(LastWord, buf);
	LastWord   *w;
	char	   *p;

	fields[0] = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	fields[1] = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	fields[2] = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	if (fields[0] == NULL)
		return;
	for (int i = 0; i < 3; i++)
		if (fields[i] != NULL)
			size += strlen(fields[i]) + 1;

	/* nothing can be raised here: a last word there is no memory for is lost */
	w = malloc(size);
	if (w == NULL)
		return;
	last_word_forget(conn);
	w->conn = conn;
	strlcpy(w->sqlstate, sqlstate != NULL && strlen(sqlstate) == 5 ? sqlstate : "",
			sizeof(w->sqlstate));
	p = w->buf;
	w->message = w->detail = w->hint = NULL;
	for (int i = 0; i < 3; i++)
	{
		char	  **dest = (i == 0) ? &w->message : (i == 1) ? &w->detail : &w->hint;

		if (fields[i] == NULL)
			continue;
		strcpy(p, fields[i]);
		*dest = p;
		p += strlen(fields[i]) + 1;
		strip_trailing_space(*dest);
	}
	w->next = last_words;
	last_words = w;
}

static void
segment_notice_receiver(void *arg, const struct pg_result *res)
{
	const char *severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
	const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	const char *fields[3];
	size_t		size = offsetof(SegmentNotice, buf);
	SegmentNotice *n;
	char	   *p;
	int			elevel;

	/* an error no statement was waiting for: the connection's last word */
	if (arg != NULL && severity != NULL &&
		(strcmp(severity, "ERROR") == 0 || strcmp(severity, "FATAL") == 0 ||
		 strcmp(severity, "PANIC") == 0))
	{
		last_word_keep((const PGconn *) arg, res);
		return;
	}

	if (severity == NULL)
		return;
	/* quiet, but for the NOTICEs of an index build's statement */
	if (notices_quiet > 0 &&
		!(notices_relayed > 0 && notices_quiet == notices_relayed &&
		  strcmp(severity, "NOTICE") == 0))
		return;
	if (strcmp(severity, "NOTICE") == 0)
		elevel = NOTICE;
	else if (strcmp(severity, "WARNING") == 0)
		elevel = WARNING;
	else if (strcmp(severity, "INFO") == 0)
		elevel = INFO;
	else if (strcmp(severity, "LOG") == 0)
	{
		/*
		 * A LOG the segment sent its client, as it does where the client's
		 * client_min_messages, which it is sent, asks for LOG: the client's,
		 * as Cloudberry's coordinator passes it on -- a table access method's
		 * account of its scan, as PAX's.  The coordinator's own log keeps a
		 * copy, as it keeps the coordinator's own LOG messages.
		 */
		elevel = LOG;
	}
	else
		return;					/* DEBUG is the segment's own log's */

	fields[0] = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	fields[1] = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	fields[2] = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	if (fields[0] == NULL)
		return;

	/* a module's own message to the coordinator, which it takes */
	for (int i = 0; i < n_notice_filters; i++)
		if (notice_filters[i] (sqlstate, fields[0]))
			return;
	for (int i = 0; i < 3; i++)
		if (fields[i] != NULL)
			size += strlen(fields[i]) + 1;

	/* nothing can be raised here: a notice there is no memory for is lost */
	n = malloc(size);
	if (n == NULL)
		return;
	n->next = NULL;
	n->elevel = elevel;
	n->sqlerrcode = (sqlstate != NULL && strlen(sqlstate) == 5)
		? MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2], sqlstate[3],
						sqlstate[4])
		: ERRCODE_SUCCESSFUL_COMPLETION;
	p = n->buf;
	n->message = n->detail = n->hint = NULL;
	for (int i = 0; i < 3; i++)
	{
		char	  **dest = (i == 0) ? &n->message : (i == 1) ? &n->detail : &n->hint;
		size_t		len;

		if (fields[i] == NULL)
			continue;
		strcpy(p, fields[i]);
		*dest = p;
		len = strlen(fields[i]);
		p += len + 1;

		/*
		 * Less its trailing whitespace, as Cloudberry's segment sends a message
		 * (cdb_tidy_message()): a message of several lines, which ends in a
		 * newline, ends at its last.
		 */
		strip_trailing_space(*dest);
	}

	/*
	 * an index build's NOTICE that another segment gave alike, waiting to be
	 * raised or raised already: given once
	 */
	if (notices_relayed > 0 &&
		(notice_among(n, notices_head) || notice_among(n, notices_relayed_given)))
	{
		free(n);
		return;
	}

	*notices_tail = n;
	notices_tail = &n->next;
}
#define PGresult libpqsrv_PGresult
#define PQresultErrorField libpqsrv_PQresultErrorField

/* Raise what the segments said, in the order they said it. */
static void
flush_segment_notices(void)
{
	while (notices_head != NULL)
	{
		SegmentNotice *n = notices_head;
		SegmentNotice copy = *n;
		char	   *message = pstrdup(n->message);
		char	   *detail = n->detail ? pstrdup(n->detail) : NULL;
		char	   *hint = n->hint ? pstrdup(n->hint) : NULL;

		notices_head = n->next;
		if (notices_head == NULL)
			notices_tail = &notices_head;
		/* an index build's, kept while its statement runs: given once */
		if (notices_relayed > 0)
		{
			n->next = notices_relayed_given;
			notices_relayed_given = n;
		}
		else
			free(n);

		ereport(copy.elevel,
				(errcode(copy.sqlerrcode),
				 errmsg_internal("%s", message),
				 detail ? errdetail_internal("%s", detail) : 0,
				 hint ? errhint("%s", hint) : 0));
	}
}

void
GpDispatchRelayNotices(PGconn *conn)
{
	PQsetNoticeReceiver(conn, segment_notice_receiver, NULL);
}

void
GpDispatchFlushNotices(void)
{
	flush_segment_notices();
}

/* Forget them: the statement failed, and what it said went with it. */
static void
drop_segment_notices(void)
{
	while (notices_head != NULL)
	{
		SegmentNotice *n = notices_head;

		notices_head = n->next;
		free(n);
	}
	notices_tail = &notices_head;
	notices_quiet = 0;
	forget_relayed_notices();
}

/* ------------------------------------------------------------------------- */
/* Sending, and waiting                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A segment waited for whose primary FTS has failed over from since: it will
 * not answer, or answers as a node that is no part of the cluster now, and
 * the gang gives up on it, as Cloudberry's dispatcher does
 * (checkSegmentAlive(), cdbdisp_async.c), in its words.
 */
static void
gang_check_moved(GpGang *g)
{
	if (!GpClusterStale())
		return;
	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];
		char	   *who;

		if (!c->busy || GpClusterIsPrimaryNow(c->seg->dbid))
			continue;
		who = psprintf("seg%d %s:%d pid=%d", c->content, c->seg->hostname,
					   c->seg->port, PQbackendPID(c->conn));
		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("FTS detected connection lost during dispatch to %s:", who)));
	}
}

/*
 * Wait until at least one connection has something to say, or an interrupt
 * arrives, or a second passes, after which FTS is asked whether a segment
 * waited for has been failed over from.  The caller loops over the
 * connections afterwards; this only stops the backend from spinning.
 */
static bool gather_poll(struct GpGatherSeg *s);

static void
gang_wait(GpGang *g)
{
	WaitEvent	occurred[1];

	flush_segment_notices();
	CHECK_FOR_INTERRUPTS();
	gang_check_moved(g);

	if (WaitEventSetWait(g->wes, 1000, occurred, 1, dispatch_wait_event()) > 0)
	{
		if (occurred[0].events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}

	/*
	 * Take in whatever has arrived for a gather, whichever gather is waiting:
	 * the sockets are waited on level-triggered, so a batch nobody reads -- a
	 * join's other side prefetching, a merge waiting on one segment while the
	 * others answer -- would make every wait after this one return at once.
	 */
	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (c->busy && c->fetching != NULL)
			(void) gather_poll(c->fetching);
	}

	/* And whatever the readers have said: a slice that failed fails this. */
	readers_poll();
	streams_raise_if_failed();
}

static void conn_park(GpSegmentConn *c);
static void gang_wait_all_counting(GpGang *g, uint64 *counts, int content,
								   int nsegments);

/*
 * Is this connection's segment one a statement is for: the one content
 * named, or else the first nsegments -- a partial table's -- or every one
 * where that is 0.
 */
static inline bool
conn_asked(const GpSegmentConn *c, int content, int nsegments)
{
	if (content >= 0)
		return c->content == content;
	return nsegments <= 0 || c->content < nsegments;
}

/* Is this connection's segment one of the ncontents "contents" lists? */
static bool
conn_listed(const GpSegmentConn *c, const int *contents, int ncontents)
{
	for (int i = 0; i < ncontents; i++)
		if (contents[i] == c->content)
			return true;
	return false;
}

static void dispatch_result_fault(void);

/* Send a statement to one segment, as the simple protocol sends it. */
static void
conn_send(GpSegmentConn *c, const char *sql)
{
	dispatch_result_fault();

	/* A gather's batch asked for ahead of need is set aside for it first. */
	if (c->busy && c->fetching != NULL)
		conn_park(c);
	if (c->busy)
		elog(ERROR, "segment %d is still busy with an earlier statement",
			 c->content);

	GANG_LOG(GANG_LOG_DEBUG, "to segment %d: %s", c->content, sql);
	if (!PQsendQuery(c->conn, sql))
		conn_send_failed(c);
	c->busy = true;
}

/*
 * A statement that could not be sent: the connection is closed.  What the
 * segment said as it closed it, if it said anything, is the error.
 */
static void
conn_send_failed(GpSegmentConn *c)
{
	char	   *msg = pstrdup(PQerrorMessage(c->conn));
	int			content = c->content;

	if (last_word_said(c->conn))
	{
		List	   *errors = NIL;

		collect_error(&errors, content, NULL, c->conn, NULL);
		gang_close();
		raise_segment_errors(errors);
	}
	gang_close();
	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("could not send a statement to segment %d", content),
			 errdetail_internal("%s", msg)));
}

static void
gang_send_all(GpGang *g, const char *sql)
{
	for (int i = 0; i < g->nconns; i++)
		conn_send(&g->conns[i], sql);
}

/*
 * The connection a fail_end_command of the command just sent names, and the
 * error it is then to raise, once the command is answered (gang_dtm_end()).
 */
static int	dtm_end_conn = -1;
static char *dtm_end_raise = NULL;

static void gang_wait_all(GpGang *g, PGresult **keep, bool commit);

/*
 * Send a command to every segment, as gang_send_all() does -- but to one
 * Cloudberry's debug_dtm_action names at it (gp_dtm_debug.c), the error
 * Cloudberry's segment raises there: instead of the command, where it fails
 * as it begins it, or after it, where it fails as it ends it.  A protocol
 * command, or with GP_DTX_NONE a SQL command of the tag; "level" is the
 * nesting level Cloudberry sends a subtransaction's command with.
 */
static void
gang_send_all_dtm(GpGang *g, const char *sql, GpDtxCommand command,
				  const char *tag, int level)
{
	dtm_end_conn = -1;
	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];
		char	   *msg;
		int			action = command != GP_DTX_NONE ?
			GpDtmDebugProtocol(command, c->content, level, &msg) :
			GpDtmDebugSql(tag, c->content, &msg);

		if (action == GP_DTM_ACTION_FAIL_BEGIN ||
			action == GP_DTM_ACTION_PANIC_BEGIN)
			conn_send(c, GpDtmDebugRaiseStatement(action, msg));
		else
		{
			conn_send(c, sql);
			if (action == GP_DTM_ACTION_FAIL_END)
			{
				dtm_end_conn = i;
				dtm_end_raise = GpDtmDebugRaiseStatement(action, msg);
			}
		}
	}
}

/* A fail_end_command's error, once the command it follows is answered. */
static void
gang_dtm_end(GpGang *g)
{
	int			i = dtm_end_conn;

	dtm_end_conn = -1;
	if (i < 0 || g != gang)
		return;
	conn_send(&g->conns[i], dtm_end_raise);
	gang_wait_all(g, NULL, false);
}

/*
 * Send a statement with parameters to one segment, as the extended protocol
 * sends it, and close the portal it runs in within the same round trip: the
 * statement, a Close of the unnamed portal and a Sync, in libpq's pipeline
 * mode.  Left open, a SELECT's portal ends its executor only when it is
 * dropped -- at the segment's next statement, or its commit -- and what a
 * table access method writes as a query finishes (ExecutorFinish: an
 * append-optimized or PAX table's last rows, and the transaction ID they
 * take) would come after the Sync's answer has told the coordinator what
 * the segment wrote (gp.dtx_xid, gp_dtx.c).  Closed here, the executor has
 * ended before that answer is sent.  The results are read as every
 * connection's are, conn_pipeline_own() taking the pipeline's own.
 */
static void
conn_send_params(GpSegmentConn *c, const char *sql, int nparams,
				 const Oid *types, const char *const *values,
				 const int *lengths, const int *formats)
{
	dispatch_result_fault();

	/* A gather's batch asked for ahead of need is set aside for it first. */
	if (c->busy && c->fetching != NULL)
		conn_park(c);
	if (c->busy)
		elog(ERROR, "segment %d is still busy with an earlier statement",
			 c->content);

	GANG_LOG(GANG_LOG_DEBUG, "to segment %d: %s", c->content, sql);
	if (!PQenterPipelineMode(c->conn) ||
		!PQsendQueryParams(c->conn, sql, nparams, types, values, lengths,
						   formats, 0) ||
		!PQsendClosePortal(c->conn, "") ||
		!PQpipelineSync(c->conn))
		conn_send_failed(c);
	c->busy = true;
	c->pipelined = true;
	c->pipe_step = 0;
}

/*
 * Whether a result read from a connection is the pipeline's own, which
 * conn_send_params() sent after its statement, rather than the caller's --
 * and if so, it is dealt with here: the end of the statement's results or of
 * the Close's, the Close's answer, the Close skipped because the statement
 * failed, and the Sync, which ends the pipeline and takes the connection out
 * of pipeline mode, after which PQgetResult() gives the NULL that ends what
 * was sent.  A Close that failed is the caller's: the executor ended there,
 * and its error is the statement's.
 */
static bool
conn_pipeline_own(GpSegmentConn *c, PGresult *res)
{
	if (!c->pipelined)
		return false;
	if (res == NULL)
	{
		c->pipe_step++;
		return true;
	}
	switch (PQresultStatus(res))
	{
		case PGRES_PIPELINE_SYNC:
			PQclear(res);
			/* with every result read, it cannot fail */
			(void) PQexitPipelineMode(c->conn);
			c->pipelined = false;
			return true;
		case PGRES_PIPELINE_ABORTED:
			PQclear(res);
			return true;
		case PGRES_COMMAND_OK:
			if (c->pipe_step == 0)
				return false;
			PQclear(res);
			return true;
		default:
			return false;
	}
}

/*
 * A source file's or function's name a segment's error gave, kept for the
 * backend's life: the error raised here points at it, and so does a copy of
 * that error (CopyErrorData() copies neither), which a PL/pgSQL handler
 * keeps past the subtransaction whose memory it was raised in.  The names
 * are the segment's code's, a few hundred at most.
 */
static const char *
location_name(const char *name)
{
	static List *names = NIL;
	MemoryContext oldcxt;
	char	   *kept;

	foreach_ptr(char, n, names)
	{
		if (strcmp(n, name) == 0)
			return n;
	}
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	kept = pstrdup(name);
	names = lappend(names, kept);
	MemoryContextSwitchTo(oldcxt);
	return kept;
}

/* Remember why a segment failed, in the caller's context. */
static void
collect_error(List **errors, int content, PGresult *res, PGconn *conn,
			  const char *why)
{
	GpSegmentError *err = (GpSegmentError *) palloc0(sizeof(GpSegmentError));
	const char *field;
	LastWord   *said = NULL;

	err->content = content;

	/*
	 * A connection that failed with no word of the segment's, which it said
	 * before, while nothing was asked of it: that is the error.
	 */
	if (why == NULL && conn != NULL &&
		(res == NULL || PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) == NULL))
		said = last_word_take(conn);
	if (said != NULL)
	{
		err->sqlstate = said->sqlstate[0] ? pstrdup(said->sqlstate) : NULL;
		err->message = pstrdup(said->message);
		err->detail = said->detail ? pstrdup(said->detail) : NULL;
		err->hint = said->hint ? pstrdup(said->hint) : NULL;
		free(said);
		*errors = lappend(*errors, err);
		return;
	}

	field = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	err->sqlstate = field ? pstrdup(field) : NULL;

	if (why != NULL)
		field = why;
	else
	{
		field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
		if (field == NULL)
			field = PQerrorMessage(conn);
	}
	/*
	 * Less its trailing whitespace, as Cloudberry's segment sends an error
	 * (cdb_tidy_message()), and as libpq's connection-level message ends in
	 * a newline, which a message does not.
	 */
	err->message = strip_trailing_space(pstrdup(field ? field : "unknown error"));

	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL) : NULL;
	err->detail = field ? strip_trailing_space(pstrdup(field)) : NULL;
	field = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_HINT) : NULL;
	err->hint = field ? strip_trailing_space(pstrdup(field)) : NULL;
	field = res ? PQresultErrorField(res, PG_DIAG_CONTEXT) : NULL;
	err->context = field ? pstrdup(field) : NULL;

	for (int i = 0; i < lengthof(error_names); i++)
	{
		field = res ? PQresultErrorField(res, error_names[i]) : NULL;
		err->names[i] = field ? pstrdup(field) : NULL;
	}
	field = res ? PQresultErrorField(res, PG_DIAG_SOURCE_FILE) : NULL;
	if (field != NULL)
	{
		const char *line = PQresultErrorField(res, PG_DIAG_SOURCE_LINE);
		const char *func = PQresultErrorField(res, PG_DIAG_SOURCE_FUNCTION);

		err->file = location_name(field);
		err->line = line ? atoi(line) : 0;
		err->func = func ? location_name(func) : NULL;
	}

	*errors = lappend(*errors, err);
}

/* An interconnect failure, or a cancel: what another's failure causes. */
static bool
error_is_consequence(GpSegmentError *err)
{
	return err->sqlstate != NULL &&
		(strcmp(err->sqlstate, "58M01") == 0 ||
		 strcmp(err->sqlstate, "57014") == 0);
}

/*
 * The first error that is neither a receiver's whose sender stopped nor a
 * cancel, first, and the others that are either left out.
 */
static List *
errors_cause_first(List *errors)
{
	ListCell   *lc;

	foreach_ptr(GpSegmentError, err, errors)
	{
		if (!error_is_consequence(err))
		{
			errors = list_delete_ptr(errors, err);
			errors = lcons(err, errors);
			break;
		}
	}
	for_each_from(lc, errors, 1)
	{
		if (error_is_consequence((GpSegmentError *) lfirst(lc)))
			errors = foreach_delete_current(errors, lc);
	}
	return errors;
}

/*
 * Raise what the segments said.
 *
 * The first segment's message is the message, with its own SQLSTATE, so that a
 * unique violation on a segment is a unique violation here; the segment it came
 * from is in the detail, as Cloudberry puts "(seg0 host:port)" in its own.  The
 * rest are counted, because a statement that fails on one segment usually fails
 * on all of them and repeating it three times helps nobody.  Where it failed
 * there -- an external table's line, a function's -- comes first in the
 * context, before where the statement was here, as Cloudberry's does.  And it
 * is raised with the objects the segment's error named -- a unique
 * violation's schema, table and constraint -- and at the segment's location
 * in its code, as Cloudberry raises it (cdbdisp_get_PQerror()): what a
 * client reads of the error is the segment's.  A connection that failed,
 * which says nowhere, fails here.
 */
static void
raise_segment_errors(List *errors)
{
	GpSegmentError *first;
	const GpSegmentConfig *seg;
	StringInfoData detail;

	flush_segment_notices();

	/*
	 * With slices running at once, what fails first is often only where the
	 * failure arrived: a receiver whose sender stopped, a slice cancelled
	 * because another failed.  Every reader is stopped and heard, and the
	 * message is the first that is neither.  The others that are either
	 * failed of no fault of their own, and are not counted: they would say
	 * the statement failed on more segments than the cluster has.
	 */
	if (active_streams != NIL)
	{
		readers_cancel_and_drain(&errors);
		errors = errors_cause_first(errors);
	}

	first = (GpSegmentError *) linitial(errors);
	seg = GpClusterSegmentByContent(first->content);

	/* where a COPY's data failed there, as its caller has it said here */
	if (copy_ending && copy_context != NULL && first->context != NULL)
		first->context = copy_context(first->content, first->context,
									  copy_context_arg);

	initStringInfo(&detail);
	appendStringInfo(&detail, "segment %d (%s:%d)", first->content,
					 seg ? seg->hostname : "?", seg ? seg->port : 0);
	if (first->detail != NULL)
		appendStringInfo(&detail, ": %s", first->detail);
	if (list_length(errors) > 1)
		appendStringInfo(&detail, "; %d other segments failed too",
						 list_length(errors) - 1);

	if (!errstart(ERROR, TEXTDOMAIN))
		pg_unreachable();
	errcode(first->sqlstate ? MAKE_SQLSTATE(first->sqlstate[0],
											first->sqlstate[1],
											first->sqlstate[2],
											first->sqlstate[3],
											first->sqlstate[4])
			: ERRCODE_INTERNAL_ERROR);
	errmsg("%s", first->message);
	errdetail_internal("%s", detail.data);
	if (first->hint)
		errhint("%s", first->hint);
	if (first->context)
		errcontext("%s", first->context);
	for (int i = 0; i < lengthof(error_names); i++)
	{
		if (first->names[i] != NULL)
			err_generic_string(error_names[i], first->names[i]);
	}
	if (first->file != NULL)
		errfinish(first->file, first->line, first->func);
	else
		errfinish(__FILE__, __LINE__, __func__);
	pg_unreachable();
}

/*
 * Read every busy connection to the end of its results.
 *
 * Every segment is waited for even after one has failed, so that the
 * connections are left idle and usable; a connection that broke is not, and
 * takes the gang with it.  Where the statement's slices run at once, the
 * rest are stopped first.  "keep", when given, receives each segment's first
 * result with rows.  "commit" says the statement was a COMMIT, whose answer
 * is ROLLBACK -- and no error -- when the segment's transaction had already
 * failed; that is an error here.
 */
static void gang_wait_all_ex(GpGang *g, PGresult **keep, bool commit,
							 bool keep_commands);

static void
gang_wait_all(GpGang *g, PGresult **keep, bool commit)
{
	gang_wait_all_ex(g, keep, commit, false);
}

/* Keep each segment's last successful result, whatever it was. */
static void
gang_wait_all_keeping_commands(GpGang *g, PGresult **keep)
{
	gang_wait_all_ex(g, keep, false, true);
}

static void
gang_wait_all_ex(GpGang *g, PGresult **keep, bool commit, bool keep_commands)
{
	List	   *errors = NIL;
	bool		broken = false;
	bool		stopped = false;
	int			nbusy;

	do
	{
		nbusy = 0;
		for (int i = 0; i < g->nconns; i++)
		{
			GpSegmentConn *c = &g->conns[i];

			/* Not ours: a gather's batch, which that gather will read. */
			if (!c->busy || c->fetching != NULL)
				continue;

			if (PQconsumeInput(c->conn) == 0)
			{
				collect_error(&errors, c->content, NULL, c->conn, NULL);
				c->busy = false;
				broken = true;
				continue;
			}

			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);
				ExecStatusType status;

				if (conn_pipeline_own(c, res))
					continue;
				if (res == NULL)
				{
					c->busy = false;
					break;
				}

				status = PQresultStatus(res);
				if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
					status != PGRES_EMPTY_QUERY)
				{
					collect_error(&errors, c->content, res, c->conn, NULL);
					if (PQstatus(c->conn) == CONNECTION_BAD)
						broken = true;
				}
				else if (commit && strcmp(PQcmdStatus(res), "ROLLBACK") == 0)
					collect_error(&errors, c->content, res, c->conn,
								  "the segment's part of this transaction had already failed");
				else if (keep != NULL && keep_commands)
				{
					if (keep[i] != NULL)
						PQclear(keep[i]);
					keep[i] = res;
					continue;	/* the caller frees it */
				}
				else if (keep != NULL && keep[i] == NULL &&
						 status == PGRES_TUPLES_OK)
				{
					keep[i] = res;
					continue;	/* the caller frees it */
				}
				PQclear(res);
			}

			if (c->busy)
				nbusy++;
		}

		/*
		 * A segment failed while the statement's slices run at once: the
		 * rest are stopped, as Cloudberry's dispatcher cancels the rest of a
		 * statement one of whose processes failed (checkDispatchResult(),
		 * cdbdisp_async.c).  Waiting for them could be for ever: a slice
		 * that sends to the one that failed as well waits for it -- in UDP
		 * packets, which a process whose statement failed no longer
		 * acknowledges -- and its other receivers wait for that slice.  What
		 * they answer, a cancel, raise_segment_errors() tells apart from
		 * the failure.
		 */
		if (nbusy > 0 && errors != NIL && active_streams != NIL && !stopped)
		{
			TimestampTz deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;

			readers_cancel_and_drain(&errors);
			for (int i = 0; i < g->nconns; i++)
			{
				GpSegmentConn *c = &g->conns[i];
				const char *err;

				if (!c->busy || c->fetching != NULL)
					continue;
				err = libpqsrv_cancel(c->conn, deadline);
				if (err != NULL)
					elog(DEBUG1, "could not cancel the query on segment %d: %s",
						 c->content, err);
			}
			stopped = true;
		}

		if (nbusy > 0)
			gang_wait(g);
	} while (nbusy > 0);

	if (broken)
		gang_close_broken();

	flush_segment_notices();
	if (errors != NIL)
		raise_segment_errors(errors);
}

/*
 * gang_wait_all(), keeping how many rows each statement changed: one count
 * per segment waited for (all, or the one "content" names), in content order.
 */
static void
gang_wait_all_counting(GpGang *g, uint64 *counts, int content, int nsegments)
{
	PGresult  **results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	int			n = 0;

	gang_wait_all_keeping_commands(g, results);

	for (int i = 0; i < g->nconns; i++)
	{
		if (!conn_asked(&g->conns[i], content, nsegments))
			continue;
		counts[n++] = results[i] ? strtou64(PQcmdTuples(results[i]), NULL, 10) : 0;
		if (results[i] != NULL)
			PQclear(results[i]);
	}
	pfree(results);
}

/*
 * Read whatever is in flight and throw it away, without raising: the paths
 * that call this are handling an error already.  A segment that does not
 * answer within a while, or whose connection breaks, costs the gang.
 */
static void
gang_drain_quietly(void)
{
	gang_drain_keeping(NULL);
}

/*
 * As gang_drain_quietly(), keeping the segments' errors in *errors where it
 * is given.
 */
static void
gang_drain_keeping(List **errors)
{
	GpGang	   *g = gang;

	if (g == NULL)
		return;

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		c->fetching = NULL;
		while (c->busy)
		{
			if (PQconsumeInput(c->conn) == 0)
			{
				gang_close();
				return;
			}

			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);

				if (conn_pipeline_own(c, res))
					continue;
				if (res == NULL)
				{
					c->busy = false;
					break;
				}
				if (errors != NULL && PQresultStatus(res) == PGRES_FATAL_ERROR)
					collect_error(errors, c->content, res, c->conn, NULL);
				PQclear(res);
			}

			if (c->busy)
			{
				WaitEvent	occurred[1];

				/*
				 * Not gang_wait(): that checks for interrupts, and this runs
				 * where an error is already on its way.  A segment that never
				 * answers would hang the abort, so the wait has a deadline.
				 */
				if (WaitEventSetWait(g->wes, 30 * 1000, occurred, 1,
									 dispatch_wait_event()) == 0)
				{
					gang_close();
					return;
				}
				if (occurred[0].events & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}
	}
}

/* Stop whatever the segments are doing, and read what is left. */
static void
gang_cancel_and_drain(void)
{
	if (gang == NULL)
		return;

	/*
	 * Cloudberry's fault as a gang's processes are cleaned up for the next
	 * statement (cleanupQE(), cdbutil.c): skipped, they are not kept, and the
	 * gang goes as one found broken does, its transaction's part with it.
	 */
	if (GP_FAULT("cleanup_qe") == GP_FAULT_SKIP)
	{
		gang_close();
		return;
	}

	readers_cancel_and_drain(NULL);

	for (int i = 0; i < gang->nconns; i++)
	{
		if (gang->conns[i].busy)
		{
			const char *err = libpqsrv_cancel(gang->conns[i].conn,
											  GetCurrentTimestamp() +
											  30 * USECS_PER_SEC);

			if (err != NULL)
				elog(DEBUG1, "could not cancel the query on segment %d: %s",
					 gang->conns[i].content, err);
		}
	}
	gang_drain_quietly();
}

/* Send a statement to every segment and wait for it, without raising. */
static void
gang_send_all_quietly(const char *sql)
{
	if (gang == NULL)
		return;

	for (int i = 0; i < gang->nconns; i++)
	{
		if (!PQsendQuery(gang->conns[i].conn, sql))
		{
			gang_close();
			return;
		}
		gang->conns[i].busy = true;
	}
	gang_drain_quietly();
}

/* ------------------------------------------------------------------------- */
/* Readers, for statements whose slices run at once                          */
/* ------------------------------------------------------------------------- */

/*
 * Read whatever the busy readers have answered, without waiting; an error is
 * kept with the stream the reader runs a slice of.  An idle reader is read
 * too: a connection that broke says so there, and its socket would wake
 * every wait after this one.
 */
static void
readers_poll(void)
{
	bool		broken = false;

	if (gang == NULL)
		return;

	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (r->conn == NULL)
			continue;

		if (PQconsumeInput(r->conn) == 0)
		{
			if (r->busy && r->stream != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				collect_error(&r->stream->errors, r->content, NULL, r->conn,
							  NULL);
				MemoryContextSwitchTo(oldcxt);
			}
			libpqsrv_disconnect(r->conn);
			r->conn = NULL;
			r->busy = false;
			broken = true;
			continue;
		}

		while (r->busy && !PQisBusy(r->conn))
		{
			PGresult   *res = PQgetResult(r->conn);
			ExecStatusType status;

			if (res == NULL)
			{
				r->busy = false;
				break;
			}
			status = PQresultStatus(res);
			if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
				status != PGRES_EMPTY_QUERY && r->stream != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

				collect_error(&r->stream->errors, r->content, res, r->conn,
							  NULL);
				MemoryContextSwitchTo(oldcxt);
			}
			PQclear(res);
		}
	}

	if (broken)
		gang_build_wes(gang);
}

/* A slice failed on a reader: the statement fails, with the best reason. */
static void
streams_raise_if_failed(void)
{
	List	   *errors = NIL;

	foreach_ptr(GpStream, stream, active_streams)
	{
		errors = list_concat(errors, stream->errors);
		stream->errors = NIL;
	}
	if (errors != NIL)
		raise_segment_errors(errors);
}

/*
 * Is this reader one readers_stop() stops: one of the stream's, when a
 * stream is given, and otherwise any but a held stream's, whose readers are
 * its cursor's to stop (GpStreamCancel()).
 */
static bool
reader_stopped_by(const GpReaderConn *r, const GpStream *only)
{
	if (r->conn == NULL || !r->busy)
		return false;
	if (only != NULL)
		return r->stream == only;
	return r->stream == NULL || !r->stream->held;
}

/*
 * Stop every reader that is still running a slice and read it to the end:
 * those of one stream, or all but a held stream's.  Raises nothing; a reader
 * that does not answer within a while is dropped.
 */
static void
readers_stop(GpStream *only)
{
	TimestampTz deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	bool		any;

	if (gang == NULL)
		return;

	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		if (reader_stopped_by(r, only))
		{
			const char *err = libpqsrv_cancel(r->conn, deadline);

			if (err != NULL)
				elog(DEBUG1, "could not cancel the slice on segment %d: %s",
					 r->content, err);
		}
	}

	do
	{
		WaitEvent	occurred[1];

		readers_poll();
		any = false;
		foreach_ptr(GpReaderConn, r, gang->readers)
			if (reader_stopped_by(r, only))
				any = true;
		if (!any)
			break;
		if (GetCurrentTimestamp() >= deadline)
		{
			foreach_ptr(GpReaderConn, r, gang->readers)
			{
				if (reader_stopped_by(r, only))
				{
					libpqsrv_disconnect(r->conn);
					r->conn = NULL;
					r->busy = false;
				}
			}
			gang_build_wes(gang);
			break;
		}
		if (WaitEventSetWait(gang->wes, 1000, occurred, 1,
							 dispatch_wait_event()) > 0 &&
			(occurred[0].events & WL_LATCH_SET))
			ResetLatch(MyLatch);
	} while (any);
}

/*
 * Stop every reader still running a slice of a statement, adding what they
 * answered to *errors, when given.  Called with an error on its way.
 */
static void
readers_cancel_and_drain(List **errors)
{
	readers_stop(NULL);

	foreach_ptr(GpStream, stream, active_streams)
	{
		if (errors != NULL)
			*errors = list_concat(*errors, stream->errors);
		stream->errors = NIL;
	}
}

/* The readers go back to the session, and the streams are forgotten. */
static void
streams_release(void)
{
	foreach_ptr(GpStream, stream, active_streams)
	{
		foreach_ptr(GpReaderConn, r, stream->readers)
			r->stream = NULL;
		list_free(stream->readers);
		pfree(stream);
	}
	list_free(active_streams);
	active_streams = NIL;
}

/* Run a statement on a reader and wait for it, raising what it answers. */
static void
reader_exec(GpReaderConn *r, const char *sql, char **value)
{
	PGresult   *res = libpqsrv_exec(r->conn, sql, dispatch_wait_event());
	ExecStatusType status = res ? PQresultStatus(res) : PGRES_FATAL_ERROR;

	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
	{
		List	   *errors = NIL;

		collect_error(&errors, r->content, res, r->conn, NULL);
		if (res != NULL)
			PQclear(res);
		if (PQstatus(r->conn) == CONNECTION_BAD)
		{
			libpqsrv_disconnect(r->conn);
			r->conn = NULL;
			gang_build_wes(gang);
		}
		raise_segment_errors(errors);
	}
	if (value != NULL)
		*value = PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)
			? MemoryContextStrdup(TopMemoryContext, PQgetvalue(res, 0, 0))
			: NULL;
	PQclear(res);
}

/*
 * One more reader on a segment, with the writer's identity, made as a gang's
 * connections are (connect_segments()), as Cloudberry makes a reader gang.
 * The user its writer connected as, who the writer's session is: the
 * session's, until a SET SESSION AUTHORIZATION, which leaves the gang as it
 * is (gp_share.c checks that a reader's is its writer's).
 */
static GpReaderConn *
reader_connect(GpGang *g, int content)
{
	GpConnAttempt attempt;
	PGconn	   *conn;
	GpReaderConn *r;

	conn_attempt_init(&attempt, GpClusterSegmentByContent(content),
					  get_database_name(MyDatabaseId), gang_username, true);
	connect_segments(&attempt, 1);
	conn = attempt.conn;

	r = MemoryContextAllocZero(TopMemoryContext, sizeof(GpReaderConn));
	r->content = content;
	r->conn = conn;
	last_word_forget(conn);
	PQsetNoticeReceiver(conn, segment_notice_receiver, conn);
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		g->readers = lappend(g->readers, r);
		MemoryContextSwitchTo(oldcxt);
	}
	gang_build_wes(g);
	return r;
}

/* Tell a reader the settings that changed since it was last told. */
static void
reader_sync_settings(GpReaderConn *r)
{
	StringInfoData sql;
	const char *values[NUM_SYNCED_SETTINGS];
	bool		any = false;
	bool		reset = false;

	run_sync_callbacks();
	initStringInfo(&sql);
	appendStringInfoString(&sql, GP_SETTINGS_MARKER "SELECT ");
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		values[i] = sync_value(i);
		if (values[i] == NULL ||
			(r->sent[i] != NULL && strcmp(r->sent[i], values[i]) == 0))
			continue;
		sync_reset_role(&sql, r->sent, i, &any, &reset);
		appendStringInfo(&sql, "%spg_catalog.set_config(%s, %s, false)",
						 any ? ", " : "",
						 quote_literal_cstr(synced_settings[i]),
						 quote_literal_cstr(values[i]));
		any = true;
	}
	if (!any)
		return;

	reader_exec(r, sql.data, NULL);
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (values[i] == NULL)
			continue;
		if (r->sent[i] != NULL)
			pfree(r->sent[i]);
		r->sent[i] = MemoryContextStrdup(TopMemoryContext, values[i]);
	}
}

GpStream *
GpStreamBegin(void)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	GpStream   *stream = palloc0(sizeof(GpStream));

	(void) gang_get();
	active_streams = lappend(active_streams, stream);
	MemoryContextSwitchTo(oldcxt);
	return stream;
}

/* The query that asks a segment process where it receives over a transport. */
static char *
icaddress_query(const GpIcTransport *transport)
{
	return psprintf("SELECT gp_internal.interconnect_address(%s)",
					quote_literal_cstr(transport->name));
}

const char *
GpStreamWriterAddress(int content, const GpIcTransport *transport, int *pid)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;
	int			t = GpIcTransportIndex(transport);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		elog(ERROR, "there is no segment with content id %d", content);

	if (c->icaddress[t] == NULL)
	{
		char	  **values = palloc0_array(char *, g->nconns);

		GpDispatchQueryFirstValues(icaddress_query(transport), -1, values);
		for (int i = 0; i < g->nconns; i++)
		{
			if (values[i] == NULL)
				elog(ERROR, "segment %d gave no interconnect address",
					 g->conns[i].content);
			g->conns[i].icaddress[t] = MemoryContextStrdup(TopMemoryContext,
														   values[i]);
		}
	}
	*pid = PQbackendPID(c->conn);
	return c->icaddress[t];
}

int
GpStreamAddReader(GpStream *stream, int content,
				  const GpIcTransport *transport, const char **address)
{
	GpGang	   *g = gang_get();
	GpReaderConn *found = NULL;
	int			count = 0;
	int			t;
	MemoryContext oldcxt;

	readers_poll();				/* a broken one is found broken now */
	foreach_ptr(GpReaderConn, r, g->readers)
	{
		if (r->content != content || r->conn == NULL)
			continue;
		count++;
		if (found == NULL && !r->busy && r->stream == NULL)
			found = r;
	}
	if (found == NULL)
	{
		if (count >= MAX_READERS_PER_SEGMENT)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("a statement needs more than %d readers on segment %d",
							MAX_READERS_PER_SEGMENT, content)));
		found = reader_connect(g, content);
	}

	/*
	 * Where it receives over the transport, asked the first time -- out of
	 * the transaction a slice that failed there left open.
	 */
	t = GpIcTransportIndex(transport);
	if (found->icaddress[t] == NULL)
	{
		if (PQtransactionStatus(found->conn) != PQTRANS_IDLE)
			reader_exec(found, "ROLLBACK", NULL);
		reader_exec(found, icaddress_query(transport), &found->icaddress[t]);
		if (found->icaddress[t] == NULL)
			elog(ERROR, "segment %d gave its reader no interconnect address",
				 content);
	}

	found->stream = stream;
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	stream->readers = lappend(stream->readers, found);
	MemoryContextSwitchTo(oldcxt);
	*address = found->icaddress[t];
	return list_length(stream->readers) - 1;
}

void
GpStreamStartReader(GpStream *stream, int reader, const char *sql)
{
	GpReaderConn *r = (GpReaderConn *) list_nth(stream->readers, reader);

	if (r->conn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost a reader's connection to segment %d", r->content)));

	/* a slice that failed left its transaction open */
	if (PQtransactionStatus(r->conn) != PQTRANS_IDLE)
		reader_exec(r, "ROLLBACK", NULL);
	reader_sync_settings(r);

	if (!PQsendQuery(r->conn, sql))
	{
		char	   *msg = pstrdup(PQerrorMessage(r->conn));

		libpqsrv_disconnect(r->conn);
		r->conn = NULL;
		gang_build_wes(gang);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a slice to segment %d", r->content),
				 errdetail_internal("%s", msg)));
	}
	r->busy = true;
}

void
GpStreamEnd(GpStream *stream)
{
	for (;;)
	{
		bool		busy = false;

		readers_poll();
		streams_raise_if_failed();
		foreach_ptr(GpReaderConn, r, stream->readers)
			if (r->conn != NULL && r->busy)
				busy = true;
		if (!busy || gang == NULL)
			break;
		gang_wait(gang);
	}

	foreach_ptr(GpReaderConn, r, stream->readers)
		r->stream = NULL;
	active_streams = list_delete_ptr(active_streams, stream);
	list_free(stream->readers);
	pfree(stream);
}

/* ------------------------------------------------------------------------- */
/* A parallel retrieve cursor's stream                                       */
/* ------------------------------------------------------------------------- */

/*
 * A parallel retrieve cursor's slices run on readers that stay the cursor's
 * until it is closed, across the statements of its transaction: a held
 * stream (gp_endpoint.c).  It is no statement's, so no statement's end
 * releases it, and no other statement's wait raises what its readers
 * answer or stops them; the cursor asks, as it waits for its endpoints and
 * as it is closed, as Cloudberry's cursor checks its own dispatcher state.
 */
GpStream *
GpStreamBeginHeld(void)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	GpStream   *stream = palloc0(sizeof(GpStream));

	(void) gang_get();
	stream->held = true;
	MemoryContextSwitchTo(oldcxt);
	return stream;
}

static GpReaderConn *
stream_reader(GpStream *stream, int reader)
{
	GpReaderConn *r = (GpReaderConn *) list_nth(stream->readers, reader);

	if (r->conn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost a reader's connection to segment %d", r->content)));
	return r;
}

/*
 * A statement on one of its readers, waited for; what it answers is raised.
 * With "settings", the settings that changed since the reader was last told
 * go first, for a statement that reads them.
 */
void
GpStreamReaderExec(GpStream *stream, int reader, const char *sql,
				   bool settings)
{
	GpReaderConn *r = stream_reader(stream, reader);

	/* one its slice left in a transaction, which is over */
	if (PQtransactionStatus(r->conn) != PQTRANS_IDLE)
		reader_exec(r, "ROLLBACK", NULL);
	if (settings)
		reader_sync_settings(r);
	reader_exec(r, sql, NULL);
}

/* A statement sent to one of its readers, in the transaction it has open. */
void
GpStreamContinueReader(GpStream *stream, int reader, const char *sql)
{
	GpReaderConn *r = stream_reader(stream, reader);

	if (!PQsendQuery(r->conn, sql))
	{
		char	   *msg = pstrdup(PQerrorMessage(r->conn));

		libpqsrv_disconnect(r->conn);
		r->conn = NULL;
		gang_build_wes(gang);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send a slice to segment %d", r->content),
				 errdetail_internal("%s", msg)));
	}
	r->busy = true;
}

/*
 * Wait for its readers to finish what they were sent, for at most timeout_ms
 * (-1: as long as it takes): true when none is busy any more.  What they
 * answered stays with the stream, for GpStreamFailed() and GpStreamRaise();
 * one that answered an error ends the wait.
 */
bool
GpStreamWait(GpStream *stream, long timeout_ms)
{
	TimestampTz deadline = timeout_ms > 0
		? TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms) : 0;

	for (;;)
	{
		bool		busy = false;
		long		wait_ms = 1000;
		WaitEvent	occurred[1];

		/*
		 * Twice: a reader whose backend ended after saying something -- its
		 * last warning -- says it closed only on the next read.
		 */
		CHECK_FOR_INTERRUPTS();
		readers_poll();
		readers_poll();
		foreach_ptr(GpReaderConn, r, stream->readers)
			if (r->conn != NULL && r->busy)
				busy = true;
		if (!busy || stream->errors != NIL || gang == NULL)
			return !busy;
		if (timeout_ms == 0)
			return false;
		if (timeout_ms > 0)
		{
			long		left = TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
															   deadline);

			if (left <= 0)
				return false;
			wait_ms = Min(wait_ms, left);
		}
		flush_segment_notices();
		if (WaitEventSetWait(gang->wes, wait_ms, occurred, 1,
							 dispatch_wait_event()) > 0 &&
			(occurred[0].events & WL_LATCH_SET))
			ResetLatch(MyLatch);
	}
}

/* Is one of its readers connected, and done with what it was sent? */
bool
GpStreamReaderDone(GpStream *stream, int reader)
{
	GpReaderConn *r = (GpReaderConn *) list_nth(stream->readers, reader);

	return r->conn != NULL && !r->busy;
}

/* Has one of its readers answered an error? */
bool
GpStreamFailed(GpStream *stream)
{
	return stream->errors != NIL;
}

/*
 * Raise what its readers answered, the cause over its consequences, once
 * the ones still busy are stopped: a slice that failed fails the cursor.
 */
void
GpStreamRaise(GpStream *stream)
{
	List	   *errors = stream->errors;

	if (errors == NIL)
		return;
	stream->errors = NIL;
	readers_stop(stream);
	errors = list_concat(errors, stream->errors);
	stream->errors = NIL;
	raise_segment_errors(errors_cause_first(errors));
}

/* Stop its readers that are still busy, and forget what they answer. */
void
GpStreamCancel(GpStream *stream)
{
	readers_stop(stream);
	stream->errors = NIL;
}

/* The readers go back to the session, and the stream is forgotten. */
void
GpStreamRelease(GpStream *stream)
{
	foreach_ptr(GpReaderConn, r, stream->readers)
		if (r->stream == stream)
			r->stream = NULL;
	list_free(stream->readers);
	pfree(stream);
}

/* ------------------------------------------------------------------------- */
/* The segments' part of the coordinator's transaction                       */
/* ------------------------------------------------------------------------- */

/*
 * Tell the segments the settings that changed since they were last told.
 *
 * set_config() rather than SET: it takes a value as SHOW prints it, which is
 * the one form every setting reads back -- a list like search_path included.
 */
static void
gang_sync_settings(GpGang *g)
{
	StringInfoData sql;
	const char *values[NUM_SYNCED_SETTINGS];
	bool		any = false;
	bool		reset = false;

	run_sync_callbacks();
	initStringInfo(&sql);
	appendStringInfoString(&sql, GP_SETTINGS_MARKER "SELECT ");

	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		values[i] = sync_value(i);
		if (values[i] == NULL)
			continue;
		if (g->sent[i] != NULL && strcmp(g->sent[i], values[i]) == 0)
			continue;

		sync_reset_role(&sql, g->sent, i, &any, &reset);
		appendStringInfo(&sql, "%spg_catalog.set_config(%s, %s, false)",
						 any ? ", " : "",
						 quote_literal_cstr(synced_settings[i]),
						 quote_literal_cstr(values[i]));
		any = true;
	}

	if (!any)
		return;

	notices_quiet++;
	gang_send_all(g, sql.data);
	gang_wait_all(g, NULL, false);
	notices_quiet--;

	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (values[i] == NULL)
			continue;
		if (g->sent[i] != NULL)
			pfree(g->sent[i]);
		g->sent[i] = MemoryContextStrdup(TopMemoryContext, values[i]);
	}
}

/* Forget what the segments were told: a rollback may have undone it. */
static void
gang_forget_settings(void)
{
	if (gang == NULL)
		return;
	for (int i = 0; i < NUM_SYNCED_SETTINGS; i++)
	{
		if (gang->sent[i] != NULL)
			pfree(gang->sent[i]);
		gang->sent[i] = NULL;
	}
}

/*
 * A SET of the client's has run here, of one of the settings above: what it
 * changed is told the session's gang now, as Cloudberry dispatches a SET to
 * the session's gangs as it runs it (DispatchSetPGVariable(), guc_funcs.c),
 * and not only with the next statement sent there -- so that a value a
 * segment refuses fails the SET, which the coordinator's rollback undoes,
 * where with the next statement it would fail that one and every one after
 * it.  Only to a gang there is, and not in the segments' transaction: there
 * the next statement tells them, as ever, and its failure aborts the
 * transaction, the SET's with it.  A reader is told before its next slice, as
 * ever, and a gang in a cluster that changed since is left to the next
 * statement, which takes the change.
 */
void
GpDispatchSyncSettingsNow(const char *name)
{
	bool		synced = false;

	for (int i = 0; i < NUM_SYNCED_SETTINGS && !synced; i++)
		synced = pg_strcasecmp(synced_settings[i], name) == 0;
	if (!synced || gang == NULL || gang_in_xact || copying != NULL ||
		active_streams != NIL || GpClusterStale() || !IsTransactionState())
		return;
	gang_sync_settings(gang);
}

static const char *
isolation_level_name(void)
{
	switch (XactIsoLevel)
	{
		case XACT_SERIALIZABLE:
			return "SERIALIZABLE";
		case XACT_REPEATABLE_READ:
			return "REPEATABLE READ";
		case XACT_READ_UNCOMMITTED:
			return "READ UNCOMMITTED";
		default:
			return "READ COMMITTED";
	}
}

/*
 * The objects whose label -- "gp", or another module's provider -- this
 * transaction changed and the segments have not been sent yet.  What is sent
 * is the label as it is when it is sent -- a savepoint rolled back, an object
 * dropped, both come out right -- so an object is noted once per provider
 * however often it changes.  In the transaction's memory, and forgotten when
 * it ends.
 */
typedef struct PendingLabel
{
	ObjectAddress object;
	char		provider[NAMEDATALEN];
} PendingLabel;

static List *labels_pending = NIL;	/* of PendingLabel * */
static bool labels_held = false;	/* a DDL tree is on its way */

void
GpDispatchNoteLabel(const ObjectAddress *object)
{
	GpDispatchNoteLabelOf(object, GP_LABEL_PROVIDER);
}

void
GpDispatchNoteLabelOf(const ObjectAddress *object, const char *provider)
{
	ListCell   *lc;
	PendingLabel *copy;
	MemoryContext oldcxt;

	/*
	 * Only what the coordinator changes, and only where there are segments.
	 * A shared object's label is not the segments' business: a tablespace,
	 * for one, is this node's alone.
	 */
	if (GpClusterIsSingleNode() || GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		IsSharedRelation(object->classId))
		return;

	foreach(lc, labels_pending)
	{
		PendingLabel *p = (PendingLabel *) lfirst(lc);

		if (p->object.classId == object->classId &&
			p->object.objectId == object->objectId &&
			p->object.objectSubId == object->objectSubId &&
			strcmp(p->provider, provider) == 0)
			return;
	}

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	copy = palloc_object(PendingLabel);
	copy->object = *object;
	strlcpy(copy->provider, provider, NAMEDATALEN);
	labels_pending = lappend(labels_pending, copy);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Send the segments the labels noted since they were last sent, each as the
 * SECURITY LABEL that writes it.  Inside the segments' transaction, so that
 * they commit or roll back with the coordinator's own.
 */
static void
gang_sync_labels(GpGang *g)
{
	List	   *pending = labels_pending;
	ListCell   *lc;

	if (labels_held || pending == NIL)
		return;
	labels_pending = NIL;

	foreach(lc, pending)
	{
		PendingLabel *p = (PendingLabel *) lfirst(lc);
		char	   *payload = GpDdlLabelPayloadOf(&p->object, p->provider,
												  GetSecurityLabel(&p->object,
																   p->provider));

		if (payload == NULL)
			continue;
		notices_quiet++;
		gang_send_all(g, payload);
		gang_wait_all(g, NULL, false);
		notices_quiet--;
		pfree(payload);
	}
}

/* Forget the distributed snapshot sent: a rollback may have undone it. */
static void
gang_forget_snapshot(void)
{
	if (gang_ds_sent != NULL)
		pfree(gang_ds_sent);
	gang_ds_sent = NULL;
}

/*
 * The statement's snapshot, as a distributed one: sent before the statement,
 * once per snapshot -- once per transaction when it is REPEATABLE READ, once
 * per statement when it is READ COMMITTED.  A segment may wait here for a
 * transaction the snapshot says committed and that it holds only prepared,
 * before its statement takes a snapshot of its own (gp_dtx.c).
 */
static void
gang_sync_snapshot(GpGang *g)
{
	char	   *ds;

	if (!ActiveSnapshotSet())
		return;
	ds = GpDtxSnapshotString(GetActiveSnapshot());
	if (ds == NULL || (gang_ds_sent != NULL && strcmp(ds, gang_ds_sent) == 0))
		return;

	notices_quiet++;
	gang_send_all(g, psprintf("SET LOCAL " GP_DTX_SNAPSHOT_SETTING " = '%s'", ds));
	gang_wait_all(g, NULL, false);
	notices_quiet--;

	gang_forget_snapshot();
	gang_ds_sent = MemoryContextStrdup(TopMemoryContext, ds);
}

/*
 * The segments' next level of subtransaction, SAVEPOINT gp_sp_N: sent as
 * Cloudberry sends a user's SAVEPOINT -- SQL, whose tag debug_dtm_action's
 * SQL target names -- or the subtransaction a function's block begins -- a
 * protocol command, with the level it begins at (gp_dtm_debug.c).
 */
static void
gang_send_savepoint(GpGang *g, bool user)
{
	int			next = gang_xact_depth + 1;
	char	   *sql = psprintf("SAVEPOINT gp_sp_%d", next);

	notices_quiet++;
	if (user)
		gang_send_all_dtm(g, sql, GP_DTX_NONE, "SAVEPOINT", 0);
	else
		gang_send_all_dtm(g, sql, GP_DTX_SUBTRANSACTION_BEGIN, NULL,
						  gang_xact_depth);
	gang_wait_all(g, NULL, false);
	gang_dtm_end(g);
	notices_quiet--;
	gang_xact_depth = next;
}

/*
 * Get the segments ready for a statement: the settings it depends on, and --
 * unless it is one that runs in a transaction of its own -- the coordinator's
 * transaction, down to the savepoint it is being run in, and its snapshot.
 */
static void
gang_prepare(GpGang *g, bool in_xact)
{
	int			level;

	GpDispatchRaiseKeptError();
	gang_sync_settings(g);

	if (!in_xact)
	{
		if (gang_in_xact)
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("cannot run this statement on the segments inside a transaction that has already used them")));
		return;
	}

	if (!gang_in_xact)
	{
		notices_quiet++;
		gang_send_all(g, psprintf("BEGIN ISOLATION LEVEL %s%s",
								  isolation_level_name(),
								  XactReadOnly ? " READ ONLY" : ""));
		gang_wait_all(g, NULL, false);
		notices_quiet--;
		gang_in_xact = true;
		gang_xact_depth = 1;
		gather_counter = 0;
	}

	level = GetCurrentTransactionNestLevel();
	while (gang_xact_depth < level)
		gang_send_savepoint(g, GpDtmDebugLevelIsUser(gang_xact_depth + 1));

	gang_sync_snapshot(g);
	gang_sync_labels(g);
}

/*
 * A user's SAVEPOINT, or ROLLBACK TO, sent as it runs, as Cloudberry sends
 * it, where the port sends a savepoint with the next statement and rolls it
 * back as the coordinator's rolls back: debug_dtm_action's SQL target asks
 * for its failure at the statement (gp_dtm_debug.c).  The segments'
 * transaction is begun if it is not, and the levels not sent yet are sent
 * first.  "level" is the savepoint's.
 */
void
GpDispatchSavepointNow(int level)
{
	GpGang	   *g = gang_get();

	gang_prepare(g, true);
	if (gang_xact_depth == level - 1)
		gang_send_savepoint(g, true);
}

void
GpDispatchRollbackToNow(int level)
{
	if (gang == NULL || !gang_in_xact || gang_xact_depth < level)
		return;
	notices_quiet++;
	gang_send_all_dtm(gang, psprintf("ROLLBACK TO SAVEPOINT gp_sp_%d", level),
					  GP_DTX_NONE, "ROLLBACK", 0);
	gang_wait_all(gang, NULL, false);
	gang_dtm_end(gang);
	notices_quiet--;

	/*
	 * The levels above are gone, and the coordinator's abort of them sends
	 * nothing; its restart of this one is sent with the next statement.
	 */
	gang_xact_depth = level;
}

/* ------------------------------------------------------------------------- */
/* Two-phase commit                                                          */
/* ------------------------------------------------------------------------- */

static void
dtx_forget(void)
{
	dtx_nprepared = 0;
	dtx_all_prepared = false;
	dtx_gid[0] = '\0';
	if (dtx_prepared != NULL)
		memset(dtx_prepared, 0, dtx_prepared_size * sizeof(bool));
}

/*
 * The contents asked to prepare, kept apart from the gang: a connection that
 * breaks as its part prepares -- its segment's panic -- closes the gang, and
 * each part is then finished over a connection of its own
 * (dtx_finish_apart()).
 */
static int *dtx_apart = NULL;	/* in TopMemoryContext */
static int	dtx_apart_size = 0;
static int	dtx_napart = 0;

static void
dtx_keep_apart(GpGang *g, const bool *writes)
{
	if (dtx_apart_size < g->nconns)
	{
		if (dtx_apart != NULL)
			pfree(dtx_apart);
		dtx_apart = MemoryContextAlloc(TopMemoryContext, g->nconns * sizeof(int));
		dtx_apart_size = g->nconns;
	}
	dtx_napart = 0;
	for (int i = 0; i < g->nconns; i++)
	{
		if (writes[i])
			dtx_apart[dtx_napart++] = g->conns[i].content;
	}
}

/*
 * Has this segment's part written?  It says so with the answer to every
 * statement it is sent, as the transaction ID its part has (gp_dtx.c): empty
 * while it has none.  A segment that has never said is taken to have
 * written, which costs it no more than being prepared.
 */
static bool
conn_wrote(const GpSegmentConn *c)
{
	const char *xid = PQparameterStatus(c->conn, GP_DTX_XID_SETTING);

	return xid == NULL || xid[0] != '\0';
}

/*
 * Commit ordering (gp_dtx.c): the coordinator transactions whose one-phase
 * parts a segment's part may have seen committed before they ended, as it
 * reported them committing or preparing.  This transaction ends after each
 * of theirs, so that no distributed snapshot sees it committed and one of
 * them in progress.  Waited for here, before this one's commit record: the
 * transactions waited for are past their own first phase, and wait for
 * nothing of this one's.
 */
static void
dtx_wait_for_depends(const GpSegmentConn *c)
{
	const char *list = PQparameterStatus(c->conn, GP_DTX_DEPENDS_SETTING);
	FullTransactionId next = ReadNextFullTransactionId();
	TransactionId self = GetTopTransactionIdIfAny();
	char	   *copy;
	char	   *save = NULL;

	if (list == NULL || list[0] == '\0')
		return;
	copy = pstrdup(list);
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
		 tok = strtok_r(NULL, ",", &save))
	{
		FullTransactionId gxid = FullTransactionIdFromU64(strtou64(tok, NULL, 10));
		TransactionId xid = XidFromFullTransactionId(gxid);

		if (!FullTransactionIdPrecedes(gxid, next) ||
			!TransactionIdIsNormal(xid) || TransactionIdEquals(xid, self))
			continue;
		if (TransactionIdIsInProgress(xid))
			XactLockTableWait(xid, NULL, NULL, XLTW_None);
	}
	pfree(copy);
}

/*
 * A command of the commit's INFO line, when gp.test_print_direct_dispatch_info
 * asks for one: to the n segments "set" lists, and every segment the
 * transaction reached when "reached" says so, in the order the transaction
 * first reached them (gp_settings.c), as Cloudberry names its dtxSegments.
 */
static void
dtx_report(const char *command, const int *set, int nset, bool reached)
{
	int		   *contents;
	int			n;

	if (!gp_test_print_direct_dispatch_info)
		return;
	n = GpReportDtxContents(set, nset, reached, &contents);
	GpReportDtxCommand(command, contents, n);
	pfree(contents);
}

/*
 * A part's PREPARE TRANSACTION or one-phase COMMIT, as debug_dtm_action asks
 * for it (gp_dtm_debug.c): failing as it begins, as the part prepares or
 * commits -- so that the part ends as after any failed PREPARE, rolled back
 * -- or after it, the error raised once it is answered (gang_dtm_end()).
 */
static const char *
dtm_commit_statement(GpSegmentConn *c, GpDtxCommand command, const char *sql)
{
	char	   *msg;
	int			action = GpDtmDebugProtocol(command, c->content, 0, &msg);

	switch (action)
	{
		case GP_DTM_ACTION_FAIL_BEGIN:
			return psprintf("%s; %s", GpDtmDebugFailAtCommitStatement(msg), sql);
		case GP_DTM_ACTION_PANIC_BEGIN:
			return GpDtmDebugRaiseStatement(action, msg);
		case GP_DTM_ACTION_FAIL_END:
			dtm_end_conn = c - gang->conns;
			dtm_end_raise = GpDtmDebugRaiseStatement(action, msg);
			return sql;
		default:
			return sql;
	}
}

/*
 * The first phase, at PRE_COMMIT, while raising still undoes the
 * coordinator's part.  Which segments' parts wrote each has said with its
 * answers (conn_wrote()).  One that did not commits now, having nothing to
 * decide.  One that wrote alone, the coordinator having written nothing and
 * written nothing to another of its databases, commits in one phase, as
 * Cloudberry's does (prepareDtxTransaction(), cdb/cdbtm.c): told the
 * coordinator's transaction ID it commits under, which this gives the
 * transaction and which the coordinator's snapshots see in progress until
 * this transaction ends.  Its commit is the decision, so nothing waits for a
 * commit record here: one round trip, one flush.  Parts that wrote beside
 * another, or beside the coordinator, are prepared under that ID, whose
 * commit record, forced to disk before any segment is told (ForceSyncCommit),
 * is the decision (gp_dtx.c).  A failure here raises, and the abort rolls
 * back whatever was prepared.  Either way, what a part that wrote reports it
 * may have seen of others' one-phase commits is waited for before this
 * transaction ends.
 */
static void
gang_commit_first_phase(GpGang *g)
{
	bool	   *writes = palloc0_array(bool, g->nconns);
	int		   *writers = palloc_array(int, g->nconns);
	int			nwriters = 0;
	int			lone = -1;

	/*
	 * A batch a gather asked for ahead of need is read first: its FETCH may
	 * have written, and the answer says so.
	 */
	for (int i = 0; i < g->nconns; i++)
	{
		if (g->conns[i].busy && g->conns[i].fetching != NULL)
			conn_park(&g->conns[i]);
	}
	for (int i = 0; i < g->nconns; i++)
	{
		writes[i] = conn_wrote(&g->conns[i]);
		if (writes[i])
		{
			writers[nwriters++] = g->conns[i].content;
			lone = i;
		}
	}

	/* whatever happens now, no segment is left in the transaction */
	gang_in_xact = false;
	gang_xact_depth = 0;

	if (nwriters == 1 && !TransactionIdIsValid(GetTopTransactionIdIfAny()) &&
		!GpLoopbackHasWrites())
	{
		FullTransactionId gxid = GetTopFullTransactionId();

		/* the others' COMMIT is a one-phase commit of nothing */
		dtx_report("Distributed Commit (one-phase)", writers, 1, true);
		notices_quiet++;
		dtm_end_conn = -1;
		for (int i = 0; i < g->nconns; i++)
			conn_send(&g->conns[i],
					  i == lone
					  ? dtm_commit_statement(&g->conns[i], GP_DTX_COMMIT_ONEPHASE,
											 psprintf("SET LOCAL " GP_DTX_ONE_PHASE_SETTING " = '" UINT64_FORMAT "'; COMMIT",
													  U64FromFullTransactionId(gxid)))
					  : "COMMIT");
		gang_wait_all(g, NULL, true);
		gang_dtm_end(g);
		notices_quiet--;
		dtx_wait_for_depends(&g->conns[lone]);
		return;
	}

	notices_quiet++;
	if (nwriters > 0)
	{
		GpDtxFormGid(GetTopFullTransactionId(), dtx_gid);
		ForceSyncCommit();
		if (dtx_prepared_size < g->nconns)
		{
			if (dtx_prepared != NULL)
				pfree(dtx_prepared);
			dtx_prepared = MemoryContextAllocZero(TopMemoryContext,
												  g->nconns * sizeof(bool));
			dtx_prepared_size = g->nconns;
		}
		dtx_report("Distributed Prepare", writers, nwriters, false);
		dtx_keep_apart(g, writes);
	}
	else
		dtx_report("Distributed Commit (one-phase)", NULL, 0, true);

	dtm_end_conn = -1;
	for (int i = 0; i < g->nconns; i++)
	{
		if (writes[i])
		{
			/* the abort rolls it back, whether or not it was prepared */
			dtx_prepared[i] = true;
			dtx_nprepared++;
			conn_send(&g->conns[i],
					  dtm_commit_statement(&g->conns[i], GP_DTX_PREPARE,
										   psprintf("PREPARE TRANSACTION '%s'", dtx_gid)));
		}
		else
			conn_send(&g->conns[i], "COMMIT");
	}
	gang_wait_all(g, NULL, true);
	gang_dtm_end(g);
	notices_quiet--;

	/*
	 * Every part that wrote is prepared, and an abort from here on rolls back
	 * a transaction prepared wherever it wrote.  Cloudberry's fault is here,
	 * after its broadcast (doPrepareTransaction()).
	 */
	if (nwriters > 0)
	{
		dtx_all_prepared = true;
		GP_FAULT("dtm_broadcast_prepare");
		for (int i = 0; i < g->nconns; i++)
		{
			if (writes[i])
				dtx_wait_for_depends(&g->conns[i]);
		}

		/* and this one after prepareDtxTransaction() (xact.c) */
		GP_FAULT("transaction_abort_after_distributed_prepared");
		if (gp_debug_abort_after_distributed_prepared)
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE('X', 'X', '0', '0', '9')),
					 errmsg("Raise an error as directed by Debug_abort_after_distributed_prepared")));

		/*
		 * and this one before its distributed commit record, which
		 * RecordTransactionCommit() writes next (xact.c): the parts are all
		 * prepared, and an error still aborts every one
		 */
		GP_FAULT("before_xlog_xact_distributed_commit");
	}
}

/*
 * A part's second phase, done again over a connection of its own to its
 * content's primary now -- Cloudberry's retried COMMIT PREPARED
 * (doNotifyingCommitPrepared(), cdbtm.c):
 *
 *   on a primary FTS has failed over from since, it is finished on the new
 *   primary -- the mirror it promoted, which has the part from PREPARE's
 *   WAL -- once the promotion has taken: it may still be in recovery, which
 *   PostgreSQL's startup process leaves up to wal_retrieve_retry_interval
 *   after a promotion is asked for.  The old primary is not told: it is no
 *   part of the cluster now, and a commit there would wait for ever for a
 *   mirror that has left it;
 *
 *   on one whose connection broke as it was told -- its backend ended, a
 *   commit's perhaps while waiting for the mirror -- it is told again.  A
 *   commit whose part is gone, committed by that backend, waits for the
 *   mirror to have its commit, as Cloudberry's does (FinishPreparedTransaction(),
 *   twophase.c), so that what the client is told committed is on the mirror.
 *
 * False when it could not be done in a while, and the recovery process is
 * left to do it.  Nothing is raised.
 */
static bool
dtx_finish_again(int content, const char *sql, bool commit)
{
	const GpSegmentConfig *nodes;
	int			nnodes = GpClusterNodes(&nodes);
	GpClusterNodeState *states = palloc_array(GpClusterNodeState, Max(nnodes, 1));
	const GpSegmentConfig *node = NULL;
	TimestampTz deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	bool		done = false;

	(void) GpClusterLiveStates(states);
	for (int i = 0; i < nnodes; i++)
		if (nodes[i].content == content && states[i].role == 'p' &&
			states[i].dbid == nodes[i].dbid)
			node = &nodes[i];
	pfree(states);
	if (node == NULL || gang_dbname == NULL || gang_username == NULL)
		return false;

	PG_TRY();
	{
		while (!done && GetCurrentTimestamp() < deadline)
		{
			const char *keywords[5 + GP_INTERNAL_CONN_OPTIONS];
			const char *values[5 + GP_INTERNAL_CONN_OPTIONS];
			char		portbuf[16];
			int			n = 0;
			PGconn	   *conn;

			snprintf(portbuf, sizeof(portbuf), "%d", node->port);
			keywords[n] = "host";
			values[n++] = node->hostname;
			keywords[n] = "port";
			values[n++] = portbuf;
			keywords[n] = "dbname";
			values[n++] = gang_dbname;
			keywords[n] = "user";
			values[n++] = gang_username;
			keywords[n] = "application_name";
			values[n++] = "cloudberry dispatcher";
			n = GpInternalConnOptions(keywords, values, n);

			conn = libpqsrv_connect_params(keywords, values, false,
										   dispatch_wait_event());
			if (conn != NULL && PQstatus(conn) == CONNECTION_OK)
			{
				PGresult   *res = libpqsrv_exec(conn, sql, dispatch_wait_event());
				const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
				bool		gone = state != NULL && strcmp(state, "42704") == 0;

				/* done, or done already: by its backend, or the recovery process */
				done = PQresultStatus(res) == PGRES_COMMAND_OK || gone;
				PQclear(res);
				if (gone && commit)
					PQclear(libpqsrv_exec(conn, "SELECT gp_internal.dtx_wait_mirror()",
										  dispatch_wait_event()));
			}
			if (conn != NULL)
				libpqsrv_disconnect(conn);
			if (!done)
			{
				(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 500, dispatch_wait_event());
				ResetLatch(MyLatch);
			}
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		done = false;
	}
	PG_END_TRY();

	ereport(done ? LOG : WARNING,
			(errmsg("%s on segment %d, again, on its primary (%s:%d): %s", sql,
					content, node->hostname, node->port,
					done ? "done" : "not done in time")));
	return done;
}

/*
 * The second phase, or the abort, of the parts asked to prepare, where the
 * gang has gone: each over a connection of its own to its content's primary,
 * which dtx_finish_again() waits for while it restarts, as Cloudberry's
 * coordinator retries its broadcast over new gangs until it is done
 * (retryAbortPrepared(), doNotifyingCommitPrepared(), cdbtm.c).  So a part
 * prepared where a segment went down ends as the statement does, and what
 * its segment's end of it does with it (gp_dirxact.c), where the recovery
 * process would end it later.  How many it did not reach.
 */
static int
dtx_finish_apart(bool commit)
{
	char	   *sql = psprintf("%s PREPARED '%s'", commit ? "COMMIT" : "ROLLBACK",
							   dtx_gid);
	int			nfailed = 0;

	for (int i = 0; i < dtx_napart; i++)
	{
		if (!dtx_finish_again(dtx_apart[i], sql, commit))
			nfailed++;
	}
	pfree(sql);
	return nfailed;
}

/*
 * COMMIT PREPARED or ROLLBACK PREPARED on the segments asked to prepare, and
 * how many of them it did not reach -- without raising: the second phase
 * and the abort are both past it.  An answer that the part does not exist,
 * or is busy, is no failure: the recovery process finished it or is
 * finishing it, and so did a PREPARE that failed.  A segment that does not
 * answer within a while costs the gang; one whose connection broke, or whose
 * primary FTS failed over from, is told again over a connection of its own
 * (dtx_finish_again()).
 */
static int
gang_finish_prepared(bool commit)
{
	GpGang	   *g = gang;
	char	   *sql;
	int			nfailed = 0;
	TimestampTz deadline;
	bool	   *again;
	bool	   *refused;
	char	  **then_raise;
	int		   *conn_content;
	int			nconns;
	int			nrefused = 0;
	GpDtxCommand command = commit ? GP_DTX_COMMIT_PREPARED :
		dtx_all_prepared ? GP_DTX_ABORT_PREPARED : GP_DTX_ABORT_SOME_PREPARED;

	if (dtx_nprepared == 0)
		return 0;
	if (g == NULL)
		return dtx_finish_apart(commit);

	/* kept apart from the gang, which a broken connection closes */
	nconns = g->nconns;
	conn_content = palloc_array(int, nconns);
	for (int i = 0; i < nconns; i++)
		conn_content[i] = g->conns[i].content;

	/*
	 * A rollback is named, as Cloudberry names it, by how far the first phase
	 * got (rollbackDtxTransaction(), cdb/cdbtm.c).
	 */
	if (gp_test_print_direct_dispatch_info)
	{
		int		   *contents = palloc_array(int, g->nconns);
		int			n = 0;

		for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
			if (dtx_prepared[i])
				contents[n++] = g->conns[i].content;
		dtx_report(commit ? "Distributed Commit Prepared" :
				   dtx_all_prepared ? "Distributed Abort Prepared" :
				   "Distributed Abort (Some Prepared)",
				   contents, n, false);
		pfree(contents);
	}

	sql = psprintf("%s PREPARED '%s'", commit ? "COMMIT" : "ROLLBACK", dtx_gid);
	again = palloc0_array(bool, g->nconns);
	refused = palloc0_array(bool, g->nconns);
	then_raise = palloc0_array(char *, g->nconns);
	for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
	{
		GpSegmentConn *c = &g->conns[i];
		const char *send = sql;
		char	   *msg;
		int			action;

		if (!dtx_prepared[i])
			continue;
		c->fetching = NULL;

		/* where debug_dtm_action asks for a part's failure (gp_dtm_debug.c) */
		action = GpDtmDebugProtocol(command, c->content, 0, &msg);
		if (action == GP_DTM_ACTION_FAIL_BEGIN ||
			action == GP_DTM_ACTION_PANIC_BEGIN)
			send = GpDtmDebugRaiseStatement(action, msg);
		else if (action == GP_DTM_ACTION_FAIL_END)
			then_raise[i] = GpDtmDebugRaiseStatement(action, msg);

		/* on a primary FTS failed over from, or not to be sent to */
		if (!GpClusterIsPrimaryNow(c->seg->dbid) ||
			c->busy || !PQsendQuery(c->conn, send))
		{
			again[i] = true;
			dtx_prepared[i] = false;
			continue;
		}
		c->busy = true;
	}

	deadline = GetCurrentTimestamp() + 30 * USECS_PER_SEC;
	for (;;)
	{
		bool		waiting = false;
		bool		broken = false;
		WaitEvent	occurred[1];

		for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
		{
			GpSegmentConn *c = &g->conns[i];

			if (!dtx_prepared[i] || !c->busy)
				continue;
			/* its backend ended, perhaps as a commit waited for the mirror */
			if (PQconsumeInput(c->conn) == 0)
			{
				again[i] = true;
				c->busy = false;
				broken = true;
				continue;
			}
			while (!PQisBusy(c->conn))
			{
				PGresult   *res = PQgetResult(c->conn);
				const char *state;

				if (res == NULL)
				{
					/* then the error a fail_end_command asks for */
					if (then_raise[i] != NULL && PQsendQuery(c->conn, then_raise[i]))
					{
						then_raise[i] = NULL;
						continue;
					}
					c->busy = false;
					break;
				}
				state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
				if (PQresultStatus(res) != PGRES_COMMAND_OK &&
					PQresultStatus(res) != PGRES_TUPLES_OK &&
					(state == NULL ||
					 (strcmp(state, "42704") != 0 && strcmp(state, "55000") != 0)))
				{
					refused[i] = true;
					ereport(LOG,
							(errmsg("%s on segment %d failed: %s", sql, c->content,
									PQresultErrorMessage(res))));
				}
				PQclear(res);
			}
			/* failed over from while it was being told */
			if (c->busy && !GpClusterIsPrimaryNow(c->seg->dbid))
			{
				again[i] = true;
				c->busy = false;
				broken = true;
			}
			if (c->busy)
				waiting = true;
		}
		if (broken)
		{
			gang_close();
			break;
		}
		if (!waiting)
			break;
		if (GetCurrentTimestamp() >= deadline)
		{
			for (int i = 0; i < g->nconns && i < dtx_prepared_size; i++)
				if (dtx_prepared[i] && g->conns[i].busy)
					nfailed++;
			gang_close();
			break;
		}
		if (WaitEventSetWait(g->wes, 1000, occurred, 1, dispatch_wait_event()) > 0 &&
			(occurred[0].events & WL_LATCH_SET))
			ResetLatch(MyLatch);
	}

	/*
	 * A part that answered with an error is told again over a connection of
	 * its own, as Cloudberry's coordinator retries a broadcast that failed
	 * over a new gang, saying so (doNotifyingCommitPrepared(),
	 * doNotifyingAbort(), cdbtm.c); and the gang goes, as Cloudberry's do.
	 */
	for (int i = 0; i < nconns; i++)
		if (refused[i])
		{
			nrefused++;
			again[i] = true;
		}
	if (nrefused > 0)
	{
		if (commit)
		{
			ereport(WARNING,
					(errmsg("the distributed transaction 'Commit Prepared' broadcast failed to one or more segments. Retrying ... try %d", 1),
					 errdetail_internal("%s", GpDtmDebugGidDetail(dtx_gid, "Retry Commit Prepared"))));
			ereport(NOTICE,
					(errmsg("Releasing segworker group to retry broadcast.")));
		}
		else
			ereport(WARNING,
					(errmsg("the distributed transaction broadcast failed to one or more segments"),
					 errdetail_internal("%s", GpDtmDebugGidDetail(dtx_gid,
																  dtx_all_prepared ? "Notifying Abort Prepared" :
																  "Notifying Abort (Some Prepared)"))));
		gang_release_for_retry();
	}

	for (int i = 0; i < nconns; i++)
		if (again[i] && !dtx_finish_again(conn_content[i], sql, commit))
			nfailed++;
	return nfailed;
}

/*
 * The second phase, at COMMIT: the coordinator's commit record is on disk,
 * and nothing may be raised.  What a segment was not told, the recovery
 * process commits by the same record; this says so, and wakes it.
 */
static void
gang_commit_second_phase(void)
{
	int			nfailed;

	PG_TRY();
	{
		GP_FAULT("dtm_broadcast_commit_prepared");
		notices_quiet++;
		nfailed = gang_finish_prepared(true);
		notices_quiet--;

		/*
		 * The segments are told; what is left is to forget the transaction,
		 * where Cloudberry writes its FORGET record (cdbtm.c).
		 */
		GP_FAULT("dtm_before_insert_forget_comitted");
	}
	PG_CATCH();
	{
		FlushErrorState();
		gang_close();
		nfailed = Max(dtx_nprepared, 1);
	}
	PG_END_TRY();

	if (nfailed > 0)
	{
		ereport(WARNING,
				(errmsg("the distributed transaction \"%s\" was committed, but %d of its segments have not been told yet",
						dtx_gid, nfailed),
				 errdetail("Distributed transaction recovery commits their parts.")));
		GpDtxWakeRecovery();
	}
	dtx_forget();
}

/*
 * ROLLBACK TO SAVEPOINT in a transaction whose part on the segments went with
 * a gang that closed: the segments' part cannot be rolled back to the
 * savepoint, so the statement fails, as Cloudberry's fails where it cannot
 * send the segments its rollback (DispatchRollbackToSavepoint(), xact.c) --
 * and the transaction, aborted, ends in a rollback, rather than going on
 * here to fail as it commits.
 */
void
GpDispatchCheckRollbackTo(const char *savepoint)
{
	Oid			temp_ns;
	Oid			temp_toast_ns;

	if (!gang_xact_lost || savepoint == NULL)
		return;

	/*
	 * Cloudberry's warnings before its error: its dispatch of the command
	 * finds the writer gang lost (dispatchDtxCommand(), cdbtm.c), and the
	 * session's temporary tables, which went with it, are dropped here too,
	 * as the next statement begins (resetSessionForPrimaryGangLoss(),
	 * cdbgang.c).
	 */
	ereport(WARNING,
			(errmsg("writer gang of current global transaction is lost")));
	GetTempNamespaceState(&temp_ns, &temp_toast_ns);
	if (OidIsValid(temp_ns))
	{
		ereport(WARNING,
				(errmsg("Any temporary tables for this session have been dropped because the gang was disconnected (session id = %d)",
						GpClusterSessionId())));
		temp_tables_lost = true;
	}
	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("Could not rollback to savepoint (ROLLBACK TO SAVEPOINT %s)",
					quote_identifier(savepoint))));
}

/*
 * O33: the second phase, once the coordinator's commit is recorded and
 * before its transaction ends for the other sessions -- as Cloudberry's
 * coordinator notifies the segments before it ends its own
 * (notifyCommittedDtxTransaction(), before ProcArrayEndTransaction() in its
 * xact.c).  So a session whose snapshot sees the transaction committed finds
 * its parts committed on the segments too, rather than waiting there for a
 * second phase on its way -- or held, as a test holds it
 * (dtm_broadcast_commit_prepared).  XACT_EVENT_COMMIT comes only after the
 * transaction has ended for the others.
 */
static xact_commit_recorded_hook_type prev_commit_recorded_hook = NULL;

static void
dispatch_commit_recorded(TransactionId latestXid)
{
	if (prev_commit_recorded_hook)
		prev_commit_recorded_hook(latestXid);
	if (dtx_nprepared > 0)
	{
		/*
		 * Cloudberry's fault once its distributed commit is recorded and
		 * its commit's critical section is over, before the second phase
		 * (RecordTransactionCommit(), xact.c): a test loops a coordinator
		 * here while a checkpoint it held goes on and fails the node
		 * (checkpoint_dtx_info).
		 */
		GP_FAULT("after_xlog_xact_distributed_commit");
		gang_commit_second_phase();
	}

	/*
	 * Cloudberry's fault as a transaction ends for the other sessions
	 * (ProcArrayEndTransaction(), in its procarray.c), after its parts
	 * committed: a test holds a coordinator here, its transaction still in
	 * progress for every snapshot, while a transaction that waited for its
	 * row on a segment goes on there (gdd/concurrent_update).  Only a suspend
	 * or a sleep belongs here: the transaction is committed, and an error
	 * would be a PANIC.  In the connection's database, as Cloudberry names
	 * it, with no catalog read after the commit.
	 */
	if (gp_fault_active != NULL && *gp_fault_active > 0)
		(void) GpFaultTrigger("before_xact_end_procarray",
							  MyProcPort != NULL ? MyProcPort->database_name : "",
							  "");
}

/*
 * Cloudberry's fault at the start of an abort, whose error Cloudberry's
 * client reads after the one that caused the abort, its abort failing too
 * (AbortTransaction(), xact.c): said to the client here, the abort going on,
 * as nothing may stop it.
 */
static void
abort_failure_fault(void)
{
	MemoryContext cxt = CurrentMemoryContext;
	uint32		holdoff = InterruptHoldoffCount;

	PG_TRY();
	{
		(void) GP_FAULT("transaction_abort_failure");
	}
	PG_CATCH();
	{
		InterruptHoldoffCount = holdoff;
		MemoryContextSwitchTo(cxt);
		EmitErrorReport();
		FlushErrorState();
	}
	PG_END_TRY();
}

static void
dispatch_xact_callback(XactEvent event, void *arg)
{
	uint32		holdoff = InterruptHoldoffCount;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			GpDispatchRaiseKeptError();
			if (gang_xact_lost)
			{
				gang_xact_lost = false;
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("lost the segments' part of this transaction"),
						 errdetail("A connection to a segment closed while the transaction was open.")));
			}

			/*
			 * Labels no statement has carried to the segments yet go now,
			 * in their transaction -- opening it, if nothing else did.
			 */
			if (labels_pending != NIL)
				gang_prepare(gang_get(), true);

			/*
			 * A part on a primary FTS has failed over from is not to be
			 * prepared there: the node is no part of the cluster now, and
			 * its commits wait for a mirror that has left it.
			 */
			if (gang != NULL && gang_in_xact && gang_lost_primary(gang))
			{
				gang_close();
				gang_xact_lost = false;
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("gang was lost due to cluster reconfiguration")));
			}

			/*
			 * Before the coordinator commits: raising here still undoes the
			 * coordinator's part.  After it, nothing could, and the second
			 * phase follows at COMMIT.
			 */
			if (gang != NULL && gang_in_xact)
				gang_commit_first_phase(gang);
			break;

		case XACT_EVENT_PRE_PREPARE:
			if (gang_in_xact)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has used the segments"),
						 errdetail("A distributed transaction is prepared on the segments by the coordinator, which commits it.")));
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:

			/*
			 * An error is already being handled here, so nothing may be
			 * raised; libpqsrv_cancel() can raise on an out-of-memory, so it
			 * is caught, and the gang dropped instead.
			 */
			PG_TRY();
			{
				/*
				 * Cloudberry's fault at the start of an abort, before the
				 * segments are told anything (AbortTransaction(), xact.c): a
				 * test holds a coordinator here whose parts are prepared,
				 * and has its standby finish them (dtm_recovery_on_standby).
				 * The abort record is written already, where Cloudberry's is
				 * not; either way the transaction did not commit.
				 */
				abort_failure_fault();

				gang_cancel_and_drain();
				if (gang != NULL && gang_in_xact)
				{
					/*
					 * Nothing prepared: the rollback of every segment the
					 * transaction reached, and of each part that wrote, as
					 * Cloudberry names it (rollbackDtxTransaction(),
					 * cdbtm.c).
					 */
					if (gp_test_print_direct_dispatch_info)
					{
						int		   *contents = palloc_array(int, gang->nconns);
						int			n = 0;

						for (int i = 0; i < gang->nconns; i++)
							if (conn_wrote(&gang->conns[i]))
								contents[n++] = gang->conns[i].content;
						dtx_report("Distributed Abort (No Prepared)",
								   contents, n, true);
						pfree(contents);
					}
					gang_send_all_quietly("ROLLBACK");
				}

				/*
				 * The first phase failed: what it prepared is rolled back,
				 * or left to the recovery process, which rolls it back too
				 * -- the coordinator's transaction did not commit.
				 */
				if (dtx_nprepared > 0)
				{
					GP_FAULT("dtm_broadcast_abort_prepared");
					if (gang_finish_prepared(false) > 0)
						GpDtxWakeRecovery();
				}
			}
			PG_CATCH();
			{
				/*
				 * errfinish() let interrupts through for the handler of the
				 * error; the abort holds them, and expects them held still.
				 */
				InterruptHoldoffCount = holdoff;
				FlushErrorState();
				gang_close();
				if (dtx_nprepared > 0)
					GpDtxWakeRecovery();
			}
			PG_END_TRY();
			lost_gang_warn();

			gang_in_xact = false;
			gang_xact_depth = 0;
			gang_xact_lost = false;
			temp_tables_dropped = false;
			forget_kept_errors();
			gang_forget_settings();
			gang_forget_snapshot();
			dtx_forget();
			GpReportDtxForget();
			streams_release();
			labels_pending = NIL;
			drop_segment_notices();
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			if (temp_tables_dropped)
				temp_tables_lost = false;
			temp_tables_dropped = false;
			forget_kept_errors();
			/* sent at PRE_COMMIT; the memory goes with the transaction */
			labels_pending = NIL;
			gang_forget_snapshot();
			/* the second phase is done already (dispatch_commit_recorded()) */
			Assert(dtx_nprepared == 0);
			GpReportDtxForget();
			break;

		case XACT_EVENT_PREPARE:
			GpReportDtxForget();
			break;

		default:
			break;
	}
}

/*
 * A subtransaction's rollback that a segment failed, while debug_dtm_action
 * is armed.  Cloudberry's coordinator sends a subtransaction's rollback after
 * its own, and raises a segment's failure of it there: the error escapes the
 * block's cleanup before the block's handler runs, and each block around it
 * that rolls back fails as well, on the segment that went on failing
 * (RollbackAndReleaseCurrentSubTransaction(), xact.c).  The coordinator's
 * abort may raise nothing, so the segments' errors are kept, those of the
 * first rollback that failed, and raised at the next point that may
 * (GpDispatchRaiseKeptError()): the next statement of a function -- the
 * handler's first -- a release, the commit, the next statement dispatched.
 * A segment that has left the level already, its savepoint released,
 * answers the rollback with an error of its own, where Cloudberry's says the
 * level was "already processed".
 */
static List *kept_errors = NIL;
static MemoryContext kept_context = NULL;

static void
keep_errors(List *errors)
{
	MemoryContext oldcxt;

	if (kept_context == NULL)
		kept_context = AllocSetContextCreate(TopMemoryContext,
											 "gp_dispatch kept errors",
											 ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(kept_context);	/* the last, raised already */
	oldcxt = MemoryContextSwitchTo(kept_context);
	foreach_ptr(GpSegmentError, err, errors)
	{
		GpSegmentError *copy = palloc_object(GpSegmentError);

		*copy = *err;
		copy->sqlstate = err->sqlstate ? pstrdup(err->sqlstate) : NULL;
		copy->message = pstrdup(err->message);
		copy->detail = err->detail ? pstrdup(err->detail) : NULL;
		copy->hint = err->hint ? pstrdup(err->hint) : NULL;
		copy->context = err->context ? pstrdup(err->context) : NULL;
		for (int i = 0; i < lengthof(error_names); i++)
			copy->names[i] = err->names[i] ? pstrdup(err->names[i]) : NULL;
		/* file and func are location_name()'s, kept for good */
		kept_errors = lappend(kept_errors, copy);
	}
	MemoryContextSwitchTo(oldcxt);
}

void
GpDispatchRaiseKeptError(void)
{
	List	   *errors = kept_errors;

	if (errors == NIL)
		return;
	/* the memory stays until the transaction ends, or the next is kept */
	kept_errors = NIL;
	raise_segment_errors(errors);
}

static void
forget_kept_errors(void)
{
	kept_errors = NIL;
	if (kept_context != NULL)
		MemoryContextReset(kept_context);
}

static void
gang_rollback_keeping_error(int level)
{
	char	   *sql = psprintf("ROLLBACK TO SAVEPOINT gp_sp_%d; RELEASE SAVEPOINT gp_sp_%d",
							   level, level);
	bool		user = GpDtmDebugLevelIsUser(level);
	char	  **instead = palloc0_array(char *, gang->nconns);
	char	  **then_raise = palloc0_array(char *, gang->nconns);
	List	   *errors = NIL;
	bool		more = false;

	for (int i = 0; i < gang->nconns; i++)
	{
		GpSegmentConn *c = &gang->conns[i];
		char	   *msg;
		int			action = user ?
			GpDtmDebugSql("ROLLBACK", c->content, &msg) :
			GpDtmDebugProtocol(GP_DTX_SUBTRANSACTION_ROLLBACK, c->content,
							   level - 1, &msg);
		const char *send = sql;

		if (action == GP_DTM_ACTION_FAIL_BEGIN ||
			action == GP_DTM_ACTION_PANIC_BEGIN)
		{
			send = GpDtmDebugRaiseStatement(action, msg);
			instead[i] = msg;
		}
		else if (action == GP_DTM_ACTION_FAIL_END)
			then_raise[i] = GpDtmDebugRaiseStatement(action, msg);
		if (!PQsendQuery(c->conn, send))
		{
			gang_close();
			return;
		}
		c->busy = true;
	}
	gang_drain_keeping(&errors);

	/*
	 * A segment whose part has failed already runs nothing but a rollback:
	 * it answers what it was sent to raise instead with 25P02, where
	 * Cloudberry's raises that error before it looks at its transaction.
	 */
	foreach_ptr(GpSegmentError, err, errors)
	{
		for (int i = 0; gang != NULL && i < gang->nconns; i++)
			if (gang->conns[i].content == err->content && instead[i] != NULL &&
				err->sqlstate != NULL && strcmp(err->sqlstate, "25P02") == 0)
			{
				err->sqlstate = pstrdup("XX009");
				err->message = instead[i];
			}
	}

	/* then the errors a fail_end_command asks for, where the rollback was done */
	for (int i = 0; gang != NULL && i < gang->nconns; i++)
	{
		bool		failed = false;

		foreach_ptr(GpSegmentError, err, errors)
			if (err->content == gang->conns[i].content)
				failed = true;
		if (then_raise[i] == NULL || failed)
			continue;
		if (!PQsendQuery(gang->conns[i].conn, then_raise[i]))
		{
			gang_close();
			return;
		}
		gang->conns[i].busy = true;
		more = true;
	}
	if (more)
		gang_drain_keeping(&errors);

	if (errors != NIL && kept_errors == NIL)
		keep_errors(errors);
}

/*
 * A function's block with an exception handler, as it begins: its
 * subtransaction sent to the segments now, and the levels not sent yet
 * before it, as Cloudberry sends "Begin Internal Subtransaction" before it
 * begins one (BeginInternalSubTransaction(), xact.c), where the port sends
 * it with the block's first statement that goes there -- while
 * debug_dtm_action asks for a subtransaction's failure (gp_dtm_debug.c's
 * PL/pgSQL plugin).
 */
void
GpDispatchSubtransactionBeginNow(void)
{
	GpGang	   *g;

	if (GpClusterIsSingleNode() || GpClusterBackendRole() != GP_ROLE_DISPATCH ||
		!IsTransactionState())
		return;
	g = gang_get();
	gang_prepare(g, true);
	if (gang_xact_depth == GetCurrentTransactionNestLevel())
		gang_send_savepoint(g, false);
}

static void
dispatch_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	uint32		holdoff = InterruptHoldoffCount;

	if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
		GpDispatchRaiseKeptError();
	if (gang == NULL || !gang_in_xact || gang_xact_depth < level)
		return;

	switch (event)
	{
		case SUBXACT_EVENT_PRE_COMMIT_SUB:
			notices_quiet++;
			if (GpDtmDebugLevelIsUser(level))
				gang_send_all_dtm(gang, psprintf("RELEASE SAVEPOINT gp_sp_%d", level),
								  GP_DTX_NONE, "RELEASE", 0);
			else
				gang_send_all_dtm(gang, psprintf("RELEASE SAVEPOINT gp_sp_%d", level),
								  GP_DTX_SUBTRANSACTION_RELEASE, NULL, level);
			gang_wait_all(gang, NULL, false);
			gang_dtm_end(gang);
			notices_quiet--;
			gang_xact_depth = level - 1;
			break;

		case SUBXACT_EVENT_ABORT_SUB:
			PG_TRY();
			{
				gang_cancel_and_drain();
				if (gang != NULL && GpDtmDebugArmed())
					gang_rollback_keeping_error(level);
				else if (gang != NULL)
					gang_send_all_quietly(psprintf("ROLLBACK TO SAVEPOINT gp_sp_%d; RELEASE SAVEPOINT gp_sp_%d",
												   level, level));
			}
			PG_CATCH();
			{
				/* as at the transaction's abort */
				InterruptHoldoffCount = holdoff;
				FlushErrorState();
				gang_close();
			}
			PG_END_TRY();
			gang_xact_depth = level - 1;
			gang_forget_settings();
			gang_forget_snapshot();
			streams_release();
			drop_segment_notices();
			break;

		default:
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* Statements                                                                */
/* ------------------------------------------------------------------------- */

void
GpDispatchCommand(const char *sql)
{
	GpGang	   *g = gang_get();

	gang_prepare(g, true);
	gang_send_all(g, sql);
	gang_wait_all(g, NULL, false);
}

void
GpDispatchCommandOnContent(int content, const char *sql)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];

	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	gang_prepare(g, true);
	conn_send(c, sql);
	gang_wait_all(g, NULL, false);
}

void
GpDispatchUtility(const char *payload, bool own_xact, bool relay_notices)
{
	GpGang	   *g = gang_get();

	/*
	 * A label the statement wrote may be of an object the statement makes --
	 * an extension's script labels its tables -- which the segments have
	 * only once they have run it.  So the labels follow it.
	 */
	labels_held = true;
	PG_TRY();
	{
		gang_prepare(g, !own_xact);
	}
	PG_FINALLY();
	{
		labels_held = false;
	}
	PG_END_TRY();
	/* as Cloudberry's DDL, sent in two phases to every segment */
	if (!own_xact)
		GpReportDtxReached(NULL, NULL, 0);
	/*
	 * the coordinator has said what the statement says, once -- but of an
	 * index build, whose NOTICEs are the segments' (relay_notices)
	 */
	notices_quiet++;
	if (relay_notices)
		notices_relayed = notices_quiet;
	gang_send_all_dtm(g, payload, GP_DTX_NONE, "MPPEXEC UTILITY", 0);
	gang_wait_all(g, NULL, false);
	forget_relayed_notices();
	gang_dtm_end(g);
	notices_quiet--;
	if (!own_xact)
		gang_sync_labels(g);
}

/*
 * A statement with parameters on every segment, the first nsegments of them,
 * or one, and how many rows each changed.  The parameters travel as text, of
 * the types given; "counts" gets one entry per segment asked, in content
 * order.
 */
void
GpDispatchCommandParams(const char *sql, int nparams, const Oid *types,
						const char *const *values, int content, int nsegments,
						uint64 *counts)
{
	GpGang	   *g = gang_get();
	PGresult  **results;
	int			n = 0;

	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (!conn_asked(c, content, nsegments))
			continue;
		conn_send_params(c, sql, nparams, types, values, NULL, NULL);
	}

	/* The counts come back as command tags, which "keep" does not keep. */
	gang_wait_all_counting(g, counts, content, nsegments);
	pfree(results);
	(void) n;
}

/*
 * The same, on the segments "contents" lists -- direct dispatch's -- and how
 * many rows each changed, in the list's order.
 */
void
GpDispatchCommandParamsOnContents(const char *sql, int nparams,
								  const Oid *types, const char *const *values,
								  const int *contents, int ncontents,
								  uint64 *counts)
{
	GpGang	   *g = gang_get();
	PGresult  **results;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (!conn_listed(c, contents, ncontents))
			continue;
		conn_send_params(c, sql, nparams, types, values, NULL, NULL);
	}

	/* The counts come back as command tags, which "keep" does not keep. */
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	gang_wait_all_keeping_commands(g, results);
	for (int k = 0; k < ncontents; k++)
	{
		counts[k] = 0;
		for (int i = 0; i < g->nconns; i++)
			if (g->conns[i].content == contents[k] && results[i] != NULL)
				counts[k] = strtou64(PQcmdTuples(results[i]), NULL, 10);
	}
	for (int i = 0; i < g->nconns; i++)
		if (results[i] != NULL)
			PQclear(results[i]);
	pfree(results);
}

/*
 * A statement with parameters, some of them binary, on one segment, waited
 * for.  What a Motion's batches of rows travel in: bytea sent as it is,
 * rather than as the hex text of it.
 */
void
GpDispatchParamsOnContent(int content, const char *sql, int nparams,
						  const char *const *values, const int *lengths,
						  const int *formats)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	conn_send_params(c, sql, nparams, NULL, values, lengths, formats);
	gang_wait_all(g, NULL, false);
}

/*
 * A write with parameters, as text, on one segment, and what it said: how
 * many rows it changed and, into "store" when it is not NULL, the rows its
 * RETURNING gave, read by "tupdesc"'s input functions.
 */
uint64
GpDispatchWriteOnContent(int content, const char *sql, int nparams,
						 const char *const *values, TupleDesc tupdesc,
						 Tuplestorestate *store)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;
	PGresult  **results;
	PGresult   *res;
	uint64		count = 0;
	int			idx = -1;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
		{
			c = &g->conns[i];
			idx = i;
		}
	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));

	conn_send_params(c, sql, nparams, NULL, values, NULL, NULL);

	results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	gang_wait_all_keeping_commands(g, results);
	res = results[idx];

	if (res != NULL)
	{
		const char *tuples = PQcmdTuples(res);

		if (tuples[0] != '\0')
			count = strtou64(tuples, NULL, 10);

		if (store != NULL && PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			int			natts = tupdesc->natts;
			FmgrInfo   *in = palloc_array(FmgrInfo, natts);
			Oid		   *ioparams = palloc_array(Oid, natts);
			Datum	   *datums = palloc_array(Datum, natts);
			bool	   *nulls = palloc_array(bool, natts);

			if (PQnfields(res) != natts)
				elog(ERROR, "segment %d returned %d columns, not %d",
					 content, PQnfields(res), natts);
			for (int j = 0; j < natts; j++)
			{
				Oid			func;

				getTypeInputInfo(GpTransferType(TupleDescAttr(tupdesc, j)->atttypid), &func,
								 &ioparams[j]);
				fmgr_info(func, &in[j]);
			}
			for (int r = 0; r < PQntuples(res); r++)
			{
				for (int j = 0; j < natts; j++)
				{
					nulls[j] = PQgetisnull(res, r, j);
					datums[j] = InputFunctionCall(&in[j],
												  nulls[j] ? NULL : PQgetvalue(res, r, j),
												  ioparams[j],
												  TupleDescAttr(tupdesc, j)->atttypmod);
				}
				tuplestore_putvalues(store, tupdesc, datums, nulls);
			}
		}
	}

	for (int i = 0; i < g->nconns; i++)
		if (results[i] != NULL)
			PQclear(results[i]);
	pfree(results);
	return count;
}

/*
 * A write whose RETURNING gives rows -- ORCA's plan of one, a ModifyTable
 * each segment runs (gp_motion.c) -- on the segments asked, all at once, and
 * what each said: how many rows it changed, and the rows it returned, read
 * by "tupdesc"'s input functions, each segment's in turn.  Every segment of a
 * replicated table returns the same rows, and "one_segment" keeps the first's.
 */
void
GpDispatchWriteReturning(const char *sql, int content, const int *contents,
						 int ncontents, TupleDesc tupdesc,
						 Tuplestorestate *store, bool one_segment,
						 uint64 *counts)
{
	GpGang	   *g = gang_get();
	PGresult  **results;
	int			natts = tupdesc->natts;
	FmgrInfo   *in = palloc_array(FmgrInfo, Max(natts, 1));
	Oid		   *ioparams = palloc_array(Oid, Max(natts, 1));
	Datum	   *datums = palloc_array(Datum, Max(natts, 1));
	bool	   *nulls = palloc_array(bool, Max(natts, 1));
	MemoryContext rowcxt;
	int			n = 0;
	bool		kept = false;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
	{
		GpSegmentConn *c = &g->conns[i];

		if (contents != NULL ? !conn_listed(c, contents, ncontents)
			: !conn_asked(c, content, 0))
			continue;
		conn_send_params(c, sql, 0, NULL, NULL, NULL, NULL);
	}

	results = (PGresult **) palloc0_array(PGresult *, g->nconns);
	gang_wait_all_keeping_commands(g, results);

	/* the counts come back as command tags, in the order they were asked */
	if (contents != NULL)
	{
		for (int k = 0; k < ncontents; k++)
		{
			counts[k] = 0;
			for (int i = 0; i < g->nconns; i++)
				if (g->conns[i].content == contents[k] && results[i] != NULL)
					counts[k] = strtou64(PQcmdTuples(results[i]), NULL, 10);
		}
	}
	else
	{
		for (int i = 0; i < g->nconns; i++)
			if (conn_asked(&g->conns[i], content, 0))
				counts[n++] = results[i] ? strtou64(PQcmdTuples(results[i]), NULL, 10) : 0;
	}

	for (int j = 0; j < natts; j++)
	{
		Oid			func;

		getTypeInputInfo(GpTransferType(TupleDescAttr(tupdesc, j)->atttypid),
						 &func, &ioparams[j]);
		fmgr_info(func, &in[j]);
	}

	/* each row's values live until it is in the store */
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "returned row",
								   ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < g->nconns; i++)
	{
		PGresult   *res = results[i];

		if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK ||
			(one_segment && kept))
			continue;
		if (PQnfields(res) != natts)
			elog(ERROR, "segment %d returned %d columns, not %d",
				 g->conns[i].content, PQnfields(res), natts);
		kept = true;

		for (int r = 0; r < PQntuples(res); r++)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(rowcxt);

			for (int j = 0; j < natts; j++)
			{
				nulls[j] = PQgetisnull(res, r, j);
				datums[j] = InputFunctionCall(&in[j],
											  nulls[j] ? NULL : PQgetvalue(res, r, j),
											  ioparams[j],
											  TupleDescAttr(tupdesc, j)->atttypmod);
			}
			MemoryContextSwitchTo(oldcxt);
			tuplestore_putvalues(store, tupdesc, datums, nulls);
			MemoryContextReset(rowcxt);
		}
	}
	MemoryContextDelete(rowcxt);

	for (int i = 0; i < g->nconns; i++)
		if (results[i] != NULL)
			PQclear(results[i]);
	pfree(results);
}

/*
 * A relation's name in SQL a segment is sent.  A temporary relation is in
 * this session's temporary schema, whose name -- pg_temp_N -- is the
 * coordinator's backend's; the segment backend's own is another number, and
 * "pg_temp" names whichever is the session's own, on either.
 */
char *
GpDispatchRelationName(Oid relid)
{
	Oid			nsp = get_rel_namespace(relid);

	if (isAnyTempNamespace(nsp))
		return psprintf("pg_temp.%s", quote_identifier(get_rel_name(relid)));
	return quote_qualified_identifier(get_namespace_name(nsp),
									  get_rel_name(relid));
}

/* ------------------------------------------------------------------------- */
/* Rows on the way out                                                       */
/* ------------------------------------------------------------------------- */

/*
 * COPY ... FROM STDIN on one segment: the way rows the coordinator routed
 * reach it.  One segment at a time, which is what a connection in COPY mode
 * allows, and why the rows are held on the coordinator until the statement
 * that produced them has finished with the gang.
 */
void
GpCopyInBegin(int content, const char *sql, GpCopyInContext context, void *arg)
{
	GpGang	   *g = gang_get();
	GpSegmentConn *c = NULL;
	List	   *errors = NIL;

	gang_prepare(g, true);

	for (int i = 0; i < g->nconns; i++)
		if (g->conns[i].content == content)
			c = &g->conns[i];
	if (c == NULL)
		elog(ERROR, "there is no segment with content id %d", content);

	conn_send(c, sql);

	for (;;)
	{
		PGresult   *res;

		if (PQconsumeInput(c->conn) == 0)
		{
			collect_error(&errors, c->content, NULL, c->conn, NULL);
			gang_close();
			raise_segment_errors(errors);
		}
		if (PQisBusy(c->conn))
		{
			gang_wait(g);
			continue;
		}
		res = PQgetResult(c->conn);
		if (res != NULL && PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			break;
		}

		/* The statement failed before it began to read. */
		collect_error(&errors, c->content, res, c->conn, NULL);
		if (res != NULL)
			PQclear(res);
		while ((res = PQgetResult(c->conn)) != NULL)
			PQclear(res);
		c->busy = false;
		raise_segment_errors(errors);
	}

	copying = c;
	copy_context = context;
	copy_context_arg = arg;
}

void
GpCopyInData(const char *data, int len)
{
	Assert(copying != NULL);

	if (PQputCopyData(copying->conn, data, len) != 1)
	{
		char	   *msg = pstrdup(PQerrorMessage(copying->conn));
		int			content = copying->content;

		copying = NULL;
		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send rows to segment %d", content),
				 errdetail_internal("%s", msg)));
	}
}

/* Wait for the COPY's result, its error's context given to its caller's. */
static void
copy_in_wait(int content, uint64 *count)
{
	copy_ending = true;
	PG_TRY();
	{
		gang_wait_all_counting(gang, count, content, 0);
	}
	PG_FINALLY();
	{
		copy_ending = false;
		copy_context = NULL;
		copy_context_arg = NULL;
	}
	PG_END_TRY();
}

uint64
GpCopyInEnd(void)
{
	GpSegmentConn *c = copying;
	uint64		count = 0;

	Assert(c != NULL);
	copying = NULL;

	/*
	 * Cloudberry's, as it begins to end a COPY to the segments
	 * (cdbCopyEndInternal(), cdbcopy.c): here as each segment's COPY ends.
	 */
	(void) GP_FAULT("cdb_copy_end_internal_start");

	if (PQputCopyEnd(c->conn, NULL) != 1)
	{
		char	   *msg = pstrdup(PQerrorMessage(c->conn));
		int			content = c->content;

		gang_close();
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not finish sending rows to segment %d", content),
				 errdetail_internal("%s", msg)));
	}

	/* The COPY's own result: its row count, or why it failed. */
	copy_in_wait(c->content, &count);
	return count;
}

/* ------------------------------------------------------------------------- */
/* Rows on the way back                                                      */
/* ------------------------------------------------------------------------- */

/*
 * How a column of a segment's answer becomes a Datum.
 *
 * Binary for the whole result or text for the whole result, because a cursor
 * is one or the other: a type with no binary send function -- an extension's,
 * usually -- makes it text for its neighbours too.  Text costs a conversion
 * and loses nothing: PostgreSQL 19's float output is round-trip exact, which
 * is what made text safe to fall back to.
 */
typedef struct GpColumnIn
{
	FmgrInfo	proc;
	Oid			ioparam;
	int32		typmod;
} GpColumnIn;

/* One segment's side of a gather. */
typedef struct GpGatherSeg
{
	struct GpGatherState *gather;
	GpSegmentConn *conn;
	PGresult   *batch;			/* the rows being handed out */
	int			row;			/* the next of them */
	PGresult   *arrived;		/* a batch read but not yet handed out */
	bool		whole;			/* the last batch read was all that was asked */
	bool		declared;		/* the cursor exists there */
	bool		done;			/* the cursor has nothing more */
	int			asked;			/* rows the batch in flight asked for */
} GpGatherSeg;

struct GpGatherState
{
	/*
	 * Where its batches live.  PostgreSQL 19 wraps every PGresult a backend
	 * receives in a palloc'd object that frees it when its context is reset
	 * (libpq-be-fe.h), and a scan reads rows from a per-tuple context that is
	 * reset between them: a batch received there was freed while rows were
	 * still being taken from it.
	 */
	MemoryContext cxt;
	GpGang	   *gang;
	TupleDesc	tupdesc;
	bool		binary;
	GpColumnIn *columns;
	int			nsegs;			/* the segments read from: all, or one */
	GpGatherSeg *segs;
	char	   *cursor;
	int			next;			/* which segment to look at first */
};

/*
 * Can a value of this type travel in binary?  Only if it has both halves, and
 * an array or a domain only if what it is made of has them too: array_send()
 * calls the element's send function, and fails at the first row if there is
 * none, which is too late to fall back to text.
 */
/*
 * The type a value of this type travels as between the nodes.  Most travel
 * as themselves.  A few refuse to be read back, on purpose, by either input
 * or receive -- a node tree, the extended statistics' values -- because
 * nothing may make one from outside; each is binary-coercible to text or
 * bytea (pg_cast), with the same bytes, so it travels as that and is kept
 * as it arrives.  pg_catalog's pg_class.relpartbound and pg_rewrite.ev_action
 * are among them, which gp.dist_random() of a catalog reads.  And a record
 * of no declared type travels as gp_internal.record_wire, which describes
 * its row type, and is made again on arrival; and a value of type anyarray
 * -- pg_attribute's attmissingval, pg_statistic's stavalues -- as
 * gp_internal.anyarray_wire, which names its element type (gp_record.c).
 */
Oid
GpTransferType(Oid type)
{
	switch (type)
	{
		case PG_NODE_TREEOID:
			return TEXTOID;
		case PG_NDISTINCTOID:
		case PG_DEPENDENCIESOID:
		case PG_MCV_LISTOID:
			return BYTEAOID;
		case RECORDOID:
			{
				/*
				 * A record of no declared type is not kept as it arrives:
				 * record_wire's input makes it again, of a row type
				 * registered here.
				 */
				Oid			wire = GpRecordWireType();

				return OidIsValid(wire) ? wire : type;
			}
		case ANYARRAYOID:
			{
				/* nor an anyarray: anyarray_wire's input makes the array */
				Oid			wire = GpAnyarrayWireType();

				return OidIsValid(wire) ? wire : type;
			}
		default:
			return type;
	}
}

/*
 * A column as a segment's query is to produce it: the cast to what it
 * travels as, where it needs one.
 */
void
GpAppendTransferColumn(StringInfo buf, const char *column, Oid type)
{
	Oid			transfer = GpTransferType(type);

	if (type == RECORDOID && transfer != type)
	{
		appendStringInfo(buf, "gp_internal.record_wire(%s)", column);
		return;
	}
	if (type == ANYARRAYOID && transfer != type)
	{
		appendStringInfo(buf, "gp_internal.anyarray_wire(%s)", column);
		return;
	}
	appendStringInfoString(buf, column);
	if (transfer != type)
		appendStringInfo(buf, "::pg_catalog.%s", transfer == TEXTOID ? "text" : "bytea");
}

/*
 * "SELECT" and every column of the relation, as "*" would give them, each
 * cast to what it travels as.
 */
char *
GpTransferSelectList(TupleDesc tupdesc)
{
	StringInfoData buf;
	bool		first = true;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "SELECT ");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (!first)
			appendStringInfoString(&buf, ", ");
		GpAppendTransferColumn(&buf, quote_identifier(NameStr(att->attname)),
							   att->atttypid);
		first = false;
	}
	if (first)
		appendStringInfoString(&buf, "*");
	return buf.data;
}

static bool
type_has_binary_io(Oid typid)
{
	HeapTuple	tp;
	Form_pg_type typ;
	bool		result;
	Oid			inner = InvalidOid;

	typid = GpTransferType(typid);

	/*
	 * int2vector's and oidvector's receive functions refuse what their send
	 * functions make of an empty vector -- array_recv() makes it no
	 * dimensions, and they want one -- which gp_distribution_policy's
	 * distkey of a randomly distributed table is.  Their text reads back.
	 */
	if (typid == INT2VECTOROID || typid == OIDVECTOROID)
		return false;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tp);

	result = OidIsValid(typ->typsend) && OidIsValid(typ->typreceive);
	if (OidIsValid(typ->typelem) && IsTrueArrayType(typ))
		inner = typ->typelem;
	else if (typ->typtype == TYPTYPE_DOMAIN)
		inner = typ->typbasetype;
	else if (typ->typtype == TYPTYPE_COMPOSITE && result)
	{
		/* record_send() calls each column's send function in turn */
		TupleDesc	td = lookup_rowtype_tupdesc(typid, -1);

		result = GpTupleDescHasBinaryIO(td);
		ReleaseTupleDesc(td);
	}
	ReleaseSysCache(tp);

	if (result && OidIsValid(inner))
		result = type_has_binary_io(inner);
	return result;
}

bool
GpTypeHasBinaryIO(Oid type)
{
	return type_has_binary_io(type);
}

bool
GpTupleDescHasBinaryIO(TupleDesc tupdesc)
{
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (!type_has_binary_io(att->atttypid))
			return false;
	}
	return true;
}

static GpGatherState *gather_start(const char *sql, TupleDesc tupdesc,
									int content, int nsegments,
									const int *contents, int ncontents);

/*
 * A set-returning function Cloudberry runs on every segment (EXECUTE ON ALL
 * SEGMENTS: its rows are the segments' own), called on a cluster's
 * coordinator: the same call, with the same arguments, run on every segment,
 * and their rows its result, materialized.  The port's planner does not
 * move such a call; a function whose rows are the segments' calls this
 * first, and returns what it gives where it answers true.  False on a
 * segment, on one node, and in a session of the coordinator's own.
 */
bool
GpDispatchFunctionToSegments(FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			fn = fcinfo->flinfo->fn_oid;
	StringInfoData sql;
	GpGatherState *gather;
	TupleTableSlot *slot;

	if (GpClusterIsSingleNode() || GpClusterBackendRole() != GP_ROLE_DISPATCH)
		return false;

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT * FROM %s(",
					 quote_qualified_identifier(get_namespace_name(get_func_namespace(fn)),
												get_func_name(fn)));
	for (int i = 0; i < PG_NARGS(); i++)
	{
		Oid			type = get_fn_expr_argtype(fcinfo->flinfo, i);
		Oid			out;
		bool		varlena;

		if (i > 0)
			appendStringInfoString(&sql, ", ");
		if (PG_ARGISNULL(i))
		{
			appendStringInfo(&sql, "NULL::%s", format_type_be_qualified(type));
			continue;
		}
		/* a relation by its OID, which is the same on every node (R1) */
		if (type == REGCLASSOID)
		{
			appendStringInfo(&sql, "%u::pg_catalog.oid::pg_catalog.regclass",
							 DatumGetObjectId(PG_GETARG_DATUM(i)));
			continue;
		}
		getTypeOutputInfo(type, &out, &varlena);
		appendStringInfo(&sql, "%s::%s",
						 quote_literal_cstr(OidOutputFunctionCall(out, PG_GETARG_DATUM(i))),
						 format_type_be_qualified(type));
	}
	appendStringInfoChar(&sql, ')');

	InitMaterializedSRF(fcinfo, 0);
	gather = GpGatherStart(sql.data, rsinfo->setDesc);
	slot = MakeSingleTupleTableSlot(rsinfo->setDesc, &TTSOpsVirtual);
	while (GpGatherNext(gather, slot, NULL))
		tuplestore_puttupleslot(rsinfo->setResult, slot);
	GpGatherEnd(gather);
	ExecDropSingleTupleTableSlot(slot);
	return true;
}

GpGatherState *
GpGatherStart(const char *sql, TupleDesc tupdesc)
{
	return gather_start(sql, tupdesc, -1, 0, NULL, 0);
}

GpGatherState *
GpGatherStartOn(const char *sql, TupleDesc tupdesc, int content)
{
	return gather_start(sql, tupdesc, content, 0, NULL, 0);
}

GpGatherState *
GpGatherStartOnSegments(const char *sql, TupleDesc tupdesc, int nsegments)
{
	return gather_start(sql, tupdesc, -1, nsegments, NULL, 0);
}

GpGatherState *
GpGatherStartOnContents(const char *sql, TupleDesc tupdesc,
						const int *contents, int ncontents)
{
	return gather_start(sql, tupdesc, -1, 0, contents, ncontents);
}

/*
 * gp.max_plan_size: the largest plan the dispatcher sends, in kB, 0 for any
 * size -- Cloudberry's gp_max_plan_size, which its dispatcher holds a plan's
 * serialized size to before it sends it (cdbdisp_buildPlanQueryParms(),
 * cdbdisp_query.c).  The port's plans are what it sends: a gather's query,
 * and ORCA's fragments, whose plan trees are text (gp_motion.c).
 */
static int	gp_max_plan_size = 0;

void
GpDispatchCheckPlanSize(const char *sql)
{
	uint64		kb = (uint64) strlen(sql) / 1024;

	if (gp_max_plan_size > 0 && kb > (uint64) gp_max_plan_size)
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 errmsg("Query plan size limit exceeded, current size: " UINT64_FORMAT "KB, max allowed size: %dKB",
						kb, gp_max_plan_size),
				 errhint("Size controlled by gp.max_plan_size")));
}

static GpGatherState *
gather_start(const char *sql, TupleDesc tupdesc, int content, int nsegments,
			 const int *contents, int ncontents)
{
	GpGatherState *gather = (GpGatherState *) palloc0(sizeof(GpGatherState));
	GpGang	   *g;
	int			n = 0;
	const char *statement = GpLogStatementComment();

	GpDispatchCheckPlanSize(sql);

	/*
	 * Where Cloudberry's coordinator sets up the interconnect its slices'
	 * rows come to it by (SetupInterconnect()): the gather its rows come by.
	 */
	GP_FAULT("interconnect_setup_palloc");
	g = gang_get();

	/*
	 * Through a cursor, inside the coordinator's transaction.  A gather that
	 * is not read to the end -- a LIMIT above it -- closes the cursor, where
	 * reading a plain query to the end would cost the rest of the table and
	 * cancelling it would abort the segment's transaction.  Each next batch
	 * is asked for as soon as the one before has arrived, so a segment is
	 * producing rows while the coordinator hands out the ones it already has.
	 */
	gang_prepare(g, true);

	gather->cxt = CurrentMemoryContext;
	gather->gang = g;
	gather->tupdesc = tupdesc;
	gather->binary = GpTupleDescHasBinaryIO(tupdesc);
	gather->columns = (GpColumnIn *) palloc0_array(GpColumnIn, tupdesc->natts);
	gather->segs = (GpGatherSeg *) palloc0_array(GpGatherSeg, g->nconns);
	gather->cursor = psprintf("gp_gather_%u", ++gather_counter);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			proc;
		Oid			ioparam;

		if (att->attisdropped)
			continue;
		if (gather->binary)
			getTypeBinaryInputInfo(GpTransferType(att->atttypid), &proc, &ioparam);
		else
			getTypeInputInfo(GpTransferType(att->atttypid), &proc, &ioparam);
		fmgr_info(proc, &gather->columns[i].proc);
		gather->columns[i].ioparam = ioparam;
		gather->columns[i].typmod = att->atttypmod;
	}

	/*
	 * A gather is a slice sent to its segments: Cloudberry's faults before
	 * and after each is (cdbdisp_dispatchX(), cdbdisp_query.c).
	 */
	(void) GP_FAULT("before_one_slice_dispatched");
	for (int i = 0; i < g->nconns; i++)
	{
		GpGatherSeg *s;

		if (contents != NULL ? !conn_listed(&g->conns[i], contents, ncontents)
			: !conn_asked(&g->conns[i], content, nsegments))
			continue;

		s = &gather->segs[n++];
		s->gather = gather;
		s->conn = &g->conns[i];
		s->asked = GATHER_FETCH_FIRST;
		conn_send(s->conn,
				  psprintf("DECLARE %s %sNO SCROLL CURSOR FOR %s; FETCH %d FROM %s%s",
						   gather->cursor, gather->binary ? "BINARY " : "",
						   sql, s->asked, gather->cursor, statement));
		s->conn->fetching = s;
		s->declared = true;
	}
	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no segment with content id %d", content)));
	gather->nsegs = n;
	(void) GP_FAULT("after_one_slice_dispatched");

	return gather;
}

/* One row of a segment's answer, into the slot. */
static void
gather_store_row(GpGatherState *gather, PGresult *res, int row,
				 TupleTableSlot *slot)
{
	TupleDesc	tupdesc = gather->tupdesc;

	if (PQnfields(res) != tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a segment answered with %d columns, not %d",
						PQnfields(res), tupdesc->natts)));

	ExecClearTuple(slot);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (PQgetisnull(res, row, i) || TupleDescAttr(tupdesc, i)->attisdropped)
		{
			slot->tts_isnull[i] = true;
			slot->tts_values[i] = (Datum) 0;
			continue;
		}

		slot->tts_isnull[i] = false;

		if (gather->binary)
		{
			StringInfoData buf;

			initReadOnlyStringInfo(&buf, PQgetvalue(res, row, i),
								   PQgetlength(res, row, i));
			slot->tts_values[i] = GpReceiveFunctionCall(&gather->columns[i].proc,
														&buf,
														gather->columns[i].ioparam,
														gather->columns[i].typmod);
		}
		else
			slot->tts_values[i] = InputFunctionCall(&gather->columns[i].proc,
												   PQgetvalue(res, row, i),
												   gather->columns[i].ioparam,
												   gather->columns[i].typmod);
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * Read whatever has arrived of a gather segment's batch, without waiting.
 * When the whole answer is in, the batch becomes the segment's "arrived" one.
 * Returns whether anything was read.
 */
static bool
gather_poll(GpGatherSeg *s)
{
	GpSegmentConn *c = s->conn;
	bool		progress = false;

	if (!c->busy || c->fetching != s)
		return false;

	if (PQconsumeInput(c->conn) == 0)
	{
		List	   *errors = NIL;

		collect_error(&errors, c->content, NULL, c->conn, NULL);
		gang_close();
		raise_segment_errors(errors);
	}

	while (!PQisBusy(c->conn))
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(s->gather->cxt);
		PGresult   *res = PQgetResult(c->conn);
		ExecStatusType status;

		MemoryContextSwitchTo(oldcxt);
		progress = true;

		/*
		 * A batch short of what was asked for is the cursor's last.  The end
		 * of the statement can come in a later read than its rows, which by
		 * then may be handed out and gone from "arrived": the batch's size is
		 * taken as it arrives.
		 */
		if (res == NULL)
		{
			c->busy = false;
			c->fetching = NULL;
			if (!s->whole)
				s->done = true;
			break;
		}

		status = PQresultStatus(res);
		if (status == PGRES_TUPLES_OK)
		{
			Assert(s->arrived == NULL);
			s->arrived = res;
			s->whole = PQntuples(res) >= s->asked;
			continue;
		}
		if (status == PGRES_COMMAND_OK)
		{
			PQclear(res);		/* the DECLARE */
			continue;
		}

		{
			List	   *errors = NIL;

			collect_error(&errors, c->content, res, c->conn, NULL);
			PQclear(res);
			if (PQstatus(c->conn) == CONNECTION_BAD)
				gang_close_broken();
			raise_segment_errors(errors);
		}
	}

	if (progress)
		flush_segment_notices();
	return progress;
}

/*
 * Set a gather's batch aside so that the connection can be used for something
 * else: read it to the end, into the gather segment it was asked for.
 */
static void
conn_park(GpSegmentConn *c)
{
	GpGatherSeg *s = c->fetching;

	while (c->busy && c->fetching == s)
	{
		if (!gather_poll(s))
			gang_wait(gang);
	}
}

/* Ask for a segment's next batch, ten times the last, up to the most. */
static void
gather_fetch(GpGatherSeg *s)
{
	s->asked = Min(s->asked * 10, GATHER_FETCH_ROWS);
	conn_send(s->conn, psprintf("FETCH %d FROM %s", s->asked,
								s->gather->cursor));
	s->conn->fetching = s;
	s->whole = false;
}

/*
 * The next row from any segment: the segment's side of the gather, whose
 * batch holds it at *row; NULL when every segment has finished.
 */
static GpGatherSeg *
gather_next_row(GpGatherState *gather, int *row)
{
	if (gather->gang != gang)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost the connections to the segments while reading from them")));

	for (;;)
	{
		bool		unfinished = false;
		bool		progress = false;

		for (int n = 0; n < gather->nsegs; n++)
		{
			int			i = (gather->next + n) % gather->nsegs;
			GpGatherSeg *s = &gather->segs[i];

			if (s->batch != NULL && s->row < PQntuples(s->batch))
			{
				*row = s->row++;
				/* The next row from the next segment: they take turns. */
				gather->next = (i + 1) % gather->nsegs;
				return s;
			}

			if (s->batch != NULL)
			{
				PQclear(s->batch);
				s->batch = NULL;
			}

			if (gather_poll(s))
				progress = true;

			if (s->arrived != NULL)
			{
				s->batch = s->arrived;
				s->arrived = NULL;
				s->row = 0;
				progress = true;

				/*
				 * Ask for the next batch while this one is handed out, unless
				 * this was the last, or the connection is busy with somebody
				 * else's statement.
				 */
				if (!s->done && !s->conn->busy)
					gather_fetch(s);

				n--;			/* look at this segment again */
				continue;
			}

			if (s->done)
				continue;

			unfinished = true;

			/* Nothing in hand and nothing asked for: ask. */
			if (s->conn->fetching != s)
			{
				gather_fetch(s);
				progress = true;
			}
		}

		if (!unfinished)
			return NULL;
		if (!progress)
			gang_wait(gather->gang);
	}
}

bool
GpGatherNext(GpGatherState *gather, TupleTableSlot *slot, int *content)
{
	int			row;
	GpGatherSeg *s = gather_next_row(gather, &row);

	if (s == NULL)
		return false;
	gather_store_row(gather, s->batch, row, slot);
	if (content != NULL)
		*content = s->conn->content;
	return true;
}

bool
GpGatherNextRaw(GpGatherState *gather, const char **values, int *lengths)
{
	int			row;
	GpGatherSeg *s = gather_next_row(gather, &row);
	int			natts = gather->tupdesc->natts;

	if (s == NULL)
		return false;
	if (PQnfields(s->batch) != natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("a segment answered with %d columns, not %d",
						PQnfields(s->batch), natts)));

	for (int i = 0; i < natts; i++)
	{
		if (PQgetisnull(s->batch, row, i))
		{
			values[i] = NULL;
			lengths[i] = -1;
		}
		else
		{
			values[i] = PQgetvalue(s->batch, row, i);
			lengths[i] = PQgetlength(s->batch, row, i);
		}
	}
	return true;
}

bool
GpGatherIsBinary(GpGatherState *gather)
{
	return gather->binary;
}

Datum
GpGatherDecodeValue(GpGatherState *gather, int col, const char *value,
					int length)
{
	GpColumnIn *in = &gather->columns[col];

	if (gather->binary)
	{
		StringInfoData buf;

		initReadOnlyStringInfo(&buf, (char *) value, length);
		return GpReceiveFunctionCall(&in->proc, &buf, in->ioparam, in->typmod);
	}
	return InputFunctionCall(&in->proc, (char *) value, in->ioparam,
							 in->typmod);
}

int
GpGatherSegmentCount(GpGatherState *gather)
{
	return gather->nsegs;
}

/*
 * The next row from one of the gather's segments, waiting for it if it has
 * not arrived; false when that segment has no more.  What a merge needs: it
 * takes the least row of the segments' next ones, so it has to be able to ask
 * for a particular segment's.
 */
bool
GpGatherNextFrom(GpGatherState *gather, int seg, TupleTableSlot *slot)
{
	GpGatherSeg *s;

	if (gather->gang != gang)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("lost the connections to the segments while reading from them")));
	Assert(seg >= 0 && seg < gather->nsegs);
	s = &gather->segs[seg];

	for (;;)
	{
		if (s->batch != NULL && s->row < PQntuples(s->batch))
		{
			gather_store_row(gather, s->batch, s->row++, slot);
			return true;
		}

		if (s->batch != NULL)
		{
			PQclear(s->batch);
			s->batch = NULL;
		}

		(void) gather_poll(s);

		if (s->arrived != NULL)
		{
			s->batch = s->arrived;
			s->arrived = NULL;
			s->row = 0;
			if (!s->done && !s->conn->busy)
				gather_fetch(s);
			continue;
		}

		if (s->done)
			return false;

		if (s->conn->fetching != s)
		{
			gather_fetch(s);
			continue;
		}

		gang_wait(gather->gang);
	}
}

void
GpGatherEnd(GpGatherState *gather)
{
	GpGang	   *g = gather->gang;

	if (gang != g)
		return;					/* the gang was lost, and its cursors with it */

	/*
	 * Read what is still on its way, which is at most a batch, and close the
	 * cursors: the segments' transaction goes on, and may gather again.
	 */
	for (int i = 0; i < gather->nsegs; i++)
	{
		GpGatherSeg *s = &gather->segs[i];

		if (s->conn->fetching == s)
			conn_park(s->conn);
		if (s->arrived != NULL)
			PQclear(s->arrived);
		if (s->batch != NULL)
			PQclear(s->batch);
		s->arrived = s->batch = NULL;
	}

	for (int i = 0; i < gather->nsegs; i++)
		conn_send(gather->segs[i].conn, psprintf("CLOSE %s", gather->cursor));
	gang_wait_all(g, NULL, false);
}

/* ------------------------------------------------------------------------- */
/* The SQL surface                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Run a query on every segment (content -1) or one, and answer the first
 * column of each one's first row, as text, NULL where there was none; one
 * entry per segment asked, in content order.
 */
void
GpDispatchQueryFirstValues(const char *sql, int content, char **values)
{
	GpGang	   *g = gang_get();
	PGresult  **results;
	int			n = 0;

	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	for (int i = 0; i < g->nconns; i++)
		if (content < 0 || g->conns[i].content == content)
			conn_send(&g->conns[i], sql);
	gang_wait_all(g, results, false);

	for (int i = 0; i < g->nconns; i++)
	{
		if (content >= 0 && g->conns[i].content != content)
			continue;
		values[n++] = (results[i] != NULL && PQntuples(results[i]) > 0 &&
					   PQnfields(results[i]) > 0 && !PQgetisnull(results[i], 0, 0))
			? pstrdup(PQgetvalue(results[i], 0, 0)) : NULL;
		if (results[i] != NULL)
			PQclear(results[i]);
	}
}

PG_FUNCTION_INFO_V1(gp_backend_info);

/* Cloudberry's SQLSTATE for a command that cannot run where it was asked */
#define ERRCODE_GP_COMMAND_ERROR	MAKE_SQLSTATE('4','2','M','0','0')

/*
 * pg_catalog.gp_backend_info()
 *		The session's backends, as Cloudberry's gp_backend_info() lists them
 *		(cdbgang.c): this one, the coordinator's, of type 'Q' and id -1; the
 *		writer on each segment, 'w'; and each reader, 'r' -- each with an id
 *		of its own, its node's content id, host and port, and its pid.  The
 *		port has no entry reader, Cloudberry's 'R': the coordinator's own
 *		slice runs in this backend.
 */
Datum
gp_backend_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const GpSegmentConfig *self = GpClusterSelf();
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};
	int			id = 0;

	if (GpClusterBackendRole() != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_GP_COMMAND_ERROR),
				 errmsg("gp_backend_info() could only be called on QD")));

	InitMaterializedSRF(fcinfo, 0);

	values[0] = Int32GetDatum(-1);
	values[1] = CharGetDatum('Q');
	values[2] = Int32GetDatum(-1);
	values[3] = CStringGetTextDatum(self != NULL ? self->hostname : "localhost");
	values[4] = Int32GetDatum(self != NULL ? self->port : PostPortNumber);
	values[5] = Int32GetDatum(MyProcPid);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	if (gang == NULL)
		return (Datum) 0;
	for (int i = 0; i < gang->nconns; i++)
	{
		GpSegmentConn *c = &gang->conns[i];

		values[0] = Int32GetDatum(id++);
		values[1] = CharGetDatum('w');
		values[2] = Int32GetDatum(c->content);
		values[3] = CStringGetTextDatum(c->seg->hostname);
		values[4] = Int32GetDatum(c->seg->port);
		values[5] = Int32GetDatum(PQbackendPID(c->conn));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	foreach_ptr(GpReaderConn, r, gang->readers)
	{
		const GpSegmentConfig *seg = NULL;

		if (r->conn == NULL)
			continue;
		for (int i = 0; i < gang->nconns; i++)
			if (gang->conns[i].content == r->content)
				seg = gang->conns[i].seg;
		values[0] = Int32GetDatum(id++);
		values[1] = CharGetDatum('r');
		values[2] = Int32GetDatum(r->content);
		values[3] = CStringGetTextDatum(seg != NULL ? seg->hostname : "");
		values[4] = Int32GetDatum(seg != NULL ? seg->port : 0);
		values[5] = Int32GetDatum(PQbackendPID(r->conn));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_exec_on_segments);

/*
 * gp.exec_on_segments(sql)
 *		Run a statement on every segment, and report what each one said.
 *
 * The answer is the first column of the first row, as text, because this is
 * for asking a cluster about itself -- "what does each segment think it is",
 * "how many rows does each one hold" -- and not for reading a table, which is
 * what the scan of a distributed table does.  It runs in the coordinator's
 * transaction, like everything else sent to the segments.
 *
 * Superuser only.  It runs arbitrary SQL on a machine the caller may have no
 * other way to reach, and a segment is not a place to widen anyone's reach.
 */
Datum
gp_exec_on_segments(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	GpGang	   *g;
	PGresult  **results;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to run a statement on the segments")));

	InitMaterializedSRF(fcinfo, 0);

	g = gang_get();
	gang_prepare(g, true);
	results = (PGresult **) palloc0_array(PGresult *, g->nconns);

	gang_send_all(g, sql);
	gang_wait_all(g, results, false);

	for (int i = 0; i < g->nconns; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, true};

		values[0] = Int32GetDatum(g->conns[i].content);
		if (results[i] != NULL && PQntuples(results[i]) > 0 &&
			PQnfields(results[i]) > 0 && !PQgetisnull(results[i], 0, 0))
		{
			values[1] = CStringGetTextDatum(PQgetvalue(results[i], 0, 0));
			nulls[1] = false;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		if (results[i] != NULL)
			PQclear(results[i]);
	}

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(gp_dist_random);

/*
 * gp.dist_random(NULL::t)
 *		The rows of a relation as the segments hold them.
 *
 * Cloudberry's gp_dist_random('t') is a function whose result type its planner
 * fills in from the argument, which an extension cannot do.  PostgreSQL has
 * the same effect through polymorphism: the argument is a value of the
 * relation's own row type -- NULL::t says which relation without reading one --
 * and the result is a set of that type.  Cloudberry's own tests reach for this
 * constantly, which is why it is here rather than later.
 *
 * It is the scan of a distributed table with nothing planned around it: no
 * qual pushed down, no column left out.  Where there is nothing to dispatch
 * to, it reads the relation here, as Cloudberry's does on a single node.
 */
Datum
gp_dist_random(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid;
	Relation	rel;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	GpGatherState *gather;
	StringInfoData sql;

	if (!OidIsValid(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine which relation to read"),
				 errhint("Write the relation's row type, as in gp.dist_random(NULL::mytable).")));

	relid = get_typ_typrelid(argtype);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("type %s is not a relation's row type",
						format_type_be(argtype))));

	/* The same lock an ordinary scan of it would take. */
	rel = table_open(relid, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	if (GpDistRandomIsLocal())
	{
		GpDistRandomLocal(relid, rsinfo->setResult, rsinfo->setDesc, false);
		table_close(rel, AccessShareLock);
		return (Datum) 0;
	}

	initStringInfo(&sql);
	appendStringInfo(&sql, "%s FROM %s", GpTransferSelectList(tupdesc),
					 GpDispatchRelationName(RelationGetRelid(rel)));

	slot = MakeSingleTupleTableSlot(GpTransferDesc(tupdesc), &TTSOpsVirtual);
	gather = GpGatherStart(sql.data, slot->tts_tupleDescriptor);
	while (GpGatherNext(gather, slot, NULL))
		GpTransferPut(tupdesc, slot, rsinfo->setResult, rsinfo->setDesc, -1);
	GpGatherEnd(gather);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * Is "sql" a query gp_segment.c makes the planner run on the segments: one
 * SELECT and nothing else, as ruleutils prints it and O26 reads it -- of one
 * gp_dist_random() alone, gp.dist_random(NULL::t) or gp_dist_random('t')
 * where gp_sql does not desugar it; of one function's rows alone, a function
 * that runs on all segments; or of no relation, a query that calls one?  A
 * SELECT is all it runs: nothing that writes but by a function it calls, as
 * the same query would through the coordinator, and nothing that locks.
 */
static bool
is_segment_query(const char *sql)
{
	List	   *stmts = raw_parser(sql, RAW_PARSE_DEFAULT);
	SelectStmt *select;
	RangeFunction *range;
	List	   *call;

	if (list_length(stmts) != 1)
		return false;
	select = (SelectStmt *) linitial_node(RawStmt, stmts)->stmt;
	if (!IsA(select, SelectStmt) || select->op != SETOP_NONE ||
		select->withClause != NULL || select->intoClause != NULL ||
		select->lockingClause != NIL || select->valuesLists != NIL)
		return false;
	if (select->fromClause == NIL)
		return true;
	if (list_length(select->fromClause) != 1 ||
		!IsA(linitial(select->fromClause), RangeFunction))
		return false;
	range = linitial_node(RangeFunction, select->fromClause);
	if (range->lateral || range->ordinality || range->is_rowsfrom ||
		list_length(range->functions) != 1)
		return false;
	call = linitial_node(List, range->functions);
	return IsA(linitial(call), FuncCall);
}

/*
 * Is the type one of the OID's aliases, regclass and the others?  Their
 * value is written for a segment as the OID, which is every node's, and not
 * as the name, which the segment would look up again.
 */
static bool
is_oid_alias(Oid type)
{
	switch (type)
	{
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGCOLLATIONOID:
		case REGTYPEOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
		case REGDATABASEOID:
			return true;
		default:
			return false;
	}
}

/*
 * The query's $n, each the call's argument n as a literal of its type -- the
 * values the coordinator evaluated for the segments (gp_segment.c) -- found
 * by PostgreSQL's own scanner, so that a $n in a string is left alone.
 */
static char *
segment_query_values(const char *sql, FunctionCallInfo fcinfo)
{
	core_yyscan_t scanner;
	core_yy_extra_type extra;
	core_YYSTYPE lval;
	YYLTYPE		loc;
	int			code;
	StringInfoData out;
	int			copied = 0;

	initStringInfo(&out);
	scanner = scanner_init(sql, &extra, &ScanKeywords, ScanKeywordTokens);
	while ((code = core_yylex(&lval, &loc, scanner)) != 0)
	{
		int			n = lval.ival;
		int			len = 1;
		Oid			type;

		if (code != GP_PARAM)
			continue;
		if (n < 1 || n >= PG_NARGS())
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no value for parameter $%d", n)));
		while (isdigit((unsigned char) sql[loc + len]))
			len++;
		appendBinaryStringInfo(&out, sql + copied, loc - copied);
		type = get_fn_expr_argtype(fcinfo->flinfo, n);
		if (PG_ARGISNULL(n))
			appendStringInfo(&out, "NULL::%s", format_type_be_qualified(type));
		else if (is_oid_alias(type))
			appendStringInfo(&out, "('%u'::%s)", DatumGetObjectId(PG_GETARG_DATUM(n)),
							 format_type_be_qualified(type));
		else
		{
			Oid			output;
			bool		varlena;

			getTypeOutputInfo(type, &output, &varlena);
			appendStringInfo(&out, "(%s::%s)",
							 quote_literal_cstr(OidOutputFunctionCall(output,
																	 PG_GETARG_DATUM(n))),
							 format_type_be_qualified(type));
		}
		copied = loc + len;
	}
	scanner_finish(scanner);
	appendStringInfoString(&out, sql + copied);
	return out.data;
}

PG_FUNCTION_INFO_V1(gp_segment_query);

/*
 * gp_internal.segment_query(sql text, VARIADIC "any")
 *		A query of gp_dist_random() alone, run on every segment: the rows
 *		each answers, in the column definition list's types.  Its $n are the
 *		further arguments.
 */
Datum
gp_segment_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *sql;
	TupleTableSlot *slot;
	GpGatherState *gather;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("gp_internal.segment_query() runs only a query of one gp_dist_random(), of one function's rows, or of no relation")));
	sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (PG_NARGS() > 1)
		sql = segment_query_values(sql, fcinfo);

	if (!is_segment_query(sql))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("gp_internal.segment_query() runs only a query of one gp_dist_random(), of one function's rows, or of no relation")));
	if (GpDistRandomIsLocal())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp_internal.segment_query() runs only on a cluster's coordinator")));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	slot = MakeSingleTupleTableSlot(rsinfo->setDesc, &TTSOpsVirtual);
	gather = GpGatherStart(sql, rsinfo->setDesc);
	while (GpGatherNext(gather, slot, NULL))
		tuplestore_puttupleslot(rsinfo->setResult, slot);
	GpGatherEnd(gather);
	ExecDropSingleTupleTableSlot(slot);

	return (Datum) 0;
}

/*
 * A relation's row as a gather reads it: its columns but the dropped ones,
 * which GpTransferSelectList() leaves out.
 */
TupleDesc
GpTransferDesc(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			n = 0;

	for (int i = 0; i < tupdesc->natts; i++)
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			n++;
	desc = CreateTemplateTupleDesc(n);
	n = 0;
	for (int i = 0; i < tupdesc->natts; i++)
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			TupleDescCopyEntry(desc, ++n, tupdesc, i + 1);
	TupleDescFinalize(desc);
	return desc;
}

/*
 * One row a gather read by GpTransferDesc(tupdesc), into "store" as the
 * relation's row -- a dropped column null -- followed by "content", when it
 * is not -1, as "desc" has it.
 */
void
GpTransferPut(TupleDesc tupdesc, TupleTableSlot *slot, Tuplestorestate *store,
			  TupleDesc desc, int content)
{
	Datum	   *values = palloc_array(Datum, desc->natts);
	bool	   *nulls = palloc_array(bool, desc->natts);
	int			j = 0;

	slot_getallattrs(slot);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}
		values[i] = slot->tts_values[j];
		nulls[i] = slot->tts_isnull[j];
		j++;
	}
	if (content != -1)
	{
		values[tupdesc->natts] = Int32GetDatum(content);
		nulls[tupdesc->natts] = false;
	}
	tuplestore_putvalues(store, desc, values, nulls);
	pfree(values);
	pfree(nulls);
}

/*
 * Is there nothing for gp.dist_random() to dispatch to: one node, or a
 * session that is not the coordinator's dispatching one?
 */
bool
GpDistRandomIsLocal(void)
{
	return GpClusterIsSingleNode() ||
		GpClusterBackendRole() != GP_ROLE_DISPATCH;
}

/*
 * gp.dist_random() where there is nothing to dispatch to: the relation's
 * rows here, inheritance children included as a gather's are, into `store`
 * as `desc` says -- the relation's row type, or with gp_segment_id after it,
 * which is this node's content id.
 */
void
GpDistRandomLocal(Oid relid, Tuplestorestate *store, TupleDesc desc,
				  bool with_content)
{
	Oid			typid = get_rel_type_id(relid);
	TupleDesc	rowdesc = lookup_rowtype_tupdesc_copy(typid, -1);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	MemoryContext outer = CurrentMemoryContext;
	char	   *sql;

	if (desc->natts != rowdesc->natts + (with_content ? 1 : 0))
		elog(ERROR, "gp.dist_random() called with %d columns for a relation of %d",
			 desc->natts, rowdesc->natts);

	sql = psprintf("SELECT r FROM %s r", GpDispatchRelationName(relid));

	SPI_connect();
	if (SPI_execute(sql, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read relation %u", relid);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		bool		isnull;
		Datum		row = SPI_getbinval(SPI_tuptable->vals[r],
										SPI_tuptable->tupdesc, 1, &isnull);
		HeapTupleHeader hdr = DatumGetHeapTupleHeader(row);
		HeapTupleData tuple;
		MemoryContext old;

		tuple.t_len = HeapTupleHeaderGetDatumLength(hdr);
		ItemPointerSetInvalid(&tuple.t_self);
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = hdr;
		heap_deform_tuple(&tuple, rowdesc, values, nulls);
		if (with_content)
		{
			values[desc->natts - 1] = Int32GetDatum(GpClusterContentId());
			nulls[desc->natts - 1] = false;
		}

		old = MemoryContextSwitchTo(outer);
		tuplestore_putvalues(store, desc, values, nulls);
		MemoryContextSwitchTo(old);
	}
	SPI_finish();
}

/* ------------------------------------------------------------------------- */
/* Start-up                                                                  */
/* ------------------------------------------------------------------------- */

int
GpInternalConnOptions(const char **keywords, const char **values, int n)
{
	static const struct
	{
		const char *keyword;
		char	  **setting;
	}			options[] =
	{
		{"passfile", &gp_internal_passfile},
		{"sslmode", &gp_internal_sslmode},
		{"sslcert", &gp_internal_sslcert},
		{"sslkey", &gp_internal_sslkey},
		{"sslrootcert", &gp_internal_sslrootcert},
		{"sslcrl", &gp_internal_sslcrl},
	};

	StaticAssertStmt(lengthof(options) + 1 == GP_INTERNAL_CONN_OPTIONS,
					 "GP_INTERNAL_CONN_OPTIONS counts the options and the terminator");
	for (int i = 0; i < lengthof(options); i++)
	{
		const char *value = *options[i].setting;

		if (value != NULL && value[0] != '\0')
		{
			keywords[n] = options[i].keyword;
			values[n++] = value;
		}
	}
	keywords[n] = NULL;
	values[n] = NULL;
	return n;
}

/* libpq's sslmode, or "" for libpq's own default. */
static bool
check_internal_sslmode(char **newval, void **extra, GucSource source)
{
	static const char *const modes[] = {
		"", "disable", "allow", "prefer", "require", "verify-ca", "verify-full"
	};

	for (int i = 0; i < lengthof(modes); i++)
		if (strcmp(*newval, modes[i]) == 0)
			return true;
	GUC_check_errdetail("Valid values are \"disable\", \"allow\", \"prefer\", \"require\", \"verify-ca\", \"verify-full\" and \"\", libpq's default.");
	return false;
}

/* ------------------------------------------------------------------------- */
/* A connection's start                                                      */
/* ------------------------------------------------------------------------- */

static ClientAuthentication_hook_type prev_client_auth = NULL;

/*
 * gp.qe_details: what a segment process reports of itself to the dispatcher
 * as it starts, as a setting PostgreSQL reports to its client -- its node's
 * content id -- as Cloudberry's QE reports its motion listener's port
 * (sendQEDetails(), dest.c).  A connection whose backend reports none is no
 * segment process of this cluster's (connect_segments()).  Empty in every
 * other backend, and in one whose report the fault
 * send_qe_details_init_backend skipped.
 */
static char *gp_qe_details = NULL;
static bool qe_details_skipped = false;

static const char *
show_qe_details(void)
{
	static char buf[16];

	if (!GpClusterIsDispatched() || qe_details_skipped)
		return "";
	snprintf(buf, sizeof(buf), "%d", GpClusterContentId());
	return buf;
}

/*
 * A setting a connection's startup packet gives -- "-c name=value" in its
 * options, "--name=value", or a parameter of its own -- as it came, before
 * InitPostgres() makes it the setting.  NULL if none.
 */
static char *
startup_option(Port *port, const char *name)
{
	size_t		namelen = strlen(name);
	ListCell   *lc;

	if (port->cmdline_options != NULL)
	{
		char	  **av = palloc0_array(char *, 2 + (strlen(port->cmdline_options) + 1) / 2);
		int			ac = 0;

		pg_split_opts(av, &ac, port->cmdline_options);
		for (int i = 0; i < ac; i++)
		{
			const char *setting = NULL;

			if (strcmp(av[i], "-c") == 0 && i + 1 < ac)
				setting = av[++i];
			else if (strncmp(av[i], "-c", 2) == 0 || strncmp(av[i], "--", 2) == 0)
				setting = av[i] + 2;
			if (setting != NULL && strncmp(setting, name, namelen) == 0 &&
				setting[namelen] == '=')
				return pstrdup(setting + namelen + 1);
		}
	}
	for (lc = list_head(port->guc_options); lc != NULL; lc = lnext(port->guc_options, lc))
	{
		const char *option = lfirst(lc);

		lc = lnext(port->guc_options, lc);
		if (lc == NULL)
			break;
		if (strcmp(option, name) == 0)
			return pstrdup(lfirst(lc));
	}
	return NULL;
}

/*
 * Every connection passes here once it is authenticated, on every node, but
 * a WAL sender's.  A client of the coordinator -- no retrieve session, no
 * connection of its own node's loopback -- takes its session id now
 * (GpClusterSessionId()), before a client that connects after it could take
 * one, and says it for pg_stat_activity (GpGddNoteSession()).  And two of
 * Cloudberry's faults are moments of a connection's start: any connection's
 * but FTS's probe's, as Cloudberry's ProcessStartupPacket() asks it
 * (postmaster.c), process_startup_packet, which answers that the node is in
 * recovery where it is skipped, and waits where it is suspended; and a
 * segment process's as it would report its details (PostgresMain()),
 * send_qe_details_init_backend, for the session it works for, which leaves
 * them unreported where it is skipped, and whose error is the process's
 * FATAL, as Cloudberry's is.  The startup packet's options are not the
 * settings yet, and are read as they came.
 */
static void
dispatch_client_auth(Port *port, int status)
{
	char	   *identity;

	if (prev_client_auth)
		prev_client_auth(port, status);
	if (status != STATUS_OK || am_walsender)
		return;

	identity = startup_option(port, "gp.qe_identity");
	if (identity == NULL && GpClusterBackendRole() == GP_ROLE_DISPATCH &&
		!GpEndpointIsRetrieveSession())
	{
		(void) GpClusterSessionId();
		GpGddNoteSession();
	}

	if (gp_fault_active == NULL || *gp_fault_active == 0)
		return;
	if ((port->application_name == NULL ||
		 strcmp(port->application_name, GP_FTS_APPNAME) != 0) &&
		GpFaultTrigger("process_startup_packet",
					   port->database_name ? port->database_name : "",
					   "") == GP_FAULT_SKIP)
		ereport(FATAL,
				(errcode(ERRCODE_CANNOT_CONNECT_NOW),
				 errmsg("the database system is in recovery mode")));
	if (identity != NULL)
	{
		const char *sess = strstr(identity, "/sess");

		if (GpFaultTriggerSession("send_qe_details_init_backend", "", "",
								  sess != NULL ? atoi(sess + strlen("/sess")) : -1) ==
			GP_FAULT_SKIP)
			qe_details_skipped = true;
	}
}

/*
 * A connection's start, from gp_core's _PG_init, after the other modules'
 * authentication hooks, whose work it follows: a retrieve session is one
 * once gp_endpoint.c's has seen it.
 */
void
GpDispatchConnectionInit(void)
{
	DefineCustomStringVariable("gp.qe_details",
							   "What a segment process reports of itself to the dispatcher as it starts.",
							   "Its node's content id, in a process the coordinator started; empty "
							   "in every other.  Cloudberry's segment process reports its motion "
							   "listener's port.",
							   &gp_qe_details,
							   "",
							   PGC_INTERNAL,
							   GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE |
							   GUC_DISALLOW_IN_FILE,
							   NULL, NULL, show_qe_details);

	prev_client_auth = ClientAuthentication_hook;
	ClientAuthentication_hook = dispatch_client_auth;
}

void
GpDispatchInit(void)
{
	DefineCustomIntVariable("gp.gang_creation_retry_count",
							"How many times a gang is tried again while a segment is in recovery.",
							NULL,
							&gp_gang_creation_retry_count,
							5, 0, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.gang_creation_retry_timer",
							"How long to wait before a gang is tried again.",
							NULL,
							&gp_gang_creation_retry_timer,
							2000, 1, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	/* Cloudberry's limits, Linux's (cdbvars.h) */
	DefineCustomIntVariable("gp.dispatch_keepalives_idle",
							"Time between issuing TCP keepalives from the coordinator to its segments.",
							"A value of 0 uses the system default.",
							&gp_dispatch_keepalives_idle,
							0, 0, 32767,
							PGC_POSTMASTER,
							GUC_UNIT_S | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.dispatch_keepalives_interval",
							"Time between TCP keepalive retransmits from the coordinator to its segments.",
							"A value of 0 uses the system default.",
							&gp_dispatch_keepalives_interval,
							0, 0, 32767,
							PGC_POSTMASTER,
							GUC_UNIT_S | GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.dispatch_keepalives_count",
							"Maximum number of TCP keepalive retransmits from the coordinator to its segments.",
							"How many consecutive keepalives may be lost before a connection to a segment is "
							"considered dead.  A value of 0 uses the system default.",
							&gp_dispatch_keepalives_count,
							0, 0, 127,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("gp.log_gang",
							 "How much the dispatcher logs of its gang.",
							 "Valid values are \"off\", \"terse\", \"verbose\" and \"debug\".",
							 &gp_log_gang,
							 GANG_LOG_OFF,
							 gp_log_gang_options,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gp.internal_passfile",
							   "Password file the dispatcher hands libpq.",
							   "The dispatcher authenticates like any other "
							   "client, so a cluster that asks for SCRAM needs "
							   "the password somewhere the server can read and "
							   "a user cannot: a file, not a setting.",
							   &gp_internal_passfile,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	/*
	 * Certificates between nodes (decision 5): libpq's options, for every
	 * connection gp_core opens to another node.  A relative path is the data
	 * directory's, where a server's processes run.
	 */
	DefineCustomStringVariable("gp.internal_sslmode",
							   "TLS a node asks of the node it connects to.",
							   "libpq's sslmode; empty for libpq's default.",
							   &gp_internal_sslmode,
							   "",
							   PGC_SUSET,
							   0,
							   check_internal_sslmode, NULL, NULL);
	DefineCustomStringVariable("gp.internal_sslcert",
							   "Certificate a node shows the node it connects to.",
							   "libpq's sslcert; empty for libpq's default.",
							   &gp_internal_sslcert,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("gp.internal_sslkey",
							   "Private key of gp.internal_sslcert.",
							   "libpq's sslkey; empty for libpq's default.",
							   &gp_internal_sslkey,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("gp.internal_sslrootcert",
							   "Certificate authorities a node checks another node's certificate by.",
							   "libpq's sslrootcert; empty for libpq's default.",
							   &gp_internal_sslrootcert,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("gp.internal_sslcrl",
							   "Certificates revoked, of those another node may show.",
							   "libpq's sslcrl; empty for libpq's default.",
							   &gp_internal_sslcrl,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("gp.segment_connect_timeout",
							"Maximum time (in seconds) allowed for a new worker process to start or a mirror to respond.",
							"0 indicates 'wait forever'.  Cloudberry calls this gp_segment_connect_timeout.",
							&gp_segment_connect_timeout,
							180, 0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gp.max_plan_size",
							"Sets the maximum size of a plan to be dispatched.",
							"0 for any size.  Cloudberry calls this gp_max_plan_size.",
							&gp_max_plan_size,
							0, 0, MAX_KILOBYTES,
							PGC_SUSET,
							GUC_UNIT_KB,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("gp.print_create_gang_time",
							 "Allow print information about create gang time.",
							 "Cloudberry calls this gp_print_create_gang_time.",
							 &gp_print_create_gang_time,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	RegisterXactCallback(dispatch_xact_callback, NULL);
	RegisterSubXactCallback(dispatch_subxact_callback, NULL);
	prev_commit_recorded_hook = xact_commit_recorded_hook;
	xact_commit_recorded_hook = dispatch_commit_recorded;

	/*
	 * O37: an error raised in an abort once the transaction has ended for
	 * the other backends -- by an abort callback, or by a test's fault,
	 * abort_after_procarray_end (test-iso2-recovery.patch) -- runs
	 * AbortTransaction() again, which then records the abort no second
	 * time, as Cloudberry's xact.c guards it (its b5c4fdc0098).  This file's
	 * own abort callback raises nothing; others may.
	 */
	XactAbortAgainAfterEnd = true;
}

/*
 * Has this transaction prepared parts on the segments, so that its commit
 * on the coordinator is the distributed transaction's decision?  Asked as it
 * commits, by a fault of Cloudberry's distributed commit alone (gp_fault.c).
 */
bool
GpDispatchDtxPrepared(void)
{
	return dtx_nprepared > 0;
}

/* ------------------------------------------------------------------------- */
/* Ending a node's segment processes                                         */
/* ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(gp_terminate_mpp_backends);

/*
 * pg_catalog.gp_terminate_mpp_backends()
 *		End every segment process of this node but the caller's, as
 *		Cloudberry's does (signalfuncs.c): a superuser's call, on a segment,
 *		in a process the coordinator dispatched to -- a query of
 *		gp_dist_random('gp_id') runs it on each.  A session whose processes
 *		it ended finds its gang gone at its next statement.
 */
Datum
gp_terminate_mpp_backends(PG_FUNCTION_ARGS)
{
	if (GpClusterBackendRole() != GP_ROLE_EXECUTE)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("terminate mpp backends on segments only")));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("Superuser only to execute it")));

	elog(LOG, "tried to terminate all (%d) mpp backends except self",
		 GpGddSignalSessionBackends(SIGTERM));

	PG_RETURN_NULL();
}

/*
 * Cloudberry's fault as it makes the result of a statement it dispatches to
 * one of a gang's processes, which a "skip" fails as an allocation that
 * failed would (cdbdisp_makeResult(), cdbdispatchresult.c), ending the
 * session (cdbdisp_dispatchToGang_async(), cdbdisp_async.c): here as a
 * statement is sent to one of the gang's connections.
 */
static void
dispatch_result_fault(void)
{
	if (GP_FAULT("make_dispatch_result_error") == GP_FAULT_SKIP)
		elog(FATAL, "could not allocate resources for segworker communication");
}
