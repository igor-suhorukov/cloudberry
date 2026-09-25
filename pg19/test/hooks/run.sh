#!/bin/bash
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Check 11: the hooks of the core patch series are called, and are useful.
#
# The vanilla suite shows the series is dormant when nothing sets it.  This
# shows the other half: with gp_probe loaded, each hook fires where it should,
# with the arguments it should, and early enough to change the outcome.
#
# What gp_probe arms lives in the backend that armed it, so each scenario runs
# as one psql session over a script that prints "key=value" lines, and the
# assertions read those back.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/hooks/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-hooks-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbh-XXXXXX)"
PORT="${PGPORT:-$((5900 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -10 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	if [ -n "${KEEP:-}" ]; then
		echo "kept: $WORK"
	else
		rm -rf "$WORK"
	fi
	rm -rf "$SOCK"
}
trap cleanup EXIT

# One session per scenario: run the script on stdin, keep its output.
session() {							# session <name>  < script
	cat > "$WORK/$1.sql"
	"$PSQL" -X -q -t -A -d postgres -f "$WORK/$1.sql" > "$WORK/$1.out" 2>&1
}

val() { sed -n "s/^$2=//p" "$WORK/$1.out" | head -1; }   # val <name> <key>

# is <label> <session> <key> <expected>
is() {
	local got; got=$(val "$2" "$3")
	if [ "$got" = "$4" ]; then
		ok "$1"
	else
		notok "$1" "want [$4], got [$got]
$(grep -i 'error' "$WORK/$2.out" | head -3)"
	fi
}

# fired <label> <session> <event>
fired() {
	local n; n=$(val "$2" "calls_$3")
	if [ "${n:-0}" -ge 1 ] 2>/dev/null; then
		ok "$1 -> $(val "$2" "detail_$3")"
	else
		notok "$1" "calls('$3') = ${n:-<none>}
$(grep -i 'error' "$WORK/$2.out" | head -3)"
	fi
}

echo "hook tests (check 11)"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_probe'"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

session setup <<'SQL'
CREATE EXTENSION gp_probe;
SELECT 'ready=' || (extname IS NOT NULL)::text FROM pg_extension WHERE extname = 'gp_probe';
SQL
[ "$(val setup ready)" = "true" ] || { echo "gp_probe did not install"; cat "$WORK/setup.out"; exit 1; }

###############################################################################
echo "R1  new_oid_hook: a dispatched OID is the OID the catalog gets"
###############################################################################
session r1 <<'SQL'
SELECT gp_probe.reset();
-- far above FirstNormalObjectId, and unused in a fresh cluster
SELECT gp_probe.arm_new_oid('pg_class'::regclass, 987654);
CREATE TABLE r1_t (a int, b text);
INSERT INTO r1_t VALUES (1, 'x');
SELECT 'calls_new_oid=' || gp_probe.calls('new_oid');
SELECT 'detail_new_oid=' || gp_probe.detail('new_oid');
SELECT 'oid=' || oid FROM pg_class WHERE relname = 'r1_t';
SELECT 'own_relfile=' || (relfilenode <> oid)::text FROM pg_class WHERE relname = 'r1_t';
SELECT 'rows=' || count(*) FROM r1_t;
SQL
fired "new_oid_hook is called when a relation OID is allocated" r1 new_oid
is "the relation got the OID the hook returned" r1 oid 987654
# The hook's OID was never checked against the files on disk, so the relation
# has to take a relfilenumber of its own.  This is the half of R1 most easily
# got wrong, and the half a dispatched CREATE TABLE depends on.
is "and a relfilenumber of its own, not the OID" r1 own_relfile true
is "the table works" r1 rows 1

###############################################################################
echo "O3  analyze_sample_rows_hook: ANALYZE believes the hook's numbers"
###############################################################################
session o3 <<'SQL'
SELECT gp_probe.reset();
CREATE TABLE o3_t (a int);
INSERT INTO o3_t SELECT generate_series(1, 10);
SELECT gp_probe.arm_analyze('o3_t'::regclass, 4242, 1234567);
ANALYZE o3_t;
SELECT 'calls_analyze_sample=' || gp_probe.calls('analyze_sample');
SELECT 'detail_analyze_sample=' || gp_probe.detail('analyze_sample');
SELECT 'relpages=' || relpages FROM pg_class WHERE relname = 'o3_t';
SELECT 'reltuples=' || reltuples::bigint FROM pg_class WHERE relname = 'o3_t';
SQL
fired "analyze_sample_rows_hook is called for the armed relation" o3 analyze_sample
# The table really holds 10 rows on one page.  These are the hook's numbers, so
# ANALYZE took both the sampler and the page count from it -- which is what a
# distributed table needs, its rows being on other servers.
is "pg_class.relpages is the hook's page count, not the real one" o3 relpages 4242
is "pg_class.reltuples is the hook's row count, not the real one" o3 reltuples 1234567

###############################################################################
echo "O4  explain_node_label_hook: a CustomScan prints as its owner names it"
###############################################################################
session o4 <<'SQL'
SELECT gp_probe.reset();
SELECT gp_probe.arm_explain(true);
\o /dev/null
EXPLAIN (COSTS OFF) SELECT 1;
\o
SELECT gp_probe.arm_explain(false);
SELECT 'calls_explain_label=' || gp_probe.calls('explain_label');
SELECT 'detail_explain_label=' || gp_probe.detail('explain_label');
SQL
# The plan text itself is easier to capture outside SQL.
"$PSQL" -X -q -t -A -d postgres > "$WORK/o4plan.out" 2>&1 <<'SQL'
SELECT gp_probe.arm_explain(true);
EXPLAIN (COSTS OFF) SELECT 1;
SQL
fired "explain_node_label_hook is called for a CustomScan" o4 explain_label
if grep -q 'Probe Motion 3:1  (slice1; segments: 3)' "$WORK/o4plan.out"; then
	ok "the node prints as \"Probe Motion 3:1  (slice1; segments: 3)\""
else
	notok "the node should carry the hook's name and suffix" "$(cat "$WORK/o4plan.out")"
fi
if grep -q 'Custom Scan' "$WORK/o4plan.out"; then
	notok "the default label should have been replaced" "$(cat "$WORK/o4plan.out")"
else
	ok "\"Custom Scan (GpProbe)\" does not appear"
fi

###############################################################################
echo "O10 columnref_fallback_hook, deparse_function_as_column_hook: a name that is a call"
###############################################################################
# The armed name, where no column has it, is a call of the armed function on
# the row, and prints back as the name: what gp_core does with gp_segment_id.
# PL/pgSQL, so that the planner keeps the call rather than inlining it.
session o10 <<'SQL'
SELECT gp_probe.reset();
CREATE TABLE o10_t (a int);
INSERT INTO o10_t VALUES (1), (2);
CREATE FUNCTION o10_f(o10_t) RETURNS int LANGUAGE plpgsql AS 'BEGIN RETURN $1.a * 10; END';
SELECT probed FROM o10_t;
SELECT gp_probe.arm_column('probed', 'o10_f(o10_t)'::regprocedure);
SELECT 'calls_before=' || gp_probe.calls('columnref') FROM o10_t WHERE a = 1;
SELECT 'values=' || string_agg(probed::text, '|' ORDER BY probed) FROM o10_t;
SELECT 'qualified=' || t.probed FROM o10_t t WHERE a = 2;
SELECT 'calls_columnref=' || gp_probe.calls('columnref');
SELECT 'detail_columnref=' || gp_probe.detail('columnref');
CREATE VIEW o10_v AS SELECT probed, a FROM o10_t WHERE probed > 10;
SELECT 'viewdef=' || regexp_replace(pg_get_viewdef('o10_v'), '\s+', ' ', 'g');
CREATE FUNCTION o10_explain() RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXPLAIN (VERBOSE, COSTS OFF) SELECT probed FROM o10_t LOOP
		r := r || ' ' || l;
	END LOOP;
	RETURN r;
END $$;
SELECT 'explain=' || btrim(regexp_replace(o10_explain(), '\s+', ' ', 'g'));
SELECT 'calls_deparse_column=' || gp_probe.calls('deparse_column');
SELECT 'detail_deparse_column=' || gp_probe.detail('deparse_column');
SELECT gp_probe.reset();
SELECT 'view_unarmed=' || string_agg(a::text, '|') FROM o10_v;
SELECT 'viewdef_unarmed=' || regexp_replace(pg_get_viewdef('o10_v'), '\s+', ' ', 'g');
SQL
is "a column that exists never reaches the hook" o10 calls_before 0
is "the name is the call, unqualified" o10 values '10|20'
is "and qualified" o10 qualified 20
fired "columnref_fallback_hook is called for the name no column has" o10 columnref
is "a view prints the call as the name" o10 viewdef ' SELECT probed, a FROM o10_t WHERE (probed > 10);'
is "and EXPLAIN does too, as it prints a column of one relation" o10 explain 'Seq Scan on public.o10_t Output: probed'
fired "deparse_function_as_column_hook is called for the call" o10 deparse_column
is "unarmed, a stored view still runs" o10 view_unarmed 2
is "and prints as the call it is" o10 viewdef_unarmed ' SELECT o10_f(o10_t.*) AS probed, a FROM o10_t WHERE (o10_f(o10_t.*) > 10);'
if grep -q 'column "probed" does not exist' "$WORK/o10.out"; then
	ok "unarmed, the name is a missing column"
