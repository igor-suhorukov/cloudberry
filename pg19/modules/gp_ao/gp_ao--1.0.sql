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
	   (CASE o.compresstype WHEN 'none' THEN '' ELSE o.compresstype END)::name AS compresstype,
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

/*
 * pg_attribute_encoding and pg_type_encoding: what a column of a table by
 * column is stored with, and what a type gives a column by default, which
 * the port keeps as security labels of gp_ao's (ao_encoding.c).
 */
CREATE VIEW pg_catalog.pg_attribute_encoding AS
SELECT a.attrelid,
	   a.attnum,
	   a.attnum AS filenum,
	   pg_catalog.string_to_array(s.label, ',') AS attoptions
  FROM pg_catalog.pg_attribute a
  JOIN pg_catalog.pg_seclabel s
	ON s.objoid = a.attrelid
   AND s.classoid = 'pg_catalog.pg_class'::pg_catalog.regclass
   AND s.objsubid = a.attnum
   AND s.provider = 'gp_ao'
 WHERE a.attnum > 0 AND NOT a.attisdropped;

CREATE VIEW pg_catalog.pg_type_encoding AS
SELECT s.objoid AS typid,
	   pg_catalog.string_to_array(s.label, ',') AS typoptions
  FROM pg_catalog.pg_seclabel s
 WHERE s.classoid = 'pg_catalog.pg_type'::pg_catalog.regclass
   AND s.provider = 'gp_ao';

/*
 * pg_compression: the compression types a table's options may name, with
 * the names of Cloudberry's functions for each, which the port's blocks
 * call no function of this catalog's to reach.
 */
CREATE VIEW pg_catalog.pg_compression (compname, compconstructor, compdestructor,
	compcompressor, compdecompressor, compvalidator, compowner) AS
VALUES ('none'::name, 'gp_dummy_compression_constructor'::text,
		'gp_dummy_compression_destructor'::text, 'gp_dummy_compression_compress'::text,
		'gp_dummy_compression_decompress'::text, 'gp_dummy_compression_validator'::text,
		10::oid),
	   ('zlib', 'gp_zlib_constructor', 'gp_zlib_destructor', 'gp_zlib_compress',
		'gp_zlib_decompress', 'gp_zlib_validator', 10),
	   ('rle_type', 'gp_rle_type_constructor', 'gp_rle_type_destructor',
		'gp_rle_type_compress', 'gp_rle_type_decompress', 'gp_rle_type_validator', 10),
	   ('zstd', 'zstd_constructor', 'zstd_destructor', 'zstd_compress',
		'zstd_decompress', 'zstd_validator', 10);

RESET allow_system_table_mods;
GRANT SELECT ON pg_catalog.pg_appendonly, pg_catalog.pg_attribute_encoding,
	pg_catalog.pg_type_encoding, pg_catalog.pg_compression TO PUBLIC;

/*
 * Each stands for a catalog table of Cloudberry's, which has gp_segment_id,
 * as every table of Cloudberry's has; gp_core gives a view so labelled the
 * column too, the node's own content id (gp_segment.c).
 */
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_appendonly IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_attribute_encoding IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_type_encoding IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_compression IS 'catalog';

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
 * names and in its shapes.  gp_toolkit itself is not a module of the port;
 * its schema is gp_core's, which gp_resource puts views of its own in too.
 * The ones Cloudberry runs on every segment are labelled to, as EXECUTE ON
 * ALL SEGMENTS is.
 */

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

CREATE FUNCTION gp_toolkit.__gp_aovisimap_compaction_info(ao_oid oid,
	OUT content int, OUT datafile int, OUT compaction_possible boolean,
	OUT hidden_tupcount bigint, OUT total_tupcount bigint,
	OUT percent_hidden numeric)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_ao_aovisimap_compaction_info'
LANGUAGE C STRICT;

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

/* ------------------------------------------------------------------------- */
/* The bitmap index                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Cloudberry's on-disk bitmap index, an index access method of its own
 * name.  Each index keeps its list of values in a heap and a B-tree of their
 * own, which go in pg_bitmapindex, as Cloudberry's do: a schema whose name,
 * starting pg_, keeps pg_dump from dumping them.
 */
SET allow_system_table_mods = on;
CREATE SCHEMA pg_bitmapindex;
RESET allow_system_table_mods;

