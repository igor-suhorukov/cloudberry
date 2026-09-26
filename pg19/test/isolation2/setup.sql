--
-- The port's setup for Cloudberry's isolation2 tests on a cluster, in place
-- of Cloudberry's own (src/test/isolation2/sql/setup.sql), which run after it.
--
-- What Cloudberry has built in, the port has as modules, preloaded on every
-- node, and as extensions, created here and by DDL dispatch on the segments.
-- Cloudberry's helpers are PL/Python; the ones the tests the manifest runs
-- call are here, in SQL: a node restarted with COPY ... TO PROGRAM, and a
-- lock waited for through each segment's own pg_locks; and Cloudberry's own
-- that are SQL already.
--
CREATE EXTENSION gp_core;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_resource;
CREATE EXTENSION gp_inject_fault;

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

--
-- wait_for_replication_replay(segid, retries): Cloudberry's, as it is.  The
-- port's gp_stat_replication is the node's own pg_stat_replication, which
-- on the coordinator is its standby's, content -1.
--
CREATE FUNCTION wait_for_replication_replay(segid int, retries int)
RETURNS bool AS $$
DECLARE
	i int;
	result bool;
BEGIN
	i := 0;
	-- Wait until the mirror/standby has replayed up to flush location
	LOOP
		SELECT flush_lsn = replay_lsn INTO result FROM gp_stat_replication WHERE gp_segment_id = segid;
		IF result THEN
			RETURN true;
		END IF;

		IF i >= retries THEN
			RETURN false;
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM pg_stat_clear_snapshot();
		i := i + 1;
	END LOOP;
END;
$$ LANGUAGE plpgsql;

--
-- M4's: what Cloudberry's FTS tests ask of a cluster with mirrors.
--

--
-- pg_ctl_start(datadir, port): start the node whose data directory that
-- is, as Cloudberry's does, and answer what pg_ctl said, less its dots.
-- The node's configuration has its port already; the harness's pg_ctl
-- writes pg_ctl's words beside the data directory, where this reads them.
--
CREATE FUNCTION pg_ctl_start(datadir text, port bigint)
RETURNS text AS $$
BEGIN
	EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
				   format('@BINDIR@/pg_ctl -l %s.log -D %s -o "-p %s" -w -t 600 start > %s.start 2>&1',
						  datadir, datadir, port, datadir));
	RETURN replace(pg_read_file(datadir || '.start'), '.', '');
END;
$$ LANGUAGE plpgsql;

-- Cloudberry's, as they are.
CREATE FUNCTION get_data_directory_for(segment_number int, segment_role text DEFAULT 'p')
RETURNS text AS $$
BEGIN
	RETURN (SELECT datadir FROM gp_segment_configuration
			 WHERE role = segment_role AND content = segment_number);
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION master() RETURNS SETOF gp_segment_configuration AS $$
	SELECT * FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
$$ LANGUAGE sql;

CREATE FUNCTION wait_until_segment_synchronized(segment_number int)
RETURNS text AS $$
BEGIN
	FOR i IN 1..6000 LOOP
		IF (SELECT count(*) = 0 FROM gp_segment_configuration
			 WHERE content = segment_number AND mode != 's') THEN
			RETURN 'OK';
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM gp_request_fts_probe_scan();
	END LOOP;
	RETURN 'Fail';
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION wait_until_all_segments_synchronized()
RETURNS text AS $$
BEGIN
	FOR i IN 1..6000 LOOP
		IF (SELECT count(*) = 0 FROM gp_segment_configuration
			 WHERE content != -1 AND mode != 's') THEN
			RETURN 'OK';
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM gp_request_fts_probe_scan();
	END LOOP;
	RETURN 'Fail';
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION wait_for_mirror_down(contentid smallint, timeout_sec integer)
RETURNS bool AS $$
DECLARE
	i int;
BEGIN
	i := 0;
	LOOP
		PERFORM gp_request_fts_probe_scan();
		IF (SELECT count(1) FROM gp_segment_configuration
			 WHERE role = 'm' AND content = $1 AND status = 'd') = 1 THEN
			RETURN true;
		END IF;
		IF i >= 2 * $2 THEN
			RETURN false;
		END IF;
		PERFORM pg_sleep(0.5);
		i := i + 1;
	END LOOP;
END;
$$ LANGUAGE plpgsql;