else
	notok "unarmed, the name should be a missing column" "$(head -5 "$WORK/o10.out")"
fi

###############################################################################
echo "O22 mdunlink_hook: an extension sees the files of a dropped relation go"
###############################################################################
session o22 <<'SQL'
SELECT gp_probe.reset();
CREATE TABLE o22_t (a int);
INSERT INTO o22_t VALUES (1);
SELECT 'relfile=' || relfilenode FROM pg_class WHERE relname = 'o22_t';
SELECT gp_probe.arm_mdunlink(true);
DROP TABLE o22_t;
SELECT gp_probe.arm_mdunlink(false);
SELECT 'calls_mdunlink=' || gp_probe.calls('mdunlink');
SELECT 'detail_mdunlink=' || gp_probe.detail('mdunlink');
SQL
fired "mdunlink_hook is called when a relation is dropped" o22 mdunlink
relfile=$(val o22 relfile)
if val o22 detail_mdunlink | grep -q "relfilenumber $relfile,"; then
	ok "it names the relfilenumber that was removed ($relfile)"
else
	notok "the hook should see relfilenumber $relfile" "$(val o22 detail_mdunlink)"
fi

###############################################################################
echo "O26 raw_parser_hook: syntax PostgreSQL cannot parse, and pass-through"
###############################################################################
session o26_before <<'SQL'
SELECT 'refused=' || 'no';
PROBE ME;
SQL
if grep -qi 'syntax error' "$WORK/o26_before.out"; then
	ok "\"PROBE ME\" is a syntax error before the hook is armed"
else
	notok "\"PROBE ME\" should not parse yet" "$(cat "$WORK/o26_before.out")"
fi

session o26 <<'SQL'
SELECT gp_probe.reset();
SELECT gp_probe.arm_parser(true);
PROBE ME;
SELECT 'calls_raw_parser=' || gp_probe.calls('raw_parser');
SELECT 'detail_raw_parser=' || gp_probe.detail('raw_parser');
-- Everything the hook passes through has to behave as before, including the
-- modes PL/pgSQL and type-name parsing use.
SELECT 'sql=' || (6 * 7);
CREATE FUNCTION o26_f(n int) RETURNS int LANGUAGE plpgsql
  AS $$ DECLARE v int; BEGIN v := n * 2; RETURN v; END $$;
SELECT 'plpgsql=' || o26_f(21);
SELECT 'typename=' || ('{1,2}'::int4[])[2];
SELECT gp_probe.arm_parser(false);
SQL
fired "raw_parser_hook is called" o26 raw_parser
is "\"PROBE ME\" now runs, returning what the hook parsed" o26 probe_result probed
is "ordinary SQL still works" o26 sql 42
is "PL/pgSQL still compiles and runs" o26 plpgsql 42
is "a type name still parses" o26 typename 2

###############################################################################
echo "O27 matview maintenance: DML on a materialized view, only while open"
###############################################################################
session o27_closed <<'SQL'
CREATE TABLE o27_base (a int);
INSERT INTO o27_base VALUES (1), (2);
CREATE MATERIALIZED VIEW o27_mv AS SELECT a FROM o27_base;
INSERT INTO o27_mv VALUES (99);
SQL
if grep -q 'cannot change materialized view' "$WORK/o27_closed.out"; then
	ok "DML on a materialized view is refused with maintenance mode closed"
else
	notok "PG19 should refuse this" "$(cat "$WORK/o27_closed.out")"
fi

session o27 <<'SQL'
SELECT 'depth0=' || gp_probe.matview_depth();
SELECT 'opened=' || gp_probe.matview_maintenance(true);
SELECT 'depth1=' || gp_probe.matview_depth();
INSERT INTO o27_mv VALUES (99);
SELECT 'applied=' || count(*) FROM o27_mv WHERE a = 99;
SELECT 'closed=' || gp_probe.matview_maintenance(false);
SQL
is "the depth starts at zero" o27 depth0 0
is "opening maintenance mode reports it is on" o27 opened true
is "and raises the depth" o27 depth1 1
is "the delta can now be applied with ordinary DML" o27 applied 1
is "closing reports it is off again" o27 closed false

# What an extension actually does: open, run code that can fail, and leave the
# depth where it found it.  Nothing lowers the depth when a transaction aborts,
# so without the restore maintenance mode would stay open for the session --
# and DML on materialized views with it.
session o27_recover <<'SQL'
SELECT 'before=' || gp_probe.matview_depth();
SELECT gp_probe.matview_apply_failing();
SQL
session o27_after <<'SQL'
SELECT 'after=' || gp_probe.matview_depth();
INSERT INTO o27_mv VALUES (1234);
SQL
is "the depth is zero before a delta is applied" o27_recover before 0
if grep -q 'pretending the delta failed' "$WORK/o27_recover.out"; then
	ok "a delta that fails raises its own error"
else
	notok "the failing delta should have errored" "$(cat "$WORK/o27_recover.out")"
fi
is "and the depth is back where it started" o27_after after 0
if grep -q 'cannot change materialized view' "$WORK/o27_after.out"; then
	ok "so DML on the view is refused again afterwards"
else
	notok "maintenance mode leaked past the failure" "$(cat "$WORK/o27_after.out")"
fi

# Restoring may only put the depth back, never raise it.
session o27_guard <<'SQL'
SELECT 'raise=' || gp_probe.matview_restore_depth(2);
SQL
if grep -q 'cannot restore materialized view maintenance depth' "$WORK/o27_guard.out"; then
	ok "restoring cannot raise the depth"
else
	notok "raising the depth should be refused" "$(cat "$WORK/o27_guard.out")"
fi

###############################################################################
echo "O28 star_expansion_filter_hook: a column stays out of SELECT *"
###############################################################################
session o28 <<'SQL'
SELECT gp_probe.reset();
CREATE TABLE o28_t (a int, __ivm_count bigint, b text);
INSERT INTO o28_t VALUES (1, 7, 'x');
SELECT gp_probe.arm_star_filter('o28_t'::regclass,
       ARRAY(SELECT attnum FROM pg_attribute
              WHERE attrelid = 'o28_t'::regclass AND attname = '__ivm_count'));
SELECT 'star=' || (SELECT string_agg(x::text, '|') FROM (SELECT * FROM o28_t) x);
SELECT 'calls_star_filter=' || gp_probe.calls('star_filter');
SELECT 'detail_star_filter=' || gp_probe.detail('star_filter');
SELECT 'by_name=' || __ivm_count FROM o28_t;
SELECT 'whole_row=' || (t.*)::text FROM o28_t t;
SQL
is "SELECT * leaves the counter column out" o28 star '(1,x)'
fired "star_expansion_filter_hook is called" o28 star_filter
is "the column is still there by name" o28 by_name 7
# A whole-row reference has the relation's composite type, so every column of
# that type belongs to it; the hook must not change that.
is "and a whole-row reference still has it" o28 whole_row '(1,7,x)'

###############################################################################
echo "O30 query_lockmode_hook: the query takes the hook's lock, and first"
###############################################################################
# The table an UPDATE or DELETE writes, or a locking clause locks, is armed
# for ExclusiveLock, as gp_core arms a distributed table without the deadlock
# detector: held from the parser's first open of it -- or the rewriter's, for
# the table under a view -- not after a weaker lock of PostgreSQL's own, the
# upgrade two sessions deadlock on; and recorded in its range table entry, so
# that a cached plan takes it again, first.
session o30 <<'SQL'
SELECT gp_probe.reset();
CREATE TABLE o30_t (a int);
INSERT INTO o30_t VALUES (1);
CREATE VIEW o30_v AS SELECT a FROM o30_t;
CREATE FUNCTION o30_modes(rel regclass) RETURNS text LANGUAGE sql AS $$
  SELECT coalesce(string_agg(mode, ',' ORDER BY mode), 'none') FROM pg_locks
   WHERE locktype = 'relation' AND relation = rel
     AND pid = pg_backend_pid() $$;
