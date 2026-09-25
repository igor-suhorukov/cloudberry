/* pg19/modules/pax/pax--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION pax" to load this file. \quit

/*
 * PAX, Cloudberry's table access method that keeps a table's rows in files
 * of its own, by column.  What Cloudberry's initdb wrote into every database
 * with fixed OIDs (contrib/pax_storage/tools/gen_sql.c), in pg_catalog and
 * pg_ext_aux, is here, in the extension's schema pax -- a name beginning
 * pg_ being PostgreSQL's -- and PAX finds each by name (pax_catalog.c).
 * Each table has an aux table beside, pax.pg_pax_blocks_<relid>, made as
 * the table is: a row a file.
 */

CREATE FUNCTION pax.pax_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'pax_tableam_handler'
LANGUAGE C STRICT;
COMMENT ON FUNCTION pax.pax_tableam_handler(internal)
    IS 'column-optimized PAX table access method handler';

CREATE ACCESS METHOD pax TYPE TABLE HANDLER pax.pax_tableam_handler;
COMMENT ON ACCESS METHOD pax IS 'column-optimized PAX table access method';

/* A file's statistics, as its row of the table's aux table keeps them. */
CREATE TYPE pax.paxauxstats;

CREATE FUNCTION pax.paxauxstats_in(cstring)
RETURNS pax.paxauxstats
AS 'MODULE_PATHNAME', 'MicroPartitionStatsInput'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION pax.paxauxstats_out(pax.paxauxstats)
RETURNS cstring
AS 'MODULE_PATHNAME', 'MicroPartitionStatsOutput'
LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE pax.paxauxstats (
    INPUT = pax.paxauxstats_in,
    OUTPUT = pax.paxauxstats_out,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT = int4,
    STORAGE = extended
);

/*
 * A PAX table's aux table, and the storage it describes: the table's
 * tablespace (0 for the database's) and relfilenode as its files were made,
 * which a rewrite swaps from one relation to another without asking PAX, so
 * that the row follows them; and the layout of its row IDs, the bits of a
 * block number its file number takes (pax_tid.h).  Each node's own, as the
 * table's files are.
 */
CREATE TABLE pax.pg_pax_tables (
    relid         oid NOT NULL,
    auxrelid      oid NOT NULL,
    reltablespace oid NOT NULL,
    relfilenode   oid NOT NULL,
    filebits      int2 NOT NULL
);
CREATE UNIQUE INDEX pg_pax_tables_relid_index
    ON pax.pg_pax_tables (relid);
CREATE INDEX pg_pax_tables_storage_index
    ON pax.pg_pax_tables (reltablespace, relfilenode);

/*
 * The number a table's next file takes, updated in place, so that an
 * aborted insert's file number is never given again.
 */
CREATE TABLE pax.pg_pax_fastsequence (
    objid oid NOT NULL,
    seq   int NOT NULL
);
CREATE INDEX pg_pax_fastsequence_objid_idx
    ON pax.pg_pax_fastsequence (objid);

GRANT USAGE ON SCHEMA pax TO PUBLIC;
