-- The port's own, beside segwalrep/commit_blocking_on_standby, whose commits
-- are a CREATE TABLE's: the commit of an INSERT, an UPDATE or a DELETE of a
-- distributed table alone waits for the standby coordinator too.  Such a
-- transaction logs nothing on the coordinator but its commit record, and
-- PostgreSQL waits for synchronous replication with a commit record only
-- where the transaction had logged something before it; so the coordinator
-- waits between the two phases of the commit (gp_dispatch.c,
-- dtx_wait_for_standby()), as Cloudberry's waits for every commit that
-- prepared.  Without that the statement returned while the standby had not
-- flushed the record that committed it, and a standby promoted then could
-- lack a commit the client had been told of.

SELECT application_name, state FROM pg_stat_replication;

-- A table for each session: an UPDATE's lock on a table waits for an
-- INSERT's, which is held to the end of its commit.
CREATE TABLE commit_blocking_dml_i (a int, b int) DISTRIBUTED BY (a);
CREATE TABLE commit_blocking_dml_u (a int, b int) DISTRIBUTED BY (a);
INSERT INTO commit_blocking_dml_u SELECT i, i % 2 FROM generate_series(1, 40) i;

-- Until n backends of this node wait for synchronous replication.
CREATE FUNCTION commit_blocking_dml_wait(n int) RETURNS bool AS $$
BEGIN	/* in func */
	FOR i IN 1..600 LOOP	/* in func */
		IF (SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'SyncRep') >= n THEN	/* in func */
			RETURN true;	/* in func */
		END IF;	/* in func */
		PERFORM pg_sleep(0.1);	/* in func */
		PERFORM pg_stat_clear_snapshot();	/* in func */
	END LOOP;	/* in func */
	RETURN false;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

-- A session's first statements log more than a commit record on the
-- coordinator, and such a commit PostgreSQL waits for of itself.  The two
-- sessions have theirs behind them: each has run what it runs below.
1: INSERT INTO commit_blocking_dml_i SELECT i, 1 FROM generate_series(1, 20) i;
1: INSERT INTO commit_blocking_dml_i SELECT i, 1 FROM generate_series(1, 20) i;
2: BEGIN;
2: UPDATE commit_blocking_dml_u SET b = 0 WHERE b = 0;
2: DELETE FROM commit_blocking_dml_u WHERE b = 1 AND a > 40;
2: COMMIT;
2: BEGIN;
2: UPDATE commit_blocking_dml_u SET b = 0 WHERE b = 0;
2: DELETE FROM commit_blocking_dml_u WHERE b = 1 AND a > 40;
2: COMMIT;

-- The standby stops flushing what it receives, and a checkpoint's record
-- has it reach the fault.
SELECT gp_inject_fault_infinite('walrecv_skip_flush', 'skip', dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'm';
CHECKPOINT;
SELECT gp_wait_until_triggered_fault('walrecv_skip_flush', 1, dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'm';

-- An INSERT into every segment commits in two phases, and waits.
1&: INSERT INTO commit_blocking_dml_i SELECT i, 2 FROM generate_series(1, 20) i;
SELECT commit_blocking_dml_wait(1);

-- And the COMMIT of an UPDATE and a DELETE.
2: BEGIN;
2: UPDATE commit_blocking_dml_u SET b = 3 WHERE b = 0;
2: DELETE FROM commit_blocking_dml_u WHERE b = 1 AND a <= 20;
2&: COMMIT;
SELECT commit_blocking_dml_wait(2);

SELECT wait_event, query FROM pg_stat_activity WHERE wait_event = 'SyncRep' ORDER BY query;

-- Neither has committed for another session: no segment has been told to.
SELECT b, count(*) FROM commit_blocking_dml_i GROUP BY b ORDER BY b;
SELECT b, count(*) FROM commit_blocking_dml_u GROUP BY b ORDER BY b;

-- The standby flushes again, with the next record it receives.
SELECT gp_inject_fault('walrecv_skip_flush', 'reset', dbid) FROM gp_segment_configuration WHERE content = -1 AND role = 'm';
CHECKPOINT;

1<:
2<:

SELECT b, count(*) FROM commit_blocking_dml_i GROUP BY b ORDER BY b;
SELECT b, count(*) FROM commit_blocking_dml_u GROUP BY b ORDER BY b;

DROP TABLE commit_blocking_dml_i;
DROP TABLE commit_blocking_dml_u;
DROP FUNCTION commit_blocking_dml_wait(int);