SELECT gp_probe.arm_lockmode('o30_t'::regclass, 'ExclusiveLock');
BEGIN;
UPDATE o30_t SET a = a + 1;
SELECT 'update=' || o30_modes('o30_t');
ROLLBACK;
BEGIN;
SELECT a FROM o30_t FOR UPDATE;
SELECT 'for_update=' || o30_modes('o30_t');
ROLLBACK;
BEGIN;
INSERT INTO o30_t VALUES (2);
SELECT 'insert=' || o30_modes('o30_t');
ROLLBACK;
BEGIN;
UPDATE o30_v SET a = a + 1;
SELECT 'view_update=' || o30_modes('o30_t') || ' view ' || o30_modes('o30_v');
ROLLBACK;
BEGIN;
SELECT a FROM o30_v FOR UPDATE;
SELECT 'view_for_update=' || o30_modes('o30_t');
ROLLBACK;
PREPARE o30_delete AS DELETE FROM o30_t WHERE a < 0;
EXECUTE o30_delete;
SELECT gp_probe.reset();
BEGIN;
EXECUTE o30_delete;
SELECT 'cached=' || o30_modes('o30_t');
SELECT 'cached_calls=' || gp_probe.calls('query_lockmode');
ROLLBACK;
SELECT gp_probe.arm_lockmode('o30_t'::regclass, 'AccessShareLock');
BEGIN;
SELECT a FROM o30_t FOR UPDATE;
SELECT 'weaker_read=' || o30_modes('o30_t');
ROLLBACK;
BEGIN;
DELETE FROM o30_t WHERE a < 0;
SELECT 'weaker_write=' || o30_modes('o30_t');
ROLLBACK;
SELECT 'calls_query_lockmode=' || gp_probe.calls('query_lockmode');
SELECT 'detail_query_lockmode=' || gp_probe.detail('query_lockmode');
SQL
is "an UPDATE's target is locked in the hook's mode alone" o30 update ExclusiveLock
is "and so is a table FOR UPDATE locks" o30 for_update ExclusiveLock
is "an INSERT keeps PostgreSQL's mode" o30 insert RowExclusiveLock
is "an UPDATE through a view: the rewriter locks the table in the hook's mode" o30 view_update "ExclusiveLock view RowExclusiveLock"
is "a locking clause through a view: so does the rewriter" o30 view_for_update ExclusiveLock
is "a cached plan takes the recorded mode, without asking again" o30 cached ExclusiveLock
is "(the hook was not asked again)" o30 cached_calls 0
is "a weaker mode is taken for a table the query only reads" o30 weaker_read AccessShareLock
is "and not for one it writes" o30 weaker_write RowExclusiveLock
fired "query_lockmode_hook is called" o30 query_lockmode

###############################################################################
echo "O31 deparse_range_function_hook: a function in FROM printed as what made it"
###############################################################################
# The armed function in FROM prints as the armed text, with no column
# definition list, and its alias only where the hook asks for it: what
# gp_core does with gp.dist_random(NULL::t), made of gp_dist_random('t').
session o31 <<'SQL'
SELECT gp_probe.reset();
CREATE FUNCTION o31_f(int) RETURNS SETOF record LANGUAGE sql AS 'SELECT $1, $1 * 2';
CREATE VIEW o31_v AS SELECT * FROM o31_f(3) AS s(a int, b int);
CREATE VIEW o31_other AS SELECT * FROM generate_series(1, 3) g;
SELECT gp_probe.arm_range('o31_f(int)'::regprocedure, 'PROBED(3)', false);
SELECT 'viewdef=' || regexp_replace(pg_get_viewdef('o31_v'), '\s+', ' ', 'g');
SELECT 'calls_deparse_range=' || gp_probe.calls('deparse_range');
SELECT 'detail_deparse_range=' || gp_probe.detail('deparse_range');
SELECT gp_probe.arm_range('o31_f(int)'::regprocedure, 'PROBED(3)', true);
SELECT 'viewdef_alias=' || regexp_replace(pg_get_viewdef('o31_v'), '\s+', ' ', 'g');
SELECT 'other=' || regexp_replace(pg_get_viewdef('o31_other'), '\s+', ' ', 'g');
SELECT gp_probe.reset();
SELECT 'viewdef_unarmed=' || regexp_replace(pg_get_viewdef('o31_v'), '\s+', ' ', 'g');
SQL
is "a view prints the call as the hook's text, with no column definitions" o31 viewdef ' SELECT a, b FROM PROBED(3);'
fired "deparse_range_function_hook is called for the call" o31 deparse_range
is "and its alias after it where the hook asks for it" o31 viewdef_alias ' SELECT a, b FROM PROBED(3) s;'
is "another function prints as it did" o31 other ' SELECT g FROM generate_series(1, 3) g(g);'
is "unarmed, the call and its column definitions print as they did" o31 viewdef_unarmed ' SELECT a, b FROM o31_f(3) s(a integer, b integer);'

###############################################################################
echo "R3  SyncRepHoldCancelDuringWait: the flag is an extension's to set"
###############################################################################
session r3 <<'SQL'
SELECT 'on=' || gp_probe.syncrep_hold(true);
SELECT 'off=' || gp_probe.syncrep_hold(false);
SQL
is "an extension can set the flag" r3 on true
is "and clear it" r3 off false
echo "  note   what the flag does needs a standby that is behind and a cancel"
echo "         during the commit wait; that belongs with the FTS tests at M4."

###############################################################################
echo "R2  XactAdoptCurrentXids: a second backend reads uncommitted rows"
###############################################################################
# A writer holds a transaction open through a FIFO, the way a writer segment
# process would; a reader adopts its XIDs and must see its uncommitted work.
session r2_setup <<'SQL'
CREATE TABLE r2_t (a int, b text);
SQL

A_IN="$WORK/a.in"
mkfifo "$A_IN"
"$PSQL" -X -q -t -A -d postgres < "$A_IN" > "$WORK/writer.out" 2>&1 &
A_PID=$!
exec 3> "$A_IN"
send_a() { printf '%s\n' "$1" >&3; sleep 0.4; }

send_a "BEGIN;"
send_a "SELECT gp_probe.arm_combocid(true);"
send_a "INSERT INTO r2_t VALUES (1, 'uncommitted');"
# Insert then update the same row in one transaction: the first version needs
# both a cmin and a cmax, and that is what makes a combo CID.
send_a "UPDATE r2_t SET b = 'updated' WHERE a = 1;"
send_a "\\o $WORK/a.xids"
send_a "SELECT gp_probe.current_xids();"
send_a "\\o"
send_a "\\o $WORK/a.combocids"
send_a "SELECT gp_probe.published_combocids();"
send_a "\\o"
sleep 0.6

xids=$(tr -d '\n' < "$WORK/a.xids" 2>/dev/null)
if [ -n "$xids" ] && [ "$xids" != "{}" ]; then
	ok "the writer reports its XIDs ($xids)"
else
	notok "the writer should report its XIDs" "$(tail -5 "$WORK/writer.out")"
	xids='{}'
fi

combocids=$(tr -d '\n' < "$WORK/a.combocids" 2>/dev/null)
[ -n "$combocids" ] || combocids='{}'
if [ "$combocids" != "{}" ]; then
	ok "the writer reports its combo CID mapping ($combocids)"
else
	notok "the writer should have made a combo CID" "$(tail -5 "$WORK/writer.out")"
fi

# Adopting the XIDs is necessary but not sufficient.  Once the writer's XID
# counts as current, MVCC treats its rows as this backend's own work and asks
# whether they were written before the current command -- so the reader also
# needs a command id at least as high as the writer's.  That is why
# Cloudberry's shared snapshot carries the writer's curcid next to its XIDs,
# and R2 alone does not make a reader see anything.
#
# Here a couple of writes stand in for that: only a command that uses the
# command id advances the counter, so read-only statements would not do.  They
# go before the adoption, because a backend that has adopted XIDs must not
# write -- its own subtransactions would then be invisible to it.
session r2 <<SQL
SELECT gp_probe.reset();
SELECT gp_probe.arm_combocid(true);
-- Without this the miss hook has nothing to answer with, and resolving the
-- writer's combo CID fails the assertion in GetRealCmax().
SELECT 'loaded=' || gp_probe.load_combocids('$combocids'::bigint[]);
CREATE TABLE IF NOT EXISTS r2_scratch (a int);
BEGIN;
INSERT INTO r2_scratch VALUES (1);
INSERT INTO r2_scratch VALUES (2);
SELECT 'before=' || count(*) FROM r2_t;
SELECT gp_probe.adopt_xids('$xids'::xid[]);
SELECT 'after=' || count(*) FROM r2_t;
SELECT 'value=' || b FROM r2_t;
SELECT 'calls_combocid_miss=' || gp_probe.calls('combocid_miss');
SELECT 'detail_combocid_miss=' || gp_probe.detail('combocid_miss');
SELECT gp_probe.adopt_xids('{}'::xid[]);
SELECT 'given_back=' || count(*) FROM r2_t;
COMMIT;
SQL
is "the reader loaded the writer's combo CID mapping" r2 loaded 1
is "a second backend cannot see the uncommitted row" r2 before 0
is "after adopting the writer's XIDs, it can" r2 after 1
is "and it reads the updated version of the row" r2 value updated
# Resolving that row's combo CID is what the miss hook is for: this backend
# never created it, so without the hook the lookup would fail an assertion.
fired "combocid_miss_hook resolves a combo CID this backend lacks" r2 combocid_miss
is "giving the XIDs back restores ordinary visibility" r2 given_back 0

