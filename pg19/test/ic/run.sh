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
# ic: the interconnect's transports, each carrying the Motions of the same
# statements -- a Redistribute, a Broadcast, a Gather to one segment, a
# merge, sixty thousand rows through a small window, a LIMIT that stops its
# senders, a slice whose plan stops reading a Motion in its middle, a cursor
# fetched across another statement, a slice that fails, and statements
# cancelled while their rows are on the way -- whose rows
# must be the planner's, which streams nothing.  So a transport that has
# regressed shows in every run, where the greenplum and isolation2 suites
# run over udp2 and the proxy only on request (../ic_udp2, ../ic_proxy).
#
# A coordinator and two segments, with gp_orca, whose plans alone stream,
# and the modules of the transports built: udp2, and interconnect, the
# proxy's, whose proxies listen on TCP at their nodes' ports and
# IC_PROXY_OFFSET more.  The coordinator's gp.interconnect_type decides
# which transport carries a statement's Motions on every node
# (gp_motion.c), so a session's SET makes it every node's.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/ic/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
PKGLIB="$("$BINDIR/pg_config" --pkglibdir)"

PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ ! -f "$PKGLIB/gp_orca.so" ]; then
	echo "  skipped: gp_orca, whose plans alone stream, is not built"
	exit 77
fi

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-ic-XXXXXX")"
BASEPORT="${PGPORT:-$((6400 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"
IC_PROXY_OFFSET=17000
SECRET="ic-secret-$RANDOM$RANDOM$RANDOM"

PRELOAD='gp_core,gp_sql,gp_orca'
[ -f "$PKGLIB/udp2.so" ] && PRELOAD="$PRELOAD,udp2"
[ -f "$PKGLIB/interconnect.so" ] && PRELOAD="$PRELOAD,interconnect"

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }

cleanup() {
	for n in 0 1 2; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/ic-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir $n)" -m immediate stop >/dev/null 2>&1
	done
	rm -rf "$ROOT"
}
trap cleanup EXIT

# Every node, as Cloudberry's gp_interconnect_proxy_addresses lists them.
PROXIES="1:-1:127.0.0.1:$((BASEPORT + IC_PROXY_OFFSET)),2:0:127.0.0.1:$((BASEPORT + 1 + IC_PROXY_OFFSET)),3:1:127.0.0.1:$((BASEPORT + 2 + IC_PROXY_OFFSET))"

start_node() {				# start_node <n> [extra postgresql.auto.conf lines]
	local n="$1"; shift
	local d; d="$(datadir "$n")"

	"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = '$SECRET'"
		echo "max_prepared_transactions = 16"
		echo "fsync = off"
		[[ "$PRELOAD" == *interconnect* ]] && echo "gp.interconnect_proxy_addresses = '$PROXIES'"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		for line in "$@"; do echo "$line"; done
	} > "$d/postgresql.auto.conf"
	"$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node$n.log" -w -t 30 start >/dev/null 2>&1
}

q() {						# q <n> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-c "$2" 2>&1
}
qf() {						# qf <n>, statements on stdin, in one session
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-f - 2>&1
}

echo "the interconnect's transports"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

for n in 0 1 2; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 \
		> "$ROOT/initdb$n.log" 2>&1 || { echo "initdb failed for node $n"; exit 1; }
done
{
	echo "# dbid content role host port datadir"
	for n in 0 1 2; do
		echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done
} > "$CONF"

###############################################################################
echo "1. the cluster, and the transports it has"
###############################################################################
started=1
for n in 1 2 0; do
	start_node "$n" || started=0
done
out=$(q 0 "CREATE EXTENSION gp_core; CREATE EXTENSION gp_orca;")
if [ "$started" -eq 0 ] || [ -n "$out" ]; then
	notok "the cluster starts" "$out $(tail -5 "$ROOT/node0.log")"
	echo; echo "  $pass passed, $fail failed"; exit 1
fi
ok "the cluster starts, with $PRELOAD"

# tcp, udpifc and relay are gp_core's; udp2 and the proxy their modules',
# where they are built.
TRANSPORTS="tcp udpifc"
for t in udp2 proxy; do
	out=$(q 0 "SET gp.interconnect_type = $t; SHOW gp.interconnect_type;")
	if [ "$out" = "$t" ]; then
		TRANSPORTS="$TRANSPORTS $t"
	else
		echo "  (no $t: $out)"
	fi
