--
-- The port's setup for Cloudberry's isolation2 tests on a cluster, in place
-- of Cloudberry's own (src/test/isolation2/sql/setup.sql), which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded on every
-- node, and as extensions, created here and by DDL dispatch on the segments.
-- Cloudberry's helpers are PL/Python; the ones the tests the manifest runs
-- call are here, in SQL: a node restarted with COPY ... TO PROGRAM, and a
-- lock waited for through each segment's own pg_locks.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_inject_fault;

--
-- pg_ctl(datadir, command, command_mode): stop or restart the node whose
-- data directory that is, waiting for it, as Cloudberry's does.  The
-- harness keeps each node's log beside its data directory.
--
CREATE FUNCTION pg_ctl(datadir text, command text, command_mode text DEFAULT 'immediate')
RETURNS text AS $$
BEGIN
	IF command NOT IN ('stop', 'restart') THEN
		RETURN 'Invalid command input';
	END IF;
	EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
				   format('@BINDIR@/pg_ctl -l %s.log -D %s -w -t 600 -m %s %s > /dev/null 2>&1',
						  datadir, datadir, command_mode, command));
	RETURN 'OK';
END;
$$ LANGUAGE plpgsql;

--
-- wait_until_waiting_for_required_lock(rel_name, lmode, segment_id): true
-- once somebody on that node waits for that lock on that relation.
-- Cloudberry's pg_locks has every node's locks, with their gp_segment_id;
-- the port asks the node's own.
--
CREATE FUNCTION wait_until_waiting_for_required_lock(rel_name text, lmode text, segment_id integer)
RETURNS bool AS $$
DECLARE
	retries int := 1200;
	q text := format('SELECT count(*) FROM pg_locks WHERE NOT granted AND relation = %L::regclass AND mode = %L',
					 rel_name, lmode);
	n int;
BEGIN
	LOOP
		IF segment_id = -1 THEN
			EXECUTE q INTO n;
		ELSE
			SELECT result::int INTO n FROM gp.exec_on_segments(q) WHERE content = segment_id;
		END IF;
		IF n > 0 THEN
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
