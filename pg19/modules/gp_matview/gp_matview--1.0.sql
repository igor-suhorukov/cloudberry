/* pg19/modules/gp_matview/gp_matview--1.0.sql */

\echo Use "CREATE EXTENSION gp_matview" to load this file. \quit

/*
 * The trigger functions that keep an incrementally maintained materialized
 * view up to date.  They are not called directly: CREATE MATERIALIZED VIEW
 * ... WITH (gp.incremental) puts the triggers on the base tables.
 */
CREATE FUNCTION gp_matview.ivm_immediate_before()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_ivm_immediate_before'
LANGUAGE C;

CREATE FUNCTION gp_matview.ivm_immediate_maintenance()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_ivm_immediate_maintenance'
LANGUAGE C;

REVOKE ALL ON FUNCTION gp_matview.ivm_immediate_before() FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_matview.ivm_immediate_maintenance() FROM PUBLIC;

/*
 * Was this row of a base table there before the statement ran?  A view over
 * more than one table is maintained by reading the tables the statement
 * changed as they were before it, and this is what says which rows those
 * were.  It is called from a subquery the module builds, in the statement's
 * own session, and errors unless that session is maintaining the view it is
 * asked about -- so it is left executable, because the maintenance runs as
 * whoever wrote to the base table.
 */
CREATE FUNCTION gp_matview.visible_in_prestate(tableoid oid, ctid tid, matview oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'gp_ivm_visible_in_prestate'
LANGUAGE C STRICT;

/*
 * How views have been kept up to date since the counters were reset. The
 * contents of a view do not say whether a delta or a whole recomputation
 * produced them, and the tests need to know.
 */
CREATE FUNCTION gp_matview.stats_reset() RETURNS void
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_reset' LANGUAGE C;
CREATE FUNCTION gp_matview.applied_delta() RETURNS bigint
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_delta' LANGUAGE C;
CREATE FUNCTION gp_matview.recomputed() RETURNS bigint
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_recompute' LANGUAGE C;