send_a "ROLLBACK;"
send_a "\\q"
exec 3>&-
wait $A_PID 2>/dev/null

###############################################################################
echo "R4  XactAdoptTransactionState: a reader reads as of the writer's command"
###############################################################################
# What R2 left open.  A writer makes a table and fills another inside a
# transaction it keeps open, and hands over its state as
# SerializeTransactionState() writes it for a parallel worker.  A reader that
# adopts that state before its first snapshot sees the new table's catalog
# row and the rows, and not the row the writer writes after handing its state
# over.  With R2's XIDs alone it sees neither: it reads as of its own command,
# 0.  (The reader reads the new table's catalog row rather than the table,
# whose lock the writer holds: sharing locks is the extension's, with PG's
# lock groups.)
session r4_setup <<'SQL'
CREATE TABLE r4_rows (a int);
SQL

W_IN="$WORK/w.in"
mkfifo "$W_IN"
"$PSQL" -X -q -t -A -d postgres < "$W_IN" > "$WORK/r4writer.out" 2>&1 &
W_PID=$!
exec 4> "$W_IN"
send_w() { printf '%s\n' "$1" >&4; sleep 0.4; }

send_w "BEGIN;"
send_w "CREATE TABLE r4_new (a int);"
send_w "INSERT INTO r4_rows VALUES (1), (2);"
send_w "\\o $WORK/w.xids"
send_w "SELECT gp_probe.current_xids();"
send_w "\\o $WORK/w.state"
send_w "SELECT gp_probe.transaction_state();"
send_w "\\o"
send_w "INSERT INTO r4_rows VALUES (3);"
send_w "\\o $WORK/w.state2"
send_w "SELECT gp_probe.transaction_state();"
send_w "\\o"
sleep 0.6

wstate=$(tr -d '\n' < "$WORK/w.state" 2>/dev/null)
wstate2=$(tr -d '\n' < "$WORK/w.state2" 2>/dev/null)
wxids=$(tr -d '\n' < "$WORK/w.xids" 2>/dev/null)
[ -n "$wxids" ] || wxids='{}'
if [ -n "$wstate" ] && [ -n "$wstate2" ]; then
	ok "the writer serializes its transaction's state"
else
	notok "the writer should serialize its transaction's state" "$(tail -5 "$WORK/r4writer.out")"
fi

session r4_r2only <<SQL
BEGIN;
SELECT gp_probe.adopt_xids('$wxids'::xid[]);
SELECT 'table=' || count(*) FROM pg_class WHERE relname = 'r4_new';
SELECT 'rows=' || count(*) FROM r4_rows;
ROLLBACK;
SQL
is "with R2's XIDs alone, the writer's new table is not in the catalog" r4_r2only table 0
is "nor are its rows seen" r4_r2only rows 0

session r4 <<SQL
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET LOCAL gp_probe.adopt_state = '$wstate';
SELECT 'table=' || count(*) FROM pg_class WHERE relname = 'r4_new';
SELECT 'rows=' || count(*) FROM r4_rows;
SELECT 'max=' || max(a) FROM r4_rows;
COMMIT;
SELECT 'after=' || count(*) FROM pg_class WHERE relname = 'r4_new';
SQL
is "a reader that adopted the state sees the writer's new table in the catalog" r4 table 1
is "and its uncommitted rows" r4 rows 2
is "and not the row the writer wrote after handing its state over" r4 max 2
is "at the end of its transaction it reads as itself again" r4 after 0

session r4_later <<SQL
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET LOCAL gp_probe.adopt_state = '$wstate2';
SELECT 'rows=' || count(*) FROM r4_rows;
COMMIT;
SQL
is "the state handed over later reads that row too" r4_later rows 3

session r4_late <<SQL
BEGIN;
SELECT 1;
SET LOCAL gp_probe.adopt_state = '$wstate';
ROLLBACK;
SQL
if grep -q "cannot adopt a transaction's state after taking a snapshot" "$WORK/r4_late.out"; then
	ok "adopting after the transaction's first snapshot is refused"
else
	notok "adopting after the first snapshot should be refused" "$(cat "$WORK/r4_late.out")"
fi

send_w "ROLLBACK;"
send_w "\\q"
exec 4>&-
wait $W_PID 2>/dev/null

###############################################################################
echo "O13 a table access method's extension routine, registered while gp_probe loads"
###############################################################################
# gp_probe_am is heap underneath, with a TableAmExtRoutine of its own; each
# member the registry has is tested where the core asks it, below.
session o13 <<'SQL'
CREATE TABLE o13_t (a int, b text) USING gp_probe_am;
INSERT INTO o13_t SELECT i, 'row ' || i FROM generate_series(1, 100) i;
SELECT 'rows=' || count(*) FROM o13_t;
SELECT 'am=' || amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'o13_t';
SQL
is "a table of the probe's method is made, and works" o13 rows 100
is "and is of that method" o13 am gp_probe_am

###############################################################################
echo "O14 the method parses its tables' options, which are kept in pg_class"
###############################################################################
session o14 <<'SQL'
CREATE TABLE o14_t (a int) USING gp_probe_am WITH (probe_level = 7, fillfactor = 70);
SELECT 'stored=' || array_to_string(reloptions, ',') FROM pg_class WHERE relname = 'o14_t';
SELECT 'level=' || gp_probe.am_level('o14_t');
SELECT 'fillfactor=' || gp_probe.am_fillfactor('o14_t');
ALTER TABLE o14_t SET (probe_level = 3);
SELECT 'altered=' || array_to_string(reloptions, ',') FROM pg_class WHERE relname = 'o14_t';
SELECT 'level_after=' || gp_probe.am_level('o14_t');
CREATE MATERIALIZED VIEW o14_mv USING gp_probe_am WITH (probe_level = 5) AS SELECT 1 AS x;
SELECT 'mv=' || gp_probe.am_level('o14_mv');
SQL
is "CREATE TABLE keeps the method's option, which heap refuses, in pg_class" o14 stored "probe_level=7,fillfactor=70"
is "the relcache gives the method's parser's result" o14 level 7
is "which begins with heap's options, as the core reads them" o14 fillfactor 70
is "ALTER TABLE ... SET is checked by the method's parser, and kept" o14 altered "fillfactor=70,probe_level=3"
is "and the relcache reads the new value" o14 level_after 3
is "a materialized view of the method keeps its option too" o14 mv 5

for stmt in \
	"CREATE TABLE o14_bad (a int) USING gp_probe_am WITH (probe_level = 99)" \
	"ALTER TABLE o14_t SET (probe_level = 99)" ; do
	out=$("$PSQL" -X -q -t -A -d postgres -c "$stmt" 2>&1)
	case "$out" in
		*'value 99 out of bounds for option "probe_level"'*)
			ok "the method's parser refuses what it refuses: ${stmt%% (*}" ;;
		*) notok "the method's parser should refuse probe_level = 99: ${stmt%% (*}" "$out" ;;
	esac
done
out=$("$PSQL" -X -q -t -A -d postgres -c "CREATE TABLE o14_bad2 (a int) USING gp_probe_am WITH (nonsense = 1)" 2>&1)
case "$out" in
	*'unrecognized parameter "nonsense"'*) ok "a name neither heap nor the method knows is refused, as heap refuses it" ;;
	*) notok "an unknown option of the probe's table should be refused" "$out" ;;
esac
out=$("$PSQL" -X -q -t -A -d postgres -c "CREATE TABLE o14_heap (a int) WITH (probe_level = 1)" 2>&1)
case "$out" in
	*'unrecognized parameter "probe_level"'*) ok "a heap table refuses the method's option, as PostgreSQL 19 does" ;;
	*) notok "a heap table should refuse probe_level" "$out" ;;
esac
session o14_left <<'SQL'
SELECT 'left=' || count(*) FROM pg_class WHERE relname IN ('o14_bad', 'o14_bad2', 'o14_heap');
SQL
is "a refused CREATE leaves no table behind" o14_left left 0

