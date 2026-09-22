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
# M2: the cluster.
#
# Three servers in one container -- a coordinator and two segments -- which is
# the smallest thing that can be got wrong in an interesting way.  What this
# checks is that each of them knows which node it is, that they agree about the
# cluster, and that a cluster described wrongly is a server that does not start
# rather than one that dispatches somewhere unexpected.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/cluster/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"

PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$PG_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-cluster-XXXXXX")"
BASEPORT="${PGPORT:-$((6100 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"

PRELOAD='gp_core,gp_sql'

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

# node <n>: 0 is the coordinator, 1..n the segments.
datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }
dbid()    { echo $(($1 + 1)); }
content() { [ "$1" -eq 0 ] && echo -1 || echo $(($1 - 1)); }

cleanup() {
	for n in 0 1 2; do
		"$BINDIR/pg_ctl" -D "$(datadir $n)" -m immediate stop >/dev/null 2>&1
	done
	rm -rf "$ROOT"
}
trap cleanup EXIT

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
		echo "gp.dbid = $(dbid "$n")"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		for line in "$@"; do echo "$line"; done
	} > "$d/postgresql.auto.conf"
	"$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node$n.log" -w -t 30 start >/dev/null 2>&1
}

q() {						# q <n> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-c "$2" 2>&1
}

echo "M2 cluster tests"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

for n in 0 1 2; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 \
		> "$ROOT/initdb$n.log" 2>&1
	if [ $? -ne 0 ]; then
		echo "initdb failed for node $n:"; tail -20 "$ROOT/initdb$n.log"; exit 1
	fi
done

# The cluster, as every node reads it.  A host that starts with "/" is a
# directory holding a socket, which is what libpq makes of it; a cluster spread
# over machines writes host names here instead.
{
	echo "# dbid content role host port datadir"
	for n in 0 1 2; do
		echo "$(dbid "$n") $(content "$n") p $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done
} > "$CONF"

###############################################################################
echo "1. every node starts and knows which one it is"
###############################################################################
started=1
for n in 0 1 2; do
	if start_node "$n"; then
		ok "node $n starts"
		q "$n" "CREATE EXTENSION gp_core;" >/dev/null
	else
		notok "node $n starts" "$(tail -5 "$ROOT/node$n.log")"
		started=0
	fi
done

if [ "$started" -eq 1 ]; then
	out=$(q 0 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "dispatch|2|-1|f|1" ] \
		&& ok "the coordinator dispatches, over 2 segments ($out)" \
		|| notok "gp.node() on the coordinator" "$out"

	# A psql opened on a segment is a utility session, as it is in Cloudberry:
	# the node executes what it is sent, but this connection was not sent.
	out=$(q 1 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "utility|2|0|f|2" ] \
		&& ok "a direct connection to segment 0 is a utility session ($out)" \
		|| notok "gp.node() on segment 0" "$out"

	out=$(q 2 "SELECT content_id, dbid FROM gp.node();")
	[ "$out" = "1|3" ] && ok "segment 1 knows its content id and dbid" \
		|| notok "gp.node() on segment 1" "$out"

	###########################################################################
	echo "2. they agree about the cluster, and it is readable as rows"
	###########################################################################
	out=$(q 0 "SELECT count(*), sum(content), count(*) FILTER (WHERE role = 'p') FROM gp.segment_configuration();")
	[ "$out" = "3|0|3" ] \
		&& ok "gp.segment_configuration() lists the three nodes" \
		|| notok "gp.segment_configuration()" "$out"

	out=$(q 0 "SELECT content || ' ' || port FROM gp.segment_configuration() WHERE content >= 0 ORDER BY content;")
	want="0 $(port 1)
1 $(port 2)"
	[ "$out" = "$want" ] && ok "the segments are in content order, with their ports" \
		|| notok "segment ports" "$out"

	out=$(q 1 "SELECT count(*) FROM gp.segment_configuration();")
	[ "$out" = "3" ] && ok "a segment reads the same file" \
		|| notok "gp.segment_configuration() on a segment" "$out"
