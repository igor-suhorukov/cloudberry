/* pg19/test/storage/gp_storage_probe--1.0.sql */

\echo Use "CREATE EXTENSION gp_storage_probe" to load this file. \quit

/*
 * The calling user's mapping for a storage server, as gp_sql hands it to a
 * storage handler: read in gp.maintenance_database, the coordinator's from a
 * segment.  "name=value" pairs, or NULL for none.
 */
CREATE FUNCTION gp_storage_probe.credentials(server text) RETURNS text
AS 'MODULE_PATHNAME', 'gp_storage_probe_credentials'
LANGUAGE C STRICT VOLATILE;
