/* pg19/modules/gp_exttable/gp_exttable--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_exttable" to load this file. \quit

/*
 * The foreign data wrapper an external table is a foreign table of, and its
 * one server, as Cloudberry's gp_exttable_fdw extension makes them
 * (gpcontrib/gp_exttable_fdw/gp_exttable_fdw--1.0.sql).
 */
CREATE FUNCTION gp_exttable_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION gp_exttable_permission_check(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER gp_exttable_fdw
	HANDLER gp_exttable_fdw_handler
	VALIDATOR gp_exttable_permission_check;

CREATE SERVER gp_exttable_server FOREIGN DATA WRAPPER gp_exttable_fdw;

-- Who may make an external table is the protocol's to say -- a file's for a
-- superuser, gpfdist's and http's for a role CREATEEXTTABLE lets, a protocol
-- of the user's for whom it is granted to (option.c) -- as Cloudberry's
-- CREATE EXTERNAL TABLE asks no one about the server.
GRANT USAGE ON FOREIGN SERVER gp_exttable_server TO PUBLIC;

/*
 * What Cloudberry has in pg_catalog, which its tests and tools name bare:
 * the catalog of external tables, and the functions of their error logs.  A
 * name that starts pg_ is PostgreSQL's to give in pg_catalog; the setting
 * that lets a superuser give it anyway is set for this script alone.
 */
SET allow_system_table_mods = on;

CREATE FUNCTION pg_catalog.pg_exttable(OUT reloid oid,
									   OUT urilocation text[],
									   OUT execlocation text[],
									   OUT fmttype "char",
									   OUT fmtopts text,
									   OUT options text[],
									   OUT command text,
									   OUT rejectlimit int4,
									   OUT rejectlimittype "char",
									   OUT logerrors bool,
									   OUT encoding int4,
									   OUT writable bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_exttable'
LANGUAGE C VOLATILE;

CREATE VIEW pg_catalog.pg_exttable AS SELECT * FROM pg_catalog.pg_exttable();
GRANT SELECT ON pg_catalog.pg_exttable TO PUBLIC;

/*
 * The error logs: every node's rows of a table's, the segments' and the
 * coordinator's (sreh.c).
 */
CREATE FUNCTION pg_catalog.gp_read_error_log(exttable text,
											 OUT cmdtime timestamptz,
											 OUT relname text,
											 OUT filename text,
											 OUT linenum int4,
											 OUT bytenum int4,
											 OUT errmsg text,
											 OUT rawdata text,
											 OUT rawbytes bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_read_error_log'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_catalog.gp_truncate_error_log(text)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_truncate_error_log'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_catalog.gp_read_persistent_error_log(exttable text,
														OUT cmdtime timestamptz,
														OUT relname text,
														OUT filename text,
														OUT linenum int4,
														OUT bytenum int4,
														OUT errmsg text,
														OUT rawdata text,
														OUT rawbytes bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_read_persistent_error_log'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_catalog.gp_truncate_persistent_error_log(text)
RETURNS bool
AS 'MODULE_PATHNAME', 'gp_truncate_persistent_error_log'
LANGUAGE C STRICT VOLATILE;

/*
 * The protocols of the user's, which Cloudberry keeps in its catalog
 * pg_extprotocol: here a table of this extension's, which the view of that
 * name shows, changed by the procedures below (protocol.c), each of which
 * the grammar makes a statement of Cloudberry's into -- CREATE [TRUSTED]
 * PROTOCOL, DROP PROTOCOL, ALTER PROTOCOL ... RENAME TO and OWNER TO, GRANT
 * and REVOKE ... ON PROTOCOL.
 */
CREATE TABLE gp_exttable.protocol (
	oid oid NOT NULL PRIMARY KEY,
	ptcname name NOT NULL UNIQUE,
	ptcreadfn regproc,
	ptcwritefn regproc,
	ptcvalidatorfn regproc,
	ptcowner oid NOT NULL,
	ptctrusted bool NOT NULL,
	ptcacl aclitem[]
);

CREATE VIEW pg_catalog.pg_extprotocol AS
	SELECT oid, ptcname, ptcreadfn, ptcwritefn, ptcvalidatorfn, ptcowner,
		   ptctrusted, ptcacl
	FROM gp_exttable.protocol;
GRANT SELECT ON pg_catalog.pg_extprotocol TO PUBLIC;

RESET allow_system_table_mods;

GRANT USAGE ON SCHEMA gp_exttable TO PUBLIC;
GRANT SELECT ON gp_exttable.protocol TO PUBLIC;

CREATE PROCEDURE gp_exttable.create_protocol(name text, trusted bool,
											 definition text[],
											 coordinator_oid oid DEFAULT 0)
AS 'MODULE_PATHNAME', 'gp_exttable_create_protocol'
LANGUAGE C;

CREATE PROCEDURE gp_exttable.drop_protocol(names text[], if_exists bool,
										   cascade bool)
AS 'MODULE_PATHNAME', 'gp_exttable_drop_protocol'
LANGUAGE C;

CREATE PROCEDURE gp_exttable.rename_protocol(name text, newname text)
AS 'MODULE_PATHNAME', 'gp_exttable_rename_protocol'
LANGUAGE C;

CREATE PROCEDURE gp_exttable.alter_protocol_owner(name text, owner text)
AS 'MODULE_PATHNAME', 'gp_exttable_alter_protocol_owner'
LANGUAGE C;

CREATE PROCEDURE gp_exttable.grant_protocol(is_grant bool, privileges text[],
											names text[], grantees text[],
											grant_option bool, cascade bool)
AS 'MODULE_PATHNAME', 'gp_exttable_grant_protocol'
LANGUAGE C;
