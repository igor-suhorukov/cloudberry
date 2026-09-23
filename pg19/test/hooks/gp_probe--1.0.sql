/* pg19/test/hooks/gp_probe--1.0.sql */

\echo Use "CREATE EXTENSION gp_probe" to load this file. \quit

-- What each hook did since the last reset.
CREATE FUNCTION gp_probe.reset() RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_reset' LANGUAGE C;
CREATE FUNCTION gp_probe.calls(event text) RETURNS bigint
  AS 'MODULE_PATHNAME', 'gp_probe_calls' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.detail(event text) RETURNS text
  AS 'MODULE_PATHNAME', 'gp_probe_detail' LANGUAGE C STRICT;

-- Arming.  Each says what the hook should do next, so a test drives one path
-- at a time and the others stay out of the way.
CREATE FUNCTION gp_probe.arm_new_oid(catalog regclass, want oid) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_new_oid' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_analyze(rel regclass, pages bigint, rows bigint)
  RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_analyze' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_star_filter(rel regclass, attnums smallint[])
  RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_star_filter' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_column(name text, func regprocedure) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_column' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_parser(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_parser' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_explain(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_explain' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_mdunlink(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_mdunlink' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_combocid(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_combocid' LANGUAGE C STRICT;

-- R2: one backend hands its XIDs, and its combo CID mapping, to another.
CREATE FUNCTION gp_probe.published_combocids() RETURNS bigint[]
  AS 'MODULE_PATHNAME', 'gp_probe_published_combocids' LANGUAGE C;
CREATE FUNCTION gp_probe.load_combocids(triples bigint[]) RETURNS int
  AS 'MODULE_PATHNAME', 'gp_probe_load_combocids' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.current_xids() RETURNS xid[]
  AS 'MODULE_PATHNAME', 'gp_probe_current_xids' LANGUAGE C;
CREATE FUNCTION gp_probe.adopt_xids(xids xid[]) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_adopt_xids' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.transaction_state() RETURNS bytea
  AS 'MODULE_PATHNAME', 'gp_probe_transaction_state' LANGUAGE C;

-- O27 and R3.
CREATE FUNCTION gp_probe.matview_maintenance(open_it boolean) RETURNS boolean
  AS 'MODULE_PATHNAME', 'gp_probe_matview_maintenance' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.matview_depth() RETURNS int
  AS 'MODULE_PATHNAME', 'gp_probe_matview_depth' LANGUAGE C;
CREATE FUNCTION gp_probe.matview_restore_depth(depth int) RETURNS int
  AS 'MODULE_PATHNAME', 'gp_probe_matview_restore_depth' LANGUAGE C STRICT;
-- Opens maintenance mode, fails, and restores the depth from PG_CATCH.
CREATE FUNCTION gp_probe.matview_apply_failing() RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_matview_apply_failing' LANGUAGE C;
CREATE FUNCTION gp_probe.syncrep_hold(on_off boolean) RETURNS boolean
  AS 'MODULE_PATHNAME', 'gp_probe_syncrep_hold' LANGUAGE C STRICT;
