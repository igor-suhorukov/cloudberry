/* pg19/modules/gp_resource/gp_resource--1.0.sql */

\echo Use "CREATE EXTENSION gp_resource" to load this file. \quit

/******************************************************************************
 * Resource queues and resource groups
 *
 * Their definitions are one JSON label each on a NOLOGIN role of their own,
 * gp_resource_queues and gp_resource_groups, as tag definitions are on
 * gp_tag_definitions: a role's label is the cluster's, as Cloudberry's shared
 * catalogs are, and pg_dumpall writes it.  Which queue and group a role is in
 * are keys of the role's "gp" label.  See resdefs.c.
 *****************************************************************************/

GRANT USAGE ON SCHEMA gp_resource TO PUBLIC;

DO $$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_roles
					WHERE rolname = 'gp_resource_queues') THEN
		CREATE ROLE gp_resource_queues NOLOGIN;
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_roles
					WHERE rolname = 'gp_resource_groups') THEN
		CREATE ROLE gp_resource_groups NOLOGIN;
	END IF;
END
$$;

/* Cloudberry's predefined role whose members may manage groups (resdefs.c) */
CREATE FUNCTION gp_resource.make_manage_role() RETURNS void
AS 'MODULE_PATHNAME', 'gp_resource_make_manage_role'
LANGUAGE C VOLATILE STRICT;
SELECT gp_resource.make_manage_role();

COMMENT ON ROLE gp_resource_queues IS
	'carries the resource queues, as its shared "gp_resource" security label; Cloudberry keeps these in the shared catalogs pg_resqueue and pg_resqueuecapability';
COMMENT ON ROLE gp_resource_groups IS
	'carries the resource groups, as its shared "gp_resource" security label; Cloudberry keeps these in the shared catalogs pg_resgroup and pg_resgroupcapability';

-----------------------------------------------------------------------------
-- The statements
--
-- Procedures, because each is what a statement of Cloudberry's becomes: O26
-- writes CREATE RESOURCE QUEUE as CALL gp_resource.create_resource_queue(...),
-- which answers as a DDL statement does, with a command tag and no row.  The
-- options are Cloudberry's, in its grammar's order (resdefs.c reads them).
-----------------------------------------------------------------------------

CREATE PROCEDURE gp_resource.create_resource_queue(queue name, options text[])
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_create_queue';
CREATE PROCEDURE gp_resource.alter_resource_queue(queue name, options text[])
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_alter_queue';
CREATE PROCEDURE gp_resource.drop_resource_queue(queue name)
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_drop_queue';
CREATE PROCEDURE gp_resource.comment_on_resource_queue(queue name, comment text)
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_comment_queue';

CREATE PROCEDURE gp_resource.create_resource_group(grp name, options text[])
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_create_group';
CREATE PROCEDURE gp_resource.alter_resource_group(grp name, options text[])
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_alter_group';
CREATE PROCEDURE gp_resource.drop_resource_group(grp name)
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_drop_group';
CREATE PROCEDURE gp_resource.comment_on_resource_group(grp name, comment text)
LANGUAGE C AS 'MODULE_PATHNAME', 'gp_resource_comment_group';

COMMENT ON PROCEDURE gp_resource.create_resource_queue(name, text[]) IS
	'define a resource queue; what Cloudberry writes as CREATE RESOURCE QUEUE';
COMMENT ON PROCEDURE gp_resource.create_resource_group(name, text[]) IS
	'define a resource group; what Cloudberry writes as CREATE RESOURCE GROUP';

/* A queue's and a group's comment, the definition's own */
CREATE FUNCTION gp_resource.queue_description(oid) RETURNS text
AS 'MODULE_PATHNAME', 'gp_resource_queue_comment'
LANGUAGE C STABLE STRICT;
CREATE FUNCTION gp_resource.group_description(oid) RETURNS text
AS 'MODULE_PATHNAME', 'gp_resource_group_comment'
LANGUAGE C STABLE STRICT;

