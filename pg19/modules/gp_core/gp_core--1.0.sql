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
 * Wait until every standby streaming from this node has replayed what the
 * node has written: a test's, force_mirrors_to_catch_up()'s (gp_standby.c).
 */
CREATE FUNCTION gp_internal.mirror_replay_wait()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_mirror_replay_wait'
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
);

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
);
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
);
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
 * into t's partitions, each returned with its table and its ctid there
 * -- firing no trigger and applying no policy, as
 * Cloudberry's Split does neither.  Not STRICT, because NULL::t is how they
 * are told which table; only for a connection that carries the cluster
 * secret.
 */
CREATE FUNCTION gp_internal.split_delete(rel anyelement, ctids tid[],
	tables oid[], numbers int8[],
	OUT gp_n int8, OUT gp_toid oid, OUT gp_ctid tid, OUT gp_row anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_split_delete'
LANGUAGE C;

CREATE FUNCTION gp_internal.split_insert(rel anyelement, rows anyarray,
	tables oid[], numbers int8[],
	OUT gp_n int8, OUT gp_toid oid, OUT gp_ctid tid, OUT gp_row anyelement)
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
 * percentile_cont() WITHIN GROUP (ORDER BY a timestamp or a timestamptz),
 * of one percentile and of an array of them: Cloudberry's, beside
 * PostgreSQL's of float8 and of interval (gp_median.c).  A date sorted this
 * way is a timestamptz, as in Cloudberry.
 */
CREATE FUNCTION gp.percentile_cont_transfn(internal, timestamp)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_percentile_cont_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.percentile_cont_transfn(internal, timestamptz)
RETURNS internal
AS 'MODULE_PATHNAME', 'gp_percentile_cont_transfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.percentile_cont_timestamp_final(internal, float8)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_percentile_cont_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.percentile_cont_timestamptz_final(internal, float8)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_percentile_cont_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.percentile_cont_timestamp_multi_final(internal, float8[])
RETURNS timestamp[]
AS 'MODULE_PATHNAME', 'gp_percentile_cont_multi_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION gp.percentile_cont_timestamptz_multi_final(internal, float8[])
RETURNS timestamptz[]
AS 'MODULE_PATHNAME', 'gp_percentile_cont_multi_finalfn'
LANGUAGE C PARALLEL SAFE;

CREATE AGGREGATE pg_catalog.percentile_cont(float8 ORDER BY timestamp) (
	SFUNC = gp.percentile_cont_transfn,
	STYPE = internal,
	FINALFUNC = gp.percentile_cont_timestamp_final,
	FINALFUNC_MODIFY = SHAREABLE,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.percentile_cont(float8[] ORDER BY timestamp) (
	SFUNC = gp.percentile_cont_transfn,
	STYPE = internal,
	FINALFUNC = gp.percentile_cont_timestamp_multi_final,
	FINALFUNC_MODIFY = SHAREABLE,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.percentile_cont(float8 ORDER BY timestamptz) (
	SFUNC = gp.percentile_cont_transfn,
	STYPE = internal,
	FINALFUNC = gp.percentile_cont_timestamptz_final,
	FINALFUNC_MODIFY = SHAREABLE,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.percentile_cont(float8[] ORDER BY timestamptz) (
	SFUNC = gp.percentile_cont_transfn,
	STYPE = internal,
	FINALFUNC = gp.percentile_cont_timestamptz_multi_final,
	FINALFUNC_MODIFY = SHAREABLE,
	PARALLEL = SAFE
);

/*
 * Cloudberry's analytic functions of arrays and of time series
 * (gp_analytic.c), by its names in pg_catalog.
 *
 * sum() of an array, element by element: of smallint, integer and bigint
 * into a bigint[], of double precision -- and so of real and numeric, cast
 * -- into a double precision[].  The transition functions are Cloudberry's
 * int2_matrix_accum(), int4_matrix_accum() and int8_matrix_accum(), all one
 * here, which is the combining function as well.
 */
CREATE FUNCTION gp.int2_matrix_accum(int8[], int2[])
RETURNS int8[]
AS 'MODULE_PATHNAME', 'gp_matrix_accum'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION gp.int4_matrix_accum(int8[], int4[])
RETURNS int8[]
AS 'MODULE_PATHNAME', 'gp_matrix_accum'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION gp.int8_matrix_accum(int8[], int8[])
RETURNS int8[]
AS 'MODULE_PATHNAME', 'gp_matrix_accum'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gp.float8_matrix_accum(float8[], float8[])
RETURNS float8[]
AS 'MODULE_PATHNAME', 'gp_matrix_accum'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE AGGREGATE pg_catalog.sum(int2[]) (
	SFUNC = gp.int2_matrix_accum,
	STYPE = int8[],
	COMBINEFUNC = gp.int8_matrix_accum,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.sum(int4[]) (
	SFUNC = gp.int4_matrix_accum,
	STYPE = int8[],
	COMBINEFUNC = gp.int8_matrix_accum,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.sum(int8[]) (
	SFUNC = gp.int8_matrix_accum,
	STYPE = int8[],
	COMBINEFUNC = gp.int8_matrix_accum,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.sum(float8[]) (
	SFUNC = gp.float8_matrix_accum,
	STYPE = float8[],
	COMBINEFUNC = gp.float8_matrix_accum,
	PARALLEL = SAFE
);

COMMENT ON AGGREGATE pg_catalog.sum(int2[]) IS 'sum of matrixes (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.sum(int4[]) IS 'sum of matrixes (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.sum(int8[]) IS 'sum of matrixes (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.sum(float8[]) IS 'sum of matrixes (Apache Cloudberry)';

/*
 * gp_array_agg(): array_agg(), which it is in Cloudberry too -- the one
 * of its releases whose array_agg() had no combining function, which this
 * one had.  PostgreSQL 19's array_agg() has one.
 */
CREATE AGGREGATE pg_catalog.gp_array_agg(anynonarray) (
	SFUNC = array_agg_transfn,
	STYPE = internal,
	FINALFUNC = array_agg_finalfn,
	FINALFUNC_EXTRA,
	COMBINEFUNC = array_agg_combine,
	SERIALFUNC = array_agg_serialize,
	DESERIALFUNC = array_agg_deserialize,
	PARALLEL = SAFE
);

CREATE AGGREGATE pg_catalog.gp_array_agg(anyarray) (
	SFUNC = array_agg_array_transfn,
	STYPE = internal,
	FINALFUNC = array_agg_array_finalfn,
	FINALFUNC_EXTRA,
	COMBINEFUNC = array_agg_array_combine,
	SERIALFUNC = array_agg_array_serialize,
	DESERIALFUNC = array_agg_array_deserialize,
	PARALLEL = SAFE
);

COMMENT ON AGGREGATE pg_catalog.gp_array_agg(anynonarray) IS
	'concatenate aggregate input into an array (Apache Cloudberry)';
COMMENT ON AGGREGATE pg_catalog.gp_array_agg(anyarray) IS
	'concatenate aggregate input into an array (Apache Cloudberry)';

/*
 * One interval divided by another, a month taken as 30 days: the quotient,
 * and the remainder, of the dividend's sign; and the operators / and %.
 */
CREATE FUNCTION pg_catalog.interval_interval_div(interval, interval)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_interval_interval_div'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_interval_mod(interval, interval)
RETURNS interval
AS 'MODULE_PATHNAME', 'gp_interval_interval_mod'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION pg_catalog.interval_interval_div(interval, interval) IS
	'implementation of / operator';
COMMENT ON FUNCTION pg_catalog.interval_interval_mod(interval, interval) IS
	'implementation of % operator';

CREATE OPERATOR pg_catalog./ (
	LEFTARG = interval,
	RIGHTARG = interval,
	FUNCTION = pg_catalog.interval_interval_div
);

CREATE OPERATOR pg_catalog.% (
	LEFTARG = interval,
	RIGHTARG = interval,
	FUNCTION = pg_catalog.interval_interval_mod
);

/*
 * interval_bound(value, width [, shift [, registration]]): the lower bound
 * of the interval of the width, counted from the registration (the epoch, or
 * 0), that holds the value, moved on shift widths.
 */
CREATE FUNCTION pg_catalog.interval_bound(numeric, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'gp_numeric_interval_bound'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(numeric, numeric, int4)
RETURNS numeric
AS 'MODULE_PATHNAME', 'gp_numeric_interval_bound'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(numeric, numeric, int4, numeric)
RETURNS numeric
AS 'MODULE_PATHNAME', 'gp_numeric_interval_bound'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(timestamp, interval)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_timestamp_interval_bound'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(timestamp, interval, int4)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_timestamp_interval_bound'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(timestamp, interval, int4, timestamp)
RETURNS timestamp
AS 'MODULE_PATHNAME', 'gp_timestamp_interval_bound'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- stable: the widths' days are added in the session's time zone
CREATE FUNCTION pg_catalog.interval_bound(timestamptz, interval)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_timestamptz_interval_bound'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(timestamptz, interval, int4)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_timestamptz_interval_bound'
LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION pg_catalog.interval_bound(timestamptz, interval, int4, timestamptz)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'gp_timestamptz_interval_bound'
LANGUAGE C STABLE PARALLEL SAFE;

/*
 * linear_interpolate(x, x0, y0, x1, y1): y at x on the line through (x0, y0)
 * and (x1, y1), x of any of eleven types, y of the function's.
 */
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, int8, anyelement, int8)
RETURNS int8 AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, int4, anyelement, int4)
RETURNS int4 AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, int2, anyelement, int2)
RETURNS int2 AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, float8, anyelement, float8)
RETURNS float8 AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, float4, anyelement, float4)
RETURNS float4 AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, date, anyelement, date)
RETURNS date AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, time, anyelement, time)
RETURNS time AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, timestamp, anyelement, timestamp)
RETURNS timestamp AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- stable: a timestamptz ordinate moves in the session's time zone
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, timestamptz, anyelement, timestamptz)
RETURNS timestamptz AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, interval, anyelement, interval)
RETURNS interval AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.linear_interpolate(anyelement, anyelement, numeric, anyelement, numeric)
RETURNS numeric AS 'MODULE_PATHNAME', 'gp_linear_interpolate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * Cloudberry's functions for looking into a query and into the cluster
 * (gp_monitor.c, gp_size.c, gp_gdd.c).
 *
 * gp_dump_query_oids(): the relations and functions a query depends on, as
 * Cloudberry's minirepro reads them.
 */
