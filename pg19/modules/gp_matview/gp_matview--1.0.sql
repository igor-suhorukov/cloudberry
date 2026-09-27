/* pg19/modules/gp_matview/gp_matview--1.0.sql */

\echo Use "CREATE EXTENSION gp_matview" to load this file. \quit

/*
 * The trigger functions that keep an incrementally maintained materialized
 * view up to date.  They are not called directly: CREATE MATERIALIZED VIEW
 * ... WITH (gp.incremental) puts the triggers on the base tables.
 */
CREATE FUNCTION gp_matview.ivm_immediate_before()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_ivm_immediate_before'
LANGUAGE C;

CREATE FUNCTION gp_matview.ivm_immediate_maintenance()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_ivm_immediate_maintenance'
LANGUAGE C;

REVOKE ALL ON FUNCTION gp_matview.ivm_immediate_before() FROM PUBLIC;
REVOKE ALL ON FUNCTION gp_matview.ivm_immediate_maintenance() FROM PUBLIC;

/*
 * Was this row of a base table there before the statement ran?  A view over
 * more than one table is maintained by reading the tables the statement
 * changed as they were before it, and this is what says which rows those
 * were.  It is called from a subquery the module builds, in the statement's
 * own session, and errors unless that session is maintaining the view it is
 * asked about -- so it is left executable, because the maintenance runs as
 * whoever wrote to the base table.
 */
