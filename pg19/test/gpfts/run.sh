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
# M8: gpfts, the coordinator's automatic failover.
#
# A coordinator with a standby -- a hot standby streaming from it as
# gp_walreceiver -- and three primaries, each with a mirror; an etcd, started
# here; and gpfts (pg19/bin/gpfts).  What this checks: that gpfts keeps in
# etcd the cluster as the coordinator shows it, and whether the standby may be
# promoted; that it does not promote a standby that was not streaming; that
# the nodes' states the coordinator's FTS finds reach the standby in WAL
# (gp_cluster.c); that of two instances one leads, and the other takes over
# when the leader dies; and that when the coordinator stops, the leader
# promotes the standby and makes it the coordinator, which then serves the
# cluster with the segments' states as the old coordinator had them -- a
# distributed read, a write to every segment -- once its distributed
# transaction recovery has reached every segment.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/gpfts/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
GPFTS="$BINDIR/gpfts"
ETCD="$(command -v etcd)"
if [ ! -x "$GPFTS" ] || [ -z "$ETCD" ]; then
	echo "gpfts, or etcd, is not installed; skipping"
	exit 77
fi

PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-gpfts-XXXXXX")"
BASEPORT="${PGPORT:-$((6500 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"
NSEG=3
STANDBY=$((2 * NSEG + 1))
PRELOAD='gp_core,gp_sql'
SLOT=internal_wal_replication_slot

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/         /' | head -20; fail=$((fail + 1)); }

# node <n>: 0 the coordinator, 1..3 the primaries of contents 0..2, 4..6
# their mirrors, 7 the coordinator's standby.
datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }
dbid()    { echo $(($1 + 1)); }
content() {
	if [ "$1" -eq 0 ] || [ "$1" -eq "$STANDBY" ]; then echo -1
	elif [ "$1" -le "$NSEG" ]; then echo $(($1 - 1))
	else echo $(($1 - NSEG - 1)); fi
}
logfile() { echo "$ROOT/node$1.log"; }

# Two TCP ports nothing listens on, for etcd's clients and its peers: the
# nodes take connections on their sockets alone.
free_port() {
	local p
	for _ in $(seq 50); do
		p=$((30000 + RANDOM % 10000))
		ss -Htan "sport = :$p" 2> /dev/null | grep -q . || { echo "$p"; return; }
	done
}
EPORT=$(free_port)
PPORT=$(free_port)
while [ "$PPORT" = "$EPORT" ]; do PPORT=$(free_port); done
ETCD_PID=
FTS_PIDS=()

cleanup() {
	for pid in "${FTS_PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
	for n in $(seq 0 "$STANDBY"); do
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop >/dev/null 2>&1
	done
	[ -n "$ETCD_PID" ] && kill "$ETCD_PID" 2>/dev/null && wait "$ETCD_PID" 2>/dev/null
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/gpfts"
		cp "$ROOT"/node*.log "$ROOT"/etcd.log "$RESULTS_DIR/gpfts/" 2>/dev/null
		for d in "$ROOT"/fts-*; do
			[ -d "$d" ] && cp "$d/fts.csv" "$RESULTS_DIR/gpfts/$(basename "$d").csv" 2>/dev/null
		done
	fi
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
}
trap cleanup EXIT

q() {						# q <n> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-c "$2" 2>&1
}

# Poll until the query gives what is wanted, or seconds pass: the last
# statement's answer, where the statements before it -- asking FTS to probe --
# answer too.
wait_for() {				# wait_for <n> <sql> <want> [seconds]
	local n="$1" sql="$2" want="$3" t="${4:-60}" out
	for _ in $(seq $((t * 5))); do
		out=$(q "$n" "$sql" | tail -1)
		[ "$out" = "$want" ] && return 0
		sleep 0.2
	done
	echo "$out"
	return 1
}

# Poll until the file has a line matching the pattern, or seconds pass.
wait_log() {				# wait_log <file> <pattern> [seconds]
	for _ in $(seq $((${3:-60} * 5))); do
		grep -q -- "$2" "$1" 2>/dev/null && return 0
		sleep 0.2
	done
	return 1
}

start_node() {
	"$BINDIR/pg_ctl" -D "$(datadir "$1")" -l "$(logfile "$1")" -w -t 60 start >/dev/null 2>&1
}
stop_node() {				# stop_node <n> [mode]
	"$BINDIR/pg_ctl" -D "$(datadir "$1")" -m "${2:-fast}" -w -t 60 stop >/dev/null 2>&1
}

