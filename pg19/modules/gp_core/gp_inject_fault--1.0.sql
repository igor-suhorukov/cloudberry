/* pg19/modules/gp_core/gp_inject_fault--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_inject_fault" to load this file. \quit

/*
 * Cloudberry's gp_inject_fault, under the same names and with the same
 * arguments, as its tests call them: set, reset or ask about a fault on the
 * node of db_id.  gp_core carries it (gp_fault.c); the places that ask for a
 * fault are the port's own, and PostgreSQL 19's injection points.
 *
 * Cloudberry's script says NO SQL of the C function.  That is its default
 * for a function in any language but SQL, and a clause only gp_sql's rewrite
 * reads (funcattr.c), so it is left out: the extension is then made on a
 * server that loads gp_core without gp_sql, as the fault injector is
 * gp_core's.
 */
CREATE FUNCTION gp_inject_fault(
  faultname text,
  type text,
  ddl text,
  database text,
  tablename text,
  start_occurrence int4,
  end_occurrence int4,
  extra_arg int4,
  db_id int4,
  gp_session_id int4)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_inject_fault'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp_inject_fault(
  faultname text,
  type text,
  ddl text,
  database text,
  tablename text,
  start_occurrence int4,
  end_occurrence int4,
  extra_arg int4,
  db_id int4)
RETURNS text
AS $$ select gp_inject_fault($1, $2, $3, $4, $5, $6, $7, $8, $9, -1) $$
LANGUAGE SQL;

CREATE FUNCTION gp_inject_fault(
  faultname text,
  type text,
  db_id int4)
RETURNS text
AS $$ select gp_inject_fault($1, $2, '', '', '', 1, 1, 0, $3, -1) $$
LANGUAGE SQL;

CREATE FUNCTION gp_inject_fault(
  faultname text,
  type text,
  db_id int4,
  gp_session_id int4)
RETURNS text
AS $$ select gp_inject_fault($1, $2, '', '', '', 1, 1, 0, $3, $4) $$
LANGUAGE SQL;

CREATE FUNCTION gp_inject_fault_infinite(
  faultname text,
  type text,
  db_id int4)
RETURNS text
AS $$ select gp_inject_fault($1, $2, '', '', '', 1, -1, 0, $3, -1) $$
LANGUAGE SQL;

CREATE FUNCTION gp_wait_until_triggered_fault(
  faultname text,
  numtimestriggered int4,
  db_id int4)
RETURNS text
AS $$ select gp_inject_fault($1, 'wait_until_triggered', '', '', '', 1, 1, $2, $3, -1) $$
LANGUAGE SQL;

/*
 * force_mirrors_to_catch_up(): every mirror of the cluster, and the
 * coordinator's standby, has replayed what its primary has written, which a
 * test waits for before it looks at a mirror's files.  Cloudberry's writes
 * a no-op record on each node and waits for each mirror's fault at its
 * redo; here each node waits for the standbys streaming from it
 * (gp_internal.mirror_replay_wait(), gp_core's).
 */
CREATE FUNCTION force_mirrors_to_catch_up() RETURNS void AS $$
BEGIN
	PERFORM gp_internal.mirror_replay_wait();
	IF EXISTS (SELECT 1 FROM gp.segment_configuration() WHERE content >= 0) THEN
		PERFORM r FROM gp.exec_on_segments('SELECT gp_internal.mirror_replay_wait()') r;
	END IF;
END;
$$ LANGUAGE plpgsql;

/*
 * Who may inject a fault: whom EXECUTE on the function that does it is
 * granted to, the wrappers above running as their caller.  Cloudberry's
 * script grants PUBLIC, as a script does unless it says otherwise, and its
 * tests call these as roles of their own; here PUBLIC has none, since a fault
 * stops a server as readily as it tests one, and the test suites' setups
 * grant it.
 */
REVOKE EXECUTE ON FUNCTION gp_inject_fault(text, text, text, text, text,
	int4, int4, int4, int4, int4) FROM PUBLIC;
