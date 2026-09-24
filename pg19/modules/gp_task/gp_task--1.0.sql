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

SELECT pg_catalog.pg_extension_config_dump('gp_task.job', '');
SELECT pg_catalog.pg_extension_config_dump('gp_task.job_jobid_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gp_task.run_history_runid_seq', '');

/*
 * The schedules are read by Cloudberry's own cron parser.  A schedule it
 * cannot read is refused here, when the task is written, rather than logged
 * once a minute afterwards.
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
 * The tables hold other people's commands, so they are not readable by
 * everybody.  Cloudberry's own catalogs are readable by everybody, which its
 * pg_task_run_history shares with them.
 */
REVOKE ALL ON gp_task.job FROM PUBLIC;
REVOKE ALL ON gp_task.run_history FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_task.validate_schedule(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_task.forward(text, text[]) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.create_task(text, text, text, text, text, boolean) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.alter_task(text, text, text, text, text, boolean, boolean) FROM PUBLIC;
REVOKE ALL ON PROCEDURE gp_task.drop_task(text[], boolean) FROM PUBLIC;
