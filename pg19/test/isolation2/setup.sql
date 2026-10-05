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
-- gp_core's own the database has already: a cluster's coordinator makes it
-- in every database a superuser makes (gp_ddl.c, create_core_extension()).
-- The planner plans the setup, as the greenplum suite's: its statements are
-- the extensions' scripts', which ORCA would try one by one.
SET gp.optimizer = off;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS gp_core;
RESET client_min_messages;
CREATE EXTENSION gp_orca;
CREATE EXTENSION gp_sql;
CREATE EXTENSION gp_ao;
CREATE EXTENSION gp_exttable;
CREATE EXTENSION gp_security;
CREATE EXTENSION gp_resource;
CREATE EXTENSION gp_matview;
CREATE EXTENSION gp_task;
CREATE EXTENSION gp_inject_fault;

--
-- PL/Python, in which some of Cloudberry's tests write helpers of their own
-- (recoverseg_from_file, dtm_recovery_on_standby), as Cloudberry's setup
-- makes it for its own.
--
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
-- pg_ctl(datadir, command, command_mode): stop, restart or promote the node
-- whose data directory that is, waiting for it, as Cloudberry's does.  The
-- harness keeps each node's log beside its data directory.
--
CREATE FUNCTION pg_ctl(datadir text, command text, command_mode text DEFAULT 'immediate')
RETURNS text AS $$
BEGIN
	IF command = 'promote' THEN
		EXECUTE format('COPY (SELECT 1) TO PROGRAM %L',
					   format('@BINDIR@/pg_ctl -D %s -w -t 600 promote > /dev/null 2>&1',
							  datadir));
		RETURN 'OK';
	END IF;
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

--
-- M7's: what Cloudberry's tests of the coordinator's standby ask of it.
--

-- wait_until_standby_in_state(state): Cloudberry's, as it is -- the node's
-- own WAL sender, on the coordinator its standby's.
CREATE FUNCTION wait_until_standby_in_state(targetstate text)
RETURNS text AS $$
DECLARE
	replstate text;
	i int;
BEGIN
	i := 0;
	WHILE i < 1200 LOOP
		SELECT state INTO replstate FROM pg_stat_replication;
		IF replstate = targetstate THEN
			RETURN replstate;
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM pg_stat_clear_snapshot();
		i := i + 1;
	END LOOP;
	RETURN replstate;
END;
$$ LANGUAGE plpgsql;

--
-- The rest of Cloudberry's helpers that the schedule's tests call, as its
-- setup makes them, but for pg_basebackup().
--

-- pg_basebackup(host, dbid, port, create_slot, slotname, datadir,
-- force_overwrite, xlog_method): a copy of the node at that address, made as
-- Cloudberry's helper makes it with its pg_basebackup's own options --
-- --target-gp-dbid, each tablespace laid out under the copy's dbid, and
-- --force-overwrite -- which gpMgmt's gpsegbasebackup.py gives PostgreSQL
-- 19's, as the port's tools take a copy.  It answers what the tool said,
-- less its dots, as Cloudberry's does.
CREATE FUNCTION pg_basebackup(host text, dbid int, port bigint, create_slot boolean,
							  slotname text, datadir text, force_overwrite boolean,
							  xlog_method text)
RETURNS text AS $$
    import os
    import subprocess
    gphome = os.path.dirname('@BINDIR@')
    cmd = [os.path.join(gphome, 'sbin', 'gpsegbasebackup.py'), '--checkpoint=fast',
           '-h', host, '-p', str(port), '-R', '-D', datadir, '--target-gp-dbid', str(dbid)]
    if create_slot:
        cmd.append('--create-slot')
    if slotname is not None:
        cmd += ['--slot', slotname]
    if force_overwrite:
        cmd.append('--force-overwrite')
    if xlog_method in ('stream', 'fetch'):
        cmd += ['--wal-method', xlog_method]
    else:
        plpy.error('invalid xlog method')
    cmd.append('--no-verify-checksums')
    env = dict(os.environ)
    # PGAPPNAME unset, so that pg_stat_replication's application_name is the tool's
    env.pop('PGAPPNAME', None)
    env['PYTHONPATH'] = os.path.join(gphome, 'lib', 'python')
    env['PATH'] = '@BINDIR@' + os.pathsep + env.get('PATH', '')
    try:
        results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, env=env).replace(b'.', b'').decode()
    except subprocess.CalledProcessError as e:
        results = str(e) + "\ncommand output: " + (e.output.decode())
    return results
