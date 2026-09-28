-- The port's own: pg_stat_last_operation's rows of an ANALYZE beside a DROP
-- of a relation it analyzed (gp_metatrack.c).
--
-- ANALYZE of more than one relation commits a transaction of each, and
-- Cloudberry writes each one's ANALYZE row in its own transaction, under the
-- ShareUpdateExclusiveLock ANALYZE holds on it (analyze_rel_internal()),
-- which a DROP of the relation waits for.  The port wrote the rows once the
-- statement was done, those locks gone, and a DROP beside it could remove a
-- row as the statement wrote it: the ANALYZE failed with "tuple concurrently
-- deleted", or the DROP with "tuple concurrently updated" -- lockmodes'
-- ANALYZE of a partitioned table, held at a fault beside two DROPs, now and
-- then.  Here each of the two, forced by locks: the ANALYZE waits for md_t
-- once md_p's transaction has committed, and md_wait() waits until a session
-- waits for a lock of the kind named -- a relation's, or the end of another
-- transaction -- or has done the statement named, and says which.

1: SET application_name = 'md_s1';
3: SET application_name = 'md_s3';
1: CREATE FUNCTION md_wait(app text, event text, stmt text) RETURNS text LANGUAGE plpgsql AS $$
   DECLARE r record; BEGIN
   FOR i IN 1 .. 600 LOOP
   PERFORM pg_stat_clear_snapshot(); SELECT state, wait_event, query INTO r FROM pg_stat_activity WHERE application_name = app; IF r.wait_event = event THEN
   RETURN 'waits for a ' || event; ELSIF r.state = 'idle' AND r.query LIKE stmt || '%' THEN
   RETURN 'done'; END IF; PERFORM pg_sleep(0.1); END LOOP; RETURN 'neither'; END $$;
1: CREATE TABLE md_p (a int) PARTITION BY RANGE (a);
1: CREATE TABLE md_q (a int) PARTITION BY RANGE (a);
1: CREATE TABLE md_t (a int);
1: INSERT INTO md_t SELECT generate_series(1, 10);
1: CREATE TABLE md_oids AS SELECT oid FROM pg_class WHERE relname IN ('md_p', 'md_q');
1: ANALYZE md_p, md_q, md_t;

-- The DROP removes md_p's rows first, and commits once the ANALYZE is done:
-- md_p's row was written in md_p's transaction, and the ANALYZE writes
-- nothing of md_p after it
2: BEGIN;
2: LOCK TABLE md_t IN SHARE UPDATE EXCLUSIVE MODE;
1&: ANALYZE md_p, md_t;
4: SELECT md_wait('md_s1', 'relation', 'ANALYZE');
3: BEGIN;
3: DROP TABLE md_p;
2: COMMIT;
4: SELECT md_wait('md_s1', 'transactionid', 'ANALYZE');
3: COMMIT;
1<:

-- The ANALYZE waits for md_t's row, which another transaction is changing,
-- and the DROP of md_q removes md_q's row, which the ANALYZE wrote in md_q's
-- transaction and committed: the DROP waits for nothing
4: SET allow_system_table_mods = on;
4: BEGIN;
4: UPDATE gp_internal.stat_last_operation SET stasubtype = stasubtype WHERE objid = 'md_t'::regclass AND staactionname = 'ANALYZE';
1&: ANALYZE md_q, md_t;
2: SELECT md_wait('md_s1', 'transactionid', 'ANALYZE');
3>: DROP TABLE md_q;
2: SELECT md_wait('md_s3', 'transactionid', 'DROP');
4: ROLLBACK;
1<:
3<:

-- No row names md_p or md_q, which are gone; and a VACUUM ANALYZE's VACUUM
-- row comes before its ANALYZE row, as each relation's are written as it
-- is analyzed
1: SELECT count(*) FROM pg_stat_last_operation WHERE objid IN (SELECT oid FROM md_oids);
1: VACUUM ANALYZE md_t;
1: SELECT staactionname FROM pg_stat_last_operation WHERE objid = 'md_t'::regclass AND staactionname IN ('VACUUM', 'ANALYZE') ORDER BY statime;

1: DROP TABLE md_t, md_oids;
1: DROP FUNCTION md_wait(text, text, text);
