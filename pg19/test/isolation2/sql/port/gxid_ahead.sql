-- The port's own: a transaction ID the segments hold parts prepared under
-- is given to nobody again, though the coordinator fails before it has
-- logged anything under it (gp_dtx.c, GpDtxGxid()).  A distributed
-- transaction's ID is the coordinator's own, taken as it commits, and an
-- INSERT writes nothing on the coordinator before its commit record: a
-- coordinator that failed between its two phases came back with that ID
-- its next, and its recovery process, which ends a part left prepared by
-- what became of the transaction of its ID, left the parts alone, then
-- ended them by what became of whoever was given the ID next -- a part
-- whose transaction never committed, committed.  The coordinator's WAL says
-- now, before the ID goes to a segment, that it gives none up to there
-- again.  Cloudberry logs its distributed transaction IDs ahead in batches
-- (XLOG_NEXTGXID), and none of its tests asks.

-- The parts every segment holds prepared, by their gids.
CREATE FUNCTION gxid_ahead_parts() RETURNS SETOF text AS $$
	SELECT gid::text FROM pg_prepared_xacts WHERE gid LIKE 'gp\_dtx\_%'	/* in func */
$$ LANGUAGE sql EXECUTE ON ALL SEGMENTS;

-- Until this node holds no prepared part, for at most secs seconds.
CREATE FUNCTION gxid_ahead_no_parts(secs int) RETURNS bool AS $$
BEGIN	/* in func */
	FOR i IN 1..secs * 10 LOOP	/* in func */
		IF NOT EXISTS (SELECT FROM pg_prepared_xacts) THEN	/* in func */
			RETURN true;	/* in func */
		END IF;	/* in func */
		PERFORM pg_sleep(0.1);	/* in func */
	END LOOP;	/* in func */
	RETURN false;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

CREATE TABLE gxid_ahead (a int) DISTRIBUTED BY (a);

-- An INSERT into every segment is held with its parts prepared.  The
-- coordinator has written nothing of it: its commit record is to come.
1: SELECT gp_inject_fault_infinite('transaction_abort_after_distributed_prepared', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
2&: INSERT INTO gxid_ahead SELECT generate_series(1, 30);
1: SELECT gp_wait_until_triggered_fault('transaction_abort_after_distributed_prepared', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1: SELECT count(*) AS parts, count(DISTINCT gid) AS ids FROM gxid_ahead_parts() gid;
-- The ID they are prepared under, kept where the coordinator's restart
-- leaves it.
1: COPY (SELECT DISTINCT substr(gid, 8) FROM gxid_ahead_parts() gid) TO '/tmp/gxid_ahead_id';

-- The coordinator fails.
1: SELECT gp_inject_fault('exec_simple_query_start', 'panic', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1: SELECT 1;
2<:

-- Back, it gives the next transaction an ID past that one.
3: SELECT pg_current_xact_id()::text::bigint > pg_read_file('/tmp/gxid_ahead_id')::bigint AS given_to_nobody_again;

-- And its recovery process has rolled the parts back as it started: their
-- ID names a transaction that did not commit.
0U: SELECT gxid_ahead_no_parts(30);
1U: SELECT gxid_ahead_no_parts(30);
2U: SELECT gxid_ahead_no_parts(30);
3: SELECT count(*) FROM gxid_ahead;

-- The table takes rows as before.
3: INSERT INTO gxid_ahead SELECT generate_series(1, 30);
3: SELECT count(*) FROM gxid_ahead;

3: DROP TABLE gxid_ahead;
3: DROP FUNCTION gxid_ahead_parts();
3: DROP FUNCTION gxid_ahead_no_parts(int);
