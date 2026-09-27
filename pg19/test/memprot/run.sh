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
# M6: memory protection -- Cloudberry's vmem tracker, red zone and runaway
# cleaner, which gp_resource runs on O25, the block allocation hook
# (memprot.c).
#
# A coordinator and two segments, started again with each limit under test,
# as Cloudberry's tests set theirs with gpconfig and gpstop.  What is checked
# is each of Cloudberry's refusals, in its words, and when it comes: a
# segment's process that cannot take the memory it starts with, 12 MB, is
# refused as it connects -- the node's limit, gp.vmem_protect_limit, is its
# sessions' together; a statement past its session's limit on a node,
# gp.vmem_limit_per_query, is refused as it takes the block, and so is one
# past the node's; past the red zone the session that holds the most is
# cancelled, and cleans up after itself; the coordinator's processes are
# counted and never refused; and a session's memory is given back as it
# ends.  isolation2's oom_startup_memory, whose two scenarios the port's
# dispatch does not bring about, says what it would have made happen
# (../isolation2/cloudberry/README): these are those refusals.
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-memprot-XXXXXX")"
BASEPORT="${PGPORT:-$((6500 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"
PRELOAD='gp_core,gp_sql,gp_resource'

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

# node <n>: 0 is the coordinator, 1 and 2 the segments.
datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }

BG=()
cleanup() {
	for p in "${BG[@]}"; do kill "$p" 2> /dev/null; done
	for n in 0 1 2; do
		"$BINDIR/pg_ctl" -D "$(datadir $n)" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
}
trap cleanup EXIT

# The cluster, started again with these settings on every node
restart() {					# restart [postgresql.auto.conf lines...]
	local n
	for n in 0 1 2; do
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m fast -w stop > /dev/null 2>&1
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(sockdir "$n")'"
			echo "listen_addresses = ''"
			echo "port = $(port "$n")"
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "max_prepared_transactions = 16"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
			for line in "$@"; do echo "$line"; done
		} > "$(datadir "$n")/postgresql.auto.conf"
	done
	for n in 1 2 0; do
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 60 start > /dev/null 2>&1 ||
			{ echo "node $n did not start"; tail -5 "$ROOT/node$n.log"; return 1; }
	done
}

q() {						# q <n> <sql>: one session's answer, and its errors
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres -c "$2" 2>&1
}

# Each segment's view of the sessions' memory, as gp_toolkit reads it
ENTRIES="gp_toolkit.session_state_memory_entries_f_on_segments() AS c(segid int, sessionid int, vmem_mb int, runaway_status int, qe_count int, active_qe_count int, dirty_qe_count int, runaway_vmem_mb int, runaway_command_cnt int, idle_start timestamptz)"

echo "memprot: Cloudberry's memory protection, on a coordinator and two segments"
echo

