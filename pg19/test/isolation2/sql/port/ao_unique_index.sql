-- Tests to ensure that unique indexes work as expected w/ ao_row tables.
--
-- The port's own: Cloudberry's ao_unique_index with its last two cases of a
-- conflict made gp_ao's.  Cases 1 to 6 and the smoke tests are Cloudberry's as they
-- are.  Cases 7 and 8 hold transaction 1 between two rows of its INSERT,
-- each row so far in the unique index; and case 8 counts the rows of two of
-- Cloudberry's block directory minipages.
--
-- On the planner's route the coordinator sends an INSERT's rows to each
-- segment by COPY, a segment at a time, and a segment's COPY appends a
-- batch of rows before it gives the index any of them: no transaction is
-- ever between two rows with the first's index entry written, and no two
-- segments are in the INSERT at once.  A conflict is found all the same,
-- once the batch's entries are written; but who waits for whom is not what
-- these two cases say.  So transaction 1 runs its INSERT under ORCA in
-- either pass, whose plan gives every segment the rows at once and its
-- segments' ModifyTable each row its index entries, as Cloudberry's does.
--
-- And gp_ao's block directory has a row a block, each with a placeholder of
-- its own while the block is built, where Cloudberry's has a minipage of 161
-- blocks and one placeholder an INSERT: case 8 is two blocks written and a
-- third in memory, 1366 rows a block of this table, and the fault a full
-- minipage's has here is a block's directory row written
-- (insert_new_entry_curr_minipage_full, ao_dml.c).

-- We use a replicated table to test each table for ease in testing edge cases
-- where conflicts arise at block directory boundaries. We can treat the table
-- as if it were being populated in utility mode on a single segment, allowing
-- us to predict block directory entries without having to worry about the
-- table's distribution.

-- Case 1: Conflict with committed transaction----------------------------------
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
-- should conflict
INSERT INTO unique_index_ao_row VALUES (1);
INSERT INTO unique_index_ao_row VALUES (329729);
-- should not conflict
INSERT INTO unique_index_ao_row VALUES (329730);
DROP TABLE unique_index_ao_row;

-- Case 2: Conflict within the same transaction---------------------------------
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
BEGIN;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
-- should conflict
INSERT INTO unique_index_ao_row VALUES (1);
END;
DROP TABLE unique_index_ao_row;

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
BEGIN;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
-- should conflict
INSERT INTO unique_index_ao_row VALUES (329729);
END;
DROP TABLE unique_index_ao_row;

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
BEGIN;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
-- should not conflict
INSERT INTO unique_index_ao_row VALUES (329730);
END;
DROP TABLE unique_index_ao_row;

-- Case 3: Conflict with aborted transaction is not a conflict------------------
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
BEGIN;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
ABORT;
-- should not conflict
INSERT INTO unique_index_ao_row VALUES (1);
INSERT INTO unique_index_ao_row VALUES (329729);
INSERT INTO unique_index_ao_row VALUES (329730);
DROP TABLE unique_index_ao_row;

-- Case 4: Conflict with to-be-committed transaction----------------------------
--
-- 1. Uncommitted tx 1 has inserted non-conflicting key = 0.
-- 2. Uncommitted tx 2 has inserted (161 * 2048 + 1 = 329729 rows), which spans
--    2 block directory rows (1st row: [1,329728] ; 2nd row: [329729,329729])
-- 3. Tx 3 tries to insert conflicting key = 2, which maps to the second rownum
--    covered by the 1st block directory row of seg 1, and blocks on tx 2.
-- 4. Tx 4 tries to insert conflicting key = 329728, which maps to the last
--    rownum covered by the 1st block directory row of seg 1, and blocks on tx 2.
-- 5. Tx 5 tries to insert conflicting key = 329729, which maps to the first
--    rownum covered by the 2nd block directory row of seg 1, and blocks on tx 2.
-- 6. Tx 6 tries to insert non-conflicting key = 329730 and is immediately
--    successful.
-- 7. Tx 2 commits
-- 8. Txs 3,4,5 report unique constraint violation
-- 9. Tx 1 commits
--
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
1: BEGIN;
1: INSERT INTO unique_index_ao_row VALUES (0);
2: BEGIN;
2: INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
3&: INSERT INTO unique_index_ao_row VALUES (1);
4&: INSERT INTO unique_index_ao_row VALUES (329728);
5&: INSERT INTO unique_index_ao_row VALUES (329729);
-- should succeed immediately
6: INSERT INTO unique_index_ao_row VALUES (329730);
2: COMMIT;
3<:
4<:
5<:
1: COMMIT;
DROP TABLE unique_index_ao_row;

