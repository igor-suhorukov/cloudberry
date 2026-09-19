/* pg19/modules/gp_orca/gp_orca--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_orca" to load this file. \quit

CREATE SCHEMA gp_orca;
GRANT USAGE ON SCHEMA gp_orca TO PUBLIC;

/*
 * What ORCA is linked in, and whether it comes up in this backend.
 *
 * "xforms" is the number of transformation rules this ORCA was built with.
 * It is what tells one ORCA from another: Cloudberry's tree and pgorca's
 * carry different rules, so this is how to see which one the server is
 * actually running.
 */
CREATE FUNCTION gp_orca.version(
	OUT source text,
	OUT xforms int,
	OUT initialized boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_version'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.version() IS
	'which ORCA is linked in, and whether it has been brought up here';

/*
 * Every transformation rule this ORCA carries, by name.
 *
 * A rule is what ORCA searches with, so this is what the optimizer can do.
 * Cloudberry's ORCA carries three rules the single-node fork does not:
 * ExfGet2ParallelTableScan, ExfPushPartialAggBelowJoin and
 * ExfImplementHashSequenceProject.
 */
CREATE FUNCTION gp_orca.xforms()
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'gp_orca_xforms'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.xforms() IS
	'the transformation rules this ORCA was built with';

/*
 * What the server-side checks say about a query, before ORCA is asked to plan
 * it.  ORCA declines some shapes on these answers.
 *
 * "orderby_ordering_op" is the KNN shape: ORDER BY over an ordering operator
 * on a plain column, which PostgreSQL's planner turns into a GiST index scan
 * and ORCA cannot, so ORCA leaves the query to the planner.
 *
 * "non_default_collation" is whether anything in the query carries a collation
 * that is not the default one.  Cloudberry's own copy of this check still
 * carries a merge marker from PostgreSQL 9.1 ("GPDB_91_MERGE_FIXME:
 * collation"), so it answers that one question and no more.
 */
CREATE FUNCTION gp_orca.explain_refusal(
	sql text,
	OUT orderby_ordering_op boolean,
	OUT non_default_collation boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_explain_refusal'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp_orca.explain_refusal(text) FROM PUBLIC;

COMMENT ON FUNCTION gp_orca.explain_refusal(text) IS
	'what the checks in front of ORCA say about a query';