###############################################################################
echo "O15 a scan of the method's table is given its plan node, which names only the columns it reads"
###############################################################################
# Each scan of a gp_probe_am table that the executor gave its plan node logs
# its kind and the columns the node reads.  A scan under an aggregate or a
# join would read a physical target list, every column, but for the
# planner's half of O15.
session o15 <<'SQL'
CREATE TABLE o15_t (a int, b int, c text, d int) USING gp_probe_am;
INSERT INTO o15_t SELECT i, i % 10, repeat('x', 50), i FROM generate_series(1, 20000) i;
CREATE INDEX o15_b ON o15_t (b);
CREATE TABLE o15_heap (a int, b int, c text, d int);
INSERT INTO o15_heap SELECT * FROM o15_t;
ANALYZE o15_t, o15_heap;
SET max_parallel_workers_per_gather = 0;
SELECT gp_probe.reset();
SELECT count(*) FROM o15_t WHERE d > 5;
SELECT 'agg=' || gp_probe.scan_log();
SELECT gp_probe.reset();
SELECT a, d FROM o15_t WHERE b = 3 LIMIT 1;
SELECT 'top=' || gp_probe.scan_log();
SELECT gp_probe.reset();
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_indexscan = off;
SET enable_bitmapscan = off; SET enable_material = off;
SELECT count(*) FROM (SELECT x.a FROM o15_t x JOIN o15_t y ON x.a = y.d WHERE x.b = 1 AND y.b = 1 LIMIT 5) s;
SELECT 'nestloop=' || array_to_string(ARRAY(SELECT e FROM unnest(string_to_array(gp_probe.scan_log(), ';')) e ORDER BY e), ';');
RESET enable_bitmapscan;
SELECT gp_probe.reset();
SET enable_seqscan = off;
SELECT sum(d) FROM o15_t WHERE b = 3;
SELECT 'bitmap=' || gp_probe.scan_log();
RESET enable_seqscan; RESET enable_indexscan;
SELECT gp_probe.reset();
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = on;
SELECT count(*) FROM o15_t WHERE d > 100;
SELECT 'parallel=' || (gp_probe.scan_log() LIKE '%parallel o15_t: 4%')::text;
SELECT 'plan=' || (SELECT count(*) FROM (SELECT 1 FROM o15_t WHERE d > 100) s)::text;
RESET ALL;
SELECT gp_probe.reset();
SELECT count(*) FROM o15_heap WHERE b > 5;
SELECT 'heap=' || gp_probe.scan_log() || '.';
SQL
is "a scan under an aggregate reads its qual's column alone" o15 agg "seq o15_t: 4"
is "a scan at the top reads what it returns and filters by" o15 top "seq o15_t: 1 2 4"
is "a nested loop's scans read their own columns, not every one" o15 nestloop "seq o15_t: 1 2;seq o15_t: 2 4"
is "a bitmap scan reads its qual's and its target list's columns" o15 bitmap "bitmap o15_t: 2 4"
is "a parallel scan is given its plan node too" o15 parallel true
is "a heap table's scans are not asked" o15 heap "."

###############################################################################
echo "O16 a unique index's probe of the method's table is the method's to answer"
###############################################################################
# While armed, gp_probe_am's own index fetch fails: a duplicate key found
# through the core's probe would end in that error, not in the unique
# violation the method's answer gives.
session o16 <<'SQL'
CREATE TABLE o16_t (a int PRIMARY KEY, b text) USING gp_probe_am;
INSERT INTO o16_t VALUES (1, 'x'), (2, 'y');
DELETE FROM o16_t WHERE a = 2;
CREATE TABLE o16_heap (a int PRIMARY KEY);
INSERT INTO o16_heap VALUES (1);
SELECT gp_probe.reset();
SELECT gp_probe.arm_fetch_fails(true);
INSERT INTO o16_t VALUES (1, 'again');
SELECT 'calls_unique_check=' || gp_probe.calls('unique_check');
SELECT 'detail_unique_check=' || gp_probe.detail('unique_check');
INSERT INTO o16_t VALUES (2, 'back');
SELECT 'reinserted=' || count(*) FROM (SELECT 1 FROM o16_t WHERE b = 'back') s;
SELECT 'dead=' || gp_probe.detail('unique_check');
SELECT gp_probe.arm_fetch_fails(false);
SELECT gp_probe.reset();
INSERT INTO o16_heap VALUES (1);
SELECT 'heap_calls=' || gp_probe.calls('unique_check');
SQL
fired "a key found in the index is checked by the method" o16 unique_check
if grep -q 'duplicate key value violates unique constraint "o16_t_pkey"' "$WORK/o16.out" \
		&& ! grep -q "the method's index fetch was called" "$WORK/o16.out"; then
	ok "and the duplicate is refused on its answer, never through the fetch"
else
	notok "the duplicate should be refused on the method's answer" "$(grep ERROR "$WORK/o16.out")"
fi
is "a key whose row was deleted is not live, and its insert goes in" o16 reinserted 1
if val o16 dead | grep -q "not live"; then
	ok "the method said the deleted row's key was not live"
else
	notok "the method should say the deleted row is not live" "$(val o16 dead)"
fi
is "a heap table's probe fetches as before, and asks no method" o16 heap_calls 0

###############################################################################
echo "O17 new columns of the method's table are written by the method, not by a rewrite"
###############################################################################
# A default PostgreSQL cannot keep as a missing value -- random(), or a
# stored generated column -- makes ALTER TABLE rewrite the table.  gp_probe_am
# writes the new columns' values itself, updating each row where it is.
session o17 <<'SQL'
CREATE FUNCTION o17_rewritten() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN RAISE NOTICE 'table_rewrite of %', pg_event_trigger_table_rewrite_oid()::regclass; END $$;
CREATE EVENT TRIGGER o17_rewrite ON table_rewrite EXECUTE FUNCTION o17_rewritten();
CREATE TABLE o17_t (a int, b int) USING gp_probe_am;
INSERT INTO o17_t SELECT i, i FROM generate_series(1, 10) i;
CREATE TABLE o17_file AS SELECT relfilenode AS before FROM pg_class WHERE relname = 'o17_t';
SELECT gp_probe.reset();
ALTER TABLE o17_t ADD COLUMN c float8 DEFAULT random();
SELECT 'calls_add_columns=' || gp_probe.calls('add_columns');
SELECT 'detail_add_columns=' || gp_probe.detail('add_columns');
SELECT 'same_file=' || (c.relfilenode = f.before)::text FROM pg_class c, o17_file f WHERE c.relname = 'o17_t';
SELECT 'filled=' || count(*) FROM o17_t WHERE c IS NOT NULL AND c >= 0 AND c < 1;
SELECT 'distinct=' || (count(DISTINCT c) > 1)::text FROM o17_t;
ALTER TABLE o17_t ADD COLUMN d int GENERATED ALWAYS AS (a * 2 + b) STORED;
SELECT 'generated=' || count(*) FROM o17_t WHERE d = a * 3;
SELECT 'gen_detail=' || gp_probe.detail('add_columns');
ALTER TABLE o17_t ADD COLUMN e int DEFAULT (random() * 0 - 1)::int CHECK (e >= 0);
SELECT 'no_e=' || count(*) FROM pg_attribute WHERE attrelid = 'o17_t'::regclass AND attname = 'e';
SELECT gp_probe.reset();
CREATE TABLE o17_heap (a int);
INSERT INTO o17_heap VALUES (1);
ALTER TABLE o17_heap ADD COLUMN c float8 DEFAULT random();
SELECT 'heap_calls=' || gp_probe.calls('add_columns');
DROP EVENT TRIGGER o17_rewrite;
SQL
fired "ADD COLUMN ... DEFAULT random() asks the method" o17 add_columns
is "the table keeps its relfilenumber" o17 same_file true
is "every row has its new value, each its own" o17 filled 10
is "which are random, not one value for all" o17 distinct true
is "a stored generated column is computed from the row, the new values first" o17 generated 10
if val o17 gen_detail | grep -q "columns 4g,"; then
	ok "and the method is told the column is generated"
else
	notok "the method should be told the column is generated" "$(val o17 gen_detail)"
fi
if grep -q 'check constraint "o17_t_e_check" of relation "o17_t" is violated by some row' "$WORK/o17.out"; then
	ok "a CHECK on a new column that a written value fails ends the statement"
else
	notok "the CHECK on the new column should fail" "$(grep ERROR "$WORK/o17.out")"
fi
is "and the column is not added" o17 no_e 0
if grep -q "table_rewrite of o17_heap" "$WORK/o17.out" && ! grep -q "table_rewrite of o17_t" "$WORK/o17.out"; then
	ok "no table_rewrite event fires for the method's table; a heap table's fires"
else
	notok "table_rewrite should fire for the heap table alone" "$(grep table_rewrite "$WORK/o17.out")"
fi
is "and a heap table is rewritten as before, asking no method" o17 heap_calls 0