-- Case 5: Conflict with to-be-aborted transaction------------------------------
--
-- 1. Uncommitted tx 1 has inserted non-conflicting key = 0.
-- 2. Uncommitted tx 2 has inserted (161 * 2048 + 1 = 329729 rows), which spans
--    2 block directory rows (1st row: [1,329728] ; 2nd row: [329729,329729])
-- 3. Tx 3 tries to insert conflicting key = 2, which maps to the second rownum
--    covered by the 1st block directory row of seg 1, and blocks on tx 2.
-- 4. Tx 4 tries to insert conflicting key = 329728, which maps to the last
--    rownum covered by the 1st block directory row of seg 1, and blocks on tx 2.
-- 5. Tx 5 tries to insert conflicting key = 329729, which maps to the first
--    rownum covered by the 2nd block directory row of seg 1, and blocks on tx 2.
-- 6. Tx 6 tries to insert non-conflicting key = 329730 and is immediately
--    successful.
-- 8. Tx 2 aborts
-- 9. Txs 3,4,5 report unique constraint violation
-- 10. Tx 1 commits
--
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
1: BEGIN;
1: INSERT INTO unique_index_ao_row VALUES (0);
2: BEGIN;
2: INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 329729);
3&: INSERT INTO unique_index_ao_row VALUES (1);
4&: INSERT INTO unique_index_ao_row VALUES (329728);
5&: INSERT INTO unique_index_ao_row VALUES (329729);
-- should succeed immediately
6: INSERT INTO unique_index_ao_row VALUES (329730);
2: ABORT;
3<:
4<:
5<:
1: COMMIT;
DROP TABLE unique_index_ao_row;

-- Case 6: Conflict with aborted rows following some committed rows ------------
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
-- 1. Tx 1 commits rows 1-100.
-- 2. Tx 2 inserts rows 101-200 and then aborts.
-- 3. Tx 3 tries to insert row in range [101,200] and is immediately successful.
-- 4. Tx 4 tries to insert conflicting row in range [1,100] and raises unique
--    constraint violation.
-- 5. Tx 5 tries to insert row in range [201, ) and is immediately successful.
1: INSERT INTO unique_index_ao_row SELECT generate_series(1, 100);
2: BEGIN;
2: INSERT INTO unique_index_ao_row SELECT generate_series(101, 200);
2: ABORT;
3: INSERT INTO unique_index_ao_row VALUES(102);
4: INSERT INTO unique_index_ao_row VALUES(2);
5: INSERT INTO unique_index_ao_row VALUES(202);
DROP TABLE unique_index_ao_row;

--------------------------------------------------------------------------------
----------------- More concurrent tests with fault injection ------------------
--------------------------------------------------------------------------------

-- Case 7: Conflict with to-be-committed transaction while only 1 placeholder
-- row exists in the block directory--------------------------------------------
--
-- This case highlights the importance of the placeholder row, inserted at the
-- beginning of an INSERT command.
--
-- 1. Uncommitted Tx 1 has inserted 3 out of its 10 rows and is suspended.
-- 2. Tx 2 inserts a conflicting row and blocks on Tx 1.
-- 3. Tx 3 inserts a non-conflicting row within the range [4,10] and is
--    immediately successful. (Index entries have been written only for [1,3] so
--    far, so conflicts shouldn't arise)
-- 4. Tx 4 inserts a non-conflicting row in range [11, ..) and should be
--    immediately successful.
-- 5. Now Tx 1 resumes and tries to insert a row in range [4,10] and reports a
--    unique constraint violation with Tx 3.
-- 6. Tx 2 succeeds as Tx 1 aborted.

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;
SELECT gp_inject_fault('appendonly_insert', 'suspend', '', '', 'unique_index_ao_row', 4, 4, 0, dbid)
    FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
1: SET gp.optimizer = on;
1&: INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 10);
-- Wait until 3 rows have been successfully inserted into the index and Tx 1
-- is just beginning to insert the 4th row.
SELECT gp_wait_until_triggered_fault('appendonly_insert', 4, dbid)
    FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
