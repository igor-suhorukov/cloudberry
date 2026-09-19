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