###############################################################################
echo "O19 the size functions ask the method what its table takes"
###############################################################################
session o19 <<'SQL'
CREATE TABLE o19_t (a int) USING gp_probe_am;
INSERT INTO o19_t SELECT generate_series(1, 1000);
CREATE TABLE o19_heap (a int);
INSERT INTO o19_heap SELECT generate_series(1, 1000);
VACUUM o19_t, o19_heap;
SELECT gp_probe.arm_size(123456789);
SELECT 'rel=' || pg_relation_size('o19_t');
SELECT 'fsm=' || pg_relation_size('o19_t', 'fsm');
SELECT 'init=' || pg_relation_size('o19_t', 'init');
SELECT 'table=' || (pg_table_size('o19_t') - 123456789 = pg_relation_size('o19_t', 'fsm') + pg_relation_size('o19_t', 'vm'))::text;
SELECT 'total=' || (pg_total_relation_size('o19_t') = pg_table_size('o19_t'))::text;
SELECT 'heap=' || (pg_relation_size('o19_heap') = 8192 * (SELECT relpages FROM pg_class WHERE relname = 'o19_heap'))::text;
SELECT gp_probe.arm_size(-1);
SELECT 'files=' || (pg_relation_size('o19_t') = pg_relation_size('o19_heap'))::text;
SQL
is "pg_relation_size reports what the method says of the main fork" o19 rel 123456789
is "and of a fork it asks for, the method's answer too" o19 fsm 24576
is "a fork the table does not have takes nothing" o19 init 0
is "pg_table_size adds up what the method says of all the forks" o19 table true
is "and pg_total_relation_size builds on it" o19 total true
is "a heap table's size is its files', as before" o19 heap true
is "disarmed, the method answers with the files it has" o19 files true

###############################################################################
echo "O20 UPDATE, DELETE ... RETURNING and MERGE take the method's old row from the plan"
###############################################################################
# While armed, gp_probe_am cannot fetch a row by its TID: each statement has
# to take the old row from the whole-row column the planner adds beside the
# ctid, which stays the row's identity for the update and the delete.
session o20 <<'SQL'
CREATE TABLE o20_t (a int, b text, c int) USING gp_probe_am;
INSERT INTO o20_t SELECT i, 'r' || i, i FROM generate_series(1, 10) i;
CREATE TABLE o20_heap (a int, b text, c int);
INSERT INTO o20_heap SELECT * FROM o20_t;
SELECT gp_probe.arm_rowfetch_fails(true);
UPDATE o20_t SET c = c + 100 WHERE a <= 5;
SELECT 'updated=' || count(*) FROM o20_t WHERE c > 100 AND b = 'r' || a;
SELECT 'versions=' || count(*) FROM o20_t;
WITH u AS (UPDATE o20_t SET c = -c WHERE a IN (6, 7) RETURNING a, old.b AS ob, old.c AS oc, new.c AS nc)
SELECT 'returning=' || string_agg(format('%s:%s>%s', ob, oc, nc), ',' ORDER BY a) FROM u;
WITH d AS (DELETE FROM o20_t WHERE a = 10 RETURNING b)
SELECT 'deleted=' || string_agg(b, ',') FROM d;
MERGE INTO o20_t t USING (VALUES (1, 7), (42, 9)) s(a, c) ON t.a = s.a
  WHEN MATCHED THEN UPDATE SET c = s.c
  WHEN NOT MATCHED THEN INSERT VALUES (s.a, 'new', s.c);
SELECT 'merged=' || string_agg(format('%s:%s:%s', a, b, c), ',' ORDER BY a) FROM o20_t WHERE a IN (1, 42);
DELETE FROM o20_t WHERE a = 9;
SELECT 'after=' || count(*) FROM o20_t;
SELECT gp_probe.arm_rowfetch_fails(false);
EXPLAIN (VERBOSE, COSTS OFF) UPDATE o20_t SET c = 0;
EXPLAIN (VERBOSE, COSTS OFF) UPDATE o20_heap SET c = 0;
UPDATE o20_heap SET c = c + 100 WHERE a <= 5;
SELECT 'heap=' || count(*) FROM o20_heap WHERE c > 100;
SQL
if grep -q "asked to fetch" "$WORK/o20.out"; then
	notok "no statement should fetch the method's row by its TID" "$(grep "asked to fetch" "$WORK/o20.out" | head -3)"
else
	ok "no statement fetched the method's row by its TID"
fi
is "UPDATE builds each new row from the whole row: the columns it does not set are kept" o20 updated 5
is "and the old versions are gone, the ctid having said which" o20 versions 10
is "RETURNING old and new reads the old row from the plan" o20 returning "r6:6>-6,r7:7>-7"
is "DELETE ... RETURNING returns the deleted row from it" o20 deleted r10
is "MERGE updates a matched row from it, and inserts the others" o20 merged "1:r1:7,42:new:9"
is "a DELETE without RETURNING needs no old row" o20 after 9
if grep -q "o20_t\.\*" "$WORK/o20.out" && ! grep -q "o20_heap\.\*" "$WORK/o20.out"; then
	ok "the plan carries the method's table's whole row beside the ctid, and not a heap table's"
else
	notok "the whole-row column belongs to the method's table's plan alone" "$(grep -E 'Output' "$WORK/o20.out" | head -6)"
fi
is "a heap table's UPDATE fetches its rows as before" o20 heap 5

###############################################################################
echo "O18 BRIN walks the ranges of the method's runs of block numbers, not the gaps between"
###############################################################################
# Ten rows of gp_probe_am, one a page; the middle four removed, so that the
# rows are in two runs, blocks 0-2 and 7-9, which the probe is told.  Each
# BRIN range is one page, and the revmap (pageinspect) says which have a
# summary: the six of the runs, and none between, where PostgreSQL 19 gives
# every range of the table one, empty or not.
session o18 <<'SQL'
CREATE EXTENSION IF NOT EXISTS pageinspect;
CREATE TABLE o18_t (a int, pad text) USING gp_probe_am WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO o18_t SELECT i, repeat('x', 1000) FROM generate_series(1, 10) i;
DELETE FROM o18_t WHERE a BETWEEN 4 AND 7;
VACUUM o18_t;
CREATE TABLE o18_h (LIKE o18_t) WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO o18_h SELECT i, repeat('x', 1000) FROM generate_series(1, 10) i;
DELETE FROM o18_h WHERE a BETWEEN 4 AND 7;
VACUUM o18_h;
SELECT 'pages=' || pg_relation_size('o18_t') / 8192;
SELECT gp_probe.arm_block_sequences('o18_t', '{0,3,7,3}');
SELECT gp_probe.reset();
CREATE INDEX o18_i ON o18_t USING brin (a) WITH (pages_per_range = 1);
SELECT 'calls_block_sequences=' || gp_probe.calls('block_sequences');
SELECT 'detail_block_sequences=' || gp_probe.detail('block_sequences');
SELECT 'serial=' || string_agg(i::text, ',' ORDER BY i)
  FROM (SELECT row_number() OVER () - 1 AS i, pages FROM brin_revmap_data(get_raw_page('o18_i', 1))) r
 WHERE pages <> '(0,0)';
SET enable_seqscan = off;
SELECT 'found=' || count(*) FROM o18_t WHERE a > 0;
RESET enable_seqscan;
SET max_parallel_maintenance_workers = 2; SET min_parallel_table_scan_size = 0;
CREATE INDEX o18_p ON o18_t USING brin (a) WITH (pages_per_range = 1);
SELECT 'parallel=' || string_agg(i::text, ',' ORDER BY i)
  FROM (SELECT row_number() OVER () - 1 AS i, pages FROM brin_revmap_data(get_raw_page('o18_p', 1))) r
 WHERE pages <> '(0,0)';
RESET max_parallel_maintenance_workers; RESET min_parallel_table_scan_size;
CREATE TABLE o18_u (a int, pad text) USING gp_probe_am WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO o18_u SELECT i, repeat('x', 1000) FROM generate_series(1, 3) i;
SELECT gp_probe.arm_block_sequences('o18_u', '{0,3}');
CREATE INDEX o18_s ON o18_u USING brin (a) WITH (pages_per_range = 1, autosummarize = off);
SELECT 'before=' || count(*) FROM brin_revmap_data(get_raw_page('o18_s', 1)) WHERE pages <> '(0,0)';
INSERT INTO o18_u SELECT i, repeat('x', 1000) FROM generate_series(4, 10) i;
DELETE FROM o18_u WHERE a BETWEEN 4 AND 7;
VACUUM o18_u;
SELECT gp_probe.arm_block_sequences('o18_u', '{0,3,7,3}');
SET enable_seqscan = off;
SELECT 'unsummarized=' || count(*) FROM o18_u WHERE a > 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM o18_u WHERE a > 0;
RESET enable_seqscan;
SELECT 'summarized=' || brin_summarize_new_values('o18_s');
SELECT 'after=' || string_agg(i::text, ',' ORDER BY i)
  FROM (SELECT row_number() OVER () - 1 AS i, pages FROM brin_revmap_data(get_raw_page('o18_s', 1))) r
 WHERE pages <> '(0,0)';
CREATE INDEX o18_hi ON o18_h USING brin (a) WITH (pages_per_range = 1);
SELECT 'heap=' || count(*) FROM brin_revmap_data(get_raw_page('o18_hi', 1)) WHERE pages <> '(0,0)';
SQL
is "the table has ten pages, four of them empty" o18 pages 10
fired "a BRIN build asks the method for its runs" o18 block_sequences
is "a serial build summarizes the ranges of the two runs, and none between" o18 serial "0,1,2,7,8,9"
is "a scan finds the rows of both runs" o18 found 6
is "a parallel build summarizes the same ranges" o18 parallel "0,1,2,7,8,9"
is "an index built while the table has one run has its three ranges" o18 before 3
is "once rows are in a second run, a scan finds them in its ranges with no summary" o18 unsummarized 6
if grep -q "lossy=6" "$WORK/o18.out"; then
	ok "and visits the six ranges of the runs, not the four between with no summary either"