done
ok "the transports: $TRANSPORTS, and relay"

q 0 "CREATE TABLE o (a int, b int, c text) DISTRIBUTED BY (a);
     INSERT INTO o SELECT i, i % 10, 'v' || i FROM generate_series(1, 1000) i;
     CREATE TABLE po (x int, y int) DISTRIBUTED BY (x);
     INSERT INTO po SELECT i, i % 7 FROM generate_series(1, 500) i;
     CREATE TABLE bo (k int, s text) DISTRIBUTED BY (k);
     INSERT INTO bo SELECT i, md5((i % 20000)::text) FROM generate_series(1, 60000) i;
     CREATE TABLE wu (a int PRIMARY KEY, b int) DISTRIBUTED BY (a);
     INSERT INTO wu SELECT i, i FROM generate_series(1, 100) i;
     CREATE TABLE lr (k int, s text) DISTRIBUTED BY (k);
     ALTER TABLE lr ALTER COLUMN s SET STORAGE EXTERNAL;
     INSERT INTO lr SELECT i, repeat(md5(i::text), 3200) FROM generate_series(1, 20) i;
     ANALYZE o; ANALYZE po; ANALYZE bo; ANALYZE wu; ANALYZE lr;" > /dev/null

###############################################################################
echo "2. the same rows over every transport"
###############################################################################
# Each statement: ORCA's plan has the Motion said, and every transport's rows
# are the planner's.
same() {					# same <what> <what EXPLAIN says, a pattern> <sql, several lines>
	local plan want got bad=""
	plan=$(printf '%s\n' "$3" | sed 's/^\(SELECT\|UPDATE\|DELETE\) /EXPLAIN (COSTS OFF) \1 /' | qf 0)
	want=$(printf '%s\n' "SET gp.optimizer = off;" "$3" | qf 0)
	case "$plan" in
		*$2*"Optimizer: GPORCA"*) ;;
		*) notok "$1: ORCA's plan" "$plan"; return ;;
	esac
	for t in $TRANSPORTS relay; do
		got=$(printf '%s\n' "SET gp.interconnect_type = $t;" "$3" | qf 0)
		[ "$got" = "$want" ] || bad="$bad $t: $got"
	done
	[ -z "$bad" ] && [ -n "$want" ] && ok "$1" || notok "$1" "planner: $want /$bad"
}
same "a Redistribute: a GROUP BY off the key" "Redistribute Motion 2:2" \
	"SELECT b, count(*), sum(a) FROM o GROUP BY b ORDER BY b;"
same "both sides of a join redistributed" "Hash Key: po.y" \
	"SELECT count(*), sum(o.a) FROM o JOIN po ON o.b = po.y;"
same "a Broadcast of the small side" "Broadcast Motion 2:2" \
	"SELECT count(*) FROM o JOIN (SELECT * FROM po WHERE x < 10) s ON o.b = s.y;"
same "two Motions below a Gather" "(slice3; segments: 2)" \
	"SELECT y, count(*), sum(a) FROM o JOIN po ON o.b = po.y GROUP BY y ORDER BY y;"
same "a window partitioned off the key, merged in order" "Merge Key" \
	"SELECT a, rank() OVER (PARTITION BY b ORDER BY a DESC) FROM o WHERE a > 990 ORDER BY b, a;"
same "sixty thousand rows through a window of one small packet" "Redistribute Motion" \
	"SET gp.interconnect_queue_depth = 1; SET gp.max_packet_size = 600;
SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;"
same "a tenth of the packets lost, and a third of the acknowledgements, where a UDP transport loses them" \
	"Redistribute Motion" \
	"SET gp.udpic_dropxmit_percent = 10; SET gp.udpic_dropacks_percent = 30;
SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;"
same "rows of a hundred kilobytes, longer than a UDP receiver's room" "Redistribute Motion" \
	"SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM lr GROUP BY s) x;"
same "a Gather to one segment, which an UPDATE's subquery reads" "Gather Motion 2:1" \
	"BEGIN;
UPDATE wu SET b = (SELECT max(x) FROM po WHERE x < 90) WHERE a < 50;
SELECT count(*), sum(b) FROM wu;
ROLLBACK;"

