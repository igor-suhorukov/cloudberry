-- The port's own: Cloudberry's ao_blkdir, its white-box test of what an
-- append-optimized table's block directory holds, for gp_ao's directory.
-- Cloudberry's is a relation of each table's own whose row is a minipage of
-- up to 161 entries, a block each, rewritten as entries are added; gp_ao's
-- is one table for every table's (gp_ao.blkdir), a row a block, written once
-- -- and for a table by column one row for all its columns' blocks of the
-- same rows, which gp_toolkit.__gp_aoblkdir() gives a column at a time.  So
-- every dump of Cloudberry's test differs: the entry numbers, the row
-- numbers, which Cloudberry hands out a hundred at a time, the offsets, and
-- the directory row's own TID, which here depends on what every other table
-- wrote before.  This is that test with the TID left out of what it prints,
-- and with the two inserts that overflow a minipage by two rows made inserts
-- that overflow a block by two.  What Cloudberry's test asserts beyond the
-- layout holds as it is: a row of a transaction in progress is another's to
-- see only once it commits; a table with a unique index has a placeholder
-- for the block being built, which gp.select_invisible shows and no MVCC
-- snapshot does; and a segment file's rows are the sum of its directory
-- rows' counts, after commits, aborts, deletes and VACUUM.
--
-- All tuples are directed to seg0 and each INSERT has an increasing row count
-- to make their identification easy.

--------------------------------------------------------------------------------
-- AO tables
--------------------------------------------------------------------------------

CREATE TABLE ao_blkdir_test(i int, j int) USING ao_row DISTRIBUTED BY (j);
CREATE INDEX ao_blkdir_test_idx ON ao_blkdir_test(i);

1: INSERT INTO ao_blkdir_test SELECT i, 2 FROM generate_series(1, 10) i;
-- There should be 1 block directory row covering 10 rows
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: INSERT INTO ao_blkdir_test SELECT i, 2 FROM generate_series(11, 30) i;
-- There should be 2 block directory rows, one for each INSERT's block. The row
-- from the first INSERT should remain unchanged.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: BEGIN;
1: INSERT INTO ao_blkdir_test SELECT i, 2 FROM generate_series(31, 60) i;
2: BEGIN;
2: INSERT INTO ao_blkdir_test SELECT i, 2 FROM generate_series(71, 110) i;
1: COMMIT;
2: COMMIT;
-- The second INSERT of 40 rows above would have landed in segfile 2 (unlike
-- segfile 1, like the first INSERT of 30 rows above). This should be reflected
-- in the block directory rows for these rows.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

TRUNCATE ao_blkdir_test;
set gp.appendonly_insert_files = 0;
-- Insert enough rows to overflow the first block by 2: a row of two integers
-- is 24 bytes of a block of 32768, which is full with the 1366th.
INSERT INTO ao_blkdir_test SELECT i, 2 FROM generate_series(1, 1368) i;
reset gp.appendonly_insert_files;
-- There should be 2 block directory rows, one covering 1366 rows and the
-- other the 2 overflow rows.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- Unique index white box tests
DROP TABLE ao_blkdir_test;
CREATE TABLE ao_blkdir_test(i int UNIQUE, j int) USING ao_row DISTRIBUTED BY (i);

SELECT gp_inject_fault('appendonly_insert', 'suspend', '', '', 'ao_blkdir_test', 1, 1, 0, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1: BEGIN;
1&: INSERT INTO ao_blkdir_test VALUES (2, 2);

-- There should be a placeholder row inserted to cover the rows of the block
-- the INSERT builds, before we insert its 1st row, that is only visible to
-- SNAPSHOT_DIRTY -- and to gp.select_invisible, which reads every version of
-- every row: it covers every row number its segment file may have from the
-- block's first.
SELECT gp_wait_until_triggered_fault('appendonly_insert', 1, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;
SET gp.select_invisible TO ON;
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;
RESET gp.select_invisible;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) while the INSERT is in progress.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

SELECT gp_inject_fault('appendonly_insert', 'reset', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1<:

-- The placeholder row is invisible to the INSERTing transaction. Since the
-- INSERT finished, there should be 1 visible blkdir row representing the INSERT.
1: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) even after the INSERT finishes. The blkdir row representing
-- the INSERT should not be visible as the INSERTing transaction hasn't
-- committed yet.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: COMMIT;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) even after the INSERTing transaction commits. Since the
-- INSERTing transaction has committed, the blkdir row representing the INSERT
-- should be visible now.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