/*
 * rolresqueue and rolresgroup of a row of pg_roles or pg_authid: what O10
 * makes the names of Cloudberry's columns a call of (gp_resource.c).
 */
CREATE FUNCTION gp_resource.rolresqueue(pg_catalog.pg_roles) RETURNS oid
AS 'MODULE_PATHNAME', 'gp_resource_role_queue'
LANGUAGE C STABLE STRICT;
CREATE FUNCTION gp_resource.rolresqueue(pg_catalog.pg_authid) RETURNS oid
AS 'MODULE_PATHNAME', 'gp_resource_role_queue'
LANGUAGE C STABLE STRICT;
CREATE FUNCTION gp_resource.rolresgroup(pg_catalog.pg_roles) RETURNS oid
AS 'MODULE_PATHNAME', 'gp_resource_role_group'
LANGUAGE C STABLE STRICT;
CREATE FUNCTION gp_resource.rolresgroup(pg_catalog.pg_authid) RETURNS oid
AS 'MODULE_PATHNAME', 'gp_resource_role_group'
LANGUAGE C STABLE STRICT;

/*
 * The backends that hold a queue's slot or wait for one, one row a backend
 * and queue as Cloudberry's pg_locks has its queues' locks; gp_toolkit's
 * views of the queues read these rows where Cloudberry's read pg_locks.
 */
