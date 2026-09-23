/* pg19/modules/gp_core/gp_inject_fault--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_inject_fault" to load this file. \quit

/*
 * Cloudberry's gp_inject_fault, under the same names and with the same
 * arguments, as its tests call them: set, reset or ask about a fault on the
 * node of db_id.  gp_core carries it (gp_fault.c); the places that ask for a
 * fault are the port's own, and PostgreSQL 19's injection points.
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
LANGUAGE C VOLATILE STRICT NO SQL;

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
