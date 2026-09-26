/* pg19/modules/pax/pax--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION pax" to load this file. \quit

/*
 * PAX, Cloudberry's table access method that keeps a table's rows in files
 * of its own, by column.  What Cloudberry's initdb wrote into every database
 * with fixed OIDs (contrib/pax_storage/tools/gen_sql.c), in pg_catalog and
 * pg_ext_aux, is here, in the extension's schema pax, and PAX finds each by
 * name (pax_catalog.c).  Each table has an aux table beside,
 * pg_ext_aux.pg_pax_blocks_<relid>, made as the table is: a row a file.
 * pg_ext_aux is Cloudberry's schema of them, whose name, beginning pg_,
 * keeps pg_dump from dumping them -- a restored table makes its own, and its
 * files are not dumped with it -- and which PostgreSQL makes only with
 * allow_system_table_mods, as gp_ao makes pg_bitmapindex.
 */
SET allow_system_table_mods = on;
CREATE SCHEMA pg_ext_aux;
RESET allow_system_table_mods;

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

/*
 * In binary, the value's bytes, as bytea's are sent: its text is a summary
 * PAX does not read back, and the rows a segment sends the coordinator,
 * pax_get_catalog_rows()'s, travel in binary where every column can.
 * Cloudberry's type has neither.
 */
CREATE FUNCTION pax.paxauxstats_recv(internal)
RETURNS pax.paxauxstats
AS 'bytearecv'
LANGUAGE internal STRICT IMMUTABLE;

CREATE FUNCTION pax.paxauxstats_send(pax.paxauxstats)
RETURNS bytea
AS 'byteasend'
LANGUAGE internal STRICT IMMUTABLE;

CREATE TYPE pax.paxauxstats (
    INPUT = pax.paxauxstats_in,
    OUTPUT = pax.paxauxstats_out,
    RECEIVE = pax.paxauxstats_recv,
    SEND = pax.paxauxstats_send,
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
