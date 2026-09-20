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
	OUT single_node boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_node'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp.node() IS
	'the role, segment count, content id and single-node flag of this node';

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
