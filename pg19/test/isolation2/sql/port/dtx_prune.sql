-- The port's own: a segment's VACUUM recycles the file it compacted once no
-- snapshot may still read it -- none the coordinator may still send that sees
-- a part the compaction followed in progress, by which the segment forgets
-- its parts ("prune", gp_dtx.c), and none of the segment's own that sees the
-- compaction in progress (gp_ao's ao_vacuum.c).  Neither counts a process
-- whose snapshot reads no table's rows: a lazy VACUUM as it vacuums a table,
-- which PostgreSQL's horizons pass over too, and on the coordinator an
-- autovacuum worker, whose snapshots the segments are never sent.  Such
-- processes held the file in full runs and reproductions of
-- uao/ao_unique_index_vacuum: the coordinator's autovacuum worker, and a
-- segment's VACUUM of gp_ao.blkdir.  Each case below holds one of them, its
-- xmin older than an INSERT and a DELETE of a row of five, at one of
-- PostgreSQL's injection points: the VACUUM after them compacts the file the
-- DELETE left and recycles it, and segment 0's block directory has the one
-- row of the file the four rows went to.

CREATE TABLE dtx_prune_heap (a int) DISTRIBUTED BY (a);

-- A VACUUM of the coordinator's held as it vacuums a table, in vacuum_rel()
-- after it has said PROC_IN_VACUUM and taken its snapshot.
SELECT gp_inject_fault('vacuum-index-cleanup-auto', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1&: VACUUM dtx_prune_heap;
SELECT gp_wait_until_triggered_fault('vacuum-index-cleanup-auto', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

CREATE TABLE dtx_prune_ao (i int UNIQUE) USING ao_row DISTRIBUTED REPLICATED;
INSERT INTO dtx_prune_ao SELECT generate_series(1, 5);
DELETE FROM dtx_prune_ao WHERE i = 5;
VACUUM dtx_prune_ao;
SELECT (gp_toolkit.__gp_aoblkdir('dtx_prune_ao')).* FROM gp_dist_random('gp_id') WHERE gp_segment_id = 0 ORDER BY 1,2,3,4,5;
DROP TABLE dtx_prune_ao;

SELECT gp_inject_fault('vacuum-index-cleanup-auto', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1<:

-- A VACUUM of segment 0's own, in a session of the segment's, held the same
-- way: the segment's VACUUM passes over it as it asks whether a snapshot
-- there may still see its compaction in progress.
SELECT gp_inject_fault('vacuum-index-cleanup-auto', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
0U&: VACUUM dtx_prune_heap;
SELECT gp_wait_until_triggered_fault('vacuum-index-cleanup-auto', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;

CREATE TABLE dtx_prune_ao (i int UNIQUE) USING ao_row DISTRIBUTED REPLICATED;
INSERT INTO dtx_prune_ao SELECT generate_series(1, 5);
DELETE FROM dtx_prune_ao WHERE i = 5;
VACUUM dtx_prune_ao;
SELECT (gp_toolkit.__gp_aoblkdir('dtx_prune_ao')).* FROM gp_dist_random('gp_id') WHERE gp_segment_id = 0 ORDER BY 1,2,3,4,5;
DROP TABLE dtx_prune_ao;

SELECT gp_inject_fault('vacuum-index-cleanup-auto', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
0U<:

-- An autovacuum worker of the coordinator's held between two tables, its
-- catalog snapshot's xmin set: autovacuum on the coordinator alone, at once,
-- and for any table changed -- ALTER SYSTEM writes every node's file, and
-- the coordinator alone reads its own again.
SELECT gp_inject_fault('autovacuum-worker-cost-balanced', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
ALTER SYSTEM SET autovacuum_naptime = 1;
ALTER SYSTEM SET autovacuum_analyze_threshold = 0;
ALTER SYSTEM SET autovacuum_analyze_scale_factor = 0;
ALTER SYSTEM SET autovacuum = on;
SELECT pg_reload_conf();
SELECT gp_wait_until_triggered_fault('autovacuum-worker-cost-balanced', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
SELECT count(*) > 0 AS worker_has_xmin FROM pg_stat_activity WHERE backend_type = 'autovacuum worker' AND backend_xmin IS NOT NULL;

CREATE TABLE dtx_prune_ao (i int UNIQUE) USING ao_row DISTRIBUTED REPLICATED;
INSERT INTO dtx_prune_ao SELECT generate_series(1, 5);
DELETE FROM dtx_prune_ao WHERE i = 5;
VACUUM dtx_prune_ao;
SELECT (gp_toolkit.__gp_aoblkdir('dtx_prune_ao')).* FROM gp_dist_random('gp_id') WHERE gp_segment_id = 0 ORDER BY 1,2,3,4,5;
DROP TABLE dtx_prune_ao;

-- The worker let go, and autovacuum as disable_autovacuum left it.
SELECT gp_inject_fault('autovacuum-worker-cost-balanced', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
ALTER SYSTEM SET autovacuum = off;
ALTER SYSTEM RESET autovacuum_analyze_scale_factor;
ALTER SYSTEM RESET autovacuum_analyze_threshold;
ALTER SYSTEM RESET autovacuum_naptime;
SELECT pg_reload_conf();

DROP TABLE dtx_prune_heap;