CREATE FUNCTION pg_catalog.gp_dump_query_oids(text)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_dump_query_oids'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;

COMMENT ON FUNCTION pg_catalog.gp_dump_query_oids(text) IS
	'List function and relation OIDs that a query depends on, as a JSON object';

/*
 * gp_log_backend_memory_contexts(session [, content]): each segment's
 * backends of the session, or one segment's, log their memory contexts; the
 * count of the segments that did.  Superuser only, as PostgreSQL's
 * pg_log_backend_memory_contexts(), which each segment calls.
 */
CREATE FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8)
RETURNS int8
AS 'MODULE_PATHNAME', 'gp_log_backend_memory_contexts'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8, int8)
RETURNS int8
AS 'MODULE_PATHNAME', 'gp_log_backend_memory_contexts'
LANGUAGE C VOLATILE STRICT;

REVOKE ALL ON FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8, int8) FROM PUBLIC;

COMMENT ON FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8) IS
	'log memory contexts of the backend for the specified session ID';
COMMENT ON FUNCTION pg_catalog.gp_log_backend_memory_contexts(int8, int8) IS
	'log memory contexts of the backend for the specified session ID and content ID';

/*
 * gp_suboverflowed_backend: on each node, the processes whose
 * subtransactions have overflowed their cache (segid, pids), the
 * coordinator's as -1, as Cloudberry's view gives them -- read on each
 * segment through the view here, which gp.dist_random() reads there.
 */
CREATE FUNCTION pg_catalog.gp_get_suboverflowed_backends()
RETURNS int4[]
AS 'MODULE_PATHNAME', 'gp_get_suboverflowed_backends'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_catalog.gp_get_suboverflowed_backends() IS
	'get backends of overflowed subtransaction';

CREATE VIEW gp_internal.suboverflowed_backends AS
	SELECT pg_catalog.gp_get_suboverflowed_backends() AS pids;

/*
 * gp_dist_wait_status(): every node's waiting relations, in Cloudberry's
 * columns (gp_gdd.c).
 */
