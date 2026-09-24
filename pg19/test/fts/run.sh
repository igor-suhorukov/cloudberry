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
#
# M4: mirrors and FTS.
#
# A coordinator and three primaries, each with a mirror: a hot standby,
# streaming from its primary under the name gp_walreceiver, from the slot
# internal_wal_replication_slot, as Cloudberry's mirrors do.  What this checks
# is what FTS does of what befalls them (gp_fts.c): that it brings each pair
# in sync and turns synchronous replication on; that a mirror that stops is
# marked down and no longer holds its primary's commits, and that until then a
# commit waits for it whatever cancels it (R3); that a primary that stops is
# failed over from, the dispatcher following, and a transaction that was open
# across it failing; that the coordinator keeps what FTS found across a
# restart; that the failed primary comes back as its mirror's mirror, and the
# roles go back to the preferred ones; and that a pair whose mirror is not in
# sync is not failed over.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/fts/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"

PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-fts-XXXXXX")"
BASEPORT="${PGPORT:-$((6500 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"
NSEG=3
PRELOAD='gp_core,gp_sql'
SLOT=internal_wal_replication_slot

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

# node <n>: 0 the coordinator, 1..3 the primaries of contents 0..2, 4..6
# their mirrors.
datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }
dbid()    { echo $(($1 + 1)); }
content() { if [ "$1" -eq 0 ]; then echo -1; elif [ "$1" -le "$NSEG" ]; then echo $(($1 - 1)); else echo $(($1 - NSEG - 1)); fi; }
logfile() { echo "$ROOT/node$1.log"; }

cleanup() {
	for n in $(seq 0 $((2 * NSEG))); do
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop >/dev/null 2>&1
	done
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/fts"
		cp "$ROOT"/node*.log "$RESULTS_DIR/fts/" 2>/dev/null
	fi
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
}
trap cleanup EXIT

q() {						# q <n> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-c "$2" 2>&1
}

# Poll until the query gives what is wanted, or seconds pass.
wait_for() {				# wait_for <n> <sql> <want> [seconds]
	local n="$1" sql="$2" want="$3" t="${4:-60}" out
	for _ in $(seq $((t * 5))); do
		out=$(q "$n" "$sql")
		[ "$out" = "$want" ] && return 0
		sleep 0.2
	done
	echo "$out"
	return 1
}

start_node() {
	"$BINDIR/pg_ctl" -D "$(datadir "$1")" -l "$(logfile "$1")" -w -t 60 start >/dev/null 2>&1
}
stop_node() {				# stop_node <n> [mode]
	"$BINDIR/pg_ctl" -D "$(datadir "$1")" -m "${2:-fast}" -w -t 60 stop >/dev/null 2>&1
}

# What gp_segment_configuration says of a content: each node's dbid, role,
# preferred role, mode and status, the primary first.
config() {					# config <content>
	q 0 "SELECT string_agg(dbid || ':' || role::text || preferred_role::text || mode::text || status::text, ' ' ORDER BY role DESC, dbid)
	       FROM gp_segment_configuration WHERE content = $1"
}

# A node made a mirror of another: its copy, taken with pg_basebackup, a hot
# standby streaming from it as gp_walreceiver from its slot -- made with the
# copy the first time, and by FTS on a mirror it promotes, for the primary it
# replaces.
make_mirror() {				# make_mirror <n> <of n> [create-slot]
	local m="$1" p="$2" slotopt=(-S "$SLOT")
	[ "${3:-}" = create-slot ] && slotopt=(-C -S "$SLOT")
	rm -rf "$(datadir "$m")"
	mkdir -p "$(sockdir "$m")"
	"$BINDIR/pg_basebackup" -D "$(datadir "$m")" -h "$(sockdir "$p")" -p "$(port "$p")" \
		-X stream -c fast "${slotopt[@]}" > "$ROOT/basebackup$m.log" 2>&1 || return 1
	grep -v -E '^(port|unix_socket_directories|gp\.dbid|primary_conninfo|primary_slot_name|hot_standby) ' \
		"$(datadir "$m")/postgresql.auto.conf" > "$ROOT/auto.conf"
	{
		cat "$ROOT/auto.conf"
		echo "port = $(port "$m")"
		echo "unix_socket_directories = '$(sockdir "$m")'"
		echo "gp.dbid = $(dbid "$m")"
		echo "hot_standby = on"
		echo "primary_conninfo = 'host=$(sockdir "$p") port=$(port "$p") application_name=gp_walreceiver'"
		echo "primary_slot_name = '$SLOT'"
	} > "$(datadir "$m")/postgresql.auto.conf"
	touch "$(datadir "$m")/standby.signal"
	start_node "$m"
}