else
	notok "the scan should visit six ranges" "$(grep -i "heap blocks" "$WORK/o18.out")"
fi
is "brin_summarize_new_values() summarizes the second run's, and nothing between" o18 summarized 3
is "which leaves the same ranges as a build" o18 after "0,1,2,7,8,9"
is "a heap table's index is built as before, with a summary for every range" o18 heap 10

###############################################################################
echo "O21 smgr_file_event_hook: a relation's files are followed as they change"
###############################################################################
session o21 <<'SQL'
SELECT gp_probe.arm_file_events(true);
CREATE TABLE o21_t (a int) WITH (autovacuum_enabled = off);
SELECT 'create=' || gp_probe.file_events('create', pg_relation_filenode('o21_t'));
INSERT INTO o21_t SELECT generate_series(1, 20000);
CREATE INDEX o21_i ON o21_t (a);
CREATE TABLE o21_f AS SELECT pg_relation_filenode('o21_t') AS t, pg_relation_filenode('o21_i') AS i;
DELETE FROM o21_t WHERE a > 100;
VACUUM o21_t;
SELECT 'extend=' || (gp_probe.file_events('extend', t) > 0)::text FROM o21_f;
SELECT 'index_extend=' || (gp_probe.file_events('extend', i) > 0)::text FROM o21_f;
SELECT 'truncate=' || (gp_probe.file_events('truncate', t) > 0)::text FROM o21_f;
DROP TABLE o21_t;
SELECT 'unlink=' || gp_probe.file_events('unlink', t) FROM o21_f;
SELECT 'index_unlink=' || gp_probe.file_events('unlink', i) FROM o21_f;
SELECT gp_probe.arm_file_events(false);
CREATE TABLE o21_q (a int);
INSERT INTO o21_q VALUES (1);
SELECT gp_probe.arm_file_events(true);
SELECT gp_probe.arm_extend_fails(pg_relation_filenode('o21_q'));
INSERT INTO o21_q SELECT generate_series(1, 100000);
SELECT gp_probe.arm_extend_fails(0);
SELECT 'readable=' || count(*) FROM o21_q;
INSERT INTO o21_q SELECT generate_series(1, 1000);
SELECT 'grows=' || count(*) FROM o21_q;
SELECT gp_probe.arm_file_events(false);
SQL
is "creating a table reports its main fork's creation" o21 create 1
is "filling it reports its extensions" o21 extend true
is "building an index reports the index's" o21 index_extend true
is "VACUUM's truncation of its empty end is reported" o21 truncate true
is "dropping it reports its unlink, once, at commit" o21 unlink 1
is "and its index's" o21 index_unlink 1
if grep -q "gp_probe: relation file [0-9]* may not grow" "$WORK/o21.out"; then
	ok "a hook that raises at an extension ends the statement with its error"
else
	notok "the INSERT should end with the hook's error" "$(grep ERROR "$WORK/o21.out")"
fi
is "and leaves the table readable, with what it had" o21 readable 1
is "and able to grow once the hook lets it" o21 grows 1001

###############################################################################
echo "O32 tablespace_location_hook and its drop hook: this server's directory of a tablespace, in redo too"
###############################################################################
# gp_probe.tablespace_subdir names the directory under the location that
# this server links to, as a node of a cluster names one by its dbid.  A
# crash before the next checkpoint, and a start with another name, show the
# redo of CREATE TABLESPACE asking the hook again, with the location as the
# statement gave it, which the record carries.
mkdir -p "$WORK/o32loc" "$WORK/o32plain"
echo "gp_probe.tablespace_subdir = 'first'" >> "$WORK/data/postgresql.conf"
session o32 <<SQL
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);
SELECT 'subdir=' || current_setting('gp_probe.tablespace_subdir');
CHECKPOINT;
CREATE TABLESPACE o32_ts LOCATION '$WORK/o32loc';
SELECT 'oid=' || oid FROM pg_tablespace WHERE spcname = 'o32_ts';
SELECT 'location=' || pg_tablespace_location(oid) FROM pg_tablespace WHERE spcname = 'o32_ts';
CREATE TABLE o32_t (a int) TABLESPACE o32_ts;
INSERT INTO o32_t VALUES (1);
SELECT 'path=' || pg_relation_filepath('o32_t');
SQL
o32_oid=$(val o32 oid)
o32_path=$(val o32 path)
if [ "$(val o32 subdir)" = "first" ] && [ -n "$o32_oid" ] &&
   [ "$(readlink "$WORK/data/pg_tblspc/$o32_oid")" = "$WORK/o32loc/first" ]; then
	ok "CREATE TABLESPACE links the directory the hook chose under the location"
else
	notok "pg_tblspc/$o32_oid should link $WORK/o32loc/first" \
		"$(readlink "$WORK/data/pg_tblspc/$o32_oid" 2>&1) $(grep -i error "$WORK/o32.out" | head -3)"
fi
is "and pg_tablespace_location(), which reads the link, says it" o32 location "$WORK/o32loc/first"
if [ -n "$o32_path" ] && [ -f "$WORK/o32loc/first/${o32_path#pg_tblspc/$o32_oid/}" ]; then
	ok "a table made in the tablespace has its file there"
else
	notok "the table's file should be under $WORK/o32loc/first" "$o32_path"
fi

"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
sed -i "s/^gp_probe.tablespace_subdir = 'first'$/gp_probe.tablespace_subdir = 'second'/" \
	"$WORK/data/postgresql.conf"
if "$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1; then
	session o32_redo <<'SQL'
SELECT 'rows=' || count(*) FROM o32_t;
SQL
	if [ "$(readlink "$WORK/data/pg_tblspc/$o32_oid")" = "$WORK/o32loc/second" ] &&
	   [ -f "$WORK/o32loc/second/${o32_path#pg_tblspc/$o32_oid/}" ]; then
		ok "its redo asks the hook again, with the location the record carries, and links what it chose"
	else
		notok "after recovery pg_tblspc/$o32_oid should link $WORK/o32loc/second" \
			"$(readlink "$WORK/data/pg_tblspc/$o32_oid" 2>&1)"
	fi
	is "and the table's row, replayed there, is read from it" o32_redo rows 1
else
	notok "the server should come back from the crash" "$(tail -5 "$WORK/log")"
fi

# DROP TABLESPACE asks the drop hook as it removes the link, which still
# links the directory the hook chose, and gp_probe removes it, as a node of
# a cluster removes the directory of its dbid; the location stays.
session o32_drop <<'SQL'
DROP TABLE o32_t;
DROP TABLESPACE o32_ts;
SQL
if grep -qF "gp_probe: tablespace $o32_oid: pg_tblspc/$o32_oid dropped, linking $WORK/o32loc/second, redo false" \
	"$WORK/log" && [ ! -e "$WORK/o32loc/second" ] && [ -d "$WORK/o32loc" ] &&
   [ ! -e "$WORK/data/pg_tblspc/$o32_oid" ]; then
	ok "DROP TABLESPACE asks the drop hook with the link, which removes the directory it chose"
else
	notok "the drop hook should have removed $WORK/o32loc/second" \
		"$(ls "$WORK/o32loc" 2>&1) $(grep 'gp_probe: tablespace' "$WORK/log" | tail -2) $(grep -i error "$WORK/o32_drop.out" | head -3)"
fi

# The redo of DROP TABLESPACE asks it too: a crash after a CREATE and a DROP
# replays both, the CREATE making the hook's directory again and the DROP
# removing it, where without the hook it would stay, emptied.
sed -i "s/^gp_probe.tablespace_subdir = 'second'$/gp_probe.tablespace_subdir = 'third'/" \
	"$WORK/data/postgresql.conf"
session o32_redo_drop <<SQL
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);
SELECT 'subdir=' || current_setting('gp_probe.tablespace_subdir');
CHECKPOINT;
CREATE TABLESPACE o32_gone LOCATION '$WORK/o32loc';
SELECT 'oid=' || oid FROM pg_tablespace WHERE spcname = 'o32_gone';
DROP TABLESPACE o32_gone;
SQL
o32_gone=$(val o32_redo_drop oid)
"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
if [ "$(val o32_redo_drop subdir)" = "third" ] && [ -n "$o32_gone" ] &&
   "$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1; then
	if grep -qF "gp_probe: tablespace $o32_gone: pg_tblspc/$o32_gone dropped, linking $WORK/o32loc/third, redo true" \
		"$WORK/log" && [ ! -e "$WORK/o32loc/third" ] &&
	   [ ! -e "$WORK/data/pg_tblspc/$o32_gone" ]; then
		ok "its redo asks the drop hook, with redo true, and the directory the redo of CREATE made goes"
	else
		notok "the redo of DROP TABLESPACE should have removed $WORK/o32loc/third" \
			"$(ls "$WORK/o32loc" 2>&1) $(grep 'gp_probe: tablespace' "$WORK/log" | tail -3)"
	fi
