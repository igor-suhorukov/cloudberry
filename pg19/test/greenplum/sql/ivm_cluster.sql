--
-- Incremental materialized views on a cluster (gp_matview's ivm_cluster.c).
--
-- A base table's rows are written on the segments, whose triggers keep what
-- each statement changed; once the statement is over the coordinator brings
-- each view up to date from all of it -- by a delta where the view's shape
-- allows one, each segment given the delta rows of its own view rows, and by
-- recomputing it where it does not.  What is checked is that every view says
-- what its query would say after every kind of write, on the planner's route
-- and ORCA's, and which of the two ways did it: the contents do not say.
--
CREATE SCHEMA ivm_cluster;
SET search_path = ivm_cluster;

-- What a view holds, less what its query says, and the other way round.
CREATE FUNCTION differ(view_query text, fresh text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  EXECUTE format('SELECT count(*) FROM ((%s EXCEPT ALL %s) UNION ALL (%s EXCEPT ALL %s)) d',
                 view_query, fresh, fresh, view_query) INTO n;
  RETURN n;
END $$;

CREATE TABLE base (id int, grp int, amt numeric) DISTRIBUTED BY (id);
INSERT INTO base SELECT g, g % 7, g FROM generate_series(1, 100) g;
CREATE TABLE tags (grp int, tag text) DISTRIBUTED BY (grp);
INSERT INTO tags SELECT g, 't' || g FROM generate_series(0, 6) g;

-- Each shape a delta is taken for, and one it is not: min() and max().
CREATE INCREMENTAL MATERIALIZED VIEW v_agg AS
  SELECT grp, count(*) AS n, sum(amt) AS total, avg(amt) AS mean FROM base GROUP BY grp;
CREATE INCREMENTAL MATERIALIZED VIEW v_all AS
  SELECT count(*) AS n, sum(amt) AS total FROM base;
CREATE INCREMENTAL MATERIALIZED VIEW v_rows AS
  SELECT id, amt FROM base WHERE amt > 50;
CREATE INCREMENTAL MATERIALIZED VIEW v_distinct AS
  SELECT DISTINCT grp FROM base;
CREATE INCREMENTAL MATERIALIZED VIEW v_join AS
  SELECT b.id, t.tag FROM base b, tags t WHERE b.grp = t.grp;
CREATE INCREMENTAL MATERIALIZED VIEW v_self AS
  SELECT a.id AS x, b.id AS y FROM base a, base b
   WHERE a.grp = b.grp AND a.id < 10 AND b.id < 10;
CREATE INCREMENTAL MATERIALIZED VIEW v_cols (g, cnt, s) AS
  SELECT grp, count(*), sum(amt) FROM base GROUP BY grp;
CREATE INCREMENTAL MATERIALIZED VIEW v_minmax AS
  SELECT grp, min(amt) AS lo, max(amt) AS hi FROM base GROUP BY grp;

-- Each view's rows are where the delta rows of them are sent: by the columns
-- it is grouped by, its base table's key where that is one of them, and a
-- view of one row -- an aggregate without GROUP BY -- on every segment.
SELECT c.relname, p.policytype, p.distkey
  FROM gp_distribution_policy p JOIN pg_class c ON c.oid = p.localoid
 WHERE c.relnamespace = 'ivm_cluster'::regnamespace ORDER BY 1;

-- The triggers that keep what changed are on every node's copy of the table.
SELECT count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass AND tgisinternal;
SELECT gp_segment_id, count(*) FROM gp_dist_random('pg_trigger')
 WHERE tgrelid = 'base'::regclass AND tgisinternal GROUP BY 1 ORDER BY 1;

CREATE FUNCTION wrong() RETURNS text LANGUAGE sql AS $$
  SELECT coalesce(string_agg(v, ' ' ORDER BY v), 'none') FROM (
    SELECT 'v_agg' AS v WHERE differ('SELECT grp, n, total, mean FROM v_agg',
      'SELECT grp, count(*), sum(amt), avg(amt) FROM base GROUP BY grp') > 0
    UNION ALL SELECT 'v_all' WHERE differ('SELECT n, total FROM v_all',
      'SELECT count(*), sum(amt) FROM base') > 0
    UNION ALL SELECT 'v_rows' WHERE differ('SELECT id, amt FROM v_rows',
      'SELECT id, amt FROM base WHERE amt > 50') > 0
    UNION ALL SELECT 'v_distinct' WHERE differ('SELECT grp FROM v_distinct',
      'SELECT DISTINCT grp FROM base') > 0
    UNION ALL SELECT 'v_join' WHERE differ('SELECT id, tag FROM v_join',
      'SELECT b.id, t.tag FROM base b, tags t WHERE b.grp = t.grp') > 0
    UNION ALL SELECT 'v_self' WHERE differ('SELECT x, y FROM v_self',
      'SELECT a.id, b.id FROM base a, base b WHERE a.grp = b.grp AND a.id < 10 AND b.id < 10') > 0
    UNION ALL SELECT 'v_cols' WHERE differ('SELECT g, cnt, s FROM v_cols',
      'SELECT grp, count(*), sum(amt) FROM base GROUP BY grp') > 0
    UNION ALL SELECT 'v_minmax' WHERE differ('SELECT grp, lo, hi FROM v_minmax',
      'SELECT grp, min(amt), max(amt) FROM base GROUP BY grp') > 0) w $$;
-- How the views were maintained since this was last asked.
CREATE FUNCTION how() RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  r := gp_matview.applied_delta() || ' by delta, ' ||
       gp_matview.recomputed() || ' recomputed';
  PERFORM gp_matview.stats_reset();
  RETURN r;
END $$;

SELECT wrong() AS "views wrong at the start", how();

-- The planner's route: an INSERT's rows routed by COPY, an UPDATE and a
-- DELETE sent to the segments as they are, and one the coordinator's plan
-- writes row by row, reading another table.  Seven views by delta and
-- v_minmax recomputed for each, the self-join's two places taken against the
-- table as the statement found it.
SET gp.optimizer = off;
INSERT INTO base SELECT g, g % 5, g * 2 FROM generate_series(101, 150) g;
SELECT wrong(), how();
UPDATE base SET amt = amt + 1 WHERE id % 3 = 0;
SELECT wrong(), how();
DELETE FROM base WHERE id % 4 = 0;
SELECT wrong(), how();
UPDATE base SET amt = amt + t.grp FROM tags t WHERE base.grp = t.grp AND t.tag = 't1';
SELECT wrong(), how();

-- The other table of the join: one view.
INSERT INTO tags VALUES (2, 't2b');
SELECT wrong(), how();

-- Both of the join's tables in one statement: each delta taken against the
-- other table as the statement found it, so the rows the two additions make
-- together are counted once.
WITH ins AS (INSERT INTO base VALUES (200, 9, 1) RETURNING grp)
  INSERT INTO tags SELECT grp, 'new' FROM ins;
SELECT wrong(), how();

-- ORCA's writes, in the segments' slices.
SET gp.optimizer = on;
INSERT INTO base SELECT g, g % 5, g * 2 FROM generate_series(151, 200) g;
SELECT wrong(), how();
UPDATE base SET amt = amt + 1 WHERE id % 3 = 1;
SELECT wrong(), how();
DELETE FROM base WHERE id % 5 = 0;
SELECT wrong(), how();
MERGE INTO base b USING (SELECT g AS id, g % 3 AS grp FROM generate_series(95, 105) g) s
   ON b.id = s.id
 WHEN MATCHED THEN UPDATE SET amt = b.amt * 2
 WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.grp, 1);
SELECT wrong(), how();
RESET gp.optimizer;

-- COPY FROM, and a statement rolled back to a savepoint: its maintenance
-- goes with it.
COPY base FROM STDIN;
500	3	7
501	4	8
\.
BEGIN;
SAVEPOINT s;
DELETE FROM base;
ROLLBACK TO s;
INSERT INTO base VALUES (502, 1, 9);
COMMIT;
SELECT wrong(), how();

-- TRUNCATE leaves no rows to take a delta from: every view is recomputed.
TRUNCATE base;
SELECT wrong(), how();
INSERT INTO base SELECT g, g % 7, g FROM generate_series(1, 20) g;
SELECT wrong(), how();

-- Whoever writes a base table need not be able to read or write the view:
-- it is maintained as its owner.
CREATE ROLE ivm_cluster_writer;
GRANT USAGE ON SCHEMA ivm_cluster TO ivm_cluster_writer;
GRANT INSERT, UPDATE, DELETE, SELECT ON base TO ivm_cluster_writer;
SET ROLE ivm_cluster_writer;
INSERT INTO base VALUES (600, 2, 60);
SELECT count(*) FROM v_agg;
RESET ROLE;
SELECT wrong();

-- A view made WITH NO DATA is left alone until REFRESH fills it.
CREATE INCREMENTAL MATERIALIZED VIEW v_later AS
  SELECT grp, count(*) AS n FROM base GROUP BY grp WITH NO DATA;
INSERT INTO base VALUES (601, 3, 1);
SELECT count(*) FROM v_later;
REFRESH MATERIALIZED VIEW v_later;
INSERT INTO base VALUES (602, 3, 1);
SELECT differ('SELECT grp, n FROM v_later', 'SELECT grp, count(*) FROM base GROUP BY grp');

-- A distribution a delta row could not be sent by is refused: the counts of
-- a group change with every delta, and a random one has no segment a row is
-- on.  Replicated is every row on every segment, each given every delta row.
CREATE INCREMENTAL MATERIALIZED VIEW v_bad AS
  SELECT grp, count(*) AS n FROM base GROUP BY grp DISTRIBUTED BY (n);
CREATE INCREMENTAL MATERIALIZED VIEW v_bad AS
  SELECT id, amt FROM base DISTRIBUTED RANDOMLY;
CREATE INCREMENTAL MATERIALIZED VIEW v_repl AS
  SELECT grp, count(*) AS n FROM base GROUP BY grp DISTRIBUTED REPLICATED;
DELETE FROM base WHERE id < 5;
SELECT differ('SELECT grp, n FROM v_repl', 'SELECT grp, count(*) FROM base GROUP BY grp');
SELECT count(DISTINCT gp_segment_id), count(*) = 3 * count(DISTINCT grp)
  FROM gp_dist_random('v_repl');

-- The key of a base table of an incremental view is not updated: the row
-- would move by a Split, which fires no trigger, as Cloudberry refuses it.
UPDATE base SET id = id + 1000 WHERE id = 10;

-- The segments' part of a view's maintenance is the coordinator's alone to
-- call.
SELECT gp_matview.ivm_apply('v_agg'::regclass, false);
SELECT gp_matview.ivm_stage('v_agg'::regclass, 'n', '{}');

-- Dropping a view drops its triggers on every node.
DROP MATERIALIZED VIEW v_self, v_minmax, v_cols, v_distinct, v_rows, v_all, v_agg, v_later, v_repl;
SELECT gp_segment_id, count(*) FROM gp_dist_random('pg_trigger')
 WHERE tgrelid = 'base'::regclass AND tgisinternal GROUP BY 1 ORDER BY 1;
DROP MATERIALIZED VIEW v_join;
SELECT count(*) FROM gp_dist_random('pg_trigger') WHERE tgrelid = 'base'::regclass;

RESET search_path;
SET client_min_messages = warning;
DROP SCHEMA ivm_cluster CASCADE;
DROP ROLE ivm_cluster_writer;
RESET client_min_messages;