# A session of the coordinator's that stays open, fed through a fifo, so that
# a transaction can be left open while something else happens.
session_open() {
	rm -f "$ROOT/s.in"; mkfifo "$ROOT/s.in"
	"$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-v ON_ERROR_STOP=0 -f "$ROOT/s.in" > "$ROOT/s.out" 2>&1 &
	SESSION_PID=$!
	exec 7> "$ROOT/s.in"
}
session_send() { printf '%s\n' "$1" >&7; }
session_close() { exec 7>&-; wait "$SESSION_PID"; }

echo "M4 mirrors and FTS"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

for n in 0 $(seq 1 "$NSEG"); do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 \
		> "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n:"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
done

{
	echo "# dbid content role host port datadir"
	for n in $(seq 0 $((2 * NSEG))); do
		role=p; [ "$n" -gt "$NSEG" ] && role=m
		echo "$(dbid "$n") $(content "$n") $role $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done
} > "$CONF"

for n in 0 $(seq 1 "$NSEG"); do
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $(dbid "$n")"
		echo "max_prepared_transactions = 16"
		echo "gp.log_fts = verbose"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
done

###############################################################################
echo "1. the cluster starts, each primary with a mirror streaming from it"
###############################################################################
# The primaries, their mirrors, and then the coordinator, whose prober's
# first probe then finds the mirrors there, as gpinitsystem leaves them.
started=1
for n in $(seq 1 "$NSEG"); do
	start_node "$n" || { notok "node $n starts" "$(tail -5 "$(logfile "$n")")"; started=0; }
done
[ "$started" -eq 1 ] || exit 1

made=1
for n in $(seq 1 "$NSEG"); do
	make_mirror $((n + NSEG)) "$n" create-slot \
		|| { notok "the mirror of node $n" "$(tail -5 "$ROOT/basebackup$((n + NSEG)).log") $(tail -5 "$(logfile $((n + NSEG)))")"; made=0; }
done
[ "$made" -eq 1 ] || exit 1

start_node 0 || { notok "the coordinator starts" "$(tail -5 "$(logfile 0)")"; exit 1; }
out=$(q 0 "CREATE EXTENSION gp_core; CREATE EXTENSION gp_inject_fault;")
[ -z "$out" ] && ok "the coordinator and its three primaries start, with gp_core, and a mirror of each" \
	|| notok "CREATE EXTENSION" "$out"

streaming=1
for n in $(seq 1 "$NSEG"); do
	wait_for "$n" "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1 \
		>/dev/null || streaming=0
done
[ "$streaming" -eq 1 ] && ok "each primary's mirror is a hot standby, streaming from it as gp_walreceiver" \
	|| notok "the mirrors stream"

out=$(q 4 "SELECT pg_is_in_recovery() || ' ' || (SELECT dbid || ' ' || content_id FROM gp.node())")
[ "$out" = "true 5 0" ] && ok "a mirror knows which node it is, and is in recovery ($out)" \
	|| notok "a mirror's gp.node()" "$out"

out=$(q 0 "SELECT pid > 0 FROM pg_stat_activity WHERE backend_type = 'gp_core fts prober'")
[ "$out" = "t" ] && ok "the coordinator runs the FTS prober" || notok "the FTS prober" "$out"