CREATE FUNCTION pg_catalog.gp_dist_wait_status(
	OUT segid int4, OUT waiter_dxid int8, OUT holder_dxid int8,
	OUT "holdTillEndXact" bool, OUT waiter_lpid int4, OUT holder_lpid int4,
	OUT waiter_lockmode text, OUT waiter_locktype text,
	OUT waiter_sessionid int4, OUT holder_sessionid int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dist_wait_status_cluster'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

/*
 * cbdb_relation_size(oid[] [, fork]): each relation's size, the cluster's,
 * the segments asked once for them all (gp_size.c).
 */
CREATE FUNCTION pg_catalog.cbdb_relation_size(reloids oid[], forkname text,
	OUT reloid oid, OUT size int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_cbdb_relation_size'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE ROWS 100;

CREATE FUNCTION pg_catalog.cbdb_relation_size(reloids oid[],
	OUT reloid oid, OUT size int8)
RETURNS SETOF record
LANGUAGE sql VOLATILE STRICT PARALLEL UNSAFE COST 1 ROWS 100
BEGIN ATOMIC
	SELECT * FROM pg_catalog.cbdb_relation_size($1, 'main');
END;

COMMENT ON FUNCTION pg_catalog.cbdb_relation_size(oid[], text) IS
	'disk space usage for the specified fork of a group of tables or indexes';
COMMENT ON FUNCTION pg_catalog.cbdb_relation_size(oid[]) IS
	'disk space usage for the main fork of a group of tables or indexes';

/*
 * gp_tablespace_location(oid): each node's location of the tablespace, as
 * pg_tablespace_location() says it there, with its content id (gp_size.c).
 * The directory of this release a node makes under it is
 * get_tablespace_version_directory_name()'s, below (gp_toolkit.c).
 */
CREATE FUNCTION gp_internal.tablespace_segment_location(oid,
	OUT gp_segment_id int4, OUT tblspc_loc text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_tablespace_segment_location'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION pg_catalog.gp_tablespace_location(tblspc_oid oid,
	OUT gp_segment_id int4, OUT tblspc_loc text)
RETURNS SETOF record
LANGUAGE sql VOLATILE STRICT
AS $$
	SELECT * FROM gp_internal.tablespace_segment_location($1)
	UNION ALL
	SELECT -1, gp_internal.tablespace_location($1)
$$;

SET allow_system_table_mods = on;

CREATE VIEW pg_catalog.gp_suboverflowed_backend (segid, pids) AS
	SELECT -1, s.pids FROM gp_internal.suboverflowed_backends s
	UNION ALL
	SELECT d.gp_segment_id, d.pids
	  FROM gp.dist_random(NULL::gp_internal.suboverflowed_backends) d
	ORDER BY 1;

/*
 * pg_stat_operations: pg_stat_last_operation and pg_stat_last_shoperation
 * of the roles, relations, schemas, databases and tablespaces there are,
 * with their names and whether the role that acted is the same still --
 * Cloudberry's view, less its resource queues, of which the port's
 * catalogs record no operation.
 */
CREATE VIEW pg_catalog.pg_stat_operations AS
SELECT 'pg_authid' AS classname, a.rolname AS objname, c.objid,
	   NULL::name AS schemaname,
	   CASE WHEN b.oid = c.stasysid AND b.rolname = c.stausename THEN 'CURRENT'
			WHEN b.rolname != c.stausename THEN 'CHANGED'
			ELSE 'DROPPED' END AS usestatus,
	   CASE WHEN b.rolname IS NULL THEN c.stausename ELSE b.rolname END AS usename,
	   c.staactionname AS actionname, c.stasubtype AS subtype, c.statime
  FROM pg_catalog.pg_authid a,
	   (pg_catalog.pg_authid b FULL JOIN pg_catalog.pg_stat_last_shoperation c
		ON b.oid = c.stasysid)
 WHERE a.oid = c.objid AND c.classid = 'pg_catalog.pg_authid'::regclass
UNION
SELECT 'pg_class', a.relname, c.objid, n.nspname,
	   CASE WHEN b.oid = c.stasysid AND b.rolname = c.stausename THEN 'CURRENT'
			WHEN b.rolname != c.stausename THEN 'CHANGED'
			ELSE 'DROPPED' END,
	   CASE WHEN b.rolname IS NULL THEN c.stausename ELSE b.rolname END,
	   c.staactionname, c.stasubtype, c.statime
  FROM pg_catalog.pg_class a, pg_catalog.pg_namespace n,
	   (pg_catalog.pg_authid b FULL JOIN pg_catalog.pg_stat_last_operation c
		ON b.oid = c.stasysid)
 WHERE a.relnamespace = n.oid AND a.oid = c.objid
   AND c.classid = 'pg_catalog.pg_class'::regclass
UNION
SELECT 'pg_namespace', a.nspname, c.objid, NULL,
	   CASE WHEN b.oid = c.stasysid AND b.rolname = c.stausename THEN 'CURRENT'
			WHEN b.rolname != c.stausename THEN 'CHANGED'
			ELSE 'DROPPED' END,
	   CASE WHEN b.rolname IS NULL THEN c.stausename ELSE b.rolname END,
	   c.staactionname, c.stasubtype, c.statime
  FROM pg_catalog.pg_namespace a,
	   (pg_catalog.pg_authid b FULL JOIN pg_catalog.pg_stat_last_operation c
		ON b.oid = c.stasysid)
 WHERE a.oid = c.objid AND c.classid = 'pg_catalog.pg_namespace'::regclass
UNION
SELECT 'pg_database', a.datname, c.objid, NULL,
	   CASE WHEN b.oid = c.stasysid AND b.rolname = c.stausename THEN 'CURRENT'
			WHEN b.rolname != c.stausename THEN 'CHANGED'
			ELSE 'DROPPED' END,
	   CASE WHEN b.rolname IS NULL THEN c.stausename ELSE b.rolname END,
	   c.staactionname, c.stasubtype, c.statime
  FROM pg_catalog.pg_database a,
	   (pg_catalog.pg_authid b FULL JOIN pg_catalog.pg_stat_last_shoperation c
		ON b.oid = c.stasysid)
 WHERE a.oid = c.objid AND c.classid = 'pg_catalog.pg_database'::regclass
UNION
SELECT 'pg_tablespace', a.spcname, c.objid, NULL,
	   CASE WHEN b.oid = c.stasysid AND b.rolname = c.stausename THEN 'CURRENT'
			WHEN b.rolname != c.stausename THEN 'CHANGED'
			ELSE 'DROPPED' END,
	   CASE WHEN b.rolname IS NULL THEN c.stausename ELSE b.rolname END,
	   c.staactionname, c.stasubtype, c.statime
  FROM pg_catalog.pg_tablespace a,
	   (pg_catalog.pg_authid b FULL JOIN pg_catalog.pg_stat_last_shoperation c
		ON b.oid = c.stasysid)
 WHERE a.oid = c.objid AND c.classid = 'pg_catalog.pg_tablespace'::regclass
ORDER BY 9;

/*
 * The summary views of the statistics, Cloudberry's (its
 * system_views_gp_summary.sql): what every node counted of a relation, an
 * index or a function, added up -- a replicated table's divided by its
 * segments, each of which holds all of it.  A user table's scans and tuples
 * are the segments', which hold its rows, and a catalog's the coordinator's;
 * its vacuums and analyzes the coordinator's, which runs them.  Those whose
 * PostgreSQL 19 view has other columns than Cloudberry's -- the archiver's,
 * the background writer's, the WAL's, a database's, the SLRUs', a vacuum's
 * progress -- are not made.
 */
CREATE VIEW pg_catalog.gp_stat_all_tables_summary AS
SELECT s.relid, s.schemaname, s.relname,
	   m.seq_scan, m.last_seq_scan, m.seq_tup_read, m.idx_scan,
	   m.last_idx_scan, m.idx_tup_fetch, m.n_tup_ins, m.n_tup_upd,
	   m.n_tup_del, m.n_tup_hot_upd, m.n_live_tup, m.n_dead_tup,
	   m.n_mod_since_analyze, s.last_vacuum, s.last_autovacuum,
	   s.last_analyze, s.last_autoanalyze, s.vacuum_count,
	   s.autovacuum_count, s.analyze_count, s.autoanalyze_count
  FROM (SELECT allt.relid,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.seq_scan) / d.numsegments)::bigint ELSE sum(allt.seq_scan) END AS seq_scan,
			   max(allt.last_seq_scan) AS last_seq_scan,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.seq_tup_read) / d.numsegments)::bigint ELSE sum(allt.seq_tup_read) END AS seq_tup_read,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.idx_scan) / d.numsegments)::bigint ELSE sum(allt.idx_scan) END AS idx_scan,
			   max(allt.last_idx_scan) AS last_idx_scan,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.idx_tup_fetch) / d.numsegments)::bigint ELSE sum(allt.idx_tup_fetch) END AS idx_tup_fetch,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_tup_ins) / d.numsegments)::bigint ELSE sum(allt.n_tup_ins) END AS n_tup_ins,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_tup_upd) / d.numsegments)::bigint ELSE sum(allt.n_tup_upd) END AS n_tup_upd,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_tup_del) / d.numsegments)::bigint ELSE sum(allt.n_tup_del) END AS n_tup_del,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_tup_hot_upd) / d.numsegments)::bigint ELSE sum(allt.n_tup_hot_upd) END AS n_tup_hot_upd,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_live_tup) / d.numsegments)::bigint ELSE sum(allt.n_live_tup) END AS n_live_tup,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_dead_tup) / d.numsegments)::bigint ELSE sum(allt.n_dead_tup) END AS n_dead_tup,
			   CASE WHEN d.policytype = 'r' THEN (sum(allt.n_mod_since_analyze) / d.numsegments)::bigint ELSE sum(allt.n_mod_since_analyze) END AS n_mod_since_analyze
		  FROM gp.dist_random(NULL::pg_catalog.pg_stat_all_tables) allt
		  JOIN pg_catalog.gp_distribution_policy d ON allt.relid = d.localoid
		 WHERE allt.relid >= 16384
		 GROUP BY allt.relid, d.policytype, d.numsegments
		UNION ALL
		SELECT relid, seq_scan, last_seq_scan, seq_tup_read, idx_scan,
			   last_idx_scan, idx_tup_fetch, n_tup_ins, n_tup_upd, n_tup_del,
			   n_tup_hot_upd, n_live_tup, n_dead_tup, n_mod_since_analyze
		  FROM pg_catalog.pg_stat_all_tables
		 WHERE relid < 16384) m
  JOIN pg_catalog.pg_stat_all_tables s ON m.relid = s.relid;

CREATE VIEW pg_catalog.gp_stat_user_tables_summary AS
	SELECT * FROM pg_catalog.gp_stat_all_tables_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_sys_tables_summary AS
	SELECT * FROM pg_catalog.gp_stat_all_tables_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_xact_all_tables_summary AS
SELECT sxa.relid, sxa.schemaname, sxa.relname,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.seq_scan) / d.numsegments)::bigint ELSE sum(sxa.seq_scan) END AS seq_scan,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.seq_tup_read) / d.numsegments)::bigint ELSE sum(sxa.seq_tup_read) END AS seq_tup_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.idx_scan) / d.numsegments)::bigint ELSE sum(sxa.idx_scan) END AS idx_scan,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.idx_tup_fetch) / d.numsegments)::bigint ELSE sum(sxa.idx_tup_fetch) END AS idx_tup_fetch,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.n_tup_ins) / d.numsegments)::bigint ELSE sum(sxa.n_tup_ins) END AS n_tup_ins,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.n_tup_upd) / d.numsegments)::bigint ELSE sum(sxa.n_tup_upd) END AS n_tup_upd,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.n_tup_del) / d.numsegments)::bigint ELSE sum(sxa.n_tup_del) END AS n_tup_del,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.n_tup_hot_upd) / d.numsegments)::bigint ELSE sum(sxa.n_tup_hot_upd) END AS n_tup_hot_upd,
	   CASE WHEN d.policytype = 'r' THEN (sum(sxa.n_tup_newpage_upd) / d.numsegments)::bigint ELSE sum(sxa.n_tup_newpage_upd) END AS n_tup_newpage_upd
  FROM (SELECT * FROM pg_catalog.pg_stat_xact_all_tables
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_stat_xact_all_tables)) sxa
  LEFT JOIN pg_catalog.gp_distribution_policy d ON sxa.relid = d.localoid
 GROUP BY sxa.relid, sxa.schemaname, sxa.relname, d.policytype, d.numsegments;

CREATE VIEW pg_catalog.gp_stat_xact_user_tables_summary AS
	SELECT * FROM pg_catalog.gp_stat_xact_all_tables_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_xact_sys_tables_summary AS
	SELECT * FROM pg_catalog.gp_stat_xact_all_tables_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_all_indexes_summary AS
SELECT s.relid, s.indexrelid, s.schemaname, s.relname, s.indexrelname,
	   m.idx_scan, m.last_idx_scan, m.idx_tup_read, m.idx_tup_fetch
  FROM (SELECT alli.indexrelid,
			   CASE WHEN d.policytype = 'r' THEN (sum(alli.idx_scan) / d.numsegments)::bigint ELSE sum(alli.idx_scan) END AS idx_scan,
			   max(alli.last_idx_scan) AS last_idx_scan,
			   CASE WHEN d.policytype = 'r' THEN (sum(alli.idx_tup_read) / d.numsegments)::bigint ELSE sum(alli.idx_tup_read) END AS idx_tup_read,
			   CASE WHEN d.policytype = 'r' THEN (sum(alli.idx_tup_fetch) / d.numsegments)::bigint ELSE sum(alli.idx_tup_fetch) END AS idx_tup_fetch
		  FROM gp.dist_random(NULL::pg_catalog.pg_stat_all_indexes) alli
		  LEFT JOIN pg_catalog.gp_distribution_policy d ON alli.relid = d.localoid
		 WHERE alli.relid >= 16384
		 GROUP BY alli.indexrelid, d.policytype, d.numsegments
		UNION ALL
		SELECT indexrelid, idx_scan, last_idx_scan, idx_tup_read, idx_tup_fetch
		  FROM pg_catalog.pg_stat_all_indexes
		 WHERE relid < 16384) m
  JOIN pg_catalog.pg_stat_all_indexes s ON m.indexrelid = s.indexrelid;

