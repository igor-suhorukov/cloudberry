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
# the roles back to the ones preferred; gpinitstandby makes a standby
# coordinator, and gpactivatestandby makes it the coordinator; and
# gpdeletesystem removes the cluster.  A second cluster, of primaries alone,
# whose nodes authenticate each other by certificates, is given its mirrors
# by gpaddmirrors, and gpmovemirrors moves one of them.
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

# Twelve ports none of which anything listens on: the coordinator's, the
# standby's, the primaries' and the mirrors', of two clusters.
free_ports() {				# free_ports <base> <n>
	local p
	for p in $(seq "$1" $(($1 + $2 - 1))); do
		ss -Htan "sport = :$p" 2> /dev/null | grep -q . && return 1
		[ -e "/tmp/.s.PGSQL.$p" ] && return 1
	done
	return 0
}
for _ in $(seq 20); do
	BASE=$((20000 + (RANDOM % 400) * 20))
	free_ports "$BASE" 20 && break
done

# cluster A: the coordinator BASE, the standby BASE+1, the primaries
# BASE+2..4, the mirrors BASE+12..14; cluster B: the coordinator BASE+5,
# its primaries BASE+6..8 and its mirrors BASE+16..18.
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
echo "7. gpinitstandby makes a standby coordinator"
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
echo "8. gpactivatestandby makes the standby the coordinator"
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
else
	notok "gpactivatestandby" "$(tail_of activate)"
fi

###############################################################################
echo "9. gpdeletesystem removes the cluster"
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
echo "10. a cluster whose nodes authenticate each other by certificates, and gpaddmirrors gives its primaries mirrors"
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
	echo "11. gpmovemirrors moves a mirror to another directory and port"
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

echo
echo "gpMgmt tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