###############################################################################
echo "2. FTS brings each pair in sync, and turns synchronous replication on"
###############################################################################
out=$(q 0 "SELECT gp_request_fts_probe_scan()")
out2=$(q 0 "SELECT string_agg(content || ':' || role::text || mode::text || status::text, ' ' ORDER BY content, role DESC) FROM gp_segment_configuration WHERE content >= 0")
[ "$out" = "t" ] && [ "$out2" = "0:psu 0:msu 1:psu 1:msu 2:psu 2:msu" ] \
	&& ok "one probe, and every pair is in sync ($out2)" \
	|| notok "FTS's first probe" "$out / $out2"

out=$(for n in $(seq 1 "$NSEG"); do q "$n" "SHOW synchronous_standby_names"; done | tr '\n' ' ')
[ "$out" = "* * * " ] && ok "every primary's commits wait for its mirror: synchronous_standby_names is '*'" \
	|| notok "synchronous_standby_names on the primaries" "$out"

out=$(q 0 "SELECT count(DISTINCT dbid) FROM gp_configuration_history WHERE \"desc\" LIKE 'FTS: update role, status, and mode for dbid % to %, u, and s'")
out2=$(q 0 "SELECT count(DISTINCT c.dbid) FROM gp_segment_configuration c JOIN gp_configuration_history h USING (dbid) WHERE c.content >= 0")
[ "$out" = "6" ] && [ "$out2" = "6" ] \
	&& ok "gp_configuration_history has what FTS changed, a row per node, in Cloudberry's words" \
	|| notok "gp_configuration_history" "$out / $out2"

out=$(sed -n '1,$p' "$(datadir 0)/gpsegconfig_dump" | awk '{print $1, $3, $4, $5, $6}' | tr '\n' ',')
[ "$out" = "1 p p n u,2 p p s u,3 p p s u,4 p p s u,5 m m s u,6 m m s u,7 m m s u," ] \
	&& ok "gpsegconfig_dump holds it, in Cloudberry's form" \
	|| notok "gpsegconfig_dump" "$out"

