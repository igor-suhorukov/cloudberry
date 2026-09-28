-- The port's own: CREATE DATABASE and DROP TABLESPACE on a cluster are parts
-- of the coordinator's distributed transaction, prepared on each segment
-- with a file of what their directories are to become, which the segment's
-- mirror replays a copy of, and ended by COMMIT PREPARED, as Cloudberry's
-- two-phase DDL is (gp_dirxact.c).  Here segment 0's primary goes down
-- between the phases of each, and is started again once the coordinator has
-- given up telling it: the coordinator's recovery process commits its parts
-- then, and its directories and its mirror's are as the commits leave them
-- -- the new database's made, the dropped tablespace's gone -- with no file
-- of a part left on either.

-- What segment 0's primary and mirror have: database db's directory under
-- base/, links under pg_tblspc/ to tablespaces that are no more, the
-- directory of the node's own under tablespace location loc, and files of
-- prepared parts.
CREATE FUNCTION dirxact_nodes(db name, loc text)
RETURNS TABLE (role "char", db_dir bool, dropped_links bigint, loc_dir bool, part_files bigint) AS $$
	SELECT c.role,
	       EXISTS (SELECT FROM pg_ls_dir(c.datadir || '/base') f
	                WHERE f = (SELECT oid::text FROM pg_database WHERE datname = db)),
	       (SELECT count(*) FROM pg_ls_dir(c.datadir || '/pg_tblspc') f
	         WHERE f::oid NOT IN (SELECT oid FROM pg_tablespace)),
	       EXISTS (SELECT FROM pg_ls_dir(loc, true, false) f WHERE f = c.dbid::text),
	       (SELECT count(*) FROM pg_ls_dir(c.datadir || '/gp_dirxact', true, false))
	  FROM gp_segment_configuration c
	 WHERE c.content = 0
	 ORDER BY c.role DESC
$$ LANGUAGE sql;

-- Until segment 0's primary has no file of a prepared part: the recovery
-- process has committed its parts, and done what their files asked.
CREATE FUNCTION dirxact_wait_parts() RETURNS bool AS $$
BEGIN	/* in func */
	FOR i IN 1..600 LOOP	/* in func */
		IF NOT EXISTS (SELECT FROM gp_segment_configuration c,
		                      pg_ls_dir(c.datadir || '/gp_dirxact', true, false) f
		                WHERE c.content = 0 AND c.role = 'p') THEN	/* in func */
			RETURN true;	/* in func */
		END IF;	/* in func */
		PERFORM pg_sleep(0.1);	/* in func */
	END LOOP;	/* in func */
	RETURN false;	/* in func */
END;	/* in func */
$$ LANGUAGE plpgsql;

-- No node has a directory of a database that is no more: the one
-- prepared_xact_deadlock_pg_rewind's CREATE DATABASE made, which the mirror
-- FTS promoted rolled back, among them.
SELECT force_mirrors_to_catch_up();
SELECT c.content, c.role, f AS directory FROM gp_segment_configuration c, pg_ls_dir(c.datadir || '/base') f WHERE f ~ '^[0-9]+$' AND f::oid NOT IN (SELECT oid FROM pg_database);

!\retcode rm -rf /tmp/dirxact_ts;
!\retcode mkdir -p /tmp/dirxact_ts;
CREATE TABLESPACE dirxact_ts LOCATION '/tmp/dirxact_ts';

-- FTS would fail segment 0 over while its primary is down.
SELECT gp_inject_fault_infinite('fts_probe', 'skip', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
SELECT gp_request_fts_probe_scan();

-- Both statements held after their commit records, their parts prepared.
SELECT gp_inject_fault_infinite('dtm_broadcast_commit_prepared', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1&: CREATE DATABASE dirxact_db;
SELECT gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
2&: DROP TABLESPACE dirxact_ts;
SELECT gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 2, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;

-- Segment 0's primary goes down, and the coordinator tells it for a while.
SELECT pg_ctl(datadir, 'stop') FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
1<:
2<:
SELECT * FROM dirxact_nodes('dirxact_db', '/tmp/dirxact_ts');

-- Started again, with its parts recovered from their PREPARE records; a
-- session of its own, whose gang reaches the primary as it is now.
SELECT pg_ctl_start(datadir, port) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
3: SELECT dirxact_wait_parts();
3: SELECT force_mirrors_to_catch_up();
3: SELECT * FROM dirxact_nodes('dirxact_db', '/tmp/dirxact_ts');
0U: SELECT datname FROM pg_database WHERE datname = 'dirxact_db';
0U: SELECT count(*) FROM pg_prepared_xacts;
0Uq:

3: SELECT gp_inject_fault('fts_probe', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = -1;
3: SELECT wait_until_all_segments_synchronized();
3: DROP DATABASE dirxact_db;
3: SELECT force_mirrors_to_catch_up();
3: SELECT c.content, c.role, f AS directory FROM gp_segment_configuration c, pg_ls_dir(c.datadir || '/base') f WHERE f ~ '^[0-9]+$' AND f::oid NOT IN (SELECT oid FROM pg_database);
3: DROP FUNCTION dirxact_nodes(name, text);
3: DROP FUNCTION dirxact_wait_parts();
