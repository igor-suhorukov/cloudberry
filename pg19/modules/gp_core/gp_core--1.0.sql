/* pg19/modules/gp_core/gp_core--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_core" to load this file. \quit

/*
 * "gp" holds what users call; "gp_internal" holds what the dispatcher calls on
 * a segment.  Anyone may reach the schema: the dispatcher connects to a
 * segment as the session's own user, and ANALYZE by a table's owner has to be
 * able to call sample_rows() there.  So every function in it either checks the
 * caller's privileges itself or has EXECUTE revoked from PUBLIC.
 */
CREATE SCHEMA IF NOT EXISTS gp_internal;
GRANT USAGE ON SCHEMA gp_internal TO PUBLIC;

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
 * Where a segment process receives the rows of a Motion whose slices run at
 * once (gp.interconnect_type = tcp), opening its listener on first use.
 * Only from a connection that carries the cluster secret.
 */
CREATE FUNCTION gp_internal.interconnect_address()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_interconnect_address'
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
 * Cloudberry's catalogs of the cluster, by Cloudberry's names and in its
 * columns, where its own are: pg_catalog, so that a query written for
 * Cloudberry finds them whatever the search path is.  Cloudberry's are
 * shared catalogs, and an extension can make none, so each database has
 * them as this script makes them there (gp_catalog.c).  PostgreSQL makes no
 * relation in pg_catalog unless allow_system_table_mods is on; a script's
 * setting lasts as long as the script.
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
 * cluster.  Nothing writes it until FTS does, at M4, but a user may, with
 * allow_system_table_mods on, as a catalog is written: so it is a table, one
 * in each database where Cloudberry's is one for the node.
 */
CREATE TABLE pg_catalog.gp_configuration_history (
	"time" timestamptz NOT NULL,
	dbid int2 NOT NULL,
	"desc" text
);

CREATE FUNCTION gp_internal.catalog_write_check()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_catalog_write_check'
LANGUAGE C;

CREATE TRIGGER gp_catalog_write_check
	BEFORE INSERT OR UPDATE OR DELETE OR TRUNCATE
	ON pg_catalog.gp_configuration_history
	FOR EACH STATEMENT EXECUTE FUNCTION gp_internal.catalog_write_check();

GRANT SELECT ON pg_catalog.gp_id, pg_catalog.gp_segment_configuration,
	pg_catalog.gp_configuration_history TO PUBLIC;

RESET allow_system_table_mods;

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