out=$(q 0 "CREATE TABLE t (a int, b text) DISTRIBUTED BY (a);
           INSERT INTO t SELECT i, 'before' FROM generate_series(1, 100) i;
           SELECT count(*) FROM t;")
[ "$out" = "100" ] && ok "a write to every segment commits, each commit sent to a mirror in sync" \
	|| notok "a write with the mirrors in sync" "$out"

out=$(q 4 "SELECT count(*) FROM t")
[ "$out" -gt 0 ] 2>/dev/null \
	&& ok "a mirror has its primary's rows, readable on the hot standby ($out of 100)" \
	|| notok "a mirror's rows" "$out"

###############################################################################
echo "3. a mirror that stops: its primary's commits wait for it, cancelled or not, until FTS marks it down"
###############################################################################
# Probes are skipped while the commit waits, as Cloudberry's commit_blocking
# skips them, so that FTS does not end the wait before it is looked at.
out=$(q 0 "SELECT gp_inject_fault_infinite('fts_probe', 'skip', 1)")
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
stop_node 5 fast
(q 0 "INSERT INTO t SELECT i, 'waited' FROM generate_series(101, 200) i" > "$ROOT/insert.out" 2>&1) &
insert_pid=$!

out=$(wait_for 2 "SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'SyncRep'" 1 30)
[ $? -eq 0 ] && ok "with the mirror of content 1 stopped, the commit waits for it on its primary (SyncRep)" \
	|| notok "a commit waiting for a stopped mirror" "$out"

waiter=$(q 2 "SELECT pid FROM pg_stat_activity WHERE wait_event = 'SyncRep'")
out=$(q 2 "SELECT pg_cancel_backend($waiter)")
sleep 1
out2=$(q 2 "SELECT wait_event FROM pg_stat_activity WHERE pid = $waiter")
[ "$out" = "t" ] && [ "$out2" = "SyncRep" ] \
	&& ok "R3: cancelled, it still waits, rather than commit without its mirror ($out2)" \
	|| notok "R3: a cancel during the commit's wait" "$out / $out2"

out=$(q 0 "SELECT gp_inject_fault('fts_probe', 'reset', 1)")
q 2 "ALTER SYSTEM SET gp.fts_mark_mirror_down_grace_period = 0" > /dev/null
q 2 "SELECT pg_reload_conf()" > /dev/null
sleep 0.5
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
wait "$insert_pid"
out=$(config 1)
out2=$(cat "$ROOT/insert.out")
out3=$(q 0 "SELECT count(*) FROM t")
[ "$out" = "3:ppnu 6:mmnd" ] && [ -z "$out2" ] && [ "$out3" = "200" ] \
	&& ok "FTS marks the mirror down and the pair not in sync, and the commit completes ($out)" \
	|| notok "FTS and a stopped mirror" "$out / $out2 / $out3"

out=$(q 2 "SHOW synchronous_standby_names")
[ -z "$out" ] && ok "its primary's synchronous_standby_names is '' now, as FTS's SYNCREP_OFF leaves it" \
	|| notok "synchronous_standby_names after SYNCREP_OFF" "$out"

out=$(grep -c "ignoring query cancel request for synchronous replication to ensure cluster consistency" "$(logfile 2)")
[ "$out" -ge 1 ] && ok "the cancel is dropped once the commit is done, in Cloudberry's words" \
	|| notok "the dropped cancel's warning" "$out"

out=$(q 0 "INSERT INTO t SELECT i, 'alone' FROM generate_series(201, 300) i; SELECT count(*) FROM t")
[ "$out" = "300" ] && ok "a write commits without the mirror, now that FTS has let its primary go on" \
	|| notok "a write with a mirror down" "$out"

###############################################################################
echo "4. the mirror comes back: FTS marks it up, the pair in sync, synchronous replication on"
###############################################################################
start_node 5
wait_for 2 "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1 >/dev/null
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out=$(config 1)
out2=$(q 2 "SHOW synchronous_standby_names")
out3=$(q 5 "SELECT count(*) FROM t WHERE b = 'alone'")
[ "$out" = "3:ppsu 6:mmsu" ] && [ "$out2" = "*" ] && [ "$out3" -gt 0 ] 2>/dev/null \
	&& ok "one probe: up, in sync, commits wait for it again, and it has caught up ($out)" \
	|| notok "a mirror that comes back" "$out / $out2 / $out3"

###############################################################################
echo "5. a primary that stops: FTS promotes its mirror, and the dispatcher follows"
###############################################################################
stop_node 1 immediate
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out=$(config 0)
[ "$out" = "5:pmnu 2:mpnd" ] \
	&& ok "content 0's mirror is its primary, not in sync; the primary a mirror, down ($out)" \
	|| notok "the configuration after a failover" "$out"

out=$(wait_for 4 "SELECT pg_is_in_recovery()" f 30)
[ $? -eq 0 ] && ok "the mirror FTS promoted has left recovery" || notok "the promotion" "$out"

out=$(q 4 "SHOW synchronous_standby_names; SELECT slot_name FROM pg_replication_slots")
[ "$(echo "$out" | tr '\n' ' ')" = " $SLOT " ] \
	&& ok "it waits for no mirror, and keeps a slot for the one it will have" \
	|| notok "the new primary's replication" "$out"

out=$(q 0 "SELECT count(*), count(*) FILTER (WHERE b = 'alone') FROM t")
[ "$out" = "300|100" ] && ok "a new session reads every row, content 0's from its new primary ($out)" \
	|| notok "reading after a failover" "$out"

out=$(q 0 "INSERT INTO t SELECT i, 'after' FROM generate_series(301, 400) i; SELECT count(*) FROM t")
[ "$out" = "400" ] && ok "and writes to every content, content 0's new primary included" \
	|| notok "writing after a failover" "$out"

###############################################################################
echo "6. a transaction open across a failover fails, even where the old primary still answers"
###############################################################################
# content 2's primary answers the dispatcher's connection it has, but no new
# one of FTS's: fts_session_reset's fault.
session_open
session_send "BEGIN;"
session_send "INSERT INTO t SELECT i, 'open' FROM generate_series(401, 500) i;"
session_send "SELECT 'inserted';"
wait_for 0 "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction' AND backend_type = 'client backend'" 1 30 >/dev/null
q 0 "SELECT gp_inject_fault_infinite('fts_conn_startup_packet', 'error', 4)" > /dev/null
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out=$(config 2)
session_send "INSERT INTO t SELECT i, 'open' FROM generate_series(501, 600) i;"
session_send "END;"
session_send "SELECT count(*) FROM t WHERE b = 'open';"
session_close
out2=$(tr '\n' '|' < "$ROOT/s.out")
[ "$out" = "7:pmnu 4:mpnd" ] \
	&& [[ "$out2" == *"inserted|"*"ERROR:  gang was lost due to cluster reconfiguration|"*"0|" ]] \
	&& ok "content 2 fails over, and the open transaction's next statement fails, and none of it commits" \
	|| notok "a transaction across a failover" "$out / $out2"
q 0 "SELECT gp_inject_fault('fts_conn_startup_packet', 'reset', 4)" > /dev/null
stop_node 3 immediate

###############################################################################
echo "7. the coordinator restarts, and keeps what FTS found"
###############################################################################
stop_node 0 fast
start_node 0
out="$(config 0) $(config 1) $(config 2)"
out2=$(q 0 "SELECT count(*) FROM t")
[ "$out" = "5:pmnu 2:mpnd 3:ppsu 6:mmsu 7:pmnu 4:mpnd" ] && [ "$out2" = "400" ] \
	&& ok "gp_segment_configuration is what it was, read back from gpsegconfig_dump, and dispatch goes to the new primaries" \
	|| notok "the configuration across a restart" "$out / $out2"

###############################################################################
echo "8. a failed primary comes back as its mirror's mirror, and then as the primary again"
###############################################################################
# What gprecoverseg -F does: a new copy of the new primary, streaming from it.
recover() {					# recover <n> <of n>
	make_mirror "$1" "$2" \
		&& wait_for "$2" "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1 >/dev/null \
		|| notok "node $1 recovered as the mirror of node $2" "$(tail -3 "$ROOT/basebackup$1.log") $(tail -3 "$(logfile "$1")")"
}
recover 1 4
recover 3 6
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out="$(config 0) $(config 2)"
[ "$out" = "5:pmsu 2:mpsu 7:pmsu 4:mpsu" ] \
	&& ok "recovered: the old primaries are up, mirrors of the new ones, in sync ($out)" \
	|| notok "a recovered primary" "$out"

# What gprecoverseg -r does: stop the primaries that are not preferred ones,
# which FTS fails over from, and once the preferred ones have been promoted --
# PostgreSQL's startup process takes up to wal_retrieve_retry_interval to act
# on it -- recover them as mirrors.
stop_node 4 immediate; stop_node 6 immediate
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
wait_for 1 "SELECT pg_is_in_recovery()" f 30 >/dev/null || notok "node 1 promoted"
wait_for 3 "SELECT pg_is_in_recovery()" f 30 >/dev/null || notok "node 3 promoted"
recover 4 1
recover 6 3
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out="$(config 0) $(config 1) $(config 2)"
out2=$(q 0 "SELECT count(*) FROM t")
[ "$out" = "2:ppsu 5:mmsu 3:ppsu 6:mmsu 4:ppsu 7:mmsu" ] && [ "$out2" = "400" ] \
	&& ok "rebalanced: every node in its preferred role, every pair in sync, every row there" \
	|| notok "rebalancing" "$out / $out2"

###############################################################################
echo "9. a primary whose mirror is not in sync is not failed over from: a double fault"
###############################################################################
q 3 "ALTER SYSTEM SET gp.fts_mark_mirror_down_grace_period = 0" > /dev/null
q 3 "SELECT pg_reload_conf()" > /dev/null
stop_node 6 fast
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out=$(config 2)
stop_node 3 immediate
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out2=$(config 2)
out3=$(grep -c "FTS double fault detected (content=2) primary dbid=4, mirror dbid=7" "$(logfile 0)")
[ "$out" = "4:ppnu 7:mmnd" ] && [ "$out2" = "4:ppnu 7:mmnd" ] && [ "$out3" -ge 1 ] \
	&& ok "the mirror marked down, the primary stops, and FTS promotes nothing, saying why" \
	|| notok "a double fault" "$out / $out2 / $out3"

echo
echo "M4 FTS tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
