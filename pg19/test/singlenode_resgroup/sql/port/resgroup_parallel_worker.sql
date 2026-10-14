-- A query's parallel workers, which one node has where a cluster's
-- coordinator rarely does: PostgreSQL's planner gives a query of one node
-- them.  A worker runs in the cgroup of the group its query's transaction
-- took its slot in, as the query's own backend does, so that the group's
-- share of the CPU is the query's whole.

-- start_ignore
DROP ROLE IF EXISTS role_parallel_test;
DROP RESOURCE GROUP rg_parallel_test;
-- end_ignore
CREATE RESOURCE GROUP rg_parallel_test WITH (concurrency=2, cpu_max_percent=20);
CREATE ROLE role_parallel_test RESOURCE GROUP rg_parallel_test;
CREATE TABLE t_parallel_test (a int);
INSERT INTO t_parallel_test SELECT generate_series(1, 1000);
GRANT SELECT ON t_parallel_test TO role_parallel_test;

-- Whether the backends of a kind that run a query of the group -- its own,
-- 'client backend', or its workers, 'parallel worker' -- are in the group's
-- cgroup: looked at until there is one and each is, for half a minute.
CREATE FUNCTION in_cgroup_of(grp text, kind text) RETURNS bool AS $$
    import time
    oid = str(plpy.execute("SELECT oid FROM pg_resgroup WHERE rsgname = '%s'" % grp)[0]['oid'])
    for i in range(300):
        plpy.execute("SELECT pg_stat_clear_snapshot()")
        pids = [r['pid'] for r in plpy.execute("""
            SELECT a.pid
              FROM pg_stat_activity a, pg_stat_activity l
             WHERE l.rsgname = '%s' AND l.backend_type = 'client backend'
               AND a.backend_type = '%s'
               AND (a.pid = l.pid OR a.leader_pid = l.pid)""" % (grp, kind))]
        try:
            if pids and all(oid in open('/proc/%d/cgroup' % p).read().strip().split('/') for p in pids):
                return True
        except IOError:
            pass
        time.sleep(0.1)
    return False
$$ LANGUAGE plpython3u;

-- a plan of one worker, which the leader waits for: fifty seconds of it
1: SET ROLE role_parallel_test;
1: SET optimizer TO off;
1: SET debug_parallel_query TO on;
1: EXPLAIN (COSTS OFF) SELECT count(*) FROM t_parallel_test WHERE pg_sleep(0.05) IS NOT NULL;
1&: SELECT count(*) FROM t_parallel_test WHERE pg_sleep(0.05) IS NOT NULL;

2: SELECT in_cgroup_of('rg_parallel_test', 'client backend');
2: SELECT in_cgroup_of('rg_parallel_test', 'parallel worker');
-- the worker takes no slot: the query is one of the group's
2: SELECT num_running FROM gp_toolkit.gp_resgroup_status WHERE groupname = 'rg_parallel_test';
2: SELECT pg_cancel_backend(pid) FROM pg_stat_activity
    WHERE rsgname = 'rg_parallel_test' AND backend_type = 'client backend';
1<:
1q:
2q:

DROP FUNCTION in_cgroup_of(text, text);
DROP TABLE t_parallel_test;
DROP ROLE role_parallel_test;
DROP RESOURCE GROUP rg_parallel_test;
