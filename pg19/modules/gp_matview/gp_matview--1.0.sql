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
