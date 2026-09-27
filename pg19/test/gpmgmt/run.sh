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
# M7: gpMgmt -- Cloudberry's management tools, installed beside the server
# (pg19/gpMgmt), on PostgreSQL 19's own pg_ctl, pg_basebackup, pg_rewind and
# initdb, against clusters they make.
#
# gpinitsystem makes a coordinator and three primaries on this host, each
# with a mirror, as Cloudberry's demo cluster is made; and then the tools do
# what they are for, each checked by what the cluster says after: gpstate
# reports on it; gpconfig sets, shows and removes a setting by Cloudberry's
# name; gpstop has it read its files again, restarts it and stops it, and
# gpstart starts it; a primary that stops is failed over from, and
# gprecoverseg brings it back -- with pg_rewind, and with pg_basebackup -- and
# the roles back to the ones preferred; gpexpand adds a segment with its
# mirror, and gpshrink takes them away; gpinitstandby makes a standby
# coordinator, and gpactivatestandby makes it the coordinator; and
# gpdeletesystem removes the cluster.  A second cluster, of primaries alone,
# whose nodes authenticate each other by certificates, is given its mirrors
# by gpaddmirrors, and gpmovemirrors moves one of them.  A third, of
# primaries alone with the modules the rest of the tools read, has them run
# on it (M8): gpcheckcat finds what a segment alone was given, analyzedb
# analyzes a table again where it changed, gpload loads through gpfdist,
# gplogfilter finds an error, gpmemwatcher and gpmemreport measure the nodes,
# gpcheckperf measures the host, gpreload sorts a table's rows, gppkg installs
# and removes a package, gpdirtableload puts files into a directory table and
# takes them back, and gpdemo makes a demo cluster and deletes it.
#
# The nodes are on this host, under its name, as the demo cluster's are, and
# take TCP connections, as a cluster's nodes must: gpinitsystem gives them
# ports of their own, which this checks are free.  gpinitsystem's scripts ssh
# to a host even when it is this one; ssh here is this directory's, which
# runs the command here.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/gpmgmt/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

HOST="$(hostname)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-gpmgmt-XXXXXX")"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-gpmgmt-XXXXXX")"
if ! . "$HERE/tools.sh" "$EXEC"; then
	echo "gpMgmt, or the Python it needs, is not installed; skipping"
	rm -rf "$WORK" "$EXEC"
	exit 77
fi
LOGDIR="$WORK/logs"
mkdir -p "$LOGDIR"
unset PGHOST PGDATABASE

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/         /' | head -20; fail=$((fail + 1)); }

# Thirty ports none of which anything listens on: the coordinator's, the
# standby's, the primaries' and the mirrors', of three clusters, gpfdist's
# and the demo cluster's.
free_ports() {				# free_ports <base> <n>
	local p
	for p in $(seq "$1" $(($1 + $2 - 1))); do
		ss -Htan "sport = :$p" 2> /dev/null | grep -q . && return 1
		[ -e "/tmp/.s.PGSQL.$p" ] && return 1
	done
	return 0
}
for _ in $(seq 20); do
	BASE=$((20000 + (RANDOM % 260) * 30))
	free_ports "$BASE" 30 && break
done

# cluster A: the coordinator BASE, the standby BASE+1, the primaries
# BASE+2..4, the mirrors BASE+12..14, and the pair gpexpand adds BASE+9 and
# BASE+15; cluster B: the coordinator BASE+5, its primaries BASE+6..8 and its
# mirrors BASE+16..18; cluster C: the coordinator BASE+20 and its primaries
# BASE+21..23, gpfdist BASE+24; the demo cluster BASE+26..29.
CPORT=$BASE
SPORT=$((BASE + 1))
A="$WORK/a"
B="$WORK/b"

stop_everything() {
	local d
	for d in $(find "$WORK" -name postmaster.pid -printf '%h\n' 2> /dev/null); do
		"$BINDIR/pg_ctl" -D "$d" -m immediate stop > /dev/null 2>&1
	done
}
cleanup() {
	stop_everything
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/gpmgmt"
		cp -r "$LOGDIR" "$HOME/gpAdminLogs" "$RESULTS_DIR/gpmgmt/" 2> /dev/null
		find "$WORK" -name '*.log' -path '*/log/*' -exec cp --parents {} "$RESULTS_DIR/gpmgmt/" \; 2> /dev/null
	fi
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$EXEC"
}
trap cleanup EXIT

q() {						# q <port> <sql>: on the node at the port, in postgres
	"$BINDIR/psql" -X -q -t -A -h "$HOST" -p "$1" -d postgres -c "$2" 2>&1
}
# Poll until the query on the node gives what is wanted, or seconds pass:
# the last statement's answer, one row, where the statements before it --
# asking FTS to probe -- answer too.
wait_for() {				# wait_for <port> <sql> <want> [seconds]
	local port="$1" sql="$2" want="$3" t="${4:-60}" out
	for _ in $(seq $((t * 5))); do
		out=$(q "$port" "$sql" | tail -1)
		[ "$out" = "$want" ] && return 0
		sleep 0.2
	done
	echo "$out"
	return 1
}
# What gp_segment_configuration says, content by content: each node's
# dbid, role, preferred role, mode and status, the coordinator's first.
config() {					# config <coordinator port>
	q "$1" "SELECT string_agg(content || ':' || dbid || ':' || role::text || preferred_role::text || mode::text || status::text, ' '
	                        ORDER BY content, role DESC, dbid)
	          FROM gp_segment_configuration"
}
# A primary and its mirror in sync, every content's, FTS asked to look.
in_sync() {					# in_sync <coordinator port>
	wait_for "$1" "SELECT gp_request_fts_probe_scan(); SELECT count(*) FROM gp_segment_configuration
	                WHERE content >= 0 AND NOT (mode = 's' AND status = 'u')" 0 120
}
# A tool's run: its output in the logs, and its exit code.
run() {						# run <name> <command...>
	local name="$1"; shift
	"$@" > "$LOGDIR/$name.out" 2>&1
}
tail_of() { tail -15 "$LOGDIR/$1.out"; }

