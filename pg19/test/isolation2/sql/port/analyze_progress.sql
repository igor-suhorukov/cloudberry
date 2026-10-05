-- The port's own: gp_stat_progress_analyze_summary, Cloudberry's view of
-- an ANALYZE's progress over the segments, on the half of Cloudberry's
-- analyze_progress the port runs -- a hash-distributed table, whose
-- segments each sample their own rows (gp_analyze.c), each held at the
-- 20th block of its sample (analyze_block).  The other half's replicated
-- table the port samples on one segment, which holds all its rows, so a
-- fault set on every segment is hit on one.  The summary adds up the three
-- segments' blocks: 60 scanned, as Cloudberry's, of their blocks to sample,
-- every block of the table, smaller than the sample -- its count is not
-- Cloudberry's 111, of 32 kB pages, nor the same in the two passes, whose
-- INSERTs fill the segments' 8 kB pages differently.
CREATE TABLE t_analyze_part (a INT, b INT) DISTRIBUTED BY (a);
INSERT INTO t_analyze_part SELECT i, i FROM generate_series(1, 100000) i;

SELECT gp_inject_fault('analyze_block', 'suspend', '', '', '', 20, 20, 0, dbid) FROM gp_segment_configuration WHERE content > -1 AND role = 'p';

1&: ANALYZE t_analyze_part;
SELECT gp_wait_until_triggered_fault('analyze_block', 1, dbid) FROM gp_segment_configuration WHERE content > -1 AND role = 'p';

2: SELECT pid IS NOT NULL as has_pid, datname, relid::regclass, phase, sample_blks_total = pg_relation_size('t_analyze_part') / current_setting('block_size')::int AS every_block, sample_blks_scanned FROM gp_stat_progress_analyze_summary;
-- and each segment's row of gp_stat_progress_analyze
2: SELECT gp_segment_id, relid::regclass, phase, sample_blks_scanned FROM gp_stat_progress_analyze WHERE gp_segment_id > -1 ORDER BY 1;

SELECT gp_inject_fault('analyze_block', 'reset', dbid) FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
1<:

-- nothing in progress once it is done
2: SELECT count(*) FROM gp_stat_progress_analyze_summary;

DROP TABLE t_analyze_part;
