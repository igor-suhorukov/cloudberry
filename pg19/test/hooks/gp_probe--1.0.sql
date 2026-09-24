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
CREATE FUNCTION gp_probe.arm_range(func regprocedure, printed text,
                                   alias boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_range' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.arm_lockmode(rel regclass, mode text) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_lockmode' LANGUAGE C STRICT;
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

-- O13 and the registry's members: a table access method of the probe's own,
-- heap underneath, with a TableAmExtRoutine registered for it.
CREATE FUNCTION gp_probe.am_handler(internal) RETURNS table_am_handler
  AS 'MODULE_PATHNAME', 'gp_probe_am_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD gp_probe_am TYPE TABLE HANDLER gp_probe.am_handler;
-- O14: the method's own option, and heap's fillfactor, as the relcache has them.
CREATE FUNCTION gp_probe.am_level(rel regclass) RETURNS int
  AS 'MODULE_PATHNAME', 'gp_probe_am_level' LANGUAGE C STRICT;
CREATE FUNCTION gp_probe.am_fillfactor(rel regclass) RETURNS int
  AS 'MODULE_PATHNAME', 'gp_probe_am_fillfactor' LANGUAGE C STRICT;
-- O15: what each scan of the method's tables that was given its plan node
-- said: its kind, table and the columns the node reads.
CREATE FUNCTION gp_probe.scan_log() RETURNS text
  AS 'MODULE_PATHNAME', 'gp_probe_scan_log' LANGUAGE C;
-- O16: make the method's own index fetch fail, so that a unique index's
-- probe that went through it would say so.
CREATE FUNCTION gp_probe.arm_fetch_fails(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_fetch_fails' LANGUAGE C STRICT;
-- O19: what the method says a table's main fork takes; -1 for its files.
CREATE FUNCTION gp_probe.arm_size(bytes bigint) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_size' LANGUAGE C STRICT;
-- O20: make the method's fetch of a row by its TID fail.
CREATE FUNCTION gp_probe.arm_rowfetch_fails(on_off boolean) RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_rowfetch_fails' LANGUAGE C STRICT;
-- O18: the runs of block numbers a table of the method has, as start and
-- length pairs.
CREATE FUNCTION gp_probe.arm_block_sequences(rel regclass, seqs bigint[])
  RETURNS void
  AS 'MODULE_PATHNAME', 'gp_probe_arm_block_sequences' LANGUAGE C STRICT;
