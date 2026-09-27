/* pg19/modules/gp_core/gp_core--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_core" to load this file. \quit

/*
 * "gp" holds what users call; "gp_internal" holds what the dispatcher calls on
 * a segment.  Anyone may reach the schema: the dispatcher connects to a
 * segment as the session's own user, and ANALYZE by a table's owner has to be
 * able to call sample_rows() there.  So every function in it either checks the
 * caller's privileges itself or has EXECUTE revoked from PUBLIC.
 *
 * Each is the extension's own, made here: the control file names pg_catalog,
 * where CREATE EXTENSION makes no schema, so that pg_dump writes none of them
 * apart from CREATE EXTENSION -- which a restore passes over where the
 * database has gp_core already, as every database a cluster's coordinator
 * makes has (gp_ddl.c).  Every object this script makes is named with its
 * schema.
 */
CREATE SCHEMA gp;
CREATE SCHEMA IF NOT EXISTS gp_internal;
GRANT USAGE ON SCHEMA gp_internal TO PUBLIC;

/*
 * gp_toolkit: Cloudberry's schema of views and functions of the cluster.
 * The port's modules each make their own objects in it -- gp_ao's of its
 * tables, gp_resource's of the queues and the groups -- so it is made here,
 * where every one of them finds it, rather than by the first of them, which
 * would own it.
 */
CREATE SCHEMA IF NOT EXISTS gp_toolkit;
GRANT USAGE ON SCHEMA gp_toolkit TO PUBLIC;

/*
 * And "gp" is anyone's to reach as well, as Cloudberry's gp_dist_random() and
 * catalog views are: gp.dist_random() reads a relation through a query run
 * as the caller, here and on the segments, so it reads only what the caller
 * may, and the one function in it that runs any statement,
 * exec_on_segments(), has EXECUTE revoked from PUBLIC below.
 */
GRANT USAGE ON SCHEMA gp TO PUBLIC;

/*
 * A segment's sample of a table, for ANALYZE on the coordinator (O3).  The
 * first row is the segment's live and dead row counts; every other row is a
 * sampled row of the table's own type.  It checks that the caller may read or
 * ANALYZE the table, and is not STRICT because NULL::t is how it is told
 * which table.
 */