# A gpinitsystem configuration file, Cloudberry's demo cluster's.
init_config() {				# init_config <dir> <coordinator port> <primary base> [<mirror base>]
	local dir="$1"
	mkdir -p "$dir/qddir" "$dir/dbfast1" "$dir/dbfast2" "$dir/dbfast3"
	echo "$HOST" > "$dir/hostfile"
	{
		echo 'ARRAY_NAME="Demo Cluster"'
		echo 'SEG_PREFIX=demoDataDir'
		echo "PORT_BASE=$3"
		echo "declare -a DATA_DIRECTORY=($dir/dbfast1 $dir/dbfast2 $dir/dbfast3)"
		echo "COORDINATOR_HOSTNAME=$HOST"
		echo "COORDINATOR_DIRECTORY=$dir/qddir"
		echo "COORDINATOR_PORT=$2"
		echo 'TRUSTED_SHELL=ssh'
		echo 'ENCODING=UNICODE'
		echo "MACHINE_LIST_FILE=$dir/hostfile"
		echo "PRELOAD_LIBRARIES=gp_core,gp_sql"
		if [ -n "${4:-}" ]; then
			mkdir -p "$dir/dbfast_mirror1" "$dir/dbfast_mirror2" "$dir/dbfast_mirror3"
			echo "MIRROR_PORT_BASE=$4"
			echo "declare -a MIRROR_DATA_DIRECTORY=($dir/dbfast_mirror1 $dir/dbfast_mirror2 $dir/dbfast_mirror3)"
		fi
	} > "$dir/gpinitsystem_config"
}

echo "gpmgmt: gpMgmt's tools on a cluster of this host, $HOST, ports from $BASE"

###############################################################################
echo "1. gpinitsystem makes a cluster, a mirror for each primary"
###############################################################################
init_config "$A" "$CPORT" $((BASE + 2)) $((BASE + 12))
export COORDINATOR_DATA_DIRECTORY="$A/qddir/demoDataDir-1" PGPORT="$CPORT"
if run init-a gpinitsystem -a -c "$A/gpinitsystem_config" -l "$LOGDIR"; then
	ok "gpinitsystem makes the cluster"
else
	notok "gpinitsystem makes the cluster" "$(tail_of init-a)"
	echo
	echo "gpMgmt tests: $pass passed, $fail failed"
	exit 1
fi
out=$(config "$CPORT")
want="-1:1:ppnu 0:2:ppsu 0:5:mmsu 1:3:ppsu 1:6:mmsu 2:4:ppsu 2:7:mmsu"
in_sync "$CPORT" > /dev/null
out=$(config "$CPORT")
[ "$out" = "$want" ] \
	&& ok "gp_segment_configuration: the coordinator, three pairs in sync" \
	|| notok "gp_segment_configuration after gpinitsystem" "$out"
q "$CPORT" "CREATE TABLE t (a int, b text) DISTRIBUTED BY (a)" > /dev/null
q "$CPORT" "INSERT INTO t SELECT g, 'row ' || g FROM generate_series(1, 300) g" > /dev/null
out=$(q "$CPORT" "SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t")
[ "$out" = "3:300" ] \
	&& ok "a distributed table's rows are on the three segments" \
	|| notok "a distributed table" "$out"
out=$(q "$CPORT" "SELECT extname FROM pg_extension WHERE extname LIKE 'gp%' ORDER BY 1" | tr '\n' ' ')
[ "$out" = "gp_core gp_sql " ] \
	&& ok "the preloaded modules' extensions are in postgres, as gpinitsystem makes them" \
	|| notok "the extensions gpinitsystem makes" "$out"

###############################################################################
echo "2. gpstate reports on the cluster"
###############################################################################
run state gpstate
grep -q "Total segment instance count from metadata *= 6" "$LOGDIR/state.out" &&
grep -q "Total primary segment valid (at coordinator) *= 3" "$LOGDIR/state.out" &&
grep -q "Total mirror segment valid (at coordinator) *= 3" "$LOGDIR/state.out" \
	&& ok "gpstate: six segments, three primaries and three mirrors valid" \
	|| notok "gpstate" "$(tail_of state)"
# gpstate -s says a primary's status as "Database status", a mirror's as
# "Segment status".
run state-s gpstate -s
grep -q "Coordinator current role *= dispatch" "$LOGDIR/state-s.out" &&
[ "$(grep -c -E "(Database|Segment) status *= Up" "$LOGDIR/state-s.out")" -eq 6 ] \
	&& ok "gpstate -s: the coordinator dispatches, and every segment is up" \
	|| notok "gpstate -s" "$(grep -E "role|status" "$LOGDIR/state-s.out" | tail -15)"
run state-m gpstate -m
[ "$(grep -c "Passive *Synchronized" "$LOGDIR/state-m.out")" -eq 3 ] \
	&& ok "gpstate -m: three mirrors, passive and in sync" \
	|| notok "gpstate -m" "$(tail_of state-m)"
run state-e gpstate -e
grep -q "All segments are running normally" "$LOGDIR/state-e.out" \
	&& ok "gpstate -e: every segment is running normally" \
	|| notok "gpstate -e" "$(tail_of state-e)"

###############################################################################
echo "3. gpconfig sets, shows and removes a setting by Cloudberry's name"
###############################################################################
run config-c gpconfig -c gp_fts_probe_interval -v 30 &&
run stop-u gpstop -u &&
run config-s gpconfig -s gp_fts_probe_interval &&
grep -q "Coordinator value: 30s" "$LOGDIR/config-s.out" &&
grep -q "Segment     value: 30s" "$LOGDIR/config-s.out" \
	&& ok "gpconfig -c gp_fts_probe_interval, gpstop -u: gp.fts_probe_interval is 30s on every node" \
	|| notok "gpconfig -c" "$(tail_of config-s)"
run config-r gpconfig -r gp_fts_probe_interval &&
run stop-u2 gpstop -u &&
run config-s2 gpconfig -s gp_fts_probe_interval &&
grep -q "Coordinator value: 1min" "$LOGDIR/config-s2.out" \
	&& ok "gpconfig -r: the default again" \
	|| notok "gpconfig -r" "$(tail_of config-s2)"
run config-bad gpconfig -s no_such_setting
[ $? -ne 0 ] \
	&& ok "gpconfig -s of a setting there is none of fails" \
	|| notok "gpconfig -s of no setting" "$(tail_of config-bad)"

###############################################################################
echo "4. gpstop and gpstart"
###############################################################################
if run stop-r gpstop -a -r; then
	out=$(q "$CPORT" "SELECT count(*) FROM t")
	[ "$out" = 300 ] \
		&& ok "gpstop -r: the cluster restarts, and the table is read" \
		|| notok "gpstop -r" "$out"
else
	notok "gpstop -r" "$(tail_of stop-r)"
fi
if run stop gpstop -a && [ ! -e "$COORDINATOR_DATA_DIRECTORY/postmaster.pid" ] &&
   [ -z "$(find "$A" -name postmaster.pid)" ]; then
	ok "gpstop: every node stops"
else
	notok "gpstop" "$(tail_of stop)"