fi

###############################################################################
echo "3. a cluster described wrongly is a server that does not start"
###############################################################################
# The message has to name the file and the line: this is read in the
# postmaster while it starts, so it is all the operator is given.
refuses() {					# refuses <what> <expected message> <config lines...>
	local what="$1"; local want="$2"; shift 2
	local d; d="$(datadir 0)"

	"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir 0)'"
		echo "listen_addresses = ''"
		echo "port = $(port 0)"
		for line in "$@"; do echo "$line"; done
	} > "$d/postgresql.auto.conf"

	: > "$ROOT/node0.log"
	if "$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node0.log" -w -t 20 start >/dev/null 2>&1; then
		notok "$what" "the server started"
		"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
		return
	fi
	if grep -qF "$want" "$ROOT/node0.log"; then
		ok "$what"
	else
		notok "$what" "$(grep -i 'FATAL\|ERROR' "$ROOT/node0.log" | head -2)"
	fi
}

cat > "$ROOT/bad-role.conf" <<EOF
1 -1 x $(sockdir 0) $(port 0) $(datadir 0)
EOF
refuses "a role that is neither p nor m names the line" \
	"line 1" \
	"gp.cluster_config = '$ROOT/bad-role.conf'" "gp.dbid = 1"

cat > "$ROOT/two-coords.conf" <<EOF
1 -1 p $(sockdir 0) $(port 0) $(datadir 0)
2 -1 p $(sockdir 1) $(port 1) $(datadir 1)
EOF
refuses "two lines for content -1 are refused" \
	"has two nodes with role" \
	"gp.cluster_config = '$ROOT/two-coords.conf'" "gp.dbid = 1"

cat > "$ROOT/hole.conf" <<EOF
1 -1 p $(sockdir 0) $(port 0) $(datadir 0)
2  1 p $(sockdir 1) $(port 1) $(datadir 1)
EOF
refuses "a hole in the content ids is refused" \
	"content id 1 is out of range" \
	"gp.cluster_config = '$ROOT/hole.conf'" "gp.dbid = 1"

refuses "a dbid that is in no line is refused" \
	"is not in cluster configuration file" \
	"gp.cluster_config = '$CONF'" "gp.dbid = 99"

refuses "a role that disagrees with the file is refused" \
	"gives dbid 2 content id 0" \
	"gp.cluster_config = '$CONF'" "gp.dbid = 2" "gp.role = 'dispatch'"

refuses "dispatch with no cluster at all is refused" \
	"no cluster is configured" \
	"gp.role = 'dispatch'"

refuses "a file that is not there is refused" \
	"could not open cluster configuration file" \
	"gp.cluster_config = '$ROOT/nowhere.conf'"

###############################################################################
echo "4. with no cluster configured, this is a single node"
###############################################################################
if start_node 0 "gp.cluster_config = ''" ; then
	notok "node 0 should not have started: gp.role is dispatch with no cluster"
else
	ok "gp.role = dispatch with no cluster is still refused"
fi

d="$(datadir 0)"
"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
{
	echo "shared_preload_libraries = '$PRELOAD'"
	echo "unix_socket_directories = '$(sockdir 0)'"
	echo "listen_addresses = ''"
	echo "port = $(port 0)"
} > "$d/postgresql.auto.conf"
if "$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node0.log" -w -t 30 start >/dev/null 2>&1; then
	out=$(q 0 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "utility|1|-1|t|1" ] \
		&& ok "one node: utility, one segment to compute with, single_node ($out)" \
		|| notok "gp.node() with no cluster" "$out"

	out=$(q 0 "SELECT count(*) FROM gp.segment_configuration();")
	[ "$out" = "0" ] && ok "and no rows in gp.segment_configuration()" \
		|| notok "gp.segment_configuration() with no cluster" "$out"
else
	notok "a server with no cluster starts" "$(tail -5 "$ROOT/node0.log")"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
