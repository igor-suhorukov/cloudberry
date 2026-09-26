/* pg19/modules/gp_task/gp_task--1.0.sql */

\echo Use "CREATE EXTENSION gp_task" to load this file. \quit

/*
 * Cloudberry keeps jobs and their history in the shared catalogs pg_task and
 * pg_task_run_history, so they are the same from every database.  An
 * extension cannot create a shared catalog, so these are ordinary tables and
 * they live in one database -- the one gp.task_database names, which is where
 * the scheduler looks.  That is how pg_cron keeps them too.  A task written
 * in any other database is written here, by the procedures below.
 */

CREATE SEQUENCE gp_task.job_jobid_seq;
CREATE SEQUENCE gp_task.run_history_runid_seq;

CREATE TABLE gp_task.job (
	jobid		bigint PRIMARY KEY DEFAULT pg_catalog.nextval('gp_task.job_jobid_seq'),
	jobname		text NOT NULL,
	schedule	text NOT NULL,
	command		text NOT NULL,
	database	text NOT NULL,
	username	text NOT NULL,
	active		boolean NOT NULL DEFAULT true,
	UNIQUE (jobname, username)
);

CREATE TABLE gp_task.run_history (
	runid		bigint PRIMARY KEY,
	jobid		bigint NOT NULL,
	job_pid		integer,
	database	text,
	username	text,
	command		text,
	status		text,
	return_message text,
	start_time	timestamptz,
	end_time	timestamptz
);

CREATE INDEX run_history_jobid_index ON gp_task.run_history (jobid);

/*
 * pg_dump writes the jobs, but a dynamic table's: its name has the view's
 * OID in it, and the view's label, restored, makes the job again under the
 * view's new one (gp_matview's dynamic.c).
 */
SELECT pg_catalog.pg_extension_config_dump('gp_task.job',
	'WHERE jobname OPERATOR(pg_catalog.!~) ''^gp_dynamic_table_refresh_[0-9]+$''');
SELECT pg_catalog.pg_extension_config_dump('gp_task.job_jobid_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gp_task.run_history_runid_seq', '');

/*
 * A job's schedule, and anything else of it, is read by the scheduler once a
 * minute, and at once when it is written: this trigger tells it, as pg_cron's
 * cron.job_cache_invalidate does, so that a job that runs by the second
 * begins within one.
 */
CREATE FUNCTION gp_task.job_changed()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_task_job_changed'
LANGUAGE C;

CREATE TRIGGER job_changed
	AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON gp_task.job
	FOR EACH STATEMENT EXECUTE FUNCTION gp_task.job_changed();

/*
 * The schedules are Cloudberry's: cron's five fields, read by Cloudberry's
 * own cron parser, or an interval of 1 to 59 seconds, as in "30 seconds".  A
 * schedule that is neither is refused here, when the task is written, rather
 * than logged once a minute afterwards.
 */