2&: INSERT INTO unique_index_ao_row VALUES(2);
4: INSERT INTO unique_index_ao_row VALUES(11);
3: INSERT INTO unique_index_ao_row VALUES(4);
SELECT gp_inject_fault('appendonly_insert', 'reset', dbid)
    FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
1<:
2<:
1: RESET gp.optimizer;
DROP TABLE unique_index_ao_row;

-- Case 8: Conflict with to-be-committed transaction - generalization of case 7
-- where there are multiple blocks (and block directory rows) in play from
-- the same insert.
--
-- Each block being built has a placeholder row of its own, which its
-- directory row replaces as the block is written: a conflict with a row of a
-- block written waits on that directory row, and one with a row of the block
-- in memory on the placeholder.
--
-- 1. Uncommitted Tx 1 has inserted (1366 * 2 + 3) = 2735 rows and is suspended,
--    enough rows to fill 2 entire blocks (covers range [1,1366] and
--    [1367,2732]) before suspension.
-- 2. Txs 2,3,4 inserts conflicting rows that map to the 1st block.
-- 3. Txs 5,6,7 inserts conflicting rows that map to the 2nd block.
-- 4. Tx 8 inserts a conflicting row that maps to the 3rd block, which is
--    currently only in-memory and it conflicts on the placeholder row and
--    blocks
-- 5. Tx 9 inserts a non-conflicting row for which there is no index entry and
--    and is immediately successful (2738).
-- 6. Now Tx 1 resumes and tries to insert 2738 and reports a unique
--    constraint violation with Tx 9.
-- 7. All blocked Txs succeed.

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;

