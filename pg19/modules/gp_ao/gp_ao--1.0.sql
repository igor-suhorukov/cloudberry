/* pg19/modules/gp_ao/gp_ao--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_ao" to load this file. \quit

/*
 * Append-optimized tables, by row (ao_row) and by column (ao_column), as two
 * table access methods of Cloudberry's names.  Their data is in 8K pages of
 * the table's own files, through the buffer manager and the module's own WAL
 * resource manager; what Cloudberry keeps in pg_appendonly and the per-table
 * pg_aoseg, pg_aovisimap and pg_aoblkdir relations is kept in the three
 * tables below, shared by every append-optimized table of the database and
 * keyed by the storage ID each table's first page holds, so that a row
 * follows its table through TRUNCATE and every rewrite.
 */

CREATE FUNCTION gp_ao.ao_row_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'gp_ao_row_handler'
LANGUAGE C STRICT;

CREATE FUNCTION gp_ao.ao_column_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'gp_ao_column_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD ao_row TYPE TABLE HANDLER gp_ao.ao_row_handler;
COMMENT ON ACCESS METHOD ao_row IS 'append-optimized table, stored by row';

CREATE ACCESS METHOD ao_column TYPE TABLE HANDLER gp_ao.ao_column_handler;
COMMENT ON ACCESS METHOD ao_column IS 'append-optimized table, stored by column';

/* Each new file of an append-optimized table takes the next of these. */
CREATE SEQUENCE gp_ao.storage_id_seq NO CYCLE;

/*
 * A segment file of a table: its committed length in each of its files --
 * one for a table by row, one a column for a table by column -- and how many
 * rows and blocks it holds.  A writer takes a segment file no other writer
 * holds, appends to it, and updates this row as its insert ends, so a reader
 * reads each file as far as the row its snapshot sees says.  state is 1, in
 * use, or 2, awaiting drop, as Cloudberry's.
 */
CREATE TABLE gp_ao.segfile (
	storage_id		bigint NOT NULL,
	segno			integer NOT NULL,
	eof				bigint[] NOT NULL,
	eof_uncompressed bigint[] NOT NULL,
	tupcount		bigint NOT NULL,
	varblockcount	bigint NOT NULL,
	modcount		bigint NOT NULL,
	state			smallint NOT NULL,
	formatversion	smallint NOT NULL,
	compacted_by	xid
);
CREATE UNIQUE INDEX segfile_key ON gp_ao.segfile (storage_id, segno);

/*
 * The rows of a segment file a transaction deleted: one bit a row, for
 * 32768 row numbers from first_row.
 */
CREATE TABLE gp_ao.visimap (
	storage_id		bigint NOT NULL,
	segno			integer NOT NULL,
	first_row		bigint NOT NULL,
	bitmap			bytea NOT NULL
);
CREATE UNIQUE INDEX visimap_key ON gp_ao.visimap (storage_id, segno, first_row);

/*
 * Where each block of rows is: the rows first_row to first_row + nrows - 1
 * of a segment file start at offsets[i] of its file i.  An index finds a row
 * by its row number through this.
 */
CREATE TABLE gp_ao.blkdir (
	storage_id		bigint NOT NULL,
	segno			integer NOT NULL,
	first_row		bigint NOT NULL,
	nrows			integer NOT NULL,
	offsets			bigint[] NOT NULL
);
CREATE UNIQUE INDEX blkdir_key ON gp_ao.blkdir (storage_id, segno, first_row);

/*
 * The storage ID of a table's current files; NULL for a table of another
 * access method.
 */