CREATE FUNCTION gp_resource.resqueue_locks(OUT pid int4, OUT queueid oid,
										   OUT granted boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resqueue_locks'
LANGUAGE C VOLATILE STRICT;

/******************************************************************************
 * Cloudberry's catalogs, by its names, where its are: pg_catalog.  A script's
 * setting lasts as long as the script (gp_core's does the same).
 *****************************************************************************/

SET allow_system_table_mods = on;

CREATE FUNCTION pg_catalog.gp_resource_resqueue(
	OUT oid oid, OUT rsqname name, OUT rsqcountlimit float4,
	OUT rsqcostlimit float4, OUT rsqovercommit boolean,
	OUT rsqignorecostlimit float4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resqueue'
LANGUAGE C STABLE STRICT;

CREATE VIEW pg_catalog.pg_resqueue AS
	SELECT * FROM pg_catalog.gp_resource_resqueue();

CREATE FUNCTION pg_catalog.gp_resource_resqueuecapability(
	OUT resqueueid oid, OUT restypid int2, OUT ressetting text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resqueuecapability'
LANGUAGE C STABLE STRICT;

CREATE VIEW pg_catalog.pg_resqueuecapability AS
	SELECT * FROM pg_catalog.gp_resource_resqueuecapability();

CREATE FUNCTION pg_catalog.gp_resource_resourcetype(
	OUT oid oid, OUT resname name, OUT restypid int2,
	OUT resrequired boolean, OUT reshasdefault boolean,
	OUT reshasdisable boolean, OUT resdefaultsetting text,
	OUT resdisabledsetting text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resourcetype'
LANGUAGE C IMMUTABLE STRICT;

CREATE VIEW pg_catalog.pg_resourcetype AS
	SELECT * FROM pg_catalog.gp_resource_resourcetype();

CREATE FUNCTION pg_catalog.gp_resource_resgroup(
	OUT oid oid, OUT rsgname name, OUT parent oid)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resgroup'
LANGUAGE C STABLE STRICT;

CREATE VIEW pg_catalog.pg_resgroup AS
	SELECT * FROM pg_catalog.gp_resource_resgroup();

CREATE FUNCTION pg_catalog.gp_resource_resgroupcapability(
	OUT resgroupid oid, OUT reslimittype int2, OUT value text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resgroupcapability'
LANGUAGE C STABLE STRICT;

CREATE VIEW pg_catalog.pg_resgroupcapability AS
	SELECT * FROM pg_catalog.gp_resource_resgroupcapability();

/* The queues' state, Cloudberry's functions and views of it */
CREATE FUNCTION pg_catalog.pg_resqueue_status()
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resqueue_status'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_resqueue_status_kv()
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_resqueue_status_kv'
LANGUAGE C VOLATILE STRICT;

CREATE VIEW pg_catalog.pg_resqueue_status AS
	SELECT
			q.rsqname,
			q.rsqcountlimit,
			s.queuecountvalue AS rsqcountvalue,
			q.rsqcostlimit,
			s.queuecostvalue AS rsqcostvalue,
			s.queuewaiters AS rsqwaiters,
			s.queueholders AS rsqholders
	FROM pg_catalog.pg_resqueue AS q
			INNER JOIN pg_catalog.pg_resqueue_status() AS s
			(queueid oid,
			 queuecountvalue float4,
			 queuecostvalue float4,
			 queuewaiters int4,
			 queueholders int4)
			ON (s.queueid = q.oid);

CREATE VIEW pg_catalog.pg_resqueue_attributes AS
SELECT rsqname, 'active_statements' AS resname,
rsqcountlimit::text AS ressetting,
1 AS restypid FROM pg_catalog.pg_resqueue
UNION
SELECT rsqname, 'max_cost' AS resname,
rsqcostlimit::text AS ressetting,
2 AS restypid FROM pg_catalog.pg_resqueue
UNION
SELECT rsqname, 'cost_overcommit' AS resname,
case when rsqovercommit then '1'
else '0' end AS ressetting,
4 AS restypid FROM pg_catalog.pg_resqueue
UNION
SELECT rsqname, 'min_cost' AS resname,
rsqignorecostlimit::text AS ressetting,
3 AS restypid FROM pg_catalog.pg_resqueue
UNION
SELECT rq.rsqname , rt.resname, rc.ressetting,
rt.restypid AS restypid FROM
pg_catalog.pg_resqueue rq, pg_catalog.pg_resourcetype rt,
pg_catalog.pg_resqueuecapability rc WHERE
rq.oid=rc.resqueueid AND rc.restypid = rt.restypid
ORDER BY rsqname, restypid;

/* Cloudberry's statistics of the queues, a statistics kind of this module */
CREATE FUNCTION pg_catalog.pg_stat_get_resqueue_stats(queueid oid,
	OUT queries_submitted int8, OUT queries_admitted int8,
	OUT queries_rejected int8, OUT queries_completed int8,
	OUT elapsed_wait_secs int8, OUT max_wait_secs int8,
	OUT elapsed_exec_secs int8, OUT max_exec_secs int8,
	OUT total_cost int8, OUT total_memory_kb int8,
	OUT stat_reset_timestamp timestamptz, OUT have_stats boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_resource_resqueue_stats'
LANGUAGE C STABLE STRICT;

CREATE VIEW pg_catalog.pg_stat_resqueues AS
	SELECT
		q.oid							AS queueid,
		q.rsqname						AS queuename,
		s.queries_submitted,
		s.queries_admitted,
		s.queries_rejected,
		s.queries_completed,
		s.elapsed_wait_secs				AS total_wait_time_secs,
		s.max_wait_secs,
		s.elapsed_exec_secs				AS total_exec_time_secs,
		s.max_exec_secs,
		s.total_cost,
		s.total_memory_kb,
		s.stat_reset_timestamp
	FROM pg_catalog.pg_resqueue AS q,
		 pg_catalog.pg_stat_get_resqueue_stats(q.oid) AS s;

GRANT SELECT ON pg_catalog.pg_resqueue, pg_catalog.pg_resqueuecapability,
	pg_catalog.pg_resourcetype, pg_catalog.pg_resgroup,
	pg_catalog.pg_resgroupcapability, pg_catalog.pg_resqueue_status,
	pg_catalog.pg_resqueue_attributes, pg_catalog.pg_stat_resqueues
	TO PUBLIC;

SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_resqueue IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_resqueuecapability IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_resourcetype IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_resgroup IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.pg_resgroupcapability IS 'catalog';

RESET allow_system_table_mods;

/******************************************************************************
 * gp_toolkit's views of the queues, in Cloudberry's shapes; gp_core's script
 * makes the schema.  Where Cloudberry's read pg_locks, these read
 * gp_resource.resqueue_locks(), in pg_locks' columns.
 *****************************************************************************/

CREATE VIEW gp_toolkit.gp_resq_activity
AS
	SELECT
		psa.pid as resqprocpid,
		psa.usename as resqrole,
		resq.resqoid,
		resq.rsqname as resqname,
		psa.query_start as resqstart,
		CASE
			WHEN resqgranted = 'f' THEN 'waiting' ELSE 'running'
		END as resqstatus
	FROM
		pg_catalog.pg_stat_activity psa
	JOIN
	(
		SELECT
			pgrq.oid as resqoid,
			pgrq.rsqname,
			pgl.pid as resqprocid,
			pgl.granted as resqgranted
		FROM
			pg_catalog.pg_resqueue pgrq,
			gp_resource.resqueue_locks() pgl
		WHERE
			pgl.queueid = pgrq.oid
	) as resq
	ON resqprocid = pid
	WHERE query != '<IDLE>'
	ORDER BY resqstart;

CREATE VIEW gp_toolkit.gp_resq_activity_by_queue
AS
	SELECT
		resqoid,
		resqname,
		MAX(resqstart) as resqlast,
		resqstatus,
		COUNT(*) as resqtotal
	FROM
		gp_toolkit.gp_resq_activity
	GROUP BY
		resqoid, resqname, resqstatus
	ORDER BY resqoid, resqlast;

CREATE VIEW gp_toolkit.gp_resq_role
AS
	SELECT
		pgr.rolname  AS rrrolname,
		pgrq.rsqname AS rrrsqname
	FROM pg_catalog.pg_roles pgr

	LEFT JOIN pg_catalog.pg_resqueue pgrq
	ON (gp_resource.rolresqueue(pgr) = pgrq.oid);

/*
 * The statements this node's backends run, and what their queues' PRIORITY
 * makes them weigh: Cloudberry's backoff entries (resqueue.c), recorded and
 * reported, and slowing nobody.  A backend's command count is its own
 * count of the statements it has begun, a segment's too, where Cloudberry's
 * is the coordinator's, which the dispatch carries; so Cloudberry's
 * gp_adjust_priority(), which finds a statement by the two, is not here.
 */
CREATE FUNCTION pg_catalog.gp_list_backend_priorities() RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_backend_priorities'
LANGUAGE C VOLATILE ROWS 1000;
COMMENT ON FUNCTION pg_catalog.gp_list_backend_priorities() IS
	'list priorities of backends';

CREATE VIEW gp_toolkit.gp_resq_priority_backend
AS
	SELECT
		session_id as rqpsession,
		command_count as rqpcommand,
		priority as rqppriority,
		weight as rqpweight
	FROM
		gp_list_backend_priorities()
			AS L(session_id int, command_count int, priority text, weight int);

CREATE VIEW gp_toolkit.gp_resq_priority_statement
AS
	SELECT
		psa.datname AS rqpdatname,
		psa.usename AS rqpusename,
		rpb.rqpsession,
		rpb.rqpcommand,
		rpb.rqppriority,
		rpb.rqpweight,
		psa.query AS rqpquery
	FROM
		gp_toolkit.gp_resq_priority_backend rpb
		JOIN pg_stat_activity psa ON (rpb.rqpsession = psa.sess_id)
	WHERE psa.query != '<IDLE>';

CREATE VIEW gp_toolkit.gp_locks_on_resqueue
AS
	SELECT
		pgsa.usename      AS lorusename,
		pgrq.rsqname      AS lorrsqname,
		'resource queue'::text AS lorlocktype,
		pgl.queueid       AS lorobjid,
		NULL::xid         AS lortransaction,
		pgl.pid           AS lorpid,
		'ExclusiveLock'::text AS lormode,
		pgl.granted       AS lorgranted,
		pgsa.wait_event   AS lorwaitevent,
		pgsa.wait_event_type AS lorwaiteventtype
	FROM pg_catalog.pg_stat_activity pgsa

	JOIN gp_resource.resqueue_locks() pgl
	ON (pgsa.pid = pgl.pid)

	JOIN pg_catalog.pg_resqueue pgrq
	ON (pgl.queueid = pgrq.oid);

CREATE VIEW gp_toolkit.gp_resqueue_status
AS
	SELECT
		q.oid as queueid,
		q.rsqname as rsqname,
		t1.value::int as rsqcountlimit,
		t2.value::int as rsqcountvalue,
		t3.value::real as rsqcostlimit,
		t4.value::real as rsqcostvalue,
		t5.value::real as rsqmemorylimit,
		t6.value::real as rsqmemoryvalue,
		t7.value::int as rsqwaiters,
		t8.value::int as rsqholders
	FROM
		pg_catalog.pg_resqueue q,
		pg_catalog.pg_resqueue_status_kv() t1 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t2 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t3 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t4 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t5 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t6 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t7 (queueid oid, key text, value text),
		pg_catalog.pg_resqueue_status_kv() t8 (queueid oid, key text, value text)
	WHERE
		q.oid = t1.queueid
		AND t1.queueid = t2.queueid
		AND t2.queueid = t3.queueid
		AND t3.queueid = t4.queueid
		AND t4.queueid = t5.queueid
		AND t5.queueid = t6.queueid
		AND t6.queueid = t7.queueid
		AND t7.queueid = t8.queueid
		AND t1.key = 'rsqcountlimit'
		AND t2.key = 'rsqcountvalue'
		AND t3.key = 'rsqcostlimit'
		AND t4.key = 'rsqcostvalue'
		AND t5.key = 'rsqmemorylimit'
		AND t6.key = 'rsqmemoryvalue'
		AND t7.key = 'rsqwaiters'
		AND t8.key = 'rsqholders'
	;

GRANT SELECT ON gp_toolkit.gp_resq_activity, gp_toolkit.gp_resq_activity_by_queue,
	gp_toolkit.gp_resq_role, gp_toolkit.gp_resq_priority_backend,
	gp_toolkit.gp_resq_priority_statement, gp_toolkit.gp_locks_on_resqueue,
	gp_toolkit.gp_resqueue_status TO PUBLIC;

/******************************************************************************
 * Memory protection (memprot.c): each session's memory on a node, as
 * Cloudberry's gp_internal_tools gives it, and gp_toolkit's two functions of
 * it -- the coordinator's, and every segment's.
 *****************************************************************************/

CREATE FUNCTION gp_resource.session_state_memory_entries(
	OUT segid int, OUT sessionid int, OUT vmem_mb int, OUT runaway_status int,
	OUT qe_count int, OUT active_qe_count int, OUT dirty_qe_count int,
	OUT runaway_vmem_mb int, OUT runaway_command_cnt int,
	OUT idle_start timestamptz)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_session_state_memory_entries'
LANGUAGE C VOLATILE;

CREATE FUNCTION gp_toolkit.session_state_memory_entries_f_on_master()
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_resource_session_state_memory_entries'
LANGUAGE C VOLATILE;

/* PL/pgSQL, whose statements are planned as they run: where gp_sql rewrites gp_dist_random() */
CREATE FUNCTION gp_toolkit.session_state_memory_entries_f_on_segments()
RETURNS SETOF record
LANGUAGE plpgsql VOLATILE
AS $$
BEGIN
	RETURN QUERY SELECT (gp_resource.session_state_memory_entries()).*
				   FROM gp_dist_random('gp_id');
END
$$;

GRANT EXECUTE ON FUNCTION gp_toolkit.session_state_memory_entries_f_on_master(),
	gp_toolkit.session_state_memory_entries_f_on_segments() TO PUBLIC;