CREATE FUNCTION gp_matview.visible_in_prestate(tableoid oid, ctid tid, matview oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'gp_ivm_visible_in_prestate'
LANGUAGE C STRICT;

/*
 * How views have been kept up to date since the counters were reset. The
 * contents of a view do not say whether a delta or a whole recomputation
 * produced them, and the tests need to know.
 */
CREATE FUNCTION gp_matview.stats_reset() RETURNS void
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_reset' LANGUAGE C;
CREATE FUNCTION gp_matview.applied_delta() RETURNS bigint
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_delta' LANGUAGE C;
CREATE FUNCTION gp_matview.recomputed() RETURNS bigint
  AS 'MODULE_PATHNAME', 'gp_ivm_stats_recompute' LANGUAGE C;

/*
 * Dynamic tables: a materialized view that refreshes on a schedule.
 *
 * Cloudberry marks one with pg_class.relisdynamic and keeps the schedule in
 * its task; here the "gp" label is both, so the schedule is what says a view
 * is dynamic.  This is Cloudberry's pg_get_dynamic_table_schedule.
 */
CREATE FUNCTION gp_matview.dynamic_schedule(matview regclass)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_dynamic_schedule'
LANGUAGE C STABLE STRICT;

COMMENT ON FUNCTION gp_matview.dynamic_schedule(regclass) IS
	'the schedule a dynamic table refreshes on, or NULL if it is not one';

CREATE VIEW gp_matview.dynamic_tables AS
	SELECT n.nspname AS schemaname,
		   c.relname AS matviewname,
		   pg_catalog.pg_get_userbyid(c.relowner) AS matviewowner,
		   s.schedule
	  FROM pg_catalog.pg_class c
	  JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
	  CROSS JOIN LATERAL gp_matview.dynamic_schedule(c.oid) AS s(schedule)
	 WHERE c.relkind = 'm' AND s.schedule IS NOT NULL;

COMMENT ON VIEW gp_matview.dynamic_tables IS
	'the materialized views that refresh on a schedule; Cloudberry calls this pg_dynamic_tables';

GRANT SELECT ON gp_matview.dynamic_tables TO PUBLIC;

/*
 * ---------------------------------------------------------------------------
 * Incremental views on a cluster (ivm_cluster.c)
 * ---------------------------------------------------------------------------
 *
 * What a cluster's coordinator calls on each segment to keep an incremental
 * view up to date: the view's triggers made there, what they kept taken, the
 * deltas brought and applied.  Each refuses any call but the coordinator's
 * own, the statement it sends on its own connection; they are executable by
 * everyone, and the schema usable by everyone, because the coordinator sends
 * them as whoever wrote the base table.
 */
GRANT USAGE ON SCHEMA gp_matview TO PUBLIC;

CREATE FUNCTION gp_matview.ivm_make_triggers(matview oid)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_ivm_make_triggers'
LANGUAGE C STRICT;

CREATE FUNCTION gp_matview.ivm_stash(matview oid,
	OUT relid oid, OUT old_rows bigint, OUT new_rows bigint, OUT truncated boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_ivm_stash'
LANGUAGE C STRICT;

-- not STRICT: the last argument is a NULL of the table's row type
CREATE FUNCTION gp_matview.ivm_take(matview oid, relid oid, old boolean, rowtype anyelement)
RETURNS SETOF anyelement
AS 'MODULE_PATHNAME', 'gp_ivm_take'
LANGUAGE C;

CREATE FUNCTION gp_matview.ivm_stage(matview oid, kind "char", rows text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_ivm_stage'
LANGUAGE C STRICT;

CREATE FUNCTION gp_matview.ivm_apply(matview oid, replace boolean)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_ivm_apply'
LANGUAGE C STRICT;

/*
 * ---------------------------------------------------------------------------
 * AQUMV: which views could answer a query, and whether each is up to date
 * (mvaux.c, aqumv.c)
 * ---------------------------------------------------------------------------
 *
 * Cloudberry keeps them in two catalogs of its own, gp_matview_aux and
 * gp_matview_tables.  Here they are three tables of the coordinator's --
 * a view registered, its base tables, and what was done to them since it
 * was refreshed, a row per kind -- and Cloudberry's two names are views over
 * them.  The status is read off the events: 'e' where there is an 'e', or an
 * 'i' and an 'r'; 'i' or 'r' where there is one; 'u' where there is none.
 * The tables are the module's, written as the bootstrap superuser; the views
 * are everyone's, as Cloudberry's catalogs are.
 */
CREATE TABLE gp_matview.matview_aux (
	mvoid oid PRIMARY KEY,
	has_foreign boolean NOT NULL
);

CREATE TABLE gp_matview.matview_aux_table (
	mvoid oid NOT NULL,
	relid oid NOT NULL,
	PRIMARY KEY (mvoid, relid)
);
CREATE INDEX matview_aux_table_relid ON gp_matview.matview_aux_table (relid);

CREATE TABLE gp_matview.matview_aux_event (
	mvoid oid NOT NULL,
	kind "char" NOT NULL
);
CREATE INDEX matview_aux_event_mvoid ON gp_matview.matview_aux_event (mvoid);

REVOKE ALL ON gp_matview.matview_aux, gp_matview.matview_aux_table,
	gp_matview.matview_aux_event FROM PUBLIC;

SET allow_system_table_mods = on;

CREATE VIEW pg_catalog.gp_matview_aux AS
	SELECT m.mvoid,
		   c.relname AS mvname,
		   m.has_foreign,
		   (SELECT CASE WHEN bool_or(e.kind = 'e') OR
							 (bool_or(e.kind = 'i') AND bool_or(e.kind = 'r')) THEN 'e'
						WHEN bool_or(e.kind = 'i') THEN 'i'
						WHEN bool_or(e.kind = 'r') THEN 'r'
						ELSE 'u' END
			  FROM gp_matview.matview_aux_event e WHERE e.mvoid = m.mvoid)::"char" AS datastatus,
		   r.ev_action AS view_query
	  FROM gp_matview.matview_aux m
	  JOIN pg_catalog.pg_class c ON c.oid = m.mvoid
	  JOIN pg_catalog.pg_rewrite r ON r.ev_class = m.mvoid AND r.rulename = '_RETURN';

CREATE VIEW pg_catalog.gp_matview_tables AS
	SELECT t.mvoid, t.relid FROM gp_matview.matview_aux_table t;

RESET allow_system_table_mods;

GRANT SELECT ON pg_catalog.gp_matview_aux, pg_catalog.gp_matview_tables TO PUBLIC;
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_matview_aux IS 'catalog';
SECURITY LABEL FOR gp ON VIEW pg_catalog.gp_matview_tables IS 'catalog';