$$ LANGUAGE plpython3u;

CREATE FUNCTION count_of_items_in_directory(user_path text) RETURNS text AS $$
    import subprocess
    cmd = 'ls {user_path}'.format(user_path=user_path)
    results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
    return len([result for result in results.splitlines() if result != ''])
$$ LANGUAGE plpython3u;

CREATE FUNCTION count_of_items_in_database_directory(user_path text, database_oid oid) RETURNS int AS $$
    import os
    import subprocess
    directory = os.path.join(user_path, str(database_oid))
    cmd = 'ls ' + directory
    results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
    return len([result for result in results.splitlines() if result != ''])
$$ LANGUAGE plpython3u;

CREATE FUNCTION validate_tablespace_symlink(datadir text, tablespacedir text, dbid int, tablespace_oid oid) RETURNS boolean AS $$
    import os
    return os.readlink('%s/pg_tblspc/%d' % (datadir, tablespace_oid)) == ('%s/%d' % (tablespacedir, dbid))
$$ LANGUAGE plpython3u;

-- wait_till_master_shutsdown(): a minute, in which a coordinator that is to
-- PANIC goes down, as Cloudberry's waits.
CREATE FUNCTION wait_till_master_shutsdown()
RETURNS void AS $$
DECLARE
	i int;
BEGIN
	i := 0;
	WHILE i < 120 LOOP
		i := i + 1;
		PERFORM pg_sleep(.5);
	END LOOP;
END;
$$ LANGUAGE plpgsql;

-- wait_until_dead_tup_change_to(relid, n) and wait_until_vacuum_count_change_to(relid, n):
-- until this node's statistics say so.
CREATE FUNCTION wait_until_dead_tup_change_to(relid oid, stat_val_expected bigint)
RETURNS text AS $$
DECLARE
	stat_val int;
	i int;
BEGIN
	i := 0;
	WHILE i < 1200 LOOP
		SELECT pg_stat_get_dead_tuples(relid) INTO stat_val;
		IF stat_val = stat_val_expected THEN
			RETURN 'OK';
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM pg_stat_clear_snapshot();
		i := i + 1;
	END LOOP;
	RETURN 'Fail';
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION wait_until_vacuum_count_change_to(relid oid, stat_val_expected bigint)
RETURNS text AS $$
DECLARE
	stat_val int;
	i int;
BEGIN
	i := 0;
	WHILE i < 1200 LOOP
		SELECT pg_stat_get_vacuum_count(relid) INTO stat_val;
		IF stat_val = stat_val_expected THEN
			RETURN 'OK';
		END IF;
		PERFORM pg_sleep(0.1);
		PERFORM pg_stat_clear_snapshot();
		i := i + 1;
	END LOOP;
	RETURN 'Fail';
END;
$$ LANGUAGE plpgsql;

-- nblocks(rel): the relation's blocks.
CREATE FUNCTION nblocks(rel regclass) RETURNS int AS $$
BEGIN
	RETURN pg_relation_size(rel) / current_setting('block_size')::int;
END;
$$ LANGUAGE plpgsql;

-- populate_pages(relname, value, upto): rows of that value into the table
-- until one lands past upto, in the blocks the session's inserts fill.
CREATE FUNCTION populate_pages(relname text, value int, upto tid) RETURNS void AS $$
DECLARE
	curtid tid;
BEGIN
	LOOP
		EXECUTE format('INSERT INTO %I VALUES($1) RETURNING ctid', relname) INTO curtid USING value;
		EXIT WHEN curtid > upto;
	END LOOP;
END;
$$ LANGUAGE plpgsql;