CREATE FUNCTION gp_task.validate_schedule(schedule text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_task_validate_schedule'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_task.validate_schedule(text) IS
	'raise unless this is a schedule the task scheduler can read';

/*
 * CREATE TASK, ALTER TASK and DROP TASK become these: CALL gp_task.create_task
 * and the rest, which O26's rewrite of Cloudberry's statements writes, and
 * which answer as a DDL statement does, with a command tag and no row.  They
 * are procedures for that reason; a job's id is in gp_task.job.
 *
 * Cloudberry's messages where it gives one: IF NOT EXISTS of a task that is
 * there, and IF EXISTS of one that is not, each say so and do nothing.
 *
 * Called in a database other than gp.task_database, where the scheduler
 * reads its jobs, each is called there instead, with the same arguments, as
 * the transaction commits (gp_task.forward): what it says, it says then.
 */
CREATE FUNCTION gp_task.forward(procedure text, args text[])
RETURNS void
AS 'MODULE_PATHNAME', 'gp_task_forward'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_task.forward(text, text[]) IS
	'call one of gp_task''s procedures in gp.task_database, as this transaction commits';

CREATE PROCEDURE gp_task.create_task(jobname text,
									 schedule text,
									 command text,
									 database text DEFAULT pg_catalog.current_database(),
									 /* a keyword, not a function, so it takes no schema */
									 username text DEFAULT CURRENT_USER,
									 if_not_exists boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
BEGIN
	PERFORM gp_task.validate_schedule(schedule);

	IF pg_catalog.current_database() <> pg_catalog.current_setting('gp.task_database') THEN
		PERFORM gp_task.forward('create_task',
								ARRAY[jobname, schedule, command, database, username,
									  if_not_exists::text]);
		RETURN;
	END IF;

	IF if_not_exists AND EXISTS (SELECT 1 FROM gp_task.job j
								  WHERE j.jobname = create_task.jobname
									AND j.username = create_task.username) THEN
		RAISE NOTICE 'task "%" already exists, skipping', jobname;
		RETURN;
	END IF;

	INSERT INTO gp_task.job (jobname, schedule, command, database, username)
		 VALUES (jobname, schedule, command, database, username);
END;
$$;

COMMENT ON PROCEDURE gp_task.create_task(text, text, text, text, text, boolean) IS
	'schedule a command; what Cloudberry writes as CREATE TASK';

/*
 * Every argument but the name may be left out, and what is left out is left
 * alone -- which is what ALTER TASK does with the clauses it is not given.
 */
CREATE PROCEDURE gp_task.alter_task(jobname text,
									schedule text DEFAULT NULL,
									command text DEFAULT NULL,
									database text DEFAULT NULL,
									username text DEFAULT NULL,
									active boolean DEFAULT NULL,
									missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	found_id bigint;
BEGIN
	IF schedule IS NOT NULL THEN
		PERFORM gp_task.validate_schedule(schedule);
	END IF;

	IF pg_catalog.current_database() <> pg_catalog.current_setting('gp.task_database') THEN
		PERFORM gp_task.forward('alter_task',
								ARRAY[jobname, schedule, command, database, username,
									  active::text, missing_ok::text]);
		RETURN;
	END IF;

	UPDATE gp_task.job j
	   SET schedule = coalesce(alter_task.schedule, j.schedule),
		   command  = coalesce(alter_task.command,  j.command),
		   database = coalesce(alter_task.database, j.database),
		   username = coalesce(alter_task.username, j.username),
		   active   = coalesce(alter_task.active,   j.active)
	 WHERE j.jobname = alter_task.jobname
	RETURNING j.jobid INTO found_id;

	IF found_id IS NULL THEN
		IF missing_ok THEN
			RAISE NOTICE 'task "%" does not exist, skipping', jobname;
			RETURN;
		END IF;
		RAISE EXCEPTION 'task "%" does not exist', jobname
			USING ERRCODE = 'undefined_object';
	END IF;
END;
$$;

COMMENT ON PROCEDURE gp_task.alter_task(text, text, text, text, text, boolean, boolean) IS
	'change a scheduled command; what Cloudberry writes as ALTER TASK';

/*
 * DROP TASK a, b is one statement, so it is one CALL, over all of them: one
 * that is not there takes the others back with it, unless missing_ok says it
 * may be missing -- which gp_matview asks for of every materialized view it
 * sees dropped, so it says nothing.
 */
CREATE PROCEDURE gp_task.drop_task(jobnames text[], missing_ok boolean DEFAULT false)
LANGUAGE plpgsql
AS $$
DECLARE
	one text;
	found_id bigint;
BEGIN
	IF pg_catalog.current_database() <> pg_catalog.current_setting('gp.task_database') THEN
		PERFORM gp_task.forward('drop_task', ARRAY[jobnames::text, missing_ok::text]);
		RETURN;
	END IF;

	FOREACH one IN ARRAY jobnames LOOP
		found_id := NULL;
		DELETE FROM gp_task.job j
			  WHERE j.jobname = one
		  RETURNING j.jobid INTO found_id;

		IF found_id IS NULL THEN
			IF missing_ok THEN
				CONTINUE;
			END IF;
			RAISE EXCEPTION 'task "%" does not exist', one
				USING ERRCODE = 'undefined_object';
		END IF;

		DELETE FROM gp_task.run_history WHERE jobid = found_id;
	END LOOP;
END;
$$;

COMMENT ON PROCEDURE gp_task.drop_task(text[], boolean) IS
	'unschedule commands; what Cloudberry writes as DROP TASK';

/*
 * The tables hold other people's commands, so a user reads only the jobs and
 * runs of their own -- a superuser every one -- as pg_cron's policy on its
 * job table has it; only the scheduler and the procedures above write them.
 * Cloudberry's own catalogs are readable by everybody, which its
 * pg_task_run_history shares with them.
 */
ALTER TABLE gp_task.job ENABLE ROW LEVEL SECURITY;
ALTER TABLE gp_task.run_history ENABLE ROW LEVEL SECURITY;
CREATE POLICY job_owner ON gp_task.job FOR SELECT
	USING (username = CURRENT_USER);
CREATE POLICY run_owner ON gp_task.run_history FOR SELECT
	USING (username = CURRENT_USER);

GRANT USAGE ON SCHEMA gp_task TO PUBLIC;
REVOKE ALL ON gp_task.job FROM PUBLIC;
REVOKE ALL ON gp_task.run_history FROM PUBLIC;
GRANT SELECT ON gp_task.job, gp_task.run_history TO PUBLIC;
REVOKE ALL ON FUNCTION gp_task.validate_schedule(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_task.job_changed() FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_task.forward(text, text[]) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.create_task(text, text, text, text, text, boolean) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.alter_task(text, text, text, text, text, boolean, boolean) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.drop_task(text[], boolean) FROM PUBLIC;

/*
 * Cloudberry's pg_task and pg_task_run_history are shared catalogs, the same
 * from every database.  Here they are views by their names and columns, in
 * pg_catalog as Cloudberry's are, of the tables in gp.task_database, which
 * are read there from whichever database asks: through gp_core's loopback,
 * as the current user, so that the policies above apply to them, or by SPI
 * in that database itself.  A job has no node of its own: it runs in a
 * background worker of this server, the node Cloudberry's nodename and
 * nodeport name, whose nodename is task_host_addr, 127.0.0.1 by default.
 */
CREATE FUNCTION gp_task.job_rows(OUT jobid bigint, OUT schedule text,
								 OUT command text, OUT database text,
								 OUT username text, OUT active boolean,
								 OUT jobname text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_task_job_rows'
LANGUAGE C;

CREATE FUNCTION gp_task.run_rows(OUT runid bigint, OUT jobid bigint,
								 OUT job_pid integer, OUT database text,
								 OUT username text, OUT command text,
								 OUT status text, OUT return_message text,
								 OUT start_time timestamptz,
								 OUT end_time timestamptz)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_task_run_rows'
LANGUAGE C;

SET allow_system_table_mods = on;

CREATE VIEW pg_catalog.pg_task AS
	SELECT j.jobid,
		   pg_catalog.current_setting('port')::integer AS nodeport,
		   j.active, j.schedule, j.command,
		   '127.0.0.1'::text AS nodename,
		   j.database, j.username, j.jobname
	  FROM gp_task.job_rows() j;

CREATE VIEW pg_catalog.pg_task_run_history AS
	SELECT r.runid, r.jobid, r.job_pid, r.start_time, r.end_time,
		   r.database, r.username, r.command, r.status, r.return_message
	  FROM gp_task.run_rows() r;

RESET allow_system_table_mods;

GRANT SELECT ON pg_catalog.pg_task, pg_catalog.pg_task_run_history TO PUBLIC;