CREATE VIEW pg_catalog.gp_stat_user_indexes_summary AS
	SELECT * FROM pg_catalog.gp_stat_all_indexes_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_sys_indexes_summary AS
	SELECT * FROM pg_catalog.gp_stat_all_indexes_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_all_tables_summary AS
SELECT sat.relid, sat.schemaname, sat.relname,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.heap_blks_read) / d.numsegments)::bigint ELSE sum(sat.heap_blks_read) END AS heap_blks_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.heap_blks_hit) / d.numsegments)::bigint ELSE sum(sat.heap_blks_hit) END AS heap_blks_hit,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.idx_blks_read) / d.numsegments)::bigint ELSE sum(sat.idx_blks_read) END AS idx_blks_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.idx_blks_hit) / d.numsegments)::bigint ELSE sum(sat.idx_blks_hit) END AS idx_blks_hit,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.toast_blks_read) / d.numsegments)::bigint ELSE sum(sat.toast_blks_read) END AS toast_blks_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.toast_blks_hit) / d.numsegments)::bigint ELSE sum(sat.toast_blks_hit) END AS toast_blks_hit,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.tidx_blks_read) / d.numsegments)::bigint ELSE sum(sat.tidx_blks_read) END AS tidx_blks_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sat.tidx_blks_hit) / d.numsegments)::bigint ELSE sum(sat.tidx_blks_hit) END AS tidx_blks_hit
  FROM (SELECT * FROM pg_catalog.pg_statio_all_tables
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_statio_all_tables)) sat
  LEFT JOIN pg_catalog.gp_distribution_policy d ON sat.relid = d.localoid
 GROUP BY sat.relid, sat.schemaname, sat.relname, d.policytype, d.numsegments;

CREATE VIEW pg_catalog.gp_statio_user_tables_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_tables_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_sys_tables_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_tables_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_all_indexes_summary AS
SELECT sai.relid, sai.indexrelid, sai.schemaname, sai.relname, sai.indexrelname,
	   CASE WHEN d.policytype = 'r' THEN (sum(sai.idx_blks_read) / d.numsegments)::bigint ELSE sum(sai.idx_blks_read) END AS idx_blks_read,
	   CASE WHEN d.policytype = 'r' THEN (sum(sai.idx_blks_hit) / d.numsegments)::bigint ELSE sum(sai.idx_blks_hit) END AS idx_blks_hit
  FROM (SELECT * FROM pg_catalog.pg_statio_all_indexes
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_statio_all_indexes)) sai
  LEFT JOIN pg_catalog.gp_distribution_policy d ON sai.relid = d.localoid
 GROUP BY sai.relid, sai.indexrelid, sai.schemaname, sai.relname,
		  sai.indexrelname, d.policytype, d.numsegments;

CREATE VIEW pg_catalog.gp_statio_user_indexes_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_indexes_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_sys_indexes_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_indexes_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_all_sequences_summary AS
SELECT sas.relid, sas.schemaname, sas.relname,
	   sum(sas.blks_read) AS blks_read, sum(sas.blks_hit) AS blks_hit
  FROM (SELECT * FROM pg_catalog.pg_statio_all_sequences
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_statio_all_sequences)) sas
 GROUP BY sas.relid, sas.schemaname, sas.relname;

CREATE VIEW pg_catalog.gp_statio_user_sequences_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_sequences_summary
	 WHERE schemaname NOT IN ('pg_catalog', 'information_schema', 'pg_aoseg')
	   AND schemaname !~ '^pg_toast';

CREATE VIEW pg_catalog.gp_statio_sys_sequences_summary AS
	SELECT * FROM pg_catalog.gp_statio_all_sequences_summary
	 WHERE schemaname IN ('pg_catalog', 'information_schema', 'pg_aoseg')
		OR schemaname ~ '^pg_toast';

CREATE VIEW pg_catalog.gp_stat_user_functions_summary AS
SELECT guf.funcid, guf.schemaname, guf.funcname,
	   sum(guf.calls) AS calls, sum(guf.total_time) AS total_time,
	   sum(guf.self_time) AS self_time
  FROM (SELECT * FROM pg_catalog.pg_stat_user_functions
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_stat_user_functions)) guf
 GROUP BY guf.funcid, guf.schemaname, guf.funcname;

CREATE VIEW pg_catalog.gp_stat_xact_user_functions_summary AS
SELECT xuf.funcid, xuf.schemaname, xuf.funcname,
	   sum(xuf.calls) AS calls, sum(xuf.total_time) AS total_time,
	   sum(xuf.self_time) AS self_time
  FROM (SELECT * FROM pg_catalog.pg_stat_xact_user_functions
		UNION ALL
		SELECT * FROM gp.dist_random(NULL::pg_catalog.pg_stat_xact_user_functions)) xuf
 GROUP BY xuf.funcid, xuf.schemaname, xuf.funcname;

RESET allow_system_table_mods;

GRANT SELECT ON pg_catalog.gp_suboverflowed_backend, pg_catalog.pg_stat_operations,
	pg_catalog.gp_stat_all_tables_summary, pg_catalog.gp_stat_user_tables_summary,
	pg_catalog.gp_stat_sys_tables_summary, pg_catalog.gp_stat_xact_all_tables_summary,
	pg_catalog.gp_stat_xact_user_tables_summary, pg_catalog.gp_stat_xact_sys_tables_summary,
	pg_catalog.gp_stat_all_indexes_summary, pg_catalog.gp_stat_user_indexes_summary,
	pg_catalog.gp_stat_sys_indexes_summary, pg_catalog.gp_statio_all_tables_summary,
	pg_catalog.gp_statio_user_tables_summary, pg_catalog.gp_statio_sys_tables_summary,
	pg_catalog.gp_statio_all_indexes_summary, pg_catalog.gp_statio_user_indexes_summary,
	pg_catalog.gp_statio_sys_indexes_summary, pg_catalog.gp_statio_all_sequences_summary,
	pg_catalog.gp_statio_user_sequences_summary, pg_catalog.gp_statio_sys_sequences_summary,
	pg_catalog.gp_stat_user_functions_summary, pg_catalog.gp_stat_xact_user_functions_summary,
	gp_internal.suboverflowed_backends
	TO PUBLIC;

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
 * functions there, and its resource managers' gp_resource's.  The servers'
 * logs and the views over them, gp_disk_free, the checks for orphaned and
 * missing files and the partitions' functions are at the end of this
 * file, "gp_toolkit's rest", and the workfile manager's views after them,
 * with gp_workfile.c's own.
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

/* ------------------------------------------------------------------------- */
/* debug_dtm_action (gp_dtm_debug.c)                                         */
/* ------------------------------------------------------------------------- */

/*
 * The error Cloudberry's segment raises for debug_dtm_action, which the
 * coordinator sends the segment the settings name, for it to raise; and the
 * one its PREPARE TRANSACTION or COMMIT is to fail with, as it prepares or
 * commits.
 */