fi
if run start gpstart -a; then
	in_sync "$CPORT" > /dev/null
	out=$(config "$CPORT")
	[ "$out" = "$want" ] && [ "$(q "$CPORT" "SELECT count(*) FROM t")" = 300 ] \
		&& ok "gpstart: every node starts, and the pairs are in sync" \
		|| notok "gpstart" "$out"
	# as Cloudberry's pg_ctl waits for "DTM recovered"
	grep -q "Distributed transaction recovery has reached every segment" "$LOGDIR/start.out" &&
	[ "$(q "$CPORT" "SELECT gp.dtx_recovered()")" = t ] \
		&& ok "gpstart waits for the coordinator's distributed transaction recovery to reach every segment" \
		|| notok "gpstart's wait for distributed transaction recovery" "$(tail_of start)"
else
	notok "gpstart" "$(tail_of start)"
fi

###############################################################################
echo "5. a primary that stops is failed over from; gprecoverseg brings it back, with pg_rewind"
###############################################################################
p0=$(q "$CPORT" "SELECT datadir FROM gp_segment_configuration WHERE content = 0 AND role = 'p'")
"$BINDIR/pg_ctl" -D "$p0" -m immediate stop > /dev/null 2>&1
out=$(wait_for "$CPORT" "SELECT gp_request_fts_probe_scan(); SELECT string_agg(dbid || ':' || role::text || status::text, ' ' ORDER BY dbid)
                          FROM gp_segment_configuration WHERE content = 0" "2:md 5:pu") \
	&& ok "FTS fails over to content 0's mirror" \
	|| notok "a failover" "$out"
[ "$(q "$CPORT" "SELECT count(*) FROM t")" = 300 ] \
	&& ok "the table is read from the promoted mirror" \
	|| notok "a read after the failover" "$(q "$CPORT" "SELECT count(*) FROM t")"
if run recover gprecoverseg -a --no-progress; then
	out=$(in_sync "$CPORT"; q "$CPORT" "SELECT string_agg(dbid || ':' || role::text || preferred_role::text || mode::text || status::text, ' ' ORDER BY dbid)
	                                   FROM gp_segment_configuration WHERE content = 0")
	[ "$out" = "2:mpsu 5:pmsu" ] \
		&& ok "gprecoverseg: the old primary is its mirror's mirror, rewound, in sync" \
		|| notok "gprecoverseg" "$out"
else
	notok "gprecoverseg" "$(tail_of recover)"
fi
if run rebalance gprecoverseg -ar --no-progress; then
	in_sync "$CPORT" > /dev/null
	out=$(config "$CPORT")
	[ "$out" = "$want" ] && [ "$(q "$CPORT" "SELECT count(*) FROM t")" = 300 ] \
		&& ok "gprecoverseg -r: every node in the role it prefers, in sync" \
		|| notok "gprecoverseg -r" "$out"
else
	notok "gprecoverseg -r" "$(tail_of rebalance)"
fi

###############################################################################
echo "6. a mirror that stops is marked down; gprecoverseg -F makes it again, with pg_basebackup"
###############################################################################
m1=$(q "$CPORT" "SELECT datadir FROM gp_segment_configuration WHERE content = 1 AND role = 'm'")
mkdir -p "$WORK/ts"
q "$CPORT" "CREATE TABLESPACE ts LOCATION '$WORK/ts'" > /dev/null
q "$CPORT" "CREATE TABLE tt (a int) TABLESPACE ts DISTRIBUTED BY (a); INSERT INTO tt SELECT generate_series(1, 100)" > /dev/null
"$BINDIR/pg_ctl" -D "$m1" -m immediate stop > /dev/null 2>&1
out=$(wait_for "$CPORT" "SELECT gp_request_fts_probe_scan(); SELECT status FROM gp_segment_configuration WHERE content = 1 AND role = 'm'" d) \
	&& ok "FTS marks content 1's mirror down" \
	|| notok "a mirror marked down" "$out"
if run recover-f gprecoverseg -aF --no-progress; then
	in_sync "$CPORT" > /dev/null
	out=$(config "$CPORT")
	dbid=$(q "$CPORT" "SELECT dbid FROM gp_segment_configuration WHERE content = 1 AND role = 'm'")
	[ "$out" = "$want" ] && [ -d "$WORK/ts/$dbid" ] \
		&& ok "gprecoverseg -F: the mirror is a new copy of its primary, its tablespace its own, in sync" \
		|| notok "gprecoverseg -F" "$out / $(ls "$WORK/ts")"
else
	notok "gprecoverseg -F" "$(tail_of recover-f)"
fi

###############################################################################
echo "7. gpexpand adds a segment and its mirror to the running cluster, and gpshrink takes them away"
###############################################################################
# As isolation2's gpexpand_gpshrink does on Cloudberry's demo cluster, on the
# cluster gpinitsystem made, whose nodes each have a cluster file of their
# own and their settings in postgresql.conf: gpexpand's input, a line a node,
# host|address|port|data directory|dbid|content|role.  The cluster has a
# tablespace (section 6), so gpexpand's first run writes where the new
# segment's goes, for the user to look at, and stops; the second adds the
# segments, the third moves the table's rows onto them; gpshrink's two runs
# take them back.
mkdir -p "$A/dbfast4" "$A/dbfast_mirror4"
{
	echo "$HOST|$HOST|$((BASE + 9))|$A/dbfast4/demoDataDir3|8|3|p"
	echo "$HOST|$HOST|$((BASE + 15))|$A/dbfast_mirror4/demoDataDir3|9|3|m"
} > "$A/expand"
run expand-ts gpexpand -i "$A/expand"
[ $? -eq 1 ] && grep -q "^8|$WORK/ts\$" "$A/expand.ts" \
	&& ok "gpexpand writes the new segment's tablespace directory to a file of its own, and asks for a rerun" \
	|| notok "gpexpand's tablespace file" "$(tail_of expand-ts; cat "$A/expand.ts")"
if run expand gpexpand -i "$A/expand" && run expand2 gpexpand -i "$A/expand"; then
	in_sync "$CPORT" > /dev/null
	out=$(config "$CPORT")
	rows=$(q "$CPORT" "SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t;
	                   SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM tt;
	                   SELECT numsegments FROM gp_distribution_policy WHERE localoid = 't'::regclass" | tr '\n' ' ')
	[ "$out" = "$want 3:8:ppsu 3:9:mmsu" ] && [ "$rows" = "4:300 4:100 4 " ] &&
	[ -d "$WORK/ts/8" ] && [ -d "$WORK/ts/9" ] \
		&& ok "gpexpand: a fourth pair, in sync, its tablespace directories their own, and the tables' rows spread over the four segments" \
		|| notok "gpexpand" "$out / $rows"
else
	notok "gpexpand" "$(tail_of expand; tail_of expand2)"
fi
run state-x gpstate
grep -q "Total segment instance count from metadata *= 8" "$LOGDIR/state-x.out" \
	&& ok "gpstate: eight segments" \
	|| notok "gpstate after gpexpand" "$(tail_of state-x)"
if run shrink gpshrink -i "$A/expand" && run shrink2 gpshrink -i "$A/expand"; then
	out=$(config "$CPORT")
	rows=$(q "$CPORT" "SELECT count(DISTINCT gp_segment_id) || ':' || count(*) FROM t")
	[ "$out" = "$want" ] && [ "$rows" = "3:300" ] && [ ! -e "$A/dbfast4/demoDataDir3/postmaster.pid" ] \
		&& ok "gpshrink: the table's rows back on three segments, and the fourth pair removed and stopped" \
		|| notok "gpshrink" "$out / $rows"
else
	notok "gpshrink" "$(tail_of shrink; tail_of shrink2)"
fi
if yes | run expand-c gpexpand -c && run shrink-c gpshrink -c; then
	out=$(q "$CPORT" "SELECT count(*) FROM pg_namespace WHERE nspname IN ('gpexpand', 'gpshrink')")
	[ "$out" = 0 ] \
		&& ok "gpexpand -c and gpshrink -c drop their schemas" \
		|| notok "gpexpand -c and gpshrink -c" "$out"
else
	notok "gpexpand -c and gpshrink -c" "$(tail_of expand-c; tail_of shrink-c)"
fi

###############################################################################
echo "8. gpinitstandby makes a standby coordinator"
###############################################################################
if run standby gpinitstandby -a -s "$HOST" -P "$SPORT" -S "$A/standby"; then
	out=$(wait_for "$CPORT" "SELECT application_name || ':' || state FROM pg_stat_replication" "gp_walreceiver:streaming")
	cfg=$(q "$CPORT" "SELECT dbid || ':' || role::text || preferred_role::text || status::text FROM gp_segment_configuration WHERE content = -1 AND role = 'm'")
	[ -n "$cfg" ] && [ "$out" = "" ] \
		&& ok "gpinitstandby: the standby ($cfg) streams from the coordinator as gp_walreceiver" \
		|| notok "gpinitstandby" "$cfg / $out"
else
	notok "gpinitstandby" "$(tail_of standby)"
fi
run state-f gpstate -f
grep -q "Standby address *= $HOST" "$LOGDIR/state-f.out" \
	&& ok "gpstate -f: the standby is reported" \
	|| notok "gpstate -f" "$(tail_of state-f)"
if run stop2 gpstop -a && [ -z "$(find "$A" -name postmaster.pid)" ] && run start2 gpstart -a; then
	out=$(wait_for "$CPORT" "SELECT application_name || ':' || state FROM pg_stat_replication" "gp_walreceiver:streaming")
	[ "$out" = "" ] \
		&& ok "gpstop stops the standby with the rest, gpstart starts it" \
		|| notok "gpstop and gpstart with a standby" "$out"
else
	notok "gpstop and gpstart with a standby" "$(tail_of stop2; tail_of start2)"
fi

###############################################################################
echo "9. gpactivatestandby makes the standby the coordinator"
###############################################################################
q "$CPORT" "INSERT INTO t VALUES (301, 'before the coordinator stops')" > /dev/null
"$BINDIR/pg_ctl" -D "$COORDINATOR_DATA_DIRECTORY" -m fast stop > /dev/null 2>&1
if PGPORT="$SPORT" COORDINATOR_DATA_DIRECTORY="$A/standby" run activate gpactivatestandby -a -d "$A/standby"; then
	export COORDINATOR_DATA_DIRECTORY="$A/standby" PGPORT="$SPORT"
	CPORT="$SPORT"
	out=$(q "$CPORT" "SELECT dbid || ':' || role::text || preferred_role::text FROM gp_segment_configuration WHERE content = -1")
	rows=$(q "$CPORT" "INSERT INTO t VALUES (302, 'after'); SELECT count(*) FROM t")
	[ "$out" = "$(q "$CPORT" "SELECT current_setting('gp.dbid')"):pp" ] && [ "$rows" = 302 ] \
		&& ok "gpactivatestandby: the standby is the coordinator, and the cluster reads and writes through it" \
		|| notok "gpactivatestandby" "$out / $rows"
	grep -q "Distributed transaction recovery has reached every segment" "$LOGDIR/activate.out" &&
	[ "$(q "$CPORT" "SELECT gp.dtx_recovered()")" = t ] \
		&& ok "gpactivatestandby waits for the new coordinator's distributed transaction recovery to reach every segment" \
		|| notok "gpactivatestandby's wait for distributed transaction recovery" "$(tail_of activate)"
else
	notok "gpactivatestandby" "$(tail_of activate)"
fi

###############################################################################
echo "10. gpdeletesystem removes the cluster"
###############################################################################
# every node's but the old coordinator's, which activation left out
dbids=$(q "$CPORT" "SELECT string_agg(dbid::text, ' ') FROM gp_segment_configuration")
if printf 'y\ny\n' | run delete gpdeletesystem -f -d "$COORDINATOR_DATA_DIRECTORY" &&
   [ -z "$(find "$A" -name postmaster.pid)" ] && [ ! -e "$A/dbfast1/demoDataDir0" ] &&
   ! (cd "$WORK/ts" && ls -d $dbids 2> /dev/null | grep -q .); then
	ok "gpdeletesystem: every node stopped, and its directories and tablespaces gone"
else
	notok "gpdeletesystem" "$(tail_of delete)"
fi

###############################################################################
echo "11. a cluster whose nodes authenticate each other by certificates, and gpaddmirrors gives its primaries mirrors"
###############################################################################
# Decision 5's certificates in production: gpinitsystem's NODE_SSL_DIR, the
# directory every host has the cluster's authority in, and its own
# certificate, whose common name is every node's.  The tools connect from a
# node's address too, so they show the certificate as well: here from the
# environment, as a host's user of the tools would have it.
certs="$WORK/certs"
mkdir -p "$certs"
if openssl req -new -x509 -nodes -newkey rsa:2048 -days 2 -subj "/CN=cluster-ca" \
		-keyout "$certs/ca.key" -out "$certs/ca.crt" &&
	openssl req -new -nodes -newkey rsa:2048 -subj "/CN=cloudberry-node" \
		-keyout "$certs/node.key" -out "$certs/node.csr" &&
	openssl x509 -req -in "$certs/node.csr" -days 2 -CA "$certs/ca.crt" \
		-CAkey "$certs/ca.key" -CAcreateserial -out "$certs/node.crt" \
		-extfile <(printf 'subjectAltName=DNS:%s,DNS:localhost,IP:127.0.0.1\n' "$HOST")
then
	chmod 600 "$certs"/*.key
else
	notok "openssl makes the certificates"
fi > "$LOGDIR/openssl.out" 2>&1
export PGSSLCERT="$certs/node.crt" PGSSLKEY="$certs/node.key" \
	PGSSLROOTCERT="$certs/ca.crt" PGSSLMODE=verify-full

init_config "$B" $((BASE + 5)) $((BASE + 6))
echo "NODE_SSL_DIR=$certs" >> "$B/gpinitsystem_config"
export COORDINATOR_DATA_DIRECTORY="$B/qddir/demoDataDir-1" PGPORT=$((BASE + 5))
CPORT=$((BASE + 5))
if run init-b gpinitsystem -a -c "$B/gpinitsystem_config" -l "$LOGDIR"; then
	out=$(q "$CPORT" "SELECT DISTINCT result FROM gp.exec_on_segments('SELECT ssl::text || '' '' || client_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid()')")
	hba=$(cat "$COORDINATOR_DATA_DIRECTORY/pg_hba.conf" "$B"/dbfast*/demoDataDir*/pg_hba.conf |
		grep -E -c '^host[[:space:]].*trust')
	[ "$out" = "true /CN=cloudberry-node" ] && [ "$hba" = 0 ] \
		&& ok "gpinitsystem with NODE_SSL_DIR: the dispatcher's connections are TLS with the node's certificate, and no node's address is trusted" \
		|| notok "gpinitsystem with NODE_SSL_DIR" "$out / $hba lines trust a host"
	p0=$(q "$CPORT" "SELECT port FROM gp_segment_configuration WHERE content = 0 AND role = 'p'")
	out=$(PGSSLCERT=/nonexistent PGSSLKEY=/nonexistent q "$p0" "SELECT 1")
	case "$out" in
		*"certificate"*) ok "a connection to a segment from a node's address without the certificate is refused" ;;
		*) notok "a segment should have asked for a certificate" "$out" ;;
	esac

	for c in 0 1 2; do
		echo "$c|$HOST|$((BASE + 16 + c))|$B/mirror/demoDataDir$c"
	done > "$B/mirrors"
	mkdir -p "$B/mirror"
	if run addmirrors gpaddmirrors -a -i "$B/mirrors"; then
		in_sync "$CPORT" > /dev/null
		out=$(config "$CPORT")
		[ "$out" = "-1:1:ppnu 0:2:ppsu 0:5:mmsu 1:3:ppsu 1:6:mmsu 2:4:ppsu 2:7:mmsu" ] \
			&& ok "gpaddmirrors: three mirrors, in sync" \
			|| notok "gpaddmirrors" "$out"
		out=$(q "$CPORT" "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM pg_stat_replication JOIN pg_stat_ssl USING (pid) WHERE ssl AND client_dn = ''/CN=cloudberry-node''') WHERE result = '1'")
		[ "$out" = 3 ] \
			&& ok "and each mirror streams from its primary over TLS, with the node's certificate" \
			|| notok "the mirrors' replication over TLS" "$out"
	else
		notok "gpaddmirrors" "$(tail_of addmirrors)"
	fi

	###########################################################################
	echo "12. gpmovemirrors moves a mirror to another directory and port"
	###########################################################################
	m0=$(q "$CPORT" "SELECT hostname || '|' || port || '|' || datadir FROM gp_segment_configuration WHERE content = 0 AND role = 'm'")
	echo "$m0 $HOST|$((BASE + 19))|$B/moved/demoDataDir0" > "$B/move"
	mkdir -p "$B/moved"
	if run movemirrors gpmovemirrors -i "$B/move"; then
		in_sync "$CPORT" > /dev/null
		out=$(q "$CPORT" "SELECT role::text || mode::text || status::text || ':' || port || ':' || datadir FROM gp_segment_configuration WHERE content = 0 AND role = 'm'")
		rows=$(q "$CPORT" "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM pg_stat_replication JOIN pg_stat_ssl USING (pid) WHERE ssl') WHERE result = '1'")
		[ "$out" = "msu:$((BASE + 19)):$B/moved/demoDataDir0" ] && [ "$rows" = 3 ] &&
		[ ! -e "${m0##*|}/postmaster.pid" ] \
			&& ok "gpmovemirrors: content 0's mirror is in its new place, in sync, over TLS, the old one stopped" \
			|| notok "gpmovemirrors" "$out / $rows"
	else
		notok "gpmovemirrors" "$(tail_of movemirrors)"
	fi
	hba=$(find "$B" -name pg_hba.conf -exec cat {} + | grep -E -c '^host[[:space:]].*trust')
	ssl=$(find "$B" -name pg_hba.conf -exec cat {} + | grep -E -c '^hostssl[[:space:]]+replication[[:space:]].*cert map=gpnodes')
	[ "$hba" = 0 ] && [ "$ssl" -gt 0 ] \
		&& ok "the lines gpaddmirrors and gpmovemirrors add for the mirrors take the certificate too" \
		|| notok "pg_hba.conf after gpaddmirrors and gpmovemirrors" "$hba lines trust a host, $ssl replication lines take the certificate"
