-- The port's own: a parallel retrieve cursor of a replicated table, whose
-- one endpoint is on the segment the session's id picks, as Cloudberry picks
-- it -- gp_session_id modulo the table's segments -- and sorts there.
-- Cloudberry's replicated_table finds, among six sessions, the one whose
-- gp_session_id modulo 3 is 1, which its session counter always gives, and
-- the port's session ids, the coordinator backends' process ids, may not;
-- this is its test with one session, the endpoint read through
-- gp_retrieve_rows (bin/), wherever it is.
CREATE TABLE prc_rt (a int) DISTRIBUTED REPLICATED;
INSERT INTO prc_rt SELECT generate_series(1, 100);

1: BEGIN;
1: DECLARE c1 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_rt;
1: SELECT count(*), bool_and(gp_segment_id = current_setting('gp_session_id')::int % 3) AS picked, bool_and(state = 'READY') AS ready FROM gp_get_endpoints() WHERE cursorname = 'c1';
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c1', 0);
! gp_retrieve_rows isolation2test c1 10;
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c1', 0);
1: SELECT state FROM gp_get_endpoints() WHERE cursorname = 'c1';
1: ROLLBACK;

-- sorted on the segment, as Cloudberry's planner leaves the Sort there
1: @post_run 'create_sub "on segment: contentid \[[0-9]+\]" "on segment: contentid [SEGIDX]"': EXPLAIN (COSTS false) DECLARE c2 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_rt ORDER BY a DESC;
1: BEGIN;
1: DECLARE c2 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_rt ORDER BY a DESC;
! gp_retrieve_rows isolation2test c2 5;
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c2', -1);
1: ROLLBACK;

-- and the session waiting for one, cancelled, takes its endpoint along
1: BEGIN;
1: DECLARE c3 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_rt;
1&: SELECT * FROM gp_wait_parallel_retrieve_cursor('c3', -1);
2: SELECT pg_cancel_backend(pid) FROM pg_stat_activity, gp_get_endpoints() WHERE sess_id = sessionid AND cursorname = 'c3';
1<:
1: ROLLBACK;
2: SELECT count(*) FROM gp_get_endpoints();

DROP TABLE prc_rt;