CREATE FUNCTION gp_ao.storage_id(rel regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'gp_ao_storage_id'
LANGUAGE C STRICT;

/* A table's options as the access method reads them. */
CREATE FUNCTION gp_ao.options(rel regclass,
	OUT blocksize integer, OUT compresstype text, OUT compresslevel integer,
	OUT checksum boolean, OUT columnstore boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_ao_options'
LANGUAGE C STRICT;

GRANT USAGE ON SCHEMA gp_ao TO PUBLIC;
REVOKE ALL ON gp_ao.segfile, gp_ao.visimap, gp_ao.blkdir FROM PUBLIC;
GRANT SELECT ON gp_ao.segfile, gp_ao.visimap, gp_ao.blkdir TO PUBLIC;
REVOKE ALL ON SEQUENCE gp_ao.storage_id_seq FROM PUBLIC;

/* ------------------------------------------------------------------------- */
/* What Cloudberry shows of an append-optimized table                        */
/* ------------------------------------------------------------------------- */

/*
 * pg_appendonly: a row for each append-optimized table, with Cloudberry's
 * columns.  The options are what pg_class.reloptions holds, read without
 * opening the table; the port has no relations of its own for a table's
 * segment files, visibility map and block directory, so their OIDs are 0,
 * and segfilecount is how many segment files the table has on this node.
 */
CREATE FUNCTION gp_ao.reloption_values(relam oid, relkind "char", reloptions text[],
	OUT blocksize integer, OUT compresstype text, OUT compresslevel integer,
	OUT checksum boolean, OUT columnstore boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_ao_reloption_values'
LANGUAGE C STABLE;

CREATE FUNCTION gp_ao.segfile_count(rel oid)
RETURNS integer
AS 'MODULE_PATHNAME', 'gp_ao_segfile_count'
LANGUAGE C STRICT STABLE;

/*
 * A name that starts pg_ is PostgreSQL's to give in pg_catalog, and the
 * setting that lets a superuser give it anyway is set for this view alone;
 * CREATE EXTENSION puts it back when the script ends.
 */
SET allow_system_table_mods = on;
CREATE VIEW pg_catalog.pg_appendonly AS
SELECT c.oid AS relid,
	   o.blocksize,
	   o.compresslevel::int2 AS compresslevel,
	   o.checksum,
	   o.compresstype::name AS compresstype,
	   o.columnstore,
	   0::oid AS segrelid,
	   gp_ao.segfile_count(c.oid)::int2 AS segfilecount,
	   2::int2 AS version,
	   0::oid AS blkdirrelid,
	   0::oid AS blkdiridxid,
	   0::oid AS visimaprelid,
	   0::oid AS visimapidxid
  FROM pg_catalog.pg_class c
	   CROSS JOIN LATERAL gp_ao.reloption_values(c.relam, c.relkind, c.reloptions) o
 WHERE c.relkind IN ('r', 'm')
   AND c.relam IN (SELECT oid FROM pg_catalog.pg_am
					WHERE amname IN ('ao_row', 'ao_column'));
RESET allow_system_table_mods;
GRANT SELECT ON pg_catalog.pg_appendonly TO PUBLIC;

/*
 * get_ao_compression_ratio(), which Cloudberry has built in, and
 * get_ao_distribution(): how many rows each segment's segment files hold.
 */
CREATE FUNCTION pg_catalog.get_ao_compression_ratio(regclass)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_ao_compression_ratio'
LANGUAGE C STRICT;

/*
 * gp_toolkit's functions of append-optimized tables, under Cloudberry's
 * names and in its shapes.  gp_toolkit itself is not a module of the port
 * yet; its schema is made here if there is none.  The ones Cloudberry runs
 * on every segment are labelled to, as EXECUTE ON ALL SEGMENTS is.
 */
CREATE SCHEMA IF NOT EXISTS gp_toolkit;
GRANT USAGE ON SCHEMA gp_toolkit TO PUBLIC;

CREATE FUNCTION gp_toolkit.__gp_aoseg(regclass)
RETURNS TABLE (segment_id integer, segno integer, eof bigint, tupcount bigint,
	varblockcount bigint, eof_uncompressed bigint, modcount bigint,
	formatversion smallint, state smallint)
AS 'MODULE_PATHNAME', 'gp_ao_aoseg'
LANGUAGE C STRICT;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_aoseg(regclass) IS 'execute_on=all_segments';

CREATE FUNCTION gp_toolkit.__gp_aocsseg(regclass)
RETURNS TABLE (segment_id integer, segno integer, column_num smallint,
	physical_segno integer, tupcount bigint, eof bigint, eof_uncompressed bigint,
	modcount bigint, formatversion smallint, state smallint)
AS 'MODULE_PATHNAME', 'gp_ao_aocsseg'
LANGUAGE C STRICT;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_aocsseg(regclass) IS 'execute_on=all_segments';

CREATE FUNCTION gp_toolkit.__gp_aovisimap(regclass)
RETURNS TABLE (tid tid, segno integer, row_num bigint)
AS 'MODULE_PATHNAME', 'gp_ao_aovisimap'
LANGUAGE C STRICT;

CREATE FUNCTION gp_toolkit.__gp_aovisimap_hidden_info(regclass)
RETURNS TABLE (segno integer, hidden_tupcount bigint, total_tupcount bigint)
AS 'MODULE_PATHNAME', 'gp_ao_aovisimap_hidden_info'
LANGUAGE C STRICT;
SECURITY LABEL FOR gp ON FUNCTION gp_toolkit.__gp_aovisimap_hidden_info(regclass) IS 'execute_on=all_segments';

CREATE FUNCTION gp_toolkit.__gp_aovisimap_entry(regclass)
RETURNS TABLE (segno integer, first_row_num bigint, hidden_tupcount integer,
	bitmap text)
AS 'MODULE_PATHNAME', 'gp_ao_aovisimap_entry'
LANGUAGE C STRICT;

CREATE FUNCTION gp_toolkit.__gp_aoblkdir(regclass)
RETURNS TABLE (tupleid tid, segno integer, columngroup_no integer,
	entry_no integer, first_row_no bigint, file_offset bigint, row_count bigint)
AS 'MODULE_PATHNAME', 'gp_ao_aoblkdir'
LANGUAGE C STRICT;

CREATE FUNCTION pg_catalog.get_ao_distribution(regclass,
	OUT segmentid integer, OUT tupcount bigint)
RETURNS SETOF record
AS $$
	SELECT segment_id, sum(tupcount)::bigint
	  FROM (SELECT segment_id, tupcount FROM gp_toolkit.__gp_aoseg($1)
			 WHERE NOT (SELECT columnstore FROM pg_catalog.pg_appendonly WHERE relid = $1)
			UNION ALL
			SELECT segment_id, tupcount FROM gp_toolkit.__gp_aocsseg($1)
			 WHERE column_num = 0
			   AND (SELECT columnstore FROM pg_catalog.pg_appendonly WHERE relid = $1)) s
	 GROUP BY segment_id
$$ LANGUAGE sql STRICT STABLE;

GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA gp_toolkit TO PUBLIC;