# What gp_segment_configuration says of a content, on the node given: each
# node's dbid, role, preferred role, mode and status, the primary first.
config() {					# config <n> <content>
	q "$1" "SELECT string_agg(dbid || ':' || role::text || preferred_role::text || mode::text || status::text, ' ' ORDER BY role DESC, dbid)
	         FROM gp_segment_configuration WHERE content = $2"
}

# A node made a copy of another, taken with pg_basebackup, a hot standby
# streaming from it as gp_walreceiver from the slot: a primary's mirror, or
# the coordinator's standby.  No tablespace is made, whose location a copy
# on the same host would share.
make_copy() {				# make_copy <n> <of n>
	local m="$1" p="$2"
	rm -rf "$(datadir "$m")"
	mkdir -p "$(sockdir "$m")"
	"$BINDIR/pg_basebackup" -D "$(datadir "$m")" -h "$(sockdir "$p")" -p "$(port "$p")" \
		-X stream -c fast -C -S "$SLOT" > "$ROOT/basebackup$m.log" 2>&1 || return 1
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

# gpfts, as the tests run it: its etcd configuration, a log of its own, and
# probes a second apart, of two seconds, two retries; a lease of twelve
# seconds, more than the ten an instance's request for the lock waits.
FTS_CONF="$ROOT/etcd.conf"
FTS_OPTIONS=(-F "$FTS_CONF" -I 1 -T 2 -R 2 -u 12)
gpfts() {					# gpfts <log name> <options...>
	local name="$1"; shift
	"$GPFTS" "${FTS_OPTIONS[@]}" -d "$ROOT/fts-$name" "$@"
}
fts_log() { echo "$ROOT/fts-$1/fts.csv"; }
fts_start() {				# fts_start <name>: an instance in the background
	"$GPFTS" "${FTS_OPTIONS[@]}" -d "$ROOT/fts-$1" > "$ROOT/fts-$1.out" 2>&1 &
	FTS_PIDS+=($!)
	eval "FTS_PID_$1=$!"
}

echo "M8 gpfts, the coordinator's automatic failover"
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
	for n in $(seq 0 "$STANDBY"); do
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
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
done

###############################################################################
echo "1. the cluster: a coordinator with a standby, and three primaries each with a mirror"
###############################################################################
started=1
for n in $(seq 1 "$NSEG"); do
	start_node "$n" || { notok "node $n starts" "$(tail -5 "$(logfile "$n")")"; started=0; }
done
for n in $(seq 1 "$NSEG"); do
	make_copy $((n + NSEG)) "$n" \
		|| { notok "the mirror of node $n" "$(tail -5 "$ROOT/basebackup$((n + NSEG)).log")"; started=0; }
done
start_node 0 || { notok "the coordinator starts" "$(tail -5 "$(logfile 0)")"; started=0; }
[ "$started" -eq 1 ] || exit 1
out=$(q 0 "CREATE EXTENSION gp_core")
make_copy "$STANDBY" 0 \
	|| { notok "the standby" "$(tail -5 "$ROOT/basebackup$STANDBY.log")"; exit 1; }

out2=$(wait_for 0 "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1)
out3=$(wait_for 0 "SELECT gp_request_fts_probe_scan(); SELECT count(*) FROM gp_segment_configuration WHERE content >= 0 AND NOT (mode = 's' AND status = 'u')" 0 60)
[ -z "$out" ] && [ -z "$out2" ] && [ -z "$out3" ] \
	&& ok "the standby streams from the coordinator as gp_walreceiver, and every pair is in sync" \
	|| notok "the cluster" "$out / $out2 / $out3"

out=$(q 0 "CREATE TABLE t (a int, b text) DISTRIBUTED BY (a);
           INSERT INTO t SELECT i, 'before' FROM generate_series(1, 300) i;
           SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t;")
[ "$out" = "3:300" ] && ok "a table's rows are on the three segments" \
	|| notok "a distributed table" "$out"

###############################################################################
echo "2. etcd, and gpfts's configuration in it"
###############################################################################
mkdir -p "$ROOT/etcd"
"$ETCD" --name gpfts-test --data-dir "$ROOT/etcd" \
	--listen-client-urls "http://127.0.0.1:$EPORT" --advertise-client-urls "http://127.0.0.1:$EPORT" \
	--listen-peer-urls "http://127.0.0.1:$PPORT" --initial-advertise-peer-urls "http://127.0.0.1:$PPORT" \
	--initial-cluster "gpfts-test=http://127.0.0.1:$PPORT" > "$ROOT/etcd.log" 2>&1 &
ETCD_PID=$!
{
	echo "gp_etcd_endpoints='127.0.0.1:$EPORT'"
	echo "gp_etcd_account_id='00000000-0000-0000-0000-000000000000'"
	echo "gp_etcd_cluster_id='00000000-0000-0000-0000-000000000000'"
	echo "gp_etcd_namespace='gpfts-test'"
} > "$FTS_CONF"
up=0
for _ in $(seq 100); do
	curl -s -X POST "http://127.0.0.1:$EPORT/v3/maintenance/status" -d '{}' 2>/dev/null | grep -q leader && { up=1; break; }
	sleep 0.2
done
[ "$up" -eq 1 ] && ok "etcd runs, at 127.0.0.1:$EPORT" \
	|| { notok "etcd starts" "$(tail -5 "$ROOT/etcd.log")"; exit 1; }

# The cluster as the coordinator shows it, loaded into etcd as Cloudberry's
# gpfts is given it: its warp call 1.
q 0 "SELECT dbid, content, role, preferred_role, mode, status, port, hostname, address, datadir
     FROM gp_segment_configuration ORDER BY dbid" | tr '|' ' ' > "$ROOT/cluster.txt"
out=$(gpfts tools -W 1 -L "$ROOT/cluster.txt" 2>&1)
out2=$(gpfts tools -W 2 2>&1)
[ "$out" = "Success dump FTS file into ETCD." ] && [ "$out2" = "$(cat "$ROOT/cluster.txt")" ] \
	&& ok "gpfts -W 1 loads the configuration into etcd, and -W 2 shows it" \
	|| notok "gpfts -W 1 and -W 2" "$out / $out2"

out=$(gpfts tools -W 4 2>&1)
[ "$out" = "The coordinator (dbid=1) at $(sockdir 0):$(port 0) answers: standby up t, in sync t, in recovery f." ] \
	&& ok "gpfts -W 4 probes the coordinator etcd names: its standby up and in sync" \
	|| notok "gpfts -W 4" "$out"

# One round, without the lock: the flag written from the coordinator's answer.
gpfts once -A > "$ROOT/fts-once.out" 2>&1
out=$(gpfts tools -W 5 2>&1)
[ "$out" = "1" ] && ok "one round (-A) says the standby may be promoted: it streams" \
	|| notok "standby promote ready after a round" "$out / $(tail -3 "$(fts_log once)")"

###############################################################################
echo "3. a standby that does not stream is not promoted"
###############################################################################
stop_node "$STANDBY" fast
wait_for 0 "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver'" 0 > /dev/null
gpfts once -A > /dev/null 2>&1
out=$(gpfts tools -W 5 2>&1)
stop_node 0 immediate
gpfts once -A > /dev/null 2>&1
out2=$(grep -c "double fault detected (content=-1) primary dbid=1, mirror dbid=$(dbid "$STANDBY"): ignoring promote request, standby not ready to promote" "$(fts_log once)")
out3=$(gpfts tools -W 2 2>&1 | awk '$2 == -1 {print $1 ":" $3}' | tr '\n' ' ')
[ "$out" = "0" ] && [ "$out2" -ge 1 ] && [ "$out3" = "1:p 8:m " ] \
	&& ok "with the standby stopped the flag is 0, and when the coordinator stops too, gpfts promotes nothing, saying why" \
	|| notok "a standby not ready" "$out / $out2 / $out3"
start_node 0 && start_node "$STANDBY"
out=$(wait_for 0 "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1)
gpfts once -A > /dev/null 2>&1
out2=$(gpfts tools -W 5 2>&1)
[ -z "$out" ] && [ "$out2" = "1" ] \
	&& ok "the two started again: the standby streams, and may be promoted again" \
	|| notok "the coordinator and its standby started again" "$out / $out2"

###############################################################################
echo "4. the nodes' states the coordinator's FTS finds reach the standby, in WAL"
###############################################################################
stop_node 1 immediate
q 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
out=$(config 0 0)
out2=$(wait_for "$STANDBY" "SELECT string_agg(dbid || ':' || role::text || preferred_role::text || mode::text || status::text, ' ' ORDER BY role DESC, dbid)
                            FROM gp_segment_configuration WHERE content = 0" "5:pmnu 2:mpnd" 10)
[ "$out" = "5:pmnu 2:mpnd" ] && [ -z "$out2" ] \
	&& cmp -s "$(datadir 0)/gpsegconfig_dump" "$(datadir "$STANDBY")/gpsegconfig_dump" \
	&& ok "content 0 failed over to its mirror, and the standby's gpsegconfig_dump and gp_segment_configuration say so too ($out)" \
	|| notok "the states on the standby" "$out / $out2 / $(diff "$(datadir 0)/gpsegconfig_dump" "$(datadir "$STANDBY")/gpsegconfig_dump")"
wait_for 4 "SELECT pg_is_in_recovery()" f 30 > /dev/null
out=$(q 0 "INSERT INTO t SELECT i, 'after the segment failover' FROM generate_series(301, 400) i; SELECT count(*) FROM t")
[ "$out" = "400" ] && ok "a write after it, to every content" || notok "a write after the segment failover" "$out"
before="$(config 0 0) $(config 0 1) $(config 0 2)"

###############################################################################
echo "5. two instances: one leads, the other takes over when the leader dies"
###############################################################################
fts_start a
fts_start b
wait_log "$(fts_log a)" "successfully retrieve FTS lock" 30 || wait_log "$(fts_log b)" "successfully retrieve FTS lock" 30
sleep 3
leaders=$(grep -l "successfully retrieve FTS lock" "$(fts_log a)" "$(fts_log b)" 2>/dev/null | wc -l)
if grep -q "successfully retrieve FTS lock" "$(fts_log a)" 2>/dev/null; then
	leader=a; follower=b
else
	leader=b; follower=a
fi
[ "$leaders" -eq 1 ] && eval "kill -0 \$FTS_PID_$follower" \
	&& ok "one of the two holds the lock, and the other waits for it ($leader leads)" \
	|| notok "a leader" "$leaders leaders"
out=$(wait_log "$(fts_log "$leader")" "Already updated segment infos\|standby promote ready" 10; gpfts tools -W 5 2>&1)
[ "$out" = "1" ] && ok "the leader probes, the standby ready" || notok "the leader's probes" "$out"

eval "kill -9 \$FTS_PID_$leader; wait \$FTS_PID_$leader" 2>/dev/null
wait_log "$(fts_log "$follower")" "successfully retrieve FTS lock" 60 \
	&& ok "the leader killed, the other takes the lock once the lease has lapsed" \
	|| notok "a new leader" "$(tail -5 "$(fts_log "$follower")")"

###############################################################################
echo "6. the coordinator stops: gpfts promotes its standby and makes it the coordinator"
###############################################################################
stop_node 0 immediate
out=$(wait_for "$STANDBY" "SELECT pg_is_in_recovery()::text || ' ' || (SELECT string_agg(dbid || ':' || role::text || preferred_role::text, ' ') FROM gp_segment_configuration WHERE content = -1)" \
	"false $(dbid "$STANDBY"):pp" 90)
[ $? -eq 0 ] && ok "the standby is promoted, and is the coordinator of the cluster file" \
	|| notok "the promotion" "$out / $(tail -5 "$(fts_log "$follower")")"
out=$(wait_for "$STANDBY" "SELECT gp.dtx_recovered()" t 60)
log=$(fts_log "$follower")
[ -z "$out" ] && grep -q "promotion triggered successfully" "$log" &&
	grep -q "distributed transaction recovery of the new coordinator (dbid=$(dbid "$STANDBY")) has reached every segment" "$log" \
	&& ok "gpfts waited for its distributed transaction recovery to reach every segment" \
	|| notok "the new coordinator's recovery" "$out / $(grep -E "FTS|promot" "$log" | tail -5)"

after="$(config "$STANDBY" 0) $(config "$STANDBY" 1) $(config "$STANDBY" 2)"
[ "$after" = "$before" ] \
	&& ok "the segments' states are as the old coordinator had them ($after)" \
	|| notok "the segments' states on the new coordinator" "$before / $after"

out=$(q "$STANDBY" "SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t")
out2=$(q "$STANDBY" "INSERT INTO t SELECT i, 'on the new coordinator' FROM generate_series(401, 500) i;
                     SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t WHERE b = 'on the new coordinator'")
[ "$out" = "3:400" ] && [ "$out2" = "3:100" ] \
	&& ok "the cluster serves from it: a distributed read of every row, and a write to every segment" \
	|| notok "the new coordinator's reads and writes" "$out / $out2"

for _ in $(seq 150); do
	out=$(gpfts tools -W 2 2>&1 | awk '$2 == -1 {print $1 ":" $3 $4}' | tr '\n' ' ')
	[ "$out" = "$(dbid "$STANDBY"):pp " ] && break
	sleep 0.2
done
out2=$(gpfts tools -W 5 2>&1)
[ "$out" = "$(dbid "$STANDBY"):pp " ] && [ "$out2" = "0" ] \
	&& ok "etcd names the new coordinator, which has no standby yet" \
	|| notok "etcd after the failover" "$out / $out2"

eval "kill \$FTS_PID_$follower"
eval "wait \$FTS_PID_$follower"
grep -q "FTS released its lock" "$log" \
	&& ok "gpfts stopped gives its lock back" \
	|| notok "gpfts stopped" "$(tail -3 "$log")"

echo
echo "M8 gpfts tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