###############################################################################
echo "3. senders stopped, statements failed and cancelled, and the session goes on"
###############################################################################
want=$(q 0 "SET gp.optimizer = off; SELECT count(*) FROM o JOIN po ON o.b = po.y;")
for t in $TRANSPORTS; do
	# a LIMIT's receivers stop their senders, twice in one session, and the
	# next statement's rows come
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" \
		"SELECT count(*) FROM (SELECT o.a FROM o JOIN po ON o.b = po.y LIMIT 7) x;" \
		"SELECT count(*) FROM (SELECT bo.k FROM bo JOIN po ON bo.k % 7 = po.y LIMIT 3) x;" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y;" | qf 0)
	[ "$out" = "7
3
$want" ] && ok "$t: a LIMIT stops its senders, twice, and the session goes on" \
		|| notok "$t: a LIMIT above streaming slices" "$out"

	# a cursor's slices wait for its next FETCH while another statement's
	# stream, in the same processes
	cursor="BEGIN;
DECLARE c CURSOR FOR SELECT po.y, count(*) FROM o JOIN po ON o.b = po.y GROUP BY po.y ORDER BY 1;
FETCH 2 FROM c;
SELECT count(*) FROM o JOIN po ON o.b = po.y;
FETCH 2 FROM c;
COMMIT;"
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" "$cursor" | qf 0)
	cwant=$(printf '%s\n' "SET gp.interconnect_type = relay;" "$cursor" | qf 0)
	[ "$out" = "$cwant" ] && [ -n "$out" ] \
		&& ok "$t: a cursor's slices wait for its next FETCH while another statement streams" \
		|| notok "$t: a cursor and another statement" "$out / $cwant"

	# a sending slice that fails: its error, and the next statement's rows
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y WHERE 1 / (po.x - 250) > -1;" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y;" | qf 0)
	case "$out" in
		*"division by zero"*"$want") ok "$t: a slice that fails is the statement's error, and the session goes on" ;;
		*) notok "$t: a failing slice" "$out" ;;
	esac

	# cancelled while the receiver waits for rows its senders are slow to
	# send, and while the senders wait for a receiver slow to take them: the
	# next statement's rows come, in the same session
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" "SET statement_timeout = '3s';" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y WHERE pg_sleep(0.05 * (po.x % 2)) IS NOT NULL;" \
		"SELECT count(*) FROM bo b1 JOIN bo b2 ON b1.s = b2.s WHERE pg_sleep(0.002 * (b1.k % 2) + 0 * b2.k) IS NOT NULL;" \
		"RESET statement_timeout;" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y;" | qf 0)
	case "$out" in
		*"canceling statement due to statement timeout"*"canceling statement due to statement timeout"*"$want")
			ok "$t: cancelled while its rows are on the way, either end waiting, and the session goes on" ;;
		*) notok "$t: a cancel while streaming" "$out" ;;
	esac
done

###############################################################################
echo "3b. a slice that stops reading a Motion stops its senders, in the middle of its plan too"
###############################################################################
# A plan that reads no more of a Motion -- a nested loop over an empty table,
# a hash join whose hash table is empty on a segment, a Limit -- stops the
# Motion's senders there and then (gp_motion.c), where before they waited on
# it: a tcp receiver that never read its Motion never took their
# connections, and a segment that went on to the next branch of its plan
# left the other segments waiting for rows its senders could not send.  Each
# statement has ORCA's plan said, answers as the planner does under every
# transport, and ends well before its timeout.
q 0 "CREATE TABLE sq_empty (i int, j int) DISTRIBUTED BY (i);
     CREATE TABLE sq_cte (i1 int, i2 int, i3 int, i4 int) DISTRIBUTED BY (i1);
     INSERT INTO sq_cte SELECT i, i % 1000, i % 100000, i % 75 FROM generate_series(1, 200000) i;
     CREATE TABLE sq_big (k int, k2 int, s text) DISTRIBUTED BY (k);
     INSERT INTO sq_big SELECT i, i % 1000, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 200000) i;
     CREATE TABLE sq_b (b int) DISTRIBUTED BY (b);
     INSERT INTO sq_b SELECT generate_series(1, 40);
     CREATE TABLE sq_one AS SELECT b AS a, b FROM sq_b WHERE gp_segment_id = 1 DISTRIBUTED BY (a);
     ANALYZE sq_big; ANALYZE sq_one;
     CREATE EXTENSION gp_inject_fault;" > /dev/null
