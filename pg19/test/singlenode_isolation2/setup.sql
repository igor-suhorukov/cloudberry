--
-- The port's setup for Cloudberry's singlenode_isolation2 tests, in place of
-- Cloudberry's sql/setup.sql, which its driver runs before any test.
--
-- Cloudberry's makes PL/Python functions that drive a cluster -- pg_ctl on a
-- segment, pg_basebackup, waiting for a mirror -- and a helper the lock tests
-- use, which reads pg_locks by segment.  The port has one node, no PL/Python
-- in the image, and no gp_segment_id in pg_locks, so this makes only what the
-- tests the port runs use, for one node, and the extensions of every M1
-- module, which Cloudberry has built in.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_task;
CREATE EXTENSION gp_matview;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_security;

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