DROP TABLE ao_blkdir_test;

-- Test `tupcount` in pg_aoseg == sum of number of `row_count` across all
-- aoblkdir entries for each segno. Test with commits, aborts and deletes.

-- Case1: without VACUUM ANALYZE
CREATE TABLE ao_blkdir_test_rowcount(i int, j int) USING ao_row DISTRIBUTED BY (j);
1: BEGIN;
2: BEGIN;
3: BEGIN;
4: BEGIN;
1: INSERT INTO ao_blkdir_test_rowcount SELECT i, 2 FROM generate_series(1, 10) i;
2: INSERT INTO ao_blkdir_test_rowcount SELECT i, 3 FROM generate_series(1, 20) i;
3: INSERT INTO ao_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 30) i;
3: ABORT;
3: BEGIN;
3: INSERT INTO ao_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 40) i;
4: INSERT INTO ao_blkdir_test_rowcount SELECT i, 7 FROM generate_series(1, 50) i;
1: COMMIT;
2: COMMIT;
3: COMMIT;
4: COMMIT;
DELETE FROM ao_blkdir_test_rowcount WHERE j = 7;

CREATE INDEX ao_blkdir_test_rowcount_idx ON ao_blkdir_test_rowcount(i);

SELECT segno, sum(row_count) AS totalrows FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno;
SELECT segno, sum(tupcount) AS totalrows FROM
  gp_toolkit.__gp_aoseg('ao_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno;

-- Case2: with VACUUM ANALYZE
DROP TABLE ao_blkdir_test_rowcount;
CREATE TABLE ao_blkdir_test_rowcount(i int, j int) USING ao_row DISTRIBUTED BY (j);
CREATE INDEX ao_blkdir_test_rowcount_idx ON ao_blkdir_test_rowcount(i);
1: BEGIN;
2: BEGIN;
3: BEGIN;
4: BEGIN;
1: INSERT INTO ao_blkdir_test_rowcount SELECT i, 2 FROM generate_series(1, 10) i;
1: INSERT INTO ao_blkdir_test_rowcount SELECT i, 2 FROM ao_blkdir_test_rowcount;
1: INSERT INTO ao_blkdir_test_rowcount SELECT i, 2 FROM ao_blkdir_test_rowcount;
2: INSERT INTO ao_blkdir_test_rowcount SELECT i, 3 FROM generate_series(1, 20) i;
2: INSERT INTO ao_blkdir_test_rowcount SELECT i, 3 FROM ao_blkdir_test_rowcount;
2: INSERT INTO ao_blkdir_test_rowcount SELECT i, 3 FROM ao_blkdir_test_rowcount;
3: INSERT INTO ao_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 30) i;
3: INSERT INTO ao_blkdir_test_rowcount SELECT i, 4 FROM ao_blkdir_test_rowcount;
3: INSERT INTO ao_blkdir_test_rowcount SELECT i, 4 FROM ao_blkdir_test_rowcount;
4: INSERT INTO ao_blkdir_test_rowcount SELECT i, 7 FROM generate_series(1, 50) i;
4: INSERT INTO ao_blkdir_test_rowcount SELECT i, 7 FROM ao_blkdir_test_rowcount;
4: INSERT INTO ao_blkdir_test_rowcount SELECT i, 7 FROM ao_blkdir_test_rowcount;
1: COMMIT;
2: COMMIT;
3: ABORT;
4: COMMIT;

DELETE FROM ao_blkdir_test_rowcount WHERE j = 7;
VACUUM ANALYZE ao_blkdir_test_rowcount;

