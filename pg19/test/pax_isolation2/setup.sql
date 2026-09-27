-- After ../isolation2/setup.sql: the append-optimized tables and PAX, which
-- Cloudberry has in every database, in the tests' database, and the helpers
-- PAX's copy of the suite's setup adds to Cloudberry's -- the rows of a
-- table's aux table, each segment's -- as PAX's setup makes them, its type
-- in the schema pax where Cloudberry's initdb made it in pg_ext_aux.  The
-- nodes' default table access method is pax (run.sh).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_ao;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;

-- and in template1, for a database a test makes, as Cloudberry's initdb
-- makes them in every database
\set pax_setup_db :DBNAME
\c template1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_ao;
CREATE EXTENSION IF NOT EXISTS pax;
RESET client_min_messages;
\c :pax_setup_db

CREATE OR REPLACE FUNCTION pax_get_catalog_rows(rel regclass,
  segment_id out int,
  ptblockname out int,
  pttupcount out int,
  ptblocksize out int,
  ptstatistics out pax.paxauxstats,
  ptvisimapname out name,
  pthastoast out boolean,
  ptisclustered out boolean
)
returns setof record
EXECUTE ON ALL SEGMENTS
AS '$libdir/pax', 'pax_get_catalog_rows' LANGUAGE C;

CREATE OR REPLACE FUNCTION get_pax_aux_table_all(rel regclass)
RETURNS TABLE(
  segment_id integer,
  ptblockname integer,
  pttupcount integer,
  ptstatistics pax.paxauxstats,
  ptexistvisimap bool,
  ptexistexttoast bool,
  ptisclustered bool
) AS $$
  SELECT segment_id, ptblockname, pttupcount, ptstatistics, ptvisimapname IS NOT NULL, pthastoast, ptisclustered
  FROM pax_get_catalog_rows(rel)
$$ LANGUAGE sql;

-- Cloudberry's helpers that the port's setup, which has those of the tests the
-- isolation2 suite runs, has not: a table's segments restarted, immediately
-- or fast (through the port's pg_ctl()), a wait for the coordinator's
-- PANIC, and a directory's entries and a tablespace's link counted, as
-- PAX's copy of the setup makes them.

create or replace function primary_segments_containing_data_for(table_name text) returns setof integer as $$
begin
	return query execute 'select distinct gp_segment_id from ' || table_name; /* in func */
end; /* in func */
$$ language plpgsql;

create or replace function restart_primary_segments_containing_data_for(table_name text) returns setof integer as $$
declare
	segment_id integer; /* in func */
begin
	for segment_id in select * from primary_segments_containing_data_for(table_name)
	loop
		perform pg_ctl(
      (select get_data_directory_for(segment_id)),
      'restart',
      'immediate'
    ); /* in func */
	end loop; /* in func */
end; /* in func */
$$ language plpgsql;

create or replace function clean_restart_primary_segments_containing_data_for(table_name text) returns setof integer as $$
declare
	segment_id integer; /* in func */
begin
	for segment_id in select * from primary_segments_containing_data_for(table_name)
	loop
		perform pg_ctl(
      (select get_data_directory_for(segment_id)),
      'restart',
      'fast'
    ); /* in func */
	end loop; /* in func */
end; /* in func */
$$ language plpgsql;

CREATE OR REPLACE FUNCTION wait_till_master_shutsdown()
RETURNS void AS
$$
  DECLARE
    i int; /* in func */
  BEGIN
    i := 0; /* in func */
    while i < 120 loop
      i := i + 1; /* in func */
      PERFORM pg_sleep(.5); /* in func */
    end loop; /* in func */
  END; /* in func */
$$ LANGUAGE plpgsql;

create or replace function count_of_items_in_directory(user_path text) returns text as $$
       import subprocess
       cmd = 'ls {user_path}'.format(user_path=user_path)
       results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
       return len([result for result in results.splitlines() if result != ''])
$$ language plpython3u;

create or replace function count_of_items_in_database_directory(user_path text, database_oid oid) returns int as $$
       import subprocess
       import os
       directory = os.path.join(user_path, str(database_oid))
       cmd = 'ls ' + directory
       results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
       return len([result for result in results.splitlines() if result != ''])
$$ language plpython3u;

create or replace function validate_tablespace_symlink(datadir text, tablespacedir text, dbid int, tablespace_oid oid) returns boolean as $$
    import os
    return os.readlink('%s/pg_tblspc/%d' % (datadir, tablespace_oid)) == ('%s/%d' % (tablespacedir, dbid))
$$ language plpython3u;