else
	notok "gpinitsystem with NODE_SSL_DIR, of primaries alone" "$(tail_of init-b)"
fi
printf 'y\ny\n' | run delete-b gpdeletesystem -f -d "$COORDINATOR_DATA_DIRECTORY"
unset PGSSLCERT PGSSLKEY PGSSLROOTCERT PGSSLMODE

###############################################################################
echo "13. a cluster for the rest of the tools, with the modules they read"
###############################################################################
# Primaries alone, as cluster B's, with the modules the tools read:
# append-optimized tables (gp_ao), external tables and gpfdist (gp_exttable),
# PAX, and ORCA.
C="$WORK/c"
init_config "$C" $((BASE + 20)) $((BASE + 21))
sed -i 's/^PRELOAD_LIBRARIES=.*/PRELOAD_LIBRARIES=gp_core,gp_orca,gp_sql,gp_ao,gp_exttable,pax/' "$C/gpinitsystem_config"
export COORDINATOR_DATA_DIRECTORY="$C/qddir/demoDataDir-1" PGPORT=$((BASE + 20))
CPORT=$((BASE + 20))
# on the tools' database, and on another
qt() {						# qt <port> <sql>
	"$BINDIR/psql" -X -q -t -A -h "$HOST" -p "$1" -d tools -c "$2" 2>&1
}
qd() {						# qd <port> <database> <sql>
	"$BINDIR/psql" -X -q -t -A -h "$HOST" -p "$1" -d "$2" -c "$3" 2>&1
}
if run init-c gpinitsystem -a -c "$C/gpinitsystem_config" -l "$LOGDIR" &&
   [ -z "$(q "$CPORT" "CREATE DATABASE tools")" ]; then
	ok "gpinitsystem makes a cluster with gp_ao, gp_exttable and PAX"
	tools=1
