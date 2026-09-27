-- The port's own: parallel retrieve cursors on one node, in place of
-- Cloudberry's parallel_retrieve_cursor_schedule of this suite, whose
-- expected output is a cluster's.  One node's cursor has its one endpoint
-- there, filled as DECLARE runs its plan (gp_endpoint.c), and a retrieve
-- session of the node (-1R:) reads it.
CREATE TABLE prc_t (a int);
INSERT INTO prc_t SELECT generate_series(1, 20);

1: BEGIN;
1: DECLARE c1 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_t;
1: @post_run 'parse_endpoint_info 1 1 2 3 4': SELECT endpointname,auth_token,hostname,port,state FROM gp_get_endpoints() WHERE cursorname='c1';
1: SELECT gp_segment_id, cursorname, state FROM gp_get_endpoints();
2: SELECT senderpid <> -1, receiverpid <> -1, state FROM gp_get_segment_endpoints();
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c1', 0);
-1R: @pre_run 'set_endpoint_variable @ENDPOINT1': RETRIEVE 5 FROM ENDPOINT "@ENDPOINT1";
2: SELECT senderpid <> -1, receiverpid <> -1, state FROM gp_get_segment_endpoints();
-1R: @pre_run 'set_endpoint_variable @ENDPOINT1': RETRIEVE ALL FROM ENDPOINT "@ENDPOINT1";
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c1', -1);
1: SELECT state FROM gp_get_endpoints();
-- a retrieve session runs RETRIEVE alone
-1R: SELECT 1;
-1R: @pre_run 'set_endpoint_variable @ENDPOINT1': RETRIEVE ALL FROM ENDPOINT "@ENDPOINT1";
1: ROLLBACK;
1: SELECT count(*) FROM gp_get_endpoints();
-1Rq:

-- where the endpoint is, as EXPLAIN says it
EXPLAIN (COSTS false) DECLARE c2 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_t ORDER BY a;

-- a retrieve session that quits partway cancels the cursor
1: BEGIN;
1: DECLARE c3 PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc_t ORDER BY a;
1: @post_run 'parse_endpoint_info 3 1 2 3 4': SELECT endpointname,auth_token,hostname,port,state FROM gp_get_endpoints() WHERE cursorname='c3';
-1R: @pre_run 'set_endpoint_variable @ENDPOINT3': RETRIEVE 5 FROM ENDPOINT "@ENDPOINT3";
-1Rq:
1: SELECT * FROM gp_wait_parallel_retrieve_cursor('c3', 0);
1: ROLLBACK;
1: SELECT count(*) FROM gp_get_endpoints();

DROP TABLE prc_t;