# segspace's UPDATE: the writers' nested loop over the empty table never
# reads its Broadcast, whose senders make a CTE another slice reads
broadcast="UPDATE sq_empty SET j = m.cc1 FROM (WITH c AS (SELECT * FROM sq_cte ORDER BY i2) SELECT t1.i1 AS cc1, t1.i2 AS cc2 FROM c t1, c t2 WHERE t1.i1 = t2.i2) m;"
# sq_one's rows all hash to segment 1: segment 0's first join has an empty
# hash table, reads nothing of sq_big, and goes on to the Append's second
# branch, whose senders wait on segment 1
union="SELECT sq_big.s FROM sq_one JOIN sq_big ON sq_one.b = sq_big.k2
UNION ALL SELECT b2.s FROM sq_big b2 JOIN po ON b2.k2 % 50 = po.y"
starve="SELECT count(*), sum(length(s)) FROM ($union) u"
same "a nested loop over an empty table never reads its Broadcast, below which a CTE another slice reads is made" \
	"Seq Scan on sq_empty*Broadcast Motion*Shared Scan" \
	"SET statement_timeout = '20s'; SET gp.cte_sharing = on;
$broadcast
SELECT count(*) FROM sq_empty;"
same "a segment's hash join never reads its outer Redistribute, and the segment goes on to another branch" \
	"(slice5; segments: 2)" \
	"SET statement_timeout = '20s'; SET gp.optimizer_enable_motion_broadcast = off;
$starve;"
same "a Limit stops reading the merge of a Gather into one segment, in the middle of a write's plan" \
	"Merge Key" \
	"SET statement_timeout = '20s';
BEGIN;
UPDATE wu SET b = b + 1 WHERE a IN (SELECT k FROM bo ORDER BY k LIMIT 10);
SELECT count(*), sum(b) FROM wu;
ROLLBACK;"
same "a nested loop's inner side, read again for each row, is never stopped before the loop's end" \
	"Nested Loop Semi Join" \
	"SET statement_timeout = '20s'; SET gp.optimizer_enable_hashjoin = off;
SELECT count(*), sum(o.a) FROM o WHERE EXISTS (SELECT 1 FROM po WHERE po.y = o.b AND po.x > o.a);"