CREATE FUNCTION gp_internal.sample_rows(
	rel anyelement,
	targrows int,
	OUT totalrows float8,
	OUT totaldeadrows float8,
	OUT sample anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_sample_rows'
LANGUAGE C;

/*
 * What a Motion sends a segment: a plan fragment, which the segment's planner
 * hook carries out in place of this call, and the key its Motions' rows are
 * kept under (gp_motion.c).  Never run itself, and only from a connection
 * that carries the cluster secret.
 */
CREATE FUNCTION gp_internal.exec_fragment(fragment text, motion_key text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_exec_fragment'
LANGUAGE C STRICT;

/*
 * A segment told that the COPY the coordinator sends next brings a
 * materialized view its rows, as its REFRESH on a cluster fills it
 * (gp_refresh.c).  Only from a connection that carries the cluster secret.
 */
CREATE FUNCTION gp_internal.matview_fill(view regclass, phase text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_matview_fill'
LANGUAGE C STRICT VOLATILE;

/*
 * A batch of a Motion's rows, relayed to the segment that receives them, and
 * the statement's word that it is done with them.  Both only from a
 * connection that carries the cluster secret.
 */
CREATE FUNCTION gp_internal.motion_put(motion_key text, slice int, rows bytea)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_motion_put'
LANGUAGE C STRICT;

CREATE FUNCTION gp_internal.motion_drop(motion_key text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_motion_drop'
LANGUAGE C STRICT;

/*
 * A batch of a Motion's rows, relayed to a segment whose reader, rather than
 * its writer, receives them: kept in files the reader can open.  Only from a
 * connection that carries the cluster secret.
 */
CREATE FUNCTION gp_internal.motion_put_shared(motion_key text, slice int,
											  rows bytea)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_motion_put_shared'
LANGUAGE C STRICT;

/*
 * Where a segment process receives the rows of a Motion whose slices run at
 * once (gp.interconnect_type = tcp or udpifc), opening its listener and its
 * datagram socket on first use.  Only from a connection that carries the
 * cluster secret.
 */
CREATE FUNCTION gp_internal.interconnect_address()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_interconnect_address'
LANGUAGE C STRICT VOLATILE;

/*
 * A segment's map of the distributed transactions prepared on it, or
 * committed there in one phase: each one's coordinator transaction ID, its
 * local one, whether its second phase has come and what it was, how many
 * subtransactions it committed (NULL when a restart left it prepared and
 * they are not known), and whether it committed in one phase.  See gp_dtx.c.
 */
CREATE FUNCTION gp_internal.dtx_map(
	OUT gxid xid8, OUT xid xid, OUT done bool, OUT committed bool,
	OUT subtransactions int, OUT one_phase bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dtx_map'
LANGUAGE C STRICT VOLATILE;

/*
 * The map as it is logged, in each database of a segment: a row for each
 * part of a distributed transaction that prepared here, or committed here in
 * one phase, written by the part itself -- so that it commits with it, and a
 * restart and a mirror have it from the WAL -- and deleted by a later one
 * once no distributed snapshot can see it in progress.  Cloudberry's
 * distributed log (distributedlog.c).  A table of each node's, as the
 * extension's own are, and a heap whatever default_table_access_method says,
 * which gp_dtx.c alone writes, with the heap's own functions.  See gp_dtx.c.
 */
CREATE TABLE gp_internal.distributed_log (
	gxid		xid8 NOT NULL,		-- the coordinator's transaction
	xid			xid8 NOT NULL,		-- the part's own, here
	one_phase	bool NOT NULL,
	children	xid[]				-- the subtransactions it committed
) USING heap;
CREATE INDEX distributed_log_gxid ON gp_internal.distributed_log (gxid);

/*
 * WHERE CURRENT OF a cursor whose plan gathered the table (gp_scan.c): the
 * row the cursor is on, at the ctid the cursor read it at, is read again on
 * its segment under the statement's snapshot -- in the version that snapshot
 * sees, following the row's updates since, as PostgreSQL's TID scan does
 * for WHERE CURRENT OF (TidNext()).  The ctid itself for a table that is not
 * heap.  STABLE, so that the segment's planner scans by the TID it returns.
 */
CREATE FUNCTION gp_internal.current_tid(rel oid, ctid tid)
RETURNS tid
AS 'MODULE_PATHNAME', 'gp_current_tid'
LANGUAGE C STRICT STABLE;

/*
 * The loopback's journal (gp_loopback.c): where this server cannot prepare, a
 * transaction that writes to another of its databases through the loopback
 * leaves its part there open until its own commit is recorded, and records
 * here, with its own commit, the part's statements, the part's transaction
 * there and who ran them -- so that a part a crash or a lost connection took
 * before its COMMIT is written there again by distributed transaction
 * recovery, once (gp_dtx.c).  Written with the heap's own functions, as
 * gp_internal.distributed_log is.
 */
CREATE TABLE gp_internal.loopback_journal (
	xid			xid8 NOT NULL,		-- the transaction here
	dbname		name NOT NULL,		-- the database it wrote to
	part_xid	xid8 NOT NULL,		-- its part's transaction there
	session_role name NOT NULL,		-- the part's session user
	current_role_name name,			-- and current user, where another
	statements	text[] NOT NULL
) USING heap;

/*
 * Whether this node's distributed transaction recovery has reached every node
 * since the server started: Cloudberry's "DTM recovered", which its pg_ctl
 * waits for on a coordinator, and gpstart polls for.  True on a node that
 * runs none.  See gp_dtx.c.
 */
CREATE FUNCTION gp.dtx_recovered()
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_dtx_recovered'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION gp.dtx_recovered() IS
	'whether distributed transaction recovery has reached every node since the server started';

/*
 * What the coordinator's distributed transaction recovery is doing, while a
 * round of it runs: Cloudberry's gp_stat_progress_dtx_recovery, the round's
 * phase and its counts of distributed transactions -- the committed ones it
 * finishes on the nodes, and the ones in doubt, still in progress or rolled
 * back.  No row between rounds.  See gp_dtx.c.
 */
CREATE FUNCTION gp_internal.dtx_recovery_progress(
	OUT phase int, OUT recover_commited_dtx_total bigint,
	OUT recover_commited_dtx_completed bigint, OUT in_doubt_tx_total bigint,
	OUT in_doubt_tx_in_progress bigint, OUT in_doubt_tx_aborted bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dtx_recovery_progress'
LANGUAGE C STRICT VOLATILE;

SET allow_system_table_mods = on;
CREATE VIEW pg_catalog.gp_stat_progress_dtx_recovery AS
	SELECT CASE p.phase
			   WHEN 0 THEN 'initializing'
			   WHEN 1 THEN 'recovering commited distributed transactions'
			   WHEN 2 THEN 'gathering in-doubt transactions'
			   WHEN 3 THEN 'aborting in-doubt transactions'
			   WHEN 4 THEN 'gathering in-doubt orphaned transactions'
			   WHEN 5 THEN 'managing in-doubt orphaned transactions'
		   END AS phase,
		   p.recover_commited_dtx_total, p.recover_commited_dtx_completed,
		   p.in_doubt_tx_total, p.in_doubt_tx_in_progress,
		   p.in_doubt_tx_aborted
	FROM gp_internal.dtx_recovery_progress() p;
RESET allow_system_table_mods;

GRANT SELECT ON pg_catalog.gp_stat_progress_dtx_recovery TO PUBLIC;

/*
 * Wait until this node's mirror has what the node has flushed: Cloudberry's
 * wait_for_mirror(), which the coordinator runs on a segment when a COMMIT
 * PREPARED it sends again finds the part committed already.  See gp_dtx.c.
 */
CREATE FUNCTION gp_internal.dtx_wait_mirror()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_dtx_wait_mirror'
LANGUAGE C STRICT VOLATILE;

/*
 * This node's waiting relations, as the global deadlock detector reads them:
 * each waiting backend and a backend that holds what it waits for, with the
 * coordinator session each works for (0: none), whether the lock lasts to
 * the end of the transaction, and its mode and kind.  See gp_gdd.c.
 */
CREATE FUNCTION gp_internal.dist_wait_status(
	OUT segid int, OUT waiter int, OUT holder int, OUT waiter_session int,
	OUT holder_session int, OUT solid bool, OUT lockmode text,
	OUT locktype text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dist_wait_status'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION gp.version()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_version'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION gp.version() IS
	'Apache Cloudberry version, as version() reports it on Cloudberry itself';

/*
 * What this node thinks it is.
 *
 * "segments" is how many segments to compute with, and is never 0: a consumer
 * divides by it -- ORCA asserts 0 < segments and its skew model computes
 * 1.0 / segments -- which is why Cloudberry's own getgpsegmentCount() answers
 * 1 for a singleton.  Whether this server has segments configured at all is
 * the separate question "single_node" answers.
 */
CREATE FUNCTION gp.node(
	OUT role text,
	OUT segments int,
	OUT content_id int,
	OUT single_node boolean,
	OUT dbid int)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_node'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp.node() IS
	'the role, segment count, content id, single-node flag and dbid of this node';

/*
 * The cluster, as this node knows it.
 *
 * Cloudberry keeps this in gp_segment_configuration, a shared catalog.  An
 * extension can create no shared catalog, and the answer is needed before any
 * database is open, so the port reads it from the file "gp.cluster_config"
 * names -- which is the step Cloudberry's own external-FTS builds already take,
 * where the rows come from etcd and the catalog becomes a view over a function.
 *
 * "mode" and "status" are not here: they are what FTS maintains, and FTS is
 * M4's.
 */
CREATE FUNCTION gp.segment_configuration(
	OUT dbid int,
	OUT content int,
	OUT role text,
	OUT hostname text,
	OUT port int,
	OUT datadir text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_segment_configuration'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp.segment_configuration() IS
	'the nodes of this cluster, as the cluster configuration file lists them';

/*
 * Cloudberry's catalogs, by Cloudberry's names and in its columns, where its
 * own are: pg_catalog, so that a query written for Cloudberry finds them
 * whatever the search path is.  The port keeps what they hold elsewhere --
 * the cluster in a file, a table's distribution in its "gp" label -- and
 * three of them are shared catalogs in Cloudberry, of which an extension can
 * make none; so each database has them as this script makes them there
 * (gp_catalog.c).  PostgreSQL makes no relation in pg_catalog unless
 * allow_system_table_mods is on; a script's setting lasts as long as the
 * script.
 */
SET allow_system_table_mods = on;

/*
 * gp_id: one row on every node, whose contents, Cloudberry's gp_id.dat says,
 * do not matter.  What it is for is gp_dist_random('gp_id'), which runs a
 * query once on each segment.
 */
CREATE VIEW pg_catalog.gp_id AS
	SELECT 'Cloudberry'::name AS gpname, (-1)::int2 AS numsegments,
		   (-1)::int2 AS dbid, (-1)::int2 AS content;

CREATE FUNCTION gp_internal.segment_configuration(
	OUT dbid int2,
	OUT content int2,
	OUT role "char",
	OUT preferred_role "char",
	OUT mode "char",
	OUT status "char",
	OUT port int4,
	OUT hostname text,
	OUT address text,
	OUT datadir text,
	OUT warehouseid oid)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_catalog_segment_configuration'
LANGUAGE C STRICT;

/*
 * gp_segment_configuration: the nodes the configuration file lists, a view
 * over a function as Cloudberry's external-FTS builds make it
 * (external_fts.sql).  Mode and status are FTS's, which is M4's.
 */
CREATE VIEW pg_catalog.gp_segment_configuration AS
	SELECT * FROM gp_internal.segment_configuration();

/*
 * gp_configuration_history: what FTS records of each change it makes to the
 * cluster, and what a user writes, with allow_system_table_mods on, as a
 * catalog is written.  FTS's rows are a file in the coordinator's data
 * directory (gp_fts.c), which every database reads, as every database reads
 * Cloudberry's shared catalog; a user's are kept in the database they were
 * written in, in gp_internal.configuration_history, which a row written to
 * the view goes to.
 */
CREATE FUNCTION gp_internal.fts_history(
	OUT "time" timestamptz,
	OUT dbid int2,
	OUT "desc" text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_fts_history'
LANGUAGE C STRICT VOLATILE;

CREATE TABLE gp_internal.configuration_history (
	"time" timestamptz NOT NULL,
	dbid int2 NOT NULL,
	"desc" text
) USING heap;

CREATE VIEW pg_catalog.gp_configuration_history AS
	SELECT * FROM gp_internal.fts_history()
	UNION ALL
	SELECT * FROM gp_internal.configuration_history;

CREATE FUNCTION gp_internal.catalog_write_check()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_catalog_write_check'
LANGUAGE C;

CREATE TRIGGER gp_catalog_write_check
	BEFORE INSERT OR UPDATE OR DELETE
	ON pg_catalog.gp_configuration_history
	FOR EACH STATEMENT EXECUTE FUNCTION gp_internal.catalog_write_check();

CREATE FUNCTION gp_internal.configuration_history_write()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_catalog_configuration_history_write'
LANGUAGE C;

CREATE TRIGGER gp_configuration_history_write
	INSTEAD OF INSERT OR UPDATE OR DELETE
	ON pg_catalog.gp_configuration_history
	FOR EACH ROW EXECUTE FUNCTION gp_internal.configuration_history_write();

/*
 * pg_stat_last_operation and pg_stat_last_shoperation, Cloudberry's catalogs
 * of when each object was last created, altered, vacuumed, analyzed or
 * truncated, and by whom: a row for each object and kind of operation, the
 * next of that kind replacing it, and none once the object is dropped.  A
 * cluster's coordinator writes them (gp_metatrack.c), into these tables of
 * each database -- a shared object's too, where Cloudberry's are in a shared
 * catalog -- and the views are what Cloudberry's catalogs are, written only
 * with allow_system_table_mods on: the check is the tables', whose statement
 * triggers are the ones a write to the views fires.
 */
CREATE TABLE gp_internal.stat_last_operation (
	classid oid NOT NULL,
	objid oid NOT NULL,
	staactionname name NOT NULL,
	stasysid oid NOT NULL,
	stausename name NOT NULL,
	stasubtype text,
	statime timestamptz
) USING heap;
CREATE UNIQUE INDEX stat_last_operation_key
	ON gp_internal.stat_last_operation (classid, objid, staactionname);

CREATE TABLE gp_internal.stat_last_shoperation (
	classid oid NOT NULL,
	objid oid NOT NULL,
	staactionname name NOT NULL,
	stasysid oid NOT NULL,
	stausename name NOT NULL,
	stasubtype text,
	statime timestamptz
) USING heap;
CREATE UNIQUE INDEX stat_last_shoperation_key
	ON gp_internal.stat_last_shoperation (classid, objid, staactionname);

CREATE VIEW pg_catalog.pg_stat_last_operation AS
	SELECT * FROM gp_internal.stat_last_operation;
CREATE VIEW pg_catalog.pg_stat_last_shoperation AS
	SELECT * FROM gp_internal.stat_last_shoperation;
GRANT SELECT ON pg_catalog.pg_stat_last_operation,
	pg_catalog.pg_stat_last_shoperation TO PUBLIC;

CREATE TRIGGER gp_catalog_write_check
	BEFORE INSERT OR UPDATE OR DELETE
	ON gp_internal.stat_last_operation
	FOR EACH STATEMENT EXECUTE FUNCTION gp_internal.catalog_write_check();
CREATE TRIGGER gp_catalog_write_check
	BEFORE INSERT OR UPDATE OR DELETE
	ON gp_internal.stat_last_shoperation
	FOR EACH STATEMENT EXECUTE FUNCTION gp_internal.catalog_write_check();

/*
 * gp_distribution_policy: how each table's rows are spread, as its "gp"
 * label records it (gp_policy.c), in Cloudberry's columns; on one node,
 * nothing, as Cloudberry's single node keeps no policy.  A row written to it
 * -- with allow_system_table_mods on, as Cloudberry's catalog is written --
 * writes the label and moves no row, as Cloudberry's does not: its tests
 * make a table of fewer segments so, and one on the coordinator alone by
 * deleting its row.
 */
CREATE FUNCTION gp_internal.distribution_policy(
	OUT localoid oid,
	OUT policytype "char",
	OUT numsegments int4,
	OUT distkey int2vector,
	OUT distclass oidvector)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_catalog_distribution_policy'
LANGUAGE C STRICT STABLE;

CREATE VIEW pg_catalog.gp_distribution_policy AS
	SELECT * FROM gp_internal.distribution_policy();

CREATE FUNCTION gp_internal.distribution_policy_write()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_catalog_distribution_policy_write'
LANGUAGE C;

CREATE TRIGGER gp_catalog_write_check
	BEFORE INSERT OR UPDATE OR DELETE
	ON pg_catalog.gp_distribution_policy
	FOR EACH STATEMENT EXECUTE FUNCTION gp_internal.catalog_write_check();

CREATE TRIGGER gp_distribution_policy_write
	INSTEAD OF INSERT OR UPDATE OR DELETE
	ON pg_catalog.gp_distribution_policy
	FOR EACH ROW EXECUTE FUNCTION gp_internal.distribution_policy_write();

/*
 * gp_stat_replication: the WAL senders of this node, in Cloudberry's columns,
 * which name the node: the coordinator's to its standby as -1.  On the
 * coordinator, and as Cloudberry's does, a row too for each segment that has
 * a mirror (M4): its primary's WAL sender to it, gathered from the primaries
 * (gp_fts.c), and NULLs where the mirror is gone.  spill_* are what
 * Cloudberry reads of a logical sender's decoding, which a physical sender
 * has none of, and sync_error what FTS says of a mirror, "none" as Cloudberry
 * says it without one.
 */
CREATE FUNCTION gp_internal.segment_replication(
	OUT gp_segment_id int,
	OUT walsender json)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_segment_replication'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_catalog.gp_stat_replication AS
	SELECT (SELECT n.content_id FROM gp.node() n) AS gp_segment_id,
		   r.pid, r.usesysid, r.usename, r.application_name, r.client_addr,
		   r.client_hostname, r.client_port, r.backend_start, r.backend_xmin,
		   r.state, r.sent_lsn, r.write_lsn, r.flush_lsn, r.replay_lsn,
		   r.write_lag, r.flush_lag, r.replay_lag, r.sync_priority,
		   r.sync_state, r.reply_time,
		   NULL::int8 AS spill_txns, NULL::int8 AS spill_count,
		   NULL::int8 AS spill_bytes, 'none'::text AS sync_error
	  FROM pg_catalog.pg_stat_replication r
	UNION ALL
	SELECT s.gp_segment_id,
		   r.pid, r.usesysid, r.usename, r.application_name, r.client_addr,
		   r.client_hostname, r.client_port, r.backend_start, r.backend_xmin,
		   r.state, r.sent_lsn, r.write_lsn, r.flush_lsn, r.replay_lsn,
		   r.write_lag, r.flush_lag, r.replay_lag, r.sync_priority,
		   r.sync_state, r.reply_time,
		   NULL::int8, NULL::int8, NULL::int8, 'none'::text
	  FROM gp_internal.segment_replication() s
	  LEFT JOIN LATERAL pg_catalog.json_populate_record(
		  NULL::pg_catalog.pg_stat_replication, s.walsender) r ON true;

GRANT SELECT ON pg_catalog.gp_id, pg_catalog.gp_segment_configuration,
	pg_catalog.gp_configuration_history, pg_catalog.gp_distribution_policy,
	pg_catalog.gp_stat_replication
	TO PUBLIC;

RESET allow_system_table_mods;

CREATE FUNCTION pg_catalog.gp_stat_force_next_flush()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_stat_force_next_flush'
LANGUAGE C;

/*
 * Each of these stands for a catalog table of Cloudberry's, which has
 * gp_segment_id, as every table of Cloudberry's has: a view so labelled
 * has the column too, the node's own content id (gp_segment.c).
 */
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_id IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_segment_configuration IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_configuration_history IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_distribution_policy IS 'catalog';

/*
 * Run a statement on every segment, and report what each one said.
 *
 * The answer is the first column of the first row, as text: this is for asking
 * a cluster about itself -- what each segment thinks it is, how many rows each
 * one holds -- and not for reading a table, which is what a scan of a
 * distributed table does.
 *
 * Superuser only: it runs arbitrary SQL on a machine the caller may have no
 * other way to reach.
 */
CREATE FUNCTION gp.exec_on_segments(
	sql text,
	OUT content int,
	OUT result text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_exec_on_segments'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp.exec_on_segments(text) FROM PUBLIC;

COMMENT ON FUNCTION gp.exec_on_segments(text) IS
	'run a statement on every segment and report the first column of each answer';

/*
 * The rows of a relation as the segments hold them.
 *
 * Cloudberry's gp_dist_random('t') has its result type filled in by its own
 * planner, which an extension cannot do; polymorphism gives the same thing --
 * the argument is a value of the relation's row type, so NULL::t says which
 * relation without reading one, and the result is a set of that type:
 *
 *     SELECT * FROM gp.dist_random(NULL::t);
 *
 * It is a scan of a distributed table with nothing planned around it.
 */
CREATE FUNCTION gp.dist_random(rel anyelement)
RETURNS SETOF anyelement
AS 'MODULE_PATHNAME', 'gp_dist_random'
LANGUAGE C;

COMMENT ON FUNCTION gp.dist_random(anyelement) IS
	'the rows of a relation as the segments hold them (Apache Cloudberry: gp_dist_random)';

/*
 * gp_segment_id, Cloudberry's system column, which the parser makes of the
 * name where no column has it (O10, gp_segment.c): segment_of(t.*) is the
 * segment that holds the row, and a segment answers it for itself.  Neither
 * is called by name.  dist_random_segments() stands in for gp.dist_random()
 * when a query asks which segment each copy came from, and takes its result
 * columns from the parser.
 */
CREATE FUNCTION gp_internal.segment_of(record)
RETURNS int
AS 'MODULE_PATHNAME', 'gp_segment_of'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp_internal.dist_random_segments(rel anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dist_random_segments'
LANGUAGE C;

/*
 * A query of gp_dist_random() alone that calls a function which is not
 * immutable, run on every segment, as Cloudberry runs a query over
 * gp_dist_random('gp_id') (gp_segment.c): the planner makes the call, with
 * the query's text, and the result columns are the query's.  Called by name,
 * it runs only such a query, as whoever calls it.
 */
CREATE FUNCTION gp_internal.segment_query(sql text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_segment_query'
LANGUAGE C STRICT;

/*
 * The same, for a query whose $n are values the coordinator evaluates, given
 * after its text.  Not STRICT: a value may be null.
 */
CREATE FUNCTION gp_internal.segment_query(sql text, VARIADIC params "any")
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_segment_query'
LANGUAGE C;

/*
 * A record of no declared type as it travels between the nodes: its row type
 * described, then its columns, and made again on arrival of a row type
 * registered there (gp_record.c).  record_wire() gives a record unchanged as
 * this type, for a segment's query to send it so.
 */
CREATE TYPE gp_internal.record_wire;
CREATE FUNCTION gp_internal.record_wire_in(cstring, oid, int4)
RETURNS gp_internal.record_wire
AS 'MODULE_PATHNAME', 'gp_record_wire_in' LANGUAGE C STRICT STABLE;
CREATE FUNCTION gp_internal.record_wire_out(gp_internal.record_wire)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gp_record_wire_out' LANGUAGE C STRICT STABLE;
CREATE FUNCTION gp_internal.record_wire_recv(internal, oid, int4)
RETURNS gp_internal.record_wire
AS 'MODULE_PATHNAME', 'gp_record_wire_recv' LANGUAGE C STRICT STABLE;
CREATE FUNCTION gp_internal.record_wire_send(gp_internal.record_wire)
RETURNS bytea
AS 'MODULE_PATHNAME', 'gp_record_wire_send' LANGUAGE C STRICT STABLE;
CREATE TYPE gp_internal.record_wire (
	INPUT = gp_internal.record_wire_in,
	OUTPUT = gp_internal.record_wire_out,
	RECEIVE = gp_internal.record_wire_recv,
	SEND = gp_internal.record_wire_send,
	INTERNALLENGTH = VARIABLE,
	ALIGNMENT = double,
	STORAGE = extended
);
CREATE FUNCTION gp_internal.record_wire(record)
RETURNS gp_internal.record_wire
AS 'MODULE_PATHNAME', 'gp_record_wire' LANGUAGE C STRICT STABLE;

/*
 * A record of no declared type made from record_wire's text of it: what
 * stands in ORCA's plan for a constant of one, which would otherwise reach a
 * segment with the coordinator's typmod (orca.c).
 */
CREATE FUNCTION gp_internal.record_from_wire(text)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_record_from_wire' LANGUAGE C STRICT STABLE;

/*
 * The size functions, the cluster's (gp_size.c): each of PostgreSQL's here
 * and every segment's added, as Cloudberry's add them.  A call of
 * PostgreSQL's is made a call of the one here of its name and arguments
 * before a statement on the coordinator is planned.
 */
CREATE FUNCTION gp_internal.relation_size(regclass)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_relation_size' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.relation_size(regclass, text)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_relation_size' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.table_size(regclass)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_table_size' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.indexes_size(regclass)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_indexes_size' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.total_relation_size(regclass)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_total_relation_size' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.database_size(name)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_database_size_name' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.database_size(oid)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_database_size_oid' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.tablespace_size(name)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_tablespace_size_name' LANGUAGE C STRICT;
CREATE FUNCTION gp_internal.tablespace_size(oid)
RETURNS bigint AS 'MODULE_PATHNAME', 'gp_tablespace_size_oid' LANGUAGE C STRICT;

/*
 * pg_tablespace_location(), each node's: the location CREATE TABLESPACE was
 * given, where PostgreSQL's says the directory of the node's dbid under it
 * (gp_ddl.c).  A call of PostgreSQL's is made a call of this on every node
 * of a cluster, as the size functions' are on its coordinator.
 */
CREATE FUNCTION gp_internal.tablespace_location(oid)
RETURNS text AS 'MODULE_PATHNAME', 'gp_tablespace_location' LANGUAGE C STRICT;

/*
 * The planner's Split on a segment (gp_split.c): rows deleted by their table
 * and ctid, returned as t's rows, and their new versions inserted, routed
 * into t's partitions -- firing no trigger and applying no policy, as
 * Cloudberry's Split does neither.  Not STRICT, because NULL::t is how they
 * are told which table; only for a connection that carries the cluster
 * secret.
 */
CREATE FUNCTION gp_internal.split_delete(rel anyelement, ctids tid[],
	tables oid[], numbers int8[],
	OUT gp_n int8, OUT gp_toid oid, OUT gp_row anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_split_delete'
LANGUAGE C;

CREATE FUNCTION gp_internal.split_insert(rel anyelement, rows anyarray,
	tables oid[], numbers int8[],
	OUT gp_n int8, OUT gp_toid oid, OUT gp_row anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_split_insert'
LANGUAGE C;

/*
 * Why a statement of the explicit write wrote fewer of a segment's rows than
 * it was sent (gp_explicit.c, gp_split.c): a row another transaction updated
 * since the coordinator read it fails the statement, and one it deleted
 * fails it where "deleted" says so, 's' for a Split and 'm' for a MERGE.  Not
 * STRICT, because NULL::t is how it is told which table; for a user who may
 * update or delete the table's rows.
 */
CREATE FUNCTION gp_internal.explicit_recheck(rel anyelement, ctids tid[],
	tables oid[], deleted "char")
RETURNS void
AS 'MODULE_PATHNAME', 'gp_explicit_recheck'
LANGUAGE C;

/*
 * A sequence's next value in a slice the segments run (gp_seq.c): ORCA's
 * plan calls nextval() there as gp_internal.nextval(), and an identity
 * column's next value as gp_internal.identity_nextval(), and the segment
 * takes the values of the coordinator's sequence from
 * gp_internal.sequence_values() there, a block of the sequence's CACHE at a
 * time, over a connection that carries the cluster secret.  nextval()'s
 * privileges are checked as nextval() checks them, of the segment's current
 * user; an identity column's value is taken only for a plan the coordinator
 * sent.
 */
CREATE FUNCTION gp_internal.nextval(regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'gp_nextval'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.identity_nextval(regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'gp_identity_nextval'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.sequence_values(seq oid, n int4, identity bool,
	role oid)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'gp_sequence_values'
LANGUAGE C VOLATILE STRICT;

/*
 * pg_locks' mppsessionid and mppiswriter, which Cloudberry's pg_locks has as
 * columns and the parser makes of the names the same way (gp_segment.c):
 * the coordinator session the locking process works for, and whether it is
 * a query's writer rather than a segment's reader.  gp_segment_id of a
 * pg_locks row is segment_of()'s, this node's.
 */
CREATE FUNCTION gp_internal.lock_session(pg_catalog.pg_locks)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_lock_session'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.lock_writer(pg_catalog.pg_locks)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_lock_writer'
LANGUAGE C VOLATILE STRICT;

/*
 * pg_stat_activity's sess_id, which Cloudberry's pg_stat_activity has as a
 * column and the parser makes of the name the same way (gp_segment.c): the
 * coordinator session the backend works for, -1 for none.
 */
CREATE FUNCTION gp_internal.activity_session(pg_catalog.pg_stat_activity)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_activity_session'
LANGUAGE C VOLATILE STRICT;

/*
 * pg_proc's prodataaccess and proexeclocation, which Cloudberry's pg_proc has
 * as columns and the parser makes of the names the same way (gp_segment.c):
 * what the function does with SQL and where it runs, which the "gp" label
 * keeps (gp_sql's funcattr.c).
 */
CREATE FUNCTION gp_internal.proc_data_access(pg_catalog.pg_proc)
RETURNS "char"
AS 'MODULE_PATHNAME', 'gp_proc_data_access'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION gp_internal.proc_exec_location(pg_catalog.pg_proc)
RETURNS "char"
AS 'MODULE_PATHNAME', 'gp_proc_exec_location'
LANGUAGE C STABLE STRICT;

/*
 * gp_stat_activity: pg_stat_activity of every node, as Cloudberry's view of
 * that name gives it (its system_views_gp.in makes one of each pg_stat
 * view): the coordinator's rows and each segment's, with the content id of
 * the node, gp_segment_id, first, and sess_id, which each node works out for
 * its own backends (stat_activity, which gp.dist_random() reads on each).
 */
CREATE VIEW gp_internal.stat_activity AS
	SELECT a.*, gp_internal.activity_session(a) AS sess_id
	FROM pg_catalog.pg_stat_activity a;

SET allow_system_table_mods = on;
CREATE VIEW pg_catalog.gp_stat_activity AS
	SELECT -1 AS gp_segment_id, s.*
	FROM gp_internal.stat_activity s
	UNION ALL
	SELECT d.gp_segment_id, d.datid, d.datname, d.pid, d.leader_pid,
		   d.usesysid, d.usename, d.application_name, d.client_addr,
		   d.client_hostname, d.client_port, d.backend_start, d.xact_start,
		   d.query_start, d.state_change, d.wait_event_type, d.wait_event,
		   d.state, d.backend_xid, d.backend_xmin, d.query_id, d.query,
		   d.backend_type, d.sess_id
	FROM gp.dist_random(NULL::gp_internal.stat_activity) d;
RESET allow_system_table_mods;

GRANT SELECT ON gp_internal.stat_activity, pg_catalog.gp_stat_activity TO PUBLIC;

/*
 * How a relation's rows are spread over the segments.
 *
 * gp_sql.set_distribution() records what DISTRIBUTED BY said as text on the
 * "gp" label; this is the policy that text becomes, which is what ORCA's
 * relcache translator asks for about every relation it sees, and what M2's
 * dispatch will read.
 *
 * "kind" is one of hash, random, replicated or entry.  "columns" is the
 * distribution key by name, empty unless the kind is hash, and "opfamilies"
 * is the operator family each of those columns is hashed with -- PostgreSQL's
 * default hash family for the type, which is the same one Cloudberry chooses.
 *
 * NULL for a relation with no policy: not a missing answer, but what an
 * unlabelled relation means, which on one node is every relation nobody wrote
 * DISTRIBUTED BY for.  Readers take it as entry -- all the rows in one place.
 *
 * It raises if the label is there but cannot be read: a column it names that
 * the relation no longer has, a type that cannot be hashed, or a shape the
 * port does not know.  Answering "no policy" to any of those would plan the
 * wrong distribution instead of reporting the problem.
 */
CREATE FUNCTION gp.policy(
	rel regclass,
	OUT kind text,
	OUT columns text[],
	OUT opfamilies oid[],
	OUT numsegments int)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_policy'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp.policy(regclass) IS
	'how a relation''s rows are spread over the segments, or NULL for none';

/*
 * Hash operator classes for bit and bit varying, which Cloudberry's catalog
 * has and PostgreSQL's does not: without one a column of either type can be
 * no distribution key.  In pg_catalog under Cloudberry's names, beside the
 * types' btree classes, so that DISTRIBUTED BY (x bit_ops) finds them on any
 * search path; the hash function is Cloudberry's bithash (gp_hash.c).
 */
CREATE FUNCTION gp.bithash(bit)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_bithash'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.bithash(varbit)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_bithash'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR FAMILY pg_catalog.bit_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.bit_ops
	DEFAULT FOR TYPE bit USING hash FAMILY pg_catalog.bit_ops AS
	OPERATOR 1 = (bit, bit),
	FUNCTION 1 gp.bithash(bit);

CREATE OPERATOR FAMILY pg_catalog.varbit_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.varbit_ops
	DEFAULT FOR TYPE varbit USING hash FAMILY pg_catalog.varbit_ops AS
	OPERATOR 1 = (varbit, varbit),
	FUNCTION 1 gp.bithash(varbit);

/*
 * Cloudberry's legacy hash operator classes, cdbhash_*_ops: Greenplum 5's
 * hash, which a table distributed before Greenplum 6 keeps so that it need
 * not be redistributed, and which gp.use_legacy_hashops gives a new key.
 * gp_legacyhash.c has the functions, and why several columns of a key are
 * not hashed by them as by the others.  In pg_catalog under Cloudberry's
 * names, as its catalog has them, none the default for its type; made after
 * the classes above, so that where an equality operator is in both, the
 * planner's hash joins, which take its first hash family, keep the others.
 */
CREATE FUNCTION gp.cdblegacyhash_int2(int2)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_int2'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_int4(int4)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_int4'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_int8(int8)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_int8'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_float4(float4)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_float4'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_float8(float8)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_float8'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_numeric(numeric)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_numeric'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_char("char")
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_char'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_text(text)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_bpchar(bpchar)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_bytea(bytea)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_name(name)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_name'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_oid(oid)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_oid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_tid(tid)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_tid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_timestamp(timestamp)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_timestamptz(timestamptz)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_timestamp'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_date(date)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_date'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_time(time)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_time'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_timetz(timetz)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_timetz'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_interval(interval)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_interval'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_inet(inet)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_inet'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_macaddr(macaddr)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_macaddr'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_bit(bit)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_bit'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_bit(varbit)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_bit'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_bool(bool)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_bool'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_array(anyarray)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_array'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_oidvector(oidvector)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_oidvector'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_cash(money)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_cash'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_uuid(uuid)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_uuid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.cdblegacyhash_anyenum(anyenum)
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_cdblegacyhash_anyenum'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR FAMILY pg_catalog.cdbhash_integer_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_int2_ops
	FOR TYPE int2 USING hash FAMILY pg_catalog.cdbhash_integer_ops AS
	OPERATOR 1 = (int2, int2),
	FUNCTION 1 gp.cdblegacyhash_int2(int2);
CREATE OPERATOR CLASS pg_catalog.cdbhash_int4_ops
	FOR TYPE int4 USING hash FAMILY pg_catalog.cdbhash_integer_ops AS
	OPERATOR 1 = (int4, int4),
	FUNCTION 1 gp.cdblegacyhash_int4(int4);
CREATE OPERATOR CLASS pg_catalog.cdbhash_int8_ops
	FOR TYPE int8 USING hash FAMILY pg_catalog.cdbhash_integer_ops AS
	OPERATOR 1 = (int8, int8),
	FUNCTION 1 gp.cdblegacyhash_int8(int8);
-- the integers of all three sizes hash alike, so their equalities are the family's
ALTER OPERATOR FAMILY pg_catalog.cdbhash_integer_ops USING hash ADD
	OPERATOR 1 = (int2, int4),
	OPERATOR 1 = (int2, int8),
	OPERATOR 1 = (int4, int2),
	OPERATOR 1 = (int4, int8),
	OPERATOR 1 = (int8, int2),
	OPERATOR 1 = (int8, int4);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_float4_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_float4_ops
	FOR TYPE float4 USING hash FAMILY pg_catalog.cdbhash_float4_ops AS
	OPERATOR 1 = (float4, float4),
	FUNCTION 1 gp.cdblegacyhash_float4(float4);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_float8_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_float8_ops
	FOR TYPE float8 USING hash FAMILY pg_catalog.cdbhash_float8_ops AS
	OPERATOR 1 = (float8, float8),
	FUNCTION 1 gp.cdblegacyhash_float8(float8);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_numeric_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_numeric_ops
	FOR TYPE numeric USING hash FAMILY pg_catalog.cdbhash_numeric_ops AS
	OPERATOR 1 = (numeric, numeric),
	FUNCTION 1 gp.cdblegacyhash_numeric(numeric);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_char_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_char_ops
	FOR TYPE "char" USING hash FAMILY pg_catalog.cdbhash_char_ops AS
	OPERATOR 1 = ("char", "char"),
	FUNCTION 1 gp.cdblegacyhash_char("char");

CREATE OPERATOR FAMILY pg_catalog.cdbhash_text_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_text_ops
	FOR TYPE text USING hash FAMILY pg_catalog.cdbhash_text_ops AS
	OPERATOR 1 = (text, text),
	FUNCTION 1 gp.cdblegacyhash_text(text);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_bpchar_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_bpchar_ops
	FOR TYPE bpchar USING hash FAMILY pg_catalog.cdbhash_bpchar_ops AS
	OPERATOR 1 = (bpchar, bpchar),
	FUNCTION 1 gp.cdblegacyhash_bpchar(bpchar);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_bytea_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_bytea_ops
	FOR TYPE bytea USING hash FAMILY pg_catalog.cdbhash_bytea_ops AS
	OPERATOR 1 = (bytea, bytea),
	FUNCTION 1 gp.cdblegacyhash_bytea(bytea);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_name_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_name_ops
	FOR TYPE name USING hash FAMILY pg_catalog.cdbhash_name_ops AS
	OPERATOR 1 = (name, name),
	FUNCTION 1 gp.cdblegacyhash_name(name);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_oid_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_oid_ops
	FOR TYPE oid USING hash FAMILY pg_catalog.cdbhash_oid_ops AS
	OPERATOR 1 = (oid, oid),
	FUNCTION 1 gp.cdblegacyhash_oid(oid);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_tid_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_tid_ops
	FOR TYPE tid USING hash FAMILY pg_catalog.cdbhash_tid_ops AS
	OPERATOR 1 = (tid, tid),
	FUNCTION 1 gp.cdblegacyhash_tid(tid);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_timestamp_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_timestamp_ops
	FOR TYPE timestamp USING hash FAMILY pg_catalog.cdbhash_timestamp_ops AS
	OPERATOR 1 = (timestamp, timestamp),
	FUNCTION 1 gp.cdblegacyhash_timestamp(timestamp);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_timestamptz_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_timestamptz_ops
	FOR TYPE timestamptz USING hash FAMILY pg_catalog.cdbhash_timestamptz_ops AS
	OPERATOR 1 = (timestamptz, timestamptz),
	FUNCTION 1 gp.cdblegacyhash_timestamptz(timestamptz);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_date_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_date_ops
	FOR TYPE date USING hash FAMILY pg_catalog.cdbhash_date_ops AS
	OPERATOR 1 = (date, date),
	FUNCTION 1 gp.cdblegacyhash_date(date);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_time_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_time_ops
	FOR TYPE time USING hash FAMILY pg_catalog.cdbhash_time_ops AS
	OPERATOR 1 = (time, time),
	FUNCTION 1 gp.cdblegacyhash_time(time);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_timetz_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_timetz_ops
	FOR TYPE timetz USING hash FAMILY pg_catalog.cdbhash_timetz_ops AS
	OPERATOR 1 = (timetz, timetz),
	FUNCTION 1 gp.cdblegacyhash_timetz(timetz);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_interval_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_interval_ops
	FOR TYPE interval USING hash FAMILY pg_catalog.cdbhash_interval_ops AS
	OPERATOR 1 = (interval, interval),
	FUNCTION 1 gp.cdblegacyhash_interval(interval);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_inet_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_inet_ops
	FOR TYPE inet USING hash FAMILY pg_catalog.cdbhash_inet_ops AS
	OPERATOR 1 = (inet, inet),
	FUNCTION 1 gp.cdblegacyhash_inet(inet);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_macaddr_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_macaddr_ops
	FOR TYPE macaddr USING hash FAMILY pg_catalog.cdbhash_macaddr_ops AS
	OPERATOR 1 = (macaddr, macaddr),
	FUNCTION 1 gp.cdblegacyhash_macaddr(macaddr);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_bit_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_bit_ops
	FOR TYPE bit USING hash FAMILY pg_catalog.cdbhash_bit_ops AS
	OPERATOR 1 = (bit, bit),
	FUNCTION 1 gp.cdblegacyhash_bit(bit);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_varbit_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_varbit_ops
	FOR TYPE varbit USING hash FAMILY pg_catalog.cdbhash_varbit_ops AS
	OPERATOR 1 = (varbit, varbit),
	FUNCTION 1 gp.cdblegacyhash_bit(varbit);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_bool_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_bool_ops
	FOR TYPE bool USING hash FAMILY pg_catalog.cdbhash_bool_ops AS
	OPERATOR 1 = (bool, bool),
	FUNCTION 1 gp.cdblegacyhash_bool(bool);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_array_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_array_ops
	FOR TYPE anyarray USING hash FAMILY pg_catalog.cdbhash_array_ops AS
	OPERATOR 1 = (anyarray, anyarray),
	FUNCTION 1 gp.cdblegacyhash_array(anyarray);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_oidvector_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_oidvector_ops
	FOR TYPE oidvector USING hash FAMILY pg_catalog.cdbhash_oidvector_ops AS
	OPERATOR 1 = (oidvector, oidvector),
	FUNCTION 1 gp.cdblegacyhash_oidvector(oidvector);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_cash_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_cash_ops
	FOR TYPE money USING hash FAMILY pg_catalog.cdbhash_cash_ops AS
	OPERATOR 1 = (money, money),
	FUNCTION 1 gp.cdblegacyhash_cash(money);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_uuid_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_uuid_ops
	FOR TYPE uuid USING hash FAMILY pg_catalog.cdbhash_uuid_ops AS
	OPERATOR 1 = (uuid, uuid),
	FUNCTION 1 gp.cdblegacyhash_uuid(uuid);

CREATE OPERATOR FAMILY pg_catalog.cdbhash_enum_ops USING hash;
CREATE OPERATOR CLASS pg_catalog.cdbhash_enum_ops
	FOR TYPE anyenum USING hash FAMILY pg_catalog.cdbhash_enum_ops AS
	OPERATOR 1 = (anyenum, anyenum),
	FUNCTION 1 gp.cdblegacyhash_anyenum(anyenum);

/*
 * median(x): Cloudberry's, as a plain aggregate rather than the ordered-set
 * one Cloudberry's grammar makes of it -- gp_median.c says why, and that the
 * answer is percentile_cont(0.5)'s.  One per type Cloudberry has.
 *
 * The aggregates are in pg_catalog, where Cloudberry has median, so that
 * median(x) finds one whatever the search path is and a view prints it as
 * median(x); their support functions are gp's.
 *
 * The final functions sort the state, so they are SHAREABLE: two median(x)
 * in one query may share it, and no row can follow.  SSPACE is what a
 * group's sort costs before its first row -- three memory contexts and an
 * array of 1,024 sort tuples -- so that the planner does not take a group's
 * state for one pointer when it weighs hashing the groups.
 *
 * A window, which calls the final function for every row and adds the next
 * ones after, needs a final function that changes nothing.  The moving-
 * aggregate functions are that: two heaps of the frame's values, a row added
 * and removed in a logarithm of the frame, and a final function that reads
 * the heaps' tops, READ_ONLY -- which makes a window take them for any frame.
 */
CREATE FUNCTION gp.median_transfn(internal, float8)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_transfn(internal, interval)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_transfn(internal, timestamp)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_transfn(internal, timestamptz)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_mtransfn(internal, float8)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_mtransfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_mtransfn(internal, interval)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_mtransfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_mtransfn(internal, timestamp)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_mtransfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_mtransfn(internal, timestamptz)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_mtransfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_minvfn(internal, float8)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_minvfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_minvfn(internal, interval)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_minvfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_minvfn(internal, timestamp)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_minvfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_minvfn(internal, timestamptz)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_median_minvfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_float8_mfinal(internal)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_median_mfinalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_interval_mfinal(internal)
RETURNS interval
AS 'MODULE_PATHNAME', 'gp_median_mfinalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_timestamp_mfinal(internal)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_median_mfinalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_timestamptz_mfinal(internal)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_median_mfinalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_float8_final(internal)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_median_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_interval_final(internal)
RETURNS interval
AS 'MODULE_PATHNAME', 'gp_median_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_timestamp_final(internal)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_median_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.median_timestamptz_final(internal)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_median_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE AGGREGATE pg_catalog.median(float8) (
	SFUNC = gp.median_transfn,
	STYPE = internal,
	SSPACE = 49152,
	FINALFUNC = gp.median_float8_final,
	FINALFUNC_MODIFY = SHAREABLE,
	MSFUNC = gp.median_mtransfn,
	MINVFUNC = gp.median_minvfn,
	MSTYPE = internal,
	MFINALFUNC = gp.median_float8_mfinal,
	MFINALFUNC_MODIFY = READ_ONLY,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.median(interval) (
	SFUNC = gp.median_transfn,
	STYPE = internal,
	SSPACE = 49152,
	FINALFUNC = gp.median_interval_final,
	FINALFUNC_MODIFY = SHAREABLE,
	MSFUNC = gp.median_mtransfn,
	MINVFUNC = gp.median_minvfn,
	MSTYPE = internal,
	MFINALFUNC = gp.median_interval_mfinal,
	MFINALFUNC_MODIFY = READ_ONLY,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.median(timestamp) (
	SFUNC = gp.median_transfn,
	STYPE = internal,
	SSPACE = 49152,
	FINALFUNC = gp.median_timestamp_final,
	FINALFUNC_MODIFY = SHAREABLE,
	MSFUNC = gp.median_mtransfn,
	MINVFUNC = gp.median_minvfn,
	MSTYPE = internal,
	MFINALFUNC = gp.median_timestamp_mfinal,
	MFINALFUNC_MODIFY = READ_ONLY,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.median(timestamptz) (
	SFUNC = gp.median_transfn,
	STYPE = internal,
	SSPACE = 49152,
	FINALFUNC = gp.median_timestamptz_final,
	FINALFUNC_MODIFY = SHAREABLE,
	MSFUNC = gp.median_mtransfn,
	MINVFUNC = gp.median_minvfn,
	MSTYPE = internal,
	MFINALFUNC = gp.median_timestamptz_mfinal,
	MFINALFUNC_MODIFY = READ_ONLY,
	PARALLEL = SAFE
);

COMMENT ON AGGREGATE pg_catalog.median(float8) IS
	'median, as percentile_cont(0.5) computes it (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.median(interval) IS
	'median, as percentile_cont(0.5) computes it (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.median(timestamp) IS
	'median, as percentile_cont(0.5) computes it (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.median(timestamptz) IS
	'median, as percentile_cont(0.5) computes it (Apache Cloudberry)';

/*
 * FTS (gp_fts.c).  gp_request_fts_probe_scan(): probe the segments now, and
 * return once a probe that began after the call has ended, as Cloudberry's
 * does -- at once where no prober runs, as on a coordinator whose segments
 * have no mirrors.  On the coordinator only.
 */
CREATE FUNCTION pg_catalog.gp_request_fts_probe_scan()
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_request_fts_probe_scan'
LANGUAGE C VOLATILE;

/*
 * Cloudberry's segment administration functions (gp_segadmin.c), which its
 * tools call to add, remove and put elsewhere the cluster's mirrors and its
 * standby: on the coordinator, by a superuser, in a transaction -- the
 * session sees its changes at once, the others once it commits, when they
 * are written to the cluster configuration file and the coordinator's live
 * copy of it; a rollback undoes them.  Their names and arguments are
 * Cloudberry's; none is granted to anybody.
 */
CREATE FUNCTION pg_catalog.gp_add_segment_primary(text, text, int4, text)
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_add_segment_primary'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_add_segment(int2, int2, "char", "char", "char", "char", int4, text, text, text)
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_add_segment'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_remove_segment(int2)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_remove_segment'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_add_segment_mirror(int2, text, text, int4, text)
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_add_segment_mirror'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_remove_segment_mirror(int2)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_remove_segment_mirror'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_add_master_standby(text, text, text)
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_add_master_standby'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_add_master_standby(text, text, text, int4)
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_add_master_standby_port'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_remove_master_standby()
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_remove_master_standby'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_catalog.gp_update_segment_configuration_mode_status(int4, "char", "char")
RETURNS int2
AS 'MODULE_PATHNAME', 'gp_update_segment_configuration_mode_status'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

/*
 * gp_activate_standby(): on a standby promoted with pg_ctl, what Cloudberry's
 * startup process does to its catalog as it promotes one -- the old
 * coordinator gone, this node the coordinator -- called by the tool that
 * activates it (gpactivatestandby), since the port's promotion changes no
 * node.
 */
CREATE FUNCTION pg_catalog.gp_activate_standby()
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_activate_standby'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION pg_catalog.gp_add_segment_primary(text, text, int4, text),
	pg_catalog.gp_add_segment(int2, int2, "char", "char", "char", "char", int4, text, text, text),
	pg_catalog.gp_remove_segment(int2),
	pg_catalog.gp_add_segment_mirror(int2, text, text, int4, text),
	pg_catalog.gp_remove_segment_mirror(int2),
	pg_catalog.gp_add_master_standby(text, text, text),
	pg_catalog.gp_add_master_standby(text, text, text, int4),
	pg_catalog.gp_remove_master_standby(),
	pg_catalog.gp_update_segment_configuration_mode_status(int4, "char", "char"),
	pg_catalog.gp_activate_standby()
	FROM PUBLIC;

/******************************************************************************
 * gp_toolkit, Cloudberry's (gpcontrib/gp_toolkit, gp_toolkit--1.3.sql and the
 * update scripts after it): its views and functions of what gp_core has what
 * they read.  Its append-optimized tables' are gp_ao's, beside gp_ao's own
 * functions there, and its resource managers' gp_resource's.  Not here: the
 * external tables of the servers' logs and the views over them, which read
 * Cloudberry's own log format; the workfile manager's views, whose manager
 * the port has not; and the checks for orphaned and missing files.
 *****************************************************************************/

/*
 * The content id of the node the call runs on, -1 on the coordinator:
 * Cloudberry's gp_execution_segment() (mpp_execution_segment(), cdbvars.c).
 */
CREATE FUNCTION pg_catalog.gp_execution_segment()
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_execution_segment'
LANGUAGE C VOLATILE;

/* And its dbid: Cloudberry's gp_execution_dbid(). */
CREATE FUNCTION pg_catalog.gp_execution_dbid()
RETURNS int4
AS 'MODULE_PATHNAME', 'gp_execution_dbid'
LANGUAGE C VOLATILE;

/*
 * The session's backends: this one, each segment's writer and each reader,
 * as Cloudberry's gp_backend_info() gives them (cdbgang.c).  See
 * gp_dispatch.c.
 */
CREATE FUNCTION pg_catalog.gp_backend_info(OUT id int4, OUT type "char",
										   OUT content int4, OUT host text,
										   OUT port int4, OUT pid int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_backend_info'
LANGUAGE C VOLATILE;

/*
 * Each node of the cluster as Cloudberry's gp_pgdatabase() gives it
 * (cdbpgdatabase.c): whether it is a primary now, valid -- up -- and a
 * primary by its preference.
 */
SET allow_system_table_mods = on;
CREATE VIEW pg_catalog.gp_pgdatabase AS
	SELECT c.dbid::smallint AS dbid, c.role = 'p' AS isprimary,
		   c.content::smallint AS content,
		   c.status = 'u' AND c.mode IN ('s', 'n') AS valid,
		   c.preferred_role = 'p' AS definedprimary
	FROM pg_catalog.gp_segment_configuration c;
RESET allow_system_table_mods;

GRANT SELECT ON pg_catalog.gp_pgdatabase TO PUBLIC;

/*
 * Whether a table is append-optimized: Cloudberry's reads pg_appendonly,
 * which is gp_ao's; its tables are those of gp_ao's two methods -- not a
 * partitioned table, which PostgreSQL 19 gives the method its partitions
 * take, and Cloudberry no row of pg_appendonly.
 */
CREATE VIEW gp_toolkit.__gp_is_append_only AS
	SELECT pgc.oid AS iaooid,
		   coalesce(am.amname IN ('ao_row', 'ao_column') AND pgc.relkind <> 'p',
					false) AS iaotype
	FROM pg_catalog.pg_class pgc
	LEFT JOIN pg_catalog.pg_am am ON am.oid = pgc.relam;

CREATE VIEW gp_toolkit.__gp_fullname AS
	SELECT pgc.oid AS fnoid, nspname AS fnnspname, relname AS fnrelname
	FROM pg_catalog.pg_class pgc, pg_catalog.pg_namespace pgn
	WHERE pgc.relnamespace = pgn.oid;

CREATE VIEW gp_toolkit.__gp_user_namespaces AS
	SELECT oid AS aunoid, nspname AS aunnspname
	FROM pg_catalog.pg_namespace
	WHERE nspname NOT LIKE 'pg_%'
	  AND nspname <> 'gp_toolkit'
	  AND nspname <> 'information_schema';

CREATE VIEW gp_toolkit.__gp_user_tables AS
	SELECT fn.fnnspname AS autnspname, fn.fnrelname AS autrelname,
		   relkind AS autrelkind, reltuples AS autreltuples,
		   relpages AS autrelpages, relacl AS autrelacl, pgc.oid AS autoid,
		   pgc.reltoastrelid AS auttoastoid, pgc.relam AS autrelam
	FROM pg_catalog.pg_class pgc, gp_toolkit.__gp_fullname fn
	WHERE pgc.relnamespace IN (SELECT aunoid FROM gp_toolkit.__gp_user_namespaces)
	  AND pgc.relkind IN ('r', 'p', 'm')
	  AND pgc.relispopulated = 't'
	  AND pgc.oid = fn.fnoid;

CREATE VIEW gp_toolkit.__gp_user_data_tables_readable AS
	SELECT *
	FROM gp_toolkit.__gp_user_tables aut
	WHERE pg_catalog.has_table_privilege(aut.autoid, 'select');

CREATE VIEW gp_toolkit.__gp_number_of_segments AS
	SELECT count(*)::smallint AS numsegments
	FROM pg_catalog.gp_segment_configuration
	WHERE preferred_role = 'p' AND content >= 0;

/*
 * A setting's value on the coordinator and on each segment.  The name is
 * the one given, Cloudberry's, and the server's own for it is found as the
 * port's gpconfig finds it: the name, or Cloudberry's gp_foo or foo as the
 * port's gp.foo -- every Cloudberry setting is a module's here, dotted.  A
 * function Cloudberry runs on every segment (EXECUTE ON ALL SEGMENTS) runs
 * there here as a query of gp_dist_random('gp_id') alone does (gp_sql).
 */
CREATE TYPE gp_toolkit.gp_param_setting_t AS (
	paramsegment int,
	paramname text,
	paramvalue text
);

CREATE FUNCTION gp_toolkit.__gp_param_name(varchar)
RETURNS text
LANGUAGE sql STABLE
AS $$
	SELECT CASE
		WHEN pg_catalog.current_setting($1, true) IS NOT NULL THEN $1::text
		WHEN pg_catalog.current_setting('gp.' || pg_catalog.regexp_replace($1, '^gp_', ''), true) IS NOT NULL
			THEN 'gp.' || pg_catalog.regexp_replace($1, '^gp_', '')
		ELSE $1::text
	END
$$;

CREATE FUNCTION gp_toolkit.__gp_param_setting_on_coordinator(varchar)
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE sql VOLATILE
AS $$
	SELECT pg_catalog.gp_execution_segment(), $1::text,
		   pg_catalog.current_setting(gp_toolkit.__gp_param_name($1))
$$;

/* prefer the *_coordinator function, but keep this for backwards compatibility */
CREATE FUNCTION gp_toolkit.__gp_param_setting_on_master(varchar)
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_param_setting_on_coordinator($1)
$$;

/* PL/pgSQL, whose statements are planned as they run: where gp_sql rewrites gp_dist_random() */
CREATE FUNCTION gp_toolkit.__gp_param_setting_on_segments(varchar)
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	RETURN QUERY EXECUTE pg_catalog.format(
		'SELECT pg_catalog.gp_execution_segment(), %L::text, pg_catalog.current_setting(%L) FROM gp_dist_random(''gp_id'')',
		$1, gp_toolkit.__gp_param_name($1));
END
$$;

CREATE FUNCTION gp_toolkit.gp_param_setting(varchar)
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_param_setting_on_coordinator($1)
	UNION ALL
	SELECT * FROM gp_toolkit.__gp_param_setting_on_segments($1)
$$;

CREATE FUNCTION gp_toolkit.__gp_param_settings_here()
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE sql VOLATILE
AS $$
	SELECT pg_catalog.gp_execution_segment(), name, setting FROM pg_catalog.pg_settings
$$;

/* every setting of every segment, by the server's own names */
CREATE FUNCTION gp_toolkit.gp_param_settings()
RETURNS SETOF gp_toolkit.gp_param_setting_t
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	RETURN QUERY SELECT (gp_toolkit.__gp_param_settings_here()).* FROM gp_dist_random('gp_id');
END
$$;

CREATE VIEW gp_toolkit.gp_param_settings_seg_value_diffs AS
	SELECT paramname AS psdname, paramvalue AS psdvalue, count(*) AS psdcount
	FROM gp_toolkit.gp_param_settings()
	WHERE paramname NOT IN ('config_file', 'data_directory', 'gp.dbid',
							'gp.qe_identity', 'hba_file', 'hosts_file',
							'ident_file', 'port')
	GROUP BY 1, 2
	HAVING count(*) < (SELECT numsegments FROM gp_toolkit.__gp_number_of_segments)
	ORDER BY 1, 2, 3;

CREATE VIEW gp_toolkit.gp_pgdatabase_invalid AS
	SELECT dbid AS pgdbidbid, isprimary AS pgdbiisprimary,
		   content AS pgdbicontent, valid AS pgdbivalid,
		   definedprimary AS pgdbidefinedprimary
	FROM pg_catalog.gp_pgdatabase
	WHERE NOT valid
	ORDER BY dbid;

/*
 * Skew: how a table's rows are spread over the segments.  An
 * append-optimized table's are counted from its segment files, as
 * Cloudberry counts them (get_ao_distribution(), gp_ao's), where the
 * caller is a superuser; any other table's by gp_segment_id.
 */
CREATE TYPE gp_toolkit.gp_skew_details_t AS (
	segoid oid,
	segid int,
	segtupcount bigint
);

CREATE FUNCTION gp_toolkit.gp_skew_details(oid)
RETURNS SETOF gp_toolkit.gp_skew_details_t
LANGUAGE plpgsql
AS $$
DECLARE
	skewcrs refcursor;
	skewrec record;
	skewsegid int;
	skewtablename record;
	skewreplicated record;
BEGIN
	PERFORM 1
	FROM pg_catalog.pg_class c, pg_catalog.pg_am am, pg_catalog.pg_roles r
	WHERE c.oid = $1 AND am.oid = c.relam AND am.amname IN ('ao_row', 'ao_column')
	  AND r.rolname = current_user AND r.rolsuper;
	IF FOUND THEN
		-- append-optimized table
		FOR skewrec IN EXECUTE
			'SELECT $1, segid, COALESCE(tupcount, 0)::bigint AS cnt'
			' FROM (SELECT generate_series(0, numsegments - 1) FROM gp_toolkit.__gp_number_of_segments) segs(segid)'
			' LEFT OUTER JOIN pg_catalog.get_ao_distribution($1) ON segid = segmentid'
			USING $1
		LOOP
			RETURN NEXT skewrec;
		END LOOP;
	ELSE
		-- heap table
		SELECT * INTO skewtablename FROM gp_toolkit.__gp_fullname WHERE fnoid = $1;
		SELECT * INTO skewreplicated FROM pg_catalog.gp_distribution_policy
		WHERE policytype = 'r' AND localoid = $1;
		IF FOUND THEN
			-- replicated table: every replica has the same rows
			OPEN skewcrs FOR EXECUTE
				'SELECT ' || $1 || '::oid, segid, ' ||
				'(SELECT COUNT(*) AS cnt FROM ' ||
					pg_catalog.quote_ident(skewtablename.fnnspname) || '.' ||
					pg_catalog.quote_ident(skewtablename.fnrelname) || ') ' ||
				'FROM (SELECT generate_series(0, numsegments - 1) FROM gp_toolkit.__gp_number_of_segments) segs(segid)';
		ELSE
			OPEN skewcrs FOR EXECUTE
				'SELECT ' || $1 || '::oid, segid, CASE WHEN gp_segment_id IS NULL THEN 0 ELSE cnt END ' ||
				'FROM (SELECT generate_series(0, numsegments - 1) FROM gp_toolkit.__gp_number_of_segments) segs(segid) ' ||
				'LEFT OUTER JOIN ' ||
					'(SELECT gp_segment_id, COUNT(*) AS cnt FROM ' ||
						pg_catalog.quote_ident(skewtablename.fnnspname) || '.' ||
						pg_catalog.quote_ident(skewtablename.fnrelname) ||
					' GROUP BY 1) details ' ||
				'ON segid = gp_segment_id';
		END IF;
		FOR skewsegid IN
			SELECT generate_series(1, numsegments) FROM gp_toolkit.__gp_number_of_segments
		LOOP
			FETCH skewcrs INTO skewrec;
			IF FOUND THEN
				RETURN NEXT skewrec;
			ELSE
				RETURN;
			END IF;
		END LOOP;
		CLOSE skewcrs;
	END IF;
	RETURN;
END;
$$;

CREATE TYPE gp_toolkit.gp_skew_analysis_t AS (
	skewoid oid,
	skewval numeric
);

CREATE FUNCTION gp_toolkit.gp_skew_coefficient(targetoid oid, OUT skcoid oid,
											   OUT skccoeff numeric)
RETURNS record
LANGUAGE sql
AS $$
	SELECT $1 AS skcoid,
		   CASE WHEN skewmean > 0 THEN ((skewdev / skewmean) * 100.0) ELSE 0 END AS skccoeff
	FROM (SELECT stddev(segtupcount) AS skewdev, avg(segtupcount) AS skewmean,
				 count(*) AS skewcnt
		  FROM gp_toolkit.gp_skew_details($1)) AS skew
$$;

CREATE FUNCTION gp_toolkit.__gp_skew_coefficients()
RETURNS SETOF gp_toolkit.gp_skew_analysis_t
LANGUAGE plpgsql
AS $$
DECLARE
	skcoid oid;
	skcrec record;
BEGIN
	FOR skcoid IN SELECT autoid FROM gp_toolkit.__gp_user_data_tables_readable
	LOOP
		SELECT * INTO skcrec FROM gp_toolkit.gp_skew_coefficient(skcoid);
		RETURN NEXT skcrec;
	END LOOP;
END;
$$;

CREATE VIEW gp_toolkit.gp_skew_coefficients AS
	SELECT skew.skewoid AS skcoid, pgn.nspname AS skcnamespace,
		   pgc.relname AS skcrelname, skew.skewval AS skccoeff
	FROM gp_toolkit.__gp_skew_coefficients() skew
	JOIN pg_catalog.pg_class pgc ON (skew.skewoid = pgc.oid)
	JOIN pg_catalog.pg_namespace pgn ON (pgc.relnamespace = pgn.oid);

CREATE FUNCTION gp_toolkit.gp_skew_idle_fraction(targetoid oid, OUT sifoid oid,
												 OUT siffraction numeric)
RETURNS record
LANGUAGE sql
AS $$
	SELECT $1 AS sifoid,
		   CASE WHEN min(skewmax) = 0 THEN 0
				ELSE (sum(skewmax - segtupcount) / (min(skewmax) * min(numsegments)))
		   END AS siffraction
	FROM (SELECT segid, segtupcount, count(segid) OVER () AS numsegments,
				 max(segtupcount) OVER () AS skewmax
		  FROM gp_toolkit.gp_skew_details($1)) AS skewbaseline
$$;

CREATE FUNCTION gp_toolkit.__gp_skew_idle_fractions()
RETURNS SETOF gp_toolkit.gp_skew_analysis_t
LANGUAGE plpgsql
AS $$
DECLARE
	skcoid oid;
	skcrec record;
BEGIN
	FOR skcoid IN SELECT autoid FROM gp_toolkit.__gp_user_data_tables_readable
	LOOP
		SELECT * INTO skcrec FROM gp_toolkit.gp_skew_idle_fraction(skcoid);
		RETURN NEXT skcrec;
	END LOOP;
END;
$$;

CREATE VIEW gp_toolkit.gp_skew_idle_fractions AS
	SELECT skew.skewoid AS sifoid, pgn.nspname AS sifnamespace,
		   pgc.relname AS sifrelname, skew.skewval AS siffraction
	FROM gp_toolkit.__gp_skew_idle_fractions() skew
	JOIN pg_catalog.pg_class pgc ON (skew.skewoid = pgc.oid)
	JOIN pg_catalog.pg_namespace pgn ON (pgc.relnamespace = pgn.oid);

/* Statistics missing, and bloat, from the coordinator's statistics. */
CREATE VIEW gp_toolkit.gp_stats_missing AS
	SELECT aut.autnspname AS smischema, aut.autrelname AS smitable,
		   CASE WHEN aut.autrelpages = 0 OR aut.autreltuples = 0 THEN false ELSE true END AS smisize,
		   attcnt AS smicols, coalesce(stacnt, 0) AS smirecs
	FROM gp_toolkit.__gp_user_tables aut
	JOIN (SELECT attrelid, count(*) AS attcnt
		  FROM pg_catalog.pg_attribute
		  WHERE attnum > 0 AND attisdropped = false
		  GROUP BY attrelid) attrs ON aut.autoid = attrelid
	LEFT OUTER JOIN (SELECT starelid, count(*) AS stacnt
					 FROM pg_catalog.pg_statistic
					 GROUP BY starelid) bar ON aut.autoid = starelid
	WHERE aut.autrelkind = 'r'
	  AND (aut.autrelpages = 0 OR aut.autreltuples = 0)
	   OR (stacnt IS NOT NULL AND attcnt > stacnt);

CREATE VIEW gp_toolkit.gp_bloat_expected_pages AS
	SELECT btdrelid, btdrelpages,
		   CASE WHEN btdexppages < numsegments THEN numsegments ELSE btdexppages END AS btdexppages
	FROM (SELECT oid AS btdrelid, pgc.relpages AS btdrelpages,
				 ceil((pgc.reltuples * (25 + width))::numeric /
					  pg_catalog.current_setting('block_size')::numeric) AS btdexppages,
				 (SELECT numsegments FROM gp_toolkit.__gp_number_of_segments) AS numsegments
		  FROM (SELECT pgc.oid, pgc.reltuples, pgc.relpages
				FROM pg_catalog.pg_class pgc
				WHERE NOT EXISTS (SELECT iaooid FROM gp_toolkit.__gp_is_append_only
								  WHERE iaooid = pgc.oid AND iaotype = 't')
				  AND pgc.relkind NOT IN ('p')) AS pgc
		  LEFT OUTER JOIN (SELECT starelid, sum(stawidth * (1.0 - stanullfrac)) AS width
						   FROM pg_catalog.pg_statistic pgs
						   GROUP BY 1) AS btwcols ON pgc.oid = btwcols.starelid
		  WHERE starelid IS NOT NULL) AS subq;

CREATE FUNCTION gp_toolkit.gp_bloat_diag(btdrelpages int, btdexppages numeric,
										 aotable bool, OUT bltidx int,
										 OUT bltdiag text)
LANGUAGE sql
AS $$
	SELECT bloatidx,
		   CASE WHEN bloatidx = 0 THEN 'no bloat detected'::text
				WHEN bloatidx = 1 THEN 'moderate amount of bloat suspected'::text
				WHEN bloatidx = 2 THEN 'significant amount of bloat suspected'::text
				WHEN bloatidx = -1 THEN 'diagnosis inconclusive or no bloat suspected'::text
		   END AS bloatdiag
	FROM (SELECT CASE WHEN $3 = 't' THEN 0
					  WHEN $1 < 10 AND $2 = 0 THEN -1
					  WHEN $2 = 0 THEN 2
					  WHEN $1 < $2 THEN 0
					  WHEN ($1 / $2)::numeric > 10 THEN 2
					  WHEN ($1 / $2)::numeric > 3 THEN 1
					  ELSE -1
				 END AS bloatidx) AS bloatmapping
$$;

CREATE VIEW gp_toolkit.gp_bloat_diag AS
	SELECT btdrelid AS bdirelid, fnnspname AS bdinspname, fnrelname AS bdirelname,
		   btdrelpages AS bdirelpages, btdexppages AS bdiexppages,
		   bltdiag(bd) AS bdidiag
	FROM (SELECT fn.*, beg.*,
				 gp_toolkit.gp_bloat_diag(btdrelpages::int, btdexppages::numeric,
										  iao.iaotype::bool) AS bd
		  FROM gp_toolkit.gp_bloat_expected_pages beg, pg_catalog.pg_class pgc,
			   gp_toolkit.__gp_fullname fn, gp_toolkit.__gp_is_append_only iao
		  WHERE beg.btdrelid = pgc.oid AND pgc.oid = fn.fnoid
			AND iao.iaooid = pgc.oid) AS bloatsummary
	WHERE bltidx(bd) > 0;

/* Locks on relations, and who has what role. */
CREATE VIEW gp_toolkit.gp_locks_on_relation AS
	SELECT pgl.locktype AS lorlocktype, pgl.database AS lordatabase,
		   pgc.relname AS lorrelname, pgl.relation AS lorrelation,
		   pgl.transactionid AS lortransaction, pgl.pid AS lorpid,
		   pgl.mode AS lormode, pgl.granted AS lorgranted,
		   pgsa.query AS lorcurrentquery
	FROM pg_catalog.pg_locks pgl
	JOIN pg_catalog.pg_class pgc ON (pgl.relation = pgc.oid)
	JOIN pg_catalog.pg_stat_activity pgsa ON (pgl.pid = pgsa.pid)
	ORDER BY pgc.relname;

CREATE VIEW gp_toolkit.gp_roles_assigned AS
	SELECT pgr.oid AS raroleid, pgr.rolname AS rarolename,
		   pgam.member AS ramemberid, pgr2.rolname AS ramembername
	FROM pg_catalog.pg_roles pgr
	LEFT JOIN pg_catalog.pg_auth_members pgam ON (pgr.oid = pgam.roleid)
	LEFT JOIN pg_catalog.pg_roles pgr2 ON (pgam.member = pgr2.oid);

/*
 * Sizes, the cluster's: the size functions of PostgreSQL a view calls on
 * the coordinator give every node's sum (gp_size.c).  An append-optimized
 * table has no auxiliary tables here -- its metadata is in gp_ao's tables,
 * its visibility map in its relation -- so a table's additional size is 0;
 * its uncompressed size, which asks gp_ao for its compression ratio, is
 * gp_ao's view.
 */
CREATE VIEW gp_toolkit.gp_size_of_index AS
	SELECT soi.soioid AS soioid, soi.soitableoid AS soitableoid,
		   soi.soisize AS soisize, fnidx.fnnspname AS soiindexschemaname,
		   fnidx.fnrelname AS soiindexname, fntbl.fnnspname AS soitableschemaname,
		   fntbl.fnrelname AS soitablename
	FROM (SELECT pgi.indexrelid AS soioid, pgi.indrelid AS soitableoid,
				 pg_catalog.pg_relation_size(pgi.indexrelid) AS soisize
		  FROM pg_catalog.pg_index pgi
		  JOIN gp_toolkit.__gp_user_data_tables_readable ut ON (pgi.indrelid = ut.autoid)) AS soi
	JOIN gp_toolkit.__gp_fullname fnidx ON (soi.soioid = fnidx.fnoid)
	JOIN gp_toolkit.__gp_fullname fntbl ON (soi.soitableoid = fntbl.fnoid);

CREATE VIEW gp_toolkit.gp_size_of_table_disk AS
	SELECT sotd.sotdoid AS sotdoid, sotd.sotdsize AS sotdsize,
		   sotd.sotdtoastsize AS sotdtoastsize,
		   sotd.sotdadditionalsize AS sotdadditionalsize,
		   fn.fnnspname AS sotdschemaname, fn.fnrelname AS sotdtablename
	FROM (SELECT autoid AS sotdoid,
				 pg_catalog.pg_relation_size(autoid) AS sotdsize,
				 CASE WHEN auttoastoid > 0
					  THEN pg_catalog.pg_total_relation_size(auttoastoid)
					  ELSE 0 END AS sotdtoastsize,
				 0::bigint AS sotdadditionalsize
		  FROM gp_toolkit.__gp_user_data_tables_readable) AS sotd
	JOIN gp_toolkit.__gp_fullname fn ON (sotd.sotdoid = fn.fnoid);

CREATE VIEW gp_toolkit.gp_table_indexes AS
	SELECT ti.tireloid AS tireloid, ti.tiidxoid AS tiidxoid,
		   fntbl.fnnspname AS titableschemaname, fntbl.fnrelname AS titablename,
		   fnidx.fnnspname AS tiindexschemaname, fnidx.fnrelname AS tiindexname
	FROM (SELECT pgc.oid AS tireloid, pgc2.oid AS tiidxoid
		  FROM pg_catalog.pg_class pgc
		  JOIN pg_catalog.pg_index pgi ON (pgc.oid = pgi.indrelid)
		  JOIN pg_catalog.pg_class pgc2 ON (pgi.indexrelid = pgc2.oid)
		  JOIN gp_toolkit.__gp_user_data_tables_readable udt ON (udt.autoid = pgc.oid)) AS ti
	JOIN gp_toolkit.__gp_fullname fntbl ON (ti.tireloid = fntbl.fnoid)
	JOIN gp_toolkit.__gp_fullname fnidx ON (ti.tiidxoid = fnidx.fnoid);

CREATE VIEW gp_toolkit.gp_size_of_all_table_indexes AS
	SELECT soati.soatioid AS soatioid, soati.soatisize AS soatisize,
		   fn.fnnspname AS soatischemaname, fn.fnrelname AS soatitablename
	FROM (SELECT tireloid AS soatioid,
				 sum(pg_catalog.pg_relation_size(tiidxoid)) AS soatisize
		  FROM gp_toolkit.gp_table_indexes ti
		  GROUP BY soatioid) AS soati
	JOIN gp_toolkit.__gp_fullname fn ON (soati.soatioid = fn.fnoid);

CREATE VIEW gp_toolkit.gp_size_of_table_and_indexes_disk AS
	SELECT sotaid.sotaidoid AS sotaidoid, sotaid.sotaidtablesize AS sotaidtablesize,
		   sotaid.sotaididxsize AS sotaididxsize, fn.fnnspname AS sotaidschemaname,
		   fn.fnrelname AS sotaidtablename
	FROM (SELECT sotd.sotdoid AS sotaidoid,
				 sotd.sotdsize + sotd.sotdtoastsize + sotd.sotdadditionalsize AS sotaidtablesize,
				 CASE WHEN soati.soatisize IS NULL THEN 0 ELSE soati.soatisize END AS sotaididxsize
		  FROM gp_toolkit.gp_size_of_table_disk sotd
		  LEFT JOIN gp_toolkit.gp_size_of_all_table_indexes soati
			ON (sotd.sotdoid = soati.soatioid)) AS sotaid
	JOIN gp_toolkit.__gp_fullname fn ON (sotaid.sotaidoid = fn.fnoid);

CREATE VIEW gp_toolkit.gp_size_of_schema_disk AS
	SELECT un.aunnspname AS sosdnsp,
		   coalesce(sum(sotaid.sotaidtablesize), 0) AS sosdschematablesize,
		   coalesce(sum(sotaid.sotaididxsize), 0) AS sosdschemaidxsize
	FROM gp_toolkit.gp_size_of_table_and_indexes_disk sotaid
	JOIN gp_toolkit.__gp_fullname fn ON (sotaid.sotaidoid = fn.fnoid)
	RIGHT JOIN gp_toolkit.__gp_user_namespaces un ON (un.aunnspname = fn.fnnspname)
	GROUP BY un.aunnspname;

CREATE VIEW gp_toolkit.gp_size_of_database AS
	SELECT datname AS sodddatname, pg_catalog.pg_database_size(oid) AS sodddatsize
	FROM pg_catalog.pg_database
	WHERE datname <> 'template0' AND datname <> 'template1' AND datname <> 'postgres';

GRANT SELECT ON gp_toolkit.__gp_is_append_only, gp_toolkit.__gp_fullname,
	gp_toolkit.__gp_user_namespaces, gp_toolkit.__gp_user_tables,
	gp_toolkit.__gp_user_data_tables_readable, gp_toolkit.__gp_number_of_segments,
	gp_toolkit.gp_param_settings_seg_value_diffs, gp_toolkit.gp_pgdatabase_invalid,
	gp_toolkit.gp_skew_coefficients, gp_toolkit.gp_skew_idle_fractions,
	gp_toolkit.gp_stats_missing, gp_toolkit.gp_bloat_expected_pages,
	gp_toolkit.gp_bloat_diag, gp_toolkit.gp_locks_on_relation,
	gp_toolkit.gp_roles_assigned, gp_toolkit.gp_size_of_index,
	gp_toolkit.gp_size_of_table_disk, gp_toolkit.gp_table_indexes,
	gp_toolkit.gp_size_of_all_table_indexes,
	gp_toolkit.gp_size_of_table_and_indexes_disk,
	gp_toolkit.gp_size_of_schema_disk, gp_toolkit.gp_size_of_database
	TO PUBLIC;
