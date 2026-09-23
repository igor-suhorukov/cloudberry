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

/*
 * A segment's map of the distributed transactions prepared on it: each one's
 * coordinator transaction ID, its local one, whether its second phase has
 * come and what it was, and how many subtransactions it committed (NULL when
 * a restart left it prepared and they are not known).  See gp_dtx.c.
 */
CREATE FUNCTION gp_internal.dtx_map(
	OUT gxid xid8, OUT xid xid, OUT done bool, OUT committed bool,
	OUT subtransactions int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_dtx_map'
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

GRANT SELECT ON pg_catalog.gp_id, pg_catalog.gp_segment_configuration,
	pg_catalog.gp_configuration_history, pg_catalog.gp_distribution_policy
	TO PUBLIC;

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
