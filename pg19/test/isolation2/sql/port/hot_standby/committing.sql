-- The port's own, beside Cloudberry's hot_standby tests: the distributed
-- transactions in their second phase, which is how a query on the standby
-- coordinator sees a transaction whole or not at all (gp_dtx_standby.c).
-- The standby has a transaction committed as it replays the coordinator's
-- commit record, written between the two phases, before any mirror has the
-- commit of its part; so the coordinator's WAL says which transactions are
-- in their second phase, the standby's queries see those in progress, and
-- a mirror hides the part of one it has already committed.  What
-- Cloudberry's tests ask of that they ask in transaction_isolation and
-- faults.  This asks the rest: what each node has of the set as a commit
-- goes through it; a mirror that has a part committed while the standby's
-- query sees none of it; a transaction block on the standby that began
-- while a commit was in its second phase and reads on after it; the
-- standby's own catalog; a second phase its backend gives up on, which
-- distributed transaction recovery finishes and forgets; and a commit in
-- one phase.

-- start_matchsubs
-- m/gp_dtx_\d+/
-- s/gp_dtx_\d+/gp_dtx_XXX/
-- m/gid = \d+-\d+/
-- s/gid = \d+-\d+/gid = XXX/
-- end_matchsubs

-- Until this node has no transaction in its second phase.
CREATE FUNCTION hs_committing_none() RETURNS bool AS $$
BEGIN	/* in func */
	FOR i IN 1..1200 LOOP	/* in func */
		IF (SELECT count(*) FROM gp_internal.dtx_committing()) = 0 THEN	/* in func */
			RETURN true;	/* in func */
		END IF;	/* in func */
		PERFORM pg_sleep(0.1);	/* in func */
	END LOOP;	/* in func */
	RETURN false;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

CREATE TABLE hs_committing (a int, b int) DISTRIBUTED BY (a);
INSERT INTO hs_committing SELECT i, 0 FROM generate_series(1, 30) i;

-- Until this node has rows of the statement that wrote b, in a session of
-- its own.
CREATE FUNCTION hs_committing_has(want int) RETURNS bool AS $$
BEGIN	/* in func */
	FOR i IN 1..1200 LOOP	/* in func */
		IF EXISTS (SELECT 1 FROM hs_committing WHERE b = want) THEN	/* in func */
			RETURN true;	/* in func */
		END IF;	/* in func */
		PERFORM pg_sleep(0.1);	/* in func */
	END LOOP;	/* in func */
	RETURN false;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

-- No transaction is in its second phase, on the coordinator or on its
-- standby, and each has the set whole.
SELECT count(*) FROM gp_internal.dtx_committing();
SELECT gp_internal.dtx_committing_whole();
-1S: SELECT count(*) FROM gp_internal.dtx_committing();
-1S: SELECT gp_internal.dtx_committing_whole();