for n in 0 1 2; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 ||
		{ echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
done
{
	echo "# dbid content role host port datadir"
	for n in 0 1 2; do
		echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done
} > "$CONF"

###############################################################################
echo "1. every process of a session is counted, where it runs"
###############################################################################
restart || exit 1
out=$(q 0 "CREATE EXTENSION gp_core; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_resource;")
[ -z "$out" ] && ok "gp_core, gp_sql and gp_resource are created" || notok "the extensions" "$out"

out=$(q 0 "SELECT segid, qe_count, active_qe_count, dirty_qe_count, runaway_status, vmem_mb >= 12 FROM $ENTRIES WHERE sessionid = pg_backend_pid() ORDER BY 1")
[ "$out" = "$(printf '0|1|1|-1|0|t\n1|1|1|-1|0|t')" ] &&
	ok "on each segment the session's process has its 12 MB and runs a statement ($(echo $out))" ||
	notok "the session on the segments" "$out"

out=$(q 0 "SELECT segid, qe_count, vmem_mb >= 12 FROM gp_resource.session_state_memory_entries() WHERE sessionid = pg_backend_pid()")
[ "$out" = "-1|1|t" ] && ok "and on the coordinator ($out)" || notok "the session on the coordinator" "$out"

old=$("$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
	-c "SELECT count(*) FROM gp_dist_random('gp_id')" -c "SELECT pg_backend_pid()" 2>&1 | tail -1)
for _ in $(seq 50); do
	out=$(q 0 "SELECT count(*) FROM $ENTRIES WHERE sessionid = $old")
	[ "$out" = "0" ] && break
	sleep 0.1
done
[ "$out" = "0" ] && ok "a session's memory on the segments is given back as it ends" ||
	notok "an ended session's state" "$out"

out=$(q 0 "SELECT length(repeat('x', 100000000))")
[ "$out" = "100000000" ] && ok "the coordinator takes 100 MB where the node's limit is 8192 MB" ||
	notok "100 MB on the coordinator" "$out"

###############################################################################
echo "2. the node's limit: a process that cannot take its 12 MB is refused"
###############################################################################
restart "gp.vmem_protect_limit = 60" "gp.runaway_detector_activation_percent = 0" || exit 1

# Five sessions, 12 MB each on each segment, hold their processes there.
for i in 1 2 3 4 5; do
	"$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "SELECT count(*) FROM gp_dist_random('gp_id')" -c "SELECT pg_sleep(60)" \
		> "$ROOT/holder$i.out" 2>&1 &
	BG+=($!)
done
# A session's first statement answers once its processes on both segments
# have connected -- what counts here, and not a connection's name: the
# coordinator's own processes connect to the segments too, just after a start.
n=0
for _ in $(seq 150); do
	n=$(cat "$ROOT"/holder*.out 2> /dev/null | grep -c '^2$')
	[ "$n" = 5 ] && break
	sleep 0.2
done
[ "$n" = 5 ] && ok "five sessions hold a process on each segment" || notok "five sessions' processes" "$n"

out=$(q 0 "SELECT count(*) FROM gp_dist_random('gp_id')")
case "$out" in
	*"FATAL:  Out of memory"*"Vmem limit reached, failed to allocate 12582912 bytes from tracker, which has 0 MB available"*)
		ok "the sixth is refused as it connects: Vmem limit reached, failed to allocate 12582912 bytes from tracker" ;;
	*) notok "the sixth session's process" "$out" ;;
esac

out=$(for i in 1 2 3 4 5 6 7 8; do q 0 "SELECT 1"; done | sort | uniq -c | tr -s ' ')
[ "$out" = " 8 1" ] && ok "the coordinator refuses none: eight more sessions run there" ||
	notok "sessions on the coordinator" "$out"

for p in "${BG[@]}"; do kill "$p" 2> /dev/null; done
wait 2> /dev/null
BG=()

###############################################################################
echo "3. a statement's limit on a node"
###############################################################################
restart "gp.vmem_limit_per_query = '20MB'" "gp.runaway_detector_activation_percent = 0" || exit 1

out=$(q 0 "SELECT length(repeat('x', (random() * 0)::int + 4000000)) FROM gp_dist_random('gp_id')")
[ "$out" = "$(printf '4000000\n4000000')" ] && ok "4 MB on each segment is within 20 MB, 12 of them the process's own" ||
	notok "4 MB on each segment" "$out"

out=$("$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
	-c "SELECT length(repeat('x', (random() * 0)::int + 10000000)) FROM gp_dist_random('gp_id')" \
	-c "SELECT length(repeat('x', (random() * 0)::int + 4000000)) FROM gp_dist_random('gp_id')" 2>&1)
case "$out" in
	*"ERROR:  Out of memory"*"Per-query memory limit reached: current limit is 20480 kB, requested 10000064 bytes, has "*" MB available for this query"*)
		ok "10 MB is refused: Per-query memory limit reached: current limit is 20480 kB, requested 10000064 bytes" ;;
	*) notok "10 MB on each segment" "$out" ;;
esac
case "$out" in
	*"$(printf '4000000\n4000000')") ok "and the session's next statement runs" ;;
	*) notok "the statement after the refusal" "$out" ;;
esac

###############################################################################
echo "4. the node's limit, for one statement"
###############################################################################
restart "gp.vmem_protect_limit = 100" "gp.runaway_detector_activation_percent = 0" || exit 1

out=$(q 0 "SELECT length(repeat('x', (random() * 0)::int + 95000000)) FROM gp_dist_random('gp_id')")
case "$out" in
	*"ERROR:  Out of memory"*"Vmem limit reached, failed to allocate 95000064 bytes from tracker, which has "*" MB available"*)
		ok "95 MB is refused: Vmem limit reached, failed to allocate 95000064 bytes from tracker" ;;
	*) notok "95 MB on each segment" "$out" ;;