CREATE FUNCTION gp_ao.bitmap_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'bmhandler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD bitmap TYPE INDEX HANDLER gp_ao.bitmap_handler;
COMMENT ON ACCESS METHOD bitmap IS 'bitmap index access method';

/*
 * Its operator classes are B-tree's, as Cloudberry's are: the list of values
 * is a real B-tree.  Each B-tree operator family of pg_catalog is made a
 * bitmap one of the same name, its classes with their own type's members,
 * and the family its members across types.  A second class of a family for
 * the same type -- cidr_ops beside inet_ops -- has no members of its own to
 * give, and is not made: the first serves the type.
 */
DO $$
DECLARE
	f record;
	c record;
	m record;
	items text[];
BEGIN
	FOR f IN SELECT opf.oid, opf.opfname
			   FROM pg_catalog.pg_opfamily opf
			   JOIN pg_catalog.pg_am am ON am.oid = opf.opfmethod
			  WHERE am.amname = 'btree'
				AND opf.opfnamespace = 'pg_catalog'::pg_catalog.regnamespace
			  ORDER BY opf.opfname
	LOOP
		EXECUTE format('CREATE OPERATOR FAMILY pg_catalog.%I USING bitmap', f.opfname);

		FOR c IN SELECT DISTINCT ON (opc.opcintype) opc.opcname, opc.opcintype, opc.opcdefault
				   FROM pg_catalog.pg_opclass opc
				  WHERE opc.opcfamily = f.oid
				  ORDER BY opc.opcintype, opc.opcdefault DESC, opc.opcname
		LOOP
			items := ARRAY(
				SELECT format('OPERATOR %s %s', amopstrategy, amopopr::pg_catalog.regoperator)
				  FROM pg_catalog.pg_amop
				 WHERE amopfamily = f.oid AND amoplefttype = c.opcintype
				   AND amoprighttype = c.opcintype AND amoppurpose = 's'
				 ORDER BY amopstrategy)
				|| ARRAY(
				SELECT format('FUNCTION %s (%s, %s) %s', amprocnum,
							  amproclefttype::pg_catalog.regtype,
							  amprocrighttype::pg_catalog.regtype,
							  amproc::pg_catalog.regprocedure)
				  FROM pg_catalog.pg_amproc
				 WHERE amprocfamily = f.oid AND amproclefttype = c.opcintype
				   AND amprocrighttype = c.opcintype
				 ORDER BY amprocnum);
			IF pg_catalog.array_length(items, 1) IS NULL THEN
				CONTINUE;
			END IF;
			EXECUTE format('CREATE OPERATOR CLASS pg_catalog.%I %s FOR TYPE %s USING bitmap FAMILY pg_catalog.%I AS %s',
						   c.opcname, CASE WHEN c.opcdefault THEN 'DEFAULT' ELSE '' END,
						   c.opcintype::pg_catalog.regtype, f.opfname,
						   pg_catalog.array_to_string(items, ', '));
		END LOOP;

		/* what the classes did not take: the members across types */
		items := ARRAY(
			SELECT format('OPERATOR %s %s', amopstrategy, amopopr::pg_catalog.regoperator)
			  FROM pg_catalog.pg_amop a
			 WHERE amopfamily = f.oid AND amoppurpose = 's'
			   AND NOT (amoplefttype = amoprighttype AND EXISTS
						(SELECT 1 FROM pg_catalog.pg_opclass
						  WHERE opcfamily = f.oid AND opcintype = a.amoplefttype))
			 ORDER BY amoplefttype, amoprighttype, amopstrategy)
			|| ARRAY(
			SELECT format('FUNCTION %s (%s, %s) %s', amprocnum,
						  amproclefttype::pg_catalog.regtype,
						  amprocrighttype::pg_catalog.regtype,
						  amproc::pg_catalog.regprocedure)
			  FROM pg_catalog.pg_amproc p
			 WHERE amprocfamily = f.oid
			   AND NOT (amproclefttype = amprocrighttype AND EXISTS
						(SELECT 1 FROM pg_catalog.pg_opclass
						  WHERE opcfamily = f.oid AND opcintype = p.amproclefttype))
			 ORDER BY amproclefttype, amprocrighttype, amprocnum);
		IF pg_catalog.array_length(items, 1) IS NOT NULL THEN
			EXECUTE format('ALTER OPERATOR FAMILY pg_catalog.%I USING bitmap ADD %s',
						   f.opfname, pg_catalog.array_to_string(items, ', '));
		END IF;
	END LOOP;
END
$$;
