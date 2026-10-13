--
-- The port's setup for Cloudberry's singlenode_isolation2 tests, in place of
-- Cloudberry's sql/setup.sql, which its driver runs before any test.
--
-- Cloudberry's makes PL/Python functions that drive a cluster -- pg_ctl on a
-- segment, pg_basebackup, waiting for a mirror -- and the helpers its tests
-- of locks and of append-optimized tables use.  The port runs these tests on
-- one node, which has no segment or mirror for the first to drive, so this
-- makes only what the tests the port runs use, for one node, and the
-- extensions of every M1 module, of M5's gp_ao and gp_exttable and of M6's
-- gp_resource, which Cloudberry has built in.
--
-- The planner plans the setup, as the isolation2 suite's: its statements are
-- the extensions' scripts', which ORCA would try one by one.
SET gp.optimizer = off;
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_task;
CREATE EXTENSION gp_matview;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_security;
CREATE EXTENSION gp_ao;
CREATE EXTENSION gp_exttable;
CREATE EXTENSION gp_resource;
CREATE EXTENSION gp_inject_fault;
-- PL/Python, in which some of Cloudberry's tests write helpers of their own,
-- as Cloudberry's setup makes it for its own.
CREATE EXTENSION plpython3u;

--
-- Faults for everyone, as Cloudberry's script grants them and its tests
-- inject them, some as roles of their own (gp_inject_fault--1.0.sql).
--
GRANT EXECUTE ON FUNCTION gp_inject_fault(text, text, text, text, text,
	int4, int4, int4, int4, int4) TO PUBLIC;

--
-- CREATE on the schema public for everyone, which PostgreSQL gave PUBLIC
-- until 15 and Cloudberry gives it still: a test's own role makes its
-- tables there.
--
GRANT CREATE ON SCHEMA public TO PUBLIC;

--
-- What Cloudberry's tests of append-optimized tables ask of its setup, as
-- it makes them, and as the isolation2 suite's setup has them for a cluster.
--

-- gp_ao_or_aocs_seg(rel): the table's segment files, of a row table
-- (gp_toolkit.__gp_aoseg) or a column table (__gp_aocsseg, a row a file
-- there, not one a column), so that one test reads both.
CREATE FUNCTION gp_ao_or_aocs_seg(rel regclass,
	segment_id OUT integer,
	segno OUT integer,
	tupcount OUT bigint,
	modcount OUT bigint,
	formatversion OUT smallint,
	state OUT smallint)
RETURNS SETOF record AS $$
DECLARE
	amname_var text;
BEGIN
	SELECT amname INTO amname_var FROM pg_class c, pg_am am WHERE c.relam = am.oid AND c.oid = rel;
	IF amname_var = 'ao_column' THEN
		FOR segment_id, segno, tupcount, modcount, formatversion, state IN
			SELECT DISTINCT x.segment_id, x.segno, x.tupcount, x.modcount, x.formatversion, x.state
			  FROM gp_toolkit.__gp_aocsseg(rel) x
		LOOP
			RETURN NEXT;
		END LOOP;
	ELSIF amname_var = 'ao_row' THEN
		FOR segment_id, segno, tupcount, modcount, formatversion, state IN
			SELECT x.segment_id, x.segno, x.tupcount, x.modcount, x.formatversion, x.state
			  FROM gp_toolkit.__gp_aoseg(rel) x
		LOOP
			RETURN NEXT;
		END LOOP;
	ELSE
		RAISE '% is not an AO_ROW or AO_COLUMN table', rel::text;
	END IF;
END;
$$ LANGUAGE plpgsql;