esac

###############################################################################
echo "5. the red zone: the session that holds the most is cancelled"
###############################################################################
restart "gp.vmem_protect_limit = 100" "gp.runaway_detector_activation_percent = 50" || exit 1

out=$(q 0 "CREATE FUNCTION eat(mb int) RETURNS int AS \$\$
	DECLARE s text := '';
	BEGIN
		FOR i IN 1..mb LOOP s := s || repeat('x', 1000000); END LOOP;
		RETURN length(s);
	END \$\$ LANGUAGE plpgsql VOLATILE")
[ -z "$out" ] && ok "eat(mb): a string that grows by 1 MB a step, and is copied at each" ||
	notok "CREATE FUNCTION eat" "$out"

out=$("$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
	-c "SELECT eat(40) FROM gp_dist_random('gp_id')" \
	-c "SELECT runaway_status, dirty_qe_count FROM $ENTRIES WHERE sessionid = pg_backend_pid()" \
	-c "SELECT eat(10) FROM gp_dist_random('gp_id')" 2>&1)
case "$out" in
	*"ERROR:  Canceling query because of high VMEM usage. Used: "*"MB, available "*"MB, red zone: 50MB"*)
		ok "past 50 MB, it is cancelled: Canceling query because of high VMEM usage ... red zone: 50MB" ;;
	*) notok "40 MB on each segment, past the red zone" "$out" ;;
esac
case "$out" in
	*"$(printf '0|-1\n0|-1\n10000000\n10000000')") ok "and cleans up: no longer a runaway, and its next statement runs" ;;
	*) notok "the runaway's session after its cancel" "$out" ;;
esac

out=$(q 0 "SELECT eat(20) FROM gp_dist_random('gp_id')")
[ "$out" = "$(printf '20000000\n20000000')" ] && ok "another session is not a runaway below the red zone" ||
	notok "20 MB after the runaway" "$out"

###############################################################################
echo "6. a runaway that takes no more memory is sent a cancel"
###############################################################################
# Session A holds 25 MB on each segment and sleeps; session B then takes a
# megabyte at a time: the node is past its red zone, A holds the most and is
# flagged, and A, which takes no chunk to cancel itself at, is sent a cancel a
# second later, by B as B takes a chunk.  B, below the red zone once A is
# gone, runs to its end.
out=$(q 0 "CREATE FUNCTION hold(mb int, secs float8) RETURNS int AS \$\$
	DECLARE s text := repeat('x', mb * 1000000);
	BEGIN
		PERFORM pg_sleep(secs);
		RETURN length(s);
	END \$\$ LANGUAGE plpgsql VOLATILE;
	CREATE FUNCTION eat_slowly(mb int, pause float8) RETURNS int AS \$\$
	DECLARE a text[];
	BEGIN
		FOR i IN 1..mb LOOP a[i] := repeat('x', 1000000); PERFORM pg_sleep(pause); END LOOP;
		RETURN array_length(a, 1);
	END \$\$ LANGUAGE plpgsql VOLATILE")
[ -z "$out" ] && ok "hold(mb, secs), and eat_slowly(mb, pause), a megabyte a pause" ||
	notok "CREATE FUNCTION hold, eat_slowly" "$out"

q 0 "SELECT hold(25, 10) FROM gp_dist_random('gp_id')" > "$ROOT/holder.out" 2>&1 &
holder=$!
BG+=($holder)
sleep 1
out=$(q 0 "SELECT eat_slowly(15, 0.15) FROM gp_dist_random('gp_id')")
wait "$holder" 2> /dev/null
held=$(cat "$ROOT/holder.out")
case "$held" in
	*"ERROR:  canceling MPP operation"*)
		ok "A, flagged and asleep, is cancelled, in the words of Cloudberry's segment" ;;
	*) notok "the sleeping runaway" "$held" ;;
esac
[ "$out" = "$(printf '15\n15')" ] && ok "and B takes its 15 MB" || notok "B's 15 MB" "$out"
out=$(q 0 "SELECT count(*) FROM $ENTRIES WHERE runaway_status <> 0 OR dirty_qe_count <> -1")
[ "$out" = "0" ] && ok "no session is a runaway after A's cancel" || notok "the sessions after the cancel" "$out"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