rows=$(q 0 "SET gp.optimizer = off; SELECT count(*) FROM ($union) u;")
seg1=$(q 0 "SELECT dbid FROM gp_segment_configuration WHERE content = 1 AND role = 'p';")
for t in $TRANSPORTS relay; do
	# a cursor's FETCHes, a segment's plan stopped in the middle of the first
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" "SET statement_timeout = '20s';" \
		"SET gp.optimizer_enable_motion_broadcast = off;" "BEGIN;" \
		"DECLARE sq CURSOR FOR SELECT u.s FROM ($union) u;" \
		'FETCH 5 FROM sq \g /dev/null' '\echo :ROW_COUNT' \
		"MOVE FORWARD ALL IN sq;" '\echo :ROW_COUNT' "COMMIT;" | qf 0)
	[ "$out" = "5
$((rows - 5))" ] && ok "$t: a cursor's FETCHes, the plan of a segment stopped in the middle of the first" \
		|| notok "$t: a cursor over a plan stopped in the middle" "$out (want 5 and $((rows - 5)))"

	# EXPLAIN ANALYZE of it, timed: a hash join that has returned rows stops
	# the Motions below it in the middle of its own measured run
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" "SET statement_timeout = '20s';" \
		"SET gp.optimizer_enable_motion_broadcast = off;" \
		"EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF) $starve;" | qf 0)
	case "$out" in
		*"Append"*"Hash Join"*"Hash Join"*"Optimizer: GPORCA"*)
			ok "$t: EXPLAIN ANALYZE of a plan a segment stopped in the middle" ;;
		*) notok "$t: EXPLAIN ANALYZE of a plan stopped in the middle" "$out" ;;
	esac

	# a sender that comes late -- the first to set up on segment 1 waits two
	# seconds -- to a receiver that has ended: it is waited for, and stopped
	[ "$t" = relay ] && continue
	out=$(printf '%s\n' "SET gp.interconnect_type = $t;" "SET statement_timeout = '20s';" \
		"SET gp.cte_sharing = on;" \
		"SELECT gp_inject_fault('interconnect_setup_palloc', 'reset', $seg1);" \
		"SELECT gp_inject_fault('interconnect_setup_palloc', 'sleep', '', '', '', 1, 1, 2, $seg1);" \
		"$broadcast" "SELECT count(*) FROM sq_empty;" \
		"SELECT gp_inject_fault('interconnect_setup_palloc', 'reset', $seg1);" | qf 0)
	[ "$out" = "Success:
Success:
0
Success:" ] && ok "$t: a sender two seconds late to a receiver that stopped reading is waited for, and stopped" \
		|| notok "$t: a late sender to a stopped receiver" "$out"
done
###############################################################################
echo "4. the proxies"
###############################################################################
if [[ " $TRANSPORTS " == *" proxy "* ]]; then
	join="SET gp.interconnect_type = proxy; SELECT count(*), sum(o.a) FROM o JOIN po ON o.b = po.y;"
	want=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(o.a) FROM o JOIN po ON o.b = po.y;")

	# a proxy on every node, the coordinator's too, as Cloudberry's: a
	# background worker of the node's postmaster
	proxy_pid() { pgrep -P "$(head -1 "$(datadir "$1")/postmaster.pid")" -f 'ic proxy process'; }
	out=$(for n in 0 1 2; do proxy_pid "$n" | wc -l; done | tr '\n' ' ')
	[ "$out" = "1 1 1 " ] && ok "a proxy on every node" || notok "the proxies" "$out"

	# What is not a proxy's, on a proxy's port -- ic_proxy_socket's test --
	# is dropped, and the proxy carries on.
	python3 - "$((BASEPORT + 1 + IC_PROXY_OFFSET))" <<'PY'
import os, socket, struct, sys
for _ in range(10):
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
    s.sendall(struct.pack('!i', 134591) + struct.pack('!i', 64) + os.urandom(64))
    s.close()
PY
	out=$(q 0 "$join")
	[ "$out" = "$want" ] && ok "a proxy given garbage on its port drops it, and carries the next statement's rows" \
		|| notok "garbage on a proxy's port" "$out / $want"

	# A proxy that exits is started again, and the statements after it
	# connect to it once it listens.
	pid=$(proxy_pid 1)
	kill -TERM "$pid" 2> /dev/null
	out=$(q 0 "$join")
	again=$(proxy_pid 1)
	[ "$out" = "$want" ] && [ -n "$again" ] && [ "$again" != "$pid" ] \
		&& ok "a proxy that exits is started again, and carries the next statement's rows" \
		|| notok "a proxy started again" "$out / $want (pid $pid, then $again)"
else
	echo "  (the proxy is not built)"
fi

###############################################################################
echo "5. a transport whose module is not loaded"
###############################################################################
# A server told to use it does not start, rather than run its statements
# over another; a session is refused it, and told which module to load.
if ! start_node 0 "shared_preload_libraries = 'gp_core,gp_sql,gp_orca'" \
		"gp.interconnect_type = 'udp2'" &&
   grep -q 'gp.interconnect_type is "udp2", which no module in "shared_preload_libraries" provides' "$ROOT/node0.log"; then
	ok "a server whose gp.interconnect_type is udp2, without udp2, does not start"
else
	notok "a server told to use a transport it has not loaded" "$(tail -3 "$ROOT/node0.log")"
fi
start_node 0 "shared_preload_libraries = 'gp_core,gp_sql,gp_orca'"
out=$(q 0 "SET gp.interconnect_type = proxy;")
case "$out" in
	*'The interconnect "proxy" is not loaded.'*'Add "interconnect" to "shared_preload_libraries"'*)
		ok "SET gp.interconnect_type = proxy, without interconnect, is refused with the module to load" ;;
	*) notok "a transport not loaded, SET" "$out" ;;
esac
start_node 0

crashes=$(cat "$ROOT"/node*.log | grep -c "terminated by signal")
[ "$crashes" = 0 ] && ok "no node crashed" \
	|| notok "a node crashed" "$(grep -h -B2 "terminated by signal" "$ROOT"/node*.log | head -10)"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
