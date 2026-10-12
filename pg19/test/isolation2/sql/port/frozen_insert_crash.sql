-- The port's own, beside frozen_insert_crash: a segment that fails after a
-- bitmap index's list of values took a new value, the transaction that
-- brought it not committed, has the value under a scan of the list and
-- under one of its index alike once it has recovered, and the index goes
-- on from there.
--
-- Cloudberry inserts the value's row into the list's table as any row,
-- inserts its index entry, and then freezes the row with a record of its
-- own, and its test fails the segment on either side of that record: before
-- it the row is the failed transaction's and neither scan has it, after it
-- both have.  gp_ao inserts the row frozen (HEAP_INSERT_FROZEN), so there
-- is no "before": this is that test's second case.  Its tests of
-- gp_fastsequence's row and of the segment file's are of two tables gp_ao
-- has not (gp_ao.segfile has a file's row from its writer's commit).

-- The list of values of tab_fi's index, as a scan of its table reads it or
-- as one of its index does, and which of the two scans it was.
CREATE FUNCTION frozen_lov(by_index bool, OUT scan text, OUT vals int[]) AS $$
DECLARE
	lov regclass;	/* in func */
	q text;	/* in func */
	line text;	/* in func */
	v int;	/* in func */
BEGIN	/* in func */
	SELECT m.auxrelid INTO lov FROM bm_metap('tab_fi_idx') m;	/* in func */
	q := format('SELECT b FROM %s ORDER BY b', lov);	/* in func */
	PERFORM set_config('gp.optimizer', 'off', true);	/* in func */
	PERFORM set_config('enable_seqscan', (NOT by_index)::text, true);	/* in func */
	PERFORM set_config('enable_indexscan', by_index::text, true);	/* in func */
	PERFORM set_config('enable_indexonlyscan', 'off', true);	/* in func */
	PERFORM set_config('enable_bitmapscan', 'off', true);	/* in func */
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP	/* in func */
		IF line ~ 'Scan' THEN	/* in func */
			scan := substring(line from '(\w+ Scan)');	/* in func */
		END IF;	/* in func */
	END LOOP;	/* in func */
	FOR v IN EXECUTE q LOOP	/* in func */
		vals := vals || v;	/* in func */
	END LOOP;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

-- No probe of FTS's fails the segment over while it recovers.
SELECT gp_inject_fault_infinite('fts_probe', 'skip', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
SELECT gp_request_fts_probe_scan();

1: CREATE TABLE tab_fi (a int, b int) WITH (appendoptimized=true) DISTRIBUTED REPLICATED;
1: CREATE INDEX tab_fi_idx ON tab_fi USING bitmap (b);
1: INSERT INTO tab_fi VALUES (1, 1);
0U: SELECT pg_switch_wal() IS NOT NULL;

-- An INSERT of a new value is held on segment 0 with the value's row in the
-- list and in the list's index, and the WAL of both is flushed.
1: SELECT gp_inject_fault('insert_bmlov_after_freeze', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
2&: INSERT INTO tab_fi VALUES (2, 2);
1: SELECT gp_wait_until_triggered_fault('insert_bmlov_after_freeze', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
0U: SELECT pg_switch_wal() IS NOT NULL;
0Uq:

-- The segment fails as its part of the INSERT is about to be prepared.
1: SELECT gp_inject_fault('start_prepare', 'panic', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1: SELECT gp_inject_fault('insert_bmlov_after_freeze', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
2<:
-- (7002 is content 0's primary in Cloudberry's demo cluster, which the
-- harness makes this cluster's)
!\retcode bash -c 'for i in $(seq 1 120); do pg_isready -q -p 7002 && exit 0; sleep 0.5; done; exit 1';

-- The list has the value, as its index has; the table has not the row.
0U: SELECT * FROM frozen_lov(true);
0U: SELECT * FROM frozen_lov(false);
0U: SELECT * FROM tab_fi ORDER BY a;
0Uq:

-- A row of that value finds the value in the list, and the index finds the
-- row and no other.
3: INSERT INTO tab_fi VALUES (3, 2);
0U: SELECT * FROM frozen_lov(true);
0U: SET gp.optimizer = off;
0U: SET enable_seqscan = off;
0U: EXPLAIN (COSTS OFF) SELECT a FROM tab_fi WHERE b = 2;
0U: SELECT a FROM tab_fi WHERE b = 2;
0Uq:
3: SELECT * FROM tab_fi ORDER BY a;

3: DROP TABLE tab_fi;
3: DROP FUNCTION frozen_lov(bool);
SELECT gp_inject_fault('fts_probe', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
