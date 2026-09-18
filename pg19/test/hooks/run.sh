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

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