SELECT segno, sum(row_count) AS totalrows FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno;
SELECT segno, sum(tupcount) AS totalrows FROM
  gp_toolkit.__gp_aoseg('ao_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno;

UPDATE ao_blkdir_test_rowcount SET i = i + 1;
VACUUM ANALYZE ao_blkdir_test_rowcount;

SELECT segno, sum(row_count) AS totalrows FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('ao_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno;
SELECT segno, sum(tupcount) AS totalrows FROM
  gp_toolkit.__gp_aoseg('ao_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno;

DROP TABLE ao_blkdir_test_rowcount;

--------------------------------------------------------------------------------
-- AOCO tables
--------------------------------------------------------------------------------

CREATE TABLE aoco_blkdir_test(i int, j int) USING ao_column DISTRIBUTED BY (j);
CREATE INDEX aoco_blkdir_test_idx ON aoco_blkdir_test(i);

1: INSERT INTO aoco_blkdir_test SELECT i, 2 FROM generate_series(1, 10) i;
-- There should be 1 block directory row covering 10 rows, given once for each
-- column.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: INSERT INTO aoco_blkdir_test SELECT i, 2 FROM generate_series(11, 30) i;
-- There should be 2 block directory rows, one for each INSERT's blocks. The
-- row from the first INSERT should remain unchanged.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: BEGIN;
1: INSERT INTO aoco_blkdir_test SELECT i, 2 FROM generate_series(31, 60) i;
2: BEGIN;
2: INSERT INTO aoco_blkdir_test SELECT i, 2 FROM generate_series(71, 110) i;
1: COMMIT;
2: COMMIT;
-- The second INSERT of 40 rows above would have landed in segfile 2 (unlike
-- segfile 1, like the first INSERT of 30 rows above). This should be reflected
-- in the block directory rows for these rows.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

TRUNCATE aoco_blkdir_test;
-- Insert enough rows to overflow the first block by 2: a column of integers
-- fills a block of 32768 bytes with its 8192nd.
set gp.appendonly_insert_files = 0;
INSERT INTO aoco_blkdir_test SELECT i, 2 FROM generate_series(1, 8194) i;
reset gp.appendonly_insert_files;
-- There should be 2 block directory rows, each given for both columns, one
-- covering 8192 rows and the other the 2 overflow rows.
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- Unique index white box tests
DROP TABLE aoco_blkdir_test;
CREATE TABLE aoco_blkdir_test(h int, i int UNIQUE, j int) USING ao_column DISTRIBUTED BY (i);

SELECT gp_inject_fault('appendonly_insert', 'suspend', '', '', 'aoco_blkdir_test', 1, 1, 0, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1: BEGIN;
1&: INSERT INTO aoco_blkdir_test VALUES (2, 2, 2);

-- There should be a placeholder row inserted to cover the rows of the block
-- the INSERT builds, one for all its columns, before we insert its 1st row,
-- that is only visible to SNAPSHOT_DIRTY and to gp.select_invisible.
SELECT gp_wait_until_triggered_fault('appendonly_insert', 1, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;
SET gp.select_invisible TO ON;
SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;
RESET gp.select_invisible;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) while the INSERT is in progress.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- Before the INSERT commits, if we try to drop column 'h', the session will
-- block (locking). So it is perfectly safe to use 1 placeholder row (and not
-- have 1 placeholder/column)
3&: ALTER TABLE aoco_blkdir_test DROP COLUMN h;

SELECT gp_inject_fault('appendonly_insert', 'reset', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1<:

-- The placeholder row is invisible to the INSERTing transaction. Since the
-- INSERT finished, there should be 1 visible blkdir row representing the
-- INSERT, given for each column.
1: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) even after the INSERT finishes. The blkdir rows representing
-- the INSERT should not be visible as the INSERTing transaction hasn't
-- committed yet.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

1: COMMIT;

-- The placeholder row is invisible to other transactions (that don't perform a
-- uniqueness check) even after the INSERTing transaction commits. Since the
-- INSERTing transaction has committed, the blkdir rows representing the INSERT
-- should be visible now.
2: SELECT segno, columngroup_no, entry_no, first_row_no, file_offset, row_count FROM
  (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test')).* FROM gp_dist_random('gp_id')
      WHERE gp_segment_id = 0) d ORDER BY 1,2,3,4,5;

-- Now even though the DROP COLUMN has finished, we would still be able to
-- properly resolve uniqueness checks (the block's directory row is every
-- column's).
3<:
4: INSERT INTO aoco_blkdir_test VALUES (2, 2);

DROP TABLE aoco_blkdir_test;

-- Test `tupcount` in pg_ao(cs)seg == sum of number of `row_count` across all
-- aoblkdir entries for each <segno, columngroup_no>. Test with commits, aborts
-- and deletes.

-- Case1: without VACUUM ANALYZE
CREATE TABLE aoco_blkdir_test_rowcount(i int, j int) USING ao_column DISTRIBUTED BY (j);
1: BEGIN;
2: BEGIN;
3: BEGIN;
4: BEGIN;
1: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 2 FROM generate_series(1, 10) i;
2: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 3 FROM generate_series(1, 20) i;
3: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 30) i;
3: ABORT;
3: BEGIN;
3: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 40) i;
4: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 7 FROM generate_series(1, 50) i;
1: COMMIT;
2: COMMIT;
3: COMMIT;
4: COMMIT;
DELETE FROM aoco_blkdir_test_rowcount WHERE j = 7;

CREATE INDEX aoco_blkdir_test_rowcount_idx ON aoco_blkdir_test_rowcount(i);

SELECT segno, columngroup_no, sum(row_count) AS totalrows FROM
    (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
     WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno, columngroup_no;
SELECT segno, column_num, sum(tupcount) AS totalrows FROM
    gp_toolkit.__gp_aocsseg('aoco_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno, column_num;

-- Case2: with VACUUM ANALYZE
DROP TABLE aoco_blkdir_test_rowcount;
CREATE TABLE aoco_blkdir_test_rowcount(i int, j int) USING ao_column DISTRIBUTED BY (j);
CREATE INDEX aoco_blkdir_test_rowcount_idx ON aoco_blkdir_test_rowcount(i);
1: BEGIN;
2: BEGIN;
3: BEGIN;
4: BEGIN;
1: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 2 FROM generate_series(1, 10) i;
1: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 2 FROM aoco_blkdir_test_rowcount;
1: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 2 FROM aoco_blkdir_test_rowcount;
2: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 3 FROM generate_series(1, 20) i;
2: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 3 FROM aoco_blkdir_test_rowcount;
2: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 3 FROM aoco_blkdir_test_rowcount;
3: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 4 FROM generate_series(1, 30) i;
3: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 4 FROM aoco_blkdir_test_rowcount;
3: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 4 FROM aoco_blkdir_test_rowcount;
4: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 7 FROM generate_series(1, 50) i;
4: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 7 FROM aoco_blkdir_test_rowcount;
4: INSERT INTO aoco_blkdir_test_rowcount SELECT i, 7 FROM aoco_blkdir_test_rowcount;
1: COMMIT;
2: COMMIT;
3: ABORT;
4: COMMIT;

DELETE FROM aoco_blkdir_test_rowcount WHERE j = 7;
VACUUM ANALYZE aoco_blkdir_test_rowcount;

SELECT segno, columngroup_no, sum(row_count) AS totalrows FROM
    (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
     WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno, columngroup_no;
SELECT segno, column_num, sum(tupcount) AS totalrows FROM
    gp_toolkit.__gp_aocsseg('aoco_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno, column_num;

UPDATE aoco_blkdir_test_rowcount SET i = i + 1;
VACUUM ANALYZE aoco_blkdir_test_rowcount;

SELECT segno, columngroup_no, sum(row_count) AS totalrows FROM
    (SELECT (gp_toolkit.__gp_aoblkdir('aoco_blkdir_test_rowcount')).* FROM gp_dist_random('gp_id')
     WHERE gp_segment_id = 0)s GROUP BY segno, columngroup_no ORDER BY segno, columngroup_no;
SELECT segno, column_num, sum(tupcount) AS totalrows FROM
    gp_toolkit.__gp_aocsseg('aoco_blkdir_test_rowcount') WHERE segment_id = 0 GROUP BY segno, column_num;

DROP TABLE aoco_blkdir_test_rowcount;