----------------------------------------------------------------
-- A commit held in its second phase on segment 1
----------------------------------------------------------------
SELECT gp_inject_fault('qe_start_commit_prepared', 'suspend', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
1&: INSERT INTO hs_committing SELECT i, 1 FROM generate_series(1, 30) i;
SELECT gp_wait_until_triggered_fault('qe_start_commit_prepared', 1, dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';

-- The other two segments have committed their parts, and their mirrors
-- have: a session of a mirror's own reads the part's rows.
0M: SELECT hs_committing_has(1);
2M: SELECT hs_committing_has(1);

-- The coordinator has the transaction as one in its second phase, its
-- backend's still, and so has the standby.
SELECT count(*), bool_or(abandoned) FROM gp_internal.dtx_committing();
-1S: SELECT count(*) FROM gp_internal.dtx_committing();

-- The standby's query sees none of it: not the rows two mirrors have
-- committed, which those mirrors hide.
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;

-- A transaction block of the standby's that begins now reads so to its end.
-1S: BEGIN ISOLATION LEVEL REPEATABLE READ;
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;

SELECT gp_inject_fault('qe_start_commit_prepared', 'reset', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
1<:

-- Forgotten, on both: the INSERT returned once the standby had replayed
-- that too.
SELECT count(*) FROM gp_internal.dtx_committing();
-1S: SELECT count(*) FROM gp_internal.dtx_committing();

-- The block reads what it read, and a statement after it the transaction
-- whole.
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;
-1S: END;
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;

----------------------------------------------------------------
-- The standby's own catalog, for a CREATE TABLE in its second phase
----------------------------------------------------------------
SELECT gp_inject_fault('qe_start_commit_prepared', 'suspend', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
1&: CREATE TABLE hs_committing_made (a int) DISTRIBUTED BY (a);
SELECT gp_wait_until_triggered_fault('qe_start_commit_prepared', 1, dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';

-- The standby has replayed the table's row, and its query does not see it
-- until the mirrors' queries would see theirs.
-1S: SELECT count(*) FROM pg_class WHERE relname = 'hs_committing_made';

SELECT gp_inject_fault('qe_start_commit_prepared', 'reset', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
1<:
-1S: SELECT count(*) FROM pg_class WHERE relname = 'hs_committing_made';
-1S: SELECT count(*) FROM hs_committing_made;

----------------------------------------------------------------
-- A second phase its backend gives up on
----------------------------------------------------------------
-- Segment 1 refuses the second phase for as long as it is asked: the
-- backend that committed asks again for half a minute, says so, and
-- leaves the part to distributed transaction recovery.
SELECT gp_inject_fault_infinite('finish_commit_prepared', 'error', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
1: INSERT INTO hs_committing SELECT i, 2 FROM generate_series(1, 30) i;

-- The transaction is recovery's to forget now, and still one in its second
-- phase, on both nodes; the standby's query sees none of it.
SELECT count(*), bool_and(abandoned) FROM gp_internal.dtx_committing();
-1S: SELECT count(*) FROM gp_internal.dtx_committing();
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;

-- Segment 1 takes the second phase again: recovery commits the part within
-- its next round, and forgets the transaction, on the standby too.
SELECT gp_inject_fault('finish_commit_prepared', 'reset', dbid) FROM gp_segment_configuration WHERE content = 1 AND role = 'p';
SELECT hs_committing_none();
-1S: SELECT hs_committing_none();
-1S: SELECT b, count(*) FROM hs_committing GROUP BY b ORDER BY b;

----------------------------------------------------------------
-- A commit in one phase
----------------------------------------------------------------
-- One segment writes, and its commit is the decision: where a commit
-- returns once the standby has applied it, as the group's do
-- (synchronous_commit = remote_apply), the standby's next query sees it.
INSERT INTO hs_committing VALUES (1, 3);
-1S: SELECT b, count(*) FROM hs_committing WHERE b = 3 GROUP BY b;

----------------------------------------------------------------
-- A node asked as what it is not
----------------------------------------------------------------
-- A segment process takes a distributed snapshot only as the node its
-- dispatcher took it for: a primary none of a standby coordinator's, a
-- mirror none of the coordinator's.  Each asked here by a session that says
-- it is that dispatcher's (gp.qe_identity), as a gang's connection does.
! port=$(psql -At -c "SELECT port FROM gp_segment_configuration WHERE content = 0 AND role = 'p'" postgres) && PGOPTIONS='-c gp.qe_identity=seg0/dbid8/sess1/nseg3/standby' psql -X -q -p $port -d isolation2-hot-standby -c "BEGIN" -c "SET LOCAL gp.distributed_snapshot = '3:3:3:'" 2>&1;
! port=$(psql -At -c "SELECT port FROM gp_segment_configuration WHERE content = 0 AND role = 'm'" postgres) && PGOPTIONS='-c gp.qe_identity=seg0/dbid1/sess1/nseg3' psql -X -q -p $port -d isolation2-hot-standby -c "BEGIN" -c "SET LOCAL gp.distributed_snapshot = '3:3:3:'" 2>&1;

-1Sq:
DROP TABLE hs_committing;
DROP TABLE hs_committing_made;
DROP FUNCTION hs_committing_none();
DROP FUNCTION hs_committing_has(int);