else
	notok "gpinitsystem makes cluster C" "$(tail_of init-c)"
	tools=
fi

if [ -n "$tools" ]; then
	qt "$CPORT" "CREATE TABLE h (a int, b text) DISTRIBUTED BY (a);
				 CREATE TABLE ao (a int, b text) WITH (appendonly = true) DISTRIBUTED BY (a);
				 CREATE TABLE aoc (a int, b text) WITH (appendonly = true, orientation = column) DISTRIBUTED BY (a);
				 CREATE TABLE px (a int, b text) USING pax DISTRIBUTED BY (a);
				 CREATE TABLE part (a int, b int) DISTRIBUTED BY (a) PARTITION BY RANGE (b) (START (0) END (30) EVERY (10));
				 CREATE INDEX h_b ON h (b);
				 CREATE VIEW v AS SELECT a FROM h;
				 CREATE DIRECTORY TABLE dt;
				 INSERT INTO h SELECT g, 'h' || g FROM generate_series(1, 300) g;
				 INSERT INTO ao SELECT g, 'ao' || g FROM generate_series(1, 300) g;
				 INSERT INTO aoc SELECT g, 'aoc' || g FROM generate_series(1, 300) g;
				 INSERT INTO px SELECT g, 'px' || g FROM generate_series(1, 300) g;
				 INSERT INTO part SELECT g, g % 30 FROM generate_series(1, 300) g" > "$LOGDIR/tools-setup.out"
	p0=$(q "$CPORT" "SELECT port FROM gp_segment_configuration WHERE content = 0 AND role = 'p'")
	p0dir=$(q "$CPORT" "SELECT datadir FROM gp_segment_configuration WHERE content = 0 AND role = 'p'")

	###########################################################################
	echo "14. gpcheckcat checks the catalogs every node has, and the modules' own"
	###########################################################################
	# Its details are in its log, of the day, from where it was before.
	catlog="$HOME/gpAdminLogs/gpcheckcat_$(date +%Y%m%d).log"
	logat=$(stat -c %s "$catlog" 2> /dev/null || echo 0)
	if run checkcat gpcheckcat tools && grep -q "Found no catalog issue" "$LOGDIR/checkcat.out"; then
		ok "gpcheckcat: no issue in a database of heap, append-optimized, PAX and partitioned tables"
	else
		notok "gpcheckcat of a sound database" "$(tail_of checkcat; tail -c +$((logat + 1)) "$catlog" | grep -E 'FAIL|ERROR' | head)"
	fi
	# What one segment alone was given, over a connection straight to it, in
	# a database of its own: a table, a distribution policy -- the "gp"
	# label -- of its own, a row of gp_ao's for no table and a PAX table
	# without its row; and a directory table's file, put through the
	# coordinator on the segment its path hashes to, emptied there.
	q "$CPORT" "CREATE DATABASE catbad" > /dev/null
	qd "$CPORT" catbad "CREATE TABLE h (a int, b text) DISTRIBUTED BY (a);
						CREATE TABLE px (a int, b text) USING pax DISTRIBUTED BY (a);
						CREATE DIRECTORY TABLE dt;
						SELECT gp_sql.directory_table_put('dt', 'x/y.txt', 'hello'::bytea)" > /dev/null
	qd "$p0" catbad "CREATE TABLE only_here (a int);
					 SECURITY LABEL FOR gp ON TABLE h IS 'distributed_by=(b)';
					 INSERT INTO gp_ao.segfile VALUES (999999, 1, '{0}', '{0}', 0, 0, 0, 1, 3, NULL);
					 DELETE FROM pax.pg_pax_tables WHERE relid = 'px'::regclass" > "$LOGDIR/checkcat-corrupt.out"
	dtseg=$(qd "$CPORT" catbad "SELECT gp_segment_id FROM dt WHERE relative_path = 'x/y.txt'")
	dtport=$(q "$CPORT" "SELECT port FROM gp_segment_configuration WHERE content = $dtseg AND role = 'p'")
	dtdir=$(q "$CPORT" "SELECT datadir FROM gp_segment_configuration WHERE content = $dtseg AND role = 'p'")
	dtfile="$dtdir/$(qd "$dtport" catbad "SELECT gp_sql.directory_table_location('dt')")/x/y.txt"
	[ -f "$dtfile" ] && : > "$dtfile"
	logat=$(stat -c %s "$catlog" 2> /dev/null || echo 0)
	run checkcat-bad gpcheckcat catbad
	rc=$?
	details=$(tail -c +$((logat + 1)) "$catlog")
	if [ "$rc" -ne 0 ] &&
	   grep -q "missing_extraneous_pg_class" "$LOGDIR/checkcat-bad.out" &&
	   grep -q "Extra relation metadata .* on content 0" "$LOGDIR/checkcat-bad.out" &&
	   grep -q "inconsistent_pg_seclabel" "$LOGDIR/checkcat-bad.out" &&
	   grep -q "label is 'distributed_by=(b)' on content 0" "$LOGDIR/checkcat-bad.out"; then
		ok "gpcheckcat: a table on one segment alone, and a distribution policy of one segment's own"
	else
		notok "gpcheckcat of what one segment alone was given" "$(tail_of checkcat-bad)"
	fi
	grep -q "content 0, .*aux_table gp_ao.segfile, storage_id 999999" <<< "$details" &&
	grep -q "content 0, .*relation public.px, issue has no pax.pg_pax_tables row" <<< "$details" &&
	grep -q "content $dtseg, .*dirtable public.dt, relative_path x/y.txt, size 5, on_disk 0" <<< "$details" \
		&& ok "gpcheckcat: gp_ao's row of no table and a PAX table without its row on content 0, and a directory table's file of another size on its segment" \
		|| notok "gpcheckcat of the modules' own" "$(grep -E 'FAIL|content 0' <<< "$details" | head)"

	###########################################################################
	echo "15. analyzedb analyzes a table again where it has changed since"
	###########################################################################
	# The tables it takes up, from its list of what it analyzes.
	analyzed() { sed -n 's/.*:-\(public\.[a-z_0-9]*\)$/\1/p' "$LOGDIR/$1.out" | sort -u | tr '\n' ' '; }
	if run analyzedb1 analyzedb -a -d tools -s public; then
		first=$(analyzed analyzedb1)
		qt "$CPORT" "INSERT INTO ao VALUES (301, 'ao301')" > /dev/null
		run analyzedb2 analyzedb -a -d tools -s public
		second=$(analyzed analyzedb2)
		qt "$CPORT" "DELETE FROM px WHERE a < 10" > /dev/null
		run analyzedb3 analyzedb -a -d tools -s public
		third=$(analyzed analyzedb3)
		[[ " $first " == *" public.ao "* && " $first " == *" public.aoc "* && " $first " == *" public.px "* ]] &&
		[[ " $second " == *" public.ao "* && " $second " != *" public.aoc "* && " $second " != *" public.px "* ]] &&
		[[ " $third " == *" public.px "* && " $third " != *" public.ao "* && " $third " != *" public.aoc "* ]] &&
		[[ " $second " == *" public.h "* ]] \
			&& ok "analyzedb: every table first, then the append-optimized and PAX tables where they changed, and heap tables always" \
			|| notok "analyzedb" "first: $first"$'\n'"second: $second"$'\n'"third: $third"
	else
		notok "analyzedb" "$(tail_of analyzedb1)"
	fi

	###########################################################################
	echo "16. gpload loads a file through gpfdist, and merges another"
	###########################################################################
	mkdir -p "$WORK/gpload"
	seq 1 1000 | awk '{ print $1 "|name " $1 }' > "$WORK/gpload/data1.txt"
	seq 995 1010 | awk '{ print $1 "|new " $1 }' > "$WORK/gpload/data2.txt"
	qt "$CPORT" "CREATE TABLE gl (id int PRIMARY KEY, name text) DISTRIBUTED BY (id)" > /dev/null
	for m in 1 2; do
		{
			echo "VERSION: 1.0.0.1"
			echo "DATABASE: tools"
			echo "USER: $USER"
			echo "HOST: $HOST"
			echo "PORT: $CPORT"
			echo "GPLOAD:"
			echo "   INPUT:"
			echo "    - SOURCE:"
			echo "         LOCAL_HOSTNAME:"
			echo "           - $HOST"
			echo "         PORT: $((BASE + 24))"
			echo "         FILE:"
			echo "           - $WORK/gpload/data$m.txt"
			echo "    - FORMAT: text"
			echo "    - DELIMITER: '|'"
			echo "    - COLUMNS:"
			echo "        - id: int"
			echo "        - name: text"
			echo "   OUTPUT:"
			echo "    - TABLE: gl"
			if [ "$m" = 1 ]; then
				echo "    - MODE: INSERT"
			else
				echo "    - MODE: MERGE"
				echo "    - MATCH_COLUMNS:"
				echo "        - id"
				echo "    - UPDATE_COLUMNS:"
				echo "        - name"
			fi
		} > "$WORK/gpload/load$m.yml"
	done
	if run gpload1 gpload -f "$WORK/gpload/load1.yml" &&
	   [ "$(qt "$CPORT" "SELECT count(*) || ':' || count(DISTINCT gp_segment_id) FROM gl")" = "1000:3" ] &&
	   run gpload2 gpload -f "$WORK/gpload/load2.yml"; then
		out=$(qt "$CPORT" "SELECT count(*) || ':' || count(*) FILTER (WHERE name LIKE 'new %') FROM gl")
		[ "$out" = "1010:16" ] && grep -q "rows Updated *= 6" "$LOGDIR/gpload2.out" \
			&& ok "gpload: 1000 rows inserted on the three segments, then 6 updated and 10 inserted by MERGE" \
			|| notok "gpload's MERGE" "$out / $(tail_of gpload2)"
	else
		notok "gpload" "$(tail_of gpload1; tail_of gpload2 2> /dev/null)"
	fi

	###########################################################################
	echo "17. gplogfilter finds an error in the coordinator's CSV log"
	###########################################################################
	qt "$CPORT" "SELECT 1 / 0" > /dev/null
	if run logfilter gplogfilter -f "division by zero" &&
	   grep -q "|ERROR: |22012|division by zero|" "$LOGDIR/logfilter.out" &&
	   grep -q "match: *1 lines" "$LOGDIR/logfilter.out"; then
		ok "gplogfilter: the error, as the server logged it, one entry of the CSV files"
	else
		notok "gplogfilter" "$(tail_of logfilter)"
	fi

	###########################################################################
	echo "18. gpmemwatcher watches the host's processes, and gpmemreport reports them"
	###########################################################################
	mkdir -p "$WORK/mw"
	echo "$HOST:$WORK/mw" > "$WORK/mw/hosts"
	if (cd "$WORK/mw" && run memwatcher gpmemwatcher -f hosts && sleep 2 &&
		run memwatcher-stop gpmemwatcher -f hosts --stop && run memreport gpmemreport "$HOST.ps.out.gz"); then
		report=$(ls "$WORK/mw"/[0-9]*-[0-9]* 2> /dev/null | head -1)
		[ -n "$report" ] && grep -q "^$CPORT " "$report" && grep -q "^$p0 " "$report" \
			&& ok "gpmemwatcher and gpmemreport: the coordinator's and the segments' memory, by postmaster" \
			|| notok "gpmemreport" "$(tail -12 "$report" 2> /dev/null)"
	else
		notok "gpmemwatcher" "$(tail_of memwatcher; tail_of memwatcher-stop; tail_of memreport)"
	fi

	###########################################################################
	echo "19. gpcheckperf measures the host's disk, memory and network"
	###########################################################################
	# It copies its programs, multidd and gpnetbench, into its directory and
	# runs them there: $EXEC's, not in /tmp.
	mkdir -p "$EXEC/cp"
	if run checkperf-ds gpcheckperf -h "$HOST" -r ds -d "$EXEC/cp" -S 32MB &&
	   grep -q "disk write tot bytes: 33554432" "$LOGDIR/checkperf-ds.out" &&
	   grep -q "stream tot bandwidth" "$LOGDIR/checkperf-ds.out"; then
		ok "gpcheckperf -r ds: the disk written and read, and stream's memory bandwidth"
	else
		notok "gpcheckperf -r ds" "$(tail_of checkperf-ds)"
	fi
	if run checkperf-n gpcheckperf -h "$HOST" -h localhost -r n -d "$EXEC/cp" --duration 5 &&
	   grep -q "^$HOST -> localhost = [0-9]" "$LOGDIR/checkperf-n.out"; then
		ok "gpcheckperf -r n: gpnetbench's bandwidth between the host's two names"
	else
		notok "gpcheckperf -r n" "$(tail_of checkperf-n)"
	fi

	###########################################################################
	echo "20. gpreload reloads a table sorted"
	###########################################################################
	qt "$CPORT" "CREATE TABLE rl (a int, b int) DISTRIBUTED BY (a);
				 INSERT INTO rl SELECT g % 10, (g * 7919) % 1000 FROM generate_series(1, 1000) g" > /dev/null
	echo "public.rl: b" > "$WORK/rl.txt"
	sorted_on_seg="SELECT bool_and(b1 <= b2) FROM (SELECT b AS b1, lead(b) OVER (ORDER BY ctid) AS b2 FROM rl) q WHERE b2 IS NOT NULL"
	if [ "$(qt "$p0" "$sorted_on_seg")" = f ] && run reload gpreload -d tools -t "$WORK/rl.txt" -a; then
		[ "$(qt "$p0" "$sorted_on_seg")" = t ] && [ "$(qt "$CPORT" "SELECT count(*) FROM rl")" = 1000 ] \
			&& ok "gpreload: the table's rows, every one, in the order of b on the segment" \
			|| notok "gpreload" "$(qt "$p0" "$sorted_on_seg") / $(tail_of reload)"
	else
		notok "gpreload" "$(tail_of reload)"
	fi

	###########################################################################
	echo "21. gppkg builds a package, installs it on every host and removes it"
	###########################################################################
	# Into a GPHOME of its own, of links to the server's, which this user may
	# write where the server's is not: a deb of one file, and gppkg's spec.
	P="$WORK/gppkg"
	mkdir -p "$P/deb/DEBIAN" "$P/deb/share/gppkg_demo" "$P/pkg"
	cp -as "$GPHOME" "$P/gphome"
	echo hello > "$P/deb/share/gppkg_demo/hello.txt"
	printf 'Package: gppkgdemo\nVersion: 1.0\nArchitecture: all\nMaintainer: the port\nDescription: gppkg check\n' > "$P/deb/DEBIAN/control"
	dpkg-deb --build --root-owner-group "$P/deb" "$P/pkg/gppkgdemo-1.0-1.all.deb" > /dev/null
	version=$(sed 's/.*Cloudberry) //; s/ build.*//' "$GPHOME/share/greenplum/gp_version")
	printf 'PkgName: gppkgdemo\nVersion: 1.0\nGPDBVersion: %s\nDescription: gppkg check\nOS: debian\nArchitecture: x86_64\n' "$version" > "$P/pkg/gppkg_spec.yml"
	if (cd "$P" && export GPHOME="$P/gphome" && . "$P/gphome/cloudberry-env.sh" &&
		run gppkg-build gppkg --build pkg && run gppkg-install gppkg -i gppkgdemo-1.0-debian-x86_64.gppkg &&
		[ -f "$P/gphome/share/gppkg_demo/hello.txt" ] && run gppkg-query gppkg -q --all &&
		grep -q "^gppkgdemo-1.0$" "$LOGDIR/gppkg-query.out" &&
		run gppkg-remove gppkg -r gppkgdemo-1.0 && [ ! -e "$P/gphome/share/gppkg_demo/hello.txt" ]); then
		ok "gppkg: a package built, installed into GPHOME by dpkg, listed, and removed"
	else
		notok "gppkg" "$(for f in build install query remove; do tail_of gppkg-$f 2> /dev/null; done | tail -15)"
	fi

	###########################################################################
	echo "22. gpdirtableload puts files into a directory table and takes them back"
	###########################################################################
	# By the COPY a directory table takes a file with, which the port's
	# directory tables on a cluster bring (m8_dirtable_19).
	mkdir -p "$WORK/dtl/in/sub" "$WORK/dtl/out"
	echo one > "$WORK/dtl/in/one.txt"
	head -c 100000 /dev/urandom > "$WORK/dtl/in/sub/two.bin"
	probe=$(echo x | "$BINDIR/psql" -X -q -h "$HOST" -p "$CPORT" -d tools -c "COPY binary dt FROM STDIN 'probe'" 2>&1)
	if [[ "$probe" == *"syntax error"* ]]; then
		echo "  skip   gpdirtableload: this build's directory tables take no COPY ... FROM STDIN '<path>' (directory tables on a cluster, m8_dirtable_19)"
	elif (cd "$WORK/dtl/in" && run dirtableload-up gpdirtableload -d tools --host "$HOST" -p "$CPORT" -U "$USER" \
			-t dt --input-file . --dest-path files --tag t1) &&
		 run dirtableload-down gpdirtableload -d tools --host "$HOST" -p "$CPORT" -U "$USER" --mode download \
			-t dt --input-file files --match regex --dest-path "$WORK/dtl/out"; then
		rows=$(qt "$CPORT" "SELECT string_agg(relative_path || ':' || size || ':' || tag, ' ' ORDER BY relative_path) FROM dt WHERE relative_path LIKE 'files/%'")
		[ "$rows" = "files/one.txt:4:t1 files/sub/two.bin:100000:t1" ] &&
		cmp -s "$WORK/dtl/in/one.txt" "$WORK/dtl/out/files/one.txt" &&
		cmp -s "$WORK/dtl/in/sub/two.bin" "$WORK/dtl/out/files/sub/two.bin" \
			&& ok "gpdirtableload: two files put with their tag, and taken back as they were" \
			|| notok "gpdirtableload" "$rows"
	else
		notok "gpdirtableload" "$(tail_of dirtableload-up; tail_of dirtableload-down 2> /dev/null)"
	fi