CREATE FUNCTION gp_internal.dtm_raise(action integer, message text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_dtm_raise'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION gp_internal.dtm_fail_at_commit(message text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_dtm_fail_at_commit'
LANGUAGE C STRICT VOLATILE;

/******************************************************************************
 * gp_toolkit's rest: the servers' logs and the views of them, gp_disk_free,
 * the checks for orphaned and missing files, and the partitions' functions,
 * as gp_toolkit--1.3.sql and the update scripts after it have them.  The
 * segment files' history is gp_ao's, beside its __gp_aoseg().
 *****************************************************************************/

/*
 * The servers' logs: the records of Cloudberry's own log, which gp_core
 * writes beside PostgreSQL's (gp_log.c) -- where Cloudberry's external
 * tables cat each node's CSV files, every segment's and the coordinator's.
 * The superuser's alone, as Cloudberry's tables are: nothing is granted.
 */
CREATE FUNCTION gp_toolkit.__gp_log_segment_rows(
	OUT logtime timestamptz, OUT loguser text, OUT logdatabase text,
	OUT logpid text, OUT logthread text, OUT loghost text, OUT logport text,
	OUT logsessiontime timestamptz, OUT logtransaction int4,
	OUT logsession text, OUT logcmdcount text, OUT logsegment text,
	OUT logslice text, OUT logdistxact text, OUT loglocalxact text,
	OUT logsubxact text, OUT logseverity text, OUT logstate text,
	OUT logmessage text, OUT logdetail text, OUT loghint text,
	OUT logquery text, OUT logquerypos int4, OUT logcontext text,
	OUT logdebug text, OUT logcursorpos int4, OUT logfunction text,
	OUT logfile text, OUT logline int4, OUT logstack text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_log_segment_rows'
LANGUAGE C VOLATILE;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_log_segment_rows() IS 'execute_on=all_segments';

CREATE FUNCTION gp_toolkit.__gp_log_coordinator_rows(
	OUT logtime timestamptz, OUT loguser text, OUT logdatabase text,
	OUT logpid text, OUT logthread text, OUT loghost text, OUT logport text,
	OUT logsessiontime timestamptz, OUT logtransaction int4,
	OUT logsession text, OUT logcmdcount text, OUT logsegment text,
	OUT logslice text, OUT logdistxact text, OUT loglocalxact text,
	OUT logsubxact text, OUT logseverity text, OUT logstate text,
	OUT logmessage text, OUT logdetail text, OUT loghint text,
	OUT logquery text, OUT logquerypos int4, OUT logcontext text,
	OUT logdebug text, OUT logcursorpos int4, OUT logfunction text,
	OUT logfile text, OUT logline int4, OUT logstack text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_log_coordinator_rows'
LANGUAGE C VOLATILE;

REVOKE ALL ON FUNCTION gp_toolkit.__gp_log_segment_rows(),
	gp_toolkit.__gp_log_coordinator_rows() FROM PUBLIC;

CREATE VIEW gp_toolkit.__gp_log_segment_ext AS
	SELECT * FROM gp_toolkit.__gp_log_segment_rows();

CREATE VIEW gp_toolkit.__gp_log_coordinator_ext AS
	SELECT * FROM gp_toolkit.__gp_log_coordinator_rows();

CREATE VIEW gp_toolkit.__gp_log_master_ext AS
	SELECT * FROM gp_toolkit.__gp_log_coordinator_ext;

CREATE VIEW gp_toolkit.gp_log_system AS
	SELECT * FROM gp_toolkit.__gp_log_segment_ext
	UNION ALL
	SELECT * FROM gp_toolkit.__gp_log_coordinator_ext
	ORDER BY logtime;

CREATE VIEW gp_toolkit.gp_log_database AS
	SELECT * FROM gp_toolkit.gp_log_system
	WHERE logdatabase = pg_catalog.current_database();

CREATE VIEW gp_toolkit.gp_log_coordinator_concise AS
	SELECT logtime, logdatabase, logsession, logcmdcount, logseverity, logmessage
	FROM gp_toolkit.__gp_log_coordinator_ext;

CREATE VIEW gp_toolkit.gp_log_master_concise AS
	SELECT * FROM gp_toolkit.gp_log_coordinator_concise;

/* Each command of the coordinator's log, and when its records began and ended. */
CREATE VIEW gp_toolkit.gp_log_command_timings AS
	SELECT logsession, logcmdcount, logdatabase, loguser, logpid,
		   min(logtime) AS logtimemin, max(logtime) AS logtimemax,
		   max(logtime) - min(logtime) AS logduration
	FROM gp_toolkit.__gp_log_coordinator_ext
	WHERE logsession IS NOT NULL AND logcmdcount IS NOT NULL
	  AND logdatabase IS NOT NULL
	GROUP BY 1, 2, 3, 4, 5;

/*
 * gp_disk_free: the space free for each segment's data directory, which
 * the segment reads itself (gp_toolkit.c), where Cloudberry's external
 * table runs df there through gppylib.  The superuser's, as Cloudberry's.
 */
CREATE FUNCTION gp_toolkit.__gp_disk_free_rows(OUT dfsegment int4,
	OUT dfhostname text, OUT dfdevice text, OUT dfspace int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_disk_free_rows'
LANGUAGE C VOLATILE;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_disk_free_rows() IS 'execute_on=all_segments';
REVOKE ALL ON FUNCTION gp_toolkit.__gp_disk_free_rows() FROM PUBLIC;

CREATE VIEW gp_toolkit.gp_disk_free AS
	SELECT * FROM gp_toolkit.__gp_disk_free_rows();

/*
 * The directory of a tablespace this version keeps its files in, which
 * Cloudberry has built in; and adminpack's pg_file_rename(), likewise, the
 * superuser's (gp_toolkit.c).
 */
CREATE FUNCTION pg_catalog.get_tablespace_version_directory_name()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_tablespace_version_directory_name'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pg_catalog.pg_file_rename(text, text, text)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_file_rename'
LANGUAGE C VOLATILE;
REVOKE ALL ON FUNCTION pg_catalog.pg_file_rename(text, text, text) FROM PUBLIC;

/*
 * The checks for orphaned and missing files: the files each node's
 * directories of the database hold, and the ones its catalog expects,
 * Cloudberry's views.  A tablespace this database has no directory in has
 * no files, where Cloudberry's pg_ls_dir() of it fails.  Those of the
 * segment files of append-optimized tables are gp_ao's, beside its
 * __gp_aoseg().
 */
CREATE VIEW gp_toolkit.__get_exist_files AS
WITH Tablespaces AS (
	-- the default tablespace
	SELECT 0 AS tablespace, 'base/' || d.oid::text AS dirname
	FROM pg_catalog.pg_database d
	WHERE d.datname = pg_catalog.current_database()
	UNION
	-- the global tablespace
	SELECT 1664 AS tablespace, 'global/' AS dirname
	UNION
	-- the user's tablespaces
	SELECT ts.oid AS tablespace,
		   'pg_tblspc/' || ts.oid::text || '/' ||
		   pg_catalog.get_tablespace_version_directory_name() || '/' ||
		   (SELECT d.oid::text FROM pg_catalog.pg_database d
			WHERE d.datname = pg_catalog.current_database()) AS dirname
	FROM pg_catalog.pg_tablespace ts
	WHERE ts.oid > 1664
)
SELECT tablespace, files.filename, dirname || '/' || files.filename AS filepath
FROM Tablespaces, pg_catalog.pg_ls_dir(dirname, true, false) AS files(filename);

CREATE VIEW gp_toolkit.__get_expect_files AS
SELECT s.reltablespace AS tablespace, s.relname, a.amname AS AM,
	   (CASE WHEN s.relfilenode != 0 THEN s.relfilenode
			 ELSE pg_catalog.pg_relation_filenode(s.oid) END)::text AS filename
FROM pg_catalog.pg_class s
LEFT JOIN pg_catalog.pg_am a ON s.relam = a.oid
WHERE s.relkind != 'v';

/*
 * A file whose relfilenode no relation has; gp_segment_id is the node's
 * that looked, as gp_toolkit 1.4 has it.
 */
CREATE VIEW gp_toolkit.__check_orphaned_files AS
SELECT f1.tablespace, f1.filename, f1.filepath,
	   pg_catalog.gp_execution_segment() AS gp_segment_id
FROM gp_toolkit.__get_exist_files f1
LEFT JOIN gp_toolkit.__get_expect_files f2
ON f1.tablespace = f2.tablespace AND substring(f1.filename from '[0-9]+') = f2.filename
WHERE f2.tablespace IS NULL
  AND f1.filename SIMILAR TO '[0-9]+(\.)?(\_)?%';

CREATE VIEW gp_toolkit.__check_missing_files AS
SELECT f1.tablespace, f1.relname, f1.filename
FROM gp_toolkit.__get_expect_files f1
LEFT JOIN gp_toolkit.__get_exist_files f2
ON f1.tablespace = f2.tablespace AND f1.filename = f2.filename
WHERE f2.tablespace IS NULL
  AND f1.filename SIMILAR TO '[0-9]+';

/* Every segment's missing files and the coordinator's, as gp_dist_random() reads them. */
CREATE VIEW gp_toolkit.gp_check_missing_files AS
SELECT d.gp_segment_id, d.tablespace, d.relname, d.filename
FROM gp.dist_random(NULL::gp_toolkit.__check_missing_files) d
UNION ALL
SELECT -1 AS gp_segment_id, *
FROM gp_toolkit.__check_missing_files;

GRANT SELECT ON gp_toolkit.__get_exist_files, gp_toolkit.__get_expect_files,
	gp_toolkit.__check_orphaned_files, gp_toolkit.__check_missing_files,
	gp_toolkit.gp_check_missing_files TO PUBLIC;

/*
 * A node's orphaned files, found with its pg_class locked and after a
 * checkpoint, which has removed the files of the relations dropped before
 * it -- and moved to target_location where one is given, each as
 * seg<content id>_<its path, "/" as "_">.  Cloudberry's LOCK and
 * CHECKPOINT reach every segment; the port's reach the node they run on,
 * so each node runs this.
 */
CREATE FUNCTION gp_toolkit.__gp_orphaned_files_here(target_location text,
	OUT gp_segment_id int4, OUT tablespace oid, OUT filename text,
	OUT filepath text, OUT move_success bool, OUT oldpath text,
	OUT newpath text)
RETURNS SETOF record
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	LOCK TABLE pg_catalog.pg_class IN SHARE MODE NOWAIT;
	CHECKPOINT;
	RETURN QUERY
	SELECT o.gp_segment_id, o.tablespace, o.filename, o.filepath,
		   CASE WHEN target_location IS NULL THEN NULL
				ELSE pg_catalog.pg_file_rename(o.oldpath, o.newpath, NULL) END,
		   o.oldpath, o.newpath
	FROM (SELECT f.gp_segment_id, f.tablespace, f.filename, f.filepath,
				 CASE WHEN target_location IS NULL THEN NULL
					  ELSE pg_catalog.current_setting('data_directory') || '/' || f.filepath END AS oldpath,
				 target_location || '/seg' || f.gp_segment_id::text || '_' ||
				 pg_catalog.replace(f.filepath, '/', '_') AS newpath
		  FROM gp_toolkit.__check_orphaned_files f
		  ORDER BY f.filepath) o;
END
$$;

/* And each segment's, run there. */
CREATE FUNCTION gp_toolkit.__gp_orphaned_files_on_segments(target_location text,
	OUT gp_segment_id int4, OUT tablespace oid, OUT filename text,
	OUT filepath text, OUT move_success bool, OUT oldpath text,
	OUT newpath text)
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_orphaned_files_here($1)
$$;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_orphaned_files_on_segments(text) IS 'execute_on=all_segments';

REVOKE ALL ON FUNCTION gp_toolkit.__gp_orphaned_files_here(text),
	gp_toolkit.__gp_orphaned_files_on_segments(text) FROM PUBLIC;

/*
 * The orphaned files of every node, gp_toolkit 1.5's: refused while another
 * session is at work, whose files might not be in the catalog yet -- the
 * port's gp.session_id is Cloudberry's gp_session_id.
 */
CREATE FUNCTION gp_toolkit.__gp_check_orphaned_files_func()
RETURNS TABLE (
	gp_segment_id int,
	tablespace oid,
	filename text,
	filepath text
)
LANGUAGE plpgsql AS $$
BEGIN
	BEGIN
		-- lock pg_class so that no one will be adding/altering relfilenodes
		LOCK TABLE pg_catalog.pg_class IN SHARE MODE NOWAIT;

		-- make sure no other active/idle transaction is running
		IF EXISTS (
			SELECT 1
			FROM pg_catalog.gp_stat_activity
			WHERE
			sess_id <> -1 AND backend_type IN ('client backend', 'unknown process type') -- exclude background worker types
			AND sess_id <> pg_catalog.current_setting('gp.session_id')::int -- Exclude the current session
			AND state <> 'idle' -- Exclude idle session like GDD
		) THEN
			RAISE EXCEPTION 'There is a client session running on one or more segment. Aborting...';
		END IF;

		RETURN QUERY
		SELECT v.gp_segment_id, v.tablespace, v.filename, v.filepath
		FROM gp_toolkit.__gp_orphaned_files_on_segments(NULL) v
		UNION ALL
		SELECT -1 AS gp_segment_id, v.tablespace, v.filename, v.filepath
		FROM gp_toolkit.__gp_orphaned_files_here(NULL) v;
	EXCEPTION
		WHEN lock_not_available THEN
			RAISE EXCEPTION 'cannot obtain SHARE lock on pg_class';
		WHEN OTHERS THEN
			RAISE;
	END;

	RETURN;
END;
$$;
GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_check_orphaned_files_func() TO PUBLIC;

CREATE VIEW gp_toolkit.gp_check_orphaned_files AS
SELECT * FROM gp_toolkit.__gp_check_orphaned_files_func();

GRANT SELECT ON gp_toolkit.gp_check_orphaned_files TO PUBLIC;

/*
 * Move every node's orphaned files to target_location, a directory of each
 * node's host: gp_toolkit 1.5's, each node's path its data_directory's.
 */
CREATE FUNCTION gp_toolkit.gp_move_orphaned_files(target_location text)
RETURNS TABLE (
	gp_segment_id int,
	move_success bool,
	oldpath text,
	newpath text
)
LANGUAGE plpgsql AS $$
BEGIN
	-- lock pg_class so that no one will be adding/altering relfilenodes
	LOCK TABLE pg_catalog.pg_class IN SHARE MODE NOWAIT;

	-- make sure no other active/idle transaction is running
	IF EXISTS (
		SELECT 1
		FROM pg_catalog.gp_stat_activity
		WHERE
		sess_id <> -1 AND backend_type IN ('client backend', 'unknown process type') -- exclude background worker types
		AND sess_id <> pg_catalog.current_setting('gp.session_id')::int -- Exclude the current session
		AND state <> 'idle' -- Exclude idle session like GDD
	) THEN
		RAISE EXCEPTION 'There is a client session running on one or more segment. Aborting...';
	END IF;

	RETURN QUERY
	SELECT q.gp_segment_id, q.move_success, q.oldpath, q.newpath
	FROM (
		SELECT h.gp_segment_id, h.move_success, h.oldpath, h.newpath
		FROM gp_toolkit.__gp_orphaned_files_here(target_location) h
		UNION ALL
		SELECT s.gp_segment_id, s.move_success, s.oldpath, s.newpath
		FROM gp_toolkit.__gp_orphaned_files_on_segments(target_location) s
	) q
	ORDER BY q.gp_segment_id, q.oldpath;
EXCEPTION
	WHEN lock_not_available THEN
		RAISE EXCEPTION 'cannot obtain SHARE lock on pg_class';
	WHEN OTHERS THEN
		RAISE;
END;
$$;
GRANT EXECUTE ON FUNCTION gp_toolkit.gp_move_orphaned_files(text) TO PUBLIC;

/*
 * The partitions' functions and gp_partitions, gp_toolkit 1.4's, over
 * PostgreSQL's partitioning (gp_partmaint.c): a range partition's rank, its
 * bounds, the lowest and highest of a table's, and every partition under a
 * table with its level, strategy and rank -- as Cloudberry 6's
 * pg_partitions had them.
 */
CREATE FUNCTION gp_toolkit.pg_partition_rank(rp regclass)
RETURNS int
AS 'MODULE_PATHNAME', 'gp_partition_rank'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_toolkit.pg_partition_bound_value(partrel regclass, bound_type text)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
	v_relpartbound text;
	v_bound_value text;
	v_parent_table regclass;
	v_nkeys int;
BEGIN
	-- Check if the given table is a non-default child partition
	SELECT inhparent INTO v_parent_table
	FROM pg_catalog.pg_inherits
	WHERE inhrelid = partrel;

	IF v_parent_table IS NULL THEN
		RETURN NULL;
	END IF;

	-- Check if the parent table is partitioned by a single key
	SELECT partnatts INTO v_nkeys
	FROM pg_catalog.pg_partitioned_table
	WHERE partrelid = v_parent_table;

	IF v_nkeys IS NOT NULL AND v_nkeys != 1 THEN
		RETURN NULL;
	END IF;

	-- Get the partition bounds
	SELECT pg_catalog.pg_get_expr(relpartbound, oid) INTO v_relpartbound
	FROM pg_catalog.pg_class
	WHERE oid = partrel;

	-- Parse the bound value from relpartbound
	IF lower(bound_type) = 'from' THEN
		SELECT (regexp_matches(v_relpartbound, 'FOR VALUES FROM \((.+)\) TO \((.+)\)'))[1] INTO v_bound_value;
	ELSIF lower(bound_type) = 'to' THEN
		SELECT (regexp_matches(v_relpartbound, 'FOR VALUES FROM \((.+)\) TO \((.+)\)'))[2] INTO v_bound_value;
	ELSIF lower(bound_type) = 'in' THEN
		SELECT (regexp_matches(v_relpartbound, 'FOR VALUES IN \((.+)\)'))[1] INTO v_bound_value;
	ELSE
		RAISE EXCEPTION 'Invalid bound type: %', bound_type;
	END IF;

	RETURN v_bound_value;
END;
$$;

CREATE FUNCTION gp_toolkit.pg_partition_range_from(rp regclass)
RETURNS text
LANGUAGE sql
AS $$
	SELECT gp_toolkit.pg_partition_bound_value(rp, 'from');
$$;

CREATE FUNCTION gp_toolkit.pg_partition_range_to(rp regclass)
RETURNS text
LANGUAGE sql
AS $$
	SELECT gp_toolkit.pg_partition_bound_value(rp, 'to');
$$;

CREATE FUNCTION gp_toolkit.pg_partition_list_values(rp regclass)
RETURNS text
LANGUAGE sql
AS $$
	SELECT gp_toolkit.pg_partition_bound_value(rp, 'in'::text);
$$;

CREATE FUNCTION gp_toolkit.pg_partition_isdefault(relid regclass)
RETURNS boolean
LANGUAGE plpgsql
AS $$
DECLARE
	boundspec text;
BEGIN
	-- Get the partition bound definition for the relation
	SELECT pg_catalog.pg_get_expr(relpartbound, oid) INTO boundspec
	FROM pg_catalog.pg_class
	WHERE oid = relid;

	-- If partition_def is null, the relation is not a partition at all
	IF boundspec IS NULL THEN
		RETURN FALSE;
	END IF;

	-- Check if the partition bound spec exactly matches 'DEFAULT'
	RETURN boundspec = 'DEFAULT';
END;
$$;

CREATE FUNCTION gp_toolkit.pg_partition_lowest_child(rp regclass)
RETURNS regclass
AS 'MODULE_PATHNAME', 'gp_partition_lowest_child'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_toolkit.pg_partition_highest_child(rp regclass)
RETURNS regclass
AS 'MODULE_PATHNAME', 'gp_partition_highest_child'
LANGUAGE C VOLATILE STRICT;

CREATE TYPE gp_toolkit.get_partition_result AS (
	relid regclass,
	parentid regclass,
	isleaf bool,
	partitionlevel int,
	partitiontype text,
	partitionrank int,
	is_default bool
);

CREATE FUNCTION gp_toolkit.gp_get_partitions(rp regclass)
RETURNS SETOF gp_toolkit.get_partition_result
AS 'MODULE_PATHNAME', 'gp_get_partitions'
LANGUAGE C VOLATILE STRICT;

CREATE VIEW gp_toolkit.gp_partitions AS
WITH default_ts(default_spcname) AS
(SELECT s.spcname
	FROM pg_catalog.pg_database, pg_catalog.pg_tablespace s
	WHERE datname = pg_catalog.current_database() AND dattablespace = s.oid),
partitions AS
(SELECT p.*,
		pg_catalog.pg_get_expr(pc.relpartbound, pc.oid) AS bound,
		rns.nspname AS rootnamespacename,
		pns.nspname AS partitionschemaname,
		pc.relname AS partitiontablename,
		coalesce(rt.spcname, default_spcname) AS parenttablespacename,
		coalesce(pt.spcname, default_spcname) AS partitiontablespacename
	FROM
	(SELECT relnamespace,
			relname AS roottablename,
			(gp_toolkit.gp_get_partitions(oid)).*
	 FROM pg_catalog.pg_class
			WHERE relkind = 'p'
			AND oid NOT IN (SELECT inhrelid FROM pg_catalog.pg_inherits)) p
	 JOIN pg_catalog.pg_class pc ON p.relid = pc.oid
	 JOIN pg_catalog.pg_class parentc ON parentc.oid = p.parentid
	 LEFT JOIN pg_catalog.pg_namespace rns ON p.relnamespace = rns.oid
	 LEFT JOIN pg_catalog.pg_namespace pns ON pc.relnamespace = pns.oid
	 LEFT JOIN pg_catalog.pg_tablespace rt ON parentc.reltablespace = rt.oid
	 LEFT JOIN pg_catalog.pg_tablespace pt ON pc.reltablespace = pt.oid
	 JOIN default_ts ON 1=1)
SELECT
	rootnamespacename AS schemaname,
	roottablename AS tablename,
	partitionschemaname,
	partitiontablename,
	parentid::regclass AS parentpartitiontablename,
	partitiontype,
	partitionlevel,
	partitionrank,
	CASE
		WHEN partitiontype = 'list'
			THEN substring(bound FROM 'FOR VALUES IN \((.+)\)')
		END AS partitionlistvalues,
	CASE
		WHEN partitiontype = 'range'
			THEN substring(bound FROM 'FOR VALUES FROM \((.+)\) TO \((.+)\)')
		END AS partitionrangestart,
	CASE
		WHEN partitiontype = 'range'
			THEN substring(bound FROM 'TO \((.+)\)')
		END AS partitionrangeend,
	is_default AS partitionisdefault,
	bound AS partitionboundary,
	parenttablespacename AS parenttablespace,
	partitiontablespacename AS partitiontablespace
FROM partitions;

GRANT SELECT ON gp_toolkit.gp_partitions TO PUBLIC;

/* ------------------------------------------------------------------------- */
/* gp_toolkit's views of the workfile manager (gp_workfile.c)                */
/* ------------------------------------------------------------------------- */

/*
 * Each temporary file of the node the call runs on -- a file, or a FileSet's
 * directory with its files -- as Cloudberry's gp_workfile_mgr_cache_entries()
 * gives each workfile set of its manager (gp_internal_tools'
 * gp_workfile_mgr.c): the node's content id, the file's name as its prefix,
 * its size, the session of the process that made it and how many files it
 * is; the operator, slice and command, which a file's name does not say,
 * NULL.  And the bytes of them all, the node's.
 */
CREATE FUNCTION gp_toolkit.__gp_workfile_entries_here(OUT segid int4,
	OUT prefix text, OUT size int8, OUT optype text, OUT slice int4,
	OUT sessionid int4, OUT commandid int4, OUT numfiles int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_workfile_entries'
LANGUAGE C VOLATILE;

CREATE FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_here(OUT segid int4,
	OUT bytes int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_workfile_used_diskspace'
LANGUAGE C VOLATILE;

/*
 * Cloudberry's functions of the names its views call, on the coordinator
 * and on every segment (EXECUTE ON ALL SEGMENTS, as a query of
 * gp_dist_random('gp_id') alone runs there), and its views, with its text
 * and grants (gp_toolkit--1.3.sql).
 */
CREATE FUNCTION gp_toolkit.__gp_workfile_entries_f_on_coordinator()
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_workfile_entries_here()
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_entries_f_on_coordinator() TO public;

/* prefer the *_coordinator function, but keep this for backwards compatibility */
CREATE FUNCTION gp_toolkit.__gp_workfile_entries_f_on_master()
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_workfile_entries_here()
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_entries_f_on_master() TO public;

CREATE FUNCTION gp_toolkit.__gp_workfile_entries_f_on_segments()
RETURNS SETOF record
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	RETURN QUERY SELECT (gp_toolkit.__gp_workfile_entries_here()).* FROM gp_dist_random('gp_id');
END
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_entries_f_on_segments() TO public;

CREATE VIEW gp_toolkit.gp_workfile_entries AS
WITH all_entries AS (
    SELECT C.*
        FROM gp_toolkit.__gp_workfile_entries_f_on_coordinator() AS C (
           segid int,
           prefix text,
           size bigint,
           optype text,
           slice int,
           sessionid int,
           commandid int,
           numfiles int
        )
    UNION ALL
    SELECT C.*
        FROM gp_toolkit.__gp_workfile_entries_f_on_segments() AS C (
            segid int,
            prefix text,
            size bigint,
            optype text,
            slice int,
            sessionid int,
            commandid int,
            numfiles int
        ))
SELECT S.datname,
       S.pid,
       C.sessionid as sess_id,
       C.commandid as command_cnt,
       S.usename,
       S.query,
       C.segid,
       C.slice,
       C.optype,
       C.size,
       C.numfiles,
       C.prefix
FROM all_entries C LEFT OUTER JOIN gp_stat_activity S
ON C.sessionid = S.sess_id and C.segid=S.gp_segment_id;

GRANT SELECT ON gp_toolkit.gp_workfile_entries TO public;

CREATE VIEW gp_toolkit.gp_workfile_usage_per_segment AS
SELECT gpseg.content AS segid, COALESCE(SUM(wfe.size),0) AS size,
       SUM(wfe.numfiles) AS numfiles
FROM (
         SELECT content
         FROM gp_segment_configuration
         WHERE role = 'p') gpseg
         LEFT JOIN gp_toolkit.gp_workfile_entries wfe
                   ON (gpseg.content = wfe.segid)
GROUP BY gpseg.content;

GRANT SELECT ON gp_toolkit.gp_workfile_usage_per_segment TO public;

CREATE VIEW gp_toolkit.gp_workfile_usage_per_query AS
SELECT datname, pid, sess_id, command_cnt, usename, query, segid,
       SUM(size) AS size, SUM(numfiles) AS numfiles
FROM gp_toolkit.gp_workfile_entries
GROUP BY datname, pid, sess_id, command_cnt, usename, query, segid;

GRANT SELECT ON gp_toolkit.gp_workfile_usage_per_query TO public;

CREATE FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_coordinator()
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_workfile_mgr_used_diskspace_here()
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_coordinator() TO public;

/* prefer the *_coordinator function, but keep this for backwards compatibility */
CREATE FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_master()
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM gp_toolkit.__gp_workfile_mgr_used_diskspace_here()
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_master() TO public;

CREATE FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_segments()
RETURNS SETOF record
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	RETURN QUERY SELECT (gp_toolkit.__gp_workfile_mgr_used_diskspace_here()).* FROM gp_dist_random('gp_id');
END
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_segments() TO public;

CREATE VIEW gp_toolkit.gp_workfile_mgr_used_diskspace AS
  SELECT C.*
	FROM gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_coordinator() as C (
	  segid int,
	  bytes bigint
	)
  UNION ALL
  SELECT C.*
	FROM gp_toolkit.__gp_workfile_mgr_used_diskspace_f_on_segments() as C (
	  segid int,
	  bytes bigint
	)
ORDER BY segid;

GRANT SELECT ON gp_toolkit.gp_workfile_mgr_used_diskspace TO public;

/* ------------------------------------------------------------------------- */
/* ANALYZE of a partitioned table                                            */
/* ------------------------------------------------------------------------- */

/*
 * A segment's sample of a table and every table under it, as one sample of
 * the table's rows, for ANALYZE of the tree on the coordinator -- what
 * Cloudberry's gp_acquire_sample_rows(t, n, 't') samples (gp_analyze.c).
 * The rows come as sample_rows()'s do, each member's as a row of the
 * table's own type; the caller may read or ANALYZE the table, as there.
 */
CREATE FUNCTION gp_internal.sample_tree(
	rel anyelement,
	targrows int,
	OUT totalrows float8,
	OUT totaldeadrows float8,
	OUT sample anyelement)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_sample_tree'
LANGUAGE C;

/*
 * Cloudberry's HyperLogLog counter, which ANALYZE keeps of a leaf's column
 * so that the root's number of distinct values can be merged from its
 * leaves', and the aggregate ANALYZE FULLSCAN counts a leaf's column with
 * (gp_hll.c).  In pg_catalog under Cloudberry's names, where Cloudberry's
 * catalog has them.  Like Cloudberry's, it travels as text: base 64, no
 * binary form.
 */
CREATE TYPE pg_catalog.gp_hyperloglog_estimator;

CREATE FUNCTION pg_catalog.gp_hyperloglog_in(value cstring)
RETURNS pg_catalog.gp_hyperloglog_estimator
AS 'MODULE_PATHNAME', 'gp_hyperloglog_in'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pg_catalog.gp_hyperloglog_out(counter pg_catalog.gp_hyperloglog_estimator)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gp_hyperloglog_out'
LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE pg_catalog.gp_hyperloglog_estimator (
	INPUT = pg_catalog.gp_hyperloglog_in,
	OUTPUT = pg_catalog.gp_hyperloglog_out,
	INTERNALLENGTH = VARIABLE,
	ALIGNMENT = int4,
	STORAGE = extended,
	CATEGORY = 'X');

COMMENT ON TYPE pg_catalog.gp_hyperloglog_estimator IS
	'gp_hyperloglog_estimator''s internal bytea representation for hyperloglog counter';

CREATE FUNCTION pg_catalog.gp_hyperloglog_comp(counter pg_catalog.gp_hyperloglog_estimator)
RETURNS pg_catalog.gp_hyperloglog_estimator
AS 'MODULE_PATHNAME', 'gp_hyperloglog_comp'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pg_catalog.gp_hyperloglog_merge(
	estimator1 pg_catalog.gp_hyperloglog_estimator,
	estimator2 pg_catalog.gp_hyperloglog_estimator)
RETURNS pg_catalog.gp_hyperloglog_estimator
AS 'MODULE_PATHNAME', 'gp_hyperloglog_merge'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pg_catalog.gp_hyperloglog_get_estimate(counter pg_catalog.gp_hyperloglog_estimator)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_hyperloglog_get_estimate'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pg_catalog.gp_hyperloglog_add_item_agg_default(
	counter pg_catalog.gp_hyperloglog_estimator, item anyelement)
RETURNS pg_catalog.gp_hyperloglog_estimator
AS 'MODULE_PATHNAME', 'gp_hyperloglog_add_item_agg_default'
LANGUAGE C IMMUTABLE;

CREATE AGGREGATE pg_catalog.gp_hyperloglog_accum(anyelement) (
	SFUNC = pg_catalog.gp_hyperloglog_add_item_agg_default,
	STYPE = pg_catalog.gp_hyperloglog_estimator,
	FINALFUNC = pg_catalog.gp_hyperloglog_comp,
	COMBINEFUNC = pg_catalog.gp_hyperloglog_merge);

/*
 * A leaf partition's HyperLogLog counter of each column ANALYZE took, which
 * its root's number of distinct values is merged from (gp_partmerge.c).
 * Cloudberry keeps it in the last slot of the leaf's pg_statistic row,
 * under kinds 98 and 99, which are in PostgreSQL's range of kinds; here it
 * goes with that row by the row's xmin, and one whose row has been replaced
 * since is not read.  Only gp_core reads and writes it.
 */
CREATE TABLE gp_internal.leaf_hll (
	starelid oid NOT NULL,
	staattnum int2 NOT NULL,
	staxmin xid NOT NULL,
	fullscan bool NOT NULL,
	counter bytea NOT NULL
);
CREATE INDEX leaf_hll_attnum ON gp_internal.leaf_hll (starelid, staattnum);
REVOKE ALL ON gp_internal.leaf_hll FROM PUBLIC;

/*
 * ------------------------------------------------------------------------
 * Parallel retrieve cursors (gp_endpoint.c)
 * ------------------------------------------------------------------------
 *
 * Cloudberry's functions and views of the endpoints, by its names and
 * columns: gp_get_endpoints() the cluster's, from the coordinator,
 * gp_get_segment_endpoints() a node's own, gp_get_session_endpoints() the
 * session's, each the user's own unless a superuser's, and
 * gp_wait_parallel_retrieve_cursor(), which waits for a cursor's endpoints
 * to be read.  The rest are gp_core's, each the coordinator's to call on a
 * segment or a retrieve session's: a segment's endpoints for the
 * coordinator's list, an endpoint opened and released by the reader that
 * runs its slice, a writer's transaction published for those readers, and
 * the rows of RETRIEVE.
 */
CREATE FUNCTION pg_catalog.gp_get_endpoints(
	OUT gp_segment_id int4, OUT auth_token text, OUT cursorname text,
	OUT sessionid int4, OUT hostname varchar, OUT port int4,
	OUT username text, OUT state text, OUT endpointname text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_get_endpoints'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_catalog.gp_get_segment_endpoints(
	OUT auth_token text, OUT databaseid oid, OUT senderpid int4,
	OUT receiverpid int4, OUT state text, OUT gp_segment_id int4,
	OUT sessionid int4, OUT username text, OUT endpointname text,
	OUT cursorname text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_get_segment_endpoints'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_catalog.gp_get_session_endpoints(
	OUT gp_segment_id int4, OUT auth_token text, OUT cursorname text,
	OUT sessionid int4, OUT hostname varchar, OUT port int4,
	OUT username text, OUT state text, OUT endpointname text)
RETURNS SETOF record
LANGUAGE sql VOLATILE
AS $$
	SELECT * FROM pg_catalog.gp_get_endpoints()
	 WHERE sessionid = pg_catalog.current_setting('gp.session_id')::int4
$$;

CREATE FUNCTION pg_catalog.gp_wait_parallel_retrieve_cursor(
	cursorname text, timeout_sec int4, OUT finished bool)
RETURNS SETOF bool
AS 'MODULE_PATHNAME', 'gp_wait_parallel_retrieve_cursor'
LANGUAGE C VOLATILE STRICT;

SET allow_system_table_mods = on;
CREATE VIEW pg_catalog.gp_endpoints AS
	SELECT * FROM pg_catalog.gp_get_endpoints();
CREATE VIEW pg_catalog.gp_segment_endpoints AS
	SELECT * FROM pg_catalog.gp_get_segment_endpoints();
CREATE VIEW pg_catalog.gp_session_endpoints AS
	SELECT * FROM pg_catalog.gp_get_session_endpoints();
RESET allow_system_table_mods;
GRANT SELECT ON pg_catalog.gp_endpoints, pg_catalog.gp_segment_endpoints,
	pg_catalog.gp_session_endpoints TO PUBLIC;

CREATE FUNCTION gp_internal.segment_endpoints(
	OUT auth_token text, OUT cursorname text, OUT sessionid int4,
	OUT userid oid, OUT state text, OUT endpointname text,
	OUT gp_segment_id int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_segment_endpoints'
LANGUAGE C VOLATILE;

CREATE FUNCTION gp_internal.endpoint_open(name text, cursorname text,
	session int4, userid oid, token text, columns text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_endpoint_open'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.endpoint_release(name text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_endpoint_release'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.share_publish(key text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_share_publish'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_internal.retrieve(endpoint text, count int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_retrieve'
LANGUAGE C VOLATILE STRICT;