-- locktest_master, locktest_segments_dist and locktest_segments: the locks
-- held in this database, on the coordinator and, in summary, on the
-- segments, of which the one node has none.  Cloudberry's, as they are: its
-- names of an append-optimized table's auxiliary relations and its lock on
-- a segment file match nothing here, where gp_ao keeps a table's segment
-- files, visibility map and block directory in tables of its own, every
-- append-optimized table's (gp_ao--1.0.sql).
CREATE VIEW locktest_master AS
SELECT coalesce(
  CASE WHEN relname LIKE 'pg_toast%index' THEN 'toast index'
       WHEN relname LIKE 'pg_toast%' THEN 'toast table'
       WHEN relname LIKE 'pg_aoseg%' THEN 'aoseg table'
       WHEN relname LIKE 'pg_aovisimap%index' THEN 'aovisimap index'
       WHEN relname LIKE 'pg_aovisimap%' THEN 'aovisimap table'
       ELSE relname END, 'dropped table'),
  mode,
  locktype,
  'master'::text AS node
FROM pg_locks l
LEFT OUTER JOIN pg_class c ON ((l.locktype = 'append-only segment file' AND l.relation = c.relfilenode) OR (l.locktype != 'append-only segment file' AND l.relation = c.oid)),
pg_database d
WHERE relation IS NOT NULL
AND l.database = d.oid
AND (relname <> 'gp_fault_strategy' AND relname != 'locktest_master' OR relname IS NULL)
AND d.datname = current_database()
AND l.gp_segment_id = -1
GROUP BY l.gp_segment_id, relation, relname, locktype, mode
ORDER BY 1, 3, 2;

CREATE VIEW locktest_segments_dist AS
SELECT relname,
  mode,
  locktype,
  l.gp_segment_id AS node,
  relation
FROM pg_locks l
LEFT OUTER JOIN pg_class c ON ((l.locktype = 'append-only segment file' AND l.relation = c.relfilenode) OR (l.locktype != 'append-only segment file' AND l.relation = c.oid)),
pg_database d
WHERE relation IS NOT NULL
AND l.database = d.oid
AND (relname <> 'gp_fault_strategy' AND relname != 'locktest_segments_dist' OR relname IS NULL)
AND d.datname = current_database()
AND l.gp_segment_id > -1
GROUP BY l.gp_segment_id, relation, relname, locktype, mode;

CREATE VIEW locktest_segments AS
SELECT coalesce(
  CASE WHEN relname LIKE 'pg_toast%index' THEN 'toast index'
       WHEN relname LIKE 'pg_toast%' THEN 'toast table'
       WHEN relname LIKE 'pg_aoseg%' THEN 'aoseg table'
       WHEN relname LIKE 'pg_aovisimap%index' THEN 'aovisimap index'
       WHEN relname LIKE 'pg_aovisimap%' THEN 'aovisimap table'
       ELSE relname END, 'dropped table'),
  mode,
  locktype,
  CASE WHEN count(*) = 1 THEN '1 segment'
       ELSE 'n segments' END AS node
  FROM gp_dist_random('locktest_segments_dist')
  GROUP BY relname, relation, mode, locktype;

-- Cloudberry's, as it is.
CREATE FUNCTION master() RETURNS SETOF gp_segment_configuration AS $$
	SELECT * FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
$$ LANGUAGE sql;

-- Cloudberry's, less the segment: -1, the coordinator, is the one node there
-- is, and any other segment has no locks to wait for.
CREATE FUNCTION wait_until_waiting_for_required_lock(rel_name text, lmode text, segment_id integer)
RETURNS bool AS
$$
DECLARE
	retries int := 1200;
BEGIN
	IF segment_id <> -1 THEN
		RETURN false;
	END IF;
	LOOP
		IF EXISTS (SELECT 1 FROM pg_locks l
					WHERE NOT l.granted
					  AND l.relation = rel_name::regclass
					  AND l.mode = lmode) THEN
			RETURN true;
		END IF;
		IF retries <= 0 THEN
			RETURN false;
		END IF;
		PERFORM pg_sleep(0.1);
		retries := retries - 1;
	END LOOP;
END;
$$ LANGUAGE plpgsql;
