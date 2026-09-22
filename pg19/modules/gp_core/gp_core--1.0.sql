/* pg19/modules/gp_core/gp_core--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_core" to load this file. \quit

/*
 * "gp" holds what users call; "gp_internal" holds what the dispatcher calls on
 * a segment.  Keeping them apart means the segment entry points can be revoked
 * from PUBLIC without taking the user-facing functions with them.
 */
CREATE SCHEMA IF NOT EXISTS gp_internal;
REVOKE ALL ON SCHEMA gp_internal FROM PUBLIC;

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