else
	notok "the server should come back from the crash after DROP TABLESPACE" \
		"$(tail -5 "$WORK/log") $(grep -i error "$WORK/o32_redo_drop.out" | head -3)"
fi

# Neither hook is asked for an in-place tablespace, whose directory is the
# link itself.
session o32_inplace <<'SQL'
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE o32_here LOCATION '';
SELECT 'oid=' || oid FROM pg_tablespace WHERE spcname = 'o32_here';
DROP TABLESPACE o32_here;
SELECT 'dropped=' || count(*) FROM pg_tablespace WHERE spcname = 'o32_here';
SQL
o32_here=$(val o32_inplace oid)
if [ -n "$o32_here" ] && [ "$(val o32_inplace dropped)" = "0" ] &&
   ! grep -qF "gp_probe: tablespace $o32_here:" "$WORK/log"; then
	ok "an in-place tablespace is dropped without the drop hook"
else
	notok "an in-place tablespace should not reach the drop hook" \
		"$(grep "gp_probe: tablespace ${o32_here:-?}:" "$WORK/log" | head -2) $(grep -i error "$WORK/o32_inplace.out" | head -3)"
fi

# A hook that chooses nothing: the location as given, as PostgreSQL 19 links
# it -- and its drop hook, asked as the link goes, leaves the location.
sed -i "/^gp_probe.tablespace_subdir = /d" "$WORK/data/postgresql.conf"
session o32_plain <<SQL
SELECT pg_reload_conf();
SELECT pg_sleep(0.5);
SELECT 'subdir=[' || current_setting('gp_probe.tablespace_subdir') || ']';
CREATE TABLESPACE o32_plain LOCATION '$WORK/o32plain';
SELECT 'oid=' || oid FROM pg_tablespace WHERE spcname = 'o32_plain';
SQL
o32_plain=$(val o32_plain oid)
if [ "$(val o32_plain subdir)" = "[]" ] &&
   [ "$(readlink "$WORK/data/pg_tblspc/$o32_plain")" = "$WORK/o32plain" ]; then
	ok "a hook that chooses nothing leaves the location as the statement gave it"
else
	notok "pg_tblspc should link $WORK/o32plain" "$(grep -i error "$WORK/o32_plain.out" | head -3)"
fi
session o32_end <<'SQL'
DROP TABLESPACE o32_plain;
SQL
if grep -qF "gp_probe: tablespace $o32_plain: pg_tblspc/$o32_plain dropped, linking $WORK/o32plain, redo false" \
	"$WORK/log" && [ -d "$WORK/o32plain" ] && [ ! -e "$WORK/data/pg_tblspc/$o32_plain" ]; then
	ok "and DROP TABLESPACE removes the link alone, the location staying"
else
	notok "$WORK/o32plain should stay after DROP TABLESPACE" \
		"$(grep "gp_probe: tablespace ${o32_plain:-?}:" "$WORK/log" | head -2) $(grep -i error "$WORK/o32_end.out" | head -3)"
fi

###############################################################################
echo "O23 extension marks: pg_checksums passes over what an extension marked, pg_upgrade carries it"
###############################################################################
# Last, because it stops the server: pg_checksums reads a stopped cluster.
# gp_probe marks "_probe" while the postmaster loads it, as gp_sql marks a
# directory table's "_dirtable"; a directory by that name in a database
# directory holds files that are no relation's pages.
if grep -qx '_probe' "$WORK/data/extension_marks" 2>/dev/null; then
	ok "the postmaster that loaded gp_probe wrote its mark"
else
	notok "extension_marks should hold _probe" "$(cat "$WORK/data/extension_marks" 2>&1)"
fi

"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w stop > /dev/null 2>&1

o23_marked() {							# o23_marked <datadir>: make one
	local d="$1/base/5/424242_probe"
	mkdir -p "$d/sub"
	# Not a whole block, which pg_checksums would stop at, and a whole
	# block of what is no page, which --enable would write a checksum into.
	printf 'no relation keeps this\n' > "$d/0"
	head -c 8192 /dev/urandom > "$d/1"
	printf 'nested\n' > "$d/sub/2"
}
o23_sum() { (cd "$1/base/5/424242_probe" && cat 0 1 sub/2 | md5sum | cut -c1-32); }

o23_marked "$WORK/data"
before=$(o23_sum "$WORK/data")

if "$BINDIR/pg_checksums" --check -D "$WORK/data" > "$WORK/o23_check.log" 2>&1; then
	ok "pg_checksums --check passes over a marked directory"
else
	notok "pg_checksums --check should pass over a marked directory" "$(tail -3 "$WORK/o23_check.log")"
fi
"$BINDIR/pg_checksums" --disable -D "$WORK/data" > "$WORK/o23_disable.log" 2>&1
if "$BINDIR/pg_checksums" --enable -D "$WORK/data" > "$WORK/o23_enable.log" 2>&1 \
		&& [ "$(o23_sum "$WORK/data")" = "$before" ]; then
	ok "pg_checksums --enable passes over it and leaves its files as they were"
else
	notok "pg_checksums --enable should leave the marked files alone" "$(tail -3 "$WORK/o23_enable.log")"
fi

# Without the list, PostgreSQL 19's pg_checksums, unchanged: it reads the
# directory's files as a relation's, and stops.
mv "$WORK/data/extension_marks" "$WORK/extension_marks.aside"
if "$BINDIR/pg_checksums" --check -D "$WORK/data" > "$WORK/o23_nomarks.log" 2>&1; then
	notok "without the list, pg_checksums should fail on the directory's files" "$(tail -3 "$WORK/o23_nomarks.log")"
elif grep -q "424242_probe" "$WORK/o23_nomarks.log"; then
	ok "without the list, pg_checksums fails on them, as PostgreSQL 19's does"
else
	notok "pg_checksums failed, but not on the marked directory" "$(tail -3 "$WORK/o23_nomarks.log")"
fi
mv "$WORK/extension_marks.aside" "$WORK/data/extension_marks"

# pg_upgrade, in copy and link modes, from a small cluster of its own whose
# postgres database has a table, and so a map, and a marked directory.
o23_cluster() {							# o23_cluster <datadir> <port>
	"$BINDIR/initdb" -D "$1" -N --locale=C --encoding=UTF8 > "$1.initdb.log" 2>&1 || return 1
	{
		echo "unix_socket_directories = '$SOCK'"
		echo "listen_addresses = ''"
		echo "port = $2"
		echo "shared_preload_libraries = 'gp_probe'"
	} >> "$1/postgresql.conf"
}
o23_upgrade() {							# o23_upgrade <mode> <new datadir>
	o23_cluster "$2" $((PORT + 3)) || return 1
	(cd "$WORK" && "$BINDIR/pg_upgrade" -b "$BINDIR" -B "$BINDIR" \
		-d "$WORK/o23old" -D "$2" -p $((PORT + 2)) -P $((PORT + 3)) -s "$SOCK" \
		--"$1" > "$2.upgrade.log" 2>&1)
}

if o23_cluster "$WORK/o23old" $((PORT + 2)) \
		&& "$BINDIR/pg_ctl" -D "$WORK/o23old" -l "$WORK/o23old.log" -w -t 60 start > /dev/null 2>&1; then
	"$PSQL" -X -q -p $((PORT + 2)) -d postgres \
		-c "CREATE TABLE o23_t (a int)" \
		-c "INSERT INTO o23_t VALUES (1)" > "$WORK/o23old.sql.log" 2>&1
	"$BINDIR/pg_ctl" -D "$WORK/o23old" -m fast -w stop > /dev/null 2>&1
	o23_marked "$WORK/o23old"
	old_sum=$(o23_sum "$WORK/o23old")

	if o23_upgrade copy "$WORK/o23copy" && [ "$(o23_sum "$WORK/o23copy" 2>/dev/null)" = "$old_sum" ]; then
		ok "pg_upgrade --copy carries a marked directory, what is under it too"
	else
		notok "pg_upgrade --copy should carry the marked directory" "$(tail -5 "$WORK/o23copy.upgrade.log")"
	fi
	if o23_upgrade link "$WORK/o23link" && [ "$(o23_sum "$WORK/o23link" 2>/dev/null)" = "$old_sum" ] \
			&& [ "$WORK/o23link/base/5/424242_probe/1" -ef "$WORK/o23old/base/5/424242_probe/1" ]; then
		ok "pg_upgrade --link carries it, as links"
	else
		notok "pg_upgrade --link should carry the marked directory" "$(tail -5 "$WORK/o23link.upgrade.log")"
	fi
else
	notok "the cluster to upgrade from should start" "$(tail -5 "$WORK/o23old.log" 2>/dev/null)"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