fi
printf 'y\ny\n' | run delete-c gpdeletesystem -f -d "$COORDINATOR_DATA_DIRECTORY"

###############################################################################
echo "23. gpdemo makes a demo cluster, probes it and deletes it"
###############################################################################
mkdir -p "$WORK/demo"
if (cd "$WORK/demo" && unset PGPORT COORDINATOR_DATA_DIRECTORY &&
	export PORT_BASE=$((BASE + 26)) NUM_PRIMARY_MIRROR_PAIRS=2 WITH_MIRRORS=false &&
	run demo gpdemo && run demo-probe gpdemo -p); then
	out=$(q $((BASE + 26)) "SELECT count(*) FROM gp_segment_configuration WHERE role = 'p' AND status = 'u'")
	[ "$out" = 3 ] && grep -q "gp_segment_configuration" "$LOGDIR/demo-probe.out" &&
	[ "$(grep -c "Apache Cloudberry" "$LOGDIR/demo-probe.out")" -eq 3 ] \
		&& ok "gpdemo: a coordinator and two segments, up, each node probed" \
		|| notok "gpdemo" "$out / $(tail_of demo-probe)"
else
	notok "gpdemo" "$(tail_of demo; tail_of demo-probe 2> /dev/null)"
fi
if (cd "$WORK/demo" && unset PGPORT COORDINATOR_DATA_DIRECTORY &&
	export PORT_BASE=$((BASE + 26)) NUM_PRIMARY_MIRROR_PAIRS=2 WITH_MIRRORS=false &&
	run demo-delete gpdemo -d) && [ ! -e "$WORK/demo/datadirs" ]; then
	ok "gpdemo -d: the demo cluster stopped and its directories gone"
else
	notok "gpdemo -d" "$(tail_of demo-delete)"
fi

echo
echo "gpMgmt tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
