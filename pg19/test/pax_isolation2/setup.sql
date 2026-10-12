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