SELECT gp_inject_fault('insert_new_entry_curr_minipage_full', 'suspend', '', '', '', 2, 2, 0, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
1: set gp.appendonly_insert_files = 0;
1: SET gp.optimizer = on;
1&: INSERT INTO unique_index_ao_row SELECT generate_series(1, 2738);

-- Wait until we have inserted 1366 * 2 = 2732 rows, the second block written,
-- and then 3 more, so that we are about to insert the 2736th row.
SELECT gp_wait_until_triggered_fault('insert_new_entry_curr_minipage_full', 2, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
SELECT gp_inject_fault('appendonly_insert', 'suspend', '', '', 'unique_index_ao_row', 4, 4, 0, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
SELECT gp_inject_fault('insert_new_entry_curr_minipage_full', 'reset', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;
SELECT gp_wait_until_triggered_fault('appendonly_insert', 4, dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;

-- maps to 1st block
2&: INSERT INTO unique_index_ao_row VALUES(1);
3&: INSERT INTO unique_index_ao_row VALUES(1000);
4&: INSERT INTO unique_index_ao_row VALUES(1366);
-- maps to 2nd block
5&: INSERT INTO unique_index_ao_row VALUES(1367);
6&: INSERT INTO unique_index_ao_row VALUES(2000);
7&: INSERT INTO unique_index_ao_row VALUES(2732);
-- maps to 3rd block
8&: INSERT INTO unique_index_ao_row VALUES(2735);
-- no index entry exists for it, so should not conflict.
9: INSERT INTO unique_index_ao_row VALUES(2738);

SELECT gp_inject_fault('appendonly_insert', 'reset', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content <> -1;

1<:
2<:
3<:
4<:
5<:
6<:
7<:
8<:
1: RESET gp.optimizer;

DROP TABLE unique_index_ao_row;

--------------------------------------------------------------------------------
--------------------------- Smoke tests for COPY -------------------------------
--------------------------------------------------------------------------------

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;

1: BEGIN;
1: COPY unique_index_ao_row FROM PROGRAM 'seq 1 10';
-- concurrent tx inserting conflicting row should block.
2&: COPY unique_index_ao_row FROM PROGRAM 'seq 1 1';
-- concurrent tx inserting non-conflicting rows should be successful.
3: COPY unique_index_ao_row FROM PROGRAM 'seq 11 20';
-- inserting a conflicting row in the same transaction should ERROR out.
1: COPY unique_index_ao_row FROM PROGRAM 'seq 1 1';
-- now that tx 1 was aborted, tx 2 is successful.
2<:
1: END;

DROP TABLE unique_index_ao_row;

--------------------------------------------------------------------------------
-------------------- Smoke tests for subtransactions ---------------------------
--------------------------------------------------------------------------------
CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;

1: BEGIN;
1: SAVEPOINT a;
1: INSERT INTO unique_index_ao_row VALUES(1);

-- concurrent tx inserting conflicting row should block.
2: BEGIN;
2&: INSERT INTO unique_index_ao_row VALUES(1);
-- concurrent tx inserting non-conflicting row should be successful.
3: INSERT INTO unique_index_ao_row VALUES(2);

-- conflict should be detected within the same subtx.
1: INSERT INTO unique_index_ao_row VALUES(1);
-- the concurrent tx should now succeed.
2<:
2: ABORT;

-- after rolling back to the savepoint, we should be able to re-insert the key
1: ROLLBACK TO SAVEPOINT a;
1: INSERT INTO unique_index_ao_row VALUES(1);
1: COMMIT;

SELECT * FROM unique_index_ao_row;

DROP TABLE unique_index_ao_row;

--------------------------------------------------------------------------------
-------------------- Smoke tests for repeatable read ---------------------------
--------------------------------------------------------------------------------

-- Test that shows that unique index checks transcend transaction isolation
-- boundaries.

CREATE TABLE unique_index_ao_row (a INT unique) USING ao_row
    DISTRIBUTED REPLICATED;

-- Begin two txs with tx level snapshot taken early.
1: BEGIN ISOLATION LEVEL REPEATABLE READ;
1: SELECT * FROM unique_index_ao_row;
2: BEGIN ISOLATION LEVEL REPEATABLE READ;
2: SELECT * FROM unique_index_ao_row;

-- Now begin a concurrent transaction which inserts a key.
3: BEGIN;
3: INSERT INTO unique_index_ao_row VALUES(1);

-- And another transaction inserts a key and commits.
INSERT INTO unique_index_ao_row VALUES(2);

-- Tx should block on insert of conflicting key, even though it can't "see" the
-- conflicting key due to its isolation level.
1: SELECT * FROM unique_index_ao_row;
1&: INSERT INTO unique_index_ao_row VALUES(1);

3: ABORT;
1<:
1: ABORT;

-- Tx should raise a conflict, even though it can't "see" the conflicting key
-- due to its isolation level.
2: SELECT * FROM unique_index_ao_row;
2: INSERT INTO unique_index_ao_row VALUES(2);
2: ABORT;

DROP TABLE unique_index_ao_row;


--------------------------------------------------------------------------------
----------------------- Smoke tests for ADD CONSTRAINT  ------------------------
--------------------------------------------------------------------------------
CREATE TABLE unique_index_ao_row (a INT) USING ao_row
    DISTRIBUTED REPLICATED;
INSERT INTO unique_index_ao_row SELECT * FROM generate_series(1, 5);

ALTER table unique_index_ao_row ADD CONSTRAINT a_unique UNIQUE(a);
-- should conflict
INSERT INTO unique_index_ao_row VALUES (1);
ALTER table unique_index_ao_row DROP CONSTRAINT a_unique;

INSERT INTO unique_index_ao_row VALUES (1);
-- should failed
ALTER table unique_index_ao_row ADD CONSTRAINT a_unique UNIQUE(a);

DROP TABLE unique_index_ao_row;


--------------------------------------------------------------------------------
----------------------- Smoke tests for Multiple Key ---------------------------
--------------------------------------------------------------------------------
CREATE TABLE unique_index_ao_row (a INT, b INT) USING ao_row
    DISTRIBUTED REPLICATED;
INSERT INTO unique_index_ao_row SELECT i,i FROM generate_series(1, 5) i;

CREATE UNIQUE INDEX a_b_unique ON unique_index_ao_row(a,b);
-- should not conflict
INSERT INTO unique_index_ao_row VALUES (1,2);
-- should conflict
INSERT INTO unique_index_ao_row VALUES (1,1);
DROP TABLE unique_index_ao_row;
